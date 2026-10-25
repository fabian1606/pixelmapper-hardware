//! Tube firmware update over RDM (contract §9.1, reference `tube/tools/flash_rdm.py`).
//!
//! [`flash_tube`] is a faithful port of `flash()` in `flash_rdm.py`:
//!
//! 1. app pre-check via DEVICE_INFO (manufacturer/model/version) before the app is stopped,
//! 2. ENTER_BOOTLOADER, or for a rescue BOOT_CATCH at the node plus a power-cycle by the user (§6.4),
//! 3. BOOT_INFO (protocol, geometry, ids, version from the installed trailer),
//! 4. BOOT_WRITE_PAGE for every non-blank page, trailer page first (trailer erased) and last (real trailer),
//! 5. BOOT_APP_CRC in 256-byte chunks, differing chunks rewritten once (two passes),
//! 6. BOOT_FINISH and a check that the new app runs with the trailer's version.
//!
//! NACK BUFFER_FULL (bootloader busy writing a page, request dropped) is retried here rather than in the link, so
//! the retries are counted in [`FlashReport::busy_retries`] and cancellation is seen while waiting.

use super::{FlashPhase, FlashReport, Progress};
use crate::error::{Error, Result};
use crate::image::tube::{boot_err, version_str, BOOT_START, PAGE};
use crate::image::TubeImage;
use crate::link::{RdmLink, RdmOptions};
use crate::pids;
use crate::rdm::pd::{decode_label, BootCatchStart, BootCatchStatus, BootInfo};
use crate::rdm::{cc, pid, NackReason, Uid};
use std::time::Duration;

/// Where to start a BOOT_CATCH for a rescue (contract §6.4).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RescueTarget {
    /// UID of the node (BOOT_CATCH_START is sent there, through the same port link).
    pub node_uid: Uid,
    /// Physical node port 1..4 the tube hangs on.
    pub port: u8,
}

/// Options of a tube update.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TubeFlashOptions {
    /// Allow an image older than the installed app.
    pub allow_downgrade: bool,
    /// Rescue via BOOT_CATCH instead of ENTER_BOOTLOADER (app dead).
    pub rescue: Option<RescueTarget>,
    /// Pause after ENTER_BOOTLOADER before polling BOOT_INFO.
    pub enter_wait: Duration,
    /// Pause between BOOT_INFO attempts.
    pub boot_info_retry_wait: Duration,
    /// Pause after BOOT_FINISH before checking the app (self-test ~2 s).
    pub finish_wait: Duration,
    /// How long to wait for BOOT_CATCH to report CAUGHT.
    pub catch_timeout: Duration,
    /// Interval of the BOOT_CATCH_STATUS polls.
    pub catch_poll_interval: Duration,
    /// Pause before repeating a request answered with NACK BUFFER_FULL (a page write takes ~9 ms).
    pub busy_delay: Duration,
    /// BUFFER_FULL answers per request after which the request fails.
    pub busy_retries: u32,
}

impl Default for TubeFlashOptions {
    fn default() -> Self {
        Self {
            allow_downgrade: false,
            rescue: None,
            enter_wait: Duration::from_millis(500),
            boot_info_retry_wait: Duration::from_millis(300),
            finish_wait: Duration::from_secs(3),
            catch_timeout: Duration::from_secs(75),
            catch_poll_interval: Duration::from_millis(250),
            busy_delay: Duration::from_millis(30),
            busy_retries: 100,
        }
    }
}

/// Attempts at BOOT_INFO after entering the bootloader.
const BOOT_INFO_ATTEMPTS: u32 = 10;
/// BOOT_INFO polls while waiting for `busy == 0`.
const IDLE_POLLS: u32 = 100;
/// Pause between those polls.
const IDLE_POLL_WAIT: Duration = Duration::from_millis(20);
/// Appended to errors that leave the tube in its bootloader.
const STAYS_ACTIVE: &str = "bootloader stays active, run again";

/// Check manufacturer and model reported by the device (`source` names where they come from) against the trailer.
pub fn check_tube_ids(
    image: &TubeImage,
    manufacturer: u16,
    model: u16,
    source: &str,
) -> Result<()> {
    let t = &image.trailer;
    if (manufacturer, model) != (t.manufacturer, t.model) {
        return Err(Error::Flash(format!(
            "wrong device: {source} reports manufacturer 0x{manufacturer:04X} model 0x{model:04X}, image is for \
             manufacturer 0x{:04X} model 0x{:04X} - nothing written",
            t.manufacturer, t.model
        )));
    }
    Ok(())
}

/// Refuse a downgrade from `installed` (if known) to the image version unless `allow_downgrade`.
pub fn check_tube_version(
    image: &TubeImage,
    installed: Option<u32>,
    allow_downgrade: bool,
) -> Result<()> {
    let new = image.trailer.version;
    match installed {
        Some(old) if new < old && !allow_downgrade => Err(Error::Flash(format!(
            "downgrade {} -> {} - allow the downgrade explicitly; nothing written",
            version_str(old),
            version_str(new)
        ))),
        _ => Ok(()),
    }
}

/// Preview check without writing: ids against the trailer and, if `installed` is known, the downgrade rule.
pub fn check_tube_image_for_device(
    image: &TubeImage,
    manufacturer: u16,
    model: u16,
    installed: Option<u32>,
    allow_downgrade: bool,
) -> Result<()> {
    check_tube_ids(image, manufacturer, model, "the device")?;
    check_tube_version(image, installed, allow_downgrade)
}

/// Whether a software version id belongs to `tube_boot_rdm` (0xB0xxxxxx) rather than to an app.
pub fn is_bootloader_version(sw: u32) -> bool {
    sw >> 24 == pids::tube::BOOT_RDM_SW_VERSION_ID >> 24
}

