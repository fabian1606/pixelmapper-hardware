"""Slow test: bootloader idle timeout (~59 s) after ENTER_BOOTLOADER.

  python3 tools/sim/test_idle.py     (~7 min)
"""
import sys, time
from harness import *
from rdmline import CC_GET, CC_SET, Tube
require_builds()
C = Checker()
t0 = time.time()
t = Tube([BOOT_HEX, APP_HEX], device_id=0xB06675AA)
t.run_ms(2400)
a = t.rdm(UID, CC_SET, 0x8000, b"BOOT")
C.check("ENTER_BOOTLOADER ACK", a.pkt and a.pkt.rt == 0)
t.run_ms(100)
a = t.rdm(UID, CC_GET, 0x8010)
C.check("in bootloader", a.pkt and a.pkt.rt == 0)
entered = t.ms
resets = len(t.cpu.resets)
t.run_ms(50_000)
a = t.rdm(UID, CC_GET, 0x0060)                    # not a bootloader PID: must not extend the timeout
C.check("still in bootloader after 50 s", len(t.cpu.resets) == resets and a.pkt and a.pkt.pd[6] == 0xB0)
t.run_ms(12_000)
C.check("app started by the idle timeout", len(t.cpu.resets) == resets + 1, t.cpu.resets[-2:])
started = t.cpu.resets[-1][0] / 16000 - entered
print(f"idle timeout: app restarted {started / 1000:.1f} s after the last bootloader PID")
C.check("timeout 55..62 s", 55_000 <= started <= 62_000, started)
print(f"real {time.time() - t0:.0f} s")
sys.exit(0 if C.summary() else 1)
