//! RDM (ANSI E1.20) message layer: [`Uid`], checksum, encode/decode of the basic RDM message
//! (start code 0xCC, `protocol/rdm-digest.md` §1) and decoding of DISC_UNIQUE_BRANCH responses (§3.1).
//!
//! PIDs specific to this project live in [`crate::pids`]; the standard PID numbers are in [`pid`] (digest §6) and
//! the parameter-data codecs in [`pd`].

pub mod pd;
pub mod pid;

use serde::{Deserialize, Deserializer, Serialize, Serializer};
use std::fmt;
use std::str::FromStr;
use std::time::Duration;
use thiserror::Error;

/// RDM start code.
pub const START_CODE: u8 = 0xCC;
/// RDM sub-start code.
pub const SUB_START_CODE: u8 = 0x01;
/// Bytes from start code up to and including PDL.
pub const HEADER_LEN: usize = 24;
/// Maximum parameter data length.
pub const MAX_PDL: usize = 231;
/// Sub-device number of the root device.
pub const SUB_DEVICE_ROOT: u16 = 0x0000;
/// Sub-device number addressing all sub-devices (SET only, digest §5).
pub const SUB_DEVICE_ALL: u16 = 0xFFFF;

/// Status types (digest §4.3), used in QUEUED_MESSAGE / STATUS_MESSAGES requests and status message entries.
pub mod status_type {
    /// STATUS_NONE.
    pub const NONE: u8 = 0x00;
    /// STATUS_GET_LAST_MESSAGE.
    pub const GET_LAST_MESSAGE: u8 = 0x01;
    /// STATUS_ADVISORY.
    pub const ADVISORY: u8 = 0x02;
    /// STATUS_WARNING.
    pub const WARNING: u8 = 0x03;
    /// STATUS_ERROR.
    pub const ERROR: u8 = 0x04;
    /// STATUS_ADVISORY_CLEARED.
    pub const ADVISORY_CLEARED: u8 = 0x12;
    /// STATUS_WARNING_CLEARED.
    pub const WARNING_CLEARED: u8 = 0x13;
    /// STATUS_ERROR_CLEARED.
    pub const ERROR_CLEARED: u8 = 0x14;
}

/// Command class values (digest §1).
pub mod cc {
    /// DISCOVERY_COMMAND.
    pub const DISCOVERY: u8 = 0x10;
    /// DISCOVERY_COMMAND_RESPONSE.
    pub const DISCOVERY_RESPONSE: u8 = 0x11;
    /// GET_COMMAND.
    pub const GET: u8 = 0x20;
    /// GET_COMMAND_RESPONSE.
    pub const GET_RESPONSE: u8 = 0x21;
    /// SET_COMMAND.
    pub const SET: u8 = 0x30;
    /// SET_COMMAND_RESPONSE.
    pub const SET_RESPONSE: u8 = 0x31;
}

/// Errors decoding RDM data.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum RdmError {
    /// Fewer bytes than the header or message length requires.
    #[error("message too short")]
    Short,
    /// First byte is not 0xCC.
    #[error("bad start code 0x{0:02x}")]
    BadStartCode(u8),
    /// Sub-start code is not 0x01.
    #[error("bad sub-start code 0x{0:02x}")]
    BadSubStartCode(u8),
    /// `Message Length != 24 + PDL`, or PDL > 231.
    #[error("inconsistent length")]
    BadLength,
    /// Checksum mismatch.
    #[error("bad checksum (got 0x{got:04x}, expected 0x{expected:04x})")]
    BadChecksum {
        /// Checksum in the packet.
        got: u16,
        /// Checksum computed over the packet.
        expected: u16,
    },
    /// DUB response without a valid 0xAA separator after 0..7 x 0xFE.
    #[error("bad DUB preamble")]
    BadPreamble,
    /// Text is not a valid `MMMM:DDDDDDDD` UID.
    #[error("invalid UID")]
    BadUid,
    /// Parameter data does not match the layout of the PID (wrong length or invalid content).
    #[error("bad parameter data: {0}")]
    BadParameterData(&'static str),
}

/// 48-bit RDM UID: ESTA manufacturer ID + device ID. Printed as `MMMM:DDDDDDDD` (upper-case hex).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct Uid {
    /// ESTA manufacturer ID.
    pub manufacturer: u16,
    /// Device ID.
    pub device: u32,
}

