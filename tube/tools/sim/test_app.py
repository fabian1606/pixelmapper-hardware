"""E1.20 / DMX tests of the current tube_fw build (+ tube_boot_rdm) on the emulator.

  python3 tools/sim/test_app.py      (~1-2 min; build both firmwares first)
"""
import os
import pickle
import sys
import time

from avrsim import us
from harness import *
from rdmline import CC_DISC, CC_GET, CC_SET, Tube, dub_decode, rdm_packet

require_builds()
C = Checker()
SYM = symbols(APP_ELF)
t0 = time.time()

CACHE.mkdir(exist_ok=True)
SNAP = CACHE / "app_booted.pkl"
if SNAP.exists() and SNAP.stat().st_mtime > max(os.path.getmtime(APP_HEX), os.path.getmtime(BOOT_HEX)):
    tube = pickle.load(open(SNAP, "rb"))
else:
    tube = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
    # E1.20 2.6: DE must go to receive early in the app (before the 2 s self-test)
    tube.run_ms(260)
    log = [e for e in tube.cpu.portd_log if e[0] > tube.cpu.resets[-1][0]]
    de_out = [e for e in log if e[2] & 0x04]
    C.check("app sets DE pin (receive) right after start", de_out and de_out[0][0] - tube.cpu.resets[-1][0] < us(1000)
            and not de_out[0][1] & 0x04, f"{de_out[:1]}")
    tube.run_ms(2100)
    pickle.dump(tube, open(SNAP, "wb"))
base = snapshot(tube)
print(f"booted ({time.time() - t0:.1f} s)")


def ram(t, name, n=None):
    a, size = SYM[name]
    return bytes(t.cpu.d[a:a + (n or size)])


def get(t, pid, pd=b"", dest=UID, **kw):
    return t.rdm(dest, CC_GET, pid, pd, **kw)


def set_(t, pid, pd=b"", dest=UID, **kw):
    return t.rdm(dest, CC_SET, pid, pd, **kw)


# ------------------------------------------------------------------ discovery
t = snapshot(base)
C.silent("UN_MUTE broadcast", t.transact(rdm_packet(BCAST, CC_DISC, 0x0003)))
a = dub(t, bytes(6), BCAST)
C.check("DUB full range -> UID", dub_decode(a.raw) == UID, a.raw.hex())
C.timing_ok("DUB full range", a, dub=True)
C.check("DUB preamble 7xFE + AA", a.raw[:8] == b"\xFE" * 7 + b"\xAA", a.raw.hex())
C.check("DUB own vendorcast", dub_decode(dub(t, bytes(6), BCAST, VCAST).raw) == UID)
C.check("DUB unicast", dub_decode(dub(t, bytes(6), BCAST, UID).raw) == UID)
C.check("DUB lower=upper=UID", dub_decode(dub(t, UID, UID).raw) == UID)
C.silent("DUB other manufacturer vendorcast", dub(t, bytes(6), BCAST, bytes.fromhex("7FEFFFFFFFFF")))
C.silent("DUB other manufacturer range", dub(t, bytes.fromhex("7FEF00000000"), bytes.fromhex("7FEFFFFFFFFF")))
C.silent("DUB signed (8000.. - FFFF..)", dub(t, bytes.fromhex("800000000000"), BCAST))
C.silent("DUB inverted full tree", dub(t, BCAST, bytes(6)))
nxt = (int.from_bytes(UID, "big") + 1).to_bytes(6, "big")
prv = (int.from_bytes(UID, "big") - 1).to_bytes(6, "big")
C.silent("DUB inverted range", dub(t, nxt, prv))
C.silent("DUB UID+1..max", dub(t, nxt, BCAST))
C.check("DUB 0..UID", dub_decode(dub(t, bytes(6), UID).raw) == UID)
C.silent("DUB with PDL 11", t.transact(rdm_packet(BCAST, CC_DISC, 0x0001, bytes(11))))
a = t.rdm(UID, CC_DISC, 0x0002)
p = C.resp("DISC_MUTE", a, CC_DISC, 0x0002, pd=b"\x00\x00")
C.silent("DUB after mute", dub(t, bytes(6), BCAST))
C.silent("DISC_MUTE with data", t.rdm(UID, CC_DISC, 0x0002, b"x"))
C.silent("DISC_UN_MUTE with data", t.rdm(UID, CC_DISC, 0x0003, b"x"))
C.silent("DISC unknown PID 0x000F", t.rdm(UID, CC_DISC, 0x000F))
a = t.rdm(UID, CC_DISC, 0x0003)
C.resp("DISC_UN_MUTE", a, CC_DISC, 0x0003, pd=b"\x00\x00")
C.check("DUB after unmute", dub_decode(dub(t, bytes(6), BCAST).raw) == UID)
C.silent("DISC_MUTE broadcast", t.transact(rdm_packet(BCAST, CC_DISC, 0x0002)))
C.silent("DUB after broadcast mute", dub(t, bytes(6), BCAST))
C.silent("DISC_UN_MUTE vendorcast", t.transact(rdm_packet(VCAST, CC_DISC, 0x0003)))
C.check("DUB after vendorcast unmute", dub_decode(dub(t, bytes(6), BCAST).raw) == UID)

