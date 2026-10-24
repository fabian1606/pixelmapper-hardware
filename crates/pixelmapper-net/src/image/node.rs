//! Node firmware image: ESP-IDF app binary with the 64-byte description block `pm_image_desc_t` at offset 288,
//! and the Art-Net `.alf` container for ArtFirmwareMaster (`protocol/image-format.md` §2).

use crate::crc::crc32;
use crate::error::{Error, Result};
use crate::pids;

/// Offset of the description block in the ESP-IDF binary (24 + 8 + 256).
pub const DESC_OFFSET: usize = 288;
/// Size of the description block.
pub const DESC_LEN: usize = 64;
/// `.alf` header length (530 words).
pub const ALF_HEADER_LEN: usize = 1060;
/// Offset of the OEM list in the `.alf` header (256 BE words).
pub const ALF_OEM_OFFSET: usize = 34;
/// Number of OEM entries.
pub const ALF_OEM_COUNT: usize = 256;
/// Offset of our description block copy in the `.alf` spare area.
pub const ALF_DESC_OFFSET: usize = 546;
/// Offset of the data length (words, BE u32).
pub const ALF_LENGTH_OFFSET: usize = 1056;

/// `pm_image_desc_t` (all fields little-endian).
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NodeImageDesc {
    /// `NODE_IMAGE_MAGIC` ("PMIM").
    pub magic: u32,
    /// `NODE_IMAGE_FORMAT`.
    pub format: u8,
    /// Block size (64).
    pub size: u8,
    /// ESTA manufacturer.
    pub esta_manufacturer: u16,
    /// DEVICE_MODEL_ID (`MODEL_NODE`).
    pub device_model_id: u16,
    /// FW_VERSION_ID 0x00MMmmpp.
    pub fw_version_id: u32,
    /// Bit n = hardware revision n supported.
    pub hw_rev_mask: u32,
    /// Binary length (0 inside the image).
    pub image_length: u32,
    /// CRC-32 of the binary (0 inside the image).
    pub image_crc32: u32,
    /// Version label, e.g. "0.1.0".
    pub version_label: String,
    /// CRC-32 over bytes 0..60.
    pub desc_crc32: u32,
}

impl NodeImageDesc {
    /// Decode 64 bytes (only the length is checked here, see [`NodeImageDesc::validate`]).
    pub fn decode(b: &[u8]) -> Result<Self> {
        let b = b
            .get(..DESC_LEN)
            .ok_or_else(|| Error::Image("description block shorter than 64 bytes".into()))?;
        let u16le = |o: usize| u16::from_le_bytes([b[o], b[o + 1]]);
        let u32le = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
        let label = &b[28..60];
        let end = label.iter().position(|&c| c == 0).unwrap_or(label.len());
        Ok(Self {
            magic: u32le(0),
            format: b[4],
            size: b[5],
            esta_manufacturer: u16le(8),
            device_model_id: u16le(10),
            fw_version_id: u32le(12),
            hw_rev_mask: u32le(16),
            image_length: u32le(20),
            image_crc32: u32le(24),
            version_label: String::from_utf8_lossy(&label[..end]).into_owned(),
            desc_crc32: u32le(60),
        })
    }

    /// Encode 64 bytes as stored (desc_crc32 as in the struct; see [`NodeImageDesc::seal`]).
    pub fn encode(&self) -> [u8; DESC_LEN] {
        let mut b = [0u8; DESC_LEN];
        b[0..4].copy_from_slice(&self.magic.to_le_bytes());
        b[4] = self.format;
        b[5] = self.size;
        b[8..10].copy_from_slice(&self.esta_manufacturer.to_le_bytes());
        b[10..12].copy_from_slice(&self.device_model_id.to_le_bytes());
        b[12..16].copy_from_slice(&self.fw_version_id.to_le_bytes());
        b[16..20].copy_from_slice(&self.hw_rev_mask.to_le_bytes());
        b[20..24].copy_from_slice(&self.image_length.to_le_bytes());
        b[24..28].copy_from_slice(&self.image_crc32.to_le_bytes());
        let l = self.version_label.as_bytes();
        let n = l.len().min(31);
        b[28..28 + n].copy_from_slice(&l[..n]);
        b[60..64].copy_from_slice(&self.desc_crc32.to_le_bytes());
        b
    }

