//! Simulated third-party RDM fixture (another manufacturer) to exercise the controller's generic RDM features:
//! personalities, slots, a sensor, a manufacturer PID with PARAMETER_DESCRIPTION, ACK_OVERFLOW for a long
//! SUPPORTED_PARAMETERS, ACK_TIMER with QUEUED_MESSAGE, status messages and fault injection (digest §4, §6, §7).

use super::tube::{
    be16, broadcast_for, check_rules, label_bytes, rule, set_bool, sub_device_ok, Pdl, Rule,
    MANUFACTURER_PID_MIN, RDM_PROTOCOL_1_0,
};
use super::SimResponder;
use crate::rdm::pd::{
    self, DefaultSlotValue, DeviceInfo, DiscMuteResponse, DmxPersonality, ParameterDescription,
    PersonalityDescription, SensorDefinition, SensorValue, SlotDescription, SlotInfo,
    StatusMessage,
};
use crate::rdm::{
    cc, pid, status_type, NackReason, RdmMessage, ResponseType, Uid, MAX_PDL, SUB_DEVICE_ROOT,
};
use std::any::Any;
use std::collections::VecDeque;
use std::time::{Duration, Instant};

/// E1.20 slot types / slot label IDs used by the default configuration.
pub mod slot {
    /// ST_PRIMARY.
    pub const ST_PRIMARY: u8 = 0x00;
    /// SD_INTENSITY.
    pub const SD_INTENSITY: u16 = 0x0001;
    /// SD_COLOR_ADD_RED.
    pub const SD_COLOR_ADD_RED: u16 = 0x0205;
    /// SD_COLOR_ADD_GREEN.
    pub const SD_COLOR_ADD_GREEN: u16 = 0x0206;
    /// SD_COLOR_ADD_BLUE.
    pub const SD_COLOR_ADD_BLUE: u16 = 0x0207;
    /// SD_COLOR_ADD_WHITE.
    pub const SD_COLOR_ADD_WHITE: u16 = 0x0212;
    /// SD_STROBE.
    pub const SD_STROBE: u16 = 0x0404;
}

/// Most entries in one STATUS_MESSAGES answer (25 × 9 B ≤ 231).
const MAX_STATUS_PER_ANSWER: usize = MAX_PDL / StatusMessage::LEN;
/// Most PIDs in one SUPPORTED_PARAMETERS chunk.
const MAX_PIDS_PER_ANSWER: usize = MAX_PDL / 2;

/// One DMX slot of a personality.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FixtureSlot {
    /// SLOT_INFO slot type (e.g. [`slot::ST_PRIMARY`]).
    pub slot_type: u8,
    /// SLOT_INFO slot label ID (e.g. [`slot::SD_INTENSITY`]).
    pub label_id: u16,
    /// SLOT_DESCRIPTION text.
    pub description: String,
    /// DEFAULT_SLOT_VALUE.
    pub default_value: u8,
}

impl FixtureSlot {
    /// A primary slot.
    pub fn primary(label_id: u16, description: &str, default_value: u8) -> Self {
        Self {
            slot_type: slot::ST_PRIMARY,
            label_id,
            description: description.into(),
            default_value,
        }
    }
}

/// One DMX personality (footprint = number of slots).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FixturePersonality {
    /// DMX_PERSONALITY_DESCRIPTION text.
    pub description: String,
    /// The slots.
    pub slots: Vec<FixtureSlot>,
}

/// One sensor: its definition (the `sensor` number is the index) and the current value.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FixtureSensor {
    /// SENSOR_DEFINITION answer.
    pub definition: SensorDefinition,
    /// Present value.
    pub value: i16,
}

/// Configuration of a [`SimFixture`]. `Default` = a plausible 2-personality RGBW LED par whose
/// SUPPORTED_PARAMETERS (15 PIDs) overflows once with the default `overflow_chunk` of 10.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FixtureConfig {
    /// MANUFACTURER_LABEL.
    pub manufacturer_label: String,
    /// DEVICE_INFO model ID.
    pub model_id: u16,
    /// DEVICE_MODEL_DESCRIPTION.
    pub model_description: String,
    /// DEVICE_INFO product category.
    pub product_category: u16,
    /// Initial DEVICE_LABEL.
    pub device_label: String,
    /// DEVICE_INFO software version ID.
    pub software_version_id: u32,
    /// SOFTWARE_VERSION_LABEL.
    pub software_version_label: String,
    /// Personalities 1..n.
    pub personalities: Vec<FixturePersonality>,
    /// Initial personality (1-based).
    pub personality: u8,
    /// Initial DMX start address.
    pub start_address: u16,
    /// Sensors 0..n.
    pub sensors: Vec<FixtureSensor>,
    /// The manufacturer-specific "Fan mode" PID (u8 0..=2, GET/SET, listed + PARAMETER_DESCRIPTION).
    pub fan_mode_pid: u16,
    /// Initial fan mode.
    pub fan_mode: u8,
    /// Initial DEVICE_HOURS.
    pub device_hours: u32,
    /// SUPPORTED_PARAMETERS answers with ACK_OVERFLOW chunks of this many PIDs (0 = never overflow; at most 115
    /// fit one answer anyway).
    pub overflow_chunk: usize,
    /// `(command class, PID)` pairs answered with ACK_TIMER (unicast only); the real answer is queued.
    pub ack_timer: Vec<(u8, u16)>,
    /// ACK_TIMER delay in 100 ms units.
    pub ack_timer_delay: u16,
}

