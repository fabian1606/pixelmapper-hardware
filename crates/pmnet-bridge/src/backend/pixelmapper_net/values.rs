//! RDM parameter data ↔ the contract's `RdmValue` (`protocol/bridge-ws.md` §4, `RdmValue` table), built on the
//! parameter-data codecs of `pixelmapper-net` (`rdm::pd`) and the generated `pids` module.
//!
//! Manufacturer PIDs are decoded when a single-value PARAMETER_DESCRIPTION is known: read from the device
//! (`rdmDescribe`, or an `rdmGet` of PARAMETER_DESCRIPTION) or generated from `pids.toml` for our own
//! manufacturer id.

use crate::backend::BackendError;
use crate::protocol::{
    RdmCommandClass, RdmDataTypeName, RdmDefaultSlotValue, RdmDeviceInfo, RdmParameterDescription,
    RdmPersonality, RdmPersonalityState, RdmSlotDescription, RdmSlotInfo, RdmStatusMessage,
    RdmStatusType, RdmUid, RdmValue,
};
use pixelmapper_net::pids;
use pixelmapper_net::rdm::pd::{
    self, DefaultSlotValue, DeviceInfo, DmxPersonality, ParameterDescription,
    PersonalityDescription, SlotDescription, SlotInfo, StatusMessage,
};
use pixelmapper_net::rdm::{pid, status_type};
use std::collections::HashMap;

/// Manufacturer-specific PID range (E1.20 Table A-?, 0x8000–0xFFDF).
const MANUFACTURER_PIDS: std::ops::RangeInclusive<u16> = 0x8000..=0xFFDF;
/// Manufacturer-specific status message ids (E1.20 Table B-2: 0x8000–0xFFDF).
const MANUFACTURER_STATUS_IDS: std::ops::RangeInclusive<u16> = 0x8000..=0xFFDF;

// E1.20 PARAMETER_DESCRIPTION data types (Table A-15) handled as single values.
const DS_BIT_FIELD: u8 = 0x01;
const DS_ASCII: u8 = 0x02;
const DS_UNSIGNED_BYTE: u8 = 0x03;
const DS_SIGNED_BYTE: u8 = 0x04;
const DS_UNSIGNED_WORD: u8 = 0x05;
const DS_SIGNED_WORD: u8 = 0x06;
const DS_UNSIGNED_DWORD: u8 = 0x07;
const DS_SIGNED_DWORD: u8 = 0x08;
/// E1.20-2025 `DS_BOOLEAN`.
const DS_BOOLEAN: u8 = 0x0D;

// ── Description cache ─────────────────────────────────────────────────────────────────────────────────────────────

/// PARAMETER_DESCRIPTIONs the bridge has read, per device.
#[derive(Debug, Default)]
pub(super) struct Descriptions {
    by_device: HashMap<RdmUid, HashMap<u16, ParameterDescription>>,
}

impl Descriptions {
    /// Remembers a description read from `uid`.
    pub(super) fn insert(&mut self, uid: RdmUid, d: ParameterDescription) {
        self.by_device.entry(uid).or_default().insert(d.pid, d);
    }

    /// The description of `pid` at `uid`: read from the device, else from `pids.toml` when `uid` is one of ours.
    pub(super) fn get(&self, uid: RdmUid, pid: u16) -> Option<ParameterDescription> {
        if let Some(d) = self.by_device.get(&uid).and_then(|m| m.get(&pid)) {
            return Some(d.clone());
        }
        generated_description(uid, pid)
    }
}

/// PARAMETER_DESCRIPTION of one of our manufacturer PIDs, generated from `pids.toml`.
fn generated_description(uid: RdmUid, p: u16) -> Option<ParameterDescription> {
    if uid.manufacturer != pids::ESTA_MANUFACTURER {
        return None;
    }
    pids::PIDS
        .iter()
        .find(|d| d.pid == p)
        .and_then(ParameterDescription::from_pid_def)
}

/// Our manufacturer PIDs whose single value field is a `bool` in `pids.toml` (reported as `boolean`, contract §4;
/// field layouts from the generated `PID_META`).
fn is_bool_pid(uid: RdmUid, p: u16) -> bool {
    uid.manufacturer == pids::ESTA_MANUFACTURER && pids::pid_meta(p).is_some_and(|m| m.is_bool())
}

