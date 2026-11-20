# Bug hunt: the areas

The GUI's code (`src/`) split into areas, each small enough for one
session and one code review: 2,000 to 7,000 lines, never over 8,000
(the most the deepest code review takes).
How to hunt one: [HUNT.md](HUNT.md). Each area has its own GitHub issue
(*Bug hunt A07: ...*, label `bug-hunt`) where it is claimed and its
findings are posted.

`tools/bug-hunt/areas.py list` prints every area with its size;
`tools/bug-hunt/areas.py files A07` prints an area's files (what you give
the code review); `tools/bug-hunt/areas.py check` (run by *Merge checks*)
fails when a new source file is in no area. **A new file goes into the
area it works with**, in the same PR that adds it. When an area grows past
8,000 lines, split it in two (a new number at the end, the old issue
says where its files went). A file too big for one area is split into
parts at named definitions: `file:start..name`, `file:name..end` (see
`tools/bug-hunt/areas.py`).

Files that work together are in the same area, even across folders: a
review that sees both sides of a conversation between files finds more.
The areas also list each area's headers. Read what the area calls into
as far as you need, but report only what is in the area (a bug found
elsewhere: a comment on that area's issue).

**Look hard at** lines are starting points from bugs already found
in this code, not limits.

## Shared GUI (the core)

### A01 · Start-up, threads and screen messages
`main.c` (start-up, the LVGL loop), `scheduler.cpp` (how other threads
hand work to the LVGL thread), events, the dialog frame every window uses,
the pop-up messages, small helpers.
**Look hard at:** anything LVGL called from a thread other than the LVGL
thread (the `realloc(): invalid pointer` crash while spinning the dial,
seen 2026-09 and 2026-10, is still unexplained); start-up order; what a
dialog's close leaves behind (timers, callbacks on freed objects).

```files
src/main.c
src/main.h
src/globals.h
src/helpers.h
src/events.c
src/events.h
src/pubsub_ids.h
src/scheduler.cpp
src/scheduler.h
src/lock_manager.c
src/lock_manager.h
src/dialog.c
src/dialog.h
src/msg.c
src/msg.h
src/msg_tiny.c
src/msg_tiny.h
src/textarea_window.c
src/textarea_window.h
src/ui_list.c
src/ui_list.h
src/util.cpp
src/util.h
src/util.hpp
src/format.c
src/format.h
src/settings_types.h
src/common/
src/display.cpp
src/display.h
src/screenshot.c
src/screenshot.h
src/usb_devices.cpp
src/usb_devices.h
src/clock.c
src/clock.h
src/hkey.c
src/hkey.h
```

### A02 · The radio: base board link, PTT, GPS
`radio.c` talks to the base board (the STM32 that is the actual radio)
over i2c-0 through X6100Control: frequency, mode, filters, TX on/off.
The ports and `app_radio.c` are how apps borrow the radio; `diag_log.c`
writes `ptt.log` (who keyed the transmitter).
**Look hard at:** **anything that could key the transmitter, or leave it
keyed**, when nobody asked (a TX with zero power was seen once at start-up,
2026-10-13); every i2c-0 access (never in a fast loop: it once glitched
the amplifier's band voltage); the `i2c-0: sendbytes: NAK bailout` in
dmesg each time the GUI starts (a write the base board didn't take);
settings that are restored when an app closes, on every way out of it.

```files
src/radio.c
src/radio.h
src/app_radio.c
src/app_radio.h
src/app_ports.cpp
src/app_ports.h
src/ports/
src/diag_log.c
src/diag_log.h
src/gps.c
src/gps.h
```

### A03 · CAT control (USB and network)
Icom CI-V emulation for PC programs over the USB serial port and the
network (`cat/lan/`), and the spectrum stream to the PC.
**Look hard at:** CAT PTT (`0x1C`) keys the mic path, `radio_set_ptt()`;
a CAT client connecting can key the radio (suspect in the 2026-10-13
start-up TX); input parsing (lengths, truncated packets); thread safety
with the GUI.

```files
src/cat/
```

### A04 · DSP, audio, transmit audio and level
`dsp.cpp` (the receive chain the spectrum and every app feed on), the
sound card, the shared transmit player and level control (FT8, JS8, WSPR,
JTTY, RTTY), the recorder, the voice (spoken menus for blind operators).
**Look hard at:** buffer sizes and wrap-around; threads (the audio threads
and the GUI); `tx_level.c` raising the drive with no limit when the
power reading can't see the peaks (PSK31/Olivia at under 1 W into an
amplifier); what happens when the sound card fails or returns short.

```files
src/dsp.cpp
src/dsp.h
src/audio.c
src/audio.h
src/tx_player.c
src/tx_player.h
src/tx_level.c
src/tx_level.h
src/recorder.c
src/recorder.h
src/voice.cpp
src/voice.h
```

### A05 · Settings storage
The SQLite settings database (`params.db`), its migrations (one per new
version), delayed writes, memories, the JS8 settings, the ATU table.
**Look hard at:** a migration that runs twice or half (power cut in the
middle); writes lost when the radio is switched off; values outside their
range read back from an old database; SQL built from strings.

```files
src/cfg/db.cpp
src/cfg/db.h
src/cfg/migrations.c
src/cfg/migrations.h
src/cfg/pending_writes.cpp
src/cfg/pending_writes.h
src/cfg/memory.cpp
src/cfg/memory.h
src/cfg/js8_db.c
src/cfg/js8_db.h
src/cfg/atu.cpp
src/cfg/atu.h
src/cfg/digital_modes.cpp
src/cfg/digital_modes.h
```

### A06 · Settings in memory: the cfg API
How the rest of the GUI reads, changes and watches settings (`cfg.*`):
parameters, subjects (change notifications), computed values, the
settings manager.
**Look hard at:** a notification running on the wrong thread or after
its object is freed; band/mode/VFO switches that leave a value from the
old band; subscriptions never removed.

```files
src/cfg/settings_manager.cpp
src/cfg/settings_manager.h
src/cfg/settings_internal.h
src/cfg/cfg_api.cpp
src/cfg/cfg_api.h
src/cfg/cfg_capi.cpp
src/cfg/parameter.h
src/cfg/computed_parameter.h
src/cfg/subject.cpp
src/cfg/subject.h
src/cfg/subject_api.h
src/cfg/encoder_bind_types.h
src/cfg/encoder_defaults.h
```

### A07 · Settings screens, callsign, QTH, frequency entry
The Settings window and its pages, the callsign and QTH (grid) windows,
GPS window, direct frequency entry, ITU region.
**Look hard at:** text fields (length limits, odd input), values saved
from a half-filled screen, what ESC saves.

```files
src/settings/
src/dialog_callsign.c
src/dialog_callsign.h
src/dialog_qth.c
src/dialog_qth.h
src/qth/
src/dialog_gps.c
src/dialog_gps.h
src/dialog_freq.c
src/dialog_freq.h
src/itu_region.c
src/itu_region.h
```

### A08 · Main screen, buttons, knobs and keyboards
The main screen and its app registrations (`ACTION_APP_*`), the button
pages, the VOL/MFK knobs and their modes, the front keys, USB and
Bluetooth keyboards.
**Look hard at:** a `case ACTION_APP_*` without its `break` (opens two
apps); LVGL 8.3 traps (deleting the focused object in its own key
handler turns a held key into a long press elsewhere: needs
`lv_indev_wait_release` first); a key arriving while a window opens or
closes; fast knob turns (the dial-spinning crash, see A01).

```files
src/main_screen.c
src/main_screen.h
src/buttons.cpp
src/buttons.h
src/controls.cpp
src/controls.h
src/knobs.cpp
src/knobs.h
src/mfk.cpp
src/mfk.h
src/vol.cpp
src/vol.h
src/rotary.cpp
src/rotary.h
src/encoder.c
src/encoder.h
src/keypad.c
src/keypad.h
src/keyboard.c
src/keyboard.h
src/kbd_rollover.c
src/kbd_rollover.h
```

### A09 · Spectrum, waterfall, meters, styles
The main spectrum (2D and 3D) and waterfall, the frequency and band
lines, the S-meter, the TX power/SWR box, the indicators, the widgets the
apps share (waterfall, bar, finder), the band plan and broadcast
overlay, the colours and fonts (`styles.c`).
**Look hard at:** drawing outside a buffer at the edges (zoom, narrow
spans, the band edges); sizes that change while drawing; the two screens
on two DRM planes (anything over the spectrum must be its child).

```files
src/spectrum.c
src/spectrum.h
src/waterfall.c
src/waterfall.h
src/freq_info.cpp
src/freq_info.h
src/band_info.cpp
src/band_info.h
src/band_bar.c
src/band_bar.h
src/meter.c
src/meter.h
src/tx_info.c
src/tx_info.h
src/indicators.cpp
src/indicators.h
src/widgets/
src/band_plan.c
src/band_plan.h
src/broadcast_overlay.c
src/broadcast_overlay.h
src/broadcast_db.c
src/broadcast_db.h
src/styles.c
src/styles.h
src/palettes/
```

### A10 · Bluetooth
Pairing and devices, Bluetooth audio (headsets, phones), the serial port
profile (CAT over Bluetooth), the Bluetooth window.
**Look hard at:** D-Bus replies arriving after the window closed; the
pairing race; a headset's PTT keying the right path (`radio_set_modem()`
for codec audio); threads.

```files
src/bt_dev.c
src/bt_dev.h
src/bt_audio.c
src/bt_audio.h
src/bt_spp.c
src/bt_spp.h
src/bt_ctl.c
src/bt_ctl.h
src/dialog_bt.c
src/dialog_bt.h
```

### A11 · WiFi and the channel list
WiFi (NetworkManager) and its window; the Channels list (stored
frequencies, imports, the broadcast and other lists).
**Look hard at:** passwords and SSIDs with odd characters; JSON and file
parsing in `channels.c`; list sizes.

```files
src/wifi.cpp
src/wifi.h
src/dialog_wifi.c
src/dialog_wifi.h
src/channels.c
src/channels.h
src/dialog_channels.c
src/dialog_channels.h
```

### A12 · CW, voice and CW messages, recorder, SWR scan
The CW keyer and its messages, the CW decoder, CW tuning, voice
messages, the audio recorder window, the SWR scan.
**Look hard at:** a message that keeps transmitting after its window
closes or the band changes; the SWR scan's transmit always ending.

```files
src/cw.cpp
src/cw.h
src/cw_encoder.c
src/cw_encoder.h
src/cw_tune_ui.c
src/cw_tune_ui.h
src/cw_decoder/
src/dialog_msg_cw.c
src/dialog_msg_cw.h
src/msg_cw_store.c
src/msg_cw_store.h
src/dialog_msg_voice.c
src/dialog_msg_voice.h
src/dialog_recorder.c
src/dialog_recorder.h
src/dialog_swrscan.c
src/dialog_swrscan.h
```

### A13 · Logging and the digital apps' shared parts
The ADIF log and QSO logs, DXCC lookup; the parts PSK31, Olivia and RTTY
(and others) share: menus, QSO panel, typed text, transmit queue, "next
station", the apps' waterfall, the Info window, the decode panel.
**Look hard at:** log files written without `.tmp` + rename (a power cut
mid-write); ADIF fields with odd text; `digi_menu.c` lists freed while
the keypad still points at them (crashed with VOL pressed, 2026-10-17).

```files
src/adif.c
src/adif.h
src/adif_rec.c
src/adif_rec.h
src/qso_log.c
src/qso_log.h
src/digi_log.c
src/digi_log.h
src/jtty_log.c
src/jtty_log.h
src/dxcc.c
src/dxcc.h
src/digi_menu.c
src/digi_menu.h
src/digi_qso.c
src/digi_qso.h
src/digi_qso_menu.c
src/digi_qso_menu.h
src/digi_text.c
src/digi_text.h
src/digi_txq.c
src/digi_txq.h
src/digi_next.c
src/digi_next.h
src/app_wf.c
src/app_wf.h
src/app_info.c
src/app_info.h
src/panel.cpp
src/panel.h
```

## The apps

### A14 · FT8/FT4/FT2: decoding and transmitting
The decoder threads, audio feed, transmit timing, QSO logic, call table,
contest messages, CQ scheduling, WSJT-X UDP.
**Look hard at:** slot timing (a TX starting 0.5-1.5 s late was seen on
the radio, 2026-11-12); a message text that crashes the parser (`A B +`
did, #383); threads sharing the decode list.

```files
src/ft8/
src/ft8_udp.c
src/ft8_udp.h
```

### A15 · FT8 window, maps, PSK Reporter
The FT8/FT4/FT2 window, its list and map, the shared map widget and
scene, PSK Reporter uploads.
**Look hard at:** list rows and map stations past their limits; PSK
Reporter's network thread and the GUI.

```files
src/dialog_ft8.c
src/dialog_ft8.h
src/ft8_ui/
src/map_scene.c
src/map_scene.h
src/map_widget.c
src/map_widget.h
src/psk_reporter.c
src/psk_reporter.h
```

### A16 · WSPR
WSPR window, receive (with WSJT-X's wsprd parts), transmit, WSPRnet
upload, its map.
**Look hard at:** the 2-minute slot and band changes in the middle; a
TX that never ends; the upload thread.

```files
src/dialog_wspr.c
src/dialog_wspr.h
src/wspr_app.c
src/wspr_app.h
src/wspr_worker.c
src/wspr_worker.h
src/wspr_demod.c
src/wspr_demod.h
src/wspr_fano.c
src/wspr_fano.h
src/wspr_spec.c
src/wspr_spec.h
src/wspr_encode.c
src/wspr_encode.h
src/wspr_mod.c
src/wspr_mod.h
src/wspr_net.c
src/wspr_net.h
src/wspr_map.c
src/wspr_map.h
src/wsprd/
```

### A17 · PSK31

```files
src/dialog_psk.c
src/dialog_psk.h
src/psk_rx.c
src/psk_rx.h
src/psk_tx.c
src/psk_tx.h
src/psk_spec.c
src/psk_spec.h
src/psk_macro.c
src/psk_macro.h
```

### A18 · Olivia
**Look hard at:** macros or rows that key the radio by themselves (found
once, #340).

```files
src/dialog_olivia.c
src/dialog_olivia.h
src/olivia.c
src/olivia.h
src/olivia_app.c
src/olivia_app.h
src/olivia_rx.c
src/olivia_rx.h
src/olivia_tx.c
src/olivia_tx.h
src/olivia_mod.c
src/olivia_mod.h
src/olivia_demod.c
src/olivia_demod.h
src/olivia_resamp.c
src/olivia_resamp.h
src/olivia_macro.c
src/olivia_macro.h
```

### A19 · RTTY
RTTY receive and transmit, its buttons and squelch. (The window is in
`main_screen.c`, area A08.)

```files
src/rtty.c
src/rtty.h
src/rtty_tx.c
src/rtty_tx.h
src/rtty_buttons.c
src/rtty_buttons.h
src/rtty_sql.c
src/rtty_sql.h
```

### A20 · JTTY

```files
src/dialog_jtty.c
src/dialog_jtty.h
src/jtty/
src/jtty_tx.c
src/jtty_tx.h
```

### A21 · SSTV window
The SSTV window: receive view, transmit (pictures, templates, drawing),
the picture browser. Its `.inc` files are part of `dialog_sstv.c`.

```files
src/dialog_sstv.c
src/dialog_sstv.h
src/dialog_sstv_rx.inc
src/dialog_sstv_tx.inc
src/dialog_sstv_tx_draw.inc
src/dialog_sstv_browser.inc
```

### A22 · SSTV decoding and encoding
VIS, sync, line decoding, denoise, the encoder, the built-in pictures.

```files
src/sstv/
src/sstv_resources.c
src/sstv_resources.h
```

### A23 · WeFax window
The WeFax window and station list (the decoder is A24).

```files
src/dialog_wefax.c
src/dialog_wefax.h
src/dialog_wefax_view.inc
src/dialog_wefax_freq.inc
src/dialog_wefax_slant.inc
src/dialog_wefax_pp.inc
src/wefax/stations.c
src/wefax/stations.h
```

### A24 · WeFax decoder and post-processing

```files
src/wefax/decoder.c
src/wefax/decoder.h
src/wefax/wefax_pp.c
src/wefax/wefax_pp.h
```

### A25 · NavTex

```files
src/dialog_navtex.c
src/dialog_navtex.h
src/dialog_navtex_bul.inc
src/dialog_navtex_freq.inc
src/dialog_navtex_scan.inc
src/navtex/
```

### A26 · DSC

```files
src/dialog_dsc.c
src/dialog_dsc.h
src/dsc/
```

### A27 · JS8 window, part 1: set-up, receive list, transmit
`dialog_js8.c` is too big for one area, so it is three parts, split at
two definitions (`compose_count_timer`, `user_touch`): the parts move
with the code. Read the other two parts as far as
you need.
**Look hard at:** nothing may transmit by itself when JS8 opens; the
filters, power cap and DSP settings JS8 changes are put back on every
way out.

```files
src/dialog_js8.c:start..compose_count_timer
src/dialog_js8.h
src/js8_wf.c
src/js8_wf.h
src/js8_clock.c
src/js8_clock.h
```

### A28 · JS8 window, part 2: compose, bands, pop-ups

```files
src/dialog_js8.c:compose_count_timer..user_touch
```

### A29 · JS8 window, part 3: auto replies, heartbeat, saved texts, settings
**Look hard at:** auto replies and heartbeats: when they may transmit,
and that turning them off stops them at once.

```files
src/dialog_js8.c:user_touch..end
```

### A30 · JS8 library: receive, decode threads, transmit, timing
The C++ side of JS8 under the window: the receiver and decode threads,
frame assembly, transmit, time sync, FFTW wisdom. The engine itself
(`third-party/js8core`) is not hunted here.
**Look hard at:** frames must be bit-identical to desktop JS8Call's;
threads; buffers.

```files
src/js8/receiver.cpp
src/js8/receiver.hpp
src/js8/js8_rx.cpp
src/js8/js8_rx.h
src/js8/js8_tx.cpp
src/js8/js8_tx.h
src/js8/tx.cpp
src/js8/tx.hpp
src/js8/assembler.cpp
src/js8/assembler.hpp
src/js8/timesync.cpp
src/js8/timesync.hpp
src/js8/wisdom.cpp
src/js8/wisdom.hpp
src/js8/js8_wisdom.h
src/js8/resampler.cpp
src/js8/resampler.hpp
src/js8/speeds.cpp
src/js8/speeds.hpp
src/js8/js8_speed.h
src/js8/cpu_fence.cpp
src/js8/cpu_fence.hpp
src/js8/wav.cpp
src/js8/wav.hpp
src/js8/testsignal.cpp
src/js8/testsignal.hpp
```

### A31 · JS8 library: messages, inbox, history, map
Directed messages and commands, auto replies, the inbox, history
database, QSO log, stations, the map and its drawing, alerts, macros.
**Look hard at:** text from the air is untrusted (lengths, odd
characters); files written via `.tmp` + rename.

```files
src/js8/js8_ops.cpp
src/js8/js8_ops.h
src/js8/directed.cpp
src/js8/directed.hpp
src/js8/commands.cpp
src/js8/commands.hpp
src/js8/classify.cpp
src/js8/classify.hpp
src/js8/autoreply.cpp
src/js8/autoreply.hpp
src/js8/inbox.cpp
src/js8/inbox.hpp
src/js8/history.cpp
src/js8/history.hpp
src/js8/js8_history.h
src/js8/qsolog.cpp
src/js8/qsolog.hpp
src/js8/stations.cpp
src/js8/stations.hpp
src/js8/callsign_place.cpp
src/js8/callsign_place.hpp
src/js8/geo.cpp
src/js8/geo.hpp
src/js8/js8_map.cpp
src/js8/js8_map.h
src/js8/map_render.cpp
src/js8/map_render.hpp
src/js8/render.cpp
src/js8/render.hpp
src/js8/alerts.cpp
src/js8/alerts.hpp
src/js8/macros.cpp
src/js8/macros.hpp
src/js8/datafile.cpp
src/js8/datafile.hpp
src/js8/strutil.hpp
```

## Not hunted

Code by others that we only use (bugs there go to its authors), the
fonts (generated), and files that are not built.

```skip
src/fonts/
src/cJSON.c
src/cJSON.h
src/goertzel.c
src/goertzel.h
src/mouse.c
src/mouse.h
src/mouse_cursor_icon.c
```
