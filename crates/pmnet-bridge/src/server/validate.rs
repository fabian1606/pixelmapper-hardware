//! Argument checks the server runs before calling the backend: shapes and ranges the JSON types cannot express,
//! node/port existence against the mirrored node table, the shared Net/Sub-Net rule of `artAddress`, and the
//! `dmxRoutes` table. Pure functions over contract types.

use crate::protocol::{
    ArtAddressArgs, CommandError, DmxRoute, ErrorCode, NetInterface, NodeId, NodeState, PortRef,
    RdmUid, MAX_PORT_ADDRESS,
};
use std::collections::{BTreeMap, HashSet};
use std::net::Ipv4Addr;

/// `port` must be bind 1–255 / index 0–3.
pub fn check_port(field: &str, port: PortRef) -> Result<(), CommandError> {
    if port.is_valid() {
        Ok(())
    } else {
        Err(CommandError::bad_request(
            field,
            format!("port {port} out of range (bind 1–255, index 0–3)"),
        ))
    }
}

/// `subDevice` must be 0–512 or 65535 (all).
pub fn check_sub_device(sub_device: u16) -> Result<(), CommandError> {
    if sub_device <= 512 || sub_device == 0xFFFF {
        Ok(())
    } else {
        Err(CommandError::bad_request(
            "subDevice",
            "subDevice must be 0–512 or 65535",
        ))
    }
}

/// Requests that need one device's answer cannot go to a broadcast UID.
pub fn check_unicast(uid: RdmUid) -> Result<(), CommandError> {
    if uid.is_broadcast() {
        Err(CommandError::bad_request(
            "uid",
            "a broadcast UID cannot answer this command",
        ))
    } else {
        Ok(())
    }
}

/// `unknownNode` error.
pub fn unknown_node(node_id: &NodeId) -> CommandError {
    CommandError::new(ErrorCode::UnknownNode, format!("unknown node {node_id}"))
}

/// The node must have the port (`unknownPort`).
pub fn check_node_port(node: &NodeState, port: PortRef) -> Result<(), CommandError> {
    match node.port(port) {
        Some(_) => Ok(()),
        None => Err(CommandError::new(
            ErrorCode::UnknownPort,
            format!("node {} has no port {port}", node.node_id),
        )),
    }
}

/// Checks `rdmGet` (`set == false`: at most one of `data`/`value`) and `rdmSet` (exactly one).
pub fn check_rdm_payload(set: bool, has_data: bool, has_value: bool) -> Result<(), CommandError> {
    match (set, has_data, has_value) {
        (_, true, true) => Err(CommandError::bad_request(
            "value",
            "send either data or value, not both",
        )),
        (true, false, false) => Err(CommandError::bad_request(
            "data",
            "rdmSet needs data or value",
        )),
        _ => Ok(()),
    }
}

/// Validates an `artAddress` command against the node's current state (contract §6 Semantics):
/// ports exist and appear once; names fit (≤ 17 / ≤ 63 bytes); Port-Addresses are ≤ 32767; and per BindIndex,
/// every written address plus the current address of every port without a written address share bits 14–4
/// (NetSwitch/SubSwitch are per BindIndex).
pub fn validate_art_address(node: &NodeState, args: &ArtAddressArgs) -> Result<(), CommandError> {
    if args.short_name.as_ref().is_some_and(|n| n.len() > 17) {
        return Err(CommandError::bad_request(
            "shortName",
            "shortName is longer than 17 characters",
        ));
    }
    if args.long_name.as_ref().is_some_and(|n| n.len() > 63) {
        return Err(CommandError::bad_request(
            "longName",
            "longName is longer than 63 characters",
        ));
    }
    let mut seen = HashSet::new();
    // Written addresses per BindIndex, and the ports of that bind that have one.
    let mut written: BTreeMap<u8, (Vec<u16>, HashSet<PortRef>)> = BTreeMap::new();
    for (i, change) in args.ports.iter().enumerate() {
        check_port(&format!("ports[{i}].port"), change.port)?;
        check_node_port(node, change.port)?;
        if !seen.insert(change.port) {
            return Err(CommandError::bad_request(
                "ports",
                format!("port {} appears twice", change.port),
            ));
        }
        for address in [change.output_port_address, change.input_port_address]
            .into_iter()
            .flatten()
        {
            if address > MAX_PORT_ADDRESS {
                return Err(CommandError::new(
                    ErrorCode::InvalidPortAddress,
                    format!(
                        "Port-Address {address} of port {} is out of range (0–32767)",
                        change.port
                    ),
                ));
            }
            let entry = written.entry(change.port.bind).or_default();
            entry.0.push(address);
            entry.1.insert(change.port);
        }
    }
    for (bind, (addresses, ports)) in written {
        let untouched = node
            .ports
            .iter()
            .filter(|p| p.port.bind == bind && !ports.contains(&p.port))
            .map(|p| p.output_port_address);
        let mut net_sub = addresses
            .iter()
            .copied()
            .chain(untouched)
            .map(|a| a & 0x7FF0);
        let first = net_sub.next().unwrap_or(0);
        if net_sub.any(|ns| ns != first) {
            return Err(CommandError::new(
                ErrorCode::InvalidPortAddress,
                format!(
                    "ports of BindIndex {bind} share Net and Sub-Net; write all of its ports to move them together"
                ),
            ));
        }
    }
    Ok(())
}

