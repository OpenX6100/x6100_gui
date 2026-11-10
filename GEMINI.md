# Gemini: read AGENTS.md

The project's guide for every AI tool is **[AGENTS.md](AGENTS.md)**. Read it
in full before suggesting or changing code. The rules that matter most:

1. Nothing transmits on its own when an app opens or starts.
2. Never poll i2c-0 (`/dev/rtc1`) in a loop: it is the base board's bus.
3. No changes to looks, behaviour, defaults or structure without the person
   deciding: give options with a recommendation.
4. Each app's window case in `src/main_screen.c` ends in its `break;`;
   every source file stays in `src/CMakeLists.txt` (`tools/checks/`).
