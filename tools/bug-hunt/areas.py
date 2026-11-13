#!/usr/bin/env python3
"""The bug hunt's areas: which source files belong to which area.

The areas are defined in docs/bug-hunt/AREAS.md: each `### Annn ...`
heading is followed by a ```files block, one path per line. A path is a
file, a folder ending in `/` (everything under it), or part of a file that
is too big for one area: `src/dialog_js8.c:start..compose_count_timer`,
from a line to the line before another. `start` and `end` are the file's
ends; any other name is the one top-level (not indented) line, other than
a prototype, that contains that word: a function or variable definition.
So the parts follow the code as it changes. The ```skip block lists what
is not hunted (third-party code, fonts).

  tools/bug-hunt/areas.py list          every area, its lines and files
  tools/bug-hunt/areas.py files A07     the area's files, one per line
                                        (what you give /code-review)
  tools/bug-hunt/areas.py show A07      the area's heading, notes and files
  tools/bug-hunt/areas.py check         every source file in src/ is in
                                        exactly one area or skipped, every
                                        listed path exists, no area is over
                                        MAX_LINES; exits 1 on a problem

With the GitHub CLI (`gh`), from the hunt comments on the area issues
(the format is in docs/bug-hunt/HUNT.md):

  tools/bug-hunt/areas.py status [A07]  every area (or one): hunts done, on
                                        which commit, changed since, a hunt
                                        to continue, the live claim
  tools/bug-hunt/areas.py next          the area to hunt next
  tools/bug-hunt/areas.py make-issues [Ann ...] [--run]
                                        the label, one issue per area that
                                        has none, and the overview issue;
                                        prints the gh commands, --run runs
                                        them

Run it from the repository's top folder.
"""

import datetime
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "checks"))
from csource import strip_comments  # noqa: E402

AREAS = "docs/bug-hunt/AREAS.md"
SOURCE = re.compile(r"\.(c|cpp|h|hpp|inc)$")
MAX_LINES = 8000  # the largest change /code-review's deepest review takes
REPO = os.environ.get("BUG_HUNT_REPO", "OpenX6100/x6100_gui")
LABEL = "bug-hunt"
ISSUE_TITLE = re.compile(r"^Bug hunt ([AX]\d+)\b")
OVERVIEW_TITLE = "Bug hunt: overview"
MARKER = "<!-- bug-hunt -->"
CLAIM = datetime.timedelta(hours=2)  # a claim lasts this long after its last edit

HEADING = re.compile(r"^### ([AX]\d+)\b(.*)$")
PART = re.compile(r"^(.*):(\w+)\.\.(\w+)$")
PROTOTYPE = re.compile(r"\)\s*;\s*$")


def parse(text=None):
    if text is None:
        text = open(AREAS, encoding="utf-8").read()
    areas, skip = [], []
    current, block = None, None
    for n, line in enumerate(text.split("\n"), 1):
        if block is not None:
            if line.startswith("```"):
                block = None
            elif line.strip() and not line.lstrip().startswith("#"):
                block.append((line.strip(), n))
            continue
        m = HEADING.match(line)
        if m:
            current = {"id": m.group(1), "title": m.group(2).strip(" ·-"),
                       "line": n, "paths": [], "notes": []}
            areas.append(current)
        elif line.startswith("```files"):
            if current is None:
                sys.exit(f"{AREAS}:{n}: a files block before any area")
            block = current["paths"]
        elif line.startswith("```skip"):
            block = skip
        elif line.startswith("## "):
            current = None
        elif current is not None and not current["paths"]:
            current["notes"].append(line)
    return areas, skip


def sources():
    out = []
    for root, _, files in os.walk("src"):
        for f in files:
            if SOURCE.search(f):
                out.append(os.path.join(root, f))
    return sorted(out)


_lines = {}
_code = {}


def file_lines(path):
    if path not in _lines:
        with open(path, encoding="utf-8", errors="replace") as f:
            _lines[path] = f.read().split("\n")
            if _lines[path] and _lines[path][-1] == "":
                _lines[path].pop()
    return _lines[path]


def code_lines(path):
    """The file's lines with comments blanked (same line numbers)."""
    if path not in _code:
        _code[path] = strip_comments("\n".join(file_lines(path))).split("\n")
    return _code[path]


