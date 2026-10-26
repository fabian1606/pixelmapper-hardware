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
    /// A firmware update failed. The text is for humans; [`Error::flash_kind`] / [`Error::flash_error`] give the
    /// machine-readable [`FlashErrorKind`] (the library appends it as a ` [kind: ...]` tag, see [`FlashError`]).
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

    /// The kind of a failed firmware update if this is [`Error::Flash`].
    /// `Some(FlashErrorKind::Other)` for a flash error without a kind tag.
    pub fn flash_kind(&self) -> Option<FlashErrorKind> {
        self.flash_error().map(|f| f.kind)
    }

    /// The parsed [`FlashError`] (kind, message without the tag, bootloader flag) if this is [`Error::Flash`].
    pub fn flash_error(&self) -> Option<FlashError> {
        match self {
            Error::Flash(text) => Some(FlashError::parse(text)),
            _ => None,
        }
    }

    /// Shorthand for `Error::from(FlashError::new(kind, message))`.
    pub fn flash(kind: FlashErrorKind, message: impl Into<String>) -> Self {
        FlashError::new(kind, message).into()
    }

    /// Whether this is an RDM timeout ([`Error::Timeout`]).
    pub fn is_timeout(&self) -> bool {
        matches!(self, Error::Timeout { .. })
    }
}

/// Why a firmware update failed (see [`Error::Flash`]). New kinds may be added; [`FlashErrorKind::code`] values
/// never change. Serialized as its code.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum FlashErrorKind {
    /// The image is for another device: manufacturer, model, hardware revision or OEM code differ. Nothing written.
    DeviceMismatch,
    /// The image is older than the installed firmware and the downgrade was not allowed. Nothing written.
    Downgrade,
    /// The tube bootloader cannot take this image (too old for the trailer, other page size / app end).
    Incompatible,
    /// No answer in time (no BOOT_INFO answer, no ArtFirmwareReply, BOOT_CATCH not caught, no answer after
    /// FINISH, node did not come back with the new version, ...).
    Timeout,
    /// The device stayed busy (NACK BUFFER_FULL, bootloader busy) for all retries.
    Busy,
    /// The device refused a request with a NACK (no update-specific error code behind it).
    Nack,
    /// The device rejected the image: FW_* NACK with FW_INFO.last_error, ArtFirmwareReply FirmFail, or the tube
    /// bootloader refused to start the app at BOOT_FINISH.
    Rejected,
    /// The written content does not match the image (APP_CRC chunks still differ after the rewrite pass).
    Verify,
    /// The update went through but the device reports another version afterwards (rolled back?).
    VersionMismatch,
    /// An answer was malformed or not allowed at this point (short answer, unexpected reply type).
    BadResponse,
    /// Anything else (BOOT_CATCH aborted at the node, internal failure, unclassified message).
    Other,
}

impl FlashErrorKind {
    /// Stable snake_case code, e.g. `"device_mismatch"`. Existing codes never change.
    pub fn code(self) -> &'static str {
        match self {
            FlashErrorKind::DeviceMismatch => "device_mismatch",
            FlashErrorKind::Downgrade => "downgrade",
            FlashErrorKind::Incompatible => "incompatible",
            FlashErrorKind::Timeout => "timeout",
            FlashErrorKind::Busy => "busy",
            FlashErrorKind::Nack => "nack",
            FlashErrorKind::Rejected => "rejected",
            FlashErrorKind::Verify => "verify",
            FlashErrorKind::VersionMismatch => "version_mismatch",
            FlashErrorKind::BadResponse => "bad_response",
            FlashErrorKind::Other => "other",
        }
    }

    /// All kinds (in declaration order).
    pub const ALL: [FlashErrorKind; 11] = [
        FlashErrorKind::DeviceMismatch,
        FlashErrorKind::Downgrade,
        FlashErrorKind::Incompatible,
        FlashErrorKind::Timeout,
        FlashErrorKind::Busy,
        FlashErrorKind::Nack,
        FlashErrorKind::Rejected,
        FlashErrorKind::Verify,
        FlashErrorKind::VersionMismatch,
        FlashErrorKind::BadResponse,
        FlashErrorKind::Other,
    ];

    /// The kind with this [`code`](Self::code).
    pub fn from_code(code: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|k| k.code() == code)
    }

    /// The kind that fits an error from a step of an update (NACK, timeout, bad answer; a flash error keeps its
    /// kind).
    pub fn of(e: &Error) -> Self {
        match e {
            Error::Flash(_) => e.flash_kind().unwrap_or(FlashErrorKind::Other),
            Error::Nack { .. } => FlashErrorKind::Nack,
            Error::Timeout { .. } | Error::WaitTimeout(..) => FlashErrorKind::Timeout,
            Error::UnexpectedResponse(_) | Error::RdmDecode(_) | Error::ArtNet(_) => {
                FlashErrorKind::BadResponse
            }
            _ => FlashErrorKind::Other,
        }
    }
}

