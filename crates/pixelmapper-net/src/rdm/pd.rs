//! Parameter-data (PD) codecs for standard PIDs (`protocol/rdm-digest.md` §3.2, §4.3, §6) and the project PIDs
//! of `protocol/pids.toml`. All multi-byte values are big-endian (E1.20).
//!
//! Every type has `decode(&[u8]) -> Result<Self, RdmError>` (wrong length or content →
//! [`RdmError::BadParameterData`]) and `encode(&self) -> Vec<u8>`. Decoders never panic on any input.

use super::{pid, RdmError, Uid};
use crate::pids;
use serde::Serialize;
use std::net::Ipv4Addr;

/// Maximum length of an RDM text field (labels, descriptions).
pub const MAX_LABEL: usize = 32;

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

/// Big-endian reader over a PD slice. Every read fails with `BadParameterData(what)` when bytes are missing.
struct Rd<'a> {
    b: &'a [u8],
    what: &'static str,
}

impl<'a> Rd<'a> {
    fn new(b: &'a [u8], what: &'static str) -> Self {
        Self { b, what }
    }
    /// Reader that requires exactly `n` bytes.
    fn exact(b: &'a [u8], n: usize, what: &'static str) -> Result<Self, RdmError> {
        if b.len() == n {
            Ok(Self::new(b, what))
        } else {
            Err(RdmError::BadParameterData(what))
        }
    }
    fn take<const N: usize>(&mut self) -> Result<[u8; N], RdmError> {
        if self.b.len() < N {
            return Err(RdmError::BadParameterData(self.what));
        }
        let (h, t) = self.b.split_at(N);
        self.b = t;
        let mut a = [0u8; N];
        a.copy_from_slice(h);
        Ok(a)
    }
    fn u8(&mut self) -> Result<u8, RdmError> {
        Ok(self.take::<1>()?[0])
    }
    fn u16(&mut self) -> Result<u16, RdmError> {
        Ok(u16::from_be_bytes(self.take()?))
    }
    fn i16(&mut self) -> Result<i16, RdmError> {
        Ok(i16::from_be_bytes(self.take()?))
    }
    fn u32(&mut self) -> Result<u32, RdmError> {
        Ok(u32::from_be_bytes(self.take()?))
    }
    fn uid(&mut self) -> Result<Uid, RdmError> {
        Ok(Uid::from_bytes(self.take()?))
    }
    fn ipv4(&mut self) -> Result<Ipv4Addr, RdmError> {
        Ok(Ipv4Addr::from(self.take::<4>()?))
    }
    /// Remaining bytes as a label of at most [`MAX_LABEL`] bytes.
    fn label(self) -> Result<String, RdmError> {
        if self.b.len() > MAX_LABEL {
            return Err(RdmError::BadParameterData(self.what));
        }
        Ok(decode_label(self.b))
    }
    fn rest(self) -> &'a [u8] {
        self.b
    }
}

/// Decode a list of fixed-size records; the PD length must be a multiple of `size`.
fn decode_records<T>(
    pd: &[u8],
    size: usize,
    what: &'static str,
    f: impl Fn(&mut Rd<'_>) -> Result<T, RdmError>,
) -> Result<Vec<T>, RdmError> {
    if !pd.len().is_multiple_of(size) {
        return Err(RdmError::BadParameterData(what));
    }
    pd.chunks_exact(size)
        .map(|c| f(&mut Rd::new(c, what)))
        .collect()
}

/// Decode an RDM text field: ASCII up to the first NUL, at most [`MAX_LABEL`] bytes, invalid bytes replaced
/// (lossy UTF-8).
pub fn decode_label(pd: &[u8]) -> String {
    let b = pd.get(..MAX_LABEL).unwrap_or(pd);
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(b.get(..end).unwrap_or(b)).into_owned()
}

/// Encode an RDM text field: non-ASCII characters and NUL become `?`, truncated to [`MAX_LABEL`] bytes, no NUL
/// terminator.
pub fn encode_label(s: &str) -> Vec<u8> {
    s.chars()
        .map(|c| {
            if c.is_ascii() && c != '\0' {
                c as u8
            } else {
                b'?'
            }
        })
        .take(MAX_LABEL)
        .collect()
}

/// Decode a list of PIDs (SUPPORTED_PARAMETERS): n × u16.
pub fn decode_pid_list(pd: &[u8]) -> Result<Vec<u16>, RdmError> {
    decode_records(pd, 2, "PID list: odd length", |r| r.u16())
}

/// Encode a list of PIDs (SUPPORTED_PARAMETERS).
pub fn encode_pid_list(pids: &[u16]) -> Vec<u8> {
    pids.iter().flat_map(|p| p.to_be_bytes()).collect()
}

/// Decode a PD consisting of exactly one u8 (e.g. a status type or sensor number).
pub fn decode_u8(pd: &[u8]) -> Result<u8, RdmError> {
    Rd::exact(pd, 1, "expected 1 byte")?.u8()
}

/// Decode a PD consisting of exactly one u16 (e.g. the PID of a PARAMETER_DESCRIPTION request).
pub fn decode_u16(pd: &[u8]) -> Result<u16, RdmError> {
    Rd::exact(pd, 2, "expected 2 bytes")?.u16()
}

/// Decode a PD consisting of exactly one u32.
pub fn decode_u32(pd: &[u8]) -> Result<u32, RdmError> {
    Rd::exact(pd, 4, "expected 4 bytes")?.u32()
}

// ---------------------------------------------------------------------------------------------------------------
// Discovery (digest §3)
// ---------------------------------------------------------------------------------------------------------------

/// DISC_UNIQUE_BRANCH request PD: lower and upper bound UID (12 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiscUniqueBranch {
    /// Lower bound (inclusive).
    pub lower_bound_uid: Uid,
    /// Upper bound (inclusive).
    pub upper_bound_uid: Uid,
}

impl DiscUniqueBranch {
    /// PID of this PD.
    pub const PID: u16 = pid::DISC_UNIQUE_BRANCH;
    /// Decode 12 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 12, "DISC_UNIQUE_BRANCH: expected 12 bytes")?;
        Ok(Self {
            lower_bound_uid: r.uid()?,
            upper_bound_uid: r.uid()?,
        })
    }
    /// Encode 12 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.lower_bound_uid.to_bytes().to_vec();
        v.extend_from_slice(&self.upper_bound_uid.to_bytes());
        v
    }
    /// Whether `uid` lies in the range (unsigned 48-bit compare; an inverted range contains nothing).
    pub fn contains(&self, uid: Uid) -> bool {
        self.lower_bound_uid.as_u64() <= uid.as_u64()
            && uid.as_u64() <= self.upper_bound_uid.as_u64()
    }
}