/// Decode a BOOT_INFO answer like `flash_rdm.py boot_info_parse`: short answers and bootloaders older than
/// `BOOT_RDM_PROTOCOL` (which do not know the trailer) are refused.
pub fn parse_boot_info(pd: &[u8]) -> Result<BootInfo> {
    if pd.len() < 6 {
        return Err(Error::Flash(format!(
            "BOOT_INFO: short answer {}",
            crate::to_hex(pd)
        )));
    }
    if u32::from(pd[0]) < pids::tube::BOOT_RDM_PROTOCOL || pd.len() < BootInfo::LEN {
        return Err(Error::Flash(format!(
            "tube_boot_rdm v{} ({}-byte BOOT_INFO) does not know the trailer of this image - its FINISH would \
             reject it. Install tube_boot_rdm v{} once via ISP",
            pd[0],
            pd.len(),
            pids::tube::BOOT_RDM_VERSION
        )));
    }
    Ok(BootInfo::decode(pd)?)
}

/// Software version id from DEVICE_INFO bytes 6..10 (`None` if the answer is shorter).
fn device_info_sw(pd: &[u8]) -> Option<u32> {
    let b = pd.get(6..10)?;
    Some(u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}

/// NACK BUFFER_FULL / PROXY_BUFFER_FULL.
pub(crate) fn is_busy(e: &Error) -> bool {
    matches!(
        e.nack_reason(),
        Some(NackReason::BUFFER_FULL) | Some(NackReason::PROXY_BUFFER_FULL)
    )
}

/// Sleep `d`, then fail with `Cancelled` if cancellation was requested. Does not use [`Progress::sleep`], which
/// returns `Cancelled` at once for a [`Progress::detached`] handle (its cancel sender is dropped).
pub(crate) async fn pause(progress: &Progress, d: Duration) -> Result<()> {
    progress.check()?;
    if !d.is_zero() {
        tokio::time::sleep(d).await;
    }
    progress.check()
}

/// One update run: link, target, options and counters.
struct Run<'a, L> {
    link: &'a L,
    uid: Uid,
    opts: &'a TubeFlashOptions,
    progress: &'a Progress,
    busy: u32,
    written: u32,
}

