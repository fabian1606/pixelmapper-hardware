//! Art-Net 4 packets (UDP port 6454).
//!
//! Every packet starts with `"Art-Net\0"` + OpCode (little-endian). All packets except [`ArtPollReply`] follow
//! with ProtVer (big-endian, 14). Layouts and offsets: `protocol/artnet4-digest.md` (§2 discovery, §3 DMX, §4
//! configuration, §5 RDM, §6 firmware update).
//!
//! Conventions of this module:
//! - Every packet type has `encode(&self) -> Vec<u8>` (never panics) and `decode(&[u8]) -> Result<Self, _>`.
//! - `decode` checks ID, OpCode, ProtVer >= 14 (where the packet has one) and the minimum length; trailing bytes
//!   are ignored; no input can make it panic or read past the buffer.
//! - Multi-byte fields are big-endian, except the OpCode and `ArtPollReply.Port` / `ArtPollReply.EstaMan`.
//! - Fixed-size name fields are NUL-padded: decoding stops at the first NUL (lossy UTF-8), encoding truncates to
//!   field length − 1 bytes so the field is always NUL-terminated.
//! - [`ArtPacket`] decodes any packet by dispatching on the OpCode.

use crate::rdm::{RdmError, RdmMessage, Uid};
use serde::{Deserialize, Serialize};
use std::fmt;
use std::net::Ipv4Addr;
use std::str::FromStr;
use thiserror::Error;

/// UDP port for all Art-Net traffic (0x1936).
pub const PORT: u16 = 0x1936;
/// The 8-byte packet ID.
pub const ID: &[u8; 8] = b"Art-Net\0";
/// Protocol version sent and minimum accepted.
pub const PROT_VER: u16 = 14;
/// Length of ID + OpCode + ProtVer.
pub const HEADER_LEN: usize = 12;

/// Errors decoding an Art-Net packet.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum ArtNetError {
    /// Packet shorter than the minimum length, or a length/count field points past the payload.
    #[error("packet too short")]
    Short,
    /// ID is not `Art-Net\0`.
    #[error("bad Art-Net ID")]
    BadId,
    /// ProtVer below 14.
    #[error("unsupported protocol version {0}")]
    ProtVer(u16),
    /// OpCode differs from the one the decoder expects.
    #[error("unexpected opcode 0x{0:04x}")]
    UnexpectedOpCode(u16),
    /// A field has a value outside its allowed range.
    #[error("invalid field: {0}")]
    InvalidField(&'static str),
}

macro_rules! opcodes {
    ($($(#[$m:meta])* $name:ident = $val:expr,)*) => {
        /// All Art-Net 4 OpCodes (Art-Net 4 Table 1, digest §1).
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        #[repr(u16)]
        pub enum OpCode {
            $($(#[$m])* $name = $val,)*
        }

        impl OpCode {
            /// Look up an OpCode by its numeric value.
            pub fn from_u16(v: u16) -> Option<Self> {
                match v {
                    $($val => Some(Self::$name),)*
                    _ => None,
                }
            }
        }
    };
}

opcodes! {
    /// ArtPoll.
    Poll = 0x2000,
    /// ArtPollReply.
    PollReply = 0x2100,
    /// ArtDiagData.
    DiagData = 0x2300,
    /// ArtCommand.
    Command = 0x2400,
    /// ArtDataRequest.
    DataRequest = 0x2700,
    /// ArtDataReply.
    DataReply = 0x2800,
    /// ArtDmx (OpOutput).
    Dmx = 0x5000,
    /// ArtNzs.
    Nzs = 0x5100,
    /// ArtSync.
    Sync = 0x5200,
    /// ArtAddress.
    Address = 0x6000,
    /// ArtInput.
    Input = 0x7000,
    /// ArtTodRequest.
    TodRequest = 0x8000,
    /// ArtTodData.
    TodData = 0x8100,
    /// ArtTodControl.
    TodControl = 0x8200,
    /// ArtRdm.
    Rdm = 0x8300,
    /// ArtRdmSub.
    RdmSub = 0x8400,
    /// ArtMedia.
    Media = 0x9000,
    /// ArtMediaPatch.
    MediaPatch = 0x9100,
    /// ArtMediaControl.
    MediaControl = 0x9200,
    /// ArtMediaContrlReply.
    MediaContrlReply = 0x9300,
    /// ArtTimeCode.
    TimeCode = 0x9700,
    /// ArtTimeSync.
    TimeSync = 0x9800,
    /// ArtTrigger.
    Trigger = 0x9900,
    /// ArtDirectory.
    Directory = 0x9A00,
    /// ArtDirectoryReply.
    DirectoryReply = 0x9B00,
    /// ArtVideoSetup.
    VideoSetup = 0xA010,
    /// ArtVideoPalette.
    VideoPalette = 0xA020,
    /// ArtVideoData.
    VideoData = 0xA040,
    /// ArtMacMaster (deprecated).
    MacMaster = 0xF000,
    /// ArtMacSlave (deprecated).
    MacSlave = 0xF100,
    /// ArtFirmwareMaster.
    FirmwareMaster = 0xF200,
    /// ArtFirmwareReply.
    FirmwareReply = 0xF300,
    /// ArtFileTnMaster.
    FileTnMaster = 0xF400,
    /// ArtFileFnMaster.
    FileFnMaster = 0xF500,
    /// ArtFileFnReply.
    FileFnReply = 0xF600,
    /// ArtIpProg.
    IpProg = 0xF800,
    /// ArtIpProgReply.
    IpProgReply = 0xF900,
}

/// Decoded common header. `op_code` stays raw so unknown OpCodes can still be reported.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    /// Raw OpCode (wire order little-endian).
    pub op_code: u16,
    /// Protocol version (wire order big-endian).
    pub prot_ver: u16,
}

impl Header {
    /// Known OpCode, if any.
    pub fn op(&self) -> Option<OpCode> {
        OpCode::from_u16(self.op_code)
    }

    /// Decode and validate ID and ProtVer (>= 14). Not usable for ArtPollReply (no ProtVer there); use
    /// [`peek_op_code`] for that.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        let op_code = peek_op_code(buf)?;
        if buf.len() < HEADER_LEN {
            return Err(ArtNetError::Short);
        }
        let prot_ver = u16::from_be_bytes([buf[10], buf[11]]);
        if prot_ver < PROT_VER {
            return Err(ArtNetError::ProtVer(prot_ver));
        }
        Ok(Self { op_code, prot_ver })
    }

    /// Append the 12 header bytes for `op` with ProtVer 14.
    pub fn encode(op: OpCode, out: &mut Vec<u8>) {
        Self::encode_raw(op as u16, out);
    }

    fn encode_raw(op_code: u16, out: &mut Vec<u8>) {
        out.extend_from_slice(ID);
        out.extend_from_slice(&op_code.to_le_bytes());
        out.extend_from_slice(&PROT_VER.to_be_bytes());
    }
}

/// Check the ID and return the raw OpCode (bytes 8–9, little-endian). Does not look at ProtVer.
pub fn peek_op_code(buf: &[u8]) -> Result<u16, ArtNetError> {
    if buf.len() < 10 {
        return Err(ArtNetError::Short);
    }
    if &buf[..8] != ID {
        return Err(ArtNetError::BadId);
    }
    Ok(u16::from_le_bytes([buf[8], buf[9]]))
}

/// Validate ID, OpCode, ProtVer (if `prot_ver`) and minimum length.
fn check(buf: &[u8], op: OpCode, prot_ver: bool, min_len: usize) -> Result<(), ArtNetError> {
    let op_code = peek_op_code(buf)?;
    if op_code != op as u16 {
        return Err(ArtNetError::UnexpectedOpCode(op_code));
    }
    if prot_ver {
        Header::decode(buf)?;
    }
    if buf.len() < min_len {
        return Err(ArtNetError::Short);
    }
    Ok(())
}

/// Bounds-safe reader: bytes beyond the buffer read as zero (callers check the minimum length first).
struct Rd<'a>(&'a [u8]);

impl Rd<'_> {
    fn u8(&self, off: usize) -> u8 {
        self.0.get(off).copied().unwrap_or(0)
    }
    fn be16(&self, off: usize) -> u16 {
        u16::from_be_bytes([self.u8(off), self.u8(off + 1)])
    }
    fn le16(&self, off: usize) -> u16 {
        u16::from_le_bytes([self.u8(off), self.u8(off + 1)])
    }
    fn be32(&self, off: usize) -> u32 {
        u32::from_be_bytes(self.arr(off))
    }
    fn arr<const N: usize>(&self, off: usize) -> [u8; N] {
        let mut a = [0u8; N];
        for (i, b) in a.iter_mut().enumerate() {
            *b = self.u8(off + i);
        }
        a
    }
    fn ip(&self, off: usize) -> Ipv4Addr {
        Ipv4Addr::from(self.arr::<4>(off))
    }
    fn uid(&self, off: usize) -> Uid {
        Uid::from_bytes(self.arr(off))
    }
    /// NUL-terminated string in a fixed field of `len` bytes (lossy UTF-8).
    fn string(&self, off: usize, len: usize) -> String {
        let start = off.min(self.0.len());
        let end = (off + len).min(self.0.len());
        decode_cstr(&self.0[start..end])
    }
}

fn decode_cstr(field: &[u8]) -> String {
    let end = field.iter().position(|&b| b == 0).unwrap_or(field.len());
    String::from_utf8_lossy(&field[..end]).into_owned()
}

/// Longest prefix of `s` with at most `max` bytes that ends on a char boundary.
fn truncate_str(s: &str, max: usize) -> &[u8] {
    let mut end = s.len().min(max);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    &s.as_bytes()[..end]
}

/// Append `s` as a NUL-padded field of `len` bytes (at most `len - 1` string bytes).
fn put_str(out: &mut Vec<u8>, s: &str, len: usize) {
    let b = truncate_str(s, len.saturating_sub(1));
    out.extend_from_slice(b);
    out.resize(out.len() + (len - b.len()), 0);
}

fn put_zeros(out: &mut Vec<u8>, n: usize) {
    out.resize(out.len() + n, 0);
}