/// DISC_MUTE / DISC_UN_MUTE response PD: control field, optional binding UID (PDL 2 or 8).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiscMuteResponse {
    /// Control field bits, see the `*_FLAG` constants.
    pub control_field: u16,
    /// Binding UID of the primary port (devices with several UIDs).
    pub binding_uid: Option<Uid>,
}

impl DiscMuteResponse {
    /// Managed Proxy flag.
    pub const MANAGED_PROXY_FLAG: u16 = 0x0001;
    /// Sub-Device flag (device has sub-devices).
    pub const SUB_DEVICE_FLAG: u16 = 0x0002;
    /// Boot-Loader flag (device cannot operate without a firmware upload).
    pub const BOOT_LOADER_FLAG: u16 = 0x0004;
    /// Proxied Device flag.
    pub const PROXIED_DEVICE_FLAG: u16 = 0x0008;

    /// Decode 2 or 8 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        const WHAT: &str = "DISC_MUTE: expected 2 or 8 bytes";
        if pd.len() != 2 && pd.len() != 8 {
            return Err(RdmError::BadParameterData(WHAT));
        }
        let mut r = Rd::new(pd, WHAT);
        let control_field = r.u16()?;
        let binding_uid = if pd.len() == 8 { Some(r.uid()?) } else { None };
        Ok(Self {
            control_field,
            binding_uid,
        })
    }
    /// Encode 2 or 8 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.control_field.to_be_bytes().to_vec();
        if let Some(u) = self.binding_uid {
            v.extend_from_slice(&u.to_bytes());
        }
        v
    }
    /// Managed Proxy flag set.
    pub fn managed_proxy(&self) -> bool {
        self.control_field & Self::MANAGED_PROXY_FLAG != 0
    }
    /// Sub-Device flag set.
    pub fn sub_device(&self) -> bool {
        self.control_field & Self::SUB_DEVICE_FLAG != 0
    }
    /// Boot-Loader flag set.
    pub fn boot_loader(&self) -> bool {
        self.control_field & Self::BOOT_LOADER_FLAG != 0
    }
    /// Proxied Device flag set.
    pub fn proxied_device(&self) -> bool {
        self.control_field & Self::PROXIED_DEVICE_FLAG != 0
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Status (digest §4.3)
// ---------------------------------------------------------------------------------------------------------------

/// One STATUS_MESSAGES entry (9 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StatusMessage {
    /// Sub-device that generated the message.
    pub sub_device: u16,
    /// Status type, see [`super::status_type`].
    pub status_type: u8,
    /// Status message ID (standard or manufacturer, e.g. [`crate::pids::status`]).
    pub status_message_id: u16,
    /// Data value 1.
    pub data_value1: i16,
    /// Data value 2.
    pub data_value2: i16,
}

impl StatusMessage {
    /// Size of one entry.
    pub const LEN: usize = 9;
    const WHAT: &'static str = "STATUS_MESSAGES: length not a multiple of 9";

    fn read(r: &mut Rd<'_>) -> Result<Self, RdmError> {
        Ok(Self {
            sub_device: r.u16()?,
            status_type: r.u8()?,
            status_message_id: r.u16()?,
            data_value1: r.i16()?,
            data_value2: r.i16()?,
        })
    }
    /// Decode exactly one entry (9 bytes).
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        Self::read(&mut Rd::exact(pd, Self::LEN, Self::WHAT)?)
    }
    /// Encode one entry.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(Self::LEN);
        v.extend_from_slice(&self.sub_device.to_be_bytes());
        v.push(self.status_type);
        v.extend_from_slice(&self.status_message_id.to_be_bytes());
        v.extend_from_slice(&self.data_value1.to_be_bytes());
        v.extend_from_slice(&self.data_value2.to_be_bytes());
        v
    }
    /// Decode a STATUS_MESSAGES response PD (n × 9 bytes, empty = no messages).
    pub fn decode_list(pd: &[u8]) -> Result<Vec<Self>, RdmError> {
        decode_records(pd, Self::LEN, Self::WHAT, Self::read)
    }
    /// Encode a STATUS_MESSAGES response PD (caller keeps n ≤ 25 to fit PDL 231).
    pub fn encode_list(list: &[Self]) -> Vec<u8> {
        list.iter().flat_map(Self::encode).collect()
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Product information (digest §6.2)
// ---------------------------------------------------------------------------------------------------------------

/// DEVICE_INFO response (19 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceInfo {
    /// RDM protocol version (0x0100).
    pub rdm_protocol_version: u16,
    /// Device model ID.
    pub device_model_id: u16,
    /// Product category.
    pub product_category: u16,
    /// Software version ID.
    pub software_version_id: u32,
    /// DMX512 footprint.
    pub dmx_footprint: u16,
    /// Current personality.
    pub current_personality: u8,
    /// Personality count.
    pub personality_count: u8,
    /// DMX512 start address (0xFFFF for footprint 0).
    pub dmx_start_address: u16,
    /// Sub-device count.
    pub sub_device_count: u16,
    /// Sensor count.
    pub sensor_count: u8,
}

impl DeviceInfo {
    /// PID of this PD.
    pub const PID: u16 = pid::DEVICE_INFO;
    /// PD length.
    pub const LEN: usize = 19;
    /// Decode 19 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, Self::LEN, "DEVICE_INFO: expected 19 bytes")?;
        Ok(Self {
            rdm_protocol_version: r.u16()?,
            device_model_id: r.u16()?,
            product_category: r.u16()?,
            software_version_id: r.u32()?,
            dmx_footprint: r.u16()?,
            current_personality: r.u8()?,
            personality_count: r.u8()?,
            dmx_start_address: r.u16()?,
            sub_device_count: r.u16()?,
            sensor_count: r.u8()?,
        })
    }
    /// Encode 19 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(Self::LEN);
        v.extend_from_slice(&self.rdm_protocol_version.to_be_bytes());
        v.extend_from_slice(&self.device_model_id.to_be_bytes());
        v.extend_from_slice(&self.product_category.to_be_bytes());
        v.extend_from_slice(&self.software_version_id.to_be_bytes());
        v.extend_from_slice(&self.dmx_footprint.to_be_bytes());
        v.push(self.current_personality);
        v.push(self.personality_count);
        v.extend_from_slice(&self.dmx_start_address.to_be_bytes());
        v.extend_from_slice(&self.sub_device_count.to_be_bytes());
        v.push(self.sensor_count);
        v
    }
}

