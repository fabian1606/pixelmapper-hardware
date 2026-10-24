#!/usr/bin/env python3
"""Host side of the tube_debug ESP32 firmware.

  .venv/bin/python tool.py "rdm disc" "sniff 250000 3000"   run commands, then exit
  .venv/bin/python tool.py                                   interactive prompt

Host-only commands: "snap [name]" takes a webcam photo into cam/, "wait <s>" pauses.

Everything sent and received is appended to logs/<date>.log.
"""
import datetime
import os
import pathlib
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

# our ESP32 sits behind a CH340 (VID 0x1A86); other boards on the Mac use other bridges
PORT = os.environ.get("TUBE_PORT") or next(
    (p.device for p in serial.tools.list_ports.comports()
     if p.vid == 0x1A86 and p.device.startswith("/dev/cu.")), "/dev/cu.usbserial-110")
LOG_DIR = pathlib.Path(__file__).parent / "logs"
CAM_DIR = pathlib.Path(__file__).parent / "cam"


def log(line):
    LOG_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    with open(LOG_DIR / f"{datetime.date.today()}.log", "a") as f:
        f.write(f"{stamp} {line}\n")


def open_port():
    ser = serial.Serial(PORT, 115200, timeout=0.1)
    # opening the port resets the ESP32 via DTR/RTS; wait for the banner
    deadline = time.time() + 4
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if line.startswith("READY"):
            log(line)
            break
    ser.reset_input_buffer()
    return ser


def snap(name=""):
    """Webcam photo into cam/, returns its path."""
    CAM_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%H%M%S")
    path = CAM_DIR / f"{stamp}{'_' + name if name else ''}.jpg"
    subprocess.run(["imagesnap", "-q", "-d", os.environ.get("TUBE_CAM", "FULL HD webcam"), "-w", "1.0", str(path)], check=False, timeout=30)
    log(f"snap {path}")
    print(f"snap {path}")
    return path


def run(ser, cmd, timeout=60, quiet=False):
    """Sends one command, returns the answer lines (without "> done").
    quiet: only log, do not print (used by flash_dmx.py)."""
    if cmd.startswith("snap"):
        snap(cmd[4:].strip())
        return []
    if cmd.startswith("wait "):
        time.sleep(float(cmd[5:]))
        return []
    out = print if not quiet else (lambda *a: None)
    log(f">>> {cmd}")
    out(f">>> {cmd}")
    ser.write((cmd + "\n").encode())
    lines = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").rstrip()
        if not line:
            continue
        deadline = time.time() + timeout  # long scans print progress, keep waiting
        if line == "> done":
            return lines
        log(line)
        out(line)
        lines.append(line)
    out("(timeout)")
    log("(timeout)")
    return lines


def main():
    ser = open_port()
    if len(sys.argv) > 1:
        for cmd in sys.argv[1:]:
            run(ser, cmd)
        return
    while True:
        try:
            cmd = input("tube> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if cmd:
            run(ser, cmd)


if __name__ == "__main__":
    main()
