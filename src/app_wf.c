/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - an app's waterfall, as JS8's and XARA's
 */

#include "app_wf.h"

#include "cfg/cfg_api.h"
#include "dsp.h"
#include "js8_wf.h"
#include "radio.h"
#include "spectrum.h"
#include "styles.h"
#include "waterfall.h"

#include <liquid/liquid.h>
#include <math.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_RATE     12000
#define ROWS_PER_SEC    15
#define ROW_SAMPLES     (SAMPLE_RATE / ROWS_PER_SEC)
#define QUEUE           16
#define TICK_MS         5
#define MIN_DB          0     /* above the noise floor, across the palette */
#define MAX_DB          30
#define CALM_NEW        0.65f /* JS8's "Light": each receive row is 65 % new */
#define TX_EDGE_PX      6     /* the red edge on rows of our own transmission */
#define TX_RED          0xe63c3c
#define MAX_W           800
#define PLAYBACK        "alsa_output.platform-sound.stereo-fallback"

static lv_obj_t   *box;
static lv_obj_t   *tx_tag;
static lv_timer_t *timer;
static Observer   *watch[4];
static int         width;
static int64_t     due_us;
static bool        was_tx;

/* Shared with the receive (dsp) and TX monitor threads, under lock */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static spgramf  sg;
static unsigned nfft;
static bool     sliding;
static float   *psd;
static int      lo_hz, hi_hz;
static unsigned row_fill;
static bool     row_tx;     /* the row being filled is our own transmission */
static float    row[MAX_W], sel[MAX_W];
static float    floor_db;
static bool     floor_set;
static float    avg[MAX_W];
static bool     avg_set;
static float    rows[QUEUE][MAX_W];
static bool     rows_tx[QUEUE];
static unsigned q_head, q_count;

static uint32_t    rx_sub = AUDIO_SUB_INVALID;
static pthread_t   txmon;
static bool        txmon_running;
static atomic_bool stop_txmon;
static atomic_int  tx_shift_hz;

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* k-th smallest of v[0..n) (partly reorders v), as JS8's. */
static float select_nth(float *v, int n, int k) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = v[lo + (hi - lo) / 2];
        int   i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) {
                float t = v[i];
                v[i]    = v[j];
                v[j]    = t;
                i++;
                j--;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else return v[k];
    }
    return v[k];
}

/* lock held */
static void emit_row(void) {
    spgramf_get_psd(sg, psd);
    liquid_vectorf_addscalar(psd, nfft, -10.f * log10f(sqrtf(nfft)), psd);
    if (sliding) spgramf_clear(sg); /* keep the audio: the next row's FFT reaches back over it */
    else spgramf_reset(sg);

    /* Our own transmission goes on the air shifted (the VFO moves). */
    int shift = row_tx ? atomic_load(&tx_shift_hz) : 0;

    /* spgram output is FFT-shifted: bin nfft/2 is 0 Hz. One value per
     * pixel: the strongest bin under it, so a narrow signal doesn't flicker. */
    int64_t lo = (int64_t)nfft / 2 + (int64_t)nfft * (lo_hz - shift) / SAMPLE_RATE;
    int64_t hi = (int64_t)nfft / 2 + (int64_t)nfft * (hi_hz - shift) / SAMPLE_RATE;
    int64_t bins = hi - lo;
    for (int x = 0; x < width; x++) {
        int64_t b0 = lo + x * bins / width, b1 = lo + (x + 1) * bins / width;
        if (b1 <= b0) b1 = b0 + 1;
        float v = -200.0f;
        for (int64_t b = b0; b < b1; b++)
            if (b >= 0 && b < nfft && psd[b] > v) v = psd[b];
        row[x] = v;
    }

    /* Noise floor: the 20th percentile of the row, smoothed over ~2 s
     * (XARA's). Our own transmission doesn't move it. */
    if (!row_tx) {
        memcpy(sel, row, width * sizeof(float));
        float now = select_nth(sel, width, width / 5);
        floor_db  = floor_set ? floor_db + 0.05f * (now - floor_db) : now;
        floor_set = true;
    }
    float base = floor_set ? floor_db : -100.0f;
    /* Our own transmission is far above the receiver's noise: drawn relative
     * to its own peak, a few dB under the top of the palette. */
    if (row_tx) {
        float top = row[0];
        for (int i = 1; i < width; i++) top = row[i] > top ? row[i] : top;
        base = top - (MAX_DB - 4);
    }
    for (int i = 0; i < width; i++) row[i] -= base;

    /* Calm: receive rows blend into the ones before; our own transmission is
     * drawn as it is, and starts the blend again. */
    if (row_tx) {
        avg_set = false;
    } else {
        if (!avg_set) memcpy(avg, row, width * sizeof(float));
        else
            for (int i = 0; i < width; i++) avg[i] += CALM_NEW * (row[i] - avg[i]);
        avg_set = true;
        memcpy(row, avg, width * sizeof(float));
    }

    if (q_count == QUEUE) {
        q_head = (q_head + 1) % QUEUE;
        q_count--;
    }
    unsigned at = (q_head + q_count) % QUEUE;
    memcpy(rows[at], row, width * sizeof(float));
    rows_tx[at] = row_tx;
    q_count++;
}

