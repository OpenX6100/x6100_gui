#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Cleans any received fax with the WEFAX post-processing network: no original needed.

    postproc.py FILE_OR_FOLDER... [--model models/wefax_unet.pt] [--out results]

Takes the faxes saved by the radio (PNG, as in /mnt/wefax), PGM, or
recordings (REC_*.wav, decoded here as on the radio: Denoise ON, Auto RX,
Manual RX if there is no start tone). Folders are searched in subfolders.
Writes in --out, for each fax:
    NAME_ai.png     the cleaned fax
    NAME_side.png   received and cleaned side by side
"""
import argparse
import glob
import os
import tempfile
import time

import numpy as np
import torch
from PIL import Image

import wfml
from model import WefaxUNet, run_full
from test_real import decode


def inputs(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for ext in ("png", "PNG", "pgm", "wav", "WAV"):
                out += glob.glob(os.path.join(p, "**", "*." + ext), recursive=True)
        else:
            out.append(p)
    return sorted(set(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--model", default=os.path.join(wfml.KIT, "models", "wefax_unet.pt"))
    ap.add_argument("--out", default=os.path.join(wfml.KIT, "results"))
    a = ap.parse_args()

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    ck = torch.load(a.model, map_location=device)
    model = WefaxUNet(**ck["cfg"]).to(device)
    model.load_state_dict(ck["model"])
    os.makedirs(a.out, exist_ok=True)

    for f in inputs(a.files):
        name = os.path.splitext(os.path.basename(f))[0]
        if name.endswith("_ai") or name.endswith("_side") or name.endswith("_compare") or name.endswith("_denoised") or name.endswith("_postproc"):
            continue        # our own results
        if f.lower().endswith(".wav"):
            with tempfile.TemporaryDirectory() as td:
                pgm = os.path.join(td, "rx.pgm")
                mode = decode(f, pgm)
                if mode is None:
                    print(f"{name}: no fax decoded")
                    continue
                rx = wfml.load_gray(pgm)
        else:
            try:
                rx = wfml.load_gray(f)
            except Exception as e:
                print(f"{name}: {e}")
                continue
            mode = "picture"
        if rx.shape[1] < 1000 or rx.shape[0] < 100:
            print(f"{name}: {rx.shape[1]} x {rx.shape[0]}, not a fax")
            continue
        t0 = time.time()
        ai = (255 * (1 - run_full(model, rx, device))).astype(np.uint8)
        Image.fromarray(ai).save(os.path.join(a.out, name + "_ai.png"))
        side = np.concatenate([rx, np.full((rx.shape[0], 8), 128, np.uint8), ai], 1)
        Image.fromarray(side).save(os.path.join(a.out, name + "_side.png"))
        print(f"{name:40s} {mode:9s} {rx.shape[1]} x {rx.shape[0]}  {time.time() - t0:4.1f} s", flush=True)


if __name__ == "__main__":
    main()
