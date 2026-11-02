#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
wefax_score.py ORIGINAL DECODED.pgm

A decoded fax (wefax_wav) against its original picture (JOMOC): the
original is stretched to the 1809-pixel line, the two are aligned on
several bands of rows, and the mean gray error (0..255) of the picture
area is printed, with the error of the rows (10/50/90/99 percentiles).
Needs numpy, scipy, Pillow.
"""
import sys, numpy as np
from PIL import Image
from numpy.fft import rfft2, irfft2
from scipy.ndimage import gaussian_filter
og=np.array(Image.open(sys.argv[1]).convert('L').resize((1809,1293),Image.BOX)).astype(np.float32)
r=np.array(Image.open(sys.argv[2])).astype(np.float32)
ob=gaussian_filter((255-og)/255,1.5); rb=gaussian_filter((255-r)/255,1.5)
res=[]
for y0 in range(100,1150,250):
    a=ob[y0:y0+120]; best=None
    for dy in range(0,min(120,r.shape[0]-y0-120)):
        b=rb[y0+dy:y0+dy+120]
        A=a-a.mean(); B=b-b.mean()
        c=irfft2(rfft2(B,s=(120,2048))*np.conj(rfft2(A,s=(120,2048))),s=(120,2048))[0]
        k=int(np.argmax(c)); v=c[k]/np.sqrt((A*A).sum()*(B*B).sum())
        if best is None or v>best[0]: best=(round(float(v),3),k if k<1024 else k-2048,dy)
    res.append(best)
dy=int(np.median([x[2] for x in res])); dx=int(np.median([x[1] for x in res]))
rr=np.roll(r,-dx,axis=1)[dy:dy+1293]
err=float(np.abs(rr[20:-20,20:-20]-og[20:-20,20:-20]).mean())
print(f"dx={dx} dy={dy} corr={min(x[0] for x in res):.2f}..{max(x[0] for x in res):.2f} gray_err={err:.1f}")
row=np.abs(rr[20:-20,20:-20]-og[20:-20,20:-20]).mean(1)
print("row err pct 10/50/90/99:", np.percentile(row,[10,50,90,99]).round(1))
