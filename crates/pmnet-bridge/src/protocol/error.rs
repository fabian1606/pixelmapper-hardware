//! Error codes of the bridge contract (`protocol/bridge-ws.md` §8) and the `error` reply payload.

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

// ── Error codes ───────────────────────────────────────────────────────────────────────────────────────────────────

/// Error codes sent by the bridge.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ErrorCode {
    /// Malformed JSON, missing or invalid arguments (`details.field` names the argument).
    BadRequest,
    /// Command type not implemented by this bridge.
    UnknownCommand,
    /// Command before the client `hello`.
    HelloRequired,
    /// Client protocol major differs.
    IncompatibleProtocol,
    /// `nodeId` is not in the node table.
    UnknownNode,
    /// The node has no such `port`.
    UnknownPort,
    /// No answer from the network after the retries.
    Timeout,
    /// RDM NACK; `details.nackReason`, `details.nackName`.
    Nack,
    /// Malformed or unexpected answer after the retries.
    BadResponse,
    /// Port-addresses violate the shared Net/SubNet of a BindIndex, or are out of range.
    InvalidPortAddress,
    /// Intel HEX or trailer invalid.
    InvalidImage,
    /// Device manufacturer/model do not match the image.
    DeviceMismatch,
    /// Image older than the device firmware and `allowDowngrade` is false.
    Downgrade,
    /// A firmware job already runs on that port.
    Busy,
    /// `jobId` unknown.
    UnknownJob,
    /// The Art-Net UDP socket is not bound.
    UdpUnavailable,
    /// Bridge bug.
    Internal,
}

// ── Error payload ─────────────────────────────────────────────────────────────────────────────────────────────────

/// Payload of an `error` reply: code, human-readable message and optional details.
#[derive(Debug, Clone, PartialEq)]
pub struct CommandError {
    /// Machine-readable code.
    pub code: ErrorCode,
    /// Human-readable text.
    pub message: String,
    /// Structured details (`field`, `nackReason`, `current`/`image`, …).
    pub details: Option<Map<String, Value>>,
}

impl CommandError {
    /// Error without details.
    pub fn new(code: ErrorCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
            details: None,
        }
    }

    /// `badRequest` naming the offending argument in `details.field`.
    pub fn bad_request(field: impl Into<String>, message: impl Into<String>) -> Self {
        let field = field.into();
        let mut e = Self::new(ErrorCode::BadRequest, message);
        if !field.is_empty() {
            e = e.with_detail("field", Value::String(field));
        }
        e
    }

    /// Adds one detail entry.
    pub fn with_detail(mut self, key: &str, value: Value) -> Self {
        self.details
            .get_or_insert_with(Map::new)
            .insert(key.to_owned(), value);
        self
    }

    /// RDM NACK with reason number and name (contract §6 Semantics).
    pub fn nack(reason: u16) -> Self {
        let name = nack_name(reason);
        Self::new(ErrorCode::Nack, format!("RDM NACK {name}"))
            .with_detail("nackReason", Value::from(reason))
            .with_detail("nackName", Value::String(name))
    }
}

// ── NACK reasons ──────────────────────────────────────────────────────────────────────────────────────────────────

/// Contract name of an RDM NACK reason (E1.20 Table A-17, `protocol/rdm-digest.md` §4.2); unknown reasons are
/// formatted as `"0x…"`.
pub fn nack_name(reason: u16) -> String {
    let name = match reason {
        0x0000 => "UNKNOWN_PID",
        0x0001 => "FORMAT_ERROR",
        0x0002 => "HARDWARE_FAULT",
        0x0003 => "PROXY_REJECT",
        0x0004 => "WRITE_PROTECT",
        0x0005 => "UNSUPPORTED_COMMAND_CLASS",
        0x0006 => "DATA_OUT_OF_RANGE",
        0x0007 => "BUFFER_FULL",
        0x0008 => "PACKET_SIZE_UNSUPPORTED",
        0x0009 => "SUB_DEVICE_OUT_OF_RANGE",
        0x000A => "PROXY_BUFFER_FULL",
        0x000B => "ACTION_NOT_SUPPORTED",
        other => return format!("0x{other:04X}"),
    };
    name.to_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nack_names() {
        assert_eq!(nack_name(0), "UNKNOWN_PID");
        assert_eq!(nack_name(0x000B), "ACTION_NOT_SUPPORTED");
        assert_eq!(nack_name(0x0020), "0x0020");
        let e = CommandError::nack(6);
        assert_eq!(e.code, ErrorCode::Nack);
        let d = e.details.unwrap();
        assert_eq!(d["nackReason"], 6);
        assert_eq!(d["nackName"], "DATA_OUT_OF_RANGE");
    }

    #[test]
    fn code_names() {
        assert_eq!(
            serde_json::to_string(&ErrorCode::UdpUnavailable).unwrap(),
            "\"udpUnavailable\""
        );
        assert_eq!(
            serde_json::to_string(&ErrorCode::BadRequest).unwrap(),
            "\"badRequest\""
        );
    }
}