impl Uid {
    /// All-devices broadcast `FFFF:FFFFFFFF`.
    pub const BROADCAST: Uid = Uid::new(0xFFFF, 0xFFFF_FFFF);

    /// Construct a UID.
    pub const fn new(manufacturer: u16, device: u32) -> Self {
        Self {
            manufacturer,
            device,
        }
    }
    /// Big-endian wire bytes.
    pub fn to_bytes(self) -> [u8; 6] {
        let m = self.manufacturer.to_be_bytes();
        let d = self.device.to_be_bytes();
        [m[0], m[1], d[0], d[1], d[2], d[3]]
    }
    /// From big-endian wire bytes.
    pub fn from_bytes(b: [u8; 6]) -> Self {
        Self::new(
            u16::from_be_bytes([b[0], b[1]]),
            u32::from_be_bytes([b[2], b[3], b[4], b[5]]),
        )
    }
    /// 48-bit integer value (for range comparisons in discovery).
    pub fn as_u64(self) -> u64 {
        (u64::from(self.manufacturer) << 32) | u64::from(self.device)
    }
    /// From a 48-bit integer (upper bits ignored).
    pub fn from_u64(v: u64) -> Self {
        Self::new((v >> 32) as u16, v as u32)
    }
    /// Broadcast to all devices or vendorcast (`MMMM:FFFFFFFF`).
    pub fn is_broadcast(self) -> bool {
        self.device == 0xFFFF_FFFF
    }
    /// Vendorcast `MMMM:FFFFFFFF` to all devices of one manufacturer (not the all-devices broadcast).
    pub fn is_vendorcast(self) -> bool {
        self.device == 0xFFFF_FFFF && self.manufacturer != 0xFFFF
    }
    /// The all-devices broadcast `FFFF:FFFFFFFF`.
    pub fn is_all_broadcast(self) -> bool {
        self == Self::BROADCAST
    }
    /// Whether a message with destination `dest` is meant for the device with this UID: unicast to this UID,
    /// all-devices broadcast, or vendorcast with this UID's manufacturer (digest §5).
    pub fn addressed_to(self, dest: Uid) -> bool {
        dest == self
            || dest.is_all_broadcast()
            || (dest.is_vendorcast() && dest.manufacturer == self.manufacturer)
    }
    /// Read a UID from the first 6 bytes of `b`; `None` if `b` is shorter.
    pub fn from_slice(b: &[u8]) -> Option<Self> {
        match b {
            [a0, a1, a2, a3, a4, a5, ..] => Some(Self::from_bytes([*a0, *a1, *a2, *a3, *a4, *a5])),
            _ => None,
        }
    }
}

impl Serialize for Uid {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.collect_str(self)
    }
}

impl<'de> Deserialize<'de> for Uid {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        s.parse()
            .map_err(|_| serde::de::Error::custom(format!("invalid RDM UID {s:?}")))
    }
}

/// Response type in byte 16 of a response (digest §4.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum ResponseType {
    /// RESPONSE_TYPE_ACK: PD carries the answer.
    Ack = 0x00,
    /// RESPONSE_TYPE_ACK_TIMER: PD is a u16 delay in 100 ms units, fetch the answer via QUEUED_MESSAGE.
    AckTimer = 0x01,
    /// RESPONSE_TYPE_NACK_REASON: PD is a u16 [`NackReason`].
    NackReason = 0x02,
    /// RESPONSE_TYPE_ACK_OVERFLOW: partial data, repeat the same request and concatenate.
    AckOverflow = 0x03,
}

impl ResponseType {
    /// From the wire value; `None` for values > 3 (invalid per E1.20).
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0x00 => Some(Self::Ack),
            0x01 => Some(Self::AckTimer),
            0x02 => Some(Self::NackReason),
            0x03 => Some(Self::AckOverflow),
            _ => None,
        }
    }
}

/// NACK reason code (digest §4.2). Unknown codes are kept as-is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize)]
#[serde(transparent)]
pub struct NackReason(pub u16);

