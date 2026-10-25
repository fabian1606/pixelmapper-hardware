#!/usr/bin/env python3
"""Generate src/ui_font_data.c (front-panel bitmap fonts) from BDF sources.

Fonts (see fonts/README.md for provenance and licenses):
  ui_font_8x16b  Terminus Font 4.49.1, ter-u16b.bdf (SIL OFL 1.1)
  ui_font_6x8    X11 misc-fixed 5x8.bdf (public domain), placed left-aligned
                 into a 6x8 cell (column 6 = letter spacing)

Code page (single byte, both fonts): 0x20..0x7E ASCII, 0x7F blank,
0x80..0x96 UI symbols (UI_G_* in src/ui_font.h), 0x97..0x9F blank.
UI symbols are either drawn below as ASCII art or taken from the source BDF.

Usage (from anywhere; paths are relative to this script):
  python3 tools/gen_fonts.py            write src/ui_font_data.c
  python3 tools/gen_fonts.py --preview  print all glyphs as ASCII art
  python3 tools/gen_fonts.py --check    exit 1 if src/ui_font_data.c is stale

Stdlib only, Python >= 3.11.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent      # components/pm_ui
FONT_DIR = ROOT / "fonts"
OUT_C = ROOT / "src" / "ui_font_data.c"

FIRST = 0x20
COUNT = 0x80                                        # codes 0x20..0x9F

# --------------------------------------------------------------------------
# UI symbol table: code, macro, description (must match src/ui_font.h)
# --------------------------------------------------------------------------
SYMBOLS: list[tuple[int, str, str]] = [
    (0x80, "UI_G_OUT", "right triangle: output"),
    (0x81, "UI_G_IN", "left triangle: input"),
    (0x82, "UI_G_DASH", "en dash: port off"),
    (0x83, "UI_G_REMOTE", "circular arrow: changed remotely"),
    (0x84, "UI_G_DOWN", "down arrow: update/download"),
    (0x85, "UI_G_CHECK", "check mark"),
    (0x86, "UI_G_BKSP", "backspace (erase left)"),
    (0x87, "UI_G_SPACE", "visible space (open box)"),
    (0x88, "UI_G_WARN", "warning triangle with !"),
    (0x89, "UI_G_LINK", "link up (up triangle)"),
    (0x8A, "UI_G_NOLINK", "cross: no link"),
    (0x8B, "UI_G_LOCK", "padlock"),
    (0x8C, "UI_G_DOT", "filled circle: staged"),
    (0x8D, "UI_G_SUB", "single right angle quote: submenu"),
    (0x8E, "UI_G_ELLIPSIS", "horizontal ellipsis"),
    (0x8F, "UI_G_MICRO", "micro sign"),
    (0x90, "UI_G_RARROW", "right arrow"),
    (0x91, "UI_G_LARROW", "left arrow"),
    (0x92, "UI_G_UARROW", "up arrow"),
    (0x93, "UI_G_LSUB", "single left angle quote"),
    (0x94, "UI_G_MIDDOT", "middle dot"),
    (0x95, "UI_G_IDENT", "sun / identify (circle with rays)"),
    (0x96, "UI_G_BLOCK", "full block (cursor)"),
]


def art(s: str) -> list[str]:
    """ASCII art -> list of rows ('#' = pixel on, '.' = off)."""
    return [ln.strip() for ln in s.strip().splitlines()]


# Each entry: int = Unicode code point taken from the source BDF,
#             list[str] = drawn here (full cell, h rows of w chars).
#
# 8x16 (Terminus 16 bold) metrics: cap height rows 2..11, x-height rows
# 5..11, baseline = row 11, descenders rows 12..14. Column 7 is normally
# the letter spacing and kept empty.
SYM_8x16: dict[int, int | list[str]] = {
    0x80: art("""
        ........
        ........
        ........
        .#......
        .##.....
        .###....
        .####...
        .#####..
        .####...
        .###....
        .##.....
        .#......
        ........
        ........
        ........
        ........
    """),
    0x81: art("""
        ........
        ........
        ........
        .....#..
        ....##..
        ...###..
        ..####..
        .#####..
        ..####..
        ...###..
        ....##..
        .....#..
        ........
        ........
        ........
        ........
    """),
    0x82: 0x2013,                                   # EN DASH
    0x83: art("""
        ........
        ........
        ....#...
        ..#####.
        .##.##..
        ##..#...
        ##......
        ##....##
        ##....##
        .##..##.
        ..####..
        ........
        ........
        ........
        ........
        ........
    """),
    0x84: 0x2193,                                   # DOWNWARDS ARROW
    0x85: art("""
        ........
        ........
        ........
        ......#.
        .....##.
        .....##.
        ....##..
        ##..##..
        ##.##...
        .####...
        .###....
        ..#.....
        ........
        ........
        ........
        ........
    """),
    0x86: art("""
        ........
        ........
        ........
        ....####
        ...#####
        ..######
        .###.#.#
        #####.##
        .###.#.#
        ..######
        ...#####
        ....####
        ........
        ........
        ........
        ........
    """),
    0x87: art("""
        ........
        ........
        ........
        ........
        ........
        ........
        ........
        ........
        ##...##.
        ##...##.
        #######.
        #######.
        ........
        ........
        ........
        ........
    """),
    0x88: art("""
        ........
        ........
        ...#....
        ..###...
        ..#.#...
        .##.##..
        .##.##..
        ###.###.
        ###.###.
        #######.
        ###.###.
        #######.
        ........
        ........
        ........
        ........
    """),
    0x89: art("""
        ........
        ........
        ........
        ........
        ...#....
        ...#....
        ..###...
        ..###...
        .#####..
        .#####..
        #######.
        #######.
        ........
        ........
        ........
        ........
    """),
    0x8A: art("""
        ........
        ........
        ........
        ........
        ##...##.
        ###.###.
        .#####..
        ..###...
        .#####..
        ###.###.
        ##...##.
        ........
        ........
        ........
        ........
        ........
    """),
    0x8B: art("""
        ........
        ........
        ..###...
        .##.##..
        .#...#..
        .#...#..
        #######.
        #######.
        ###.###.
        ###.###.
        #######.
        #######.
        ........
        ........
        ........
        ........
    """),
    0x8C: 0x25CF,                                   # BLACK CIRCLE
    0x8D: 0x203A,                                   # SINGLE RIGHT-POINTING ANGLE QUOTATION MARK
    0x8E: 0x2026,                                   # HORIZONTAL ELLIPSIS
    0x8F: 0x00B5,                                   # MICRO SIGN
    0x90: 0x2192,                                   # RIGHTWARDS ARROW
    0x91: 0x2190,                                   # LEFTWARDS ARROW
    0x92: 0x2191,                                   # UPWARDS ARROW
    0x93: 0x2039,                                   # SINGLE LEFT-POINTING ANGLE QUOTATION MARK
    0x94: 0x00B7,                                   # MIDDLE DOT
    0x95: art("""
        ........
        ........
        ........
        ...#....
        #..#..#.
        .#...#..
        ..###...
        #.###.#.
        ..###...
        .#...#..
        #..#..#.
        ...#....
        ........
        ........
        ........
        ........
    """),
    0x96: 0x2588,                                   # FULL BLOCK
}

# 6x8 (misc-fixed 5x8) metrics: cap height rows 1..6, x-height rows 3..6,
# baseline = row 6, descender row 7. Column 5 is the letter spacing.
SYM_6x8: dict[int, int | list[str]] = {
    # ASCII override: the 5x8 period is a 3-px cross that looks like "+" in IP addresses; use a 2x2 dot.
    0x2E: art("""
        ......
        ......
        ......
        ......
        ......
        .##...
        .##...
        ......
    """),
    0x80: art("""
        ......
        ......
        .#....
        .##...
        .###..
        .##...
        .#....
        ......
    """),
    0x81: art("""
        ......
        ......
        ...#..
        ..##..
        .###..
        ..##..
        ...#..
        ......
    """),
    0x82: 0x2013,
    0x83: art("""
        ......
        ..#...
        .###..
        #.#...
        #...#.
        #...#.
        .###..
        ......
    """),
    0x84: 0x2193,
    0x85: art("""
        ......
        ......
        ....#.
        ...##.
        #.##..
        ###...
        .#....
        ......
    """),
    0x86: art("""
        ......
        ......
        ..####
        .#.#.#
        ###.##
        .#.#.#
        ..####
        ......
    """),
    0x87: art("""
        ......
        ......
        ......
        ......
        #...#.
        #...#.
        #####.
        ......
    """),
    0x88: art("""
        ......
        ..#...
        .###..
        .#.#..
        ##.##.
        #####.
        ##.##.
        ......
    """),
    0x89: 0x25B2,                                   # BLACK UP-POINTING TRIANGLE
    0x8A: art("""
        ......
        ......
        #...#.
        .#.#..
        ..#...
        .#.#..
        #...#.
        ......
    """),
    0x8B: art("""
        ......
        .###..
        .#.#..
        #####.
        ##.##.
        ##.##.
        #####.
        ......
    """),
    0x8C: 0x25CF,
    0x8D: 0x203A,
    0x8E: 0x2026,
    0x8F: 0x00B5,
    0x90: 0x2192,
    0x91: 0x2190,
    0x92: 0x2191,
    0x93: 0x2039,
    0x94: 0x00B7,
    0x95: art("""
        ......
        ......
        #.#.#.
        .###..
        #####.
        .###..
        #.#.#.
        ......
    """),
    0x96: art("""
        ######
        ######
        ######
        ######
        ######
        ######
        ######
        ######
    """),
}


# --------------------------------------------------------------------------
# BDF parsing
# --------------------------------------------------------------------------
@dataclass
class BdfGlyph:
    enc: int
    bbx: tuple[int, int, int, int]                  # w, h, xoff, yoff
    rows: list[int]                                 # left-aligned, byte-padded


@dataclass
class BdfFont:
    path: Path
    ascent: int
    descent: int
    copyright: str
    glyphs: dict[int, BdfGlyph]


def parse_bdf(path: Path) -> BdfFont:
    ascent = descent = None
    copyright_ = ""
    glyphs: dict[int, BdfGlyph] = {}
    enc = None
    bbx = None
    rows: list[int] | None = None
    with path.open(encoding="latin-1") as f:
        for line in f:
            tok = line.split()
            if not tok:
                continue
            key = tok[0]
            if rows is not None and key != "ENDCHAR":
                rows.append(int(tok[0], 16))
            elif key == "FONT_ASCENT":
                ascent = int(tok[1])
            elif key == "FONT_DESCENT":
                descent = int(tok[1])
            elif key == "COPYRIGHT":
                copyright_ = line.split(None, 1)[1].strip().strip('"')
            elif key == "STARTCHAR":
                enc, bbx = None, None
            elif key == "ENCODING":
                enc = int(tok[1])
            elif key == "BBX":
                bbx = tuple(int(t) for t in tok[1:5])
            elif key == "BITMAP":
                rows = []
            elif key == "ENDCHAR":
                if enc is not None and enc >= 0 and bbx is not None and rows is not None:
                    glyphs[enc] = BdfGlyph(enc, bbx, rows)
                rows = None
    if ascent is None or descent is None:
        raise SystemExit(f"{path}: missing FONT_ASCENT/FONT_DESCENT")
    return BdfFont(path, ascent, descent, copyright_, glyphs)


def bdf_cell(font: BdfFont, cp: int, w: int, h: int) -> list[str]:
    """Render BDF glyph `cp` into a w x h cell, baseline at row ascent-1."""
    g = font.glyphs.get(cp)
    if g is None:
        raise SystemExit(f"{font.path.name}: no glyph U+{cp:04X}")
    gw, gh, xo, yo = g.bbx
    if len(g.rows) != gh:
        raise SystemExit(f"{font.path.name}: U+{cp:04X} row count mismatch")
    nbits = ((gw + 7) // 8) * 8
    top = font.ascent - (yo + gh)                   # cell row of first bitmap row
    cell = [["."] * w for _ in range(h)]
    for i, r in enumerate(g.rows):
        for x in range(gw):
            if (r >> (nbits - 1 - x)) & 1:
                cx, cy = xo + x, top + i
                if not (0 <= cx < w and 0 <= cy < h):
                    raise SystemExit(
                        f"{font.path.name}: U+{cp:04X} pixel ({cx},{cy}) outside {w}x{h} cell")
                cell[cy][cx] = "#"
    return ["".join(r) for r in cell]


# --------------------------------------------------------------------------
# Font assembly
# --------------------------------------------------------------------------
@dataclass
class Font:
    cname: str
    w: int
    h: int
    glyphs: list[list[str]]                         # COUNT entries, h rows each
    origin: dict[int, str]                          # code -> origin note


def build(cname: str, bdf: BdfFont, w: int, h: int,
          symbols: dict[int, int | list[str]]) -> Font:
    blank = ["." * w] * h
    glyphs: list[list[str]] = []
    origin: dict[int, str] = {}
    for code in range(FIRST, FIRST + COUNT):
        if 0x20 <= code <= 0x7E and code not in symbols:
            cell = bdf_cell(bdf, code, w, h)
            origin[code] = bdf.path.name
        elif code in symbols:
            spec = symbols[code]
            if isinstance(spec, int):
                cell = bdf_cell(bdf, spec, w, h)
                origin[code] = f"{bdf.path.name} U+{spec:04X}"
            else:
                cell = spec
                origin[code] = "drawn"
                if len(cell) != h or any(len(r) != w or set(r) - set(".#") for r in cell):
                    raise SystemExit(f"{cname}: art for 0x{code:02X} is not {w}x{h}")
        else:
            cell = blank
            origin[code] = "blank"
        glyphs.append(cell)
    return Font(cname, w, h, glyphs, origin)


def fonts() -> list[Font]:
    ter = parse_bdf(FONT_DIR / "ter-u16b.bdf")
    fix = parse_bdf(FONT_DIR / "5x8.bdf")
    return [
        build("ui_font_8x16b", ter, 8, 16, SYM_8x16),
        build("ui_font_6x8", fix, 6, 8, SYM_6x8),
    ]


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------
SYM_NAME = {code: (macro, desc) for code, macro, desc in SYMBOLS}


def label(code: int) -> str:
    if 0x21 <= code <= 0x7E:
        return "backslash" if code == 0x5C else f"'{chr(code)}'"
    if code == 0x20:
        return "space"
    if code in SYM_NAME:
        return SYM_NAME[code][0]
    return "blank"


def c_comment_safe(s: str) -> str:
    return s.replace("*/", "* /").replace("/*", "/ *")


def row_byte(row: str) -> int:
    v = 0
    for x, c in enumerate(row):
        if c == "#":
            v |= 0x80 >> x
    return v


def gen_c(fs: list[Font]) -> str:
    out: list[str] = []
    out.append("""\
