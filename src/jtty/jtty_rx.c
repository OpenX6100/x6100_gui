/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY streaming receiver
 */

#include "jtty_rx.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <complex.h>

#include "jtty_demod.h"

#define BUF_SAMPLES     (4 * JTTY_RATE)         /* audio kept */
#define PASS_SAMPLES    (JTTY_RATE / 2)         /* a pass every 0.5 s */
#define TAIL            (JTTY_NSPS / 2)         /* refinement room after a frame */
#define MAX_MSGS        24
#define MAX_RECENT      64
#define MAX_DEC         64

#define PSD_N           2048                    /* 5.86 Hz bins */
#define PSD_HOP         1536                    /* a line every 0.128 s */
#define PSD_MAX_BINS    1024

typedef struct {
    bool       open;
    jtty_msg_t m;
    uint64_t   next;                    /* where its next frame must start */
    bool       tried;                   /* next frame tried at its place */
} slot_t;

typedef struct {
    uint64_t start;
    float    freq;
    float    quality;
    float    snr;
    jtty_payload_t payload;
} recent_t;

struct jtty_rx_s {
    jtty_rx_cb_t    cb;
    void           *ctx;

    pthread_mutex_t lock;               /* audio and band */
    float          *buf;
    int             len;
    uint64_t        abs0;               /* sample number of buf[0] */
    uint64_t        fed;
    uint64_t        next_pass;          /* run a pass once fed reaches it */
    float           fmin;
    float           fmax;
    bool            reset;

    /* worker side */
    float          *work;
    uint64_t        searched;           /* frame starts below this are done */
    slot_t          slots[MAX_MSGS];
    recent_t        recent[MAX_RECENT];
    int             nrecent;
    uint32_t        next_id;

    /* waterfall */
    jtty_psd_cb_t   psd_cb;
    void           *psd_ctx;
    float           psd_lo;
    float           psd_hi;
    int             psd_bins;
    uint64_t        next_psd;               /* line due when fed reaches it */

    pthread_t       thread;
    bool            started;
    atomic_bool     stop_req;
    pthread_cond_t  cv;
};

/* ---- Messages ---------------------------------------------------------- */

static void emit(jtty_rx_t *rx, slot_t *s)
{
    if (rx->cb != NULL) {
        rx->cb(&s->m, rx->ctx);
    }
}

static void close_slot(jtty_rx_t *rx, slot_t *s)
{
    size_t len = strlen(s->m.text);

    while (len > 0 && s->m.text[len - 1] == ' ') {
        s->m.text[--len] = '\0';
    }
    s->m.done = true;
    s->open = false;
    emit(rx, s);
}

static bool seen(jtty_rx_t *rx, uint64_t start, float freq)
{
    for (int i = 0; i < rx->nrecent; i++) {
        uint64_t a = rx->recent[i].start;
        uint64_t d = a > start ? a - start : start - a;

        if (d < JTTY_NSPS / 2 && fabsf(rx->recent[i].freq - freq) < JTTY_BAUD / 2) {
            return true;
        }
    }
    return false;
}

/* A frame decoded in an earlier pass, at the same time, whose band
 * overlaps this one and that fits its signal better: this is a ghost of
 * it (see jtty_demod_search()). */
static bool ghost(jtty_rx_t *rx, uint64_t start, const jtty_decode_t *d)
{
    for (int i = 0; i < rx->nrecent; i++) {
        const recent_t *r = &rx->recent[i];

        if (jtty_demod_ghost((int64_t)start, d->freq, d->quality, d->snr,
                             (int64_t)r->start, r->freq, r->quality, r->snr)) {
            return true;
        }
        /* a weaker copy of a frame already taken */
        if (jtty_demod_copy((int64_t)start, d->payload, (int64_t)r->start, r->payload) &&
            r->snr >= d->snr) {
            return true;
        }
    }
    return false;
}

