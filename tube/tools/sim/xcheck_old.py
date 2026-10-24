"""Cross-check the emulator against the hardware log (logs/2026-10-24.log, 12:42,
tube 7FF0:E59521BA with tube_boot_rdm 1 + tube_fw 0.3.1): run the old merged
image (tube_boot_rdm 1 + tube_fw 0.3.0) and compare the answers byte by byte."""
from harness import *
from rdmline import CC_DISC, CC_GET, Tube, rdm_packet
import sys
import os
# the backup lives in the archive project ledTubeXBAR500 (not copied into this repo)
OLD = pathlib.Path(os.environ.get("TUBE_ARCHIVE", pathlib.Path.home() / "Documents/projekte/ledTubeXBAR500")) \
    / "backup/2026-10-24_vor_RDM-Audit/tube_merged_rdm.hex"
if not OLD.exists():
    sys.exit(f"{OLD} missing - nothing to compare")
U = bytes.fromhex("7FF0E59521BA")
t = Tube([str(OLD)], device_id=0xE59521BA)
t.run_ms(2400)
# hardware answers (RX lines of the log without the leading 0x00 break byte), TN patched below
hw = {
 0x0060: "CC012B7FF0000000017FF0E59521BABB00000000210060130100000101010000030100 0C01040001000000 0795",
 0x0050: "CC012C7FF0000000017FF0E59521BABF000000002100501400510080008100820 0E000E101200121012280000BEC",
}
def emu(pid, tn):
    a = t.transact(rdm_packet(U, CC_GET, pid, tn=tn, src=bytes.fromhex("7FF000000001")))
    return a.raw
for pid, h in hw.items():
    h = bytes.fromhex(h.replace(" ", ""))
    e = emu(pid, h[15])
    # 0.3.0 vs 0.3.1: software version byte 9 of DEVICE_INFO (offset 24+9) and the checksum differ
    diff = [i for i in range(max(len(h), len(e))) if i >= len(e) or i >= len(h) or h[i] != e[i]]
    print(f"PID 0x{pid:04X}: hw {h.hex()}\n            emu {e.hex()}\n            differing byte offsets {diff}")
a = t.transact(rdm_packet(U, CC_DISC, 0x0002, tn=0xB9, src=bytes.fromhex("7FF000000001")))
print("DISC_MUTE emu", a.raw.hex(), "\n          hw  cc011a7ff0000000017ff0e59521bab90000000011000202000006e9")
