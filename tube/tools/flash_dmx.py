#!/usr/bin/env python3
"""Firmware update over the DMX line (RS-485) via tube_boot and the ESP32 tool.

  .venv/bin/python flash_dmx.py 7FF0:74B6510C firmware/tube_fw/.pio/build/isp/firmware.hex
  .venv/bin/python flash_dmx.py --rescue 7FF0:74B6510C firmware.hex
        app dead / no RDM: power-cycle the tube and catch the ~150 ms boot window
  .venv/bin/python flash_dmx.py --hex-out merged.hex [--boot boot.hex] firmware.hex
        no tube access: write the app image incl. length/CRC trailer (optionally
        merged with the bootloader) as Intel HEX for the one-time ISP flash

Protocol (see firmware/tube_boot/src/main.c), 250000 8N2, each packet after a BREAK:
  request  F1 uid[6] cmd len payload crc16      response  F2 uid[6] cmd status len payload crc16
  crc16 = CRC16-CCITT (0x1021, init 0xFFFF), big-endian, over start code..payload
"""
import argparse
import sys
import time

import tool

PAGE = 64
BOOT_START = 0x1C00
TRAILER = BOOT_START - 4            # uint16 length + uint16 CRC, little-endian
APP_MAX = TRAILER                   # 7164 B
LAST_PAGE = BOOT_START - PAGE       # page holding the trailer

CMD_HELLO, CMD_WRITE_PAGE, CMD_READ_CRC, CMD_FINISH = 1, 2, 3, 4
STATUS = {0: "OK", 1: "bad argument", 2: "verify error", 3: "app invalid", 4: "unknown command"}


class FlashError(Exception):
    pass


def crc16(data, c=0xFFFF):
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def parse_uid(s):
    h = s.replace(":", "")
    if len(h) != 12:
        raise FlashError(f"bad UID '{s}' (expected e.g. 7FF0:74B6510C)")
    return bytes.fromhex(h)


def read_ihex(path):
    """Intel HEX -> {address: byte}."""
    mem, base = {}, 0
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise FlashError(f"{path}:{n}: not Intel HEX")
            rec = bytes.fromhex(line[1:])
            if sum(rec) & 0xFF:
                raise FlashError(f"{path}:{n}: checksum error")
            cnt, addr, typ, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:4 + rec[0]]
            if len(data) != cnt:
                raise FlashError(f"{path}:{n}: short record")
            if typ == 0:
                for i, b in enumerate(data):
                    mem[base + addr + i] = b
            elif typ == 1:
                break
            elif typ == 2:
                base = ((data[0] << 8) | data[1]) << 4
            elif typ == 4:
                base = ((data[0] << 8) | data[1]) << 16
            # 3/5 = start address: irrelevant
    return mem


def build_image(app_hex):
    """App image for 0..0x1BFF with trailer. Returns (image, length, crc)."""
    mem = read_ihex(app_hex)
    if not mem:
        raise FlashError(f"{app_hex}: no data")
    if min(mem) < 0 or max(mem) >= APP_MAX:
        raise FlashError(f"{app_hex}: data at 0x{max(mem):04X}, app must stay below 0x{APP_MAX:04X} "
                         f"({APP_MAX} B, the rest is trailer + bootloader)")
    length = (max(mem) + 2) & ~1                 # whole words
    img = bytearray([0xFF] * BOOT_START)
    for a, b in mem.items():
        img[a] = b
    crc = crc16(img[:length])
    img[TRAILER:TRAILER + 4] = bytes([length & 0xFF, length >> 8, crc & 0xFF, crc >> 8])
    return img, length, crc


