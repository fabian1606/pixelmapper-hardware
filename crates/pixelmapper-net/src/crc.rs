//! Checksums used by the firmware images: CRC16-CCITT (tube trailer, BOOT_APP_CRC) and CRC-32 (node image).

/// CRC16-CCITT, polynomial 0x1021, not reflected, continued from `crc` (start with [`CRC16_INIT`]).
pub fn crc16_ccitt(data: &[u8], mut crc: u16) -> u16 {
    for &b in data {
        crc ^= u16::from(b) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
        }
    }
    crc
}

/// Initial value of the tube CRC16.
pub const CRC16_INIT: u16 = 0xFFFF;

/// CRC16-CCITT with init 0xFFFF over `data` (what BOOT_APP_CRC returns).
pub fn crc16(data: &[u8]) -> u16 {
    crc16_ccitt(data, CRC16_INIT)
}

/// CRC-32 (ISO-HDLC, as zlib): reflected poly 0xEDB88320, init and xor-out 0xFFFFFFFF.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc ^= u32::from(b);
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn check_values() {
        assert_eq!(crc16(b"123456789"), 0x29B1); // CRC-16/CCITT-FALSE
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc16_ccitt(b"6789", crc16(b"12345")), 0x29B1);
    }
}
