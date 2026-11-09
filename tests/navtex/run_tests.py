#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Synthetic test of the NAVTEX decoder: messages at several signal to noise
ratios (and with frequency offset, clock error, fading), decoded by the
firmware decoder (navtex_wav).

    ./run_tests.py [--quick]

For each case it prints the character error rate (edit distance between
the message sent and the text decoded, divided by the length of the
message): 0 % is perfect.
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORDS = ("NAVAREA GALE WARNING FORCE VESSELS AREA NAVIGATIONAL BUOY LIGHT "
         "UNLIT WRECK POSITION DANGEROUS SEA STATE ROUGH VERY HIGH WIND "
         "NORTHWEST SOUTHEAST VISIBILITY POOR FOG MILITARY EXERCISES CANCEL "
         "THIS MESSAGE COAST GUARD SEARCH AND RESCUE OPERATION IN PROGRESS "
         "KEEP CLEAR ADVISED").split()


def message(seed):
    r = random.Random(seed)
    lines = ["ZCZC %s%s%02d" % (r.choice("ABCDEFGHJKL"), r.choice("ABDEFLV"), r.randint(1, 99)),
             "%02d%02d%02d UTC OCT 26" % (r.randint(1, 28), r.randint(0, 23), r.randint(0, 59))]
    for _ in range(12):
        words = []
        for _ in range(r.randint(4, 8)):
            if r.random() < 0.25:
                words.append("%02d-%02dN %03d-%02dE" % (r.randint(0, 89), r.randint(0, 59),
                                                       r.randint(0, 179), r.randint(0, 59)))
            else:
                words.append(r.choice(WORDS))
        lines.append(" ".join(words) + ".")
    lines.append("NNNN")
    return "\n".join(lines) + "\n"


def edit_distance(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
        prev = cur
    return prev[-1]


def decode(tool, wav, extra=()):
    p = subprocess.run([os.path.join(HERE, tool), wav] + list(extra),
                       capture_output=True, text=True)
    return p.stdout


def cer(sent, got):
    # what comes before the first and after the last line of the message
    # (lost characters while the signal starts and stops) is not counted
    got = got.strip("\n*")
    return edit_distance(sent.strip("\n"), got) / len(sent)


def main():
    quick = "--quick" in sys.argv
    seeds = [1, 2] if quick else [1, 2, 3, 4]
    cases = [("clean", []),
             ("SNR 10 dB", ["--snr", "10"]),
             ("SNR 5 dB", ["--snr", "5"]),
             ("SNR 2 dB", ["--snr", "2"]),
             ("SNR 0 dB", ["--snr", "0"]),
             ("SNR -2 dB", ["--snr", "-2"]),
             ("SNR -4 dB", ["--snr", "-4"]),
             ("SNR -6 dB", ["--snr", "-6"]),
             ("SNR -8 dB", ["--snr", "-8"]),
             ("SNR -10 dB", ["--snr", "-10"]),
             ("SNR 5, offset +30 Hz", ["--snr", "5", "--off", "30"]),
             ("SNR 5, offset -50 Hz", ["--snr", "5", "--off", "-50"]),
             ("SNR 5, clock +100 ppm", ["--snr", "5", "--ppm", "100"]),
             ("SNR 10, fading 3 s", ["--snr", "10", "--fade", "3"]),
             ("SNR 5, fading 3 s", ["--snr", "5", "--fade", "3"]),
             ("SNR 0, fading 8 s", ["--snr", "0", "--fade", "8"]),
             ("SNR 10, selective 3 s", ["--snr", "10", "--sfade", "3"]),
             ("SNR 5, selective 5 s", ["--snr", "5", "--sfade", "5"]),
             ("SNR 10, 11025 Hz", ["--snr", "10", "--rate", "11025"])]
    tmp = tempfile.mkdtemp(prefix="navtex_test_")
    print("%-24s %10s" % ("case", "errors"))
    for name, args in cases:
        tot_new = 0.0
        for seed in seeds:
            msg = message(seed)
            wav = os.path.join(tmp, "t.wav")
            subprocess.run([os.path.join(HERE, "sitorb_gen"), wav, "--seed", str(seed)] + args,
                           input=msg, text=True, check=True)
            tot_new += cer(msg, decode("navtex_wav", wav))
        print("%-24s %9.1f%%" % (name, 100 * tot_new / len(seeds)))


if __name__ == "__main__":
    main()
