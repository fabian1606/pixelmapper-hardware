//! Node firmware update via ArtFirmwareMaster (`.alf`) or the FW_* PIDs (contract §9.2, image-format.md §2).
//!
//! Both methods first check the image against the node (FW_INFO, NODE_HW_REVISION, and for ArtFirmwareMaster the
//! OEM code from the ArtPollReply) and refuse before the first write; the node repeats these checks itself.
//! After the transfer the node reboots; the flow waits for the new version in its ArtPollReply and confirms it
//! via RDM DEVICE_INFO / SOFTWARE_VERSION_LABEL. A failed or cancelled transfer leaves the old firmware active
//! (the node drops the session after 30 s without data).
//!
//! ArtFirmwareMaster block types: the `.alf` file always has at least two 1024-byte blocks (the header alone is
//! 1060 bytes), so the transfer is always FirmFirst (block 0), FirmCont for the middle blocks and FirmLast for
//! the final one; there is no single-block case.

use super::tube::{is_busy, pause};
use super::{FlashPhase, FlashReport, Progress};
use crate::artnet::{ArtFirmwareMaster, ArtFirmwareReply};
use crate::error::{Error, FlashErrorKind, Result};
use crate::image::tube::version_str;
use crate::image::NodeImage;
use crate::link::{NodeLink, RdmOptions};
use crate::pids;
use crate::rdm::pd::{decode_label, decode_u8, DeviceInfo, FwInfo, FwWrite};
use crate::rdm::{cc, pid, Uid};
use std::time::Duration;

/// Transfer method.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum NodeFlashMethod {
    /// Weg A: ArtFirmwareMaster with the `.alf` file.
    ArtFirmwareMaster,
    /// Weg B: RDM FW_BEGIN / FW_WRITE / FW_FINISH to the node UID.
    Rdm,
}

/// Options of a node update.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NodeFlashOptions {
    /// Transfer method.
    pub method: NodeFlashMethod,
    /// Allow an image older than the running firmware.
    pub allow_downgrade: bool,
    /// Timeout for each ArtFirmwareReply (spec: 30 s).
    pub block_timeout: Duration,
    /// How long to wait for the node to come back with the new version.
    pub reboot_timeout: Duration,
    /// Pause before repeating an RDM request answered with NACK BUFFER_FULL.
    pub busy_delay: Duration,
    /// BUFFER_FULL answers per RDM request after which the request fails.
    pub busy_retries: u32,
}

impl Default for NodeFlashOptions {
    fn default() -> Self {
        Self {
            method: NodeFlashMethod::Rdm,
            allow_downgrade: false,
            block_timeout: Duration::from_secs(30),
            reboot_timeout: Duration::from_secs(90),
            busy_delay: Duration::from_millis(30),
            busy_retries: 100,
        }
    }
}

/// Name of a `fw_error` code (FW_INFO.last_error), e.g. `"DESC_HW_REV"`.
pub fn fw_error_name(code: u8) -> &'static str {
    use pids::fw_error as e;
    match code {
        e::NONE => "NONE",
        e::DESC_MAGIC => "DESC_MAGIC",
        e::DESC_MANUFACTURER => "DESC_MANUFACTURER",
        e::DESC_MODEL => "DESC_MODEL",
        e::DESC_HW_REV => "DESC_HW_REV",
        e::TOO_LARGE => "TOO_LARGE",
        e::SEQUENCE => "SEQUENCE",
        e::DESC_MISMATCH => "DESC_MISMATCH",
        e::CRC => "CRC",
        e::IMAGE_INVALID => "IMAGE_INVALID",
        e::FLASH => "FLASH",
        e::TIMEOUT => "TIMEOUT",
        e::OEM => "OEM",
        _ => "unknown",
    }
}

/// ArtPollReply `VersInfo` (`(MM << 8) | mm`) of a FW_VERSION_ID 0x00MMmmpp.
pub fn vers_info(fw_version_id: u32) -> u16 {
    (((fw_version_id >> 16) & 0xFF) << 8 | ((fw_version_id >> 8) & 0xFF)) as u16
}

/// What the node reports about itself, for [`check_node_image`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NodeIdentity {
    /// ESTA manufacturer (FW_INFO).
    pub manufacturer: u16,
    /// DEVICE_MODEL_ID (FW_INFO).
    pub model: u16,
    /// NODE_HW_REVISION.
    pub hw_revision: u8,
    /// Running FW_VERSION_ID (FW_INFO).
    pub fw_version: u32,
    /// Art-Net OEM code (ArtPollReply); `None` = not checked (RDM method).
    pub oem: Option<u16>,
}

