//! The simulated Pixelmapper Node: one tokio task that speaks real Art-Net 4 over UDP exactly as
//! `protocol/contract.md` describes the node (4 ports, one per BindIndex, RDM proxying with a FIFO per port, its own
//! RDM responder, BOOT_CATCH, both firmware-update paths).
//!
//! Addressing: every answer goes to the **source address** of the request (so test sockets on ephemeral ports
//! work); unsolicited packets (ArtPollReply on change, ArtTodData on TOD change) go to the remembered source
//! addresses of earlier ArtPoll (Flags bit 1) / ArtTodRequest / ArtTodControl packets. A reboot (firmware update,
//! RESET_DEVICE) forgets them, like the real node.
//!
//! Lifetime: [`SimNode::start`] spawns the task; it runs until [`SimNode::shutdown`] is called (dropping all
//! handles does **not** stop it, because the task owns a handle to the shared state itself).

use super::node_fw::{FwTarget, FwUpdate, NewFirmware};
use super::node_rdm::{BootCatch, MessageQueue};
use super::SimResponder;
use crate::artnet::{
    self, address_command as ac, ArtAddress, ArtDmx, ArtFirmwareMaster, ArtFirmwareReply, ArtInput,
    ArtIpProg, ArtIpProgReply, ArtPacket, ArtPoll, ArtPollReply, ArtRdm, ArtTodControl, ArtTodData,
    ArtTodRequest, PortAddress, IPPROG_DHCP, IPPROG_GATEWAY, IPPROG_IP, IPPROG_MASK, IPPROG_RESET,
    TOD_FULL,
};
use crate::pids;
use crate::rdm::pd::{BootCatchStatus, LocalRdmSet};
use crate::rdm::{cc, RdmMessage, Uid};
use rand::Rng;
use std::collections::VecDeque;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};
use tokio::net::UdpSocket;
use tokio::sync::{watch, Notify};

/// Number of DMX ports of the simulated node.
pub const SIM_PORTS: usize = 4;
/// Maximum short name length (Art-Net PortName / DEVICE_LABEL of the node).
pub(super) const SHORT_NAME_MAX: usize = 17;
/// Maximum long name length.
const LONG_NAME_MAX: usize = 63;
/// A merge source is dropped after this time without ArtDmx (Art-Net 4, digest §3.2).
const MERGE_SOURCE_TIMEOUT: Duration = Duration::from_secs(10);
/// Remembered pollers / TOD requesters (oldest dropped beyond this).
const MAX_SUBSCRIBERS: usize = 32;
/// Tick period of the node task (responder ticks, FIFO, timers).
const TICK: Duration = Duration::from_millis(1);
/// Art-Net RefreshRate of a DMX512 gateway.
const REFRESH_RATE: u16 = 44;
/// NodeReport code RcPowerOk.
const NODE_REPORT_CODE: u16 = 0x0001;

/// Current direction of a simulated port.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub enum SimDirection {
    /// Art-Net → DMX (with RDM).
    Output,
    /// DMX → Art-Net.
    Input,
}

/// Factory configuration of one port.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SimPortConfig {
    /// Direction.
    pub direction: SimDirection,
    /// Port-Address (Net/Sub-Net/Universe).
    pub port_address: PortAddress,
    /// RDM on the line enabled (ArtAddress AcRdmEnable/Disable).
    pub rdm_enabled: bool,
    /// Port on (PORTn_ENABLED; for inputs also ArtInput).
    pub enabled: bool,
}

impl SimPortConfig {
    /// An enabled output port with RDM at `port_address`.
    pub fn output(port_address: PortAddress) -> Self {
        Self {
            direction: SimDirection::Output,
            port_address,
            rdm_enabled: true,
            enabled: true,
        }
    }
}

/// Configuration of a [`SimNode`].
#[derive(Debug, Clone)]
pub struct SimConfig {
    /// Local socket address. Port 0 = ephemeral (tests run in parallel); the real node uses 6454.
    pub bind: SocketAddr,
    /// IP reported in ArtPollReply/ArtIpProgReply while DHCP is on; default = the bound IP (or the Art-Net
    /// fallback 2.x.y.z when bound to 0.0.0.0).
    pub reply_ip: Option<Ipv4Addr>,
    /// Ethernet MAC; the node UID is `7FF0:(NODE_UID_DEVICE_FLAG | lower 28 bits of the MAC)`.
    pub mac: [u8; 6],
    /// Factory short name (≤ 17 characters).
    pub short_name: String,
    /// Factory long name (≤ 63 characters).
    pub long_name: String,
    /// Running firmware FW_VERSION_ID `0x00MMmmpp` (VersInfo = `MM << 8 | mm`).
    pub fw_version: u32,
    /// Running SOFTWARE_VERSION_LABEL.
    pub version_label: String,
    /// Hardware revision (NODE_HW_REVISION, checked against `hw_rev_mask` of an update).
    pub hw_revision: u8,
    /// Size of the inactive OTA partition (an update image may not be larger).
    pub ota_partition_size: u32,
    /// Factory configuration of the 4 ports (BindIndex 1–4).
    pub ports: [SimPortConfig; SIM_PORTS],
    /// Maximum random delay before ArtPollReply (spec: up to 1 s). Default 0 for tests.
    pub reply_delay_max: Duration,
    /// Duration of one RDM transaction on a port (request + answer on the line). Default 3 ms.
    pub rdm_latency: Duration,
    /// ArtRdm queue size per port (FifoMax). Requests beyond it are dropped. Default 4.
    pub fifo_max: u8,
    /// How long the node is silent after a firmware update or RESET_DEVICE. Default 300 ms.
    pub reboot_delay: Duration,
    /// Period of the simulated background discovery (presence check via [`SimResponder::discoverable`]).
    /// The real node uses RDM_BG_DISCOVERY_INTERVAL (3 s); default 100 ms so tests stay fast.
    pub discovery_interval: Duration,
    /// Firmware update session timeout without data (spec: 30 s).
    pub fw_timeout: Duration,
}

impl Default for SimConfig {
    fn default() -> Self {
        let pa = |n: u8| SimPortConfig::output(PortAddress::from_net_low(0, n));
        Self {
            bind: SocketAddr::from(([127, 0, 0, 1], 0)),
            reply_ip: None,
            mac: [0x02, 0x00, 0x00, 0x12, 0x34, 0x56],
            short_name: "PM Node".into(),
            long_name: "Pixelmapper Node (simulated)".into(),
            fw_version: 0x0000_0100,
            version_label: "0.1.0".into(),
            hw_revision: 0,
            ota_partition_size: 0x0040_0000,
            ports: [pa(1), pa(2), pa(3), pa(4)],
            reply_delay_max: Duration::ZERO,
            rdm_latency: Duration::from_millis(3),
            fifo_max: 4,
            reboot_delay: Duration::from_millis(300),
            discovery_interval: Duration::from_millis(100),
            fw_timeout: Duration::from_secs(30),
        }
    }
}

/// Front panel indicators (ArtAddress AcLed*, IDENTIFY_DEVICE = Locate).
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub enum SimIndicators {
    /// Normal operation.
    Normal,
    /// Display off (LED Mute).
    Mute,
    /// Locate / identify.
    Locate,
}

