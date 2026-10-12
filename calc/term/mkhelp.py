#!/usr/bin/env python3
"""Turns help.txt into help.h: the pages the menu's F5 shows.

    mkhelp.py help.txt help.h
"""
import sys

COLS, ROWS = 60, 20  # the 21st row holds the "next page" line

source, target = sys.argv[1], sys.argv[2]
pages = [[]]
for n, line in enumerate(open(source, encoding="ascii"), 1):
    line = line.rstrip("\n")
    if line.startswith("#"):
        continue
    if line and not line.strip("-"):
        pages.append([])
        continue
    if len(line) > COLS:
        sys.exit(f"{source}:{n}: longer than {COLS} columns")
    pages[-1].append(line)
for number, page in enumerate(pages, 1):
    if len(page) > ROWS:
        sys.exit(f"{source}: page {number} has {len(page)} lines, more than {ROWS}")


def literal(line):
    return '"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\r\\n"'


with open(target, "w") as out:
    out.write("// Generated from help.txt by mkhelp.py. Do not edit.\n")
    out.write(f"#define HELP_PAGES {len(pages)}\n\n")
    out.write("static const char *const helpPages[HELP_PAGES] = {\n")
    for page in pages:
        out.write("\n".join("  " + literal(line) for line in page) + ",\n\n")
    out.write("};\n")
