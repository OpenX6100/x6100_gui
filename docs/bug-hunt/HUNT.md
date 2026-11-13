# Bug hunt: instructions for an AI agent

Got tokens left over that would expire? Point your agent here:

> Read docs/bug-hunt/HUNT.md in OpenX6100/x6100_gui and follow it.

The agent picks an area nobody is working on, reads every line of it,
runs a code review on it, checks what it finds, and reports on GitHub.
Anyone can do this at any time; several people at once is fine. Most of
this code was written by hand before AI tools, and much of it has never
had a review: the goal is a review of every line.

**Agent: everything below is for you. Follow it step by step.**

## The rules

1. **Change no code in the project.** No commits, no branches, no pull
   requests, no pushes. Your output is GitHub comments (and, for
   confirmed bugs, issues). Scratch files go in a folder outside the
   repository.
2. **You may build and run on this PC:** the unit tests, the sanitizers
   (AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer), and
   small test programs of your own in a scratch folder. That is how a
   suspected bug becomes a confirmed one.
3. **Never touch a radio.** No USB console, no CAT port, no SD card, no
   network access to a radio, even if one is plugged in. Bugs that need
   the radio to check are reported as *suspected*, with what someone
   should try on it.
4. **Bugs only.** Something that does the wrong thing: crashes, hangs,
   leaks, races, wrong results, lost data, a transmitter keyed when
   nobody asked. No style remarks, renames, refactors, "could be
   simpler", missing comments, or wishes for features.
5. **Say how you know.** Every finding says whether it was reproduced
   or read from the code. Never write as if it was seen on a radio.
6. **Stay in your area.** Read whatever the area calls into, but report
   bugs in the area's own files. A bug you trip over somewhere else: a
   short note in your hunt comment, under *Found elsewhere*.
7. **Save as you go.** You may run out of tokens at any moment. Your
   hunt comment on GitHub (step 2) is your only memory: update it after
   every step below, so a sudden stop loses one step at most, and the
   next agent carries on where you were.

## Step 1: set up

- A full clone of `OpenX6100/x6100_gui` (not `--depth 1`: the status
  tool compares commits), up to date (`git pull` on `main`), with
  `git submodule update --init --depth 1 lvgl`.
- The GitHub CLI `gh`, logged in (`gh auth status`). You post as the
  person running you.
- Write down the commit you hunt: `git rev-parse --short HEAD`.

## Step 2: pick an area and claim it

```
tools/bug-hunt/areas.py status
```

lists every area's issue: hunts done, the commit each was done on,
whether the area's code changed since, a hunt left half done, and who
has it claimed now. `tools/bug-hunt/areas.py next` prints the area to
take: **a half-done hunt first**, then the one with the fewest hunts
(an area whose code changed since its last hunt counts as not hunted).
If the person running you named an area, take that one.

**Your hunt comment.** One comment on the area's issue holds your
whole hunt: the claim, your progress and your findings. Write it to a
file outside the repository (`hunt.md`), post it, and from then on edit
the same comment. Start with (fill in the `<...>`):

```
<!-- bug-hunt -->
**Bug hunt** of <Ann>
State: working
Commit: <short sha>
By: <GitHub name of the person running you>, <your model>
Until: <now + 2 hours, UTC, as 2026-11-14T08:00Z>
Stopped at: starting
Continues: <link to the hunt comment you continue, or "none">

#### Pass 1: code review
(not run yet)

#### Pass 2: files read
(none yet)

#### Findings
(none yet)

#### Found elsewhere
(none)
```

Post it **before you start reading**:

```
gh issue comment <issue> -R OpenX6100/x6100_gui --body-file hunt.md
```

Then run `tools/bug-hunt/areas.py status <Ann>`. If it shows someone
else's claim (an earlier comment than yours), you were both quick: set
yours to `State: withdrawn`, update it, and pick another area.

**Update it** after every step, the same command with `--edit-last`
added:

```
gh issue comment <issue> -R OpenX6100/x6100_gui --edit-last --body-file hunt.md
```

(`--edit-last` edits your newest comment on that issue: don't post other
comments there during your hunt.) **Each update sets `Stopped at:` to
where you are, and `Until:` to now + 2 hours.** A claim lasts **2 hours
after GitHub last saw the comment edited** (`Until:` is only for people
reading it; the status tool goes by the edit time): an agent that died
silently frees its area two hours after its last update, and the next
one continues from its comment. So update at least once an hour, even
in a long step.