/// The DMX frame a port currently outputs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SimDmxFrame {
    /// Sequence of the last ArtDmx.
    pub sequence: u8,
    /// Physical of the last ArtDmx.
    pub physical: u8,
    /// Source of the last ArtDmx.
    pub source: SocketAddr,
    /// Output data (merge result while merging).
    pub data: Vec<u8>,
}

/// State of one port in a [`SimSnapshot`].
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SimPortSnapshot {
    /// Current direction.
    pub direction: SimDirection,
    /// Current Port-Address.
    pub port_address: PortAddress,
    /// PORTn_ENABLED (for inputs: not disabled by ArtInput).
    pub enabled: bool,
    /// RDM enabled.
    pub rdm_enabled: bool,
    /// Merge mode LTP (else HTP).
    pub merge_ltp: bool,
    /// Two sources are merged right now.
    pub merging: bool,
    /// Output style continuous (else delta).
    pub continuous: bool,
    /// Background discovery on.
    pub bg_discovery: bool,
    /// PORTn_DMX_BREAK in µs.
    pub dmx_break: u16,
    /// PORTn_DMX_MAB in µs.
    pub dmx_mab: u16,
    /// PORTn_DMX_RATE in Hz.
    pub dmx_rate: u8,
    /// PORTn_DMX_MBS (mark between slots) in µs.
    pub dmx_mbs: u16,
    /// Current TOD.
    pub tod: Vec<Uid>,
}

/// Inspectable state of the simulated node.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SimSnapshot {
    /// Node UID.
    pub uid: Uid,
    /// Short name (= DEVICE_LABEL).
    pub short_name: String,
    /// Long name.
    pub long_name: String,
    /// Running FW_VERSION_ID.
    pub fw_version: u32,
    /// Running SOFTWARE_VERSION_LABEL.
    pub version_label: String,
    /// Hardware revision.
    pub hw_revision: u8,
    /// Indicators (Locate = identify on).
    pub indicators: SimIndicators,
    /// Failsafe mode (0 hold, 1 zero, 2 full, 3 scene).
    pub failsafe: u8,
    /// Current IP.
    pub ip: Ipv4Addr,
    /// Current netmask.
    pub netmask: Ipv4Addr,
    /// Default gateway.
    pub gateway: Ipv4Addr,
    /// DHCP active.
    pub dhcp: bool,
    /// The four ports.
    pub ports: Vec<SimPortSnapshot>,
    /// DISPLAY_LEVEL.
    pub display_level: u8,
    /// DISPLAY_DIM_TIMEOUT in s.
    pub display_dim_timeout: u16,
    /// DISPLAY_ADDRESS_FORMAT.
    pub display_address_format: u8,
    /// PANEL_LOCK.
    pub panel_locked: bool,
    /// RDM_BG_DISCOVERY_INTERVAL in s.
    pub bg_discovery_interval: u16,
    /// FW_INFO.state.
    pub fw_state: u8,
    /// FW_INFO.last_error.
    pub fw_last_error: u8,
    /// FW_INFO.received.
    pub fw_received: u32,
    /// FW_INFO.total.
    pub fw_total: u32,
    /// Silent because of a reboot right now.
    pub rebooting: bool,
    /// Number of (simulated) reboots since start.
    pub boot_count: u32,
    /// BOOT_CATCH_STATUS.
    pub boot_catch: BootCatchStatus,
    /// Queued messages waiting (Message Count).
    pub message_count: u8,
    /// Remembered pollers (unsolicited ArtPollReply).
    pub pollers: Vec<SocketAddr>,
    /// Remembered TOD requesters (unsolicited ArtTodData).
    pub tod_requesters: Vec<SocketAddr>,
}

// ---------------------------------------------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------------------------------------------

pub(super) struct RdmJob {
    requester: SocketAddr,
    request: RdmMessage,
}

/// A transaction on the line; the port is busy until `done`.
pub(super) struct InFlight {
    done: Instant,
    requester: Option<SocketAddr>,
    response: Option<RdmMessage>,
}

struct DmxSource {
    ip: IpAddr,
    physical: u8,
    data: Vec<u8>,
    last: Instant,
}

pub(super) struct Port {
    pub(super) factory: SimPortConfig,
    pub(super) direction: SimDirection,
    pub(super) port_address: PortAddress,
    pub(super) rdm_enabled: bool,
    pub(super) enabled: bool,
    pub(super) merge_ltp: bool,
    pub(super) continuous: bool,
    pub(super) bg_discovery: bool,
    pub(super) dmx_break: u16,
    pub(super) dmx_mab: u16,
    pub(super) dmx_rate: u8,
    pub(super) dmx_mbs: u16,
    pub(super) responders: Vec<Box<dyn SimResponder>>,
    pub(super) tod: Vec<Uid>,
    reported_tod: Vec<Uid>,
    line_was_active: bool,
    fifo: VecDeque<RdmJob>,
    pub(super) in_flight: Option<InFlight>,
    drop_responses: u32,
    dmx: Option<SimDmxFrame>,
    sources: Vec<DmxSource>,
    cancel_merge: bool,
    exclusive: Option<(IpAddr, u8)>,
    output_active: bool,
}

/// Default of a single-value node PID from `pids.toml` (`[pid.pd].default`).
pub(super) fn pid_default(pid: u16) -> u32 {
    pids::PIDS
        .iter()
        .find(|d| d.pid == pid)
        .and_then(|d| d.desc)
        .map_or(0, |d| d.default)
}

impl Port {
    fn new(factory: SimPortConfig) -> Self {
        let mut p = Self {
            factory,
            direction: factory.direction,
            port_address: factory.port_address,
            rdm_enabled: factory.rdm_enabled,
            enabled: factory.enabled,
            merge_ltp: false,
            continuous: true,
            bg_discovery: true,
            dmx_break: 0,
            dmx_mab: 0,
            dmx_rate: 0,
            dmx_mbs: 0,
            responders: Vec::new(),
            tod: Vec::new(),
            reported_tod: Vec::new(),
            line_was_active: false,
            fifo: VecDeque::new(),
            in_flight: None,
            drop_responses: 0,
            dmx: None,
            sources: Vec::new(),
            cancel_merge: false,
            exclusive: None,
            output_active: false,
        };
        p.reset_settings();
        p.line_was_active = p.line_active();
        p
    }

    /// Factory values of everything configurable (FACTORY_DEFAULTS).
    pub(super) fn reset_settings(&mut self) {
        self.direction = self.factory.direction;
        self.port_address = self.factory.port_address;
        self.rdm_enabled = self.factory.rdm_enabled;
        self.enabled = self.factory.enabled;
        self.merge_ltp = false;
        self.continuous = true;
        self.bg_discovery = pid_default(pids::PID_PORT1_RDM_BG_DISCOVERY) != 0;
        self.dmx_break = u16::try_from(pid_default(pids::PID_PORT1_DMX_BREAK)).unwrap_or(176);
        self.dmx_mab = u16::try_from(pid_default(pids::PID_PORT1_DMX_MAB)).unwrap_or(16);
        self.dmx_rate = u8::try_from(pid_default(pids::PID_PORT1_DMX_RATE)).unwrap_or(40);
        self.dmx_mbs = u16::try_from(pid_default(pids::PID_PORT1_DMX_MBS)).unwrap_or(0);
    }