# ------------------------------------------------------------------ device info & labels
t = snapshot(base)
p = C.resp("GET DEVICE_INFO", get(t, 0x0060), CC_GET, 0x0060,
           pd=bytes.fromhex("0100" "0001" "0101") + FW_VERSION.to_bytes(4, "big") + bytes.fromhex("000C" "0104" "0001" "0000" "00"))
p = C.resp("GET SUPPORTED_PARAMETERS", get(t, 0x0050), CC_GET, 0x0050)
if p:
    pids = [int.from_bytes(p.pd[i:i + 2], "big") for i in range(0, len(p.pd), 2)]
    C.check("SUPPORTED_PARAMETERS list", pids == [0x0080, 0x0081, 0x0082, 0x00E0, 0x00E1, 0x0120, 0x0121, 0x0122, 0x8000],
            [hex(x) for x in pids])
    C.check("SUPPORTED_PARAMETERS has no required PID (10.4.1)",
            not set(pids) & {0x0001, 0x0002, 0x0003, 0x0050, 0x0051, 0x0060, 0x00C0, 0x00F0, 0x1000})
C.resp("GET SUPPORTED_PARAMETERS + data", get(t, 0x0050, b"foo"), CC_GET, 0x0050, nack="FORMAT_ERROR")
C.resp("SET SUPPORTED_PARAMETERS", set_(t, 0x0050), CC_SET, 0x0050, nack="UNSUPPORTED_CC")
C.resp("GET DEVICE_INFO + data", get(t, 0x0060, b"x"), CC_GET, 0x0060, nack="FORMAT_ERROR")
C.resp("GET DEVICE_INFO + 231 B (max PDL)", get(t, 0x0060, bytes(range(231))), CC_GET, 0x0060, nack="FORMAT_ERROR")
C.resp("SET DEVICE_INFO", set_(t, 0x0060), CC_SET, 0x0060, nack="UNSUPPORTED_CC")
C.resp("SET DEVICE_INFO + data", set_(t, 0x0060, b"foo"), CC_SET, 0x0060, nack="UNSUPPORTED_CC")
p = C.resp("PARAMETER_DESCRIPTION 0x8000", get(t, 0x0051, b"\x80\x00"), CC_GET, 0x0051)
if p:
    C.check("PARAMETER_DESCRIPTION fields", p.pd[:20] == bytes.fromhex("8000" "04" "02" "02" "00" "00" "00") + bytes(12)
            and p.pd[20:] == b'Enter bootloader (SET "BOOT")', p.pd.hex())
