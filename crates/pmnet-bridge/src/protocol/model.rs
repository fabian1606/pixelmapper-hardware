//! Decoded state shapes of the bridge contract (`protocol/bridge-ws.md` §4): bridge status, nodes, ports, TODs,
//! RDM descriptions and values, firmware jobs. These are the types the `NetBackend` produces and the server
//! serializes 1:1; field names follow the contract (camelCase).

use super::error::ErrorCode;
use super::ids::{MacAddress, NodeId, PortRef, RdmUid};
use serde::{Deserialize, Serialize};
use std::net::Ipv4Addr;

// ── Bridge ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// A network interface of the host running the bridge.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NetInterface {
    /// OS name, e.g. `"en7"`.
    pub name: String,
    /// IPv4 address.
    pub ip: Ipv4Addr,
    /// Netmask.
    pub mask: Ipv4Addr,
    /// Hardware address, `None` when unknown.
    pub mac: Option<MacAddress>,
}

/// State of the bridge's Art-Net UDP socket (port 6454).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UdpState {
    /// True when the socket is bound.
    pub bound: bool,
    /// UDP port (6454).
    pub port: u16,
    /// Bind error, e.g. `"address in use"`.
    pub error: Option<String>,
}

/// Interfaces + UDP state, sent in `hello` and `bridgeStatus`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BridgeStatus {
    /// Local interfaces.
    pub interfaces: Vec<NetInterface>,
    /// Art-Net socket state.
    pub udp: UdpState,
}

// ── Nodes ─────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Indicator state (Status1 bits 7–6).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum Indicator {
    Unknown,
    Locate,
    Mute,
    Normal,
}

/// Port-Address programming authority (Status1 bits 5–4).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum PortAddressAuthority {
    Unknown,
    Panel,
    Network,
}

/// Failsafe mode of a port (Status3 bits 7–6 of the port's ArtPollReply, ArtAddress AcFail* to its BindIndex).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum Failsafe {
    Hold,
    Zero,
    Full,
    Scene,
}

/// Node style (ArtPollReply `Style`, StNode …).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum NodeStyle {
    Node,
    Controller,
    Media,
    Route,
    Backup,
    Config,
    Visual,
    Unknown,
}

/// Decoded Status1/2/3 of the node's root ArtPollReply.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct NodeStatus {
    pub indicator: Indicator,
    pub port_address_authority: PortAddressAuthority,
    pub booted_from_rom: bool,
    pub rdm_capable: bool,
    pub ubea_present: bool,
    pub web_config: bool,
    pub dhcp_configured: bool,
    pub dhcp_capable: bool,
    #[serde(rename = "portAddress15Bit")]
    pub port_address_15_bit: bool,
    pub sacn_switchable: bool,
    pub squawking: bool,
    pub output_style_switchable: bool,
    pub rdm_switchable: bool,
    /// Status3 bit 5 of the root reply (the mode itself is per port: [`PortState::failsafe`]).
    pub failsafe_programmable: bool,
    pub llrp: bool,
    pub port_direction_switchable: bool,
    pub rdmnet: bool,
    pub background_queue: bool,
    pub background_discovery_control: bool,
}

/// Raw status bytes of the root reply.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RawStatus {
    pub status1: u8,
    pub status2: u8,
    pub status3: u8,
}

/// Port protocol (PortTypes bits 5–0).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum PortProtocol {
    Dmx512,
    Midi,
    Avab,
    Cmx,
    Adb,
    Artnet,
    Dali,
    Unknown,
}

/// Direction of a port from PortTypes bits 7/6. With Status3 bit 3 set these bits show the *current* direction
/// (node contract §3), so a switchable port is `Output` or `Input`; `Both` only for fixed ports that do both.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum PortDirection {
    Output,
    Input,
    Both,
    None,
}

/// Merge mode of an output.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum MergeMode {
    Htp,
    Ltp,
}

/// Output style of an output.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum OutputStyle {
    Delta,
    Continuous,
}

/// Output flags of a port (GoodOutputA/B).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct PortOutputState {
    pub data: bool,
    pub merging: bool,
    pub merge: MergeMode,
    pub short_circuit: bool,
    pub sacn: bool,
    pub rdm: bool,
    pub style: OutputStyle,
    pub discovery_running: bool,
    pub background_discovery: bool,
}

/// Input flags of a port (GoodInput).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct PortInputState {
    pub data: bool,
    pub disabled: bool,
    pub errors: bool,
    pub sacn: bool,
}