static void remember(jtty_rx_t *rx, uint64_t start, const jtty_decode_t *d)
{
    if (rx->nrecent == MAX_RECENT) {
        memmove(rx->recent, rx->recent + 1, sizeof(recent_t) * (MAX_RECENT - 1));
        rx->nrecent--;
    }
    rx->recent[rx->nrecent].start = start;
    rx->recent[rx->nrecent].freq = d->freq;
    rx->recent[rx->nrecent].quality = d->quality;
    rx->recent[rx->nrecent].snr = d->snr;
    rx->recent[rx->nrecent].payload = d->payload;
    rx->nrecent++;
}

/* Puts a decoded frame into its message, or starts one. */
static void take_frame(jtty_rx_t *rx, const jtty_decode_t *d, uint64_t start)
{
    slot_t *s = NULL;
    int     missed = 0;

    if (seen(rx, start, d->freq) || ghost(rx, start, d)) {
        return;
    }
    remember(rx, start, d);

    /* the message this frame continues: same frequency, and it starts
     * where that message's next frame (or the one after) had to */
    for (int i = 0; i < MAX_MSGS && s == NULL; i++) {
        slot_t *c = &rx->slots[i];

        if (!c->open || fabsf(c->m.freq - d->freq) > JTTY_BAUD / 2) {
            continue;
        }
        for (int k = 0; k <= 1; k++) {
            uint64_t want = c->next + (uint64_t)k * JTTY_FRAME_SAMPLES;
            uint64_t diff = want > start ? want - start : start - want;

            if (diff < JTTY_NSPS / 2) {
                s = c;
                missed = k;
                break;
            }
        }
    }

    if (s == NULL) {
        for (int i = 0; i < MAX_MSGS; i++) {
            if (!rx->slots[i].open) {
                s = &rx->slots[i];
                break;
            }
        }
        if (s == NULL) {
            /* all busy: the oldest message is closed */
            s = &rx->slots[0];
            for (int i = 1; i < MAX_MSGS; i++) {
                if (rx->slots[i].m.start < s->m.start) {
                    s = &rx->slots[i];
                }
            }
            close_slot(rx, s);
        }
        memset(s, 0, sizeof(*s));
        s->open = true;
        s->m.id = ++rx->next_id;
        s->m.start = start;
        s->m.freq = d->freq;
    }

    if (missed) {
        size_t len = strlen(s->m.text);

        /* the missing frame's text is unknown; mark the hole */
        snprintf(s->m.text + len, sizeof(s->m.text) - len, "... ");
        s->m.gap = true;
    }
    jtty_source_append(s->m.text, &d->atom);
    s->m.snr = d->snr;
    s->m.frames++;
    s->next = start + JTTY_FRAME_SAMPLES;
    s->tried = false;

    if (d->atom.eom) {
        close_slot(rx, s);
    } else {
        emit(rx, s);
    }
}

/* ---- One pass ---------------------------------------------------------- */

static int cmp_start(const void *a, const void *b)
{
    int d = ((const jtty_decode_t *)a)->start - ((const jtty_decode_t *)b)->start;

    return (d > 0) - (d < 0);
}