def write_ihex(path, mem):
    """{address: byte} -> Intel HEX (16-byte records, skips 0xFF-only gaps)."""
    lines = []
    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        start = addrs[i]
        chunk = [mem[start]]
        i += 1
        while i < len(addrs) and addrs[i] == start + len(chunk) and len(chunk) < 16 and (start + len(chunk)) % 16:
            chunk.append(mem[addrs[i]])
            i += 1
        rec = bytes([len(chunk), start >> 8, start & 0xFF, 0]) + bytes(chunk)
        lines.append(":" + (rec + bytes([-sum(rec) & 0xFF])).hex().upper())
    lines.append(":00000001FF")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def hex_out(app_hex, out, boot_hex=None):
    img, length, crc = build_image(app_hex)
    mem = {a: b for a, b in enumerate(img) if a < length or a >= TRAILER}
    if boot_hex:
        boot = read_ihex(boot_hex)
        if min(boot) < BOOT_START or max(boot) > 0x1FFF:
            raise FlashError(f"{boot_hex}: data outside the boot section 0x1C00..0x1FFF")
        mem.update(boot)
    write_ihex(out, mem)
    print(f"{out}: app {length} B, crc 0x{crc:04X}" + (f", + bootloader {len(boot)} B" if boot_hex else ""))


# ---------------------------------------------------------------- bus access via the ESP32

def packet(uid, cmd, payload=b""):
    p = bytes([0xF1]) + uid + bytes([cmd, len(payload)]) + bytes(payload)
    c = crc16(p)
    return p + bytes([c >> 8, c & 0xFF])


def parse_reply(rx, uid, cmd):
    """Finds a valid F2 response in the raw RX bytes -> (status, payload) or None."""
    for i in range(len(rx)):
        if rx[i] != 0xF2 or i + 12 > len(rx):
            continue
        n = rx[i + 9]
        end = i + 10 + n
        if end + 2 > len(rx):
            continue
        if crc16(rx[i:end]) != (rx[end] << 8) | rx[end + 1]:
            continue
        if rx[i + 1:i + 7] != uid or rx[i + 7] != cmd:
            continue
        return rx[i + 8], rx[i + 10:end]
    return None


def rx_bytes(lines):
    for line in lines:
        if line.startswith("RX: "):
            h = line[4:].strip()
            return b"" if h == "-" else bytes.fromhex(h)
    return b""


def request(ser, uid, cmd, payload=b"", tries=3, what=""):
    pkt = packet(uid, cmd, payload)
    for t in range(tries):
        r = parse_reply(rx_bytes(tool.run(ser, "bl " + pkt.hex().upper(), timeout=5, quiet=True)), uid, cmd)
        if r is not None:
            return r
        if t + 1 < tries:
            print(f"  {what or 'cmd %d' % cmd}: no answer, retry {t + 1}")
    raise FlashError(f"{what or 'cmd %d' % cmd}: no valid answer from the bootloader after {tries} tries")


def check_hello(r):
    st, pl = r
    if st != 0 or len(pl) < 4:
        raise FlashError(f"HELLO: unexpected answer status {st} payload {pl.hex()}")
    ver, page, amax = pl[0], pl[1], (pl[2] << 8) | pl[3]
    print(f"bootloader v{ver}, page {page} B, app space 0x{amax:04X}")
    if page != PAGE or amax != BOOT_START:
        raise FlashError("bootloader geometry does not match this script")


def enter_bootloader(ser, uid, uid_s, rescue):
    hello = packet(uid, CMD_HELLO)
    if rescue:
        print("rescue: power cycle, then HELLO spam during the boot window ...")
        tool.run(ser, "power cycle 3000", timeout=10, quiet=True)
        lines = tool.run(ser, "blr 2000 " + hello.hex().upper(), timeout=10, quiet=True)
        r = parse_reply(rx_bytes(lines), uid, CMD_HELLO)
        if r is None:
            raise FlashError("rescue: no HELLO answer in the boot window (bootloader installed? fuses set? UID right?)")
        return r

    print(f"RDM SET 0x8000 'BOOT' -> {uid_s}")
    lines = tool.run(ser, f"rdm set {uid_s} 8000 424F4F54", timeout=5, quiet=True)
    if any(" ACK " in l and "pid=0x8000" in l for l in lines):
        print("  ACK, app resets into the bootloader")
    else:
        print("  no ACK (" + (lines[-1] if lines else "no output") + ") - trying HELLO anyway "
              "(maybe the bootloader is already active)")
    time.sleep(0.5)
    return request(ser, uid, CMD_HELLO, tries=5, what="HELLO")


