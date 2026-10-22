/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 SSTV - progressive display (see preview.h)
 *
 * (c) 2026 Hany El Imam
 */
#include "preview.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PV_SR      SSTV_SAMPLE_RATE
#define PV_MARGIN  100            /* > FM front-end half length (30 + 7)    */

void sstv_preview_init(sstv_preview_t *p, sstv_mode_t mode, double row0_anchor)
{
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    p->width = sstv_row_width(mode);
    p->height = (mode == SSTV_MODE_ROBOT8_BW) ? 120 :
                (mode == SSTV_MODE_ROBOT36 || mode == SSTV_MODE_ROBOT72) ? 240 : 256;
    p->nominal = sstv_linesync_line_seconds(mode) * PV_SR;
    p->row0 = row0_anchor >= 0.0 ? row0_anchor + SSTV_PREVIEW_VIS_BIAS : -1.0;
}

bool sstv_preview_need(sstv_preview_t *p, long written, const sstv_ls_status_t *ls, long *a, long *b)
{
    if (p->width <= 0 || p->nominal <= 0.0 || sstv_preview_done(p)) return false;
    bool locked = ls && ls->state != SSTV_LS_SEARCH && ls->mode == p->mode && ls->hits >= 3;

    /* Manual without VIS: row 0 is the first sync the tracker received. */
    if (p->row0 < 0.0) {
        if (!locked) return false;
        /* earliest complete row in the buffer, whole periods before the
         * sync the tracker locked on */
        double sync = (double)ls->last_sync + SSTV_PREVIEW_SYNC_BIAS;
        double k = floor((sync - 0.8 * ls->period - PV_MARGIN) / ls->period);
        p->row0 = sync - (k > 0.0 ? k : 0.0) * ls->period;
    }

    double anchor, period;
    if (locked) {
        /* phase and period from the received syncs; the row index of the
         * last sync comes from the row-0 estimate (integer, robust) */
        double sync = (double)ls->last_sync + SSTV_PREVIEW_SYNC_BIAS;
        double r_last = floor((sync - p->row0) / p->nominal + 0.5);
        period = ls->period;
        anchor = sync + ((double)p->next_row - r_last) * period;
    } else {
        period = p->nominal;
        anchor = p->row0 + (double)p->next_row * period;
    }
    /* every mode's row lies within [anchor - 0.8 P, anchor + 1.1 P] */
    long sa = (long)floor(anchor - 0.8 * period) - PV_MARGIN;
    long sb = (long)ceil(anchor + 1.1 * period) + PV_MARGIN;
    if (sb > written) return false;                 /* not received yet */
    if (sa < 0) sa = 0;
    p->pend_anchor = anchor;
    p->pend_scale = period / p->nominal;
    p->pend_a = sa;
    p->pend_b = sb;
    *a = sa;
    *b = sb;
    return true;
}

int sstv_preview_render(sstv_preview_t *p, const float *slice, uint8_t *rgb_row)
{
    int row = p->next_row++;
    long n = p->pend_b - p->pend_a;
    float *freq = sstv_fm_demod(slice, n);
    if (!freq) return -1;
    long base = lround(p->pend_anchor) - p->pend_a;
    int rc = sstv_render_row(p->mode, freq, n, base, p->pend_scale, &p->st, rgb_row);
    free(freq);
    if (rc == 0 && p->mode == SSTV_MODE_ROBOT36) {
        /* Robot 36 preview in grey: deciding R-Y/B-Y row by row from the
         * separator was unreliable on real recordings (consistently wrong
         * colours on a noisy picture); the final decode estimates it over
         * the whole picture and brings the colour.  Luma is exact anyway:
         * 0.299 R + 0.587 G + 0.114 B gives back Y whatever the chroma. */
        for (int px = 0; px < p->width; ++px) {
            uint8_t *q = rgb_row + px * 3;
            int y = (299 * q[0] + 587 * q[1] + 114 * q[2] + 500) / 1000;
            q[0] = q[1] = q[2] = (uint8_t)(y > 255 ? 255 : y);
        }
    }
    return rc < 0 ? -1 : row;
}
