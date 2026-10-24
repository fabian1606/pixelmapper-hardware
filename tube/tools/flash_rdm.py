#!/usr/bin/env python3
"""Firmware update over RDM (ANSI E1.20) via tube_boot_rdm - through the ESP32
debug tool or through any Art-Net 4 node (ArtRdm).

  .venv/bin/python flash_rdm.py 7FF0:B06675AA tube/firmware/.pio/build/isp/tube_fw-0.5.0.hex
  .venv/bin/python flash_rdm.py 7FF0:B06675AA tube_fw-0.5.0.hex --via artnet --node 2.0.0.10 --universe 0
  .venv/bin/python flash_rdm.py --tod --via artnet --node 2.0.0.10 --universe 0
        show the node's table of devices (ArtTodRequest / ArtTodData)
  .venv/bin/python flash_rdm.py --rescue 7FF0:B06675AA tube_fw-0.5.0.hex
        (esp32 only) app dead / no RDM: power-cycle and catch the ~200 ms boot window
  .venv/bin/python flash_rdm.py --hex-out tube_merged_rdm.hex \\
        --boot tube/boot_rdm/.pio/build/boot/firmware.hex tube_fw-0.5.0.hex
        no tube access: app + trailer (+ bootloader) as one Intel HEX for the
        one-time ISP flash (2 KB layout, hfuse 0x88)
  .venv/bin/python flash_rdm.py --selftest
        offline tests: RDM / ArtRdm packets, checksums, HEX + trailer, full update
        flow against a simulated bootloader

Flash layout (tube/firmware/include/boot_shared.h, BOOT_LAYOUT_2K):
  0x0000..0x17EF app (max 6128 B), 0x17F0..0x17FF trailer (16 B, little-endian):
    magic 0x4D50, format 1, reserved 0xFF, manufacturer ID, model ID, FW_VERSION_ID,
    length, CRC16-CCITT (0x1021, init 0xFFFF) over 0..length-1 then 0x17F0..0x17FB
  0x1800..0x1FFF tube_boot_rdm.
The input is the finished image of the tube_fw build (.pio/build/isp/tube_fw-<version>.hex,
trailer complete) - this script only checks the trailer, it never computes one.
Before writing, manufacturer + model ID of the tube (DEVICE_INFO / BOOT_INFO) must match
the trailer; a lower FW_VERSION_ID than the installed one needs --allow-downgrade.
Bootloader PIDs: tube/firmware/include/rdm_boot.h (parsed below).
The old 1 KB tube_boot (0x1C00, F1/F2 protocol) is handled by flash_dmx.py.
"""
import argparse
import dataclasses
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import time

from flash_dmx import FlashError, crc16, parse_uid, read_ihex, write_ihex

HERE = pathlib.Path(__file__).resolve().parent
TUBE = HERE.parent                                   # tube/ (firmware/, boot_rdm/, tools/)
INCLUDE = TUBE / "firmware" / "include"
PID_HEADER = INCLUDE / "rdm_boot.h"


def read_defines(path):
    """'#define NAME <int literal>' lines of a C header -> {NAME: int}."""
    return {m.group(1): int(m.group(2), 0) for m in
            re.finditer(r"^#define\s+(\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)\b", pathlib.Path(path).read_text(), re.M)}


H = read_defines(PID_HEADER)
PID_ENTER_BOOTLOADER = H["PID_ENTER_BOOTLOADER"]   # app, SET "BOOT"
PID_BOOT_INFO = H["PID_BOOT_INFO"]                 # GET -> 19 B, see boot_info()
PID_WRITE_PAGE = H["PID_BOOT_WRITE_PAGE"]          # SET addr BE16 + 64 B
PID_APP_CRC = H["PID_BOOT_APP_CRC"]                # GET len BE16 -> crc BE16
PID_FINISH = H["PID_BOOT_FINISH"]                  # SET
BOOT_SW_VERSION = H["BOOT_RDM_SW_VERSION_ID"]      # DEVICE_INFO software version in the bootloader
PAGE = H["BOOT_RDM_PAGE_SIZE"]
CRC_CHUNK = H["BOOT_RDM_CRC_MAX_LEN"]            # APP_CRC: address + length <= 256 per request
BOOT_VERSION = H["BOOT_RDM_VERSION"]             # current tube_boot_rdm (BOOT_INFO byte 0)
BOOT_PROTOCOL = H["BOOT_RDM_PROTOCOL"]           # oldest BOOT_INFO byte 0 with the 19-byte answer / these PIDs
ERR_NONE, ERR_VERIFY, ERR_INVALID, ERR_MODEL, ERR_FORMAT = (
    H["BOOT_ERR_NONE"], H["BOOT_ERR_VERIFY"], H["BOOT_ERR_APP_INVALID"], H["BOOT_ERR_MODEL"], H["BOOT_ERR_FORMAT"])
BOOT_ERRORS = {ERR_NONE: "none", ERR_VERIFY: "verify error (page content differs)",
               ERR_INVALID: "app invalid (trailer length/CRC wrong)",
               ERR_MODEL: "wrong model (trailer manufacturer/model ID not this device)",
               ERR_FORMAT: "unknown trailer format (magic/format, or blank)"}

S = read_defines(INCLUDE / "boot_shared.h")         # (BOOT_START is defined twice there - not used)
TRAILER_MAGIC, TRAILER_FORMAT, META_LEN = S["TRAILER_MAGIC"], S["TRAILER_FORMAT"], S["TRAILER_META_LEN"]
BOOT_START = 0x1800                 # 2 KB boot section (BOOTSZ=00)
TRAILER_START = BOOT_START - 16     # 0x17F0: 16 B trailer, see module docstring
TRAILER = BOOT_START - 4            # 0x17FC: uint16 length + uint16 CRC, little-endian
APP_MAX = TRAILER_START             # 6128 B
LAST_PAGE = TRAILER_START & ~(PAGE - 1)   # page holding the trailer (0x17C0)

# ---------------------------------------------------------------- RDM (E1.20)
SC_RDM, SC_SUB = 0xCC, 0x01
CC_DISC, CC_GET, CC_SET = 0x10, 0x20, 0x30
RT_ACK, RT_ACK_TIMER, RT_NACK, RT_ACK_OVERFLOW = 0, 1, 2, 3
PID_DEVICE_INFO, PID_SOFTWARE_VERSION_LABEL = 0x0060, 0x00C0
PID_SUPPORTED_PARAMETERS, PID_PARAMETER_DESCRIPTION = 0x0050, 0x0051
NR_BUFFER_FULL = 0x0007                    # tube_boot_rdm: busy writing a page, repeat the request
NACK_REASONS = {0x0000: "UNKNOWN_PID", 0x0001: "FORMAT_ERROR", 0x0002: "HARDWARE_FAULT", 0x0003: "PROXY_REJECT",
                0x0004: "WRITE_PROTECT", 0x0005: "UNSUPPORTED_COMMAND_CLASS", 0x0006: "DATA_OUT_OF_RANGE",
                0x0007: "BUFFER_FULL", 0x0008: "PACKET_SIZE_UNSUPPORTED", 0x0009: "SUB_DEVICE_OUT_OF_RANGE",
                0x000A: "PROXY_BUFFER_FULL"}
HOST_UID = bytes.fromhex("7FF000000002")   # our controller UID (ESP32 tool uses ...01)


class NoAnswer(FlashError):
    pass


class RdmNack(FlashError):
    def __init__(self, pid, reason):
        self.reason = reason
        super().__init__(f"PID 0x{pid:04X}: NACK {NACK_REASONS.get(reason, '0x%04X' % reason)}")


@dataclasses.dataclass
class RdmPacket:
    dest: bytes
    src: bytes
    tn: int
    rt: int            # port id (request) / response type (response)
    msgcnt: int
    subdev: int
    cc: int
    pid: int
    pd: bytes


def uid_str(uid):
    return f"{uid[:2].hex()}:{uid[2:].hex()}".upper()


def rdm_checksum(data):
    return sum(data) & 0xFFFF


