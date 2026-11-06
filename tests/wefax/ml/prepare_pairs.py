#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Training pairs for the WEFAX post-processing network: every original picture (JOMOC BMP)
goes through the simulated HF channel several times, each time with other
random conditions (from the measured ones down to almost no signal), and
through the decoder of the firmware, Denoise ON or OFF at random.

    prepare_pairs.py ORIGINALS_FOLDER PAIRS_FOLDER [--per 20] [--jobs N] [--exclude SETS_FOLDER]

--exclude leaves out every original that is also in a FAX SET (same
picture, whatever the file name): those are kept for test_real.py, and a
network trained on them would look better on the test than it is.

Each pair is PAIRS_FOLDER/<name>_<k>.npz with
    rx      uint8 1809 x 1293, the received fax, aligned with the original
    orig    uint8 1809 x 1293, the original (0 black, 255 white)
    params  the channel used (text)
Pictures already done are skipped, so the script can be run again when
new originals arrive. ORIGINALS_FOLDER is searched in subfolders too.
"""
import argparse
import glob
import hashlib
import os
import random
import sys
import tempfile
import zlib
from concurrent.futures import ProcessPoolExecutor

import numpy as np

import wfml


def random_channel(rng, denoise_only=False):
    """conditions around the measured Northwood recordings (8-9 dB SNR, two
    close paths of similar power, line jitter), much wider both ways: down
    to almost no signal, with deep fades where the fax sinks into the noise"""
    p = ["--snr", f"{rng.uniform(0, 15):.1f}"]
    paths = rng.choices([1, 2, 3], [0.1, 0.6, 0.3])[0]
    p += ["--paths", str(paths)]
    if paths > 1:
        p += ["--scatter", f"{rng.uniform(-9, -1):.1f}",
              "--delay", f"{rng.uniform(0.15, 1.2):.2f}",
              "--doppler", f"{10 ** rng.uniform(-1, 0.2):.2f}"]
    p += ["--jitter", f"{rng.uniform(0.3, 2.5):.1f}"]
    if rng.random() < 0.5:
        p += ["--clicks", f"{rng.uniform(5, 60):.0f}"]
    if rng.random() < 0.5:
        p += ["--flat", f"{rng.uniform(8, 60):.0f}", "--depth", f"{rng.uniform(5, 30):.0f}"]
    p += ["--off", f"{rng.uniform(-15, 15):.1f}", "--seed", str(rng.randrange(1 << 30))]
    # both decoders of the radio: Denoise ON (now the default) and OFF (the
    # classic one, every fax saved before 1.0.2 beta 6); their noise differs
    if rng.random() < 0.5 or denoise_only:
        p.append("--denoise")
    return p


def one(job):
    try:
        return make_pair(*job)
    except Exception as e:      # one bad picture must not stop the others
        return f"FAILED {job[1]}: {e}"


def make_pair(orig_path, out_path, seed, denoise_only=False):
    rng = random.Random(seed)
    try:
        orig = wfml.load_original(orig_path)
    except ValueError as e:
        return f"skip {e}"
    with tempfile.TemporaryDirectory() as td:
        pgm_in = os.path.join(td, "in.pgm")
        pgm_out = os.path.join(td, "out.pgm")
        from PIL import Image
        Image.open(orig_path).convert("L").save(pgm_in)
        params = random_channel(rng, denoise_only)
        # Manual RX: a weak signal whose start tone is lost is still a pair
        r = wfml.run([wfml.tool("wefax_pair"), pgm_in, pgm_out, *params, "--cont"])
        if r.returncode != 0 or not os.path.exists(pgm_out):
            return f"FAILED {out_path}: {r.stderr.strip()[-200:]}"
        rx = wfml.load_gray(pgm_out)
    try:
        dx, dy, cmin, cmax = wfml.align(orig, rx)
    except ValueError as e:
        return f"skip {os.path.basename(out_path)}: {e}, {' '.join(params)}"
    if cmax < 0.3:
        return f"skip {os.path.basename(out_path)}: not aligned (corr {cmax:.2f}), {' '.join(params)}"
    rx = wfml.cut(rx, dx, dy, orig.shape[0])
    np.savez_compressed(out_path + ".tmp.npz", rx=rx, orig=orig, params=" ".join(params))
    os.replace(out_path + ".tmp.npz", out_path + ".npz")
    return f"{os.path.basename(out_path)}  err {wfml.gray_error(rx, orig):5.1f}  corr {cmin:.2f}..{cmax:.2f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("originals")
    ap.add_argument("pairs")
    ap.add_argument("--per", type=int, default=20, help="pairs per original (default 20)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--denoise-only", action="store_true",
                    help="all pairs with the Denoise ON decoder (named NAME_dNN, next to the others)")
    ap.add_argument("--exclude", help="FAX SETs folder: its originals are not used")
    a = ap.parse_args()

    files = sorted(set(glob.glob(os.path.join(a.originals, "**", "*.bmp"), recursive=True) +
                       glob.glob(os.path.join(a.originals, "**", "*.BMP"), recursive=True)))
    if not files:
        sys.exit(f"no .bmp in {a.originals}")

    def digest(f):
        with open(f, "rb") as fh:
            return hashlib.sha1(fh.read()).hexdigest()

    if a.exclude:
        test = {digest(f) for f in glob.glob(os.path.join(a.exclude, "**", "*.bmp"), recursive=True)}
        before = len(files)
        files = [f for f in files if digest(f) not in test]
        print(f"{before - len(files)} originals left out: they are in the FAX SETs ({len(test)} there)")
    else:
        print("WARNING: no --exclude: originals of the FAX SETs, if any, are used for training too")
    os.makedirs(a.pairs, exist_ok=True)
    jobs = []
    for f in files:
        name = os.path.splitext(os.path.basename(f))[0]
        tag = "d" if a.denoise_only else ""
        for k in range(a.per):
            out = os.path.join(a.pairs, f"{name}_{tag}{k:02d}")
            if not os.path.exists(out + ".npz"):
                jobs.append((f, out, zlib.crc32(f"{name}/{tag}{k}".encode()), a.denoise_only))
    print(f"{len(files)} originals, {len(jobs)} pairs to make, {a.jobs} at a time")
    with ProcessPoolExecutor(a.jobs) as ex:
        for i, msg in enumerate(ex.map(one, jobs), 1):
            print(f"[{i}/{len(jobs)}] {msg}", flush=True)


if __name__ == "__main__":
    main()
