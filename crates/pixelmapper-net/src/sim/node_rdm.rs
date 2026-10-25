//! The node's own RDM responder (`protocol/contract.md` §6.3, §6.4): standard PIDs from
//! `pids.toml [node.standard_pids]`, all node manufacturer PIDs, the queued-message/status queue
//! (rdm-digest §4.3) and BOOT_CATCH.
//!
//! Request checks follow the NACK order of rdm-digest §8: sub-device → UNKNOWN_PID → UNSUPPORTED_COMMAND_CLASS →
//! FORMAT_ERROR (PDL) → DATA_OUT_OF_RANGE (value, ranges from `pids.toml`).

use super::node::{mask_to_prefix, prefix_to_mask, truncate, Node, SimIndicators, SHORT_NAME_MAX};
use super::node_fw::FwRefusal;
use crate::pids::{self, boot_catch_state, Device, PidDef};
use crate::rdm::pd::{
    decode_label, encode_label, encode_pid_list, BootCatchStart, BootCatchStatus, DeviceInfo,
    FwWrite, InterfaceEntry, Ipv4CurrentAddress, LocalRdmSet, ParameterDescription, StatusMessage,
};
use crate::rdm::{
    cc, pid as p, status_type, NackReason, RdmMessage, ResponseType, Uid, SUB_DEVICE_ALL,
    SUB_DEVICE_ROOT,
};
use std::collections::VecDeque;
use std::net::Ipv4Addr;
use std::time::{Duration, Instant};

/// DEVICE_MODEL_DESCRIPTION of the node.
const MODEL_DESCRIPTION: &str = "Pixelmapper Node";
/// MANUFACTURER_LABEL.
const MANUFACTURER_LABEL: &str = "Pixelmapper";
/// BOOT_SOFTWARE_VERSION_ID of the simulated (ESP-IDF) bootloader.
const BOOT_SW_VERSION_ID: u32 = 0x0005_0506;
/// BOOT_SOFTWARE_VERSION_LABEL.
const BOOT_SW_VERSION_LABEL: &str = "ESP-IDF v5.5.6 bootloader";
/// E1.37-2 interface id of the Ethernet port.
const IFACE: u32 = 1;
/// E1.37-2 hardware type Ethernet.
const HW_ETHERNET: u16 = 1;
/// INTERFACE_LABEL.
const IFACE_LABEL: &str = "eth0";
/// PARAMETER_DESCRIPTION command class GET_SET (E1.20).
const CC_GET_SET: u8 = 0x03;
/// RDM protocol version 1.0.
const RDM_PROTOCOL_VERSION: u16 = 0x0100;
/// Queued messages kept (oldest dropped beyond).
const MAX_QUEUED: usize = 64;
/// Status messages kept (= what fits in one STATUS_MESSAGES response).
const MAX_STATUS: usize = 25;

/// One queued message: the response the controller gets for QUEUED_MESSAGE.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct QueuedMsg {
    pid: u16,
    cc: u8,
    data: Vec<u8>,
}

/// Queued messages + status messages (rdm-digest §4.3).
#[derive(Debug, Default)]
pub(super) struct MessageQueue {
    msgs: VecDeque<QueuedMsg>,
    statuses: Vec<StatusMessage>,
    last: Option<QueuedMsg>,
    last_status: Vec<StatusMessage>,
}

impl MessageQueue {
    /// Queue the GET response `data` of `pid`. `replace` drops an older unread message of the same PID (for
    /// settings only the latest value matters).
    pub(super) fn push(&mut self, pid: u16, data: Vec<u8>, replace: bool) {
        if replace {
            self.msgs.retain(|m| m.pid != pid);
        }
        if self.msgs.len() >= MAX_QUEUED {
            self.msgs.pop_front();
        }
        self.msgs.push_back(QueuedMsg {
            pid,
            cc: cc::GET_RESPONSE,
            data,
        });
    }

    /// Add a status message (sub-device 0).
    pub(super) fn push_status(&mut self, kind: u8, id: u16, data1: i16, data2: i16) {
        if self.statuses.len() >= MAX_STATUS {
            self.statuses.remove(0);
        }
        self.statuses.push(StatusMessage {
            sub_device: SUB_DEVICE_ROOT,
            status_type: kind,
            status_message_id: id,
            data_value1: data1,
            data_value2: data2,
        });
    }

