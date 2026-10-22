/*
 * olivia_rx.h - Olivia receiver: synchronisation + decoding
 *
 * Written from scratch from the public Olivia specification.
 * The SP9VRC reference code is GPL-2 and was NOT copied.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 * Part of OpenX6100 (github.com/OpenX6100)
 *
 * The receiver finds the block boundary and the frequency offset by
 * itself. It needs no preamble, since Olivia has none - synchronisation
 * rests on the Hadamard transform giving a sharp peak for the right
 * hypothesis and none for a wrong one.
 */

#ifndef OLIVIA_RX_H
#define OLIVIA_RX_H

#include <stdint.h>
#include <stddef.h>

#include "olivia.h"
#include "olivia_demod.h"

/* Slices per block - the number of block phases to try. */
#define OLIVIA_BLOCK_SLICES \
    (OLIVIA_BLOCK_SYMBOLS * OLIVIA_SLICES_PER_SYMBOL)

/* FHT figure above which the synchronisation counts as certain.
 * Below 3.0 decoding is unreliable, 3.5-4 is the edge of usefulness. */
#define OLIVIA_SYNC_THRESHOLD 3.5f

/* Blocks of averaging after which a hypothesis is trustworthy. */
#define OLIVIA_SYNC_INTEG 4

typedef void (*olivia_rx_text_cb)(void *user, const uint8_t *chars, int count);

typedef struct {
    olivia_demod_t demod;
    olivia_cfg_t cfg;

    int margin;        /* tuning margin in tones              */
    int offsets;       /* number of frequency hypotheses      */
    int variants;      /* offsets * 2 (normal and reversed)   */
    int margin_bins;   /* margin in FFT bins                  */
    int offset_step;   /* hypothesis step in bins             */
    int bits;

    /* Soft decision history: [slice][offset][bit].
     * Exactly one block back is kept. */
    float *hist;
    int slice_pos;     /* current position in the history, 0..255 */

    /* Average figures for each hypothesis: [phase][offset]. */
    float *score;
    uint16_t *count;

    /* Energy buffer for one slice. */
    float *energy;

    /* Leftover samples short of a full slice. Without this, feeding
     * arbitrary amounts would lose samples and break continuity. */
    float *pend;
    int pend_n;

    int64_t slices_seen;
    int sweeps;        /* full passes over all phases */

    int locked;
    int lock_phase;    /* block phase 0..255                  */
    int lock_offset;   /* offset in FFT BINS                  */
    int lock_rev;      /* locked on the reversed tone order   */
    int lock_variant;  /* variant index after lock            */
    float lock_score;

    /* Blocks in a row below the threshold before the lock is dropped. */
    int bad_blocks;

    olivia_rx_text_cb cb;
    void *cb_user;
} olivia_rx_t;

/*
 * margin: how many tones to search either way. 4 is sensible - at
 * 32/1000 it gives +-125 Hz of tuning tolerance.
 */
int olivia_rx_init(olivia_rx_t *rx, int tones, int bandwidth,
                   int sample_rate, double lower_edge, int margin);

void olivia_rx_free(olivia_rx_t *rx);

/* Set the function called for every batch of decoded characters. */
void olivia_rx_set_callback(olivia_rx_t *rx, olivia_rx_text_cb cb, void *user);

/*
 * Feed audio samples. Any number of samples, not necessarily a multiple
 * of a slice - the remainder is kept for the next call. Returns the
 * number of slices processed.
 */
int olivia_rx_process(olivia_rx_t *rx, const float *samples, int count);

/*
 * Whether the lock is on the reversed tone order. The receiver checks
 * both orientations itself - the operator cannot tell, the spectrum looks
 * identical.
 */
int olivia_rx_reversed(const olivia_rx_t *rx);

/* Drop the synchronisation - call after a frequency change. */
void olivia_rx_reset(olivia_rx_t *rx);

/* Whether the receiver is synchronised. */
int olivia_rx_locked(const olivia_rx_t *rx);

/* Quality figure of the current synchronisation. Zero until locked. */
float olivia_rx_score(const olivia_rx_t *rx);

/*
 * Best figure among ALL hypotheses considered, before lock too. This is
 * the number that says whether there is any Olivia structure on the air
 * at all: below ~3 nothing, above 4 something. Without it a "searching"
 * state tells nothing.
 */
float olivia_rx_best_score(const olivia_rx_t *rx);

/* As above, also returning the offset of the best hypothesis in bins. */
float olivia_rx_best_hypothesis(const olivia_rx_t *rx, int *offset_bins);

/* Tuning error in Hz. Positive = the sender is higher. */
double olivia_rx_freq_error(const olivia_rx_t *rx);

#endif /* OLIVIA_RX_H */
