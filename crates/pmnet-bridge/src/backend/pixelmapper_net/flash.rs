//! Tube firmware jobs (contract §6 `flash`, node contract §9.1) on top of `pixelmapper-net`'s flash flow.
//!
//! The library runs the update (`Controller::flash_tube_with` → `FlashJob`); this module owns the jobs (they
//! outlive the WebSocket connection that started them), checks image and device before a job starts (so
//! `invalidImage` / `deviceMismatch` / `downgrade` / `busy` are command errors), maps the library's progress onto
//! the contract's `FlashJob` (phases, pages, `inBootloader`) and throttles progress events to 10/s per job.

use super::convert::{error_text, flash_error_code};
use super::describe::Target;
use crate::backend::BackendError;
use crate::protocol::{
    FirmwareImageInfo, FlashError, FlashJob, FlashPhase, FlashResult, NodeId, PortRef,
};
use pixelmapper_net as net;
use pixelmapper_net::flash::tube::{is_bootloader_version, parse_boot_info};
use pixelmapper_net::flash::{FlashPhase as NetPhase, FlashProgress, FlashReport};
use pixelmapper_net::image::tube::Trailer;
use pixelmapper_net::image::TubeImage;
use pixelmapper_net::pids;
use pixelmapper_net::rdm::pd::DeviceInfo;
use pixelmapper_net::rdm::pid;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::time::{Duration, Instant};
use tokio::sync::watch;

/// Minimum time between two progress events of one job within a phase (contract: at most 10/s).
const PROGRESS_INTERVAL: Duration = Duration::from_millis(100);
/// Finished jobs stay listed this long (`flashJobs`).
const KEEP_FINISHED: Duration = Duration::from_secs(600);

// ── Image ─────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Parses and checks an Intel HEX tube image (`invalidImage` on failure).
pub(super) fn parse_image(hex: &str) -> Result<TubeImage, BackendError> {
    TubeImage::parse_hex(hex).map_err(|e| {
        BackendError::InvalidImage(match e {
            net::Error::Image(m) => m,
            e => e.to_string(),
        })
    })
}

/// The trailer as the contract reports it.
pub(super) fn image_info(image: &TubeImage) -> FirmwareImageInfo {
    let i = image.info();
    FirmwareImageInfo {
        format: image.trailer.format,
        manufacturer_id: i.manufacturer,
        model_id: i.model,
        version_id: i.version,
        version: i.version_label,
        length: u32::from(i.length),
    }
}

// ── Checks before the job ─────────────────────────────────────────────────────────────────────────────────────────

/// DEVICE_INFO of the running app must match the trailer (manufacturer of the UID, model) and must not be newer
/// than the image unless `allow_downgrade`. A tube that is already in its bootloader (DEVICE_INFO missing or
/// reporting the bootloader version) is checked with BOOT_INFO instead.
pub(super) async fn precheck(
    t: &Target<'_>,
    image: &TubeImage,
    allow_downgrade: bool,
) -> Result<(), BackendError> {
    let trailer = &image.trailer;
    match t.get(pid::DEVICE_INFO, &[]).await {
        Ok(raw) => {
            let di =
                DeviceInfo::decode(&raw).map_err(|e| BackendError::BadResponse(e.to_string()))?;
            if is_bootloader_version(di.software_version_id) {
                return boot_check(t, trailer, allow_downgrade).await;
            }
            check_ids(
                t.uid.manufacturer,
                di.device_model_id,
                trailer,
                "DEVICE_INFO",
            )?;
            check_version(di.software_version_id, trailer.version, allow_downgrade)
        }
        Err(e @ (BackendError::Timeout | BackendError::Nack(_))) => {
            match boot_check(t, trailer, allow_downgrade).await {
                // Neither app nor bootloader answered: report the DEVICE_INFO failure.
                Err(BackendError::Timeout | BackendError::Nack(_)) => Err(e),
                other => other,
            }
        }
        Err(e) => Err(e),
    }
}

