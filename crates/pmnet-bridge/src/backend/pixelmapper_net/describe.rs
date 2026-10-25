//! `rdmDescribe` (contract §6 Semantics): everything the UI needs about one device, read through the
//! controller's RDM transport with the library's parameter-data codecs.
//!
//! Own reader instead of `Controller::describe_device`, because the contract needs per-PID error records
//! (`errors[]` with code and NACK reason), BOOT_SOFTWARE_VERSION_ID and the requested sub-device. Only a
//! DEVICE_INFO failure fails the command. Personalities and parameter descriptions are cached per
//! (`uid`, `subDevice`) and reused while `softwareVersionId` and SUPPORTED_PARAMETERS are unchanged.

use super::convert::backend_error;
use super::values::{device_info, parameter_description, personality};
use crate::backend::BackendError;
use crate::protocol::{
    CommandError, NodeId, PortRef, RdmDescribeError, RdmDeviceDescription, RdmPersonality, RdmSlot,
    RdmUid,
};
use pixelmapper_net::artnet::PortAddress;
use pixelmapper_net::link::RdmOptions;
use pixelmapper_net::rdm::pd::{
    self, DefaultSlotValue, DeviceInfo, ParameterDescription, PersonalityDescription,
    SlotDescription, SlotInfo,
};
use pixelmapper_net::rdm::{cc, pid, Uid};
use pixelmapper_net::{Controller, RdmRequest};

/// Static parts of a description, reusable while the firmware and the PID list stay the same.
#[derive(Debug, Clone)]
pub(super) struct Cached {
    software_version_id: u32,
    supported: Vec<u16>,
    personalities: Vec<RdmPersonality>,
    /// Raw descriptions (also fed to the value decoder).
    pub(super) parameter_descriptions: Vec<ParameterDescription>,
}

/// One device behind one node port.
pub(super) struct Target<'a> {
    /// Controller.
    pub(super) ctrl: &'a Controller,
    /// Library id of the node.
    pub(super) node: pixelmapper_net::NodeId,
    /// Port-Address of the port.
    pub(super) pa: PortAddress,
    /// Device.
    pub(super) uid: RdmUid,
    /// Sub-device.
    pub(super) sub_device: u16,
    /// Contract ids of node and port (for errors).
    pub(super) node_id: &'a NodeId,
    /// Contract port.
    pub(super) port: PortRef,
}

impl Target<'_> {
    /// GET `p` with `data`.
    pub(super) async fn get(&self, p: u16, data: &[u8]) -> Result<Vec<u8>, BackendError> {
        let req = RdmRequest {
            uid: Uid::new(self.uid.manufacturer, self.uid.device),
            command_class: cc::GET,
            pid: p,
            data: data.to_vec(),
            options: RdmOptions {
                sub_device: self.sub_device,
                ..RdmOptions::default()
            },
        };
        self.ctrl
            .rdm(&self.node, self.pa, req)
            .await
            .map_err(|e| backend_error(e, self.node_id, Some(self.port)))
    }
}

/// Collects failures of optional reads.
struct Reader<'a, 'b> {
    target: &'a Target<'b>,
    errors: Vec<RdmDescribeError>,
}

impl Reader<'_, '_> {
    /// GET `p`; a failure is recorded and gives `None`.
    async fn get(&mut self, p: u16, data: &[u8]) -> Option<Vec<u8>> {
        match self.target.get(p, data).await {
            Ok(d) => Some(d),
            Err(e) => {
                self.fail(p, e);
                None
            }
        }
    }

    /// GET `p` and decode it; a failure of either is recorded.
    async fn read<T>(
        &mut self,
        p: u16,
        data: &[u8],
        decode: impl FnOnce(&[u8]) -> Result<T, pixelmapper_net::rdm::RdmError>,
    ) -> Option<T> {
        let raw = self.get(p, data).await?;
        match decode(&raw) {
            Ok(v) => Some(v),
            Err(e) => {
                self.fail(p, BackendError::BadResponse(e.to_string()));
                None
            }
        }
    }

    /// A label.
    async fn label(&mut self, p: u16) -> Option<String> {
        self.get(p, &[]).await.map(|d| pd::decode_label(&d))
    }

    /// Records a failed PID.
    fn fail(&mut self, p: u16, e: BackendError) {
        let nack_reason = match e {
            BackendError::Nack(r) => Some(r),
            _ => None,
        };
        let c = CommandError::from(e);
        self.errors.push(RdmDescribeError {
            pid: p,
            code: c.code,
            message: c.message,
            nack_reason,
        });
    }
}

