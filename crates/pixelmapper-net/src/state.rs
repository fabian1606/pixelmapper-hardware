//! Serializable state of discovered nodes, built from ArtPollReply packets (`protocol/contract.md` §3).
//!
//! A node sends one ArtPollReply per BindIndex (one port each). The controller merges them into one
//! [`NodeState`] keyed by the node's RDM UID (`DefaultRespUID`), falling back to the MAC and then the IP.

use crate::artnet::{ArtPollReply, PortAddress};
use crate::rdm::Uid;
use serde::{Serialize, Serializer};
use std::fmt;
use std::net::{Ipv4Addr, SocketAddr};
use std::str::FromStr;

/// Stable identity of a node. Serialized as text: `"7FF0:80123456"` (UID), `"mac:02:00:00:12:34:56"`,
/// or `"ip:2.0.0.10:6454"`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum NodeId {
    /// The node's RDM UID from `DefaultRespUID` (preferred).
    Uid(Uid),
    /// MAC address, when the node reports no UID.
    Mac([u8; 6]),
    /// UDP address, when the node reports neither.
    Addr(SocketAddr),
}

impl fmt::Display for NodeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            NodeId::Uid(u) => write!(f, "{u}"),
            NodeId::Mac(m) => write!(f, "mac:{}", format_mac(m)),
            NodeId::Addr(a) => write!(f, "ip:{a}"),
        }
    }
}

impl FromStr for NodeId {
    type Err = crate::Error;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let bad = || crate::Error::InvalidArgument(format!("bad node id {s:?}"));
        if let Some(m) = s.strip_prefix("mac:") {
            let parts: Vec<u8> = m
                .split(':')
                .map(|p| u8::from_str_radix(p, 16))
                .collect::<Result<_, _>>()
                .map_err(|_| bad())?;
            return <[u8; 6]>::try_from(parts)
                .map(NodeId::Mac)
                .map_err(|_| bad());
        }
        if let Some(a) = s.strip_prefix("ip:") {
            return a.parse().map(NodeId::Addr).map_err(|_| bad());
        }
        s.parse().map(NodeId::Uid).map_err(|_| bad())
    }
}

impl Serialize for NodeId {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.collect_str(self)
    }
}

impl<'de> serde::Deserialize<'de> for NodeId {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        s.parse().map_err(serde::de::Error::custom)
    }
}

/// `aa:bb:cc:dd:ee:ff`.
pub fn format_mac(m: &[u8; 6]) -> String {
    m.iter()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(":")
}

fn ser_mac<S: Serializer>(m: &[u8; 6], s: S) -> Result<S::Ok, S::Error> {
    s.collect_str(&format_mac(m))
}

/// Front-panel indicator state (Status1 bits 7-6).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Indicators {
    /// 00.
    Unknown,
    /// 01: Locate/Identify.
    Locate,
    /// 10: Mute.
    Mute,
    /// 11: Normal.
    Normal,
}

/// Failsafe behaviour (Status3 bits 7-6).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Failsafe {
    /// Hold last state.
    Hold,
    /// All outputs zero.
    Zero,
    /// All outputs full.
    Full,
    /// Play failsafe scene.
    Scene,
}

/// Decoded Status1.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status1 {
    /// Raw byte.
    pub raw: u8,
    /// Indicator state.
    pub indicators: Indicators,
    /// Port-Address authority (0 unknown, 1 front panel, 2 network).
    pub port_address_authority: u8,
    /// Booted from ROM.
    pub rom_boot: bool,
    /// RDM capable.
    pub rdm_capable: bool,
    /// UBEA present.
    pub ubea_present: bool,
}

impl From<u8> for Status1 {
    fn from(v: u8) -> Self {
        Self {
            raw: v,
            indicators: match v >> 6 {
                1 => Indicators::Locate,
                2 => Indicators::Mute,
                3 => Indicators::Normal,
                _ => Indicators::Unknown,
            },
            port_address_authority: (v >> 4) & 3,
            rom_boot: v & 0x04 != 0,
            rdm_capable: v & 0x02 != 0,
            ubea_present: v & 0x01 != 0,
        }
    }
}