/// One port of a node.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct PortState {
    pub port: PortRef,
    pub protocol: PortProtocol,
    pub can_output: bool,
    pub can_input: bool,
    pub direction: PortDirection,
    /// NetSwitch, SubSwitch, SwOut of the port.
    pub output_port_address: u16,
    /// NetSwitch, SubSwitch, SwIn of the port.
    pub input_port_address: u16,
    /// Failsafe mode (Status3 bits 7–6 of this port's reply); `None` when not known.
    pub failsafe: Option<Failsafe>,
    pub output: PortOutputState,
    pub input: PortInputState,
}

/// One Art-Net node, merged from all its ArtPollReplies (one per `BindIndex`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct NodeState {
    pub node_id: NodeId,
    /// DefaultRespUID; `None` when all-zero.
    pub uid: Option<RdmUid>,
    /// `None` when all-zero.
    pub mac: Option<MacAddress>,
    pub ip: Ipv4Addr,
    pub interface: Option<String>,
    pub short_name: String,
    pub long_name: String,
    pub node_report: String,
    /// VersInfoH << 8 | VersInfoL.
    pub firmware: u16,
    pub esta_man: u16,
    pub oem: u16,
    pub ubea_version: u8,
    pub style: NodeStyle,
    pub refresh_rate: u16,
    pub status: NodeStatus,
    pub raw: RawStatus,
    /// Every port of every BindIndex, ordered by bind, then index.
    pub ports: Vec<PortState>,
    /// Last reply, ms since the Unix epoch.
    pub last_seen: u64,
}

impl NodeState {
    /// The port with the given reference, if the node has it.
    pub fn port(&self, port: PortRef) -> Option<&PortState> {
        self.ports.iter().find(|p| p.port == port)
    }

    /// True when both states are equal apart from the volatile fields `lastSeen` and the ArtPollReply counter in
    /// `nodeReport` (no `node` event needed, contract §5).
    pub fn same_state(&self, other: &NodeState) -> bool {
        let mut a = self.clone();
        a.last_seen = other.last_seen;
        if node_report_without_counter(&a.node_report)
            == node_report_without_counter(&other.node_report)
        {
            a.node_report.clone_from(&other.node_report);
        }
        &a == other
    }

    /// Takes over the volatile fields (`lastSeen`, `nodeReport`) of `newer`, a state with [`Self::same_state`].
    pub fn refresh_volatile(&mut self, newer: &NodeState) {
        self.last_seen = newer.last_seen;
        self.node_report.clone_from(&newer.node_report);
    }
}

/// A NodeReport without its ArtPollReply counter: `"#0001 [0123] OK"` → `"#0001 [] OK"` (Art-Net 4: the decimal
/// counter in brackets counts every ArtPollReply of the node). Other texts are returned unchanged.
pub fn node_report_without_counter(report: &str) -> std::borrow::Cow<'_, str> {
    let counter = report.find('[').and_then(|open| {
        let close = open + report[open..].find(']')?;
        let inner = &report[open + 1..close];
        (!inner.trim().is_empty() && inner.trim().bytes().all(|b| b.is_ascii_digit()))
            .then_some((open + 1, close))
    });
    match counter {
        Some((start, end)) => format!("{}{}", &report[..start], &report[end..]).into(),
        None => report.into(),
    }
}

/// Table of devices of one port (ArtTodData, all blocks assembled).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Tod {
    /// Node the TOD belongs to.
    pub node_id: NodeId,
    /// Port of that node.
    pub port: PortRef,
    /// Port-Address the ArtTodData was sent for.
    pub port_address: u16,
    /// False when the node answered TodNak.
    pub available: bool,
    /// Sorted ascending; empty when `!available`.
    pub uids: Vec<RdmUid>,
}

// ── RDM ───────────────────────────────────────────────────────────────────────────────────────────────────────────

/// DEVICE_INFO (0x0060).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmDeviceInfo {
    pub rdm_protocol_version: u16,
    pub device_model_id: u16,
    pub product_category: u16,
    pub software_version_id: u32,
    pub dmx_footprint: u16,
    /// 1-based.
    pub current_personality: u8,
    pub personality_count: u8,
    /// 1–512; 65535 = none.
    pub dmx_start_address: u16,
    pub sub_device_count: u16,
    pub sensor_count: u8,
}

/// DMX_PERSONALITY_DESCRIPTION (0x00E1).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmPersonality {
    /// 1-based.
    pub index: u8,
    pub footprint: u16,
    pub description: Option<String>,
}

