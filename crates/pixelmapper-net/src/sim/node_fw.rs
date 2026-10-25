//! Firmware update of the simulated node, both ways (`protocol/image-format.md` §2.3–2.5):
//! RDM FW_BEGIN/FW_WRITE/FW_FINISH and Art-Net ArtFirmwareMaster with the `.alf` container.
//!
//! The simulation keeps the received image in memory and checks exactly what the real node checks before the
//! ESP-IDF image verification (which is not simulated): the description block copy, the copy inside the image
//! (bytes 288..352), length and CRC-32.

use crate::artnet::{ArtFirmwareMaster, ArtFirmwareReply};
use crate::crc::crc32;
use crate::image::node::{
    NodeImageDesc, ALF_DESC_OFFSET, ALF_HEADER_LEN, ALF_OEM_COUNT, ALF_OEM_OFFSET, DESC_LEN,
    DESC_OFFSET,
};
use crate::pids::{self, fw_error, fw_state};
use crate::rdm::pd::FwInfo;
use crate::rdm::Uid;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

/// Words of the `.alf` header (1060 bytes).
const ALF_HEADER_WORDS: u32 = (ALF_HEADER_LEN / 2) as u32;

/// Who drives the running session. Only one session at a time (image-format.md §2.5).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum FwSource {
    /// RDM FW_* from this controller UID.
    Rdm(Uid),
    /// ArtFirmwareMaster from this address.
    ArtNet(SocketAddr),
}

#[derive(Debug)]
struct Session {
    source: FwSource,
    desc: NodeImageDesc,
    data: Vec<u8>,
    desc_checked: bool,
    last_activity: Instant,
    /// ArtFirmwareMaster: BlockId expected next (mod 256).
    next_block: u8,
    /// ArtFirmwareMaster: `.alf` file bytes received so far.
    file_pos: usize,
}

/// What the node checks a description block against.
#[derive(Debug, Clone, Copy)]
pub(super) struct FwTarget {
    /// Own ESTA manufacturer.
    pub manufacturer: u16,
    /// Own hardware revision.
    pub hw_revision: u8,
    /// Size of the inactive OTA partition.
    pub partition_size: u32,
}

/// Why an RDM FW_* request is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum FwRefusal {
    /// NACK DATA_OUT_OF_RANGE (details in FW_INFO.last_error).
    OutOfRange,
    /// NACK BUFFER_FULL: another source owns the session.
    Busy,
}

/// A successfully finished update: the node reboots into this version.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct NewFirmware {
    pub fw_version: u32,
    pub version_label: String,
}

/// Firmware update state of the node (FW_INFO).
#[derive(Debug)]
pub(super) struct FwUpdate {
    pub state: u8,
    pub last_error: u8,
    pub received: u32,
    pub total: u32,
    /// An update failed since boot: NodeReport "Firmware update failed" (RcFirmwareFail), as pm_node does.
    pub failed_since_boot: bool,
    session: Option<Session>,
}

impl Default for FwUpdate {
    fn default() -> Self {
        Self {
            state: fw_state::IDLE,
            last_error: fw_error::NONE,
            received: 0,
            total: 0,
            failed_since_boot: false,
            session: None,
        }
    }
}

impl FwUpdate {
    /// FW_INFO response.
    pub fn info(&self, hw_revision: u8, fw_version: u32) -> FwInfo {
        FwInfo {
            manufacturer: pids::ESTA_MANUFACTURER,
            model: pids::MODEL_NODE,
            hw_revision,
            fw_version,
            state: self.state,
            last_error: self.last_error,
            max_block: u8::try_from(pids::NODE_FW_MAX_BLOCK).unwrap_or(u8::MAX),
            received: self.received,
            total: self.total,
        }
    }

    /// Source of the running session, if any.
    pub fn active_source(&self) -> Option<FwSource> {
        self.session.as_ref().map(|s| s.source)
    }

    fn fail(&mut self, code: u8) {
        self.state = fw_state::FAILED;
        self.last_error = code;
        self.failed_since_boot = true;
        self.session = None;
    }

    /// 30 s (configurable) without data → TIMEOUT, the old firmware stays.
    pub fn tick(&mut self, now: Instant, timeout: Duration) {
        if let Some(s) = &self.session {
            if now.saturating_duration_since(s.last_activity) >= timeout {
                self.fail(fw_error::TIMEOUT);
            }
        }
    }

