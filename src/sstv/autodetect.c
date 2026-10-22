/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#include "autodetect.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AD_SAMPLE_RATE          12000.0
#define AD_TIMEOUT_S            10.0
#define AD_SYNC_MIN_MS          3.0
#define AD_SYNC_MAX_MS          15.0
#define AD_SYNC_RATIO_ON        1.70
#define AD_SYNC_RATIO_OFF       1.35
#define AD_MIN_TONE_LEVEL       0.0020
#define AD_LPF_ALPHA            0.080
#define AD_MIN_VALID_INTERVALS  3
#define AD_MATCH_TOLERANCE      0.035
#define AD_REPORT_EVERY_SYNCS   1

typedef struct {
    sstv_autodetect_mode_t mode;
    const char *name;
    double line_ms;
} mode_spec_t;

static const mode_spec_t mode_specs[] = {
    { SSTV_AUTODETECT_ROBOT36,    "Robot 36",   150.000 },
    { SSTV_AUTODETECT_MARTIN_M1,  "Martin M1",  446.446 },
    { SSTV_AUTODETECT_MARTIN_M2,  "Martin M2",  226.798 },
    { SSTV_AUTODETECT_SCOTTIE_S1, "Scottie S1", 428.223 },
    { SSTV_AUTODETECT_SCOTTIE_S2, "Scottie S2", 277.692 },
    { SSTV_AUTODETECT_SCOTTIE_DX, "Scottie DX", 1050.300 }
};

static pthread_mutex_t ad_mutex = PTHREAD_MUTEX_INITIALIZER;
static sstv_autodetect_state_t ad_state = SSTV_AUTODETECT_IDLE;
static sstv_autodetect_mode_t ad_result = SSTV_AUTODETECT_NONE;
static uint64_t ad_samples = 0;
static uint64_t ad_sync_start = 0;
static uint64_t ad_last_sync_mid = 0;
static bool ad_in_sync = false;
static unsigned ad_sync_candidates = 0;
static unsigned ad_valid_syncs = 0;
static unsigned ad_valid_intervals = 0;
static unsigned ad_rejected_syncs = 0;
static unsigned ad_match_streak = 0;
static sstv_autodetect_mode_t ad_streak_mode = SSTV_AUTODETECT_NONE;

/* Three narrow quadrature detectors.  The SSTV video range starts at 1500 Hz,
 * so a stable 1200-Hz component is a particularly useful sync signature. */
typedef struct {
    double c, s;
    double rot_c, rot_s;
    double i_lp, q_lp;
} tone_detector_t;

static tone_detector_t td1200, td1500, td1900;

static void tone_init(tone_detector_t *t, double hz)
{
    double w = 2.0 * M_PI * hz / AD_SAMPLE_RATE;
    memset(t, 0, sizeof(*t));
    t->c = 1.0;
    t->s = 0.0;
    t->rot_c = cos(w);
    t->rot_s = sin(w);
}

static double tone_step(tone_detector_t *t, double x)
{
    double ni = x * t->c;
    double nq = x * t->s;
    t->i_lp += AD_LPF_ALPHA * (ni - t->i_lp);
    t->q_lp += AD_LPF_ALPHA * (nq - t->q_lp);

    double nc = t->c * t->rot_c - t->s * t->rot_s;
    double ns = t->s * t->rot_c + t->c * t->rot_s;
    t->c = nc;
    t->s = ns;

    return sqrt(t->i_lp * t->i_lp + t->q_lp * t->q_lp);
}

const char *sstv_autodetect_mode_name(sstv_autodetect_mode_t mode)
{
    for (size_t i = 0; i < sizeof(mode_specs)/sizeof(mode_specs[0]); ++i)
        if (mode_specs[i].mode == mode) return mode_specs[i].name;
    return "Unknown";
}

static const mode_spec_t *closest_mode(double interval_ms, double *error_out)
{
    const mode_spec_t *best = NULL;
    double best_error = 1e9;
    for (size_t i = 0; i < sizeof(mode_specs)/sizeof(mode_specs[0]); ++i) {
        double e = fabs(interval_ms - mode_specs[i].line_ms) / mode_specs[i].line_ms;
        if (e < best_error) {
            best_error = e;
            best = &mode_specs[i];
        }
    }
    if (error_out) *error_out = best_error;
    return best;
}

void sstv_autodetect_start(void)
{
    pthread_mutex_lock(&ad_mutex);
    ad_state = SSTV_AUTODETECT_LISTENING;
    ad_result = SSTV_AUTODETECT_NONE;
    ad_samples = 0;
    ad_sync_start = 0;
    ad_last_sync_mid = 0;
    ad_in_sync = false;
    ad_sync_candidates = 0;
    ad_valid_syncs = 0;
    ad_valid_intervals = 0;
    ad_rejected_syncs = 0;
    ad_match_streak = 0;
    ad_streak_mode = SSTV_AUTODETECT_NONE;
    tone_init(&td1200, 1200.0);
    tone_init(&td1500, 1500.0);
    tone_init(&td1900, 1900.0);
    fprintf(stderr, "SSTV AUTODETECT: session started\n");
    fprintf(stderr, "SSTV AUTODETECT: sample rate=12000 Hz, timeout=10.0 s\n");
    fprintf(stderr, "SSTV AUTODETECT: looking for 1200-Hz line syncs (Robot 36/Martin/Scottie)\n");
    pthread_mutex_unlock(&ad_mutex);
}

