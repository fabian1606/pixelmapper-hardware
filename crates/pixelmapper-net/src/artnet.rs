//! Art-Net 4 packets (UDP port 6454).
//!
//! Every packet starts with the 12-byte header `"Art-Net\0"` + OpCode (little-endian) + ProtVer (big-endian,
//! 14), see `protocol/artnet4-digest.md` §0 and §1. Implemented so far: [`ArtPoll`] (§2.1) and [`ArtDmx`] (§3.1).
//! The remaining packet types the node uses are declared as stubs below; their layouts are in the digest
//! (§2.2 ArtPollReply, §3.3 ArtSync, §4 configuration, §5 RDM, §6 firmware update).

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
    /// Packet shorter than the minimum length, or a length field points past the payload.
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

    /// Decode and validate ID and ProtVer (>= 14).
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        if buf.len() < HEADER_LEN {
            return Err(ArtNetError::Short);
        }
        if &buf[..8] != ID {
            return Err(ArtNetError::BadId);
        }
        let op_code = u16::from_le_bytes([buf[8], buf[9]]);
        let prot_ver = u16::from_be_bytes([buf[10], buf[11]]);
        if prot_ver < PROT_VER {
            return Err(ArtNetError::ProtVer(prot_ver));
        }
        Ok(Self { op_code, prot_ver })
    }

    /// Append the 12 header bytes for `op` with ProtVer 14.
    pub fn encode(op: OpCode, out: &mut Vec<u8>) {
        out.extend_from_slice(ID);
        out.extend_from_slice(&(op as u16).to_le_bytes());
        out.extend_from_slice(&PROT_VER.to_be_bytes());
    }
}

fn decode_expect(buf: &[u8], op: OpCode) -> Result<Header, ArtNetError> {
    let h = Header::decode(buf)?;
    if h.op_code != op as u16 {
        return Err(ArtNetError::UnexpectedOpCode(h.op_code));
    }
    Ok(h)
}

fn be16(buf: &[u8], off: usize) -> u16 {
    // Missing trailing bytes count as zero (ArtPoll minimum-length rule).
    let b = |i: usize| buf.get(i).copied().unwrap_or(0);
    u16::from_be_bytes([b(off), b(off + 1)])
}

/// 15-bit Port-Address: Net (7 bit) | Sub-Net (4 bit) | Universe (4 bit), digest §0 "Port-Address".
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PortAddress(u16);

impl PortAddress {
    /// Create from a raw value; `None` if above 32767.
    pub fn new(raw: u16) -> Option<Self> {
        (raw <= 0x7FFF).then_some(Self(raw))
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
    /// SubUni byte (Sub-Net << 4 | Universe) as used in ArtDmx.
    pub fn sub_uni(self) -> u8 {
        (self.0 & 0xFF) as u8
    }
}

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
    /// Flags bit 5: targeted mode.
    pub const FLAG_TARGETED: u8 = 1 << 5;

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
        decode_expect(buf, OpCode::Poll)?;
        if buf.len() < Self::MIN_LEN {
            return Err(ArtNetError::Short);
        }
        Ok(Self {
            flags: buf[12],
            diag_priority: buf[13],
            target_port_address_top: be16(buf, 14),
            target_port_address_bottom: be16(buf, 16),
            esta_man: be16(buf, 18),
            oem: be16(buf, 20),
        })
    }
}

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

    /// Encode the packet. Panics if `data` is longer than 512 bytes.
    pub fn encode(&self) -> Vec<u8> {
        assert!(self.data.len() <= Self::MAX_DATA, "ArtDmx data > 512");
        let mut out = Vec::with_capacity(Self::HEADER_LEN + self.data.len());
        Header::encode(OpCode::Dmx, &mut out);
        out.push(self.sequence);
        out.push(self.physical);
        out.push(self.port_address.sub_uni());
        out.push(self.port_address.net());
        out.extend_from_slice(&(self.data.len() as u16).to_be_bytes());
        out.extend_from_slice(&self.data);
        out
    }

    /// Decode a UDP payload. Never reads past the payload: a `Length` beyond it is [`ArtNetError::Short`].
    pub fn decode(buf: &[u8]) -> Result<Self, ArtNetError> {
        decode_expect(buf, OpCode::Dmx)?;
        if buf.len() < Self::HEADER_LEN {
            return Err(ArtNetError::Short);
        }
        let net = buf[15];
        if net > 0x7F {
            return Err(ArtNetError::InvalidField("net"));
        }
        let port_address = PortAddress((u16::from(net) << 8) | u16::from(buf[14]));
        let len = usize::from(be16(buf, 16));
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

/// ArtPollReply (digest §2.2, 239 bytes, min 207). TODO: fields + encode/decode.
#[derive(Debug, Clone, Default)]
pub struct ArtPollReply;
/// ArtSync (digest §3.3, 14 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtSync;
/// ArtAddress (digest §4.1, 107 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtAddress;
/// ArtInput (digest §4.2, 20 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtInput;
/// ArtIpProg (digest §4.3, 34 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtIpProg;
/// ArtIpProgReply (digest §4.4, 34 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtIpProgReply;
/// ArtDiagData (digest §4.5). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtDiagData;
/// ArtTodRequest (digest §5.1, 56 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtTodRequest;
/// ArtTodData (digest §5.2, 28 + 6 * UidCount bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtTodData;
/// ArtTodControl (digest §5.3, 24 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtTodControl;
/// ArtRdm (digest §5.4, 24 + n bytes; carries an RDM message without start code). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtRdm;
/// ArtRdmSub (digest §5.5). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtRdmSub;
/// ArtFirmwareMaster (digest §6.1, 1064 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtFirmwareMaster;
/// ArtFirmwareReply (digest §6.2, 36 bytes). TODO.
#[derive(Debug, Clone, Default)]
pub struct ArtFirmwareReply;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn poll_roundtrip() {
        let p = ArtPoll {
            flags: ArtPoll::FLAG_REPLY_ON_CHANGE,
            diag_priority: 0x10,
            esta_man: 0x7FF0,
            oem: 0x00FF,
            ..Default::default()
        };
        let bytes = p.encode();
        assert_eq!(bytes.len(), ArtPoll::LEN);
        assert_eq!(&bytes[8..12], &[0x00, 0x20, 0x00, 0x0e]);
        assert_eq!(ArtPoll::decode(&bytes).unwrap(), p);
    }

    #[test]
    fn dmx_roundtrip() {
        let d = ArtDmx {
            sequence: 7,
            physical: 1,
            port_address: PortAddress::from_parts(2, 1, 3).unwrap(),
            data: vec![1, 2, 3, 4],
        };
        assert_eq!(ArtDmx::decode(&d.encode()).unwrap(), d);
    }

    #[test]
    fn opcode_lookup() {
        assert_eq!(OpCode::from_u16(0xF200), Some(OpCode::FirmwareMaster));
        assert_eq!(OpCode::from_u16(0x1234), None);
    }
}
