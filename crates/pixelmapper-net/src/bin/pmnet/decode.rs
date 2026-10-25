//! Decoded forms of known parameter data.

use crate::out::to_value;
use pixelmapper_net::pids::{self, PidDef};
use pixelmapper_net::rdm::pd::{
    self, BootCatchStatus, BootInfo, DeviceInfo, DmxPersonality, FwInfo, LocalRdmSet,
    ParameterDescription, StatusMessage,
};
use pixelmapper_net::rdm::pid;
use serde_json::{json, Value};

/// Manufacturer PID definition from `pids.toml`.
pub fn pid_def(p: u16) -> Option<&'static PidDef> {
    pids::PIDS.iter().find(|d| d.pid == p)
}

/// `NAME (0xPPPP)` or `0xPPPP`.
pub fn pid_label(p: u16) -> String {
    match pid::name(p) {
        Some(n) => format!("{n} (0x{p:04X})"),
        None => format!("0x{p:04X}"),
    }
}

fn pid_list(data: &[u8]) -> Option<Value> {
    let list = pd::decode_pid_list(data).ok()?;
    Some(Value::Array(
        list.iter()
            .map(|p| json!({"pid": p, "name": pid::name(*p)}))
            .collect(),
    ))
}

fn param_desc(data: &[u8]) -> Option<Value> {
    let d = ParameterDescription::decode(data).ok()?;
    let mut v = to_value(&d);
    if let Value::Object(m) = &mut v {
        m.insert("pidName".into(), json!(pid::name(d.pid)));
        m.insert(
            "dataTypeName".into(),
            json!(pd::data_type_name(d.data_type)),
        );
        m.insert("unitName".into(), json!(pd::unit_name(d.unit)));
        m.insert("prefixName".into(), json!(pd::prefix_name(d.prefix)));
    }
    Some(v)
}

fn number(data: &[u8]) -> Option<Value> {
    match data.len() {
        1 => Some(json!(data[0])),
        2 => Some(json!(u16::from_be_bytes([data[0], data[1]]))),
        4 => Some(json!(u32::from_be_bytes([
            data[0], data[1], data[2], data[3]
        ]))),
        _ => None,
    }
}

/// Decode the parameter data of a response to `p`; `None` when the PID is unknown or the data does not fit.
pub fn decode(p: u16, data: &[u8]) -> Option<Value> {
    if data.is_empty() {
        return None;
    }
    let v = match p {
        pid::DEVICE_LABEL
        | pid::MANUFACTURER_LABEL
        | pid::DEVICE_MODEL_DESCRIPTION
        | pid::SOFTWARE_VERSION_LABEL
        | pid::BOOT_SOFTWARE_VERSION_LABEL
        | pid::STATUS_ID_DESCRIPTION => json!(pd::decode_label(data)),
        pid::DEVICE_INFO => to_value(&DeviceInfo::decode(data).ok()?),
        pid::PARAMETER_DESCRIPTION => param_desc(data)?,
        pid::SUPPORTED_PARAMETERS => pid_list(data)?,
        pid::STATUS_MESSAGES => {
            let list = StatusMessage::decode_list(data).ok()?;
            Value::Array(list.iter().map(status_value).collect())
        }
        pid::DMX_PERSONALITY => to_value(&DmxPersonality::decode(data).ok()?),
        pid::DMX_START_ADDRESS
        | pid::IDENTIFY_DEVICE
        | pid::BOOT_SOFTWARE_VERSION_ID
        | pid::DISPLAY_LEVEL => number(data)?,
        pids::PID_BOOT_INFO => to_value(&BootInfo::decode(data).ok()?),
        pids::PID_FW_INFO => to_value(&FwInfo::decode(data).ok()?),
        pids::PID_BOOT_CATCH_STATUS => to_value(&BootCatchStatus::decode(data).ok()?),
        pids::PID_LOCAL_RDM_SET => to_value(&LocalRdmSet::decode(data).ok()?),
        _ => {
            let desc = pid_def(p)?.desc?;
            if desc.data_type == 0x02 {
                json!(pd::decode_label(data))
            } else {
                number(data)?
            }
        }
    };
    Some(v)
}

fn status_value(m: &StatusMessage) -> Value {
    let mut v = to_value(m);
    if let Value::Object(o) = &mut v {
        use pids::status as st;
        let name = match m.status_message_id {
            st::BOOT_CATCH_CAUGHT => Some("BOOT_CATCH_CAUGHT"),
            st::BOOT_CATCH_TIMEOUT => Some("BOOT_CATCH_TIMEOUT"),
            st::PANEL_UNLOCKED_LOCAL => Some("PANEL_UNLOCKED_LOCAL"),
            st::LOCAL_RDM_SET => Some("LOCAL_RDM_SET"),
            _ => None,
        };
        o.insert("statusName".into(), json!(name));
    }
    v
}