macro_rules! nack_reasons {
    ($($(#[$m:meta])* $name:ident = $val:expr;)*) => {
        impl NackReason {
            $($(#[$m])* pub const $name: NackReason = NackReason($val);)*

            /// Name without the `NR_` prefix, e.g. `"BUFFER_FULL"`; `None` for unknown codes.
            pub fn name(&self) -> Option<&'static str> {
                match self.0 {
                    $($val => Some(stringify!($name)),)*
                    _ => None,
                }
            }
        }
    };
}

nack_reasons! {
    /// NR_UNKNOWN_PID (E1.20).
    UNKNOWN_PID = 0x0000;
    /// NR_FORMAT_ERROR: wrong PDL or unparsable PD (E1.20).
    FORMAT_ERROR = 0x0001;
    /// NR_HARDWARE_FAULT (E1.20).
    HARDWARE_FAULT = 0x0002;
    /// NR_PROXY_REJECT (E1.20).
    PROXY_REJECT = 0x0003;
    /// NR_WRITE_PROTECT (E1.20).
    WRITE_PROTECT = 0x0004;
    /// NR_UNSUPPORTED_COMMAND_CLASS (E1.20).
    UNSUPPORTED_COMMAND_CLASS = 0x0005;
    /// NR_DATA_OUT_OF_RANGE (E1.20).
    DATA_OUT_OF_RANGE = 0x0006;
    /// NR_BUFFER_FULL (E1.20).
    BUFFER_FULL = 0x0007;
    /// NR_PACKET_SIZE_UNSUPPORTED (E1.20).
    PACKET_SIZE_UNSUPPORTED = 0x0008;
    /// NR_SUB_DEVICE_OUT_OF_RANGE (E1.20).
    SUB_DEVICE_OUT_OF_RANGE = 0x0009;
    /// NR_PROXY_BUFFER_FULL (E1.20).
    PROXY_BUFFER_FULL = 0x000A;
    /// NR_ACTION_NOT_SUPPORTED (E1.37-2).
    ACTION_NOT_SUPPORTED = 0x000B;
    /// NR_ENDPOINT_NUMBER_INVALID (E1.37-7).
    ENDPOINT_NUMBER_INVALID = 0x000C;
    /// NR_INVALID_ENDPOINT_MODE (E1.37-7).
    INVALID_ENDPOINT_MODE = 0x000D;
    /// NR_UNKNOWN_UID (E1.37-7).
    UNKNOWN_UID = 0x000E;
    /// NR_UNKNOWN_SCOPE (E1.33).
    UNKNOWN_SCOPE = 0x000F;
    /// NR_INVALID_STATIC_CONFIG_TYPE (E1.33).
    INVALID_STATIC_CONFIG_TYPE = 0x0010;
    /// NR_INVALID_IPV4_ADDRESS (E1.33).
    INVALID_IPV4_ADDRESS = 0x0011;
    /// NR_INVALID_IPV6_ADDRESS (E1.33).
    INVALID_IPV6_ADDRESS = 0x0012;
    /// NR_INVALID_PORT (E1.33).
    INVALID_PORT = 0x0013;
    /// NR_DEVICE_ABSENT (E1.37-5 / E1.20-2025).
    DEVICE_ABSENT = 0x0014;
    /// NR_SENSOR_OUT_OF_RANGE (E1.37-5 / E1.20-2025).
    SENSOR_OUT_OF_RANGE = 0x0015;
    /// NR_SENSOR_FAULT (E1.37-5 / E1.20-2025).
    SENSOR_FAULT = 0x0016;
    /// NR_PACKING_NOT_SUPPORTED (E1.37-5 / E1.20-2025).
    PACKING_NOT_SUPPORTED = 0x0017;
    /// NR_ERROR_IN_PACKED_LIST_TRANSACTION (E1.37-5 / E1.20-2025).
    ERROR_IN_PACKED_LIST_TRANSACTION = 0x0018;
    /// NR_PROXY_DROP (E1.37-5 / E1.20-2025).
    PROXY_DROP = 0x0019;
    /// NR_ALL_CALL_SET_FAIL (E1.20-2025).
    ALL_CALL_SET_FAIL = 0x0020;
}

impl fmt::Display for NackReason {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.name() {
            Some(n) => f.write_str(n),
            None => write!(f, "0x{:04X}", self.0),
        }
    }
}

impl fmt::Display for Uid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:04X}:{:08X}", self.manufacturer, self.device)
    }
}

