"""E1.20 + update-path tests of the current tube_boot_rdm build (with tube_fw) on the emulator.

  python3 tools/sim/test_boot.py     (~6-8 min; builds two test variants of tube_fw first)
"""
import contextlib
import io
import sys
import time

from avrsim import read_ihex, us
from harness import *
from rdmline import CC_DISC, CC_GET, CC_SET, Tube, dub_decode, rdm_packet

require_builds()
C = Checker()
fr = load_flash_rdm()
APP2_HEX = newer_app()
HANG_HEX = hanging_app()
BV = BOOTH["BOOT_RDM_SW_VERSION_ID"]
t0 = time.time()


def crc16(data, c=0xFFFF):
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def label(t, uid=UID):
    a = t.rdm(uid, CC_GET, 0x00C0)
    return a.pkt.pd if a.pkt else None


def leds_on(t):
    c = t.cpu
    return (c.portb & 0x03 == 0x03 and c.portc & 0x3F == 0x3F and c.portd & 0xF0 == 0xF0
            and c.ddrb & 0x03 == 0x03 and c.ddrc & 0x3F == 0x3F and c.ddrd & 0xF0 == 0xF0)


def leds_off(t):
    c = t.cpu
    return c.portb & 0x03 == 0 and c.portc & 0x3F == 0 and c.portd & 0xF0 == 0


def boot_info(t):
    a = t.rdm(UID, CC_GET, 0x8010)
    return a, (a.pkt.pd if a.pkt and a.pkt.rt == 0 else None)


def quiet(fn, *a, **kw):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            return fn(*a, **kw), buf.getvalue()
        except fr.FlashError as e:
            return e, buf.getvalue()


def run_flash(t, hexfile, **kw):
    tr = EmuTransport(t, fr)
    rdm = fr.Rdm(tr, sleep=lambda s: t.run_us(s * 1e6))
    res, out = quiet(fr.flash, rdm, UID, hexfile, sleep=lambda s: t.run_us(s * 1e6), **kw)
    return res, out, tr


# ------------------------------------------------------------------ normal power-on
t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
t.run_ms(100)
C.check("bootloader answers in the window", label(t) == BOOT_LABEL)
t.run_ms(160)
C.check("app started after the window (WDT reset)", len(t.cpu.resets) == 2 and t.cpu.resets[1][1] == 0 and
        t.cpu.pc * 2 < 0x1800, t.cpu.resets)
start = t.cpu.resets[1][0] / 16000
C.check("app start ~225-245 ms after power-on", 220 <= start <= 250, f"{start:.1f} ms")
print(f"power-on -> app reset at {start:.1f} ms")

# ------------------------------------------------------------------ rescue window
t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
t.run_ms(30)
a, bi = boot_info(t)
C.resp("BOOT_INFO in window", a, CC_GET, 0x8010, pdl=19)
C.check("BOOT_INFO content", bi == bytes([BOOTH["BOOT_RDM_VERSION"], 64, 0x18, 0, 0, 0, 0x7F, 0xF0, 0, 1]) + BV.to_bytes(4, "big")
        + b"\x01" + FW_VERSION.to_bytes(4, "big"),
        bi and bi.hex())
t.run_ms(500)
C.check("stays after rescue BOOT_INFO", label(t) == BOOT_LABEL and len(t.cpu.resets) == 1)
# earliest / latest window
for when in (1, 5, 20, 26, 200, 220, 230):
    tt = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
    tt.run_ms(when)
    a, bi = boot_info(tt)
    print(f"  rescue GET BOOT_INFO at {when:3d} ms: {'answer' if bi else 'no answer'}")
    if when in (26, 200):
        C.check(f"rescue at {when} ms accepted", bi is not None)
boot = snapshot(t)                                # in the bootloader, valid app installed

