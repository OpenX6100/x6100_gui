#!/usr/bin/env python3
"""Every app's window case in src/main_screen.c must end in its `break`.

A merge of two new apps that keeps "both sides" drops the `break` at the
end of the first one's case, and that app then opens the next app's window
as well (two windows at once). The compiler doesn't warn about it.

  tools/checks/check_app_cases.py [FILE]    (default src/main_screen.c)

Prints each case that runs into the next one and exits 1, or exits 0.
"""

import re
import sys

LABEL = re.compile(r"^\s*(case\s+\w+\s*:|default\s*:)")
APP = re.compile(r"^\s*case\s+(ACTION_APP_\w+)\s*:")
END = re.compile(r"^\s*(break|return)\b")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//.*", "", text)


def check(path):
    lines = strip_comments(open(path, encoding="utf-8").read()).split("\n")
    problems = []
    depth = 0
    block = None  # the open ACTION_APP_* case: name, line, depth, has code, ended

    def close(next_line):
        if block and block["code"] and not block["ended"]:
            problems.append(f"{path}:{block['line']}: case {block['name']} runs into the next "
                            f"case (line {next_line}): its `break;` is missing")

    for n, line in enumerate(lines, 1):
        s = line.strip()
        if LABEL.match(line):
            if block and not block["code"] and block["depth"] == depth:
                continue  # stacked labels share one block
            close(n)
            m = APP.match(line)
            block = {"name": m.group(1), "line": n, "depth": depth, "code": False,
                     "ended": False} if m else None
            continue
        depth += s.count("{") - s.count("}")
        if block is None:
            continue
        if depth < block["depth"]:
            block = None  # the end of the switch: nothing runs into anything
            continue
        if not s or s in ("{", "}"):
            continue
        if depth == block["depth"] and END.match(s):
            block["code"] = block["ended"] = True
        else:
            block["code"], block["ended"] = True, False
    return problems


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "src/main_screen.c"
    problems = check(path)
    for p in problems:
        print(p)
    if problems:
        print("\nEach app's window case must end in `break;`: see AGENTS.md, \"Adding an app\".")
        return 1
    print(f"{path}: every ACTION_APP_* case ends in its break")
    return 0


if __name__ == "__main__":
    sys.exit(main())
