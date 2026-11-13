#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Synthetic test of the DSC decoder: random individual calls at several
signal to noise ratios and fadings.

    ./run_tests.py [--calls N] [--decoder PATH ...]

For each case, out of N calls:
  verified   decoded with a correct error check and right
  WRONG      decoded with a correct error check but different from the call
             sent (must stay 0)
  ids        both MMSIs right (verified or not)
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def mmsi_syms(r, coast=False):
    digits = ("00" + "".join(str(r.randint(0, 9)) for _ in range(7))) if coast else \
             (str(r.randint(2, 7)) + "".join(str(r.randint(0, 9)) for _ in range(8)))
    digits += "0"
    return [int(digits[i:i + 2]) for i in range(0, 10, 2)]


def call(seed):
    r = random.Random(seed)
    sym = [120] + mmsi_syms(r, coast=r.random() < 0.5) + [108] + mmsi_syms(r) + [118, 126]
    if r.random() < 0.5:
        sym += [126] * 6
    else:
        sym += [r.randint(0, 99) for _ in range(6)]
    sym += [r.choice([117, 122])]
    return sym


def decode(tool, wav):
    out = subprocess.run([tool, wav], capture_output=True, text=True).stdout
    calls = []
    for line in out.splitlines():
        head = line.split("|")[0].split()
        calls.append((head[1] == "OK", [int(v) for v in head[2:]]))
    return calls


def main():
    n = 20
    tools = [os.path.join(HERE, "dsc_wav")]
    args = sys.argv[1:]
    if "--calls" in args:
        n = int(args[args.index("--calls") + 1])
    if "--decoder" in args:
        tools = [a for a in args[args.index("--decoder") + 1:] if not a.startswith("--")]
    cases = [("SNR 10 dB", ["--snr", "10"]), ("SNR 0 dB", ["--snr", "0"]),
             ("SNR -4 dB", ["--snr", "-4"]), ("SNR -6 dB", ["--snr", "-6"]),
             ("SNR -8 dB", ["--snr", "-8"]), ("SNR -10 dB", ["--snr", "-10"]),
             ("SNR 5, offset 30 Hz", ["--snr", "5", "--off", "30"]),
             ("SNR 10, fading 2 s", ["--snr", "10", "--fade", "2"]),
             ("SNR 5, fading 2 s", ["--snr", "5", "--fade", "2"]),
             ("SNR 10, selective 2 s", ["--snr", "10", "--sfade", "2"])]
    tmp = tempfile.mkdtemp(prefix="dsc_test_")
    print("%-22s" % "case" + "".join("  %-26s" % os.path.basename(t) for t in tools))
    print("%-22s" % "" + "  verified WRONG  ids       " * len(tools))
    for name, extra in cases:
        res = [[0, 0, 0] for _ in tools]
        for s in range(n):
            sym = call(s)
            ecc = 0
            for v in sym:
                ecc ^= v
            full = [sym[0]] + sym + [ecc]
            wav = os.path.join(tmp, "c.wav")
            subprocess.run([os.path.join(HERE, "dsc_gen"), wav, "--seed", str(s)] + extra +
                           [str(v) for v in sym], check=True)
            for k, t in enumerate(tools):
                for ok, got in decode(t, wav):
                    if ok:
                        if got == full:
                            res[k][0] += 1
                        else:
                            res[k][1] += 1
                    if got[2:7] == full[2:7] and got[8:13] == full[8:13]:
                        res[k][2] += 1
        print("%-22s" % name + "".join("  %5d/%-2d %5d %5d       " % (r[0], n, r[1], r[2]) for r in res))


if __name__ == "__main__":
    main()