C.resp("PARAMETER_DESCRIPTION 0x0060", get(t, 0x0051, b"\x00\x60"), CC_GET, 0x0051, nack="DATA_OUT_OF_RANGE")
C.resp("PARAMETER_DESCRIPTION no data", get(t, 0x0051), CC_GET, 0x0051, nack="FORMAT_ERROR")
C.resp("PARAMETER_DESCRIPTION + data", get(t, 0x0051, b"\x80\x00\x00"), CC_GET, 0x0051, nack="FORMAT_ERROR")
C.resp("SET PARAMETER_DESCRIPTION", set_(t, 0x0051, b"\x80\x00"), CC_SET, 0x0051, nack="UNSUPPORTED_CC")
C.resp("SOFTWARE_VERSION_LABEL", get(t, 0x00C0), CC_GET, 0x00C0, pd=FW_LABEL)
C.resp("SOFTWARE_VERSION_LABEL + data", get(t, 0x00C0, b"foo"), CC_GET, 0x00C0, nack="FORMAT_ERROR")
C.resp("SET SOFTWARE_VERSION_LABEL", set_(t, 0x00C0, b"foo"), CC_SET, 0x00C0, nack="UNSUPPORTED_CC")
C.resp("DEVICE_MODEL_DESCRIPTION", get(t, 0x0080), CC_GET, 0x0080, pd=b"XBAR500 LED tube (custom fw)")
C.resp("MANUFACTURER_LABEL", get(t, 0x0081), CC_GET, 0x0081, pd=b"Prototype")
C.resp("DEVICE_LABEL empty", get(t, 0x0082), CC_GET, 0x0082, pd=b"")
for name, lab, exp in (("test label", b"test label", b"test label"),
                       ("32 chars", b"this is a string with 32 charact", b"this is a string with 32 charact"),
                       ("non-printable", b"str w\x0d non\x1bprint ASCII\x7f", b"str w\x0d non\x1bprint ASCII\x7f"),
                       ("NUL terminates", b"ab\x00cd", b"ab"),
                       ("empty", b"", b"")):
    C.resp(f"SET DEVICE_LABEL {name}", set_(t, 0x0082, lab), CC_SET, 0x0082, pdl=0)
    C.resp(f"GET DEVICE_LABEL {name}", get(t, 0x0082), CC_GET, 0x0082, pd=exp)
set_(t, 0x0082, b"keep")
C.resp("SET DEVICE_LABEL 49 B (oversized)", set_(t, 0x0082, b"this is a string which is more than 32 characters"),
       CC_SET, 0x0082, nack="FORMAT_ERROR")
C.resp("label unchanged after oversized", get(t, 0x0082), CC_GET, 0x0082, pd=b"keep")
C.resp("GET DEVICE_LABEL + data", get(t, 0x0082, b"foo"), CC_GET, 0x0082, nack="FORMAT_ERROR")

# ------------------------------------------------------------------ DMX setup
t = snapshot(base)
C.resp("GET DMX_START_ADDRESS", get(t, 0x00F0), CC_GET, 0x00F0, pd=b"\x00\x01")
C.resp("GET DMX_START_ADDRESS + data", get(t, 0x00F0, b"foo"), CC_GET, 0x00F0, nack="FORMAT_ERROR")
C.resp("SET DMX_START_ADDRESS 0", set_(t, 0x00F0, b"\x00\x00"), CC_SET, 0x00F0, nack="DATA_OUT_OF_RANGE")
C.resp("SET DMX_START_ADDRESS 513", set_(t, 0x00F0, b"\x02\x01"), CC_SET, 0x00F0, nack="DATA_OUT_OF_RANGE")
C.resp("SET DMX_START_ADDRESS no data", set_(t, 0x00F0), CC_SET, 0x00F0, nack="FORMAT_ERROR")
C.resp("SET DMX_START_ADDRESS + data", set_(t, 0x00F0, b"foo"), CC_SET, 0x00F0, nack="FORMAT_ERROR")
C.resp("SET DMX_START_ADDRESS 501 (12 ch fits)", set_(t, 0x00F0, (501).to_bytes(2, "big")), CC_SET, 0x00F0, pdl=0)
C.resp("SET DMX_START_ADDRESS 502 (would end at 513)", set_(t, 0x00F0, (502).to_bytes(2, "big")), CC_SET, 0x00F0,
       nack="DATA_OUT_OF_RANGE")
