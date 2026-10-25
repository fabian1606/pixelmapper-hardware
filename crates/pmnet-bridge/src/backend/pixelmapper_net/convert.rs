//! Pure conversions between `pixelmapper-net` types and the bridge contract types (`protocol/bridge-ws.md`):
//! identifiers, interfaces, node and port state, ArtDiagData priorities and errors. No I/O; everything that
//! depends on the library's data model lives here so an API change of the library touches one file.

use crate::backend::BackendError;
use crate::protocol::{
    DiagPriority, ErrorCode, Failsafe, Indicator, MacAddress, MergeMode, NetInterface, NodeId,
    NodeState, NodeStatus, NodeStyle, OutputStyle, PortAddressAuthority, PortDirection,
    PortInputState, PortOutputState, PortProtocol, PortRef, PortState, RawStatus, RdmUid,
};
use pixelmapper_net as net;
use pixelmapper_net::controller::Interface;
use pixelmapper_net::rdm::Uid;
use pixelmapper_net::state as ns;
use std::net::{Ipv4Addr, SocketAddr};

/// UDP port of real Art-Net nodes; a node id from a source address only carries the port for other ports
/// (simulators on localhost).
const ARTNET_PORT: u16 = net::artnet::PORT;

// ── Identifiers ───────────────────────────────────────────────────────────────────────────────────────────────────

/// Library UID → contract UID.
pub(super) fn uid_to_bridge(u: Uid) -> RdmUid {
    RdmUid::new(u.manufacturer, u.device)
}

/// Contract UID → library UID.
pub(super) fn uid_to_net(u: RdmUid) -> Uid {
    Uid::new(u.manufacturer, u.device)
}

/// Library node id → contract `nodeId` (contract §3): the UID, else `mac:…`, else `ip:<ip>`. The library keys
/// address-only nodes by IP *and* UDP port; the port is appended only when it is not 6454 (simulators), so two
/// simulated nodes on one host do not collide.
pub(super) fn node_id(id: &net::NodeId) -> NodeId {
    match id {
        net::NodeId::Uid(u) => NodeId::from_uid(uid_to_bridge(*u)),
        net::NodeId::Mac(m) => NodeId::from_mac(MacAddress(*m)),
        net::NodeId::Addr(SocketAddr::V4(a)) if a.port() == ARTNET_PORT => NodeId::from_ip(*a.ip()),
        net::NodeId::Addr(a) => NodeId(format!("ip:{a}")),
    }
}

// ── Interfaces ────────────────────────────────────────────────────────────────────────────────────────────────────

/// One interface as the contract reports it (the library does not read MAC addresses).
pub(super) fn interface(i: &Interface) -> NetInterface {
    NetInterface {
        name: i.name.clone(),
        ip: i.ip,
        mask: i.netmask,
        mac: None,
    }
}

/// The interfaces shown in `hello`/`bridgeStatus`: every non-loopback interface (only the bound one when the
/// socket is bound to a specific address); loopback interfaces only when the socket is bound to loopback
/// (simulator setups).
pub(super) fn visible_interfaces(all: &[Interface], bind_ip: Ipv4Addr) -> Vec<NetInterface> {
    all.iter()
        .filter(|i| {
            if bind_ip.is_loopback() {
                i.loopback
            } else {
                !i.loopback && (bind_ip.is_unspecified() || i.ip == bind_ip)
            }
        })
        .map(interface)
        .collect()
}

/// Name of the local interface whose subnet contains `ip` (most specific mask wins).
pub(super) fn interface_for(ip: Ipv4Addr, all: &[Interface]) -> Option<String> {
    all.iter()
        .filter(|i| {
            let mask = u32::from(i.netmask);
            mask != 0 && u32::from(ip) & mask == u32::from(i.ip) & mask
        })
        .max_by_key(|i| u32::from(i.netmask).count_ones())
        .map(|i| i.name.clone())
}