// ── Decoding ──────────────────────────────────────────────────────────────────────────────────────────────────────

/// Decodes the parameter data of a GET response (or a non-empty SET response) of `pid` from device `uid`;
/// `None` when the PID has no decoded form or the data does not fit it (`data` is returned anyway).
pub(super) fn decode(
    p: u16,
    data: &[u8],
    uid: RdmUid,
    desc: Option<&ParameterDescription>,
) -> Option<RdmValue> {
    let value = match p {
        pid::DEVICE_INFO => {
            RdmValue::DeviceInfo(Box::new(device_info(&DeviceInfo::decode(data).ok()?)))
        }
        pid::SUPPORTED_PARAMETERS => RdmValue::Integers(
            pd::decode_pid_list(data)
                .ok()?
                .into_iter()
                .map(i64::from)
                .collect(),
        ),
        pid::PARAMETER_DESCRIPTION => RdmValue::ParameterDescription(Box::new(
            parameter_description(&ParameterDescription::decode(data).ok()?),
        )),
        pid::DEVICE_MODEL_DESCRIPTION
        | pid::MANUFACTURER_LABEL
        | pid::DEVICE_LABEL
        | pid::SOFTWARE_VERSION_LABEL
        | pid::BOOT_SOFTWARE_VERSION_LABEL => RdmValue::Text(pd::decode_label(data)),
        pid::BOOT_SOFTWARE_VERSION_ID => RdmValue::Integer(pd::decode_u32(data).ok()?.into()),
        pid::DMX_PERSONALITY => {
            let d = DmxPersonality::decode(data).ok()?;
            RdmValue::PersonalityState(RdmPersonalityState {
                current: d.current,
                count: d.count,
            })
        }
        pid::DMX_PERSONALITY_DESCRIPTION => {
            RdmValue::Personality(personality(&PersonalityDescription::decode(data).ok()?))
        }
        pid::DMX_START_ADDRESS => RdmValue::Integer(pd::decode_u16(data).ok()?.into()),
        pid::SLOT_INFO => RdmValue::SlotInfo(
            SlotInfo::decode_list(data)
                .ok()?
                .iter()
                .map(|s| RdmSlotInfo {
                    offset: s.slot_offset,
                    slot_type: s.slot_type,
                    label_id: s.slot_label_id,
                })
                .collect(),
        ),
        pid::SLOT_DESCRIPTION => {
            let d = SlotDescription::decode(data).ok()?;
            RdmValue::SlotDescription(RdmSlotDescription {
                offset: d.slot,
                description: d.description,
            })
        }
        pid::DEFAULT_SLOT_VALUE => RdmValue::DefaultSlotValues(
            DefaultSlotValue::decode_list(data)
                .ok()?
                .iter()
                .map(|d| RdmDefaultSlotValue {
                    offset: d.slot_offset,
                    value: d.value,
                })
                .collect(),
        ),
        pid::IDENTIFY_DEVICE => RdmValue::Bool(pd::decode_u8(data).ok()? != 0),
        pid::STATUS_MESSAGES => RdmValue::StatusMessages(
            StatusMessage::decode_list(data)
                .ok()?
                .iter()
                .map(|m| status_message(m, uid))
                .collect(),
        ),
        p if MANUFACTURER_PIDS.contains(&p) => decode_single(desc?, data, is_bool_pid(uid, p))?,
        _ => return None,
    };
    Some(value)
}

/// A single-value manufacturer parameter.
fn decode_single(d: &ParameterDescription, data: &[u8], boolean: bool) -> Option<RdmValue> {
    match d.data_type {
        DS_ASCII => Some(RdmValue::Text(pd::decode_label(data))),
        DS_BOOLEAN => Some(RdmValue::Bool(pd::decode_u8(data).ok()? != 0)),
        t => {
            let v = decode_int(t, data)?;
            Some(match v {
                0 | 1 if boolean => RdmValue::Bool(v == 1),
                v => RdmValue::Integer(v),
            })
        }
    }
}

