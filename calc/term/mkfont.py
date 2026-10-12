#!/usr/bin/env python3
"""Turns font.txt into font.h: the glyphs, and which glyph each character gets.

    mkfont.py font.txt font.h     write the header
    mkfont.py font.txt --list     print every character covered beyond ASCII

A glyph is 6 bytes. Each byte holds one row of the 4x6 cell in its low
nibble, leftmost pixel in bit 3.

Glyph codes, as term.c uses them:
    32-127   ASCII, drawn from font.txt. 127 stands for "no glyph for this".
    128-143  box-drawing lines: 128 + a mask of the arms that leave the
             centre of the cell (1 up, 2 down, 4 left, 8 right)
    144-     letters with their accent (see ACCENTS_ABOVE), then the block
             elements: halves, eighths, quadrants and shades

Text letters keep the fourth column and the sixth row blank. Lines and blocks
use the whole cell, so that they join up with their neighbours.

Characters outside ASCII are looked up in three tables: two dense ones (Latin
letters with accents, boxes and blocks) and a short sorted list for the rest.
A glyph code of 0 means the character takes no room at all.
"""
import sys
import unicodedata

NAMES = {"SP": " ", "DEL": "\x7f"}
UNKNOWN = 127
BOX = 128
UP, DOWN, LEFT, RIGHT = 1, 2, 4, 8
CELL_W, CELL_H = 4, 6

LATIN = range(0x00A0, 0x0180)   # Latin-1 Supplement and Latin Extended-A
BOXES = range(0x2500, 0x25A0)   # Box Drawing and Block Elements

# Latin characters that do not reduce to a letter by dropping their accent.
LATIN_AS = {
    " ": " ", "¡": "!", "¢": "c", "£": "L", "¤": "*", "¥": "Y", "¦": "|",
    "§": "S", "¨": '"', "©": "c", "ª": "a", "«": "<", "¬": "-", "­": "-",
    "®": "r", "¯": "-", "°": "o", "±": "+", "´": "'", "µ": "u", "¶": "P",
    "·": ".", "¸": ",", "º": "o", "»": ">", "¿": "?", "Æ": "A", "Ð": "D",
    "×": "x", "Ø": "O", "Þ": "P", "ß": "s", "æ": "a", "ð": "d", "÷": "/",
    "ø": "o", "þ": "p", "Đ": "D", "đ": "d", "Ħ": "H", "ħ": "h", "ı": "i",
    "ĸ": "k", "Ł": "L", "ł": "l", "ŉ": "n", "Ŋ": "N", "ŋ": "n", "Œ": "O",
    "œ": "o", "Ŧ": "T", "ŧ": "t",
}

# The accents that fit in one row of three pixels, by their combining mark.
# One above takes the top row of the cell, which most small letters leave
# empty; on an i it takes the place of the dot. The cedilla takes the gap row
# below the letter. A letter with no room for its accent is drawn plain: the
# capitals, and the small letters with an ascender. So is one whose accent
# would only make it look taller, as two dots do on a u. Some accents end
# up looking alike: on an a, the grave and the circumflex.
ACCENTS_ABOVE = {
    "\u0301": "..#",  # acute
    "\u0300": "#..",  # grave
    "\u0302": ".#.",  # circumflex
    "\u0303": "#.#",  # tilde: a bar instead made the a look like a 5
    "\u0308": "#.#",  # diaeresis, the same
}
ACCENTS_BELOW = {"\u0327": ".#."}  # cedilla

# Everything else, grouped by the ASCII character that stands in for it.
OTHERS_AS = {
    "'": "‘’‚‛′",
    '"': "“”„‟″",
    "-": "‐‑‒–—―−",
    ".": "…‥∙⋅",
    "*": "•‣●◆★☆✱✳",
    "o": "○◦◯◇",
    "#": "□▢▫☐",
    "<": "←⇐‹◀◁◂◄≤❮",
    ">": "→⇒›▶▷▸►≥❯➜➤",
    "^": "↑⇑▲△▴",
    "v": "↓⇓▼▽▾✓✔☑",
    "x": "✗✘✕✖☒",
    "/": "⁄∕",
    "~": "≈∼",
    "=": "≡",
    "!": "≠⚠",
    "E": "€",
    "|": "⎪⎮❘",
    " ": "            　",
}
FULL_BLOCK_AS = "■▪▮"
# Marks that only say how to join or order their neighbours.
NO_WIDTH = "​‌‍‎‏⁠﻿"


