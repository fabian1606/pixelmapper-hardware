//! Binary DMX frame sets (`protocol/bridge-ws.md` §7): the WebSocket framing between app and bridge, not an
//! Art-Net packet. Layout: `"PM"` magic, kind 0x01, flags (bit 0 = ArtSync), count u16 LE, then per entry
//! route u16 LE, length u16 LE (1–512), `length` slot bytes.

use bytes::Bytes;

/// Magic bytes `"PM"`.
pub const FRAME_MAGIC: [u8; 2] = *b"PM";
/// Message kind: DMX frame set (app → bridge).
pub const KIND_DMX_SET: u8 = 0x01;
/// Flag bit: send ArtSync after the set.
pub const FLAG_SYNC: u8 = 0x01;
/// Header bytes before the first entry.
pub const HEADER_BYTES: usize = 6;
/// Bytes before the slot data of every entry.
pub const ENTRY_HEADER_BYTES: usize = 4;
/// Largest DMX payload of one entry.
pub const MAX_DMX_LENGTH: usize = 512;

/// One routed universe of a frame set.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameEntry {
    /// Route id from the connection's `dmxRoutes` table.
    pub route: u16,
    /// Slot values, slot 1 first, 1–512 bytes (zero-copy slice of the message).
    pub data: Bytes,
}

/// A decoded frame set.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameSet {
    /// Flag bit 0: send ArtSync after the set.
    pub sync: bool,
    /// Entries in message order.
    pub entries: Vec<FrameEntry>,
}

/// Why a binary message is not a valid frame set.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum FrameError {
    /// Shorter than the 6-byte header.
    #[error("binary message shorter than the frame-set header")]
    TooShort,
    /// Does not start with `"PM"`.
    #[error("binary message without \"PM\" magic")]
    BadMagic,
    /// Kind other than 0x01 (0x02 is reserved for DMX input, bridge → app).
    #[error("unsupported binary message kind 0x{0:02x}")]
    UnknownKind(u8),
    /// An entry header or its data runs past the end of the message.
    #[error("frame-set entry {0} is truncated")]
    Truncated(usize),
    /// An entry length outside 1–512.
    #[error("frame-set entry {entry} has invalid length {length}")]
    BadLength {
        /// Entry index.
        entry: usize,
        /// Length field.
        length: usize,
    },
}

/// Decodes a frame set. Trailing bytes after the last entry and unknown flag bits are ignored.
pub fn decode_frame_set(msg: &Bytes) -> Result<FrameSet, FrameError> {
    if msg.len() < HEADER_BYTES {
        return Err(FrameError::TooShort);
    }
    if msg[0..2] != FRAME_MAGIC {
        return Err(FrameError::BadMagic);
    }
    if msg[2] != KIND_DMX_SET {
        return Err(FrameError::UnknownKind(msg[2]));
    }
    let sync = msg[3] & FLAG_SYNC != 0;
    let count = usize::from(u16::from_le_bytes([msg[4], msg[5]]));
    let mut entries = Vec::with_capacity(count);
    let mut o = HEADER_BYTES;
    for entry in 0..count {
        let header = msg
            .get(o..o + ENTRY_HEADER_BYTES)
            .ok_or(FrameError::Truncated(entry))?;
        let route = u16::from_le_bytes([header[0], header[1]]);
        let length = usize::from(u16::from_le_bytes([header[2], header[3]]));
        o += ENTRY_HEADER_BYTES;
        if !(1..=MAX_DMX_LENGTH).contains(&length) {
            return Err(FrameError::BadLength { entry, length });
        }
        if o + length > msg.len() {
            return Err(FrameError::Truncated(entry));
        }
        entries.push(FrameEntry {
            route,
            data: msg.slice(o..o + length),
        });
        o += length;
    }
    Ok(FrameSet { sync, entries })
}

/// Encodes a frame set (used by tests and Rust clients). Data longer than 512 bytes is cut; empty entries skipped.
pub fn encode_frame_set(entries: &[(u16, &[u8])], sync: bool) -> Vec<u8> {
    let usable: Vec<(u16, &[u8])> = entries
        .iter()
        .filter(|(_, d)| !d.is_empty())
        .map(|&(r, d)| (r, &d[..d.len().min(MAX_DMX_LENGTH)]))
        .collect();
    let mut out =
        Vec::with_capacity(HEADER_BYTES + usable.iter().map(|(_, d)| 4 + d.len()).sum::<usize>());
    out.extend_from_slice(&FRAME_MAGIC);
    out.push(KIND_DMX_SET);
    out.push(if sync { FLAG_SYNC } else { 0 });
    out.extend_from_slice(&(usable.len() as u16).to_le_bytes());
    for (route, data) in usable {
        out.extend_from_slice(&route.to_le_bytes());
        out.extend_from_slice(&(data.len() as u16).to_le_bytes());
        out.extend_from_slice(data);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let raw = encode_frame_set(&[(1, &[1, 2, 3]), (7, &[9; 512]), (3, &[])], true);
        let set = decode_frame_set(&Bytes::from(raw)).unwrap();
        assert!(set.sync);
        assert_eq!(set.entries.len(), 2);
        assert_eq!(
            set.entries[0],
            FrameEntry {
                route: 1,
                data: Bytes::from_static(&[1, 2, 3])
            }
        );
        assert_eq!(set.entries[1].route, 7);
        assert_eq!(set.entries[1].data.len(), 512);
    }

    #[test]
    fn malformed() {
        assert_eq!(
            decode_frame_set(&Bytes::from_static(&[0])),
            Err(FrameError::TooShort)
        );
        assert_eq!(
            decode_frame_set(&Bytes::from_static(b"XX\x01\x00\x00\x00")),
            Err(FrameError::BadMagic)
        );
        assert_eq!(
            decode_frame_set(&Bytes::from_static(b"PM\x02\x00\x00\x00")),
            Err(FrameError::UnknownKind(2))
        );
        // count 1 but no entry header
        assert_eq!(
            decode_frame_set(&Bytes::from_static(b"PM\x01\x00\x01\x00")),
            Err(FrameError::Truncated(0))
        );
        // length 3 but only 2 data bytes
        let msg = Bytes::from_static(b"PM\x01\x00\x01\x00\x01\x00\x03\x00ab");
        assert_eq!(decode_frame_set(&msg), Err(FrameError::Truncated(0)));
        // length 0
        let msg = Bytes::from_static(b"PM\x01\x00\x01\x00\x01\x00\x00\x00");
        assert_eq!(
            decode_frame_set(&msg),
            Err(FrameError::BadLength {
                entry: 0,
                length: 0
            })
        );
        // length 513
        let msg = Bytes::from_static(b"PM\x01\x00\x01\x00\x01\x00\x01\x02");
        assert_eq!(
            decode_frame_set(&msg),
            Err(FrameError::BadLength {
                entry: 0,
                length: 513
            })
        );
        // empty set is valid
        assert_eq!(
            decode_frame_set(&Bytes::from_static(b"PM\x01\x01\x00\x00"))
                .unwrap()
                .entries
                .len(),
            0
        );
    }
}