    /// Set `desc_crc32` to the CRC-32 of bytes 0..60.
    pub fn seal(&mut self) {
        let b = self.encode();
        self.desc_crc32 = crc32(&b[..60]);
    }

    /// New block for the node with defaults from pids.toml (lengths/CRCs 0, not sealed).
    pub fn new(fw_version_id: u32, hw_rev_mask: u32, version_label: &str) -> Self {
        Self {
            magic: pids::NODE_IMAGE_MAGIC,
            format: pids::NODE_IMAGE_FORMAT as u8,
            size: DESC_LEN as u8,
            esta_manufacturer: pids::ESTA_MANUFACTURER,
            device_model_id: pids::MODEL_NODE,
            fw_version_id,
            hw_rev_mask,
            image_length: 0,
            image_crc32: 0,
            version_label: version_label.into(),
            desc_crc32: 0,
        }
    }

    /// Whether hardware revision `rev` is in `hw_rev_mask`.
    pub fn supports_hw_rev(&self, rev: u8) -> bool {
        rev < 32 && self.hw_rev_mask & (1 << rev) != 0
    }

    /// The node's checks 1-5 of image-format.md §2.3 on a filled copy. `Err` holds the `fw_error` code.
    pub fn validate(
        &self,
        manufacturer: u16,
        hw_rev: u8,
        partition_size: u32,
    ) -> std::result::Result<(), u8> {
        use pids::fw_error as e;
        let b = self.encode();
        if self.magic != pids::NODE_IMAGE_MAGIC
            || u16::from(self.format) != pids::NODE_IMAGE_FORMAT
            || usize::from(self.size) != DESC_LEN
            || crc32(&b[..60]) != self.desc_crc32
        {
            return Err(e::DESC_MAGIC);
        }
        if self.esta_manufacturer != manufacturer {
            return Err(e::DESC_MANUFACTURER);
        }
        if self.device_model_id != pids::MODEL_NODE {
            return Err(e::DESC_MODEL);
        }
        if !self.supports_hw_rev(hw_rev) {
            return Err(e::DESC_HW_REV);
        }
        if self.image_length > partition_size {
            return Err(e::TOO_LARGE);
        }
        Ok(())
    }

    /// Compare with the block inside the image, ignoring bytes 20..28 and 60..64 (check 6).
    pub fn matches_image_block(&self, in_image: &[u8]) -> bool {
        let a = self.encode();
        in_image.len() >= DESC_LEN && a[..20] == in_image[..20] && a[28..60] == in_image[28..60]
    }
}

/// A node firmware image ready for transfer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NodeImage {
    /// The ESP-IDF binary.
    pub bin: Vec<u8>,
    /// Filled-in copy of the description block (image_length, image_crc32, desc_crc32 set).
    pub desc: NodeImageDesc,
    /// OEM codes the image is valid for (`.alf` OEM list); `[ARTNET_OEM_NODE]` for a plain binary.
    pub oems: Vec<u16>,
}

/// Summary for a preview.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NodeImageInfo {
    /// Filled description block.
    pub desc: NodeImageDesc,
    /// OEM list.
    pub oems: Vec<u16>,
    /// Binary length.
    pub length: u32,
}

impl NodeImage {
    /// From the ESP-IDF binary (`build/pixelmapper_node.bin`): checks magic/format/size of the block at 288 and
    /// fills length and CRCs.
    pub fn from_bin(bin: Vec<u8>) -> Result<Self> {
        let block = bin
            .get(DESC_OFFSET..DESC_OFFSET + DESC_LEN)
            .ok_or_else(|| Error::Image("binary too short for a description block".into()))?;
        let mut desc = NodeImageDesc::decode(block)?;
        if desc.magic != pids::NODE_IMAGE_MAGIC {
            return Err(Error::Image(format!(
                "no PMIM description block at offset {DESC_OFFSET} (magic 0x{:08X})",
                desc.magic
            )));
        }
        if u16::from(desc.format) != pids::NODE_IMAGE_FORMAT || usize::from(desc.size) != DESC_LEN {
            return Err(Error::Image(format!(
                "description block format {} size {} not supported",
                desc.format, desc.size
            )));
        }
        let len = u32::try_from(bin.len()).map_err(|_| Error::Image("image too large".into()))?;
        desc.image_length = len;
        desc.image_crc32 = crc32(&bin);
        desc.seal();
        Ok(Self {
            bin,
            desc,
            oems: vec![pids::ARTNET_OEM_NODE],
        })
    }

