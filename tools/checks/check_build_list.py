#!/usr/bin/env python3
"""Every source file in src/ must be in the GUI's build list.

src/CMakeLists.txt names the GUI's .c/.cpp files by hand. A merge that
resolves a conflict there with "keep ours" can drop another PR's files
(tx_level.c and tx_player.c were lost that way, #311/#312). Compiling each
file still works, so only the link fails, and the link happens only in the
full image build, an hour later.

  tools/checks/check_build_list.py [--base REF]

Checks that every .c/.cpp file under src/ (outside the folders with their own
CMakeLists.txt) is listed in src/CMakeLists.txt, and every listed file
exists. With --base (a git ref, e.g. the PR's target branch), also lists
files the base builds and this tree no longer does. Exits 1 on a problem.
"""

import glob
import os
import re
import subprocess
import sys

CMAKE = "src/CMakeLists.txt"

# In src/ but not built, on purpose. Add a file here only with a reason.
NOT_BUILT = {
    "goertzel.c",           # R1CBU's, unused since 2023 (nothing includes goertzel.h)
    "mouse.c",              # R1CBU's mouse input, not in the build since 2023
    "mouse_cursor_icon.c",  # its cursor picture
}


def listed(text):
    m = re.search(r"target_sources\s*\((.*?)\)", text, re.S)
    if not m:
        sys.exit(f"{CMAKE}: no target_sources(...) found")
    body = re.sub(r"#.*", "", m.group(1))
    return set(re.findall(r"[\w./-]+\.(?:c|cpp)\b", body))


def on_disk():
    own = {os.path.basename(os.path.dirname(p)) for p in glob.glob("src/*/CMakeLists.txt")}
    files = set()
    for p in glob.glob("src/**/*.c", recursive=True) + glob.glob("src/**/*.cpp", recursive=True):
        rel = os.path.relpath(p, "src")
        if "/" in rel and rel.split("/")[0] in own:
            continue  # built by that folder's own CMakeLists.txt
        files.add(rel)
    return files


def main():
    args = sys.argv[1:]
    base = None
    if args[:1] == ["--base"] and len(args) > 1:
        base = args[1]
    elif args:
        sys.exit(__doc__)

    now = listed(open(CMAKE, encoding="utf-8").read())
    files = on_disk()
    problems = []
    for f in sorted(files - now - NOT_BUILT):
        problems.append(f"src/{f} is not in {CMAKE}: add it under its app's name line "
                        f"(or to NOT_BUILT in {sys.argv[0]} with a reason)")
    for f in sorted(now - files):
        problems.append(f"{CMAKE} lists {f}, which doesn't exist")
    if base:
        old = subprocess.run(["git", "show", f"{base}:{CMAKE}"], capture_output=True, text=True)
        if old.returncode:
            sys.exit(f"git show {base}:{CMAKE} failed: {old.stderr.strip()}")
        for f in sorted(listed(old.stdout) - now):
            if f in files:
                problems.append(f"{f} is built on {base} but dropped here (a merge that kept only one side?)")
    for p in problems:
        print(p)
    if problems:
        return 1
    print(f"{CMAKE}: all {len(files - NOT_BUILT)} source files listed" + (f", nothing dropped since {base}" if base else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
