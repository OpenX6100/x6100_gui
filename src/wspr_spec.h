/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  WSPR spectrum for the waterfall display
 *
 *  Separate from wspr_demod.c on purpose. That file carries the
 *  synchroniser and the soft decision path that were tuned by
 *  measurement and verified on air; a display feature has no business
 *  reaching into it. The cost is a second, much smaller FFT here.
 *
 *  Input is the 375 Hz complex baseband the worker already produces, so
 *  no extra filtering or decimation is needed. A 256 point transform on
 *  it gives 1.4648 Hz bins - exactly the WSPR tone spacing - across a
 *  375 Hz span, and one line per 256 samples means one line per symbol
 *  period, 0.683 s. A whole transmission is then 162 lines, which fills
 *  the display height at a natural rate.
 *
 *  Output is in dB relative to the median of the same line, so the
 *  noise floor sits near zero whatever the audio gain and RF gain
 *  happen to be. The alternative, absolute dBFS, would need the display
 *  range retuned every time the operator touched a knob.
 */

#ifndef WSPR_SPEC_H
#define WSPR_SPEC_H

#include <stdbool.h>

#include "wspr_demod.h"

/* Bins per line, and samples consumed per line. */
#define WSPR_SPEC_BINS WSPR_WORK_SYMBOL     /* 256 */

typedef struct {
    int    n;
    int   *rev;
    float *cos_t;
    float *sin_t;
    float *win;

    float  buf_re[WSPR_SPEC_BINS];
    float  buf_im[WSPR_SPEC_BINS];
    int    fill;
} wspr_spec_t;

/*
 * One completed line. mag_db holds nbins values ordered by ascending
 * frequency, the lowest bin being center_hz - WSPR_WORK_RATE/2.
 */
typedef void (*wspr_spec_cb)(const float *mag_db, int nbins, void *ctx);

int  wspr_spec_init(wspr_spec_t *s);
void wspr_spec_free(wspr_spec_t *s);
void wspr_spec_reset(wspr_spec_t *s);

/*
 * Feed complex baseband at WSPR_WORK_RATE. The callback fires once per
 * completed line, which may be zero, one or several times per call.
 */
void wspr_spec_process(wspr_spec_t *s, const float *re, const float *im,
                       long n, wspr_spec_cb cb, void *ctx);

#endif /* WSPR_SPEC_H */
