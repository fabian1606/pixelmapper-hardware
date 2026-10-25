//! Intel HEX reader/writer (record types 00, 01, 02, 04; 03/05 ignored), as `tube/tools/flash_dmx.py`.

use crate::error::{Error, Result};
use std::collections::BTreeMap;

/// Parse Intel HEX text into `address -> byte`.
pub fn parse(text: &str) -> Result<BTreeMap<u32, u8>> {
    let mut mem = BTreeMap::new();
    let mut base: u32 = 0;
    for (n, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let err = |what: &str| Error::Image(format!("Intel HEX line {}: {what}", n + 1));
        let body = line.strip_prefix(':').ok_or_else(|| err("not Intel HEX"))?;
        let rec = crate::parse_hex(body).ok_or_else(|| err("bad hex digits"))?;
        if rec.len() < 5 {
            return Err(err("short record"));
        }
        if rec.iter().fold(0u8, |a, &b| a.wrapping_add(b)) != 0 {
            return Err(err("checksum error"));
        }
        let cnt = usize::from(rec[0]);
        let addr = u32::from(u16::from_be_bytes([rec[1], rec[2]]));
        let typ = rec[3];
        let data = rec.get(4..4 + cnt).ok_or_else(|| err("short record"))?;
        if rec.len() != cnt + 5 {
            return Err(err("record length mismatch"));
        }
        match typ {
            0 => {
                for (i, &b) in data.iter().enumerate() {
                    mem.insert(base.wrapping_add(addr).wrapping_add(i as u32), b);
                }
            }
            1 => break,
            2 | 4 if data.len() == 2 => {
                let v = u32::from(u16::from_be_bytes([data[0], data[1]]));
                base = if typ == 2 { v << 4 } else { v << 16 };
            }
            3 | 5 => {}
            _ => return Err(err("unsupported record type")),
        }
    }
    Ok(mem)
}

/// Write `address -> byte` as Intel HEX (16-byte data records, type-04 records above 64 KiB).
pub fn write(mem: &BTreeMap<u32, u8>) -> String {
    let mut out = String::new();
    let mut upper: u32 = 0;
    let rec = |out: &mut String, typ: u8, addr: u16, data: &[u8]| {
        let mut r = vec![data.len() as u8, (addr >> 8) as u8, addr as u8, typ];
        r.extend_from_slice(data);
        let cs = r.iter().fold(0u8, |a, &b| a.wrapping_add(b)).wrapping_neg();
        r.push(cs);
        out.push(':');
        out.push_str(&crate::to_hex(&r).to_uppercase());
        out.push('\n');
    };
    let mut it = mem.iter().peekable();
    while let Some((&a, &b)) = it.next() {
        if a >> 16 != upper {
            upper = a >> 16;
            rec(&mut out, 4, 0, &(upper as u16).to_be_bytes());
        }
        let mut data = vec![b];
        while data.len() < 16 {
            match it.peek() {
                Some((&n, &v)) if n == a + data.len() as u32 && (n & 0xFFFF) != 0 => {
                    data.push(v);
                    it.next();
                }
                _ => break,
            }
        }
        rec(&mut out, 0, a as u16, &data);
    }
    rec(&mut out, 1, 0, &[]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let mut mem = BTreeMap::new();
        for a in 0..40u32 {
            mem.insert(a, a as u8);
        }
        mem.insert(0x1800, 0xAA);
        mem.insert(0x1_0002, 0x55);
        let text = write(&mem);
        assert_eq!(parse(&text).unwrap(), mem);
    }

    #[test]
    fn rejects() {
        assert!(parse("garbage").is_err());
        assert!(parse(":0100000000FE").is_err()); // checksum
        assert!(parse(":10000000").is_err());
        assert_eq!(parse(":0100000055AA\n:00000001FF\n").unwrap()[&0], 0x55);
    }
}
