//! Abstract transports used by the firmware-update flows, so they can run against the real
//! [`crate::controller::Controller`] or against an in-memory fake in unit tests.

use crate::artnet::{ArtFirmwareMaster, ArtFirmwareReply};
use crate::error::Result;
use crate::rdm::Uid;
use crate::state::NodeState;
use std::future::Future;
use std::time::Duration;

/// Per-request RDM options.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RdmOptions {
    /// Sub-device (0 = root).
    pub sub_device: u16,
    /// Timeout per attempt; `None` = controller default (500 ms).
    pub timeout: Option<Duration>,
    /// Attempts after the first when no answer comes; `None` = controller default (2).
    pub retries: Option<u32>,
    /// How often a NACK BUFFER_FULL / PROXY_BUFFER_FULL is retried before it is returned as an error (0 = never).
    pub buffer_full_retries: u32,
    /// Pause before a BUFFER_FULL retry.
    pub buffer_full_delay: Duration,
}

impl Default for RdmOptions {
    fn default() -> Self {
        Self {
            sub_device: 0,
            timeout: None,
            retries: None,
            buffer_full_retries: 0,
            buffer_full_delay: Duration::from_millis(30),
        }
    }
}

impl RdmOptions {
    /// Options with `retries` attempts after the first.
    pub fn retries(n: u32) -> Self {
        Self {
            retries: Some(n),
            ..Self::default()
        }
    }

    /// Builder: retry BUFFER_FULL up to `n` times.
    pub fn with_buffer_full_retries(mut self, n: u32) -> Self {
        self.buffer_full_retries = n;
        self
    }
}

/// RDM access to the devices behind one node port (one Port-Address).
///
/// `rdm` sends GET (`cc::GET`) or SET (`cc::SET`) to `uid` and returns the parameter data of the final ACK.
/// ACK_TIMER (fetch via QUEUED_MESSAGE) and ACK_OVERFLOW (concatenate) are resolved inside; NACK becomes
/// [`crate::Error::Nack`] (after the BUFFER_FULL retries of the options), no answer [`crate::Error::Timeout`].
/// A broadcast/vendorcast `uid` is sent once and returns `Ok(vec![])` without waiting.
pub trait RdmLink: Send + Sync {
    /// Send one request.
    fn rdm(
        &self,
        uid: Uid,
        command_class: u8,
        pid: u16,
        data: Vec<u8>,
        opts: RdmOptions,
    ) -> impl Future<Output = Result<Vec<u8>>> + Send;
}

/// Access to one node for the node firmware update (RDM to the node's own UID + ArtFirmwareMaster).
pub trait NodeLink: RdmLink {
    /// UID of the node's built-in RDM responder.
    fn node_uid(&self) -> Uid;

    /// Latest known state of the node (OEM, firmware version, ...).
    fn node_state(&self) -> Option<NodeState>;

    /// Send one ArtFirmwareMaster block (unicast) and wait up to `timeout` for the ArtFirmwareReply.
    fn firmware_block(
        &self,
        block: ArtFirmwareMaster,
        timeout: Duration,
    ) -> impl Future<Output = Result<ArtFirmwareReply>> + Send;

    /// Poll until the node reports firmware version `vers_info` (ArtPollReply VersInfo = `(MM << 8) | mm`) and
    /// return its state; [`crate::Error::WaitTimeout`] after `timeout`. Used after a reboot into new firmware.
    fn wait_for_version(
        &self,
        vers_info: u16,
        timeout: Duration,
    ) -> impl Future<Output = Result<NodeState>> + Send;
}