def read_font(path):
    glyphs = {}
    for n, line in enumerate(open(path, encoding="ascii"), 1):
        if line.startswith("//") or not line.strip():
            continue
        name, *rows = line.split()
        ch = NAMES.get(name, name)
        if len(ch) != 1 or len(rows) != 6 or any(len(r) != 3 or set(r) - set("#.") for r in rows):
            sys.exit(f"{path}:{n}: bad glyph line")
        if ch in glyphs:
            sys.exit(f"{path}:{n}: duplicate glyph {name}")
        glyphs[ch] = tuple(sum(8 >> i for i, c in enumerate(r) if c == "#") for r in rows)
    missing = [chr(c) for c in range(32, 128) if chr(c) not in glyphs]
    if missing:
        sys.exit(f"{path}: missing glyphs: {missing}")
    return [glyphs[chr(c)] for c in range(32, 128)]


def accented_glyph(ch, glyphs):
    """The glyph of a letter with its accent drawn, or None if it has to go without."""
    parts = unicodedata.normalize("NFD", ch)
    if len(parts) != 2 or not "A" <= parts[0] <= "z":
        return None
    letter, mark = parts
    plain = glyphs[ord(letter) - 32]
    rows = list(plain)
    if mark in ACCENTS_ABOVE and (not rows[0] or letter == "i"):
        row, pixels = 0, ACCENTS_ABOVE[mark]
    elif mark in ACCENTS_BELOW and not rows[5]:
        row, pixels = 5, ACCENTS_BELOW[mark]
    else:
        return None
    bits = sum(8 >> i for i, c in enumerate(pixels) if c == "#")
    if row == 0 and letter != "i":
        middle = 4
        if bits == rows[1] and bits != middle:
            return None  # two dots on a u
        # A pixel right above a stroke reads as part of the letter: a grave
        # turns an a into a b. It moves to the middle, always on the left
        # and on the right when the middle is free.
        if bits & rows[1] and bits != middle and (bits == 8 or not middle & rows[1]):
            bits = middle
    rows[row] = bits
    return None if tuple(rows) == plain else tuple(rows)  # a circumflex on an i is its dot


def bitmap(filled):
    """The glyph whose pixels are the (x, y) for which filled(x, y) holds."""
    return tuple(sum(8 >> x for x in range(CELL_W) if filled(x, y)) for y in range(CELL_H))


def box_glyph(arms):
    """Lines from the centre of the cell (column 1, row 2) out to its edges."""
    def filled(x, y):
        if not arms:
            return False
        if (x, y) == (1, 2):
            return True
        if x == 1:
            return bool(arms & (UP if y < 2 else DOWN))
        if y == 2:
            return bool(arms & (LEFT if x < 1 else RIGHT))
        return False
    return bitmap(filled)


def box_arms(name):
    """The arms of a box-drawing character, read off its Unicode name."""
    words = name.split()
    arms = 0
    for word, bits in (("UP", UP), ("DOWN", DOWN), ("LEFT", LEFT), ("RIGHT", RIGHT),
                       ("VERTICAL", UP | DOWN), ("HORIZONTAL", LEFT | RIGHT)):
        if word in words:
            arms |= bits
    return arms


