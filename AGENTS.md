# OpenX6100 GUI: guide for AI assistants

This is the one guide for any AI tool working on this repository (Claude
Code, Codex, Copilot, Gemini, Cursor, ...). `CLAUDE.md`, `GEMINI.md` and
`.github/copilot-instructions.md` only point here, so every tool reads the
same rules. For the people using those tools, the companion page is
[docs/working-with-ai.md](docs/working-with-ai.md): how we work with AI here,
and why.

**Read this whole file before changing anything.** It is short on purpose.
Every rule in it was learned the hard way, on real radios.

## The project

The alternative firmware GUI for the Xiegu X6100: one C/C++
program, `x6100_gui`, drawn with LVGL 8.3, running on the radio's Linux
system (OpenX6100/AetherX6100Buildroot) and talking to the radio's base
board through X6100Control. Started by R1CBU, continued by the OpenX6100
group. Several people build apps for it (FT8, JS8, WSPR, PSK31, Olivia,
RTTY, SSTV, WeFax, NavTex, JTTY, ...), many of them with AI help, and many
of them hams first and programmers second. The maintainers review every
change and decide the project's direction.

## Safety on a live radio

A test radio is live hardware, often with an amplifier behind it that takes
its band from the radio. An AI with a console on the radio must follow
these, and so must its code:

1. **Nothing transmits on its own.** No app may key the transmitter when it
   opens, starts or loads settings. Transmitting is always the operator's
   act (a button, a key, an answer they turned on themselves).
2. **Never poll i2c-0** (`/dev/rtc1` or anything else on it) in a loop. It
   is the GUI's control bus to the base board; a loop on it once glitched
   an amplifier's band voltage. One write (setting the clock) is fine.
3. **Hands off during a flash or a reboot.** If the console shows a boot, a
   login prompt you didn't expect, or the stock `XIEGU-x6100 login:`, type
   nothing (not even Enter) and ask the person. `tools/radio/x6100-console`
   refuses to type in those states.
4. **Tell the person before any command that touches the hardware** (audio
   devices, the transmitter, Bluetooth, i2c, writing files the GUI uses),
   and only with the radio idle. Reading logs and `/proc` is fine.
5. **Don't break something that works to test a tool** (re-crashing an app,
   re-running a flash).
6. **Write the SD card only when the person says so.** Back up DATA first
   (settings, logs, QSOs: `tools/radio/x6100-flash` does it). For an image
   not from our CI, write the `.img` alone, never every file in its zip,
   and know which backup brings the old settings back.
7. **One card, several AI sessions:** if more than one session works on the
   same radio, check what's on the card before writing it.

## What an AI may not decide alone

The person who opens the PR answers for it, however it was written. AI help
is welcome and gets the same review as anything else. Without the person
(and, for the project, the maintainers) deciding, an AI must not change:

- **Looks:** colours, fonts, layout, sizes, the theme.
- **Behaviour:** what a button, key or knob does; what happens when an app
  opens or closes.
- **Defaults** that every user gets.
- **Structure:** new threads, new libraries or packages, a new settings
  store.
- **Scope:** features beyond what was asked; "while I was here" clean-ups
  of other people's code.

When a choice is needed, give **2 to 4 options with a recommendation**, each
explained in plain words (what the thing *is*, not only its name), and let
the person pick. For anything visual, **mock-ups first**: drawn on a real
screenshot of the radio, at true pixel positions, with marks big enough for
the 5" screen. Write the decision down (who, when) where the next session
will find it.

## How a change gets in

1. **A branch per change**, with a plain name (*Build image* names its
   downloads after the branch). One topic per branch.
2. **A test image** from the branch: Actions > *Build image* > Run workflow
   (about an hour; `buildroot_ref` picks an open Buildroot PR's branch to
   test with). The image and the unstripped GUI stay on the run's page for
   30 days.
3. **Test on a radio** before the PR. Say in the PR which image (run) was
   tested and what was tried. Station-to-station features need two
   stations (a second radio or SDR with desktop software, a dummy load).
4. **The PR** (the template asks for the rest). One topic per PR.
   **Anything outside an app is its own PR**: shared code that comes with
   an app (a log, a widget, a web page) goes separately, so it can be
   reviewed by the people who use it.