def rdm_build(dest, src, tn, cc, pid, pd=b"", port=1, subdev=0, msgcnt=0):
    """Complete RDM packet incl. start code 0xCC and checksum."""
    pd = bytes(pd)
    if len(pd) > 231:
        raise ValueError("RDM parameter data > 231 B")
    body = (bytes([SC_RDM, SC_SUB, 24 + len(pd)]) + bytes(dest) + bytes(src) +
            bytes([tn & 0xFF, port, msgcnt]) + subdev.to_bytes(2, "big") + bytes([cc]) +
            pid.to_bytes(2, "big") + bytes([len(pd)]) + pd)
    return body + rdm_checksum(body).to_bytes(2, "big")


def rdm_parse(p):
    """RDM packet starting with 0xCC -> RdmPacket; ValueError if invalid."""
    if len(p) < 26 or p[0] != SC_RDM or p[1] != SC_SUB:
        raise ValueError("no RDM start code")
    n = p[2]
    if n < 24 or len(p) < n + 2 or p[23] + 24 != n:
        raise ValueError("bad RDM length")
    if rdm_checksum(p[:n]) != int.from_bytes(p[n:n + 2], "big"):
        raise ValueError("bad RDM checksum")
    return RdmPacket(bytes(p[3:9]), bytes(p[9:15]), p[15], p[16], p[17], int.from_bytes(p[18:20], "big"),
                     p[20], int.from_bytes(p[21:23], "big"), bytes(p[24:n]))


def rdm_find(raw):
    """All valid RDM packets in raw line bytes (e.g. with a leading break 0x00)."""
    out, i = [], 0
    while i + 26 <= len(raw):
        try:
            pkt = rdm_parse(raw[i:])
        except ValueError:
            i += 1
            continue
        out.append(pkt)
        i += raw[i + 2] + 2
    return out


def dub_encode(uid):
    """DISC_UNIQUE_BRANCH answer per E1.20 7.5.1 (reference for the firmware shortcut)."""
    enc = b"".join(bytes([b | 0xAA, b | 0x55]) for b in uid)
    s = sum(enc)
    return b"\xFE" * 7 + b"\xAA" + enc + bytes([(s >> 8) | 0xAA, (s >> 8) | 0x55, (s & 0xFF) | 0xAA, (s & 0xFF) | 0x55])


def dub_decode(raw):
    """UID from a DUB answer, None if garbled/collision."""
    i = 0
    while i < len(raw) and i < 7 and raw[i] == 0xFE:
        i += 1
    if i >= len(raw) or raw[i] != 0xAA or len(raw) - i - 1 < 16:
        return None
    e = raw[i + 1:i + 17]
    uid = bytes(e[2 * k] & e[2 * k + 1] for k in range(6))
    s = bytes(e[12 + 2 * k] & e[13 + 2 * k] for k in range(2))
    return uid if int.from_bytes(s, "big") == sum(e[:12]) else None


# ---------------------------------------------------------------- Art-Net 4
ARTNET_PORT = 6454
ART_ID = b"Art-Net\x00"
OP_TODREQUEST, OP_TODDATA, OP_RDM = 0x8000, 0x8100, 0x8300
ART_PROTVER = 14


def art_header(op):
    return ART_ID + op.to_bytes(2, "little") + bytes([0, ART_PROTVER])


def artrdm_build(universe, rdm_pkt):
    """ArtRdm: RDM packet WITHOUT the 0xCC start code (Art-Net 4). universe = 15-bit port-address."""
    if rdm_pkt[0] != SC_RDM:
        raise ValueError("expects a packet starting with 0xCC")
    return (art_header(OP_RDM) + bytes([0x01, 0x00]) + bytes(7) +        # RdmVer 1.0, Filler2, Spare1..7
            bytes([(universe >> 8) & 0x7F, 0x00, universe & 0xFF]) +    # Net, Command ArProcess, Address
            bytes(rdm_pkt[1:]))


def artrdm_parse(data):
    """ArtRdm -> (universe, RDM packet incl. 0xCC) or None."""
    if len(data) < 24 + 25 or data[:8] != ART_ID or int.from_bytes(data[8:10], "little") != OP_RDM:
        return None
    return ((data[21] & 0x7F) << 8) | data[23], bytes([SC_RDM]) + bytes(data[24:])


def arttodrequest_build(universe):
    return (art_header(OP_TODREQUEST) + bytes(2) + bytes(7) +              # Filler1/2, Spare1..7
            bytes([(universe >> 8) & 0x7F, 0x00, 1, universe & 0xFF]) +  # Net, TodFull, AdCount, Address[0]
            bytes(31))


def arttoddata_parse(data):
    """ArtTodData -> (universe, uid_total, block, [uids]) or None."""
    if len(data) < 28 or data[:8] != ART_ID or int.from_bytes(data[8:10], "little") != OP_TODDATA:
        return None
    n = data[27]
    uids = [bytes(data[28 + 6 * k:34 + 6 * k]) for k in range(n) if 34 + 6 * k <= len(data)]
    return ((data[21] & 0x7F) << 8) | data[23], (data[24] << 8) | data[25], data[26], uids


# ---------------------------------------------------------------- transports
# transact(uid, cc, pid, pd) -> RdmPacket (response) or None (no / no valid answer)

def lines_rx_bytes(lines):
    """Raw RX bytes from tube_debug output: 'RX 27 bytes: 00 CC ..' (rdm) or 'RX: 00CC..' / 'RX: -' (bl/blr)."""
    for line in lines:
        m = re.match(r"\s*RX(?: \d+ bytes)?:\s*(.*)$", line)
        if m:
            h = re.sub(r"[^0-9A-Fa-f]", "", m.group(1).split('"')[0])
            return bytes.fromhex(h[:len(h) & ~1])
    return b""


def match_response(pkts, uid, cc, pid, tn=None):
    for p in pkts:
        if p.src == uid and p.cc == cc + 1 and p.pid == pid and (tn is None or p.tn == tn):
            return p
    return None


