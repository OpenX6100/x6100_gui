#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Xiegu X6100 - DSC flags file builder (run once, the result lives in rootfs/).

Reads a folder of flag images named with the ISO 3166 alpha-2 code
("it.png", "gr.png", "gf.jpeg" ...), scales each one to fit W x H keeping
its proportions, centres it on a transparent background and writes the
pixels already in the LVGL format of the screen (LV_COLOR_DEPTH 32,
LV_IMG_CF_TRUE_COLOR_ALPHA: B, G, R, A per pixel), read by
src/dsc/dsc_flags.c:

  header   16 bytes   "DSCF", u16 version (1), u16 count, u16 W, u16 H, u32 0
  index    count x 8 bytes, sorted by code: char code[2], u16 0, u32 offset
  pixels   count x W x H x 4 bytes

All integers little endian.  Usage (from the x6100_gui folder):

  python3 src/dsc/make_flags_db.py flags/ rootfs/usr/share/x6100/dsc_flags.bin

Needs Pillow (pip install pillow).
"""

import os
import re
import struct
import sys

from PIL import Image

W, H = 36, 24

# not countries: Earth, Asia-Pacific, European Union
SKIP = {"zz", "ap", "eu"}


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: make_flags_db.py flags_folder out.bin")
    src = sys.argv[1]
    flags = {}
    for f in sorted(os.listdir(src)):
        m = re.match(r"^([a-z]{2})\.(png|jpe?g)$", f, re.I)
        if not m or m.group(1).lower() in SKIP:
            continue
        code = m.group(1).lower()
        im = Image.open(os.path.join(src, f)).convert("RGBA")
        im.thumbnail((W, H), Image.LANCZOS)
        canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        canvas.paste(im, ((W - im.width) // 2, (H - im.height) // 2), im)
        r, g, b, a = canvas.split()
        flags[code] = Image.merge("RGBA", (b, g, r, a)).tobytes()     # B, G, R, A

    codes = sorted(flags)
    n = len(codes)
    base = 16 + 8 * n
    index = bytearray()
    data = bytearray()
    for c in codes:
        index += c.encode("ascii") + struct.pack("<HI", 0, base + len(data))
        data += flags[c]
    with open(sys.argv[2], "wb") as f:
        f.write(b"DSCF" + struct.pack("<HHHHI", 1, n, W, H, 0))
        f.write(index)
        f.write(data)
    print("DSC flags: %d flags of %dx%d written (%.0f KB)" % (n, W, H, (base + len(data)) / 1024))


if __name__ == "__main__":
    main()