**Continuing a half-done hunt** (`next` gave you its link): read that
comment. Don't run the code review again. Copy its pass 1 list, files
read and findings into your own hunt comment, set `Continues:` to its
link, and start with its first unchecked item and the first file not
read.

Then read the area's notes and file list:

```
tools/bug-hunt/areas.py show <Ann>
```

The **Look hard at** lines in the notes come from bugs already found
in this code. Also read `docs/bug-hunt/AREAS.md`'s top section, and
`AGENTS.md` if the repository has one (how the code is built and the
traps of this radio).

**Keep a reserve.** When you think less than about a sixth of your
budget is left, stop hunting and go to step 8 with `State: partial`.
A clean stop helps the next agent more than one more finding.

## Step 3: what this code is

The X6100 is a small HF radio. This program is its screen and front
panel: C and C++ on LVGL 8.3, running on a 32-bit ARM board (Allwinner
R16, 4 cores, 512 MB) under Linux. It talks to the base board (the
actual radio) over i2c-0, and it **can key a transmitter, often with a
power amplifier after it**. Things that matter here more than usual:

- **Transmitting:** anything that starts a transmission no one asked
  for, keeps transmitting after a window closes / a band changes / an
  error, or keys the wrong audio path. This is the worst kind of bug.
- **Threads:** LVGL is single-threaded. Decoders, audio, network and
  Bluetooth run on their own threads and must hand work to the LVGL
  thread (`scheduler.cpp`, `lv_async_call`, a lock). LVGL touched from
  another thread corrupts memory far from the cause.
- **LVGL object lifetimes:** callbacks, timers and async calls that run
  after their window (and its objects) were deleted.
- **Power is cut without warning:** files written in place instead of
  `.tmp` + rename are lost or half-written.
- **Fixed buffers:** `char buf[N]` with `strcpy`, `sprintf`, `strcat`,
  text and packets from the air or the network (untrusted).
- **The network:** CAT, the web pages and WiFi face the LAN; anything
  that lets another computer key the radio or change settings without
  being asked to is a bug.
- **Small machine:** memory that grows without limit, work in a loop
  that never sleeps.

## Step 4: pass 1, the code review

**Claude Code:** run the code review on the area's files (the list from
`tools/bug-hunt/areas.py files <Ann>`, separated by spaces), e.g.

```
/code-review high src/cat/
```

A part of a file (`src/dialog_js8.c:3012-5915`) can't be given with its
lines: give the file and say which lines are the area's. The review
reads the files as they are (there is no diff), takes a few minutes and
about 120,000 tokens for a 4,000-line area, and returns its top ten
findings **plus a list of the ones its cap cut**: those are candidates
too.

**Other agents** (Codex, Gemini, ...): do the same by hand, file by file:
what can go wrong on each path, with the classes above.

**Save it at once:** every candidate goes into *Pass 1* as an unchecked
line, then update the comment. The review is the most expensive step;
this way it is never lost.

```
- [ ] 1. src/cat/cat.cpp:237: feed() returns 0 for a half frame and at EOF; Bluetooth drops the client
- [ ] 2. ...
```

**A review's findings are candidates, not bugs.** Most are right, some
are by design or only true on paper. They all go through step 6.

## Step 5: pass 2, every line

A code review reports its top findings. Now read **every line** of the
area, file by file, the parts the review skipped first:

- error paths: what happens when a call fails, a file is missing, a
  list is empty or full, a thread can't start
- the edges: first and last item, zero, negative, the largest value,
  wrap-around, a window closed while its work still runs
- clean-up on close: every timer, thread, callback and setting the
  window started or changed, on every way out (ESC, another app
  opening, power button, band change, an error)
- everything the "Look hard at" line names

New candidates go into *Pass 1*'s list too (`- [ ] 14. ... (pass 2)`).
**After each file**, add it to *Pass 2* (`- [x] src/cat/cat.cpp`, or
`src/cat/cat.cpp to line 400` if you stop inside it) and update the
comment.

## Step 6: check each candidate

For each candidate, from the review or your own reading:

1. **Read the code again**, the whole path: who calls it, with what,
   on which thread. Many candidates die here (a check two calls up, a
   caller that never passes that value).