impl Default for FixtureConfig {
    fn default() -> Self {
        use slot::*;
        let p = FixtureSlot::primary;
        Self {
            manufacturer_label: "Simulated Lighting".into(),
            model_id: 0x0042,
            model_description: "SimPar RGBW 7x10W".into(),
            product_category: 0x0101, // PRODUCT_CATEGORY_FIXTURE_FIXED
            device_label: String::new(),
            software_version_id: 0x0002_0103,
            software_version_label: "2.1.3".into(),
            personalities: vec![
                FixturePersonality {
                    description: "RGBW".into(),
                    slots: vec![
                        p(SD_COLOR_ADD_RED, "Red", 0),
                        p(SD_COLOR_ADD_GREEN, "Green", 0),
                        p(SD_COLOR_ADD_BLUE, "Blue", 0),
                        p(SD_COLOR_ADD_WHITE, "White", 0),
                    ],
                },
                FixturePersonality {
                    description: "Dimmer RGBW Strobe".into(),
                    slots: vec![
                        p(SD_INTENSITY, "Dimmer", 255),
                        p(SD_COLOR_ADD_RED, "Red", 0),
                        p(SD_COLOR_ADD_GREEN, "Green", 0),
                        p(SD_COLOR_ADD_BLUE, "Blue", 0),
                        p(SD_COLOR_ADD_WHITE, "White", 0),
                        p(SD_STROBE, "Strobe", 0),
                    ],
                },
            ],
            personality: 1,
            start_address: 1,
            sensors: vec![FixtureSensor {
                definition: SensorDefinition {
                    sensor: 0,
                    sensor_type: 0x00, // SENSOR_TEMPERATURE
                    unit: 0x01,        // UNITS_CENTIGRADE
                    prefix: 0x00,
                    range_min: -40,
                    range_max: 125,
                    normal_min: 0,
                    normal_max: 70,
                    recorded_support: 0x03,
                    description: "LED temperature".into(),
                },
                value: 35,
            }],
            fan_mode_pid: 0x8001,
            fan_mode: 0,
            device_hours: 1234,
            overflow_chunk: 10,
            ack_timer: vec![(cc::SET, pid::DEVICE_LABEL)],
            ack_timer_delay: 1,
        }
    }
}

/// A queued response: delivered by GET QUEUED_MESSAGE with its own PID and command class.
#[derive(Debug, Clone, PartialEq, Eq)]
struct Queued {
    pid: u16,
    cc: u8,
    data: Vec<u8>,
}

/// Successful outcome of one request.
enum Reply {
    Ack(Vec<u8>),
    Overflow(Vec<u8>),
    Queued(Queued),
}

/// Live sensor state.
#[derive(Debug, Clone, PartialEq, Eq)]
struct SensorState {
    present: i16,
    lowest: i16,
    highest: i16,
    recorded: i16,
}

/// A simulated third-party fixture (see module docs). Never answers broadcasts/vendorcasts (but executes SETs).
#[derive(Debug, Clone)]
pub struct SimFixture {
    /// Drop every n-th answer (0 = never); the request is still executed.
    pub lose_every: u32,
    uid: Uid,
    cfg: FixtureConfig,
    rules: Vec<Rule>,
    supported: Vec<u16>,
    label: Vec<u8>,
    personality: u8,
    start_address: u16,
    identify: bool,
    muted: bool,
    fan_mode: u8,
    device_hours: u32,
    sensors: Vec<SensorState>,
    queue: VecDeque<Queued>,
    pending: Vec<(Instant, Queued)>,
    last_delivered: Option<Queued>,
    status: Vec<StatusMessage>,
    last_status: Vec<StatusMessage>,
    /// ACK_OVERFLOW in progress: (controller, next index into `supported`).
    overflow: Option<(Uid, usize)>,
    drop_next: u32,
    nack_next: Vec<(u16, NackReason)>,
    answers: u32,
}

impl SimFixture {
    /// A fixture with `uid` (pick another manufacturer than the project's, e.g. `0x0001`) and `cfg`.
    pub fn new(uid: Uid, cfg: FixtureConfig) -> Self {
        let mut rules = vec![
            rule(pid::SUPPORTED_PARAMETERS, cc::GET, Pdl::Exact(0)),
            rule(pid::PARAMETER_DESCRIPTION, cc::GET, Pdl::Exact(2)),
            rule(pid::DEVICE_INFO, cc::GET, Pdl::Exact(0)),
            rule(pid::SOFTWARE_VERSION_LABEL, cc::GET, Pdl::Exact(0)),
            rule(pid::DEVICE_MODEL_DESCRIPTION, cc::GET, Pdl::Exact(0)),
            rule(pid::MANUFACTURER_LABEL, cc::GET, Pdl::Exact(0)),
            rule(pid::DEVICE_LABEL, cc::GET, Pdl::Exact(0)),
            rule(pid::DEVICE_LABEL, cc::SET, Pdl::Max(32)),
            rule(pid::DMX_START_ADDRESS, cc::GET, Pdl::Exact(0)),
            rule(pid::DMX_START_ADDRESS, cc::SET, Pdl::Exact(2)),
            rule(pid::DMX_PERSONALITY, cc::GET, Pdl::Exact(0)),
            rule(pid::DMX_PERSONALITY, cc::SET, Pdl::Exact(1)),
            rule(pid::DMX_PERSONALITY_DESCRIPTION, cc::GET, Pdl::Exact(1)),
            rule(pid::SLOT_INFO, cc::GET, Pdl::Exact(0)),
            rule(pid::SLOT_DESCRIPTION, cc::GET, Pdl::Exact(2)),
            rule(pid::DEFAULT_SLOT_VALUE, cc::GET, Pdl::Exact(0)),
            rule(pid::DEVICE_HOURS, cc::GET, Pdl::Exact(0)),
            rule(pid::DEVICE_HOURS, cc::SET, Pdl::Exact(4)),
            rule(pid::IDENTIFY_DEVICE, cc::GET, Pdl::Exact(0)),
            rule(pid::IDENTIFY_DEVICE, cc::SET, Pdl::Exact(1)),
            rule(pid::QUEUED_MESSAGE, cc::GET, Pdl::Exact(1)),
            rule(pid::STATUS_MESSAGES, cc::GET, Pdl::Exact(1)),
            rule(pid::CLEAR_STATUS_ID, cc::SET, Pdl::Exact(0)),
            rule(cfg.fan_mode_pid, cc::GET, Pdl::Exact(0)),
            rule(cfg.fan_mode_pid, cc::SET, Pdl::Exact(1)),
        ];
        let mut supported = vec![
            pid::QUEUED_MESSAGE,
            pid::STATUS_MESSAGES,
            pid::CLEAR_STATUS_ID,
            pid::DEVICE_MODEL_DESCRIPTION,
            pid::MANUFACTURER_LABEL,
            pid::DEVICE_LABEL,
            pid::DMX_PERSONALITY,
            pid::DMX_PERSONALITY_DESCRIPTION,
            pid::SLOT_INFO,
            pid::SLOT_DESCRIPTION,
            pid::DEFAULT_SLOT_VALUE,
        ];
        if !cfg.sensors.is_empty() {
            rules.extend([
                rule(pid::SENSOR_DEFINITION, cc::GET, Pdl::Exact(1)),
                rule(pid::SENSOR_VALUE, cc::GET, Pdl::Exact(1)),
                rule(pid::SENSOR_VALUE, cc::SET, Pdl::Exact(1)),
            ]);
            supported.extend([pid::SENSOR_DEFINITION, pid::SENSOR_VALUE]);
        }
        supported.extend([pid::DEVICE_HOURS, cfg.fan_mode_pid]);
        let sensors = cfg
            .sensors
            .iter()
            .map(|s| SensorState {
                present: s.value,
                lowest: s.value,
                highest: s.value,
                recorded: s.value,
            })
            .collect();
        Self {
            lose_every: 0,
            uid,
            rules,
            supported,
            label: pd::encode_label(&cfg.device_label),
            personality: cfg.personality,
            start_address: cfg.start_address,
            identify: false,
            muted: false,
            fan_mode: cfg.fan_mode,
            device_hours: cfg.device_hours,
            sensors,
            queue: VecDeque::new(),
            pending: Vec::new(),
            last_delivered: None,
            status: Vec::new(),
            last_status: Vec::new(),
            overflow: None,
            drop_next: 0,
            nack_next: Vec::new(),
            answers: 0,
            cfg,
        }
    }