/// Size in bytes of an integer data type (`DS_BIT_FIELD` takes the length of the data: 1, 2 or 4).
fn int_size(data_type: u8, len: usize) -> Option<usize> {
    match data_type {
        DS_UNSIGNED_BYTE | DS_SIGNED_BYTE => Some(1),
        DS_UNSIGNED_WORD | DS_SIGNED_WORD => Some(2),
        DS_UNSIGNED_DWORD | DS_SIGNED_DWORD => Some(4),
        DS_BIT_FIELD if matches!(len, 1 | 2 | 4) => Some(len),
        _ => None,
    }
}

/// Big-endian integer of an integer data type (raw, unscaled).
fn decode_int(data_type: u8, data: &[u8]) -> Option<i64> {
    if int_size(data_type, data.len())? != data.len() {
        return None;
    }
    Some(match data_type {
        DS_SIGNED_BYTE => i64::from(pd::decode_u8(data).ok()? as i8),
        DS_SIGNED_WORD => i64::from(pd::decode_u16(data).ok()? as i16),
        DS_SIGNED_DWORD => i64::from(pd::decode_u32(data).ok()? as i32),
        _ => match data.len() {
            1 => i64::from(pd::decode_u8(data).ok()?),
            2 => i64::from(pd::decode_u16(data).ok()?),
            _ => i64::from(pd::decode_u32(data).ok()?),
        },
    })
}

// ── Encoding ──────────────────────────────────────────────────────────────────────────────────────────────────────

/// Encodes a decoded `value` into parameter data: the GET argument (`set == false`) or the SET value of `pid`.
/// Fails with `InvalidArgument` (`field: "value"`) when the PID takes no decoded value or the value does not fit.
pub(super) fn encode(
    p: u16,
    set: bool,
    value: &RdmValue,
    desc: Option<&ParameterDescription>,
) -> Result<Vec<u8>, BackendError> {
    match (set, p) {
        (false, pid::PARAMETER_DESCRIPTION) => Ok(uint(value, 0, 0xFFFF)?.to_be_bytes()[6..].to_vec()),
        (false, pid::DMX_PERSONALITY_DESCRIPTION) => Ok(vec![uint(value, 1, 255)? as u8]),
        (false, pid::SLOT_DESCRIPTION) => Ok(uint(value, 0, 0xFFFF)?.to_be_bytes()[6..].to_vec()),
        (false, pid::STATUS_MESSAGES) => match value {
            RdmValue::Text(t) => match t.as_str() {
                "none" => Ok(vec![status_type::NONE]),
                "advisory" => Ok(vec![status_type::ADVISORY]),
                "warning" => Ok(vec![status_type::WARNING]),
                "error" => Ok(vec![status_type::ERROR]),
                _ => Err(invalid("status type must be 'advisory', 'warning', 'error' or 'none'")),
            },
            _ => Err(invalid("STATUS_MESSAGES takes a status type string")),
        },
        (true, pid::DEVICE_LABEL) => match value {
            RdmValue::Text(t) => ascii(t, pd::MAX_LABEL),
            _ => Err(invalid("DEVICE_LABEL takes a string")),
        },
        (true, pid::DMX_PERSONALITY) => Ok(vec![uint(value, 1, 255)? as u8]),
        (true, pid::DMX_START_ADDRESS) => Ok((uint(value, 1, 512)? as u16).to_be_bytes().to_vec()),
        (true, pid::IDENTIFY_DEVICE) => Ok(vec![u8::from(boolean(value)?)]),
        (true, p) if MANUFACTURER_PIDS.contains(&p) => match desc {
            Some(d) => encode_single(d, value),
            None => Err(invalid(&format!(
                "no PARAMETER_DESCRIPTION known for PID 0x{p:04X} at this device (run rdmDescribe, or send data)"
            ))),
        },
        (set, p) => Err(invalid(&format!(
            "PID 0x{p:04X} takes no decoded {}; send data (hex)",
            if set { "SET value" } else { "GET argument" }
        ))),
    }
}