    /// Steps 1–5 of image-format.md §2.3 and opening a session.
    fn open(
        &mut self,
        source: FwSource,
        desc_bytes: &[u8],
        target: FwTarget,
        now: Instant,
    ) -> Result<(), u8> {
        let desc = NodeImageDesc::decode(desc_bytes).map_err(|_| fw_error::DESC_MAGIC)?;
        desc.validate(
            target.manufacturer,
            target.hw_revision,
            target.partition_size,
        )?;
        self.state = fw_state::RECEIVING;
        self.last_error = fw_error::NONE;
        self.received = 0;
        self.total = desc.image_length;
        self.session = Some(Session {
            source,
            desc,
            data: Vec::new(),
            desc_checked: false,
            last_activity: now,
            next_block: 1,
            file_pos: 0,
        });
        Ok(())
    }

    /// Append image bytes and run step 6 (description block inside the image) once it is complete.
    fn append(&mut self, bytes: &[u8]) -> Result<(), u8> {
        let Some(s) = self.session.as_mut() else {
            return Err(fw_error::SEQUENCE);
        };
        let room = (s.desc.image_length as usize).saturating_sub(s.data.len());
        if bytes.len() > room {
            return Err(fw_error::TOO_LARGE);
        }
        s.data.extend_from_slice(bytes);
        if !s.desc_checked && s.data.len() >= DESC_OFFSET + DESC_LEN {
            s.desc_checked = true;
            if !s
                .desc
                .matches_image_block(&s.data[DESC_OFFSET..DESC_OFFSET + DESC_LEN])
            {
                return Err(fw_error::DESC_MISMATCH);
            }
        }
        self.received = u32::try_from(s.data.len()).unwrap_or(u32::MAX);
        Ok(())
    }

    /// Step 7: length and CRC-32. Ends the session either way.
    fn verify(&mut self) -> Result<NewFirmware, u8> {
        let Some(s) = self.session.take() else {
            return Err(fw_error::SEQUENCE);
        };
        if s.data.len() != s.desc.image_length as usize || crc32(&s.data) != s.desc.image_crc32 {
            return Err(fw_error::CRC);
        }
        self.state = fw_state::READY;
        Ok(NewFirmware {
            fw_version: s.desc.fw_version_id,
            version_label: s.desc.version_label,
        })
    }

    fn owned_by(&self, source: FwSource) -> bool {
        self.active_source() == Some(source)
    }

    /// RDM SET FW_BEGIN (PD = 64-byte description copy). Restarts a session of the same controller.
    pub fn rdm_begin(
        &mut self,
        src: Uid,
        pd: &[u8],
        target: FwTarget,
        now: Instant,
    ) -> Result<(), FwRefusal> {
        let source = FwSource::Rdm(src);
        if self.active_source().is_some_and(|s| s != source) {
            return Err(FwRefusal::Busy);
        }
        self.session = None;
        self.open(source, pd, target, now).map_err(|code| {
            self.fail(code);
            FwRefusal::OutOfRange
        })
    }

    /// RDM SET FW_WRITE: offset == received → append; already received → ACK (retransmission); gap → NACK.
    pub fn rdm_write(
        &mut self,
        src: Uid,
        offset: u32,
        data: &[u8],
        now: Instant,
    ) -> Result<(), FwRefusal> {
        let source = FwSource::Rdm(src);
        match self.active_source() {
            // no session: SEQUENCE without a state change (pm_ota_session: FW_INFO keeps its state)
            None => return Err(FwRefusal::OutOfRange),
            Some(s) if s != source => return Err(FwRefusal::Busy),
            Some(_) => {}
        }
        let received = self.received as usize;
        let offset = offset as usize;
        if offset > received {
            // gap: NACK, the session continues and FW_INFO is unchanged; the controller may resend from
            // `received` (pm_rdm_responder h_fw_write checks this before the sink)
            return Err(FwRefusal::OutOfRange);
        }
        if let Some(s) = self.session.as_mut() {
            s.last_activity = now;
        }
        let end = offset + data.len();
        if end <= received {
            return Ok(());
        }
        let new = &data[received - offset..];
        self.append(new).map_err(|code| {
            self.fail(code);
            FwRefusal::OutOfRange
        })
    }