    /// Message Count: queued messages + 1 while status messages wait.
    pub(super) fn count(&self) -> u8 {
        let n = self.msgs.len() + usize::from(!self.statuses.is_empty());
        u8::try_from(n).unwrap_or(u8::MAX)
    }

    /// Remove and return the status messages of severity ≥ `kind` (ADVISORY < WARNING < ERROR).
    fn take_statuses(&mut self, kind: u8) -> Vec<StatusMessage> {
        let (take, keep): (Vec<_>, Vec<_>) = self
            .statuses
            .drain(..)
            .partition(|s| s.status_type & 0x0F >= kind);
        self.statuses = keep;
        take
    }
}

/// BOOT_CATCH state (§6.4).
#[derive(Debug, Default)]
pub(super) struct BootCatch {
    state: u8,
    port: u8,
    target: Uid,
    started: Option<Instant>,
    elapsed_ms: u32,
    attempts: u16,
    last_attempt: Option<Instant>,
}

impl BootCatch {
    /// BOOT_CATCH_STATUS response.
    pub(super) fn status(&self, now: Instant) -> BootCatchStatus {
        let elapsed_ms = match (self.state, self.started) {
            (boot_catch_state::RUNNING, Some(t)) => {
                u32::try_from(now.saturating_duration_since(t).as_millis()).unwrap_or(u32::MAX)
            }
            _ => self.elapsed_ms,
        };
        BootCatchStatus {
            state: self.state,
            port: self.port,
            target_uid: self.target,
            elapsed_ms,
            attempts: self.attempts,
        }
    }
}

/// Allowed PDL range of a request.
#[derive(Debug, Clone, Copy)]
struct Pdl(usize, usize);

#[derive(Debug, Clone, Copy)]
struct Spec {
    get: Option<Pdl>,
    set: Option<Pdl>,
}

enum Answer {
    Ack(Vec<u8>),
    Nack(NackReason),
    Queued(QueuedMsg),
}

/// A node manufacturer PID from `pids.toml`.
fn node_pid_def(pid: u16) -> Option<&'static PidDef> {
    pids::PIDS
        .iter()
        .find(|d| d.pid == pid && d.device == Device::Node)
}

/// Request PDL rules of every PID the node supports; `None` = unknown PID.
fn pid_spec(pid: u16) -> Option<Spec> {
    let e = |n: usize| Some(Pdl(n, n));
    let get = |n| Spec {
        get: e(n),
        set: None,
    };
    let set = |n| Spec {
        get: None,
        set: e(n),
    };
    let both = |g, s| Spec {
        get: e(g),
        set: e(s),
    };
    Some(match pid {
        p::DEVICE_INFO
        | p::SUPPORTED_PARAMETERS
        | p::SOFTWARE_VERSION_LABEL
        | p::PRODUCT_DETAIL_ID_LIST
        | p::DEVICE_MODEL_DESCRIPTION
        | p::MANUFACTURER_LABEL
        | p::BOOT_SOFTWARE_VERSION_ID
        | p::BOOT_SOFTWARE_VERSION_LABEL
        | p::LIST_INTERFACES => get(0),
        p::PARAMETER_DESCRIPTION | p::STATUS_ID_DESCRIPTION => get(2),
        p::QUEUED_MESSAGE | p::STATUS_MESSAGES => get(1),
        p::IDENTIFY_DEVICE | p::DISPLAY_LEVEL => both(0, 1),
        p::CLEAR_STATUS_ID => set(0),
        p::DEVICE_LABEL => Spec {
            get: e(0),
            set: Some(Pdl(0, crate::rdm::pd::MAX_LABEL)),
        },
        p::FACTORY_DEFAULTS => both(0, 0),
        p::RESET_DEVICE => set(1),
        p::INTERFACE_LABEL | p::INTERFACE_HARDWARE_ADDRESS_TYPE1 | p::IPV4_CURRENT_ADDRESS => {
            get(4)
        }
        p::IPV4_DHCP_MODE => both(4, 5),
        p::IPV4_STATIC_ADDRESS => both(4, 9),
        p::INTERFACE_RENEW_DHCP | p::INTERFACE_APPLY_CONFIGURATION => set(4),
        p::IPV4_DEFAULT_ROUTE => both(0, 8),
        _ => {
            let def = node_pid_def(pid)?;
            Spec {
                get: def.get_request_pdl.map(|n| Pdl(n.into(), n.into())),
                set: def.set_request_pdl.map(|n| {
                    if pid == pids::PID_FW_WRITE {
                        Pdl(4, n.into())
                    } else {
                        Pdl(n.into(), n.into())
                    }
                }),
            }
        }
    })
}

