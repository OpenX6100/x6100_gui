/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - sent and received text of the keyboard modes
 *  saved to the SD card
 *
 *  While a PSK31, Olivia or RTTY window is open, what it receives and
 *  what it sends is appended to a plain text file on the card, one file
 *  per mode (/mnt/psk31_text.txt, /mnt/olivia_text.txt,
 *  /mnt/rtty_text.txt), so a QSO can be read again later or copied off.
 *  Each session, and each change of frequency, starts with a line
 *
 *      --- 2026-11-06 14:03:12 UTC  PSK31  14.071 MHz ---
 *
 *  received text follows as it came, sent text on lines of its own
 *  starting "TX: ". A file over 4 MB is renamed to <file>.1 when the
 *  window opens, so the card never fills up with it.
 *
 *  On by default; Menu > QSO switches it (p_digi_text_save, all modes).
 *  Text is buffered and written every two seconds by a thread of its
 *  own, so the card's fsync() never holds up the screen; the rx/tx calls
 *  only copy into the buffer and are safe from any thread.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The radio frequency of the signal, for the header lines. */
typedef uint64_t (*digi_text_freq_fn)(void);

/* UI thread: start saving for `mode` ("PSK31") into `path`. Does
 * nothing when saving is switched off. */
void        digi_text_open(const char *mode, const char *path, digi_text_freq_fn freq);

/* UI thread: write out what is buffered and stop. Safe to call when not
 * open. */
void        digi_text_close(void);

/* Any thread. */
void        digi_text_rx(const char *text);
void        digi_text_rx_char(char c);
void        digi_text_tx(const char *text);

/* UI thread, after a band or dial change: the next text gets a new
 * header with the new frequency. */
void        digi_text_new_header(void);

/* Settings, shared by the three modes. Switching it on or off takes
 * effect at once for the open window. */
bool        digi_text_enabled(void);
void        digi_text_set_enabled(bool on);

/* The open window's file, "" when not saving. */
const char *digi_text_path(void);

#ifdef __cplusplus
}
#endif
