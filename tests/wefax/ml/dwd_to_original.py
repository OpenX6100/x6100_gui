#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Turns the charts of the DWD (the PNG files of the web site, about 4400 pixels
wide) into originals for prepare_pairs.py: black and white, at the size the
radio receives them.

    dwd_to_original.py FILES_OR_FOLDERS... [--out originals/dwd]

Measured on faxes of DWD Hamburg received by the X6100:
- the surface charts ("Boden", files dwdna): frame 1638 pixels wide in the
  1809-pixel fax line, from pixel 50, rows about 1 % taller;
- the sea chart with fronts and WKN/INTSF (files dwdsk): sent sideways,
  turned a quarter, 1690 pixels wide from pixel 40.
A file already done is left alone.
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image
from scipy import ndimage

WIDTH = 1809        # pixels of the fax line
FRAME_W = 1638      # pixels of the chart frame in the fax line
FRAME_X = 50        # first pixel of the frame
ROW_STRETCH = 1.01  # rows a little taller than the columns
MARGIN = 16         # white rows above and below


def convert(path, sideways=None):
    """sideways: None = by the file name (dwdsk charts are sent sideways)"""
    if sideways is None:
        sideways = "dwdsk" in os.path.basename(path)
    frame_w, frame_x = (1690, 40) if sideways else (FRAME_W, FRAME_X)
    im = Image.open(path)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        white = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(white, im)
    im = im.convert("L")
    if sideways:
        im = im.rotate(90, expand=True)
    w, h = im.size
    if w < 1500 or (w < h) != sideways:
        raise ValueError(f"{w} x {h}: not a DWD chart (too small, or upright)")
    nh = round(h * frame_w / w * ROW_STRETCH)
    small = np.array(im.resize((frame_w, nh), Image.LANCZOS))
    out = np.full((nh + 2 * MARGIN, WIDTH), 255, np.uint8)
    # black: the dark pixels, and the gray edges next to them (the faxed
    # charts are bolder); gray areas (land, fills) stay white
    dark = small < 128
    black = dark | ((small < 230) & ndimage.binary_dilation(dark))
    out[MARGIN:MARGIN + nh, frame_x:frame_x + frame_w] = np.where(black, 0, 255)
    return Image.fromarray(out).convert("1")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--out", default=os.path.join("originals", "dwd"))
    a = ap.parse_args()
    files = []
    for p in a.inputs:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, "**", "*.png"), recursive=True) +
                            glob.glob(os.path.join(p, "**", "*.PNG"), recursive=True))
        else:
            files.append(p)
    os.makedirs(a.out, exist_ok=True)
    done = 0
    for f in files:
        dst = os.path.join(a.out, "DWD_" + os.path.splitext(os.path.basename(f))[0] + ".bmp")
        if os.path.exists(dst):
            continue
        try:
            convert(f).save(dst)
            done += 1
            print("ok  ", os.path.basename(dst))
        except Exception as e:
            print("skip", os.path.basename(f), e)
    print(f"{done} new originals in {a.out}")


if __name__ == "__main__":
    main()