    /// From an `.alf` file: header (OEM list, block copy), data. Checks length and that the copy matches the
    /// binary.
    pub fn from_alf(alf: &[u8]) -> Result<Self> {
        if alf.len() < ALF_HEADER_LEN {
            return Err(Error::Image(
                ".alf shorter than its 1060-byte header".into(),
            ));
        }
        let words = u32::from_be_bytes([
            alf[ALF_LENGTH_OFFSET],
            alf[ALF_LENGTH_OFFSET + 1],
            alf[ALF_LENGTH_OFFSET + 2],
            alf[ALF_LENGTH_OFFSET + 3],
        ]) as usize;
        let data = alf
            .get(ALF_HEADER_LEN..ALF_HEADER_LEN + 2 * words)
            .ok_or_else(|| Error::Image(".alf data shorter than its length field".into()))?;
        let desc = NodeImageDesc::decode(&alf[ALF_DESC_OFFSET..ALF_DESC_OFFSET + DESC_LEN])?;
        let len = desc.image_length as usize;
        if len > data.len() || data.len() > len + 1 {
            return Err(Error::Image(format!(
                ".alf data length {} does not match image_length {len}",
                data.len()
            )));
        }
        let bin = data[..len].to_vec();
        if crc32(&bin) != desc.image_crc32 {
            return Err(Error::Image(".alf image CRC-32 mismatch".into()));
        }
        let inner = bin
            .get(DESC_OFFSET..DESC_OFFSET + DESC_LEN)
            .ok_or_else(|| Error::Image("binary too short for a description block".into()))?;
        if !desc.matches_image_block(inner) {
            return Err(Error::Image(
                ".alf description copy differs from the block in the image".into(),
            ));
        }
        let oems = (0..ALF_OEM_COUNT)
            .map(|i| {
                let o = ALF_OEM_OFFSET + 2 * i;
                u16::from_be_bytes([alf[o], alf[o + 1]])
            })
            .filter(|&o| o != 0)
            .collect();
        Ok(Self { bin, desc, oems })
    }

    /// Detect `.alf` (valid header + data) or a raw binary.
    pub fn from_file_bytes(bytes: Vec<u8>) -> Result<Self> {
        if bytes.len() > DESC_OFFSET + 4
            && bytes[DESC_OFFSET..DESC_OFFSET + 4] == pids::NODE_IMAGE_MAGIC.to_le_bytes()
        {
            return Self::from_bin(bytes);
        }
        Self::from_alf(&bytes)
    }

    /// Preview information.
    pub fn info(&self) -> NodeImageInfo {
        NodeImageInfo {
            desc: self.desc.clone(),
            oems: self.oems.clone(),
            length: self.desc.image_length,
        }
    }

    /// Build the `.alf` file (image-format.md §2.5).
    pub fn to_alf(&self) -> Vec<u8> {
        let mut data = self.bin.clone();
        if data.len() % 2 == 1 {
            data.push(0);
        }
        let mut h = vec![0u8; ALF_HEADER_LEN];
        h[0..2].copy_from_slice(&alf_checksum(&data).to_be_bytes());
        let v = self.desc.fw_version_id;
        let vers = (((v >> 16) & 0xFF) << 8 | ((v >> 8) & 0xFF)) as u16;
        h[2..4].copy_from_slice(&vers.to_be_bytes());
        let name = format!("Pixelmapper Node {}", self.desc.version_label);
        let n = name.len().min(29);
        h[4..4 + n].copy_from_slice(&name.as_bytes()[..n]);
        for (i, &oem) in self.oems.iter().take(ALF_OEM_COUNT).enumerate() {
            let o = ALF_OEM_OFFSET + 2 * i;
            h[o..o + 2].copy_from_slice(&oem.to_be_bytes());
        }
        h[ALF_DESC_OFFSET..ALF_DESC_OFFSET + DESC_LEN].copy_from_slice(&self.desc.encode());
        h[ALF_LENGTH_OFFSET..ALF_LENGTH_OFFSET + 4]
            .copy_from_slice(&((data.len() / 2) as u32).to_be_bytes());
        h.extend_from_slice(&data);
        h
    }

