/*
 * olivia_rx.c - Olivia receiver: synchronisation + decoding
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 * The core of the synchronisation:
 *
 * There are 256 block phases times (2*margin+1) frequency offsets. Trying
 * them all on every slice would be unaffordable. But a block lasts
 * exactly 256 slices, so each slice COMPLETES exactly one block phase -
 * the one numbered (slice number mod 256). So per slice there are only
 * as many evaluations as there are frequency offsets.
 *
 * For each hypothesis an average FHT figure is kept (peak divided by the
 * RMS of the other 63). With the right phase and offset it stands out,
 * with a wrong one it hovers around 2.8. Lock happens when the best
 * hypothesis passes the threshold after at least a few blocks of
 * averaging.
 *
 * About drift: the clock is NOT free-running with incremental
 * correction. The block phase is picked from the grid and held; if the
 * signal drifts away, the figure drops and the synchronisation is worked
 * out again from scratch. A deliberately different architecture from an
 * accumulating correction.
 */

#include "olivia_rx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A reversed-order hypothesis loses to the normal one at the same phase
 * and offset unless it scores clearly better (see the lock decision). */
#define OLIVIA_REV_TIE 0.8f

/* ------------------------------------------------------------------ */

int olivia_rx_init(olivia_rx_t *rx, int tones, int bandwidth,
                   int sample_rate, double lower_edge, int margin)
{
    size_t hist_len, score_len;

    memset(rx, 0, sizeof(*rx));

    if (olivia_demod_init(&rx->demod, tones, bandwidth, sample_rate,
                          lower_edge, margin) < 0)
        return -1;

    rx->cfg = rx->demod.cfg;
    rx->bits = rx->cfg.bits;
    rx->margin = margin;
    /* Hypotheses every bin, i.e. every quarter of the tone spacing. */
    rx->offsets = rx->demod.offsets;
    rx->margin_bins = margin * rx->demod.carrier_separ;
    rx->offset_step = rx->demod.offset_step;
    rx->variants = rx->offsets * 2;

    hist_len = (size_t)OLIVIA_BLOCK_SLICES * rx->variants * rx->bits;
    score_len = (size_t)OLIVIA_BLOCK_SLICES * rx->variants;

    rx->hist = calloc(hist_len, sizeof(float));
    rx->score = calloc(score_len, sizeof(float));
    rx->count = calloc(score_len, sizeof(uint16_t));
    rx->energy = calloc(rx->demod.width, sizeof(float));
    rx->pend = calloc(rx->demod.slice_separ, sizeof(float));

    if (!rx->hist || !rx->score || !rx->count || !rx->energy || !rx->pend) {
        olivia_rx_free(rx);
        return -1;
    }

    rx->lock_offset = 0;
    return 0;
}

int olivia_rx_reversed(const olivia_rx_t *rx)
{
    return rx->lock_rev;
}

void olivia_rx_free(olivia_rx_t *rx)
{
    free(rx->hist);
    free(rx->score);
    free(rx->count);
    free(rx->energy);
    free(rx->pend);
    rx->hist = NULL;
    rx->score = NULL;
    rx->count = NULL;
    rx->energy = NULL;
    rx->pend = NULL;
    olivia_demod_free(&rx->demod);
}

void olivia_rx_set_callback(olivia_rx_t *rx, olivia_rx_text_cb cb, void *user)
{
    rx->cb = cb;
    rx->cb_user = user;
}

void olivia_rx_reset(olivia_rx_t *rx)
{
    size_t score_len = (size_t)OLIVIA_BLOCK_SLICES * rx->variants;

    memset(rx->score, 0, score_len * sizeof(float));
    memset(rx->count, 0, score_len * sizeof(uint16_t));
    rx->locked = 0;
    rx->sweeps = 0;
    rx->lock_score = 0.0f;
    rx->bad_blocks = 0;
    rx->slices_seen = 0;
    rx->pend_n = 0;
    rx->lock_rev = 0;
    rx->lock_variant = 0;
}

int olivia_rx_locked(const olivia_rx_t *rx)
{
    return rx->locked;
}

float olivia_rx_score(const olivia_rx_t *rx)
{
    return rx->lock_score;
}