/// A single-value manufacturer parameter (SET value = the GET result's type).
fn encode_single(d: &ParameterDescription, value: &RdmValue) -> Result<Vec<u8>, BackendError> {
    match d.data_type {
        DS_ASCII => match value {
            RdmValue::Text(t) => {
                let max = if d.pdl_size == 0 {
                    pd::MAX_LABEL
                } else {
                    usize::from(d.pdl_size).min(pd::MAX_LABEL)
                };
                ascii(t, max)
            }
            _ => Err(invalid("this parameter takes a string")),
        },
        DS_BOOLEAN => Ok(vec![u8::from(boolean(value)?)]),
        t => {
            let size = match t {
                DS_BIT_FIELD => match d.pdl_size {
                    1 | 2 | 4 => usize::from(d.pdl_size),
                    _ => return Err(invalid("bit field of unknown size; send data")),
                },
                t => int_size(t, 0).ok_or_else(|| {
                    invalid(&format!(
                        "PID 0x{:04X} is a structured parameter; send data (hex)",
                        d.pid
                    ))
                })?,
            };
            let signed = matches!(t, DS_SIGNED_BYTE | DS_SIGNED_WORD | DS_SIGNED_DWORD);
            let bits = 8 * size as u32;
            let (min, max) = if signed {
                (-(1i64 << (bits - 1)), (1i64 << (bits - 1)) - 1)
            } else {
                (0, (1i64 << bits) - 1)
            };
            let v = integer(value)?;
            if v < min || v > max {
                return Err(invalid(&format!("value {v} is out of range {min}…{max}")));
            }
            Ok(v.to_be_bytes()[8 - size..].to_vec())
        }
    }
}

/// `value` as an integer (`true`/`false` count as 1/0).
fn integer(value: &RdmValue) -> Result<i64, BackendError> {
    match value {
        RdmValue::Integer(v) => Ok(*v),
        RdmValue::Bool(b) => Ok(i64::from(*b)),
        _ => Err(invalid("this parameter takes a number")),
    }
}

/// `value` as an unsigned integer in `min..=max`.
fn uint(value: &RdmValue, min: i64, max: i64) -> Result<i64, BackendError> {
    let v = integer(value)?;
    if (min..=max).contains(&v) {
        Ok(v)
    } else {
        Err(invalid(&format!("value {v} is out of range {min}…{max}")))
    }
}

/// `value` as a boolean (numbers 0/1 are accepted).
fn boolean(value: &RdmValue) -> Result<bool, BackendError> {
    match value {
        RdmValue::Bool(b) => Ok(*b),
        RdmValue::Integer(0) => Ok(false),
        RdmValue::Integer(1) => Ok(true),
        _ => Err(invalid("this parameter takes a boolean")),
    }
}

/// An ASCII text of at most `max` bytes.
fn ascii(text: &str, max: usize) -> Result<Vec<u8>, BackendError> {
    if !text.is_ascii() || text.contains('\0') {
        return Err(invalid("text must be printable ASCII"));
    }
    if text.len() > max {
        return Err(invalid(&format!("text is longer than {max} characters")));
    }
    Ok(pd::encode_label(text))
}

/// `badRequest` on `value`.
fn invalid(message: &str) -> BackendError {
    BackendError::InvalidArgument {
        field: "value".into(),
        message: message.to_owned(),
    }
}

// ── Contract shapes ───────────────────────────────────────────────────────────────────────────────────────────────

/// DEVICE_INFO.
pub(super) fn device_info(d: &DeviceInfo) -> RdmDeviceInfo {
    RdmDeviceInfo {
        rdm_protocol_version: d.rdm_protocol_version,
        device_model_id: d.device_model_id,
        product_category: d.product_category,
        software_version_id: d.software_version_id,
        dmx_footprint: d.dmx_footprint,
        current_personality: d.current_personality,
        personality_count: d.personality_count,
        dmx_start_address: d.dmx_start_address,
        sub_device_count: d.sub_device_count,
        sensor_count: d.sensor_count,
    }
}

/// DMX_PERSONALITY_DESCRIPTION.
pub(super) fn personality(d: &PersonalityDescription) -> RdmPersonality {
    RdmPersonality {
        index: d.personality,
        footprint: d.slots_required,
        description: Some(d.description.clone()),
    }
}