    /// Whether the port's settings equal the factory values.
    pub(super) fn at_factory_settings(&self) -> bool {
        self.direction == self.factory.direction
            && self.port_address == self.factory.port_address
            && self.rdm_enabled == self.factory.rdm_enabled
            && self.enabled == self.factory.enabled
            && !self.merge_ltp
            && self.continuous
            && u32::from(self.bg_discovery) == pid_default(pids::PID_PORT1_RDM_BG_DISCOVERY)
            && u32::from(self.dmx_break) == pid_default(pids::PID_PORT1_DMX_BREAK)
            && u32::from(self.dmx_mab) == pid_default(pids::PID_PORT1_DMX_MAB)
            && u32::from(self.dmx_rate) == pid_default(pids::PID_PORT1_DMX_RATE)
            && u32::from(self.dmx_mbs) == pid_default(pids::PID_PORT1_DMX_MBS)
    }

    /// RDM runs on the line: enabled output with RDM on.
    pub(super) fn line_active(&self) -> bool {
        self.direction == SimDirection::Output && self.enabled && self.rdm_enabled
    }

    /// Full discovery: every discoverable responder (empty TOD when RDM does not run on the port).
    pub(super) fn discover(&mut self) {
        self.tod = if self.line_active() {
            let mut t: Vec<Uid> = self
                .responders
                .iter()
                .filter(|r| r.discoverable())
                .map(|r| r.uid())
                .collect();
            t.sort();
            t.dedup();
            t
        } else {
            Vec::new()
        };
    }

    fn merging(&self) -> bool {
        self.sources.len() >= 2
    }

    /// One request on the DMX line. Broadcast/vendorcast reach every addressed responder and never get an
    /// answer.
    pub(super) fn line_transaction(
        &mut self,
        req: &RdmMessage,
        now: Instant,
    ) -> Option<RdmMessage> {
        let dest = req.dest_uid;
        if dest.is_broadcast() {
            for r in self.responders.iter_mut() {
                if r.uid().addressed_to(dest) {
                    let _ = r.handle(req, now);
                }
            }
            return None;
        }
        self.responders
            .iter_mut()
            .find(|r| r.uid() == dest)
            .and_then(|r| r.handle(req, now))
    }

    /// Occupy the line until `done` (a transaction the network does not see, e.g. BOOT_CATCH).
    pub(super) fn occupy(&mut self, done: Instant) {
        self.in_flight = Some(InFlight {
            done,
            requester: None,
            response: None,
        });
    }

    fn reset_runtime(&mut self) {
        self.fifo.clear();
        self.in_flight = None;
        self.dmx = None;
        self.sources.clear();
        self.cancel_merge = false;
        self.exclusive = None;
        self.output_active = false;
    }

    fn receive_dmx(&mut self, src: SocketAddr, d: &ArtDmx, now: Instant) {
        let key = (src.ip(), d.physical);
        self.sources
            .retain(|s| now.saturating_duration_since(s.last) < MERGE_SOURCE_TIMEOUT);
        if self.cancel_merge {
            self.cancel_merge = false;
            self.sources.retain(|s| (s.ip, s.physical) == key);
            self.exclusive = Some(key);
        }
        if let Some(ex) = self.exclusive {
            if ex != key {
                if self.sources.iter().any(|s| (s.ip, s.physical) == ex) {
                    return;
                }
                self.exclusive = None;
            }
        }
        let n_sources = self.sources.len();
        match self.sources.iter_mut().find(|s| (s.ip, s.physical) == key) {
            Some(s) => {
                s.data.clone_from(&d.data);
                s.last = now;
            }
            None if n_sources < 2 => self.sources.push(DmxSource {
                ip: key.0,
                physical: key.1,
                data: d.data.clone(),
                last: now,
            }),
            // a third source is ignored
            None => return,
        }
        let data = if self.sources.len() >= 2 && !self.merge_ltp {
            let len = self.sources.iter().map(|s| s.data.len()).max().unwrap_or(0);
            (0..len)
                .map(|i| {
                    self.sources
                        .iter()
                        .map(|s| s.data.get(i).copied().unwrap_or(0))
                        .max()
                        .unwrap_or(0)
                })
                .collect()
        } else {
            d.data.clone()
        };
        self.dmx = Some(SimDmxFrame {
            sequence: d.sequence,
            physical: d.physical,
            source: src,
            data,
        });
        self.output_active = true;
    }
}

/// IP configuration (ArtIpProg and E1.37-2 change the same settings).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct IpSettings {
    pub(super) dhcp: bool,
    pub(super) static_ip: Ipv4Addr,
    pub(super) static_mask: Ipv4Addr,
    pub(super) gateway: Ipv4Addr,
}

pub(super) struct IpState {
    /// Settings in effect.
    pub(super) active: IpSettings,
    /// E1.37-2 settings not applied yet (INTERFACE_APPLY_CONFIGURATION).
    pub(super) pending: IpSettings,
    pub(super) factory: IpSettings,
    /// Address "the DHCP server" hands out.
    pub(super) dhcp_ip: Ipv4Addr,
    pub(super) dhcp_mask: Ipv4Addr,
}

impl IpState {
    /// Current address and mask.
    pub(super) fn current(&self) -> (Ipv4Addr, Ipv4Addr) {
        if self.active.dhcp {
            (self.dhcp_ip, self.dhcp_mask)
        } else {
            (self.active.static_ip, self.active.static_mask)
        }
    }

    fn set_active(&mut self, s: IpSettings) {
        self.active = s;
        self.pending = s;
    }
}

/// Netmask → CIDR prefix length.
pub(super) fn mask_to_prefix(mask: Ipv4Addr) -> u8 {
    u32::from(mask).leading_ones() as u8
}

/// CIDR prefix length (≤ 32) → netmask.
pub(super) fn prefix_to_mask(prefix: u8) -> Ipv4Addr {
    let p = u32::from(prefix.min(32));
    Ipv4Addr::from(u32::MAX.checked_shl(32 - p).unwrap_or(0))
}

struct Outgoing {
    due: Instant,
    to: SocketAddr,
    data: Vec<u8>,
}

/// Configurable node-wide settings that only RDM reaches.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct Settings {
    pub(super) display_level: u8,
    pub(super) dim_timeout: u16,
    pub(super) address_format: u8,
    pub(super) panel_lock: bool,
    pub(super) bg_interval: u16,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            display_level: u8::MAX,
            dim_timeout: u16::try_from(pid_default(pids::PID_DISPLAY_DIM_TIMEOUT)).unwrap_or(60),
            address_format: u8::try_from(pid_default(pids::PID_DISPLAY_ADDRESS_FORMAT))
                .unwrap_or(0),
            panel_lock: pid_default(pids::PID_PANEL_LOCK) != 0,
            bg_interval: u16::try_from(pid_default(pids::PID_RDM_BG_DISCOVERY_INTERVAL))
                .unwrap_or(3),
        }
    }
}