    /// The default LED par ([`FixtureConfig::default`]).
    pub fn default_par(uid: Uid) -> Self {
        Self::new(uid, FixtureConfig::default())
    }

    /// The configuration it was built with.
    pub fn config(&self) -> &FixtureConfig {
        &self.cfg
    }

    /// The full SUPPORTED_PARAMETERS list (all chunks).
    pub fn supported_parameters(&self) -> &[u16] {
        &self.supported
    }

    /// Queue a response for GET QUEUED_MESSAGE (`command_class` of the response; GET/SET become
    /// GET_RESPONSE/SET_RESPONSE). It counts in the Message Count at once.
    pub fn queue_message(&mut self, pid: u16, command_class: u8, data: Vec<u8>) {
        self.queue.push_back(Queued {
            pid,
            cc: command_class | 0x01,
            data,
        });
    }

    /// Add a status message (returned by STATUS_MESSAGES, or by QUEUED_MESSAGE when the queue is empty).
    pub fn queue_status(&mut self, msg: StatusMessage) {
        self.status.push(msg);
    }

    /// Drop the next `n` answers (requests are still executed).
    pub fn drop_next(&mut self, n: u32) {
        self.drop_next = n;
    }

    /// Answer the next unicast GET/SET of `pid` with NACK `reason` (not executed).
    pub fn nack_next(&mut self, pid: u16, reason: NackReason) {
        self.nack_next.push((pid, reason));
    }

    /// Set a sensor's present value (lowest/highest follow).
    pub fn set_sensor_value(&mut self, sensor: usize, value: i16) {
        if let Some(s) = self.sensors.get_mut(sensor) {
            s.present = value;
            s.lowest = s.lowest.min(value);
            s.highest = s.highest.max(value);
        }
    }

    /// Queued messages ready for QUEUED_MESSAGE (= Message Count, capped at 255 on the wire).
    pub fn queued_count(&self) -> usize {
        self.queue.len()
    }