# ------------------------------------------------------------------ RDM conformance in the bootloader
t = snapshot(boot)
C.silent("UN_MUTE broadcast", t.transact(rdm_packet(BCAST, CC_DISC, 0x0003)))
a = dub(t, bytes(6), BCAST)
C.check("DUB -> UID", dub_decode(a.raw) == UID, a.raw.hex())
C.timing_ok("bootloader DUB", a, dub=True)
C.check("DUB vendorcast", dub_decode(dub(t, bytes(6), BCAST, VCAST).raw) == UID)
C.silent("DUB other range", dub(t, bytes.fromhex("7FEF00000000"), bytes.fromhex("7FEFFFFFFFFF")))
C.silent("DUB inverted", dub(t, BCAST, bytes(6)))
C.resp("DISC_MUTE (valid app: control field 0)", t.rdm(UID, CC_DISC, 0x0002), CC_DISC, 0x0002, pd=b"\x00\x00")
C.silent("DUB when muted", dub(t, bytes(6), BCAST))
C.silent("DISC_MUTE with data", t.rdm(UID, CC_DISC, 0x0002, b"x"))
C.resp("DISC_UN_MUTE", t.rdm(UID, CC_DISC, 0x0003), CC_DISC, 0x0003, pd=b"\x00\x00")
C.resp("DEVICE_INFO", t.rdm(UID, CC_GET, 0x0060), CC_GET, 0x0060,
       pd=bytes.fromhex("0100 0001 0101".replace(" ", "")) + BV.to_bytes(4, "big") + bytes.fromhex("0000 0000 FFFF 0000 00".replace(" ", "")))
C.resp("SOFTWARE_VERSION_LABEL", t.rdm(UID, CC_GET, 0x00C0), CC_GET, 0x00C0, pd=BOOT_LABEL)
C.resp("SUPPORTED_PARAMETERS empty", t.rdm(UID, CC_GET, 0x0050), CC_GET, 0x0050, pd=b"")
C.resp("PARAMETER_DESCRIPTION -> UNKNOWN_PID", t.rdm(UID, CC_GET, 0x0051, b"\x80\x10"), CC_GET, 0x0051,
       nack="UNKNOWN_PID")
C.resp("DMX_START_ADDRESS -> UNKNOWN_PID (footprint 0)", t.rdm(UID, CC_GET, 0x00F0), CC_GET, 0x00F0,
       nack="UNKNOWN_PID")
C.resp("GET IDENTIFY", t.rdm(UID, CC_GET, 0x1000), CC_GET, 0x1000, pd=b"\x00")
C.check("LEDs driven off in the bootloader", leds_off(t) and t.cpu.ddrc & 0x3F == 0x3F)
C.resp("SET IDENTIFY 1", t.rdm(UID, CC_SET, 0x1000, b"\x01"), CC_SET, 0x1000, pdl=0)
on = off = 0
for _ in range(40):
    t.run_ms(25)
    on += leds_on(t)
    off += leds_off(t)
C.check("bootloader identify blinks all LEDs", on >= 10 and off >= 10, f"on {on} off {off}")
C.resp("GET IDENTIFY = 1", t.rdm(UID, CC_GET, 0x1000), CC_GET, 0x1000, pd=b"\x01")
C.resp("SET IDENTIFY 0", t.rdm(UID, CC_SET, 0x1000, b"\x00"), CC_SET, 0x1000, pdl=0)
t.run_ms(30)
C.check("LEDs off after identify 0", leds_off(t))
C.resp("SET IDENTIFY 2", t.rdm(UID, CC_SET, 0x1000, b"\x02"), CC_SET, 0x1000, nack="DATA_OUT_OF_RANGE")
C.resp("SET IDENTIFY no data", t.rdm(UID, CC_SET, 0x1000), CC_SET, 0x1000, nack="FORMAT_ERROR")
C.resp("GET IDENTIFY + data", t.rdm(UID, CC_GET, 0x1000, b"foo"), CC_GET, 0x1000, nack="FORMAT_ERROR")
C.resp("SET DEVICE_INFO", t.rdm(UID, CC_SET, 0x0060), CC_SET, 0x0060, nack="UNSUPPORTED_CC")
C.resp("GET DEVICE_INFO + data", t.rdm(UID, CC_GET, 0x0060, b"x"), CC_GET, 0x0060, nack="FORMAT_ERROR")
C.resp("GET DEVICE_INFO sub-device 1", t.rdm(UID, CC_GET, 0x0060, subdev=1), CC_GET, 0x0060, subdev=1,
       nack="SUB_DEVICE_OUT_OF_RANGE")
C.resp("GET all sub-devices", t.rdm(UID, CC_GET, 0x0060, subdev=0xFFFF), CC_GET, 0x0060, subdev=0xFFFF,
       nack="SUB_DEVICE_OUT_OF_RANGE")
C.resp("SET IDENTIFY all sub-devices", t.rdm(UID, CC_SET, 0x1000, b"\x00", subdev=0xFFFF), CC_SET, 0x1000,
       subdev=0xFFFF, pdl=0)
