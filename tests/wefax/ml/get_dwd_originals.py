#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Downloads the black and white analysis charts of the DWD that are faxed by
DWD Hamburg (open data: https://opendata.dwd.de/weather/charts/analysis/)
and keeps them as originals for prepare_pairs.py, through
dwd_to_original.py.

    get_dwd_originals.py [--out originals/dwd]

- surface analysis North Atlantic, Europe ("Boden", every 6 hours)
- sea chart with fronts, WKN/INTSF ... (00 and 12 UTC), sent sideways

The site keeps about two days: run it once a day (or more) to collect
them. A chart already kept is not downloaded again.
"""
import argparse
import os
import re
import tempfile
import urllib.request

try:
    # the certificates of Windows (or macOS): the ones of Python may miss the
    # chain of some sites ("unable to get local issuer certificate")
    import truststore
    truststore.inject_into_ssl()
except ImportError:
    pass

import dwd_to_original

SITE = "https://opendata.dwd.de/weather/charts/analysis/"
WANTED = re.compile(r"tka01%2Cana_bwkman_(dwdna_O_\w+_WV12SW|dwdsk_O_\w+_WVHASW)\.png")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("originals", "dwd"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    with urllib.request.urlopen(SITE, timeout=60) as r:
        listing = r.read().decode("latin-1")
    names = sorted(set(n for n in re.findall(r'href="([^"]+\.png)"', listing)
                       if WANTED.search(n) and "LATEST" not in n))
    new = 0
    for n in names:
        kind, when = re.search(r"(dwdna|dwdsk)_O_\d+_\d+_(\d{12})", n).groups()
        dst = os.path.join(a.out, f"DWD_{kind}_{when}.bmp")
        if os.path.exists(dst):
            continue
        with tempfile.TemporaryDirectory() as td:
            png = os.path.join(td, f"{kind}.png")
            with urllib.request.urlopen(SITE + n, timeout=120) as r, open(png, "wb") as f:
                f.write(r.read())
            try:
                dwd_to_original.convert(png).save(dst)
            except Exception as e:
                print("skip", n, e)
                continue
        new += 1
        print("ok  ", os.path.basename(dst))
    print(f"{len(names)} charts on the site, {new} new in {a.out}")


if __name__ == "__main__":
    main()