/// Decoded Status2.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status2 {
    /// Raw byte.
    pub raw: u8,
    /// RDM switchable via ArtAddress.
    pub rdm_switchable: bool,
    /// Output style switchable via ArtAddress.
    pub output_style_switchable: bool,
    /// Squawking.
    pub squawking: bool,
    /// Art-Net/sACN switchable.
    pub sacn_switchable: bool,
    /// 15-bit Port-Address support.
    pub port_address_15bit: bool,
    /// DHCP capable.
    pub dhcp_capable: bool,
    /// IP obtained by DHCP.
    pub dhcp_active: bool,
    /// Web configuration supported.
    pub web_config: bool,
}

impl From<u8> for Status2 {
    fn from(v: u8) -> Self {
        Self {
            raw: v,
            rdm_switchable: v & 0x80 != 0,
            output_style_switchable: v & 0x40 != 0,
            squawking: v & 0x20 != 0,
            sacn_switchable: v & 0x10 != 0,
            port_address_15bit: v & 0x08 != 0,
            dhcp_capable: v & 0x04 != 0,
            dhcp_active: v & 0x02 != 0,
            web_config: v & 0x01 != 0,
        }
    }
}

/// Decoded Status3.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status3 {
    /// Raw byte.
    pub raw: u8,
    /// Failsafe mode.
    pub failsafe: Failsafe,
    /// Programmable failsafe supported.
    pub failsafe_programmable: bool,
    /// LLRP supported.
    pub llrp: bool,
    /// Port direction switchable (PortTypes shows the current direction).
    pub direction_switchable: bool,
    /// RDMnet supported.
    pub rdmnet: bool,
    /// BackgroundQueue supported.
    pub background_queue: bool,
    /// Background discovery switchable via ArtAddress.
    pub background_discovery_switchable: bool,
}

impl From<u8> for Status3 {
    fn from(v: u8) -> Self {
        Self {
            raw: v,
            failsafe: match v >> 6 {
                0 => Failsafe::Hold,
                1 => Failsafe::Zero,
                2 => Failsafe::Full,
                _ => Failsafe::Scene,
            },
            failsafe_programmable: v & 0x20 != 0,
            llrp: v & 0x10 != 0,
            direction_switchable: v & 0x08 != 0,
            rdmnet: v & 0x04 != 0,
            background_queue: v & 0x02 != 0,
            background_discovery_switchable: v & 0x01 != 0,
        }
    }
}

/// Current direction of a port.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum PortDirection {
    /// Art-Net -> DMX.
    Output,
    /// DMX -> Art-Net.
    Input,
    /// Port types says neither (disabled / unused slot).
    None,
}

/// Merge mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum MergeMode {
    /// Highest takes precedence (default).
    Htp,
    /// Latest takes precedence.
    Ltp,
}

/// Output style.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum OutputStyle {
    /// Output only on ArtDmx.
    Delta,
    /// Continuous output.
    Continuous,
}