C.resp("unknown PID", t.rdm(UID, CC_GET, 0x0400), CC_GET, 0x0400, nack="UNKNOWN_PID")
C.resp("GET WRITE_PAGE", t.rdm(UID, CC_GET, 0x8011), CC_GET, 0x8011, nack="UNSUPPORTED_CC")
C.resp("SET BOOT_INFO", t.rdm(UID, CC_SET, 0x8010), CC_SET, 0x8010, nack="UNSUPPORTED_CC")
C.resp("SET DEVICE_LABEL 49 B", t.rdm(UID, CC_SET, 0x0082, b"this is a string which is more than 32 characters"),
       CC_SET, 0x0082, nack="UNKNOWN_PID")
C.resp("GET DEVICE_INFO + 228 B", t.rdm(UID, CC_GET, 0x0060, bytes(228)), CC_GET, 0x0060, nack="FORMAT_ERROR")
a = t.rdm(UID, CC_GET, 0x0060, bytes(229))
print(f"  PDL 229 to the bootloader: {'answered' if a.raw else 'no answer (known limit, PD <= 228)'}")
C.silent("GET broadcast", t.rdm(BCAST, CC_GET, 0x0060))
C.silent("SET IDENTIFY 1 broadcast", t.rdm(BCAST, CC_SET, 0x1000, b"\x01"))
C.resp("identify set by broadcast", t.rdm(UID, CC_GET, 0x1000), CC_GET, 0x1000, pd=b"\x01")
t.rdm(UID, CC_SET, 0x1000, b"\x00")
bad = bytearray(rdm_packet(UID, CC_GET, 0x0060, tn=9))
bad[-2] ^= 1
C.silent("bad checksum", t.transact(bytes(bad)))
img = bytes(t.cpu.flash)
p = C.resp("APP_CRC 0..255", t.rdm(UID, CC_GET, 0x8012, b"\x00\x00\x01\x00"), CC_GET, 0x8012, pdl=2)
C.check("APP_CRC value", p and p.pd == crc16(img[0:256]).to_bytes(2, "big"), p and p.pd.hex())
p = C.resp("APP_CRC 0x1700..0x17FF", t.rdm(UID, CC_GET, 0x8012, b"\x17\x00\x01\x00"), CC_GET, 0x8012, pdl=2)
C.check("APP_CRC tail value", p and p.pd == crc16(img[0x1700:0x1800]).to_bytes(2, "big"))
C.resp("APP_CRC 257 B", t.rdm(UID, CC_GET, 0x8012, b"\x00\x00\x01\x01"), CC_GET, 0x8012, nack="DATA_OUT_OF_RANGE")
C.resp("APP_CRC into the bootloader", t.rdm(UID, CC_GET, 0x8012, b"\x17\x01\x01\x00"), CC_GET, 0x8012,
       nack="DATA_OUT_OF_RANGE")
C.resp("APP_CRC wrong PDL", t.rdm(UID, CC_GET, 0x8012, b"\x00\x00"), CC_GET, 0x8012, nack="FORMAT_ERROR")
C.resp("WRITE_PAGE 0x1800", t.rdm(UID, CC_SET, 0x8011, b"\x18\x00" + bytes(64)), CC_SET, 0x8011,
       nack="DATA_OUT_OF_RANGE")
C.resp("WRITE_PAGE unaligned", t.rdm(UID, CC_SET, 0x8011, b"\x00\x01" + bytes(64)), CC_SET, 0x8011,
       nack="DATA_OUT_OF_RANGE")
C.resp("WRITE_PAGE PDL 65", t.rdm(UID, CC_SET, 0x8011, b"\x00\x00" + bytes(63)), CC_SET, 0x8011, nack="FORMAT_ERROR")
# broadcast-all manufacturer PIDs are ignored (E1.20 6.2.10.2), vendorcast ones executed
before = bytes(t.cpu.flash[0x0F00:0x0F40])
C.silent("WRITE_PAGE to FFFF:FFFFFFFF", t.rdm(BCAST, CC_SET, 0x8011, b"\x0F\x00" + bytes(64)))
t.run_ms(20)
C.check("broadcast-all WRITE_PAGE ignored", bytes(t.cpu.flash[0x0F00:0x0F40]) == before)
C.silent("FINISH to FFFF:FFFFFFFF", t.rdm(BCAST, CC_SET, 0x8013))
t.run_ms(100)
C.check("broadcast-all FINISH ignored", len(t.cpu.resets) == 1 and label(t) == BOOT_LABEL)
tv = snapshot(t)
C.silent("FINISH vendorcast", tv.rdm(VCAST, CC_SET, 0x8013))
tv.run_ms(100)
C.check("vendorcast FINISH executed (app starts)", len(tv.cpu.resets) == 2, tv.cpu.resets)
# busy handling: every check right after its own WRITE_PAGE (erase + write ~9 ms)
wp = b"\x0F\x00" + before
for name, cc, pid, pd in (("WRITE_PAGE while busy", CC_SET, 0x8011, b"\x0F\x40" + bytes(64)),
                          ("APP_CRC while busy", CC_GET, 0x8012, b"\x00\x00\x01\x00"),
                          ("FINISH while busy", CC_SET, 0x8013, b"")):
    t.run_ms(20)
    C.resp("WRITE_PAGE (same content)", t.rdm(UID, CC_SET, 0x8011, wp), CC_SET, 0x8011, pdl=0)
    C.resp(name, t.rdm(UID, cc, pid, pd), cc, pid, nack="BUFFER_FULL")