/// PARAMETER_DESCRIPTION response (20 bytes + description ≤ 32).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ParameterDescription {
    /// Described PID.
    pub pid: u16,
    /// PDL of the parameter.
    pub pdl_size: u8,
    /// Data type, see [`data_type_name`].
    pub data_type: u8,
    /// Command class: 0x01 GET, 0x02 SET, 0x03 GET_SET.
    pub command_class: u8,
    /// The `type` byte (should be 0).
    pub param_type: u8,
    /// Unit, see [`unit_name`].
    pub unit: u8,
    /// Prefix, see [`prefix_name`].
    pub prefix: u8,
    /// Minimum valid value.
    pub min_valid_value: u32,
    /// Maximum valid value.
    pub max_valid_value: u32,
    /// Default value.
    pub default_value: u32,
    /// Description (ASCII ≤ 32).
    pub description: String,
}

impl ParameterDescription {
    /// PID of this PD.
    pub const PID: u16 = pid::PARAMETER_DESCRIPTION;
    /// Length of the fixed part.
    pub const FIXED_LEN: usize = 20;

    /// Decode 20..=52 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::new(pd, "PARAMETER_DESCRIPTION: expected 20..52 bytes");
        Ok(Self {
            pid: r.u16()?,
            pdl_size: r.u8()?,
            data_type: r.u8()?,
            command_class: r.u8()?,
            param_type: r.u8()?,
            unit: r.u8()?,
            prefix: r.u8()?,
            min_valid_value: r.u32()?,
            max_valid_value: r.u32()?,
            default_value: r.u32()?,
            description: r.label()?,
        })
    }
    /// Encode (description truncated to 32 ASCII bytes).
    pub fn encode(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(Self::FIXED_LEN + MAX_LABEL);
        v.extend_from_slice(&self.pid.to_be_bytes());
        v.extend_from_slice(&[
            self.pdl_size,
            self.data_type,
            self.command_class,
            self.param_type,
            self.unit,
            self.prefix,
        ]);
        v.extend_from_slice(&self.min_valid_value.to_be_bytes());
        v.extend_from_slice(&self.max_valid_value.to_be_bytes());
        v.extend_from_slice(&self.default_value.to_be_bytes());
        v.extend_from_slice(&encode_label(&self.description));
        v
    }
    /// PARAMETER_DESCRIPTION answer for a manufacturer PID from `pids.toml`; `None` if it has no `[pid.pd]`.
    pub fn from_pid_def(def: &pids::PidDef) -> Option<Self> {
        let d = def.desc?;
        Some(Self {
            pid: def.pid,
            pdl_size: d.pdl_size,
            data_type: d.data_type,
            command_class: def.command_class,
            param_type: 0,
            unit: d.unit,
            prefix: d.prefix,
            min_valid_value: d.min,
            max_valid_value: d.max,
            default_value: d.default,
            description: d.description.to_owned(),
        })
    }
}

/// Name of a PARAMETER_DESCRIPTION data type without the `DS_` prefix (e.g. `"UNSIGNED_WORD"`); `None` for
/// unknown and manufacturer-specific (0x80–0xDF) codes.
pub fn data_type_name(code: u8) -> Option<&'static str> {
    Some(match code {
        0x00 => "NOT_DEFINED",
        0x01 => "BIT_FIELD",
        0x02 => "ASCII",
        0x03 => "UNSIGNED_BYTE",
        0x04 => "SIGNED_BYTE",
        0x05 => "UNSIGNED_WORD",
        0x06 => "SIGNED_WORD",
        0x07 => "UNSIGNED_DWORD",
        0x08 => "SIGNED_DWORD",
        0x09 => "UINT64",
        0x0A => "INT64",
        0x0B => "GROUP",
        0x0C => "UID",
        0x0D => "BOOLEAN",
        0x0E => "URL",
        0x0F => "MAC",
        0x10 => "IPV4",
        0x11 => "IPV6",
        0x12 => "ENUMERATION",
        _ => return None,
    })
}

/// Name of a unit without the `UNITS_` prefix (e.g. `"SECOND"`); `None` for unknown and manufacturer-specific
/// (0x80–0xFF) codes.
pub fn unit_name(code: u8) -> Option<&'static str> {
    const UNITS: [&str; 0x25] = [
        "NONE",
        "CENTIGRADE",
        "VOLTS_DC",
        "VOLTS_AC_PEAK",
        "VOLTS_AC_RMS",
        "AMPERE_DC",
        "AMPERE_AC_PEAK",
        "AMPERE_AC_RMS",
        "HERTZ",
        "OHM",
        "WATT",
        "KILOGRAM",
        "METERS",
        "METERS_SQUARED",
        "METERS_CUBED",
        "KILOGRAMMES_PER_METER_CUBED",
        "METERS_PER_SECOND",
        "METERS_PER_SECOND_SQUARED",
        "NEWTON",
        "JOULE",
        "PASCAL",
        "SECOND",
        "DEGREE",
        "STERADIAN",
        "CANDELA",
        "LUMEN",
        "LUX",
        "IRE",
        "BYTE",
        "DECIBEL",
        "DECIBEL_VOLT",
        "DECIBEL_WATT",
        "DECIBEL_METER",
        "PERCENT",
        "MOLES_PER_METER_CUBED",
        "RPM",
        "BYTE_PER_SECOND",
    ];
    UNITS.get(usize::from(code)).copied()
}

const PREFIXES: [(u8, &str, i8); 21] = [
    (0x00, "NONE", 0),
    (0x01, "DECI", -1),
    (0x02, "CENTI", -2),
    (0x03, "MILLI", -3),
    (0x04, "MICRO", -6),
    (0x05, "NANO", -9),
    (0x06, "PICO", -12),
    (0x07, "FEMTO", -15),
    (0x08, "ATTO", -18),
    (0x09, "ZEPTO", -21),
    (0x0A, "YOCTO", -24),
    (0x11, "DECA", 1),
    (0x12, "HECTO", 2),
    (0x13, "KILO", 3),
    (0x14, "MEGA", 6),
    (0x15, "GIGA", 9),
    (0x16, "TERA", 12),
    (0x17, "PETA", 15),
    (0x18, "EXA", 18),
    (0x19, "ZETTA", 21),
    (0x1A, "YOTTA", 24),
];

/// Name of a unit prefix without the `PREFIX_` prefix (e.g. `"MICRO"`); `None` for unknown codes.
pub fn prefix_name(code: u8) -> Option<&'static str> {
    PREFIXES.iter().find(|p| p.0 == code).map(|p| p.1)
}

/// Decimal exponent of a unit prefix (MICRO → -6, KILO → 3); 0 for NONE and unknown codes.
pub fn prefix_exponent(code: u8) -> i8 {
    PREFIXES.iter().find(|p| p.0 == code).map_or(0, |p| p.2)
}

