//! Identifiers and small value types of the bridge contract (`protocol/bridge-ws.md` §2–§3): node ids, RDM UIDs,
//! port references, MAC addresses and hex-encoded parameter data. All of them serialize to the exact JSON strings
//! the contract prescribes.

use serde::{de, Deserialize, Deserializer, Serialize, Serializer};
use std::fmt;
use std::net::Ipv4Addr;
use std::str::FromStr;

// ── NodeId ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Stable identity of an Art-Net node: `"7FF0:8A1B2C3D"` (UID), else `"mac:02:7f:f0:1b:2c:3d"`, else `"ip:2.0.0.5"`.
///
/// The backend forms it (contract §3); the server treats it as an opaque string.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(transparent)]
pub struct NodeId(pub String);

impl NodeId {
    /// Node id from the node's `DefaultRespUID`.
    pub fn from_uid(uid: RdmUid) -> Self {
        Self(uid.to_string())
    }

    /// Fallback node id from the MAC address (UID all-zero).
    pub fn from_mac(mac: MacAddress) -> Self {
        Self(format!("mac:{mac}"))
    }

    /// Last-resort node id from the IP (UID and MAC all-zero; not stable).
    pub fn from_ip(ip: Ipv4Addr) -> Self {
        Self(format!("ip:{ip}"))
    }

    /// The id as a string slice.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for NodeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl From<&str> for NodeId {
    fn from(s: &str) -> Self {
        Self(s.to_owned())
    }
}

// ── RdmUid ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// RDM UID, serialized as `"MMMM:DDDDDDDD"` (uppercase hex). Parsing accepts either case.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RdmUid {
    /// ESTA manufacturer id.
    pub manufacturer: u16,
    /// Device id.
    pub device: u32,
}

impl RdmUid {
    /// All-devices broadcast `FFFF:FFFFFFFF`.
    pub const BROADCAST: RdmUid = RdmUid {
        manufacturer: 0xFFFF,
        device: 0xFFFF_FFFF,
    };

    /// Creates a UID from its parts.
    pub const fn new(manufacturer: u16, device: u32) -> Self {
        Self {
            manufacturer,
            device,
        }
    }

    /// True for the all-devices broadcast and for manufacturer broadcasts (`MMMM:FFFFFFFF`).
    pub fn is_broadcast(self) -> bool {
        self.device == 0xFFFF_FFFF
    }
}

impl fmt::Display for RdmUid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:04X}:{:08X}", self.manufacturer, self.device)
    }
}

/// Error for malformed identifier strings.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("{0}")]
pub struct ParseIdError(pub &'static str);

impl FromStr for RdmUid {
    type Err = ParseIdError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        const MSG: &str = "expected an RDM UID \"MMMM:DDDDDDDD\" (hex)";
        let (m, d) = s.split_once(':').ok_or(ParseIdError(MSG))?;
        if m.len() != 4
            || d.len() != 8
            || !m.bytes().chain(d.bytes()).all(|b| b.is_ascii_hexdigit())
        {
            return Err(ParseIdError(MSG));
        }
        let manufacturer = u16::from_str_radix(m, 16).map_err(|_| ParseIdError(MSG))?;
        let device = u32::from_str_radix(d, 16).map_err(|_| ParseIdError(MSG))?;
        Ok(Self {
            manufacturer,
            device,
        })
    }
}

impl Serialize for RdmUid {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.collect_str(self)
    }
}

impl<'de> Deserialize<'de> for RdmUid {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        s.parse().map_err(de::Error::custom)
    }
}

// ── PortRef ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// One port of a node: `BindIndex` of its ArtPollReply (1–255) and the port within that reply (0–3).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub struct PortRef {
    /// BindIndex, 1–255.
    pub bind: u8,
    /// Port within that reply, 0–3.
    pub index: u8,
}

impl PortRef {
    /// Creates a port reference.
    pub const fn new(bind: u8, index: u8) -> Self {
        Self { bind, index }
    }

    /// True when `bind` is 1–255 and `index` is 0–3 (contract §3).
    pub fn is_valid(self) -> bool {
        self.bind >= 1 && self.index <= 3
    }
}

impl fmt::Display for PortRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}", self.bind, self.index)
    }
}

// ── MacAddress ────────────────────────────────────────────────────────────────────────────────────────────────────

/// MAC address, serialized as `"aa:bb:cc:dd:ee:ff"` (lowercase).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct MacAddress(pub [u8; 6]);

impl MacAddress {
    /// True for `00:00:00:00:00:00` (field not filled in by the node).
    pub fn is_zero(self) -> bool {
        self.0 == [0; 6]
    }
}

impl fmt::Display for MacAddress {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let b = self.0;
        write!(
            f,
            "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            b[0], b[1], b[2], b[3], b[4], b[5]
        )
    }
}