p = C.resp("DEVICE_INFO start 501", get(t, 0x0060), CC_GET, 0x0060)
C.check("DEVICE_INFO start address = 501", p and p.pd[14:16] == (501).to_bytes(2, "big"), p and p.pd.hex())
C.resp("SET personality 4 at 501 (13 ch -> 513)", set_(t, 0x00E0, b"\x04"), CC_SET, 0x00E0, nack="DATA_OUT_OF_RANGE")
set_(t, 0x00F0, b"\x00\x01")
C.resp("GET DMX_PERSONALITY", get(t, 0x00E0), CC_GET, 0x00E0, pd=b"\x01\x04")
C.resp("GET DMX_PERSONALITY + data", get(t, 0x00E0, b"foo"), CC_GET, 0x00E0, nack="FORMAT_ERROR")
fps = {1: 12, 2: 3, 3: 4, 4: 13}
descs = {1: b"4x RGB", 2: b"RGB", 3: b"RGB + Dimmer", 4: b"4x RGB + Dimmer"}
for n in (1, 2, 3, 4):
    C.resp(f"SET personality {n}", set_(t, 0x00E0, bytes([n])), CC_SET, 0x00E0, pdl=0)
    C.resp(f"GET personality {n}", get(t, 0x00E0), CC_GET, 0x00E0, pd=bytes([n, 4]))
    p = C.resp(f"DEVICE_INFO personality {n}", get(t, 0x0060), CC_GET, 0x0060)
    C.check(f"DEVICE_INFO footprint/personality {n}", p and p.pd[10:14] == bytes([0, fps[n], n, 4]), p and p.pd.hex())
    C.resp(f"PERSONALITY_DESCRIPTION {n}", get(t, 0x00E1, bytes([n])), CC_GET, 0x00E1,
           pd=bytes([n, 0, fps[n]]) + descs[n])
    p = C.resp(f"SLOT_INFO p{n}", get(t, 0x0120), CC_GET, 0x0120, pdl=5 * fps[n])
    if p:
        ok = True
        for s in range(fps[n]):
            e = p.pd[5 * s:5 * s + 5]
            dim = n in (3, 4) and s == fps[n] - 1
            lab = 0x0001 if dim else (0x0205, 0x0206, 0x0207)[s % 3]
            ok &= e == s.to_bytes(2, "big") + b"\x00" + lab.to_bytes(2, "big")
        C.check(f"SLOT_INFO p{n} content", ok, p.pd.hex())
    p = C.resp(f"DEFAULT_SLOT_VALUE p{n}", get(t, 0x0122), CC_GET, 0x0122, pdl=3 * fps[n])
    if p:
        exp = b"".join(s.to_bytes(2, "big") + bytes([255 if n in (3, 4) and s == fps[n] - 1 else 0])
                       for s in range(fps[n]))
        C.check(f"DEFAULT_SLOT_VALUE p{n} content", p.pd == exp, p.pd.hex())
    for s in range(fps[n]):
        p = C.resp(f"SLOT_DESCRIPTION p{n} s{s}", get(t, 0x0121, s.to_bytes(2, "big")), CC_GET, 0x0121)
    C.check(f"SLOT_DESCRIPTION p{n} last", p and p.pd[2:] == (b"Dimmer" if n in (3, 4) else
                                                              (b"Seg 4 Blue" if n == 1 else b"Blue")), p and p.pd)
    C.resp(f"SLOT_DESCRIPTION p{n} out of range", get(t, 0x0121, fps[n].to_bytes(2, "big")), CC_GET, 0x0121,
           nack="DATA_OUT_OF_RANGE")