impl FromStr for Uid {
    type Err = RdmError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let (m, d) = s.split_once(':').ok_or(RdmError::BadUid)?;
        let hex = |x: &str, n: usize| x.len() == n && x.bytes().all(|b| b.is_ascii_hexdigit());
        if !hex(m, 4) || !hex(d, 8) {
            return Err(RdmError::BadUid);
        }
        Ok(Self::new(
            u16::from_str_radix(m, 16).map_err(|_| RdmError::BadUid)?,
            u32::from_str_radix(d, 16).map_err(|_| RdmError::BadUid)?,
        ))
    }
}

/// 16-bit additive checksum over `bytes` (start code included), modulo 2^16.
pub fn checksum(bytes: &[u8]) -> u16 {
    bytes
        .iter()
        .fold(0u16, |acc, &b| acc.wrapping_add(u16::from(b)))
}

/// A basic RDM message (request or response), digest §1.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RdmMessage {
    /// Destination UID.
    pub dest_uid: Uid,
    /// Source UID.
    pub src_uid: Uid,
    /// Transaction number.
    pub transaction_number: u8,
    /// Port ID (requests) or response type (responses), byte 16.
    pub port_id_or_response_type: u8,
    /// Queued message count.
    pub message_count: u8,
    /// Sub-device (0 = root).
    pub sub_device: u16,
    /// Command class, see [`cc`].
    pub command_class: u8,
    /// Parameter ID.
    pub parameter_id: u16,
    /// Parameter data (0..=231 bytes).
    pub parameter_data: Vec<u8>,
}

impl RdmMessage {
    /// Build a request (message count 0).
    #[allow(clippy::too_many_arguments)]
    pub fn request(
        dest: Uid,
        src: Uid,
        tn: u8,
        port_id: u8,
        sub_device: u16,
        command_class: u8,
        pid: u16,
        data: Vec<u8>,
    ) -> Self {
        Self {
            dest_uid: dest,
            src_uid: src,
            transaction_number: tn,
            port_id_or_response_type: port_id,
            message_count: 0,
            sub_device,
            command_class,
            parameter_id: pid,
            parameter_data: data,
        }
    }

    /// Build the response to this request as responder `src` (digest §1 response rules): destination = request
    /// source, TN, sub-device and PID mirrored, command class + 1 (computed as `cc | 0x01`).
    pub fn response_to(
        &self,
        src: Uid,
        rt: ResponseType,
        message_count: u8,
        data: Vec<u8>,
    ) -> Self {
        Self {
            dest_uid: self.src_uid,
            src_uid: src,
            transaction_number: self.transaction_number,
            port_id_or_response_type: rt as u8,
            message_count,
            sub_device: self.sub_device,
            command_class: self.command_class | 0x01,
            parameter_id: self.parameter_id,
            parameter_data: data,
        }
    }

    /// Response type of a response; `None` for requests or invalid values (> 3).
    pub fn response_type(&self) -> Option<ResponseType> {
        if self.is_response() {
            ResponseType::from_u8(self.port_id_or_response_type)
        } else {
            None
        }
    }

    /// NACK reason of a NACK_REASON response with PDL 2; `None` otherwise.
    pub fn nack_reason(&self) -> Option<NackReason> {
        match (self.response_type(), self.parameter_data.as_slice()) {
            (Some(ResponseType::NackReason), [hi, lo]) => {
                Some(NackReason(u16::from_be_bytes([*hi, *lo])))
            }
            _ => None,
        }
    }

    /// Delay of an ACK_TIMER response with PDL 2 (u16 × 100 ms); `None` otherwise.
    pub fn ack_timer_delay(&self) -> Option<Duration> {
        match (self.response_type(), self.parameter_data.as_slice()) {
            (Some(ResponseType::AckTimer), [hi, lo]) => Some(Duration::from_millis(
                u64::from(u16::from_be_bytes([*hi, *lo])) * 100,
            )),
            _ => None,
        }
    }

    /// Whether the command class is a response.
    pub fn is_response(&self) -> bool {
        self.command_class & 0x01 != 0
    }

    /// Message length field value (24 + PDL).
    pub fn message_length(&self) -> u8 {
        (HEADER_LEN + self.parameter_data.len()) as u8
    }

    /// Encode to wire bytes incl. start code and checksum; [`RdmError::BadLength`] if PDL > 231.
    pub fn try_encode(&self) -> Result<Vec<u8>, RdmError> {
        if self.parameter_data.len() > MAX_PDL {
            return Err(RdmError::BadLength);
        }
        Ok(self.encode_unchecked())
    }