void sstv_autodetect_stop(void)
{
    pthread_mutex_lock(&ad_mutex);
    if (ad_state == SSTV_AUTODETECT_LISTENING)
        fprintf(stderr, "SSTV AUTODETECT: session stopped by caller\n");
    ad_state = SSTV_AUTODETECT_IDLE;
    ad_result = SSTV_AUTODETECT_NONE;
    pthread_mutex_unlock(&ad_mutex);
}

void sstv_autodetect_process(const float *samples, size_t count)
{
    if (!samples || count == 0) return;

    pthread_mutex_lock(&ad_mutex);
    if (ad_state != SSTV_AUTODETECT_LISTENING) {
        pthread_mutex_unlock(&ad_mutex);
        return;
    }

    for (size_t k = 0; k < count; ++k) {
        double x = samples[k];
        double a1200 = tone_step(&td1200, x);
        double a1500 = tone_step(&td1500, x);
        double a1900 = tone_step(&td1900, x);
        double other = a1500 > a1900 ? a1500 : a1900;
        bool sync_on = a1200 > AD_MIN_TONE_LEVEL && a1200 > other * AD_SYNC_RATIO_ON;
        bool sync_keep = a1200 > AD_MIN_TONE_LEVEL && a1200 > other * AD_SYNC_RATIO_OFF;

        if (!ad_in_sync && sync_on) {
            ad_in_sync = true;
            ad_sync_start = ad_samples;
            ad_sync_candidates++;
        } else if (ad_in_sync && !sync_keep) {
            uint64_t end = ad_samples;
            double dur_ms = (double)(end - ad_sync_start) * 1000.0 / AD_SAMPLE_RATE;
            ad_in_sync = false;

            if (dur_ms >= AD_SYNC_MIN_MS && dur_ms <= AD_SYNC_MAX_MS) {
                uint64_t mid = ad_sync_start + (end - ad_sync_start) / 2;
                ad_valid_syncs++;
                fprintf(stderr, "SSTV AUTODETECT: valid 1200-Hz sync #%u, duration=%.2f ms\n",
                        ad_valid_syncs, dur_ms);

                if (ad_last_sync_mid != 0) {
                    double interval_ms = (double)(mid - ad_last_sync_mid) * 1000.0 / AD_SAMPLE_RATE;
                    double error = 0.0;
                    const mode_spec_t *m = closest_mode(interval_ms, &error);
                    ad_valid_intervals++;

                    fprintf(stderr,
                            "SSTV AUTODETECT: line interval=%.2f ms -> nearest=%s (expected %.3f ms, error=%.2f%%)\n",
                            interval_ms, m->name, m->line_ms, error * 100.0);

                    if (error <= AD_MATCH_TOLERANCE) {
                        if (ad_streak_mode == m->mode)
                            ad_match_streak++;
                        else {
                            ad_streak_mode = m->mode;
                            ad_match_streak = 1;
                        }

                        unsigned confidence = ad_match_streak * 25u + 25u;
                        if (confidence > 99u) confidence = 99u;
                        fprintf(stderr, "SSTV AUTODETECT: candidate=%s confidence=%u%% (%u consecutive matching intervals)\n",
                                m->name, confidence, ad_match_streak);

                        if (ad_match_streak >= AD_MIN_VALID_INTERVALS) {
                            ad_result = m->mode;
                            ad_state = SSTV_AUTODETECT_DETECTED;
                            double elapsed = (double)ad_samples / AD_SAMPLE_RATE;
                            fprintf(stderr, "SSTV AUTODETECT: DETECTED %s\n", m->name);
                            fprintf(stderr, "SSTV AUTODETECT: elapsed=%.2f s, valid syncs=%u, valid intervals=%u\n",
                                    elapsed, ad_valid_syncs, ad_valid_intervals);
                            fprintf(stderr, "SSTV AUTODETECT: session stopped\n");
                            pthread_mutex_unlock(&ad_mutex);
                            return;
                        }
                    } else {
                        ad_streak_mode = SSTV_AUTODETECT_NONE;
                        ad_match_streak = 0;
                    }
                }
                ad_last_sync_mid = mid;
            } else {
                ad_rejected_syncs++;
                if (dur_ms >= 1.0)
                    fprintf(stderr, "SSTV AUTODETECT: rejected 1200-Hz pulse, duration=%.2f ms\n", dur_ms);
            }
        }

        ad_samples++;
        if ((double)ad_samples / AD_SAMPLE_RATE >= AD_TIMEOUT_S) {
            ad_state = SSTV_AUTODETECT_TIMEOUT;
            fprintf(stderr, "SSTV AUTODETECT: TIMEOUT after 10.00 s\n");
            fprintf(stderr, "SSTV AUTODETECT: sync candidates=%u, valid syncs=%u, valid intervals=%u, rejected=%u\n",
                    ad_sync_candidates, ad_valid_syncs, ad_valid_intervals, ad_rejected_syncs);
            fprintf(stderr, "SSTV AUTODETECT: no reliable mode detected\n");
            fprintf(stderr, "SSTV AUTODETECT: session stopped\n");
            pthread_mutex_unlock(&ad_mutex);
            return;
        }
    }

    pthread_mutex_unlock(&ad_mutex);
}

sstv_autodetect_state_t sstv_autodetect_state(void)
{
    pthread_mutex_lock(&ad_mutex);
    sstv_autodetect_state_t s = ad_state;
    pthread_mutex_unlock(&ad_mutex);
    return s;
}

sstv_autodetect_mode_t sstv_autodetect_result(void)
{
    pthread_mutex_lock(&ad_mutex);
    sstv_autodetect_mode_t r = ad_result;
    pthread_mutex_unlock(&ad_mutex);
    return r;
}

bool sstv_autodetect_is_listening(void)
{
    return sstv_autodetect_state() == SSTV_AUTODETECT_LISTENING;
}