// ---------------------------------------------------------------------------------------------------------------
// DMX setup and sensors (digest §6.3)
// ---------------------------------------------------------------------------------------------------------------

/// DMX_PERSONALITY GET response (2 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DmxPersonality {
    /// Current personality (1-based).
    pub current: u8,
    /// Number of personalities.
    pub count: u8,
}

impl DmxPersonality {
    /// PID of this PD.
    pub const PID: u16 = pid::DMX_PERSONALITY;
    /// Decode 2 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 2, "DMX_PERSONALITY: expected 2 bytes")?;
        Ok(Self {
            current: r.u8()?,
            count: r.u8()?,
        })
    }
    /// Encode 2 bytes.
    pub fn encode(&self) -> Vec<u8> {
        vec![self.current, self.count]
    }
}

/// DMX_PERSONALITY_DESCRIPTION response (3 bytes + description ≤ 32).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PersonalityDescription {
    /// Personality number.
    pub personality: u8,
    /// DMX slots required.
    pub slots_required: u16,
    /// Description.
    pub description: String,
}

impl PersonalityDescription {
    /// PID of this PD.
    pub const PID: u16 = pid::DMX_PERSONALITY_DESCRIPTION;
    /// Decode 3..=35 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::new(pd, "DMX_PERSONALITY_DESCRIPTION: expected 3..35 bytes");
        Ok(Self {
            personality: r.u8()?,
            slots_required: r.u16()?,
            description: r.label()?,
        })
    }
    /// Encode (description truncated to 32 ASCII bytes).
    pub fn encode(&self) -> Vec<u8> {
        let mut v = vec![self.personality];
        v.extend_from_slice(&self.slots_required.to_be_bytes());
        v.extend_from_slice(&encode_label(&self.description));
        v
    }
}

/// One SLOT_INFO entry (5 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SlotInfo {
    /// Slot offset (0-based from the start address).
    pub slot_offset: u16,
    /// Slot type (ST_*).
    pub slot_type: u8,
    /// Slot label ID (SD_*) or the primary slot for secondary types.
    pub slot_label_id: u16,
}

impl SlotInfo {
    /// PID of this PD.
    pub const PID: u16 = pid::SLOT_INFO;
    /// Size of one entry.
    pub const LEN: usize = 5;
    const WHAT: &'static str = "SLOT_INFO: length not a multiple of 5";
    fn read(r: &mut Rd<'_>) -> Result<Self, RdmError> {
        Ok(Self {
            slot_offset: r.u16()?,
            slot_type: r.u8()?,
            slot_label_id: r.u16()?,
        })
    }
    /// Decode exactly one entry.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        Self::read(&mut Rd::exact(pd, Self::LEN, Self::WHAT)?)
    }
    /// Encode one entry.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.slot_offset.to_be_bytes().to_vec();
        v.push(self.slot_type);
        v.extend_from_slice(&self.slot_label_id.to_be_bytes());
        v
    }
    /// Decode the SLOT_INFO response (n × 5 bytes).
    pub fn decode_list(pd: &[u8]) -> Result<Vec<Self>, RdmError> {
        decode_records(pd, Self::LEN, Self::WHAT, Self::read)
    }
    /// Encode the SLOT_INFO response.
    pub fn encode_list(list: &[Self]) -> Vec<u8> {
        list.iter().flat_map(Self::encode).collect()
    }
}

/// SLOT_DESCRIPTION response (2 bytes + description ≤ 32).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SlotDescription {
    /// Slot offset.
    pub slot: u16,
    /// Description.
    pub description: String,
}

impl SlotDescription {
    /// PID of this PD.
    pub const PID: u16 = pid::SLOT_DESCRIPTION;
    /// Decode 2..=34 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::new(pd, "SLOT_DESCRIPTION: expected 2..34 bytes");
        Ok(Self {
            slot: r.u16()?,
            description: r.label()?,
        })
    }
    /// Encode (description truncated to 32 ASCII bytes).
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.slot.to_be_bytes().to_vec();
        v.extend_from_slice(&encode_label(&self.description));
        v
    }
}

/// One DEFAULT_SLOT_VALUE entry (3 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DefaultSlotValue {
    /// Slot offset.
    pub slot_offset: u16,
    /// Default value.
    pub value: u8,
}

impl DefaultSlotValue {
    /// PID of this PD.
    pub const PID: u16 = pid::DEFAULT_SLOT_VALUE;
    /// Size of one entry.
    pub const LEN: usize = 3;
    const WHAT: &'static str = "DEFAULT_SLOT_VALUE: length not a multiple of 3";
    fn read(r: &mut Rd<'_>) -> Result<Self, RdmError> {
        Ok(Self {
            slot_offset: r.u16()?,
            value: r.u8()?,
        })
    }
    /// Decode exactly one entry.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        Self::read(&mut Rd::exact(pd, Self::LEN, Self::WHAT)?)
    }
    /// Encode one entry.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.slot_offset.to_be_bytes().to_vec();
        v.push(self.value);
        v
    }
    /// Decode the DEFAULT_SLOT_VALUE response (n × 3 bytes).
    pub fn decode_list(pd: &[u8]) -> Result<Vec<Self>, RdmError> {
        decode_records(pd, Self::LEN, Self::WHAT, Self::read)
    }
    /// Encode the DEFAULT_SLOT_VALUE response.
    pub fn encode_list(list: &[Self]) -> Vec<u8> {
        list.iter().flat_map(Self::encode).collect()
    }
}

/// SENSOR_DEFINITION response (13 bytes + description ≤ 32).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SensorDefinition {
    /// Sensor number (0..254).
    pub sensor: u8,
    /// Sensor type.
    pub sensor_type: u8,
    /// Unit, see [`unit_name`].
    pub unit: u8,
    /// Prefix, see [`prefix_name`].
    pub prefix: u8,
    /// Range minimum.
    pub range_min: i16,
    /// Range maximum.
    pub range_max: i16,
    /// Normal minimum.
    pub normal_min: i16,
    /// Normal maximum.
    pub normal_max: i16,
    /// Bit 0: recorded value supported, bit 1: lowest/highest supported.
    pub recorded_support: u8,
    /// Description.
    pub description: String,
}

impl SensorDefinition {
    /// PID of this PD.
    pub const PID: u16 = pid::SENSOR_DEFINITION;
    /// Decode 13..=45 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::new(pd, "SENSOR_DEFINITION: expected 13..45 bytes");
        Ok(Self {
            sensor: r.u8()?,
            sensor_type: r.u8()?,
            unit: r.u8()?,
            prefix: r.u8()?,
            range_min: r.i16()?,
            range_max: r.i16()?,
            normal_min: r.i16()?,
            normal_max: r.i16()?,
            recorded_support: r.u8()?,
            description: r.label()?,
        })
    }
    /// Encode (description truncated to 32 ASCII bytes).
    pub fn encode(&self) -> Vec<u8> {
        let mut v = vec![self.sensor, self.sensor_type, self.unit, self.prefix];
        for x in [
            self.range_min,
            self.range_max,
            self.normal_min,
            self.normal_max,
        ] {
            v.extend_from_slice(&x.to_be_bytes());
        }
        v.push(self.recorded_support);
        v.extend_from_slice(&encode_label(&self.description));
        v
    }
}