/*
 * ui_font_data.c - bitmap fonts of the front panel.
 *
 * GENERATED by tools/gen_fonts.py, DO NOT EDIT. Regenerate with:
 *   python3 tools/gen_fonts.py
 *
 * ui_font_8x16b: Terminus Font 4.49.1 (ter-u16b.bdf),
 *   Copyright (C) 2020 Dimitar Toshkov Zhekov,
 *   licensed under the SIL Open Font License 1.1 (fonts/OFL-Terminus.txt).
 *   Modified: subset to ASCII + UI symbols, re-encoded into this code page,
 *   some UI symbols drawn for this project. Not distributed as "Terminus Font".
 * ui_font_6x8: X11 misc-fixed 5x8 (5x8.bdf from xorg font-misc-misc 1.1.3),
 *   public domain (fonts/LICENSE-misc-fixed.txt), placed left-aligned into
 *   a 6x8 cell; some UI symbols drawn for this project.
 *
 * Layout: codes 0x20..0x9F, h bytes per glyph, one byte per row,
 * bit 7 = leftmost pixel. See fonts/README.md for provenance.
 */
#include "ui_font.h"
""")
    for f in fs:
        arr = f"{f.cname}_bits"
        out.append(f"static const uint8_t {arr}[{COUNT} * {f.h}] = {{")
        for i, cell in enumerate(f.glyphs):
            code = FIRST + i
            b = ", ".join(f"0x{row_byte(r):02X}" for r in cell)
            out.append(f"    /* 0x{code:02X} {c_comment_safe(label(code))} */")
            out.append(f"    {b},")
        out.append("};")
        out.append("")
    for f in fs:
        out.append(f"const ui_font_t {f.cname} = {{")
        out.append(f"    .w = {f.w}, .h = {f.h},")
        out.append(f"    .first = 0x{FIRST:02X}, .count = 0x{COUNT:02X},")
        out.append(f"    .bits = {f.cname}_bits,")
        out.append("};")
        out.append("")
    return "\n".join(out)


def preview(fs: list[Font]) -> str:
    out: list[str] = []
    per_line = 8
    for f in fs:
        out.append(f"=== {f.cname} ({f.w}x{f.h}) ===")
        for start in range(0, COUNT, per_line):
            codes = range(FIRST + start, FIRST + min(start + per_line, COUNT))
            out.append("  ".join(f"0x{c:02X}".ljust(f.w + 2) for c in codes))
            for y in range(f.h):
                out.append("  ".join(("|" + f.glyphs[c - FIRST][y] + "|") for c in codes))
            out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--preview", action="store_true", help="print all glyphs as ASCII art")
    ap.add_argument("--check", action="store_true", help="fail if the output is stale")
    args = ap.parse_args()
    fs = fonts()
    if args.preview:
        sys.stdout.write(preview(fs))
        return 0
    text = gen_c(fs)
    if args.check:
        if not OUT_C.exists() or OUT_C.read_text(encoding="utf-8") != text:
            print(f"{OUT_C} is stale, run tools/gen_fonts.py", file=sys.stderr)
            return 1
        return 0
    OUT_C.write_text(text, encoding="utf-8")
    print(f"wrote {OUT_C.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
