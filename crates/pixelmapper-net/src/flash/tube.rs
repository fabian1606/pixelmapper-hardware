//! Tube firmware update over RDM (contract §9.1, reference `tube/tools/flash_rdm.py`).

use super::{FlashReport, Progress};
use crate::error::{Error, Result};
use crate::image::TubeImage;
use crate::link::RdmLink;
use crate::rdm::Uid;
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
        }
    }
}

/// Run a tube update against `uid` through `link`.
pub async fn flash_tube<L: RdmLink>(
    link: &L,
    uid: Uid,
    image: &TubeImage,
    opts: &TubeFlashOptions,
    progress: &Progress,
) -> Result<FlashReport> {
    let _ = (link, uid, image, opts, progress);
    Err(Error::Flash("not implemented".into()))
}