/// SENSOR_VALUE response (9 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SensorValue {
    /// Sensor number.
    pub sensor: u8,
    /// Present value.
    pub present: i16,
    /// Lowest detected value.
    pub lowest: i16,
    /// Highest detected value.
    pub highest: i16,
    /// Recorded value.
    pub recorded: i16,
}

impl SensorValue {
    /// PID of this PD.
    pub const PID: u16 = pid::SENSOR_VALUE;
    /// Decode 9 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 9, "SENSOR_VALUE: expected 9 bytes")?;
        Ok(Self {
            sensor: r.u8()?,
            present: r.i16()?,
            lowest: r.i16()?,
            highest: r.i16()?,
            recorded: r.i16()?,
        })
    }
    /// Encode 9 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = vec![self.sensor];
        for x in [self.present, self.lowest, self.highest, self.recorded] {
            v.extend_from_slice(&x.to_be_bytes());
        }
        v
    }
}

// ---------------------------------------------------------------------------------------------------------------
// E1.37-2 (digest §6.5)
// ---------------------------------------------------------------------------------------------------------------

/// IPV4_CURRENT_ADDRESS response (10 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Ipv4CurrentAddress {
    /// Interface ID.
    pub interface_id: u32,
    /// Address (0.0.0.0 = unconfigured).
    pub address: Ipv4Addr,
    /// Netmask as CIDR prefix length (0..32).
    pub netmask: u8,
    /// DHCP status: 0 inactive, 1 active, 2 unknown.
    pub dhcp_status: u8,
}

impl Ipv4CurrentAddress {
    /// PID of this PD.
    pub const PID: u16 = pid::IPV4_CURRENT_ADDRESS;
    /// Decode 10 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 10, "IPV4_CURRENT_ADDRESS: expected 10 bytes")?;
        Ok(Self {
            interface_id: r.u32()?,
            address: r.ipv4()?,
            netmask: r.u8()?,
            dhcp_status: r.u8()?,
        })
    }
    /// Encode 10 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.interface_id.to_be_bytes().to_vec();
        v.extend_from_slice(&self.address.octets());
        v.push(self.netmask);
        v.push(self.dhcp_status);
        v
    }
}

/// One LIST_INTERFACES entry (6 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InterfaceEntry {
    /// Interface ID (1..0xFFFFFF00).
    pub interface_id: u32,
    /// Hardware type (1 = Ethernet).
    pub hardware_type: u16,
}

impl InterfaceEntry {
    /// PID of this PD.
    pub const PID: u16 = pid::LIST_INTERFACES;
    /// Size of one entry.
    pub const LEN: usize = 6;
    const WHAT: &'static str = "LIST_INTERFACES: length not a multiple of 6";
    fn read(r: &mut Rd<'_>) -> Result<Self, RdmError> {
        Ok(Self {
            interface_id: r.u32()?,
            hardware_type: r.u16()?,
        })
    }
    /// Decode exactly one entry.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        Self::read(&mut Rd::exact(pd, Self::LEN, Self::WHAT)?)
    }
    /// Encode one entry.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.interface_id.to_be_bytes().to_vec();
        v.extend_from_slice(&self.hardware_type.to_be_bytes());
        v
    }
    /// Decode the LIST_INTERFACES response (n × 6 bytes).
    pub fn decode_list(pd: &[u8]) -> Result<Vec<Self>, RdmError> {
        decode_records(pd, Self::LEN, Self::WHAT, Self::read)
    }
    /// Encode the LIST_INTERFACES response.
    pub fn encode_list(list: &[Self]) -> Vec<u8> {
        list.iter().flat_map(Self::encode).collect()
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Project PIDs (pids.toml)
// ---------------------------------------------------------------------------------------------------------------

/// BOOT_INFO response of the tube bootloader (19 bytes, `pids.toml`, `flash_rdm.py boot_info_parse`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BootInfo {
    /// Bootloader version (`BOOT_RDM_VERSION`).
    pub version: u8,
    /// Flash page size (64).
    pub page_size: u8,
    /// End of the application space (0x1800).
    pub app_end: u16,
    /// 1 while a page is erased/written.
    pub busy: u8,
    /// `BOOT_ERR_*`, cleared by reading.
    pub last_error: u8,
    /// ESTA manufacturer ID.
    pub manufacturer: u16,
    /// Device model ID.
    pub model: u16,
    /// Bootloader software version ID (`BOOT_RDM_SW_VERSION_ID`).
    pub boot_sw_version: u32,
    /// Installed app trailer valid (0/1).
    pub app_valid: u8,
    /// FW_VERSION_ID from the app trailer.
    pub app_fw_version: u32,
}

impl BootInfo {
    /// PID of this PD.
    pub const PID: u16 = pids::PID_BOOT_INFO;
    /// PD length of bootloader v3.
    pub const LEN: usize = 19;
    /// Decode at least 19 bytes; extra trailing bytes (future bootloader versions) are ignored. Shorter answers
    /// come from bootloaders older than `BOOT_RDM_PROTOCOL` and are rejected.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        if pd.len() < Self::LEN {
            return Err(RdmError::BadParameterData(
                "BOOT_INFO: shorter than 19 bytes (old bootloader)",
            ));
        }
        let mut r = Rd::new(pd, "BOOT_INFO: shorter than 19 bytes");
        Ok(Self {
            version: r.u8()?,
            page_size: r.u8()?,
            app_end: r.u16()?,
            busy: r.u8()?,
            last_error: r.u8()?,
            manufacturer: r.u16()?,
            model: r.u16()?,
            boot_sw_version: r.u32()?,
            app_valid: r.u8()?,
            app_fw_version: r.u32()?,
        })
    }
    /// Encode 19 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = vec![self.version, self.page_size];
        v.extend_from_slice(&self.app_end.to_be_bytes());
        v.push(self.busy);
        v.push(self.last_error);
        v.extend_from_slice(&self.manufacturer.to_be_bytes());
        v.extend_from_slice(&self.model.to_be_bytes());
        v.extend_from_slice(&self.boot_sw_version.to_be_bytes());
        v.push(self.app_valid);
        v.extend_from_slice(&self.app_fw_version.to_be_bytes());
        v
    }
}