static void run_pass(jtty_rx_t *rx)
{
    jtty_search_t s;
    jtty_decode_t dec[MAX_DEC];
    int           n;
    int           ndec;
    uint64_t      abs0;
    uint64_t      end;
    float         fmin;
    float         fmax;

    pthread_mutex_lock(&rx->lock);
    if (rx->reset) {
        rx->reset = false;
        rx->searched = 0;
        rx->nrecent = 0;
        for (int i = 0; i < MAX_MSGS; i++) {
            rx->slots[i].open = false;
        }
    }
    n = rx->len;
    abs0 = rx->abs0;
    fmin = rx->fmin;
    fmax = rx->fmax;
    memcpy(rx->work, rx->buf, sizeof(float) * (size_t)n);
    pthread_mutex_unlock(&rx->lock);

    end = abs0 + (uint64_t)n;
    if (n < JTTY_FRAME_SAMPLES + TAIL) {
        return;
    }

    /* Frames whose last sample (plus room to refine) arrived since the
     * previous pass; a little overlap, duplicates are dropped. */
    jtty_search_defaults(&s, n);
    s.fmin = fmin;
    s.fmax = fmax;
    s.start_max = n - JTTY_FRAME_SAMPLES - TAIL;
    s.start_min = (rx->searched > abs0 + 2 * (JTTY_NSPS / 4))
                      ? (int)(rx->searched - abs0) - 2 * (JTTY_NSPS / 4)
                      : 0;
    if (s.start_min > s.start_max) {
        return;
    }
    s.max_candidates = 20 + (int)(20.0f * (float)(s.start_max - s.start_min) / JTTY_RATE);
    rx->searched = abs0 + (uint64_t)s.start_max + 1;

    ndec = jtty_demod_search(rx->work, n, &s, dec, MAX_DEC);

    /* The next frame of every open message, at its exact place, if the
     * search did not bring it: weak frames are found this way. */
    for (int i = 0; i < MAX_MSGS && ndec < MAX_DEC; i++) {
        slot_t       *sl = &rx->slots[i];
        jtty_decode_t d;
        bool          found = false;

        if (!sl->open || sl->tried || sl->next < abs0 ||
            sl->next + JTTY_FRAME_SAMPLES + TAIL > end) {
            continue;
        }
        sl->tried = true;
        for (int k = 0; k < ndec; k++) {
            uint64_t st = abs0 + (uint64_t)dec[k].start;
            uint64_t diff = st > sl->next ? st - sl->next : sl->next - st;

            if (diff < JTTY_NSPS / 2 && fabsf(dec[k].freq - sl->m.freq) < JTTY_BAUD / 2) {
                found = true;
                break;
            }
        }
        if (!found && jtty_demod_try(rx->work, n, (int)(sl->next - abs0), sl->m.freq, &d)) {
            dec[ndec++] = d;
        }
    }

    qsort(dec, (size_t)ndec, sizeof(dec[0]), cmp_start);
    for (int i = 0; i < ndec; i++) {
        take_frame(rx, &dec[i], abs0 + (uint64_t)dec[i].start);
    }

    /* A message whose next two frames would have arrived by now (and
     * been searched for) has ended without its EOM frame: the sender
     * stopped, or we lost it. */
    for (int i = 0; i < MAX_MSGS; i++) {
        slot_t *sl = &rx->slots[i];

        if (sl->open && sl->next + 2 * JTTY_FRAME_SAMPLES + TAIL + PASS_SAMPLES < end) {
            sl->m.gap = true;
            close_slot(rx, sl);
        }
    }
}

/* ---- Waterfall --------------------------------------------------------- */

