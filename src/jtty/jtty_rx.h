/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY streaming receiver
 *
 *  Audio goes in as it comes (12 kHz); every half second the newest
 *  frames are searched and decoded, and frames are joined into messages:
 *  one per transmission, growing frame by frame until its end-of-message
 *  frame, or until two frames in a row are missing.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "jtty_source.h"

typedef struct {
    uint32_t id;                        /* unique per message */
    float    freq;                      /* Hz of tone 0 */
    float    snr;                       /* dB, of the newest frame */
    uint64_t start;                     /* sample of its first frame */
    int      frames;
    bool     gap;                       /* a frame in it was not decoded */
    bool     done;                      /* EOM, or the sender stopped */
    char     text[JTTY_MSG_MAX + 1];
} jtty_msg_t;

/* Called from the receiver's thread on every change of a message. */
typedef void (*jtty_rx_cb_t)(const jtty_msg_t *msg, void *ctx);

typedef struct jtty_rx_s jtty_rx_t;

jtty_rx_t *jtty_rx_create(jtty_rx_cb_t cb, void *ctx);
void       jtty_rx_destroy(jtty_rx_t *rx);

/* Tone 0 searched between fmin and fmax Hz. */
void jtty_rx_set_band(jtty_rx_t *rx, float fmin, float fmax);

/*
 * Waterfall: about eight lines a second, each `nbins` values of dB above
 * that line's median, from f_lo to f_hi Hz. Called from the receiver's
 * thread; the array is only valid during the call.
 */
typedef void (*jtty_psd_cb_t)(const float *db, int nbins, void *ctx);
void jtty_rx_set_psd(jtty_rx_t *rx, jtty_psd_cb_t cb, void *ctx,
                     float f_lo, float f_hi, int nbins);

/* Forgets the audio and the open messages (new band, new dial). */
void jtty_rx_reset(jtty_rx_t *rx);

/* Any thread; never blocks for long. */
void jtty_rx_feed(jtty_rx_t *rx, const float *x, int n);

/* Worker thread: decodes in the background until stopped. */
int  jtty_rx_start(jtty_rx_t *rx);
void jtty_rx_stop(jtty_rx_t *rx);

/* Without the thread (tests): runs the passes due. Returns their count. */
int jtty_rx_poll(jtty_rx_t *rx);

/* Samples fed so far; with jtty_msg_t.start this dates a message. */
uint64_t jtty_rx_samples(jtty_rx_t *rx);
