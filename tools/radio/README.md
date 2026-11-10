# Tools for working on a real radio

Small helpers that let you, or an AI assistant working for you, see and
control a test radio from a Linux PC over the radio's **DEV** USB-C port:
read logs, take screenshots, measure CPU load, and write a new image to the
SD card without losing your settings. Each one has its usage at the top of
the file. They were written while porting JS8 and are used every day on one
radio; you're welcome to improve them.

**Read [AGENTS.md](../../AGENTS.md), "Safety on a live radio", before
pointing an AI at these.** A radio is live hardware, often with an amplifier
behind it.

| Tool | What it does |
|---|---|
| `x6100-console` | Holds the radio's Linux console (a background `daemon` logs everything), runs commands (`cmd 'ls /mnt' 10`), types text (`send root`), shows the log (`tail`). **Refuses to type while the radio boots or shows the stock Xiegu login** (a firmware flash: hands off). Needs `pyserial`. |
| `x6100-screenshot` | The radio's screen as a PNG in ~/Pictures, over the console (~40 s). Needs ImageMagick. |
| `x6100-cpulog` | Copies a small logger to the radio's RAM and records each core, each GUI thread and the GUI's memory once a second (a climb over hours is a leak): `check`, `start [MIN]`, `mark "the waterfall stuttered"`, `stop`, `fetch` (prints a report). Reads /proc and /sys only, never i2c. |
| `x6100-flash` | Writes an image (a *Build image* run id, an `.img.zip` or an `.img`) to the SD card in a USB reader: backs up DATA first, keeps the DATA partition, checks it afterwards. Finds the card by its BOOT and DATA labels on a USB/removable disk and asks before writing. |
| `x6100-syncheck` | Checks that GUI files compile for the radio in seconds (ARM GCC 12, syntax only), with a local Buildroot build or a matching toolchain. Some files need more headers than a toolchain alone has (`main.c`, `dialog_js8.c`); the image build is the real check for those. |
| `70-x6100-serial.rules` | udev rule so your user can open the radio's serial ports. |

## Getting started

1. Install the udev rule (see the file), plug the radio's **DEV** port into
   the PC (the HOST port is for keyboards and sticks).
2. `x6100-console daemon &` (it finds the radio by its USB id, usually
   `/dev/ttyACM0`; the second port is CAT). The daemon lives as long as the
   terminal or AI session that started it.
3. Look at the radio: if it's on its normal screen (not booting, not
   flashing), `x6100-console send '' --force` once. Right after the daemon
   starts, the tool won't type until the radio shows a prompt, since it
   can't know what happened while nobody was listening.
   `x6100-console tail`: at `X6100 login:` log in with `x6100-console send root`
   and `x6100-console send <password>` (the image's root password).
4. `x6100-console cmd 'tail /tmp/x6100_log.txt'`: the GUI's log, live.

## Good to know

- Always `tail` before typing: a command typed into a login prompt becomes a
  user name, and anything typed during a flash or boot can stop it.
- Serial runs at ~11 KB/s: fine for logs and small files, slow for big ones
  (use the SD card for those).
- The radio's busybox has no `timeout`, `nc` or `logread`; `python3` is there.
- The clock has no network time at home and drifts (~0.5 s a day here): FT8
  and JS8 need it within a second or two. Write the battery clock once with
  `hwclock -w -u -f /dev/rtc1`, never in a loop: rtc1 is on i2c-0, the GUI's
  bus to the base board.
- A GUI crash: *Build image* also keeps `x6100_gui-unstripped.<branch>`, the
  same program before stripping. It is a Release build: it has function
  names (gdb's backtrace, `addr2line -f`), not source lines or variables.
