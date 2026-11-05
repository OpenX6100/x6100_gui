/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY receiver: frame search and decoding in a
 *  block of audio
 */

#pragma once

#include "jtty_proto.h"
#include "jtty_source.h"

typedef struct {
    int            start;       /* first sample of the frame in the block */
    float          freq;        /* Hz of tone 0 */
    float          snr;         /* dB in 2500 Hz */
    float          sync;        /* sync quality, 0..1 */
    float          quality;     /* energy on the decoded frame's tones, 0..1 */
    int            rank;        /* position in the FEC list, 0 = best path */
    jtty_payload_t payload;
    jtty_atom_t    atom;
} jtty_decode_t;

typedef struct {
    float fmin;                 /* tone 0 between fmin and fmax, Hz */
    float fmax;
    int   start_min;            /* frame start between these samples */
    int   start_max;
    int   max_candidates;       /* sync peaks tried, best first */
    float min_score;            /* sync peak threshold (noise = 0, sigma ~0.3) */
    int   list_size;            /* FEC list depth */
} jtty_search_t;

void jtty_search_defaults(jtty_search_t *s, int nsamples);

/*
 * Searches a block of 12 kHz audio for JTTY frames that lie completely
 * inside it and start in [start_min, start_max]. Writes up to `max_out`
 * valid decodes, one per frame. Returns how many.
 */
int jtty_demod_search(const float *x, int n, const jtty_search_t *s,
                      jtty_decode_t *out, int max_out);

/*
 * Tries one frame at a known place: the next frame of a message being
 * received (`start` = previous start + JTTY_FRAME_SAMPLES, same `freq`).
 * Time and frequency are still refined around it; the FEC list is the
 * deepest. Returns true with a valid decode.
 */
bool jtty_demod_try(const float *x, int n, int start, float freq, jtty_decode_t *d);

/*
 * True when `weak` is a ghost of `strong`: frames that overlap in time
 * (not the next frame of a message, which starts exactly one frame
 * later) on overlapping bands, `strong` fitting its signal better; or,
 * at least 10 dB weaker, within two signal widths of it, where a strong
 * signal's GFSK side lobes still stand above the noise. Starts in
 * samples on any common time base.
 */
bool jtty_demod_ghost(int64_t weak_start, float weak_freq, float weak_q, float weak_snr,
                      int64_t strong_start, float strong_freq, float strong_q, float strong_snr);

/*
 * True when two frames are copies of one transmission: the same payload
 * at the same moment (within two symbols) on another frequency, as made
 * by intermodulation with mains hum in a transmitter, or any spur. Two
 * stations do not send the same frame at the same instant.
 */
bool jtty_demod_copy(int64_t a_start, jtty_payload_t a_payload,
                     int64_t b_start, jtty_payload_t b_payload);
