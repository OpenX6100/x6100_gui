/*
 * olivia_resamp.h - rational resampler
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 * What it is for:
 *
 * Olivia is defined at 8000 Hz - at 32/1000 that gives exactly 256
 * samples per symbol and an FFT window of 1024 samples, a power of two.
 * The FFT bins must land on the tones exactly, so the modulator and the
 * demodulator work at 8000 Hz. The receiver gets 8000 Hz straight from
 * the DSP; the transmitter's output is converted here to the 48 kHz of
 * the audio player.
 *
 * Method: windowed-sinc interpolation with a table of phases.
 */

#ifndef OLIVIA_RESAMP_H
#define OLIVIA_RESAMP_H

#include <stddef.h>

#define OLIVIA_RESAMP_TAPS   24    /* kernel length per phase */

typedef struct {
    int in_rate;
    int out_rate;
    int phases;              /* L after reduction        */
    int step;                /* M after reduction        */

    float *kernel;           /* [phases][TAPS]           */
    float *hist;             /* circular input buffer    */
    int hist_len;
    int hist_pos;
    long long in_count;      /* samples pushed in        */

    long long out_pos;       /* output sample counter    */
} olivia_resamp_t;

/*
 * Returns 0 or -1. With in_rate == out_rate the module passes samples
 * straight through, which keeps a single code path.
 */
int olivia_resamp_init(olivia_resamp_t *r, int in_rate, int out_rate);

void olivia_resamp_free(olivia_resamp_t *r);

void olivia_resamp_reset(olivia_resamp_t *r);

/*
 * How many output samples n input samples can produce. One too many,
 * for sizing a buffer.
 */
int olivia_resamp_max_out(const olivia_resamp_t *r, int n);

/*
 * Process n input samples into out. Returns the number of samples
 * actually written.
 */
int olivia_resamp_process(olivia_resamp_t *r, const float *in, int n,
                          float *out);

#endif /* OLIVIA_RESAMP_H */
