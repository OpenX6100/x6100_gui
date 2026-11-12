---
name: new-app
description: Add a new app (a window opened from the APP pages) to the X6100 GUI, or check that an app branch registers its app the way every other app does. Use when someone says "/new-app NAME", "add an app", "register my app", "give my app a button", or is bringing an app from their fork into OpenX6100.
---

# /new-app: add an app the way the others are added

The app's name is in `$ARGUMENTS` (ask if missing). Read AGENTS.md first,
"Adding an app" and "What an AI may not decide alone": the button's label,
its page and anything the user will see are the person's choice.

Every app touches the same five shared places. Put each of the app's lines
**under its own name line** (`/* <NAME> */`), the way the existing apps do,
so app PRs merge in any order (#313).

1. **Number:** in `src/settings_types.h`, `ACTION_APP_<NAME> = <n>,` with
   the next free number after the highest one, written out. Check the
   open PRs too (`gh pr list` and their diffs of `settings_types.h`): two
   apps must not take the same number. Never change or reuse a number:
   numbers are saved on users' cards (long-press keys).
2. **Button:** in `src/buttons.cpp`,
   `static button_data_t btn_<name> = make_app_btn("<Label>", ACTION_APP_<NAME>);`
   under its name line, then `&btn_<name>,` **at the end of the last APP
   page**. A full page (the page button + 4) means a new page: its page
   button `(APP n:m)`, every other page button's `m` updated, the page in
   `buttons_group_app`. If another open PR needs the same new page, make
   it one shared commit merged into both branches. No empty places.
3. **Window case:** in `src/main_screen.c`, `#include "dialog_<name>.h"`,
   then in `main_screen_start_app()`:
   ```c
           /* <NAME> */
           case ACTION_APP_<NAME>:
               dialog_construct(dialog_<name>, obj);
               voice_say_text_fmt("<Name> window");
               break;
   ```
   and its label in `main_screen_action()`'s list that calls
   `main_screen_start_app(action)`.
4. **Build list:** the app's `.c`/`.cpp` files in `src/CMakeLists.txt`
   under `# <NAME>` (shared files it needs may be listed again under its
   name; duplicates are fine).
5. **The window itself:** `src/dialog_<name>.c/.h` with a `dialog_t` and
   `extern dialog_t *dialog_<name>;`, built like an existing app (copy the
   shape of a small one, e.g. `dialog_jtty.c`). **Nothing transmits when
   it opens.** Restore on close anything it changes on the radio (filters,
   DSP, power).

Then check:

```sh
python3 tools/checks/check_app_cases.py
python3 tools/checks/check_build_list.py
tools/radio/x6100-syncheck src/main_screen.c src/buttons.cpp src/dialog_<name>.c   # if set up
```

Report to the person: the number taken, where the button sits (page and
place), the files added, the check results, and what they decide (label,
page). A test image from the branch and a radio test come before the PR.
