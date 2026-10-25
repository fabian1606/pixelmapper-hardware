//! `describe_device`: everything Pixelmapper needs to build a generic settings page for one RDM device.

use super::Controller;
use crate::artnet::PortAddress;
use crate::error::{Error, Result};
use crate::rdm::pd::{
    decode_label, decode_pid_list, DefaultSlotValue, DeviceInfo, DmxPersonality,
    ParameterDescription, PersonalityDescription, SensorDefinition, SlotDescription, SlotInfo,
};
use crate::rdm::{pid, Uid};
use crate::state::NodeId;
use serde::Serialize;

/// One slot with its description.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SlotDescriptionEntry {
    /// SLOT_INFO entry.
    pub info: SlotInfo,
    /// SLOT_DESCRIPTION text, if supported.
    pub description: Option<String>,
    /// DEFAULT_SLOT_VALUE, if supported.
    pub default_value: Option<u8>,
}

/// One sensor.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SensorDescription {
    /// SENSOR_DEFINITION.
    pub definition: SensorDefinition,
}

/// Result of [`Controller::describe_device`]. Optional parts are `None`/empty when the device does not support
/// them; failures of optional reads are listed in `warnings` instead of failing the call.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceDescription {
    /// Device UID.
    pub uid: Uid,
    /// DEVICE_INFO.
    pub device_info: DeviceInfo,
    /// MANUFACTURER_LABEL.
    pub manufacturer_label: Option<String>,
    /// DEVICE_MODEL_DESCRIPTION.
    pub model_description: Option<String>,
    /// DEVICE_LABEL.
    pub device_label: Option<String>,
    /// SOFTWARE_VERSION_LABEL.
    pub software_version_label: Option<String>,
    /// BOOT_SOFTWARE_VERSION_LABEL.
    pub boot_software_version_label: Option<String>,
    /// SUPPORTED_PARAMETERS (empty if not supported).
    pub supported_parameters: Vec<u16>,
    /// DMX_PERSONALITY (current, count).
    pub personality: Option<DmxPersonality>,
    /// DMX_PERSONALITY_DESCRIPTION for 1..=count.
    pub personalities: Vec<PersonalityDescription>,
    /// SLOT_INFO with SLOT_DESCRIPTION and DEFAULT_SLOT_VALUE.
    pub slots: Vec<SlotDescriptionEntry>,
    /// SENSOR_DEFINITION for each sensor.
    pub sensors: Vec<SensorDescription>,
    /// PARAMETER_DESCRIPTION of every manufacturer PID (≥ 0x8000) in SUPPORTED_PARAMETERS.
    pub parameter_descriptions: Vec<ParameterDescription>,
    /// Optional reads that failed (`"PID_NAME: error"`).
    pub warnings: Vec<String>,
}

struct Reader<'a> {
    ctrl: &'a Controller,
    node: &'a NodeId,
    pa: PortAddress,
    uid: Uid,
    warnings: Vec<String>,
}

impl Reader<'_> {
    async fn get(&mut self, p: u16, data: &[u8]) -> Option<Vec<u8>> {
        match self
            .ctrl
            .rdm_get(self.node, self.pa, self.uid, p, data)
            .await
        {
            Ok(d) => Some(d),
            Err(e) => {
                let name = pid::name(p).unwrap_or("?");
                self.warnings.push(format!("{name} (0x{p:04X}): {e}"));
                None
            }
        }
    }

    async fn label(&mut self, p: u16) -> Option<String> {
        self.get(p, &[]).await.map(|d| decode_label(&d))
    }
}