// ── Node state ────────────────────────────────────────────────────────────────────────────────────────────────────

/// The source IP of the node's replies (where requests go), falling back to the IP field of the reply.
pub(super) fn node_ip(n: &ns::NodeState) -> Ipv4Addr {
    match n.address {
        SocketAddr::V4(a) => *a.ip(),
        SocketAddr::V6(_) => n.ip,
    }
}

/// Merged library node state → contract `NodeState`.
///
/// Gaps of the library model: `ubeaVersion` is not kept (reported as 0). A port carries one Port-Address, the one
/// of its current direction; the field of the other direction gets Net and Sub-Net with universe 0, as the
/// Pixelmapper Node reports it (node contract §3). Status3 is kept only from the reply with the lowest BindIndex,
/// so a port's `failsafe` is known only for that BindIndex (`None` for the others). `lastSeen` is supplied by the
/// caller.
pub(super) fn node_state(n: &ns::NodeState, interfaces: &[Interface], last_seen: u64) -> NodeState {
    let ip = node_ip(n);
    let switchable = n.status3.direction_switchable;
    let root = n.ports.iter().map(|p| p.bind_index).min();
    let root_failsafe = failsafe(n.status3.failsafe);
    NodeState {
        node_id: node_id(&n.id),
        uid: n.uid.map(uid_to_bridge),
        mac: (n.mac != [0; 6]).then_some(MacAddress(n.mac)),
        ip,
        interface: interface_for(ip, interfaces),
        short_name: n.short_name.clone(),
        long_name: n.long_name.clone(),
        node_report: n.node_report.clone(),
        firmware: n.firmware_version,
        esta_man: n.esta_man,
        oem: n.oem,
        ubea_version: 0,
        style: node_style(n.style),
        refresh_rate: n.refresh_rate,
        status: node_status(n),
        raw: RawStatus {
            status1: n.status1.raw,
            status2: n.status2.raw,
            status3: n.status3.raw,
        },
        ports: n
            .ports
            .iter()
            .map(|p| {
                let failsafe = (Some(p.bind_index) == root).then_some(root_failsafe);
                port_state(p, switchable, failsafe)
            })
            .collect(),
        last_seen,
    }
}

/// ArtPollReply `Style`.
fn node_style(style: u8) -> NodeStyle {
    match style {
        0 => NodeStyle::Node,
        1 => NodeStyle::Controller,
        2 => NodeStyle::Media,
        3 => NodeStyle::Route,
        4 => NodeStyle::Backup,
        5 => NodeStyle::Config,
        6 => NodeStyle::Visual,
        _ => NodeStyle::Unknown,
    }
}

/// Status1/2/3 of the root reply.
fn node_status(n: &ns::NodeState) -> NodeStatus {
    let (s1, s2, s3) = (&n.status1, &n.status2, &n.status3);
    NodeStatus {
        indicator: match s1.indicators {
            ns::Indicators::Unknown => Indicator::Unknown,
            ns::Indicators::Locate => Indicator::Locate,
            ns::Indicators::Mute => Indicator::Mute,
            ns::Indicators::Normal => Indicator::Normal,
        },
        port_address_authority: match s1.port_address_authority {
            1 => PortAddressAuthority::Panel,
            2 => PortAddressAuthority::Network,
            _ => PortAddressAuthority::Unknown,
        },
        booted_from_rom: s1.rom_boot,
        rdm_capable: s1.rdm_capable,
        ubea_present: s1.ubea_present,
        web_config: s2.web_config,
        dhcp_configured: s2.dhcp_active,
        dhcp_capable: s2.dhcp_capable,
        port_address_15_bit: s2.port_address_15bit,
        sacn_switchable: s2.sacn_switchable,
        squawking: s2.squawking,
        output_style_switchable: s2.output_style_switchable,
        rdm_switchable: s2.rdm_switchable,
        failsafe_programmable: s3.failsafe_programmable,
        llrp: s3.llrp,
        port_direction_switchable: s3.direction_switchable,
        rdmnet: s3.rdmnet,
        background_queue: s3.background_queue,
        background_discovery_control: s3.background_discovery_switchable,
    }
}