    /// Encode to wire bytes incl. start code and checksum.
    ///
    /// # Panics
    /// If PDL > 231 (a programming error). Use [`RdmMessage::try_encode`] when the PD length is not under the
    /// caller's control (e.g. data taken from a network peer or user input).
    pub fn encode(&self) -> Vec<u8> {
        assert!(self.parameter_data.len() <= MAX_PDL, "RDM PDL > 231");
        self.encode_unchecked()
    }

    fn encode_unchecked(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(HEADER_LEN + self.parameter_data.len() + 2);
        out.push(START_CODE);
        out.push(SUB_START_CODE);
        out.push(self.message_length());
        out.extend_from_slice(&self.dest_uid.to_bytes());
        out.extend_from_slice(&self.src_uid.to_bytes());
        out.push(self.transaction_number);
        out.push(self.port_id_or_response_type);
        out.push(self.message_count);
        out.extend_from_slice(&self.sub_device.to_be_bytes());
        out.push(self.command_class);
        out.extend_from_slice(&self.parameter_id.to_be_bytes());
        out.push(self.parameter_data.len() as u8);
        out.extend_from_slice(&self.parameter_data);
        let cs = checksum(&out);
        out.extend_from_slice(&cs.to_be_bytes());
        out
    }

    /// Decode wire bytes starting with 0xCC. Trailing bytes after the checksum are ignored.
    pub fn decode(buf: &[u8]) -> Result<Self, RdmError> {
        if buf.len() < HEADER_LEN + 2 {
            return Err(RdmError::Short);
        }
        if buf[0] != START_CODE {
            return Err(RdmError::BadStartCode(buf[0]));
        }
        if buf[1] != SUB_START_CODE {
            return Err(RdmError::BadSubStartCode(buf[1]));
        }
        let msg_len = usize::from(buf[2]);
        let pdl = usize::from(buf[23]);
        if pdl > MAX_PDL || msg_len != HEADER_LEN + pdl {
            return Err(RdmError::BadLength);
        }
        if buf.len() < msg_len + 2 {
            return Err(RdmError::Short);
        }
        let expected = checksum(&buf[..msg_len]);
        let got = u16::from_be_bytes([buf[msg_len], buf[msg_len + 1]]);
        if got != expected {
            return Err(RdmError::BadChecksum { got, expected });
        }
        let uid = |o: usize| Uid::from_slice(&buf[o..]).ok_or(RdmError::Short);
        Ok(Self {
            dest_uid: uid(3)?,
            src_uid: uid(9)?,
            transaction_number: buf[15],
            port_id_or_response_type: buf[16],
            message_count: buf[17],
            sub_device: u16::from_be_bytes([buf[18], buf[19]]),
            command_class: buf[20],
            parameter_id: u16::from_be_bytes([buf[21], buf[22]]),
            parameter_data: buf[HEADER_LEN..msg_len].to_vec(),
        })
    }
}

/// Decoded DISC_UNIQUE_BRANCH response (digest §3.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DubResponse {
    /// Number of 0xFE preamble bytes seen (0..=7).
    pub preamble_length: usize,
    /// Responding UID.
    pub uid: Uid,
    /// Checksum (sum of the 12 encoded UID bytes).
    pub checksum: u16,
}

impl DubResponse {
    /// Encode a DUB response with the full 7-byte preamble.
    pub fn encode(uid: Uid) -> Vec<u8> {
        let mut out = vec![0xFE; 7];
        out.push(0xAA);
        let euid: Vec<u8> = uid
            .to_bytes()
            .iter()
            .flat_map(|&b| [b | 0xAA, b | 0x55])
            .collect();
        let cs = checksum(&euid).to_be_bytes();
        out.extend_from_slice(&euid);
        for b in cs {
            out.extend_from_slice(&[b | 0xAA, b | 0x55]);
        }
        out
    }

