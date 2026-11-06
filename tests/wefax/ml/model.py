# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
The WEFAX post-processing network: a small U-Net, received gray picture in, the
probability that each pixel of the original is black out (as a logit).

Plain 3x3 convolutions, ReLU, 2x2 max pooling and nearest upsampling: easy
to run on the radio later, in C, a strip of rows at a time. Three levels
see about 70 rows around each pixel: the rows above and below a damaged
row, where WEFAX keeps the information the noise took.
"""
import torch
import torch.nn as nn
import torch.nn.functional as F


def block(cin, cout):
    return nn.Sequential(
        nn.Conv2d(cin, cout, 3, padding=1), nn.ReLU(inplace=True),
        nn.Conv2d(cout, cout, 3, padding=1), nn.ReLU(inplace=True))


class WefaxUNet(nn.Module):
    def __init__(self, base=16):
        super().__init__()
        c1, c2, c3, c4 = base, base * 2, base * 4, base * 4
        self.cfg = {"base": base}
        self.e1 = block(1, c1)
        self.e2 = block(c1, c2)
        self.e3 = block(c2, c3)
        self.mid = block(c3, c4)
        self.d3 = block(c4 + c3, c3)
        self.d2 = block(c3 + c2, c2)
        self.d1 = block(c2 + c1, c1)
        self.out = nn.Conv2d(c1, 1, 1)

    def forward(self, x):
        # x: N x 1 x H x W, received picture, 0 white .. 1 black; H, W multiple of 8
        e1 = self.e1(x)
        e2 = self.e2(F.max_pool2d(e1, 2))
        e3 = self.e3(F.max_pool2d(e2, 2))
        m = self.mid(F.max_pool2d(e3, 2))
        d3 = self.d3(torch.cat([F.interpolate(m, scale_factor=2, mode="nearest"), e3], 1))
        d2 = self.d2(torch.cat([F.interpolate(d3, scale_factor=2, mode="nearest"), e2], 1))
        d1 = self.d1(torch.cat([F.interpolate(d2, scale_factor=2, mode="nearest"), e1], 1))
        return self.out(d1)


def run_full(model, gray, device, tile=512, pad=48):
    """the whole fax (uint8 numpy, 0 black .. 255 white) through the
    network in overlapping tiles; returns probability of black, float32"""
    import numpy as np
    h, w = gray.shape
    x = torch.from_numpy((255 - gray.astype(np.float32)) / 255.0)
    out = np.zeros((h, w), np.float32)
    model.eval()
    with torch.no_grad():
        for y0 in range(0, h, tile):
            for x0 in range(0, w, tile):
                ya, xa = max(0, y0 - pad), max(0, x0 - pad)
                yb, xb = min(h, y0 + tile + pad), min(w, x0 + tile + pad)
                t = x[ya:yb, xa:xb]
                ph, pw = (-t.shape[0]) % 8, (-t.shape[1]) % 8
                t = F.pad(t[None, None], (0, pw, 0, ph), mode="replicate").to(device)
                p = torch.sigmoid(model(t))[0, 0, :yb - ya, :xb - xa].float().cpu().numpy()
                out[y0:min(h, y0 + tile), x0:min(w, x0 + tile)] = \
                    p[y0 - ya:y0 - ya + min(tile, h - y0), x0 - xa:x0 - xa + min(tile, w - x0)]
    return out
