//! Texts of the standard RDM status message IDs (E1.20 Table B-2) and of our manufacturer IDs (`pids.toml`).
//!
//! Source of the standard texts: OLA `common/rdm/RDMHelper.cpp` (`StatusMessageIdToString`) and
//! `protocol/rdm-digest.md` (IDs from OLA `RDMEnums.h`). The ESTA text of E1.20 Table B-2 has not been checked
//! ([ungeprüft]); entries with `verified == false` come from later standards (E1.20-2010 additions, E1.37-5) or
//! OLA only and are less certain. Templates use `%1` / `%2` for data value 1 / 2.

use super::pd::StatusMessage;
use crate::pids;
use serde::Serialize;

/// One standard status message ID.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StandardStatus {
    /// Status message ID.
    pub id: u16,
    /// Name without the `STS_` prefix, e.g. `"OVERTEMP"`.
    pub name: &'static str,
    /// English text, `%1` / `%2` stand for data value 1 / 2.
    pub template: &'static str,
    /// `true`: in E1.20 Table B-2 as listed by OLA and the digest. `false`: less certain (later additions,
    /// wording or data meaning taken from OLA only).
    pub verified: bool,
}

const fn s(id: u16, name: &'static str, template: &'static str, verified: bool) -> StandardStatus {
    StandardStatus {
        id,
        name,
        template,
        verified,
    }
}

/// E1.20 Table B-2 (and later standard additions), sorted by ID.
pub static STANDARD_STATUS: &[StandardStatus] = &[
    s(0x0001, "CAL_FAIL", "Slot %1 failed calibration", true),
    s(0x0002, "SENS_NOT_FOUND", "Sensor %1 not found", true),
    s(0x0003, "SENS_ALWAYS_ON", "Sensor %1 always on", true),
    s(0x0004, "FEEDBACK_ERROR", "Slot %1 feedback error", true),
    s(0x0005, "INDEX_ERROR", "Slot %1 index circuit error", true),
    s(0x0011, "LAMP_DOUSED", "Lamp doused", true),
    s(0x0012, "LAMP_STRIKE", "Lamp failed to strike", true),
    s(0x0013, "LAMP_ACCESS_OPEN", "Lamp access open", true),
    s(0x0014, "LAMP_ALWAYS_ON", "Lamp on without command", true),
    s(
        0x0021,
        "OVERTEMP",
        "Sensor %1 over temp at %2 degrees C",
        true,
    ),
    s(
        0x0022,
        "UNDERTEMP",
        "Sensor %1 under temp at %2 degrees C",
        true,
    ),
    s(0x0023, "SENS_OUT_RANGE", "Sensor %1 out of range", true),
    s(
        0x0031,
        "OVERVOLTAGE_PHASE",
        "Phase %1 over voltage at %2V",
        true,
    ),
    s(
        0x0032,
        "UNDERVOLTAGE_PHASE",
        "Phase %1 under voltage at %2V",
        true,
    ),
    s(0x0033, "OVERCURRENT", "Phase %1 over current at %2A", true),
    s(
        0x0034,
        "UNDERCURRENT",
        "Phase %1 under current at %2A",
        true,
    ),
    s(0x0035, "PHASE", "Phase %1 is at %2 degrees", true),
    s(0x0036, "PHASE_ERROR", "Phase %1 error", true),
    s(0x0037, "AMPS", "%1 Amps", true),
    s(0x0038, "VOLTS", "%1 Volts", true),
    s(0x0041, "DIMSLOT_OCCUPIED", "No Dimmer", true),
    s(0x0042, "BREAKER_TRIP", "Tripped Breaker", true),
    s(0x0043, "WATTS", "%1 Watts", true),
    s(0x0044, "DIM_FAILURE", "Dimmer Failure", true),
    s(0x0045, "DIM_PANIC", "Dimmer panic mode", true),
    s(0x0046, "LOAD_FAILURE", "Lamp or cable failure", false),
    s(0x0050, "READY", "Slot %1 ready", true),
    s(0x0051, "NOT_READY", "Slot %1 not ready", true),
    s(0x0052, "LOW_FLUID", "Slot %1 low fluid", true),
    s(0x0060, "EEPROM_ERROR", "EEPROM error", false),
    s(0x0061, "RAM_ERROR", "RAM error", false),
    s(0x0062, "FPGA_ERROR", "FPGA programming error", false),
    s(
        0x0070,
        "PROXY_BROADCAST_DROPPED",
        "Proxy Drop: PID %1 at TN %2",
        false,
    ),
    s(0x0071, "ASC_RXOK", "DMX ASC %1 received OK", false),
    s(0x0072, "ASC_DROPPED", "DMX ASC %1 now dropped", false),
    s(0x0080, "DMXNSCNONE", "DMX NSC never received", false),
    s(0x0081, "DMXNSCLOSS", "DMX NSC received, now dropped", false),
    s(
        0x0082,
        "DMXNSCERROR",
        "DMX NSC timing or packet error",
        false,
    ),
    s(0x0083, "DMXNSC_OK", "DMX NSC received OK", false),
];