/// One DMX port of a node.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PortState {
    /// BindIndex of the ArtPollReply this port came from (1 = root).
    pub bind_index: u8,
    /// Index inside that reply (0..3; always 0 for our node).
    pub index: u8,
    /// Physical port number 1..n: `(bind_index - 1) * num_ports + index + 1` (BOOT_CATCH_START port).
    pub port_number: u8,
    /// Current direction.
    pub direction: PortDirection,
    /// Can output DMX (PortTypes bit 7).
    pub can_output: bool,
    /// Can input DMX (PortTypes bit 6).
    pub can_input: bool,
    /// Protocol (PortTypes bits 5-0, 0 = DMX512).
    pub protocol: u8,
    /// Port-Address the port uses in its current direction.
    pub port_address: PortAddress,
    /// Input Port-Address (Net:Sub:SwIn of this reply), whatever the current direction.
    pub sw_in: PortAddress,
    /// Output Port-Address (Net:Sub:SwOut of this reply), whatever the current direction.
    pub sw_out: PortAddress,
    /// Status3 of the reply this port came from (the node reports failsafe etc. per BindIndex page).
    pub status3: Status3,
    /// RDM enabled (GoodOutputB bit 7 clear).
    pub rdm_enabled: bool,
    /// Output is merging (GoodOutputA bit 3).
    pub merging: bool,
    /// Merge mode (GoodOutputA bit 1).
    pub merge_mode: MergeMode,
    /// Output: ArtDmx data being output (GoodOutputA bit 7). Input: DMX received (GoodInput bit 7).
    pub data_active: bool,
    /// Input disabled (GoodInput bit 3).
    pub input_disabled: bool,
    /// Input receive errors (GoodInput bit 2).
    pub input_errors: bool,
    /// Output style (GoodOutputB bit 6).
    pub output_style: OutputStyle,
    /// RDM discovery currently running (GoodOutputB bit 5 clear).
    pub discovery_running: bool,
    /// Background discovery enabled (GoodOutputB bit 4 clear).
    pub background_discovery: bool,
    /// Raw GoodInput.
    pub good_input: u8,
    /// Raw GoodOutputA.
    pub good_output_a: u8,
    /// Raw GoodOutputB.
    pub good_output_b: u8,
}

/// Everything known about one node, merged from all its BindIndex replies.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NodeState {
    /// Identity.
    pub id: NodeId,
    /// RDM UID of the built-in responder (`DefaultRespUID`), if reported.
    pub uid: Option<Uid>,
    /// IP from the reply.
    pub ip: Ipv4Addr,
    /// UDP address the replies come from (where requests are sent).
    pub address: SocketAddr,
    /// MAC.
    #[serde(serialize_with = "ser_mac")]
    pub mac: [u8; 6],
    /// Short name.
    pub short_name: String,
    /// Long name.
    pub long_name: String,
    /// Last NodeReport text.
    pub node_report: String,
    /// Firmware version (VersInfo).
    pub firmware_version: u16,
    /// UBEA version (0 = no UBEA).
    pub ubea_version: u8,
    /// ESTA manufacturer.
    pub esta_man: u16,
    /// OEM code.
    pub oem: u16,
    /// Style (0 = StNode).
    pub style: u8,
    /// Max ArtDmx refresh rate.
    pub refresh_rate: u16,
    /// Bind IP (root device IP).
    pub bind_ip: Ipv4Addr,
    /// Decoded Status1 (of the root reply).
    pub status1: Status1,
    /// Decoded Status2.
    pub status2: Status2,
    /// Decoded Status3 of the root (lowest BindIndex) reply; per page see [`PortState::status3`].
    pub status3: Status3,
    /// Ports sorted by (bind_index, index).
    pub ports: Vec<PortState>,
}

impl NodeState {
    /// The port with this Port-Address (output preferred).
    pub fn port(&self, pa: PortAddress) -> Option<&PortState> {
        self.ports
            .iter()
            .find(|p| p.port_address == pa && p.direction == PortDirection::Output)
            .or_else(|| self.ports.iter().find(|p| p.port_address == pa))
    }

    /// Ports of one BindIndex.
    pub fn bind_ports(&self, bind_index: u8) -> impl Iterator<Item = &PortState> {
        self.ports
            .iter()
            .filter(move |p| p.bind_index == bind_index)
    }
}

/// Identity of a reply: UID, else MAC, else source address.
pub fn reply_node_id(r: &ArtPollReply, src: SocketAddr) -> NodeId {
    if r.default_resp_uid != Uid::default() {
        NodeId::Uid(r.default_resp_uid)
    } else if r.mac != [0; 6] {
        NodeId::Mac(r.mac)
    } else {
        NodeId::Addr(src)
    }
}

/// The ArtPollReply counter of a NodeReport `"#xxxx [yyyy] text"` (Art-Net 4: decimal, counts every
/// ArtPollReply the node sends, restarts with the node). `None` if the text has no counter.
pub fn report_counter(node_report: &str) -> Option<u16> {
    let rest = node_report.strip_prefix('#')?;
    let open = rest.find('[')?;
    let close = rest[open..].find(']')? + open;
    rest[open + 1..close].trim().parse().ok()
}

