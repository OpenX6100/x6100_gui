/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#ifndef SSTV_SYNCGRID_H
#define SSTV_SYNCGRID_H
/*
 * Line-sync grid check (12 kHz mono float).
 *
 * Used to confirm a VIS heard during a reception (Restart): a real new
 * picture brings the line syncs of its mode at the right distance from the
 * end of its VIS, and the syncs of the picture that was coming in stop.
 * Picture content that reads as a VIS does neither.
 *
 * The measure is the share of the 1200-Hz sync tone (plus the tuning offset)
 * in the sync slots of a grid, averaged over the slots, for the best common
 * shift within a tolerance: about 1 for clean syncs, 0.1..0.4 for noise or
 * picture content.
 */
#include <stdbool.h>
#include "decoder.h"

/* Seconds from the end of the VIS to the first line sync of the mode
 * (Scottie: the sync in the middle of the line), < 0 if unknown. */
double sstv_syncgrid_first_after_vis(sstv_mode_t mode);

/* Sync pulse length of the mode, seconds (0 if unknown). */
double sstv_syncgrid_sync_s(sstv_mode_t mode);

/* Mean sync share on the grid t_k = first_s + k * period_s, for every slot
 * that ends before until_s (times in seconds from x[0]), at the best common
 * shift within +/- tol_s.  *slots (optional): slots measured.  Returns -1 if
 * no slot fits in x[0..n). */
float sstv_syncgrid_measure(const float *x, long n, double first_s, double period_s, double sync_s,
                            double until_s, double tol_s, float offset_hz, int *slots);

/* Phase of a sync grid of period_s found in x[from_s .. to_s): the time (s
 * from x[0]) of a sync, best over one period.  *share: its measure.
 * Returns < 0 if the stretch is shorter than two periods. */
double sstv_syncgrid_find(const float *x, long n, double from_s, double to_s, double period_s, double sync_s,
                          float offset_hz, float *share);

/* Restart check.  x[0..n): the audio of the picture coming in, up to at
 * least vis_end_s + span_s (seconds from x[0]).  The new picture (new_mode,
 * tuning new_off) must bring its syncs after vis_end_s; the syncs of the old
 * one (old_mode, old_off), found in the 3 s before the VIS, must stop from
 * the VIS on.  Two
 * pictures that both go on: the one coming in wins (not confirmed). */
typedef struct {
    float new_share;        /* syncs of the new mode after the VIS            */
    int   new_slots;
    bool  old_known;        /* the old picture's syncs were found before it    */
    float old_before;       /* ... with this share                             */
    float old_share;        /* ... and from the VIS on (should be gone)        */
    int   old_slots;
    bool  confirmed;
} sstv_syncgrid_verdict_t;

bool sstv_syncgrid_restart_check(const float *x, long n, double vis_end_s, double span_s,
                                 sstv_mode_t new_mode, float new_off, sstv_mode_t old_mode, float old_off,
                                 sstv_syncgrid_verdict_t *v);

#endif