/// BOOT_INFO of a tube in its bootloader.
async fn boot_check(
    t: &Target<'_>,
    trailer: &Trailer,
    allow_downgrade: bool,
) -> Result<(), BackendError> {
    let raw = t.get(pids::PID_BOOT_INFO, &[]).await?;
    let info = parse_boot_info(&raw).map_err(|e| BackendError::BadResponse(error_text(&e)))?;
    check_ids(info.manufacturer, info.model, trailer, "BOOT_INFO")?;
    if info.app_valid != 0 {
        check_version(info.app_fw_version, trailer.version, allow_downgrade)?;
    }
    Ok(())
}

/// `deviceMismatch` unless manufacturer and model match the trailer.
fn check_ids(
    manufacturer: u16,
    model: u16,
    trailer: &Trailer,
    source: &str,
) -> Result<(), BackendError> {
    if (manufacturer, model) == (trailer.manufacturer, trailer.model) {
        return Ok(());
    }
    Err(BackendError::DeviceMismatch(format!(
        "{source} reports manufacturer 0x{manufacturer:04X} model 0x{model:04X}, the image is for manufacturer \
         0x{:04X} model 0x{:04X}",
        trailer.manufacturer, trailer.model
    )))
}

/// `downgrade` when the device runs a newer version than the image.
fn check_version(current: u32, image: u32, allow_downgrade: bool) -> Result<(), BackendError> {
    if current > image && !allow_downgrade {
        return Err(BackendError::Downgrade { current, image });
    }
    Ok(())
}

// ── Job table ─────────────────────────────────────────────────────────────────────────────────────────────────────

/// One job.
struct Entry {
    job: FlashJob,
    lib_id: u64,
    cancel: watch::Sender<bool>,
    progress: watch::Receiver<FlashProgress>,
    finished: Option<Instant>,
    last_emit: Option<Instant>,
}

/// All jobs of the bridge, keyed by their number (`jobId` = `"f<number>"`).
#[derive(Default)]
pub(super) struct Jobs {
    entries: BTreeMap<u64, Entry>,
    by_lib: HashMap<u64, u64>,
    reserved: HashSet<(NodeId, PortRef)>,
    next: u64,
}

impl Jobs {
    /// Reserves (`node`, `port`) for a job about to start; `busy` while a job runs or starts there.
    pub(super) fn reserve(&mut self, node: &NodeId, port: PortRef) -> Result<(), BackendError> {
        let running = self
            .entries
            .values()
            .any(|e| e.job.node_id == *node && e.job.port == port && !e.job.phase.is_terminal());
        if running || !self.reserved.insert((node.clone(), port)) {
            return Err(BackendError::Busy);
        }
        Ok(())
    }

    /// Releases a reservation.
    pub(super) fn release(&mut self, node: &NodeId, port: PortRef) {
        self.reserved.remove(&(node.clone(), port));
    }

    /// Registers a started library job; returns the new contract job and its number.
    pub(super) fn insert(
        &mut self,
        template: FlashJob,
        job: &net::flash::FlashJob,
    ) -> (u64, FlashJob) {
        self.next += 1;
        let n = self.next;
        let fj = FlashJob {
            job_id: format!("f{n}"),
            ..template
        };
        self.by_lib.insert(job.id(), n);
        self.entries.insert(
            n,
            Entry {
                job: fj.clone(),
                lib_id: job.id(),
                cancel: job.canceller(),
                progress: job.progress(),
                finished: None,
                last_emit: Some(Instant::now()),
            },
        );
        (n, fj)
    }

    /// Applies a library progress update; returns the job when an event is due.
    pub(super) fn on_progress(&mut self, p: &FlashProgress, now: Instant) -> Option<FlashJob> {
        let n = *self.by_lib.get(&p.job_id)?;
        let e = self.entries.get_mut(&n)?;
        if e.job.phase.is_terminal() {
            return None;
        }
        let changed = apply_progress(&mut e.job, p)?;
        let due = e
            .last_emit
            .is_none_or(|t| now.duration_since(t) >= PROGRESS_INTERVAL);
        if changed || due {
            e.last_emit = Some(now);
            Some(e.job.clone())
        } else {
            None
        }
    }