    /// ACK_TIMER answers not yet due.
    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }

    /// Status messages not yet collected.
    pub fn status_count(&self) -> usize {
        self.status.len()
    }

    /// Device label.
    pub fn label(&self) -> String {
        pd::decode_label(&self.label)
    }

    /// DMX start address.
    pub fn start_address(&self) -> u16 {
        self.start_address
    }

    /// Current personality (1-based).
    pub fn personality(&self) -> u8 {
        self.personality
    }

    /// IDENTIFY_DEVICE state.
    pub fn identify(&self) -> bool {
        self.identify
    }

    /// Fan mode (manufacturer PID).
    pub fn fan_mode(&self) -> u8 {
        self.fan_mode
    }

    /// DEVICE_HOURS.
    pub fn device_hours(&self) -> u32 {
        self.device_hours
    }

    /// Discovery mute flag.
    pub fn muted(&self) -> bool {
        self.muted
    }

    fn message_count(&self) -> u8 {
        self.queue.len().min(255) as u8
    }

    fn personality_cfg(&self, p: u8) -> Option<&FixturePersonality> {
        let i = usize::from(p).checked_sub(1)?;
        self.cfg.personalities.get(i)
    }

    fn footprint_of(&self, p: u8) -> Option<u16> {
        self.personality_cfg(p)
            .map(|p| p.slots.len().min(512) as u16)
    }

    fn slots(&self) -> &[FixtureSlot] {
        self.personality_cfg(self.personality)
            .map_or(&[], |p| p.slots.as_slice())
    }

    /// Move due ACK_TIMER answers into the queue (in due order).
    fn advance(&mut self, now: Instant) {
        let (due, later): (Vec<_>, Vec<_>) = self.pending.drain(..).partition(|(t, _)| *t <= now);
        self.pending = later;
        self.queue.extend(due.into_iter().map(|(_, q)| q));
    }

    fn sensor_value(&self, i: u8) -> Option<SensorValue> {
        let s = self.sensors.get(usize::from(i))?;
        Some(SensorValue {
            sensor: i,
            present: s.present,
            lowest: s.lowest,
            highest: s.highest,
            recorded: s.recorded,
        })
    }

    /// Collect status messages of severity ≥ `min` (2..4; the *_CLEARED types count as their base type),
    /// clear-on-read, at most 25.
    fn take_status(&mut self, min: u8) -> Vec<StatusMessage> {
        let mut out = Vec::new();
        let mut keep = Vec::new();
        for m in self.status.drain(..) {
            if (m.status_type & 0x0F) >= min && out.len() < MAX_STATUS_PER_ANSWER {
                out.push(m);
            } else {
                keep.push(m);
            }
        }
        self.status = keep;
        self.last_status = out.clone();
        out
    }

    fn respond(&mut self, req: &RdmMessage) -> Result<Reply, NackReason> {
        let d = req.parameter_data.as_slice();
        check_rules(&self.rules, req.command_class, req.parameter_id, d.len())?;
        let fp = self.footprint_of(self.personality).unwrap_or(0);
        let p = req.parameter_id;
        let ack = Reply::Ack;
        let out = match req.command_class {
            cc::GET => match p {
                pid::SUPPORTED_PARAMETERS => return Ok(self.supported_chunk(req.src_uid)),
                pid::PARAMETER_DESCRIPTION => {
                    if be16(d) != Some(self.cfg.fan_mode_pid) {
                        return Err(NackReason::DATA_OUT_OF_RANGE);
                    }
                    ack(ParameterDescription {
                        pid: self.cfg.fan_mode_pid,
                        pdl_size: 1,
                        data_type: 0x03, // DS_UNSIGNED_BYTE
                        command_class: 0x03,
                        param_type: 0,
                        unit: 0,
                        prefix: 0,
                        min_valid_value: 0,
                        max_valid_value: 2,
                        default_value: 0,
                        description: "Fan mode".into(),
                    }
                    .encode())
                }
                pid::DEVICE_INFO => ack(DeviceInfo {
                    rdm_protocol_version: RDM_PROTOCOL_1_0,
                    device_model_id: self.cfg.model_id,
                    product_category: self.cfg.product_category,
                    software_version_id: self.cfg.software_version_id,
                    dmx_footprint: fp,
                    current_personality: self.personality,
                    personality_count: self.cfg.personalities.len().min(255) as u8,
                    dmx_start_address: if fp > 0 { self.start_address } else { 0xFFFF },
                    sub_device_count: 0,
                    sensor_count: self.sensors.len().min(255) as u8,
                }
                .encode()),
                pid::SOFTWARE_VERSION_LABEL => {
                    ack(pd::encode_label(&self.cfg.software_version_label))
                }
                pid::DEVICE_MODEL_DESCRIPTION => ack(pd::encode_label(&self.cfg.model_description)),
                pid::MANUFACTURER_LABEL => ack(pd::encode_label(&self.cfg.manufacturer_label)),
                pid::DEVICE_LABEL => ack(self.label.clone()),
                pid::DMX_START_ADDRESS => ack(self.start_address.to_be_bytes().to_vec()),
                pid::DMX_PERSONALITY => ack(DmxPersonality {
                    current: self.personality,
                    count: self.cfg.personalities.len().min(255) as u8,
                }
                .encode()),
                pid::DMX_PERSONALITY_DESCRIPTION => {
                    let n = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                    let pers = self
                        .personality_cfg(n)
                        .ok_or(NackReason::DATA_OUT_OF_RANGE)?;
                    ack(PersonalityDescription {
                        personality: n,
                        slots_required: pers.slots.len().min(512) as u16,
                        description: pers.description.clone(),
                    }
                    .encode())
                }
                pid::SLOT_INFO => ack(SlotInfo::encode_list(
                    &self
                        .slots()
                        .iter()
                        .enumerate()
                        .map(|(i, s)| SlotInfo {
                            slot_offset: i as u16,
                            slot_type: s.slot_type,
                            slot_label_id: s.label_id,
                        })
                        .collect::<Vec<_>>(),
                )),
                pid::SLOT_DESCRIPTION => {
                    let s = be16(d).ok_or(NackReason::FORMAT_ERROR)?;
                    let slot = self
                        .slots()
                        .get(usize::from(s))
                        .ok_or(NackReason::DATA_OUT_OF_RANGE)?;
                    ack(SlotDescription {
                        slot: s,
                        description: slot.description.clone(),
                    }
                    .encode())
                }
                pid::DEFAULT_SLOT_VALUE => ack(DefaultSlotValue::encode_list(
                    &self
                        .slots()
                        .iter()
                        .enumerate()
                        .map(|(i, s)| DefaultSlotValue {
                            slot_offset: i as u16,
                            value: s.default_value,
                        })
                        .collect::<Vec<_>>(),
                )),
                pid::SENSOR_DEFINITION => {
                    let n = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                    let s = self
                        .cfg
                        .sensors
                        .get(usize::from(n))
                        .ok_or(NackReason::DATA_OUT_OF_RANGE)?;
                    let mut def = s.definition.clone();
                    def.sensor = n;
                    ack(def.encode())
                }
                pid::SENSOR_VALUE => {
                    let n = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                    let v = self.sensor_value(n).ok_or(NackReason::DATA_OUT_OF_RANGE)?;
                    ack(v.encode())
                }
                pid::DEVICE_HOURS => ack(self.device_hours.to_be_bytes().to_vec()),
                pid::IDENTIFY_DEVICE => ack(vec![u8::from(self.identify)]),
                pid::QUEUED_MESSAGE => return self.queued_message(d),
                pid::STATUS_MESSAGES => {
                    let t = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                    let list = match t {
                        status_type::NONE => Vec::new(),
                        status_type::GET_LAST_MESSAGE => self.last_status.clone(),
                        status_type::ADVISORY..=status_type::ERROR => self.take_status(t),
                        _ => return Err(NackReason::DATA_OUT_OF_RANGE),
                    };
                    ack(StatusMessage::encode_list(&list))
                }
                _ if p == self.cfg.fan_mode_pid => ack(vec![self.fan_mode]),
                _ => return Err(NackReason::UNKNOWN_PID),
            },
            _ => {
                match p {
                    pid::DEVICE_LABEL => self.label = label_bytes(d),
                    pid::DMX_START_ADDRESS => {
                        let a = be16(d).ok_or(NackReason::FORMAT_ERROR)?;
                        if a < 1 || u32::from(a) + u32::from(fp) > 513 {
                            return Err(NackReason::DATA_OUT_OF_RANGE);
                        }
                        self.start_address = a;
                    }
                    pid::DMX_PERSONALITY => {
                        let n = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                        let f = self.footprint_of(n).ok_or(NackReason::DATA_OUT_OF_RANGE)?;
                        if u32::from(self.start_address) + u32::from(f) > 513 {
                            return Err(NackReason::DATA_OUT_OF_RANGE);
                        }
                        self.personality = n;
                    }
                    pid::SENSOR_VALUE => return self.reset_sensor(d),
                    pid::DEVICE_HOURS => {
                        let [a, b, c, e] = d else {
                            return Err(NackReason::FORMAT_ERROR);
                        };
                        self.device_hours = u32::from_be_bytes([*a, *b, *c, *e]);
                    }
                    pid::IDENTIFY_DEVICE => self.identify = set_bool(d)?,
                    pid::CLEAR_STATUS_ID => self.status.clear(),
                    _ if p == self.cfg.fan_mode_pid => {
                        let m = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                        if m > 2 {
                            return Err(NackReason::DATA_OUT_OF_RANGE);
                        }
                        self.fan_mode = m;
                    }
                    _ => return Err(NackReason::UNKNOWN_PID),
                }
                ack(Vec::new())
            }
        };
        Ok(out)
    }

    /// SET SENSOR_VALUE: reset lowest/highest/recorded of one sensor (or all with 0xFF).
    fn reset_sensor(&mut self, d: &[u8]) -> Result<Reply, NackReason> {
        let n = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
        let reset = |s: &mut SensorState| {
            s.lowest = s.present;
            s.highest = s.present;
            s.recorded = s.present;
        };
        if n == 0xFF {
            self.sensors.iter_mut().for_each(reset);
            let all = SensorValue {
                sensor: 0xFF,
                present: 0,
                lowest: 0,
                highest: 0,
                recorded: 0,
            };
            return Ok(Reply::Ack(all.encode()));
        }
        let s = self
            .sensors
            .get_mut(usize::from(n))
            .ok_or(NackReason::DATA_OUT_OF_RANGE)?;
        reset(s);
        let v = self.sensor_value(n).ok_or(NackReason::DATA_OUT_OF_RANGE)?;
        Ok(Reply::Ack(v.encode()))
    }

    /// Next SUPPORTED_PARAMETERS chunk for `src` (ACK_OVERFLOW while more follow).
    fn supported_chunk(&mut self, src: Uid) -> Reply {
        let chunk = match self.cfg.overflow_chunk {
            0 => MAX_PIDS_PER_ANSWER,
            n => n.min(MAX_PIDS_PER_ANSWER),
        };
        let start = match self.overflow {
            Some((uid, next)) if uid == src => next,
            _ => 0,
        };
        let end = (start + chunk).min(self.supported.len());
        let data = pd::encode_pid_list(self.supported.get(start..end).unwrap_or(&[]));
        if end < self.supported.len() {
            self.overflow = Some((src, end));
            Reply::Overflow(data)
        } else {
            self.overflow = None;
            Reply::Ack(data)
        }
    }

    /// GET QUEUED_MESSAGE (digest §4.3).
    fn queued_message(&mut self, d: &[u8]) -> Result<Reply, NackReason> {
        let t = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
        if !(status_type::GET_LAST_MESSAGE..=status_type::ERROR).contains(&t) {
            return Err(NackReason::DATA_OUT_OF_RANGE);
        }
        let empty_status = || Queued {
            pid: pid::STATUS_MESSAGES,
            cc: cc::GET_RESPONSE,
            data: Vec::new(),
        };
        if t == status_type::GET_LAST_MESSAGE {
            return Ok(Reply::Queued(
                self.last_delivered.clone().unwrap_or_else(empty_status),
            ));
        }
        let q = match self.queue.pop_front() {
            Some(q) => q,
            None => {
                // queue empty: the status messages of this severity (empty list = nothing)
                let list = self.take_status(t);
                Queued {
                    data: StatusMessage::encode_list(&list),
                    ..empty_status()
                }
            }
        };
        self.last_delivered = Some(q.clone());
        Ok(Reply::Queued(q))
    }
}