t.run_ms(20)
t.rdm(UID, CC_SET, 0x8011, wp)
a, bi = boot_info(t)
C.check("BOOT_INFO busy=1, app_valid=0 after WRITE_PAGE", bi and bi[4] == 1 and bi[14] == 0, bi and bi.hex())
C.resp("DISC_MUTE after WRITE_PAGE: Boot-Loader flag", t.rdm(UID, CC_DISC, 0x0002), CC_DISC, 0x0002,
       pd=b"\x00\x04")
C.resp("DISC_UN_MUTE: same control field", t.rdm(UID, CC_DISC, 0x0003), CC_DISC, 0x0003, pd=b"\x00\x04")
t.run_ms(20)
a, bi = boot_info(t)
C.check("BOOT_INFO idle, no verify error", bi and bi[4] == 0 and bi[5] == 0, bi and bi.hex())
C.resp("FINISH (app unchanged -> valid)", t.rdm(UID, CC_SET, 0x8013), CC_SET, 0x8013, pdl=0)
t.run_ms(60)
C.check("app starts after FINISH", len(t.cpu.resets) == 2 and t.cpu.pc * 2 < 0x1800)

# ------------------------------------------------------------------ full RDM update current -> newer (flash_rdm.py)
t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
t.run_ms(2400)
res, out, tr = run_flash(t, APP2_HEX)
C.check("update current -> newer", res is None, f"{res}\n{out[-600:]}")
a = t.rdm(UID, CC_GET, 0x0060)
C.check("newer app runs", a.pkt and a.pkt.pd[6:10] == (FW_VERSION + 1).to_bytes(4, "big"), a.pkt and a.pkt.pd.hex())
turn = [x[3] for x in tr.log if x[3] is not None]
C.check("all update answers < 2 ms", max(turn) < 2000, f"max {max(turn):.0f} us")
print(f"update: {len(tr.log)} RDM transactions, turnaround {min(turn):.0f}..{max(turn):.0f} us, "
      f"sim {t.ms / 1000:.1f} s")
res, out, tr = run_flash(t, APP_HEX)
C.check("downgrade refused", res is not None and "downgrade" in str(res), str(res))
res, out, tr = run_flash(t, APP_HEX, allow_downgrade=True)
C.check("downgrade with --allow-downgrade", res is None, f"{res}\n{out[-400:]}")

# ------------------------------------------------------------------ power loss in the middle of an update
for cut in (1, 3, 40):
    t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
    t.run_ms(2400)
    tr = EmuTransport(t, fr)
    n = [0]
    orig = tr.transact

    def cutting(uid, cc, pid, pd=b"", _orig=orig):
        r = _orig(uid, cc, pid, pd)
        if pid == fr.PID_WRITE_PAGE and r is not None and r.rt == 0:
            n[0] += 1
            if n[0] == cut:
                t.cpu.power_on()                   # 24 V off/on right after this page ...
                raise fr.FlashError("power cut")   # ... and the uploader is gone too
        return r
    tr.transact = cutting
    rdm = fr.Rdm(tr, sleep=lambda s: t.run_us(s * 1e6))
    res, out = quiet(fr.flash, rdm, UID, APP2_HEX, sleep=lambda s: t.run_us(s * 1e6))
    C.check(f"cut after page {cut}: update aborted", res is not None, str(res))
    t.run_ms(3000)
    C.check(f"cut after page {cut}: bootloader stays (app invalid)", label(t) == BOOT_LABEL)
    C.resp(f"cut after page {cut}: Boot-Loader flag", t.rdm(UID, CC_DISC, 0x0002), CC_DISC, 0x0002, pd=b"\x00\x04")
    res, out, tr = run_flash(t, APP2_HEX)
    C.check(f"cut after page {cut}: second run succeeds", res is None, f"{res}\n{out[-400:]}")

