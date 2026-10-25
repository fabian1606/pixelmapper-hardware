//! Tube firmware image (`tube_fw-<version>.hex`): Intel HEX with app + 16-byte trailer at 0x17F0,
//! `protocol/image-format.md` §1. Mirrors `read_image`, `trailer_check` and `page_plan` of
//! `tube/tools/flash_rdm.py`; the trailer is only checked, never computed.

use super::ihex;
use crate::crc::{crc16, crc16_ccitt};
use crate::error::{Error, Result};
use crate::pids;

/// Start of the 2 KB bootloader section (end of app space, BOOT_INFO `app_end`).
pub const BOOT_START: u16 = 0x1800;
/// Start of the 16-byte trailer.
pub const TRAILER_START: u16 = BOOT_START - 16;
/// Maximum app length in bytes (6128).
pub const APP_MAX: u16 = TRAILER_START;
/// Trailer bytes covered by the CRC after the app (0x17F0..0x17FB).
pub const TRAILER_META_LEN: usize = 12;
/// Trailer format value.
pub const TRAILER_FORMAT: u8 = 1;
/// Bootloader flash page size.
pub const PAGE: u16 = pids::tube::BOOT_RDM_PAGE_SIZE as u16;
/// Maximum length of one BOOT_APP_CRC request.
pub const CRC_CHUNK: u16 = pids::tube::BOOT_RDM_CRC_MAX_LEN as u16;
/// The page that holds the trailer (0x17C0).
pub const LAST_PAGE: u16 = TRAILER_START & !(PAGE - 1);

/// Bootloader FINISH error codes (`BOOT_ERR_*`), as returned by [`trailer_check`].
pub mod boot_err {
    use crate::pids::tube;
    /// Trailer valid.
    pub const NONE: u8 = tube::BOOT_ERR_NONE as u8;
    /// Page content differs.
    pub const VERIFY: u8 = tube::BOOT_ERR_VERIFY as u8;
    /// Trailer length/CRC wrong.
    pub const APP_INVALID: u8 = tube::BOOT_ERR_APP_INVALID as u8;
    /// Trailer manufacturer/model is not this device.
    pub const MODEL: u8 = tube::BOOT_ERR_MODEL as u8;
    /// Magic/format wrong or blank.
    pub const FORMAT: u8 = tube::BOOT_ERR_FORMAT as u8;

    /// Human-readable text for a code.
    pub fn text(code: u8) -> &'static str {
        match code {
            NONE => "none",
            VERIFY => "verify error (page content differs)",
            APP_INVALID => "app invalid (trailer length/CRC wrong)",
            MODEL => "wrong model (trailer manufacturer/model ID not this device)",
            FORMAT => "unknown trailer format (magic/format, or blank)",
            _ => "unknown error",
        }
    }
}

/// Decoded trailer (all fields little-endian in flash).
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Trailer {
    /// Magic, 0x4D50 when valid.
    pub magic: u16,
    /// Format (1).
    pub format: u8,
    /// ESTA manufacturer ID.
    pub manufacturer: u16,
    /// DEVICE_MODEL_ID.
    pub model: u16,
    /// FW_VERSION_ID 0x00MMmmpp.
    pub version: u32,
    /// App length in bytes.
    pub length: u16,
    /// CRC16 over app + trailer metadata.
    pub crc: u16,
}

impl Trailer {
    /// Read the trailer from a flash image (at least 0x1800 bytes; missing bytes read as 0xFF).
    pub fn parse(img: &[u8]) -> Self {
        let b = |i: u16| {
            img.get(usize::from(TRAILER_START + i))
                .copied()
                .unwrap_or(0xFF)
        };
        let u16le = |i| u16::from_le_bytes([b(i), b(i + 1)]);
        Self {
            magic: u16le(0),
            format: b(2),
            manufacturer: u16le(4),
            model: u16le(6),
            version: u32::from_le_bytes([b(8), b(9), b(10), b(11)]),
            length: u16le(12),
            crc: u16le(14),
        }
    }
}

