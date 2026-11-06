#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Trains the WEFAX post-processing network on the pairs of prepare_pairs.py.

    train.py PAIRS_FOLDER [--out models/wefax_unet.pt] [--steps 30000]
                          [--text-weight 5] [--text-crops 0.5] [--base 16]

About 1 original in 10 (by name) is kept out of the training to check the
network on pictures it never saw; its errors (whole fax and text, see
wfml.text_error) are printed every --check steps and the network with the
lowest sum of the two is saved. Uses the Nvidia card when there is one.

The text matters most: the pixels of the text of the original count
--text-weight times more in the training, and --text-crops of the pieces
the network learns on are taken around text (wfml.text_mask: the
characters standing alone). --text-full also takes the numbers that touch
a line (wfml.text_mask_full); tried once (model 5): worse than without.
"""
import argparse
import glob
import os
import random
import time
import zlib

import numpy as np
import torch
import torch.nn.functional as F

import wfml
from model import WefaxUNet, run_full


def is_check(path):
    """pairs of about 1 original in 10 are kept for the check"""
    orig = os.path.basename(path).rsplit("_", 1)[0]
    return zlib.crc32(orig.encode()) % 10 == 0


def denoise_pair(path):
    """the pair was made with the Denoise ON decoder of the radio"""
    with np.load(path) as z:
        return "--denoise" in str(z["params"]).split()


def load(paths, limit):
    """the received faxes and their originals; the 20 versions of one
    original share one copy of it"""
    rx, orig = [], []
    seen = {}
    for p in paths[:limit]:
        with np.load(p) as z:
            rx.append(z["rx"])
            o = z["orig"]
        orig.append(seen.setdefault(zlib.crc32(o.tobytes()), o))
    return rx, orig


def find_text(origs, library, device):
    """the text of every original, once per original"""
    done = {}
    out = []
    for o in origs:
        if id(o) not in done:
            m = wfml.text_mask_full(o, library, device=device) if library is not None else wfml.text_mask(o)
            done[id(o)] = (m, np.argwhere(m))
        out.append(done[id(o)])
    return out


def batch(rx, orig, text, n, size, rng, text_crops):
    xs, ys, ts = [], [], []
    for _ in range(n):
        i = rng.randrange(len(rx))
        h, w = rx[i].shape
        mask, where = text[i]
        if len(where) and rng.random() < text_crops:
            cy, cx = where[rng.randrange(len(where))]   # a piece with text
            y0 = min(max(cy - rng.randrange(size), 0), h - size - 1)
            x0 = min(max(cx - rng.randrange(size), 0), w - size - 1)
        else:
            y0, x0 = rng.randrange(h - size), rng.randrange(w - size)
        a = rx[i][y0:y0 + size, x0:x0 + size]
        b = orig[i][y0:y0 + size, x0:x0 + size]
        t = mask[y0:y0 + size, x0:x0 + size]
        if rng.random() < 0.5:          # pictures are not mirror-symmetric, but
            a, b, t = a[:, ::-1], b[:, ::-1], t[:, ::-1]  # noise is: more variety for free
        xs.append(a)
        ys.append(b)
        ts.append(t)
    x = torch.from_numpy((255 - np.stack(xs).astype(np.float32)) / 255)[:, None]
    y = torch.from_numpy((np.stack(ys) < 128).astype(np.float32))[:, None]
    t = torch.from_numpy(np.stack(ts).astype(np.float32))[:, None]
    return x, y, t


def loss_fn(logit, y, t, text_weight):
    # black pixels and their surroundings (lines, text) count 4 times more,
    # the text of the original (and its margin) text_weight times more
    near = F.max_pool2d(y, 5, stride=1, padding=2)
    w = (1 + 3 * near) * (1 + (text_weight - 1) * t)
    return F.binary_cross_entropy_with_logits(logit, y, weight=w)


def check(model, rx, orig, text, device):
    """errors of the network (and, the first time, of the received faxes):
    whole fax and text"""
    errs, terrs = [], []
    for a, b, (m, _) in zip(rx, orig, text):
        p = 255 * (1 - run_full(model, a, device)) if model is not None else a
        errs.append(wfml.gray_error(p, b))
        terrs.append(wfml.text_error(p, b, m))
    return float(np.mean(errs)), float(np.nanmean(terrs))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pairs")
    ap.add_argument("--out", default=os.path.join(wfml.KIT, "models", "wefax_unet.pt"))
    ap.add_argument("--steps", type=int, default=30000)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--base", type=int, default=16, help="channels of the first level (16)")
    ap.add_argument("--check", type=int, default=1000)
    ap.add_argument("--max-pairs", type=int, default=4000, help="pairs kept in memory (2.3 MB each, twice)")
    ap.add_argument("--text-weight", type=float, default=5, help="how much more the text counts (5)")
    ap.add_argument("--text-crops", type=float, default=0.5, help="part of the pieces taken around text (0.5)")
    ap.add_argument("--text-full", action="store_true", help="also the numbers touching lines as text (model 5: worse)")
    ap.add_argument("--denoise-only", action="store_true",
                    help="only the pairs made with the Denoise ON decoder (postprocessing of Denoise ON faxes)")
    ap.add_argument("--resume", action="store_true")
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(a.pairs, "*.npz")))
    if a.denoise_only:
        files = [f for f in files if denoise_pair(f)]
        print(f"{len(files)} pairs made with the Denoise ON decoder")
    train_f = [f for f in files if not is_check(f)]
    check_f = [f for f in files if is_check(f)]
    if not train_f or not check_f:
        raise SystemExit(f"{len(files)} pairs: too few (some originals are needed for the check too)")
    random.Random(1).shuffle(train_f)
    rx, orig = load(train_f, a.max_pairs)
    crx, corig = load(check_f, 24)
    print(f"{len(rx)} training pairs, {len(crx)} check pairs")

    device = torch.device("cuda" if torch.cuda.is_available() else
                          "mps" if torch.backends.mps.is_available() else "cpu")
    print("device:", device, torch.cuda.get_device_name(0) if device.type == "cuda" else "")
    t0 = time.time()
    uniq = list({id(o): o for o in orig + corig}.values())
    library = wfml.glyph_library(uniq) if a.text_full else None
    fdev = device if device.type == "cuda" else torch.device("cpu")   # no FFT on mps
    text, ctext = find_text(orig, library, fdev), find_text(corig, library, fdev)
    print(f"text of {len(uniq)} originals found in {time.time() - t0:.0f} s")
    model = WefaxUNet(a.base).to(device)
    print(f"{sum(p.numel() for p in model.parameters()):,} parameters")
    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=a.steps, pct_start=0.05)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    start, best = 0, 1e9
    if a.resume and os.path.exists(a.out + ".last"):
        ck = torch.load(a.out + ".last", map_location=device)
        model.load_state_dict(ck["model"]); opt.load_state_dict(ck["opt"]); sched.load_state_dict(ck["sched"])
        start, best = ck["step"], ck["best"]
        print(f"resumed at step {start}, best check error {best:.2f}")

    r_all, r_text = check(None, crx, corig, ctext, device)
    use_amp = device.type == "cuda"
    rng = random.Random(start)
    t0, run_loss = time.time(), 0.0
    for step in range(start, a.steps):
        model.train()
        x, y, t = batch(rx, orig, text, a.batch, a.size, rng, a.text_crops)
        x, y, t = (v.to(device, non_blocking=True) for v in (x, y, t))
        with torch.autocast(device.type, dtype=torch.bfloat16, enabled=use_amp):
            logit = model(x)
        loss = loss_fn(logit.float(), y, t, a.text_weight)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        sched.step()
        run_loss += loss.item()
        if (step + 1) % a.check == 0 or step + 1 == a.steps:
            e_all, e_text = check(model, crx, corig, ctext, device)
            err = e_all + e_text
            tag = ""
            if err < best:
                best = err
                torch.save({"model": model.state_dict(), "cfg": model.cfg, "step": step + 1,
                            "check_error": e_all, "check_text_error": e_text}, a.out)
                tag = "  saved"
            torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "sched": sched.state_dict(),
                        "step": step + 1, "best": best}, a.out + ".last")
            print(f"step {step + 1:6d}  loss {run_loss / a.check:.4f}  check: all {e_all:5.2f} "
                  f"(received {r_all:5.2f})  text {e_text:5.2f} (received {r_text:5.2f})  "
                  f"{time.time() - t0:5.0f} s{tag}", flush=True)
            run_loss = 0.0
    print(f"best check error (all + text) {best:.2f}: {a.out}")


if __name__ == "__main__":
    main()
