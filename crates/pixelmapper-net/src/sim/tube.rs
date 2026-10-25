//! Simulated LED tube: the app `tube_fw` 0.5 and the RDM bootloader `tube_boot_rdm` v3 as an RDM controller
//! sees them. Reference semantics: `FakeTube` in `tube/tools/flash_rdm.py`, `tube/boot_rdm/src/main.c` and
//! `tube/firmware/src/rdm.c` (contract §9.1, digest §8).
//!
//! Time model: the node calls [`SimResponder::handle`] / [`SimResponder::tick`] with the current time; all timers
//! (boot window, 60 s bootloader timeout, silent phases after a reset) are evaluated against it. A page write
//! keeps the bootloader busy for a number of *requests* ([`SimTube::busy_requests`]), not for a time, like
//! `FakeTube`.

use super::SimResponder;
use crate::crc::crc16;
use crate::image::tube::{trailer_check, version_str, Trailer, BOOT_START, CRC_CHUNK, PAGE};
use crate::image::TubeImage;
use crate::pids;
use crate::rdm::pd::{
    self, BootInfo, DefaultSlotValue, DeviceInfo, DiscMuteResponse, DmxPersonality,
    ParameterDescription, PersonalityDescription, SlotDescription, SlotInfo,
};
use crate::rdm::{
    cc, pid, NackReason, RdmMessage, ResponseType, Uid, SUB_DEVICE_ALL, SUB_DEVICE_ROOT,
};
use std::any::Any;
use std::time::{Duration, Instant};

// ---------------------------------------------------------------------------------------------------------------
// Helpers shared with the other simulated responders
// ---------------------------------------------------------------------------------------------------------------

/// Allowed request PDL of a (PID, CC) pair.
#[derive(Debug, Clone, Copy)]
pub(crate) enum Pdl {
    /// Exactly this many bytes.
    Exact(u8),
    /// At most this many bytes.
    Max(u8),
}

/// One supported (PID, command class) pair with its request PDL (the bootloader's `pids[]` table pattern).
#[derive(Debug, Clone, Copy)]
pub(crate) struct Rule {
    pub(crate) pid: u16,
    pub(crate) cc: u8,
    pub(crate) pdl: Pdl,
}

/// Shorthand constructor for [`Rule`].
pub(crate) const fn rule(pid: u16, cc: u8, pdl: Pdl) -> Rule {
    Rule { pid, cc, pdl }
}

/// Table lookup in the NACK order of digest §8: unknown PID → UNKNOWN_PID, known PID with another CC →
/// UNSUPPORTED_COMMAND_CLASS, wrong PDL → FORMAT_ERROR. (The sub-device check comes before, value checks after.)
pub(crate) fn check_rules(rules: &[Rule], cc: u8, pid: u16, pdl: usize) -> Result<(), NackReason> {
    let mut known = false;
    for r in rules.iter().filter(|r| r.pid == pid) {
        known = true;
        if r.cc == cc {
            let ok = match r.pdl {
                Pdl::Exact(n) => pdl == usize::from(n),
                Pdl::Max(n) => pdl <= usize::from(n),
            };
            return if ok {
                Ok(())
            } else {
                Err(NackReason::FORMAT_ERROR)
            };
        }
    }
    Err(if known {
        NackReason::UNSUPPORTED_COMMAND_CLASS
    } else {
        NackReason::UNKNOWN_PID
    })
}

/// Root only; ALL_SUB_DEVICES only for SET (digest §5).
pub(crate) fn sub_device_ok(command_class: u8, sub_device: u16) -> bool {
    sub_device == SUB_DEVICE_ROOT || (command_class == cc::SET && sub_device == SUB_DEVICE_ALL)
}

/// `Some(false)` = unicast to `me`, `Some(true)` = broadcast or own vendorcast, `None` = not for `me`.
pub(crate) fn broadcast_for(me: Uid, dest: Uid) -> Option<bool> {
    if dest == me {
        Some(false)
    } else if me.addressed_to(dest) {
        Some(true)
    } else {
        None
    }
}

/// First big-endian u16 of a PD.
pub(crate) fn be16(d: &[u8]) -> Option<u16> {
    match d {
        [a, b, ..] => Some(u16::from_be_bytes([*a, *b])),
        _ => None,
    }
}

/// Text of a label SET: up to the first NUL (E1.20 10.1), at most 32 bytes.
pub(crate) fn label_bytes(d: &[u8]) -> Vec<u8> {
    d.iter()
        .take(pd::MAX_LABEL)
        .take_while(|&&b| b != 0)
        .copied()
        .collect()
}

/// Start of the manufacturer PID range (digest §6): such PIDs to FFFF:FFFFFFFF are ignored (E1.20 6.2.10.2).
pub(crate) const MANUFACTURER_PID_MIN: u16 = 0x8000;

/// DEVICE_INFO RDM protocol version 1.0.
pub(crate) const RDM_PROTOCOL_1_0: u16 = 0x0100;

// ---------------------------------------------------------------------------------------------------------------
// Tube constants (tube/firmware/include/config.h, src/personality.c, boot_rdm/src/main.c)
// ---------------------------------------------------------------------------------------------------------------

/// PRODUCT_CATEGORY_FIXTURE_FIXED.
const PRODUCT_CATEGORY: u16 = 0x0101;
/// Boot-Loader flag in the DISC_MUTE control field (E1.20 7.6.1).
const CF_BOOT_LOADER: u16 = 0x0004;
/// Largest request PD the bootloader receives (8-bit RX index).
const BOOT_RX_MAX_PD: usize = 228;
/// ENTER_BOOTLOADER payload.
const BOOT_REQUEST: &[u8] = b"BOOT";
/// Number of DMX personalities of the app.
const PERSONALITY_COUNT: u8 = 4;
/// RGB segments of the tube.
const SEGMENTS: u16 = 4;
const MANUFACTURER_LABEL: &str = "Prototype";
const MODEL_DESCRIPTION: &str = "XBAR500 LED tube (custom fw)";
/// Slot label IDs (E1.20 Table C-2).
const SD_INTENSITY: u16 = 0x0001;
const SD_COLOR_ADD_RED: u16 = 0x0205;
const SD_COLOR_ADD_GREEN: u16 = 0x0206;
const SD_COLOR_ADD_BLUE: u16 = 0x0207;
/// ST_PRIMARY.
const ST_PRIMARY: u8 = 0x00;

/// PIDs the app lists in SUPPORTED_PARAMETERS (without the E1.20 required ones, 10.4.1).
const APP_SUPPORTED: &[u16] = &[
    pid::DEVICE_MODEL_DESCRIPTION,
    pid::MANUFACTURER_LABEL,
    pid::DEVICE_LABEL,
    pid::DMX_PERSONALITY,
    pid::DMX_PERSONALITY_DESCRIPTION,
    pid::SLOT_INFO,
    pid::SLOT_DESCRIPTION,
    pid::DEFAULT_SLOT_VALUE,
    pids::PID_ENTER_BOOTLOADER,
];

const APP_RULES: &[Rule] = &[
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
    rule(pid::IDENTIFY_DEVICE, cc::GET, Pdl::Exact(0)),
    rule(pid::IDENTIFY_DEVICE, cc::SET, Pdl::Exact(1)),
    rule(pids::PID_ENTER_BOOTLOADER, cc::SET, Pdl::Exact(4)),
];

const BOOT_RULES: &[Rule] = &[
    rule(pid::SUPPORTED_PARAMETERS, cc::GET, Pdl::Exact(0)),
    rule(pid::DEVICE_INFO, cc::GET, Pdl::Exact(0)),
    rule(pid::SOFTWARE_VERSION_LABEL, cc::GET, Pdl::Exact(0)),
    rule(pid::IDENTIFY_DEVICE, cc::GET, Pdl::Exact(0)),
    rule(pid::IDENTIFY_DEVICE, cc::SET, Pdl::Exact(1)),
    rule(pids::PID_BOOT_INFO, cc::GET, Pdl::Exact(0)),
    rule(
        pids::PID_BOOT_WRITE_PAGE,
        cc::SET,
        Pdl::Exact(2 + PAGE as u8),
    ),
    rule(pids::PID_BOOT_APP_CRC, cc::GET, Pdl::Exact(4)),
    rule(pids::PID_BOOT_FINISH, cc::SET, Pdl::Exact(0)),
];