2. **Try to reproduce it on the PC** if you can: a unit test run under
   the sanitizers, or a small test program in your scratch folder that
   calls the function with the bad input. The unit tests:
   `./run_tests.sh` (CMake, Catch2, AddressSanitizer + UBSan; needs
   `libboost-dev libfftw3-dev libsqlite3-dev`). Most window code
   (`dialog_*.c`) can't run on a PC: then reading has to do.
3. **Search for it** on GitHub, open and closed issues and PRs, so you
   don't report a known one:
   `gh search issues --repo OpenX6100/x6100_gui "<function or file>"`.
4. Give it a **confidence**:
   - **confirmed**: reproduced (test output or sanitizer report)
   - **likely**: traced end to end in the code, with a concrete input
     or sequence of steps that gets there
   - **suspected**: needs the radio, timing, or knowledge you don't have
     to settle; say what would settle it

   Drop what you can't make *suspected* at least: "might be a problem"
   with no way to get there is not a finding.
5. Give it a **severity**:
   - **S1**: transmits unasked or won't stop transmitting; lets another
     computer key the radio; could harm the radio or amplifier
   - **S2**: crash, hang, memory corruption, lost or corrupted settings,
     logs or messages
   - **S3**: wrong behaviour the user sees (wrong value, frequency, decode,
     text; a setting not kept)
   - **S4**: small: a leak that takes days, a wrong message, a rare edge

**After each candidate**, tick it in *Pass 1* with the result
(`- [x] 1. ... → finding 2` or `- [x] 3. ... → dropped: the caller checks
the length, cat.cpp:180`), add the finding (step 7) if it is one, and
update the comment.

## Step 7: the findings

Findings go into your hunt comment's *Findings*, worst first:

```
##### 1. S2 likely: <one line: what goes wrong>
`src/cat/cat.cpp:212` (`function_name`)
**What happens:** <the concrete steps or input, and what goes wrong>
**Why:** <the code path, a few lines quoted if it helps>
**How checked:** <read the callers / reproduced with ... / needs the radio: try ...>
**Fix idea:** <one line, optional; no patch>
```

**Only for findings that are confirmed, or S1/S2 likely:** open its own
issue too, so it gets fixed:

```
gh issue create -R OpenX6100/x6100_gui --title "[bug hunt <Ann>] <what goes wrong>" --body-file finding.md
```

with the same text plus a link to your hunt comment, and put the
issue's number after the finding's title. Everything else stays in the
comment until a person promotes it.

A comment holds about 60,000 characters. If yours gets close, move
*Findings* into a second comment of your own (without the
`<!-- bug-hunt -->` line), link it from the first, and from then on
update the first one with the `gh api` way below.

## Step 8: finish

Set the top of your hunt comment, even when you stopped early, and
update it one last time:

```
State: done | partial
Stopped at: <file:line and the next unchecked candidate, if partial>
```

and add at the end:

```
#### Summary
Covered: <the files read, all or which>
Findings: <n> (S1 <n>, S2 <n>, S3 <n>, S4 <n>), issues opened: #...
Not checked: <what you left out, and why>
```

`done` means every line was read and every candidate checked.
`partial` leaves the area to the next agent, who starts from your
*Stopped at*. Then tell the person running you what you posted, with
the link.

If your comment ever has to be edited and `--edit-last` would hit the
wrong one: `gh api -X PATCH repos/OpenX6100/x6100_gui/issues/comments/<id>
-F body=@hunt.md`, the `<id>` from the comment's link
(`#issuecomment-<id>`).

## For maintainers

- **Areas** live in [AREAS.md](AREAS.md). *Merge checks* runs
  `tools/bug-hunt/areas.py check`: a PR that adds a source file must put
  it in an area. Split an area once it passes 8,000 lines.
- **Issues:** one per area (`Bug hunt Ann: <title>`, label `bug-hunt`)
  and one pinned overview, made by `tools/bug-hunt/areas.py
  make-issues` (prints the `gh` commands; `--run` runs them; then pin
  the overview). After adding an area, run it again: it makes the new
  area's issue and updates the overview.
- **Findings** in hunt comments are not issues yet: read them, and turn
  the real ones into issues (or fix them).
- **Cross-cutting hunts** (one theme through all the code: threads and
  LVGL, transmit safety, file writes, buffers) are areas too, `X1`, `X2`,
  ..., added once the areas have had a first pass. Their files blocks
  may name code that is in A areas, of any size: the coverage check
  skips them. Their notes say what to follow and how to split the work.
