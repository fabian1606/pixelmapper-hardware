//! RDM (ANSI E1.20) message layer: [`Uid`], checksum, encode/decode of the basic RDM message
//! (start code 0xCC, `protocol/rdm-digest.md` §1) and decoding of DISC_UNIQUE_BRANCH responses (§3.1).
//!
//! PIDs specific to this project live in [`crate::pids`]; the standard PID tables are in digest §6.

use std::fmt;
use std::str::FromStr;
use thiserror::Error;

/// RDM start code.
pub const START_CODE: u8 = 0xCC;
/// RDM sub-start code.
pub const SUB_START_CODE: u8 = 0x01;
/// Bytes from start code up to and including PDL.
pub const HEADER_LEN: usize = 24;
/// Maximum parameter data length.
pub const MAX_PDL: usize = 231;

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
    /// Whether the command class is a response.
    pub fn is_response(&self) -> bool {
        self.command_class & 0x01 != 0
    }

    /// Message length field value (24 + PDL).
    pub fn message_length(&self) -> u8 {
        (HEADER_LEN + self.parameter_data.len()) as u8
    }

    /// Encode to wire bytes incl. start code and checksum. Panics if PDL > 231.
    pub fn encode(&self) -> Vec<u8> {
        assert!(self.parameter_data.len() <= MAX_PDL, "RDM PDL > 231");
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
        let uid = |o: usize| Uid::from_bytes(buf[o..o + 6].try_into().expect("6 bytes"));
        Ok(Self {
            dest_uid: uid(3),
            src_uid: uid(9),
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
    fn dub_roundtrip() {
        let u = Uid::new(0x7FF0, 0x1234_5678);
        let d = DubResponse::decode(&DubResponse::encode(u)).unwrap();
        assert_eq!(d.uid, u);
        assert_eq!(d.preamble_length, 7);
    }
}