/// SUPPORTED_PARAMETERS: listed standard PIDs + listed node manufacturer PIDs.
pub(super) fn supported_parameters() -> Vec<u16> {
    pids::NODE_STD_PIDS_LISTED
        .iter()
        .copied()
        .chain(
            pids::PIDS
                .iter()
                .filter(|d| d.device == Device::Node && d.listed)
                .map(|d| d.pid),
        )
        .collect()
}

/// `pid - base` if it is one of the 4 per-port PIDs starting at `base`.
fn port_of(pid: u16, base: u16) -> Option<usize> {
    pid.checked_sub(base)
        .filter(|&d| usize::from(d) < super::node::SIM_PORTS)
        .map(usize::from)
}

fn u32_at(pd: &[u8], o: usize) -> u32 {
    pd.get(o..o + 4)
        .map_or(0, |b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}

fn ipv4_at(pd: &[u8], o: usize) -> Ipv4Addr {
    Ipv4Addr::from(u32_at(pd, o))
}

fn with_iface(rest: &[u8]) -> Vec<u8> {
    let mut v = IFACE.to_be_bytes().to_vec();
    v.extend_from_slice(rest);
    v
}

/// Whether a broadcast/vendorcast SET of `pid` is executed by the node itself (never answered).
fn broadcast_allowed(pid: u16, dest: Uid) -> bool {
    match pid {
        p::IDENTIFY_DEVICE | p::DEVICE_LABEL | p::DISPLAY_LEVEL => true,
        // manufacturer settings only per vendorcast (E1.20 6.2.10.2), never FW_*/BOOT_CATCH_*
        _ => {
            dest.is_vendorcast() && node_pid_def(pid).is_some_and(|d| d.command_class == CC_GET_SET)
        }
    }
}

impl Node {
    /// Handle an RDM request for the node's own responder. Unicast to the node UID → answer; broadcast /
    /// vendorcast SET → executed where allowed, no answer.
    pub(super) fn handle_node_rdm(&mut self, req: &RdmMessage, now: Instant) -> Option<RdmMessage> {
        if req.dest_uid != self.uid {
            if req.command_class == cc::SET
                && self.uid.addressed_to(req.dest_uid)
                && broadcast_allowed(req.parameter_id, req.dest_uid)
            {
                let _ = self.process(req, now);
            }
            return None;
        }
        let answer = self.process(req, now);
        // the change of a setting must already be in the Message Count of this answer
        self.emit_changes(now, None);
        let count = self.queue.count();
        Some(match answer {
            Answer::Ack(data) => req.response_to(self.uid, ResponseType::Ack, count, data),
            Answer::Nack(r) => req.response_to(
                self.uid,
                ResponseType::NackReason,
                count,
                r.0.to_be_bytes().to_vec(),
            ),
            Answer::Queued(m) => {
                let mut r = req.response_to(self.uid, ResponseType::Ack, count, m.data);
                r.parameter_id = m.pid;
                r.command_class = m.cc;
                r
            }
        })
    }

    fn process(&mut self, req: &RdmMessage, now: Instant) -> Answer {
        let get = req.command_class == cc::GET;
        if req.sub_device != SUB_DEVICE_ROOT && (get || req.sub_device != SUB_DEVICE_ALL) {
            return Answer::Nack(NackReason::SUB_DEVICE_OUT_OF_RANGE);
        }
        let Some(spec) = pid_spec(req.parameter_id) else {
            return Answer::Nack(NackReason::UNKNOWN_PID);
        };
        let Some(Pdl(min, max)) = (if get { spec.get } else { spec.set }) else {
            return Answer::Nack(NackReason::UNSUPPORTED_COMMAND_CLASS);
        };
        let pd = req.parameter_data.as_slice();
        if !(min..=max).contains(&pd.len()) {
            return Answer::Nack(NackReason::FORMAT_ERROR);
        }
        if get {
            self.rdm_get(req.parameter_id, pd, now)
        } else {
            self.rdm_set(req.parameter_id, pd, req.src_uid, now)
        }
    }

    // -----------------------------------------------------------------------------------------------------------
    // GET
    // -----------------------------------------------------------------------------------------------------------

    fn rdm_get(&mut self, pid: u16, pd: &[u8], now: Instant) -> Answer {
        let iface_ok = || u32_at(pd, 0) == IFACE;
        let ack = Answer::Ack;
        let dor = Answer::Nack(NackReason::DATA_OUT_OF_RANGE);
        match pid {
            p::DEVICE_INFO => ack(DeviceInfo {
                rdm_protocol_version: RDM_PROTOCOL_VERSION,
                device_model_id: pids::MODEL_NODE,
                product_category: pids::NODE_PRODUCT_CATEGORY,
                software_version_id: self.fw_version,
                dmx_footprint: 0,
                current_personality: 0,
                personality_count: 0,
                dmx_start_address: 0xFFFF,
                sub_device_count: 0,
                sensor_count: 0,
            }
            .encode()),
            p::SUPPORTED_PARAMETERS => ack(encode_pid_list(&supported_parameters())),
            p::PARAMETER_DESCRIPTION => {
                let q = u16::from_be_bytes([pd[0], pd[1]]);
                match node_pid_def(q)
                    .filter(|d| d.listed)
                    .and_then(ParameterDescription::from_pid_def)
                {
                    Some(d) => ack(d.encode()),
                    None => dor,
                }
            }
            p::SOFTWARE_VERSION_LABEL => ack(encode_label(&self.version_label)),
            p::IDENTIFY_DEVICE => ack(vec![u8::from(self.indicators == SimIndicators::Locate)]),
            p::QUEUED_MESSAGE => self.queued_message(pd[0]),
            p::STATUS_MESSAGES => self.status_messages(pd[0]),
            p::STATUS_ID_DESCRIPTION => {
                match pids::status_description(u16::from_be_bytes([pd[0], pd[1]])) {
                    Some(t) => ack(encode_label(t)),
                    None => dor,
                }
            }
            p::PRODUCT_DETAIL_ID_LIST => ack(pids::NODE_PRODUCT_DETAIL.to_be_bytes().to_vec()),
            p::DEVICE_MODEL_DESCRIPTION => ack(encode_label(MODEL_DESCRIPTION)),
            p::MANUFACTURER_LABEL => ack(encode_label(MANUFACTURER_LABEL)),
            p::DEVICE_LABEL => ack(encode_label(&self.short_name)),
            p::FACTORY_DEFAULTS => ack(vec![u8::from(self.at_factory_defaults())]),
            p::BOOT_SOFTWARE_VERSION_ID => ack(BOOT_SW_VERSION_ID.to_be_bytes().to_vec()),
            p::BOOT_SOFTWARE_VERSION_LABEL => ack(encode_label(BOOT_SW_VERSION_LABEL)),
            p::DISPLAY_LEVEL => ack(vec![self.settings.display_level]),
            p::LIST_INTERFACES => ack(InterfaceEntry::encode_list(&[InterfaceEntry {
                interface_id: IFACE,
                hardware_type: HW_ETHERNET,
            }])),
            p::INTERFACE_LABEL if iface_ok() => ack(with_iface(&encode_label(IFACE_LABEL))),
            p::INTERFACE_HARDWARE_ADDRESS_TYPE1 if iface_ok() => ack(with_iface(&self.cfg.mac)),
            p::IPV4_DHCP_MODE if iface_ok() => ack(self.ipv4_dhcp_mode()),
            p::IPV4_CURRENT_ADDRESS if iface_ok() => {
                let (ip, mask) = self.ip.current();
                ack(Ipv4CurrentAddress {
                    interface_id: IFACE,
                    address: ip,
                    netmask: mask_to_prefix(mask),
                    dhcp_status: u8::from(self.ip.active.dhcp),
                }
                .encode())
            }
            p::IPV4_STATIC_ADDRESS if iface_ok() => ack(self.ipv4_static()),
            p::INTERFACE_LABEL
            | p::INTERFACE_HARDWARE_ADDRESS_TYPE1
            | p::IPV4_DHCP_MODE
            | p::IPV4_CURRENT_ADDRESS
            | p::IPV4_STATIC_ADDRESS => dor,
            p::IPV4_DEFAULT_ROUTE => ack(self.ipv4_route()),
            pids::PID_BOOT_CATCH_STATUS => ack(self.boot_catch.status(now).encode()),
            pids::PID_LOCAL_RDM_SET => ack(self
                .local_set
                .clone()
                .unwrap_or(LocalRdmSet {
                    sequence: 0,
                    port: 0,
                    uid: Uid::default(),
                    sub_device: 0,
                    pid: 0,
                })
                .encode()),
            pids::PID_FW_INFO => ack(self.fw.info(self.cfg.hw_revision, self.fw_version).encode()),
            _ => self
                .setting_value(pid)
                .map_or(Answer::Nack(NackReason::UNKNOWN_PID), Answer::Ack),
        }
    }

    fn ipv4_dhcp_mode(&self) -> Vec<u8> {
        with_iface(&[u8::from(self.ip.pending.dhcp)])
    }

    fn ipv4_static(&self) -> Vec<u8> {
        let mut v = with_iface(&self.ip.pending.static_ip.octets());
        v.push(mask_to_prefix(self.ip.pending.static_mask));
        v
    }

    fn ipv4_route(&self) -> Vec<u8> {
        let mut v = IFACE.to_be_bytes().to_vec();
        v.extend_from_slice(&self.ip.pending.gateway.octets());
        v
    }

    /// GET value of a single-value node manufacturer PID (`None` for the structured ones).
    fn setting_value(&self, pid: u16) -> Option<Vec<u8>> {
        let ports = &self.ports;
        if pid == pids::PID_NODE_HW_REVISION {
            return Some(vec![self.cfg.hw_revision]);
        }
        if let Some(i) = port_of(pid, pids::PID_PORT1_ENABLED) {
            return Some(vec![u8::from(ports[i].enabled)]);
        }
        if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_BREAK) {
            return Some(ports[i].dmx_break.to_be_bytes().to_vec());
        }
        if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_MAB) {
            return Some(ports[i].dmx_mab.to_be_bytes().to_vec());
        }
        if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_RATE) {
            return Some(vec![ports[i].dmx_rate]);
        }
        if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_MBS) {
            return Some(ports[i].dmx_mbs.to_be_bytes().to_vec());
        }
        if let Some(i) = port_of(pid, pids::PID_PORT1_RDM_BG_DISCOVERY) {
            return Some(vec![u8::from(ports[i].bg_discovery)]);
        }
        let s = &self.settings;
        match pid {
            pids::PID_RDM_BG_DISCOVERY_INTERVAL => Some(s.bg_interval.to_be_bytes().to_vec()),
            pids::PID_DISPLAY_DIM_TIMEOUT => Some(s.dim_timeout.to_be_bytes().to_vec()),
            pids::PID_DISPLAY_ADDRESS_FORMAT => Some(vec![s.address_format]),
            pids::PID_PANEL_LOCK => Some(vec![u8::from(s.panel_lock)]),
            _ => None,
        }
    }

    /// Current GET responses of every setting whose change is reported as a queued message.
    pub(super) fn rdm_setting_values(&self) -> Vec<(u16, Vec<u8>)> {
        let mut v: Vec<(u16, Vec<u8>)> = pids::PIDS
            .iter()
            .filter(|d| d.device == Device::Node && d.command_class == CC_GET_SET)
            .filter_map(|d| self.setting_value(d.pid).map(|x| (d.pid, x)))
            .collect();
        v.push((p::DISPLAY_LEVEL, vec![self.settings.display_level]));
        v.push((p::IPV4_DHCP_MODE, self.ipv4_dhcp_mode()));
        v.push((p::IPV4_STATIC_ADDRESS, self.ipv4_static()));
        v.push((p::IPV4_DEFAULT_ROUTE, self.ipv4_route()));
        v
    }

    fn queued_message(&mut self, kind: u8) -> Answer {
        if !(status_type::GET_LAST_MESSAGE..=status_type::ERROR).contains(&kind) {
            return Answer::Nack(NackReason::DATA_OUT_OF_RANGE);
        }
        let empty = || QueuedMsg {
            pid: p::STATUS_MESSAGES,
            cc: cc::GET_RESPONSE,
            data: Vec::new(),
        };
        if kind == status_type::GET_LAST_MESSAGE {
            return Answer::Queued(self.queue.last.clone().unwrap_or_else(empty));
        }
        let m = match self.queue.msgs.pop_front() {
            Some(m) => m,
            None => {
                let list = self.queue.take_statuses(kind);
                let m = QueuedMsg {
                    data: StatusMessage::encode_list(&list),
                    ..empty()
                };
                self.queue.last_status = list;
                m
            }
        };
        self.queue.last = Some(m.clone());
        Answer::Queued(m)
    }

    fn status_messages(&mut self, kind: u8) -> Answer {
        match kind {
            status_type::NONE => Answer::Ack(Vec::new()),
            status_type::GET_LAST_MESSAGE => {
                Answer::Ack(StatusMessage::encode_list(&self.queue.last_status))
            }
            status_type::ADVISORY..=status_type::ERROR => {
                let list = self.queue.take_statuses(kind);
                let data = StatusMessage::encode_list(&list);
                self.queue.last_status = list;
                Answer::Ack(data)
            }
            _ => Answer::Nack(NackReason::DATA_OUT_OF_RANGE),
        }
    }

    // -----------------------------------------------------------------------------------------------------------
    // SET
    // -----------------------------------------------------------------------------------------------------------

    fn rdm_set(&mut self, pid: u16, pd: &[u8], src: Uid, now: Instant) -> Answer {
        let ok = Answer::Ack(Vec::new());
        let dor = Answer::Nack(NackReason::DATA_OUT_OF_RANGE);
        let iface_ok = u32_at(pd, 0) == IFACE;
        match pid {
            p::IDENTIFY_DEVICE => match pd[0] {
                0 => {
                    if self.indicators == SimIndicators::Locate {
                        self.indicators = SimIndicators::Normal;
                    }
                    ok
                }
                1 => {
                    self.indicators = SimIndicators::Locate;
                    ok
                }
                _ => dor,
            },
            p::CLEAR_STATUS_ID => {
                self.queue.statuses.clear();
                ok
            }
            p::DEVICE_LABEL => {
                self.short_name = truncate(&decode_label(pd), SHORT_NAME_MAX);
                ok
            }
            p::FACTORY_DEFAULTS => {
                self.factory_reset();
                ok
            }
            p::DISPLAY_LEVEL => {
                self.settings.display_level = pd[0];
                ok
            }
            p::RESET_DEVICE => match pd[0] {
                // warm (0x01) and cold (0xFF) reset are the same for the node
                0x01 | 0xFF => {
                    self.pending_reboot = Some(None);
                    ok
                }
                _ => dor,
            },
            _ if !iface_ok
                && matches!(
                    pid,
                    p::IPV4_DHCP_MODE
                        | p::IPV4_STATIC_ADDRESS
                        | p::INTERFACE_RENEW_DHCP
                        | p::INTERFACE_APPLY_CONFIGURATION
                ) =>
            {
                dor
            }
            p::IPV4_DHCP_MODE => match pd[4] {
                v @ (0 | 1) => {
                    self.ip.pending.dhcp = v == 1;
                    ok
                }
                _ => dor,
            },
            p::IPV4_STATIC_ADDRESS => {
                if pd[8] > 32 {
                    return dor;
                }
                self.ip.pending.static_ip = ipv4_at(pd, 4);
                self.ip.pending.static_mask = prefix_to_mask(pd[8]);
                ok
            }
            p::INTERFACE_RENEW_DHCP => {
                if self.ip.active.dhcp {
                    ok
                } else {
                    Answer::Nack(NackReason::ACTION_NOT_SUPPORTED)
                }
            }
            p::INTERFACE_APPLY_CONFIGURATION => {
                self.ip.active = self.ip.pending;
                ok
            }
            p::IPV4_DEFAULT_ROUTE => match u32_at(pd, 0) {
                0 => {
                    self.ip.pending.gateway = Ipv4Addr::UNSPECIFIED;
                    ok
                }
                IFACE => {
                    self.ip.pending.gateway = ipv4_at(pd, 4);
                    ok
                }
                _ => dor,
            },
            pids::PID_BOOT_CATCH_START => self.boot_catch_start(pd, now),
            pids::PID_BOOT_CATCH_ABORT => {
                if self.boot_catch.state == boot_catch_state::RUNNING {
                    self.boot_catch_finish(boot_catch_state::ABORTED, None, now);
                }
                ok
            }
            pids::PID_FW_BEGIN => {
                let target = self.fw_target();
                fw_answer(self.fw.rdm_begin(src, pd, target, now))
            }
            pids::PID_FW_WRITE => match FwWrite::decode(pd) {
                Ok(w) => fw_answer(self.fw.rdm_write(src, w.offset, &w.data, now)),
                Err(_) => Answer::Nack(NackReason::FORMAT_ERROR),
            },
            pids::PID_FW_FINISH => match self.fw.rdm_finish(src) {
                Ok(fw) => {
                    self.pending_reboot = Some(Some(fw));
                    ok
                }
                Err(e) => fw_answer(Err(e)),
            },
            _ => self.set_setting(pid, pd),
        }
    }

    /// SET of a single-value node manufacturer PID with the range from `pids.toml`.
    fn set_setting(&mut self, pid: u16, pd: &[u8]) -> Answer {
        let Some(desc) = node_pid_def(pid).and_then(|d| d.desc) else {
            return Answer::Nack(NackReason::UNKNOWN_PID);
        };
        let v = pd.iter().fold(0u32, |a, &b| (a << 8) | u32::from(b));
        if v < desc.min || v > desc.max {
            return Answer::Nack(NackReason::DATA_OUT_OF_RANGE);
        }
        let (b, w) = (v != 0, u16::try_from(v).unwrap_or(u16::MAX));
        let byte = u8::try_from(v).unwrap_or(u8::MAX);
        let ports = &mut self.ports;
        if let Some(i) = port_of(pid, pids::PID_PORT1_ENABLED) {
            ports[i].enabled = b;
        } else if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_BREAK) {
            ports[i].dmx_break = w;
        } else if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_MAB) {
            ports[i].dmx_mab = w;
        } else if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_RATE) {
            ports[i].dmx_rate = byte;
        } else if let Some(i) = port_of(pid, pids::PID_PORT1_DMX_MBS) {
            ports[i].dmx_mbs = w;
        } else if let Some(i) = port_of(pid, pids::PID_PORT1_RDM_BG_DISCOVERY) {
            ports[i].bg_discovery = b;
        } else {
            let s = &mut self.settings;
            match pid {
                pids::PID_RDM_BG_DISCOVERY_INTERVAL => s.bg_interval = w,
                pids::PID_DISPLAY_DIM_TIMEOUT => s.dim_timeout = w,
                pids::PID_DISPLAY_ADDRESS_FORMAT => s.address_format = byte,
                pids::PID_PANEL_LOCK => s.panel_lock = b,
                _ => return Answer::Nack(NackReason::UNSUPPORTED_COMMAND_CLASS),
            }
        }
        Answer::Ack(Vec::new())
    }

    // -----------------------------------------------------------------------------------------------------------
    // BOOT_CATCH and panel SETs
    // -----------------------------------------------------------------------------------------------------------

    fn boot_catch_start(&mut self, pd: &[u8], now: Instant) -> Answer {
        let Ok(s) = BootCatchStart::decode(pd) else {
            return Answer::Nack(NackReason::FORMAT_ERROR);
        };
        let usable = usize::from(s.port)
            .checked_sub(1)
            .and_then(|i| self.ports.get(i))
            .is_some_and(|p| p.line_active());
        if !usable || s.target_uid.is_broadcast() {
            return Answer::Nack(NackReason::DATA_OUT_OF_RANGE);
        }
        self.boot_catch = BootCatch {
            state: boot_catch_state::RUNNING,
            port: s.port,
            target: s.target_uid,
            started: Some(now),
            elapsed_ms: 0,
            attempts: 0,
            last_attempt: None,
        };
        let status = self.boot_catch.status(now).encode();
        self.queue.push(pids::PID_BOOT_CATCH_STATUS, status, true);
        Answer::Ack(Vec::new())
    }

    fn boot_catch_finish(&mut self, state: u8, status: Option<(u8, u16)>, now: Instant) {
        let st = self.boot_catch.status(now);
        self.boot_catch.elapsed_ms = st.elapsed_ms;
        self.boot_catch.state = state;
        let data = self.boot_catch.status(now).encode();
        self.queue.push(pids::PID_BOOT_CATCH_STATUS, data, true);
        if let Some((kind, id)) = status {
            self.queue
                .push_status(kind, id, i16::from(self.boot_catch.port), 0);
        }
    }

    /// Every BOOT_CATCH_PERIOD_MS: GET BOOT_INFO from the node UID to the target, between other transactions.
    pub(super) fn boot_catch_tick(&mut self, now: Instant) {
        let bc = &self.boot_catch;
        if bc.state != boot_catch_state::RUNNING {
            return;
        }
        let Some(i) = usize::from(bc.port).checked_sub(1) else {
            return;
        };
        let elapsed = bc
            .started
            .map_or(Duration::ZERO, |t| now.saturating_duration_since(t));
        if !self.ports[i].line_active() {
            self.boot_catch_finish(boot_catch_state::ABORTED, None, now);
            return;
        }
        if elapsed >= Duration::from_millis(pids::BOOT_CATCH_DURATION_MS.into()) {
            let id = pids::status::BOOT_CATCH_TIMEOUT;
            self.boot_catch_finish(
                boot_catch_state::TIMEOUT,
                Some((status_type::WARNING, id)),
                now,
            );
            return;
        }
        let period = Duration::from_millis(pids::BOOT_CATCH_PERIOD_MS.into());
        if self.ports[i].in_flight.is_some()
            || bc
                .last_attempt
                .is_some_and(|t| now.saturating_duration_since(t) < period)
        {
            return;
        }
        let target = bc.target;
        self.tn = self.tn.wrapping_add(1);
        let req = RdmMessage::request(
            target,
            self.uid,
            self.tn,
            1,
            SUB_DEVICE_ROOT,
            cc::GET,
            pids::PID_BOOT_INFO,
            Vec::new(),
        );
        self.boot_catch.attempts = self.boot_catch.attempts.saturating_add(1);
        self.boot_catch.last_attempt = Some(now);
        let resp = self.ports[i].line_transaction(&req, now);
        self.ports[i].occupy(now + self.cfg.rdm_latency);
        let caught = resp.is_some_and(|r| {
            r.response_type() == Some(ResponseType::Ack) && r.parameter_id == pids::PID_BOOT_INFO
        });
        if caught {
            let id = pids::status::BOOT_CATCH_CAUGHT;
            self.boot_catch_finish(
                boot_catch_state::CAUGHT,
                Some((status_type::ADVISORY, id)),
                now,
            );
        }
    }

    /// A SET made at the panel to device `uid` on port 1..=4; reported when the device ACKs.
    pub(super) fn local_rdm_set(
        &mut self,
        port: u8,
        uid: Uid,
        pid: u16,
        data: Vec<u8>,
        now: Instant,
    ) -> bool {
        let Some(i) = usize::from(port)
            .checked_sub(1)
            .filter(|&i| i < super::node::SIM_PORTS)
        else {
            return false;
        };
        self.tn = self.tn.wrapping_add(1);
        let req = RdmMessage::request(
            uid,
            self.uid,
            self.tn,
            1,
            SUB_DEVICE_ROOT,
            cc::SET,
            pid,
            data,
        );
        let acked = self.ports[i]
            .line_transaction(&req, now)
            .is_some_and(|r| r.response_type() == Some(ResponseType::Ack));
        if acked {
            self.local_seq = self.local_seq.wrapping_add(1);
            let l = LocalRdmSet {
                sequence: self.local_seq,
                port,
                uid,
                sub_device: SUB_DEVICE_ROOT,
                pid,
            };
            self.queue.push(pids::PID_LOCAL_RDM_SET, l.encode(), false);
            self.queue.push_status(
                status_type::ADVISORY,
                pids::status::LOCAL_RDM_SET,
                i16::from(port),
                pid as i16,
            );
            self.local_set = Some(l);
        }
        acked
    }
}