    /// RDM SET FW_FINISH: verify; `Ok` → reboot into the new image.
    pub fn rdm_finish(&mut self, src: Uid) -> Result<NewFirmware, FwRefusal> {
        match self.active_source() {
            None => Err(FwRefusal::OutOfRange), // SEQUENCE without a state change, as pm_ota_session_finish
            Some(s) if s != FwSource::Rdm(src) => Err(FwRefusal::Busy),
            Some(_) => self.verify().map_err(|code| {
                self.fail(code);
                FwRefusal::OutOfRange
            }),
        }
    }

    /// One ArtFirmwareMaster packet → ArtFirmwareReply type, plus the new firmware after FirmLast/FirmAllGood.
    pub fn artnet_block(
        &mut self,
        src: SocketAddr,
        pkt: &ArtFirmwareMaster,
        target: FwTarget,
        now: Instant,
    ) -> (u8, Option<NewFirmware>) {
        let source = FwSource::ArtNet(src);
        let fail = ArtFirmwareReply::FIRM_FAIL;
        match pkt.kind {
            ArtFirmwareMaster::FIRM_FIRST => {
                if self.active_source().is_some_and(|s| s != source) {
                    return (fail, None);
                }
                self.session = None;
                match self.artnet_first(source, pkt, target, now) {
                    Ok(()) => (ArtFirmwareReply::FIRM_BLOCK_GOOD, None),
                    Err(code) => {
                        self.fail(code);
                        (fail, None)
                    }
                }
            }
            ArtFirmwareMaster::FIRM_CONT | ArtFirmwareMaster::FIRM_LAST => {
                if !self.owned_by(source) {
                    return (fail, None);
                }
                if let Err(code) = self.artnet_data(pkt, now) {
                    self.fail(code);
                    return (fail, None);
                }
                if pkt.kind == ArtFirmwareMaster::FIRM_CONT {
                    return (ArtFirmwareReply::FIRM_BLOCK_GOOD, None);
                }
                match self.verify() {
                    Ok(fw) => (ArtFirmwareReply::FIRM_ALL_GOOD, Some(fw)),
                    Err(code) => {
                        self.fail(code);
                        (fail, None)
                    }
                }
            }
            // UBEA files are not supported
            _ => (fail, None),
        }
    }

    fn artnet_first(
        &mut self,
        source: FwSource,
        pkt: &ArtFirmwareMaster,
        target: FwTarget,
        now: Instant,
    ) -> Result<(), u8> {
        let d = &pkt.data;
        let oem_listed = (0..ALF_OEM_COUNT).any(|i| {
            let o = ALF_OEM_OFFSET + 2 * i;
            d.get(o..o + 2)
                .is_some_and(|w| u16::from_be_bytes([w[0], w[1]]) == pids::ARTNET_OEM_NODE)
        });
        if !oem_listed {
            return Err(fw_error::OEM);
        }
        let desc = d
            .get(ALF_DESC_OFFSET..ALF_DESC_OFFSET + DESC_LEN)
            .ok_or(fw_error::DESC_MAGIC)?;
        // Like pm_artnet_node.c fw_first: FirmwareLength must match image_length of the copy, before step 1-5.
        let image_len = NodeImageDesc::decode(desc)
            .map_err(|_| fw_error::DESC_MAGIC)?
            .image_length;
        if pkt.firmware_length != ALF_HEADER_WORDS + image_len.div_ceil(2) {
            return Err(fw_error::DESC_MISMATCH);
        }
        self.open(source, desc, target, now)?;
        if let Some(s) = self.session.as_mut() {
            s.file_pos = pkt.data.len();
            s.next_block = pkt.block_id.wrapping_add(1);
        }
        Ok(())
    }

    fn artnet_data(&mut self, pkt: &ArtFirmwareMaster, now: Instant) -> Result<(), u8> {
        let Some(s) = self.session.as_mut() else {
            return Err(fw_error::SEQUENCE);
        };
        if pkt.block_id != s.next_block {
            return Err(fw_error::SEQUENCE);
        }
        s.next_block = s.next_block.wrapping_add(1);
        s.last_activity = now;
        let start = s.file_pos;
        s.file_pos += pkt.data.len();
        let image_len = s.desc.image_length as usize;
        // file offsets >= 1060 are image bytes; padding beyond image_length is dropped
        let skip = ALF_HEADER_LEN.saturating_sub(start).min(pkt.data.len());
        let first_image_byte = (start + skip).saturating_sub(ALF_HEADER_LEN);
        let take = image_len
            .saturating_sub(first_image_byte)
            .min(pkt.data.len() - skip);
        let bytes = pkt.data[skip..skip + take].to_vec();
        self.append(&bytes)
    }
}