/// DMX footprint of a personality (0 = no such personality).
fn footprint(p: u8) -> u16 {
    match p {
        1 => 3 * SEGMENTS,
        2 => 3,
        3 => 4,
        4 => 3 * SEGMENTS + 1,
        _ => 0,
    }
}

fn personality_description(p: u8) -> String {
    match p {
        1 => format!("{SEGMENTS}x RGB"),
        2 => "RGB".into(),
        3 => "RGB + Dimmer".into(),
        _ => format!("{SEGMENTS}x RGB + Dimmer"),
    }
}

/// Personalities 3/4 carry the master dimmer as the last slot.
fn slot_is_dimmer(p: u8, s: u16) -> bool {
    (p == 3 || p == 4) && s + 1 == footprint(p)
}

fn slot_label(p: u8, s: u16) -> u16 {
    if slot_is_dimmer(p, s) {
        SD_INTENSITY
    } else {
        [SD_COLOR_ADD_RED, SD_COLOR_ADD_GREEN, SD_COLOR_ADD_BLUE][usize::from(s % 3)]
    }
}

fn slot_default(p: u8, s: u16) -> u8 {
    if slot_is_dimmer(p, s) {
        255
    } else {
        0
    }
}

fn slot_description(p: u8, s: u16) -> String {
    if slot_is_dimmer(p, s) {
        return "Dimmer".into();
    }
    let colour = ["Red", "Green", "Blue"][usize::from(s % 3)];
    if p == 1 || p == 4 {
        format!("Seg {} {colour}", s / 3 + 1)
    } else {
        colour.into()
    }
}

fn is_boot_pid(p: u16) -> bool {
    (pids::PID_BOOT_INFO..=pids::PID_BOOT_FINISH).contains(&p)
}

// ---------------------------------------------------------------------------------------------------------------
// SimTube
// ---------------------------------------------------------------------------------------------------------------

/// Which firmware is running.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TubeMode {
    /// The app `tube_fw` (DMX fixture, ENTER_BOOTLOADER).
    App,
    /// The RDM bootloader `tube_boot_rdm` v3 (boot window after power-on, or staying for an update).
    Boot,
}

/// Timing of the simulated tube (defaults = measured on the real tube / emulator).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TubeTiming {
    /// After power-on the bootloader checks the app CRC and answers nothing for this long (26 ms).
    pub window_start: Duration,
    /// End of the rescue window after power-on (225 ms): a bootloader PID before that keeps the bootloader.
    pub window_end: Duration,
    /// The staying bootloader starts a valid app after this long without a bootloader PID (60 s).
    pub boot_timeout: Duration,
    /// Silence after ENTER_BOOTLOADER: EEPROM flush, watchdog reset, app CRC check (40 ms).
    pub enter_boot_delay: Duration,
    /// Silence after the app starts (FINISH, window end, timeout): power-on self-test 12 × 150 ms (1.8 s).
    pub app_start_delay: Duration,
}

impl Default for TubeTiming {
    fn default() -> Self {
        Self {
            window_start: Duration::from_millis(26),
            window_end: Duration::from_millis(225),
            boot_timeout: Duration::from_secs(60),
            enter_boot_delay: Duration::from_millis(40),
            app_start_delay: Duration::from_millis(1800),
        }
    }
}

/// What happens after the response is out.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum After {
    Nothing,
    EnterBoot,
    Finish,
}

/// A simulated LED tube (app + RDM bootloader v3, same UID in both).
///
/// Flash is the 0x1800-byte app area (app + 16-byte trailer). The app starts only if the trailer passes the
/// bootloader's check against the UID's manufacturer and [`SimTube::with_model`]. Settings (start address,
/// personality, label) live in "EEPROM" and survive resets; identify and mute do not.
#[derive(Debug, Clone)]
pub struct SimTube {
    /// How many following requests a page write keeps the bootloader busy (NACK BUFFER_FULL, BOOT_INFO busy).
    pub busy_requests: u8,
    /// Drop every n-th answer (0 = never). The request is still executed (lost on the way back).
    pub lose_every: u32,
    /// Boot window, bootloader timeout and silent phases.
    pub timing: TubeTiming,
    uid: Uid,
    model: u16,
    flash: Vec<u8>,
    mode: TubeMode,
    /// Bootloader stays (requested, rescued or invalid app); false = only the boot window.
    stay: bool,
    window_end: Option<Instant>,
    /// Bootloader idle timeout; `None` while staying = armed at the next call.
    idle_deadline: Option<Instant>,
    silent_until: Option<Instant>,
    busy: u8,
    last_error: u8,
    /// BOOT_INFO bytes 14..18 (`app_st[]`): from the last trailer check, WRITE_PAGE clears `valid`.
    cached_valid: bool,
    cached_version: u32,
    running_version: u32,
    start_address: u16,
    personality: u8,
    label: Vec<u8>,
    identify: bool,
    muted: bool,
    writes: Vec<u16>,
    buffer_full: u32,
    answers: u32,
    /// Constructed with [`SimTube::in_bootloader`].
    force_boot: bool,
}

impl SimTube {
    /// A tube with `uid` whose flash holds `app` (`None` = blank flash: stays in the bootloader). It runs the app
    /// if the trailer is valid for this UID's manufacturer and model [`pids::MODEL_TUBE`], else the bootloader.
    pub fn new(uid: Uid, app: Option<TubeImage>) -> Self {
        let mut flash = vec![0xFFu8; usize::from(BOOT_START)];
        if let Some(img) = app {
            let n = img.img.len().min(flash.len());
            flash[..n].copy_from_slice(&img.img[..n]);
        }
        let mut t = Self {
            busy_requests: 2,
            lose_every: 0,
            timing: TubeTiming::default(),
            uid,
            model: pids::MODEL_TUBE,
            flash,
            mode: TubeMode::Boot,
            stay: true,
            window_end: None,
            idle_deadline: None,
            silent_until: None,
            busy: 0,
            last_error: 0,
            cached_valid: false,
            cached_version: 0,
            running_version: 0,
            start_address: 1,
            personality: 1,
            label: Vec::new(),
            identify: false,
            muted: false,
            writes: Vec::new(),
            buffer_full: 0,
            answers: 0,
            force_boot: false,
        };
        t.cold_start();
        t
    }

    /// Use another DEVICE_MODEL_ID (reported and checked against the trailer); re-evaluates the start.
    pub fn with_model(mut self, model: u16) -> Self {
        self.model = model;
        self.cold_start();
        self
    }

    /// Start in the staying bootloader (as after ENTER_BOOTLOADER).
    pub fn in_bootloader(mut self) -> Self {
        self.force_boot = true;
        self.cold_start();
        self
    }

    /// Set the device label (EEPROM).
    pub fn with_label(mut self, label: &str) -> Self {
        self.label = pd::encode_label(label);
        self
    }

    /// Replace the timing.
    pub fn with_timing(mut self, timing: TubeTiming) -> Self {
        self.timing = timing;
        self
    }

    /// Fill the flash page at `addr` with old content (bytes 0..63), like `FakeTube(garbage_page=...)`: a page the
    /// new image leaves blank must be caught by the CRC compare. Re-evaluates the start like a reset.
    pub fn with_garbage_page(mut self, addr: u16) -> Self {
        let a = usize::from(addr & !(PAGE - 1));
        if let Some(page) = self.flash.get_mut(a..a + usize::from(PAGE)) {
            for (i, b) in page.iter_mut().enumerate() {
                *b = i as u8;
            }
        }
        self.cold_start();
        self
    }

    /// The tube's UID.
    pub fn uid(&self) -> Uid {
        self.uid
    }

    /// Running firmware.
    pub fn mode(&self) -> TubeMode {
        self.mode
    }

    /// App flash 0x0000..0x17FF.
    pub fn flash(&self) -> &[u8] {
        &self.flash
    }

    /// Page addresses written by BOOT_WRITE_PAGE, in order.
    pub fn writes(&self) -> &[u16] {
        &self.writes
    }

    /// How many NACK BUFFER_FULL the bootloader produced.
    pub fn buffer_full_count(&self) -> u32 {
        self.buffer_full
    }

    /// Version of the running app (`None` in the bootloader).
    pub fn app_version(&self) -> Option<u32> {
        (self.mode == TubeMode::App).then_some(self.running_version)
    }

    /// DMX start address.
    pub fn start_address(&self) -> u16 {
        self.start_address
    }

    /// Current personality 1..4.
    pub fn personality(&self) -> u8 {
        self.personality
    }

