# Working on the X6100 GUI with AI

Many of the apps in this firmware were written with an AI assistant (Claude,
ChatGPT, Copilot, Gemini, ...), often by hams who aren't programmers. That's
welcome. This page is how we do it so the radio stays safe, the reviewers
aren't swamped, and the next person (or the next AI session) can pick up
where you left off. It's written from what worked, and what went wrong, over
a few months of daily work on a real radio.

Your AI reads [AGENTS.md](../AGENTS.md) by itself (most tools look for it, or
for `CLAUDE.md`, `GEMINI.md` or `.github/copilot-instructions.md`, which point
to it). This page is for you.

**The one rule behind all of it: you answer for your PR, however it was
written.** AI code gets the same review as anyone's.

## The loop

```
 idea ─► options ─► mock-up ─► branch ─► test image ─► radio test ─► AI review ─► PR ─► maintainer review ─► merge
           ▲  you pick   ▲ you pick                       │ you report
           └─────────────┴────────────────────────────────┘ (fix, new image, test again)
```

1. **Ask for options, not a solution.** "Give me 3 or 4 ways to do this, with
   a recommendation, and explain each in plain words." You pick. The AI
   shouldn't decide looks, behaviour or defaults on its own (AGENTS.md says
   so too).
2. **Mock-ups before code** for anything you'll see. Ask the AI to draw the
   options on a real screenshot of your radio, at the real size (the screen
   is 5", so marks bigger and thicker than on a desktop). Picking from
   pictures is quick; changing finished code is slow.
3. **A branch per change**, with a plain name (e.g. `tx-meter-led`).
4. **A test image** from your branch: Actions > *Build image* > Run workflow,
   about an hour. The image stays on the run's page for 30 days.
5. **Test on the radio.** You're the tester: try it, tell the AI what you saw,
   let it fix, build again. Several small loops beat one big one.
6. **An AI code review** of the finished branch (in Claude Code:
   `/code-review`). Treat its findings as suggestions: have the AI check each
   one against the code before fixing it. In one review of about 60
   findings, a handful were wrong or were the design working as meant.
7. **The PR**, filled in from the template: what it does, what you see,
   which image you tested. Then a maintainer reviews it.

## Keep notes the AI can find

An AI forgets everything between sessions; only files survive.

- **One plan file** (for example `docs/PLAN.md` in your fork) is the only
  "where are we" list: what's done, what's next, what's waiting on whom.
  Have the AI tick items in the same commit as the work. Copies of the
  status in several places drift apart (we had four once).
- **Write decisions down** with who decided and when ("picked option 3,
  2026-11-05"). Keep a **"decided: not doing"** list too, so a dropped idea
  doesn't come back three sessions later.
- **One topic per session.** Long sessions that wander over many topics
  squeeze out the early details, and the AI starts to guess.
- **Close every session.** Before you stop, ask the AI to: finish or note
  every running build, update the plan, commit and push everything, and
  tell you in plain words what's done and what's waiting. Nothing may live
  only in the chat. In Claude Code: `/close-session`.
- **Your own notes** (your radio's port, your card, private plans) go in
  `CLAUDE.local.md` or `AGENTS.local.md`, which git ignores.

## Make it show you

- **Let the AI see the radio.** [tools/radio](../tools/radio/README.md) has a
  USB console, screenshots, a CPU logger and a card-flash script that keeps
  your settings. With them the AI can read the app's log while you test,
  take a screenshot to draw mock-ups on, or measure whether a new view is
  too heavy.
- **Two stations for station-to-station features.** A second radio or an
  SDR with the desktop program (JS8Call, WSJT-X), into a dummy load at low
  power, lets one person test both ends: messages, relays, replies. When
  you ask for a test, the AI should say which gear it needs switched on.
- **Compare with the reference program.** For a mode that exists on the
  desktop, have the AI read the desktop program's source and copy what it
  does, then compare the decoders with real recordings on the PC. On such
  a bench the FT8 decoder went from about half of WSJT-X's decodes to most
  of them (#362-#371).
- **Flicker is measurable:** a phone's slow-motion video of the screen,
  analysed frame by frame, found a waterfall stutter nobody could see in
  the code.

## Keep it honest

- **"Done" only after checking.** Ask where each claim comes from: the
  code, a test, a CI run, or your test on the radio.
- **Separate what you saw from what the AI thinks.** In a PR comment or an
  issue under your name: what you saw on the radio, what the code shows
  (say "from the code"), and what's a guess (say so, or leave it out). Once
  a confident wrong cause went into a review comment because the AI's guess
  read like a fact; it had to be corrected.
- **Read what goes out under your name** before it's posted.
- **Before merging, read the PR's last comments.** A question asked in a
  PR and never answered got lost in a batch merge, and the release shipped
  a range we had asked to shrink (#305, fixed in #332).

## Keep the radio safe

The full list is in [AGENTS.md](../AGENTS.md), "Safety on a live radio". The
short version:

- **Nothing transmits on its own** when an app opens.
- **Never poll i2c-0 in a loop.** It's the GUI's bus to the base board; once
  it glitched an amplifier's band voltage.
- **Hands off during a flash or a reboot.** Nothing typed, not even Enter.
- **Back up DATA before writing the card**, and write only when you say so.
  With someone else's image, write just the `.img` (a README in the zip
  would land on your DATA), and know which backup brings your settings back.
- **Several AI sessions, one card:** tell each one what the others did, or
  have it check the card first. Once an automatic flash was almost started
  while another session's test build was on the radio.

## Keep merges clean

Apps share a few lines (app numbers, buttons, window cases, the build list).
Each app keeps its lines under its own name line, so app PRs merge in any
order (#313). Two mistakes happened in real merges, compiled fine, and broke
things later; **Merge checks** now catches both on every PR:

- a lost `break;` made one app's button open a second app on top of it;
- a "keep ours" merge of the build list dropped two shared files
  (`tx_level.c`, `tx_player.c`, #311/#312), and only the full image build
  showed it, a week later.

The same goes for tests: until November, CI ran only 3 of the 11 unit
tests. The first run of the other eight found one that had been out of date
for a month (the FT8 protocol setting gained FT2). Now every test runs on
every PR, and a check fails if a new one is left out. Tests that don't run
protect nothing.

## Plain words

If you aren't a programmer, say so to the AI once and ask for plain words.
A good AI explains what a setting *is* before asking you about it, names
the part of the radio a change touches (an app, the shared screen code, the
Linux image, the base board), and prefers a fix inside your app over one in
code everyone shares.

## Where this comes from

Mostly from porting JS8 to the radio with Claude Code, one radio and one
tester, every build tried on the air. The shape of AGENTS.md (one guide
every AI tool reads, written limits on what an AI may decide, repeat jobs as
skills, checks on every PR) comes from
[AetherSDR](https://github.com/aethersdr/AetherSDR), a much bigger ham
project built mostly with AI, scaled down for a group our size.