/// Status3 failsafe mode.
fn failsafe(f: ns::Failsafe) -> Failsafe {
    match f {
        ns::Failsafe::Hold => Failsafe::Hold,
        ns::Failsafe::Zero => Failsafe::Zero,
        ns::Failsafe::Full => Failsafe::Full,
        ns::Failsafe::Scene => Failsafe::Scene,
    }
}

/// One port; the flags are decoded from the raw GoodInput/GoodOutputA/GoodOutputB bytes (contract §4).
fn port_state(
    p: &ns::PortState,
    direction_switchable: bool,
    failsafe: Option<Failsafe>,
) -> PortState {
    let (gi, ga, gb) = (p.good_input, p.good_output_a, p.good_output_b);
    let direction = if p.can_output && p.can_input && !direction_switchable {
        // Fixed ports with both bits do both; with Status3 bit 3 the bits show the current direction.
        PortDirection::Both
    } else {
        match p.direction {
            ns::PortDirection::Output => PortDirection::Output,
            ns::PortDirection::Input => PortDirection::Input,
            ns::PortDirection::None => PortDirection::None,
        }
    };
    // Only the field of the current direction carries the universe (node contract §3); fixed ports doing both
    // directions report one address for both.
    let pa = p.port_address.raw();
    let (output_port_address, input_port_address) = match direction {
        PortDirection::Output => (pa, pa & 0x7FF0),
        PortDirection::Input => (pa & 0x7FF0, pa),
        PortDirection::Both | PortDirection::None => (pa, pa),
    };
    PortState {
        port: PortRef::new(p.bind_index, p.index),
        protocol: match p.protocol {
            0 => PortProtocol::Dmx512,
            1 => PortProtocol::Midi,
            2 => PortProtocol::Avab,
            3 => PortProtocol::Cmx,
            4 => PortProtocol::Adb,
            5 => PortProtocol::Artnet,
            6 => PortProtocol::Dali,
            _ => PortProtocol::Unknown,
        },
        can_output: p.can_output,
        can_input: p.can_input,
        direction,
        output_port_address,
        input_port_address,
        failsafe,
        output: PortOutputState {
            data: ga & 0x80 != 0,
            merging: ga & 0x08 != 0,
            merge: if ga & 0x02 != 0 {
                MergeMode::Ltp
            } else {
                MergeMode::Htp
            },
            short_circuit: ga & 0x04 != 0,
            sacn: ga & 0x01 != 0,
            rdm: gb & 0x80 == 0,
            style: if gb & 0x40 != 0 {
                OutputStyle::Continuous
            } else {
                OutputStyle::Delta
            },
            discovery_running: gb & 0x20 == 0,
            background_discovery: gb & 0x10 == 0,
        },
        input: PortInputState {
            data: gi & 0x80 != 0,
            disabled: gi & 0x08 != 0,
            errors: gi & 0x04 != 0,
            sacn: gi & 0x01 != 0,
        },
    }
}

// ── Diagnostics ───────────────────────────────────────────────────────────────────────────────────────────────────

/// ArtDiagData `DiagPriority` (DpLow 0x10, DpMed 0x40, DpHigh 0x80, DpCritical 0xE0, DpVolatile 0xF0); values
/// between the codes fall to the next lower one.
pub(super) fn diag_priority(p: u8) -> DiagPriority {
    match p {
        0x00..=0x3F => DiagPriority::Low,
        0x40..=0x7F => DiagPriority::Medium,
        0x80..=0xDF => DiagPriority::High,
        0xE0..=0xEF => DiagPriority::Critical,
        _ => DiagPriority::Volatile,
    }
}

