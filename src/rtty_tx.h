/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - RTTY transmitter
 *
 *  Baudot/ITA2 AFSK transmitter. Synthesises a phase-continuous
 *  two-tone waveform using the shared RTTY parameters (rate, shift,
 *  center, reverse) and streams it through the modem TX path, the
 *  same way the FT8 tx_worker does (48 kHz audio player,
 *  radio_set_modem()).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the on-screen text window (USB/BT keyboard friendly).
 * OK starts the transmission of the entered text. */
void rtty_tx_open_input(void);

/* --- Macros ---------------------------------------------------------- */

#define RTTY_MACRO_MAX      16
#define RTTY_MACRO_NAME_LEN 24
#define RTTY_MACRO_TEXT_LEN 160

typedef struct {
    char name[RTTY_MACRO_NAME_LEN];
    char text[RTTY_MACRO_TEXT_LEN];
} rtty_macro_t;

/* Load macros from /mnt/rtty_macros.txt (seeds defaults on first run). */
void          rtty_macros_load(void);
int           rtty_macros_count(void);
rtty_macro_t *rtty_macro_get(int idx);

/* Persist current macros to the SD card. Returns true on success. */
bool          rtty_macros_save(void);

/* Add / edit / delete. add returns new index or -1 if full. */
int  rtty_macro_add(const char *name, const char *text);
void rtty_macro_set(int idx, const char *name, const char *text);
void rtty_macro_delete(int idx);

/* What macro idx would send now, its tokens expanded (for a preview). */
void rtty_macro_expand(int idx, char *out, size_t size);

/* Expand <CALL>/<RST> tokens and transmit macro idx. */
bool rtty_macro_send(int idx);

/* --- QSO state ------------------------------------------------------
 * Macros carry <CALL>, <RST> and <RSTR> tokens; the values they expand
 * to live in digi_qso.c (callsign typed in or picked up from the
 * received text, RST sent/received). */

/* Type the correspondent's callsign in. */
void        rtty_qso_open_dx_input(void);

/* UI entry points for the button pages. */
void rtty_macros_open_menu(void);
void rtty_macros_edit_slot(int idx);

/* Start transmitting a text asynchronously (worker thread).
 * Returns false if a transmission is already running. */
bool rtty_tx_send(const char *text);

/* Request the running transmission to stop after the current block. */
void rtty_tx_stop(void);

/* True while the worker thread is transmitting. */
bool rtty_tx_is_on(void);

#ifdef __cplusplus
}
#endif
