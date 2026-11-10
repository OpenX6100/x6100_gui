#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Xiegu X6100 - DSC ship names database builder.

Turns a vessels CSV (columns mmsi, ship_name, call_sign, ship_type; all the
others are ignored) into the compact binary file read by src/dsc/dsc_ships.c:

  header   16 bytes   "DSCS", u32 version (2), u32 count, u32 0
  index    count x 8 bytes, sorted by MMSI: u32 mmsi, u32 offset of the strings
  strings  name \\0 call sign \\0 ship type \\0   (ASCII only)

All integers little endian.  Usage (from the x6100_gui folder):

  python3 src/dsc/make_ships_db.py vessels.csv ships.bin \
          [extra_ships.txt] [extra_special.txt] [extra_coast.txt]

The optional extra lists only add MMSIs that vessels.csv
does not have, and fill a missing call sign / ship type of a known MMSI with
the same name.  Three formats, recognised by their content:

  ship      201100124,RAFAELO,ZADN3,Cargo
  special   211204590Jade Hochschule DH6000       (training / test sites)
  coast     002040100,AZR,Delgada,37.73,25.67     (ship type "Coast station")

No ships file comes with the firmware: put ships.bin in /mnt/dsc/ on the
SD card (see rootfs/usr/share/x6100/README-dsc.md), built from lists you
may use.
"""

import csv
import struct
import sys
import unicodedata

MAX_NAME, MAX_CALL, MAX_TYPE = 56, 11, 23


def ascii_text(s, n):
    s = unicodedata.normalize("NFKD", s or "")
    s = "".join(c for c in s if 32 <= ord(c) < 127)
    s = " ".join(s.split())
    return s[:n].strip()


def num(s, lo, hi, scale=1.0):
    try:
        v = float(s)
    except (TypeError, ValueError):
        return 0
    v = int(round(v * scale))
    return v if lo <= v <= hi else 0


def first(row, *keys):
    for k in keys:
        v = row.get(k)
        if v and v.strip():
            return v.strip()
    return ""


def ship_mmsi(m):
    """a ship (MID first) or a coast station (00 + MID)"""
    return len(m) == 9 and m.isdigit() and (m[0] in "234567" or m[:2] == "00")


def add_extra(ships, path):
    """extra list: add new MMSIs, complete known ones; returns (added, completed)"""
    added = completed = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        line = line.strip()
        if not line or "=" in line[:12]:            # "Version = ...", "Date= ..."
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) >= 4 and ship_mmsi(parts[0]) and len(parts) == 4:
            m, name, call, typ = parts                          # ship list
        elif len(parts) >= 5 and ship_mmsi(parts[0]):
            m, name, call = parts[0], parts[2], ""                  # coast list
            typ = "Coast station" if m[:2] == "00" else "Training/test site"
        elif len(parts) == 1 and ship_mmsi(line[:9]):
            m, name, call, typ = line[:9], line[9:], "", "Training/test site"   # special list
        else:
            continue
        rec = {
            "name": ascii_text(name, MAX_NAME).upper(),
            "call": ascii_text(call, MAX_CALL).upper().replace(" ", ""),
            "type": ascii_text(typ, MAX_TYPE),
        }
        if not rec["name"]:
            continue
        old = ships.get(int(m))
        if old is None:
            ships[int(m)] = (sum(1 for k in ("call", "type") if rec[k]), 0, rec)
            added += 1
        elif old[2]["name"] == rec["name"]:
            r = old[2]
            filled = False
            for k in ("call", "type"):
                if not r[k] and rec[k]:
                    r[k] = rec[k]
                    filled = True
            completed += filled
    return added, completed


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: make_ships_db.py vessels.csv out.bin [extra lists...]")
    csv.field_size_limit(1 << 24)
    ships = {}
    rows = bad = no_mmsi = no_name = 0
    with open(sys.argv[1], newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            rows += 1
            m = first(row, "mmsi").replace(" ", "")
            if m.endswith(".0"):            # "235095435.0" from spreadsheet exports
                m = m[:-2]
            # ship and coast stations: 9 digits, MID first or 00 + MID
            if not ship_mmsi(m):
                bad += 1
                no_mmsi += 1
                continue
            name = ascii_text(first(row, "ship_name"), MAX_NAME)
            if m[:2] != "00":               # coast names keep "+2 sites"
                name = name.upper()
            if not name:
                bad += 1
                no_name += 1
                continue
            rec = {
                "name": name,
                "call": ascii_text(first(row, "call_sign"), MAX_CALL).upper().replace(" ", ""),
                "type": ascii_text(first(row, "ship_type", "gfw_ship_type"), MAX_TYPE),
            }
            score = sum(1 for k in ("call", "type") if rec[k])
            old = ships.get(int(m))
            # MMSIs get reused: keep the most complete record, then the newest ship
            year = num(first(row, "year_built"), 1850, 2100)
            if old is None or (score, year) > (old[0], old[1]):
                ships[int(m)] = (score, year, rec)

    base_n = len(ships)
    for extra in sys.argv[3:]:
        a, c = add_extra(ships, extra)
        print("DSC ships: %s: %d new MMSIs, %d records completed" % (extra, a, c))
    if len(sys.argv) > 3:
        print("DSC ships: %d from vessels.csv, %d in total" % (base_n, len(ships)))

    keys = sorted(ships)
    n = len(keys)
    strings = bytearray()
    index = bytearray()
    base = 16 + 8 * n
    for m in keys:
        r = ships[m][2]
        off = base + len(strings)
        strings += (r["name"] + "\0" + r["call"] + "\0" + r["type"] + "\0").encode("ascii")
        index += struct.pack("<II", m, off)
    with open(sys.argv[2], "wb") as f:
        f.write(b"DSCS" + struct.pack("<III", 2, n, 0))
        f.write(index)
        f.write(strings)
    print("DSC ships: %d rows read, %d skipped (%d without a ship MMSI, %d without a name), "
          "%d ships written (%.1f MB)"
          % (rows, bad, no_mmsi, no_name, n, (base + len(strings)) / 1e6))
    if n == 0:
        sys.exit("DSC ships: no usable rows - check the 'mmsi' and 'ship_name' columns")


if __name__ == "__main__":
    main()
