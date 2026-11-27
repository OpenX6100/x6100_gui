/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4: more tries at a signal that didn't decode
 */

#pragma once

#include <ft8lib/decode.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* As JS8's decoder (js8core legacy_decoder.cpp, from JS8Call): when
 * ftx_decode_candidate() has failed, the same LDPC decoder again on other
 * views of the signal: the tones' amplitude instead of their dB, then with the
 * first 24 and the first 48 bits marked unknown (a fade or a burst at the
 * start). Each try has JS8's limit on how many bits the result may have
 * changed, and the CRC decides. False if none gave a message. */
bool ftx_decode_retry(const ftx_waterfall_t *wf, const ftx_candidate_t *cand, int max_iterations, ftx_message_t *message);

#ifdef __cplusplus
}
#endif