/// The whole node state, behind one mutex.
pub(super) struct Node {
    pub(super) cfg: SimConfig,
    pub(super) uid: Uid,
    pub(super) short_name: String,
    pub(super) long_name: String,
    pub(super) fw_version: u32,
    pub(super) version_label: String,
    pub(super) indicators: SimIndicators,
    pub(super) failsafe: u8,
    pub(super) acn_priority: u8,
    pub(super) ports: [Port; SIM_PORTS],
    pub(super) settings: Settings,
    pub(super) ip: IpState,
    pub(super) queue: MessageQueue,
    pub(super) boot_catch: BootCatch,
    pub(super) fw: FwUpdate,
    pub(super) local_set: Option<LocalRdmSet>,
    pub(super) local_seq: u16,
    /// Reboot requested (firmware update / RESET_DEVICE); executed in the next tick after the ACK went out.
    pub(super) pending_reboot: Option<Option<NewFirmware>>,
    pub(super) tn: u8,
    silent_until: Option<Instant>,
    boot_count: u32,
    pollers: Vec<SocketAddr>,
    tod_requesters: Vec<SocketAddr>,
    report_counter: u16,
    outbox: Vec<Outgoing>,
    reported_replies: Vec<Vec<u8>>,
    reported_settings: Vec<(u16, Vec<u8>)>,
    sync_count: u64,
    last_dmx_ip: Option<IpAddr>,
    next_discovery: Instant,
}

pub(super) fn truncate(s: &str, max: usize) -> String {
    let mut end = s.len().min(max);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_owned()
}

fn push_unique(list: &mut Vec<SocketAddr>, a: SocketAddr) {
    if !list.contains(&a) {
        if list.len() >= MAX_SUBSCRIBERS {
            list.remove(0);
        }
        list.push(a);
    }
}

/// ArtRdm carrying `msg`; `None` if the message cannot be encoded (PDL > 231).
fn art_rdm_packet(pa: PortAddress, msg: &RdmMessage, avail: u8, max: u8) -> Option<Vec<u8>> {
    let mut bytes = msg.try_encode().ok()?;
    bytes.remove(0);
    Some(
        ArtRdm {
            rdm_ver: ArtTodData::RDM_VER_1_0,
            fifo_avail: avail,
            fifo_max: max,
            net: pa.net(),
            command: ArtRdm::AR_PROCESS,
            address: pa.sub_uni(),
            rdm_packet: bytes,
        }
        .encode(),
    )
}

impl Node {
    fn new(cfg: SimConfig, local: SocketAddr, now: Instant) -> Self {
        let m = cfg.mac;
        let device = u32::from_be_bytes([m[2], m[3], m[4], m[5]]) & 0x0FFF_FFFF;
        let uid = Uid::new(pids::ESTA_MANUFACTURER, pids::NODE_UID_DEVICE_FLAG | device);
        let [oem_hi, oem_lo] = pids::ARTNET_OEM_NODE.to_be_bytes();
        let fallback = Ipv4Addr::new(
            2,
            m[3].wrapping_add(oem_hi).wrapping_add(oem_lo),
            m[4],
            m[5],
        );
        let class_a = Ipv4Addr::new(255, 0, 0, 0);
        let dhcp_ip = cfg.reply_ip.unwrap_or(match local.ip() {
            IpAddr::V4(v4) if !v4.is_unspecified() => v4,
            _ => fallback,
        });
        let dhcp_mask = match dhcp_ip.octets()[0] {
            2 | 10 | 127 => class_a,
            _ => Ipv4Addr::new(255, 255, 255, 0),
        };
        let factory_ip = IpSettings {
            dhcp: true,
            static_ip: fallback,
            static_mask: class_a,
            gateway: Ipv4Addr::UNSPECIFIED,
        };
        let ports = cfg.ports.map(Port::new);
        let mut node = Self {
            uid,
            short_name: truncate(&cfg.short_name, SHORT_NAME_MAX),
            long_name: truncate(&cfg.long_name, LONG_NAME_MAX),
            fw_version: cfg.fw_version,
            version_label: cfg.version_label.clone(),
            indicators: SimIndicators::Normal,
            failsafe: 0,
            acn_priority: 100,
            ports,
            settings: Settings::default(),
            ip: IpState {
                active: factory_ip,
                pending: factory_ip,
                factory: factory_ip,
                dhcp_ip,
                dhcp_mask,
            },
            queue: MessageQueue::default(),
            boot_catch: BootCatch::default(),
            fw: FwUpdate::default(),
            local_set: None,
            local_seq: 0,
            pending_reboot: None,
            tn: 0,
            silent_until: None,
            boot_count: 0,
            pollers: Vec::new(),
            tod_requesters: Vec::new(),
            report_counter: 0,
            outbox: Vec::new(),
            reported_replies: Vec::new(),
            reported_settings: Vec::new(),
            sync_count: 0,
            last_dmx_ip: None,
            next_discovery: now,
            cfg,
        };
        node.mark_reported();
        node
    }

    /// Take the current state as "already reported" (start, reboot).
    fn mark_reported(&mut self) {
        for p in self.ports.iter_mut() {
            p.line_was_active = p.line_active();
            p.discover();
            p.reported_tod = p.tod.clone();
        }
        self.reported_replies = (0..SIM_PORTS).map(|b| self.reply_fingerprint(b)).collect();
        self.reported_settings = self.rdm_setting_values();
    }

    pub(super) fn fw_target(&self) -> FwTarget {
        FwTarget {
            manufacturer: self.uid.manufacturer,
            hw_revision: self.cfg.hw_revision,
            partition_size: self.cfg.ota_partition_size,
        }
    }

    fn is_silent(&self, now: Instant) -> bool {
        self.silent_until.is_some_and(|t| now < t)
    }

    fn send(&mut self, to: SocketAddr, data: Vec<u8>, due: Instant) {
        self.outbox.push(Outgoing { due, to, data });
    }

    fn take_due(&mut self, now: Instant) -> Vec<(SocketAddr, Vec<u8>)> {
        let mut due = Vec::new();
        let mut rest = Vec::with_capacity(self.outbox.len());
        for o in self.outbox.drain(..) {
            if o.due <= now {
                due.push((o.to, o.data));
            } else {
                rest.push(o);
            }
        }
        self.outbox = rest;
        due
    }

    // -----------------------------------------------------------------------------------------------------------
    // ArtPollReply
    // -----------------------------------------------------------------------------------------------------------

    fn poll_reply(&self, bind: usize) -> ArtPollReply {
        let port = &self.ports[bind];
        let (ip, _) = self.ip.current();
        let pa = port.port_address;
        let mut port_type = 0u8;
        let mut good_input = 0u8;
        let mut good_a = 0u8;
        let mut good_b = 0x20u8; // discovery is not running (it is instantaneous in the simulation)
        match port.direction {
            SimDirection::Output => {
                port_type |= 0x80;
                if port.enabled && port.output_active {
                    good_a |= 0x80;
                }
            }
            SimDirection::Input => {
                port_type |= 0x40;
                if !port.enabled {
                    good_input |= 0x08;
                }
            }
        }
        if port.merging() {
            good_a |= 0x08;
        }
        if port.merge_ltp {
            good_a |= 0x02;
        }
        if !port.rdm_enabled {
            good_b |= 0x80;
        }
        if port.continuous {
            good_b |= 0x40;
        }
        if !port.bg_discovery {
            good_b |= 0x10;
        }
        let indicators = match self.indicators {
            SimIndicators::Locate => 0x40,
            SimIndicators::Mute => 0x80,
            SimIndicators::Normal => 0xC0,
        };
        // Status1: indicators, Port-Address authority = network (10), RDM capable
        let status1 = indicators | 0x20 | 0x02;
        // Status2: RDM and output style switchable via ArtAddress, 15-bit Port-Address, DHCP capable (+ active)
        let status2 = 0x80 | 0x40 | 0x08 | 0x04 | if self.ip.active.dhcp { 0x02 } else { 0 };
        // Status3: failsafe mode, programmable failsafe, port direction switchable
        let status3 = (self.failsafe << 6) | 0x20 | 0x08;
        let fw = self.fw_version;
        ArtPollReply {
            ip,
            port: artnet::PORT,
            vers_info: ((((fw >> 16) & 0xFF) << 8) | ((fw >> 8) & 0xFF)) as u16,
            net_switch: pa.net(),
            sub_switch: pa.sub_net(),
            oem: pids::ARTNET_OEM_NODE,
            ubea_version: 0,
            status1,
            esta_man: pids::ESTA_MANUFACTURER,
            short_name: self.short_name.clone(),
            long_name: self.long_name.clone(),
            node_report: String::new(),
            num_ports: 1,
            port_types: [port_type, 0, 0, 0],
            good_input: [good_input, 0, 0, 0],
            good_output_a: [good_a, 0, 0, 0],
            sw_in: [pa.universe(), 0, 0, 0],
            sw_out: [pa.universe(), 0, 0, 0],
            acn_priority: self.acn_priority,
            sw_macro: 0,
            sw_remote: 0,
            style: 0,
            mac: self.cfg.mac,
            bind_ip: ip,
            bind_index: (bind + 1) as u8,
            status2,
            good_output_b: [good_b, 0, 0, 0],
            status3,
            default_resp_uid: self.uid,
            user: 0,
            refresh_rate: REFRESH_RATE,
            background_queue_policy: 0,
        }
    }