/// BOOT_CATCH_START request (7 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BootCatchStart {
    /// DMX port 1..4.
    pub port: u8,
    /// Tube to catch.
    pub target_uid: Uid,
}

impl BootCatchStart {
    /// PID of this PD.
    pub const PID: u16 = pids::PID_BOOT_CATCH_START;
    /// Decode 7 bytes (port range is checked by the node, not here).
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 7, "BOOT_CATCH_START: expected 7 bytes")?;
        Ok(Self {
            port: r.u8()?,
            target_uid: r.uid()?,
        })
    }
    /// Encode 7 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = vec![self.port];
        v.extend_from_slice(&self.target_uid.to_bytes());
        v
    }
}

/// BOOT_CATCH_STATUS response (14 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BootCatchStatus {
    /// State, see [`crate::pids::boot_catch_state`].
    pub state: u8,
    /// Port of the catch.
    pub port: u8,
    /// Target UID.
    pub target_uid: Uid,
    /// Time since start.
    pub elapsed_ms: u32,
    /// GET BOOT_INFO attempts sent.
    pub attempts: u16,
}

impl BootCatchStatus {
    /// PID of this PD.
    pub const PID: u16 = pids::PID_BOOT_CATCH_STATUS;
    /// Decode 14 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 14, "BOOT_CATCH_STATUS: expected 14 bytes")?;
        Ok(Self {
            state: r.u8()?,
            port: r.u8()?,
            target_uid: r.uid()?,
            elapsed_ms: r.u32()?,
            attempts: r.u16()?,
        })
    }
    /// Encode 14 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = vec![self.state, self.port];
        v.extend_from_slice(&self.target_uid.to_bytes());
        v.extend_from_slice(&self.elapsed_ms.to_be_bytes());
        v.extend_from_slice(&self.attempts.to_be_bytes());
        v
    }
}

/// LOCAL_RDM_SET response (13 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LocalRdmSet {
    /// Sequence number, counts up from boot.
    pub sequence: u16,
    /// Port of the device.
    pub port: u8,
    /// Device UID.
    pub uid: Uid,
    /// Sub-device.
    pub sub_device: u16,
    /// PID that was set.
    pub pid: u16,
}

impl LocalRdmSet {
    /// PID of this PD.
    pub const PID: u16 = pids::PID_LOCAL_RDM_SET;
    /// Decode 13 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 13, "LOCAL_RDM_SET: expected 13 bytes")?;
        Ok(Self {
            sequence: r.u16()?,
            port: r.u8()?,
            uid: r.uid()?,
            sub_device: r.u16()?,
            pid: r.u16()?,
        })
    }
    /// Encode 13 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.sequence.to_be_bytes().to_vec();
        v.push(self.port);
        v.extend_from_slice(&self.uid.to_bytes());
        v.extend_from_slice(&self.sub_device.to_be_bytes());
        v.extend_from_slice(&self.pid.to_be_bytes());
        v
    }
}

/// FW_INFO response (20 bytes).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FwInfo {
    /// ESTA manufacturer ID.
    pub manufacturer: u16,
    /// Device model ID.
    pub model: u16,
    /// Hardware revision.
    pub hw_revision: u8,
    /// Running FW_VERSION_ID 0x00MMmmpp.
    pub fw_version: u32,
    /// State, see [`crate::pids::fw_state`].
    pub state: u8,
    /// Last error, see [`crate::pids::fw_error`].
    pub last_error: u8,
    /// Maximum FW_WRITE data bytes (`NODE_FW_MAX_BLOCK`).
    pub max_block: u8,
    /// Bytes received.
    pub received: u32,
    /// Announced image length.
    pub total: u32,
}

impl FwInfo {
    /// PID of this PD.
    pub const PID: u16 = pids::PID_FW_INFO;
    /// Decode 20 bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::exact(pd, 20, "FW_INFO: expected 20 bytes")?;
        Ok(Self {
            manufacturer: r.u16()?,
            model: r.u16()?,
            hw_revision: r.u8()?,
            fw_version: r.u32()?,
            state: r.u8()?,
            last_error: r.u8()?,
            max_block: r.u8()?,
            received: r.u32()?,
            total: r.u32()?,
        })
    }
    /// Encode 20 bytes.
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.manufacturer.to_be_bytes().to_vec();
        v.extend_from_slice(&self.model.to_be_bytes());
        v.push(self.hw_revision);
        v.extend_from_slice(&self.fw_version.to_be_bytes());
        v.extend_from_slice(&[self.state, self.last_error, self.max_block]);
        v.extend_from_slice(&self.received.to_be_bytes());
        v.extend_from_slice(&self.total.to_be_bytes());
        v
    }
}

/// FW_WRITE request: offset + 0..=`NODE_FW_MAX_BLOCK` data bytes.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FwWrite {
    /// Byte offset in the image.
    pub offset: u32,
    /// Image bytes.
    pub data: Vec<u8>,
}

impl FwWrite {
    /// PID of this PD.
    pub const PID: u16 = pids::PID_FW_WRITE;
    const WHAT: &'static str = "FW_WRITE: expected 4 + 0..NODE_FW_MAX_BLOCK bytes";