/// Whether the NodeReport counter going from `prev` to `now` means the node restarted: it went down, and not
/// by the wrap-around 9999 -> 1 (a restart close to the wrap is not detected; a reordered reply only causes a
/// harmless extra ArtTodRequest).
pub fn report_counter_restarted(prev: u16, now: u16) -> bool {
    now < prev && !(prev >= 9900 && now <= 100)
}

/// Ports described by one reply (`NumPorts` ≤ 4 entries).
pub fn reply_ports(r: &ArtPollReply) -> Vec<PortState> {
    let n = usize::from(r.num_ports.min(4));
    let bind = r.bind_index.max(1);
    (0..n)
        .map(|i| {
            let pt = r.port_types[i];
            let (can_output, can_input) = (pt & 0x80 != 0, pt & 0x40 != 0);
            let gi = r.good_input[i];
            let ga = r.good_output_a[i];
            let gb = r.good_output_b[i];
            let direction = match (can_output, can_input) {
                (true, false) => PortDirection::Output,
                (false, true) => PortDirection::Input,
                // both bits: not switchable or unknown -> output if it is outputting, else input if receiving
                (true, true) if gi & 0x80 != 0 && ga & 0x80 == 0 => PortDirection::Input,
                (true, true) => PortDirection::Output,
                (false, false) => PortDirection::None,
            };
            let port_address = if direction == PortDirection::Input {
                r.port_address_in(i)
            } else {
                r.port_address_out(i)
            };
            PortState {
                bind_index: bind,
                index: i as u8,
                port_number: ((usize::from(bind) - 1) * n + i + 1).min(255) as u8,
                direction,
                can_output,
                can_input,
                protocol: pt & 0x3F,
                port_address,
                sw_in: r.port_address_in(i),
                sw_out: r.port_address_out(i),
                status3: r.status3.into(),
                rdm_enabled: gb & 0x80 == 0,
                merging: ga & 0x08 != 0,
                merge_mode: if ga & 0x02 != 0 {
                    MergeMode::Ltp
                } else {
                    MergeMode::Htp
                },
                data_active: if direction == PortDirection::Input {
                    gi & 0x80 != 0
                } else {
                    ga & 0x80 != 0
                },
                input_disabled: gi & 0x08 != 0,
                input_errors: gi & 0x04 != 0,
                output_style: if gb & 0x40 != 0 {
                    OutputStyle::Continuous
                } else {
                    OutputStyle::Delta
                },
                discovery_running: gb & 0x20 == 0,
                background_discovery: gb & 0x10 == 0,
                good_input: gi,
                good_output_a: ga,
                good_output_b: gb,
            }
        })
        .collect()
}

