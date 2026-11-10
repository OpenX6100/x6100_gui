---
name: pr-check
description: Check a branch before its PR is opened, or a PR before it is merged, for the X6100 GUI - the merge checks, open questions in the PR's comments, whether it was tested on a radio, scope, and what the description must say. Read-only unless the person asks for fixes. Use for "/pr-check", "/pr-check 372", "is this ready for a PR", "can I merge these", "check before merging".
---

# /pr-check: ready for a PR, or ready to merge?

Target in `$ARGUMENTS`: a PR number or URL (`gh pr view`), several of them,
or nothing (the current branch against `upstream/main` or `origin/main`).
Read AGENTS.md first. Don't post anything: report to the person, who
decides. For several PRs, do each and give one list at the end.

1. **The checks** (on the PR's head: `gh pr checkout <n>` in a scratch
   worktree, or the current branch):
   `python3 tools/checks/check_app_cases.py` and
   `python3 tools/checks/check_build_list.py`, `check_tests_in_ci.py`,
   `check_ratchets.py`.
   Also the PR's CI (`gh pr checks <n>`).
2. **Conflicts with other open PRs:** for each open PR touching the same
   files (`gh pr list --json number,files`),
   `git merge-tree --write-tree --name-only <this> <that>`. Shared app
   lines (numbers, buttons, cases, build list) must sit under each app's
   name line.
3. **Open questions** (merging only): read the PR's last comments and
   review threads (`gh pr view <n> --comments`, `gh api
   repos/{owner}/{repo}/pulls/<n>/comments`). List every question nobody
   answered and every change someone promised and didn't push. A batch
   merge once shipped a value that was still being discussed (#305).
4. **Tested?** The description says which image (run) went on a radio and
   what was tried. Commits pushed after that test: say which.
5. **Scope:** one topic. Anything outside an app (a shared widget, log,
   web page) should be its own PR. Look for looks, behaviour or default
   changes the description doesn't mention (AGENTS.md, "What an AI may not
   decide alone").
6. **Safety:** anything that could key the transmitter without the
   operator, i2c access, file writes on DATA without `.tmp` + rename.
7. **A PR that is mostly a merge:** the description should show the
   author's own lines with links, since "Files changed" is mostly other
   people's work.

Report, per PR: **ready / not yet**, then each problem with `file:line` or
the comment's link, from the code or from the PR (say which), and what the
person (not the AI) should decide.
