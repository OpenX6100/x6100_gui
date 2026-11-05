#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
The WEFAX post-processing network on real faxes: the FAX SETs (a folder each, with the
recording REC_*.wav and the original *.bmp from JOMOC).

    test_real.py SETS_FOLDER [--model models/wefax_unet.pt] [--out results]

Each recording is decoded again by the decoder of the firmware (Denoise
ON), aligned with the original and passed through the network. Printed:
the error (mean gray difference from the original, 0..255) of the received
fax and of the cleaned one, on the whole fax ("all") and on its text only
("text": the character-sized shapes of the original, compared softened and
 word by word within 2 pixels, see wfml.text_error;
--text-png shows them in red). Written in --out, for each set: the cleaned
fax (PNG) and a side by side of original / received / cleaned.
"""
import argparse
import glob
import os
import tempfile

import numpy as np
import torch
from PIL import Image

import wfml
from model import WefaxUNet, run_full


def rows(path):
    """rows of a decoded fax, 0 if there is none (no start tone: 0 rows)"""
    try:
        return wfml.load_gray(path).shape[0]
    except Exception:
        return 0


def decode(wav, out):
    """Auto RX as on the radio; Manual RX if the start tone is not in the recording"""
    wfml.run([wfml.tool("wefax_wav"), wav, out, "--denoise"])
    if rows(out) > 200:
        return "Auto RX"
    wfml.run([wfml.tool("wefax_wav"), wav, out, "--denoise", "--cont"])
    return "Manual RX" if rows(out) > 200 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sets")
    ap.add_argument("--model", default=os.path.join(wfml.KIT, "models", "wefax_unet.pt"))
    ap.add_argument("--out", default=os.path.join(wfml.KIT, "results"))
    ap.add_argument("--min-corr", type=float, default=0.5,
                    help="below this the recording is not the fax of the original: set left out (0.5)")
    ap.add_argument("--text-png", action="store_true", help="also write NAME_textmask.png: the text found, in red")
    a = ap.parse_args()

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    ck = torch.load(a.model, map_location=device)
    model = WefaxUNet(**ck["cfg"]).to(device)
    model.load_state_dict(ck["model"])
    os.makedirs(a.out, exist_ok=True)

    rows = []
    for d in sorted(glob.glob(os.path.join(a.sets, "**", ""), recursive=True)):
        wavs = glob.glob(os.path.join(d, "*.wav"))
        origs = [f for f in glob.glob(os.path.join(d, "*.bmp"))]
        if len(wavs) != 1 or len(origs) != 1:
            continue
        name = os.path.basename(os.path.normpath(d))
        try:
            orig = wfml.load_original(origs[0])
        except ValueError as e:
            print(f"{name}: {e}")
            continue
        with tempfile.TemporaryDirectory() as td:
            pgm = os.path.join(td, "rx.pgm")
            mode = decode(wavs[0], pgm)
            if mode is None:
                print(f"{name}: no fax decoded from {os.path.basename(wavs[0])}")
                continue
            rx = wfml.load_gray(pgm)
        try:
            dx, dy, cmin, cmax = wfml.align(orig, rx)
        except ValueError as e:
            print(f"{name}: {e}")
            continue
        if cmax < a.min_corr:
            print(f"{name:24s} {mode:9s} corr {cmax:.2f}  the recording does not match the original: left out")
            continue
        rx = wfml.cut(rx, dx, dy, orig.shape[0])
        p = run_full(model, rx, device)
        ai = (255 * (1 - p)).astype(np.uint8)
        e_rx, e_ai = wfml.gray_error(rx, orig), wfml.gray_error(ai, orig)
        tm = wfml.text_mask(orig)
        t_rx, t_ai = wfml.text_error(rx, orig, tm), wfml.text_error(ai, orig, tm)
        rows.append((name, e_rx, e_ai, t_rx, t_ai))
        print(f"{name:24s} {mode:9s} corr {cmax:.2f}  all: {e_rx:5.1f} -> {e_ai:5.1f} ({100 * (e_ai - e_rx) / e_rx:+3.0f} %)"
              f"   text: {t_rx:5.1f} -> {t_ai:5.1f} ({100 * (t_ai - t_rx) / t_rx:+3.0f} %)", flush=True)
        if a.text_png:
            ov = np.stack([orig] * 3, -1).copy()
            ov[tm] = (ov[tm] * 0.5 + np.array([255, 0, 0]) * 0.5).astype(np.uint8)
            Image.fromarray(ov).save(os.path.join(a.out, name + "_textmask.png"))
        Image.fromarray(ai).save(os.path.join(a.out, name + "_postproc.png"))
        side = np.concatenate([orig, np.full((orig.shape[0], 8), 128, np.uint8), rx,
                               np.full((orig.shape[0], 8), 128, np.uint8), ai], 1)
        Image.fromarray(side).save(os.path.join(a.out, name + "_compare.png"))
    if rows:
        m = lambda k: float(np.nanmean([r[k] for r in rows]))
        print(f"{'mean':24s} {'':9s} {'':9s}  all: {m(1):5.1f} -> {m(2):5.1f} ({100 * (m(2) - m(1)) / m(1):+3.0f} %)"
              f"   text: {m(3):5.1f} -> {m(4):5.1f} ({100 * (m(4) - m(3)) / m(3):+3.0f} %)")


if __name__ == "__main__":
    main()
