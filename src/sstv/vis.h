/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#ifndef SSTV_VIS_H
#define SSTV_VIS_H

#include <stdbool.h>
#include "decoder.h"

/*
 * SSTV VIS header detector (12 kHz mono float).
 *
 * Same contract as the old live_find_vis(): x is a block of n samples
 * (normally 1 s, scanned every 50 ms of audio) and the VIS must lie
 * entirely inside it.  A VIS starting in the first ~63 ms of the block is
 * found, so consecutive blocks must not be more than 50 ms apart.
 * n may be at most 2 s.  Reentrant: may be called from several threads.
 *
 * Returns true and fills *out for a supported mode.  For a valid header of a
 * known but unsupported mode it returns false and stores the VIS code in
 * res->unsupported_vis (otherwise -1), so the dialog can show its popup.
 */
typedef struct {
    int   unsupported_vis;  /* known VIS code we do not decode, or -1 */
    float leader_q;         /* leader tone energy fraction (gate)      */
    float offset_hz;        /* measured tuning offset (leader - 1900)  */
    float score;            /* frame score 0..1, -1 if no frame        */
    float start_s;          /* VIS start inside the block, seconds     */
    float bit_ms;           /* measured bit period                     */
    float slip_s;           /* gap found inside the data bits, seconds */
    bool  near_miss;        /* 1KO125: leaders and start bit found, frame rejected */
} sstv_vis_result_t;

bool sstv_vis_detect(const float *x, int n, sstv_info_t *out, sstv_vis_result_t *res);

/*
 * Locate the VIS with code vis_code inside an already captured buffer and
 * return the sample index where it ends (end of the stop bit = start of
 * the first picture line), or -1 if it is not found.  The search covers
 * VIS starts within the first search_samples samples of the buffer.
 */
long sstv_vis_find_end(const float *x, long n, int vis_code, long search_samples);

#endif
