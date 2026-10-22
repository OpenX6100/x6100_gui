/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#ifndef SSTV_PREVIEW_H
#define SSTV_PREVIEW_H

/*
 * Progressive display: decides which picture row can be drawn next, which
 * slice of the capture it needs, and draws it with the decoder's own row
 * renderer (sstv_render_row()).  Pure C, no LVGL, no locking: the caller
 * copies the slice out of the capture buffer under its own mutex, then
 * renders outside of it.  The final picture still comes from the batch
 * decoder; the preview is only shown while receiving.
 *
 * Robot 36 is previewed in grey (see sstv_preview_render()).
 *
 * Positions are sample indices of the capture buffer.  Row timing:
 *  - before the line-sync tracker locks: row 0 from the VIS + nominal period;
 *  - afterwards: phase and period of the received syncs (slant follows).
 */

#include <stdbool.h>
#include <stdint.h>
#include "decoder.h"
#include "linesync.h"

/* Measured biases (synthetic loopback, all modes): the VIS end found by
 * sstv/vis.c is 32 samples before the true row-0 sync, the tracker's sync
 * start 6-11 samples before the true sync. */
#define SSTV_PREVIEW_VIS_BIAS   32.0
#define SSTV_PREVIEW_SYNC_BIAS  10.0

typedef struct {
    sstv_mode_t mode;
    int         width, height;
    double      row0;          /* anchor of row 0 (bias corrected)           */
    double      nominal;       /* nominal line period, samples               */
    int         next_row;      /* first row not drawn yet                    */
    double      pend_anchor;   /* anchor of next_row, from the last need()   */
    double      pend_scale;
    long        pend_a, pend_b;
    sstv_row_state_t st;
} sstv_preview_t;

/* row0_anchor: sample index of the anchor of row 0 as estimated from the
 * VIS (e.g. vis_end + sstv_row0_anchor_after_vis(mode) * SR), WITHOUT bias
 * correction; it is applied here.  Pass < 0 if unknown (Manual): row 0 is
 * then the first tracker sync. */
void sstv_preview_init(sstv_preview_t *p, sstv_mode_t mode, double row0_anchor);

/* Is the next row ready?  written: samples available in the capture
 * buffer; ls: tracker status (may be NULL).  On true, [*a, *b) is the
 * slice to copy and pass to sstv_preview_render(). */
bool sstv_preview_need(sstv_preview_t *p, long written, const sstv_ls_status_t *ls, long *a, long *b);

/* Draw the row prepared by sstv_preview_need() from slice = capture[a..b).
 * rgb_row: sstv_row_width(mode) * 3 bytes.  Returns the row index drawn,
 * or -1 if it could not be drawn (it is skipped). */
int sstv_preview_render(sstv_preview_t *p, const float *slice, uint8_t *rgb_row);

/* Rows of the picture, and whether all of them have been handled. */
static inline bool sstv_preview_done(const sstv_preview_t *p) { return p->next_row >= p->height; }

#endif