impl std::fmt::Display for FlashErrorKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.code())
    }
}

impl serde::Serialize for FlashErrorKind {
    fn serialize<S: serde::Serializer>(&self, s: S) -> std::result::Result<S::Ok, S::Error> {
        s.serialize_str(self.code())
    }
}

/// Parsed form of an [`Error::Flash`] text: a machine-readable [`kind`](Self::kind), the human-readable message
/// and whether the tube stays in its bootloader.
///
/// `Error::Flash` keeps carrying a `String` (source compatibility). Errors built by the library from a
/// `FlashError` end in a tag: `"<message> [kind: <code>]"` or `"<message> [kind: <code>; bootloader active]"`;
/// [`Error::flash_error`] parses it back. A text without a tag (e.g. `Error::Flash("x".into())`) parses as
/// [`FlashErrorKind::Other`].
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
#[non_exhaustive]
pub struct FlashError {
    /// Why the update failed.
    pub kind: FlashErrorKind,
    /// Details for humans, without the kind tag.
    pub message: String,
    /// The tube is known to be left in its bootloader (tube_boot_rdm) when the update failed.
    pub bootloader_active: bool,
}

const TAG_OPEN: &str = " [kind: ";
const TAG_BOOT: &str = "; bootloader active";

impl FlashError {
    /// A flash error of `kind`.
    pub fn new(kind: FlashErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
            bootloader_active: false,
        }
    }

    /// Mark that the tube stays in its bootloader.
    pub fn in_bootloader(mut self) -> Self {
        self.bootloader_active = true;
        self
    }

    /// Parse an [`Error::Flash`] text (see the type docs).
    pub fn parse(text: &str) -> Self {
        let tagged = text.strip_suffix(']').and_then(|t| {
            let i = t.rfind(TAG_OPEN)?;
            let tag = &t[i + TAG_OPEN.len()..];
            let (code, boot) = match tag.strip_suffix(TAG_BOOT) {
                Some(c) => (c, true),
                None => (tag, false),
            };
            let kind = FlashErrorKind::from_code(code)?;
            Some(Self {
                kind,
                message: t[..i].to_string(),
                bootloader_active: boot,
            })
        });
        tagged.unwrap_or_else(|| Self::new(FlashErrorKind::Other, text))
    }

    /// The `Error::Flash` text: message plus kind tag.
    pub fn to_text(&self) -> String {
        let boot = if self.bootloader_active { TAG_BOOT } else { "" };
        format!("{}{TAG_OPEN}{}{boot}]", self.message, self.kind.code())
    }
}

impl std::fmt::Display for FlashError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.to_text())
    }
}

impl From<FlashError> for Error {
    fn from(f: FlashError) -> Self {
        Error::Flash(f.to_text())
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(Arc::new(e))
    }
}

