/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 NAVTEX decoder: the SITOR-B receiver of sitorb.c behind the
 * interface of decoder.h (12000 Hz audio, tones around 1000 Hz).
 */
#include "decoder.h"
#include "sitorb.h"

#include <stdlib.h>

#define NAVTEX_RATE     12000.0f
#define NAVTEX_CENTRE   1000.0f

struct navtex_decoder {
    sitorb_t *rx;
    navtex_char_cb_t char_cb;
    void *char_user;
    navtex_char_q_cb_t q_cb;
    void *q_user;
};

static void on_char(int c, int quality, void *user)
{
    navtex_decoder_t *d = user;
    if (quality != NAVTEX_Q_EVENT && d->char_cb)
        d->char_cb(c, d->char_user);
    if (d->q_cb)
        d->q_cb(c, quality, d->q_user);
}

navtex_decoder_t *navtex_decoder_create(int reverse)
{
    navtex_decoder_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->rx = sitorb_create(NAVTEX_RATE, NAVTEX_CENTRE, reverse);
    if (!d->rx) {
        free(d);
        return NULL;
    }
    sitorb_set_callback(d->rx, on_char, d);
    return d;
}

void navtex_decoder_destroy(navtex_decoder_t *d)
{
    if (!d)
        return;
    sitorb_destroy(d->rx);
    free(d);
}

void navtex_decoder_set_char_callback(navtex_decoder_t *d, navtex_char_cb_t cb, void *userdata)
{
    if (!d)
        return;
    d->char_cb = cb;
    d->char_user = userdata;
}

void navtex_decoder_set_char_q_callback(navtex_decoder_t *d, navtex_char_q_cb_t cb, void *userdata)
{
    if (!d)
        return;
    d->q_cb = cb;
    d->q_user = userdata;
}

void navtex_decoder_process(navtex_decoder_t *d, const float *samples, unsigned int n)
{
    if (!d || !samples || n == 0)
        return;
    sitorb_process(d->rx, samples, n);
}
