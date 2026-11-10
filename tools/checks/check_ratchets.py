#!/usr/bin/env python3
"""Ratchets: risky patterns may only get rarer.

Each ratchet counts one pattern per file in src/ (comments left out) and
compares with tools/checks/ratchets.json. A count may go down (then lower
the number there, or run --update), never up, and a file not listed must
have none. Old code is left alone; new code can't add more.

  tools/checks/check_ratchets.py            check (exit 1 if any count grew)
  tools/checks/check_ratchets.py --update   write today's counts to the json

Raising a number in ratchets.json is a decision for the PR's reviewers, so
say why in the PR.
"""

import glob
import json
import os
import re
import sys

BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ratchets.json")

RATCHETS = {
    "i2c": {
        "why": "i2c-0 is the GUI's control bus to the base board; a loop on it once "
               "glitched an amplifier's band voltage (AGENTS.md, safety rule 2). The battery "
               "clock /dev/rtc1 is on it. Write it once, from the places that already do.",
        "pattern": r"/dev/rtc1|/dev/i2c|\bI2C_SLAVE\b|\bi2c_smbus_\w+|\bI2C_RDWR\b",
    },
    "colours": {
        "why": "colours typed into the code instead of coming from src/styles.c; themes "
               "(other looks for the whole GUI) only work when colours come from one place.",
        "pattern": r"\blv_color_hex3?\s*\(|\blv_color_make\s*\(|#[0-9A-Fa-f]{6}\s",
        "skip": ["styles.c", "palettes/"],
    },
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def counts():
    files = sorted(glob.glob("src/**/*.[ch]", recursive=True) + glob.glob("src/**/*.[ch]pp", recursive=True)
                   + glob.glob("src/**/*.inc", recursive=True))
    out = {name: {} for name in RATCHETS}
    for f in files:
        rel = os.path.relpath(f, "src")
        text = strip_comments(open(f, encoding="utf-8", errors="replace").read())
        for name, r in RATCHETS.items():
            if any(rel.startswith(s) for s in r.get("skip", [])):
                continue
            n = len(re.findall(r["pattern"], text))
            if n:
                out[name][rel] = n
    return out


def main():
    now = counts()
    if sys.argv[1:] == ["--update"]:
        with open(BASELINE, "w") as f:
            json.dump(now, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"wrote {BASELINE}")
        return 0
    if sys.argv[1:]:
        sys.exit(__doc__)
    base = json.load(open(BASELINE))
    grew, fell = [], []
    for name, r in RATCHETS.items():
        old = base.get(name, {})
        for f, n in sorted(now[name].items()):
            if n > old.get(f, 0):
                grew.append(f"src/{f}: {n} {name} (allowed {old.get(f, 0)}): {r['why']}")
        for f, n in sorted(old.items()):
            if now[name].get(f, 0) < n:
                fell.append(f"src/{f}: {name} down from {n} to {now[name].get(f, 0)}")
    for g in grew:
        print(g)
    for f in fell:
        print(f"good: {f} (lower it in tools/checks/ratchets.json, or --update)")
    if grew:
        return 1
    print("ratchets: " + ", ".join(f"{name} {sum(now[name].values())} in {len(now[name])} files"
                                    for name in RATCHETS) + ", none grew")
    return 0


if __name__ == "__main__":
    sys.exit(main())