# ------------------------------------------------------------------ app that never answers -> rescue
t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
t.run_ms(2400)
res, out, tr = run_flash(t, HANG_HEX)
C.check("hanging app: flash_rdm notices (no answer after FINISH)", res is not None and "no answer" in str(res), str(res))
res, out, tr = run_flash(t, APP_HEX, rescue="relay", allow_downgrade=True)
C.check("rescue (power cycle + BOOT_INFO spam) restores the current app", res is None, f"{res}\n{out[-600:]}")
a = t.rdm(UID, CC_GET, 0x00C0)
C.check("current app after rescue", a.pkt and a.pkt.pd == FW_LABEL, a.pkt and a.pkt.pd)

# ------------------------------------------------------------------ rescue without known UID
# vendorcast SET of a bootloader PID inside the window keeps every tube in the
# bootloader (then discovery finds them); FFFF:FFFFFFFF does not (6.2.10.2)
rescue_all = rdm_packet(VCAST, CC_SET, 0x8010, b"", tn=0)
t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
t.run_ms(120)
C.silent("rescue-all vendorcast SET 0x8010", t.transact(rescue_all))
t.run_ms(600)
C.check("rescue-all: bootloader stays", len(t.cpu.resets) == 1 and label(t) == BOOT_LABEL)
C.check("rescue-all: discoverable", dub_decode(dub(t, bytes(6), BCAST).raw) == UID)
t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
t.run_ms(120)
t.transact(rdm_packet(BCAST, CC_SET, 0x8010, b"", tn=0))
t.run_ms(600)
C.check("FFFF:FFFFFFFF SET 0x8010 ignored (app starts)", len(t.cpu.resets) == 2)
t.run_ms(2000)
t.transact(rescue_all)
t.run_ms(100)
C.check("rescue-all packet has no effect on the running app", label(t) == FW_LABEL and len(t.cpu.resets) == 2)

# ------------------------------------------------------------------ FINISH with a foreign image (model 2)
t = snapshot(boot)
im = fr.read_image(APP2_HEX)
img = bytearray(im.img)
img[0x17F6] = 2                                   # model ID 2, CRC recomputed -> trailer valid, model wrong
img[0x17FE:0x1800] = fr.trailer_crc(img, im.length).to_bytes(2, "little")
rdm = fr.Rdm(EmuTransport(t, fr), sleep=lambda s: t.run_us(s * 1e6))
for addr, data in fr.page_plan(img, im.length):
    rdm.set(UID, fr.PID_WRITE_PAGE, addr.to_bytes(2, "big") + data)
t.run_ms(20)
rdm.set(UID, fr.PID_FINISH)
t.run_ms(100)
a, bi = boot_info(t)
C.check("foreign model: FINISH refused, error 3, stays", bi and bi[5] == 3 and bi[14] == 0 and len(t.cpu.resets) == 1,
        bi and bi.hex())

# ------------------------------------------------------------------ bootloader only (no app), blank EEPROM
t = Tube([BOOT_HEX])
t.run_ms(400)
a = dub(t, bytes(6), BCAST)
uid0 = dub_decode(a.raw)
print(f"  blank EEPROM, no app: bootloader UID {uid0.hex() if uid0 else None}")
C.check("no app: bootloader stays", len(t.cpu.resets) == 1)
t = Tube([BOOT_HEX, APP_HEX])                     # merged image, blank EEPROM (fresh ISP flash)
t.run_ms(100)
uid_first = dub_decode(dub(t, bytes(6), BCAST).raw)
t.run_ms(2500)
gen = bytes(t.cpu.eeprom[0:4])
C.check("app generated a device ID", gen not in (b"\xFF" * 4, b"\x00" * 4), gen.hex())
a = dub(t, bytes(6), BCAST)
C.check("app answers with 7FF0:<generated ID>", dub_decode(a.raw) == b"\x7F\xF0" + gen, a.raw.hex())
t.cpu.power_on()
t.run_ms(100)
C.check("bootloader uses the generated ID after the next reset",
        dub_decode(dub(t, bytes(6), BCAST).raw) == b"\x7F\xF0" + gen)
print(f"  first boot after ISP (blank EEPROM): bootloader UID in the window {uid_first.hex() if uid_first else None}, "
      f"generated ID {gen.hex()}")

print(f"\nbootloader tests: {time.time() - t0:.0f} s")
sys.exit(0 if C.summary() else 1)