/// Build a node state from its replies (any order; the root/lowest BindIndex supplies node-wide fields).
pub fn merge_replies(id: NodeId, replies: &[(SocketAddr, ArtPollReply)]) -> Option<NodeState> {
    let (addr, root) = replies.iter().min_by_key(|(_, r)| r.bind_index.max(1))?;
    let mut ports: Vec<PortState> = replies.iter().flat_map(|(_, r)| reply_ports(r)).collect();
    ports.sort_by_key(|p| (p.bind_index, p.index));
    Some(NodeState {
        id,
        uid: (root.default_resp_uid != Uid::default()).then_some(root.default_resp_uid),
        ip: root.ip,
        address: *addr,
        mac: root.mac,
        short_name: root.short_name.clone(),
        long_name: root.long_name.clone(),
        node_report: root.node_report.clone(),
        firmware_version: root.vers_info,
        ubea_version: root.ubea_version,
        esta_man: root.esta_man,
        oem: root.oem,
        style: root.style,
        refresh_rate: root.refresh_rate,
        bind_ip: root.bind_ip,
        status1: root.status1.into(),
        status2: root.status2.into(),
        status3: root.status3.into(),
        ports,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn node_report_counter() {
        assert_eq!(
            report_counter("#0001 [0042] Power On Tests successful"),
            Some(42)
        );
        assert_eq!(report_counter("#0001 [9999] x"), Some(9999));
        assert_eq!(report_counter("no counter"), None);
        assert_eq!(report_counter(""), None);
        assert!(report_counter_restarted(57, 1));
        assert!(!report_counter_restarted(57, 58));
        assert!(!report_counter_restarted(57, 57));
        assert!(!report_counter_restarted(9998, 3), "wrap-around");
    }

    fn reply(bind: u8, uni: u8) -> ArtPollReply {
        ArtPollReply {
            ip: Ipv4Addr::new(2, 0, 0, 10),
            short_name: "PM Node".into(),
            num_ports: 1,
            port_types: [0x80, 0, 0, 0],
            sw_out: [uni, 0, 0, 0],
            good_output_a: [0x80, 0, 0, 0],
            good_output_b: [0x60, 0, 0, 0],
            bind_index: bind,
            default_resp_uid: Uid::new(0x7FF0, 0x8012_3456),
            mac: [2, 0, 0, 0x12, 0x34, 0x56],
            status1: 0xE2,
            status3: 0x28,
            ..Default::default()
        }
    }

    #[test]
    fn merge_four_binds() {
        let src: SocketAddr = "2.0.0.10:6454".parse().unwrap();
        let replies: Vec<_> = (1..=4).rev().map(|b| (src, reply(b, b))).collect();
        let id = reply_node_id(&replies[0].1, src);
        assert_eq!(id.to_string(), "7FF0:80123456");
        let s = merge_replies(id, &replies).unwrap();
        assert_eq!(s.ports.len(), 4);
        assert_eq!(s.ports[2].port_number, 3);
        assert_eq!(s.ports[2].port_address.raw(), 3);
        assert!(s.ports[0].rdm_enabled && s.ports[0].data_active);
        assert_eq!(s.ports[0].output_style, OutputStyle::Continuous);
        assert!(!s.ports[0].discovery_running);
        assert_eq!(s.status1.indicators, Indicators::Normal);
        assert!(s.status3.direction_switchable);
        let j = serde_json::to_value(&s).unwrap();
        assert_eq!(j["id"], "7FF0:80123456");
        assert_eq!(j["mac"], "02:00:00:12:34:56");
        assert_eq!(j["ports"][1]["portAddress"], 2);
        assert_eq!(j["status1"]["rdmCapable"], true);
    }

    #[test]
    fn status3_sw_in_out_per_bind() {
        let src: SocketAddr = "2.0.0.10:6454".parse().unwrap();
        let replies: Vec<_> = (1..=4u8)
            .map(|b| {
                let mut r = reply(b, b);
                r.status3 = ((b - 1) << 6) | 0x28; // Hold, Zero, Full, Scene
                r.sw_in = [b + 4, 0, 0, 0];
                r.ubea_version = 7;
                (src, r)
            })
            .collect();
        let s = merge_replies(reply_node_id(&replies[0].1, src), &replies).unwrap();
        let fs: Vec<Failsafe> = s.ports.iter().map(|p| p.status3.failsafe).collect();
        assert_eq!(
            fs,
            [
                Failsafe::Hold,
                Failsafe::Zero,
                Failsafe::Full,
                Failsafe::Scene
            ]
        );
        assert_eq!(s.ports[3].sw_out.raw(), 4);
        assert_eq!(s.ports[3].sw_in.raw(), 8);
        assert_eq!(s.ports[3].port_address, s.ports[3].sw_out);
        assert_eq!(s.ubea_version, 7);
        let j = serde_json::to_value(&s).unwrap();
        assert_eq!(j["ubeaVersion"], 7);
        assert_eq!(j["ports"][2]["status3"]["failsafe"], "full");
        assert_eq!(j["ports"][1]["swIn"], 6);
        assert_eq!(j["ports"][1]["swOut"], 2);
    }

    #[test]
    fn node_id_text() {
        for s in [
            "7FF0:80123456",
            "mac:02:00:00:12:34:56",
            "ip:127.0.0.1:6454",
        ] {
            assert_eq!(s.parse::<NodeId>().unwrap().to_string(), s);
        }
        assert!("mac:1:2".parse::<NodeId>().is_err());
    }
}