impl serde::Serialize for Error {
    /// Serialized as `{"code": "...", "message": "..."}` (plus `"nackReason"` for NACKs, `"flashKind"` and, if
    /// set, `"bootloaderActive": true` for [`Error::Flash`]).
    fn serialize<S: serde::Serializer>(&self, s: S) -> std::result::Result<S::Ok, S::Error> {
        use serde::ser::SerializeMap;
        let mut m = s.serialize_map(None)?;
        m.serialize_entry("code", self.code())?;
        m.serialize_entry("message", &self.to_string())?;
        if let Some(r) = self.nack_reason() {
            m.serialize_entry("nackReason", &r.0)?;
        }
        if let Some(f) = self.flash_error() {
            m.serialize_entry("flashKind", &f.kind)?;
            if f.bootloader_active {
                m.serialize_entry("bootloaderActive", &true)?;
            }
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
        assert_eq!(io.flash_kind(), None);
        assert!(serde_json::to_value(&io)
            .unwrap()
            .get("flashKind")
            .is_none());
    }

    #[test]
    fn flash_kind_codes_and_serde() {
        let e = Error::flash(FlashErrorKind::DeviceMismatch, "wrong device: x");
        assert_eq!(e.code(), "flash");
        assert_eq!(e.flash_kind(), Some(FlashErrorKind::DeviceMismatch));
        assert_eq!(
            e.to_string(),
            "flash: wrong device: x [kind: device_mismatch]"
        );
        let f = e.flash_error().unwrap();
        assert_eq!(
            (f.message.as_str(), f.bootloader_active),
            ("wrong device: x", false)
        );
        let j = serde_json::to_value(&e).unwrap();
        assert_eq!(j["code"], "flash");
        assert_eq!(j["flashKind"], "device_mismatch");
        assert_eq!(
            j["message"],
            "flash: wrong device: x [kind: device_mismatch]"
        );
        assert!(j.get("bootloaderActive").is_none());
        let e: Error = FlashError::new(FlashErrorKind::Verify, "x")
            .in_bootloader()
            .into();
        assert!(matches!(&e, Error::Flash(m) if m == "x [kind: verify; bootloader active]"));
        let j = serde_json::to_value(&e).unwrap();
        assert_eq!(
            (j["flashKind"].clone(), j["bootloaderActive"].clone()),
            ("verify".into(), true.into())
        );
        let codes = [
            (FlashErrorKind::DeviceMismatch, "device_mismatch"),
            (FlashErrorKind::Downgrade, "downgrade"),
            (FlashErrorKind::Incompatible, "incompatible"),
            (FlashErrorKind::Timeout, "timeout"),
            (FlashErrorKind::Busy, "busy"),
            (FlashErrorKind::Nack, "nack"),
            (FlashErrorKind::Rejected, "rejected"),
            (FlashErrorKind::Verify, "verify"),
            (FlashErrorKind::VersionMismatch, "version_mismatch"),
            (FlashErrorKind::BadResponse, "bad_response"),
            (FlashErrorKind::Other, "other"),
        ];
        for (k, c) in codes {
            assert_eq!(k.code(), c);
            assert_eq!(k.to_string(), c);
            assert_eq!(serde_json::to_value(k).unwrap(), c);
            assert_eq!(FlashErrorKind::from_code(c), Some(k));
            let f = FlashError::parse(&FlashError::new(k, "a [b] c").to_text());
            assert_eq!((f.kind, f.message.as_str()), (k, "a [b] c"));
        }
        assert_eq!(FlashErrorKind::ALL.len(), codes.len());
    }

    #[test]
    fn flash_compat_shims() {
        // Error::Flash keeps its String payload; untagged texts are kind Other
        let e = Error::Flash("downgrade 1 -> 0".into());
        assert_eq!(e.flash_kind(), Some(FlashErrorKind::Other));
        assert_eq!(e.to_string(), "flash: downgrade 1 -> 0");
        let e2 = Error::flash(FlashErrorKind::Timeout, "no answer 1");
        match &e2 {
            Error::Flash(m) if m.starts_with("no answer") => {
                let s: String = m.clone();
                assert!(s.contains('1'));
            }
            _ => panic!("{e2}"),
        }
        assert_eq!(FlashErrorKind::of(&e2), FlashErrorKind::Timeout);
        assert_eq!(FlashErrorKind::of(&e), FlashErrorKind::Other);
        assert_eq!(
            FlashError::parse("x [kind: nonsense]").kind,
            FlashErrorKind::Other
        );
        assert_eq!(
            FlashErrorKind::of(&Error::Nack {
                uid: Uid::new(1, 1),
                pid: 1,
                reason: NackReason(0)
            }),
            FlashErrorKind::Nack
        );
        assert_eq!(FlashErrorKind::of(&Error::Cancelled), FlashErrorKind::Other);
    }
}
