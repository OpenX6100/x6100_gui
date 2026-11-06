#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Writes a trained network (models/*.pt of train.py) as the model file of the
radio (src/wefax/wefax_pp.h): wefax_pp.bin, to be put on the SD card as
/usr/share/x6100/wefax_pp.bin.

    export_model.py MODEL.pt [OUT.bin]
"""
import argparse
import struct

import numpy as np
import torch

from model import WefaxUNet

ORDER = ["e1", "e2", "e3", "mid", "d3", "d2", "d1"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model")
    ap.add_argument("out", nargs="?", default="wefax_pp.bin")
    a = ap.parse_args()

    ck = torch.load(a.model, map_location="cpu")
    net = WefaxUNet(**ck["cfg"])
    net.load_state_dict(ck["model"])
    base = ck["cfg"]["base"]

    convs = []
    for name in ORDER:
        blk = getattr(net, name)
        convs += [blk[0], blk[2]]          # Conv, ReLU, Conv, ReLU
    convs.append(net.out)

    with open(a.out, "wb") as f:
        f.write(b"WFXUNET1")
        f.write(struct.pack("<I", base))
        for c in convs:
            w = c.weight.detach().float().numpy()
            b = c.bias.detach().float().numpy()
            cout, cin, k, _ = w.shape
            f.write(struct.pack("<III", cin, cout, k))
            f.write(np.ascontiguousarray(w, dtype="<f4").tobytes())
            f.write(np.ascontiguousarray(b, dtype="<f4").tobytes())
    n = sum(p.numel() for p in net.parameters())
    print(f"{a.out}: base {base}, {n:,} numbers, step {ck.get('step')}")


if __name__ == "__main__":
    main()
