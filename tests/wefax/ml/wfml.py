# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Shared code of the WEFAX post-processing kit: pictures, alignment of a received fax
with its original, scores, the C tools.
"""
import os
import platform
import subprocess

import numpy as np
from PIL import Image

KIT = os.path.dirname(os.path.abspath(__file__))
WIDTH = 1809            # pixels of the fax line (decoder)
PIC_H = 1293            # rows of the Northwood pictures


def tool(name):
    """path of a C tool: bin/<name>.exe on Windows, else ../<name> built by make"""
    if platform.system() == "Windows":
        p = os.path.join(KIT, "bin", name + ".exe")
    else:
        p = os.path.join(KIT, "bin", name)
        if not os.path.exists(p):
            p = os.path.join(KIT, "..", name)
    if not os.path.exists(p):
        raise SystemExit(f"{p} not found: see README.md, 'The C tools'")
    return p


def load_original(path):
    """the original (JOMOC, 1720 x 1293, 1 bit) stretched to the fax line:
    uint8 0 black .. 255 white, WIDTH x PIC_H"""
    im = Image.open(path).convert("L")
    if im.size[0] < 1000:
        raise ValueError(f"{path}: {im.size[0]} pixels wide, a preview, not an original")
    return np.array(im.resize((WIDTH, im.size[1]), Image.BOX))


def load_gray(path):
    return np.array(Image.open(path).convert("L"))


def _blur(a, r=2):
    """box blur, separable (stands for a gaussian in the alignment)"""
    k = np.ones(2 * r + 1, np.float32) / (2 * r + 1)
    a = np.apply_along_axis(lambda v: np.convolve(v, k, "same"), 1, a)
    return np.apply_along_axis(lambda v: np.convolve(v, k, "same"), 0, a)


def align(original, received, max_dy=140):
    """(dx, dy, worst correlation, best correlation): the received fax row
    dy + r, column (c + dx) mod WIDTH, is original row r, column c"""
    ob = _blur((255 - original.astype(np.float32)) / 255)
    rb = _blur((255 - received.astype(np.float32)) / 255)
    res = []
    h = original.shape[0]
    for y0 in range(100, h - 150, 250):
        a = ob[y0:y0 + 120]
        A = a - a.mean()
        FA = np.conj(np.fft.rfft2(A, s=(120, 2048)))
        best = None
        for dy in range(0, min(max_dy, received.shape[0] - y0 - 120)):
            b = rb[y0 + dy:y0 + dy + 120]
            B = b - b.mean()
            c = np.fft.irfft2(np.fft.rfft2(B, s=(120, 2048)) * FA, s=(120, 2048))[0]
            k = int(np.argmax(c))
            v = c[k] / np.sqrt((A * A).sum() * (B * B).sum() + 1e-9)
            if best is None or v > best[0]:
                best = (float(v), k if k < 1024 else k - 2048, dy)
        if best is not None:
            res.append(best)
    if not res:
        raise ValueError("received fax too short")
    dy = int(np.median([x[2] for x in res]))
    dx = int(np.median([x[1] for x in res]))
    return dx, dy, min(x[0] for x in res), max(x[0] for x in res)


def cut(received, dx, dy, rows):
    """the part of the received fax that matches the original rows"""
    r = np.roll(received, -dx, axis=1)[dy:dy + rows]
    if r.shape[0] < rows:
        r = np.vstack([r, np.full((rows - r.shape[0], r.shape[1]), 255, r.dtype)])
    return r


def gray_error(a, b, margin=20):
    """mean absolute gray difference (0..255) away from the borders"""
    a = a.astype(np.float32)[margin:-margin, margin:-margin]
    b = b.astype(np.float32)[margin:-margin, margin:-margin]
    return float(np.abs(a - b).mean())


def run(args, **kw):
    return subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, **kw)


def text_mask(original, dilate=2):
    """the pixels of the text of the original (and a margin around): the
    connected black shapes the size of a character (4..22 rows, at most
    30 columns), not lines, fronts or coasts, which are longer"""
    from scipy import ndimage
    black = original < 128
    lab, n = ndimage.label(black, structure=np.ones((3, 3)))
    if n == 0:
        return np.zeros_like(black)
    sl = ndimage.find_objects(lab)
    keep = np.zeros(n + 1, bool)
    for i, s in enumerate(sl, 1):
        h = s[0].stop - s[0].start
        w = s[1].stop - s[1].start
        if 4 <= h <= 22 and w <= 30:
            keep[i] = True
    m = keep[lab]
    # characters come in words: keep shapes with a neighbour of the same kind
    near = ndimage.binary_dilation(m, iterations=6)
    lab2, n2 = ndimage.label(near)
    counts = np.bincount(lab2[m], minlength=n2 + 1)
    m &= counts[lab2] >= 2 * 8      # at least about two characters
    return ndimage.binary_dilation(m, iterations=dilate) if dilate > 0 else m


def glyph_library(originals, min_count=2):
    """the characters of the charts: the character-sized shapes of
    text_mask that stand alone somewhere (same pixels, seen at least
    min_count times in all the originals). Used by text_mask_full to find
    the same characters where they touch a line or a coast"""
    from scipy import ndimage
    count, shape = {}, {}
    for o in originals:
        b = o < 128
        m = text_mask(o, dilate=0) & b
        lab, n = ndimage.label(m, structure=np.ones((3, 3)))
        for i, s in enumerate(ndimage.find_objects(lab), 1):
            g = lab[s] == i
            if g.shape[0] < 6 or g.shape[1] < 4 or g.sum() < 10:
                continue            # dots, dashes: pieces of lines look the same
            k = bytes(g.shape) + np.packbits(g).tobytes()
            count[k] = count.get(k, 0) + 1
            shape[k] = g
    return [shape[k] for k in count if count[k] >= min_count]


def text_mask_full(original, library, dilate=2, device="cpu", max_hits=40):
    """text_mask, plus every character of the library found in the original
    even where it touches a line (the pressure numbers on the isobars):
    all its black pixels black, (almost) no black around it. A shape found
    more than max_hits times is a piece of line, not a character"""
    import torch
    from scipy import ndimage
    b = original < 128
    found = text_mask(original, dilate=0) & b
    if library:
        H, W = b.shape
        th = max(g.shape[0] for g in library)
        tw = max(g.shape[1] for g in library)
        PH, PW = H + th, W + tw
        img = torch.zeros(PH, PW, device=device)
        img[:H, :W] = torch.from_numpy(b.astype(np.float32)).to(device)
        X = torch.fft.rfft2(img)
        for c0 in range(0, len(library), 32):
            chunk = library[c0:c0 + 32]
            T = torch.zeros(len(chunk), PH, PW, device=device)
            Wt = torch.zeros_like(T)
            nb, nw = [], []
            for j, g in enumerate(chunk):
                h, w = g.shape
                white = ~ndimage.binary_dilation(g, iterations=1)
                T[j, :h, :w] = torch.from_numpy(g.astype(np.float32))
                Wt[j, :h, :w] = torch.from_numpy(white.astype(np.float32))
                nb.append(g.sum())
                nw.append(white.sum())
            # correlation: value at (y, x) = pixels of the shape placed at (y, x)
            hit = torch.fft.irfft2(X[None] * torch.fft.rfft2(T).conj(), s=(PH, PW))
            bad = torch.fft.irfft2(X[None] * torch.fft.rfft2(Wt).conj(), s=(PH, PW))
            for j, g in enumerate(chunk):
                h, w = g.shape
                ok = (hit[j, :H - h + 1, :W - w + 1] > nb[j] - 0.5) & \
                     (bad[j, :H - h + 1, :W - w + 1] <= 0.35 * nw[j] + 0.5)
                ys, xs = torch.nonzero(ok, as_tuple=True)
                if len(ys) == 0 or len(ys) > max_hits:
                    continue
                for y, x in zip(ys.tolist(), xs.tolist()):
                    found[y:y + h, x:x + w] |= g
    return ndimage.binary_dilation(found, iterations=dilate) if dilate > 0 else found


def text_error(a, original, mask=None, margin=20, shift=2, soft=1.0):
    """mean gray difference on the text of the original only, 0..255.
    Both pictures are softened a little (gaussian, soft pixels): a received
    character is never as sharp as the 1-bit original, and without this
    even a perfectly readable received word scores as bad as a white page.
    Every word is compared on its own, at its best place within +-shift
    pixels: a word one pixel off is the same word (the alignment of the
    whole fax cannot be better than that). So the shapes of the characters
    count, not their exact pixels"""
    from scipy import ndimage
    if mask is None:
        mask = text_mask(original)
    m = np.zeros_like(mask)
    m[margin:-margin, margin:-margin] = mask[margin:-margin, margin:-margin]
    if m.sum() == 0:
        return float("nan")
    a = a.astype(np.float32)
    o = original.astype(np.float32)
    if soft:
        a = ndimage.gaussian_filter(a, soft)
        o = ndimage.gaussian_filter(o, soft)
    lab, n = ndimage.label(m)
    tot = 0.0
    cnt = 0
    for i, s in enumerate(ndimage.find_objects(lab), 1):
        mm = lab[s] == i
        ow = o[s][mm]
        best = None
        for dy in range(-shift, shift + 1):
            for dx in range(-shift, shift + 1):
                ys = slice(s[0].start + dy, s[0].stop + dy)
                xs = slice(s[1].start + dx, s[1].stop + dx)
                e = np.abs(a[ys, xs][mm] - ow).sum()
                if best is None or e < best:
                    best = e
        tot += best
        cnt += mm.sum()
    return float(tot / cnt)