/// The pre-checks of contract §9.2 without network access: manufacturer, model, hardware revision, OEM code (if
/// given) and the downgrade rule. Usable for a preview.
pub fn check_node_image(
    image: &NodeImage,
    node: &NodeIdentity,
    allow_downgrade: bool,
) -> Result<()> {
    let d = &image.desc;
    let refuse =
        |k: FlashErrorKind, m: String| Err(Error::flash(k, format!("{m} - nothing written")));
    if node.manufacturer != d.esta_manufacturer {
        return refuse(
            FlashErrorKind::DeviceMismatch,
            format!(
                "wrong manufacturer: node 0x{:04X}, image 0x{:04X}",
                node.manufacturer, d.esta_manufacturer
            ),
        );
    }
    if node.model != pids::MODEL_NODE || d.device_model_id != pids::MODEL_NODE {
        return refuse(
            FlashErrorKind::DeviceMismatch,
            format!(
                "wrong model: device 0x{:04X}, image 0x{:04X}, expected 0x{:04X}",
                node.model,
                d.device_model_id,
                pids::MODEL_NODE
            ),
        );
    }
    if !d.supports_hw_rev(node.hw_revision) {
        return refuse(
            FlashErrorKind::DeviceMismatch,
            format!(
                "image does not support hardware revision {} (hw_rev_mask 0x{:08X})",
                node.hw_revision, d.hw_rev_mask
            ),
        );
    }
    if let Some(oem) = node.oem {
        if !image.oems.contains(&oem) {
            let list: Vec<String> = image.oems.iter().map(|o| format!("0x{o:04X}")).collect();
            return refuse(
                FlashErrorKind::DeviceMismatch,
                format!(
                    "node OEM code 0x{oem:04X} not in the image's OEM list [{}]",
                    list.join(", ")
                ),
            );
        }
    }
    if d.fw_version_id < node.fw_version && !allow_downgrade {
        return refuse(
            FlashErrorKind::Downgrade,
            format!(
                "downgrade {} -> {} - allow the downgrade explicitly",
                version_str(node.fw_version),
                version_str(d.fw_version_id)
            ),
        );
    }
    Ok(())
}

/// Appended to errors after the first write.
const OLD_STAYS: &str = "the old firmware stays active";

struct Run<'a, L> {
    link: &'a L,
    node: Uid,
    opts: &'a NodeFlashOptions,
    progress: &'a Progress,
    busy: u32,
    written: u32,
}