class Esp32Transport:
    """ESP32 tube_debug firmware via tool.py ('rdm get/set', 'power', 'blr')."""
    name = "esp32"

    def __init__(self):
        import tool
        self.tool = tool
        self.ser = tool.open_port()

    def transact(self, uid, cc, pid, pd=b""):
        # pid as 0x.... : the ESP32 locates the data after the first occurrence of the pid text
        cmd = f"rdm {'get' if cc == CC_GET else 'set'} {uid_str(uid)} 0x{pid:04X}" + (f" {pd.hex().upper()}" if pd else "")
        lines = self.tool.run(self.ser, cmd, timeout=5, quiet=True)
        return match_response(rdm_find(lines_rx_bytes(lines)), uid, cc, pid)

    def rescue(self, uid, mode="relay"):
        """Catch the ~200 ms boot window with a GET BOOT_INFO spam (ESP32 'blr'); returns the answer or None.
        relay: power cycle via relay; manual: user replugs 24 V (60 s); isp: Uno avrdude signature read = reset."""
        pkt = rdm_build(uid, HOST_UID, 0, CC_GET, PID_BOOT_INFO)
        hexpkt = pkt.hex().upper()
        if mode == "relay":
            self.tool.run(self.ser, "power cycle 3000", timeout=10, quiet=True)
            lines = self.tool.run(self.ser, "blr 2000 " + hexpkt, timeout=10, quiet=True)
        elif mode == "manual":
            self.ser.write(f"blr 60000 {hexpkt}\n".encode())
            print("Jetzt die 24 V der Tube kurz trennen und wieder anstecken ... (60 s)", flush=True)
            lines = self._read_blr(65)
        elif mode == "isp":
            self.ser.write(f"blr 15000 {hexpkt}\n".encode())
            time.sleep(0.5)
            self._isp_reset()
            lines = self._read_blr(20)
        else:
            raise FlashError(f"unknown rescue mode {mode!r}")
        return match_response(rdm_find(lines_rx_bytes(lines)), uid, CC_GET, PID_BOOT_INFO)

    def _read_blr(self, timeout):
        lines, end = [], time.time() + timeout
        while time.time() < end:
            line = self.ser.readline().decode(errors="replace").rstrip()
            if line == "> done":
                break
            if line:
                lines.append(line)
        return lines

    @staticmethod
    def _isp_reset():
        """Read-only avrdude signature read via the Uno (ArduinoISP): the reset pulse restarts the tube."""
        import glob
        base = os.path.expanduser("~/Library/Arduino15/packages/arduino/tools/avrdude/8.0.0-arduino1")
        exe, conf = f"{base}/bin/avrdude", f"{base}/etc/avrdude.conf"
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if not ports:
            raise FlashError("rescue isp: no Uno found (/dev/cu.usbmodem*)")
        try:
            av = subprocess.run([exe, "-C", conf, "-p", "m8", "-c", "stk500v1", "-P", ports[0], "-b", "19200",
                                 "-U", "signature:r:-:h", "-q"], capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise FlashError(f"rescue isp: avrdude failed ({e})") from e
        print(f"rescue isp: avrdude rc {av.returncode} {av.stdout.strip()} ({ports[0]})")


class ArtNetTransport:
    """ArtRdm to one Art-Net 4 node (unicast), replies come back to UDP 6454."""
    name = "artnet"

    def __init__(self, node, universe, timeout=0.6):
        self.node, self.universe, self.timeout, self.tn = node, universe, timeout, 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            self.sock.bind(("", ARTNET_PORT))   # nodes always answer to port 6454
        except OSError as e:
            raise FlashError(f"cannot bind UDP {ARTNET_PORT} ({e}) - close other Art-Net software") from e

    def _recv_until(self, deadline):
        while True:
            left = deadline - time.time()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                return None
            yield data

    def transact(self, uid, cc, pid, pd=b""):
        self.tn = (self.tn + 1) & 0xFF
        pkt = rdm_build(uid, HOST_UID, self.tn, cc, pid, pd)
        self.sock.sendto(artrdm_build(self.universe, pkt), (self.node, ARTNET_PORT))
        for data in self._recv_until(time.time() + self.timeout) or ():
            r = artrdm_parse(data)
            if r is None or r[0] != self.universe:
                continue
            p = match_response(rdm_find(r[1]), uid, cc, pid, self.tn)
            if p is not None and p.dest == HOST_UID:
                return p
        return None

    def tod(self, wait=2.0):
        self.sock.sendto(arttodrequest_build(self.universe), (self.node, ARTNET_PORT))
        uids = []
        for data in self._recv_until(time.time() + wait) or ():
            r = arttoddata_parse(data)
            if r is not None and r[0] == self.universe:
                uids += [u for u in r[3] if u not in uids]
        return uids

    def rescue(self, uid, mode="relay"):
        raise FlashError("--rescue needs the ESP32")


# ---------------------------------------------------------------- RDM client

class Rdm:
    def __init__(self, transport, sleep=time.sleep, timer_wait=0.03):
        self.t, self.sleep, self.timer_wait = transport, sleep, timer_wait
        self.buffer_full = 0                       # BUFFER_FULL retries (summary)

    def call(self, uid, cc, pid, pd=b"", tries=3, timer_tries=100):
        """-> ACK response. Retries on no answer (tries) and on busy (timer_tries):
        NACK BUFFER_FULL from tube_boot_rdm (request dropped while a page is written)
        or ACK_TIMER (not used by our firmware, generic). Other NACK -> RdmNack."""
        lost = timers = 0
        while True:
            r = self.t.transact(uid, cc, pid, bytes(pd))
            if r is None:
                lost += 1
                if lost >= tries:
                    raise NoAnswer(f"PID 0x{pid:04X}: no answer from {uid_str(uid)} after {tries} tries")
                continue
            if r.rt == RT_ACK:
                return r
            if r.rt == RT_NACK:
                reason = int.from_bytes(r.pd[:2], "big") if len(r.pd) >= 2 else -1
                if reason != NR_BUFFER_FULL:
                    raise RdmNack(pid, reason)
                timers += 1
                self.buffer_full += 1
                if timers >= timer_tries:
                    raise FlashError(f"PID 0x{pid:04X}: still busy (BUFFER_FULL) after {timers} tries")
                self.sleep(self.timer_wait)                # page write ~9 ms
                continue
            if r.rt == RT_ACK_TIMER:
                timers += 1
                if timers >= timer_tries:
                    raise FlashError(f"PID 0x{pid:04X}: still busy (ACK_TIMER) after {timers} tries")
                est = int.from_bytes(r.pd[:2], "big") if len(r.pd) >= 2 else 1
                self.sleep(min(self.timer_wait, est * 0.1))   # page write ~9 ms, CRC ~15 ms
                continue
            raise FlashError(f"PID 0x{pid:04X}: unexpected response type {r.rt}")

    def get(self, uid, pid, pd=b"", **kw):
        return self.call(uid, CC_GET, pid, pd, **kw)

    def set(self, uid, pid, pd=b"", **kw):
        return self.call(uid, CC_SET, pid, pd, **kw)


# ---------------------------------------------------------------- image

def version_str(v):
    """FW_VERSION_ID 0x00MMmmpp -> 'M.m.p' (bootloader IDs 0xB0.. as hex)."""
    return f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}" if v >> 24 == 0 else f"0x{v:08X}"


@dataclasses.dataclass
class Trailer:
    magic: int
    format: int
    manufacturer: int
    model: int
    version: int
    length: int
    crc: int


def trailer_parse(img):
    """16-byte trailer at 0x17F0 of a 0..0x17FF image (boot_shared.h)."""
    t = bytes(img[TRAILER_START:BOOT_START])
    u16 = lambda o: int.from_bytes(t[o:o + 2], "little")
    return Trailer(u16(0), t[2], u16(4), u16(6), int.from_bytes(t[8:12], "little"), u16(12), u16(14))


def trailer_crc(img, length):
    """CRC16 over app 0..length-1, continued over the metadata 0x17F0..0x17FB."""
    return crc16(img[TRAILER_START:TRAILER_START + META_LEN], crc16(img[:length]))


def trailer_check(img, manufacturer=None, model=None):
    """Exactly the bootloader's FINISH check (tube_boot_rdm app_check) -> ERR_* code.
    manufacturer/model None: not checked (image file without a device)."""
    t = trailer_parse(img)
    if t.magic != TRAILER_MAGIC or t.format != TRAILER_FORMAT:
        return ERR_FORMAT
    if (manufacturer is not None and t.manufacturer != manufacturer) or (model is not None and t.model != model):
        return ERR_MODEL
    if not 0 < t.length <= APP_MAX or trailer_crc(img, t.length) != t.crc:
        return ERR_INVALID
    return ERR_NONE


@dataclasses.dataclass
class Image:
    img: bytearray          # 0x0000..0x17FF, 0xFF where the hex has no data
    trailer: Trailer

    @property
    def length(self):
        return self.trailer.length


def read_image(app_hex):
    """Finished app image (tube_fw-<version>.hex: app + complete trailer) -> Image.
    The trailer is checked, never computed here."""
    mem = read_ihex(app_hex)
    if not mem:
        raise FlashError(f"{app_hex}: no data")
    if min(mem) < 0 or max(mem) >= BOOT_START:
        raise FlashError(f"{app_hex}: data at 0x{max(mem):04X} - an app image ends at 0x{BOOT_START - 1:04X} "
                         "(the 2 KB bootloader is merged only by --hex-out --boot)")
    img = bytearray([0xFF] * BOOT_START)
    for a, b in mem.items():
        img[a] = b
    err = trailer_check(img)
    t = trailer_parse(img)
    if err == ERR_FORMAT:
        raise FlashError(f"{app_hex}: no trailer (magic 0x{t.magic:04X}, format {t.format} at 0x{TRAILER_START:04X}) - "
                         "use the image of the tube_fw build, .pio/build/isp/tube_fw-<version>.hex, not firmware.hex")
    if err != ERR_NONE and (t.length, t.crc) == (0xFFFF, 0xFFFF):
        raise FlashError(f"{app_hex}: trailer metadata but no length/CRC - this is the linker output; use "
                         ".pio/build/isp/tube_fw-<version>.hex (written by the tube_fw post-build step)")
    if err != ERR_NONE:
        raise FlashError(f"{app_hex}: trailer length/CRC wrong (length {t.length}, crc 0x{t.crc:04X}, "
                         f"computed 0x{trailer_crc(img, min(t.length, APP_MAX)):04X}) - image damaged or edited")
    stray = [a for a in mem if t.length <= a < TRAILER_START]
    if stray:
        raise FlashError(f"{app_hex}: data at 0x{stray[0]:04X} behind the app length {t.length} (not covered by the CRC)")
    return Image(img, t)