    /// The reply without the NodeReport counter (to detect changes).
    fn reply_fingerprint(&self, bind: usize) -> Vec<u8> {
        self.poll_reply(bind).encode()
    }

    fn push_reply(&mut self, to: SocketAddr, bind: usize, due: Instant) {
        self.report_counter = self.report_counter % 9999 + 1;
        let mut r = self.poll_reply(bind);
        r.node_report = format!(
            "#{NODE_REPORT_CODE:04x} [{:04}] Power On Tests successful",
            self.report_counter
        );
        self.send(to, r.encode(), due);
    }

    // -----------------------------------------------------------------------------------------------------------
    // Change reporting
    // -----------------------------------------------------------------------------------------------------------

    /// Report every change since the last call: unsolicited ArtPollReply to the pollers (and to `direct`, the
    /// sender of an ArtAddress/ArtInput, which always gets its bind), queued messages for RDM-only settings,
    /// unsolicited ArtTodData to the TOD requesters.
    pub(super) fn emit_changes(&mut self, now: Instant, direct: Option<(SocketAddr, usize)>) {
        for p in self.ports.iter_mut() {
            let active = p.line_active();
            if active != p.line_was_active {
                p.line_was_active = active;
                p.discover();
                if !active {
                    p.fifo.clear();
                }
            }
        }
        for b in 0..SIM_PORTS {
            let fp = self.reply_fingerprint(b);
            let changed = fp != self.reported_replies[b];
            if changed {
                self.reported_replies[b] = fp;
                for to in self.pollers.clone() {
                    self.push_reply(to, b, now);
                }
            }
            if let Some((to, db)) = direct {
                let got_it = changed && self.pollers.contains(&to);
                if !got_it && (changed || db == b) {
                    self.push_reply(to, b, now);
                }
            }
        }
        let current = self.rdm_setting_values();
        for (pid, value) in &current {
            let old = self
                .reported_settings
                .iter()
                .find(|(p, _)| p == pid)
                .map(|(_, v)| v);
            if old != Some(value) {
                self.queue.push(*pid, value.clone(), true);
            }
        }
        self.reported_settings = current;
        for i in 0..SIM_PORTS {
            if self.ports[i].tod != self.ports[i].reported_tod {
                self.ports[i].reported_tod = self.ports[i].tod.clone();
                for to in self.tod_requesters.clone() {
                    self.send_tod(i, to, now);
                }
            }
        }
    }

    // -----------------------------------------------------------------------------------------------------------
    // Incoming packets
    // -----------------------------------------------------------------------------------------------------------

    fn handle_packet(&mut self, buf: &[u8], src: SocketAddr, now: Instant) {
        if self.is_silent(now) {
            return;
        }
        let Ok(pkt) = ArtPacket::decode(buf) else {
            return;
        };
        let mut direct = None;
        match pkt {
            ArtPacket::Poll(p) => self.on_poll(&p, src, now),
            ArtPacket::Address(a) => direct = self.on_address(&a).map(|b| (src, b)),
            ArtPacket::Input(i) => direct = self.on_input(&i).map(|b| (src, b)),
            ArtPacket::IpProg(p) => self.on_ip_prog(&p, src, now),
            ArtPacket::Dmx(d) => self.on_dmx(&d, src, now),
            ArtPacket::Sync(_) => self.on_sync(src),
            ArtPacket::TodRequest(r) => self.on_tod_request(&r, src, now),
            ArtPacket::TodControl(c) => self.on_tod_control(&c, src, now),
            ArtPacket::Rdm(r) => self.on_rdm(&r, src, now),
            ArtPacket::FirmwareMaster(f) => self.on_firmware(&f, src, now),
            _ => {}
        }
        self.emit_changes(now, direct);
    }

    fn on_poll(&mut self, p: &ArtPoll, src: SocketAddr, now: Instant) {
        if p.reply_on_change() {
            push_unique(&mut self.pollers, src);
        }
        let max = self.cfg.reply_delay_max.as_micros() as u64;
        let delay = if max == 0 {
            Duration::ZERO
        } else {
            Duration::from_micros(rand::rng().random_range(0..=max))
        };
        for b in 0..SIM_PORTS {
            let pa = self.ports[b].port_address.raw();
            if p.targeted_mode()
                && !(p.target_port_address_bottom..=p.target_port_address_top).contains(&pa)
            {
                continue;
            }
            self.push_reply(src, b, now + delay);
        }
    }

    /// ArtAddress for one BindIndex (= port). Returns the bind index (0-based) to answer, `None` = ignored.
    fn on_address(&mut self, a: &ArtAddress) -> Option<usize> {
        let bind = usize::from(a.bind_index.max(1));
        if bind > SIM_PORTS {
            return None;
        }
        let i = bind - 1;
        if !a.short_name.is_empty() {
            self.short_name = truncate(&a.short_name, SHORT_NAME_MAX);
        }
        if !a.long_name.is_empty() {
            self.long_name = truncate(&a.long_name, LONG_NAME_MAX);
        }
        if a.acn_priority <= 200 {
            self.acn_priority = a.acn_priority;
        }
        let port = &mut self.ports[i];
        let pa = port.port_address;
        let f = port.factory.port_address;
        let program = |v: u8, cur: u8, factory: u8, mask: u8| {
            if v & 0x80 != 0 {
                v & mask
            } else if v == ArtAddress::RESET {
                factory
            } else {
                cur
            }
        };
        let sw = match port.direction {
            SimDirection::Output => a.sw_out[0],
            SimDirection::Input => a.sw_in[0],
        };
        let net = program(a.net_switch, pa.net(), f.net(), 0x7F);
        let sub = program(a.sub_switch, pa.sub_net(), f.sub_net(), 0x0F);
        let uni = program(sw, pa.universe(), f.universe(), 0x0F);
        port.port_address = PortAddress::from_parts(net, sub, uni).unwrap_or(pa);
        match a.command {
            ac::AC_CANCEL_MERGE => port.cancel_merge = true,
            ac::AC_LED_NORMAL => self.indicators = SimIndicators::Normal,
            ac::AC_LED_MUTE => self.indicators = SimIndicators::Mute,
            ac::AC_LED_LOCATE => self.indicators = SimIndicators::Locate,
            c @ ac::AC_FAIL_HOLD..=ac::AC_FAIL_SCENE => self.failsafe = c - ac::AC_FAIL_HOLD,
            _ => self.port_command(i, a.command),
        }
        Some(i)
    }