/// SLOT_INFO + SLOT_DESCRIPTION + DEFAULT_SLOT_VALUE of one slot.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmSlot {
    pub offset: u16,
    #[serde(rename = "type")]
    pub slot_type: u8,
    pub label_id: u16,
    pub description: Option<String>,
    pub default_value: Option<u8>,
}

/// Data type names of PARAMETER_DESCRIPTION (E1.20 Table A-15).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum RdmDataTypeName {
    NotDefined,
    BitField,
    Ascii,
    Uint8,
    Int8,
    Uint16,
    Int16,
    Uint32,
    Int32,
    Manufacturer,
}

/// Command class of a parameter.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum RdmCommandClass {
    Get,
    Set,
    GetSet,
}

/// PARAMETER_DESCRIPTION (0x0051).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmParameterDescription {
    pub pid: u16,
    pub pdl_size: u8,
    pub data_type: u8,
    pub data_type_name: RdmDataTypeName,
    pub command_class: RdmCommandClass,
    #[serde(rename = "type")]
    pub param_type: u8,
    pub unit: u8,
    pub prefix: u8,
    /// int32 for signed data types, uint32 otherwise.
    pub min_value: i64,
    pub max_value: i64,
    pub default_value: i64,
    pub description: String,
}

/// Status type of a STATUS_MESSAGES entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum RdmStatusType {
    None,
    Advisory,
    Warning,
    Error,
    AdvisoryCleared,
    WarningCleared,
    ErrorCleared,
}

/// One STATUS_MESSAGES (0x0030) entry.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmStatusMessage {
    pub sub_device: u16,
    pub status_type: RdmStatusType,
    pub status_message_id: u16,
    pub data_value1: i16,
    pub data_value2: i16,
    pub text: Option<String>,
}

/// A PID that failed during `rdmDescribe`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmDescribeError {
    pub pid: u16,
    pub code: ErrorCode,
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub nack_reason: Option<u16>,
}

/// Result of `rdmDescribe`: everything the UI needs about one device.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmDeviceDescription {
    pub uid: RdmUid,
    pub device_info: RdmDeviceInfo,
    pub manufacturer_label: Option<String>,
    pub device_model_description: Option<String>,
    pub device_label: Option<String>,
    pub software_version_label: Option<String>,
    pub boot_software_version_id: Option<u32>,
    pub boot_software_version_label: Option<String>,
    pub personalities: Vec<RdmPersonality>,
    pub slots: Vec<RdmSlot>,
    pub supported_parameters: Vec<u16>,
    pub parameter_descriptions: Vec<RdmParameterDescription>,
    pub errors: Vec<RdmDescribeError>,
}

/// `DMX_PERSONALITY` GET result.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct RdmPersonalityState {
    pub current: u8,
    pub count: u8,
}

/// `SLOT_DESCRIPTION` GET result.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct RdmSlotDescription {
    pub offset: u16,
    pub description: String,
}

/// One `SLOT_INFO` entry.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct RdmSlotInfo {
    pub offset: u16,
    #[serde(rename = "type")]
    pub slot_type: u8,
    pub label_id: u16,
}

/// One `DEFAULT_SLOT_VALUE` entry.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct RdmDefaultSlotValue {
    pub offset: u16,
    pub value: u8,
}

/// Decoded parameter data (`RdmValue` table of the contract, §4). Untagged: the JSON shape alone identifies it.
///
/// Inputs from the app are only ever scalars (`number`, `string`, `boolean`); the structured variants are produced
/// by the backend. Variant order matters for deserialization (first match wins).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(untagged)]
#[allow(missing_docs)]
pub enum RdmValue {
    Bool(bool),
    Integer(i64),
    Text(String),
    Integers(Vec<i64>),
    DeviceInfo(Box<RdmDeviceInfo>),
    ParameterDescription(Box<RdmParameterDescription>),
    Personality(RdmPersonality),
    PersonalityState(RdmPersonalityState),
    SlotDescription(RdmSlotDescription),
    StatusMessages(Vec<RdmStatusMessage>),
    SlotInfo(Vec<RdmSlotInfo>),
    DefaultSlotValues(Vec<RdmDefaultSlotValue>),
}

// ── Firmware ──────────────────────────────────────────────────────────────────────────────────────────────────────

/// Trailer of a tube firmware image (node contract §9.1, `image-format.md` §1).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct FirmwareImageInfo {
    pub format: u8,
    pub manufacturer_id: u16,
    pub model_id: u16,
    /// FW_VERSION_ID 0x00MMmmpp.
    pub version_id: u32,
    /// `"M.m.p"`.
    pub version: String,
    pub length: u32,
}

