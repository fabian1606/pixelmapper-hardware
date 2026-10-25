#!/usr/bin/env python3
"""Post-build check of the node app binary (protocol/image-format.md §2.1/§2.2).

Verifies the pm_image_desc_t block at image offset 288 (magic, format, size, manufacturer, model,
image_length/image_crc32 = 0 inside the image) and optionally that the binary fits into the OTA slot.

Usage: check_image.py build/pixelmapper_node.bin [--max-size N]
Exit code 0 = ok, 1 = error.
"""
import argparse
import struct
import sys

DESC_OFFSET = 288
DESC_SIZE = 64
MAGIC = 0x4D494D50  # "PMIM"
FORMAT = 1
ESTA = 0x7FF0
MODEL_NODE = 2


def check(path: str, max_size: int | None) -> list[str]:
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        return [f"cannot read {path}: {e}"]
    errors = []
    if len(data) < DESC_OFFSET + DESC_SIZE:
        return [f"{path}: too short ({len(data)} bytes) for the description block at {DESC_OFFSET}"]
    block = data[DESC_OFFSET:DESC_OFFSET + DESC_SIZE]
    magic, fmt, size, _res, esta, model, ver, hw_mask, img_len, img_crc = struct.unpack_from("<IBBHHHIIII", block)
    label = block[28:60].split(b"\0", 1)[0].decode("ascii", "replace")
    expect = [("magic", magic, MAGIC), ("format", fmt, FORMAT), ("size", size, DESC_SIZE),
              ("esta_manufacturer", esta, ESTA), ("device_model_id", model, MODEL_NODE),
              ("image_length", img_len, 0), ("image_crc32", img_crc, 0)]
    for name, got, want in expect:
        if got != want:
            errors.append(f"desc @{DESC_OFFSET}: {name} = 0x{got:X}, expected 0x{want:X}")
    if hw_mask == 0:
        errors.append(f"desc @{DESC_OFFSET}: hw_rev_mask is 0")
    print(f"pm_image_desc @ {DESC_OFFSET}: version {label} (0x{ver:06X}), hw_rev_mask 0x{hw_mask:X}")
    if max_size is not None:
        free = max_size - len(data)
        print(f"image size {len(data)} bytes, slot {max_size} bytes, free {free} bytes ({100.0 * free / max_size:.1f} %)")
        if free < 0:
            errors.append(f"image ({len(data)} bytes) does not fit into the OTA slot ({max_size} bytes)")
    else:
        print(f"image size {len(data)} bytes")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", help="path to build/pixelmapper_node.bin")
    ap.add_argument("--max-size", type=lambda s: int(s, 0), default=None, help="OTA slot size in bytes")
    args = ap.parse_args()
    errors = check(args.image, args.max_size)
    for e in errors:
        print(f"check_image: ERROR: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