// ── Errors ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Library error → backend error. `node`/`port` name the addressed node and port (for `unknownNode` /
/// `unknownPort`, which the library reports with its own ids).
pub(super) fn backend_error(e: net::Error, node: &NodeId, port: Option<PortRef>) -> BackendError {
    match e {
        net::Error::Nack { reason, .. } => BackendError::Nack(reason.0),
        net::Error::Timeout { .. } | net::Error::WaitTimeout(..) => BackendError::Timeout,
        net::Error::UnexpectedResponse(m) => BackendError::BadResponse(m),
        net::Error::RdmDecode(e) => BackendError::BadResponse(e.to_string()),
        net::Error::ArtNet(e) => BackendError::BadResponse(e.to_string()),
        net::Error::UnknownNode(_) => BackendError::UnknownNode(node.clone()),
        net::Error::UnknownPort { .. } => match port {
            Some(p) => BackendError::UnknownPort(p),
            None => BackendError::UnknownNode(node.clone()),
        },
        net::Error::Broadcast(_) => BackendError::InvalidArgument {
            field: "uid".into(),
            message: "a broadcast UID cannot answer this command".into(),
        },
        net::Error::InvalidArgument(m) => BackendError::InvalidArgument {
            field: "data".into(),
            message: m,
        },
        net::Error::Image(m) => BackendError::InvalidImage(m),
        net::Error::Closed => BackendError::UdpUnavailable,
        net::Error::Io(e) => BackendError::Internal(format!("network I/O error: {e}")),
        other => BackendError::Internal(other.to_string()),
    }
}