    /// ArtFirmwareMaster `FirmwareLength`: total `.alf` length in 16-bit words including the header.
    pub fn alf_firmware_length(&self) -> u32 {
        (self.to_alf().len() / 2) as u32
    }
}

/// `.alf` header checksum (our interpretation, digest §6.3): complement of the one's-complement sum of the
/// big-endian data words.
pub fn alf_checksum(data: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    for w in data.chunks(2) {
        let v = u16::from_be_bytes([w[0], *w.get(1).unwrap_or(&0)]);
        sum += u32::from(v);
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

/// Build a fake ESP-IDF binary with a description block, for tests and the simulator.
pub fn build_test_bin(len: usize, fw_version_id: u32, hw_rev_mask: u32, label: &str) -> Vec<u8> {
    let mut bin: Vec<u8> = (0..len.max(DESC_OFFSET + DESC_LEN))
        .map(|i| (i * 31 % 251) as u8)
        .collect();
    bin[0] = 0xE9; // ESP image magic
    let desc = NodeImageDesc::new(fw_version_id, hw_rev_mask, label);
    bin[DESC_OFFSET..DESC_OFFSET + DESC_LEN].copy_from_slice(&desc.encode());
    bin
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bin_alf_roundtrip() {
        let bin = build_test_bin(5001, 0x0001_0200, 1, "1.2.0");
        let im = NodeImage::from_bin(bin.clone()).unwrap();
        assert_eq!(im.desc.image_length, 5001);
        assert_eq!(im.desc.validate(0x7FF0, 0, 1 << 20), Ok(()));
        assert_eq!(
            im.desc.validate(0x7FF0, 1, 1 << 20),
            Err(pids::fw_error::DESC_HW_REV)
        );
        assert_eq!(
            im.desc.validate(0x1234, 0, 1 << 20),
            Err(pids::fw_error::DESC_MANUFACTURER)
        );
        assert_eq!(
            im.desc.validate(0x7FF0, 0, 100),
            Err(pids::fw_error::TOO_LARGE)
        );
        let mut tampered = im.desc.clone();
        tampered.fw_version_id += 1;
        assert_eq!(
            tampered.validate(0x7FF0, 0, 1 << 20),
            Err(pids::fw_error::DESC_MAGIC)
        );
        assert!(im.desc.matches_image_block(&bin[DESC_OFFSET..]));
        let alf = im.to_alf();
        assert_eq!(alf.len(), ALF_HEADER_LEN + 5002);
        assert_eq!(&alf[2..4], &[0x01, 0x02]);
        assert_eq!(&alf[34..36], &[0x00, 0xFF]);
        assert_eq!(im.alf_firmware_length(), 530 + 2501);
        let back = NodeImage::from_alf(&alf).unwrap();
        assert_eq!(back, im);
        assert_eq!(NodeImage::from_file_bytes(alf).unwrap(), im);
        assert_eq!(NodeImage::from_file_bytes(bin).unwrap(), im);
    }

    #[test]
    fn rejects() {
        assert!(NodeImage::from_bin(vec![0; 400]).is_err());
        assert!(NodeImage::from_alf(&[0; 100]).is_err());
        let im = NodeImage::from_bin(build_test_bin(2000, 1, 1, "x")).unwrap();
        let mut alf = im.to_alf();
        alf[ALF_HEADER_LEN + 5] ^= 1;
        assert!(NodeImage::from_alf(&alf).is_err());
    }

    #[test]
    fn checksum() {
        assert_eq!(alf_checksum(&[0x00, 0x01, 0xFF, 0xFF]), !0x0001);
    }
}