def image_info(im):
    t = im.trailer
    return (f"fw {version_str(t.version)} (0x{t.version:08X}), manufacturer 0x{t.manufacturer:04X}, "
            f"model 0x{t.model:04X}, {t.length} B (max {APP_MAX}), crc 0x{t.crc:04X}")


def hex_out(app_hex, out, boot_hex=None):
    im = read_image(app_hex)
    mem = {a: b for a, b in enumerate(im.img) if a < im.length or a >= TRAILER_START}
    boot = {}
    if boot_hex:
        boot = read_ihex(boot_hex)
        if not boot or min(boot) != BOOT_START or max(boot) > 0x1FFF:
            raise FlashError(f"{boot_hex}: not a 2 KB bootloader starting at 0x1800 (tube_boot_rdm) "
                             "- the old 1 KB tube_boot (0x1C00) belongs to flash_dmx.py")
        mem.update(boot)
    write_ihex(out, mem)
    print(f"{out}: {image_info(im)}" + (f", + bootloader {len(boot)} B" if boot_hex else ""))


def page_plan(img, length, skip_blank=True):
    """Trailer page first with the whole 16-byte trailer erased (magic 0xFFFF: the
    bootloader rejects the app as long as we write - a half-written image never passes
    FINISH or the boot check), then the app pages, the trailer page with the real
    trailer last (one page write: app tail + complete trailer at once)."""
    blank = bytes([0xFF] * PAGE)
    pages = [a for a in range(0, length, PAGE)
             if a != LAST_PAGE and not (skip_blank and bytes(img[a:a + PAGE]) == blank)]
    return ([(LAST_PAGE, bytes(img[LAST_PAGE:TRAILER_START]) + b"\xFF" * (BOOT_START - TRAILER_START))] +
            [(a, bytes(img[a:a + PAGE])) for a in pages] +
            [(LAST_PAGE, bytes(img[LAST_PAGE:BOOT_START]))])


# ---------------------------------------------------------------- update flow

def boot_info_parse(pl):
    """BOOT_INFO answer (rdm_boot.h), big-endian:
    0 version, 1 page size, 2-3 app space end, 4 busy, 5 last error, 6-7 manufacturer ID,
    8-9 model ID, 10-13 bootloader sw version, 14 installed app trailer valid, 15-18 its FW_VERSION_ID"""
    if len(pl) < 6:
        raise FlashError(f"BOOT_INFO: short answer {pl.hex()}")
    if pl[0] < BOOT_PROTOCOL or len(pl) < 19:
        raise FlashError(f"tube_boot_rdm v{pl[0]} ({len(pl)}-byte BOOT_INFO) does not know the trailer of this "
                         f"image - its FINISH would reject it. Install tube_boot_rdm v{BOOT_VERSION} once via ISP "
                         "(README 'Einmalig per ISP')")
    be = lambda o, n: int.from_bytes(pl[o:o + n], "big")
    return {"version": pl[0], "page": pl[1], "app_max": be(2, 2), "busy": pl[4], "error": pl[5],
            "manufacturer": be(6, 2), "model": be(8, 2), "boot_sw": be(10, 4), "app_valid": pl[14],
            "fw_version": be(15, 4)}


def boot_info(rdm, uid, **kw):
    return boot_info_parse(rdm.get(uid, PID_BOOT_INFO, **kw).pd)


def check_ids(im, manufacturer, model, where):
    t = im.trailer
    if (manufacturer, model) != (t.manufacturer, t.model):
        raise FlashError(f"wrong device: {where} reports manufacturer 0x{manufacturer:04X} model 0x{model:04X}, "
                         f"image is for manufacturer 0x{t.manufacturer:04X} model 0x{t.model:04X} - nothing written")


def check_version(im, installed, allow_downgrade, where):
    new = im.trailer.version
    print(f"version {version_str(installed) if installed is not None else 'unknown'} -> {version_str(new)}"
          + (f" ({where})" if installed is not None else ""))
    if installed is not None and new < installed and not allow_downgrade:
        raise FlashError(f"downgrade {version_str(installed)} -> {version_str(new)} - use --allow-downgrade; nothing written")


def app_precheck(rdm, uid, im, allow_downgrade):
    """App running? DEVICE_INFO: model + version (manufacturer = UID). Done before ENTER_BOOTLOADER,
    so a wrong image never even stops the app. -> True if the app answered."""
    try:
        di = rdm.get(uid, PID_DEVICE_INFO, tries=2).pd
    except FlashError:
        return False                               # no answer / NACK: let the bootloader steps decide
    if len(di) < 10:
        return False
    sw = int.from_bytes(di[6:10], "big")
    if sw >> 24 == BOOT_SW_VERSION >> 24:          # already in tube_boot_rdm
        return False
    check_ids(im, int.from_bytes(uid[:2], "big"), int.from_bytes(di[2:4], "big"), "DEVICE_INFO (app)")
    check_version(im, sw, allow_downgrade, "installed app")
    return True


def enter_bootloader(rdm, uid, rescue, sleep=time.sleep):
    if rescue:
        mode = rescue if isinstance(rescue, str) else "relay"
        print(f"rescue ({mode}): BOOT_INFO spam during the boot window ...")
        if rdm.t.rescue(uid, mode) is None:
            raise FlashError(f"rescue ({mode}): no BOOT_INFO answer "
                             + ("within 60 s - 24 V not replugged? " if mode == "manual" else "in the boot window ")
                             + "(tube_boot_rdm installed? hfuse 0x88? UID right?)")
    else:
        print(f"RDM SET 0x{PID_ENTER_BOOTLOADER:04X} 'BOOT' -> {uid_str(uid)}")
        try:
            rdm.set(uid, PID_ENTER_BOOTLOADER, b"BOOT", tries=2)
            print("  ACK, app resets into the bootloader")
        except RdmNack as e:
            print(f"  {e} - bootloader already active?")
        except NoAnswer:
            print("  no answer - trying BOOT_INFO anyway (bootloader already active?)")
        sleep(0.5)
    for attempt in range(10):
        try:
            info = boot_info(rdm, uid, tries=2)      # read clears a stale last error
            break
        except NoAnswer:
            sleep(0.3)
    else:
        raise FlashError(f"no BOOT_INFO answer from {uid_str(uid)} - not in tube_boot_rdm "
                         "(old 1 KB tube_boot -> flash_dmx.py; app dead -> --rescue)")
    print(f"tube_boot_rdm v{info['version']} (0x{info['boot_sw']:08X}), page {info['page']} B, "
          f"app space 0x{info['app_max']:04X}, manufacturer 0x{info['manufacturer']:04X} model 0x{info['model']:04X}, "
          f"installed app " + (version_str(info["fw_version"]) if info["app_valid"] else "invalid"))
    if info["page"] != PAGE or info["app_max"] != BOOT_START:
        raise FlashError("bootloader geometry does not match this script")
    return info


def wait_idle(rdm, uid, sleep=time.sleep):
    for _ in range(100):
        info = boot_info(rdm, uid)
        if not info["busy"]:
            return info
        sleep(0.02)
    raise FlashError("bootloader stays busy")


def write_pages(rdm, uid, plan):
    t0 = time.time()
    for i, (a, data) in enumerate(plan, 1):
        try:
            rdm.set(uid, PID_WRITE_PAGE, a.to_bytes(2, "big") + data, tries=5)
        except FlashError as e:
            print()
            raise FlashError(f"WRITE_PAGE 0x{a:04X}: {e} - bootloader stays active, run again") from e
        print(f"\r  page 0x{a:04X}  {i}/{len(plan)}", end="", flush=True)
    print(f"  ({time.time() - t0:.1f} s)")


def device_crc(rdm, uid, addr, n):
    pl = rdm.get(uid, PID_APP_CRC, addr.to_bytes(2, "big") + n.to_bytes(2, "big")).pd
    if len(pl) < 2:
        raise FlashError(f"APP_CRC: short answer {pl.hex()}")
    return (pl[0] << 8) | pl[1]