fn pa(net: u8, low: u8) -> PortAddress {
    PortAddress((u16::from(net & 0x7F) << 8) | u16::from(low))
}

// ---------------------------------------------------------------------------------------------------------------
// Port-Address
// ---------------------------------------------------------------------------------------------------------------

/// 15-bit Port-Address: Net (7 bit) | Sub-Net (4 bit) | Universe (4 bit), digest §0 "Port-Address".
///
/// Serialized (serde) as a plain number; deserializing rejects values above 32767. Displayed as
/// `Net:Sub-Net:Universe` in decimal (e.g. `0:0:1`); [`FromStr`] accepts that form or a decimal 15-bit number.
#[derive(
    Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Default, Serialize, Deserialize,
)]
#[serde(try_from = "u16", into = "u16")]
pub struct PortAddress(u16);

impl PortAddress {
    /// Highest valid Port-Address.
    pub const MAX: u16 = 0x7FFF;

    /// Create from a raw value; `None` if above 32767.
    pub fn new(raw: u16) -> Option<Self> {
        (raw <= Self::MAX).then_some(Self(raw))
    }
    /// Build from Net, Sub-Net and Universe.
    pub fn from_parts(net: u8, sub_net: u8, universe: u8) -> Option<Self> {
        if net > 0x7F || sub_net > 0x0F || universe > 0x0F {
            return None;
        }
        Some(Self(
            (u16::from(net) << 8) | (u16::from(sub_net) << 4) | u16::from(universe),
        ))
    }
    /// Build from Net and the low byte (SubUni / `Address` field). Bit 7 of `net` is ignored.
    pub fn from_net_low(net: u8, low: u8) -> Self {
        pa(net, low)
    }
    /// Raw 15-bit value.
    pub fn raw(self) -> u16 {
        self.0
    }
    /// Net (bits 14-8).
    pub fn net(self) -> u8 {
        (self.0 >> 8) as u8
    }
    /// Sub-Net (bits 7-4).
    pub fn sub_net(self) -> u8 {
        ((self.0 >> 4) & 0x0F) as u8
    }
    /// Universe (bits 3-0).
    pub fn universe(self) -> u8 {
        (self.0 & 0x0F) as u8
    }
    /// SubUni byte (Sub-Net << 4 | Universe) as used in ArtDmx; also the `Address` byte of the RDM packets.
    pub fn sub_uni(self) -> u8 {
        (self.0 & 0xFF) as u8
    }
}

impl From<PortAddress> for u16 {
    fn from(p: PortAddress) -> u16 {
        p.0
    }
}

impl TryFrom<u16> for PortAddress {
    type Error = ArtNetError;
    fn try_from(v: u16) -> Result<Self, Self::Error> {
        Self::new(v).ok_or(ArtNetError::InvalidField("port_address"))
    }
}

impl fmt::Display for PortAddress {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.net(), self.sub_net(), self.universe())
    }
}

impl FromStr for PortAddress {
    type Err = ArtNetError;
    /// Parse `"Net:Sub:Uni"` (decimal parts) or a decimal number 0..=32767.
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        const E: ArtNetError = ArtNetError::InvalidField("port_address");
        let s = s.trim();
        let parts: Vec<&str> = s.split(':').collect();
        match parts.as_slice() {
            [n] => n.parse::<u16>().ok().and_then(Self::new).ok_or(E),
            [n, sub, u] => {
                let p = |x: &str| x.trim().parse::<u8>().map_err(|_| E);
                Self::from_parts(p(n)?, p(sub)?, p(u)?).ok_or(E)
            }
            _ => Err(E),
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// ArtPoll
// ---------------------------------------------------------------------------------------------------------------

/// ArtPoll (digest §2.1). 22 bytes; decoders accept the 14-byte minimum with missing fields as zero.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ArtPoll {
    /// Flags byte (bit 5 targeted mode, bit 1 reply on change, ...).
    pub flags: u8,
    /// Lowest diagnostics priority wanted.
    pub diag_priority: u8,
    /// Targeted mode upper bound (big-endian on the wire).
    pub target_port_address_top: u16,
    /// Targeted mode lower bound.
    pub target_port_address_bottom: u16,
    /// ESTA manufacturer code of the sender (big-endian here, unlike ArtPollReply).
    pub esta_man: u16,
    /// OEM code of the sender.
    pub oem: u16,
}

impl ArtPoll {
    /// Full packet length.
    pub const LEN: usize = 22;
    /// Minimum accepted length.
    pub const MIN_LEN: usize = 14;
    /// Flags bit 1: send ArtPollReply whenever node conditions change.
    pub const FLAG_REPLY_ON_CHANGE: u8 = 1 << 1;
    /// Flags bit 2: send diagnostics messages.
    pub const FLAG_DIAG: u8 = 1 << 2;
    /// Flags bit 3: diagnostics unicast (only with bit 2).
    pub const FLAG_DIAG_UNICAST: u8 = 1 << 3;
    /// Flags bit 4: disable VLC transmission.
    pub const FLAG_VLC_DISABLE: u8 = 1 << 4;
    /// Flags bit 5: targeted mode.
    pub const FLAG_TARGETED: u8 = 1 << 5;
    /// DiagPriority DpLow (Table 5).
    pub const DP_LOW: u8 = 0x10;

    /// The poll Pixelmapper sends: reply-on-change, DiagPriority DpLow, our ESTA code and the OEM placeholder
    /// (identical to the `poll_reply_on_change` vector).
    pub fn new_reply_on_change() -> Self {
        Self {
            flags: Self::FLAG_REPLY_ON_CHANGE,
            diag_priority: Self::DP_LOW,
            esta_man: crate::pids::ESTA_MANUFACTURER,
            oem: crate::pids::ARTNET_OEM_NODE,
            ..Default::default()
        }
    }

    /// Like [`Self::new_reply_on_change`], in targeted mode for `bottom..=top`.
    pub fn new_targeted(bottom: PortAddress, top: PortAddress) -> Self {
        Self {
            flags: Self::FLAG_REPLY_ON_CHANGE | Self::FLAG_TARGETED,
            target_port_address_top: top.raw(),
            target_port_address_bottom: bottom.raw(),
            ..Self::new_reply_on_change()
        }
    }

    /// Whether targeted mode is enabled.
    pub fn targeted_mode(&self) -> bool {
        self.flags & Self::FLAG_TARGETED != 0
    }
    /// Whether the sender wants replies on change.
    pub fn reply_on_change(&self) -> bool {
        self.flags & Self::FLAG_REPLY_ON_CHANGE != 0
    }

    /// Encode the full 22-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::Poll, &mut out);
        out.push(self.flags);
        out.push(self.diag_priority);
        out.extend_from_slice(&self.target_port_address_top.to_be_bytes());
        out.extend_from_slice(&self.target_port_address_bottom.to_be_bytes());
        out.extend_from_slice(&self.esta_man.to_be_bytes());
        out.extend_from_slice(&self.oem.to_be_bytes());
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::Poll, true, Self::MIN_LEN)?;
        let r = Rd(buf);
        Ok(Self {
            flags: r.u8(12),
            diag_priority: r.u8(13),
            target_port_address_top: r.be16(14),
            target_port_address_bottom: r.be16(16),
            esta_man: r.be16(18),
            oem: r.be16(20),
        })
    }
}

// ---------------------------------------------------------------------------------------------------------------
// ArtPollReply
// ---------------------------------------------------------------------------------------------------------------

/// ArtPollReply (digest §2.2): 239 bytes, minimum 207 (fields after `mac` read as 0 if absent). Has **no**
/// ProtVer field. `port` and `esta_man` are little-endian on the wire.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArtPollReply {
    /// Node IP address.
    pub ip: Ipv4Addr,
    /// UDP port, always 0x1936 (little-endian on the wire).
    pub port: u16,
    /// Firmware version.
    pub vers_info: u16,
    /// Port-Address bits 14–8 in bits 6–0.
    pub net_switch: u8,
    /// Port-Address bits 7–4 in bits 3–0.
    pub sub_switch: u8,
    /// OEM code.
    pub oem: u16,
    /// UBEA version (0 = none).
    pub ubea_version: u8,
    /// Status1 bit field.
    pub status1: u8,
    /// ESTA manufacturer code (little-endian on the wire).
    pub esta_man: u16,
    /// Short name (max 17 chars).
    pub short_name: String,
    /// Long name (max 63 chars).
    pub long_name: String,
    /// Node report `#xxxx [yyyy] text` (max 63 chars).
    pub node_report: String,
    /// Number of ports (max 4).
    pub num_ports: u16,
    /// Port types (bit 7 output, bit 6 input, bits 5–0 protocol).
    pub port_types: [u8; 4],
    /// GoodInput status per port.
    pub good_input: [u8; 4],
    /// GoodOutputA status per port.
    pub good_output_a: [u8; 4],
    /// Input universe nibble per port.
    pub sw_in: [u8; 4],
    /// Output universe nibble per port.
    pub sw_out: [u8; 4],
    /// sACN priority.
    pub acn_priority: u8,
    /// Macro key inputs.
    pub sw_macro: u8,
    /// Remote trigger inputs.
    pub sw_remote: u8,
    /// Style code (Table 4, 0 = StNode).
    pub style: u8,
    /// MAC address (0 if unknown).
    pub mac: [u8; 6],
    /// IP of the root device.
    pub bind_ip: Ipv4Addr,
    /// Bind index (0 or 1 = root).
    pub bind_index: u8,
    /// Status2 bit field.
    pub status2: u8,
    /// GoodOutputB status per port.
    pub good_output_b: [u8; 4],
    /// Status3 bit field.
    pub status3: u8,
    /// RDMnet & LLRP default responder UID (used as the node's RDM UID).
    pub default_resp_uid: Uid,
    /// User-specific data.
    pub user: u16,
    /// Max ArtDmx refresh rate in Hz.
    pub refresh_rate: u16,
    /// Background queue policy.
    pub background_queue_policy: u8,
}

