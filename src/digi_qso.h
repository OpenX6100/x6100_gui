/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - QSO state for the keyboard digital modes
 *
 *  The correspondent's callsign and the RST reports that macro tokens
 *  (<CALL>, <RST>, <RSTR>) expand to. Kept outside any one mode, so a
 *  callsign picked up in one window is still there in another.
 *
 *  The callsign can be typed in or picked up automatically from the
 *  received text ("... DE <call>").
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *digi_qso_dx(void);
void        digi_qso_set_dx(const char *call);

/* Forget the callsign and put both reports back to 599. */
void        digi_qso_clear(void);

/* RST: readability and tone are fixed at 5 and 9, as usual on the
 * keyboard modes; only the strength digit changes. df > 0 steps up,
 * df < 0 down (wrapping 9 <-> 1), df == 0 only reads. Returns the
 * strength digit. */
uint8_t     digi_qso_change_rst_sent(int16_t df);
uint8_t     digi_qso_change_rst_rcvd(int16_t df);
const char *digi_qso_rst_sent(void);
const char *digi_qso_rst_rcvd(void);

/* Fed by a decoder, one character at a time: watches for "DE <call>"
 * in the incoming text and takes the callsign that follows, when auto
 * pick-up is on. Safe to call from the audio thread. */
void        digi_qso_feed_rx(char c);
bool        digi_qso_autograb(void);
bool        digi_qso_toggle_autograb(void);

#ifdef __cplusplus
}
#endif
