# Claude Code: read AGENTS.md

The project's guide for every AI tool is **[AGENTS.md](AGENTS.md)**: the
safety rules for a live radio, what an AI may not decide alone, how a change
gets in, adding an app, merging, building, the code map. Read it in full
before changing anything.

This file only exists because Claude Code loads `CLAUDE.md` by itself.
Keep project rules in AGENTS.md, so every tool reads the same ones; your own
notes go in `CLAUDE.local.md` (ignored by git). Repeat jobs are skills in
`.claude/skills/` (`/new-app`, `/pr-check`, `/close-session`).