impl Default for ArtPollReply {
    /// All fields zero, except `port` = 0x1936.
    fn default() -> Self {
        Self {
            ip: Ipv4Addr::UNSPECIFIED,
            port: PORT,
            vers_info: 0,
            net_switch: 0,
            sub_switch: 0,
            oem: 0,
            ubea_version: 0,
            status1: 0,
            esta_man: 0,
            short_name: String::new(),
            long_name: String::new(),
            node_report: String::new(),
            num_ports: 0,
            port_types: [0; 4],
            good_input: [0; 4],
            good_output_a: [0; 4],
            sw_in: [0; 4],
            sw_out: [0; 4],
            acn_priority: 0,
            sw_macro: 0,
            sw_remote: 0,
            style: 0,
            mac: [0; 6],
            bind_ip: Ipv4Addr::UNSPECIFIED,
            bind_index: 0,
            status2: 0,
            good_output_b: [0; 4],
            status3: 0,
            default_resp_uid: Uid::default(),
            user: 0,
            refresh_rate: 0,
            background_queue_policy: 0,
        }
    }
}

impl ArtPollReply {
    /// Full packet length.
    pub const LEN: usize = 239;
    /// Minimum accepted length (up to and including MAC).
    pub const MIN_LEN: usize = 207;
    /// Size of the short-name field (incl. NUL).
    pub const SHORT_NAME_LEN: usize = 18;
    /// Size of the long-name / node-report fields (incl. NUL).
    pub const LONG_NAME_LEN: usize = 64;

    fn port_address(&self, sw: u8) -> PortAddress {
        PortAddress(
            (u16::from(self.net_switch & 0x7F) << 8)
                | (u16::from(self.sub_switch & 0x0F) << 4)
                | u16::from(sw & 0x0F),
        )
    }

    /// Port-Address of output `i` (`Net << 8 | Sub << 4 | SwOut[i] & 0x0F`). Panics if `i >= 4`.
    pub fn port_address_out(&self, i: usize) -> PortAddress {
        self.port_address(self.sw_out[i])
    }

    /// Port-Address of input `i` (`Net << 8 | Sub << 4 | SwIn[i] & 0x0F`). Panics if `i >= 4`.
    pub fn port_address_in(&self, i: usize) -> PortAddress {
        self.port_address(self.sw_in[i])
    }

    /// Encode the full 239-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        out.extend_from_slice(ID);
        out.extend_from_slice(&(OpCode::PollReply as u16).to_le_bytes());
        out.extend_from_slice(&self.ip.octets());
        out.extend_from_slice(&self.port.to_le_bytes());
        out.extend_from_slice(&self.vers_info.to_be_bytes());
        out.push(self.net_switch);
        out.push(self.sub_switch);
        out.extend_from_slice(&self.oem.to_be_bytes());
        out.push(self.ubea_version);
        out.push(self.status1);
        out.extend_from_slice(&self.esta_man.to_le_bytes());
        put_str(&mut out, &self.short_name, Self::SHORT_NAME_LEN);
        put_str(&mut out, &self.long_name, Self::LONG_NAME_LEN);
        put_str(&mut out, &self.node_report, Self::LONG_NAME_LEN);
        out.extend_from_slice(&self.num_ports.to_be_bytes());
        out.extend_from_slice(&self.port_types);
        out.extend_from_slice(&self.good_input);
        out.extend_from_slice(&self.good_output_a);
        out.extend_from_slice(&self.sw_in);
        out.extend_from_slice(&self.sw_out);
        out.push(self.acn_priority);
        out.push(self.sw_macro);
        out.push(self.sw_remote);
        put_zeros(&mut out, 3);
        out.push(self.style);
        out.extend_from_slice(&self.mac);
        out.extend_from_slice(&self.bind_ip.octets());
        out.push(self.bind_index);
        out.push(self.status2);
        out.extend_from_slice(&self.good_output_b);
        out.push(self.status3);
        out.extend_from_slice(&self.default_resp_uid.to_bytes());
        out.extend_from_slice(&self.user.to_be_bytes());
        out.extend_from_slice(&self.refresh_rate.to_be_bytes());
        out.push(self.background_queue_policy);
        put_zeros(&mut out, 10);
        debug_assert_eq!(out.len(), Self::LEN);
        out
    }

    /// Decode a UDP payload (no ProtVer check: this packet has none).
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::PollReply, false, Self::MIN_LEN)?;
        let r = Rd(buf);
        Ok(Self {
            ip: r.ip(10),
            port: r.le16(14),
            vers_info: r.be16(16),
            net_switch: r.u8(18),
            sub_switch: r.u8(19),
            oem: r.be16(20),
            ubea_version: r.u8(22),
            status1: r.u8(23),
            esta_man: r.le16(24),
            short_name: r.string(26, Self::SHORT_NAME_LEN),
            long_name: r.string(44, Self::LONG_NAME_LEN),
            node_report: r.string(108, Self::LONG_NAME_LEN),
            num_ports: r.be16(172),
            port_types: r.arr(174),
            good_input: r.arr(178),
            good_output_a: r.arr(182),
            sw_in: r.arr(186),
            sw_out: r.arr(190),
            acn_priority: r.u8(194),
            sw_macro: r.u8(195),
            sw_remote: r.u8(196),
            style: r.u8(200),
            mac: r.arr(201),
            bind_ip: r.ip(207),
            bind_index: r.u8(211),
            status2: r.u8(212),
            good_output_b: r.arr(213),
            status3: r.u8(217),
            default_resp_uid: r.uid(218),
            user: r.be16(224),
            refresh_rate: r.be16(226),
            background_queue_policy: r.u8(228),
        })
    }
}

// ---------------------------------------------------------------------------------------------------------------
// ArtDmx / ArtSync
// ---------------------------------------------------------------------------------------------------------------

/// ArtDmx (digest §3.1): 18 header bytes + `Length` DMX slots (no start code).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArtDmx {
    /// Sequence 1..=255, 0 = re-sequencing disabled.
    pub sequence: u8,
    /// Physical input port of the source.
    pub physical: u8,
    /// Destination Port-Address.
    pub port_address: PortAddress,
    /// DMX slots 1..n (should be even, 2..=512).
    pub data: Vec<u8>,
}

impl ArtDmx {
    /// Bytes before the DMX data.
    pub const HEADER_LEN: usize = 18;
    /// Maximum number of DMX slots.
    pub const MAX_DATA: usize = 512;

    /// Build a packet, rejecting more than 512 slots.
    pub fn new(
        sequence: u8,
        physical: u8,
        port_address: PortAddress,
        data: Vec<u8>,
    ) -> Result<Self, ArtNetError> {
        if data.len() > Self::MAX_DATA {
            return Err(ArtNetError::InvalidField("length"));
        }
        Ok(Self {
            sequence,
            physical,
            port_address,
            data,
        })
    }

    /// Encode the packet. Never panics: if `data` is longer than 512 bytes (only possible when the struct was
    /// built without [`Self::new`]) only the first 512 slots are sent.
    pub fn encode(&self) -> Vec<u8> {
        let data = &self.data[..self.data.len().min(Self::MAX_DATA)];
        let mut out = Vec::with_capacity(Self::HEADER_LEN + data.len());
        Header::encode(OpCode::Dmx, &mut out);
        out.push(self.sequence);
        out.push(self.physical);
        out.push(self.port_address.sub_uni());
        out.push(self.port_address.net());
        out.extend_from_slice(&(data.len() as u16).to_be_bytes());
        out.extend_from_slice(data);
        out
    }

    /// Decode a UDP payload. Never reads past the payload: a `Length` beyond it is [`ArtNetError::Short`].
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::Dmx, true, Self::HEADER_LEN)?;
        let net = buf[15];
        if net > 0x7F {
            return Err(ArtNetError::InvalidField("net"));
        }
        let port_address = pa(net, buf[14]);
        let len = usize::from(Rd(buf).be16(16));
        if len > Self::MAX_DATA {
            return Err(ArtNetError::InvalidField("length"));
        }
        let data = buf
            .get(Self::HEADER_LEN..Self::HEADER_LEN + len)
            .ok_or(ArtNetError::Short)?
            .to_vec();
        Ok(Self {
            sequence: buf[12],
            physical: buf[13],
            port_address,
            data,
        })
    }
}

/// ArtSync (digest §3.3), 14 bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ArtSync {
    /// Aux1 (send 0).
    pub aux1: u8,
    /// Aux2 (send 0).
    pub aux2: u8,
}

impl ArtSync {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 14;

    /// Encode the 14-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::Sync, &mut out);
        out.push(self.aux1);
        out.push(self.aux2);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::Sync, true, Self::LEN)?;
        Ok(Self {
            aux1: buf[12],
            aux2: buf[13],
        })
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Configuration: ArtAddress, ArtInput, ArtIpProg, ArtIpProgReply, ArtDiagData
// ---------------------------------------------------------------------------------------------------------------