def bad_chunks(rdm, uid, img, length):
    """APP_CRC chunk by chunk (E1.20 2 ms limit) over the app + trailer -> differing chunks."""
    chunks = [(a, min(CRC_CHUNK, length - a)) for a in range(0, length, CRC_CHUNK)] + \
        [(TRAILER_START, BOOT_START - TRAILER_START)]
    return [(a, n) for a, n in chunks if device_crc(rdm, uid, a, n) != crc16(img[a:a + n])]


def flash(rdm, uid, app_hex, rescue=False, sleep=time.sleep, allow_downgrade=False):
    im = read_image(app_hex)
    img, length, crc = im.img, im.length, im.trailer.crc
    print(f"{app_hex}: {image_info(im)}")
    checked = False if rescue else app_precheck(rdm, uid, im, allow_downgrade)
    info = enter_bootloader(rdm, uid, rescue, sleep)
    try:                                           # the bootloader's own IDs, always
        check_ids(im, info["manufacturer"], info["model"], "BOOT_INFO (bootloader)")
        if not checked:
            check_version(im, info["fw_version"] if info["app_valid"] else None, allow_downgrade, "BOOT_INFO trailer")
    except FlashError as e:
        raise FlashError(f"{e} (the bootloader starts its app after ~60 s if that one is valid)") from e

    plan = page_plan(img, length, skip_blank=True)
    print(f"writing {len(plan)} pages (all-0xFF pages skipped)")
    write_pages(rdm, uid, plan)
    # check chunk CRCs; differing chunks (old content in a skipped page, verify
    # error) are written once more, all their pages
    for attempt in (1, 2):
        info = wait_idle(rdm, uid, sleep)
        bad = bad_chunks(rdm, uid, img, length)
        if info["error"] == 0 and not bad:
            print(f"CRC ok (0x{crc:04X}, {length} B)")
            break
        why = (BOOT_ERRORS.get(info["error"], info["error"]) + "; " if info["error"] else "") + \
            f"{len(bad)} chunk(s) differ: " + ", ".join(f"0x{a:04X}" for a, _ in bad)
        if attempt == 2:
            raise FlashError(f"{why} - bootloader stays active, run again")
        pages = sorted({p - p % PAGE for a, n in bad for p in range(a, a + n)}) if bad else \
            [a for a, _ in page_plan(img, length, skip_blank=False)[1:]]
        print(f"  {why} - rewriting {len(pages)} page(s)")
        write_pages(rdm, uid, [(a, bytes(img[a:a + PAGE])) for a in pages])

    rdm.set(uid, PID_FINISH)
    print("FINISH ok, app starts (self-test ~2 s)")
    sleep(3)
    try:
        label = rdm.get(uid, PID_SOFTWARE_VERSION_LABEL, tries=5).pd.decode(errors="replace")
    except NoAnswer as e:
        raise FlashError("no answer after FINISH - app does not run?") from e
    if label.startswith("tube_boot_rdm"):
        info = boot_info(rdm, uid)
        raise FlashError(f"still in the bootloader ({label}), last error: {BOOT_ERRORS.get(info['error'], info['error'])}")
    di = rdm.get(uid, PID_DEVICE_INFO).pd
    sw = int.from_bytes(di[6:10], "big") if len(di) >= 10 else -1
    if sw != im.trailer.version:
        raise FlashError(f"app runs ('{label}') but DEVICE_INFO reports 0x{sw:08X}, image 0x{im.trailer.version:08X}")
    print(f"done: app runs - '{label}', DEVICE_INFO sw 0x{sw:08X} ({version_str(sw)})")


# ---------------------------------------------------------------- selftest

class FakeTube:
    """Simulated tube_boot_rdm v2/v3 (+ app) with the firmware's semantics: busy for a few
    requests after WRITE_PAGE (NACK BUFFER_FULL, request dropped), chunked APP_CRC, lost
    answers, trailer check (magic/format, manufacturer/model, length, CRC) on FINISH,
    BOOT_INFO with the cached state of the installed app (WRITE_PAGE clears 'valid')."""

    def __init__(self, uid, lose_every=0, garbage_page=None, model=1, app_version=0x301, mode="app"):
        self.uid, self.flash, self.mode = uid, bytearray([0xFF] * BOOT_START), mode
        self.manufacturer, self.model = int.from_bytes(uid[:2], "big"), model
        self.app_version = app_version          # what the running (old) app reports in DEVICE_INFO
        self.busy, self.err, self.n, self.lose_every = 0, 0, 0, lose_every
        self.writes, self.timers = [], 0
        if garbage_page is not None:            # old content in a page the new image leaves blank
            self.flash[garbage_page:garbage_page + PAGE] = bytes(range(PAGE))
        self.snapshot()

    def check(self):
        return trailer_check(self.flash, self.manufacturer, self.model)

    def valid(self):
        return self.check() == ERR_NONE

    def snapshot(self):                         # bootloader app_check(): app_st[]
        e = self.check()
        self.app_st = bytes([e == ERR_NONE]) + trailer_parse(self.flash).version.to_bytes(4, "big")
        return e

    def transact(self, uid, cc, pid, pd=b""):
        self.n += 1
        if uid != self.uid or (self.lose_every and self.n % self.lose_every == 0):
            return None
        rt, out = RT_ACK, b""

        def nack(reason):
            return RT_NACK, reason.to_bytes(2, "big")

        if self.busy:
            self.busy -= 1
        if self.mode == "app":
            if cc == CC_SET and pid == PID_ENTER_BOOTLOADER and pd == b"BOOT":
                self.mode = "boot"
                self.snapshot()
            elif cc == CC_GET and pid == PID_SOFTWARE_VERSION_LABEL:
                out = f"tube_fw {version_str(self.app_version)}".encode()
            elif cc == CC_GET and pid == PID_DEVICE_INFO:
                out = (bytes([1, 0]) + self.model.to_bytes(2, "big") + bytes([1, 1]) +
                       self.app_version.to_bytes(4, "big") + bytes.fromhex("000c010400010000000000"))[:19]
            else:
                rt, out = nack(0)
        elif cc == CC_GET and pid == PID_BOOT_INFO:
            out = (bytes([BOOT_VERSION, PAGE]) + BOOT_START.to_bytes(2, "big") + bytes([self.busy > 0, self.err]) +
                   self.manufacturer.to_bytes(2, "big") + self.model.to_bytes(2, "big") +
                   BOOT_SW_VERSION.to_bytes(4, "big") + self.app_st)
            self.err = 0
        elif cc == CC_GET and pid == PID_DEVICE_INFO:
            out = (bytes([1, 0]) + self.model.to_bytes(2, "big") + bytes([1, 1]) +
                   BOOT_SW_VERSION.to_bytes(4, "big") + bytes([0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0]))
        elif cc == CC_SET and pid == PID_WRITE_PAGE:
            a = int.from_bytes(pd[:2], "big")
            if len(pd) != 2 + PAGE:
                rt, out = nack(1)
            elif a >= BOOT_START or a % PAGE:
                rt, out = nack(6)
            elif self.busy:
                rt, out = nack(NR_BUFFER_FULL)
                self.timers += 1
            else:
                self.flash[a:a + PAGE] = pd[2:]
                self.writes.append(a)
                self.app_st = b"\x00" + self.app_st[1:]
                self.busy = 2
        elif cc == CC_GET and pid == PID_APP_CRC:
            a, n = int.from_bytes(pd[:2], "big"), int.from_bytes(pd[2:4], "big")
            if len(pd) != 4:
                rt, out = nack(1)
            elif n > CRC_CHUNK or a + n > BOOT_START:
                rt, out = nack(6)
            elif self.busy:
                rt, out = nack(NR_BUFFER_FULL)
            else:
                out = crc16(self.flash[a:a + n]).to_bytes(2, "big")
        elif cc == CC_SET and pid == PID_FINISH:
            if self.busy:
                rt, out = nack(NR_BUFFER_FULL)
            else:
                self.err = self.snapshot()
                if self.err == ERR_NONE:
                    self.mode, self.app_version = "app", trailer_parse(self.flash).version
        elif cc == CC_GET and pid == PID_SOFTWARE_VERSION_LABEL:
            out = f"tube_boot_rdm {BOOT_VERSION}".encode()
        else:
            rt, out = nack(0)
        resp = rdm_build(HOST_UID, self.uid, 0, cc + 1, pid, out, port=rt)
        return rdm_parse(resp)

    rescue = None