def anchor(path, name):
    """The 1-based line of `name` in path: start, end+1, or its one definition."""
    lines = code_lines(path)
    if name == "start":
        return 1
    if name == "end":
        return len(lines) + 1
    word = re.compile(r"\b" + re.escape(name) + r"\b")
    hits = [i + 1 for i, l in enumerate(lines)
            if l and not l[0].isspace() and word.search(l) and not PROTOTYPE.search(l)]
    if len(hits) != 1:
        raise ValueError(f"{path}: '{name}' is on {len(hits)} top-level lines, needs exactly 1")
    return hits[0]


def expand(path, all_sources):
    """(file, first, last) for each file a path names; first None = whole file."""
    m = PART.match(path)
    if m:
        f = m.group(1)
        if not os.path.exists(f):
            return [(f, None, None)]
        first, stop = anchor(f, m.group(2)), anchor(f, m.group(3))
        return [(f, first, stop - 1)]
    if path.endswith("/"):
        return [(s, None, None) for s in all_sources if s.startswith(path)]
    return [(path, None, None)]


def area_files(area, all_sources):
    out = []
    for path, _ in area["paths"]:
        out.extend(expand(path, all_sources))
    return out


def area_lines(area, all_sources):
    total = 0
    for f, first, last in area_files(area, all_sources):
        if os.path.exists(f):
            total += (last - first + 1) if first else len(file_lines(f))
    return total


def find(areas, wanted):
    for a in areas:
        if a["id"].lower() == wanted.lower():
            return a
    sys.exit(f"no area {wanted} in {AREAS} (see: tools/bug-hunt/areas.py list)")


def fmt(f, first, last):
    return f"{f}:{first}-{last}" if first else f


def check(areas, skip, all_sources):
    problems = []
    owner = {}  # file -> list of (area id, first, last)
    seen = {}
    for a in areas:
        if a["id"] in seen:
            problems.append(f"{AREAS}:{a['line']}: {a['id']} is used twice (also line {seen[a['id']]})")
        seen.setdefault(a["id"], a["line"])
    for a in areas:
        if a["id"].startswith("X"):
            continue  # a theme through code that is in A areas: no coverage or size rule
        if not a["paths"]:
            problems.append(f"{AREAS}:{a['line']}: {a['id']} has no files block")
        for path, n in a["paths"]:
            try:
                files = expand(path, all_sources)
            except ValueError as e:
                problems.append(f"{AREAS}:{n}: {e}")
                continue
            if not files:
                problems.append(f"{AREAS}:{n}: {path} matches no source file")
            for f, first, last in files:
                if not os.path.exists(f):
                    problems.append(f"{AREAS}:{n}: {f} does not exist")
                    continue
                if first and last < first:
                    problems.append(f"{AREAS}:{n}: {path} is empty (its end comes first)")
                owner.setdefault(f, []).append((a["id"], first, last))
        try:
            lines = area_lines(a, all_sources)
        except ValueError:
            continue
        if lines > MAX_LINES:
            problems.append(f"{a['id']} is {lines} lines, over {MAX_LINES}: split it")
    skipped = set()
    for path, n in skip:
        files = expand(path, all_sources)
        if not files or not all(os.path.exists(f) for f, _, _ in files):
            problems.append(f"{AREAS}:{n}: skipped {path} does not exist")
        skipped.update(f for f, _, _ in files)
    for f in all_sources:
        parts = owner.get(f, [])
        if f in skipped:
            if parts:
                problems.append(f"{f} is skipped and also in {parts[0][0]}")
            continue
        if not parts:
            problems.append(f"{f} is in no bug-hunt area: add it to one in {AREAS}")
            continue
        ranged = [p for p in parts if p[1] is not None]
        if len(parts) > 1 and len(ranged) != len(parts):
            problems.append(f"{f} is in more than one area: "
                            + ", ".join(p[0] for p in parts))
        if ranged:
            # the parts must cover the whole file once, in order
            end, total = 0, len(file_lines(f))
            for _, first, last in sorted(ranged, key=lambda p: p[1]):
                if first > end + 1:
                    problems.append(f"{f}: lines {end + 1}-{first - 1} are in no area")
                elif first < end + 1:
                    problems.append(f"{f}: lines {first}-{end} are in two areas")
                end = max(end, last)
            if end < total:
                problems.append(f"{f}: lines {end + 1}-{total} are in no area (end the last part with ..end)")
    return problems