/// ArtAddress `Command` values (digest §4.1). Port 1–3 variants are deprecated in Art-Net 4.
pub mod address_command {
    /// No action.
    pub const AC_NONE: u8 = 0x00;
    /// End merge on the next ArtDmx.
    pub const AC_CANCEL_MERGE: u8 = 0x01;
    /// Front panel indicators normal.
    pub const AC_LED_NORMAL: u8 = 0x02;
    /// Front panel indicators off.
    pub const AC_LED_MUTE: u8 = 0x03;
    /// Fast flash (locate / identify).
    pub const AC_LED_LOCATE: u8 = 0x04;
    /// Reset SIP/text/test/error flags.
    pub const AC_RESET_RX_FLAGS: u8 = 0x05;
    /// Analysis / debug mode on.
    pub const AC_ANALYSIS_ON: u8 = 0x06;
    /// Analysis / debug mode off.
    pub const AC_ANALYSIS_OFF: u8 = 0x07;
    /// Failsafe: hold last state.
    pub const AC_FAIL_HOLD: u8 = 0x08;
    /// Failsafe: outputs to zero.
    pub const AC_FAIL_ZERO: u8 = 0x09;
    /// Failsafe: outputs to full.
    pub const AC_FAIL_FULL: u8 = 0x0A;
    /// Failsafe: play the failsafe scene.
    pub const AC_FAIL_SCENE: u8 = 0x0B;
    /// Record the current output as failsafe scene.
    pub const AC_FAIL_RECORD: u8 = 0x0C;
    /// Port 0 merge LTP.
    pub const AC_MERGE_LTP0: u8 = 0x10;
    /// Port 1 merge LTP (deprecated).
    pub const AC_MERGE_LTP1: u8 = 0x11;
    /// Port 2 merge LTP (deprecated).
    pub const AC_MERGE_LTP2: u8 = 0x12;
    /// Port 3 merge LTP (deprecated).
    pub const AC_MERGE_LTP3: u8 = 0x13;
    /// Port 0 direction output.
    pub const AC_DIRECTION_TX0: u8 = 0x20;
    /// Port 1 direction output (deprecated).
    pub const AC_DIRECTION_TX1: u8 = 0x21;
    /// Port 2 direction output (deprecated).
    pub const AC_DIRECTION_TX2: u8 = 0x22;
    /// Port 3 direction output (deprecated).
    pub const AC_DIRECTION_TX3: u8 = 0x23;
    /// Port 0 direction input (also clears the port's subscriber list).
    pub const AC_DIRECTION_RX0: u8 = 0x30;
    /// Port 1 direction input (deprecated).
    pub const AC_DIRECTION_RX1: u8 = 0x31;
    /// Port 2 direction input (deprecated).
    pub const AC_DIRECTION_RX2: u8 = 0x32;
    /// Port 3 direction input (deprecated).
    pub const AC_DIRECTION_RX3: u8 = 0x33;
    /// Port 0 merge HTP (default).
    pub const AC_MERGE_HTP0: u8 = 0x50;
    /// Port 1 merge HTP (deprecated).
    pub const AC_MERGE_HTP1: u8 = 0x51;
    /// Port 2 merge HTP (deprecated).
    pub const AC_MERGE_HTP2: u8 = 0x52;
    /// Port 3 merge HTP (deprecated).
    pub const AC_MERGE_HTP3: u8 = 0x53;
    /// Port 0 Art-Net for DMX and RDM (default).
    pub const AC_ARTNET_SEL0: u8 = 0x60;
    /// Port 1 Art-Net (deprecated).
    pub const AC_ARTNET_SEL1: u8 = 0x61;
    /// Port 2 Art-Net (deprecated).
    pub const AC_ARTNET_SEL2: u8 = 0x62;
    /// Port 3 Art-Net (deprecated).
    pub const AC_ARTNET_SEL3: u8 = 0x63;
    /// Port 0 sACN for DMX (RDM stays Art-Net).
    pub const AC_ACN_SEL0: u8 = 0x70;
    /// Port 1 sACN (deprecated).
    pub const AC_ACN_SEL1: u8 = 0x71;
    /// Port 2 sACN (deprecated).
    pub const AC_ACN_SEL2: u8 = 0x72;
    /// Port 3 sACN (deprecated).
    pub const AC_ACN_SEL3: u8 = 0x73;
    /// Port 0 clear DMX output buffer.
    pub const AC_CLEAR_OP0: u8 = 0x90;
    /// Port 1 clear output (deprecated).
    pub const AC_CLEAR_OP1: u8 = 0x91;
    /// Port 2 clear output (deprecated).
    pub const AC_CLEAR_OP2: u8 = 0x92;
    /// Port 3 clear output (deprecated).
    pub const AC_CLEAR_OP3: u8 = 0x93;
    /// Port 0 output style delta.
    pub const AC_STYLE_DELTA0: u8 = 0xA0;
    /// Port 1 output style delta (deprecated).
    pub const AC_STYLE_DELTA1: u8 = 0xA1;
    /// Port 2 output style delta (deprecated).
    pub const AC_STYLE_DELTA2: u8 = 0xA2;
    /// Port 3 output style delta (deprecated).
    pub const AC_STYLE_DELTA3: u8 = 0xA3;
    /// Port 0 output style continuous.
    pub const AC_STYLE_CONST0: u8 = 0xB0;
    /// Port 1 output style continuous (deprecated).
    pub const AC_STYLE_CONST1: u8 = 0xB1;
    /// Port 2 output style continuous (deprecated).
    pub const AC_STYLE_CONST2: u8 = 0xB2;
    /// Port 3 output style continuous (deprecated).
    pub const AC_STYLE_CONST3: u8 = 0xB3;
    /// Port 0 RDM enable.
    pub const AC_RDM_ENABLE0: u8 = 0xC0;
    /// Port 1 RDM enable (deprecated).
    pub const AC_RDM_ENABLE1: u8 = 0xC1;
    /// Port 2 RDM enable (deprecated).
    pub const AC_RDM_ENABLE2: u8 = 0xC2;
    /// Port 3 RDM enable (deprecated).
    pub const AC_RDM_ENABLE3: u8 = 0xC3;
    /// Port 0 RDM disable.
    pub const AC_RDM_DISABLE0: u8 = 0xD0;
    /// Port 1 RDM disable (deprecated).
    pub const AC_RDM_DISABLE1: u8 = 0xD1;
    /// Port 2 RDM disable (deprecated).
    pub const AC_RDM_DISABLE2: u8 = 0xD2;
    /// Port 3 RDM disable (deprecated).
    pub const AC_RDM_DISABLE3: u8 = 0xD3;
    /// Background queue policy 0 (0xE0..=0xEF = policy 0..=15).
    pub const AC_BQP0: u8 = 0xE0;

    /// `AcBqp<n>` for background queue policy `n` (0..=15; higher bits of `n` are masked).
    pub const fn ac_bqp(n: u8) -> u8 {
        AC_BQP0 | (n & 0x0F)
    }
}

/// ArtAddress (digest §4.1), 107 bytes. [`Default`] is "change nothing": switches 0x7F ([`Self::NO_CHANGE`]),
/// AcnPriority 255, empty names, `AC_NONE`, BindIndex 1.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArtAddress {
    /// Net switch: programmed only with bit 7 set, 0x00 = reset to hardware switch.
    pub net_switch: u8,
    /// Target bind index (1 = root).
    pub bind_index: u8,
    /// Short name; empty = no change.
    pub short_name: String,
    /// Long name; empty = no change.
    pub long_name: String,
    /// Input universe nibbles (same programming rule as `net_switch`).
    pub sw_in: [u8; 4],
    /// Output universe nibbles (same programming rule as `net_switch`).
    pub sw_out: [u8; 4],
    /// Sub-Net switch (same programming rule as `net_switch`).
    pub sub_switch: u8,
    /// sACN priority 0..=200, 255 = no change.
    pub acn_priority: u8,
    /// Command, see [`address_command`].
    pub command: u8,
}

impl Default for ArtAddress {
    fn default() -> Self {
        Self {
            net_switch: Self::NO_CHANGE,
            bind_index: 1,
            short_name: String::new(),
            long_name: String::new(),
            sw_in: [Self::NO_CHANGE; 4],
            sw_out: [Self::NO_CHANGE; 4],
            sub_switch: Self::NO_CHANGE,
            acn_priority: Self::ACN_PRIORITY_NO_CHANGE,
            command: address_command::AC_NONE,
        }
    }
}

impl ArtAddress {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 107;
    /// Switch value meaning "no change" (project convention, digest §4.1).
    pub const NO_CHANGE: u8 = 0x7F;
    /// Switch value meaning "reset to hardware switch".
    pub const RESET: u8 = 0x00;
    /// AcnPriority meaning "no change".
    pub const ACN_PRIORITY_NO_CHANGE: u8 = 255;

    /// A packet that only carries `cmd` for `bind_index`.
    pub fn command(bind_index: u8, cmd: u8) -> Self {
        Self {
            bind_index,
            command: cmd,
            ..Default::default()
        }
    }

    /// Switch value that programs `v` (sets bit 7).
    pub const fn program(v: u8) -> u8 {
        v | 0x80
    }

    /// Port-Address output `i` would get if Net, Sub-Net and SwOut\[i\] are all programmed. Panics if `i >= 4`.
    pub fn port_address_out(&self, i: usize) -> PortAddress {
        PortAddress(
            (u16::from(self.net_switch & 0x7F) << 8)
                | (u16::from(self.sub_switch & 0x0F) << 4)
                | u16::from(self.sw_out[i] & 0x0F),
        )
    }

    /// Encode the 107-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::Address, &mut out);
        out.push(self.net_switch);
        out.push(self.bind_index);
        put_str(&mut out, &self.short_name, ArtPollReply::SHORT_NAME_LEN);
        put_str(&mut out, &self.long_name, ArtPollReply::LONG_NAME_LEN);
        out.extend_from_slice(&self.sw_in);
        out.extend_from_slice(&self.sw_out);
        out.push(self.sub_switch);
        out.push(self.acn_priority);
        out.push(self.command);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::Address, true, Self::LEN)?;
        let r = Rd(buf);
        Ok(Self {
            net_switch: r.u8(12),
            bind_index: r.u8(13),
            short_name: r.string(14, ArtPollReply::SHORT_NAME_LEN),
            long_name: r.string(32, ArtPollReply::LONG_NAME_LEN),
            sw_in: r.arr(96),
            sw_out: r.arr(100),
            sub_switch: r.u8(104),
            acn_priority: r.u8(105),
            command: r.u8(106),
        })
    }
}

/// ArtInput (digest §4.2), 20 bytes. Only affects DMX inputs.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ArtInput {
    /// Target bind index.
    pub bind_index: u8,
    /// Number of ports (max 4).
    pub num_ports: u16,
    /// Per input: bit 0 = disable ([`Self::DISABLE`]).
    pub input: [u8; 4],
}

impl ArtInput {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 20;
    /// `input[i]` bit 0: disable this input.
    pub const DISABLE: u8 = 0x01;

    /// Encode the 20-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::Input, &mut out);
        out.push(0);
        out.push(self.bind_index);
        out.extend_from_slice(&self.num_ports.to_be_bytes());
        out.extend_from_slice(&self.input);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::Input, true, Self::LEN)?;
        let r = Rd(buf);
        Ok(Self {
            bind_index: r.u8(13),
            num_ports: r.be16(14),
            input: r.arr(16),
        })
    }
}