/// Error code of a failed firmware job. The library reports flow failures as `Error::Flash(text)` without a
/// kind, so the few cases with their own contract code are recognised by their text.
pub(super) fn flash_error_code(e: &net::Error) -> ErrorCode {
    match e {
        net::Error::Flash(m) if m.starts_with("wrong device") => ErrorCode::DeviceMismatch,
        net::Error::Flash(m) if m.starts_with("downgrade") => ErrorCode::Downgrade,
        net::Error::Flash(m)
            if m.starts_with("no BOOT_INFO answer") || m.starts_with("no answer") =>
        {
            ErrorCode::Timeout
        }
        net::Error::Flash(_) => ErrorCode::BadResponse,
        net::Error::Nack { .. } => ErrorCode::Nack,
        net::Error::Timeout { .. } | net::Error::WaitTimeout(..) => ErrorCode::Timeout,
        net::Error::UnexpectedResponse(_) | net::Error::RdmDecode(_) | net::Error::ArtNet(_) => {
            ErrorCode::BadResponse
        }
        net::Error::UnknownNode(_) => ErrorCode::UnknownNode,
        net::Error::UnknownPort { .. } => ErrorCode::UnknownPort,
        net::Error::Image(_) => ErrorCode::InvalidImage,
        net::Error::Closed => ErrorCode::UdpUnavailable,
        _ => ErrorCode::Internal,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use net::artnet::{ArtPollReply, PortAddress};
    use net::rdm::NackReason;

    fn reply(bind: u8) -> ArtPollReply {
        ArtPollReply {
            ip: Ipv4Addr::new(2, 0, 0, 10),
            short_name: "PM Node".into(),
            long_name: "Pixelmapper Node".into(),
            num_ports: 1,
            port_types: [0x80, 0, 0, 0],
            sw_out: [bind, 0, 0, 0],
            good_output_a: [0x8A, 0, 0, 0],
            good_output_b: [0xC0, 0, 0, 0],
            good_input: [0x08, 0, 0, 0],
            bind_index: bind,
            default_resp_uid: Uid::new(0x7FF0, 0x8012_3456),
            mac: [2, 0, 0, 0x12, 0x34, 0x56],
            status1: 0xE2,
            status2: 0xCE,
            status3: 0x68,
            esta_man: 0x7FF0,
            oem: 0x00FF,
            vers_info: 0x0102,
            refresh_rate: 44,
            ..Default::default()
        }
    }

    fn lib_node() -> ns::NodeState {
        let src: SocketAddr = "2.0.0.10:6454".parse().unwrap();
        let replies: Vec<_> = (1..=4).map(|b| (src, reply(b))).collect();
        let id = ns::reply_node_id(&replies[0].1, src);
        ns::merge_replies(id, &replies).unwrap()
    }

    fn iface(name: &str, ip: [u8; 4], mask: [u8; 4], loopback: bool) -> Interface {
        Interface {
            name: name.into(),
            ip: ip.into(),
            netmask: mask.into(),
            broadcast: Ipv4Addr::BROADCAST,
            loopback,
        }
    }

    #[test]
    fn node_ids() {
        let uid = net::NodeId::Uid(Uid::new(0x7FF0, 0x8A1B_2C3D));
        assert_eq!(node_id(&uid).as_str(), "7FF0:8A1B2C3D");
        let mac = net::NodeId::Mac([2, 0x7f, 0xf0, 0x1b, 0x2c, 0x3d]);
        assert_eq!(node_id(&mac).as_str(), "mac:02:7f:f0:1b:2c:3d");
        let addr = net::NodeId::Addr("2.0.0.5:6454".parse().unwrap());
        assert_eq!(node_id(&addr).as_str(), "ip:2.0.0.5");
        let sim = net::NodeId::Addr("127.0.0.1:50000".parse().unwrap());
        assert_eq!(node_id(&sim).as_str(), "ip:127.0.0.1:50000");
        let u = RdmUid::new(0x7FF0, 7);
        assert_eq!(uid_to_bridge(uid_to_net(u)), u);
    }

    #[test]
    fn node_state_fields() {
        let ifs = [
            iface("en7", [2, 0, 0, 1], [255, 0, 0, 0], false),
            iface("lo0", [127, 0, 0, 1], [255, 0, 0, 0], true),
        ];
        let n = node_state(&lib_node(), &ifs, 42);
        assert_eq!(n.node_id.as_str(), "7FF0:80123456");
        assert_eq!(n.uid, Some(RdmUid::new(0x7FF0, 0x8012_3456)));
        assert_eq!(n.mac.unwrap().to_string(), "02:00:00:12:34:56");
        assert_eq!(n.ip, Ipv4Addr::new(2, 0, 0, 10));
        assert_eq!(n.interface.as_deref(), Some("en7"));
        assert_eq!(n.firmware, 0x0102);
        assert_eq!((n.esta_man, n.oem, n.refresh_rate), (0x7FF0, 0x00FF, 44));
        assert_eq!(n.style, NodeStyle::Node);
        assert_eq!(n.status.indicator, Indicator::Normal);
        assert_eq!(
            n.status.port_address_authority,
            PortAddressAuthority::Network
        );
        assert!(n.status.rdm_capable && n.status.port_address_15_bit);
        assert!(n.status.dhcp_capable && n.status.dhcp_configured);
        assert!(n.status.failsafe_programmable && n.status.port_direction_switchable);
        assert_eq!(
            n.ports[0].failsafe,
            Some(Failsafe::Zero),
            "root reply's Status3"
        );
        assert_eq!(
            n.ports[1].failsafe, None,
            "Status3 of other pages is not kept"
        );
        assert_eq!(n.raw.status3, 0x68);
        assert_eq!(n.last_seen, 42);
        assert_eq!(n.ports.len(), 4);
        let p = &n.ports[2];
        assert_eq!(p.port, PortRef::new(3, 0));
        assert_eq!(p.protocol, PortProtocol::Dmx512);
        assert_eq!(p.direction, PortDirection::Output);
        assert_eq!((p.output_port_address, p.input_port_address), (3, 0));
        assert!(p.output.data && p.output.merging && !p.output.short_circuit);
        assert_eq!(p.output.merge, MergeMode::Ltp);
        assert!(!p.output.rdm, "GoodOutputB bit 7 set = RDM off");
        assert_eq!(p.output.style, OutputStyle::Continuous);
        assert!(p.output.discovery_running && p.output.background_discovery);
        assert!(p.input.disabled && !p.input.data);
    }

    #[test]
    fn fixed_bidirectional_port_is_both() {
        let mut r = reply(1);
        r.port_types[0] = 0xC0;
        r.status3 = 0;
        let src: SocketAddr = "2.0.0.10:6454".parse().unwrap();
        let n = ns::merge_replies(ns::reply_node_id(&r, src), &[(src, r)]).unwrap();
        assert_eq!(
            node_state(&n, &[], 0).ports[0].direction,
            PortDirection::Both
        );
    }

    #[test]
    fn interfaces_shown() {
        let ifs = [
            iface("en0", [192, 168, 1, 2], [255, 255, 255, 0], false),
            iface("en7", [2, 0, 0, 1], [255, 0, 0, 0], false),
            iface("lo0", [127, 0, 0, 1], [255, 0, 0, 0], true),
        ];
        let names = |v: Vec<NetInterface>| v.into_iter().map(|i| i.name).collect::<Vec<_>>();
        assert_eq!(
            names(visible_interfaces(&ifs, Ipv4Addr::UNSPECIFIED)),
            ["en0", "en7"]
        );
        assert_eq!(
            names(visible_interfaces(&ifs, Ipv4Addr::new(2, 0, 0, 1))),
            ["en7"]
        );
        assert_eq!(
            names(visible_interfaces(&ifs, Ipv4Addr::LOCALHOST)),
            ["lo0"]
        );
        assert_eq!(
            interface_for(Ipv4Addr::new(192, 168, 1, 77), &ifs).as_deref(),
            Some("en0")
        );
        assert_eq!(interface_for(Ipv4Addr::new(10, 0, 0, 1), &ifs), None);
    }

    #[test]
    fn diag_priorities() {
        assert_eq!(diag_priority(0x10), DiagPriority::Low);
        assert_eq!(diag_priority(0x40), DiagPriority::Medium);
        assert_eq!(diag_priority(0x80), DiagPriority::High);
        assert_eq!(diag_priority(0xE0), DiagPriority::Critical);
        assert_eq!(diag_priority(0xF0), DiagPriority::Volatile);
    }

    #[test]
    fn errors() {
        let node = NodeId::from("7FF0:00000001");
        let port = PortRef::new(2, 0);
        let nack = net::Error::Nack {
            uid: Uid::new(1, 2),
            pid: 0x60,
            reason: NackReason::DATA_OUT_OF_RANGE,
        };
        assert_eq!(backend_error(nack, &node, None), BackendError::Nack(6));
        let timeout = net::Error::Timeout {
            uid: Uid::new(1, 2),
            pid: 0x60,
            attempts: 3,
        };
        assert_eq!(backend_error(timeout, &node, None), BackendError::Timeout);
        let unknown_port = net::Error::UnknownPort {
            node: "x".into(),
            port_address: PortAddress::new(9).unwrap().to_string(),
        };
        assert_eq!(
            backend_error(unknown_port, &node, Some(port)),
            BackendError::UnknownPort(port)
        );
        assert_eq!(
            backend_error(net::Error::UnknownNode("x".into()), &node, None),
            BackendError::UnknownNode(node.clone())
        );
        assert_eq!(
            backend_error(net::Error::Closed, &node, None),
            BackendError::UdpUnavailable
        );
        let mismatch = net::Error::Flash("wrong device: DEVICE_INFO reports …".into());
        assert_eq!(flash_error_code(&mismatch), ErrorCode::DeviceMismatch);
        let stays = net::Error::Flash("3 chunk(s) differ - bootloader stays active".into());
        assert_eq!(flash_error_code(&stays), ErrorCode::BadResponse);
        assert_eq!(
            flash_error_code(&net::Error::Flash("no BOOT_INFO answer from x".into())),
            ErrorCode::Timeout
        );
    }
}
