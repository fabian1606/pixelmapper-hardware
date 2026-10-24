"""Test helpers: images (incl. self-built test variants), ELF symbols, E1.20
checks, and a flash_rdm.py transport that talks to the emulated tube."""
import copy
import glob
import os
import pathlib
import re
import shutil
import subprocess
import sys

from avrsim import us
from rdmline import CC_DISC, CC_GET, CC_SET, CTRL_UID, Tube, dub_decode, rdm_packet, rdm_parse

PROJ = pathlib.Path(__file__).resolve().parents[1]   # tube/tools (flash_rdm.py)
TUBE = PROJ.parent                                    # tube/
FW = TUBE                                             # FW / "tube_fw" -> tube/firmware
CACHE = pathlib.Path(__file__).resolve().parent / ".build"
PIO = os.path.expanduser("~/.platformio/penv/bin/pio")
NM = os.path.expanduser("~/.platformio/packages/toolchain-atmelavr/bin/avr-nm")


def defines(path):
    return {m.group(1): int(m.group(2), 0) for m in
            re.finditer(r"^#define\s+(\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)\b", pathlib.Path(path).read_text(), re.M)}


CFG = defines(TUBE / "firmware/include/config.h")
BOOTH = defines(TUBE / "firmware/include/rdm_boot.h")
FW_VERSION = (CFG["FW_VERSION_MAJOR"] << 16) | (CFG["FW_VERSION_MINOR"] << 8) | CFG["FW_VERSION_PATCH"]
FW_LABEL = f"tube_fw {CFG['FW_VERSION_MAJOR']}.{CFG['FW_VERSION_MINOR']}.{CFG['FW_VERSION_PATCH']}".encode()
BOOT_LABEL = f"tube_boot_rdm {BOOTH['BOOT_RDM_VERSION']}".encode()

BOOT_HEX = str(TUBE / "boot_rdm/.pio/build/boot/firmware.hex")
APP_ELF = str(TUBE / "firmware/.pio/build/isp/firmware.elf")
_apps = glob.glob(str(TUBE / "firmware/.pio/build/isp/tube_fw-*.hex"))
APP_HEX = _apps[0] if len(_apps) == 1 else None

UID = bytes.fromhex("7FF0B06675AA")
BCAST = b"\xFF" * 6
VCAST = bytes.fromhex("7FF0FFFFFFFF")

NR = {"UNKNOWN_PID": 0, "FORMAT_ERROR": 1, "UNSUPPORTED_CC": 5, "DATA_OUT_OF_RANGE": 6,
      "BUFFER_FULL": 7, "PACKET_SIZE_UNSUPPORTED": 8, "SUB_DEVICE_OUT_OF_RANGE": 9}


def require_builds():
    if APP_HEX is None or not os.path.exists(BOOT_HEX):
        sys.exit("build first: pio run -d tube/boot_rdm && pio run -d tube/firmware -e isp")