def block_shape(cp, name):
    """The pixels of a block element, as a test on (x, y)."""
    def rows(eighths):
        return max(1, int(CELL_H * eighths / 8 + 0.5))

    def columns(eighths):
        return max(1, int(CELL_W * eighths / 8 + 0.5))

    if cp == 0x2580: return lambda x, y: y < CELL_H // 2
    if 0x2581 <= cp <= 0x2588: return lambda x, y: y >= CELL_H - rows(cp - 0x2580)
    if 0x2589 <= cp <= 0x258F: return lambda x, y: x < columns(0x2590 - cp)
    if cp == 0x2590: return lambda x, y: x >= CELL_W // 2
    if cp == 0x2591: return lambda x, y: (x + 2 * y) % 4 == 0
    if cp == 0x2592: return lambda x, y: (x + y) % 2 == 0
    if cp == 0x2593: return lambda x, y: (x + 2 * y) % 4 != 0
    if cp == 0x2594: return lambda x, y: y < 1
    if cp == 0x2595: return lambda x, y: x >= CELL_W - 1
    quadrants = name.replace("QUADRANT ", "").split(" AND ")
    return lambda x, y: (("UPPER" if y < CELL_H // 2 else "LOWER") + " "
                         + ("LEFT" if x < CELL_W // 2 else "RIGHT")) in quadrants


def build(font_path):
    glyphs = read_font(font_path) + [box_glyph(arms) for arms in range(16)]

    def code_for(shape):
        if shape not in glyphs[96:]:
            glyphs.append(shape)
        return 32 + 96 + glyphs[96:].index(shape)

    latin = []
    for cp in LATIN:
        ch = chr(cp)
        accented = accented_glyph(ch, glyphs)
        if accented:
            latin.append(code_for(accented))
            continue
        base = LATIN_AS.get(ch) or unicodedata.normalize("NFKD", ch)[0]
        if not " " <= base <= "~":
            sys.exit(f"no stand-in for U+{cp:04X} {unicodedata.name(ch)}")
        latin.append(ord(base))

    boxes = []
    for cp in BOXES:
        name = unicodedata.name(chr(cp))
        if "DIAGONAL" in name:
            boxes.append(ord("X" if "CROSS" in name else "/" if "UPPER RIGHT TO" in name else "\\"))
        elif name.startswith("BOX DRAWINGS"):
            boxes.append(BOX + box_arms(name))
        else:
            boxes.append(code_for(bitmap(block_shape(cp, name))))

    others = {}
    for stand_in, chars in OTHERS_AS.items():
        others.update((ord(ch), ord(stand_in)) for ch in chars)
    full_block = boxes[0x2588 - BOXES.start]
    others.update((ord(ch), full_block) for ch in FULL_BLOCK_AS)
    others.update((ord(ch), 0) for ch in NO_WIDTH)
    return glyphs, latin, boxes, sorted(others.items())


def write_header(path, glyphs, latin, boxes, others):
    def byte_table(name, values):
        lines = [f"static const unsigned char {name}[{len(values)}] = {{"]
        for i in range(0, len(values), 16):
            lines.append("  " + ",".join(str(v) for v in values[i:i + 16]) + ",")
        return "\n".join(lines) + "\n};\n"

    with open(path, "w") as out:
        out.write("// Generated from font.txt by mkfont.py. Do not edit.\n")
        out.write(f"#define GLYPHS {len(glyphs)}\n")
        out.write(f"#define GLYPH_UNKNOWN {UNKNOWN}\n")
        out.write(f"#define GLYPH_BOX {BOX}  // + arms: 1 up, 2 down, 4 left, 8 right\n")
        out.write(f"#define LATIN_FIRST 0x{LATIN.start:04X}\n")
        out.write(f"#define BOXES_FIRST 0x{BOXES.start:04X}\n")
        out.write(f"#define OTHERS {len(others)}\n\n")
        out.write("static const unsigned char font[GLYPHS][6] = {\n")
        for i, glyph in enumerate(glyphs):
            code = 32 + i
            label = {32: "space", 92: "backslash", 127: "no glyph"}.get(
                code, chr(code) if code < 127 else f"glyph {code}")
            out.write("  {" + ",".join(f"0x{b:X}" for b in glyph) + f"}}, // {label}\n")
        out.write("};\n\n")
        out.write(byte_table("latinGlyphs", latin))
        out.write(byte_table("boxGlyphs", boxes))
        out.write("static const unsigned short otherCodes[OTHERS] = {\n")
        for i in range(0, len(others), 8):
            out.write("  " + ",".join(f"0x{cp:04X}" for cp, _ in others[i:i + 8]) + ",\n")
        out.write("};\n")
        out.write(byte_table("otherGlyphs", [glyph for _, glyph in others]))


def print_list(glyphs, latin, boxes, others):
    def picture(code):
        if code == 0:
            return ["(takes no room)"]
        if code < 127:
            return [f"as {chr(code)!r}"]
        return ["".join("#" if row & (8 >> x) else "." for x in range(CELL_W))
                for row in glyphs[code - 32]]

    covered = [(LATIN.start + i, code) for i, code in enumerate(latin)]
    covered += [(BOXES.start + i, code) for i, code in enumerate(boxes)]
    covered += others
    for cp, code in sorted(covered):
        shown = chr(cp) if chr(cp).isprintable() else " "
        print(f"U+{cp:04X} {shown}  {' '.join(picture(code))}  {unicodedata.name(chr(cp), '')}")
    print(f"\n{len(covered)} characters beyond ASCII, {len(glyphs) - 96} glyphs beyond ASCII")


if __name__ == "__main__":
    tables = build(sys.argv[1])
    if sys.argv[2] == "--list":
        print_list(*tables)
    else:
        write_header(sys.argv[2], *tables)