impl<L: NodeLink> Run<'_, L> {
    async fn call(&mut self, cmd: u8, p: u16, data: &[u8]) -> Result<Vec<u8>> {
        let mut busy = 0;
        loop {
            let o = RdmOptions::default();
            match self.link.rdm(self.node, cmd, p, data.to_vec(), o).await {
                Err(e) if is_busy(&e) => {
                    busy += 1;
                    self.busy += 1;
                    if busy >= self.opts.busy_retries {
                        return Err(Error::flash(FlashErrorKind::Busy, format!(
                            "PID 0x{p:04X}: node still busy (BUFFER_FULL) after {busy} tries (another update \
                             session running?)"
                        )));
                    }
                    pause(self.progress, self.opts.busy_delay).await?;
                }
                r => return r,
            }
        }
    }

    async fn fw_info(&mut self) -> Result<FwInfo> {
        let pd = self.call(cc::GET, pids::PID_FW_INFO, &[]).await?;
        Ok(FwInfo::decode(&pd)?)
    }

    /// FW_INFO.last_error as text, best effort.
    async fn last_error(&mut self) -> String {
        match self.fw_info().await {
            Ok(i) => format!("{} ({})", fw_error_name(i.last_error), i.last_error),
            Err(e) => format!("unknown (FW_INFO: {e})"),
        }
    }

    /// Turn an RDM error of `step` into a flash error (NACK: with FW_INFO.last_error).
    async fn fail(&mut self, step: &str, e: Error) -> Error {
        match e {
            Error::Cancelled => e,
            Error::Nack { reason, .. } => {
                let le = self.last_error().await;
                Error::flash(
                    FlashErrorKind::Rejected,
                    format!("{step} refused (NACK {reason}), last error {le} - {OLD_STAYS}"),
                )
            }
            e => Error::flash(FlashErrorKind::of(&e), format!("{step}: {e} - {OLD_STAYS}")),
        }
    }

    async fn precheck(&mut self, image: &NodeImage) -> Result<FwInfo> {
        let m = format!("checking node {}", self.node);
        self.progress.set(FlashPhase::Checking, 0, 0, m);
        let hw = self.call(cc::GET, pids::PID_NODE_HW_REVISION, &[]).await?;
        let hw_revision = decode_u8(&hw)?;
        let fi = self.fw_info().await?;
        let oem = match self.opts.method {
            NodeFlashMethod::Rdm => None,
            NodeFlashMethod::ArtFirmwareMaster => {
                let st = self.link.node_state().ok_or_else(|| {
                    let m = "node state unknown (no ArtPollReply yet), cannot check its OEM code - nothing written";
                    Error::flash(FlashErrorKind::Other, m)
                })?;
                Some(st.oem)
            }
        };
        let id = NodeIdentity {
            manufacturer: fi.manufacturer,
            model: fi.model,
            hw_revision,
            fw_version: fi.fw_version,
            oem,
        };
        check_node_image(image, &id, self.opts.allow_downgrade)?;
        Ok(fi)
    }

    /// Weg B: FW_BEGIN, FW_WRITE..., FW_FINISH.
    async fn transfer_rdm(&mut self, image: &NodeImage, max_block: u8) -> Result<()> {
        self.progress.set(
            FlashPhase::Writing,
            0,
            0,
            "FW_BEGIN (erasing the OTA partition)",
        );
        if let Err(e) = self
            .call(cc::SET, pids::PID_FW_BEGIN, &image.desc.encode())
            .await
        {
            return Err(self.fail("FW_BEGIN", e).await);
        }
        let block = match usize::from(max_block) {
            0 => FwWrite::max_block(),
            n => n.min(FwWrite::max_block()),
        };
        let total =
            u32::try_from(image.bin.len()).map_err(|_| Error::Image("image too large".into()))?;
        let chunks: Vec<&[u8]> = image.bin.chunks(block).collect();
        let mut offset = 0u32;
        for (i, chunk) in chunks.iter().enumerate() {
            self.progress.check()?;
            let w = FwWrite::new(offset, chunk.to_vec())?;
            if let Err(e) = self.call(cc::SET, pids::PID_FW_WRITE, &w.encode()).await {
                let step = format!("FW_WRITE at offset {offset}");
                return Err(self.fail(&step, e).await);
            }
            self.written += 1;
            offset += chunk.len() as u32;
            if i % 16 == 15 || i + 1 == chunks.len() {
                let m = format!("{offset}/{total} bytes");
                self.progress.set(FlashPhase::Writing, offset, total, m);
            }
        }
        let m = "FW_FINISH (node checks CRC and image, then reboots)";
        self.progress.set(FlashPhase::Finishing, 0, 0, m);
        match self.call(cc::SET, pids::PID_FW_FINISH, &[]).await {
            Ok(_) => Ok(()),
            // the node may have rebooted before the answer got through: the version check decides
            Err(e) if e.is_timeout() => Ok(()),
            Err(e) => Err(self.fail("FW_FINISH", e).await),
        }
    }

    /// Weg A: the `.alf` file in 1024-byte ArtFirmwareMaster blocks.
    async fn transfer_art(&mut self, image: &NodeImage) -> Result<()> {
        let alf = image.to_alf();
        let length = image.alf_firmware_length();
        let blocks: Vec<&[u8]> = alf.chunks(ArtFirmwareMaster::BLOCK_SIZE).collect();
        let n = blocks.len();
        for (i, data) in blocks.iter().enumerate() {
            self.progress.check()?;
            let last = i + 1 == n;
            let kind = match i {
                0 => ArtFirmwareMaster::FIRM_FIRST,
                _ if last => ArtFirmwareMaster::FIRM_LAST,
                _ => ArtFirmwareMaster::FIRM_CONT,
            };
            let mut data = data.to_vec();
            data.resize(ArtFirmwareMaster::BLOCK_SIZE, 0);
            let block = ArtFirmwareMaster::new(kind, (i % 256) as u8, length, data)?;
            if last {
                let m = "FirmLast (node checks CRC and image, then reboots)";
                self.progress
                    .set(FlashPhase::Finishing, i as u32, n as u32, m);
            }
            let reply = match self
                .link
                .firmware_block(block, self.opts.block_timeout)
                .await
            {
                Ok(r) => r,
                Err(Error::WaitTimeout(..)) => {
                    return Err(Error::flash(
                        FlashErrorKind::Timeout,
                        format!(
                            "block {}/{n}: no ArtFirmwareReply within {:?} - {OLD_STAYS}",
                            i + 1,
                            self.opts.block_timeout
                        ),
                    ))
                }
                Err(e) => return Err(e),
            };
            let want = if last {
                ArtFirmwareReply::FIRM_ALL_GOOD
            } else {
                ArtFirmwareReply::FIRM_BLOCK_GOOD
            };
            if reply.kind != want {
                let what = match reply.kind {
                    ArtFirmwareReply::FIRM_FAIL => "FirmFail".to_string(),
                    k => format!("unexpected reply type 0x{k:02X}"),
                };
                let kind = match reply.kind {
                    ArtFirmwareReply::FIRM_FAIL => FlashErrorKind::Rejected,
                    _ => FlashErrorKind::BadResponse,
                };
                let le = self.last_error().await;
                return Err(Error::flash(
                    kind,
                    format!("block {}/{n}: {what}, last error {le} - {OLD_STAYS}", i + 1),
                ));
            }
            self.written += 1;
            if !last {
                let m = format!("block {}/{n}", i + 1);
                self.progress
                    .set(FlashPhase::Writing, i as u32 + 1, n as u32, m);
            }
        }
        Ok(())
    }

    /// Wait for the reboot and confirm the version. Returns the label.
    async fn confirm(&mut self, image: &NodeImage) -> Result<String> {
        let want = image.desc.fw_version_id;
        let m = format!("waiting for the node to reboot into {}", version_str(want));
        self.progress.set(FlashPhase::Confirming, 0, 0, m);
        self.progress.check()?;
        match self
            .link
            .wait_for_version(vers_info(want), self.opts.reboot_timeout)
            .await
        {
            Ok(_) => {}
            Err(Error::WaitTimeout(t, _)) => {
                return Err(Error::flash(FlashErrorKind::Timeout, format!(
                    "node did not come back with version {} within {t:?} (image rejected or rolled back?)",
                    version_str(want)
                )))
            }
            Err(e) => return Err(e),
        }
        self.node = self.link.node_uid();
        let di = self.call(cc::GET, pid::DEVICE_INFO, &[]).await?;
        let sw = DeviceInfo::decode(&di)?.software_version_id;
        if sw != want {
            return Err(Error::flash(
                FlashErrorKind::VersionMismatch,
                format!("node runs 0x{sw:08X} after the update, image 0x{want:08X} (rolled back?)"),
            ));
        }
        let label = self.call(cc::GET, pid::SOFTWARE_VERSION_LABEL, &[]).await?;
        Ok(decode_label(&label))
    }
}