C.resp("SET personality 0", set_(t, 0x00E0, b"\x00"), CC_SET, 0x00E0, nack="DATA_OUT_OF_RANGE")
C.resp("SET personality 5", set_(t, 0x00E0, b"\x05"), CC_SET, 0x00E0, nack="DATA_OUT_OF_RANGE")
C.resp("SET personality no data", set_(t, 0x00E0), CC_SET, 0x00E0, nack="FORMAT_ERROR")
C.resp("SET personality + data", set_(t, 0x00E0, b"foo"), CC_SET, 0x00E0, nack="FORMAT_ERROR")
C.resp("PERSONALITY_DESCRIPTION 0", get(t, 0x00E1, b"\x00"), CC_GET, 0x00E1, nack="DATA_OUT_OF_RANGE")
C.resp("PERSONALITY_DESCRIPTION 5", get(t, 0x00E1, b"\x05"), CC_GET, 0x00E1, nack="DATA_OUT_OF_RANGE")
C.resp("PERSONALITY_DESCRIPTION no data", get(t, 0x00E1), CC_GET, 0x00E1, nack="FORMAT_ERROR")
C.resp("SLOT_DESCRIPTION no data", get(t, 0x0121), CC_GET, 0x0121, nack="FORMAT_ERROR")
C.resp("SLOT_INFO + data", get(t, 0x0120, b"foo"), CC_GET, 0x0120, nack="FORMAT_ERROR")
C.resp("SET SLOT_INFO", set_(t, 0x0120), CC_SET, 0x0120, nack="UNSUPPORTED_CC")

# ------------------------------------------------------------------ identify, sub-devices, unknown, broadcast
t = snapshot(base)
C.resp("GET IDENTIFY", get(t, 0x1000), CC_GET, 0x1000, pd=b"\x00")
C.resp("GET IDENTIFY + data", get(t, 0x1000, b"foo"), CC_GET, 0x1000, nack="FORMAT_ERROR")
C.resp("SET IDENTIFY 1", set_(t, 0x1000, b"\x01"), CC_SET, 0x1000, pdl=0)
C.resp("GET IDENTIFY = 1", get(t, 0x1000), CC_GET, 0x1000, pd=b"\x01")
seen = set()
for _ in range(30):
    t.run_ms(40)
    seen.add(ram(t, "lv"))
C.check("identify blinks all channels 0/255", seen == {bytes(12), b"\xFF" * 12}, seen)
C.resp("SET IDENTIFY 2", set_(t, 0x1000, b"\x02"), CC_SET, 0x1000, nack="DATA_OUT_OF_RANGE")
C.resp("SET IDENTIFY no data", set_(t, 0x1000), CC_SET, 0x1000, nack="FORMAT_ERROR")
C.resp("SET IDENTIFY + data", set_(t, 0x1000, b"foo"), CC_SET, 0x1000, nack="FORMAT_ERROR")
C.silent("SET IDENTIFY 0 broadcast", set_(t, 0x1000, b"\x00", dest=BCAST))
C.resp("identify off after broadcast", get(t, 0x1000), CC_GET, 0x1000, pd=b"\x00")
set_(t, 0x1000, b"\x01")
C.silent("SET IDENTIFY 0 other vendorcast", set_(t, 0x1000, b"\x00", dest=bytes.fromhex("7FEFFFFFFFFF")))
C.resp("identify still on (other vendor)", get(t, 0x1000), CC_GET, 0x1000, pd=b"\x01")
C.silent("SET IDENTIFY 0 vendorcast", set_(t, 0x1000, b"\x00", dest=VCAST))
C.resp("identify off after vendorcast", get(t, 0x1000), CC_GET, 0x1000, pd=b"\x00")
C.resp("GET DEVICE_INFO sub-device 1", get(t, 0x0060, subdev=1), CC_GET, 0x0060, subdev=1,
       nack="SUB_DEVICE_OUT_OF_RANGE")
C.resp("GET IDENTIFY all sub-devices", get(t, 0x1000, subdev=0xFFFF), CC_GET, 0x1000, subdev=0xFFFF,
       nack="SUB_DEVICE_OUT_OF_RANGE")
C.resp("SET IDENTIFY all sub-devices", set_(t, 0x1000, b"\x01", subdev=0xFFFF), CC_SET, 0x1000, subdev=0xFFFF, pdl=0)
C.resp("SET IDENTIFY sub-device 3", set_(t, 0x1000, b"\x00", subdev=3), CC_SET, 0x1000, subdev=3,
       nack="SUB_DEVICE_OUT_OF_RANGE")