/// ArtIpProg `Command` bit: enable programming.
pub const IPPROG_ENABLE: u8 = 0x80;
/// ArtIpProg `Command` bit: enable DHCP (lower bits ignored).
pub const IPPROG_DHCP: u8 = 0x40;
/// ArtIpProg `Command` bit: program default gateway.
pub const IPPROG_GATEWAY: u8 = 0x10;
/// ArtIpProg `Command` bit: reset IP, mask and port to defaults.
pub const IPPROG_RESET: u8 = 0x08;
/// ArtIpProg `Command` bit: program IP.
pub const IPPROG_IP: u8 = 0x04;
/// ArtIpProg `Command` bit: program subnet mask.
pub const IPPROG_MASK: u8 = 0x02;
/// ArtIpProg `Command` bit: program port (deprecated).
pub const IPPROG_PORT: u8 = 0x01;

/// ArtIpProg (digest §4.3), 34 bytes. `command == 0` is an enquiry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ArtIpProg {
    /// Command bits (`IPPROG_*`).
    pub command: u8,
    /// IP to program.
    pub prog_ip: Ipv4Addr,
    /// Subnet mask to program.
    pub prog_sm: Ipv4Addr,
    /// Port (deprecated).
    pub prog_port: u16,
    /// Default gateway to program.
    pub prog_dg: Ipv4Addr,
}

impl Default for ArtIpProg {
    fn default() -> Self {
        Self {
            command: 0,
            prog_ip: Ipv4Addr::UNSPECIFIED,
            prog_sm: Ipv4Addr::UNSPECIFIED,
            prog_port: 0,
            prog_dg: Ipv4Addr::UNSPECIFIED,
        }
    }
}

impl ArtIpProg {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 34;

    /// Encode the 34-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::IpProg, &mut out);
        put_zeros(&mut out, 2);
        out.push(self.command);
        out.push(0);
        out.extend_from_slice(&self.prog_ip.octets());
        out.extend_from_slice(&self.prog_sm.octets());
        out.extend_from_slice(&self.prog_port.to_be_bytes());
        out.extend_from_slice(&self.prog_dg.octets());
        put_zeros(&mut out, 4);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::IpProg, true, Self::LEN)?;
        let r = Rd(buf);
        Ok(Self {
            command: r.u8(14),
            prog_ip: r.ip(16),
            prog_sm: r.ip(20),
            prog_port: r.be16(24),
            prog_dg: r.ip(26),
        })
    }
}

/// ArtIpProgReply (digest §4.4), 34 bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ArtIpProgReply {
    /// Node IP.
    pub prog_ip: Ipv4Addr,
    /// Node subnet mask.
    pub prog_sm: Ipv4Addr,
    /// Port (deprecated).
    pub prog_port: u16,
    /// Status: bit 6 = DHCP enabled.
    pub status: u8,
    /// Default gateway.
    pub prog_dg: Ipv4Addr,
}

impl Default for ArtIpProgReply {
    fn default() -> Self {
        Self {
            prog_ip: Ipv4Addr::UNSPECIFIED,
            prog_sm: Ipv4Addr::UNSPECIFIED,
            prog_port: 0,
            status: 0,
            prog_dg: Ipv4Addr::UNSPECIFIED,
        }
    }
}

impl ArtIpProgReply {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 34;
    /// Status bit 6: DHCP enabled.
    pub const STATUS_DHCP: u8 = 0x40;

    /// Whether the node reports DHCP enabled.
    pub fn dhcp_enabled(&self) -> bool {
        self.status & Self::STATUS_DHCP != 0
    }

    /// Encode the 34-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::IpProgReply, &mut out);
        put_zeros(&mut out, 4);
        out.extend_from_slice(&self.prog_ip.octets());
        out.extend_from_slice(&self.prog_sm.octets());
        out.extend_from_slice(&self.prog_port.to_be_bytes());
        out.push(self.status);
        out.push(0);
        out.extend_from_slice(&self.prog_dg.octets());
        put_zeros(&mut out, 2);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::IpProgReply, true, Self::LEN)?;
        let r = Rd(buf);
        Ok(Self {
            prog_ip: r.ip(16),
            prog_sm: r.ip(20),
            prog_port: r.be16(24),
            status: r.u8(26),
            prog_dg: r.ip(28),
        })
    }
}

/// ArtDiagData (digest §4.5): 18 bytes + `Length` bytes of NUL-terminated ASCII (max 512 incl. NUL).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ArtDiagData {
    /// Priority (Table 5: 0x10 low … 0xF0 volatile).
    pub diag_priority: u8,
    /// Logical port (0 = general, informative only).
    pub logical_port: u8,
    /// Message text (without the terminating NUL).
    pub text: String,
}

impl ArtDiagData {
    /// Bytes before the text.
    pub const HEADER_LEN: usize = 18;
    /// Maximum `Length` (text bytes incl. NUL).
    pub const MAX_DATA: usize = 512;

    /// Encode; the text is truncated to 511 bytes and NUL-terminated.
    pub fn encode(&self) -> Vec<u8> {
        let text = truncate_str(&self.text, Self::MAX_DATA - 1);
        let len = text.len() + 1;
        let mut out = Vec::with_capacity(Self::HEADER_LEN + len);
        Header::encode(OpCode::DiagData, &mut out);
        out.push(0);
        out.push(self.diag_priority);
        out.push(self.logical_port);
        out.push(0);
        out.extend_from_slice(&(len as u16).to_be_bytes());
        out.extend_from_slice(text);
        out.push(0);
        out
    }

    /// Decode a UDP payload; text ends at the first NUL or at `Length`.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::DiagData, true, Self::HEADER_LEN)?;
        let len = usize::from(Rd(buf).be16(16));
        if len > Self::MAX_DATA {
            return Err(ArtNetError::InvalidField("length"));
        }
        let data = buf
            .get(Self::HEADER_LEN..Self::HEADER_LEN + len)
            .ok_or(ArtNetError::Short)?;
        Ok(Self {
            diag_priority: buf[13],
            logical_port: buf[14],
            text: decode_cstr(data),
        })
    }
}

// ---------------------------------------------------------------------------------------------------------------
// RDM: ArtTodRequest, ArtTodData, ArtTodControl, ArtRdm
// ---------------------------------------------------------------------------------------------------------------

/// ArtTodRequest / ArtTodData command value TodFull.
pub const TOD_FULL: u8 = 0x00;
/// ArtTodData command response TodNak (TOD not available / discovery incomplete).
pub const TOD_NAK: u8 = 0xFF;

/// ArtTodRequest (digest §5.1), 56 bytes. Requests the TOD of up to 32 Port-Addresses in one Net; does not
/// start discovery.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ArtTodRequest {
    /// Net (bits 14–8 of all requested Port-Addresses).
    pub net: u8,
    /// Command ([`TOD_FULL`]).
    pub command: u8,
    /// Low bytes of the requested Port-Addresses (`AddCount = len`, max 32).
    pub addresses: Vec<u8>,
}

impl ArtTodRequest {
    /// Full packet length (always sent).
    pub const LEN: usize = 56;
    /// Minimum accepted length (up to AddCount); `AddCount` address bytes must follow.
    pub const MIN_LEN: usize = 24;
    /// Maximum number of addresses.
    pub const MAX_ADDRESSES: usize = 32;

    /// Request for the given Port-Addresses. Entries with a Net other than `net` are skipped, and at most 32
    /// are taken.
    pub fn for_port_addresses(net: u8, port_addresses: &[PortAddress]) -> Self {
        Self {
            net,
            command: TOD_FULL,
            addresses: port_addresses
                .iter()
                .filter(|p| p.net() == net)
                .take(Self::MAX_ADDRESSES)
                .map(|p| p.sub_uni())
                .collect(),
        }
    }

    /// The requested Port-Addresses.
    pub fn port_addresses(&self) -> Vec<PortAddress> {
        self.addresses.iter().map(|&a| pa(self.net, a)).collect()
    }

    /// Encode the 56-byte packet (only the first 32 addresses are sent).
    pub fn encode(&self) -> Vec<u8> {
        let addrs = &self.addresses[..self.addresses.len().min(Self::MAX_ADDRESSES)];
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::TodRequest, &mut out);
        put_zeros(&mut out, 9);
        out.push(self.net);
        out.push(self.command);
        out.push(addrs.len() as u8);
        out.extend_from_slice(addrs);
        out.resize(Self::LEN, 0);
        out
    }

    /// Decode a UDP payload. `AddCount > 32` is invalid; fewer address bytes than `AddCount` is
    /// [`ArtNetError::Short`].
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::TodRequest, true, Self::MIN_LEN)?;
        let n = usize::from(buf[23]);
        if n > Self::MAX_ADDRESSES {
            return Err(ArtNetError::InvalidField("add_count"));
        }
        let addresses = buf
            .get(Self::MIN_LEN..Self::MIN_LEN + n)
            .ok_or(ArtNetError::Short)?
            .to_vec();
        Ok(Self {
            net: buf[21],
            command: buf[22],
            addresses,
        })
    }
}

/// ArtTodData (digest §5.2): 28 + 6 · UidCount bytes, at most [`Self::MAX_UIDS`] UIDs per packet.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ArtTodData {
    /// RDM version (0x01 = RDM Standard V1.0).
    pub rdm_ver: u8,
    /// Physical port 1–4.
    pub port: u8,
    /// Bind index (matches the ArtPollReply).
    pub bind_index: u8,
    /// Net of the Port-Address.
    pub net: u8,
    /// [`TOD_FULL`] or [`TOD_NAK`].
    pub command_response: u8,
    /// Low byte of the Port-Address.
    pub address: u8,
    /// Total number of UIDs of this Port-Address (over all packets).
    pub uid_total: u16,
    /// Packet index (0 for the first).
    pub block_count: u8,
    /// UIDs in this packet (`UidCount = len`).
    pub uids: Vec<Uid>,
}

impl ArtTodData {
    /// Bytes before the UID list (also the minimum length).
    pub const HEADER_LEN: usize = 28;
    /// Maximum UIDs per packet.
    pub const MAX_UIDS: usize = 200;
    /// RDM version: RDM Standard V1.0.
    pub const RDM_VER_1_0: u8 = 0x01;

    /// Port-Address (`Net << 8 | Address`).
    pub fn port_address(&self) -> PortAddress {
        pa(self.net, self.address)
    }

