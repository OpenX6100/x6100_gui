/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#ifndef SSTV_LINESYNC_H
#define SSTV_LINESYNC_H

/*
 * Real-time SSTV line-sync tracker (12 kHz mono float, streaming).
 *
 * Finds 1200-Hz sync pulses, measures their start and length, recognises the
 * mode from line period + sync length, then follows the line grid and says
 * when the syncs are gone.  No allocation, no LVGL: the caller keeps one
 * sstv_linesync_t and feeds it audio blocks of any size.
 * Not thread-safe: one tracker is used by one thread at a time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "decoder.h"

typedef enum {
    SSTV_LS_SEARCH = 0,   /* no line grid yet                                */
    SSTV_LS_LOCKED,       /* following the line grid of status.mode          */
    SSTV_LS_LOST          /* was locked; no coherent sync for lost_after     */
} sstv_ls_state_t;

#define SSTV_LS_WIN     24    /* 2-ms correlation window                      */
#define SSTV_LS_HIST    64    /* remembered pulses                             */
#define SSTV_LS_FIT     32    /* received syncs used for the period estimate   */
#define SSTV_LS_NMODES  8

typedef struct {
    sstv_ls_state_t state;
    sstv_mode_t mode;         /* locked mode, SSTV_MODE_UNKNOWN while searching */
    uint64_t samples;         /* audio processed so far                         */
    uint64_t last_sync;       /* start of the last received sync (sample)       */
    uint64_t lock_sample;     /* when the lock was declared                     */
    uint32_t hits;            /* syncs received while locked                    */
    double   period;          /* line period estimate, samples                  */
    double   clock_ppm;       /* period error vs nominal                        */
    uint64_t lost_after;      /* silence (no sync) that means LOST, samples     */
    uint64_t last_video;      /* end of the last 1-s block with SSTV video      */
    float    band_ratio;      /* last 1-s in-band energy fraction (0..1)        */
} sstv_ls_status_t;

typedef struct {
    sstv_mode_t forced;       /* SSTV_MODE_UNKNOWN = autodetect                 */
    /* 1200-Hz sliding correlator */
    float    bc[SSTV_LS_WIN], bs[SSTV_LS_WIN], be[SSTV_LS_WIN];
    double   sc, ss, se;
    int      rp, ph;
    bool     in_pulse;
    uint64_t run_start;
    /* pulse history */
    uint64_t pt[SSTV_LS_HIST];
    float    pl[SSTV_LS_HIST];
    uint8_t  chain[SSTV_LS_HIST][SSTV_LS_NMODES];  /* chained syncs ending here */
    uint8_t  direct[SSTV_LS_HIST][SSTV_LS_NMODES]; /* ...of which 1-line links  */
    int      np, hp;
    /* mode evidence */
    long     ev_total[SSTV_LS_NMODES];  /* confirmed intervals, whole run */
    /* locked tracking */
    int      mi;
    double   fx[SSTV_LS_FIT], fy[SSTV_LS_FIT];
    int      nfit, hfit;
    double   cur_line;        /* line index of st.last_sync (lock pulse = 0) */
    double   len_avg;         /* mean measured length of received syncs, ms  */
    /* SSTV video-band monitor: biquad states (2x HP 1100, 2x LP 2400 | 2x HP 300) */
    float    bq[6][2];
    double   e_in, e_all;
    uint32_t e_n;
    sstv_ls_status_t st;
} sstv_linesync_t;

/* forced_mode: mode chosen by the user, or SSTV_MODE_UNKNOWN to autodetect.
 * Resets everything; sample counting restarts at 0. */
void sstv_linesync_init(sstv_linesync_t *ls, sstv_mode_t forced_mode);
void sstv_linesync_process(sstv_linesync_t *ls, const float *x, size_t n);

static inline const sstv_ls_status_t *sstv_linesync_status(const sstv_linesync_t *ls)
{
    return &ls->st;
}

/* Nominal line period and picture duration (lines only), seconds; 0 if unknown. */
double sstv_linesync_line_seconds(sstv_mode_t mode);
double sstv_linesync_picture_seconds(sstv_mode_t mode);

/* A-posteriori mode recognition over a whole captured buffer, more reliable
 * than the first seconds.  prefer: the mode the real-time tracker locked on
 * (or SSTV_MODE_UNKNOWN); it is confirmed when it has at least half the
 * evidence of the best mode - a second, overlapping SSTV transmission or
 * chance confirmations in noise must not override it.  Returns
 * SSTV_MODE_UNKNOWN if no mode has a credible chain; *evidence (optional)
 * = confirmed line intervals of the result. */
sstv_mode_t sstv_linesync_classify(const float *x, long n, sstv_mode_t prefer, long *evidence);

#endif