float olivia_rx_best_hypothesis(const olivia_rx_t *rx, int *offset_bins)
{
    float best = 0.0f;
    int best_off = 0;
    int p, o;

    if (!rx->score) {
        if (offset_bins) *offset_bins = 0;
        return 0.0f;
    }

    for (p = 0; p < OLIVIA_BLOCK_SLICES; p++) {
        for (o = 0; o < rx->variants; o++) {
            size_t i = (size_t)p * rx->variants + o;

            if (rx->count[i] > 0 && rx->score[i] > best) {
                best = rx->score[i];
                best_off = (o % rx->offsets) * rx->offset_step -
                           rx->margin_bins;
            }
        }
    }

    if (offset_bins)
        *offset_bins = best_off;
    return best;
}

float olivia_rx_best_score(const olivia_rx_t *rx)
{
    return olivia_rx_best_hypothesis(rx, NULL);
}

double olivia_rx_freq_error(const olivia_rx_t *rx)
{
    /* lock_offset is in bins, not in tones. */
    return olivia_demod_offset_hz(&rx->demod, rx->lock_offset);
}

/* ------------------------------------------------------------------ */

/*
 * Assemble the block ending at the current slice for the given offset
 * and decode it. Returns the average FHT figure.
 * If chars != NULL, the decoded characters are written there.
 */
static float eval_block(olivia_rx_t *rx, int offset_idx, uint8_t *chars)
{
    float ordered[OLIVIA_BLOCK_SYMBOLS * OLIVIA_MAX_BITS];
    float snr[OLIVIA_MAX_BITS];
    uint8_t local[OLIVIA_MAX_BITS];
    int bits = rx->bits;
    int t, b;
    float sum = 0.0f;

    for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++) {
        /*
         * Symbol t of the block lies (63 - t) symbols before the
         * current slice, i.e. 4*(63-t) slices back.
         */
        int back = OLIVIA_SLICES_PER_SYMBOL *
                   (OLIVIA_BLOCK_SYMBOLS - 1 - t);
        int slot = rx->slice_pos - back;

        while (slot < 0)
            slot += OLIVIA_BLOCK_SLICES;

        for (b = 0; b < bits; b++)
            ordered[t * bits + b] =
                rx->hist[((size_t)slot * rx->variants + offset_idx) * bits + b];
    }

    olivia_decode_block_soft(&rx->cfg, ordered, local, snr);

    for (b = 0; b < bits; b++)
        sum += snr[b];

    if (chars)
        memcpy(chars, local, bits);

    return sum / bits;
}

