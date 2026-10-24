//! Node firmware update via ArtFirmwareMaster (`.alf`) or the FW_* PIDs (contract §9.2, image-format.md §2).

use super::{FlashReport, Progress};
use crate::error::{Error, Result};
use crate::image::NodeImage;
use crate::link::NodeLink;
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
}

impl Default for NodeFlashOptions {
    fn default() -> Self {
        Self {
            method: NodeFlashMethod::Rdm,
            allow_downgrade: false,
            block_timeout: Duration::from_secs(30),
            reboot_timeout: Duration::from_secs(90),
        }
    }
}

/// Run a node update through `link`.
pub async fn flash_node<L: NodeLink>(
    link: &L,
    image: &NodeImage,
    opts: &NodeFlashOptions,
    progress: &Progress,
) -> Result<FlashReport> {
    let _ = (link, image, opts, progress);
    Err(Error::Flash("not implemented".into()))
}