    /// Port-specific ArtAddress commands (BindIndex = port, so only the "port 0" variants apply).
    fn port_command(&mut self, i: usize, command: u8) {
        let port = &mut self.ports[i];
        match command {
            ac::AC_MERGE_LTP0 => port.merge_ltp = true,
            ac::AC_MERGE_HTP0 => port.merge_ltp = false,
            ac::AC_DIRECTION_TX0 | ac::AC_DIRECTION_RX0 => {
                let d = if command == ac::AC_DIRECTION_TX0 {
                    SimDirection::Output
                } else {
                    SimDirection::Input
                };
                if port.direction != d {
                    port.direction = d;
                    port.reset_runtime();
                }
            }
            ac::AC_CLEAR_OP0 => {
                if let Some(f) = port.dmx.as_mut() {
                    f.data.fill(0);
                }
                port.sources.clear();
            }
            ac::AC_STYLE_DELTA0 => port.continuous = false,
            ac::AC_STYLE_CONST0 => port.continuous = true,
            ac::AC_RDM_ENABLE0 => port.rdm_enabled = true,
            ac::AC_RDM_DISABLE0 => port.rdm_enabled = false,
            // AcFailRecord, AcResetRxFlags, analysis, sACN select, deprecated port 1-3 commands: no visible effect
            _ => {}
        }
    }

    fn on_input(&mut self, inp: &ArtInput) -> Option<usize> {
        let bind = usize::from(inp.bind_index.max(1));
        if bind > SIM_PORTS {
            return None;
        }
        let port = &mut self.ports[bind - 1];
        if port.direction == SimDirection::Input {
            port.enabled = inp.input[0] & ArtInput::DISABLE == 0;
        }
        Some(bind - 1)
    }

    /// Same rules as the firmware (`pm_artnet_node.c` `on_ip_prog`): Command 0 (bit 7 clear) = query only;
    /// DHCP bit wins; otherwise reset first, then IP (switches DHCP off), mask, gateway. Invalid IPs
    /// (0.0.0.0, 255.255.255.255) and non-contiguous/empty masks are ignored.
    fn on_ip_prog(&mut self, p: &ArtIpProg, src: SocketAddr, now: Instant) {
        let c = p.command;
        if !p.is_query() {
            let mut s = self.ip.active;
            if c & IPPROG_DHCP != 0 {
                s.dhcp = true;
            } else {
                if c & IPPROG_RESET != 0 {
                    // factory network configuration: DHCP with the Art-Net fallback as static values
                    s = self.ip.factory;
                }
                if c & IPPROG_IP != 0 && !p.prog_ip.is_unspecified() && !p.prog_ip.is_broadcast() {
                    s.static_ip = p.prog_ip;
                    s.dhcp = false;
                }
                let m = u32::from(p.prog_sm);
                if c & IPPROG_MASK != 0 && m != 0 && m.leading_ones() + m.trailing_zeros() == 32 {
                    s.static_mask = p.prog_sm;
                }
                if c & IPPROG_GATEWAY != 0 {
                    s.gateway = p.prog_dg;
                }
            }
            self.ip.set_active(s);
        }
        let (ip, mask) = self.ip.current();
        let reply = ArtIpProgReply {
            prog_ip: ip,
            prog_sm: mask,
            prog_port: artnet::PORT,
            status: if self.ip.active.dhcp {
                ArtIpProgReply::STATUS_DHCP
            } else {
                0
            },
            prog_dg: self.ip.active.gateway,
        };
        self.send(src, reply.encode(), now);
    }

    fn on_dmx(&mut self, d: &ArtDmx, src: SocketAddr, now: Instant) {
        for p in self.ports.iter_mut() {
            if p.direction == SimDirection::Output && p.enabled && p.port_address == d.port_address
            {
                p.receive_dmx(src, d, now);
            }
        }
        self.last_dmx_ip = Some(src.ip());
    }

    fn on_sync(&mut self, src: SocketAddr) {
        if self.ports.iter().any(Port::merging) || self.last_dmx_ip != Some(src.ip()) {
            return;
        }
        self.sync_count += 1;
    }

    fn send_tod(&mut self, i: usize, to: SocketAddr, now: Instant) {
        let p = &self.ports[i];
        let pa = p.port_address;
        let uids = if p.line_active() {
            p.tod.clone()
        } else {
            Vec::new()
        };
        let total = u16::try_from(uids.len()).unwrap_or(u16::MAX);
        let mut chunks: Vec<&[Uid]> = uids.chunks(ArtTodData::MAX_UIDS).collect();
        if chunks.is_empty() {
            chunks.push(&[]);
        }
        let packets: Vec<Vec<u8>> = chunks
            .iter()
            .enumerate()
            .map(|(k, c)| {
                ArtTodData {
                    rdm_ver: ArtTodData::RDM_VER_1_0,
                    port: 1,
                    bind_index: (i + 1) as u8,
                    net: pa.net(),
                    command_response: TOD_FULL,
                    address: pa.sub_uni(),
                    uid_total: total,
                    block_count: k as u8,
                    uids: c.to_vec(),
                }
                .encode()
            })
            .collect();
        for pkt in packets {
            self.send(to, pkt, now);
        }
    }

    fn on_tod_request(&mut self, r: &ArtTodRequest, src: SocketAddr, now: Instant) {
        push_unique(&mut self.tod_requesters, src);
        for pa in r.port_addresses() {
            for i in 0..SIM_PORTS {
                if self.ports[i].port_address == pa {
                    self.send_tod(i, src, now);
                }
            }
        }
    }

    fn on_tod_control(&mut self, c: &ArtTodControl, src: SocketAddr, now: Instant) {
        push_unique(&mut self.tod_requesters, src);
        let pa = c.port_address();
        for i in 0..SIM_PORTS {
            if self.ports[i].port_address != pa {
                continue;
            }
            let port = &mut self.ports[i];
            match c.command {
                ArtTodControl::ATC_FLUSH => port.discover(),
                ArtTodControl::ATC_INC_ON => port.bg_discovery = true,
                ArtTodControl::ATC_INC_OFF => port.bg_discovery = false,
                _ => {}
            }
            self.send_tod(i, src, now);
        }
    }

    /// Port index for an ArtRdm/ArtDmx Port-Address (outputs first).
    fn port_index_for(&self, pa: PortAddress) -> Option<usize> {
        self.ports
            .iter()
            .position(|p| p.port_address == pa && p.direction == SimDirection::Output)
            .or_else(|| self.ports.iter().position(|p| p.port_address == pa))
    }

    fn fifo_avail(&self, i: usize) -> u8 {
        let p = &self.ports[i];
        let used = p.fifo.len() + usize::from(p.in_flight.is_some());
        usize::from(self.cfg.fifo_max)
            .saturating_sub(used)
            .try_into()
            .unwrap_or(0)
    }