/// Reads the description of `t`. `cached` is the previous result for the same device, if any (ignored when
/// the firmware or PID list changed). Returns the description and the cache entry to keep.
pub(super) async fn describe(
    t: &Target<'_>,
    cached: Option<Cached>,
) -> Result<(RdmDeviceDescription, Cached), BackendError> {
    let raw = t.get(pid::DEVICE_INFO, &[]).await?;
    let info = DeviceInfo::decode(&raw).map_err(|e| BackendError::BadResponse(e.to_string()))?;
    let mut r = Reader {
        target: t,
        errors: Vec::new(),
    };
    let listed = r
        .read(pid::SUPPORTED_PARAMETERS, &[], pd::decode_pid_list)
        .await;
    // Without a PID list every optional PID is tried (failures end up in `errors`).
    let has = |p: u16| listed.as_ref().is_none_or(|s| s.contains(&p));
    let supported = listed.clone().unwrap_or_default();
    let cached = cached
        .filter(|c| c.software_version_id == info.software_version_id && c.supported == supported);

    let software_version_label = r.label(pid::SOFTWARE_VERSION_LABEL).await;
    let manufacturer_label = if has(pid::MANUFACTURER_LABEL) {
        r.label(pid::MANUFACTURER_LABEL).await
    } else {
        None
    };
    let device_model_description = if has(pid::DEVICE_MODEL_DESCRIPTION) {
        r.label(pid::DEVICE_MODEL_DESCRIPTION).await
    } else {
        None
    };
    let device_label = if has(pid::DEVICE_LABEL) {
        r.label(pid::DEVICE_LABEL).await
    } else {
        None
    };
    let boot_software_version_id = if has(pid::BOOT_SOFTWARE_VERSION_ID) {
        r.read(pid::BOOT_SOFTWARE_VERSION_ID, &[], pd::decode_u32)
            .await
    } else {
        None
    };
    let boot_software_version_label = if has(pid::BOOT_SOFTWARE_VERSION_LABEL) {
        r.label(pid::BOOT_SOFTWARE_VERSION_LABEL).await
    } else {
        None
    };

    let personalities = match &cached {
        Some(c) => c.personalities.clone(),
        None => read_personalities(&mut r, &info, has(pid::DMX_PERSONALITY_DESCRIPTION)).await,
    };

    let mut slots = Vec::new();
    if has(pid::SLOT_INFO) {
        if let Some(infos) = r.read(pid::SLOT_INFO, &[], SlotInfo::decode_list).await {
            let defaults = if has(pid::DEFAULT_SLOT_VALUE) {
                r.read(pid::DEFAULT_SLOT_VALUE, &[], DefaultSlotValue::decode_list)
                    .await
                    .unwrap_or_default()
            } else {
                Vec::new()
            };
            for s in infos {
                let description = if has(pid::SLOT_DESCRIPTION) {
                    r.read(
                        pid::SLOT_DESCRIPTION,
                        &s.slot_offset.to_be_bytes(),
                        SlotDescription::decode,
                    )
                    .await
                    .map(|d| d.description)
                } else {
                    None
                };
                slots.push(RdmSlot {
                    offset: s.slot_offset,
                    slot_type: s.slot_type,
                    label_id: s.slot_label_id,
                    description,
                    default_value: defaults
                        .iter()
                        .find(|d| d.slot_offset == s.slot_offset)
                        .map(|d| d.value),
                });
            }
        }
    }

    let parameter_descriptions = match &cached {
        Some(c) => c.parameter_descriptions.clone(),
        None => {
            let mut out = Vec::new();
            for &p in supported.iter().filter(|p| (0x8000..=0xFFDF).contains(*p)) {
                if let Some(d) = r
                    .read(
                        pid::PARAMETER_DESCRIPTION,
                        &p.to_be_bytes(),
                        ParameterDescription::decode,
                    )
                    .await
                {
                    out.push(d);
                }
            }
            out
        }
    };

    let description = RdmDeviceDescription {
        uid: t.uid,
        device_info: device_info(&info),
        manufacturer_label,
        device_model_description,
        device_label,
        software_version_label,
        boot_software_version_id,
        boot_software_version_label,
        personalities: personalities.clone(),
        slots,
        supported_parameters: supported.clone(),
        parameter_descriptions: parameter_descriptions
            .iter()
            .map(parameter_description)
            .collect(),
        errors: r.errors,
    };
    let keep = Cached {
        software_version_id: info.software_version_id,
        supported,
        personalities,
        parameter_descriptions,
    };
    Ok((description, keep))
}

/// DMX_PERSONALITY_DESCRIPTION 1…count. Without the PID (or for a failed read) the entry has no description;
/// the footprint is then only known for the current personality (DEVICE_INFO), else 0.
async fn read_personalities(
    r: &mut Reader<'_, '_>,
    info: &DeviceInfo,
    supported: bool,
) -> Vec<RdmPersonality> {
    let mut out = Vec::new();
    for n in 1..=info.personality_count {
        let read = if supported {
            r.read(
                pid::DMX_PERSONALITY_DESCRIPTION,
                &[n],
                PersonalityDescription::decode,
            )
            .await
        } else {
            None
        };
        out.push(match read {
            Some(d) => RdmPersonality {
                index: n,
                ..personality(&d)
            },
            None => RdmPersonality {
                index: n,
                footprint: if n == info.current_personality {
                    info.dmx_footprint
                } else {
                    0
                },
                description: None,
            },
        });
    }
    out
}