C.resp("GET unknown PID DEVICE_HOURS", get(t, 0x0400), CC_GET, 0x0400, nack="UNKNOWN_PID")
C.resp("SET unknown PID RESET_DEVICE", set_(t, 0x1001, b"\x01"), CC_SET, 0x1001, nack="UNKNOWN_PID")
C.resp("GET unknown manufacturer PID 0x8001", get(t, 0x8001), CC_GET, 0x8001, nack="UNKNOWN_PID")
C.resp("GET ENTER_BOOTLOADER", get(t, 0x8000), CC_GET, 0x8000, nack="UNSUPPORTED_CC")
C.resp("SET ENTER_BOOTLOADER wrong data", set_(t, 0x8000, b"BOOX"), CC_SET, 0x8000, nack="DATA_OUT_OF_RANGE")
C.resp("SET ENTER_BOOTLOADER no data", set_(t, 0x8000), CC_SET, 0x8000, nack="FORMAT_ERROR")
C.silent("GET DEVICE_INFO broadcast", get(t, 0x0060, dest=BCAST))
C.silent("GET DEVICE_INFO vendorcast", get(t, 0x0060, dest=VCAST))
C.silent("GET other UID", get(t, 0x0060, dest=bytes.fromhex("7FF0B06675AB")))
C.silent("SET DMX_START_ADDRESS broadcast", set_(t, 0x00F0, b"\x00\x0A", dest=BCAST))
C.resp("start address set by broadcast", get(t, 0x00F0), CC_GET, 0x00F0, pd=b"\x00\x0A")
C.silent("SET DMX_START_ADDRESS vendorcast", set_(t, 0x00F0, b"\x00\x14", dest=VCAST))
C.resp("start address set by vendorcast", get(t, 0x00F0), CC_GET, 0x00F0, pd=b"\x00\x14")
C.silent("SET DEVICE_LABEL broadcast", set_(t, 0x0082, b"bc", dest=BCAST))
C.resp("label set by broadcast", get(t, 0x0082), CC_GET, 0x0082, pd=b"bc")
r0 = t.cpu.reset_count
C.silent("SET ENTER_BOOTLOADER broadcast", set_(t, 0x8000, b"BOOT", dest=BCAST))
C.silent("SET ENTER_BOOTLOADER vendorcast", set_(t, 0x8000, b"BOOT", dest=VCAST))
t.run_ms(50)
C.check("no reset after broadcast/vendorcast ENTER_BOOTLOADER", t.cpu.reset_count == r0)
# malformed packets
good = rdm_packet(UID, CC_GET, 0x0060, tn=1)
bad = bytearray(good)
bad[-1] ^= 1
C.silent("bad checksum", t.transact(bytes(bad)))
C.silent("sub-start code 0x02", t.transact(rdm_packet(UID, CC_GET, 0x0060, sub_sc=0x02)))
C.silent("message length != 24 + PDL", t.transact(rdm_packet(UID, CC_GET, 0x0060, b"ab", length=27)))
C.silent("message length 23", t.transact(rdm_packet(UID, CC_GET, 0x0060, length=23)[:-3] + b"\x00\x00"))
C.resp("still answering after malformed packets", get(t, 0x0060), CC_GET, 0x0060)

# ------------------------------------------------------------------ DMX
t = snapshot(base)
dmx_lv = lambda: ram(t, "lv")


def frame(slots, **kw):
    t.send_dmx(slots, **kw)
    t.run_ms(25)


