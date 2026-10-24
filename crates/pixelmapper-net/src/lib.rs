//! Host-side networking for the Pixelmapper hardware: Art-Net 4 towards the node, RDM (E1.20) towards the
//! LED tubes behind it, and the firmware-update flows for both.
//!
//! Wire formats follow `protocol/artnet4-digest.md` and `protocol/rdm-digest.md`; the shared test vectors in
//! `protocol/vectors/` are checked by `tests/vectors.rs`. Project-specific behaviour (PIDs, flash flow,
//! node config) is fixed in `protocol/contract.md`.

#![warn(missing_docs)]

pub mod artnet;
pub mod crc;
pub mod error;
pub mod event;
pub mod flash;
pub mod image;
pub mod link;
pub mod rdm;
pub mod sim;
pub mod state;

pub use error::{Error, Result};

/// Manufacturer-specific PIDs and constants, generated from `protocol/pids.toml` by `protocol/gen.py`.
#[allow(missing_docs, dead_code, clippy::all)]
#[rustfmt::skip]
#[path = "pids_generated.rs"]
pub mod pids;

/// Parse a hex string (whitespace ignored, case-insensitive) into bytes.
pub fn parse_hex(s: &str) -> Option<Vec<u8>> {
    let digits: Vec<u8> = s.bytes().filter(|b| !b.is_ascii_whitespace()).collect();
    if !digits.len().is_multiple_of(2) {
        return None;
    }
    digits
        .chunks(2)
        .map(|pair| {
            let hi = (pair[0] as char).to_digit(16)?;
            let lo = (pair[1] as char).to_digit(16)?;
            Some((hi * 16 + lo) as u8)
        })
        .collect()
}

/// Format bytes as a lowercase hex string without separators.
pub fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_roundtrip() {
        assert_eq!(parse_hex("CC 01 ff").unwrap(), vec![0xcc, 0x01, 0xff]);
        assert_eq!(to_hex(&[0xcc, 0x01]), "cc01");
        assert!(parse_hex("abc").is_none());
        assert!(parse_hex("zz").is_none());
    }
}