/// Phase of a firmware job, in order; the last three are terminal.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub enum FlashPhase {
    Entering,
    Writing,
    Verifying,
    Finishing,
    Confirming,
    Done,
    Failed,
    Cancelled,
}

impl FlashPhase {
    /// True for `done`, `failed` and `cancelled`.
    pub fn is_terminal(self) -> bool {
        matches!(
            self,
            FlashPhase::Done | FlashPhase::Failed | FlashPhase::Cancelled
        )
    }
}

/// Error of a failed firmware job.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[allow(missing_docs)]
pub struct FlashError {
    pub code: ErrorCode,
    pub message: String,
}

/// Result of a finished firmware job.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct FlashResult {
    pub software_version_id: u32,
    pub software_version_label: Option<String>,
}

/// A firmware update job run by the bridge.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(missing_docs)]
pub struct FlashJob {
    pub job_id: String,
    pub node_id: NodeId,
    pub port: PortRef,
    pub uid: RdmUid,
    pub phase: FlashPhase,
    /// Pages written (all-0xFF pages are skipped and not counted).
    pub pages_done: u32,
    pub pages_total: u32,
    /// Chunks rewritten after an APP_CRC mismatch.
    pub rewrites: u32,
    /// True while the tube runs its bootloader (also after failed/cancelled).
    pub in_bootloader: bool,
    pub image: FirmwareImageInfo,
    /// Set when phase = failed.
    pub error: Option<FlashError>,
    /// Set when phase = done.
    pub result: Option<FlashResult>,
}

// ── DMX routes ────────────────────────────────────────────────────────────────────────────────────────────────────

/// One entry of a connection's DMX route table (`dmxRoutes`, contract §6): exactly one of `nodeId` / `ip`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DmxRoute {
    /// Route id used in binary frame sets, 1–65535.
    pub route: u16,
    /// Destination node (follows its IP).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub node_id: Option<NodeId>,
    /// Manual destination IP.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ip: Option<Ipv4Addr>,
    /// Port-Address of the ArtDmx packets, 0–32767.
    pub port_address: u16,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enum_names_match_contract() {
        let names = serde_json::to_string(&(
            PortProtocol::Dmx512,
            RdmDataTypeName::Uint8,
            RdmCommandClass::GetSet,
            RdmStatusType::AdvisoryCleared,
            PortDirection::None,
            FlashPhase::Cancelled,
        ))
        .unwrap();
        assert_eq!(
            names,
            r#"["dmx512","uint8","getSet","advisoryCleared","none","cancelled"]"#
        );
    }

    #[test]
    fn rdm_value_scalars_parse() {
        assert_eq!(
            serde_json::from_str::<RdmValue>("true").unwrap(),
            RdmValue::Bool(true)
        );
        assert_eq!(
            serde_json::from_str::<RdmValue>("17").unwrap(),
            RdmValue::Integer(17)
        );
        assert_eq!(
            serde_json::from_str::<RdmValue>("\"warning\"").unwrap(),
            RdmValue::Text("warning".into())
        );
        let pers: RdmValue = serde_json::from_str(r#"{"current":2,"count":3}"#).unwrap();
        assert_eq!(
            pers,
            RdmValue::PersonalityState(RdmPersonalityState {
                current: 2,
                count: 3
            })
        );
        let slots: RdmValue =
            serde_json::from_str(r#"[{"offset":0,"type":0,"labelId":1}]"#).unwrap();
        assert!(matches!(slots, RdmValue::SlotInfo(_)));
    }

    #[test]
    fn node_report_counter_is_volatile() {
        assert_eq!(
            node_report_without_counter("#0001 [0123] Power On Tests successful"),
            "#0001 [] Power On Tests successful"
        );
        assert_eq!(node_report_without_counter("[x] text"), "[x] text");
        assert_eq!(node_report_without_counter(""), "");
        let node = crate::backend::mock::sample_node("7FF0:00000001", Ipv4Addr::LOCALHOST, 1);
        let mut next = node.clone();
        next.node_report = "#0001 [0002] OK".into();
        next.last_seen += 1;
        assert!(node.same_state(&next));
        next.node_report = "#000e [0003] Firmware update failed".into();
        assert!(!node.same_state(&next));
    }

    #[test]
    fn status_15_bit_field_name() {
        let v = serde_json::to_value(RawStatus {
            status1: 1,
            status2: 2,
            status3: 3,
        })
        .unwrap();
        assert_eq!(v["status3"], 3);
    }
}