    fn on_rdm(&mut self, r: &ArtRdm, src: SocketAddr, now: Instant) {
        if r.command != ArtRdm::AR_PROCESS {
            return;
        }
        let Ok(msg) = r.message() else {
            return;
        };
        if msg.command_class != cc::GET && msg.command_class != cc::SET {
            return;
        }
        let Some(i) = self.port_index_for(r.port_address()) else {
            return;
        };
        if msg.dest_uid == self.uid {
            if let Some(resp) = self.handle_node_rdm(&msg, now) {
                let pa = self.ports[i].port_address;
                if let Some(pkt) = art_rdm_packet(pa, &resp, self.fifo_avail(i), self.cfg.fifo_max)
                {
                    self.send(src, pkt, now);
                }
            }
            return;
        }
        if self.uid.addressed_to(msg.dest_uid) {
            // broadcast/vendorcast SET also reaches the node's own responder (never answered)
            let _ = self.handle_node_rdm(&msg, now);
        }
        let max = usize::from(self.cfg.fifo_max);
        let port = &mut self.ports[i];
        if !port.line_active() || port.fifo.len() + usize::from(port.in_flight.is_some()) >= max {
            return;
        }
        port.fifo.push_back(RdmJob {
            requester: src,
            request: msg,
        });
    }

    fn on_firmware(&mut self, f: &ArtFirmwareMaster, src: SocketAddr, now: Instant) {
        let target = self.fw_target();
        let (kind, new_fw) = self.fw.artnet_block(src, f, target, now);
        self.send(src, ArtFirmwareReply { kind }.encode(), now);
        if let Some(fw) = new_fw {
            self.pending_reboot = Some(Some(fw));
        }
    }

    // -----------------------------------------------------------------------------------------------------------
    // Timers
    // -----------------------------------------------------------------------------------------------------------

    fn tick(&mut self, now: Instant) {
        if let Some(fw) = self.pending_reboot.take() {
            self.reboot(fw, now);
        }
        if self.silent_until.is_some_and(|t| now >= t) {
            self.silent_until = None;
        }
        for p in self.ports.iter_mut() {
            for r in p.responders.iter_mut() {
                r.tick(now);
            }
            p.sources
                .retain(|s| now.saturating_duration_since(s.last) < MERGE_SOURCE_TIMEOUT);
        }
        if now >= self.next_discovery {
            self.next_discovery = now + self.cfg.discovery_interval;
            for p in self.ports.iter_mut().filter(|p| p.bg_discovery) {
                p.discover();
            }
        }
        self.fw.tick(now, self.cfg.fw_timeout);
        self.boot_catch_tick(now);
        for i in 0..SIM_PORTS {
            self.port_tick(i, now);
        }
        self.emit_changes(now, None);
    }

    fn port_tick(&mut self, i: usize, now: Instant) {
        let max = self.cfg.fifo_max;
        let latency = self.cfg.rdm_latency;
        if self.ports[i]
            .in_flight
            .as_ref()
            .is_some_and(|f| now >= f.done)
        {
            let done = self.ports[i].in_flight.take();
            if let Some(InFlight {
                requester: Some(to),
                response: Some(resp),
                ..
            }) = done
            {
                if self.ports[i].drop_responses > 0 {
                    self.ports[i].drop_responses -= 1;
                } else {
                    let pa = self.ports[i].port_address;
                    if let Some(pkt) = art_rdm_packet(pa, &resp, self.fifo_avail(i), max) {
                        self.send(to, pkt, now);
                    }
                }
            }
        }
        let port = &mut self.ports[i];
        if port.in_flight.is_some() {
            return;
        }
        if let Some(job) = port.fifo.pop_front() {
            if !port.line_active() {
                return;
            }
            let response = port.line_transaction(&job.request, now);
            port.in_flight = Some(InFlight {
                done: now + latency,
                requester: Some(job.requester),
                response,
            });
        }
    }

    fn reboot(&mut self, new_fw: Option<NewFirmware>, now: Instant) {
        if let Some(fw) = new_fw {
            self.fw_version = fw.fw_version;
            self.version_label = fw.version_label;
        }
        self.boot_count += 1;
        self.silent_until = Some(now + self.cfg.reboot_delay);
        self.pollers.clear();
        self.tod_requesters.clear();
        self.outbox.clear();
        self.queue = MessageQueue::default();
        self.boot_catch = BootCatch::default();
        self.fw = FwUpdate::default();
        self.local_set = None;
        self.local_seq = 0;
        self.indicators = SimIndicators::Normal;
        self.report_counter = 0;
        self.last_dmx_ip = None;
        for p in self.ports.iter_mut() {
            p.reset_runtime();
        }
        self.mark_reported();
    }

    /// FACTORY_DEFAULTS: names, ports, settings, failsafe, network back to factory (the node keeps running).
    pub(super) fn factory_reset(&mut self) {
        self.short_name = truncate(&self.cfg.short_name, SHORT_NAME_MAX);
        self.long_name = truncate(&self.cfg.long_name, LONG_NAME_MAX);
        self.indicators = SimIndicators::Normal;
        self.failsafe = 0;
        self.acn_priority = 100;
        self.settings = Settings::default();
        let factory = self.ip.factory;
        self.ip.set_active(factory);
        for p in self.ports.iter_mut() {
            let dir = p.direction;
            p.reset_settings();
            if p.direction != dir {
                p.reset_runtime();
            }
        }
    }

    /// Whether everything FACTORY_DEFAULTS resets is at its factory value.
    pub(super) fn at_factory_defaults(&self) -> bool {
        self.short_name == truncate(&self.cfg.short_name, SHORT_NAME_MAX)
            && self.long_name == truncate(&self.cfg.long_name, LONG_NAME_MAX)
            && self.failsafe == 0
            && self.acn_priority == 100
            && self.settings == Settings::default()
            && self.ip.active == self.ip.factory
            && self.ports.iter().all(Port::at_factory_settings)
    }

    fn snapshot(&self, now: Instant) -> SimSnapshot {
        let (ip, netmask) = self.ip.current();
        SimSnapshot {
            uid: self.uid,
            short_name: self.short_name.clone(),
            long_name: self.long_name.clone(),
            fw_version: self.fw_version,
            version_label: self.version_label.clone(),
            hw_revision: self.cfg.hw_revision,
            indicators: self.indicators,
            failsafe: self.failsafe,
            ip,
            netmask,
            gateway: self.ip.active.gateway,
            dhcp: self.ip.active.dhcp,
            ports: self
                .ports
                .iter()
                .map(|p| SimPortSnapshot {
                    direction: p.direction,
                    port_address: p.port_address,
                    enabled: p.enabled,
                    rdm_enabled: p.rdm_enabled,
                    merge_ltp: p.merge_ltp,
                    merging: p.merging(),
                    continuous: p.continuous,
                    bg_discovery: p.bg_discovery,
                    dmx_break: p.dmx_break,
                    dmx_mab: p.dmx_mab,
                    dmx_rate: p.dmx_rate,
                    dmx_mbs: p.dmx_mbs,
                    tod: p.tod.clone(),
                })
                .collect(),
            display_level: self.settings.display_level,
            display_dim_timeout: self.settings.dim_timeout,
            display_address_format: self.settings.address_format,
            panel_locked: self.settings.panel_lock,
            bg_discovery_interval: self.settings.bg_interval,
            fw_state: self.fw.state,
            fw_last_error: self.fw.last_error,
            fw_received: self.fw.received,
            fw_total: self.fw.total,
            rebooting: self.is_silent(now),
            boot_count: self.boot_count,
            boot_catch: self.boot_catch.status(now),
            message_count: self.queue.count(),
            pollers: self.pollers.clone(),
            tod_requesters: self.tod_requesters.clone(),
        }
    }