/* Receive audio (dsp's thread) or the audio going out (the monitor's). */
static void feed(const float *samples, unsigned n, bool tx) {
    pthread_mutex_lock(&lock);
    if (!sg || !psd) {
        pthread_mutex_unlock(&lock);
        return;
    }
    if (tx != row_tx) { /* a new row for the other side */
        spgramf_reset(sg);
        row_fill = 0;
        row_tx   = tx;
    }
    while (n) {
        unsigned take = ROW_SAMPLES - row_fill;
        if (take > n) take = n;
        spgramf_write(sg, (float *)samples, take);
        samples += take;
        n -= take;
        row_fill += take;
        if (row_fill >= ROW_SAMPLES) {
            row_fill = 0;
            emit_row();
        }
    }
    pthread_mutex_unlock(&lock);
}

static void rx_audio_cb(size_t n, float *samples) {
    if (radio_get_state() != RADIO_RX) return; /* the receiver is off while we send */
    feed(samples, (unsigned)n, false);
}

/* The audio going out: the playback device's monitor, read the whole time
 * the window is open (it's small), drawn only while the radio transmits. */
static void *txmon_main(void *arg) {
    (void)arg;
    pa_sample_spec spec = {.format = PA_SAMPLE_FLOAT32NE, .channels = 1, .rate = SAMPLE_RATE};
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .fragsize  = ROW_SAMPLES * sizeof(float),
    };
    int        err;
    pa_simple *pa = NULL;
    float      buf[ROW_SAMPLES / 2];

    while (!atomic_load(&stop_txmon)) {
        if (!pa) {
            pa = pa_simple_new(NULL, "X6100 GUI waterfall TX", PA_STREAM_RECORD, PLAYBACK ".monitor",
                               "Waterfall TX monitor", &spec, NULL, &attr, &err);
            if (!pa) {
                LV_LOG_WARN("Waterfall: TX monitor: %s", pa_strerror(err));
                for (int i = 0; i < 10 && !atomic_load(&stop_txmon); i++) usleep(200000);
                continue;
            }
        }
        if (pa_simple_read(pa, buf, sizeof(buf), &err) < 0) {
            pa_simple_free(pa);
            pa = NULL;
            continue;
        }
        if (radio_get_state() == RADIO_TX) feed(buf, ROW_SAMPLES / 2, true);
    }
    if (pa) pa_simple_free(pa);
    return NULL;
}