impl FromStr for MacAddress {
    type Err = ParseIdError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        const MSG: &str = "expected a MAC address \"aa:bb:cc:dd:ee:ff\"";
        let mut out = [0u8; 6];
        let mut parts = s.split(':');
        for byte in &mut out {
            let p = parts.next().ok_or(ParseIdError(MSG))?;
            if p.len() != 2 {
                return Err(ParseIdError(MSG));
            }
            *byte = u8::from_str_radix(p, 16).map_err(|_| ParseIdError(MSG))?;
        }
        if parts.next().is_some() {
            return Err(ParseIdError(MSG));
        }
        Ok(Self(out))
    }
}

impl Serialize for MacAddress {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.collect_str(self)
    }
}

impl<'de> Deserialize<'de> for MacAddress {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        s.parse().map_err(de::Error::custom)
    }
}

// ── HexData ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// Binary data (RDM parameter data) as a hex string without separators; serialized lowercase, parsed in either
/// case. `""` is empty data.
#[derive(Debug, Clone, Default, PartialEq, Eq, Hash)]
pub struct HexData(pub Vec<u8>);

impl HexData {
    /// Lowercase hex encoding.
    pub fn to_hex(&self) -> String {
        self.0.iter().map(|b| format!("{b:02x}")).collect()
    }

    /// Parses a hex string (even length, hex digits only, no separators).
    pub fn parse(s: &str) -> Result<Self, ParseIdError> {
        const MSG: &str = "expected hex data without separators (even number of hex digits)";
        if !s.len().is_multiple_of(2) || !s.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(ParseIdError(MSG));
        }
        let bytes = s
            .as_bytes()
            .chunks(2)
            .map(|pair| {
                // Both bytes are ASCII hex digits (checked above), so the slice is valid UTF-8 and parses.
                std::str::from_utf8(pair)
                    .ok()
                    .and_then(|p| u8::from_str_radix(p, 16).ok())
                    .ok_or(ParseIdError(MSG))
            })
            .collect::<Result<Vec<u8>, _>>()?;
        Ok(Self(bytes))
    }
}

impl From<Vec<u8>> for HexData {
    fn from(v: Vec<u8>) -> Self {
        Self(v)
    }
}

impl Serialize for HexData {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&self.to_hex())
    }
}

impl<'de> Deserialize<'de> for HexData {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        Self::parse(&s).map_err(de::Error::custom)
    }
}

// ── Port-Address ──────────────────────────────────────────────────────────────────────────────────────────────────

/// Largest 15-bit Port-Address (`net << 8 | subNet << 4 | universe`).
pub const MAX_PORT_ADDRESS: u16 = 0x7FFF;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uid_roundtrip_and_case() {
        let uid: RdmUid = "7ff0:74b6510c".parse().unwrap();
        assert_eq!(uid, RdmUid::new(0x7FF0, 0x74B6_510C));
        assert_eq!(uid.to_string(), "7FF0:74B6510C");
        assert_eq!(serde_json::to_string(&uid).unwrap(), "\"7FF0:74B6510C\"");
        assert!("7FF0-74B6510C".parse::<RdmUid>().is_err());
        assert!("7FF0:74B6510".parse::<RdmUid>().is_err());
        assert!("7FF0:+4B6510C".parse::<RdmUid>().is_err());
        assert!(RdmUid::BROADCAST.is_broadcast());
        assert!("7FF0:FFFFFFFF".parse::<RdmUid>().unwrap().is_broadcast());
    }

    #[test]
    fn mac_roundtrip() {
        let mac: MacAddress = "02:7F:F0:1B:2C:3D".parse().unwrap();
        assert_eq!(mac.to_string(), "02:7f:f0:1b:2c:3d");
        assert!("02:7f:f0:1b:2c".parse::<MacAddress>().is_err());
        assert!("02:7f:f0:1b:2c:3d:00".parse::<MacAddress>().is_err());
        assert_eq!(NodeId::from_mac(mac).as_str(), "mac:02:7f:f0:1b:2c:3d");
    }

    #[test]
    fn hex_roundtrip() {
        let h = HexData::parse("00FFa0").unwrap();
        assert_eq!(h.0, vec![0x00, 0xFF, 0xA0]);
        assert_eq!(serde_json::to_string(&h).unwrap(), "\"00ffa0\"");
        assert_eq!(HexData::parse("").unwrap().0, Vec::<u8>::new());
        assert!(HexData::parse("abc").is_err());
        assert!(HexData::parse("0g").is_err());
        assert!(HexData::parse("0 1").is_err());
    }

    #[test]
    fn port_ref_validity() {
        assert!(PortRef::new(1, 0).is_valid());
        assert!(PortRef::new(255, 3).is_valid());
        assert!(!PortRef::new(0, 0).is_valid());
        assert!(!PortRef::new(1, 4).is_valid());
    }
}
