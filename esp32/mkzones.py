#!/usr/bin/env python3
"""Makes Zones.h: the time zones' names and their POSIX TZ strings, for /tz.

    mkzones.py [zoneinfo dir] > esp32_tilink_term/Zones.h

The ESP32 has no time zone database, only POSIX TZ strings. Each zone's
string is the footer of its TZif file (version 2 and up), so this needs a
computer that has one, /usr/share/zoneinfo by default. The names are those
of zone.tab, and UTC.
"""
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/zoneinfo"
names = ["UTC"]
for line in open(os.path.join(root, "zone.tab"), encoding="utf-8"):
    if not line.startswith("#"):
        names.append(line.split("\t")[2].strip())

zones = []
for name in sorted(set(names)):
    data = open(os.path.join(root, name), "rb").read()
    if not data.startswith(b"TZif") or data[4:5] < b"2":
        sys.exit(f"{name}: not a TZif file of version 2 or later")
    footer = data.rstrip(b"\n").rsplit(b"\n", 1)[1].decode("ascii")
    if not footer:
        sys.exit(f"{name}: no POSIX TZ string at the end")
    zones.append((name, footer))

try:
    version = open(os.path.join(root, "+VERSION")).read().strip()
except OSError:
    version = "unknown"

print(f"""// Made by esp32/mkzones.py from the tz database {version}: do not edit.
// Each zone's name and its POSIX TZ string, one after the other, each ended
// by a NUL, in the order of the names. An empty name ends the list.
#pragma once

const char ZONES[] =""")
for name, tz in zones:
    print(f'  "{name}\\0{tz}\\0"')
print('  "";')
print(f"\nconst int ZONE_COUNT = {len(zones)};")
