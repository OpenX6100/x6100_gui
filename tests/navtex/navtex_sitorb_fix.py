#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Marks as SITOR-B messages the bulletins saved before 1KO125 knew them
(SITOR-B messages sent without ZCZC / NNNN, flagged "start and end lost").

    navtex_sitorb_fix.py FOLDER [--dry-run]

FOLDER holds the bul-*.txt files of the radio (/mnt/navtex), copied to the
computer with the dates kept:

    scp -p -r root@RADIO:/mnt/navtex ~/navtex
    ./navtex_sitorb_fix.py ~/navtex
    scp -p ~/navtex/bul-*.txt root@RADIO:/mnt/navtex/

A file is changed only if it has no header, start and end lost, and its
text starts with a date-time group ("190730 UTC JUN 26"): it gets
"KIND SITORB" and "LOST 0 0".  Its text and its date stay as they were
(the radio lists the bulletins by the date of their file).  A bulletin
whose NNNN was received is left alone: a NAVTEX bulletin whose header was
lost.
"""
import argparse
import glob
import os
import sys

FIGS = "PQWERTYUIO"         # digits received as letters: Q = 1 ... P = 0


def is_digit(c):
    return c.isdigit() or c in FIGS or c == "*"


def is_dtg(line):
    """as is_dtg() of src/navtex/bulletin.c"""
    s = line.lstrip(" ")
    if len(s) < 7 or sum(is_digit(c) for c in s[:6]) < 5 or s[6] not in " *":
        return False
    w = s[6:].lstrip(" ")[:3]
    if len(w) < 3:
        return False
    return any(sum(a == b for a, b in zip(w, z)) >= 2 for z in ("UTC", "LMT", "GMT"))


def fix(path, dry_run):
    with open(path, "rb") as f:
        data = f.read()
    head, sep, rest = data.partition(b"\nLEN ")
    if not sep or not head.startswith(b"NAVTEX-BULLETIN"):
        return "not a bulletin"
    lines = head.decode("latin-1").split("\n")
    if any(l.startswith("KIND ") for l in lines):
        return None
    if "KNOWN 0 0 0" not in lines or "LOST 1 1" not in lines:
        return None
    n, _, text = rest.partition(b"\n")
    body = text[:int(n)].decode("latin-1")
    first = next((l for l in body.split("\n") if l.strip(" ")), "")
    if not is_dtg(first):
        return None
    i = lines.index("LOST 1 1")
    lines[i:i + 1] = ["LOST 0 0", "KIND SITORB"]
    if not dry_run:
        st = os.stat(path)
        with open(path, "wb") as f:
            f.write("\n".join(lines).encode("latin-1") + sep + rest)
        os.utime(path, (st.st_atime, st.st_mtime))
    return "SITOR-B: " + first.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--dry-run", action="store_true", help="only tell what would change")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.folder, "bul-*.txt")))
    if not files:
        sys.exit(f"no bul-*.txt files in {a.folder}")
    changed = 0
    for p in files:
        r = fix(p, a.dry_run)
        if r:
            print(os.path.basename(p), "-", r)
            changed += r.startswith("SITOR-B")
    print(f"{len(files)} bulletins, {changed} {'to be ' if a.dry_run else ''}marked SITOR-B")


if __name__ == "__main__":
    main()
