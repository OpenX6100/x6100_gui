---
name: close-session
description: End a working session on the X6100 GUI so nothing lives only in the chat - running builds noted, plan file updated, everything committed and pushed, the radio's state recorded, and a plain-words report. Use when the person says "/close-session", "offload", "wrap up", "closing down", "end of session", or is about to stop for the day.
---

# /close-session: nothing left only in the chat

Do every step without asking each time, then report. If the person's fork
has its own closing checklist (its CLAUDE.md or AGENTS.local.md), follow
that one; this is the general one.

1. **Running work:** every build, CI run, background job or test started
   in this session. Finish what can be finished now. For the rest, write
   down where it is: the run ID or link and the exact command to pick it
   up.
2. **The plan file** (e.g. `docs/PLAN.md`, wherever the person keeps their
   one "where are we" list): tick what's done, update each item touched
   (its state, what the person decided and when, what's waiting on whom),
   add items for anything found or promised, and put dropped ideas on the
   "decided: not doing" list. Status lives there only, not in the AI's own
   memory.
3. **Docs:** anything users or testers read whose behaviour changed (the
   README, release notes, the PR's description).
4. **Git and GitHub:** everything committed and pushed (`git status` clean
   on every branch used); results posted on open PRs only where the person
   approved the text; say which branch is current.
5. **The radio:** what's on the SD card (which image, which commit), the
   newest DATA backup, nothing left running on it that matters.
6. **Lessons:** a new rule or preference the person stated goes in their
   notes (`CLAUDE.local.md`, `AGENTS.local.md`) or, if it's for everyone,
   as a proposed change to AGENTS.md in its own PR.
7. **Report in plain words:** what was done, what is still running or
   waiting (and on whom), and anything the person must do themselves (a
   merge, a post, a radio test, flashing the card).