    fn port_mut(&mut self, port: u8) -> Option<&mut Port> {
        let i = usize::from(port).checked_sub(1)?;
        self.ports.get_mut(i)
    }

    fn responder_mut(&mut self, uid: Uid) -> Option<&mut Box<dyn SimResponder>> {
        self.ports
            .iter_mut()
            .flat_map(|p| p.responders.iter_mut())
            .find(|r| r.uid() == uid)
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Public handle and task
// ---------------------------------------------------------------------------------------------------------------

struct Shared {
    state: Mutex<Node>,
    socket: UdpSocket,
    local: SocketAddr,
    uid: Uid,
    wake: Notify,
    stop: watch::Sender<bool>,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, Node> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// Handle to a running simulated node. Cheap to clone; all clones control the same node.
#[derive(Clone)]
pub struct SimNode {
    shared: Arc<Shared>,
}

impl std::fmt::Debug for SimNode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SimNode")
            .field("local_addr", &self.shared.local)
            .field("uid", &self.shared.uid)
            .finish()
    }
}

impl SimNode {
    /// Bind the UDP socket and spawn the node task (needs a tokio runtime). The task runs until
    /// [`SimNode::shutdown`]; dropping all handles does not stop it.
    pub async fn start(cfg: SimConfig) -> crate::Result<SimNode> {
        let socket = UdpSocket::bind(cfg.bind).await?;
        let local = socket.local_addr()?;
        let node = Node::new(cfg, local, Instant::now());
        let uid = node.uid;
        let (stop, stop_rx) = watch::channel(false);
        let shared = Arc::new(Shared {
            state: Mutex::new(node),
            socket,
            local,
            uid,
            wake: Notify::new(),
            stop,
        });
        tokio::spawn(run(Arc::clone(&shared), stop_rx));
        Ok(SimNode { shared })
    }

    /// The bound socket address (send Art-Net here).
    pub fn local_addr(&self) -> SocketAddr {
        self.shared.local
    }

    /// The node's RDM UID (= ArtPollReply DefaultRespUID).
    pub fn uid(&self) -> Uid {
        self.shared.uid
    }

    /// Run `f` on the state, then report changes and wake the task to send what `f` queued.
    fn with<T>(&self, f: impl FnOnce(&mut Node, Instant) -> T) -> T {
        let now = Instant::now();
        let r = {
            let mut node = self.shared.lock();
            let r = f(&mut node, now);
            node.emit_changes(now, None);
            r
        };
        self.shared.wake.notify_one();
        r
    }

    /// Plug a responder into port 1..=4 (ignored for other numbers). The TOD is updated at once, so requesters
    /// get an unsolicited ArtTodData.
    pub fn add_responder(&self, port: u8, r: Box<dyn SimResponder>) {
        self.with(|n, _| {
            if let Some(p) = n.port_mut(port) {
                p.responders.push(r);
                p.discover();
            }
        });
    }

    /// Unplug the responder `uid` from port 1..=4 and return it (TOD updated at once).
    pub fn remove_responder(&self, port: u8, uid: Uid) -> Option<Box<dyn SimResponder>> {
        self.with(|n, _| {
            let p = n.port_mut(port)?;
            let i = p.responders.iter().position(|r| r.uid() == uid)?;
            let r = p.responders.remove(i);
            p.discover();
            Some(r)
        })
    }

    /// Access the responder `uid` (on any port) as its concrete type `R`; `None` if absent or another type.
    pub fn with_responder<R: 'static, T>(
        &self,
        uid: Uid,
        f: impl FnOnce(&mut R) -> T,
    ) -> Option<T> {
        self.with(|n, _| {
            let r = n.responder_mut(uid)?;
            r.as_any_mut().downcast_mut::<R>().map(f)
        })
    }

    /// Cut and restore the power of responder `uid` ([`SimResponder::power_cycle`]). The TOD follows with the
    /// next background discovery.
    pub fn power_cycle(&self, uid: Uid) {
        self.with(|n, now| {
            if let Some(r) = n.responder_mut(uid) {
                r.power_cycle(now);
            }
        });
    }

    /// Lose the next `n` RDM answers coming back on port 1..=4 (the requests still reach the devices).
    pub fn drop_rdm_responses(&self, port: u8, n: u32) {
        self.with(|node, _| {
            if let Some(p) = node.port_mut(port) {
                p.drop_responses = n;
            }
        });
    }

    /// Change the short name at the device panel (→ unsolicited ArtPollReply).
    pub fn set_short_name_local(&self, name: &str) {
        self.with(|n, _| n.short_name = truncate(name, SHORT_NAME_MAX));
    }

    /// Lock/unlock the panel at the device (→ queued PANEL_LOCK; unlocking also queues status
    /// PANEL_UNLOCKED_LOCAL).
    pub fn set_panel_lock_local(&self, locked: bool) {
        self.with(|n, now| {
            let was = n.settings.panel_lock;
            n.settings.panel_lock = locked;
            n.emit_changes(now, None);
            if was && !locked {
                n.queue.push_status(
                    crate::rdm::status_type::ADVISORY,
                    pids::status::PANEL_UNLOCKED_LOCAL,
                    0,
                    0,
                );
            }
        });
    }

    /// RDM SET from the panel to device `uid` on port 1..=4. If the device ACKs, the node queues the GET
    /// response LOCAL_RDM_SET and status LOCAL_RDM_SET (0x8004). Returns whether the device ACKed.
    pub fn local_rdm_set(&self, port: u8, uid: Uid, pid: u16, data: Vec<u8>) -> bool {
        self.with(|n, now| n.local_rdm_set(port, uid, pid, data, now))
    }

    /// The frame port 1..=4 currently outputs (`None` before the first ArtDmx).
    pub fn last_dmx(&self, port: u8) -> Option<SimDmxFrame> {
        self.with(|n, _| n.port_mut(port).and_then(|p| p.dmx.clone()))
    }

    /// Number of accepted ArtSync packets.
    pub fn sync_count(&self) -> u64 {
        self.shared.lock().sync_count
    }

    /// Current state for inspection.
    pub fn snapshot(&self) -> SimSnapshot {
        self.shared.lock().snapshot(Instant::now())
    }

    /// Stop the node task (the socket closes when the last handle is dropped).
    pub fn shutdown(&self) {
        let _ = self.shared.stop.send(true);
    }
}

async fn run(shared: Arc<Shared>, mut stop: watch::Receiver<bool>) {
    let mut buf = vec![0u8; 2048];
    let mut tick = tokio::time::interval(TICK);
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    loop {
        if *stop.borrow() {
            break;
        }
        tokio::select! {
            _ = stop.changed() => {}
            r = shared.socket.recv_from(&mut buf) => {
                // receive errors (e.g. ICMP port unreachable reported on some platforms) are ignored
                if let Ok((n, src)) = r {
                    shared.lock().handle_packet(&buf[..n], src, Instant::now());
                }
            }
            _ = tick.tick() => shared.lock().tick(Instant::now()),
            _ = shared.wake.notified() => {}
        }
        let due = shared.lock().take_due(Instant::now());
        for (to, data) in due {
            let _ = shared.socket.send_to(&data, to).await;
        }
    }
}