    /// Encode; only the first 200 UIDs are sent.
    pub fn encode(&self) -> Vec<u8> {
        let uids = &self.uids[..self.uids.len().min(Self::MAX_UIDS)];
        let mut out = Vec::with_capacity(Self::HEADER_LEN + 6 * uids.len());
        Header::encode(OpCode::TodData, &mut out);
        out.push(self.rdm_ver);
        out.push(self.port);
        put_zeros(&mut out, 6);
        out.push(self.bind_index);
        out.push(self.net);
        out.push(self.command_response);
        out.push(self.address);
        out.extend_from_slice(&self.uid_total.to_be_bytes());
        out.push(self.block_count);
        out.push(uids.len() as u8);
        for u in uids {
            out.extend_from_slice(&u.to_bytes());
        }
        out
    }

    /// Decode a UDP payload. `UidCount` beyond the payload is [`ArtNetError::Short`].
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::TodData, true, Self::HEADER_LEN)?;
        let n = usize::from(buf[27]);
        let list = buf
            .get(Self::HEADER_LEN..Self::HEADER_LEN + 6 * n)
            .ok_or(ArtNetError::Short)?;
        let r = Rd(list);
        Ok(Self {
            rdm_ver: buf[12],
            port: buf[13],
            bind_index: buf[20],
            net: buf[21],
            command_response: buf[22],
            address: buf[23],
            uid_total: Rd(buf).be16(24),
            block_count: buf[26],
            uids: (0..n).map(|i| r.uid(6 * i)).collect(),
        })
    }
}

/// ArtTodControl (digest §5.3), 24 bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ArtTodControl {
    /// Net of the Port-Address.
    pub net: u8,
    /// Command (`ATC_*`).
    pub command: u8,
    /// Low byte of the Port-Address.
    pub address: u8,
}

impl ArtTodControl {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 24;
    /// No action.
    pub const ATC_NONE: u8 = 0x00;
    /// Flush the TOD and run full discovery.
    pub const ATC_FLUSH: u8 = 0x01;
    /// End running discovery, keep the TOD.
    pub const ATC_END: u8 = 0x02;
    /// Incremental discovery on.
    pub const ATC_INC_ON: u8 = 0x03;
    /// Incremental discovery off.
    pub const ATC_INC_OFF: u8 = 0x04;

    /// Command `cmd` for Port-Address `pa`.
    pub fn new(pa: PortAddress, cmd: u8) -> Self {
        Self {
            net: pa.net(),
            command: cmd,
            address: pa.sub_uni(),
        }
    }

    /// Port-Address (`Net << 8 | Address`).
    pub fn port_address(&self) -> PortAddress {
        pa(self.net, self.address)
    }

    /// Encode the 24-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::TodControl, &mut out);
        put_zeros(&mut out, 9);
        out.push(self.net);
        out.push(self.command);
        out.push(self.address);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::TodControl, true, Self::LEN)?;
        Ok(Self {
            net: buf[21],
            command: buf[22],
            address: buf[23],
        })
    }
}

/// ArtRdm (digest §5.4): 24 bytes + the RDM message **without** start code 0xCC.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ArtRdm {
    /// RDM version (0x01 = RDM Standard V1.0).
    pub rdm_ver: u8,
    /// Free entries in the gateway's RDM queue (0 = no queue).
    pub fifo_avail: u8,
    /// Size of that queue (0 = no queue).
    pub fifo_max: u8,
    /// Net of the Port-Address.
    pub net: u8,
    /// Command (0x00 ArProcess).
    pub command: u8,
    /// Low byte of the Port-Address.
    pub address: u8,
    /// RDM bytes from the sub-start code (0x01) to the checksum.
    pub rdm_packet: Vec<u8>,
}

impl ArtRdm {
    /// Bytes before the RDM packet (also the minimum length).
    pub const HEADER_LEN: usize = 24;
    /// Command ArProcess.
    pub const AR_PROCESS: u8 = 0x00;

    /// Wrap `msg` for Port-Address `pa` (RdmVer 1, FIFO fields 0, start code stripped).
    pub fn from_message(pa: PortAddress, msg: &RdmMessage) -> Self {
        let mut bytes = msg.encode();
        bytes.remove(0);
        Self {
            rdm_ver: ArtTodData::RDM_VER_1_0,
            fifo_avail: 0,
            fifo_max: 0,
            net: pa.net(),
            command: Self::AR_PROCESS,
            address: pa.sub_uni(),
            rdm_packet: bytes,
        }
    }

    /// Decode the carried RDM message (start code 0xCC prepended).
    pub fn message(&self) -> Result<RdmMessage, RdmError> {
        let mut bytes = Vec::with_capacity(self.rdm_packet.len() + 1);
        bytes.push(crate::rdm::START_CODE);
        bytes.extend_from_slice(&self.rdm_packet);
        RdmMessage::decode(&bytes)
    }

    /// Port-Address (`Net << 8 | Address`).
    pub fn port_address(&self) -> PortAddress {
        pa(self.net, self.address)
    }

    /// Encode the packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::HEADER_LEN + self.rdm_packet.len());
        Header::encode(OpCode::Rdm, &mut out);
        out.push(self.rdm_ver);
        put_zeros(&mut out, 6);
        out.push(self.fifo_avail);
        out.push(self.fifo_max);
        out.push(self.net);
        out.push(self.command);
        out.push(self.address);
        out.extend_from_slice(&self.rdm_packet);
        out
    }

    /// Decode a UDP payload; everything after byte 24 is the RDM packet.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::Rdm, true, Self::HEADER_LEN)?;
        Ok(Self {
            rdm_ver: buf[12],
            fifo_avail: buf[19],
            fifo_max: buf[20],
            net: buf[21],
            command: buf[22],
            address: buf[23],
            rdm_packet: buf[Self::HEADER_LEN..].to_vec(),
        })
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Firmware update
// ---------------------------------------------------------------------------------------------------------------

/// ArtFirmwareMaster (digest §6.1), 1064 bytes: one 1024-byte block of the firmware file.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ArtFirmwareMaster {
    /// Block type (`FIRM_*` / `UBEA_*`), the spec's `Type` field.
    pub kind: u8,
    /// Block number (0 in the first packet, wraps after 255).
    pub block_id: u8,
    /// Total file length in 16-bit words, header included.
    pub firmware_length: u32,
    /// Block data, up to 1024 bytes (zero-padded on the wire; decode always yields 1024 bytes).
    pub data: Vec<u8>,
}

impl ArtFirmwareMaster {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 1064;
    /// Offset of the data block.
    pub const DATA_OFFSET: usize = 40;
    /// Data bytes per block.
    pub const BLOCK_SIZE: usize = 1024;
    /// First firmware block.
    pub const FIRM_FIRST: u8 = 0x00;
    /// Continuation firmware block.
    pub const FIRM_CONT: u8 = 0x01;
    /// Last firmware block.
    pub const FIRM_LAST: u8 = 0x02;
    /// First UBEA block.
    pub const UBEA_FIRST: u8 = 0x03;
    /// Continuation UBEA block.
    pub const UBEA_CONT: u8 = 0x04;
    /// Last UBEA block.
    pub const UBEA_LAST: u8 = 0x05;

    /// Build a block, rejecting more than 1024 data bytes.
    pub fn new(
        kind: u8,
        block_id: u8,
        firmware_length: u32,
        data: Vec<u8>,
    ) -> Result<Self, ArtNetError> {
        if data.len() > Self::BLOCK_SIZE {
            return Err(ArtNetError::InvalidField("data"));
        }
        Ok(Self {
            kind,
            block_id,
            firmware_length,
            data,
        })
    }

    /// Encode the 1064-byte packet. Data is zero-padded to 1024 bytes; more than 1024 bytes (only possible
    /// without [`Self::new`]) are cut off.
    pub fn encode(&self) -> Vec<u8> {
        let data = &self.data[..self.data.len().min(Self::BLOCK_SIZE)];
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::FirmwareMaster, &mut out);
        put_zeros(&mut out, 2);
        out.push(self.kind);
        out.push(self.block_id);
        out.extend_from_slice(&self.firmware_length.to_be_bytes());
        put_zeros(&mut out, 20);
        out.extend_from_slice(data);
        out.resize(Self::LEN, 0);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::FirmwareMaster, true, Self::LEN)?;
        Ok(Self {
            kind: buf[14],
            block_id: buf[15],
            firmware_length: Rd(buf).be32(16),
            data: buf[Self::DATA_OFFSET..Self::LEN].to_vec(),
        })
    }
}

/// ArtFirmwareReply (digest §6.2), 36 bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ArtFirmwareReply {
    /// Reply type (`FIRM_*`).
    pub kind: u8,
}

impl ArtFirmwareReply {
    /// Packet length (also the minimum accepted).
    pub const LEN: usize = 36;
    /// Block received OK.
    pub const FIRM_BLOCK_GOOD: u8 = 0x00;
    /// Whole file received OK.
    pub const FIRM_ALL_GOOD: u8 = 0x01;
    /// Any failure.
    pub const FIRM_FAIL: u8 = 0xFF;

    /// Encode the 36-byte packet.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(Self::LEN);
        Header::encode(OpCode::FirmwareReply, &mut out);
        put_zeros(&mut out, 2);
        out.push(self.kind);
        out.resize(Self::LEN, 0);
        out
    }

    /// Decode a UDP payload.
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        check(buf, OpCode::FirmwareReply, true, Self::LEN)?;
        Ok(Self { kind: buf[14] })
    }
}

/// ArtRdmSub (digest §5.5). Not supported by this project (contract §6.2); decoded as [`ArtPacket::Other`].
#[derive(Debug, Clone, Default)]
pub struct ArtRdmSub;

// ---------------------------------------------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------------------------------------------