def make_test_hex(path, app, manufacturer=0x7FF0, model=1, version=0x400, fmt=None, magic=None, edit=None):
    """Test image like the tube_fw post-build step writes it (app + complete trailer).
    edit(img): change bytes after the CRC was computed."""
    img = bytearray([0xFF] * BOOT_START)
    for a, b in app.items():
        img[a] = b
    length = (max(app) + 2) & ~1
    img[TRAILER_START:TRAILER_START + META_LEN] = (
        (TRAILER_MAGIC if magic is None else magic).to_bytes(2, "little") +
        bytes([TRAILER_FORMAT if fmt is None else fmt, 0xFF]) + manufacturer.to_bytes(2, "little") +
        model.to_bytes(2, "little") + version.to_bytes(4, "little"))
    img[TRAILER:BOOT_START] = length.to_bytes(2, "little") + trailer_crc(img, length).to_bytes(2, "little")
    if edit:
        edit(img)
    write_ihex(path, {a: img[a] for a in range(BOOT_START) if a < length or a >= TRAILER_START})
    return img, length


def selftest():
    ok = fails = 0

    def check(name, cond):
        nonlocal ok, fails
        if cond:
            ok += 1
        else:
            fails += 1
            print(f"FAIL {name}")

    # header values
    check("pids", (PID_ENTER_BOOTLOADER, PID_BOOT_INFO, PID_WRITE_PAGE, PID_APP_CRC, PID_FINISH) ==
          (0x8000, 0x8010, 0x8011, 0x8012, 0x8013))
    check("boot sw version", BOOT_SW_VERSION == 0xB0000003 and BOOT_VERSION == 3 and BOOT_PROTOCOL == 2
          and PAGE == 64 and CRC_CHUNK == 256)
    check("boot_shared magic", S["BOOT_MAGIC_VALUE"] == 0xB007)
    check("trailer contract", (TRAILER_MAGIC, TRAILER_FORMAT, META_LEN, TRAILER_START, TRAILER, APP_MAX, LAST_PAGE) ==
          (0x4D50, 1, 12, 0x17F0, 0x17FC, 6128, 0x17C0))
    check("error codes", (ERR_NONE, ERR_VERIFY, ERR_INVALID, ERR_MODEL, ERR_FORMAT) == (0, 1, 2, 3, 4))
    cfg = read_defines(INCLUDE / "config.h")
    check("config ids", (cfg["RDM_MANUFACTURER_ID"], cfg["RDM_DEVICE_MODEL_ID"]) == (0x7FF0, 0x0001))

    # request PDLs the bootloader expects (pids[] table in tube_boot_rdm) = what we send
    boot_c = (TUBE / "boot_rdm/src/main.c").read_text()
    for name, cc, pdl in (("PID_BOOT_INFO", "CC_GET", "0"), ("PID_BOOT_WRITE_PAGE", "CC_SET", "WRITE_PAGE_PDL"),
                          ("PID_BOOT_APP_CRC", "CC_GET", "4"), ("PID_BOOT_FINISH", "CC_SET", "0"),
                          ("PID_IDENTIFY_DEVICE", "CC_SET", "1")):
        check(f"bootloader table {name}", re.search(r"\{\s*%s,\s*%s,\s*%s\s*\}" % (name, cc, pdl), boot_c) is not None)
    check("bootloader no ACK_TIMER", "RT_ACK_TIMER" not in boot_c)
    check("bootloader BOOT_INFO 14 + 5 B", re.search(r"boot_info_c\[14\] PROGMEM", boot_c) is not None and
          re.search(r"static uint8_t app_st\[5\];", boot_c) is not None)

    # BOOT_INFO v2/v3 (19 B) parse; v1 (6 B) refused
    bi = boot_info_parse(bytes.fromhex("0240180000037FF00001B0000002010000 0400".replace(" ", "")))
    check("boot_info parse", bi == {"version": 2, "page": 64, "app_max": 0x1800, "busy": 0, "error": 3,
                                    "manufacturer": 0x7FF0, "model": 1, "boot_sw": 0xB0000002, "app_valid": 1,
                                    "fw_version": 0x400})
    try:
        boot_info_parse(bytes.fromhex("014018000000"))
        check("boot_info v1 refused", False)
    except FlashError:
        check("boot_info v1 refused", True)

    # RDM build: exact bytes captured from the ESP32 tool (logs/2026-10-23.log)
    tx = rdm_build(bytes.fromhex("7FF0B06675AA"), bytes.fromhex("7FF000000001"), 0x0E, CC_SET, 0x8000, b"BOOT")
    check("build vs captured TX", tx == bytes.fromhex("CC011C7FF0B06675AA7FF0000000010E010000003080000442" "4F4F5407F4"))
    # parse captured answers (leading 0x00 = break byte as seen by the ESP32)
    rx = rdm_find(bytes.fromhex("00CC01187FF0000000017FF0B06675AA0E000000003180000006B8"))
    check("parse captured ACK", len(rx) == 1 and rx[0].rt == RT_ACK and rx[0].pid == 0x8000 and rx[0].cc == 0x31)
    rx = rdm_find(bytes.fromhex("00CC012B7FF0000000017FF0B06675AA0500000000210060130100000101010000"
                                "0100000C0104000100000006BC"))
    check("parse captured DEVICE_INFO", len(rx) == 1 and int.from_bytes(rx[0].pd[6:10], "big") == 0x100)
    rx = rdm_find(bytes.fromhex("00CC01257FF0000000017FF0B06675AA08000000002100C00D747562655F6677"
                                "20302E312E300AF5"))
    check("parse captured label", len(rx) == 1 and rx[0].pd == b"tube_fw 0.1.0")
    bad = bytearray(tx)
    bad[-1] ^= 1
    check("checksum rejects", rdm_find(bytes(bad)) == [])
    check("length rejects", rdm_find(tx[:-1]) == [])
    p = rdm_parse(rdm_build(bytes(6), HOST_UID, 7, CC_GET, PID_APP_CRC, b"\x12\x34", subdev=3))
    check("round trip", (p.tn, p.cc, p.pid, p.pd, p.subdev, p.src) == (7, CC_GET, PID_APP_CRC, b"\x12\x34", 3, HOST_UID))
    check("tool RX line rdm", lines_rx_bytes(["TX: CC", "RX 3 bytes: 00 CC 01 "]) == bytes.fromhex("00CC01"))
    check("tool RX line blr", lines_rx_bytes(["tries 4", "RX: 00CC01"]) == bytes.fromhex("00CC01") and
          lines_rx_bytes(["RX: -"]) == b"")
    write_page = rdm_build(bytes(6), HOST_UID, 1, CC_SET, PID_WRITE_PAGE, bytes(2 + PAGE))
    rx_max = int(re.search(r"#define RX_MAX_PD (\d+)", boot_c).group(1))
    check("WRITE_PAGE fits bootloader RX buffer", len(write_page) == 26 + 66 <= 26 + rx_max)
    check("bootloader WRITE_PAGE PDL = 2 + page", re.search(r"#define WRITE_PAGE_PDL \(2 \+ SPM_PAGESIZE\)", boot_c) is not None)

    # DUB: firmware computes the checksum as sum(uid) + 6 * 0xFF
    for uid in (bytes.fromhex("7FF0B06675AA"), bytes.fromhex("7FF0FFFFFFFF"), bytes(6)):
        enc = dub_encode(uid)
        fw_sum = (sum(uid) + 6 * 0xFF) & 0xFFFF
        check("dub checksum shortcut", int.from_bytes(bytes([enc[20] & enc[21], enc[22] & enc[23]]), "big") == fw_sum)
        check("dub decode", dub_decode(enc) == uid and len(enc) == 24)

    # ArtRdm / ArtTodData
    a = artrdm_build(0x123, tx)
    check("artrdm header", a[:8] == ART_ID and a[8:10] == b"\x00\x83" and a[10:12] == b"\x00\x0e" and a[12] == 1
          and a[21] == 0x01 and a[22] == 0 and a[23] == 0x23 and len(a) == 24 + len(tx) - 1)
    check("artrdm without start code", a[24] == SC_SUB)
    u, r = artrdm_parse(a)
    check("artrdm parse", u == 0x123 and r == tx)
    t = arttodrequest_build(5)
    check("arttodrequest", len(t) == 56 and t[8:10] == b"\x00\x80" and t[21] == 0 and t[22] == 0 and t[23] == 1 and t[24] == 5)
    td = art_header(OP_TODDATA) + bytes([1, 1]) + bytes(6) + bytes([0, 0, 0, 5, 0, 2, 0, 2]) + \
        bytes.fromhex("7FF0B06675AA7FF012345678")
    check("arttoddata", arttoddata_parse(td) == (5, 2, 0, [bytes.fromhex("7FF0B06675AA"), bytes.fromhex("7FF012345678")]))

    # HEX / trailer
    with tempfile.TemporaryDirectory() as d:
        app = {i: (i * 7) & 0xFF for i in range(0, 1001)}
        app.update({i: 0xFF for i in range(1001, 1200)})          # an all-0xFF page inside the image
        app.update({i: 0x5A for i in range(1200, 1301)})
        ref, length = make_test_hex(f"{d}/app.hex", app)
        im = read_image(f"{d}/app.hex")
        img, crc = im.img, im.trailer.crc
        check("length even", length == im.length == 1302)
        check("trailer fields", (im.trailer.magic, im.trailer.format, im.trailer.manufacturer, im.trailer.model,
                                 im.trailer.version) == (0x4D50, 1, 0x7FF0, 1, 0x400) and img[TRAILER_START + 3] == 0xFF)
        check("crc over app + metadata", crc == crc16(img[TRAILER_START:TRAILER_START + 12], crc16(img[:length]))
              and crc != crc16(img[:length]) and img[length] == 0xFF and img[1301] == 0xFF)
        check("trailer_check ok", trailer_check(img, 0x7FF0, 1) == ERR_NONE)
        check("trailer_check wrong model", trailer_check(img, 0x7FF0, 2) == ERR_MODEL)
        check("trailer_check wrong manufacturer", trailer_check(img, 0x1234, 1) == ERR_MODEL)
        bad = bytearray(img)
        bad[TRAILER_START + 2] = 2                                  # format 2
        check("trailer_check wrong format", trailer_check(bad, 0x7FF0, 1) == ERR_FORMAT)
        bad = bytearray(img)
        bad[TRAILER_START] = 0x00                                   # magic
        check("trailer_check wrong magic", trailer_check(bad, 0x7FF0, 1) == ERR_FORMAT)
        check("trailer_check blank", trailer_check(bytearray([0xFF] * BOOT_START), 0x7FF0, 1) == ERR_FORMAT)
        bad = bytearray(img)
        bad[TRAILER_START + 8] ^= 1                                 # version edited, CRC kept
        check("trailer_check metadata in crc", trailer_check(bad, 0x7FF0, 1) == ERR_INVALID)
        bad = bytearray(img)
        bad[TRAILER_START + 3] = 0x00                               # reserved: in the CRC, not compared
        check("trailer_check reserved in crc", trailer_check(bad, 0x7FF0, 1) == ERR_INVALID)
        bad = bytearray(img)
        bad[TRAILER:TRAILER + 2] = (APP_MAX + 2).to_bytes(2, "little")
        check("trailer_check length limit", trailer_check(bad, 0x7FF0, 1) == ERR_INVALID)
        for name, kw in (("wrong format", {"fmt": 2}), ("wrong magic", {"magic": 0x1234}),
                         ("metadata changed after crc", {"edit": lambda m: m.__setitem__(TRAILER_START + 9, 0x05)}),
                         ("app changed after crc", {"edit": lambda m: m.__setitem__(5, 0x00)}),
                         ("no length/crc (firmware.hex)", {"edit": lambda m: m.__setitem__(slice(TRAILER, BOOT_START), b"\xFF" * 4)})):
            make_test_hex(f"{d}/bad.hex", app, **kw)
            try:
                read_image(f"{d}/bad.hex")
                check(f"read_image rejects {name}", False)
            except FlashError:
                check(f"read_image rejects {name}", True)
        write_ihex(f"{d}/stray.hex", {**{a: ref[a] for a in range(length)}, 1400: 0,
                                      **{a: ref[a] for a in range(TRAILER_START, BOOT_START)}})
        try:
            read_image(f"{d}/stray.hex")
            check("read_image rejects data behind length", False)
        except FlashError:
            check("read_image rejects data behind length", True)

        plan = page_plan(img, length)
        check("plan trailer first/last", plan[0][0] == LAST_PAGE == plan[-1][0] == 0x17C0 and plan[0][1][-16:] == b"\xFF" * 16
              and plan[-1][1][-16:] == img[TRAILER_START:] and all(len(p[1]) == PAGE for p in plan))
        check("plan skips blank page", 1088 not in [p[0] for p in plan] and 1088 in [p[0] for p in page_plan(img, length, False)])
        # a half-written image never passes the bootloader check (old valid app installed first);
        # with blank pages skipped, stale old content there is caught by the CRC (-> second pass)
        old, _ = make_test_hex(f"{d}/old.hex", {i: 0x11 for i in range(3000)}, version=0x301)
        for skip in (False, True):
            tube = FakeTube(bytes.fromhex("7FF0B06675AA"))
            tube.flash[:] = old
            states = [tube.check()]
            for a, data in page_plan(img, length, skip_blank=skip):
                tube.flash[a:a + PAGE] = data
                states.append(tube.check())
            check(f"half-written never valid (skip_blank={skip})", states[0] == ERR_NONE and states[1] == ERR_FORMAT
                  and all(e == ERR_FORMAT for e in states[1:-1]) and states[-1] == (ERR_INVALID if skip else ERR_NONE))
        make_test_hex(f"{d}/big.hex", {i: 0 for i in range(APP_MAX + 1)})
        try:
            read_image(f"{d}/big.hex")
            check("limit", False)
        except FlashError:
            check("limit", True)
        write_ihex(f"{d}/boot.hex", {BOOT_START + i: i & 0xFF for i in range(100)})
        out, sys.stdout = sys.stdout, open(os.devnull, "w")
        try:
            hex_out(f"{d}/app.hex", f"{d}/merged.hex", f"{d}/boot.hex")
        finally:
            sys.stdout = out
        m = read_ihex(f"{d}/merged.hex")
        check("merged", m[BOOT_START + 5] == 5 and all(m[a] == img[a] for a in range(TRAILER_START, BOOT_START))
              and m[0] == 0 and 1400 not in m and max(a for a in m if a < BOOT_START) == BOOT_START - 1)
        write_ihex(f"{d}/oldboot.hex", {0x1C00 + i: 1 for i in range(10)})
        try:
            hex_out(f"{d}/app.hex", f"{d}/x.hex", f"{d}/oldboot.hex")
            check("old 1 KB bootloader rejected", False)
        except FlashError:
            check("old 1 KB bootloader rejected", True)

        # full flow against the simulated bootloader (lost answers, BUFFER_FULL, second pass)
        uid = bytes.fromhex("7FF0B06675AA")
        quiet = open(os.devnull, "w")

        def run(tube, path=f"{d}/app.hex", **kw):
            out, sys.stdout = sys.stdout, quiet
            try:
                flash(Rdm(tube, sleep=lambda s: None), tube.uid, path, sleep=lambda s: None, **kw)
                return None
            except FlashError as e:
                return e
            finally:
                sys.stdout = out

        for lose, garbage in ((0, None), (7, None), (0, 1088)):
            tube = FakeTube(uid, lose_every=lose, garbage_page=garbage)
            res = run(tube)
            check(f"flow lose={lose} garbage={garbage}: {res}", res is None and tube.mode == "app" and tube.valid()
                  and tube.flash[:length] == img[:length] and tube.app_version == 0x400)
            if garbage is not None:
                check("second pass wrote the blank page", tube.writes.count(1088) == 1)
            check("BUFFER_FULL seen", tube.timers > 0)

        # wrong model / manufacturer: refused before anything is written
        tube = FakeTube(uid, model=2)
        res = run(tube)
        check(f"wrong model (app) refused: {res}", res is not None and "wrong device" in str(res)
              and tube.writes == [] and tube.mode == "app")
        tube = FakeTube(bytes.fromhex("1234B06675AA"))
        res = run(tube)
        check(f"wrong manufacturer refused: {res}", res is not None and tube.writes == [] and tube.mode == "app")
        tube = FakeTube(uid, model=2, mode="boot")                  # already in the bootloader (no app)
        res = run(tube)
        check(f"wrong model (bootloader) refused: {res}", res is not None and "BOOT_INFO" in str(res) and tube.writes == [])
        # downgrade
        tube = FakeTube(uid, app_version=0x500)
        res = run(tube)
        check(f"downgrade refused: {res}", res is not None and "downgrade" in str(res) and tube.writes == []
              and tube.mode == "app")
        tube = FakeTube(uid, app_version=0x500)
        res = run(tube, allow_downgrade=True)
        check(f"downgrade allowed: {res}", res is None and tube.app_version == 0x400)
        tube = FakeTube(uid, mode="boot")                           # bootloader: version from BOOT_INFO trailer
        newer, _ = make_test_hex(f"{d}/newer.hex", app, version=0x501)
        tube.flash[:] = newer
        tube.snapshot()
        res = run(tube)
        check(f"downgrade refused (BOOT_INFO): {res}", res is not None and "downgrade" in str(res) and tube.writes == [])

        # the bootloader's own FINISH check (images written past the host checks)
        for name, kw, err in (("wrong model", {"model": 2}, ERR_MODEL), ("wrong format", {"fmt": 3}, ERR_FORMAT),
                              ("crc over metadata", {"edit": lambda m: m.__setitem__(TRAILER_START + 10, 0x07)}, ERR_INVALID)):
            bad, blen = make_test_hex(f"{d}/x.hex", app, **kw)
            tube = FakeTube(uid, mode="boot")
            rdm = Rdm(tube, sleep=lambda s: None)
            for a, data in page_plan(bad, blen):
                rdm.set(uid, PID_WRITE_PAGE, a.to_bytes(2, "big") + data)
            rdm.set(uid, PID_FINISH)
            info = boot_info(rdm, uid)
            check(f"FINISH {name} -> error {err}", tube.mode == "boot" and info["error"] == err and not info["app_valid"])

    # real build outputs, if present
    build = TUBE / "firmware/.pio/build/isp"
    real_apps = sorted(build.glob("tube_fw-*.hex"))
    real_boot = TUBE / "boot_rdm/.pio/build/boot/firmware.hex"
    if len(real_apps) == 1 and real_boot.exists():
        im = read_image(real_apps[0])
        t = im.trailer
        boot = read_ihex(real_boot)
        ver = (cfg["FW_VERSION_MAJOR"] << 16) | (cfg["FW_VERSION_MINOR"] << 8) | cfg["FW_VERSION_PATCH"]
        check("real app trailer = config.h", (t.manufacturer, t.model, t.version) ==
              (cfg["RDM_MANUFACTURER_ID"], cfg["RDM_DEVICE_MODEL_ID"], ver)
              and real_apps[0].name == f"tube_fw-{version_str(ver)}.hex")
        fw = read_ihex(build / "firmware.hex")
        check("real app = linker output", all(im.img[a] == b for a, b in fw.items())
              and max(a for a in fw if a < TRAILER_START) < im.length)
        check("real boot 0x1800..0x1FFF", min(boot) == BOOT_START and max(boot) <= 0x1FFF)
        with tempfile.TemporaryDirectory() as d:
            out, sys.stdout = sys.stdout, open(os.devnull, "w")
            try:
                hex_out(real_apps[0], f"{d}/m.hex", real_boot)
            finally:
                sys.stdout = out
            m = read_ihex(f"{d}/m.hex")
            check("real merged", all(m[a] == b for a, b in boot.items()) and m[TRAILER + 2] == t.crc & 0xFF)
        print(f"real images: {real_apps[0].name} {image_info(im)}; bootloader {len(boot)} B (max 2048)")
    else:
        print("real images not built - skipped (pio run -d tube/firmware -e isp / pio run -d tube/boot_rdm)")

    print(f"selftest: {ok} ok, {fails} failed")
    return 1 if fails else 0