pub(super) async fn describe(
    ctrl: &Controller,
    node: &NodeId,
    pa: PortAddress,
    uid: Uid,
) -> Result<DeviceDescription> {
    if uid.is_broadcast() {
        return Err(Error::Broadcast(uid));
    }
    let raw = ctrl.rdm_get(node, pa, uid, pid::DEVICE_INFO, &[]).await?;
    let device_info = DeviceInfo::decode(&raw)?;
    let mut r = Reader {
        ctrl,
        node,
        pa,
        uid,
        warnings: Vec::new(),
    };
    let supported = match r.get(pid::SUPPORTED_PARAMETERS, &[]).await {
        Some(d) => decode_pid_list(&d).unwrap_or_default(),
        None => Vec::new(),
    };
    let has = |p: u16| supported.contains(&p);
    let software_version_label = r.label(pid::SOFTWARE_VERSION_LABEL).await;
    let manufacturer_label = if has(pid::MANUFACTURER_LABEL) {
        r.label(pid::MANUFACTURER_LABEL).await
    } else {
        None
    };
    let model_description = if has(pid::DEVICE_MODEL_DESCRIPTION) {
        r.label(pid::DEVICE_MODEL_DESCRIPTION).await
    } else {
        None
    };
    let device_label = if has(pid::DEVICE_LABEL) {
        r.label(pid::DEVICE_LABEL).await
    } else {
        None
    };
    let boot_software_version_label = if has(pid::BOOT_SOFTWARE_VERSION_LABEL) {
        r.label(pid::BOOT_SOFTWARE_VERSION_LABEL).await
    } else {
        None
    };
    let personality = if has(pid::DMX_PERSONALITY) {
        r.get(pid::DMX_PERSONALITY, &[])
            .await
            .and_then(|d| DmxPersonality::decode(&d).ok())
    } else {
        None
    };
    let mut personalities = Vec::new();
    if has(pid::DMX_PERSONALITY_DESCRIPTION) {
        for n in 1..=device_info.personality_count {
            if let Some(d) = r.get(pid::DMX_PERSONALITY_DESCRIPTION, &[n]).await {
                match PersonalityDescription::decode(&d) {
                    Ok(p) => personalities.push(p),
                    Err(e) => r
                        .warnings
                        .push(format!("DMX_PERSONALITY_DESCRIPTION {n}: {e}")),
                }
            }
        }
    }
    let mut slots = Vec::new();
    if has(pid::SLOT_INFO) {
        let infos = match r.get(pid::SLOT_INFO, &[]).await {
            Some(d) => SlotInfo::decode_list(&d).unwrap_or_default(),
            None => Vec::new(),
        };
        let defaults = if has(pid::DEFAULT_SLOT_VALUE) {
            match r.get(pid::DEFAULT_SLOT_VALUE, &[]).await {
                Some(d) => DefaultSlotValue::decode_list(&d).unwrap_or_default(),
                None => Vec::new(),
            }
        } else {
            Vec::new()
        };
        for info in infos {
            let description = if has(pid::SLOT_DESCRIPTION) {
                r.get(pid::SLOT_DESCRIPTION, &info.slot_offset.to_be_bytes())
                    .await
                    .and_then(|d| SlotDescription::decode(&d).ok())
                    .map(|s| s.description)
            } else {
                None
            };
            let default_value = defaults
                .iter()
                .find(|d| d.slot_offset == info.slot_offset)
                .map(|d| d.value);
            slots.push(SlotDescriptionEntry {
                info,
                description,
                default_value,
            });
        }
    }
    let mut sensors = Vec::new();
    if has(pid::SENSOR_DEFINITION) {
        for s in 0..device_info.sensor_count {
            if let Some(d) = r.get(pid::SENSOR_DEFINITION, &[s]).await {
                if let Ok(definition) = SensorDefinition::decode(&d) {
                    sensors.push(SensorDescription { definition });
                }
            }
        }
    }
    let mut parameter_descriptions = Vec::new();
    for &p in supported.iter().filter(|&&p| p >= 0x8000) {
        if let Some(d) = r.get(pid::PARAMETER_DESCRIPTION, &p.to_be_bytes()).await {
            match ParameterDescription::decode(&d) {
                Ok(pd) => parameter_descriptions.push(pd),
                Err(e) => r
                    .warnings
                    .push(format!("PARAMETER_DESCRIPTION 0x{p:04X}: {e}")),
            }
        }
    }
    Ok(DeviceDescription {
        uid,
        device_info,
        manufacturer_label,
        model_description,
        device_label,
        software_version_label,
        boot_software_version_label,
        supported_parameters: supported,
        personality,
        personalities,
        slots,
        sensors,
        parameter_descriptions,
        warnings: r.warnings,
    })
}