/// `FW_VERSION_ID` 0x00MMmmpp as `"M.m.p"` (bootloader ids 0xB0... as hex).
pub fn version_str(v: u32) -> String {
    if v >> 24 == 0 {
        format!("{}.{}.{}", (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
    } else {
        format!("0x{v:08X}")
    }
}

/// CRC16 over app `0..length` continued over the trailer metadata.
pub fn trailer_crc(img: &[u8], length: u16) -> u16 {
    let app = img.get(..usize::from(length)).unwrap_or(img);
    let start = usize::from(TRAILER_START);
    let meta = img.get(start..start + TRAILER_META_LEN).unwrap_or(&[]);
    crc16_ccitt(meta, crc16(app))
}

/// Exactly the bootloader's FINISH check -> `boot_err` code. `None` = not checked.
pub fn trailer_check(img: &[u8], manufacturer: Option<u16>, model: Option<u16>) -> u8 {
    let t = Trailer::parse(img);
    if t.magic != pids::TUBE_TRAILER_MAGIC || t.format != TRAILER_FORMAT {
        return boot_err::FORMAT;
    }
    if manufacturer.is_some_and(|m| m != t.manufacturer) || model.is_some_and(|m| m != t.model) {
        return boot_err::MODEL;
    }
    if t.length == 0 || t.length > APP_MAX || trailer_crc(img, t.length) != t.crc {
        return boot_err::APP_INVALID;
    }
    boot_err::NONE
}

/// Summary of a tube image for a preview (no network needed).
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TrailerInfo {
    /// ESTA manufacturer ID.
    pub manufacturer: u16,
    /// DEVICE_MODEL_ID.
    pub model: u16,
    /// FW_VERSION_ID.
    pub version: u32,
    /// Version as text, e.g. `"0.5.0"`.
    pub version_label: String,
    /// App length in bytes.
    pub length: u16,
    /// Maximum app length.
    pub max_length: u16,
    /// Trailer CRC16.
    pub crc: u16,
}

/// A checked tube image: flash content 0x0000..0x17FF (0xFF where the hex has no data) and its trailer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TubeImage {
    /// Flash bytes 0..0x1800.
    pub img: Vec<u8>,
    /// The (valid) trailer.
    pub trailer: Trailer,
}

impl TubeImage {
    /// Parse and check Intel HEX text (the finished `tube_fw-<version>.hex`).
    pub fn parse_hex(text: &str) -> Result<Self> {
        let mem = ihex::parse(text)?;
        let (Some((&lo, _)), Some((&hi, _))) = (mem.first_key_value(), mem.last_key_value()) else {
            return Err(Error::Image("no data".into()));
        };
        if hi >= u32::from(BOOT_START) {
            return Err(Error::Image(format!(
                "data at 0x{hi:04X} - an app image ends at 0x{:04X} (no bootloader in an update image)",
                BOOT_START - 1
            )));
        }
        let _ = lo;
        let mut img = vec![0xFFu8; usize::from(BOOT_START)];
        for (&a, &b) in &mem {
            img[a as usize] = b;
        }
        let t = Trailer::parse(&img);
        match trailer_check(&img, None, None) {
            boot_err::NONE => {}
            boot_err::FORMAT => {
                return Err(Error::Image(format!(
                    "no trailer (magic 0x{:04X}, format {}) - use .pio/build/isp/tube_fw-<version>.hex, not firmware.hex",
                    t.magic, t.format
                )))
            }
            _ if (t.length, t.crc) == (0xFFFF, 0xFFFF) => {
                return Err(Error::Image(
                    "trailer metadata but no length/CRC - this is the linker output, use tube_fw-<version>.hex".into(),
                ))
            }
            _ => {
                return Err(Error::Image(format!(
                    "trailer length/CRC wrong (length {}, crc 0x{:04X}, computed 0x{:04X}) - image damaged",
                    t.length,
                    t.crc,
                    trailer_crc(&img, t.length.min(APP_MAX))
                )))
            }
        }
        if let Some(&a) = mem
            .keys()
            .find(|&&a| a >= u32::from(t.length) && a < u32::from(TRAILER_START))
        {
            return Err(Error::Image(format!(
                "data at 0x{a:04X} behind the app length {} (not covered by the CRC)",
                t.length
            )));
        }
        Ok(Self { img, trailer: t })
    }

    /// App length in bytes.
    pub fn length(&self) -> u16 {
        self.trailer.length
    }

    /// Preview information.
    pub fn info(&self) -> TrailerInfo {
        TrailerInfo {
            manufacturer: self.trailer.manufacturer,
            model: self.trailer.model,
            version: self.trailer.version,
            version_label: version_str(self.trailer.version),
            length: self.trailer.length,
            max_length: APP_MAX,
            crc: self.trailer.crc,
        }
    }

    /// One page of the image.
    pub fn page(&self, addr: u16) -> Vec<u8> {
        let a = usize::from(addr);
        self.img[a..a + usize::from(PAGE)].to_vec()
    }

    /// Write plan: trailer page first with the trailer erased (0xFF), then the app pages (all-0xFF pages skipped
    /// if `skip_blank`), then the trailer page with the real trailer last.
    pub fn page_plan(&self, skip_blank: bool) -> Vec<(u16, Vec<u8>)> {
        let mut first = self.img[usize::from(LAST_PAGE)..usize::from(TRAILER_START)].to_vec();
        first.resize(usize::from(PAGE), 0xFF);
        let mut plan = vec![(LAST_PAGE, first)];
        for a in (0..self.length()).step_by(usize::from(PAGE)) {
            if a == LAST_PAGE {
                continue;
            }
            let p = self.page(a);
            if skip_blank && p.iter().all(|&b| b == 0xFF) {
                continue;
            }
            plan.push((a, p));
        }
        plan.push((LAST_PAGE, self.page(LAST_PAGE)));
        plan
    }

    /// CRC chunks to compare with BOOT_APP_CRC: `(address, length, expected crc)` over the app in
    /// ≤ 256-byte pieces plus the 16-byte trailer.
    pub fn crc_chunks(&self) -> Vec<(u16, u16, u16)> {
        let len = self.length();
        let mut out: Vec<(u16, u16, u16)> = (0..len)
            .step_by(usize::from(CRC_CHUNK))
            .map(|a| {
                let n = CRC_CHUNK.min(len - a);
                (a, n, crc16(&self.img[usize::from(a)..usize::from(a + n)]))
            })
            .collect();
        let t = usize::from(TRAILER_START);
        out.push((TRAILER_START, 16, crc16(&self.img[t..t + 16])));
        out
    }
}

/// Build a test image like the tube_fw post-build step (app + complete trailer); for tests and the simulator.
/// `app` bytes are placed at 0; the length is rounded up to even.
pub fn build_test_image(app: &[u8], manufacturer: u16, model: u16, version: u32) -> TubeImage {
    let mut img = vec![0xFFu8; usize::from(BOOT_START)];
    img[..app.len()].copy_from_slice(app);
    let length = ((app.len() + 1) & !1) as u16;
    let t = usize::from(TRAILER_START);
    img[t..t + 2].copy_from_slice(&pids::TUBE_TRAILER_MAGIC.to_le_bytes());
    img[t + 2] = TRAILER_FORMAT;
    img[t + 3] = 0xFF;
    img[t + 4..t + 6].copy_from_slice(&manufacturer.to_le_bytes());
    img[t + 6..t + 8].copy_from_slice(&model.to_le_bytes());
    img[t + 8..t + 12].copy_from_slice(&version.to_le_bytes());
    img[t + 12..t + 14].copy_from_slice(&length.to_le_bytes());
    let crc = trailer_crc(&img, length);
    img[t + 14..t + 16].copy_from_slice(&crc.to_le_bytes());
    let trailer = Trailer::parse(&img);
    TubeImage { img, trailer }
}

impl TubeImage {
    /// Intel HEX text of this image (app + trailer), as the tube_fw build writes it.
    pub fn to_hex(&self) -> String {
        let mem = self
            .img
            .iter()
            .enumerate()
            .filter(|(a, _)| *a < usize::from(self.length()) || *a >= usize::from(TRAILER_START))
            .map(|(a, &b)| (a as u32, b))
            .collect();
        ihex::write(&mem)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn app() -> Vec<u8> {
        let mut a: Vec<u8> = (0..1001u32).map(|i| (i * 7) as u8).collect();
        a.extend(std::iter::repeat_n(0xFF, 199));
        a.extend(std::iter::repeat_n(0x5A, 101));
        a
    }

    #[test]
    fn constants() {
        assert_eq!(
            (TRAILER_START, APP_MAX, LAST_PAGE, PAGE, CRC_CHUNK),
            (0x17F0, 6128, 0x17C0, 64, 256)
        );
    }

    #[test]
    fn parse_and_plan() {
        let im = build_test_image(&app(), 0x7FF0, 1, 0x400);
        assert_eq!(im.length(), 1302);
        let parsed = TubeImage::parse_hex(&im.to_hex()).unwrap();
        assert_eq!(parsed, im);
        assert_eq!(
            trailer_check(&im.img, Some(0x7FF0), Some(1)),
            boot_err::NONE
        );
        assert_eq!(
            trailer_check(&im.img, Some(0x7FF0), Some(2)),
            boot_err::MODEL
        );
        let plan = im.page_plan(true);
        assert_eq!(plan[0].0, LAST_PAGE);
        assert!(plan[0].1[48..].iter().all(|&b| b == 0xFF));
        assert_eq!(plan.last().unwrap().1, im.page(LAST_PAGE));
        assert!(!plan.iter().any(|p| p.0 == 1088));
        assert!(im.page_plan(false).iter().any(|p| p.0 == 1088));
        assert_eq!(im.info().version_label, "0.4.0");
        let chunks = im.crc_chunks();
        assert_eq!(chunks.len(), 7);
        assert_eq!(chunks[5], (1280, 22, crc16(&im.img[1280..1302])));
    }

    #[test]
    fn half_written_never_valid() {
        let old = build_test_image(&[0x11; 3000], 0x7FF0, 1, 0x301);
        let new = build_test_image(&app(), 0x7FF0, 1, 0x400);
        let mut flash = old.img.clone();
        let plan = new.page_plan(false);
        for (i, (a, d)) in plan.iter().enumerate() {
            flash[usize::from(*a)..usize::from(*a) + 64].copy_from_slice(d);
            let e = trailer_check(&flash, Some(0x7FF0), Some(1));
            if i + 1 < plan.len() {
                assert_eq!(e, boot_err::FORMAT);
            } else {
                assert_eq!(e, boot_err::NONE);
            }
        }
    }

    #[test]
    fn rejects() {
        let im = build_test_image(&app(), 0x7FF0, 1, 0x400);
        let mut bad = im.clone();
        bad.img[5] ^= 1;
        assert!(TubeImage::parse_hex(&bad.to_hex()).is_err());
        let mut bad = im.clone();
        bad.img[usize::from(TRAILER_START)] = 0;
        assert!(TubeImage::parse_hex(&bad.to_hex()).is_err());
        let mut text = im.to_hex();
        text.insert_str(0, ":010578000082\n"); // stray byte at 1400, behind the app length
        assert!(TubeImage::parse_hex(&text).is_err());
        assert!(TubeImage::parse_hex(":0218000000FFE7\n").is_err());
        assert!(TubeImage::parse_hex("").is_err());
    }
}