    /// Device label.
    pub fn label(&self) -> String {
        pd::decode_label(&self.label)
    }

    /// IDENTIFY_DEVICE state.
    pub fn identify(&self) -> bool {
        self.identify
    }

    /// Discovery mute flag.
    pub fn muted(&self) -> bool {
        self.muted
    }

    /// The bootloader's trailer check of the current flash (`boot_err` code; 0 = app valid).
    pub fn check(&self) -> u8 {
        trailer_check(&self.flash, Some(self.uid.manufacturer), Some(self.model))
    }

    /// Bootloader `app_check()`: check + cache BOOT_INFO bytes 14..18.
    fn snapshot(&mut self) -> u8 {
        let e = self.check();
        self.cached_valid = e == 0;
        self.cached_version = Trailer::parse(&self.flash).version;
        e
    }

    /// Reset without timing (constructor/builders): app if valid and not [`SimTube::in_bootloader`], else
    /// staying bootloader.
    fn cold_start(&mut self) {
        self.busy = 0;
        self.last_error = 0;
        self.identify = false;
        self.muted = false;
        self.window_end = None;
        self.idle_deadline = None;
        self.silent_until = None;
        if self.snapshot() == 0 && !self.force_boot {
            self.mode = TubeMode::App;
            self.stay = false;
            self.running_version = self.cached_version;
        } else {
            self.mode = TubeMode::Boot;
            self.stay = true;
        }
    }

    fn silence(&mut self, from: Instant, d: Duration) {
        self.silent_until = (!d.is_zero()).then(|| from + d);
    }

    /// Bootloader `start_app()` (watchdog reset into the app) at `at`.
    fn start_app(&mut self, at: Instant) {
        self.mode = TubeMode::App;
        self.stay = false;
        self.window_end = None;
        self.idle_deadline = None;
        self.busy = 0;
        self.identify = false;
        self.muted = false;
        self.running_version = Trailer::parse(&self.flash).version;
        self.silence(at, self.timing.app_start_delay);
    }

    /// App: ENTER_BOOTLOADER acknowledged -> reset into the staying bootloader.
    fn enter_boot(&mut self, at: Instant) {
        self.mode = TubeMode::Boot;
        self.stay = true;
        self.window_end = None;
        self.idle_deadline = Some(at + self.timing.boot_timeout);
        self.busy = 0;
        self.last_error = 0;
        self.identify = false;
        self.muted = false;
        self.snapshot();
        self.silence(at, self.timing.enter_boot_delay);
    }

    /// Bootloader `stay_idle()`: stay, restart the idle timeout.
    fn stay_idle(&mut self, now: Instant) {
        self.stay = true;
        self.window_end = None;
        self.idle_deadline = Some(now + self.timing.boot_timeout);
    }

    /// Run the timers up to `now`.
    fn advance(&mut self, now: Instant) {
        if self.mode == TubeMode::Boot {
            if !self.stay {
                if let Some(end) = self.window_end.filter(|&e| now >= e) {
                    self.start_app(end);
                }
            } else {
                match self.idle_deadline {
                    None => self.idle_deadline = Some(now + self.timing.boot_timeout),
                    Some(d) if now >= d => {
                        self.busy = 0; // a page write takes ms, the timeout 60 s
                        if self.snapshot() == 0 {
                            self.start_app(d);
                        } else {
                            self.idle_deadline = Some(d + self.timing.boot_timeout);
                        }
                    }
                    Some(_) => {}
                }
            }
        }
        if self.silent_until.is_some_and(|t| now >= t) {
            self.silent_until = None;
        }
    }

