/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - Olivia MFSK transmitter
 *
 *  Olivia sends BLOCKS of log2(N) characters, not one character at a
 *  time - at 32 tones a block is 5 characters and lasts 2.05 s. The text
 *  is buffered and padded with zeros, as fldigi does.
 *
 *  The modulator runs at 8000 Hz, where the modulation is defined (256
 *  samples per symbol at 32/1000, a 1024 sample window). Its output goes
 *  through a resampler to the 48 kHz of the audio player.
 *
 *  The mode variant and the centre frequency live here and are shared
 *  with the receiver (olivia_app.c), so both always agree.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mode variants. The order is the order the Mode button steps through. */
typedef enum {
    OLIVIA_MODE_8_250 = 0,
    OLIVIA_MODE_16_500,
    OLIVIA_MODE_32_1000,
    OLIVIA_MODE_COUNT
} olivia_mode_t;

olivia_mode_t olivia_tx_get_mode(void);
void          olivia_tx_set_mode(olivia_mode_t mode);

/* Name of a variant for a button, e.g. "32/1000". */
const char *olivia_tx_mode_name(olivia_mode_t mode);

/* Number of tones and bandwidth of a variant. */
int olivia_tx_mode_tones(olivia_mode_t mode);
int olivia_tx_mode_bandwidth(olivia_mode_t mode);

/* Centre of the band in Hz (audio). */
uint16_t olivia_tx_get_center(void);
void     olivia_tx_set_center(uint16_t hz);

/* Transmit. While a transmission runs the text is added to its end
 * (digi_txq.h). False when the queue has no room for it. */
bool olivia_tx_send(const char *text);

/* Ask to stop: what is still queued is dropped, the current block is
 * finished, then one closing block. */
void olivia_tx_stop(void);

/* Stop for the window closing: the current block, then the fade-out, no
 * closing block (at 32/1000 each block is 2 s the screen would wait). */
void olivia_tx_stop_quick(void);

bool olivia_tx_is_on(void);

/* While the operator types during a transmission: send empty blocks
 * rather than end when the text runs out (digi_txq_hold). */
bool olivia_tx_hold(bool on);

/* The Olivia window shows what is sent in its own text area: when set,
 * olivia_tx_send() hands the text here. Called on the thread that calls
 * olivia_tx_send(), which is the UI thread. */
typedef void (*olivia_tx_echo_cb_t)(const char *text);
void olivia_tx_set_echo_cb(olivia_tx_echo_cb_t cb);

#ifdef __cplusplus
}
#endif
