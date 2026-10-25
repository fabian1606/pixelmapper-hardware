//! Deterministic random/mutation test of the decoders that see network input: no decoder may panic on any byte
//! sequence, and whatever decodes must encode and decode again to the same value.

use pixelmapper_net::artnet::{ArtPacket, ArtTodData};
use pixelmapper_net::rdm::pd::{
    self, BootCatchStatus, BootInfo, DefaultSlotValue, DeviceInfo, DiscMuteResponse,
    DiscUniqueBranch, DmxPersonality, FwInfo, FwWrite, InterfaceEntry, Ipv4CurrentAddress,
    LocalRdmSet, ParameterDescription, PersonalityDescription, SensorDefinition, SensorValue,
    SlotDescription, SlotInfo, StatusMessage,
};
use pixelmapper_net::rdm::{DubResponse, RdmMessage};

/// xorshift64: reproducible without extra dependencies.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        (self.0 >> 16) as u32
    }
    fn below(&mut self, n: usize) -> usize {
        if n == 0 {
            0
        } else {
            self.next() as usize % n
        }
    }
    fn bytes(&mut self, n: usize) -> Vec<u8> {
        (0..n).map(|_| self.next() as u8).collect()
    }
}

const OPCODES: [u16; 17] = [
    0x2000, 0x2100, 0x2300, 0x5000, 0x5200, 0x6000, 0x7000, 0x8000, 0x8100, 0x8200, 0x8300, 0x8400,
    0xF200, 0xF300, 0xF800, 0xF900, 0x9900,
];

fn artnet_candidate(rng: &mut Rng) -> Vec<u8> {
    let len = if rng.below(10) < 8 {
        rng.below(300)
    } else {
        rng.below(1500)
    };
    let mut b = rng.bytes(len);
    if len >= 10 && rng.below(10) < 9 {
        b[..8].copy_from_slice(b"Art-Net\0");
        let op = OPCODES[rng.below(OPCODES.len())];
        b[8..10].copy_from_slice(&op.to_le_bytes());
        if len >= 12 && rng.below(2) == 0 {
            b[10..12].copy_from_slice(&14u16.to_be_bytes());
        }
        // plausible length / count fields
        if op == 0x5000 && len >= 18 {
            let l = rng.below(600) as u16;
            b[16..18].copy_from_slice(&l.to_be_bytes());
        }
        if (op == 0x8000 || op == 0x8100) && len >= 28 {
            b[23] = rng.below(40) as u8;
            b[27] = rng.below(250) as u8;
        }
        if op == 0x8300 && len >= 26 {
            b[25] = rng.below(260) as u8; // RDM Message Length
        }
    }
    b
}

fn rdm_candidate(rng: &mut Rng) -> Vec<u8> {
    let pdl = match rng.below(4) {
        0 => 0,
        1 => rng.below(16),
        2 => rng.below(232),
        _ => rng.below(300),
    };
    let mut b = rng.bytes(24 + pdl + 2);
    b[0] = 0xCC;
    b[1] = 0x01;
    b[2] = (24 + pdl.min(255 - 24)) as u8;
    b[23] = pdl.min(255) as u8;
    if rng.below(10) < 8 && pdl <= 231 {
        let ml = 24 + pdl;
        let cs: u16 = b[..ml]
            .iter()
            .fold(0u16, |a, &x| a.wrapping_add(u16::from(x)));
        b[ml..ml + 2].copy_from_slice(&cs.to_be_bytes());
    }
    let keep = if rng.below(10) == 0 {
        rng.below(b.len() + 1)
    } else {
        b.len()
    };
    b.truncate(keep);
    b
}

#[test]
fn artnet_decoders_never_panic_and_roundtrip() {
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15);
    let mut decoded = 0usize;
    for _ in 0..200_000 {
        let b = artnet_candidate(&mut rng);
        if let Ok(p) = ArtPacket::decode(&b) {
            decoded += 1;
            if matches!(p, ArtPacket::Other { .. }) {
                continue;
            }
            let again = ArtPacket::decode(&p.encode()).expect("re-encoded packet decodes");
            // text fields are decoded lossily (non-ASCII -> U+FFFD), so only text-free packets roundtrip exactly
            let has_text = matches!(
                p,
                ArtPacket::PollReply(_) | ArtPacket::Address(_) | ArtPacket::DiagData(_)
            );
            // the decoder is lenient with UidCount 201..255, the encoder sends at most 200 UIDs per packet
            let long_tod =
                matches!(&p, ArtPacket::TodData(t) if t.uids.len() > ArtTodData::MAX_UIDS);
            if !has_text && !long_tod {
                assert_eq!(again, p, "roundtrip of {b:02x?}");
            }
        }
    }
    assert!(
        decoded > 10_000,
        "generator reaches the decoders ({decoded})"
    );
}

#[test]
fn rdm_decoders_never_panic_and_roundtrip() {
    let mut rng = Rng(0x0123_4567_89AB_CDEF);
    let mut decoded = 0usize;
    for _ in 0..200_000 {
        let b = rdm_candidate(&mut rng);
        if let Ok(m) = RdmMessage::decode(&b) {
            decoded += 1;
            assert_eq!(RdmMessage::decode(&m.encode()).expect("re-encoded"), m);
            let pd = &m.parameter_data;
            // every PD decoder on every PD (the PID does not matter for robustness)
            let _ = pd::decode_label(pd);
            let _ = pd::decode_pid_list(pd);
            let _ = pd::decode_u8(pd);
            let _ = pd::decode_u16(pd);
            let _ = pd::decode_u32(pd);
            let _ = DiscUniqueBranch::decode(pd);
            let _ = DiscMuteResponse::decode(pd);
            let _ = StatusMessage::decode(pd);
            let _ = StatusMessage::decode_list(pd);
            let _ = DeviceInfo::decode(pd);
            let _ = ParameterDescription::decode(pd);
            let _ = DmxPersonality::decode(pd);
            let _ = PersonalityDescription::decode(pd);
            let _ = SlotInfo::decode(pd);
            let _ = SlotInfo::decode_list(pd);
            let _ = SlotDescription::decode(pd);
            let _ = DefaultSlotValue::decode(pd);
            let _ = DefaultSlotValue::decode_list(pd);
            let _ = SensorDefinition::decode(pd);
            let _ = SensorValue::decode(pd);
            let _ = Ipv4CurrentAddress::decode(pd);
            let _ = InterfaceEntry::decode(pd);
            let _ = InterfaceEntry::decode_list(pd);
            let _ = BootInfo::decode(pd);
            let _ = BootCatchStatus::decode(pd);
            let _ = LocalRdmSet::decode(pd);
            let _ = FwInfo::decode(pd);
            let _ = FwWrite::decode(pd);
        }
        let _ = DubResponse::decode(&b);
        let n = rng.below(40);
        let raw = rng.bytes(n);
        let _ = DubResponse::decode(&raw);
    }
    assert!(
        decoded > 10_000,
        "generator reaches the decoders ({decoded})"
    );
}