frame([10 * i + 1 for i in range(12)] + [0] * 20)
C.check("p1 4xRGB mapping", dmx_lv() == bytes(10 * i + 1 for i in range(12)), dmx_lv().hex())
set_(t, 0x00E0, b"\x02")
frame([200, 100, 50] + [0] * 20)
C.check("p2 RGB whole tube", dmx_lv() == bytes([200, 100, 50] * 4), dmx_lv().hex())
set_(t, 0x00E0, b"\x03")
frame([200, 100, 50, 128])
C.check("p3 RGB + dimmer", dmx_lv() == bytes([100, 50, 25] * 4), dmx_lv().hex())
set_(t, 0x00E0, b"\x04")
frame([255] * 12 + [51])
C.check("p4 4xRGB + dimmer", dmx_lv() == bytes([51] * 12), dmx_lv().hex())
set_(t, 0x00E0, b"\x01")
set_(t, 0x00F0, (100).to_bytes(2, "big"))
frame([0] * 99 + list(range(1, 13)) + [99] * 10)
C.check("start address 100", dmx_lv() == bytes(range(1, 13)), dmx_lv().hex())
before = dmx_lv()
frame([0] * 99 + [7] * 11)                      # ends one slot early -> no update
C.check("short frame ignored", dmx_lv() == before, dmx_lv().hex())
t.send_dmx([0] * 99 + [9] * 12, start_code=0x17)
t.run_ms(25)
C.check("alternate start code ignored", dmx_lv() == before, dmx_lv().hex())
frame([0] * 99 + [5] * 12, brk_us=88, mab_us=8, isd_us=100)   # E1.20 Table 3-3 receive limits
C.check("DMX with 88us break / 8us MAB / gaps", dmx_lv() == bytes([5] * 12), dmx_lv().hex())
t.run_ms(2500)
C.check("DMX loss: values held", dmx_lv() == bytes([5] * 12), dmx_lv().hex())
set_(t, 0x00F0, (501).to_bytes(2, "big"))
frame([0] * 500 + [3] * 12)
C.check("start address 501, frame of 512 slots", dmx_lv() == bytes([3] * 12), dmx_lv().hex())
# RDM between DMX frames
for i in range(5):
    t.send_dmx([0] * 500 + [i] * 12)
    a = get(t, 0x0060)
    C.check(f"RDM between DMX frames {i}", a.pkt is not None and a.pkt.rt == 0)
t.run_ms(25)
C.check("DMX after interleaved RDM", dmx_lv() == bytes([4] * 12), dmx_lv().hex())

# ------------------------------------------------------------------ EEPROM persistence + bootloader entry
t = snapshot(base)
set_(t, 0x00F0, (100).to_bytes(2, "big"))
set_(t, 0x00E0, b"\x03")
set_(t, 0x0082, b"Tube 07 / Buehne links 12345678"[:32])
a = set_(t, 0x8000, b"BOOT")                      # immediately: EEPROM writes still pending
C.resp("ENTER_BOOTLOADER ACK", a, CC_SET, 0x8000, pdl=0)
t.run_ms(400)                                     # app flushes ~35 EEPROM bytes (~300 ms) first
a = t.rdm(UID, CC_GET, 0x00C0)
C.check("bootloader active after ENTER_BOOTLOADER", a.pkt and a.pkt.pd == BOOT_LABEL, a.pkt and a.pkt.pd)
a = t.rdm(UID, CC_SET, 0x8013)                    # FINISH -> app
C.check("FINISH ACK", a.pkt and a.pkt.rt == 0)
t.run_ms(2500)
p = C.resp("DEVICE_INFO after bootloader round trip", get(t, 0x0060), CC_GET, 0x0060)
C.check("settings survived bootloader entry (flush)", p and p.pd[10:16] == bytes([0, 4, 3, 4, 0, 100]), p and p.pd.hex())
C.resp("label survived", get(t, 0x0082), CC_GET, 0x0082, pd=b"Tube 07 / Buehne links 12345678"[:32])
t.cpu.power_on()
t.run_ms(2400)
p = C.resp("DEVICE_INFO after power cycle", get(t, 0x0060), CC_GET, 0x0060)
C.check("settings survived power cycle", p and p.pd[10:16] == bytes([0, 4, 3, 4, 0, 100]), p and p.pd.hex())
C.check("EEPROM layout v2", bytes(t.cpu.eeprom[0x10:0x12]) == b"\x5A\x02", bytes(t.cpu.eeprom[0x10:0x18]).hex())

print(f"\napp tests: {time.time() - t0:.0f} s")
sys.exit(0 if C.summary() else 1)
