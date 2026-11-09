/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4: take a decoded signal out of the audio
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* As WSJT-X's subtractft8: the decoded message's waveform is made again
 * (GFSK, as gfsk.c sends it), its start fitted within a symbol of the
 * decoder's estimate, its amplitude and phase followed through the message
 * by a low-pass over ~2 symbols, and that signal subtracted from the audio,
 * so what it hid can be decoded on the next pass.
 *
 * nsps: samples per symbol (FT8 1920, FT4 576 at 12 kHz), bt: the GFSK
 * bandwidth-time product (FT8_SYMBOL_BT, FT4_SYMBOL_BT). */
bool ftx_subtract_init(int nsps, int n_tones, float bt);
void ftx_subtract_free(void);

/* audio[0..n) at rate; tones: the message's n_tones channel symbols;
 * freq_hz: its lowest tone; start: the decoder's estimate of its first
 * sample. */
void ftx_subtract(float *audio, int n, int rate, const uint8_t *tones, float freq_hz, int start);

#ifdef __cplusplus
}
#endif
