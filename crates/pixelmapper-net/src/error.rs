//! The crate-wide error type.
//!
//! [`Error`] is stable: variants are only added (it is `#[non_exhaustive]`), and [`Error::code`] gives a
//! machine-readable identifier that callers (e.g. a WebSocket bridge) can forward to a UI. The `Display` text is
//! meant for humans.

use crate::artnet::ArtNetError;
use crate::rdm::{NackReason, RdmError, Uid};
use std::sync::Arc;
use std::time::Duration;

/// Result alias with [`Error`].
pub type Result<T, E = Error> = std::result::Result<T, E>;

/// Every error the crate returns. Cheap to clone (I/O errors are shared).
#[derive(Debug, Clone, thiserror::Error)]
#[non_exhaustive]
pub enum Error {
    /// Socket or file I/O failed.
    #[error("I/O error: {0}")]
    Io(Arc<std::io::Error>),
    /// An Art-Net packet could not be decoded.
    #[error("Art-Net: {0}")]
    ArtNet(#[from] ArtNetError),
    /// An RDM message or parameter data could not be decoded.
    #[error("RDM: {0}")]
    RdmDecode(#[from] RdmError),
    /// The device answered with NACK.
    #[error("RDM NACK {reason} for PID 0x{pid:04X} from {uid}")]
    Nack {
        /// Responding UID.
        uid: Uid,
        /// Requested PID.
        pid: u16,
        /// NACK reason code.
        reason: NackReason,
    },
    /// No (valid) answer within the timeout, after all retries.
    #[error("RDM timeout: no answer from {uid} for PID 0x{pid:04X} after {attempts} attempt(s)")]
    Timeout {
        /// Addressed UID.
        uid: Uid,
        /// Requested PID.
        pid: u16,
        /// Number of attempts sent.
        attempts: u32,
    },
    /// A request that expects an answer was sent to a broadcast/vendorcast UID; use a broadcast SET instead.
    #[error("no answer possible from broadcast UID {0}")]
    Broadcast(Uid),
    /// Waiting for something other than an RDM answer timed out (ArtIpProgReply, ArtFirmwareReply, ...).
    #[error("timeout after {0:?} waiting for {1}")]
    WaitTimeout(Duration, &'static str),
    /// The answer was well-formed but not what the protocol allows (wrong PID, wrong response type, ...).
    #[error("unexpected response: {0}")]
    UnexpectedResponse(String),
    /// No node with this id is known (not discovered yet, or gone).
    #[error("unknown node {0}")]
    UnknownNode(String),
    /// The node has no port with this Port-Address.
    #[error("node {node} has no port {port_address}")]
    UnknownPort {
        /// Node id as text.
        node: String,
        /// Requested Port-Address (Net:Sub:Uni).
        port_address: String,
    },
    /// An argument is out of range or malformed.
    #[error("invalid argument: {0}")]
    InvalidArgument(String),
    /// A firmware image file is malformed or does not fit the device.
    #[error("image: {0}")]
    Image(String),
    /// A firmware update failed (details in the text).
    #[error("flash: {0}")]
    Flash(String),
    /// The operation was cancelled by the caller.
    #[error("cancelled")]
    Cancelled,
    /// The controller has been shut down.
    #[error("controller closed")]
    Closed,
}

impl Error {
    /// Stable machine-readable code, e.g. `"rdm_timeout"`. New codes may be added; existing ones never change.
    pub fn code(&self) -> &'static str {
        match self {
            Error::Io(_) => "io",
            Error::ArtNet(_) => "artnet_decode",
            Error::RdmDecode(_) => "rdm_decode",
            Error::Nack { .. } => "rdm_nack",
            Error::Timeout { .. } => "rdm_timeout",
            Error::Broadcast(_) => "rdm_broadcast",
            Error::WaitTimeout(..) => "timeout",
            Error::UnexpectedResponse(_) => "unexpected_response",
            Error::UnknownNode(_) => "unknown_node",
            Error::UnknownPort { .. } => "unknown_port",
            Error::InvalidArgument(_) => "invalid_argument",
            Error::Image(_) => "image",
            Error::Flash(_) => "flash",
            Error::Cancelled => "cancelled",
            Error::Closed => "closed",
        }
    }

    /// The NACK reason if this is [`Error::Nack`].
    pub fn nack_reason(&self) -> Option<NackReason> {
        match self {
            Error::Nack { reason, .. } => Some(*reason),
            _ => None,
        }
    }

    /// Whether this is an RDM timeout ([`Error::Timeout`]).
    pub fn is_timeout(&self) -> bool {
        matches!(self, Error::Timeout { .. })
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(Arc::new(e))
    }
}

impl serde::Serialize for Error {
    /// Serialized as `{"code": "...", "message": "..."}` (plus `"nackReason"` for NACKs).
    fn serialize<S: serde::Serializer>(&self, s: S) -> std::result::Result<S::Ok, S::Error> {
        use serde::ser::SerializeMap;
        let mut m = s.serialize_map(None)?;
        m.serialize_entry("code", self.code())?;
        m.serialize_entry("message", &self.to_string())?;
        if let Some(r) = self.nack_reason() {
            m.serialize_entry("nackReason", &r.0)?;
        }
        m.end()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_and_serde() {
        let e = Error::Nack {
            uid: Uid::new(0x7FF0, 1),
            pid: 0x8011,
            reason: NackReason(7),
        };
        assert_eq!(e.code(), "rdm_nack");
        let j = serde_json::to_value(&e).unwrap();
        assert_eq!(j["code"], "rdm_nack");
        assert_eq!(j["nackReason"], 7);
        assert!(j["message"].as_str().unwrap().contains("0x8011"));
        let io: Error = std::io::Error::other("x").into();
        assert_eq!(io.code(), "io");
    }
}
