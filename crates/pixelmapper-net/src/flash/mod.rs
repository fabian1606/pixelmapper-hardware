//! Firmware updates as cancellable background jobs with progress.
//!
//! [`tube`] updates LED tubes over RDM through a node port (`protocol/contract.md` §9.1, reference
//! `tube/tools/flash_rdm.py`), [`node`] updates the node itself via ArtFirmwareMaster or the FW_* PIDs (§9.2,
//! `protocol/image-format.md` §2). Both flows are written against [`crate::link`] traits.

pub mod node;
pub mod tube;

use crate::error::{Error, Result};
use serde::Serialize;
use std::future::Future;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::watch;
use tokio::task::JoinHandle;

/// Phase of an update.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum FlashPhase {
    /// Not started yet.
    Pending,
    /// Checking the image against the device (DEVICE_INFO, NODE_HW_REVISION, FW_INFO, ...).
    Checking,
    /// Tube: ENTER_BOOTLOADER sent, waiting for BOOT_INFO.
    EnteringBootloader,
    /// Tube rescue: BOOT_CATCH running, the user must power-cycle the tube now.
    Catching,
    /// Writing pages / blocks; `done`/`total` count them.
    Writing,
    /// Tube: comparing BOOT_APP_CRC chunks; node: device verifies.
    Verifying,
    /// Tube: rewriting pages whose CRC differs.
    Rewriting,
    /// BOOT_FINISH / FW_FINISH / FirmLast sent.
    Finishing,
    /// Waiting for the new firmware to come up and checking its version.
    Confirming,
    /// Finished successfully.
    Done,
    /// Failed (see `error`).
    Failed,
    /// Cancelled by the caller.
    Cancelled,
}

/// Snapshot of a job's progress.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FlashProgress {
    /// Job id (unique per controller).
    pub job_id: u64,
    /// Current phase.
    pub phase: FlashPhase,
    /// Units done in this phase (pages, blocks, chunks).
    pub done: u32,
    /// Units in this phase (0 = unknown).
    pub total: u32,
    /// Human-readable status line.
    pub message: String,
    /// Error once `phase == Failed`.
    pub error: Option<Error>,
}

/// Result of a successful update.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FlashReport {
    /// Firmware version id before the update, if known.
    pub old_version: Option<u32>,
    /// Firmware version id now running.
    pub new_version: u32,
    /// Software version label now running.
    pub version_label: String,
    /// Pages/blocks written (incl. rewrites).
    pub units_written: u32,
    /// NACK BUFFER_FULL / ACK_TIMER retries seen.
    pub busy_retries: u32,
}

/// Callback receiving every progress update of a job.
pub type Observer = Arc<dyn Fn(&FlashProgress) + Send + Sync>;

/// Handle a flow uses to report progress and to observe cancellation. Cheap to clone.
#[derive(Clone)]
pub struct Progress {
    job_id: u64,
    tx: Arc<watch::Sender<FlashProgress>>,
    cancel: watch::Receiver<bool>,
    observer: Option<Observer>,
}

impl Progress {
    /// Report a new phase/state.
    pub fn set(&self, phase: FlashPhase, done: u32, total: u32, message: impl Into<String>) {
        let p = FlashProgress {
            job_id: self.job_id,
            phase,
            done,
            total,
            message: message.into(),
            error: None,
        };
        self.publish(p);
    }

    fn publish(&self, p: FlashProgress) {
        if let Some(o) = &self.observer {
            o(&p);
        }
        self.tx.send_replace(p);
    }

    /// Current snapshot.
    pub fn current(&self) -> FlashProgress {
        self.tx.borrow().clone()
    }

    /// Whether cancellation was requested.
    pub fn is_cancelled(&self) -> bool {
        *self.cancel.borrow()
    }

    /// `Err(Error::Cancelled)` if cancellation was requested.
    pub fn check(&self) -> Result<()> {
        if self.is_cancelled() {
            Err(Error::Cancelled)
        } else {
            Ok(())
        }
    }

    /// Sleep, returning early with `Err(Cancelled)` on cancellation.
    pub async fn sleep(&self, d: Duration) -> Result<()> {
        let mut c = self.cancel.clone();
        tokio::select! {
            _ = tokio::time::sleep(d) => self.check(),
            r = async { c.wait_for(|v| *v).await.is_ok() } => match r {
                true => Err(Error::Cancelled),
                // sender gone: nobody can cancel any more
                false => {
                    tokio::time::sleep(d).await;
                    Ok(())
                }
            },
        }
    }

