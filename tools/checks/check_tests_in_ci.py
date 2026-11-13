#!/usr/bin/env python3
"""Every unit test registered in tests/ must be run by CI.

The test workflow names the tests it builds and runs by hand (the firmware's
LVGL doesn't build on a PC, so "build everything" isn't possible). A new
test added to tests/CMakeLists.txt but not to .github/workflows/tests.yml
is built by nobody and never runs: until 2026-11 eight of eleven tests were
in that state.

  tools/checks/check_tests_in_ci.py

Lists each add_test(NAME ...) that no step of tests.yml runs (a `ctest`
line or a direct `build_test/tests/...` call: building it, or naming it in a
comment, doesn't count); exit 1 if any.
"""

import glob
import re
import sys

WORKFLOW = ".github/workflows/tests.yml"


def main():
    names = set()
    for p in glob.glob("tests/**/CMakeLists.txt", recursive=True):
        text = re.sub(r"#.*", "", open(p, encoding="utf-8").read())
        names |= set(re.findall(r"add_test\s*\(\s*NAME\s+([\w-]+)", text))
    runs = []
    for line in open(WORKFLOW, encoding="utf-8"):
        if line.lstrip().startswith("#"):
            continue
        if "ctest" in line or "build_test/tests/" in line:
            runs.append(line)
    ran = "\n".join(runs)
    missing = sorted(n for n in names if not re.search(rf"(?<![\w-]){re.escape(n)}(?![\w-])", ran))
    for n in missing:
        print(f"{n} is registered in tests/ but no step of {WORKFLOW} runs it (ctest -R or build_test/tests/{n})")
    if missing:
        return 1
    print(f"all {len(names)} unit tests are run by {WORKFLOW}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