/// Any packet, dispatched on the OpCode.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ArtPacket {
    /// ArtPoll.
    Poll(ArtPoll),
    /// ArtPollReply.
    PollReply(ArtPollReply),
    /// ArtDmx.
    Dmx(ArtDmx),
    /// ArtSync.
    Sync(ArtSync),
    /// ArtAddress.
    Address(ArtAddress),
    /// ArtInput.
    Input(ArtInput),
    /// ArtIpProg.
    IpProg(ArtIpProg),
    /// ArtIpProgReply.
    IpProgReply(ArtIpProgReply),
    /// ArtDiagData.
    DiagData(ArtDiagData),
    /// ArtTodRequest.
    TodRequest(ArtTodRequest),
    /// ArtTodData.
    TodData(ArtTodData),
    /// ArtTodControl.
    TodControl(ArtTodControl),
    /// ArtRdm.
    Rdm(ArtRdm),
    /// ArtFirmwareMaster.
    FirmwareMaster(ArtFirmwareMaster),
    /// ArtFirmwareReply.
    FirmwareReply(ArtFirmwareReply),
    /// Valid ID but an OpCode this crate does not decode (ProtVer not checked).
    Other {
        /// Raw OpCode.
        op_code: u16,
    },
}

impl ArtPacket {
    /// Decode any Art-Net packet. Errors as the per-packet decoders; unknown OpCodes give [`ArtPacket::Other`].
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        let op_code = peek_op_code(buf)?;
        let Some(op) = OpCode::from_u16(op_code) else {
            return Ok(Self::Other { op_code });
        };
        Ok(match op {
            OpCode::Poll => Self::Poll(ArtPoll::decode(buf)?),
            OpCode::PollReply => Self::PollReply(ArtPollReply::decode(buf)?),
            OpCode::Dmx => Self::Dmx(ArtDmx::decode(buf)?),
            OpCode::Sync => Self::Sync(ArtSync::decode(buf)?),
            OpCode::Address => Self::Address(ArtAddress::decode(buf)?),
            OpCode::Input => Self::Input(ArtInput::decode(buf)?),
            OpCode::IpProg => Self::IpProg(ArtIpProg::decode(buf)?),
            OpCode::IpProgReply => Self::IpProgReply(ArtIpProgReply::decode(buf)?),
            OpCode::DiagData => Self::DiagData(ArtDiagData::decode(buf)?),
            OpCode::TodRequest => Self::TodRequest(ArtTodRequest::decode(buf)?),
            OpCode::TodData => Self::TodData(ArtTodData::decode(buf)?),
            OpCode::TodControl => Self::TodControl(ArtTodControl::decode(buf)?),
            OpCode::Rdm => Self::Rdm(ArtRdm::decode(buf)?),
            OpCode::FirmwareMaster => Self::FirmwareMaster(ArtFirmwareMaster::decode(buf)?),
            OpCode::FirmwareReply => Self::FirmwareReply(ArtFirmwareReply::decode(buf)?),
            _ => Self::Other { op_code },
        })
    }

    /// Encode the packet. [`ArtPacket::Other`] encodes as a bare 12-byte header (ID, OpCode, ProtVer 14).
    pub fn encode(&self) -> Vec<u8> {
        match self {
            Self::Poll(p) => p.encode(),
            Self::PollReply(p) => p.encode(),
            Self::Dmx(p) => p.encode(),
            Self::Sync(p) => p.encode(),
            Self::Address(p) => p.encode(),
            Self::Input(p) => p.encode(),
            Self::IpProg(p) => p.encode(),
            Self::IpProgReply(p) => p.encode(),
            Self::DiagData(p) => p.encode(),
            Self::TodRequest(p) => p.encode(),
            Self::TodData(p) => p.encode(),
            Self::TodControl(p) => p.encode(),
            Self::Rdm(p) => p.encode(),
            Self::FirmwareMaster(p) => p.encode(),
            Self::FirmwareReply(p) => p.encode(),
            Self::Other { op_code } => {
                let mut out = Vec::with_capacity(HEADER_LEN);
                Header::encode_raw(*op_code, &mut out);
                out
            }
        }
    }

    /// Raw OpCode of the packet.
    pub fn op_code(&self) -> u16 {
        let op = match self {
            Self::Poll(_) => OpCode::Poll,
            Self::PollReply(_) => OpCode::PollReply,
            Self::Dmx(_) => OpCode::Dmx,
            Self::Sync(_) => OpCode::Sync,
            Self::Address(_) => OpCode::Address,
            Self::Input(_) => OpCode::Input,
            Self::IpProg(_) => OpCode::IpProg,
            Self::IpProgReply(_) => OpCode::IpProgReply,
            Self::DiagData(_) => OpCode::DiagData,
            Self::TodRequest(_) => OpCode::TodRequest,
            Self::TodData(_) => OpCode::TodData,
            Self::TodControl(_) => OpCode::TodControl,
            Self::Rdm(_) => OpCode::Rdm,
            Self::FirmwareMaster(_) => OpCode::FirmwareMaster,
            Self::FirmwareReply(_) => OpCode::FirmwareReply,
            Self::Other { op_code } => return *op_code,
        };
        op as u16
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn uid(m: u16, d: u32) -> Uid {
        Uid::new(m, d)
    }

    fn sample_reply() -> ArtPollReply {
        ArtPollReply {
            ip: Ipv4Addr::new(2, 0, 0, 10),
            vers_info: 0x0100,
            oem: 0x00FF,
            status1: 0xE2,
            esta_man: 0x7FF0,
            short_name: "PM Node".into(),
            long_name: "Pixelmapper Node".into(),
            node_report: "#0001 [0001] Power On Tests successful".into(),
            num_ports: 4,
            port_types: [0x80; 4],
            good_output_a: [0x80, 0, 0, 0],
            sw_out: [0, 1, 2, 3],
            mac: [2, 0, 0, 0x12, 0x34, 0x56],
            bind_ip: Ipv4Addr::new(2, 0, 0, 10),
            bind_index: 1,
            status2: 0xCC,
            good_output_b: [0x60; 4],
            status3: 0x28,
            default_resp_uid: uid(0x7FF0, 0x8012_3456),
            refresh_rate: 44,
            ..Default::default()
        }
    }

    /// One sample of every packet type.
    fn samples() -> Vec<ArtPacket> {
        let pa1 = PortAddress::new(1).unwrap();
        let msg = RdmMessage {
            dest_uid: uid(0x7FF0, 0x8012_3456),
            src_uid: uid(0x7FF0, 1),
            transaction_number: 5,
            port_id_or_response_type: 1,
            message_count: 0,
            sub_device: 0,
            command_class: crate::rdm::cc::GET,
            parameter_id: 0x0060,
            parameter_data: vec![],
        };
        vec![
            ArtPacket::Poll(ArtPoll::new_targeted(pa1, PortAddress::new(4).unwrap())),
            ArtPacket::PollReply(sample_reply()),
            ArtPacket::Dmx(ArtDmx::new(1, 0, pa1, vec![1, 2, 3, 4]).unwrap()),
            ArtPacket::Sync(ArtSync::default()),
            ArtPacket::Address(ArtAddress {
                net_switch: ArtAddress::program(0),
                short_name: "Stage L".into(),
                long_name: "Pixelmapper Node Stage Left".into(),
                sw_out: [0x81, 0x82, 0x83, 0x84],
                sub_switch: ArtAddress::program(0),
                ..Default::default()
            }),
            ArtPacket::Input(ArtInput {
                bind_index: 1,
                num_ports: 4,
                input: [0, ArtInput::DISABLE, 0, 0],
            }),
            ArtPacket::IpProg(ArtIpProg {
                command: IPPROG_ENABLE | IPPROG_IP | IPPROG_MASK,
                prog_ip: Ipv4Addr::new(2, 0, 0, 20),
                prog_sm: Ipv4Addr::new(255, 0, 0, 0),
                ..Default::default()
            }),
            ArtPacket::IpProgReply(ArtIpProgReply {
                prog_ip: Ipv4Addr::new(192, 168, 1, 50),
                prog_sm: Ipv4Addr::new(255, 255, 255, 0),
                status: ArtIpProgReply::STATUS_DHCP,
                prog_dg: Ipv4Addr::new(192, 168, 1, 1),
                ..Default::default()
            }),
            ArtPacket::DiagData(ArtDiagData {
                diag_priority: 0x10,
                logical_port: 1,
                text: "hello".into(),
            }),
            ArtPacket::TodRequest(ArtTodRequest::for_port_addresses(
                0,
                &[pa1, PortAddress::new(2).unwrap()],
            )),
            ArtPacket::TodData(ArtTodData {
                rdm_ver: 1,
                port: 1,
                bind_index: 1,
                address: 1,
                uid_total: 2,
                uids: vec![uid(0x7FF0, 1), uid(0x7FF0, 2)],
                ..Default::default()
            }),
            ArtPacket::TodControl(ArtTodControl::new(pa1, ArtTodControl::ATC_FLUSH)),
            ArtPacket::Rdm(ArtRdm::from_message(pa1, &msg)),
            ArtPacket::FirmwareMaster(
                ArtFirmwareMaster::new(ArtFirmwareMaster::FIRM_FIRST, 0, 538, vec![0xAB; 10])
                    .unwrap(),
            ),
            ArtPacket::FirmwareReply(ArtFirmwareReply {
                kind: ArtFirmwareReply::FIRM_ALL_GOOD,
            }),
        ]
    }

    /// Normalise what decode cannot reproduce (firmware data is always 1024 bytes after decode).
    fn normalise(p: &ArtPacket) -> ArtPacket {
        match p {
            ArtPacket::FirmwareMaster(f) => {
                let mut f = f.clone();
                f.data.resize(ArtFirmwareMaster::BLOCK_SIZE, 0);
                ArtPacket::FirmwareMaster(f)
            }
            other => other.clone(),
        }
    }

    #[test]
    fn roundtrip_all() {
        let s = samples();
        assert_eq!(s.len(), 15);
        for p in &s {
            let bytes = p.encode();
            let d = ArtPacket::decode(&bytes).unwrap_or_else(|e| panic!("{p:?}: {e}"));
            assert_eq!(d, normalise(p));
            assert_eq!(d.op_code(), p.op_code());
            assert_eq!(d.encode(), bytes, "re-encode {p:?}");
        }
    }

    #[test]
    fn lengths() {
        let lens: Vec<usize> = samples().iter().map(|p| p.encode().len()).collect();
        assert_eq!(
            lens,
            [22, 239, 22, 14, 107, 20, 34, 34, 24, 56, 40, 24, 49, 1064, 36]
        );
    }

    #[test]
    fn short_packets_rejected() {
        for p in samples() {
            let bytes = p.encode();
            let min = match &p {
                ArtPacket::Poll(_) => ArtPoll::MIN_LEN,
                ArtPacket::PollReply(_) => ArtPollReply::MIN_LEN,
                ArtPacket::TodRequest(_) => ArtTodRequest::MIN_LEN + 2,
                ArtPacket::Rdm(_) => ArtRdm::HEADER_LEN,
                _ => bytes.len(),
            };
            assert_eq!(
                ArtPacket::decode(&bytes[..min - 1]),
                Err(ArtNetError::Short),
                "{p:?}"
            );
            assert!(ArtPacket::decode(&bytes[..min]).is_ok(), "{p:?}");
        }
    }

    #[test]
    fn trailing_bytes_ignored() {
        for p in samples() {
            let mut bytes = p.encode();
            bytes.extend_from_slice(&[0, 0]);
            let d = ArtPacket::decode(&bytes).unwrap();
            // ArtRdm keeps everything after the header as RDM bytes.
            if !matches!(p, ArtPacket::Rdm(_)) {
                assert_eq!(d, normalise(&p), "{p:?}");
            }
        }
    }

    #[test]
    fn prot_ver_below_14_rejected() {
        for p in samples() {
            if matches!(p, ArtPacket::PollReply(_)) {
                continue;
            }
            let mut bytes = p.encode();
            bytes[11] = 13;
            assert_eq!(
                ArtPacket::decode(&bytes),
                Err(ArtNetError::ProtVer(13)),
                "{p:?}"
            );
        }
    }

    #[test]
    fn poll_reply_has_no_prot_ver() {
        let r = ArtPollReply {
            ip: Ipv4Addr::new(0, 0, 0, 1), // bytes 10/11 = 0 would fail a ProtVer check
            ..sample_reply()
        };
        assert_eq!(ArtPollReply::decode(&r.encode()).unwrap(), r);
    }

    #[test]
    fn wrong_opcode_and_id() {
        let bytes = ArtSync::default().encode();
        assert_eq!(
            ArtPoll::decode(&bytes),
            Err(ArtNetError::UnexpectedOpCode(0x5200))
        );
        let mut bad = bytes.clone();
        bad[0] = b'X';
        assert_eq!(ArtPacket::decode(&bad), Err(ArtNetError::BadId));
        let mut other = bytes;
        other[8..10].copy_from_slice(&0x8400u16.to_le_bytes());
        assert_eq!(
            ArtPacket::decode(&other),
            Ok(ArtPacket::Other { op_code: 0x8400 })
        );
        assert_eq!(ArtPacket::Other { op_code: 0x8400 }.encode(), &other[..12]);
    }

    #[test]
    fn poll_reply_little_endian_fields() {
        let b = sample_reply().encode();
        assert_eq!(b.len(), ArtPollReply::LEN);
        assert_eq!(&b[14..16], &[0x36, 0x19]);
        assert_eq!(&b[24..26], &[0xF0, 0x7F]);
        assert_eq!(&b[16..18], &[0x01, 0x00]); // VersInfo BE
        assert_eq!(&b[218..224], &[0x7F, 0xF0, 0x80, 0x12, 0x34, 0x56]);
    }

    #[test]
    fn poll_reply_min_length_defaults() {
        let r = sample_reply();
        let d = ArtPollReply::decode(&r.encode()[..ArtPollReply::MIN_LEN]).unwrap();
        assert_eq!(d.mac, r.mac);
        assert_eq!(d.bind_ip, Ipv4Addr::UNSPECIFIED);
        assert_eq!(d.default_resp_uid, Uid::default());
        assert_eq!(d.refresh_rate, 0);
    }

    #[test]
    fn poll_reply_port_addresses() {
        let r = ArtPollReply {
            net_switch: 0x82,
            sub_switch: 0x13,
            sw_out: [0x10, 1, 2, 0xFF],
            sw_in: [5, 0, 0, 0],
            ..Default::default()
        };
        assert_eq!(r.port_address_out(0).raw(), 0x0230);
        assert_eq!(r.port_address_out(3).raw(), 0x023F);
        assert_eq!(
            r.port_address_in(0),
            PortAddress::from_parts(2, 3, 5).unwrap()
        );
    }

    #[test]
    fn strings_truncated_and_padded() {
        let r = ArtPollReply {
            short_name: "x".repeat(40),
            long_name: "ä".repeat(40), // 80 bytes, truncated on a char boundary
            ..Default::default()
        };
        let b = r.encode();
        assert_eq!(b.len(), ArtPollReply::LEN);
        assert_eq!(b[26 + 17], 0);
        let d = ArtPollReply::decode(&b).unwrap();
        assert_eq!(d.short_name, "x".repeat(17));
        assert_eq!(d.long_name, "ä".repeat(31));
    }

    #[test]
    fn address_defaults_and_helpers() {
        let a = ArtAddress::command(2, address_command::AC_MERGE_LTP0);
        let b = a.encode();
        assert_eq!(b.len(), ArtAddress::LEN);
        assert_eq!(b[12], 0x7F);
        assert_eq!(b[13], 2);
        assert_eq!(&b[96..104], &[0x7F; 8]);
        assert_eq!(&b[104..107], &[0x7F, 255, 0x10]);
        assert_eq!(ArtAddress::program(3), 0x83);
        assert_eq!(address_command::ac_bqp(5), 0xE5);
    }

    #[test]
    fn dmx_new_and_encode_limits() {
        let pa = PortAddress::new(1).unwrap();
        assert!(ArtDmx::new(0, 0, pa, vec![0; 513]).is_err());
        let d = ArtDmx {
            sequence: 0,
            physical: 0,
            port_address: pa,
            data: vec![7; 600],
        };
        let b = d.encode();
        assert_eq!(b.len(), 18 + 512);
        assert_eq!(&b[16..18], &[0x02, 0x00]);
    }

    #[test]
    fn dmx_length_exceeds_payload() {
        let mut b = ArtDmx::new(1, 0, PortAddress::new(1).unwrap(), vec![1, 2, 3, 4])
            .unwrap()
            .encode();
        b[17] = 6;
        assert_eq!(ArtDmx::decode(&b), Err(ArtNetError::Short));
    }

    #[test]
    fn firmware_master_limits() {
        assert!(ArtFirmwareMaster::new(0, 0, 0, vec![0; 1025]).is_err());
        let f = ArtFirmwareMaster {
            data: vec![1; 2000],
            ..Default::default()
        };
        assert_eq!(f.encode().len(), ArtFirmwareMaster::LEN);
    }

    #[test]
    fn tod_data_count_exceeds_payload() {
        let t = ArtTodData {
            uids: vec![uid(1, 1)],
            ..Default::default()
        };
        let mut b = t.encode();
        b[27] = 2;
        assert_eq!(ArtTodData::decode(&b), Err(ArtNetError::Short));
    }

    #[test]
    fn tod_request_limits() {
        let pas: Vec<PortAddress> = (0..40).map(|i| PortAddress::new(i).unwrap()).collect();
        let r = ArtTodRequest::for_port_addresses(0, &pas);
        assert_eq!(r.addresses.len(), 32);
        assert_eq!(r.port_addresses(), pas[..32]);
        let mut b = r.encode();
        b[23] = 33;
        assert_eq!(
            ArtTodRequest::decode(&b),
            Err(ArtNetError::InvalidField("add_count"))
        );
        let other_net = ArtTodRequest::for_port_addresses(1, &pas);
        assert!(other_net.addresses.is_empty());
    }

    #[test]
    fn rdm_message_roundtrip() {
        let pa = PortAddress::from_parts(1, 2, 3).unwrap();
        let ArtPacket::Rdm(p) = &samples()[12] else {
            panic!("sample 12 is ArtRdm");
        };
        let msg = p.message().unwrap();
        let r = ArtRdm::from_message(pa, &msg);
        assert_eq!(r.rdm_packet[0], crate::rdm::SUB_START_CODE);
        assert_eq!(r.port_address(), pa);
        assert_eq!(r.message().unwrap(), msg);
    }

    #[test]
    fn ip_prog_reply_dhcp() {
        let r = ArtIpProgReply {
            status: 0x40,
            ..Default::default()
        };
        assert!(r.dhcp_enabled());
        assert!(!ArtIpProgReply::default().dhcp_enabled());
    }

    #[test]
    fn port_address_text_and_serde() {
        let pa = PortAddress::from_parts(2, 1, 3).unwrap();
        assert_eq!(pa.to_string(), "2:1:3");
        assert_eq!("2:1:3".parse::<PortAddress>(), Ok(pa));
        assert_eq!("531".parse::<PortAddress>(), Ok(pa));
        assert!("32768".parse::<PortAddress>().is_err());
        assert!("128:0:0".parse::<PortAddress>().is_err());
        assert!("0:16:0".parse::<PortAddress>().is_err());
        assert!("1:2".parse::<PortAddress>().is_err());
        assert_eq!(u16::from(pa), 531);
        assert_eq!(serde_json::to_string(&pa).unwrap(), "531");
        assert_eq!(serde_json::from_str::<PortAddress>("531").unwrap(), pa);
        assert!(serde_json::from_str::<PortAddress>("40000").is_err());
    }

    #[test]
    fn poll_helpers() {
        let p = ArtPoll::new_reply_on_change();
        assert!(p.reply_on_change() && !p.targeted_mode());
        assert_eq!(p.esta_man, 0x7FF0);
    }

    #[test]
    fn no_panic_on_any_prefix_or_mutation() {
        for p in samples() {
            let bytes = p.encode();
            for n in 0..=bytes.len() {
                let _ = ArtPacket::decode(&bytes[..n]);
            }
            // Max out every header/length/count byte position.
            for i in 12..bytes.len().min(40) {
                let mut m = bytes.clone();
                m[i] = 0xFF;
                for n in [m.len(), m.len().saturating_sub(1), 24.min(m.len())] {
                    let _ = ArtPacket::decode(&m[..n]);
                }
            }
        }
    }
}