5. **Review by a maintainer.** Nobody merges their own PR without an
   approval. Before merging, **read the PR's last comments** for a
   question nobody answered or a change someone promised: a batch merge
   once shipped a range we had asked to shrink (#305, fixed in #332).

## Adding an app

An app touches the same few shared lines as every other app, so several
app PRs can be open at once and merge in any order **if each app's lines
sit under its own name line** (`/* NavTex */`, #313):

- **Its number:** `ACTION_APP_<NAME>` in `src/settings_types.h`, the next
  free number, written out. Numbers are saved on the card (long-press
  keys), so a number is never reused or changed.
- **Its button:** `make_app_btn(...)` in `src/buttons.cpp`, and a place
  **at the end** of the APP pages. **No empty APP buttons** (an empty
  button looks like a bug); add a page when the last one is full.
- **Its window case** in `main_screen_start_app()` and its label in
  `main_screen_action()` (`src/main_screen.c`). **Each window case ends
  in its `break;`**: a "keep both sides" merge of two new apps drops it,
  and one app's button then opens two windows.
- **Its files** in `src/CMakeLists.txt`, under its name line.
- A change several apps need (e.g. a new APP page) is **one shared commit**
  merged into each app's branch, so git sees the same change on both sides.

`tools/checks/` checks the window cases and the build list on every PR. On
Claude Code, the `/new-app` skill walks through it.

## Merging

- **Check every merge, not only the conflicts.** After a merge that brings
  in an app, run `tools/checks/check_app_cases.py` and
  `tools/checks/check_build_list.py`. A compile
  check doesn't link, so a dropped file in the build list shows up only
  in the image build (`tx_level.c` and `tx_player.c` were lost that way,
  #311/#312).
- **Check a pair of branches before merging:**
  `git merge-tree --write-tree --name-only A B` lists the real conflicts.
- **rerere:** if `rerere.autoupdate` is on, a merge can stop with no
  conflict markers because git replayed an old answer. Compare the
  conflicts merge-tree lists with what you resolved yourself, and read
  rerere's answers before committing.
- **A PR that is mostly a merge:** its "Files changed" is mostly other
  people's work, so the description shows your own lines, with a link to
  each.

## Build and test

- **The image:** Buildroot builds it, locally (README, *Building*) or on
  GitHub with *Build image* (about an hour). There is no quick way to run
  the whole GUI on a PC.
- **Quick checks (seconds),** all in `tools/checks/`, all run by CI on every
  PR (*Merge checks*), all runnable on a PC:
  - `check_app_cases.py`: every app's window case ends in its `break;`
    (and a warning when an app doesn't announce its window for the voice
    mode, which tells a blind operator what opened);
  - `check_build_list.py`: every source file is in the build list;
  - `check_tests_in_ci.py`: every unit test is run by CI;
  - `check_ratchets.py`: risky patterns only get rarer, file by file
    (`ratchets.json`): i2c and `/dev/rtc1` only where they are today.
    Raising a number there is the reviewers' decision: say why in the PR.
- **Compile check for ARM:** `tools/radio/x6100-syncheck src/file.c ...`
  with a local Buildroot build or the matching toolchain (see the file).
- **Unit tests** (Catch2, on a PC): `./run_tests.sh` builds and runs them
  all; CI (*Unit tests*) runs all of them in two jobs, because JS8 needs
  Ubuntu 24.04's Boost and the others need 22.04's libgpiod 1.6 (plus
  SQLite and liquid-dsp at the image's versions, built in the job). A new
  test goes in `tests/CMakeLists.txt` **and** `.github/workflows/tests.yml`.
  Catch2 splits test names at commas: escape `\,`.
- **A fresh checkout** needs `git submodule update --init` (LVGL, its
  drivers).
- **Never say "done" without checking**, and say where each claim comes
  from: the code, a test, a CI run, or the person's test on the radio.

## Code map

| Where | What |
|---|---|
| `src/main.c` | Start-up, the two screens (`obj` and `overlay_scr`) |
| `src/main_screen.c` | The main screen, app start (`ACTION_APP_*` cases) |
| `src/buttons.cpp` | The button pages, the APP pages |
| `src/settings_types.h` | Action and app numbers (saved on the card) |
| `src/dialog.c` | The app window framework (`dialog_construct`, close) |
| `src/dialog_<app>.c` | One app each (FT8, JS8, WSPR, PSK31, Olivia, SSTV, ...) |
| `src/radio.c`, `src/dsp.cpp` | The radio (base board via X6100Control), the receive DSP |
| `src/spectrum.c`, `src/waterfall.c` | The main spectrum and waterfall |
| `src/styles.c` | The look |
| `src/cfg/` | Settings (`params.db` on DATA); `migrations.c` when a stored setting changes |
| `src/tx_player.c`, `src/tx_level.c` | Shared transmit audio and level for the digital apps |
| `third-party/js8core/` | The JS8 engine (GPL-3.0) |
| `tests/` | Unit tests |
| `tools/checks/` | Merge checks (CI) |
| `tools/radio/` | Console, screenshot, CPU log, flash, compile check |

The DATA partition holds the user's settings, logs and inboxes: never
delete or rewrite a user's file without a backup (write a `.tmp`, then
rename).

## Lessons from the code

- **Two screens on two layers:** the spectrum and waterfall are on
  `overlay_scr`, drawn **above** the main screen (`obj`). Anything shown
  over the spectrum must be a child of that layer; on `obj` it is hidden.
- **LVGL 8.3:** resetting an input device turns a held key into a long
  press in the next focus (deleting the focused object from its own key
  handler does it: call `lv_indev_wait_release(lv_indev_get_act())`
  first); the keypad drops a press while another key is down; an encoder
  never sends `LONG_PRESSED` to an editable object (use
  `LONG_PRESSED_REPEAT`); `lv_img` never covers the area under it.
- **No flicker or stutter:** pace waterfall rows by `CLOCK_MONOTONIC`, not
  an `lv_timer` period; redraw only what changed.
- **Two ways to key the transmitter:** `radio_set_ptt()` (CAT PTT) sends
  the radio's mic/line input; `radio_set_modem()` sends the codec, where
  app audio goes.
- **App audio played on the speaker comes back into capture** on the base
  board: a decoder hears its own beeps unless it is fed silence then.
- **A udev `KERNEL=="hci*"` rule also matches every Bluetooth
  connection** (`hci0:<handle>`); match the adapter with
  `ENV{DEVTYPE}=="host"`.
- **GitHub forks can have a tag and a branch with the same name:** pass
  commit SHAs to tools.
- **Match the reference program.** For a mode that exists on the desktop
  (FT8 and WSJT-X, JS8 and JS8Call), read its source and do what it does;
  compare decoders with real recordings on a PC.

## Reading a GUI crash

*Build image* keeps `x6100_gui-unstripped.<branch>`, the same program before
stripping. It is a Release build, so it gives function names, not source
lines or variables. With a core dump from the radio (`prlimit --pid $(pidof
x6100_gui) --core=unlimited:unlimited` and a `core_pattern` into `/tmp`,
which is RAM), read it with the toolchain's `gdb`, or resolve return
addresses with `addr2line` on the unstripped GUI (it is position
independent: subtract its load address from the core's file maps). The
GUI's log on the radio is `/tmp/x6100_log.txt`; it is block-buffered, so
the last lines before a crash may be missing.

## Writing for people

- **Plain words.** Many of us aren't programmers. Name the layer a change
  touches (an app, the shared GUI, the Linux image, the base board), and
  prefer a fix inside the app over one in shared code.
- **In anything posted under someone's name** (a PR comment, an issue, a
  reply), keep apart what they saw on the radio (quote it), what the code
  shows (say "from the code") and what is only a guess (say so, or leave
  it out). Show them the text before it is posted.
- **Licences:** the GUI's own files are LGPL-2.1-or-later and keep their
  licence; `third-party/js8core` is GPL-3.0, so the program as a whole is
  GPL-3.0 (README, *License*). Code from elsewhere needs its licence and
  credit; never paste code whose licence you don't know.

## Skills (Claude Code)

`.claude/skills/` holds repeat jobs as checklists, plain Markdown that any
tool can follow: `/new-app` (adding an app), `/pr-check` (before a PR or a
merge), `/close-session` (end of a working session: nothing left only in
the chat).

## Your own notes

Things only for your PC (the radio's port, your card, private plans) go in
`CLAUDE.local.md` or `AGENTS.local.md` (both ignored by git), never in this
file.