    /// Decode raw DUB response bytes (no start code). A checksum mismatch means a collision.
    pub fn decode(buf: &[u8]) -> Result<Self, RdmError> {
        let preamble_length = buf.iter().take_while(|&&b| b == 0xFE).count();
        if preamble_length > 7 {
            return Err(RdmError::BadPreamble);
        }
        match buf.get(preamble_length) {
            Some(0xAA) => {}
            Some(_) => return Err(RdmError::BadPreamble),
            None => return Err(RdmError::Short),
        }
        let body = buf
            .get(preamble_length + 1..preamble_length + 17)
            .ok_or(RdmError::Short)?;
        let (euid, ecs) = body.split_at(12);
        let mut uid = [0u8; 6];
        for (i, u) in uid.iter_mut().enumerate() {
            *u = euid[2 * i] & euid[2 * i + 1];
        }
        let got = (u16::from(ecs[0] & ecs[1]) << 8) | u16::from(ecs[2] & ecs[3]);
        let expected = checksum(euid);
        if got != expected {
            return Err(RdmError::BadChecksum { got, expected });
        }
        Ok(Self {
            preamble_length,
            uid: Uid::from_bytes(uid),
            checksum: got,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uid_parse_print() {
        let u: Uid = "7FF0:12345678".parse().unwrap();
        assert_eq!(u, Uid::new(0x7FF0, 0x1234_5678));
        assert_eq!(u.to_string(), "7FF0:12345678");
        assert_eq!(
            "7ff0:a0000001".parse::<Uid>().unwrap().to_string(),
            "7FF0:A0000001"
        );
        assert_eq!(Uid::BROADCAST.to_string(), "FFFF:FFFFFFFF");
        for bad in [
            "",
            "7FF0",
            "7FF0:1234567",
            "7FF0:123456789",
            "7FF:12345678",
            "7FFG:12345678",
            "+FF0:12345678",
        ] {
            assert!(bad.parse::<Uid>().is_err(), "{bad}");
        }
        assert_eq!(Uid::from_bytes(u.to_bytes()), u);
        assert_eq!(Uid::from_u64(u.as_u64()), u);
    }

    #[test]
    fn checksum_values() {
        assert_eq!(checksum(&[]), 0);
        assert_eq!(checksum(&[0xCC, 0x01]), 0xCD);
        assert_eq!(checksum(&[0xFF; 300]), (0xFFu32 * 300 % 65536) as u16);
        // get_request from vectors/rdm/RDM_GET_DEVICE_INFO.json -> 0x05FA
        let msg = crate::parse_hex(
            "CC 01 18 7F F0 12 34 56 78 7F F0 A0 00 00 01 01 01 00 00 00 20 00 60 00",
        )
        .unwrap();
        assert_eq!(checksum(&msg), 0x05FA);
    }

    #[test]
    fn message_roundtrip_and_reject() {
        let m = RdmMessage {
            dest_uid: Uid::new(0x7FF0, 0x1234_5678),
            src_uid: Uid::new(0x7FF0, 0xA000_0001),
            transaction_number: 1,
            port_id_or_response_type: 1,
            message_count: 0,
            sub_device: 0,
            command_class: cc::GET,
            parameter_id: 0x0060,
            parameter_data: vec![],
        };
        let mut b = m.encode();
        assert_eq!(RdmMessage::decode(&b).unwrap(), m);
        *b.last_mut().unwrap() ^= 1;
        assert!(matches!(
            RdmMessage::decode(&b),
            Err(RdmError::BadChecksum { .. })
        ));
    }

    #[test]
    fn uid_casts_and_serde() {
        let me = Uid::new(0x7FF0, 0x1234_5678);
        assert!(me.addressed_to(me));
        assert!(me.addressed_to(Uid::BROADCAST));
        assert!(me.addressed_to(Uid::new(0x7FF0, 0xFFFF_FFFF)));
        assert!(!me.addressed_to(Uid::new(0x7FF1, 0xFFFF_FFFF)));
        assert!(!me.addressed_to(Uid::new(0x7FF0, 0x1234_5679)));
        assert!(Uid::new(0x7FF0, 0xFFFF_FFFF).is_vendorcast());
        assert!(!Uid::BROADCAST.is_vendorcast());
        assert!(Uid::BROADCAST.is_all_broadcast());
        assert!(!me.is_vendorcast() && !me.is_all_broadcast());
        assert_eq!(Uid::from_slice(&me.to_bytes()), Some(me));
        assert_eq!(Uid::from_slice(&[1, 2, 3, 4, 5]), None);
        let j = serde_json::to_string(&me).unwrap();
        assert_eq!(j, "\"7FF0:12345678\"");
        assert_eq!(serde_json::from_str::<Uid>(&j).unwrap(), me);
        assert!(serde_json::from_str::<Uid>("\"7FF0\"").is_err());
        assert!(serde_json::from_str::<Uid>("5").is_err());
    }

    #[test]
    fn response_types_and_nacks() {
        for v in 0..=3u8 {
            assert_eq!(ResponseType::from_u8(v).map(|r| r as u8), Some(v));
        }
        assert_eq!(ResponseType::from_u8(4), None);
        assert_eq!(NackReason::BUFFER_FULL.to_string(), "BUFFER_FULL");
        assert_eq!(NackReason(0x1234).to_string(), "0x1234");
        assert_eq!(NackReason::ACTION_NOT_SUPPORTED.0, 0x000B);
        assert_eq!(NackReason(0x0020).name(), Some("ALL_CALL_SET_FAIL"));
        assert_eq!(NackReason(0x001A).name(), None);
        assert_eq!(
            serde_json::to_string(&NackReason::FORMAT_ERROR).unwrap(),
            "1"
        );
    }

    #[test]
    fn request_response_helpers() {
        let ctl = Uid::new(0x7FF0, 0xA000_0001);
        let dev = Uid::new(0x7FF0, 0x1234_5678);
        let req = RdmMessage::request(dev, ctl, 9, 1, SUB_DEVICE_ROOT, cc::GET, 0x0060, vec![]);
        assert!(!req.is_response());
        assert_eq!(req.response_type(), None);
        let rsp = req.response_to(dev, ResponseType::NackReason, 3, vec![0x00, 0x07]);
        assert_eq!(rsp.dest_uid, ctl);
        assert_eq!(rsp.src_uid, dev);
        assert_eq!(rsp.transaction_number, 9);
        assert_eq!(rsp.command_class, cc::GET_RESPONSE);
        assert_eq!(rsp.parameter_id, 0x0060);
        assert_eq!(rsp.message_count, 3);
        assert_eq!(rsp.response_type(), Some(ResponseType::NackReason));
        assert_eq!(rsp.nack_reason(), Some(NackReason::BUFFER_FULL));
        assert_eq!(rsp.ack_timer_delay(), None);
        let t = req.response_to(dev, ResponseType::AckTimer, 0, vec![0x00, 0x0B]);
        assert_eq!(t.ack_timer_delay(), Some(Duration::from_millis(1100)));
        assert_eq!(t.nack_reason(), None);
        let s = RdmMessage::request(dev, ctl, 1, 1, SUB_DEVICE_ALL, cc::SET, 0x1000, vec![1]);
        assert_eq!(
            s.response_to(dev, ResponseType::Ack, 0, vec![])
                .command_class,
            cc::SET_RESPONSE
        );
        let d = RdmMessage::request(dev, ctl, 1, 1, 0, cc::DISCOVERY, 0x0002, vec![]);
        assert_eq!(
            d.response_to(dev, ResponseType::Ack, 0, vec![])
                .command_class,
            cc::DISCOVERY_RESPONSE
        );
        assert_eq!(rsp.try_encode().unwrap(), rsp.encode());
        let big = RdmMessage::request(dev, ctl, 1, 1, 0, cc::SET, 0x8182, vec![0; MAX_PDL + 1]);
        assert_eq!(big.try_encode(), Err(RdmError::BadLength));
        let max = RdmMessage::request(dev, ctl, 1, 1, 0, cc::SET, 0x8182, vec![0; MAX_PDL]);
        assert_eq!(RdmMessage::decode(&max.try_encode().unwrap()).unwrap(), max);
    }

    #[test]
    fn decode_never_panics() {
        let base = RdmMessage::request(
            Uid::new(1, 2),
            Uid::new(3, 4),
            1,
            1,
            0,
            cc::GET,
            0x0060,
            vec![0xAB; 20],
        )
        .encode();
        for len in 0..=base.len() + 2 {
            let mut b = base.clone();
            b.resize(len, 0);
            let _ = RdmMessage::decode(&b);
            let _ = DubResponse::decode(&b);
            for i in 0..len.min(26) {
                let mut c = b.clone();
                c[i] = 0xFF;
                let _ = RdmMessage::decode(&c);
            }
        }
    }

    #[test]
    fn dub_roundtrip() {
        let u = Uid::new(0x7FF0, 0x1234_5678);
        let d = DubResponse::decode(&DubResponse::encode(u)).unwrap();
        assert_eq!(d.uid, u);
        assert_eq!(d.preamble_length, 7);
    }
}