/* At most one row per tick, each when it's due by the clock (JS8's pacing). */
static void timer_cb(lv_timer_t *t) {
    (void)t;
    js8_wf_tick();
    pthread_mutex_lock(&lock);
    unsigned waiting = q_count;
    pthread_mutex_unlock(&lock);
    if (!waiting) return;
    int64_t now    = now_us();
    int64_t period = 1000000 / ROWS_PER_SEC;
    if (waiting > 6) period = period * 3 / 4;
    else if (waiting > 3) period = period * 19 / 20;
    if (now - due_us > period) due_us = now;
    if (now < due_us) return;

    static float r[MAX_W];
    bool         tx;
    pthread_mutex_lock(&lock);
    memcpy(r, rows[q_head], width * sizeof(float));
    tx     = rows_tx[q_head];
    q_head = (q_head + 1) % QUEUE;
    q_count--;
    pthread_mutex_unlock(&lock);
    js8_wf_add_row(r, width);
    if (tx) {
        js8_wf_fill_rect(0, 0, TX_EDGE_PX - 1, 0, lv_color_hex(TX_RED));
        js8_wf_fill_rect(width - TX_EDGE_PX, 0, width - 1, 0, lv_color_hex(TX_RED));
    }
    if (tx != was_tx && tx_tag) {
        if (tx) lv_obj_clear_flag(tx_tag, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(tx_tag, LV_OBJ_FLAG_HIDDEN);
        was_tx = tx;
    }
    due_us += period;
    js8_wf_tick();
}

static void watch_cb(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    js8_wf_repaint_soon();
}

/* lock held: an FFT with at least a bin per pixel, a power of two (as JS8). */
static void make_sg(void) {
    unsigned want = (unsigned)((int64_t)width * SAMPLE_RATE / (hi_hz - lo_hz));
    unsigned n    = 256;
    while (n < want && n < 8192) n *= 2; /* WSPR: 1.46 Hz bins, its tone spacing */
    if (sg && n == nfft) return;
    if (sg) spgramf_destroy(sg);
    free(psd);
    nfft = n;
    /* Sliding: a transform every row, over the last nfft samples. */
    unsigned step = sliding ? ROW_SAMPLES : (nfft / 2 < ROW_SAMPLES / 2 ? nfft / 2 : ROW_SAMPLES / 2);
    sg            = spgramf_create(nfft, LIQUID_WINDOW_HANN, nfft, step);
    psd           = malloc(nfft * sizeof(float));
    row_fill      = 0;
}

bool app_wf_start(lv_obj_t *b, const app_wf_cfg_t *opt) {
    lv_obj_update_layout(b);
    width = lv_obj_get_width(b);
    if (width > MAX_W) return false;
    if (!js8_wf_create(b, (const lv_color_t *)style.wf_palette, MIN_DB, MAX_DB)) return false;
    box = b;

    pthread_mutex_lock(&lock);
    sliding   = opt->sliding;
    lo_hz     = opt->lo_hz;
    hi_hz     = opt->hi_hz > opt->lo_hz ? opt->hi_hz : opt->lo_hz + 1;
    make_sg();
    row_tx    = false;
    floor_set = false;
    avg_set   = false;
    q_head = q_count = 0;
    pthread_mutex_unlock(&lock);
    due_us = 0;
    was_tx = false;
    atomic_store(&tx_shift_hz, 0); /* WSPR sends where it is; FT8 sets its own */

    tx_tag = lv_label_create(box);
    lv_obj_set_style_text_font(tx_tag, &sony_28, 0);
    lv_obj_set_style_text_color(tx_tag, lv_color_white(), 0);
    lv_obj_set_style_bg_color(tx_tag, lv_color_hex(TX_RED), 0);
    lv_obj_set_style_bg_opa(tx_tag, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(tx_tag, 14, 0);
    lv_label_set_text(tx_tag, "TX");
    lv_obj_align(tx_tag, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_obj_add_flag(tx_tag, LV_OBJ_FLAG_HIDDEN);

    waterfall_set_enabled(false);
    spectrum_set_enabled(false);
    /* The main screen still redraws its part of the plane when these change. */
    watch[0] = subject_subscribe((Subject *)cfg.cur.fg_freq(), watch_cb, NULL);
    watch[1] = subject_subscribe((Subject *)cfg.band.if_shift(), watch_cb, NULL);
    watch[2] = subject_subscribe((Subject *)cfg.mode.zoom(), watch_cb, NULL);
    watch[3] = subject_subscribe((Subject *)cfg.cur.mode_lo_offset(), watch_cb, NULL);
    timer    = lv_timer_create(timer_cb, TICK_MS, NULL);

    atomic_store(&stop_txmon, false);
    txmon_running = pthread_create(&txmon, NULL, txmon_main, NULL) == 0;

    rx_sub = dsp_audio_subscribe_float(rx_audio_cb, SAMPLE_RATE);
    dsp_audio_set_active(rx_sub, true);
    return true;
}

void app_wf_stop(void) {
    if (!box) return;
    /* No receive callback after this returns (it takes dsp's lock). */
    dsp_audio_unsubscribe(rx_sub);
    rx_sub = AUDIO_SUB_INVALID;
    atomic_store(&stop_txmon, true);
    if (txmon_running) pthread_join(txmon, NULL);
    txmon_running = false;
    if (timer) lv_timer_del(timer);
    timer = NULL;
    for (unsigned i = 0; i < sizeof(watch) / sizeof(watch[0]); i++) {
        if (watch[i]) param_unsubscribe(watch[i]);
        watch[i] = NULL;
    }
    js8_wf_destroy();
    waterfall_set_enabled(true);
    spectrum_set_enabled(true);
    box    = NULL;
    tx_tag = NULL; /* deleted with the box, by the window */

    pthread_mutex_lock(&lock);
    if (sg) spgramf_destroy(sg);
    sg = NULL;
    free(psd);
    psd = NULL;
    pthread_mutex_unlock(&lock);
}

void app_wf_set_range(int lo, int hi) {
    pthread_mutex_lock(&lock);
    if (lo != lo_hz || hi != hi_hz) {
        lo_hz     = lo;
        hi_hz     = hi > lo ? hi : lo + 1;
        floor_set = false;
        avg_set   = false;
        if (sg) make_sg();
    }
    pthread_mutex_unlock(&lock);
}

void app_wf_clear(void) {
    pthread_mutex_lock(&lock);
    avg_set = false;
    q_head = q_count = 0;
    pthread_mutex_unlock(&lock);
    js8_wf_clear();
}

void app_wf_set_tx_shift(int shift_hz) {
    atomic_store(&tx_shift_hz, shift_hz);
}