    /// The library job ended; returns the final contract job.
    pub(super) fn finish(
        &mut self,
        n: u64,
        result: Result<FlashReport, net::Error>,
        now: Instant,
    ) -> Option<FlashJob> {
        let e = self.entries.get_mut(&n)?;
        apply_result(&mut e.job, result);
        e.finished = Some(now);
        Some(e.job.clone())
    }

    /// Latest progress of every running job (after the event stream lagged); returns the changed jobs.
    pub(super) fn resync(&mut self, now: Instant) -> Vec<FlashJob> {
        let mut out = Vec::new();
        for e in self.entries.values_mut() {
            if e.job.phase.is_terminal() {
                continue;
            }
            let p = e.progress.borrow().clone();
            if apply_progress(&mut e.job, &p).is_some() {
                e.last_emit = Some(now);
                out.push(e.job.clone());
            }
        }
        out
    }

    /// Requests cancellation (`unknownJob` for unknown ids; a finished job stays as it is).
    pub(super) fn cancel(&self, job_id: &str) -> Result<(), BackendError> {
        let e = job_id
            .strip_prefix('f')
            .and_then(|n| n.parse::<u64>().ok())
            .and_then(|n| self.entries.get(&n))
            .ok_or_else(|| BackendError::UnknownJob(job_id.to_owned()))?;
        if !e.job.phase.is_terminal() {
            e.cancel.send_replace(true);
        }
        Ok(())
    }

    /// Running jobs and jobs that finished in the last 10 minutes.
    pub(super) fn list(&mut self, now: Instant) -> Vec<FlashJob> {
        let old: Vec<(u64, u64)> = self
            .entries
            .iter()
            .filter(|(_, e)| {
                e.finished
                    .is_some_and(|t| now.duration_since(t) > KEEP_FINISHED)
            })
            .map(|(n, e)| (*n, e.lib_id))
            .collect();
        for (n, lib) in old {
            self.entries.remove(&n);
            self.by_lib.remove(&lib);
        }
        self.entries.values().map(|e| e.job.clone()).collect()
    }
}

// ── Progress mapping ──────────────────────────────────────────────────────────────────────────────────────────────

/// Applies a library progress snapshot to the contract job. Returns `Some(phase changed)`, `None` for the
/// library's terminal phases (the job result sets those, with `result`/`error`).
fn apply_progress(job: &mut FlashJob, p: &FlashProgress) -> Option<bool> {
    let before = job.phase;
    match p.phase {
        NetPhase::Pending
        | NetPhase::Checking
        | NetPhase::EnteringBootloader
        | NetPhase::Catching => {
            job.phase = FlashPhase::Entering;
        }
        NetPhase::Writing => {
            job.phase = FlashPhase::Writing;
            job.pages_done = p.done;
            if p.total > 0 {
                job.pages_total = p.total;
            }
            job.in_bootloader = true;
        }
        NetPhase::Verifying => {
            job.phase = FlashPhase::Verifying;
            job.in_bootloader = true;
        }
        NetPhase::Rewriting => {
            // Pages of chunks whose APP_CRC differed are written again; part of the verification.
            job.phase = FlashPhase::Verifying;
            job.rewrites = p.done;
            job.in_bootloader = true;
        }
        NetPhase::Finishing => {
            job.phase = FlashPhase::Finishing;
            job.in_bootloader = true;
        }
        NetPhase::Confirming => {
            // BOOT_FINISH was acknowledged: the tube starts the new app.
            job.phase = FlashPhase::Confirming;
            job.in_bootloader = false;
        }
        NetPhase::Done | NetPhase::Failed | NetPhase::Cancelled => return None,
    }
    Some(job.phase != before)
}