impl SimResponder for SimFixture {
    fn uid(&self) -> Uid {
        self.uid
    }

    fn handle(&mut self, req: &RdmMessage, now: Instant) -> Option<RdmMessage> {
        self.advance(now);
        let bcast = broadcast_for(self.uid, req.dest_uid)?;
        if bcast && req.dest_uid.is_all_broadcast() && req.parameter_id >= MANUFACTURER_PID_MIN {
            return None;
        }
        let p = req.parameter_id;
        let mut timer = None;
        let reply = match req.command_class {
            cc::DISCOVERY => match p {
                pid::DISC_MUTE | pid::DISC_UN_MUTE if req.parameter_data.is_empty() => {
                    self.muted = p == pid::DISC_MUTE;
                    let cf = DiscMuteResponse {
                        control_field: 0,
                        binding_uid: None,
                    };
                    Ok(Reply::Ack(cf.encode()))
                }
                _ => return None, // DISC_UNIQUE_BRANCH is simulated by the node
            },
            cc::GET | cc::SET => {
                if bcast && req.command_class == cc::GET {
                    return None;
                }
                if p != pid::SUPPORTED_PARAMETERS {
                    self.overflow = None;
                }
                let injected = if bcast {
                    None
                } else {
                    self.nack_next
                        .iter()
                        .position(|(x, _)| *x == p)
                        .map(|i| self.nack_next.remove(i).1)
                };
                if let Some(r) = injected {
                    Err(r)
                } else if !sub_device_ok(req.command_class, req.sub_device) {
                    Err(NackReason::SUB_DEVICE_OUT_OF_RANGE)
                } else {
                    let r = self.respond(req);
                    if let Ok(Reply::Ack(data)) = &r {
                        if !bcast && self.cfg.ack_timer.contains(&(req.command_class, p)) {
                            let delay = self.cfg.ack_timer_delay;
                            let due = now + Duration::from_millis(u64::from(delay) * 100);
                            let q = Queued {
                                pid: p,
                                cc: req.command_class | 0x01,
                                data: data.clone(),
                            };
                            self.pending.push((due, q));
                            timer = Some(delay);
                        }
                    }
                    r
                }
            }
            _ => return None,
        };
        if bcast {
            return None;
        }
        self.advance(now); // a zero ACK_TIMER delay is due at once
        let mc = self.message_count();
        let resp = match (timer, reply) {
            (Some(delay), _) => req.response_to(
                self.uid,
                ResponseType::AckTimer,
                mc,
                delay.to_be_bytes().to_vec(),
            ),
            (None, Ok(Reply::Ack(data))) => req.response_to(self.uid, ResponseType::Ack, mc, data),
            (None, Ok(Reply::Overflow(data))) => {
                req.response_to(self.uid, ResponseType::AckOverflow, mc, data)
            }
            (None, Ok(Reply::Queued(q))) => {
                let mut r = req.response_to(self.uid, ResponseType::Ack, mc, q.data);
                r.parameter_id = q.pid;
                r.command_class = q.cc;
                r.sub_device = SUB_DEVICE_ROOT;
                r
            }
            (None, Err(r)) => req.response_to(
                self.uid,
                ResponseType::NackReason,
                mc,
                r.0.to_be_bytes().to_vec(),
            ),
        };
        if self.drop_next > 0 {
            self.drop_next -= 1;
            return None;
        }
        self.answers += 1;
        if self.lose_every > 0 && self.answers.is_multiple_of(self.lose_every) {
            return None;
        }
        Some(resp)
    }