/// The standard status message with this ID.
pub fn standard_status(id: u16) -> Option<&'static StandardStatus> {
    STANDARD_STATUS.iter().find(|s| s.id == id)
}

/// Name of a status message ID: the standard name (without `STS_`) or our manufacturer name from `pids.toml`.
pub fn status_name(id: u16) -> Option<&'static str> {
    standard_status(id)
        .map(|s| s.name)
        .or_else(|| pids::status_name(id))
}

/// Human-readable text of a status message ID with its data values filled in: standard IDs from E1.20 Table B-2,
/// manufacturer IDs from `pids.toml` (their STATUS_ID_DESCRIPTION; each `%d` takes the next data value).
/// `None` for unknown IDs (another manufacturer's: ask the device with STATUS_ID_DESCRIPTION).
pub fn status_text(id: u16, data1: i16, data2: i16) -> Option<String> {
    if let Some(s) = standard_status(id) {
        return Some(
            s.template
                .replace("%1", &data1.to_string())
                .replace("%2", &data2.to_string()),
        );
    }
    let d = pids::status_description(id)?;
    let mut out = String::new();
    let mut values = [data1, data2].into_iter();
    let mut rest = d;
    while let Some(i) = rest.find("%d") {
        out.push_str(&rest[..i]);
        match values.next() {
            Some(v) => out.push_str(&v.to_string()),
            None => out.push_str("%d"),
        }
        rest = &rest[i + 2..];
    }
    out.push_str(rest);
    Some(out)
}

impl StatusMessage {
    /// [`status_text`] of this message.
    pub fn text(&self) -> Option<String> {
        status_text(self.status_message_id, self.data_value1, self.data_value2)
    }

    /// [`status_name`] of this message.
    pub fn name(&self) -> Option<&'static str> {
        status_name(self.status_message_id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn standard_texts() {
        assert_eq!(
            status_text(0x0021, 3, 85).as_deref(),
            Some("Sensor 3 over temp at 85 degrees C")
        );
        assert_eq!(status_text(0x0011, 0, 0).as_deref(), Some("Lamp doused"));
        assert_eq!(status_name(0x0050), Some("READY"));
        assert!(!standard_status(0x0080).unwrap().verified);
        assert_eq!(status_text(0x0099, 0, 0), None);
        // sorted, unique, no clash with the manufacturer range
        assert!(STANDARD_STATUS.windows(2).all(|w| w[0].id < w[1].id));
        assert!(STANDARD_STATUS.iter().all(|s| s.id < 0x8000));
    }

    #[test]
    fn manufacturer_texts() {
        assert_eq!(
            status_text(pids::status::BOOT_CATCH_CAUGHT, 2, -1).as_deref(),
            Some("Boot catch: port 2 caught")
        );
        assert_eq!(
            status_name(pids::status::LOCAL_RDM_SET),
            Some("LOCAL_RDM_SET")
        );
        let m = StatusMessage {
            sub_device: 0,
            status_type: crate::rdm::status_type::WARNING,
            status_message_id: 0x0022,
            data_value1: 1,
            data_value2: -5,
        };
        assert_eq!(
            m.text().as_deref(),
            Some("Sensor 1 under temp at -5 degrees C")
        );
        assert_eq!(m.name(), Some("UNDERTEMP"));
    }
}