/// Final state from the library job's result.
fn apply_result(job: &mut FlashJob, result: Result<FlashReport, net::Error>) {
    let was = job.phase;
    match result {
        Ok(report) => {
            job.phase = FlashPhase::Done;
            job.in_bootloader = false;
            job.result = Some(FlashResult {
                software_version_id: report.new_version,
                software_version_label: Some(report.version_label),
            });
        }
        Err(net::Error::Cancelled) => {
            job.phase = FlashPhase::Cancelled;
            if was == FlashPhase::Confirming {
                job.in_bootloader = false;
            }
        }
        Err(e) => {
            // The library flags failures that leave the tube in its bootloader (also those before the first
            // write, e.g. the bootloader's ids not fitting the image). Without the flag the tube is where the
            // progress put it, except after BOOT_FINISH: the app was started (or the state is unknown).
            let bootloader = e.flash_error().is_some_and(|f| f.bootloader_active);
            job.in_bootloader = bootloader || (was != FlashPhase::Confirming && job.in_bootloader);
            job.phase = FlashPhase::Failed;
            job.error = Some(FlashError {
                code: flash_error_code(&e),
                message: error_text(&e),
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{ErrorCode, RdmUid};
    use pixelmapper_net::image::tube::build_test_image;
    use pixelmapper_net::FlashErrorKind;

    fn job() -> FlashJob {
        let image = build_test_image(
            &[1, 2, 3, 4],
            pids::ESTA_MANUFACTURER,
            pids::MODEL_TUBE,
            0x0200,
        );
        FlashJob {
            job_id: String::new(),
            node_id: "7FF0:00000001".into(),
            port: PortRef::new(1, 0),
            uid: RdmUid::new(0x7FF0, 2),
            phase: FlashPhase::Entering,
            pages_done: 0,
            pages_total: 3,
            rewrites: 0,
            in_bootloader: false,
            image: image_info(&image),
            error: None,
            result: None,
        }
    }

    fn progress(phase: NetPhase, done: u32, total: u32) -> FlashProgress {
        FlashProgress {
            job_id: 9,
            phase,
            done,
            total,
            message: String::new(),
            error: None,
        }
    }

    #[test]
    fn image_info_from_trailer() {
        let image = build_test_image(
            &[1, 2, 3],
            pids::ESTA_MANUFACTURER,
            pids::MODEL_TUBE,
            0x0001_0200,
        );
        let i = image_info(&parse_image(&image.to_hex()).unwrap());
        assert_eq!((i.format, i.manufacturer_id, i.model_id), (1, 0x7FF0, 1));
        assert_eq!(
            (i.version_id, i.version.as_str(), i.length),
            (0x0001_0200, "1.2.0", 4)
        );
        assert!(matches!(
            parse_image("garbage"),
            Err(BackendError::InvalidImage(_))
        ));
    }

    #[test]
    fn phases_and_pages() {
        let mut j = job();
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Checking, 0, 0)),
            Some(false)
        );
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::EnteringBootloader, 0, 0)),
            Some(false)
        );
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Writing, 1, 4)),
            Some(true)
        );
        assert_eq!((j.pages_done, j.pages_total, j.in_bootloader), (1, 4, true));
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Writing, 4, 4)),
            Some(false)
        );
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Verifying, 1, 2)),
            Some(true)
        );
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Rewriting, 2, 2)),
            Some(false)
        );
        assert_eq!(j.rewrites, 2);
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Finishing, 0, 0)),
            Some(true)
        );
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Confirming, 0, 0)),
            Some(true)
        );
        assert!(!j.in_bootloader);
        assert_eq!(
            apply_progress(&mut j, &progress(NetPhase::Done, 0, 0)),
            None
        );
        let report = FlashReport {
            old_version: Some(0x100),
            new_version: 0x200,
            version_label: "0.2.0".into(),
            units_written: 4,
            busy_retries: 0,
        };
        apply_result(&mut j, Ok(report));
        assert_eq!(j.phase, FlashPhase::Done);
        assert_eq!(j.result.as_ref().unwrap().software_version_id, 0x200);
    }

    /// A library flash error of `kind`, optionally flagged "bootloader active".
    fn flash_err(kind: FlashErrorKind, message: &str, bootloader: bool) -> net::Error {
        let f = net::FlashError::new(kind, message);
        if bootloader {
            f.in_bootloader().into()
        } else {
            f.into()
        }
    }

    #[test]
    fn failures() {
        let mut j = job();
        apply_progress(&mut j, &progress(NetPhase::Writing, 1, 4));
        let msg = "WRITE_PAGE 0x0040: timeout - bootloader stays active, run again";
        apply_result(&mut j, Err(flash_err(FlashErrorKind::Timeout, msg, true)));
        assert_eq!(j.phase, FlashPhase::Failed);
        assert!(j.in_bootloader);
        let e = j.error.unwrap();
        assert_eq!(e.code, ErrorCode::Timeout);
        assert_eq!(e.message, msg, "without the library's kind tag");

        // Kinds without a contract code of their own.
        let mut j = job();
        apply_progress(&mut j, &progress(NetPhase::Verifying, 1, 4));
        apply_result(&mut j, Err(flash_err(FlashErrorKind::Verify, "x", true)));
        assert_eq!(j.error.unwrap().code, ErrorCode::BadResponse);

        // The bootloader's ids do not fit (ENTER_BOOTLOADER worked): in the bootloader before any write.
        let mut j = job();
        let mismatch = flash_err(FlashErrorKind::DeviceMismatch, "wrong device", true);
        apply_result(&mut j, Err(mismatch));
        assert!(j.in_bootloader);
        assert_eq!(j.error.unwrap().code, ErrorCode::DeviceMismatch);

        // After BOOT_FINISH: still in the bootloader only when the library says so.
        let mut j = job();
        apply_progress(&mut j, &progress(NetPhase::Confirming, 0, 0));
        let still = "still in the bootloader (tube_boot_rdm), last error: x";
        apply_result(
            &mut j,
            Err(flash_err(FlashErrorKind::Rejected, still, true)),
        );
        assert!(j.in_bootloader);
        let mut j = job();
        apply_progress(&mut j, &progress(NetPhase::Finishing, 0, 0));
        apply_progress(&mut j, &progress(NetPhase::Confirming, 0, 0));
        let silent = "no answer after FINISH - app does not run?";
        apply_result(
            &mut j,
            Err(flash_err(FlashErrorKind::Timeout, silent, false)),
        );
        assert!(!j.in_bootloader);
        assert_eq!(j.error.unwrap().code, ErrorCode::Timeout);

        // Errors that are not flash errors keep their own code and text.
        let mut j = job();
        apply_progress(&mut j, &progress(NetPhase::Writing, 1, 4));
        apply_result(&mut j, Err(net::Error::Closed));
        assert!(j.in_bootloader);
        let e = j.error.unwrap();
        assert_eq!(
            (e.code, e.message.as_str()),
            (ErrorCode::UdpUnavailable, "controller closed")
        );

        let mut j = job();
        apply_result(&mut j, Err(net::Error::Cancelled));
        assert_eq!((j.phase, j.in_bootloader), (FlashPhase::Cancelled, false));
    }

    #[test]
    fn version_and_ids() {
        let image = build_test_image(&[1, 2], pids::ESTA_MANUFACTURER, pids::MODEL_TUBE, 0x0200);
        assert!(check_ids(0x7FF0, 1, &image.trailer, "DEVICE_INFO").is_ok());
        assert!(matches!(
            check_ids(0x7FF0, 2, &image.trailer, "DEVICE_INFO"),
            Err(BackendError::DeviceMismatch(_))
        ));
        assert_eq!(
            check_version(0x0300, 0x0200, false),
            Err(BackendError::Downgrade {
                current: 0x0300,
                image: 0x0200
            })
        );
        assert!(check_version(0x0300, 0x0200, true).is_ok());
        assert!(check_version(0x0200, 0x0200, false).is_ok());
    }

    #[test]
    fn busy_and_cancel() {
        let mut jobs = Jobs::default();
        let node = NodeId::from("n");
        let port = PortRef::new(1, 0);
        jobs.reserve(&node, port).unwrap();
        assert_eq!(jobs.reserve(&node, port), Err(BackendError::Busy));
        assert!(jobs.reserve(&node, PortRef::new(2, 0)).is_ok());
        jobs.release(&node, port);
        assert!(jobs.reserve(&node, port).is_ok());
        assert_eq!(
            jobs.cancel("f99"),
            Err(BackendError::UnknownJob("f99".into()))
        );
        assert!(jobs.list(Instant::now()).is_empty());
    }
}