def flash(ser, uid_s, app_hex, rescue):
    uid = parse_uid(uid_s)
    img, length, crc = build_image(app_hex)
    print(f"{app_hex}: {length} B (max {APP_MAX}), crc 0x{crc:04X}")

    check_hello(enter_bootloader(ser, uid, uid_s, rescue))

    # trailer page first with erased trailer (app invalid while we write),
    # then the app pages, the trailer page with the real trailer last
    pages = [a for a in range(0, length, PAGE) if a != LAST_PAGE]
    blank_trailer = img[LAST_PAGE:TRAILER] + bytes([0xFF] * 4)
    plan = [(LAST_PAGE, blank_trailer)] + [(a, img[a:a + PAGE]) for a in pages] + [(LAST_PAGE, img[LAST_PAGE:BOOT_START])]
    t0 = time.time()
    for i, (a, data) in enumerate(plan, 1):
        what = f"WRITE_PAGE 0x{a:04X}"
        err = "no answer"
        for attempt in range(3):
            try:
                st, _ = request(ser, uid, CMD_WRITE_PAGE, bytes([a >> 8, a & 0xFF]) + data, tries=1, what=what)
            except FlashError:
                st = None
            if st == 0:
                break
            err = "no answer" if st is None else STATUS.get(st, st)
            print(f"\n  {what}: {err}, retry {attempt + 1}")
        else:
            raise FlashError(f"{what}: {err} after 3 tries - bootloader stays active, run again")
        print(f"\r  page 0x{a:04X}  {i}/{len(plan)}", end="", flush=True)
    print(f"  ({time.time() - t0:.1f} s)")

    st, pl = request(ser, uid, CMD_READ_CRC, bytes([length >> 8, length & 0xFF]), what="READ_CRC")
    dev_crc = (pl[0] << 8) | pl[1] if st == 0 and len(pl) >= 2 else None
    if dev_crc != crc:
        raise FlashError(f"READ_CRC: device 0x{dev_crc if dev_crc is not None else 0:04X} != image 0x{crc:04X} "
                         f"(status {STATUS.get(st, st)}) - bootloader stays active, run again")
    print(f"CRC ok (0x{crc:04X})")

    st, _ = request(ser, uid, CMD_FINISH, what="FINISH")
    if st != 0:
        raise FlashError(f"FINISH: {STATUS.get(st, st)}")
    print("FINISH ok, app starts (self-test ~2 s)")

    time.sleep(3)
    lines = tool.run(ser, "rdm disc", timeout=60)
    want = uid_s.upper() if ":" in uid_s else f"{uid_s[:4]}:{uid_s[4:]}".upper()
    if not any(l.startswith("FOUND") and want in l.upper() for l in lines):
        raise FlashError(f"app does not answer RDM discovery ({want} not found)")
    tool.run(ser, f"rdm info {uid_s}", timeout=20)
    print("done: app runs")


def main():
    ap = argparse.ArgumentParser(description="tube firmware update over RS-485 (tube_boot)")
    ap.add_argument("args", nargs="+", help="<uid> <firmware.hex>, or only <firmware.hex> with --hex-out")
    ap.add_argument("--rescue", action="store_true", help="power-cycle and catch the 150 ms boot window")
    ap.add_argument("--hex-out", metavar="OUT", help="only write app image + trailer as Intel HEX")
    ap.add_argument("--boot", metavar="BOOT_HEX", help="with --hex-out: merge this bootloader hex")
    a = ap.parse_args()
    try:
        if a.hex_out:
            if len(a.args) != 1:
                ap.error("--hex-out takes exactly one <firmware.hex>")
            hex_out(a.args[0], a.hex_out, a.boot)
            return 0
        if len(a.args) != 2:
            ap.error("need <uid> <firmware.hex>")
        parse_uid(a.args[0])                     # fail early, before touching the tube
        build_image(a.args[1])
        ser = tool.open_port()
        flash(ser, a.args[0], a.args[1], a.rescue)
        return 0
    except (FlashError, OSError) as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