/// Validates an `artInput` command: the BindIndex exists and `disabled` has one entry per port of it.
pub fn validate_art_input(
    node: &NodeState,
    bind: u8,
    disabled: &[bool],
) -> Result<(), CommandError> {
    let ports = node.ports.iter().filter(|p| p.port.bind == bind).count();
    if ports == 0 {
        return Err(CommandError::new(
            ErrorCode::UnknownPort,
            format!("node {} has no BindIndex {bind}", node.node_id),
        ));
    }
    if disabled.len() != ports {
        return Err(CommandError::bad_request(
            "disabled",
            format!(
                "BindIndex {bind} has {ports} port(s), disabled has {} entries",
                disabled.len()
            ),
        ));
    }
    Ok(())
}

/// True for 255.255.255.255 and for the directed broadcast address of any local interface.
pub fn is_broadcast(ip: Ipv4Addr, interfaces: &[NetInterface]) -> bool {
    ip.is_broadcast()
        || interfaces.iter().any(|i| {
            let mask = u32::from(i.mask);
            // /31 and /32 networks have no broadcast address.
            mask.count_ones() < 31 && u32::from(ip) == u32::from(i.ip) | !mask
        })
}

/// Validates a `dmxRoutes` table: route ids 1–65535 and unique, exactly one of `nodeId`/`ip`, Port-Address ≤ 32767,
/// and no broadcast `ip` (Art-Net 4 forbids broadcast ArtDmx, contract §6 DmxRoute).
pub fn validate_routes(
    routes: &[DmxRoute],
    interfaces: &[NetInterface],
) -> Result<(), CommandError> {
    let mut ids = HashSet::new();
    for (i, r) in routes.iter().enumerate() {
        if r.route == 0 {
            return Err(CommandError::bad_request(
                format!("routes[{i}].route"),
                "route must be 1–65535",
            ));
        }
        if !ids.insert(r.route) {
            return Err(CommandError::bad_request(
                format!("routes[{i}].route"),
                format!("route {} twice", r.route),
            ));
        }
        if r.node_id.is_some() == r.ip.is_some() {
            return Err(CommandError::bad_request(
                format!("routes[{i}]"),
                "a route needs exactly one of nodeId / ip",
            ));
        }
        if r.port_address > MAX_PORT_ADDRESS {
            return Err(CommandError::bad_request(
                format!("routes[{i}].portAddress"),
                "portAddress must be 0–32767",
            ));
        }
        if let Some(ip) = r.ip.filter(|ip| is_broadcast(*ip, interfaces)) {
            return Err(CommandError::bad_request(
                format!("routes[{i}].ip"),
                format!("{ip} is a broadcast address; Art-Net 4 forbids broadcast ArtDmx"),
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::mock::sample_node;
    use crate::protocol::ArtAddressPort;
    use std::net::Ipv4Addr;

    fn change(bind: u8, index: u8, out: Option<u16>) -> ArtAddressPort {
        ArtAddressPort {
            port: PortRef::new(bind, index),
            output_port_address: out,
            input_port_address: None,
            direction: None,
            merge: None,
            rdm: None,
            output_style: None,
            protocol: None,
            clear_output: false,
        }
    }

    fn args(ports: Vec<ArtAddressPort>) -> ArtAddressArgs {
        ArtAddressArgs {
            node_id: NodeId::from("n"),
            short_name: None,
            long_name: None,
            ports,
            failsafe: None,
            record_failsafe_scene: false,
            indicator: None,
            cancel_merge: false,
            reset_rx_flags: false,
        }
    }

    /// A node with 4 ports in one BindIndex (like a classic 4-port node), Port-Addresses 0x10–0x13.
    fn four_port_node() -> NodeState {
        let mut node = sample_node("n", Ipv4Addr::LOCALHOST, 4);
        for (i, p) in node.ports.iter_mut().enumerate() {
            p.port = PortRef::new(1, i as u8);
            p.output_port_address = 0x10 + i as u16;
            p.input_port_address = 0x10 + i as u16;
        }
        node
    }

    #[test]
    fn one_port_per_bind_moves_freely() {
        let node = sample_node("n", Ipv4Addr::LOCALHOST, 4);
        assert!(validate_art_address(&node, &args(vec![change(2, 0, Some(0x0123))])).is_ok());
    }

    #[test]
    fn shared_net_sub_rule() {
        let node = four_port_node();
        // Same Sub-Net: fine.
        assert!(validate_art_address(&node, &args(vec![change(1, 2, Some(0x1F))])).is_ok());
        // Other Sub-Net while the other ports stay: rejected.
        let e = validate_art_address(&node, &args(vec![change(1, 2, Some(0x25))])).unwrap_err();
        assert_eq!(e.code, ErrorCode::InvalidPortAddress);
        // All four ports together: fine.
        let all = (0..4)
            .map(|i| change(1, i, Some(0x20 + u16::from(i))))
            .collect();
        assert!(validate_art_address(&node, &args(all)).is_ok());
        // Out of range.
        let e = validate_art_address(&node, &args(vec![change(1, 0, Some(0x8000))])).unwrap_err();
        assert_eq!(e.code, ErrorCode::InvalidPortAddress);
    }

    #[test]
    fn ports_must_exist_once() {
        let node = sample_node("n", Ipv4Addr::LOCALHOST, 2);
        let e = validate_art_address(&node, &args(vec![change(3, 0, None)])).unwrap_err();
        assert_eq!(e.code, ErrorCode::UnknownPort);
        let e = validate_art_address(&node, &args(vec![change(1, 0, None), change(1, 0, None)]))
            .unwrap_err();
        assert_eq!(e.code, ErrorCode::BadRequest);
        let e = validate_art_address(&node, &args(vec![change(1, 4, None)])).unwrap_err();
        assert_eq!(e.details.unwrap()["field"], "ports[0].port");
    }

    #[test]
    fn art_input_and_routes() {
        let node = sample_node("n", Ipv4Addr::LOCALHOST, 2);
        assert!(validate_art_input(&node, 1, &[true]).is_ok());
        assert_eq!(
            validate_art_input(&node, 1, &[true, false])
                .unwrap_err()
                .code,
            ErrorCode::BadRequest
        );
        assert_eq!(
            validate_art_input(&node, 9, &[true]).unwrap_err().code,
            ErrorCode::UnknownPort
        );

        let route = |route, node: Option<&str>, ip: Option<Ipv4Addr>, pa| DmxRoute {
            route,
            node_id: node.map(NodeId::from),
            ip,
            port_address: pa,
        };
        let ifs = [NetInterface {
            name: "en0".into(),
            ip: Ipv4Addr::new(2, 0, 0, 1),
            mask: Ipv4Addr::new(255, 0, 0, 0),
            mac: None,
        }];
        let lo = Some(Ipv4Addr::LOCALHOST);
        assert!(
            validate_routes(&[route(1, Some("n"), None, 1), route(2, None, lo, 2)], &ifs).is_ok()
        );
        assert!(validate_routes(&[route(0, Some("n"), None, 1)], &ifs).is_err());
        assert!(validate_routes(
            &[route(1, Some("n"), None, 1), route(1, Some("n"), None, 2)],
            &ifs
        )
        .is_err());
        assert!(validate_routes(&[route(1, Some("n"), lo, 1)], &ifs).is_err());
        assert!(validate_routes(&[route(1, None, None, 1)], &ifs).is_err());
        assert!(validate_routes(&[route(1, Some("n"), None, 0x8000)], &ifs).is_err());
        let e = validate_routes(
            &[route(1, None, Some(Ipv4Addr::new(2, 255, 255, 255)), 1)],
            &ifs,
        )
        .unwrap_err();
        assert_eq!(e.details.unwrap()["field"], "routes[0].ip");
        assert!(validate_routes(&[route(1, None, Some(Ipv4Addr::BROADCAST), 1)], &ifs).is_err());
        assert!(
            validate_routes(&[route(1, None, Some(Ipv4Addr::new(2, 1, 2, 3)), 1)], &ifs).is_ok()
        );
    }

    #[test]
    fn small_checks() {
        assert!(check_sub_device(0).is_ok());
        assert!(check_sub_device(512).is_ok());
        assert!(check_sub_device(0xFFFF).is_ok());
        assert!(check_sub_device(513).is_err());
        assert!(check_rdm_payload(false, false, false).is_ok());
        assert!(check_rdm_payload(false, true, true).is_err());
        assert!(check_rdm_payload(true, false, false).is_err());
        assert!(check_rdm_payload(true, false, true).is_ok());
        assert!(check_unicast(RdmUid::BROADCAST).is_err());
    }
}