    /// Progress handle that is not connected to a job (tests, CLI without job).
    pub fn detached() -> Self {
        let (tx, _) = watch::channel(initial(0));
        let (_ctx, cancel) = watch::channel(false);
        Self {
            job_id: 0,
            tx: Arc::new(tx),
            cancel,
            observer: None,
        }
    }
}

fn initial(job_id: u64) -> FlashProgress {
    FlashProgress {
        job_id,
        phase: FlashPhase::Pending,
        done: 0,
        total: 0,
        message: String::new(),
        error: None,
    }
}

/// A running update. Dropping the handle does not stop the job; call [`FlashJob::cancel`].
pub struct FlashJob {
    id: u64,
    rx: watch::Receiver<FlashProgress>,
    cancel: watch::Sender<bool>,
    handle: JoinHandle<Result<FlashReport>>,
}

impl FlashJob {
    /// Spawn `flow` on the tokio runtime. `observer` is called for every progress update (e.g. to forward it as
    /// an event). The final phase (`Done`, `Failed`, `Cancelled`) is set here.
    pub fn spawn<F, Fut>(id: u64, observer: Option<Observer>, flow: F) -> Self
    where
        F: FnOnce(Progress) -> Fut,
        Fut: Future<Output = Result<FlashReport>> + Send + 'static,
    {
        let (tx, rx) = watch::channel(initial(id));
        let (ctx, crx) = watch::channel(false);
        let p = Progress {
            job_id: id,
            tx: Arc::new(tx),
            cancel: crx,
            observer,
        };
        let fut = flow(p.clone());
        let handle = tokio::spawn(async move {
            let r = fut.await;
            let last = p.current();
            let fin = match &r {
                Ok(rep) => FlashProgress {
                    phase: FlashPhase::Done,
                    message: format!("done: {}", rep.version_label),
                    error: None,
                    ..last
                },
                Err(Error::Cancelled) => FlashProgress {
                    phase: FlashPhase::Cancelled,
                    message: if last.message.is_empty() {
                        "cancelled".into()
                    } else {
                        format!("cancelled: {}", last.message)
                    },
                    error: Some(Error::Cancelled),
                    ..last
                },
                Err(e) => FlashProgress {
                    phase: FlashPhase::Failed,
                    message: e.to_string(),
                    error: Some(e.clone()),
                    ..last
                },
            };
            p.publish(fin);
            r
        });
        Self {
            id,
            rx,
            cancel: ctx,
            handle,
        }
    }

    /// Job id.
    pub fn id(&self) -> u64 {
        self.id
    }

    /// Progress receiver (latest snapshot + change notification).
    pub fn progress(&self) -> watch::Receiver<FlashProgress> {
        self.rx.clone()
    }

    /// Request cancellation. The flow stops at the next safe point; a tube may stay in its bootloader.
    pub fn cancel(&self) {
        self.cancel.send_replace(true);
    }

    /// A clonable handle that cancels this job.
    pub fn canceller(&self) -> watch::Sender<bool> {
        self.cancel.clone()
    }

    /// Whether the job has finished.
    pub fn is_finished(&self) -> bool {
        self.handle.is_finished()
    }

    /// Wait for the result.
    pub async fn wait(self) -> Result<FlashReport> {
        match self.handle.await {
            Ok(r) => r,
            Err(e) => Err(Error::Flash(format!("job task failed: {e}"))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn job_progress_and_cancel() {
        let seen = Arc::new(std::sync::Mutex::new(Vec::new()));
        let s2 = seen.clone();
        let job = FlashJob::spawn(
            7,
            Some(Arc::new(move |p: &FlashProgress| {
                s2.lock().unwrap().push(p.phase)
            })),
            |p| async move {
                p.set(FlashPhase::Writing, 1, 10, "page");
                p.sleep(Duration::from_secs(10)).await?;
                unreachable!()
            },
        );
        tokio::time::sleep(Duration::from_millis(20)).await;
        assert_eq!(job.progress().borrow().phase, FlashPhase::Writing);
        job.cancel();
        let rx = job.progress();
        assert!(matches!(job.wait().await, Err(Error::Cancelled)));
        assert_eq!(rx.borrow().phase, FlashPhase::Cancelled);
        assert_eq!(rx.borrow().done, 1);
        assert_eq!(
            *seen.lock().unwrap(),
            vec![FlashPhase::Writing, FlashPhase::Cancelled]
        );
    }
}