# --- GitHub: the hunt comments --------------------------------------------

def gh(*args):
    r = subprocess.run(["gh", *args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"gh {' '.join(args)} failed:\n{r.stderr.strip()}")
    return r.stdout


def area_issues(with_comments=True, only=None):
    """{area id: issue} for the issues titled 'Bug hunt Ann', oldest first."""
    out = gh("issue", "list", "-R", REPO, "--label", LABEL, "--state", "all",
             "--limit", "1000", "--json", "number,title,state")
    issues = {}
    for i in sorted(json.loads(out), key=lambda i: i["number"]):
        m = ISSUE_TITLE.match(i["title"])
        if m and m.group(1) not in issues:
            issues[m.group(1)] = i
    if with_comments:
        for aid, i in issues.items():
            if only and aid != only:
                continue
            # every page: `gh issue list --json comments` stops at 100
            lines = gh("api", "--paginate", f"repos/{REPO}/issues/{i['number']}/comments",
                       "--jq", ".[] | {body: .body, url: .html_url, updated: .updated_at, login: (.user.login // \"?\")}")
            i["comments"] = [json.loads(l) for l in lines.splitlines() if l.strip()]
    return issues


FIELD = re.compile(r"^[ \t]*\**(State|Commit|By|Until|Stopped at)\**:\**[ \t]*(.*?)[ \t]*$", re.M | re.I)
TIME = re.compile(r"(\d{4}-\d\d-\d\d)[T ](\d\d:\d\d)")
SHA = re.compile(r"\b[0-9a-f]{7,40}\b")


def fields(body):
    return {k.lower(): v for k, v in FIELD.findall(body) if v}


def utc(text):
    """The first 2026-11-14T08:00 (or with a space) in text, read as UTC."""
    m = TIME.search(text or "")
    if not m:
        return None
    return datetime.datetime.fromisoformat(f"{m.group(1)}T{m.group(2)}+00:00")


def claim_end(c):
    """When a working hunt's claim runs out: 2 hours after GitHub saw its last
    edit. The comment's own Until: is only for people (a wrong year or time
    zone in it must not lock or free the area)."""
    edited = utc(c.get("updated"))
    return edited + CLAIM if edited else None


_diffs = {}
_old_areas = {}


def changed_lines(commit, path):
    """Line numbers (today's file) changed since commit; None if unknown here."""
    if (commit, path) not in _diffs:
        _diffs[(commit, path)] = _changed_lines(commit, path)
    return _diffs[(commit, path)]


def _changed_lines(commit, path):
    r = subprocess.run(["git", "diff", "-U0", commit, "HEAD", "--", path],
                       capture_output=True, text=True)
    if r.returncode:
        return None
    out = []
    for m in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", r.stdout, re.M):
        start, count = max(int(m.group(1)), 1), int(m.group(2) or 1)
        out.append((start, start + max(count, 1) - 1))
    return out


def paths_then(commit, area_id):
    """The area's paths in AREAS.md at commit; None if it had none or unknown."""
    if commit not in _old_areas:
        r = subprocess.run(["git", "show", f"{commit}:{AREAS}"], capture_output=True, text=True)
        _old_areas[commit] = ({a["id"]: [p for p, _ in a["paths"]] for a in parse(r.stdout)[0]}
                              if r.returncode == 0 else {})
    return _old_areas[commit].get(area_id)


def changed_since(commit, area, all_sources):
    """True if the area's lines, or its list of files, changed after commit
    (or it's unknown here)."""
    if paths_then(commit, area["id"]) != [p for p, _ in area["paths"]]:
        return True
    for f, first, last in area_files(area, all_sources):
        spans = changed_lines(commit, f)
        if spans is None:
            return True
        for a, b in spans:
            if not first or (a <= last and b >= first):
                return True
    return False


def hunt_state(area, issue, all_sources, now):
    """What the issue's hunt comments say (each comment is one hunt, edited
    as it goes): done hunts, a hunt to continue, the live claim."""
    st = {"full": 0, "stale": 0, "resume": None, "claim": None, "last": None}
    for c in issue.get("comments", []):
        body = c["body"]
        if MARKER not in body:
            continue
        f = fields(body)
        state = f.get("state", "working").split()[0].lower().strip("*`")
        who = f.get("by") or c["login"]
        if state == "withdrawn":
            continue
        until = claim_end(c)
        sha = SHA.search(f.get("commit", ""))
        commit = sha.group(0) if sha else ""
        if state == "working" and until and until > now:
            if not st["claim"]:  # the first live claim holds; later ones withdraw
                st["claim"] = (who, until, c["url"])
            continue
        stale = not commit or changed_since(commit, area, all_sources)
        st["last"] = (commit or "?", who)
        if state == "done":
            st["stale" if stale else "full"] += 1
            st["resume"] = None
        elif not stale:  # partial, or working with its time run out: continue it
            st["resume"] = (f.get("stopped at", "?"), c["url"])
    return st


def status(areas, all_sources, only=None):
    if only:
        only = find(areas, only)["id"]
    issues = area_issues(only=only)
    now = datetime.datetime.now(datetime.timezone.utc)
    rows = []
    for a in areas:
        if only and a["id"] != only:
            continue
        issue = issues.get(a["id"])
        rows.append((a, issue, hunt_state(a, issue, all_sources, now) if issue else None))
    return rows


def print_status(rows):
    if rows and not any(i for _, i, _ in rows):
        print("No area has an issue yet: a maintainer runs tools/bug-hunt/areas.py make-issues")
        return
    for a, issue, st in rows:
        if not issue:
            print(f"{a['id']}  (no issue yet)  {a['title']}")
            continue
        parts = [f"{st['full']} hunt(s)"]
        if st["stale"]:
            parts.append(f"{st['stale']} on older code")
        if st["resume"]:
            parts.append(f"to continue from {st['resume'][0]} ({st['resume'][1]})")
        if st["last"]:
            parts.append(f"last {st['last'][0]} by {st['last'][1]}")
        if st["claim"]:
            parts.append(f"CLAIMED by {st['claim'][0]} until "
                         f"{st['claim'][1].strftime('%Y-%m-%dT%H:%MZ')} ({st['claim'][2]})")
        print(f"{a['id']}  #{issue['number']:<5} {a['title']}\n       " + "; ".join(parts))


def pick_next(rows):
    free = [(a, i, s) for a, i, s in rows if i and not s["claim"]]
    if not free:
        return None
    # a hunt to continue first, then the fewest hunts on today's code
    return min(free, key=lambda r: (r[2]["resume"] is None, r[2]["full"]))


def issue_body(a, all_sources):
    files = "\n".join(p for p, _ in a["paths"])
    notes = "\n".join(a["notes"]).strip()
    return (f"Bug hunt area **{a['id']} · {a['title']}**, "
            f"{area_lines(a, all_sources)} lines.\n\n{notes}\n\n"
            f"**How to hunt it:** [docs/bug-hunt/HUNT.md](https://github.com/{REPO}/blob/main/docs/bug-hunt/HUNT.md). "
            "Each hunt is one comment here, kept up to date as it goes, in HUNT.md's format "
            "(`tools/bug-hunt/areas.py status` reads them).\n\n"
            f"Files (from [AREAS.md](https://github.com/{REPO}/blob/main/docs/bug-hunt/AREAS.md); "
            "`tools/bug-hunt/areas.py files "
            f"{a['id']}` gives today's list):\n\n```\n{files}\n```\n")


def overview_body(areas, issues):
    rows = "\n".join(
        f"- {a['id']} · {a['title']}: " + (f"#{issues[a['id']]['number']}" if a["id"] in issues else "(no issue)")
        for a in areas)
    return ("A review of every line of the GUI, done by AI agents in spare tokens, "
            "one area at a time. Point your agent at it:\n\n"
            "> Read docs/bug-hunt/HUNT.md in OpenX6100/x6100_gui and follow it.\n\n"
            "Which area is free and how far each one got: `tools/bug-hunt/areas.py status` "
            "(reads the hunt comments on the issues below). The areas and their files: "
            f"[docs/bug-hunt/AREAS.md](https://github.com/{REPO}/blob/main/docs/bug-hunt/AREAS.md).\n\n"
            f"{rows}\n")


def make_issues(areas, all_sources, wanted, run):
    issues = area_issues(with_comments=False)
    todo = [a for a in areas if a["id"] not in issues
            and (not wanted or a["id"] in wanted)]
    tmp = tempfile.mkdtemp(prefix="bug-hunt-")
    cmds = []
    labels = gh("label", "list", "-R", REPO, "--limit", "200", "--json", "name")
    if LABEL not in {l["name"] for l in json.loads(labels)}:
        cmds.append(["label", "create", LABEL, "-R", REPO, "--color", "B60205",
                     "--description", "Bug hunt areas and findings (docs/bug-hunt/HUNT.md)"])
    for a in todo:
        path = os.path.join(tmp, f"{a['id']}.md")
        open(path, "w", encoding="utf-8").write(issue_body(a, all_sources))
        cmds.append(["issue", "create", "-R", REPO, "--label", LABEL,
                     "--title", f"Bug hunt {a['id']}: {a['title']}", "--body-file", path])
    for c in cmds:
        print(shlex.join(["gh", *c]))
        if run:
            print(gh(*c).strip())
    # the overview lists every area's issue, so it comes last
    listed = json.loads(gh("issue", "list", "-R", REPO, "--label", LABEL, "--state", "all",
                           "--limit", "1000", "--json", "number,title"))
    overview = [i["number"] for i in listed if i["title"] == OVERVIEW_TITLE]
    path = os.path.join(tmp, "overview.md")
    open(path, "w", encoding="utf-8").write(
        overview_body(areas, area_issues(with_comments=False) if run else issues))
    if overview:
        c = ["issue", "edit", str(overview[0]), "-R", REPO, "--body-file", path]
    else:
        c = ["issue", "create", "-R", REPO, "--label", LABEL, "--title", OVERVIEW_TITLE,
             "--body-file", path]
    print(shlex.join(["gh", *c]) + ("" if overview else "   # then pin it"))
    if run:
        print(gh(*c).strip())
        if not overview:
            print("pin the overview issue on GitHub (Pin issue, on its page)")
    else:
        print(f"# bodies written to {tmp}; nothing created (add --run)")


def main():
    if not os.path.exists(AREAS):
        sys.exit(f"run from the repository's top folder ({AREAS} not found)")
    areas, skip = parse()
    all_sources = sources()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    try:
        if cmd == "list":
            total = 0
            for a in areas:
                lines = area_lines(a, all_sources)
                total += lines
                print(f"{a['id']}  {lines:5d} lines  {len(area_files(a, all_sources)):3d} files  {a['title']}")
            print(f"      {total:5d} lines in {len(areas)} areas")
        elif cmd in ("files", "show") and len(sys.argv) == 3:
            a = find(areas, sys.argv[2])
            if cmd == "show":
                print(f"### {a['id']} {a['title']}")
                print("\n".join(a["notes"]).strip())
                print(f"\n{area_lines(a, all_sources)} lines:")
            for f, first, last in area_files(a, all_sources):
                print(fmt(f, first, last))
        elif cmd == "check":
            problems = check(areas, skip, all_sources)
            for p in problems:
                print(p)
            if problems:
                sys.exit(1)
            print(f"{len(areas)} areas cover every source file in src/")
        elif cmd == "status" and len(sys.argv) <= 3:
            print_status(status(areas, all_sources, sys.argv[2] if len(sys.argv) == 3 else None))
        elif cmd == "next":
            rows = status(areas, all_sources)
            if not any(i for _, i, _ in rows):
                sys.exit("No area has an issue yet: a maintainer runs tools/bug-hunt/areas.py make-issues")
            r = pick_next(rows)
            if not r:
                sys.exit("every area is claimed right now: try again later")
            a, issue, st = r
            print(f"{a['id']}  issue #{issue['number']}  {a['title']}")
            if st["resume"]:
                print(f"continue the hunt in {st['resume'][1]}: it stopped at {st['resume'][0]}")
        elif cmd == "make-issues":
            rest = sys.argv[2:]
            make_issues(areas, all_sources, {find(areas, x)["id"] for x in rest if x != "--run"},
                        "--run" in rest)
        else:
            sys.exit(__doc__)
    except ValueError as e:  # an anchor that moved: check says where
        sys.exit(f"{e} (run tools/bug-hunt/areas.py check)")


if __name__ == "__main__":
    main()
