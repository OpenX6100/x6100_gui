#!/usr/bin/env python3
"""Every app's window case in src/main_screen.c must end in its `break`.

A merge of two new apps that keeps "both sides" drops the `break` at the
end of the first one's case, and that app then opens the next app's window
as well (two windows at once). The compiler doesn't warn about it.

  tools/checks/check_app_cases.py [FILE]    (default src/main_screen.c)

Prints each case that runs into the next one, and each window case whose
label has no code of its own (it would open the next app's window), and
exits 1; or exits 0.

It also warns (never fails) about an app case that doesn't announce its
window with voice_say_text_fmt(): with the radio's voice mode, that is how
a blind operator knows which window opened.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csource import strip_comments  # noqa: E402

LABEL = re.compile(r"^\s*(case\s+\w+\s*:|default\s*:)")
APP = re.compile(r"^\s*case\s+(ACTION_APP_\w+)\s*:")
END = re.compile(r"^\s*(break|return)\b")
STRING = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def check(path):
    lines = strip_comments(open(path, encoding="utf-8").read()).split("\n")
    problems, warnings = [], []
    depth = 0
    block = None  # the open ACTION_APP_* case: its labels, line, depth, has code, ended

    def close(next_line):
        if not block or not block["code"]:
            return
        name = block["labels"][0]
        if not block["ended"]:
            problems.append(f"{path}:{block['line']}: case {name} runs into the next "
                            f"case (line {next_line}): its `break;` is missing")
        if len(block["labels"]) > 1 and not block["dispatch"]:
            # Stacked labels are right only in main_screen_action()'s list
            # that hands every app to main_screen_start_app(); in a window
            # case, a label with no code of its own opens the next app.
            problems.append(f"{path}:{block['line']}: case {name} has no code of its own: "
                            f"it opens {block['labels'][-1]}'s window")
        if not block["voice"]:
            warnings.append((block["line"], f"case {name} opens a window without "
                             "voice_say_text_fmt(\"... window\") for the voice mode"))

    for n, line in enumerate(lines, 1):
        s = line.strip()
        if LABEL.match(line):
            m = APP.match(line)
            if block and not block["code"] and block["depth"] == depth:
                if m:
                    block["labels"].append(m.group(1))
                continue  # stacked labels share one block
            close(n)
            block = {"labels": [m.group(1)], "line": n, "depth": depth, "code": False,
                     "ended": False, "voice": False, "dispatch": False} if m else None
            continue
        bare = STRING.sub('""', s)  # braces inside strings don't count
        depth += bare.count("{") - bare.count("}")
        if block is None:
            continue
        if depth < block["depth"]:
            close(n)
            block = None  # the end of the switch: nothing runs into anything
            continue
        if not s or s in ("{", "}"):
            continue
        if "main_screen_start_app(" in s:
            block["dispatch"] = True
        if "voice_say_text" in s or block["dispatch"]:
            block["voice"] = True  # a dispatched app's own case speaks
        if depth == block["depth"] and END.match(s):
            block["code"] = block["ended"] = True
        else:
            block["code"], block["ended"] = True, False
    return problems, warnings


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "src/main_screen.c"
    problems, warnings = check(path)
    for line, w in warnings:
        if os.environ.get("GITHUB_ACTIONS"):
            print(f"::warning file={path},line={line},title=Voice mode::{w}")
        else:
            print(f"warning: {path}:{line}: {w}")
    for p in problems:
        print(p)
    if problems:
        print("\nEach app's window case has its own code and ends in `break;`: see AGENTS.md, \"Adding an app\".")
        return 1
    print(f"{path}: every ACTION_APP_* case ends in its break")
    return 0


if __name__ == "__main__":
    sys.exit(main())