/// PARAMETER_DESCRIPTION; min/max/default are int32 for signed data types, uint32 otherwise.
pub(super) fn parameter_description(d: &ParameterDescription) -> RdmParameterDescription {
    let signed = matches!(
        d.data_type,
        DS_SIGNED_BYTE | DS_SIGNED_WORD | DS_SIGNED_DWORD
    );
    let number = |v: u32| {
        if signed {
            i64::from(v as i32)
        } else {
            i64::from(v)
        }
    };
    RdmParameterDescription {
        pid: d.pid,
        pdl_size: d.pdl_size,
        data_type: d.data_type,
        data_type_name: match d.data_type {
            DS_BIT_FIELD => RdmDataTypeName::BitField,
            DS_ASCII => RdmDataTypeName::Ascii,
            DS_UNSIGNED_BYTE => RdmDataTypeName::Uint8,
            DS_SIGNED_BYTE => RdmDataTypeName::Int8,
            DS_UNSIGNED_WORD => RdmDataTypeName::Uint16,
            DS_SIGNED_WORD => RdmDataTypeName::Int16,
            DS_UNSIGNED_DWORD => RdmDataTypeName::Uint32,
            DS_SIGNED_DWORD => RdmDataTypeName::Int32,
            0x80..=0xDF => RdmDataTypeName::Manufacturer,
            _ => RdmDataTypeName::NotDefined,
        },
        command_class: match d.command_class {
            0x02 => RdmCommandClass::Set,
            0x03 => RdmCommandClass::GetSet,
            _ => RdmCommandClass::Get,
        },
        param_type: d.param_type,
        unit: d.unit,
        prefix: d.prefix,
        min_value: number(d.min_valid_value),
        max_value: number(d.max_valid_value),
        default_value: number(d.default_value),
        description: d.description.clone(),
    }
}