    fn tick(&mut self, now: Instant) {
        self.advance(now);
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

#[cfg(test)]
mod tests {
    use super::super::tube::test_util::*;
    use super::*;

    const UID: Uid = Uid::new(0x0001, 0x0000_0042);

    fn par() -> SimFixture {
        SimFixture::default_par(UID)
    }

    /// GET SUPPORTED_PARAMETERS the way OLA does it: repeat while ACK_OVERFLOW, concatenate.
    fn supported(c: &mut Ctl, f: &mut SimFixture) -> (Vec<u16>, usize) {
        let mut all = Vec::new();
        for n in 1..20 {
            let r = c
                .send(f, UID, cc::GET, pid::SUPPORTED_PARAMETERS, &[])
                .expect("answer");
            all.extend(pd::decode_pid_list(&r.parameter_data).expect("list"));
            match r.response_type() {
                Some(ResponseType::AckOverflow) => continue,
                Some(ResponseType::Ack) => return (all, n),
                other => panic!("unexpected {other:?}"),
            }
        }
        panic!("overflow never ended");
    }

    #[test]
    fn supported_parameters_overflow_concatenates() {
        let mut f = par();
        let mut c = Ctl::new();
        let (list, answers) = supported(&mut c, &mut f);
        assert_eq!(answers, 2, "default config overflows once");
        assert_eq!(list, f.supported_parameters());
        assert_eq!(list.len(), 15);
        assert!(list.contains(&0x8001) && !list.contains(&pid::DEVICE_INFO));
        assert!(!list.contains(&pid::IDENTIFY_DEVICE) && !list.contains(&pid::DMX_START_ADDRESS));
        // again from the start
        assert_eq!(supported(&mut c, &mut f).0, list);
        // an interleaved request restarts the overflow
        let r = c
            .send(&mut f, UID, cc::GET, pid::SUPPORTED_PARAMETERS, &[])
            .expect("a");
        assert_eq!(r.response_type(), Some(ResponseType::AckOverflow));
        ack(c.send(&mut f, UID, cc::GET, pid::DEVICE_INFO, &[]));
        assert_eq!(supported(&mut c, &mut f), (list.clone(), 2));
        // chunk 4: four answers; chunk 0: one
        let cfg = FixtureConfig {
            overflow_chunk: 4,
            ..Default::default()
        };
        let mut f = SimFixture::new(UID, cfg);
        assert_eq!(supported(&mut c, &mut f), (list.clone(), 4));
        let cfg = FixtureConfig {
            overflow_chunk: 0,
            ..Default::default()
        };
        let mut f = SimFixture::new(UID, cfg);
        assert_eq!(supported(&mut c, &mut f), (list, 1));
    }

    #[test]
    fn ack_timer_then_queued_message() {
        let mut f = par();
        let mut c = Ctl::new();
        let r = c
            .send(&mut f, UID, cc::SET, pid::DEVICE_LABEL, b"Stage left")
            .expect("answer");
        assert_eq!(r.response_type(), Some(ResponseType::AckTimer));
        assert_eq!(r.ack_timer_delay(), Some(Duration::from_millis(100)));
        assert_eq!(r.message_count, 0);
        // not yet: empty STATUS_MESSAGES
        let qm = |c: &mut Ctl, f: &mut SimFixture, t: u8| {
            c.send(f, UID, cc::GET, pid::QUEUED_MESSAGE, &[t])
                .expect("answer")
        };
        let r = qm(&mut c, &mut f, status_type::ERROR);
        let got = (r.parameter_id, r.command_class, r.parameter_data.len());
        assert_eq!(got, (pid::STATUS_MESSAGES, cc::GET_RESPONSE, 0));
        c.wait(&mut f, Duration::from_millis(100));
        let r = c
            .send(&mut f, UID, cc::GET, pid::DEVICE_INFO, &[])
            .expect("answer");
        assert_eq!(r.message_count, 1);
        let r = qm(&mut c, &mut f, status_type::ADVISORY);
        assert_eq!(r.response_type(), Some(ResponseType::Ack));
        let got = (r.parameter_id, r.command_class, r.message_count);
        assert_eq!(got, (pid::DEVICE_LABEL, cc::SET_RESPONSE, 0));
        assert!(r.parameter_data.is_empty());
        assert_eq!(f.label(), "Stage left");
        // lost answer: GET_LAST_MESSAGE repeats it
        let r = qm(&mut c, &mut f, status_type::GET_LAST_MESSAGE);
        assert_eq!(
            (r.parameter_id, r.command_class),
            (pid::DEVICE_LABEL, cc::SET_RESPONSE)
        );
        // a configurable GET ACK_TIMER queues the GET_RESPONSE with its data
        let cfg = FixtureConfig {
            ack_timer: vec![(cc::GET, pid::DEVICE_HOURS)],
            ack_timer_delay: 5,
            ..Default::default()
        };
        let mut f = SimFixture::new(UID, cfg);
        let r = c
            .send(&mut f, UID, cc::GET, pid::DEVICE_HOURS, &[])
            .expect("answer");
        assert_eq!(r.ack_timer_delay(), Some(Duration::from_millis(500)));
        c.wait(&mut f, Duration::from_millis(400));
        assert_eq!((f.pending_count(), f.queued_count()), (1, 0));
        c.wait(&mut f, Duration::from_millis(100));
        assert_eq!((f.pending_count(), f.queued_count()), (0, 1));
        let r = qm(&mut c, &mut f, status_type::ERROR);
        let got = (r.parameter_id, r.command_class, r.parameter_data);
        assert_eq!(
            got,
            (
                pid::DEVICE_HOURS,
                cc::GET_RESPONSE,
                1234u32.to_be_bytes().to_vec()
            )
        );
        // broadcast SET of an ACK_TIMER PID: executed, nothing queued, no answer
        let mut f = par();
        assert!(c
            .send(&mut f, Uid::BROADCAST, cc::SET, pid::DEVICE_LABEL, b"all")
            .is_none());
        assert_eq!(
            (f.label().as_str(), f.pending_count(), f.queued_count()),
            ("all", 0, 0)
        );
    }

    #[test]
    fn message_count_and_status_messages() {
        let mut f = par();
        let mut c = Ctl::new();
        f.queue_message(pid::DMX_START_ADDRESS, cc::SET, Vec::new());
        f.queue_message(pid::DEVICE_HOURS, cc::GET_RESPONSE, vec![0, 0, 0, 9]);
        let r = c
            .send(&mut f, UID, cc::GET, pid::IDENTIFY_DEVICE, &[])
            .expect("a");
        assert_eq!(r.message_count, 2);
        let qm = |c: &mut Ctl, f: &mut SimFixture, t: u8| {
            c.send(f, UID, cc::GET, pid::QUEUED_MESSAGE, &[t])
                .expect("answer")
        };
        let r = qm(&mut c, &mut f, status_type::ERROR);
        let got = (r.parameter_id, r.command_class, r.message_count);
        assert_eq!(got, (pid::DMX_START_ADDRESS, cc::SET_RESPONSE, 1));
        let r = qm(&mut c, &mut f, status_type::ERROR);
        let got = (r.parameter_id, r.parameter_data.as_slice(), r.message_count);
        assert_eq!(got, (pid::DEVICE_HOURS, &[0u8, 0, 0, 9][..], 0));

        let adv = StatusMessage {
            sub_device: 0,
            status_type: status_type::ADVISORY,
            status_message_id: 0x0050,
            data_value1: 1,
            data_value2: 0,
        };
        let err = StatusMessage {
            status_type: status_type::ERROR,
            status_message_id: 0x0021,
            data_value1: 85,
            ..adv.clone()
        };
        f.queue_status(adv.clone());
        f.queue_status(err.clone());
        let get = |c: &mut Ctl, f: &mut SimFixture, t: u8| {
            let pd = ack(c.send(f, UID, cc::GET, pid::STATUS_MESSAGES, &[t]));
            StatusMessage::decode_list(&pd).expect("list")
        };
        assert_eq!(get(&mut c, &mut f, status_type::ERROR), vec![err.clone()]);
        assert_eq!(
            get(&mut c, &mut f, status_type::ADVISORY),
            vec![adv.clone()]
        );
        assert_eq!(
            get(&mut c, &mut f, status_type::GET_LAST_MESSAGE),
            vec![adv.clone()]
        );
        assert_eq!(get(&mut c, &mut f, status_type::ADVISORY), vec![]);
        // empty queue: QUEUED_MESSAGE returns the status messages
        f.queue_status(err.clone());
        let r = qm(&mut c, &mut f, status_type::WARNING);
        assert_eq!(r.parameter_id, pid::STATUS_MESSAGES);
        assert_eq!(
            StatusMessage::decode_list(&r.parameter_data).ok(),
            Some(vec![err])
        );
        // CLEAR_STATUS_ID
        f.queue_status(adv);
        ack(c.send(&mut f, UID, cc::SET, pid::CLEAR_STATUS_ID, &[]));
        assert_eq!(f.status_count(), 0);
        let r = c.send(&mut f, UID, cc::GET, pid::STATUS_MESSAGES, &[5]);
        assert_eq!(nack(r), NackReason::DATA_OUT_OF_RANGE);
    }

    #[test]
    fn nack_cases() {
        let mut f = par();
        let mut c = Ctl::new();
        let cases: &[(u16, u8, u16, &[u8], NackReason)] = &[
            (3, cc::GET, 0x7777, &[], NackReason::SUB_DEVICE_OUT_OF_RANGE),
            (
                0xFFFF,
                cc::GET,
                pid::DEVICE_INFO,
                &[],
                NackReason::SUB_DEVICE_OUT_OF_RANGE,
            ),
            (0, cc::GET, 0x7777, &[], NackReason::UNKNOWN_PID),
            (0, cc::GET, 0x8000, &[], NackReason::UNKNOWN_PID),
            (
                0,
                cc::SET,
                pid::SLOT_INFO,
                &[],
                NackReason::UNSUPPORTED_COMMAND_CLASS,
            ),
            (
                0,
                cc::SET,
                pid::QUEUED_MESSAGE,
                &[4],
                NackReason::UNSUPPORTED_COMMAND_CLASS,
            ),
            (
                0,
                cc::GET,
                pid::QUEUED_MESSAGE,
                &[],
                NackReason::FORMAT_ERROR,
            ),
            (
                0,
                cc::GET,
                pid::DEVICE_INFO,
                &[0; 231],
                NackReason::FORMAT_ERROR,
            ),
            (0, cc::SET, 0x8001, &[], NackReason::FORMAT_ERROR),
            (
                0,
                cc::SET,
                pid::DEVICE_LABEL,
                &[b'x'; 33],
                NackReason::FORMAT_ERROR,
            ),
            (0, cc::SET, 0x8001, &[3], NackReason::DATA_OUT_OF_RANGE),
            (
                0,
                cc::GET,
                pid::QUEUED_MESSAGE,
                &[0],
                NackReason::DATA_OUT_OF_RANGE,
            ),
            (
                0,
                cc::GET,
                pid::SENSOR_VALUE,
                &[1],
                NackReason::DATA_OUT_OF_RANGE,
            ),
            (
                0,
                cc::GET,
                pid::DMX_PERSONALITY_DESCRIPTION,
                &[3],
                NackReason::DATA_OUT_OF_RANGE,
            ),
            (
                0,
                cc::GET,
                pid::PARAMETER_DESCRIPTION,
                &[0x00, 0x60],
                NackReason::DATA_OUT_OF_RANGE,
            ),
            (
                0,
                cc::SET,
                pid::DMX_START_ADDRESS,
                &[0x01, 0xFE],
                NackReason::DATA_OUT_OF_RANGE,
            ),
            (
                0,
                cc::SET,
                pid::IDENTIFY_DEVICE,
                &[2],
                NackReason::DATA_OUT_OF_RANGE,
            ),
        ];
        for &(sd, k, p, d, want) in cases {
            assert_eq!(
                nack(c.send_sd(&mut f, UID, sd, k, p, d)),
                want,
                "{k:#x} {p:#06x}"
            );
        }
        // the ACK_TIMER PID with a bad value was NACKed at once, nothing queued
        assert_eq!(f.pending_count(), 0);
        // injected NACK: once, request not executed
        f.nack_next(pid::IDENTIFY_DEVICE, NackReason::HARDWARE_FAULT);
        let r = c.send(&mut f, UID, cc::SET, pid::IDENTIFY_DEVICE, &[1]);
        assert_eq!(nack(r), NackReason::HARDWARE_FAULT);
        assert!(!f.identify());
        ack(c.send(&mut f, UID, cc::SET, pid::IDENTIFY_DEVICE, &[1]));
        assert!(f.identify());
    }

    #[test]
    fn parameters() {
        let mut f = par();
        let mut c = Ctl::new();
        let get =
            |c: &mut Ctl, f: &mut SimFixture, p: u16, d: &[u8]| ack(c.send(f, UID, cc::GET, p, d));
        let di = DeviceInfo::decode(&get(&mut c, &mut f, pid::DEVICE_INFO, &[])).expect("di");
        let got = (
            di.device_model_id,
            di.dmx_footprint,
            di.current_personality,
            di.personality_count,
            di.sensor_count,
        );
        assert_eq!(got, (0x0042, 4, 1, 2, 1));
        let label = get(&mut c, &mut f, pid::MANUFACTURER_LABEL, &[]);
        assert_eq!(pd::decode_label(&label), "Simulated Lighting");
        let raw = get(&mut c, &mut f, pid::PARAMETER_DESCRIPTION, &[0x80, 0x01]);
        let pdesc = ParameterDescription::decode(&raw).expect("pd");
        let got = (
            pdesc.pid,
            pdesc.command_class,
            pdesc.max_valid_value,
            pdesc.param_type,
        );
        assert_eq!(got, (0x8001, 0x03, 2, 0));
        assert_eq!(pdesc.description, "Fan mode");
        ack(c.send(&mut f, UID, cc::SET, 0x8001, &[2]));
        assert_eq!(get(&mut c, &mut f, 0x8001, &[]), vec![2]);
        ack(c.send(&mut f, UID, cc::SET, pid::DMX_PERSONALITY, &[2]));
        let slots = SlotInfo::decode_list(&get(&mut c, &mut f, pid::SLOT_INFO, &[])).expect("si");
        assert_eq!(slots.len(), 6);
        assert_eq!(slots[5].slot_label_id, slot::SD_STROBE);
        let raw = get(&mut c, &mut f, pid::DEFAULT_SLOT_VALUE, &[]);
        assert_eq!(
            DefaultSlotValue::decode_list(&raw).expect("dv")[0].value,
            255
        );
        let raw = get(&mut c, &mut f, pid::SLOT_DESCRIPTION, &[0, 4]);
        assert_eq!(
            SlotDescription::decode(&raw).expect("sd").description,
            "White"
        );
        let raw = get(&mut c, &mut f, pid::SENSOR_DEFINITION, &[0]);
        let def = SensorDefinition::decode(&raw).expect("def");
        assert_eq!(
            (def.unit, def.description.as_str()),
            (0x01, "LED temperature")
        );
        f.set_sensor_value(0, 50);
        let v = SensorValue::decode(&get(&mut c, &mut f, pid::SENSOR_VALUE, &[0])).expect("v");
        assert_eq!((v.present, v.lowest, v.highest), (50, 35, 50));
        let raw = ack(c.send(&mut f, UID, cc::SET, pid::SENSOR_VALUE, &[0]));
        let v = SensorValue::decode(&raw).expect("v");
        assert_eq!(
            (v.present, v.lowest, v.highest, v.recorded),
            (50, 50, 50, 50)
        );
        ack(c.send(&mut f, UID, cc::SET, pid::DEVICE_HOURS, &7u32.to_be_bytes()));
        assert_eq!(f.device_hours(), 7);
        ack(c.send_sd(
            &mut f,
            UID,
            0xFFFF,
            cc::SET,
            pid::DMX_START_ADDRESS,
            &[0, 100],
        ));
        assert_eq!(f.start_address(), 100);
        let r = c
            .send(&mut f, UID, cc::DISCOVERY, pid::DISC_MUTE, &[])
            .expect("mute");
        let cf = DiscMuteResponse::decode(&r.parameter_data).map(|m| m.control_field);
        assert_eq!(cf.ok(), Some(0));
        assert!(f.muted());
        assert!(c
            .send(
                &mut f,
                UID,
                cc::DISCOVERY,
                pid::DISC_UNIQUE_BRANCH,
                &[0; 12]
            )
            .is_none());
    }

    #[test]
    fn broadcasts_and_fault_injection() {
        let mut f = par();
        let mut c = Ctl::new();
        assert!(c
            .send(&mut f, Uid::BROADCAST, cc::SET, pid::IDENTIFY_DEVICE, &[1])
            .is_none());
        assert!(f.identify());
        let own_vc = Uid::new(0x0001, 0xFFFF_FFFF);
        assert!(c
            .send(&mut f, own_vc, cc::SET, pid::DMX_START_ADDRESS, &[0, 9])
            .is_none());
        assert_eq!(f.start_address(), 9);
        let other_vc = Uid::new(0x7FF0, 0xFFFF_FFFF);
        assert!(c
            .send(&mut f, other_vc, cc::SET, pid::DMX_START_ADDRESS, &[0, 5])
            .is_none());
        assert!(c
            .send(&mut f, Uid::BROADCAST, cc::SET, 0x8001, &[1])
            .is_none());
        assert_eq!((f.start_address(), f.fan_mode()), (9, 0), "ignored");
        assert!(c
            .send(&mut f, Uid::BROADCAST, cc::GET, pid::DEVICE_INFO, &[])
            .is_none());
        // drop_next: executed, answer lost
        f.drop_next(2);
        assert!(c
            .send(&mut f, UID, cc::SET, pid::DMX_START_ADDRESS, &[0, 20])
            .is_none());
        assert!(c
            .send(&mut f, UID, cc::GET, pid::DEVICE_INFO, &[])
            .is_none());
        assert_eq!(f.start_address(), 20);
        ack(c.send(&mut f, UID, cc::GET, pid::DEVICE_INFO, &[]));
        f.lose_every = 3;
        let lost = (0..6)
            .filter(|_| {
                c.send(&mut f, UID, cc::GET, pid::IDENTIFY_DEVICE, &[])
                    .is_none()
            })
            .count();
        assert_eq!(lost, 2);
    }
}
