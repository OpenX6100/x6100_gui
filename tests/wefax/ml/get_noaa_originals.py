#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Downloads the radiofax charts of the US National Weather Service (Boston,
New Orleans, Point Reyes, Kodiak, Honolulu: the files at
https://tgftp.nws.noaa.gov/fax/) and keeps them as originals for
prepare_pairs.py.

    get_noaa_originals.py [--out originals/noaa]

The TIFF files are the very pictures that are faxed: 1 bit, 1728 pixels a
line. They are put in the 1809-pixel fax line as the radio receives them
(charts sent sideways are turned), saved as NOAA_<name>_<UTC time>.bmp.
The files on the site are replaced by new charts every few hours: run it
again later (every 6 hours is enough); a chart already kept is not kept
twice.
"""
import argparse
import datetime
import glob
import hashlib
import io
import os
import re
import urllib.request

try:
    # the certificates of Windows (or macOS): the ones of Python may miss the
    # chain of some sites ("unable to get local issuer certificate")
    import truststore
    truststore.inject_into_ssl()
except ImportError:
    pass

import numpy as np
from PIL import Image

SITE = "https://tgftp.nws.noaa.gov/fax/"
WIDTH = 1809        # pixels of the fax line
LINE = 1728         # pixels of the picture in the line (IOC 576)
OFFSET = (WIDTH - LINE) // 2


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def to_fax(data):
    """the chart as received: 1809 pixels a line, uint8 0 black .. 255 white,
    or None when it is not a 1728-pixel fax picture"""
    im = Image.open(io.BytesIO(data))
    w, h = im.size
    if w != LINE:
        if h != LINE:
            return None         # a small picture, not sent at this size
        im = im.rotate(90, expand=True)
    a = np.array(im.convert("L"))
    out = np.full((a.shape[0], WIDTH), 255, np.uint8)
    out[:, OFFSET:OFFSET + LINE] = np.where(a < 128, 0, 255)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("originals", "noaa"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    # charts already kept, by content
    kept = set()
    for f in glob.glob(os.path.join(a.out, "*.bmp")):
        kept.add(hashlib.sha1(np.array(Image.open(f).convert("L")).tobytes()).hexdigest())

    listing = get(SITE).decode("latin-1")
    names = sorted(set(re.findall(r'href="([A-Za-z0-9_]+\.TIF)"', listing, re.I)))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M")
    print(f"{len(names)} charts on the site, {len(kept)} already kept", flush=True)
    new = 0
    for i, n in enumerate(names, 1):
        print(f"\r[{i}/{len(names)}] {n}   ", end="", flush=True)
        try:
            pic = to_fax(get(SITE + n))
        except Exception as e:
            print("skip", n, e)
            continue
        if pic is None:
            continue
        key = hashlib.sha1(pic.tobytes()).hexdigest()
        if key in kept:
            continue
        kept.add(key)
        Image.fromarray(pic).convert("1").save(
            os.path.join(a.out, f"NOAA_{os.path.splitext(n)[0]}_{stamp}.bmp"))
        new += 1
    print()
    print(f"{len(names)} charts on the site, {new} new in {a.out}")


if __name__ == "__main__":
    main()