    fn respond_app(
        &mut self,
        req: &RdmMessage,
        bcast: bool,
        after: &mut After,
    ) -> Result<Vec<u8>, NackReason> {
        let d = req.parameter_data.as_slice();
        check_rules(APP_RULES, req.command_class, req.parameter_id, d.len())?;
        let fp = footprint(self.personality);
        let out = match (req.command_class, req.parameter_id) {
            (cc::GET, pid::SUPPORTED_PARAMETERS) => pd::encode_pid_list(APP_SUPPORTED),
            (cc::GET, pid::PARAMETER_DESCRIPTION) => {
                if be16(d) != Some(pids::PID_ENTER_BOOTLOADER) {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                pids::PIDS
                    .iter()
                    .find(|p| p.pid == pids::PID_ENTER_BOOTLOADER)
                    .and_then(ParameterDescription::from_pid_def)
                    .ok_or(NackReason::HARDWARE_FAULT)?
                    .encode()
            }
            (cc::GET, pid::DEVICE_INFO) => DeviceInfo {
                rdm_protocol_version: RDM_PROTOCOL_1_0,
                device_model_id: self.model,
                product_category: PRODUCT_CATEGORY,
                software_version_id: self.running_version,
                dmx_footprint: fp,
                current_personality: self.personality,
                personality_count: PERSONALITY_COUNT,
                dmx_start_address: if fp > 0 { self.start_address } else { 0xFFFF },
                sub_device_count: 0,
                sensor_count: 0,
            }
            .encode(),
            (cc::GET, pid::SOFTWARE_VERSION_LABEL) => {
                pd::encode_label(&format!("tube_fw {}", version_str(self.running_version)))
            }
            (cc::GET, pid::DEVICE_MODEL_DESCRIPTION) => pd::encode_label(MODEL_DESCRIPTION),
            (cc::GET, pid::MANUFACTURER_LABEL) => pd::encode_label(MANUFACTURER_LABEL),
            (cc::GET, pid::DEVICE_LABEL) => self.label.clone(),
            (cc::SET, pid::DEVICE_LABEL) => {
                self.label = label_bytes(d);
                Vec::new()
            }
            (cc::GET, pid::DMX_START_ADDRESS) => self.start_address.to_be_bytes().to_vec(),
            (cc::SET, pid::DMX_START_ADDRESS) => {
                let a = be16(d).ok_or(NackReason::FORMAT_ERROR)?;
                if a < 1 || u32::from(a) + u32::from(fp) > 513 {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                self.start_address = a;
                Vec::new()
            }
            (cc::GET, pid::DMX_PERSONALITY) => DmxPersonality {
                current: self.personality,
                count: PERSONALITY_COUNT,
            }
            .encode(),
            (cc::SET, pid::DMX_PERSONALITY) => {
                let p = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                let f = footprint(p);
                if f == 0 || u32::from(self.start_address) + u32::from(f) > 513 {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                self.personality = p;
                Vec::new()
            }
            (cc::GET, pid::DMX_PERSONALITY_DESCRIPTION) => {
                let p = d.first().copied().ok_or(NackReason::FORMAT_ERROR)?;
                if footprint(p) == 0 {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                PersonalityDescription {
                    personality: p,
                    slots_required: footprint(p),
                    description: personality_description(p),
                }
                .encode()
            }
            (cc::GET, pid::SLOT_INFO) => SlotInfo::encode_list(
                &(0..fp)
                    .map(|s| SlotInfo {
                        slot_offset: s,
                        slot_type: ST_PRIMARY,
                        slot_label_id: slot_label(self.personality, s),
                    })
                    .collect::<Vec<_>>(),
            ),
            (cc::GET, pid::SLOT_DESCRIPTION) => {
                let s = be16(d).ok_or(NackReason::FORMAT_ERROR)?;
                if s >= fp {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                SlotDescription {
                    slot: s,
                    description: slot_description(self.personality, s),
                }
                .encode()
            }
            (cc::GET, pid::DEFAULT_SLOT_VALUE) => DefaultSlotValue::encode_list(
                &(0..fp)
                    .map(|s| DefaultSlotValue {
                        slot_offset: s,
                        value: slot_default(self.personality, s),
                    })
                    .collect::<Vec<_>>(),
            ),
            (cc::GET, pid::IDENTIFY_DEVICE) => vec![u8::from(self.identify)],
            (cc::SET, pid::IDENTIFY_DEVICE) => {
                self.identify = set_bool(d)?;
                Vec::new()
            }
            (cc::SET, pids::PID_ENTER_BOOTLOADER) => {
                if d != BOOT_REQUEST {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                if !bcast {
                    // never all tubes at once into the bootloader
                    *after = After::EnterBoot;
                }
                Vec::new()
            }
            _ => return Err(NackReason::UNKNOWN_PID),
        };
        Ok(out)
    }

    fn respond_boot(
        &mut self,
        req: &RdmMessage,
        was_busy: bool,
        after: &mut After,
    ) -> Result<Vec<u8>, NackReason> {
        let d = req.parameter_data.as_slice();
        check_rules(BOOT_RULES, req.command_class, req.parameter_id, d.len())?;
        let out = match (req.command_class, req.parameter_id) {
            (cc::GET, pid::SUPPORTED_PARAMETERS) => Vec::new(),
            (cc::GET, pid::DEVICE_INFO) => DeviceInfo {
                rdm_protocol_version: RDM_PROTOCOL_1_0,
                device_model_id: self.model,
                product_category: PRODUCT_CATEGORY,
                software_version_id: pids::tube::BOOT_RDM_SW_VERSION_ID,
                dmx_footprint: 0,
                current_personality: 0,
                personality_count: 0,
                dmx_start_address: 0xFFFF,
                sub_device_count: 0,
                sensor_count: 0,
            }
            .encode(),
            (cc::GET, pid::SOFTWARE_VERSION_LABEL) => {
                pd::encode_label(&format!("tube_boot_rdm {}", pids::tube::BOOT_RDM_VERSION))
            }
            (cc::GET, pid::IDENTIFY_DEVICE) => vec![u8::from(self.identify)],
            (cc::SET, pid::IDENTIFY_DEVICE) => {
                self.identify = set_bool(d)?;
                Vec::new()
            }
            (cc::GET, pids::PID_BOOT_INFO) => {
                let info = BootInfo {
                    version: pids::tube::BOOT_RDM_VERSION as u8,
                    page_size: PAGE as u8,
                    app_end: BOOT_START,
                    busy: u8::from(was_busy),
                    last_error: self.last_error,
                    manufacturer: self.uid.manufacturer,
                    model: self.model,
                    boot_sw_version: pids::tube::BOOT_RDM_SW_VERSION_ID,
                    app_valid: u8::from(self.cached_valid),
                    app_fw_version: self.cached_version,
                };
                self.last_error = 0; // read clears
                info.encode()
            }
            (cc::SET, pids::PID_BOOT_WRITE_PAGE) => {
                let (Some(a), Some(page)) = (be16(d), d.get(2..)) else {
                    return Err(NackReason::FORMAT_ERROR);
                };
                if a >= BOOT_START || a % PAGE != 0 {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                if was_busy {
                    return Err(NackReason::BUFFER_FULL);
                }
                let start = usize::from(a);
                let dst = self
                    .flash
                    .get_mut(start..start + page.len())
                    .ok_or(NackReason::DATA_OUT_OF_RANGE)?;
                dst.copy_from_slice(page);
                self.writes.push(a);
                self.cached_valid = false; // installed app no longer known valid
                self.busy = self.busy_requests;
                Vec::new()
            }
            (cc::GET, pids::PID_BOOT_APP_CRC) => {
                let (Some(a), Some(n)) = (be16(d), d.get(2..).and_then(be16)) else {
                    return Err(NackReason::FORMAT_ERROR);
                };
                if n > CRC_CHUNK || a > BOOT_START - n {
                    return Err(NackReason::DATA_OUT_OF_RANGE);
                }
                if was_busy {
                    return Err(NackReason::BUFFER_FULL);
                }
                let data = self
                    .flash
                    .get(usize::from(a)..usize::from(a) + usize::from(n))
                    .ok_or(NackReason::DATA_OUT_OF_RANGE)?;
                crc16(data).to_be_bytes().to_vec()
            }
            (cc::SET, pids::PID_BOOT_FINISH) => {
                if was_busy {
                    return Err(NackReason::BUFFER_FULL);
                }
                *after = After::Finish;
                Vec::new()
            }
            _ => return Err(NackReason::UNKNOWN_PID),
        };
        Ok(out)
    }
}

/// IDENTIFY_DEVICE SET value: 0/1, else DATA_OUT_OF_RANGE.
pub(crate) fn set_bool(d: &[u8]) -> Result<bool, NackReason> {
    match d {
        [0] => Ok(false),
        [1] => Ok(true),
        [_] => Err(NackReason::DATA_OUT_OF_RANGE),
        _ => Err(NackReason::FORMAT_ERROR),
    }
}

impl SimResponder for SimTube {
    fn uid(&self) -> Uid {
        self.uid
    }

    fn handle(&mut self, req: &RdmMessage, now: Instant) -> Option<RdmMessage> {
        self.advance(now);
        if self.silent_until.is_some() {
            return None;
        }
        let bcast = broadcast_for(self.uid, req.dest_uid)?;
        let pdl = req.parameter_data.len();
        if self.mode == TubeMode::Boot && pdl > BOOT_RX_MAX_PD {
            return None; // not received (8-bit RX index)
        }
        if bcast && req.dest_uid.is_all_broadcast() && req.parameter_id >= MANUFACTURER_PID_MIN {
            return None; // E1.20 6.2.10.2
        }
        let was_busy = self.busy > 0;
        self.busy = self.busy.saturating_sub(1);
        let mut after = After::Nothing;
        let reply = match req.command_class {
            cc::DISCOVERY => match req.parameter_id {
                pid::DISC_MUTE | pid::DISC_UN_MUTE if pdl == 0 => {
                    self.muted = req.parameter_id == pid::DISC_MUTE;
                    let boot_flag = self.mode == TubeMode::Boot && !self.cached_valid;
                    Ok(DiscMuteResponse {
                        control_field: if boot_flag { CF_BOOT_LOADER } else { 0 },
                        binding_uid: None,
                    }
                    .encode())
                }
                // DISC_UNIQUE_BRANCH is simulated by the node (SimResponder::discoverable)
                _ => return None,
            },
            cc::GET | cc::SET => {
                if bcast && req.command_class == cc::GET {
                    return None; // broadcast GET: invalid, ignored
                }
                if self.mode == TubeMode::Boot && is_boot_pid(req.parameter_id) {
                    self.stay_idle(now);
                }
                if !sub_device_ok(req.command_class, req.sub_device) {
                    Err(NackReason::SUB_DEVICE_OUT_OF_RANGE)
                } else if self.mode == TubeMode::App {
                    self.respond_app(req, bcast, &mut after)
                } else {
                    self.respond_boot(req, was_busy, &mut after)
                }
            }
            _ => return None,
        };
        if reply == Err(NackReason::BUFFER_FULL) {
            self.buffer_full += 1;
        }
        let mut response = (!bcast).then(|| match reply {
            Ok(data) => req.response_to(self.uid, ResponseType::Ack, 0, data),
            Err(r) => req.response_to(
                self.uid,
                ResponseType::NackReason,
                0,
                r.0.to_be_bytes().to_vec(),
            ),
        });
        if response.is_some() {
            self.answers += 1;
            if self.lose_every > 0 && self.answers.is_multiple_of(self.lose_every) {
                response = None;
            }
        }
        match after {
            After::Nothing => {}
            After::EnterBoot => self.enter_boot(now),
            After::Finish => {
                self.last_error = self.snapshot();
                if self.last_error == 0 {
                    self.start_app(now);
                }
            }
        }
        response
    }

    fn tick(&mut self, now: Instant) {
        self.advance(now);
    }

    fn power_cycle(&mut self, now: Instant) {
        self.mode = TubeMode::Boot;
        self.busy = 0;
        self.last_error = 0;
        self.identify = false;
        self.muted = false;
        self.silent_until = Some(now + self.timing.window_start);
        if self.snapshot() == 0 {
            self.stay = false;
            self.window_end = Some(now + self.timing.window_end);
            self.idle_deadline = None;
        } else {
            self.stay = true;
            self.window_end = None;
            self.idle_deadline = Some(now + self.timing.window_start + self.timing.boot_timeout);
        }
    }

    fn discoverable(&self) -> bool {
        self.silent_until.is_none()
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------------------------

#[cfg(test)]
pub(crate) mod test_util {
    //! A minimal hand-driven RDM controller for the responder tests.
    use super::*;

    /// Controller UID used by the tests.
    pub const CTL: Uid = Uid::new(0x7FF0, 0xB066_75AA);

    /// Hand-driven controller: numbers requests, checks every answer, owns the clock.
    pub struct Ctl {
        pub tn: u8,
        pub now: Instant,
    }

    impl Ctl {
        pub fn new() -> Self {
            Self {
                tn: 0,
                now: Instant::now(),
            }
        }

        /// Advance the clock and tick the device.
        pub fn wait(&mut self, dev: &mut dyn SimResponder, d: Duration) {
            self.now += d;
            dev.tick(self.now);
        }

        /// Send one request (sub-device 0) and check the answer's framing.
        pub fn send(
            &mut self,
            dev: &mut dyn SimResponder,
            dest: Uid,
            command_class: u8,
            pid: u16,
            data: &[u8],
        ) -> Option<RdmMessage> {
            self.send_sd(dev, dest, 0, command_class, pid, data)
        }

        /// Send one request to a sub-device and check the answer's framing.
        pub fn send_sd(
            &mut self,
            dev: &mut dyn SimResponder,
            dest: Uid,
            sub_device: u16,
            command_class: u8,
            pid: u16,
            data: &[u8],
        ) -> Option<RdmMessage> {
            self.tn = self.tn.wrapping_add(1);
            let req = RdmMessage::request(
                dest,
                CTL,
                self.tn,
                1,
                sub_device,
                command_class,
                pid,
                data.to_vec(),
            );
            let resp = dev.handle(&req, self.now)?;
            check_framing(&req, &resp, dev.uid());
            Some(resp)
        }
    }

    /// Every answer decodes and mirrors TN, UIDs, sub-device, PID and CC (QUEUED_MESSAGE: PID/CC/sub-device of
    /// the queued response).
    pub fn check_framing(req: &RdmMessage, resp: &RdmMessage, me: Uid) {
        let wire = resp.encode();
        assert_eq!(
            RdmMessage::decode(&wire).ok().as_ref(),
            Some(resp),
            "decode round trip"
        );
        assert_eq!(resp.dest_uid, req.src_uid);
        assert_eq!(resp.src_uid, me);
        assert_eq!(resp.transaction_number, req.transaction_number);
        assert!(resp.response_type().is_some(), "valid response type");
        if req.parameter_id == pid::QUEUED_MESSAGE
            && resp.response_type() == Some(ResponseType::Ack)
        {
            assert!(resp.is_response());
        } else {
            assert_eq!(resp.command_class, req.command_class + 1);
            assert_eq!(resp.parameter_id, req.parameter_id);
            assert_eq!(resp.sub_device, req.sub_device);
        }
    }

    /// NACK reason of an answer (panics if there is none).
    pub fn nack(resp: Option<RdmMessage>) -> NackReason {
        let r = resp.expect("answer");
        r.nack_reason().expect("NACK")
    }

    /// PD of an ACK (panics otherwise).
    pub fn ack(resp: Option<RdmMessage>) -> Vec<u8> {
        let r = resp.expect("answer");
        assert_eq!(
            r.response_type(),
            Some(ResponseType::Ack),
            "ACK expected: {r:?}"
        );
        r.parameter_data
    }
}

#[cfg(test)]
mod tests {
    use super::test_util::*;
    use super::*;
    use crate::image::tube::{boot_err, build_test_image, LAST_PAGE};

    const UID: Uid = Uid::new(0x7FF0, 0x0000_0001);

    fn app_bytes() -> Vec<u8> {
        // flash_rdm.py selftest image: data, an all-0xFF page inside the image, data
        let mut v: Vec<u8> = (0..1001u32).map(|i| (i * 7) as u8).collect();
        v.extend(std::iter::repeat_n(0xFF, 199));
        v.extend(std::iter::repeat_n(0x5A, 101));
        v
    }

    fn new_image(version: u32) -> TubeImage {
        build_test_image(&app_bytes(), 0x7FF0, pids::MODEL_TUBE, version)
    }

    fn old_image() -> TubeImage {
        // ends before 1088, so a garbage page there does not break the old app's CRC
        build_test_image(&[0x11; 1000], 0x7FF0, pids::MODEL_TUBE, 0x301)
    }

    /// Send with the host retry policy: up to 5 tries on no answer, up to 100 on BUFFER_FULL.
    fn call(
        ctl: &mut Ctl,
        t: &mut SimTube,
        command_class: u8,
        p: u16,
        data: &[u8],
    ) -> Option<RdmMessage> {
        let (mut lost, mut full) = (0, 0);
        loop {
            match ctl.send(t, UID, command_class, p, data) {
                None if lost < 5 => {
                    lost += 1;
                    ctl.wait(t, Duration::from_millis(10));
                }
                None => return None,
                Some(r) if r.nack_reason() == Some(NackReason::BUFFER_FULL) && full < 100 => {
                    full += 1;
                    ctl.wait(t, Duration::from_millis(1));
                }
                Some(r) => return Some(r),
            }
        }
    }

    fn boot_info(ctl: &mut Ctl, t: &mut SimTube) -> BootInfo {
        let pd = ack(call(ctl, t, cc::GET, pids::PID_BOOT_INFO, &[]));
        BootInfo::decode(&pd).expect("BOOT_INFO")
    }

    fn device_info(ctl: &mut Ctl, t: &mut SimTube) -> DeviceInfo {
        DeviceInfo::decode(&ack(call(ctl, t, cc::GET, pid::DEVICE_INFO, &[]))).expect("DEVICE_INFO")
    }

    fn write_page(ctl: &mut Ctl, t: &mut SimTube, a: u16, data: &[u8]) {
        let mut pd = a.to_be_bytes().to_vec();
        pd.extend_from_slice(data);
        ack(call(ctl, t, cc::SET, pids::PID_BOOT_WRITE_PAGE, &pd));
    }

    /// The whole update as contract §9.1 / flash_rdm.py `flash` does it.
    fn update(ctl: &mut Ctl, t: &mut SimTube, img: &TubeImage) {
        let di = device_info(ctl, t);
        assert_eq!(di.device_model_id, pids::MODEL_TUBE);
        // a lost ACK: the retry reaches the bootloader, which does not know the PID
        let r = call(ctl, t, cc::SET, pids::PID_ENTER_BOOTLOADER, b"BOOT").expect("answer");
        assert!(
            matches!(r.nack_reason(), None | Some(NackReason::UNKNOWN_PID)),
            "{r:?}"
        );
        assert_eq!(t.mode(), TubeMode::Boot);
        ctl.wait(t, Duration::from_millis(100));
        let bi = boot_info(ctl, t);
        assert_eq!(
            (bi.manufacturer, bi.model, bi.version),
            (0x7FF0, pids::MODEL_TUBE, 3)
        );
        for (a, data) in img.page_plan(true) {
            write_page(ctl, t, a, &data);
        }
        let mut rewritten = 0;
        for (a, n, crc) in img.crc_chunks() {
            let req: Vec<u8> = [a.to_be_bytes(), n.to_be_bytes()].concat();
            let got = be16(&ack(call(ctl, t, cc::GET, pids::PID_BOOT_APP_CRC, &req)));
            if got != Some(crc) {
                let first = a & !(PAGE - 1);
                for p in (first..a + n).step_by(usize::from(PAGE)) {
                    write_page(ctl, t, p, &img.page(p));
                    rewritten += 1;
                }
                let again = be16(&ack(call(ctl, t, cc::GET, pids::PID_BOOT_APP_CRC, &req)));
                assert_eq!(again, Some(crc), "second pass at 0x{a:04X}");
            }
        }
        let _ = rewritten;
        // FINISH: a lost ACK leaves the retry unanswered (the app is in its self-test)
        if let Some(r) = call(ctl, t, cc::SET, pids::PID_BOOT_FINISH, &[]) {
            assert_eq!(r.response_type(), Some(ResponseType::Ack));
        }
        assert_eq!(t.mode(), TubeMode::App);
        assert!(!t.discoverable(), "self-test after FINISH");
        ctl.wait(t, Duration::from_secs(3));
        let di = device_info(ctl, t);
        assert_eq!(di.software_version_id, img.trailer.version);
        let label = ack(call(ctl, t, cc::GET, pid::SOFTWARE_VERSION_LABEL, &[]));
        assert_eq!(
            pd::decode_label(&label),
            format!("tube_fw {}", version_str(img.trailer.version))
        );
    }

    #[test]
    fn full_update_flows() {
        let img = new_image(0x400);
        for (lose, garbage) in [(0, None), (7, None), (3, None), (0, Some(1088u16))] {
            let mut t = SimTube::new(UID, Some(old_image()));
            if let Some(g) = garbage {
                t = t.with_garbage_page(g);
            }
            t.lose_every = lose;
            assert_eq!(t.mode(), TubeMode::App, "old app valid");
            assert_eq!(t.app_version(), Some(0x301));
            let mut ctl = Ctl::new();
            update(&mut ctl, &mut t, &img);
            assert_eq!(t.app_version(), Some(0x400));
            assert_eq!(t.check(), boot_err::NONE);
            assert_eq!(
                &t.flash()[..usize::from(img.length())],
                &img.img[..usize::from(img.length())]
            );
            assert!(t.buffer_full_count() > 0, "BUFFER_FULL seen");
            if garbage.is_some() {
                assert_eq!(
                    t.writes().iter().filter(|&&a| a == 1088).count(),
                    1,
                    "second pass"
                );
            }
        }
    }

    #[test]
    fn busy_requests_and_boot_info_busy_flag() {
        let mut t = SimTube::new(UID, None);
        t.busy_requests = 3;
        let mut ctl = Ctl::new();
        write_page(&mut ctl, &mut t, 0, &[0u8; 64]);
        let full = t.buffer_full_count();
        let mut pd = 64u16.to_be_bytes().to_vec();
        pd.extend_from_slice(&[1u8; 64]);
        assert_eq!(
            nack(ctl.send(&mut t, UID, cc::SET, pids::PID_BOOT_WRITE_PAGE, &pd)),
            NackReason::BUFFER_FULL
        );
        assert_eq!(
            BootInfo::decode(&ack(ctl.send(
                &mut t,
                UID,
                cc::GET,
                pids::PID_BOOT_INFO,
                &[]
            )))
            .ok()
            .map(|b| b.busy),
            Some(1)
        );
        assert_eq!(
            nack(ctl.send(&mut t, UID, cc::SET, pids::PID_BOOT_FINISH, &[])),
            NackReason::BUFFER_FULL
        );
        // three requests saw busy, the fourth goes through; the dropped write did not happen
        assert_eq!(t.flash()[64], 0xFF);
        ack(ctl.send(&mut t, UID, cc::SET, pids::PID_BOOT_WRITE_PAGE, &pd));
        assert_eq!(t.flash()[64], 1);
        assert_eq!(t.buffer_full_count(), full + 2);
        assert_eq!(t.writes(), &[0, 64]);
    }

    #[test]
    fn half_written_image_never_valid() {
        let img = new_image(0x400);
        for skip in [false, true] {
            let mut t = SimTube::new(UID, Some(old_image())).in_bootloader();
            let mut ctl = Ctl::new();
            assert_eq!(boot_info(&mut ctl, &mut t).app_valid, 1);
            let plan = img.page_plan(skip);
            let mut states = vec![t.check()];
            for (a, data) in &plan {
                write_page(&mut ctl, &mut t, *a, data);
                states.push(t.check());
                assert_eq!(
                    boot_info(&mut ctl, &mut t).app_valid,
                    0,
                    "WRITE clears app_valid"
                );
            }
            assert_eq!(states[0], boot_err::NONE);
            assert!(states[1..states.len() - 1]
                .iter()
                .all(|&e| e == boot_err::FORMAT));
            assert_eq!(states[states.len() - 1], boot_err::NONE);
        }
        // FINISH in the middle: stays with FORMAT
        let mut t = SimTube::new(UID, Some(old_image())).in_bootloader();
        let mut ctl = Ctl::new();
        let plan = img.page_plan(true);
        for (a, data) in &plan[..plan.len() - 1] {
            write_page(&mut ctl, &mut t, *a, data);
        }
        ack(call(&mut ctl, &mut t, cc::SET, pids::PID_BOOT_FINISH, &[]));
        assert_eq!(t.mode(), TubeMode::Boot);
        let bi = boot_info(&mut ctl, &mut t);
        assert_eq!((bi.last_error, bi.app_valid), (boot_err::FORMAT, 0));
        assert_eq!(boot_info(&mut ctl, &mut t).last_error, 0, "read clears");
    }

    #[test]
    fn finish_checks_the_trailer() {
        let wrong_model = build_test_image(&app_bytes(), 0x7FF0, 2, 0x400);
        let wrong_manufacturer = build_test_image(&app_bytes(), 0x1234, pids::MODEL_TUBE, 0x400);
        let mut wrong_format = new_image(0x400);
        wrong_format.img[usize::from(crate::image::tube::TRAILER_START) + 2] = 3;
        let mut meta_changed = new_image(0x400);
        meta_changed.img[usize::from(crate::image::tube::TRAILER_START) + 10] = 7;
        for (img, err) in [
            (wrong_model, boot_err::MODEL),
            (wrong_manufacturer, boot_err::MODEL),
            (wrong_format, boot_err::FORMAT),
            (meta_changed, boot_err::APP_INVALID),
        ] {
            let mut t = SimTube::new(UID, None);
            assert_eq!(t.mode(), TubeMode::Boot);
            let mut ctl = Ctl::new();
            for (a, data) in img.page_plan(true) {
                write_page(&mut ctl, &mut t, a, &data);
            }
            ack(call(&mut ctl, &mut t, cc::SET, pids::PID_BOOT_FINISH, &[]));
            let bi = boot_info(&mut ctl, &mut t);
            assert_eq!(
                (t.mode(), bi.last_error, bi.app_valid),
                (TubeMode::Boot, err, 0)
            );
        }
        // a model-2 tube accepts the model-2 image
        let img = build_test_image(&app_bytes(), 0x7FF0, 2, 0x400);
        let mut t = SimTube::new(UID, None).with_model(2);
        let mut ctl = Ctl::new();
        assert_eq!(boot_info(&mut ctl, &mut t).model, 2);
        for (a, data) in img.page_plan(true) {
            write_page(&mut ctl, &mut t, a, &data);
        }
        ack(call(&mut ctl, &mut t, cc::SET, pids::PID_BOOT_FINISH, &[]));
        assert_eq!(t.app_version(), Some(0x400));
    }

    #[test]
    fn rescue_window_after_power_cycle() {
        let mut ctl = Ctl::new();
        // BOOT_INFO inside the window keeps the bootloader
        let mut t = SimTube::new(UID, Some(old_image()));
        t.power_cycle(ctl.now);
        assert!(!t.discoverable());
        ctl.wait(&mut t, Duration::from_millis(10));
        assert!(
            ctl.send(&mut t, UID, cc::GET, pids::PID_BOOT_INFO, &[])
                .is_none(),
            "silent < 26 ms"
        );
        ctl.wait(&mut t, Duration::from_millis(20));
        assert!(t.discoverable());
        let bi = BootInfo::decode(&ack(ctl.send(
            &mut t,
            UID,
            cc::GET,
            pids::PID_BOOT_INFO,
            &[],
        )))
        .expect("bi");
        assert_eq!((bi.app_valid, bi.app_fw_version), (1, 0x301));
        ctl.wait(&mut t, Duration::from_millis(500));
        assert_eq!(t.mode(), TubeMode::Boot);
        ctl.wait(&mut t, Duration::from_secs(61));
        assert_eq!(t.mode(), TubeMode::App, "60 s timeout after the rescue");

        // other requests inside the window are answered (bootloader DEVICE_INFO) but do not keep it
        let mut t = SimTube::new(UID, Some(old_image()));
        t.power_cycle(ctl.now);
        ctl.wait(&mut t, Duration::from_millis(100));
        let di = device_info(&mut ctl, &mut t);
        assert_eq!(
            (di.software_version_id, di.dmx_start_address),
            (pids::tube::BOOT_RDM_SW_VERSION_ID, 0xFFFF)
        );
        ctl.wait(&mut t, Duration::from_millis(130));
        assert_eq!(t.mode(), TubeMode::App);
        assert!(!t.discoverable(), "self-test");
        ctl.wait(&mut t, Duration::from_secs(2));
        assert_eq!(device_info(&mut ctl, &mut t).software_version_id, 0x301);

        // BOOT_INFO too late
        let mut t = SimTube::new(UID, Some(old_image()));
        t.power_cycle(ctl.now);
        ctl.wait(&mut t, Duration::from_millis(230));
        assert_eq!(t.mode(), TubeMode::App);
        assert!(ctl
            .send(&mut t, UID, cc::GET, pids::PID_BOOT_INFO, &[])
            .is_none());

        // configurable window
        let mut t = SimTube::new(UID, Some(old_image()));
        t.timing.window_end = Duration::from_millis(400);
        t.power_cycle(ctl.now);
        ctl.wait(&mut t, Duration::from_millis(300));
        ack(ctl.send(&mut t, UID, cc::GET, pids::PID_BOOT_INFO, &[]));
        ctl.wait(&mut t, Duration::from_millis(200));
        assert_eq!(t.mode(), TubeMode::Boot);

        // blank flash: stays after the window
        let mut t = SimTube::new(UID, None);
        t.power_cycle(ctl.now);
        ctl.wait(&mut t, Duration::from_secs(120));
        assert_eq!(t.mode(), TubeMode::Boot);
    }

    #[test]
    fn bootloader_timeout() {
        let mut ctl = Ctl::new();
        let mut t = SimTube::new(UID, Some(old_image())).in_bootloader();
        ctl.wait(&mut t, Duration::ZERO); // arms the timeout
        ctl.wait(&mut t, Duration::from_secs(59));
        ack(ctl.send(&mut t, UID, cc::GET, pids::PID_BOOT_INFO, &[]));
        ctl.wait(&mut t, Duration::from_secs(59));
        assert_eq!(t.mode(), TubeMode::Boot, "BOOT_INFO restarted the timeout");
        // non-bootloader PIDs do not restart it
        ack(ctl.send(&mut t, UID, cc::GET, pid::DEVICE_INFO, &[]));
        ctl.wait(&mut t, Duration::from_secs(2));
        assert_eq!(t.mode(), TubeMode::App);
        assert_eq!(t.app_version(), Some(0x301));

        // a page write invalidates the app: no timeout start
        let mut t = SimTube::new(UID, Some(old_image())).in_bootloader();
        let mut ctl = Ctl::new();
        write_page(&mut ctl, &mut t, LAST_PAGE, &[0xFF; 64]);
        ctl.wait(&mut t, Duration::from_secs(200));
        assert_eq!(t.mode(), TubeMode::Boot);
    }

    #[test]
    fn app_nack_order_and_values() {
        let mut t = SimTube::new(UID, Some(old_image()));
        let mut c = Ctl::new();
        let n = |c: &mut Ctl, t: &mut SimTube, sd: u16, k: u8, p: u16, d: &[u8]| {
            nack(c.send_sd(t, UID, sd, k, p, d))
        };
        assert_eq!(
            n(&mut c, &mut t, 1, cc::GET, 0x1234, &[]),
            NackReason::SUB_DEVICE_OUT_OF_RANGE
        );
        assert_eq!(
            n(&mut c, &mut t, 0xFFFF, cc::GET, pid::DEVICE_INFO, &[]),
            NackReason::SUB_DEVICE_OUT_OF_RANGE
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::GET, 0x1234, &[1]),
            NackReason::UNKNOWN_PID
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::GET, pids::PID_BOOT_INFO, &[]),
            NackReason::UNKNOWN_PID
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::SET, pid::DEVICE_INFO, &[1]),
            NackReason::UNSUPPORTED_COMMAND_CLASS
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::GET, pids::PID_ENTER_BOOTLOADER, &[]),
            NackReason::UNSUPPORTED_COMMAND_CLASS
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::GET, pid::DEVICE_INFO, &[0]),
            NackReason::FORMAT_ERROR
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::SET, pid::DEVICE_LABEL, &[b'x'; 33]),
            NackReason::FORMAT_ERROR
        );
        assert_eq!(
            n(
                &mut c,
                &mut t,
                0,
                cc::SET,
                pid::DMX_START_ADDRESS,
                &502u16.to_be_bytes()
            ),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(
                &mut c,
                &mut t,
                0,
                cc::SET,
                pid::DMX_START_ADDRESS,
                &0u16.to_be_bytes()
            ),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::SET, pid::DMX_PERSONALITY, &[5]),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(&mut c, &mut t, 0, cc::SET, pid::IDENTIFY_DEVICE, &[2]),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(
                &mut c,
                &mut t,
                0,
                cc::SET,
                pids::PID_ENTER_BOOTLOADER,
                b"BOOX"
            ),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(
                &mut c,
                &mut t,
                0,
                cc::GET,
                pid::PARAMETER_DESCRIPTION,
                &pid::DEVICE_INFO.to_be_bytes()
            ),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(
                &mut c,
                &mut t,
                0,
                cc::GET,
                pid::SLOT_DESCRIPTION,
                &12u16.to_be_bytes()
            ),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(t.mode(), TubeMode::App);
        // SET to ALL_SUB_DEVICES acts on root, answered with 0xFFFF
        let r = c
            .send_sd(&mut t, UID, 0xFFFF, cc::SET, pid::IDENTIFY_DEVICE, &[1])
            .expect("answer");
        assert_eq!(
            (r.response_type(), r.sub_device, r.message_count),
            (Some(ResponseType::Ack), 0xFFFF, 0)
        );
        assert!(t.identify());
        // the app accepts PDL 231 (FORMAT_ERROR, not silence)
        assert_eq!(
            n(&mut c, &mut t, 0, cc::GET, pid::DEVICE_INFO, &[0; 231]),
            NackReason::FORMAT_ERROR
        );
    }

    #[test]
    fn app_parameters() {
        let mut t = SimTube::new(UID, Some(old_image())).with_label("Bar 1");
        let mut c = Ctl::new();
        let get =
            |c: &mut Ctl, t: &mut SimTube, p: u16, d: &[u8]| ack(c.send(t, UID, cc::GET, p, d));
        let di = DeviceInfo::decode(&get(&mut c, &mut t, pid::DEVICE_INFO, &[])).expect("di");
        assert_eq!(
            (
                di.device_model_id,
                di.product_category,
                di.software_version_id,
                di.dmx_footprint,
                di.current_personality,
                di.personality_count,
                di.dmx_start_address,
                di.sub_device_count
            ),
            (pids::MODEL_TUBE, 0x0101, 0x301, 12, 1, 4, 1, 0)
        );
        assert_eq!(
            pd::decode_label(&get(&mut c, &mut t, pid::SOFTWARE_VERSION_LABEL, &[])),
            "tube_fw 0.3.1"
        );
        assert_eq!(
            pd::decode_label(&get(&mut c, &mut t, pid::DEVICE_LABEL, &[])),
            "Bar 1"
        );
        assert_eq!(
            pd::decode_label(&get(&mut c, &mut t, pid::MANUFACTURER_LABEL, &[])),
            "Prototype"
        );
        let sp = pd::decode_pid_list(&get(&mut c, &mut t, pid::SUPPORTED_PARAMETERS, &[]))
            .expect("list");
        assert!(
            sp.contains(&pids::PID_ENTER_BOOTLOADER)
                && !sp.contains(&pid::DEVICE_INFO)
                && !sp.contains(&pid::DMX_START_ADDRESS)
        );
        let pdesc = ParameterDescription::decode(&get(
            &mut c,
            &mut t,
            pid::PARAMETER_DESCRIPTION,
            &pids::PID_ENTER_BOOTLOADER.to_be_bytes(),
        ))
        .expect("pdesc");
        assert_eq!(
            (
                pdesc.pid,
                pdesc.pdl_size,
                pdesc.command_class,
                pdesc.param_type
            ),
            (0x8000, 4, 0x02, 0)
        );
        // personality 4: 13 slots, dimmer last with default 255
        ack(c.send(&mut t, UID, cc::SET, pid::DMX_PERSONALITY, &[4]));
        let slots =
            SlotInfo::decode_list(&get(&mut c, &mut t, pid::SLOT_INFO, &[])).expect("slots");
        assert_eq!(slots.len(), 13);
        assert_eq!(
            (slots[12].slot_label_id, slots[4].slot_label_id),
            (SD_INTENSITY, SD_COLOR_ADD_GREEN)
        );
        let defaults =
            DefaultSlotValue::decode_list(&get(&mut c, &mut t, pid::DEFAULT_SLOT_VALUE, &[]))
                .expect("defaults");
        assert_eq!((defaults[0].value, defaults[12].value), (0, 255));
        let sd = SlotDescription::decode(&get(
            &mut c,
            &mut t,
            pid::SLOT_DESCRIPTION,
            &4u16.to_be_bytes(),
        ))
        .expect("sd");
        assert_eq!(sd.description, "Seg 2 Green");
        let pdsc = PersonalityDescription::decode(&get(
            &mut c,
            &mut t,
            pid::DMX_PERSONALITY_DESCRIPTION,
            &[3],
        ))
        .expect("pd");
        assert_eq!(
            (pdsc.slots_required, pdsc.description.as_str()),
            (4, "RGB + Dimmer")
        );
        // start address limit depends on the footprint (13): 500 ok, 501 not
        ack(c.send(
            &mut t,
            UID,
            cc::SET,
            pid::DMX_START_ADDRESS,
            &500u16.to_be_bytes(),
        ));
        assert_eq!(
            nack(c.send(
                &mut t,
                UID,
                cc::SET,
                pid::DMX_START_ADDRESS,
                &501u16.to_be_bytes()
            )),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            nack(c.send(&mut t, UID, cc::SET, pid::DMX_PERSONALITY, &[5])),
            NackReason::DATA_OUT_OF_RANGE
        );
        ack(c.send(&mut t, UID, cc::SET, pid::DMX_PERSONALITY, &[2]));
        assert_eq!(t.start_address(), 500);
        // label: NUL ends the text
        ack(c.send(&mut t, UID, cc::SET, pid::DEVICE_LABEL, b"Left\0junk"));
        assert_eq!(t.label(), "Left");
        // settings survive the bootloader
        ack(c.send(&mut t, UID, cc::SET, pids::PID_ENTER_BOOTLOADER, b"BOOT"));
        assert!(
            c.send(&mut t, UID, cc::GET, pid::DEVICE_INFO, &[])
                .is_none(),
            "resetting"
        );
        c.wait(&mut t, Duration::from_millis(50));
        assert_eq!(
            pd::decode_label(&get(&mut c, &mut t, pid::SOFTWARE_VERSION_LABEL, &[])),
            "tube_boot_rdm 3"
        );
        assert_eq!(
            get(&mut c, &mut t, pid::SUPPORTED_PARAMETERS, &[]),
            Vec::<u8>::new()
        );
        assert_eq!(t.label(), "Left");
    }

    #[test]
    fn bootloader_nacks_and_disc_mute() {
        let mut t = SimTube::new(UID, Some(old_image())).in_bootloader();
        let mut c = Ctl::new();
        let n =
            |c: &mut Ctl, t: &mut SimTube, k: u8, p: u16, d: &[u8]| nack(c.send(t, UID, k, p, d));
        assert_eq!(
            nack(c.send_sd(&mut t, UID, 2, cc::GET, pids::PID_BOOT_INFO, &[])),
            NackReason::SUB_DEVICE_OUT_OF_RANGE
        );
        assert_eq!(
            n(&mut c, &mut t, cc::GET, pid::DMX_START_ADDRESS, &[]),
            NackReason::UNKNOWN_PID
        );
        assert_eq!(
            n(&mut c, &mut t, cc::SET, pids::PID_ENTER_BOOTLOADER, b"BOOT"),
            NackReason::UNKNOWN_PID
        );
        assert_eq!(
            n(&mut c, &mut t, cc::GET, pids::PID_BOOT_FINISH, &[]),
            NackReason::UNSUPPORTED_COMMAND_CLASS
        );
        assert_eq!(
            n(&mut c, &mut t, cc::SET, pids::PID_BOOT_WRITE_PAGE, &[0; 65]),
            NackReason::FORMAT_ERROR
        );
        let mut pd = 0x0020u16.to_be_bytes().to_vec();
        pd.extend_from_slice(&[0; 64]);
        assert_eq!(
            n(&mut c, &mut t, cc::SET, pids::PID_BOOT_WRITE_PAGE, &pd),
            NackReason::DATA_OUT_OF_RANGE
        );
        pd[..2].copy_from_slice(&BOOT_START.to_be_bytes());
        assert_eq!(
            n(&mut c, &mut t, cc::SET, pids::PID_BOOT_WRITE_PAGE, &pd),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(
                &mut c,
                &mut t,
                cc::GET,
                pids::PID_BOOT_APP_CRC,
                &[0, 0, 1, 1]
            ),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(
                &mut c,
                &mut t,
                cc::GET,
                pids::PID_BOOT_APP_CRC,
                &[0x17, 0xF1, 0, 16]
            ),
            NackReason::DATA_OUT_OF_RANGE
        );
        assert_eq!(
            n(&mut c, &mut t, cc::SET, pid::IDENTIFY_DEVICE, &[2]),
            NackReason::DATA_OUT_OF_RANGE
        );
        let crc = ack(c.send(
            &mut t,
            UID,
            cc::GET,
            pids::PID_BOOT_APP_CRC,
            &[0x17, 0xF0, 0, 16],
        ));
        assert_eq!(be16(&crc), Some(crc16(&old_image().img[0x17F0..0x1800])));
        // PDL 229..231 is not received by the bootloader
        assert!(c
            .send(&mut t, UID, cc::GET, pid::DEVICE_INFO, &[0; 229])
            .is_none());
        // DISC_MUTE: Boot-Loader flag only without a valid app
        let mute = |c: &mut Ctl, t: &mut SimTube| {
            let r = c
                .send(t, UID, cc::DISCOVERY, pid::DISC_MUTE, &[])
                .expect("mute");
            DiscMuteResponse::decode(&r.parameter_data)
                .expect("cf")
                .control_field
        };
        assert_eq!(mute(&mut c, &mut t), 0);
        assert!(t.muted());
        write_page(&mut c, &mut t, LAST_PAGE, &[0xFF; 64]);
        assert_eq!(mute(&mut c, &mut t), CF_BOOT_LOADER);
        assert!(c
            .send(
                &mut t,
                UID,
                cc::DISCOVERY,
                pid::DISC_UNIQUE_BRANCH,
                &[0; 12]
            )
            .is_none());
    }

    #[test]
    fn broadcasts_are_executed_never_answered() {
        let mut t = SimTube::new(UID, Some(old_image()));
        let mut c = Ctl::new();
        let vendorcast = Uid::new(0x7FF0, 0xFFFF_FFFF);
        assert!(c
            .send(&mut t, Uid::BROADCAST, cc::SET, pid::IDENTIFY_DEVICE, &[1])
            .is_none());
        assert!(t.identify());
        assert!(c
            .send(&mut t, vendorcast, cc::SET, pid::DEVICE_LABEL, b"vc")
            .is_none());
        assert_eq!(t.label(), "vc");
        assert!(c
            .send(
                &mut t,
                Uid::new(0x1234, 0xFFFF_FFFF),
                cc::SET,
                pid::DEVICE_LABEL,
                b"other"
            )
            .is_none());
        assert!(c
            .send(
                &mut t,
                Uid::new(0x7FF0, 2),
                cc::SET,
                pid::DEVICE_LABEL,
                b"other"
            )
            .is_none());
        assert_eq!(t.label(), "vc");
        assert!(c
            .send(&mut t, Uid::BROADCAST, cc::GET, pid::DEVICE_INFO, &[])
            .is_none());
        assert!(c
            .send(
                &mut t,
                vendorcast,
                cc::SET,
                pids::PID_ENTER_BOOTLOADER,
                b"BOOT"
            )
            .is_none());
        assert!(c
            .send(
                &mut t,
                Uid::BROADCAST,
                cc::SET,
                pids::PID_ENTER_BOOTLOADER,
                b"BOOT"
            )
            .is_none());
        assert_eq!(
            t.mode(),
            TubeMode::App,
            "ENTER_BOOTLOADER not executed via broadcast"
        );
        assert!(c
            .send(&mut t, Uid::BROADCAST, cc::DISCOVERY, pid::DISC_MUTE, &[])
            .is_none());
        assert!(t.muted());
        // bootloader: vendorcast FINISH is executed, manufacturer PIDs to FFFF:FFFFFFFF ignored
        let mut t = SimTube::new(UID, Some(old_image())).in_bootloader();
        assert!(c
            .send(&mut t, Uid::BROADCAST, cc::SET, pids::PID_BOOT_FINISH, &[])
            .is_none());
        assert_eq!(t.mode(), TubeMode::Boot);
        assert!(c
            .send(&mut t, vendorcast, cc::SET, pids::PID_BOOT_FINISH, &[])
            .is_none());
        assert_eq!(t.mode(), TubeMode::App);
    }

    #[test]
    fn lose_every_drops_answers_but_executes() {
        let mut t = SimTube::new(UID, Some(old_image()));
        t.lose_every = 2;
        let mut c = Ctl::new();
        assert!(c
            .send(&mut t, UID, cc::SET, pid::IDENTIFY_DEVICE, &[1])
            .is_some());
        assert!(c
            .send(&mut t, UID, cc::SET, pid::DMX_START_ADDRESS, &[0, 7])
            .is_none());
        assert_eq!(t.start_address(), 7);
        assert!(c
            .send(&mut t, UID, cc::GET, pid::DEVICE_INFO, &[])
            .is_some());
    }
}
