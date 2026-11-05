/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY 4-GFSK waveform
 */

#pragma once

#include "jtty_proto.h"

/* Samples written by jtty_mod_wave() for `nsym` symbols: the symbols
 * plus a short raised-cosine ramp at each end. */
#define JTTY_MOD_RAMP   (JTTY_NSPS / 8)
int jtty_mod_length(int nsym);

/*
 * Continuous-phase 4-GFSK (BT 2.0, h = 1) for `nsym` tones, tone 0 at
 * `f0` Hz, `rate` samples per second (a multiple of 12000/384 Hz baud:
 * 12000, 24000, 48000). Writes jtty_mod_length(nsym) * rate / 12000
 * samples of amplitude 1. Returns the number written.
 */
int jtty_mod_wave(const uint8_t *tones, int nsym, float f0, int rate, float *out);
