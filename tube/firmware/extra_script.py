# Post-build for the 2 KB layout (BOOT_LAYOUT_2K, tube_boot_rdm):
#  - links the trailer metadata (section .trailer, src/main.c) to 0x17F0 and
#    keeps it despite --gc-sections
#  - after firmware.hex: fills in app length + CRC16 (boot_shared.h) and writes
#    tube_fw-<version>.hex = app + complete 16 B trailer, ready for any
#    uploader (flash_rdm.py, Pixelmapper) - nothing left to compute there.
# The legacy env isp_1k (no BOOT_LAYOUT_2K) is left untouched.
import glob
import os

Import("env")

BOOT_START = 0x1800
TRAILER_START = BOOT_START - 16          # 0x17F0, boot_shared.h
META_LEN = 12                            # 0x17F0..0x17FB, in the CRC
TRAILER_MAGIC, TRAILER_FORMAT = 0x4D50, 1


def crc16(data, c=0xFFFF):
    """CRC16-CCITT, poly 0x1021, no reflection (= avr-libc _crc_xmodem_update)."""
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def read_ihex(path):
    mem, base = {}, 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(":"):
            continue
        r = bytes.fromhex(line[1:])
        n, a, t = r[0], (r[1] << 8) | r[2], r[3]
        if t == 0:
            for i in range(n):
                mem[base + a + i] = r[4 + i]
        elif t == 2:
            base = ((r[4] << 8) | r[5]) << 4
        elif t == 4:
            base = ((r[4] << 8) | r[5]) << 16
        elif t == 1:
            break
    return mem


def write_ihex(path, segments):
    """segments: [(start address, bytes)], 16 B records, addresses < 64 KB."""
    with open(path, "w") as f:
        for start, data in segments:
            for o in range(0, len(data), 16):
                a = start + o
                rec = bytes([len(data[o:o + 16]), a >> 8, a & 0xFF, 0]) + data[o:o + 16]
                f.write(":%s%02X\n" % (rec.hex().upper(), (-sum(rec)) & 0xFF))
        f.write(":00000001FF\n")


def add_trailer(source, target, env):
    hexfile = str(target[0])
    mem = read_ihex(hexfile)
    meta = bytes(mem.get(TRAILER_START + i, 0xFF) for i in range(META_LEN))
    if int.from_bytes(meta[0:2], "little") != TRAILER_MAGIC or meta[2] != TRAILER_FORMAT:
        print(f"ERROR: {hexfile}: no trailer metadata at 0x{TRAILER_START:04X} (section .trailer missing?)")
        env.Exit(1)
    app = [a for a in mem if a < TRAILER_START]
    if any(a >= BOOT_START or TRAILER_START + META_LEN <= a < BOOT_START for a in mem):
        print(f"ERROR: {hexfile}: data behind the trailer metadata")
        env.Exit(1)
    length = (max(app) + 2) & ~1                       # whole words
    img = bytearray([0xFF] * length)
    for a in app:
        img[a] = mem[a]
    crc = crc16(meta, crc16(img))
    trailer = meta + length.to_bytes(2, "little") + crc.to_bytes(2, "little")
    v = int.from_bytes(meta[8:12], "little")
    ver = f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"
    out_dir = os.path.dirname(hexfile)
    out = os.path.join(out_dir, f"tube_fw-{ver}.hex")
    for old in glob.glob(os.path.join(out_dir, "tube_fw-*.hex")):
        if old != out:
            os.remove(old)                             # one image per build dir
    write_ihex(out, [(0, bytes(img)), (TRAILER_START, trailer)])
    print(f"{out}: app {length} B (max {TRAILER_START}), fw {ver} (0x{v:08X}), "
          f"model 0x{int.from_bytes(meta[6:8], 'little'):04X}, crc 0x{crc:04X}")


if "BOOT_LAYOUT_2K" in [d if isinstance(d, str) else d[0] for d in env.get("CPPDEFINES", [])]:
    env.Append(LINKFLAGS=[f"-Wl,--section-start=.trailer=0x{TRAILER_START:X}", "-Wl,-u,app_trailer_meta"])
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", add_trailer)