# ---------------------------------------------------------------- main

def make_transport(a):
    if a.via == "artnet":
        if not a.node:
            raise FlashError("--via artnet needs --node <ip>")
        return ArtNetTransport(a.node, a.universe, timeout=a.timeout)
    return Esp32Transport()


def main():
    ap = argparse.ArgumentParser(description="tube firmware update over RDM (tube_boot_rdm)")
    ap.add_argument("args", nargs="*", help="<uid 7FF0:XXXXXXXX> <tube_fw-<version>.hex>, or only the .hex with --hex-out")
    ap.add_argument("--via", choices=("esp32", "artnet"), default="esp32")
    ap.add_argument("--node", help="Art-Net node IP (unicast)")
    ap.add_argument("--universe", type=int, default=0, help="Art-Net 15-bit port-address (Net/SubNet/Universe), default 0")
    ap.add_argument("--timeout", type=float, default=0.6, help="Art-Net answer timeout per request [s]")
    ap.add_argument("--rescue", choices=("manual", "relay", "isp"), metavar="{manual,relay,isp}",
                    help="(esp32) catch the ~200 ms boot window: manual = replug 24 V yourself (default without value), "
                         "relay = power cycle via relay, isp = reset via Uno/avrdude")
    ap.add_argument("--tod", action="store_true", help="(artnet) show the node's table of devices")
    ap.add_argument("--allow-downgrade", action="store_true", help="flash even if the image is older than the installed app")
    ap.add_argument("--hex-out", metavar="OUT", help="no tube access: write app + trailer (+ --boot) as one Intel HEX")
    ap.add_argument("--boot", metavar="BOOT_HEX", help="with --hex-out: merge this bootloader hex (tube_boot_rdm)")
    ap.add_argument("--selftest", action="store_true", help="offline tests, no hardware")
    argv = sys.argv[1:]
    for i, x in enumerate(argv):                 # plain '--rescue' (no value) = manual
        if x == "--rescue" and (i + 1 == len(argv) or argv[i + 1] not in ("manual", "relay", "isp")):
            argv.insert(i + 1, "manual")
            break
    a = ap.parse_args(argv)
    try:
        if a.selftest:
            return selftest()
        if a.hex_out:
            if len(a.args) != 1:
                ap.error("--hex-out takes exactly one <tube_fw-<version>.hex>")
            hex_out(a.args[0], a.hex_out, a.boot)
            return 0
        if a.tod and not a.args:
            if a.via != "artnet":
                ap.error("--tod needs --via artnet")
            t = make_transport(a)
            uids = t.tod()
            print(f"TOD universe {a.universe}: " + (", ".join(uid_str(u) for u in uids) if uids else "(empty / no answer)"))
            return 0
        if len(a.args) != 2:
            ap.error("need <uid> <tube_fw-<version>.hex>")
        uid = parse_uid(a.args[0])               # fail early, before touching the tube
        read_image(a.args[1])
        if a.rescue and a.via != "esp32":
            ap.error("--rescue needs --via esp32")
        t = make_transport(a)
        if a.tod:
            uids = t.tod()
            print(f"TOD universe {a.universe}: " + (", ".join(uid_str(u) for u in uids) if uids else "(empty / no answer)"))
            if uid not in uids:
                print(f"  warning: {uid_str(uid)} not in the TOD - the node may not forward RDM to it")
        rdm = Rdm(t)
        try:
            flash(rdm, uid, a.args[1], a.rescue, allow_downgrade=a.allow_downgrade)
        finally:
            print(f"BUFFER_FULL retries: {rdm.buffer_full}")
        return 0
    except (FlashError, OSError) as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