impl<L: RdmLink> Run<'_, L> {
    /// Request to `to` with `retries` repeats on no answer; BUFFER_FULL is repeated up to `busy_retries` times.
    async fn call_to(
        &mut self,
        to: Uid,
        cmd: u8,
        p: u16,
        data: &[u8],
        retries: u32,
    ) -> Result<Vec<u8>> {
        let mut busy = 0;
        loop {
            let o = RdmOptions::retries(retries);
            match self.link.rdm(to, cmd, p, data.to_vec(), o).await {
                Err(e) if is_busy(&e) => {
                    busy += 1;
                    self.busy += 1;
                    if busy >= self.opts.busy_retries {
                        return Err(Error::Flash(format!(
                            "PID 0x{p:04X}: still busy (BUFFER_FULL) after {busy} tries"
                        )));
                    }
                    pause(self.progress, self.opts.busy_delay).await?;
                }
                r => return r,
            }
        }
    }

    async fn get(&mut self, p: u16, data: &[u8], retries: u32) -> Result<Vec<u8>> {
        self.call_to(self.uid, cc::GET, p, data, retries).await
    }

    async fn set(&mut self, p: u16, data: &[u8], retries: u32) -> Result<Vec<u8>> {
        self.call_to(self.uid, cc::SET, p, data, retries).await
    }

    async fn boot_info(&mut self, retries: u32) -> Result<BootInfo> {
        let pd = self.get(pids::PID_BOOT_INFO, &[], retries).await?;
        parse_boot_info(&pd)
    }

    /// `app_precheck`: `Some(installed version)` if the app answered and the image fits, `None` if no app answers.
    async fn app_precheck(&mut self, image: &TubeImage) -> Result<Option<u32>> {
        self.progress.set(
            FlashPhase::Checking,
            0,
            0,
            "checking the running app (DEVICE_INFO)",
        );
        let di = match self.get(pid::DEVICE_INFO, &[], 1).await {
            Ok(di) => di,
            Err(Error::Cancelled) => return Err(Error::Cancelled),
            Err(_) => return Ok(None), // no answer / NACK: the bootloader steps decide
        };
        let (Some(sw), Some(m)) = (device_info_sw(&di), di.get(2..4)) else {
            return Ok(None);
        };
        if is_bootloader_version(sw) {
            return Ok(None); // already in tube_boot_rdm
        }
        let model = u16::from_be_bytes([m[0], m[1]]);
        check_tube_ids(image, self.uid.manufacturer, model, "DEVICE_INFO (app)")?;
        check_tube_version(image, Some(sw), self.opts.allow_downgrade)?;
        Ok(Some(sw))
    }

    async fn catch_abort(&self, node: Uid) {
        // best effort: the node gives up after BOOT_CATCH_DURATION_MS anyway
        let o = RdmOptions::retries(1);
        let _ = self
            .link
            .rdm(node, cc::SET, pids::PID_BOOT_CATCH_ABORT, vec![], o)
            .await;
    }

    /// Rescue: let the node catch the bootloader's boot window after a power-cycle (contract §6.4).
    async fn catch(&mut self, rescue: RescueTarget) -> Result<()> {
        let start = BootCatchStart {
            port: rescue.port,
            target_uid: self.uid,
        };
        let pd = start.encode();
        self.call_to(rescue.node_uid, cc::SET, pids::PID_BOOT_CATCH_START, &pd, 2)
            .await
            .map_err(|e| match e {
                Error::Cancelled => e,
                e => Error::Flash(format!("BOOT_CATCH_START at node {}: {e}", rescue.node_uid)),
            })?;
        let msg = format!(
            "power-cycle tube {} on port {} now (switch its supply off and on again)",
            self.uid, rescue.port
        );
        self.progress.set(FlashPhase::Catching, 0, 0, msg.clone());
        let r = self.catch_poll(rescue, &msg).await;
        if r.is_err() {
            self.catch_abort(rescue.node_uid).await;
        }
        r
    }

    async fn catch_poll(&mut self, rescue: RescueTarget, msg: &str) -> Result<()> {
        use pids::boot_catch_state as st;
        let deadline = tokio::time::Instant::now() + self.opts.catch_timeout;
        let mut polls = 0u32;
        loop {
            pause(self.progress, self.opts.catch_poll_interval).await?;
            polls += 1;
            let p = pids::PID_BOOT_CATCH_STATUS;
            let status = match self.call_to(rescue.node_uid, cc::GET, p, &[], 1).await {
                Ok(pd) => Some(BootCatchStatus::decode(&pd)?),
                Err(e) if e.is_timeout() => None,
                Err(e) => return Err(e),
            };
            if let Some(s) = status {
                match s.state {
                    st::CAUGHT if s.target_uid == self.uid => return Ok(()),
                    st::CAUGHT => {
                        return Err(Error::Flash(format!(
                            "BOOT_CATCH caught {} instead of {} (another catch replaced ours)",
                            s.target_uid, self.uid
                        )))
                    }
                    st::TIMEOUT => {
                        return Err(Error::Flash(format!(
                            "BOOT_CATCH: no answer from {} in the catch window ({} attempts) - tube not \
                             power-cycled, wrong port/UID, or tube_boot_rdm not installed (hfuse 0x88)? \
                             nothing written",
                            self.uid, s.attempts
                        )))
                    }
                    st::ABORTED => {
                        return Err(Error::Flash(
                            "BOOT_CATCH aborted at the node (abort, port disabled or switched to input); \
                             nothing written"
                                .into(),
                        ))
                    }
                    st::IDLE => {
                        return Err(Error::Flash(
                            "BOOT_CATCH not running at the node (node restarted?); nothing written".into(),
                        ))
                    }
                    _ => {
                        let s = s.elapsed_ms / 1000;
                        let m = format!("{msg} ({s} s)");
                        self.progress.set(FlashPhase::Catching, polls, 0, m);
                    }
                }
            }
            if tokio::time::Instant::now() >= deadline {
                return Err(Error::Flash(format!(
                    "BOOT_CATCH: not caught within {:?}; nothing written",
                    self.opts.catch_timeout
                )));
            }
        }
    }

    /// `enter_bootloader`: ENTER_BOOTLOADER (or BOOT_CATCH), then BOOT_INFO with the geometry check.
    async fn enter(&mut self) -> Result<BootInfo> {
        if let Some(rescue) = self.opts.rescue {
            self.catch(rescue).await?;
            let m = "caught, reading BOOT_INFO";
            self.progress.set(FlashPhase::EnteringBootloader, 0, 0, m);
        } else {
            let m = "ENTER_BOOTLOADER";
            self.progress.set(FlashPhase::EnteringBootloader, 0, 0, m);
            match self.set(pids::PID_ENTER_BOOTLOADER, b"BOOT", 1).await {
                // NACK: bootloader already active? no answer: try BOOT_INFO anyway
                Ok(_) | Err(Error::Nack { .. }) | Err(Error::Timeout { .. }) => {}
                Err(e) => return Err(e),
            }
            pause(self.progress, self.opts.enter_wait).await?;
        }
        let mut info = None;
        for _ in 0..BOOT_INFO_ATTEMPTS {
            match self.boot_info(1).await {
                Ok(i) => {
                    info = Some(i);
                    break;
                }
                Err(e) if e.is_timeout() => {
                    pause(self.progress, self.opts.boot_info_retry_wait).await?
                }
                Err(e) => return Err(e),
            }
        }
        let info = info.ok_or_else(|| {
            Error::Flash(format!(
                "no BOOT_INFO answer from {} - not in tube_boot_rdm (old 1 KB tube_boot -> flash_dmx.py; app dead \
                 -> rescue via BOOT_CATCH); nothing written",
                self.uid
            ))
        })?;
        if u16::from(info.page_size) != PAGE || info.app_end != BOOT_START {
            return Err(Error::Flash(format!(
                "bootloader geometry (page {} B, app end 0x{:04X}) does not match the image (page {PAGE} B, app \
                 end 0x{BOOT_START:04X}); nothing written",
                info.page_size, info.app_end
            )));
        }
        Ok(info)
    }

    /// `wait_idle`: BOOT_INFO until busy == 0. A nonzero last error seen on the way is kept (reading clears it).
    async fn wait_idle(&mut self) -> Result<BootInfo> {
        let mut err = 0;
        for _ in 0..IDLE_POLLS {
            let mut info = self.boot_info(2).await?;
            if err == 0 {
                err = info.last_error;
            }
            if info.busy == 0 {
                info.last_error = err;
                return Ok(info);
            }
            pause(self.progress, IDLE_POLL_WAIT).await?;
        }
        Err(Error::Flash(format!(
            "bootloader stays busy - {STAYS_ACTIVE}"
        )))
    }

    /// `write_pages` with progress in `phase`.
    async fn write_pages(&mut self, plan: &[(u16, Vec<u8>)], phase: FlashPhase) -> Result<()> {
        let total = plan.len() as u32;
        for (i, (a, data)) in plan.iter().enumerate() {
            self.progress.check()?;
            let mut pd = a.to_be_bytes().to_vec();
            pd.extend_from_slice(data);
            match self.set(pids::PID_BOOT_WRITE_PAGE, &pd, 4).await {
                Ok(_) => {}
                Err(Error::Cancelled) => return Err(Error::Cancelled),
                Err(e) => {
                    return Err(Error::Flash(format!(
                        "WRITE_PAGE 0x{a:04X}: {e} - {STAYS_ACTIVE}"
                    )))
                }
            }
            self.written += 1;
            let m = format!("page 0x{a:04X} {}/{total}", i + 1);
            self.progress.set(phase, i as u32 + 1, total, m);
        }
        Ok(())
    }

    /// `bad_chunks`: compare BOOT_APP_CRC with the image chunk by chunk.
    async fn bad_chunks(&mut self, image: &TubeImage) -> Result<Vec<(u16, u16)>> {
        let chunks = image.crc_chunks();
        let total = chunks.len() as u32;
        let mut bad = Vec::new();
        for (i, &(a, n, want)) in chunks.iter().enumerate() {
            let mut req = a.to_be_bytes().to_vec();
            req.extend_from_slice(&n.to_be_bytes());
            let pd = self.get(pids::PID_BOOT_APP_CRC, &req, 2).await?;
            let Some(b) = pd.get(..2) else {
                return Err(Error::Flash(format!(
                    "APP_CRC: short answer {} - {STAYS_ACTIVE}",
                    crate::to_hex(&pd)
                )));
            };
            if u16::from_be_bytes([b[0], b[1]]) != want {
                bad.push((a, n));
            }
            let m = format!("CRC 0x{a:04X}");
            self.progress
                .set(FlashPhase::Verifying, i as u32 + 1, total, m);
        }
        Ok(bad)
    }

    /// Verify in up to two passes; differing chunks (stale content in a skipped page, verify error) are written
    /// once more, all their pages; a bootloader error without differing chunks rewrites all non-blank pages.
    async fn verify(&mut self, image: &TubeImage) -> Result<()> {
        for pass in 1..=2 {
            let m = format!("verify pass {pass}");
            self.progress.set(FlashPhase::Verifying, 0, 0, m);
            let info = self.wait_idle().await?;
            let bad = self.bad_chunks(image).await?;
            if info.last_error == 0 && bad.is_empty() {
                return Ok(());
            }
            let mut why = String::new();
            if info.last_error != 0 {
                why = format!("{}; ", boot_err::text(info.last_error));
            }
            let list: Vec<String> = bad.iter().map(|(a, _)| format!("0x{a:04X}")).collect();
            why.push_str(&format!(
                "{} chunk(s) differ: {}",
                bad.len(),
                list.join(", ")
            ));
            if pass == 2 {
                return Err(Error::Flash(format!("{why} - {STAYS_ACTIVE}")));
            }
            let pages: Vec<u16> = if bad.is_empty() {
                image.page_plan(false)[1..]
                    .iter()
                    .map(|(a, _)| *a)
                    .collect()
            } else {
                let mut p: Vec<u16> = bad
                    .iter()
                    .flat_map(|&(a, n)| (a..a + n).map(|x| x - x % PAGE))
                    .collect();
                p.sort_unstable();
                p.dedup();
                p
            };
            let plan: Vec<(u16, Vec<u8>)> = pages.iter().map(|&a| (a, image.page(a))).collect();
            let m = format!("{why} - rewriting {} page(s)", plan.len());
            self.progress
                .set(FlashPhase::Rewriting, 0, plan.len() as u32, m);
            self.write_pages(&plan, FlashPhase::Rewriting).await?;
        }
        Ok(())
    }

    /// BOOT_FINISH and the check that the new app runs. Returns (DEVICE_INFO version, label).
    async fn finish(&mut self, image: &TubeImage) -> Result<(u32, String)> {
        self.progress
            .set(FlashPhase::Finishing, 0, 0, "BOOT_FINISH");
        match self.set(pids::PID_BOOT_FINISH, &[], 2).await {
            Ok(_) => {}
            Err(Error::Cancelled) => return Err(Error::Cancelled),
            Err(e) => return Err(Error::Flash(format!("BOOT_FINISH: {e} - {STAYS_ACTIVE}"))),
        }
        let m = "FINISH ok, app starts (self-test ~2 s)";
        self.progress.set(FlashPhase::Confirming, 0, 0, m);
        pause(self.progress, self.opts.finish_wait).await?;
        let label = match self.get(pid::SOFTWARE_VERSION_LABEL, &[], 4).await {
            Ok(pd) => decode_label(&pd),
            Err(e) if e.is_timeout() => {
                return Err(Error::Flash(
                    "no answer after FINISH - app does not run?".into(),
                ))
            }
            Err(e) => return Err(e),
        };
        if label.starts_with("tube_boot_rdm") {
            let err = match self.boot_info(2).await {
                Ok(i) => boot_err::text(i.last_error).to_string(),
                Err(e) => format!("unknown ({e})"),
            };
            return Err(Error::Flash(format!(
                "still in the bootloader ({label}), last error: {err} - {STAYS_ACTIVE}"
            )));
        }
        let di = self.get(pid::DEVICE_INFO, &[], 2).await?;
        let want = image.trailer.version;
        match device_info_sw(&di) {
            Some(sw) if sw == want => Ok((sw, label)),
            sw => Err(Error::Flash(format!(
                "app runs ('{label}') but DEVICE_INFO reports {}, image 0x{want:08X}",
                sw.map_or("no version".to_string(), |v| format!("0x{v:08X}"))
            ))),
        }
    }
}