def build_variant(name, patch, edit_main=None):
    """Copy of tube_fw with FW_VERSION_PATCH = patch (and an optional main.c edit),
    built with pio into tools/sim/.build/<name>; returns the tube_fw-*.hex path."""
    d = CACHE / name
    src = TUBE / "firmware"
    newest = max(p.stat().st_mtime for p in src.rglob("*") if ".pio" not in p.parts and p.is_file())
    out = sorted(d.glob(".pio/build/isp/tube_fw-*.hex"))
    if out and out[0].stat().st_mtime > newest:
        return str(out[0])
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    for x in ("src", "include"):
        shutil.copytree(src / x, d / x)
    for x in ("platformio.ini", "extra_script.py"):
        shutil.copy(src / x, d / x)
    cfg = d / "include/config.h"
    cfg.write_text(re.sub(r"#define FW_VERSION_PATCH\s+\d+", f"#define FW_VERSION_PATCH      {patch}", cfg.read_text()))
    if edit_main:
        m = d / "src/main.c"
        m.write_text(edit_main(m.read_text()))
    r = subprocess.run([PIO, "run", "-d", str(d), "-e", "isp"], capture_output=True, text=True)
    out = sorted(d.glob(".pio/build/isp/tube_fw-*.hex"))
    if r.returncode or not out:
        sys.exit(f"variant {name} failed to build:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    return str(out[0])


def newer_app():
    """same app, patch version + 1 (update target)"""
    return build_variant("newer", CFG["FW_VERSION_PATCH"] + 1)


def hanging_app():
    """valid trailer, but never answers RDM (rescue test)"""
    def edit(s):
        key = "    rs485_init();"
        if key not in s:
            sys.exit("hanging_app: rs485_init() call not found in main.c")
        return s.replace(key, key + "\n    for (;;) wdt_reset();               /* TEST IMAGE: never answers */", 1)
    return build_variant("hang", CFG["FW_VERSION_PATCH"] + 2, edit)


def symbols(elf):
    out = subprocess.run([NM, "-S", elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4:
            syms[f[3]] = (int(f[0], 16) & 0xFFFF, int(f[1], 16))
    return syms


class Checker:
    def __init__(self):
        self.ok = 0
        self.fail = []
        self.timing = []          # (name, turnaround, brk, mab, off, de_before)

    def check(self, name, cond, detail=""):
        if cond:
            self.ok += 1
        else:
            self.fail.append(f"{name}: {detail}")
            print(f"  FAIL {name}: {detail}", flush=True)
        return cond

    def timing_ok(self, name, a, dub=False):
        """E1.20 3.2: Table 3-3 (break 176-352, MAB 12-88, slots back-to-back),
        Table 3-4 (EOP -> SOP 176 us .. 2 ms), 3.2.3 (12 us), 3.2.4 (88 us)."""
        if not a.raw:
            return self.check(name + " answered", False, "no answer")
        self.timing.append((name, a.turnaround, a.brk, a.mab, a.de_after_end, a.de_before_start))
        ok = self.check(name + " turnaround 176us..2ms", 176 <= a.turnaround <= 2000, f"{a.turnaround:.1f} us")
        ok &= self.check(name + " no early drive", not a.de_early, "DE before EOP + 176 us")
        ok &= self.check(name + " driver off <= 88us", a.de_after_end is not None and 0 <= a.de_after_end <= 88,
                         f"{a.de_after_end}")
        ok &= self.check(name + " slots back-to-back", a.max_gap is not None and a.max_gap <= 76, f"{a.max_gap}")
        if dub:
            ok &= self.check(name + " DUB without break", a.brk is None, f"brk {a.brk}")
            ok &= self.check(name + " DUB driver <= 12us before start", a.de_before_start is not None
                             and a.de_before_start <= 12, f"{a.de_before_start}")
        else:
            ok &= self.check(name + " break 176..352", a.brk is not None and 176 <= a.brk <= 352, f"{a.brk}")
            ok &= self.check(name + " MAB 12..88", a.mab is not None and 12 <= a.mab <= 88, f"{a.mab}")
        return ok

    def resp(self, name, a, req_cc, req_pid, subdev=0, uid=UID, rt=0, nack=None, pd=None, pdl=None):
        """Generic E1.20 response checks (6.2, 6.3) + timing; returns the packet or None."""
        if not self.check(name + " answered", bool(a.raw), "no answer"):
            return None
        try:
            p = rdm_parse(a.raw)
        except ValueError as e:
            self.check(name + " parse", False, str(e))
            return None
        self.timing_ok(name, a)
        self.check(name + " dest = controller", p.dest == CTRL_UID, p.dest.hex())
        self.check(name + " src = UID", p.src == uid, p.src.hex())
        self.check(name + " TN echoed", p.tn == a.req_tn, f"{p.tn} != {a.req_tn}")
        self.check(name + " message count 0", p.msgcnt == 0, str(p.msgcnt))
        self.check(name + " sub-device echoed", p.subdev == subdev, f"0x{p.subdev:04X}")
        self.check(name + " CC", p.cc == req_cc + 1, f"0x{p.cc:02X}")
        self.check(name + " PID echoed", p.pid == req_pid, f"0x{p.pid:04X}")
        if nack is not None:
            self.check(name + f" NACK {nack}", p.rt == 2 and p.pd == NR[nack].to_bytes(2, "big"),
                       f"rt={p.rt} pd={p.pd.hex()}")
        else:
            self.check(name + " ACK", p.rt == rt, f"rt={p.rt} pd={p.pd.hex()}")
        if pd is not None:
            self.check(name + " data", p.pd == pd, f"{p.pd.hex()} != {pd.hex()}")
        if pdl is not None:
            self.check(name + " PDL", len(p.pd) == pdl, str(len(p.pd)))
        return p

    def silent(self, name, a):
        return self.check(name + " no answer", not a.raw, a.raw.hex())

    def summary(self):
        print(f"\n{self.ok} checks ok, {len(self.fail)} failed")
        for f in self.fail:
            print("  -", f)
        if self.timing:
            t = [x[1] for x in self.timing]
            b = [x[2] for x in self.timing if x[2] is not None]
            m = [x[3] for x in self.timing if x[3] is not None]
            o = [x[4] for x in self.timing if x[4] is not None]
            print(f"timing over {len(t)} answers: turnaround {min(t):.1f}..{max(t):.1f} us, "
                  f"break {min(b):.1f}..{max(b):.1f} us, MAB {min(m):.1f}..{max(m):.1f} us, "
                  f"driver off {min(o):.2f}..{max(o):.2f} us")
        return not self.fail


def snapshot(t):
    return copy.deepcopy(t)


def dub(t, lo, hi, dest=BCAST):
    return t.transact(rdm_packet(dest, CC_DISC, 0x0001, lo + hi))


class EmuTransport:
    """flash_rdm.py transport (transact / rescue) on top of the emulated tube."""
    name = "emu"

    def __init__(self, tube, fr):
        self.tube, self.fr, self.tn = tube, fr, 0
        self.log = []

    def transact(self, uid, cc, pid, pd=b""):
        self.tn = (self.tn + 1) & 0xFF
        a = self.tube.transact(self.fr.rdm_build(uid, self.fr.HOST_UID, self.tn, cc, pid, pd), wait_us=2500)
        self.log.append((cc, pid, len(pd), a.turnaround))
        if not a.raw:
            return None
        return self.fr.match_response(self.fr.rdm_find(a.raw), uid, cc, pid, self.tn)

    def rescue(self, uid, mode="relay"):
        """Power cycle, then GET BOOT_INFO back-to-back like the ESP32 'blr' spam."""
        t = self.tube
        t.cpu.power_on()
        pkt = self.fr.rdm_build(uid, self.fr.HOST_UID, 0, self.fr.CC_GET, self.fr.PID_BOOT_INFO)
        end = t.cpu.cyc + us(2_000_000)
        while t.cpu.cyc < end:
            a = t.transact(pkt, wait_us=2000)
            if a.raw:
                r = self.fr.match_response(self.fr.rdm_find(a.raw), uid, self.fr.CC_GET, self.fr.PID_BOOT_INFO)
                if r is not None:
                    return r
        return None


def load_flash_rdm():
    sys.path.insert(0, str(PROJ))
    import flash_rdm
    return flash_rdm