/* Process one slice: update the history, evaluate the hypotheses. */
static void process_slice(olivia_rx_t *rx, const float *in)
{
    int offset_idx;
    int phase;
    float *row;

    olivia_demod_slice(&rx->demod, in, rx->energy);

    /* Soft decisions for every frequency offset under consideration. */
    for (offset_idx = 0; offset_idx < rx->variants; offset_idx++) {
        int off = (offset_idx % rx->offsets) * rx->offset_step -
                  rx->margin_bins;
        int rev = offset_idx >= rx->offsets;

        row = rx->hist +
              ((size_t)rx->slice_pos * rx->variants + offset_idx) * rx->bits;
        olivia_demod_soft(&rx->demod, rx->energy, off, rev, row);
    }

    rx->slices_seen++;

    /* Until a whole block of history is in, there is nothing to judge. */
    if (rx->slices_seen < OLIVIA_BLOCK_SLICES) {
        rx->slice_pos = (rx->slice_pos + 1) % OLIVIA_BLOCK_SLICES;
        return;
    }

    phase = rx->slice_pos;

    if (rx->locked && phase == rx->lock_phase) {
        /* Locked, and our block has just completed. */
        uint8_t chars[OLIVIA_MAX_BITS];
        float m = eval_block(rx, rx->lock_variant, chars);

        rx->lock_score = m;

        if (m < OLIVIA_SYNC_THRESHOLD) {
            rx->bad_blocks++;
            if (rx->bad_blocks >= 3) {
                /* The signal is gone - back to searching. */
                olivia_rx_reset(rx);
                rx->slice_pos = (rx->slice_pos + 1) % OLIVIA_BLOCK_SLICES;
                return;
            }
        } else {
            rx->bad_blocks = 0;
            if (rx->cb)
                rx->cb(rx->cb_user, chars, rx->bits);
        }
    } else if (!rx->locked) {
        /*
         * Searching. Only the phase that has just completed is evaluated,
         * for all offsets - the trick that keeps the cost per slice
         * constant.
         */
        float best = -1.0f;
        int best_off = 0;
        int ready = 1;

        for (offset_idx = 0; offset_idx < rx->variants; offset_idx++) {
            size_t idx = (size_t)phase * rx->variants + offset_idx;
            float m = eval_block(rx, offset_idx, NULL);
            float avg;

            if (rx->count[idx] < OLIVIA_SYNC_INTEG) {
                /* running mean until the window has filled */
                rx->score[idx] =
                    (rx->score[idx] * rx->count[idx] + m) /
                    (rx->count[idx] + 1);
                rx->count[idx]++;
            } else {
                /* then exponential, to follow changes */
                rx->score[idx] += (m - rx->score[idx]) / OLIVIA_SYNC_INTEG;
            }

            avg = rx->score[idx];
            if (avg > best) {
                best = avg;
                best_off = offset_idx;
            }
        }

        /*
         * Lock only once EVERY phase has been evaluated at least twice.
         * Otherwise the one that completed first would win - phases
         * complete in turn, so comparing with those judged so far is
         * premature. This did happen: the right phase scored 18051
         * while the receiver locked onto another one scoring 7.
         */
        ready = (rx->sweeps >= 2);

        if (ready && best > OLIVIA_SYNC_THRESHOLD) {
            int p, o;
            float global_best = best;
            int global_phase = phase;

            for (p = 0; p < OLIVIA_BLOCK_SLICES; p++) {
                for (o = 0; o < rx->variants; o++) {
                    size_t idx = (size_t)p * rx->variants + o;

                    if (rx->score[idx] > global_best) {
                        global_best = rx->score[idx];
                        global_phase = p;
                    }
                }
            }

            if (global_phase == phase) {
                /*
                 * With an even number of bits per symbol (16, 64, 256
                 * tones) the reversed tone order is NOT distinguishable
                 * by the score. Reversing N tones complements the tone
                 * number, which after the Gray code XORs every symbol
                 * with a constant that flips every other bit plane; with
                 * the diagonal interleave that multiplies each character
                 * vector by an alternating +-1 sequence, which is itself
                 * a Walsh row - so the reversed hypothesis decodes into
                 * other, equally "valid" characters at full strength.
                 * Measured at 16/500: 12 of 40 noisy runs locked reversed
                 * and printed rubbish. Normal order is the standard, so
                 * it wins whenever it scores nearly as well.
                 */
                if (best_off >= rx->offsets) {
                    int    normal = best_off - rx->offsets;
                    size_t ni = (size_t)phase * rx->variants + normal;

                    if (rx->score[ni] >= OLIVIA_REV_TIE * best) {
                        best_off = normal;
                        best = rx->score[ni];
                    }
                }

                rx->locked = 1;
                rx->lock_phase = phase;
                rx->lock_variant = best_off;
                rx->lock_rev = best_off >= rx->offsets;
                rx->lock_offset = (best_off % rx->offsets) * rx->offset_step -
                                  rx->margin_bins;
                rx->lock_score = best;
                rx->bad_blocks = 0;
            }
        }
    }

    rx->slice_pos = (rx->slice_pos + 1) % OLIVIA_BLOCK_SLICES;
    if (rx->slice_pos == 0)
        rx->sweeps++;
}

int olivia_rx_process(olivia_rx_t *rx, const float *samples, int count)
{
    int sep = rx->demod.slice_separ;
    int done = 0;
    int pos = 0;

    /* First finish the slice started in the previous call. */
    if (rx->pend_n > 0) {
        int need = sep - rx->pend_n;

        if (count < need) {
            memcpy(rx->pend + rx->pend_n, samples, sizeof(float) * count);
            rx->pend_n += count;
            return 0;
        }
        memcpy(rx->pend + rx->pend_n, samples, sizeof(float) * need);
        process_slice(rx, rx->pend);
        rx->pend_n = 0;
        pos = need;
        done++;
    }

    while (pos + sep <= count) {
        process_slice(rx, samples + pos);
        pos += sep;
        done++;
    }

    /* The remainder waits for the next call. */
    if (pos < count) {
        rx->pend_n = count - pos;
        memcpy(rx->pend, samples + pos, sizeof(float) * rx->pend_n);
    }

    return done;
}