/// Run a tube update against `uid` through `link`.
///
/// Errors before the first page write end in "nothing written"; errors after it leave the tube in its bootloader
/// and end in "bootloader stays active, run again" (the bootloader starts the old app after ~60 s only if that one
/// is still valid, which it is not once a page was written). On cancellation after the first write the last
/// progress message before the final `Cancelled` says that the tube stays in its bootloader.
pub async fn flash_tube<L: RdmLink>(
    link: &L,
    uid: Uid,
    image: &TubeImage,
    opts: &TubeFlashOptions,
    progress: &Progress,
) -> Result<FlashReport> {
    progress.check()?;
    let mut run = Run {
        link,
        uid,
        opts,
        progress,
        busy: 0,
        written: 0,
    };
    let checked = match opts.rescue {
        Some(_) => None,
        None => run.app_precheck(image).await?,
    };
    let info = run.enter().await?;
    // the bootloader's own ids, always; the version from its trailer only if the app did not answer
    let ids = check_tube_ids(
        image,
        info.manufacturer,
        info.model,
        "BOOT_INFO (bootloader)",
    )
    .and_then(|_| {
        if checked.is_some() {
            return Ok(());
        }
        let installed = (info.app_valid != 0).then_some(info.app_fw_version);
        check_tube_version(image, installed, opts.allow_downgrade)
    });
    if let Err(e) = ids {
        return Err(match e {
            Error::Flash(m) => Error::Flash(format!(
                "{m} (the bootloader starts its app after ~60 s if that one is valid)"
            )),
            e => e,
        });
    }
    let old_version = checked.or((info.app_valid != 0).then_some(info.app_fw_version));

    let r = async {
        let plan = image.page_plan(true);
        let m = format!("writing {} pages (all-0xFF pages skipped)", plan.len());
        run.progress
            .set(FlashPhase::Writing, 0, plan.len() as u32, m);
        run.write_pages(&plan, FlashPhase::Writing).await?;
        run.verify(image).await?;
        run.finish(image).await
    }
    .await;
    let (new_version, version_label) = match r {
        Ok(v) => v,
        Err(Error::Cancelled) => {
            let p = run.progress.current();
            let m = if p.phase == FlashPhase::Confirming {
                format!("cancelled after BOOT_FINISH - tube {uid} should run the new app")
            } else {
                format!("cancelled - tube {uid} stays in its bootloader, run the update again")
            };
            run.progress.set(p.phase, p.done, p.total, m);
            return Err(Error::Cancelled);
        }
        Err(e) => return Err(e),
    };
    Ok(FlashReport {
        old_version,
        new_version,
        version_label,
        units_written: run.written,
        busy_retries: run.busy,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crc::crc16;
    use crate::flash::{FlashJob, FlashProgress};
    use crate::image::tube::{build_test_image, trailer_check, Trailer, CRC_CHUNK};
    use crate::rdm::pd::DeviceInfo;
    use std::future::Future;
    use std::sync::{Arc, Mutex};
    use tokio::sync::watch;

    const UID: Uid = Uid::new(0x7FF0, 0xB066_75AA);
    const NODE: Uid = Uid::new(0x7FF0, 0x8012_3456);

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum Mode {
        App,
        Boot,
        /// App dead, bootloader not active: no answers until BOOT_CATCH catches it.
        Dead,
    }

    /// Port of `flash_rdm.py FakeTube`: tube_boot_rdm v3 (+ app) with BUFFER_FULL busy after WRITE_PAGE, lost
    /// answers, chunked APP_CRC, trailer check on FINISH, BOOT_INFO with the cached state of the installed app.
    /// Requests to `NODE` emulate the node's BOOT_CATCH.
    struct Tube {
        uid: Uid,
        flash: Vec<u8>,
        mode: Mode,
        manufacturer: u16,
        model: u16,
        app_version: u32,
        busy: u32,
        err: u8,
        n: u32,
        lose_every: u32,
        writes: Vec<u16>,
        timers: u32,
        app_st: (bool, u32),
        boot_version: u8,
        /// FINISH fails with this error even for a valid trailer.
        finish_error: Option<u8>,
        /// BOOT_CATCH: CAUGHT after this many status polls, `None` = TIMEOUT after 3.
        catch_after: Option<u32>,
        catch_state: u8,
        catch_polls: u32,
        aborts: u32,
        cancel: Option<watch::Sender<bool>>,
        cancel_at_write: Option<usize>,
        cancel_at_poll: Option<u32>,
    }

    struct FakeTube(Mutex<Tube>);

    type Answer = Option<std::result::Result<Vec<u8>, u16>>;

    impl Tube {
        fn new(uid: Uid) -> Self {
            let mut t = Self {
                uid,
                flash: vec![0xFF; usize::from(BOOT_START)],
                mode: Mode::App,
                manufacturer: uid.manufacturer,
                model: 1,
                app_version: 0x301,
                busy: 0,
                err: 0,
                n: 0,
                lose_every: 0,
                writes: vec![],
                timers: 0,
                app_st: (false, 0),
                boot_version: 3,
                finish_error: None,
                catch_after: Some(3),
                catch_state: pids::boot_catch_state::IDLE,
                catch_polls: 0,
                aborts: 0,
                cancel: None,
                cancel_at_write: None,
                cancel_at_poll: None,
            };
            t.snapshot();
            t
        }

        fn garbage(mut self, page: usize) -> Self {
            for (i, b) in self.flash[page..page + 64].iter_mut().enumerate() {
                *b = i as u8;
            }
            self
        }

        fn check(&self) -> u8 {
            trailer_check(&self.flash, Some(self.manufacturer), Some(self.model))
        }

        fn valid(&self) -> bool {
            self.check() == boot_err::NONE
        }

        fn snapshot(&mut self) -> u8 {
            let e = self.check();
            self.app_st = (e == boot_err::NONE, Trailer::parse(&self.flash).version);
            e
        }

        fn fire_cancel(&self) {
            if let Some(c) = &self.cancel {
                c.send_replace(true);
            }
        }

        fn device_info(&self, sw: u32) -> Vec<u8> {
            DeviceInfo {
                rdm_protocol_version: 0x0100,
                device_model_id: self.model,
                product_category: 0x0101,
                software_version_id: sw,
                dmx_footprint: 12,
                current_personality: 1,
                personality_count: 4,
                dmx_start_address: 1,
                sub_device_count: 0,
                sensor_count: 0,
            }
            .encode()
        }

        fn node(&mut self, cmd: u8, p: u16, pd: &[u8]) -> Answer {
            use pids::boot_catch_state as st;
            match (cmd, p) {
                (cc::SET, pids::PID_BOOT_CATCH_START) => {
                    let s = BootCatchStart::decode(pd).ok()?;
                    if s.target_uid != self.uid || !(1..=4).contains(&s.port) {
                        return Some(Err(NackReason::DATA_OUT_OF_RANGE.0));
                    }
                    self.catch_state = st::RUNNING;
                    self.catch_polls = 0;
                    Some(Ok(vec![]))
                }
                (cc::GET, pids::PID_BOOT_CATCH_STATUS) => {
                    if self.catch_state == st::RUNNING {
                        self.catch_polls += 1;
                        if Some(self.catch_polls) == self.cancel_at_poll {
                            self.fire_cancel();
                        }
                        match self.catch_after {
                            Some(k) if self.catch_polls >= k => {
                                self.catch_state = st::CAUGHT;
                                self.mode = Mode::Boot;
                                self.snapshot();
                            }
                            None if self.catch_polls >= 3 => self.catch_state = st::TIMEOUT,
                            _ => {}
                        }
                    }
                    let s = BootCatchStatus {
                        state: self.catch_state,
                        port: 1,
                        target_uid: self.uid,
                        elapsed_ms: self.catch_polls * 250,
                        attempts: (self.catch_polls * 12) as u16,
                    };
                    Some(Ok(s.encode()))
                }
                (cc::SET, pids::PID_BOOT_CATCH_ABORT) => {
                    self.aborts += 1;
                    self.catch_state = st::ABORTED;
                    Some(Ok(vec![]))
                }
                _ => Some(Err(NackReason::UNKNOWN_PID.0)),
            }
        }

        fn transact(&mut self, uid: Uid, cmd: u8, p: u16, pd: &[u8]) -> Answer {
            if uid == NODE {
                return self.node(cmd, p, pd);
            }
            self.n += 1;
            if uid != self.uid || (self.lose_every != 0 && self.n.is_multiple_of(self.lose_every)) {
                return None;
            }
            let nack = |r: NackReason| Some(Err(r.0));
            if self.busy > 0 {
                self.busy -= 1;
            }
            let page = usize::from(PAGE);
            match self.mode {
                Mode::Dead => None,
                Mode::App => match (cmd, p) {
                    (cc::SET, pids::PID_ENTER_BOOTLOADER) if pd == b"BOOT" => {
                        self.mode = Mode::Boot;
                        self.snapshot();
                        Some(Ok(vec![]))
                    }
                    (cc::GET, pid::SOFTWARE_VERSION_LABEL) => {
                        Some(Ok(
                            format!("tube_fw {}", version_str(self.app_version)).into_bytes()
                        ))
                    }
                    (cc::GET, pid::DEVICE_INFO) => Some(Ok(self.device_info(self.app_version))),
                    _ => nack(NackReason::UNKNOWN_PID),
                },
                Mode::Boot => match (cmd, p) {
                    (cc::GET, pids::PID_BOOT_INFO) => {
                        let mut out = BootInfo {
                            version: self.boot_version,
                            page_size: PAGE as u8,
                            app_end: BOOT_START,
                            busy: u8::from(self.busy > 0),
                            last_error: self.err,
                            manufacturer: self.manufacturer,
                            model: self.model,
                            boot_sw_version: pids::tube::BOOT_RDM_SW_VERSION_ID,
                            app_valid: u8::from(self.app_st.0),
                            app_fw_version: self.app_st.1,
                        }
                        .encode();
                        if self.boot_version < 3 {
                            out.truncate(14);
                        }
                        self.err = 0;
                        Some(Ok(out))
                    }
                    (cc::GET, pid::DEVICE_INFO) => {
                        Some(Ok(self.device_info(pids::tube::BOOT_RDM_SW_VERSION_ID)))
                    }
                    (cc::SET, pids::PID_BOOT_WRITE_PAGE) => {
                        if pd.len() != 2 + page {
                            return nack(NackReason::FORMAT_ERROR);
                        }
                        let a = usize::from(u16::from_be_bytes([pd[0], pd[1]]));
                        if a >= usize::from(BOOT_START) || a % page != 0 {
                            return nack(NackReason::DATA_OUT_OF_RANGE);
                        }
                        if self.busy > 0 {
                            self.timers += 1;
                            return nack(NackReason::BUFFER_FULL);
                        }
                        self.flash[a..a + page].copy_from_slice(&pd[2..]);
                        self.writes.push(a as u16);
                        self.app_st.0 = false;
                        self.busy = 2;
                        if Some(self.writes.len()) == self.cancel_at_write {
                            self.fire_cancel();
                        }
                        Some(Ok(vec![]))
                    }
                    (cc::GET, pids::PID_BOOT_APP_CRC) => {
                        if pd.len() != 4 {
                            return nack(NackReason::FORMAT_ERROR);
                        }
                        let a = usize::from(u16::from_be_bytes([pd[0], pd[1]]));
                        let n = usize::from(u16::from_be_bytes([pd[2], pd[3]]));
                        if n > usize::from(CRC_CHUNK) || a + n > usize::from(BOOT_START) {
                            return nack(NackReason::DATA_OUT_OF_RANGE);
                        }
                        if self.busy > 0 {
                            return nack(NackReason::BUFFER_FULL);
                        }
                        Some(Ok(crc16(&self.flash[a..a + n]).to_be_bytes().to_vec()))
                    }
                    (cc::SET, pids::PID_BOOT_FINISH) => {
                        if self.busy > 0 {
                            return nack(NackReason::BUFFER_FULL);
                        }
                        self.err = self.snapshot();
                        if let Some(e) = self.finish_error {
                            self.err = e;
                        }
                        if self.err == boot_err::NONE {
                            self.mode = Mode::App;
                            self.app_version = Trailer::parse(&self.flash).version;
                        }
                        Some(Ok(vec![]))
                    }
                    (cc::GET, pid::SOFTWARE_VERSION_LABEL) => {
                        Some(Ok(
                            format!("tube_boot_rdm {}", self.boot_version).into_bytes()
                        ))
                    }
                    _ => nack(NackReason::UNKNOWN_PID),
                },
            }
        }
    }

    impl FakeTube {
        fn new(t: Tube) -> Arc<Self> {
            Arc::new(Self(Mutex::new(t)))
        }

        fn with<R>(&self, f: impl FnOnce(&mut Tube) -> R) -> R {
            f(&mut self.0.lock().unwrap())
        }
    }

    /// The `RdmLink` contract: retries on no answer, BUFFER_FULL retried `buffer_full_retries` times.
    impl RdmLink for FakeTube {
        fn rdm(
            &self,
            uid: Uid,
            command_class: u8,
            p: u16,
            data: Vec<u8>,
            opts: RdmOptions,
        ) -> impl Future<Output = Result<Vec<u8>>> + Send {
            let attempts = 1 + opts.retries.unwrap_or(2);
            let mut busy_left = opts.buffer_full_retries;
            let mut lost = 0;
            let r = loop {
                match self.with(|t| t.transact(uid, command_class, p, &data)) {
                    None => {
                        lost += 1;
                        if lost >= attempts {
                            break Err(Error::Timeout {
                                uid,
                                pid: p,
                                attempts,
                            });
                        }
                    }
                    Some(Ok(pd)) => break Ok(pd),
                    Some(Err(r)) if r == NackReason::BUFFER_FULL.0 && busy_left > 0 => {
                        busy_left -= 1
                    }
                    Some(Err(r)) => {
                        break Err(Error::Nack {
                            uid,
                            pid: p,
                            reason: NackReason(r),
                        })
                    }
                }
            };
            async move { r }
        }
    }

    fn app() -> Vec<u8> {
        let mut a: Vec<u8> = (0..1001u32).map(|i| (i * 7) as u8).collect();
        a.extend(std::iter::repeat_n(0xFF, 199));
        a.extend(std::iter::repeat_n(0x5A, 101));
        a
    }

    fn image(version: u32) -> TubeImage {
        build_test_image(&app(), 0x7FF0, 1, version)
    }

    /// Run as a job (so cancellation works); returns the result and every progress update.
    async fn run(
        tube: &Arc<FakeTube>,
        image: TubeImage,
        opts: TubeFlashOptions,
    ) -> (Result<FlashReport>, Vec<FlashProgress>) {
        let seen = Arc::new(Mutex::new(Vec::new()));
        let s2 = seen.clone();
        let link = tube.clone();
        let uid = tube.with(|t| t.uid);
        let job = FlashJob::spawn(
            1,
            Some(Arc::new(move |p: &FlashProgress| {
                s2.lock().unwrap().push(p.clone())
            })),
            move |p| async move { flash_tube(&*link, uid, &image, &opts, &p).await },
        );
        tube.with(|t| t.cancel = Some(job.canceller()));
        let r = job.wait().await;
        let seen = seen.lock().unwrap().clone();
        (r, seen)
    }

    fn err_text(r: &Result<FlashReport>) -> String {
        match r {
            Ok(_) => "ok".into(),
            Err(e) => e.to_string(),
        }
    }

    #[tokio::test(start_paused = true)]
    async fn happy_path_lost_answers_and_second_pass() {
        let im = image(0x400);
        let len = usize::from(im.length());
        for (lose, garbage) in [(0, None), (7, None), (0, Some(1088))] {
            let mut t = Tube::new(UID);
            t.flash = build_test_image(&[0x11; 3000], 0x7FF0, 1, 0x301).img;
            t.lose_every = lose;
            if let Some(g) = garbage {
                t = t.garbage(g);
            }
            t.snapshot();
            let tube = FakeTube::new(t);
            let (r, seen) = run(&tube, im.clone(), TubeFlashOptions::default()).await;
            let rep = r.unwrap_or_else(|e| panic!("lose={lose} garbage={garbage:?}: {e}"));
            tube.with(|t| {
                assert_eq!(t.mode, Mode::App);
                assert!(t.valid());
                assert_eq!(t.flash[..len], im.img[..len]);
                assert_eq!(t.app_version, 0x400);
                assert!(t.timers > 0, "BUFFER_FULL seen");
                assert_eq!(rep.units_written as usize, t.writes.len());
                if garbage.is_some() {
                    assert_eq!(t.writes.iter().filter(|&&a| a == 1088).count(), 1);
                    assert!(seen.iter().any(|p| p.phase == FlashPhase::Rewriting));
                }
            });
            assert_eq!(rep.old_version, Some(0x301));
            assert_eq!(rep.new_version, 0x400);
            assert_eq!(rep.version_label, "tube_fw 0.4.0");
            assert!(rep.busy_retries > 0);
            let phases: Vec<FlashPhase> = seen.iter().map(|p| p.phase).collect();
            for ph in [
                FlashPhase::Checking,
                FlashPhase::EnteringBootloader,
                FlashPhase::Writing,
                FlashPhase::Verifying,
                FlashPhase::Finishing,
                FlashPhase::Confirming,
                FlashPhase::Done,
            ] {
                assert!(phases.contains(&ph), "{ph:?} missing");
            }
            let last_write = seen
                .iter()
                .rfind(|p| p.phase == FlashPhase::Writing)
                .unwrap();
            assert_eq!(last_write.done, last_write.total);
            assert_eq!(last_write.total as usize, im.page_plan(true).len());
        }
    }

    #[tokio::test(start_paused = true)]
    async fn wrong_device_refused_without_writes() {
        let tube = FakeTube::new(Tube {
            model: 2,
            ..Tube::new(UID)
        });
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        let e = err_text(&r);
        assert!(
            e.contains("wrong device") && e.contains("DEVICE_INFO"),
            "{e}"
        );
        assert!(e.contains("nothing written"), "{e}");
        tube.with(|t| assert!(t.writes.is_empty() && t.mode == Mode::App));

        let tube = FakeTube::new(Tube::new(Uid::new(0x1234, 0xB066_75AA)));
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        assert!(err_text(&r).contains("wrong device"), "{}", err_text(&r));
        tube.with(|t| assert!(t.writes.is_empty() && t.mode == Mode::App));

        // already in the bootloader (no app): its BOOT_INFO ids decide
        let tube = FakeTube::new(Tube {
            model: 2,
            mode: Mode::Boot,
            ..Tube::new(UID)
        });
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        let e = err_text(&r);
        assert!(e.contains("BOOT_INFO") && e.contains("wrong device"), "{e}");
        tube.with(|t| assert!(t.writes.is_empty()));
    }

    #[tokio::test(start_paused = true)]
    async fn downgrade() {
        let tube = FakeTube::new(Tube {
            app_version: 0x500,
            ..Tube::new(UID)
        });
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        assert!(
            err_text(&r).contains("downgrade 0.5.0 -> 0.4.0"),
            "{}",
            err_text(&r)
        );
        tube.with(|t| assert!(t.writes.is_empty() && t.mode == Mode::App));

        let tube = FakeTube::new(Tube {
            app_version: 0x500,
            ..Tube::new(UID)
        });
        let opts = TubeFlashOptions {
            allow_downgrade: true,
            ..Default::default()
        };
        let (r, _) = run(&tube, image(0x400), opts).await;
        assert_eq!(r.unwrap().old_version, Some(0x500));
        tube.with(|t| assert_eq!(t.app_version, 0x400));

        // bootloader active: the version comes from the installed trailer (BOOT_INFO)
        let mut t = Tube::new(UID);
        t.mode = Mode::Boot;
        t.flash = image(0x501).img;
        t.snapshot();
        let tube = FakeTube::new(t);
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        let e = err_text(&r);
        assert!(e.contains("downgrade") && e.contains("~60 s"), "{e}");
        tube.with(|t| assert!(t.writes.is_empty()));

        // same in the bootloader, newer image: ok, old version from BOOT_INFO
        let mut t = Tube::new(UID);
        t.mode = Mode::Boot;
        t.flash = image(0x301).img;
        t.snapshot();
        let tube = FakeTube::new(t);
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        assert_eq!(r.unwrap().old_version, Some(0x301));
    }

    #[tokio::test(start_paused = true)]
    async fn finish_failure_reports_bootloader_error() {
        let tube = FakeTube::new(Tube {
            finish_error: Some(boot_err::MODEL),
            ..Tube::new(UID)
        });
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        let e = err_text(&r);
        assert!(
            e.contains("still in the bootloader (tube_boot_rdm 3)"),
            "{e}"
        );
        assert!(e.contains(boot_err::text(boot_err::MODEL)), "{e}");
        assert!(e.contains("run again"), "{e}");
        tube.with(|t| assert_eq!(t.mode, Mode::Boot));
    }

    #[tokio::test(start_paused = true)]
    async fn old_bootloader_refused() {
        let tube = FakeTube::new(Tube {
            boot_version: 1,
            mode: Mode::Boot,
            ..Tube::new(UID)
        });
        let (r, _) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        assert!(
            err_text(&r).contains("Install tube_boot_rdm v3"),
            "{}",
            err_text(&r)
        );
        tube.with(|t| assert!(t.writes.is_empty()));
        assert!(parse_boot_info(&[3, 64, 0x18]).is_err());
    }

    #[tokio::test(start_paused = true)]
    async fn cancel_mid_write() {
        let tube = FakeTube::new(Tube {
            cancel_at_write: Some(5),
            ..Tube::new(UID)
        });
        let (r, seen) = run(&tube, image(0x400), TubeFlashOptions::default()).await;
        assert!(matches!(r, Err(Error::Cancelled)), "{}", err_text(&r));
        tube.with(|t| {
            assert_eq!(t.writes.len(), 5);
            assert_eq!(t.mode, Mode::Boot);
        });
        assert!(seen
            .iter()
            .any(|p| p.message.contains("stays in its bootloader")));
        assert_eq!(seen.last().unwrap().phase, FlashPhase::Cancelled);
        assert_eq!(seen.last().unwrap().done, 5);
    }

    #[tokio::test(start_paused = true)]
    async fn rescue_via_boot_catch() {
        let rescue = Some(RescueTarget {
            node_uid: NODE,
            port: 1,
        });
        let opts = TubeFlashOptions {
            rescue,
            ..Default::default()
        };
        let tube = FakeTube::new(Tube {
            mode: Mode::Dead,
            ..Tube::new(UID)
        });
        let (r, seen) = run(&tube, image(0x400), opts.clone()).await;
        let rep = r.unwrap();
        assert_eq!(rep.old_version, None);
        tube.with(|t| {
            assert_eq!(t.mode, Mode::App);
            assert_eq!(t.catch_polls, 3);
            assert_eq!(t.aborts, 0);
        });
        assert!(seen
            .iter()
            .any(|p| p.phase == FlashPhase::Catching && p.message.contains("power-cycle")));

        let tube = FakeTube::new(Tube {
            mode: Mode::Dead,
            catch_after: None,
            ..Tube::new(UID)
        });
        let (r, _) = run(&tube, image(0x400), opts.clone()).await;
        let e = err_text(&r);
        assert!(
            e.contains("BOOT_CATCH") && e.contains("nothing written"),
            "{e}"
        );
        tube.with(|t| assert!(t.writes.is_empty()));

        // our own deadline
        let tube = FakeTube::new(Tube {
            mode: Mode::Dead,
            catch_after: Some(1000),
            ..Tube::new(UID)
        });
        let short = TubeFlashOptions {
            catch_timeout: Duration::from_secs(2),
            ..opts.clone()
        };
        let (r, _) = run(&tube, image(0x400), short).await;
        assert!(
            err_text(&r).contains("not caught within"),
            "{}",
            err_text(&r)
        );
        tube.with(|t| assert_eq!(t.aborts, 1));

        // cancelled while catching: BOOT_CATCH_ABORT sent
        let tube = FakeTube::new(Tube {
            mode: Mode::Dead,
            catch_after: Some(100),
            cancel_at_poll: Some(2),
            ..Tube::new(UID)
        });
        let (r, _) = run(&tube, image(0x400), opts).await;
        assert!(matches!(r, Err(Error::Cancelled)));
        tube.with(|t| {
            assert_eq!(t.aborts, 1);
            assert!(t.writes.is_empty());
        });
    }

    #[test]
    fn preview_checks() {
        let im = image(0x400);
        assert!(check_tube_image_for_device(&im, 0x7FF0, 1, Some(0x301), false).is_ok());
        assert!(check_tube_image_for_device(&im, 0x7FF0, 2, None, false).is_err());
        assert!(check_tube_image_for_device(&im, 0x7FF0, 1, Some(0x500), false).is_err());
        assert!(check_tube_image_for_device(&im, 0x7FF0, 1, Some(0x500), true).is_ok());
        assert!(is_bootloader_version(0xB000_0003) && !is_bootloader_version(0x400));
    }

    #[tokio::test]
    async fn detached_progress_does_not_cancel() {
        let p = Progress::detached();
        assert!(pause(&p, Duration::from_millis(1)).await.is_ok());
    }
}
