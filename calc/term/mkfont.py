#!/usr/bin/env python3
"""Turns font.txt into font.h: 96 glyphs (codes 32-127), 6 bytes each.

Each byte holds one row of the 4x6 cell in its low nibble, leftmost pixel in
bit 3. Bit 0 is the blank fourth column.
"""
import sys

NAMES = {"SP": " ", "DEL": "\x7f"}

glyphs = {}
for n, line in enumerate(open(sys.argv[1], encoding="ascii"), 1):
    if line.startswith("//") or not line.strip():
        continue
    name, *rows = line.split()
    ch = NAMES.get(name, name)
    if len(ch) != 1 or len(rows) != 6 or any(len(r) != 3 or set(r) - set("#.") for r in rows):
        sys.exit(f"{sys.argv[1]}:{n}: bad glyph line")
    if ch in glyphs:
        sys.exit(f"{sys.argv[1]}:{n}: duplicate glyph {name}")
    glyphs[ch] = [sum(8 >> i for i, c in enumerate(r) if c == "#") for r in rows]

missing = [chr(c) for c in range(32, 128) if chr(c) not in glyphs]
if missing:
    sys.exit(f"{sys.argv[1]}: missing glyphs: {missing}")

with open(sys.argv[2], "w") as out:
    out.write("// Generated from font.txt by mkfont.py. Do not edit.\n")
    out.write("static const unsigned char font[96][6] = {\n")
    for c in range(32, 128):
        row = ",".join(f"0x{b:X}" for b in glyphs[chr(c)])
        label = {32: "space", 92: "backslash", 127: "DEL"}.get(c, chr(c))
        out.write(f"  {{{row}}}, // {label}\n")
    out.write("};\n")
