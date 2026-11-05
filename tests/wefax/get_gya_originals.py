#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Download the original pictures of the Northwood (GYA) radio fax charts from
the JOMOC web site, for the WEFAX denoiser (originals to pair with received
faxes, and pictures for the training).

The site keeps only the latest picture of each chart, under the same file
name: run this script often (every hour) and it keeps every new picture,
named after its creation time:

    OUTPUT/2026-10-22/20261022-1238_HFFAX_Polar_AA_SfcAnalysis_UNCL_1200_T000.bmp

A picture already kept is not downloaded again.

    get_gya_originals.py [--out FOLDER] [--area North_Atlantic] [--html FILE]

--html reads a saved copy of the page instead of the site (test).
Python 3 standard library only.
"""

import argparse
import datetime
import os
import re
import sys
import time
import urllib.request

SITE = "http://www.jomoc.net/HF-Fax/"

# title='Name: ...\nSize: ...\nProduct Created: 22-10-2026 12:38\nFile: X.bmp'
RE_MATRIX = re.compile(r"Product Created:\s*(\d\d)-(\d\d)-(\d{4})\s+(\d\d):(\d\d)\s*File:\s*([^'\"\s]+)")
# Other products: <a href='X.bmp'> ... <td>08:10 22-10-2026</td>
RE_OTHER = re.compile(r"<a href='([^'/]+\.(?:bmp|png|gif))'>.*?<td[^>]*>[^<]*</td>\s*<td[^>]*>(\d\d):(\d\d) (\d\d)-(\d\d)-(\d{4})</td>",
                      re.S | re.I)


def fetch(url, tries=3):
    for k in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                return r.read()
        except Exception as e:  # network errors: try again a little later
            if k == tries - 1:
                raise
            print(f"  retry {url}: {e}", file=sys.stderr)
            time.sleep(10)


def products(html):
    """(created datetime, file name) of every picture on the page"""
    out = {}
    for d, m, y, hh, mm, name in RE_MATRIX.findall(html):
        out[name] = datetime.datetime(int(y), int(m), int(d), int(hh), int(mm))
    for name, hh, mm, d, m, y in RE_OTHER.findall(html):
        out.setdefault(name, datetime.datetime(int(y), int(m), int(d), int(hh), int(mm)))
    return sorted(out.items(), key=lambda kv: kv[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.expanduser("~/x6100/gya_originals"))
    ap.add_argument("--area", default="North_Atlantic")
    ap.add_argument("--html", help="saved page to read instead of the site (test)")
    a = ap.parse_args()

    base = SITE + a.area + "/"
    if a.html:
        html = open(a.html, encoding="utf-8", errors="replace").read()
    else:
        html = fetch(base + "matrix.html").decode("utf-8", "replace")

    new = 0
    for name, created in products(html):
        folder = os.path.join(a.out, created.strftime("%Y-%m-%d"))
        path = os.path.join(folder, created.strftime("%Y%m%d-%H%M_") + name)
        if os.path.exists(path):
            continue
        if a.html:
            print("would download", path)
            continue
        try:
            data = fetch(base + urllib.request.quote(name))
        except Exception as e:
            print(f"FAILED {name}: {e}", file=sys.stderr)
            continue
        os.makedirs(folder, exist_ok=True)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
        new += 1
        print(f"{path}  {len(data) // 1024} kB")
    print(f"{datetime.datetime.now():%Y-%m-%d %H:%M}  {a.area}: {new} new pictures")


if __name__ == "__main__":
    main()
