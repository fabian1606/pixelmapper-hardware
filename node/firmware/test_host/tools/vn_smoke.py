#!/usr/bin/env python3
"""Smoke test of the virtual node (ctest): start it, ArtPoll via the emulated broadcast port, expect 4
ArtPollReply (BindIndex 1-4) and a TOD with the configured tubes; control channel answers."""
import socket
import subprocess
import sys
import time


def main() -> int:
    exe = sys.argv[1]
    p = subprocess.Popen([exe, "--port", "0", "--ctl-port", "0", "--tubes", "1:2", "--stdin-watch"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        line = p.stdout.readline().split()
        kv = dict(x.split("=", 1) for x in line[1:])
        bport, cport = int(kv["bcast"]), int(kv["ctl"])
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        s.settimeout(2.0)
        poll = b"Art-Net\x00" + (0x2000).to_bytes(2, "little") + bytes([0, 14, 0x02, 0])
        s.sendto(poll, ("127.0.0.1", bport))
        binds = set()
        deadline = time.time() + 3
        while time.time() < deadline and len(binds) < 4:
            try:
                d, _ = s.recvfrom(2048)
            except socket.timeout:
                break
            if d[8:10] == (0x2100).to_bytes(2, "little"):
                binds.add(d[211])
        if binds != {1, 2, 3, 4}:
            print("ArtPollReply BindIndex:", binds)
            return 1
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.settimeout(2.0)
        for _ in range(40):
            c.sendto(b"tod 1", ("127.0.0.1", cport))
            r = c.recv(8192).decode()
            if r.startswith("ok 2 "):
                return 0
            time.sleep(0.1)
        print("TOD:", r)
        return 1
    finally:
        p.kill()
        p.wait()


if __name__ == "__main__":
    sys.exit(main())