static void fft_pow2(float complex *a, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;

        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float complex t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float complex wl = cexpf(-2.0f * (float)M_PI * I / (float)len);

        for (int i = 0; i < n; i += len) {
            float complex w = 1.0f;

            for (int k = 0; k < len / 2; k++) {
                float complex u = a[i + k];
                float complex v = a[i + k + len / 2] * w;

                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

static int cmp_f(const void *a, const void *b)
{
    float d = *(const float *)a - *(const float *)b;

    return (d > 0) - (d < 0);
}

/* One waterfall line from the newest PSD_N samples. */
static void psd_line(jtty_rx_t *rx)
{
    static float complex a[PSD_N];
    static float         line[PSD_MAX_BINS];
    static float         sorted[PSD_MAX_BINS];
    static float         win[PSD_N];
    static bool          win_ready;
    int                  nb;
    float                lo;
    float                hi;

    pthread_mutex_lock(&rx->lock);
    if (rx->psd_cb == NULL || rx->len < PSD_N) {
        pthread_mutex_unlock(&rx->lock);
        return;
    }
    if (!win_ready) {
        for (int i = 0; i < PSD_N; i++) {
            win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (PSD_N - 1));
        }
        win_ready = true;
    }
    for (int i = 0; i < PSD_N; i++) {
        a[i] = rx->buf[rx->len - PSD_N + i] * win[i];
    }
    nb = rx->psd_bins;
    lo = rx->psd_lo;
    hi = rx->psd_hi;
    pthread_mutex_unlock(&rx->lock);

    fft_pow2(a, PSD_N);

    /* each output bin: the strongest FFT bin it covers */
    for (int b = 0; b < nb; b++) {
        float f0 = lo + (hi - lo) * b / nb;
        float f1 = lo + (hi - lo) * (b + 1) / nb;
        int   k0 = (int)(f0 * PSD_N / JTTY_RATE);
        int   k1 = (int)(f1 * PSD_N / JTTY_RATE);
        float p = 0.0f;

        if (k1 < k0) {
            k1 = k0;
        }
        for (int k = k0; k <= k1 && k < PSD_N / 2; k++) {
            float v = crealf(a[k]) * crealf(a[k]) + cimagf(a[k]) * cimagf(a[k]);

            if (v > p) {
                p = v;
            }
        }
        line[b] = 10.0f * log10f(p + 1e-20f);
    }
    memcpy(sorted, line, sizeof(float) * (size_t)nb);
    qsort(sorted, (size_t)nb, sizeof(float), cmp_f);
    for (int b = 0; b < nb; b++) {
        line[b] -= sorted[nb / 2];
    }
    rx->psd_cb(line, nb, rx->psd_ctx);
}

/* ---- Public ------------------------------------------------------------ */

jtty_rx_t *jtty_rx_create(jtty_rx_cb_t cb, void *ctx)
{
    jtty_rx_t *rx = calloc(1, sizeof(*rx));

    if (rx == NULL) {
        return NULL;
    }
    rx->buf = malloc(sizeof(float) * BUF_SAMPLES);
    rx->work = malloc(sizeof(float) * BUF_SAMPLES);
    if (rx->buf == NULL || rx->work == NULL) {
        free(rx->buf);
        free(rx->work);
        free(rx);
        return NULL;
    }
    rx->cb = cb;
    rx->ctx = ctx;
    rx->fmin = 200.0f;
    rx->fmax = 2800.0f;
    rx->next_pass = JTTY_FRAME_SAMPLES + TAIL;
    pthread_mutex_init(&rx->lock, NULL);
    pthread_cond_init(&rx->cv, NULL);
    atomic_store(&rx->stop_req, false);
    return rx;
}

void jtty_rx_destroy(jtty_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    jtty_rx_stop(rx);
    pthread_cond_destroy(&rx->cv);
    pthread_mutex_destroy(&rx->lock);
    free(rx->buf);
    free(rx->work);
    free(rx);
}

void jtty_rx_set_band(jtty_rx_t *rx, float fmin, float fmax)
{
    pthread_mutex_lock(&rx->lock);
    rx->fmin = fmin;
    rx->fmax = fmax;
    pthread_mutex_unlock(&rx->lock);
}

void jtty_rx_set_psd(jtty_rx_t *rx, jtty_psd_cb_t cb, void *ctx,
                     float f_lo, float f_hi, int nbins)
{
    pthread_mutex_lock(&rx->lock);
    rx->psd_cb = cb;
    rx->psd_ctx = ctx;
    rx->psd_lo = f_lo;
    rx->psd_hi = f_hi;
    rx->psd_bins = nbins < 1 ? 1 : (nbins > PSD_MAX_BINS ? PSD_MAX_BINS : nbins);
    rx->next_psd = rx->fed + PSD_HOP;
    pthread_mutex_unlock(&rx->lock);
}

void jtty_rx_reset(jtty_rx_t *rx)
{
    pthread_mutex_lock(&rx->lock);
    rx->abs0 += (uint64_t)rx->len;
    rx->len = 0;
    rx->next_pass = rx->fed + JTTY_FRAME_SAMPLES + TAIL;
    rx->reset = true;
    pthread_mutex_unlock(&rx->lock);
}

void jtty_rx_feed(jtty_rx_t *rx, const float *x, int n)
{
    pthread_mutex_lock(&rx->lock);
    if (n >= BUF_SAMPLES) {
        /* more than the buffer holds: keep the newest */
        memcpy(rx->buf, x + n - BUF_SAMPLES, sizeof(float) * BUF_SAMPLES);
        rx->len = BUF_SAMPLES;
    } else {
        if (rx->len + n > BUF_SAMPLES) {
            int drop = rx->len + n - BUF_SAMPLES;

            memmove(rx->buf, rx->buf + drop, sizeof(float) * (size_t)(rx->len - drop));
            rx->len -= drop;
        }
        memcpy(rx->buf + rx->len, x, sizeof(float) * (size_t)n);
        rx->len += n;
    }
    rx->fed += (uint64_t)n;
    rx->abs0 = rx->fed - (uint64_t)rx->len;
    if (rx->fed >= rx->next_pass || (rx->psd_cb != NULL && rx->fed >= rx->next_psd)) {
        pthread_cond_signal(&rx->cv);
    }
    pthread_mutex_unlock(&rx->lock);
}

uint64_t jtty_rx_samples(jtty_rx_t *rx)
{
    uint64_t v;

    pthread_mutex_lock(&rx->lock);
    v = rx->fed;
    pthread_mutex_unlock(&rx->lock);
    return v;
}

int jtty_rx_poll(jtty_rx_t *rx)
{
    int passes = 0;

    for (;;) {
        bool due;
        bool line;

        /* waterfall: one line if due; when late, skip to the newest */
        pthread_mutex_lock(&rx->lock);
        line = rx->psd_cb != NULL && rx->fed >= rx->next_psd;
        if (line) {
            rx->next_psd += PSD_HOP;
            if (rx->next_psd <= rx->fed) {
                rx->next_psd = rx->fed + PSD_HOP;
            }
        }
        pthread_mutex_unlock(&rx->lock);
        if (line) {
            psd_line(rx);
        }

        pthread_mutex_lock(&rx->lock);
        due = rx->fed >= rx->next_pass;
        if (due) {
            /* one pass covers everything up to now; schedule the next */
            rx->next_pass = rx->fed + PASS_SAMPLES;
        }
        pthread_mutex_unlock(&rx->lock);
        if (!due) {
            return passes;
        }
        run_pass(rx);
        passes++;
    }
}

static void *worker_main(void *arg)
{
    jtty_rx_t *rx = arg;

    while (!atomic_load(&rx->stop_req)) {
        struct timespec ts;

        pthread_mutex_lock(&rx->lock);
        while (!atomic_load(&rx->stop_req) && rx->fed < rx->next_pass &&
               !(rx->psd_cb != NULL && rx->fed >= rx->next_psd)) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&rx->cv, &rx->lock, &ts);
        }
        pthread_mutex_unlock(&rx->lock);
        if (atomic_load(&rx->stop_req)) {
            break;
        }
        jtty_rx_poll(rx);
    }
    return NULL;
}

int jtty_rx_start(jtty_rx_t *rx)
{
    if (rx->started) {
        return 0;
    }
    atomic_store(&rx->stop_req, false);
    if (pthread_create(&rx->thread, NULL, worker_main, rx) != 0) {
        return -1;
    }
    rx->started = true;
    return 0;
}

void jtty_rx_stop(jtty_rx_t *rx)
{
    if (!rx->started) {
        return;
    }
    pthread_mutex_lock(&rx->lock);
    atomic_store(&rx->stop_req, true);
    pthread_cond_signal(&rx->cv);
    pthread_mutex_unlock(&rx->lock);
    pthread_join(rx->thread, NULL);
    rx->started = false;
}