/// One STATUS_MESSAGES entry. `text` (with the data values filled in) from the library's status texts: E1.20
/// Table B-2 for standard ids of any device, the STATUS_ID_DESCRIPTION of `pids.toml` for manufacturer ids of our
/// own devices (another manufacturer's ids mean something else: `null`).
pub(super) fn status_message(m: &StatusMessage, uid: RdmUid) -> RdmStatusMessage {
    let ours = uid.manufacturer == pids::ESTA_MANUFACTURER;
    let text = (ours || !MANUFACTURER_STATUS_IDS.contains(&m.status_message_id))
        .then(|| m.text())
        .flatten();
    RdmStatusMessage {
        sub_device: m.sub_device,
        status_type: match m.status_type {
            status_type::ADVISORY => RdmStatusType::Advisory,
            status_type::WARNING => RdmStatusType::Warning,
            status_type::ERROR => RdmStatusType::Error,
            status_type::ADVISORY_CLEARED => RdmStatusType::AdvisoryCleared,
            status_type::WARNING_CLEARED => RdmStatusType::WarningCleared,
            status_type::ERROR_CLEARED => RdmStatusType::ErrorCleared,
            _ => RdmStatusType::None,
        },
        status_message_id: m.status_message_id,
        data_value1: m.data_value1,
        data_value2: m.data_value2,
        text,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const OURS: RdmUid = RdmUid::new(pids::ESTA_MANUFACTURER, 0x8012_3456);
    const OTHER: RdmUid = RdmUid::new(0x4142, 1);

    fn desc(p: u16, data_type: u8, pdl_size: u8) -> ParameterDescription {
        ParameterDescription {
            pid: p,
            pdl_size,
            data_type,
            command_class: 0x03,
            param_type: 0,
            unit: 0,
            prefix: 0,
            min_valid_value: 0,
            max_valid_value: 0xFFFF_FFFF,
            default_value: 0xFFFF_FFFE,
            description: "x".into(),
        }
    }

    #[test]
    fn standard_pids_decode() {
        let di = DeviceInfo {
            rdm_protocol_version: 0x0100,
            device_model_id: 1,
            product_category: 0x0101,
            software_version_id: 0x0100,
            dmx_footprint: 30,
            current_personality: 1,
            personality_count: 2,
            dmx_start_address: 7,
            sub_device_count: 0,
            sensor_count: 0,
        };
        let v = decode(pid::DEVICE_INFO, &di.encode(), OTHER, None).unwrap();
        assert_eq!(serde_json::to_value(&v).unwrap()["dmxStartAddress"], 7);
        assert_eq!(
            decode(pid::DMX_START_ADDRESS, &[0, 42], OTHER, None),
            Some(RdmValue::Integer(42))
        );
        assert_eq!(
            decode(pid::IDENTIFY_DEVICE, &[1], OTHER, None),
            Some(RdmValue::Bool(true))
        );
        assert_eq!(
            decode(pid::DEVICE_LABEL, b"Tube 1", OTHER, None),
            Some(RdmValue::Text("Tube 1".into()))
        );
        assert_eq!(
            decode(
                pid::SUPPORTED_PARAMETERS,
                &[0x00, 0x82, 0x80, 0x00],
                OTHER,
                None
            ),
            Some(RdmValue::Integers(vec![0x82, 0x8000]))
        );
        assert_eq!(
            serde_json::to_value(decode(pid::DMX_PERSONALITY, &[2, 3], OTHER, None)).unwrap(),
            serde_json::json!({"current": 2, "count": 3})
        );
        let slots = decode(pid::SLOT_INFO, &[0, 0, 0, 0, 1, 0, 1, 1, 0, 0], OTHER, None);
        assert_eq!(
            serde_json::to_value(slots).unwrap(),
            serde_json::json!([{"offset":0,"type":0,"labelId":1},{"offset":1,"type":1,"labelId":0}])
        );
        assert_eq!(
            decode(pid::DEVICE_INFO, &[1, 2], OTHER, None),
            None,
            "bad length"
        );
        assert_eq!(decode(0x0901, &[1], OTHER, None), None, "unknown PID");
    }

    #[test]
    fn status_messages_with_our_texts() {
        let m = StatusMessage {
            sub_device: 0,
            status_type: status_type::ADVISORY,
            status_message_id: pids::status::LOCAL_RDM_SET,
            data_value1: 3,
            data_value2: 0,
        };
        let v = decode(pid::STATUS_MESSAGES, &m.encode(), OURS, None).unwrap();
        let RdmValue::StatusMessages(list) = v else {
            panic!("{v:?}")
        };
        assert_eq!(list[0].status_type, RdmStatusType::Advisory);
        assert_eq!(list[0].text.as_deref(), Some("Port 3: RDM SET from panel"));
        let RdmValue::StatusMessages(list) =
            decode(pid::STATUS_MESSAGES, &m.encode(), OTHER, None).unwrap()
        else {
            panic!()
        };
        assert_eq!(list[0].text, None, "manufacturer ids of other vendors");
        // Standard ids (E1.20 Table B-2) for every device.
        let overtemp = StatusMessage {
            status_type: status_type::WARNING,
            status_message_id: 0x0021,
            data_value1: 2,
            data_value2: 71,
            ..m
        };
        let RdmValue::StatusMessages(list) =
            decode(pid::STATUS_MESSAGES, &overtemp.encode(), OTHER, None).unwrap()
        else {
            panic!()
        };
        assert_eq!(
            list[0].text.as_deref(),
            Some("Sensor 2 over temp at 71 degrees C")
        );
        let unknown = StatusMessage {
            status_message_id: 0x0099,
            ..m
        };
        assert_eq!(status_message(&unknown, OURS).text, None);
    }

    #[test]
    fn bool_pids_from_pid_meta() {
        // The generated field layouts give exactly the bool PIDs of pids.toml.
        let mut bools: Vec<u16> = pids::PID_META
            .iter()
            .map(|m| m.pid)
            .filter(|&p| is_bool_pid(OURS, p))
            .collect();
        bools.sort();
        let mut expected: Vec<u16> = (pids::PID_PORT1_ENABLED..=pids::PID_PORT4_ENABLED)
            .chain(pids::PID_PORT1_RDM_BG_DISCOVERY..=pids::PID_PORT4_RDM_BG_DISCOVERY)
            .chain([pids::PID_PANEL_LOCK])
            .collect();
        expected.sort();
        assert_eq!(bools, expected);
        assert!(!is_bool_pid(OTHER, pids::PID_PANEL_LOCK), "ours only");
    }

    #[test]
    fn manufacturer_pids_from_descriptions() {
        let cache = Descriptions::default();
        // pids.toml: PORT1_ENABLED is a bool field, PORT1_DMX_BREAK a uint16.
        let enabled = cache.get(OURS, pids::PID_PORT1_ENABLED);
        assert_eq!(
            decode(pids::PID_PORT1_ENABLED, &[1], OURS, enabled.as_ref()),
            Some(RdmValue::Bool(true))
        );
        let brk = cache.get(OURS, pids::PID_PORT1_DMX_BREAK).unwrap();
        assert_eq!(
            decode(pids::PID_PORT1_DMX_BREAK, &[0, 176], OURS, Some(&brk)),
            Some(RdmValue::Integer(176))
        );
        assert_eq!(
            encode(
                pids::PID_PORT1_DMX_BREAK,
                true,
                &RdmValue::Integer(200),
                Some(&brk)
            )
            .unwrap(),
            vec![0, 200]
        );
        assert_eq!(
            encode(
                pids::PID_PORT1_ENABLED,
                true,
                &RdmValue::Bool(false),
                enabled.as_ref()
            )
            .unwrap(),
            vec![0]
        );
        // Structured PIDs only take data.
        let lrs = cache.get(OURS, pids::PID_LOCAL_RDM_SET);
        assert_eq!(
            decode(pids::PID_LOCAL_RDM_SET, &[0; 13], OURS, lrs.as_ref()),
            None
        );
        assert!(
            cache.get(OTHER, pids::PID_PORT1_ENABLED).is_none(),
            "pids.toml is ours only"
        );
        // A device's own description.
        let mut cache = Descriptions::default();
        cache.insert(OTHER, desc(0x8001, DS_SIGNED_WORD, 2));
        let d = cache.get(OTHER, 0x8001).unwrap();
        assert_eq!(
            decode(0x8001, &[0xFF, 0xFE], OTHER, Some(&d)),
            Some(RdmValue::Integer(-2))
        );
        assert_eq!(
            encode(0x8001, true, &RdmValue::Integer(-2), Some(&d)).unwrap(),
            vec![0xFF, 0xFE]
        );
        assert!(encode(0x8001, true, &RdmValue::Integer(40000), Some(&d)).is_err());
        let text = desc(0x8002, DS_ASCII, 8);
        assert_eq!(
            encode(0x8002, true, &RdmValue::Text("abc".into()), Some(&text)).unwrap(),
            b"abc".to_vec()
        );
        assert!(encode(
            0x8002,
            true,
            &RdmValue::Text("too long text".into()),
            Some(&text)
        )
        .is_err());
        assert!(encode(0x8003, true, &RdmValue::Integer(1), None).is_err());
    }

    #[test]
    fn standard_encodings() {
        assert_eq!(
            encode(pid::DMX_START_ADDRESS, true, &RdmValue::Integer(512), None).unwrap(),
            vec![2, 0]
        );
        assert!(encode(pid::DMX_START_ADDRESS, true, &RdmValue::Integer(0), None).is_err());
        assert_eq!(
            encode(pid::IDENTIFY_DEVICE, true, &RdmValue::Bool(true), None).unwrap(),
            vec![1]
        );
        assert_eq!(
            encode(
                pid::PARAMETER_DESCRIPTION,
                false,
                &RdmValue::Integer(0x8101),
                None
            )
            .unwrap(),
            vec![0x81, 0x01]
        );
        assert_eq!(
            encode(
                pid::DMX_PERSONALITY_DESCRIPTION,
                false,
                &RdmValue::Integer(2),
                None
            )
            .unwrap(),
            vec![2]
        );
        assert_eq!(
            encode(
                pid::STATUS_MESSAGES,
                false,
                &RdmValue::Text("warning".into()),
                None
            )
            .unwrap(),
            vec![status_type::WARNING]
        );
        assert_eq!(
            encode(pid::DEVICE_LABEL, true, &RdmValue::Text("Bar".into()), None).unwrap(),
            b"Bar".to_vec()
        );
        let e = encode(pid::DEVICE_INFO, false, &RdmValue::Integer(1), None).unwrap_err();
        assert!(matches!(e, BackendError::InvalidArgument { ref field, .. } if field == "value"));
    }

    #[test]
    fn parameter_description_shape() {
        let mut d = desc(0x8001, DS_SIGNED_DWORD, 4);
        d.min_valid_value = (-5i32) as u32;
        let p = parameter_description(&d);
        assert_eq!(p.data_type_name, RdmDataTypeName::Int32);
        assert_eq!(p.command_class, RdmCommandClass::GetSet);
        assert_eq!((p.min_value, p.max_value, p.default_value), (-5, -1, -2));
        let u = parameter_description(&desc(0x8001, DS_UNSIGNED_DWORD, 4));
        assert_eq!(u.max_value, 0xFFFF_FFFF);
    }
}