fn fw_answer(r: Result<(), FwRefusal>) -> Answer {
    match r {
        Ok(()) => Answer::Ack(Vec::new()),
        Err(FwRefusal::OutOfRange) => Answer::Nack(NackReason::DATA_OUT_OF_RANGE),
        Err(FwRefusal::Busy) => Answer::Nack(NackReason::BUFFER_FULL),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_node_pid_has_a_spec() {
        for &pid in pids::NODE_STD_PIDS_REQUIRED
            .iter()
            .chain(pids::NODE_STD_PIDS_LISTED)
        {
            assert!(pid_spec(pid).is_some(), "standard PID 0x{pid:04X}");
        }
        for d in pids::PIDS.iter().filter(|d| d.device == Device::Node) {
            assert!(pid_spec(d.pid).is_some(), "{}", d.name);
        }
        assert!(pid_spec(pids::PID_BOOT_INFO).is_none());
        let list = supported_parameters();
        assert!(list.len() * 2 <= crate::rdm::MAX_PDL);
        for r in pids::NODE_STD_PIDS_REQUIRED {
            assert!(!list.contains(r));
        }
    }

    #[test]
    fn masks() {
        assert_eq!(mask_to_prefix(Ipv4Addr::new(255, 0, 0, 0)), 8);
        assert_eq!(prefix_to_mask(24), Ipv4Addr::new(255, 255, 255, 0));
        assert_eq!(prefix_to_mask(0), Ipv4Addr::UNSPECIFIED);
        assert_eq!(prefix_to_mask(32), Ipv4Addr::BROADCAST);
    }
}
