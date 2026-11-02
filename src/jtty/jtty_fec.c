/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY soft-decision decoder for the tail-biting
 *  convolutional code
 *
 *  A tail-biting path starts and ends in the same state, which is not
 *  known. Two circular Viterbi passes over the frame (the second one
 *  starting from the metrics the first one ended with) rank the states
 *  by how likely they are to be that state. The best few are then each
 *  decoded exactly: a Viterbi pass that may only start in that state,
 *  read out where it ends in it. Every resulting path gets the CRC and
 *  the caller's check.
 */

#include "jtty_fec.h"

#include <string.h>

#define NEG_INF     (-1e30f)
#define MAX_LIST    32

/* tone of the transition into state n whose dropped bit was d */
static uint8_t tone_tab[JTTY_NSTATES][2];
static bool    tables_ready;

static void tables_init(void)
{
    if (tables_ready) {
        return;
    }
    for (unsigned n = 0; n < JTTY_NSTATES; n++) {
        tone_tab[n][0] = jtty_branch_tone(n, 0);
        tone_tab[n][1] = jtty_branch_tone(n, 1);
    }
    tables_ready = true;
}

/* One Viterbi pass over the 46 symbols. pm[] holds the start metrics on
 * entry and the end metrics on return; dec[t][n] the dropped bit of the
 * survivor into n at step t. */
static void viterbi_pass(const float metric[JTTY_NDATA][JTTY_TONES],
                         float pm[JTTY_NSTATES],
                         uint8_t dec[JTTY_NDATA][JTTY_NSTATES])
{
    float next[JTTY_NSTATES];

    for (int t = 0; t < JTTY_NDATA; t++) {
        const float *m = metric[t];

        for (unsigned n = 0; n < JTTY_NSTATES; n++) {
            unsigned p0 = n >> 1;                       /* dropped 0 */
            unsigned p1 = p0 | (1u << (JTTY_NU - 1));   /* dropped 1 */
            float    a = pm[p0] + m[tone_tab[n][0]];
            float    b = pm[p1] + m[tone_tab[n][1]];

            if (b > a) {
                next[n] = b;
                dec[t][n] = 1;
            } else {
                next[n] = a;
                dec[t][n] = 0;
            }
        }
        memcpy(pm, next, sizeof(next));
    }
}

/* Information bits of the survivor ending in `state`. */
static void traceback(uint8_t dec[JTTY_NDATA][JTTY_NSTATES], unsigned state,
                      uint8_t info[JTTY_INFO_BITS])
{
    for (int t = JTTY_NDATA - 1; t >= 0; t--) {
        info[t] = (uint8_t)(state & 1u);
        state = (state >> 1) | ((unsigned)dec[t][state] << (JTTY_NU - 1));
    }
}

bool jtty_fec_decode(const float metric[JTTY_NDATA][JTTY_TONES], int list_size,
                     jtty_accept_fn accept, void *ctx,
                     jtty_payload_t *payload, int *rank)
{
    uint8_t        dec[JTTY_NDATA][JTTY_NSTATES];     /* 23 kB, on the stack */
    float          pm[JTTY_NSTATES];
    unsigned       cand[MAX_LIST];
    float          cand_pm[MAX_LIST];
    int            ncand = 0;

    struct {
        jtty_payload_t payload;
        float          metric;
        bool           crc_ok;
    } paths[MAX_LIST];
    int npaths = 0;

    tables_init();

    if (list_size < 1) {
        list_size = 1;
    }
    if (list_size > MAX_LIST) {
        list_size = MAX_LIST;
    }

    /* Two circular passes to rank the start/end states. */
    for (unsigned s = 0; s < JTTY_NSTATES; s++) {
        pm[s] = 0.0f;
    }
    for (int pass = 0; pass < 2; pass++) {
        float best = NEG_INF;

        viterbi_pass(metric, pm, dec);
        for (unsigned s = 0; s < JTTY_NSTATES; s++) {
            if (pm[s] > best) {
                best = pm[s];
            }
        }
        for (unsigned s = 0; s < JTTY_NSTATES; s++) {
            pm[s] -= best;          /* keep the numbers small */
        }
    }

    /* The `list_size` best end states. */
    for (unsigned s = 0; s < JTTY_NSTATES; s++) {
        int i;

        if (ncand == list_size && pm[s] <= cand_pm[ncand - 1]) {
            continue;
        }
        i = (ncand < list_size) ? ncand++ : ncand - 1;
        while (i > 0 && cand_pm[i - 1] < pm[s]) {
            cand[i] = cand[i - 1];
            cand_pm[i] = cand_pm[i - 1];
            i--;
        }
        cand[i] = s;
        cand_pm[i] = pm[s];
    }

    /* Exact tail-biting decode through each of them. */
    for (int c = 0; c < ncand; c++) {
        uint8_t        info[JTTY_INFO_BITS];
        jtty_payload_t p;
        bool           ok;
        bool           dup = false;
        int            i;

        for (unsigned s = 0; s < JTTY_NSTATES; s++) {
            pm[s] = NEG_INF;
        }
        pm[cand[c]] = 0.0f;
        viterbi_pass(metric, pm, dec);
        traceback(dec, cand[c], info);
        ok = jtty_info_payload(info, &p);

        for (i = 0; i < npaths; i++) {
            if (paths[i].payload == p && paths[i].crc_ok == ok) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        /* keep the list sorted by path metric */
        i = npaths++;
        while (i > 0 && paths[i - 1].metric < pm[cand[c]]) {
            paths[i] = paths[i - 1];
            i--;
        }
        paths[i].payload = p;
        paths[i].metric = pm[cand[c]];
        paths[i].crc_ok = ok;
    }

    for (int i = 0; i < npaths; i++) {
        if (!paths[i].crc_ok) {
            continue;
        }
        if (accept != NULL && !accept(paths[i].payload, ctx)) {
            continue;
        }
        *payload = paths[i].payload;
        if (rank != NULL) {
            *rank = i;
        }
        return true;
    }
    return false;
}