/// Run a node update through `link`.
pub async fn flash_node<L: NodeLink>(
    link: &L,
    image: &NodeImage,
    opts: &NodeFlashOptions,
    progress: &Progress,
) -> Result<FlashReport> {
    progress.check()?;
    let mut run = Run {
        link,
        node: link.node_uid(),
        opts,
        progress,
        busy: 0,
        written: 0,
    };
    let fi = run.precheck(image).await?;
    let r = match opts.method {
        NodeFlashMethod::Rdm => run.transfer_rdm(image, fi.max_block).await,
        NodeFlashMethod::ArtFirmwareMaster => run.transfer_art(image).await,
    };
    if let Err(e) = r {
        if matches!(e, Error::Cancelled) {
            let p = run.progress.current();
            let m =
                format!("cancelled - the node drops the partial update after 30 s, {OLD_STAYS}");
            run.progress.set(p.phase, p.done, p.total, m);
        }
        return Err(e);
    }
    let version_label = run.confirm(image).await?;
    Ok(FlashReport {
        old_version: Some(fi.fw_version),
        new_version: image.desc.fw_version_id,
        version_label,
        units_written: run.written,
        busy_retries: run.busy,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::artnet::ArtPollReply;
    use crate::crc::crc32;
    use crate::flash::{FlashJob, FlashProgress};
    use crate::image::node::{
        build_test_bin, NodeImageDesc, ALF_DESC_OFFSET, ALF_HEADER_LEN, ALF_OEM_OFFSET,
    };
    use crate::link::RdmLink;
    use crate::rdm::pd::encode_label;
    use crate::rdm::NackReason;
    use crate::state::{merge_replies, reply_node_id, NodeState};
    use std::future::Future;
    use std::sync::{Arc, Mutex};

    const NODE: Uid = Uid::new(0x7FF0, 0x8012_3456);

    struct Session {
        desc: NodeImageDesc,
        data: Vec<u8>,
    }

    struct ArtSession {
        next: usize,
        length: u32,
        buf: Vec<u8>,
        desc: NodeImageDesc,
    }

    /// Simulated node: FW_* PIDs (image-format.md §2.3/2.4) and an ArtFirmwareMaster session (§2.5).
    struct Node {
        hw_rev: u8,
        oem: u16,
        model: u16,
        version: u32,
        label: String,
        partition: u32,
        last_error: u8,
        session: Option<Session>,
        art: Option<ArtSession>,
        begins: u32,
        writes: u32,
        busy_every: u32,
        busy_hits: u32,
        art_blocks: u32,
        fail_at_block: Option<usize>,
    }

    impl Node {
        fn new() -> Self {
            Self {
                hw_rev: 0,
                oem: pids::ARTNET_OEM_NODE,
                model: pids::MODEL_NODE,
                version: 0x0001_0100,
                label: "1.1.0".into(),
                partition: 1 << 20,
                last_error: 0,
                session: None,
                art: None,
                begins: 0,
                writes: 0,
                busy_every: 0,
                busy_hits: 0,
                art_blocks: 0,
                fail_at_block: None,
            }
        }

        fn fw_info(&self) -> FwInfo {
            FwInfo {
                manufacturer: pids::ESTA_MANUFACTURER,
                model: self.model,
                hw_revision: self.hw_rev,
                fw_version: self.version,
                state: 0,
                last_error: self.last_error,
                max_block: pids::NODE_FW_MAX_BLOCK as u8,
                received: 0,
                total: 0,
            }
        }

        fn install(&mut self, desc: &NodeImageDesc, bin: &[u8]) -> bool {
            if bin.len() as u32 != desc.image_length || crc32(bin) != desc.image_crc32 {
                self.last_error = pids::fw_error::CRC;
                return false;
            }
            self.version = desc.fw_version_id;
            self.label = desc.version_label.clone();
            true
        }

        fn rdm(&mut self, cmd: u8, p: u16, pd: &[u8]) -> std::result::Result<Vec<u8>, NackReason> {
            let range = NackReason::DATA_OUT_OF_RANGE;
            match (cmd, p) {
                (cc::GET, pids::PID_NODE_HW_REVISION) => Ok(vec![self.hw_rev]),
                (cc::GET, pids::PID_FW_INFO) => Ok(self.fw_info().encode()),
                (cc::GET, pid::SOFTWARE_VERSION_LABEL) => Ok(encode_label(&self.label)),
                (cc::GET, pid::DEVICE_INFO) => Ok(DeviceInfo {
                    rdm_protocol_version: 0x0100,
                    device_model_id: self.model,
                    product_category: pids::NODE_PRODUCT_CATEGORY,
                    software_version_id: self.version,
                    dmx_footprint: 0,
                    current_personality: 0,
                    personality_count: 0,
                    dmx_start_address: 0xFFFF,
                    sub_device_count: 0,
                    sensor_count: 0,
                }
                .encode()),
                (cc::SET, pids::PID_FW_BEGIN) => {
                    self.begins += 1;
                    let desc = NodeImageDesc::decode(pd).map_err(|_| NackReason::FORMAT_ERROR)?;
                    if let Err(code) =
                        desc.validate(pids::ESTA_MANUFACTURER, self.hw_rev, self.partition)
                    {
                        self.last_error = code;
                        return Err(range);
                    }
                    self.session = Some(Session { desc, data: vec![] });
                    Ok(vec![])
                }
                (cc::SET, pids::PID_FW_WRITE) => {
                    let w = FwWrite::decode(pd).map_err(|_| NackReason::FORMAT_ERROR)?;
                    self.writes += 1;
                    if self.busy_every != 0 && self.writes.is_multiple_of(self.busy_every) {
                        self.busy_hits += 1;
                        return Err(NackReason::BUFFER_FULL);
                    }
                    let Some(s) = self.session.as_mut() else {
                        self.last_error = pids::fw_error::SEQUENCE;
                        return Err(range);
                    };
                    let off = w.offset as usize;
                    if off > s.data.len() {
                        self.last_error = pids::fw_error::SEQUENCE;
                        return Err(range);
                    }
                    if off == s.data.len() {
                        s.data.extend_from_slice(&w.data);
                    }
                    Ok(vec![])
                }
                (cc::SET, pids::PID_FW_FINISH) => {
                    let Some(s) = self.session.take() else {
                        self.last_error = pids::fw_error::SEQUENCE;
                        return Err(range);
                    };
                    if self.install(&s.desc, &s.data) {
                        Ok(vec![])
                    } else {
                        Err(range)
                    }
                }
                _ => Err(NackReason::UNKNOWN_PID),
            }
        }

        fn art(&mut self, b: &ArtFirmwareMaster) -> u8 {
            const FAIL: u8 = ArtFirmwareReply::FIRM_FAIL;
            self.art_blocks += 1;
            let idx = self.art.as_ref().map_or(0, |a| a.next);
            if self.fail_at_block == Some(idx) {
                self.last_error = pids::fw_error::FLASH;
                self.art = None;
                return FAIL;
            }
            match b.kind {
                ArtFirmwareMaster::FIRM_FIRST => {
                    if b.block_id != 0 {
                        self.last_error = pids::fw_error::SEQUENCE;
                        return FAIL;
                    }
                    let d = &b.data;
                    let oems: Vec<u16> = (0..256)
                        .map(|i| {
                            u16::from_be_bytes([
                                d[ALF_OEM_OFFSET + 2 * i],
                                d[ALF_OEM_OFFSET + 2 * i + 1],
                            ])
                        })
                        .collect();
                    if !oems.contains(&self.oem) {
                        self.last_error = pids::fw_error::OEM;
                        return FAIL;
                    }
                    let Ok(desc) = NodeImageDesc::decode(&d[ALF_DESC_OFFSET..]) else {
                        return FAIL;
                    };
                    if let Err(code) =
                        desc.validate(pids::ESTA_MANUFACTURER, self.hw_rev, self.partition)
                    {
                        self.last_error = code;
                        return FAIL;
                    }
                    let words = ALF_HEADER_LEN as u32 / 2 + desc.image_length.div_ceil(2);
                    if b.firmware_length != words {
                        self.last_error = pids::fw_error::DESC_MISMATCH;
                        return FAIL;
                    }
                    self.art = Some(ArtSession {
                        next: 1,
                        length: b.firmware_length,
                        buf: d.clone(),
                        desc,
                    });
                    ArtFirmwareReply::FIRM_BLOCK_GOOD
                }
                ArtFirmwareMaster::FIRM_CONT | ArtFirmwareMaster::FIRM_LAST => {
                    let Some(mut a) = self.art.take() else {
                        self.last_error = pids::fw_error::SEQUENCE;
                        return FAIL;
                    };
                    if usize::from(b.block_id) != a.next % 256 {
                        self.last_error = pids::fw_error::SEQUENCE;
                        return FAIL;
                    }
                    a.buf.extend_from_slice(&b.data);
                    a.next += 1;
                    if b.kind == ArtFirmwareMaster::FIRM_CONT {
                        self.art = Some(a);
                        return ArtFirmwareReply::FIRM_BLOCK_GOOD;
                    }
                    a.buf.truncate(2 * a.length as usize);
                    let len = a.desc.image_length as usize;
                    let bin = a.buf[ALF_HEADER_LEN..ALF_HEADER_LEN + len].to_vec();
                    if self.install(&a.desc, &bin) {
                        ArtFirmwareReply::FIRM_ALL_GOOD
                    } else {
                        FAIL
                    }
                }
                _ => FAIL,
            }
        }

        fn state(&self) -> NodeState {
            let r = ArtPollReply {
                oem: self.oem,
                esta_man: pids::ESTA_MANUFACTURER,
                vers_info: vers_info(self.version),
                default_resp_uid: NODE,
                ..Default::default()
            };
            let src = "2.0.0.10:6454".parse().unwrap();
            let id = reply_node_id(&r, src);
            merge_replies(id, &[(src, r)]).unwrap()
        }
    }

    struct FakeNode(Mutex<Node>);

    impl FakeNode {
        fn with<R>(&self, f: impl FnOnce(&mut Node) -> R) -> R {
            f(&mut self.0.lock().unwrap())
        }
    }

    impl RdmLink for FakeNode {
        fn rdm(
            &self,
            uid: Uid,
            command_class: u8,
            p: u16,
            data: Vec<u8>,
            opts: RdmOptions,
        ) -> impl Future<Output = Result<Vec<u8>>> + Send {
            let mut busy_left = opts.buffer_full_retries;
            let r = loop {
                if uid != NODE {
                    break Err(Error::Timeout {
                        uid,
                        pid: p,
                        attempts: 3,
                    });
                }
                match self.with(|n| n.rdm(command_class, p, &data)) {
                    Ok(pd) => break Ok(pd),
                    Err(NackReason::BUFFER_FULL) if busy_left > 0 => busy_left -= 1,
                    Err(reason) => {
                        break Err(Error::Nack {
                            uid,
                            pid: p,
                            reason,
                        })
                    }
                }
            };
            async move { r }
        }
    }

    impl NodeLink for FakeNode {
        fn node_uid(&self) -> Uid {
            NODE
        }

        fn node_state(&self) -> Option<NodeState> {
            Some(self.with(|n| n.state()))
        }

        fn firmware_block(
            &self,
            block: ArtFirmwareMaster,
            _timeout: Duration,
        ) -> impl Future<Output = Result<ArtFirmwareReply>> + Send {
            let kind = self.with(|n| n.art(&block));
            async move { Ok(ArtFirmwareReply { kind }) }
        }

        fn wait_for_version(
            &self,
            v: u16,
            timeout: Duration,
        ) -> impl Future<Output = Result<NodeState>> + Send {
            let st = self.with(|n| n.state());
            async move {
                if st.firmware_version == v {
                    Ok(st)
                } else {
                    tokio::time::sleep(timeout).await;
                    Err(Error::WaitTimeout(timeout, "node version"))
                }
            }
        }
    }

    fn image() -> NodeImage {
        NodeImage::from_bin(build_test_bin(5001, 0x0001_0200, 1, "1.2.0")).unwrap()
    }

    fn opts(method: NodeFlashMethod) -> NodeFlashOptions {
        NodeFlashOptions {
            method,
            ..Default::default()
        }
    }

    async fn run(
        node: Node,
        image: NodeImage,
        opts: NodeFlashOptions,
    ) -> (Result<FlashReport>, Vec<FlashProgress>, Arc<FakeNode>) {
        let fake = Arc::new(FakeNode(Mutex::new(node)));
        let link = fake.clone();
        let seen = Arc::new(Mutex::new(Vec::new()));
        let s2 = seen.clone();
        let job = FlashJob::spawn(
            2,
            Some(Arc::new(move |p: &FlashProgress| {
                s2.lock().unwrap().push(p.clone())
            })),
            move |p| async move { flash_node(&*link, &image, &opts, &p).await },
        );
        let r = job.wait().await;
        let seen = seen.lock().unwrap().clone();
        (r, seen, fake)
    }

    fn err_text(r: &Result<FlashReport>) -> String {
        match r {
            Ok(_) => "ok".into(),
            Err(e) => e.to_string(),
        }
    }

    fn kind(r: &Result<FlashReport>) -> Option<FlashErrorKind> {
        r.as_ref().err().and_then(Error::flash_kind)
    }

    #[tokio::test(start_paused = true)]
    async fn rdm_happy_path() {
        let node = Node {
            busy_every: 5,
            ..Node::new()
        };
        let (r, seen, fake) = run(node, image(), opts(NodeFlashMethod::Rdm)).await;
        let rep = r.unwrap();
        let blocks = 5001usize.div_ceil(FwWrite::max_block()) as u32;
        assert_eq!(rep.units_written, blocks);
        assert_eq!(rep.old_version, Some(0x0001_0100));
        assert_eq!(rep.new_version, 0x0001_0200);
        assert_eq!(rep.version_label, "1.2.0");
        assert!(rep.busy_retries > 0);
        fake.with(|n| {
            assert_eq!(n.version, 0x0001_0200);
            assert_eq!(n.busy_hits, rep.busy_retries);
        });
        let w = seen
            .iter()
            .rfind(|p| p.phase == FlashPhase::Writing)
            .unwrap();
        assert_eq!((w.done, w.total), (5001, 5001));
        for ph in [
            FlashPhase::Checking,
            FlashPhase::Finishing,
            FlashPhase::Confirming,
            FlashPhase::Done,
        ] {
            assert!(seen.iter().any(|p| p.phase == ph), "{ph:?}");
        }
    }

    #[tokio::test(start_paused = true)]
    async fn art_firmware_master_happy_path() {
        let im = image();
        let n = im.to_alf().len().div_ceil(1024) as u32;
        assert!(n >= 2);
        let (r, seen, fake) = run(Node::new(), im, opts(NodeFlashMethod::ArtFirmwareMaster)).await;
        let rep = r.unwrap();
        assert_eq!(rep.units_written, n);
        assert_eq!(rep.version_label, "1.2.0");
        fake.with(|node| {
            assert_eq!(node.version, 0x0001_0200);
            assert_eq!(node.art_blocks, n);
        });
        assert!(seen
            .iter()
            .any(|p| p.phase == FlashPhase::Writing && p.total == n));
    }

    #[tokio::test(start_paused = true)]
    async fn refused_before_writing() {
        // hardware revision 1 not in mask 0b1
        for method in [NodeFlashMethod::Rdm, NodeFlashMethod::ArtFirmwareMaster] {
            let node = Node {
                hw_rev: 1,
                ..Node::new()
            };
            let (r, _, fake) = run(node, image(), opts(method)).await;
            let e = err_text(&r);
            assert!(
                e.contains("hardware revision 1") && e.contains("nothing written"),
                "{e}"
            );
            assert_eq!(kind(&r), Some(FlashErrorKind::DeviceMismatch));
            fake.with(|n| assert_eq!((n.begins, n.art_blocks), (0, 0)));
        }
        // OEM code not in the image's list
        let node = Node {
            oem: 0x1234,
            ..Node::new()
        };
        let (r, _, fake) = run(node, image(), opts(NodeFlashMethod::ArtFirmwareMaster)).await;
        assert!(err_text(&r).contains("OEM code 0x1234"), "{}", err_text(&r));
        assert_eq!(kind(&r), Some(FlashErrorKind::DeviceMismatch));
        fake.with(|n| assert_eq!(n.art_blocks, 0));
        // wrong model
        let node = Node {
            model: pids::MODEL_TUBE,
            ..Node::new()
        };
        let (r, _, fake) = run(node, image(), opts(NodeFlashMethod::Rdm)).await;
        assert!(err_text(&r).contains("wrong model"), "{}", err_text(&r));
        assert_eq!(kind(&r), Some(FlashErrorKind::DeviceMismatch));
        fake.with(|n| assert_eq!(n.begins, 0));
        // downgrade
        let node = Node {
            version: 0x0002_0000,
            ..Node::new()
        };
        let (r, _, fake) = run(node, image(), opts(NodeFlashMethod::Rdm)).await;
        assert!(
            err_text(&r).contains("downgrade 2.0.0 -> 1.2.0"),
            "{}",
            err_text(&r)
        );
        assert_eq!(kind(&r), Some(FlashErrorKind::Downgrade));
        fake.with(|n| assert_eq!(n.begins, 0));
        let node = Node {
            version: 0x0002_0000,
            ..Node::new()
        };
        let o = NodeFlashOptions {
            allow_downgrade: true,
            ..opts(NodeFlashMethod::Rdm)
        };
        let (r, _, _) = run(node, image(), o).await;
        assert_eq!(r.unwrap().old_version, Some(0x0002_0000));
    }

    #[tokio::test(start_paused = true)]
    async fn fw_begin_nack_reports_last_error() {
        let node = Node {
            partition: 1000,
            ..Node::new()
        };
        let (r, _, fake) = run(node, image(), opts(NodeFlashMethod::Rdm)).await;
        let e = err_text(&r);
        assert!(
            e.contains("FW_BEGIN refused") && e.contains("TOO_LARGE (5)"),
            "{e}"
        );
        assert_eq!(kind(&r), Some(FlashErrorKind::Rejected));
        fake.with(|n| {
            assert_eq!(n.writes, 0);
            assert_eq!(n.version, 0x0001_0100);
        });
    }

    #[tokio::test(start_paused = true)]
    async fn firm_fail_mid_transfer() {
        let node = Node {
            fail_at_block: Some(2),
            ..Node::new()
        };
        let (r, _, fake) = run(node, image(), opts(NodeFlashMethod::ArtFirmwareMaster)).await;
        let e = err_text(&r);
        assert!(
            e.contains("block 3/") && e.contains("FirmFail") && e.contains("FLASH (10)"),
            "{e}"
        );
        assert_eq!(kind(&r), Some(FlashErrorKind::Rejected));
        fake.with(|n| {
            assert_eq!(n.art_blocks, 3);
            assert_eq!(n.version, 0x0001_0100);
        });
    }

    #[test]
    fn helpers() {
        assert_eq!(vers_info(0x0001_0203), 0x0102);
        assert_eq!(fw_error_name(pids::fw_error::DESC_HW_REV), "DESC_HW_REV");
        assert_eq!(fw_error_name(200), "unknown");
    }
}