    /// Maximum data bytes per request.
    pub fn max_block() -> usize {
        usize::from(pids::NODE_FW_MAX_BLOCK)
    }
    /// Construct, rejecting more than `NODE_FW_MAX_BLOCK` data bytes.
    pub fn new(offset: u32, data: Vec<u8>) -> Result<Self, RdmError> {
        if data.len() > Self::max_block() {
            return Err(RdmError::BadParameterData(Self::WHAT));
        }
        Ok(Self { offset, data })
    }
    /// Decode 4..=4+`NODE_FW_MAX_BLOCK` bytes.
    pub fn decode(pd: &[u8]) -> Result<Self, RdmError> {
        let mut r = Rd::new(pd, Self::WHAT);
        let offset = r.u32()?;
        Self::new(offset, r.rest().to_vec())
    }
    /// Encode (writes all of `data`; build with [`FwWrite::new`] to respect the block limit).
    pub fn encode(&self) -> Vec<u8> {
        let mut v = self.offset.to_be_bytes().to_vec();
        v.extend_from_slice(&self.data);
        v
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn uid() -> Uid {
        Uid::new(pids::ESTA_MANUFACTURER, 0x1234_5678)
    }

    /// Every decoder must not panic on any length 0..=231; a successful decode must also encode without panic.
    fn fuzz<T>(dec: impl Fn(&[u8]) -> Result<T, RdmError>, enc: impl Fn(&T) -> Vec<u8>) {
        for fill in [0x00u8, 0x41, 0xFF] {
            for len in 0..=crate::rdm::MAX_PDL {
                let buf: Vec<u8> = (0..len)
                    .map(|i| fill.wrapping_add(i as u8 & 0x0F))
                    .collect();
                if let Ok(v) = dec(&buf) {
                    let _ = enc(&v);
                }
            }
        }
    }

    fn check_lengths<T: std::fmt::Debug>(dec: impl Fn(&[u8]) -> Result<T, RdmError>, ok: &[usize]) {
        for len in 0..=crate::rdm::MAX_PDL {
            let buf = vec![0x20u8; len];
            let r = dec(&buf);
            assert_eq!(r.is_ok(), ok.contains(&len), "len {len}: {r:?}");
            if let Err(e) = r {
                assert!(matches!(e, RdmError::BadParameterData(_)), "{e:?}");
            }
        }
    }

    #[test]
    fn no_panic_any_length() {
        fuzz(DiscUniqueBranch::decode, DiscUniqueBranch::encode);
        fuzz(DiscMuteResponse::decode, DiscMuteResponse::encode);
        fuzz(StatusMessage::decode, StatusMessage::encode);
        fuzz(StatusMessage::decode_list, |l| {
            StatusMessage::encode_list(l)
        });
        fuzz(DeviceInfo::decode, DeviceInfo::encode);
        fuzz(ParameterDescription::decode, ParameterDescription::encode);
        fuzz(DmxPersonality::decode, DmxPersonality::encode);
        fuzz(
            PersonalityDescription::decode,
            PersonalityDescription::encode,
        );
        fuzz(SlotInfo::decode_list, |l| SlotInfo::encode_list(l));
        fuzz(SlotDescription::decode, SlotDescription::encode);
        fuzz(DefaultSlotValue::decode_list, |l| {
            DefaultSlotValue::encode_list(l)
        });
        fuzz(SensorDefinition::decode, SensorDefinition::encode);
        fuzz(SensorValue::decode, SensorValue::encode);
        fuzz(Ipv4CurrentAddress::decode, Ipv4CurrentAddress::encode);
        fuzz(InterfaceEntry::decode_list, |l| {
            InterfaceEntry::encode_list(l)
        });
        fuzz(BootInfo::decode, BootInfo::encode);
        fuzz(BootCatchStart::decode, BootCatchStart::encode);
        fuzz(BootCatchStatus::decode, BootCatchStatus::encode);
        fuzz(LocalRdmSet::decode, LocalRdmSet::encode);
        fuzz(FwInfo::decode, FwInfo::encode);
        fuzz(FwWrite::decode, FwWrite::encode);
        fuzz(decode_pid_list, |l| encode_pid_list(l));
        fuzz(|b| Ok::<_, RdmError>(decode_label(b)), |s| encode_label(s));
        fuzz(decode_u8, |_| vec![]);
        fuzz(decode_u16, |_| vec![]);
        fuzz(decode_u32, |_| vec![]);
    }

    #[test]
    fn length_rules() {
        let range = |a: usize, b: usize| (a..=b).collect::<Vec<_>>();
        let multiples = |n: usize| (0..=231).filter(|l| l % n == 0).collect::<Vec<_>>();
        check_lengths(DiscUniqueBranch::decode, &[12]);
        check_lengths(DiscMuteResponse::decode, &[2, 8]);
        check_lengths(StatusMessage::decode, &[9]);
        check_lengths(StatusMessage::decode_list, &multiples(9));
        check_lengths(DeviceInfo::decode, &[19]);
        check_lengths(ParameterDescription::decode, &range(20, 52));
        check_lengths(DmxPersonality::decode, &[2]);
        check_lengths(PersonalityDescription::decode, &range(3, 35));
        check_lengths(SlotInfo::decode_list, &multiples(5));
        check_lengths(SlotDescription::decode, &range(2, 34));
        check_lengths(DefaultSlotValue::decode_list, &multiples(3));
        check_lengths(SensorDefinition::decode, &range(13, 45));
        check_lengths(SensorValue::decode, &[9]);
        check_lengths(Ipv4CurrentAddress::decode, &[10]);
        check_lengths(InterfaceEntry::decode_list, &multiples(6));
        check_lengths(BootInfo::decode, &range(19, 231));
        check_lengths(BootCatchStart::decode, &[7]);
        check_lengths(BootCatchStatus::decode, &[14]);
        check_lengths(LocalRdmSet::decode, &[13]);
        check_lengths(FwInfo::decode, &[20]);
        check_lengths(FwWrite::decode, &range(4, 4 + FwWrite::max_block()));
        check_lengths(decode_pid_list, &multiples(2));
        check_lengths(decode_u8, &[1]);
        check_lengths(decode_u16, &[2]);
        check_lengths(decode_u32, &[4]);
    }

    fn roundtrip<T: PartialEq + std::fmt::Debug>(
        v: T,
        dec: impl Fn(&[u8]) -> Result<T, RdmError>,
        enc: impl Fn(&T) -> Vec<u8>,
    ) -> Vec<u8> {
        let b = enc(&v);
        assert_eq!(dec(&b).unwrap(), v);
        b
    }

    #[test]
    fn roundtrips() {
        let dub = DiscUniqueBranch {
            lower_bound_uid: Uid::new(0, 0),
            upper_bound_uid: Uid::new(0x7FFF, 0xFFFF_FFFF),
        };
        assert!(dub.contains(uid()));
        assert!(!dub.contains(Uid::new(0x8000, 0)));
        roundtrip(dub, DiscUniqueBranch::decode, DiscUniqueBranch::encode);

        let m = DiscMuteResponse {
            control_field: DiscMuteResponse::BOOT_LOADER_FLAG | DiscMuteResponse::SUB_DEVICE_FLAG,
            binding_uid: Some(uid()),
        };
        assert!(m.boot_loader() && m.sub_device() && !m.managed_proxy() && !m.proxied_device());
        assert_eq!(
            roundtrip(m, DiscMuteResponse::decode, DiscMuteResponse::encode).len(),
            8
        );

        let list = vec![
            StatusMessage {
                sub_device: 0,
                status_type: crate::rdm::status_type::ADVISORY,
                status_message_id: pids::status::BOOT_CATCH_CAUGHT,
                data_value1: 2,
                data_value2: -1,
            };
            25
        ];
        let b = roundtrip(list, StatusMessage::decode_list, |l| {
            StatusMessage::encode_list(l)
        });
        assert_eq!(b.len(), 225);
        assert_eq!(&b[..9], &[0, 0, 2, 0x80, 0x01, 0, 2, 0xFF, 0xFF]);

        roundtrip(
            DmxPersonality {
                current: 1,
                count: 4,
            },
            DmxPersonality::decode,
            DmxPersonality::encode,
        );
        roundtrip(
            PersonalityDescription {
                personality: 1,
                slots_required: 12,
                description: "12 x RGBW".into(),
            },
            PersonalityDescription::decode,
            PersonalityDescription::encode,
        );
        roundtrip(
            vec![SlotInfo {
                slot_offset: 3,
                slot_type: 0,
                slot_label_id: 0x0205,
            }],
            SlotInfo::decode_list,
            |l| SlotInfo::encode_list(l),
        );
        roundtrip(
            SlotDescription {
                slot: 3,
                description: "Red".into(),
            },
            SlotDescription::decode,
            SlotDescription::encode,
        );
        roundtrip(
            vec![DefaultSlotValue {
                slot_offset: 1,
                value: 255,
            }],
            DefaultSlotValue::decode_list,
            |l| DefaultSlotValue::encode_list(l),
        );
        roundtrip(
            SensorDefinition {
                sensor: 0,
                sensor_type: 0,
                unit: 1,
                prefix: 0,
                range_min: -40,
                range_max: 125,
                normal_min: 0,
                normal_max: 70,
                recorded_support: 3,
                description: "Board temperature".into(),
            },
            SensorDefinition::decode,
            SensorDefinition::encode,
        );
        roundtrip(
            SensorValue {
                sensor: 0,
                present: -5,
                lowest: -10,
                highest: 50,
                recorded: 0,
            },
            SensorValue::decode,
            SensorValue::encode,
        );
        roundtrip(
            Ipv4CurrentAddress {
                interface_id: 1,
                address: Ipv4Addr::new(2, 0, 0, 10),
                netmask: 8,
                dhcp_status: 0,
            },
            Ipv4CurrentAddress::decode,
            Ipv4CurrentAddress::encode,
        );
        roundtrip(
            vec![InterfaceEntry {
                interface_id: 1,
                hardware_type: 1,
            }],
            InterfaceEntry::decode_list,
            |l| InterfaceEntry::encode_list(l),
        );
        roundtrip(
            BootCatchStart {
                port: 2,
                target_uid: uid(),
            },
            BootCatchStart::decode,
            BootCatchStart::encode,
        );
        roundtrip(
            BootCatchStatus {
                state: pids::boot_catch_state::CAUGHT,
                port: 2,
                target_uid: uid(),
                elapsed_ms: 1234,
                attempts: 61,
            },
            BootCatchStatus::decode,
            BootCatchStatus::encode,
        );
        roundtrip(
            LocalRdmSet {
                sequence: 7,
                port: 1,
                uid: uid(),
                sub_device: 0,
                pid: pid::DMX_START_ADDRESS,
            },
            LocalRdmSet::decode,
            LocalRdmSet::encode,
        );
        roundtrip(
            FwInfo {
                manufacturer: pids::ESTA_MANUFACTURER,
                model: pids::MODEL_NODE,
                hw_revision: 0,
                fw_version: 0x0001_0203,
                state: pids::fw_state::RECEIVING,
                last_error: pids::fw_error::NONE,
                max_block: pids::NODE_FW_MAX_BLOCK as u8,
                received: 4096,
                total: 1 << 20,
            },
            FwInfo::decode,
            FwInfo::encode,
        );
        let w = FwWrite::new(448, vec![0xAB; FwWrite::max_block()]).unwrap();
        assert_eq!(
            roundtrip(w, FwWrite::decode, FwWrite::encode).len(),
            4 + FwWrite::max_block()
        );
        assert!(FwWrite::new(0, vec![0; FwWrite::max_block() + 1]).is_err());
        roundtrip(
            vec![pid::DEVICE_INFO, pids::PID_FW_INFO],
            decode_pid_list,
            |l| encode_pid_list(l),
        );
    }

    #[test]
    fn labels() {
        assert_eq!(decode_label(b"abc\0def"), "abc");
        assert_eq!(decode_label(&[b'x'; 40]).len(), 32);
        assert_eq!(decode_label(b""), "");
        assert_eq!(encode_label("Grüße"), b"Gr??e");
        assert_eq!(encode_label(&"y".repeat(40)).len(), 32);
        assert_eq!(encode_label("a\0b"), b"a?b");
    }

    #[test]
    fn parameter_description_from_pids_toml() {
        let mut n = 0;
        for def in pids::PIDS {
            let Some(pd) = ParameterDescription::from_pid_def(def) else {
                assert!(def.desc.is_none());
                continue;
            };
            n += 1;
            assert_eq!(pd.pid, def.pid);
            assert!(pd.description.len() <= MAX_LABEL, "{}", def.name);
            assert!(data_type_name(pd.data_type).is_some(), "{}", def.name);
            assert!(unit_name(pd.unit).is_some(), "{}", def.name);
            assert!(prefix_name(pd.prefix).is_some(), "{}", def.name);
            roundtrip(
                pd,
                ParameterDescription::decode,
                ParameterDescription::encode,
            );
        }
        assert!(n > 0);
        let brk = pids::PIDS
            .iter()
            .find(|d| d.pid == pids::PID_PORT1_DMX_BREAK)
            .and_then(ParameterDescription::from_pid_def)
            .unwrap();
        assert_eq!(unit_name(brk.unit), Some("SECOND"));
        assert_eq!(prefix_name(brk.prefix), Some("MICRO"));
        assert_eq!(prefix_exponent(brk.prefix), -6);
        assert_eq!(data_type_name(brk.data_type), Some("UNSIGNED_WORD"));
        assert_eq!(brk.command_class, 0x03);
    }

    #[test]
    fn enum_names() {
        assert_eq!(data_type_name(0x12), Some("ENUMERATION"));
        assert_eq!(data_type_name(0x80), None);
        assert_eq!(unit_name(0x24), Some("BYTE_PER_SECOND"));
        assert_eq!(unit_name(0x25), None);
        assert_eq!(prefix_name(0x13), Some("KILO"));
        assert_eq!(prefix_exponent(0x1A), 24);
        assert_eq!(prefix_exponent(0x0B), 0);
        assert_eq!(prefix_name(0x0B), None);
    }

    #[test]
    fn serde_camel_case() {
        let s = serde_json::to_value(BootCatchStart {
            port: 1,
            target_uid: uid(),
        })
        .unwrap();
        assert_eq!(
            s,
            serde_json::json!({"port": 1, "targetUid": "7FF0:12345678"})
        );
        let s = serde_json::to_value(Ipv4CurrentAddress {
            interface_id: 1,
            address: Ipv4Addr::new(2, 0, 0, 10),
            netmask: 8,
            dhcp_status: 1,
        })
        .unwrap();
        assert_eq!(s["address"], "2.0.0.10");
    }
}
