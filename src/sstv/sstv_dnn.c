/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */

/* SSTV ML denoiser, inference - see sstv_dnn.h */

#include "sstv_dnn.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LAYERS   64
#define MAX_CH       256
#define MAX_THREADS  8

typedef struct {
    int    cin, cout, k, act;
    float *w;           /* [cout][cin][3][3] */
    float *b;           /* [cout] */
} layer_t;

struct sstv_dnn {
    int     n;
    int     residual;
    int     max_ch;
    layer_t l[MAX_LAYERS];
};

/* ---- model file ------------------------------------------------------------------ */

static int rd_u32(FILE *f, uint32_t *v)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

static int rd_f32(FILE *f, float *v, size_t n)
{
    /* the targets (ARM, x86, Apple) are all little endian */
    return fread(v, sizeof(float), n, f) == n ? 0 : -1;
}

void sstv_dnn_free(sstv_dnn_t *m)
{
    if (!m) return;
    for (int i = 0; i < m->n; i++) {
        free(m->l[i].w);
        free(m->l[i].b);
    }
    free(m);
}

sstv_dnn_t *sstv_dnn_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sstv_dnn: cannot open %s\n", path);
        return NULL;
    }
    char magic[8];
    uint32_t n, flags;
    sstv_dnn_t *m = calloc(1, sizeof(*m));
    if (!m || fread(magic, 1, 8, f) != 8 || memcmp(magic, "SSTVDNN1", 8) != 0 || rd_u32(f, &n) || rd_u32(f, &flags) ||
        n < 1 || n > MAX_LAYERS) {
        fprintf(stderr, "sstv_dnn: %s is not a model file\n", path);
        goto fail;
    }
    m->residual = flags & 1;
    int prev = 3;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cin, cout, k, act;
        if (rd_u32(f, &cin) || rd_u32(f, &cout) || rd_u32(f, &k) || rd_u32(f, &act)) goto bad;
        if ((int)cin != prev || cout < 1 || cout > MAX_CH || k != 3 || act > 1) goto bad;
        if (i == n - 1 && cout != 3) goto bad;
        layer_t *L = &m->l[i];
        L->cin = (int)cin;
        L->cout = (int)cout;
        L->k = 3;
        L->act = (int)act;
        L->w = malloc(sizeof(float) * cout * cin * 9);
        L->b = malloc(sizeof(float) * cout);
        m->n = (int)i + 1;
        if (!L->w || !L->b || rd_f32(f, L->w, (size_t)cout * cin * 9) || rd_f32(f, L->b, cout)) goto bad;
        if ((int)cout > m->max_ch) m->max_ch = (int)cout;
        prev = (int)cout;
    }
    if (m->max_ch < 3) m->max_ch = 3;
    fclose(f);
    return m;
bad:
    fprintf(stderr, "sstv_dnn: %s: bad layer data\n", path);
fail:
    fclose(f);
    sstv_dnn_free(m);
    return NULL;
}

double sstv_dnn_macs(const sstv_dnn_t *m, int w, int h)
{
    double s = 0;
    for (int i = 0; i < m->n; i++) s += (double)m->l[i].cin * m->l[i].cout * 9;
    return s * w * h;
}

/* ---- inference ------------------------------------------------------------------- */

/* Activations: one plane per channel, (w+2) x (h+2) with a border of zeros
 * (the zero padding of the convolution), so the inner loops have no tests.
 *
 * Speed: the work is done in strips of STRIP_H lines, all the channels of a
 * strip before the next one, so that what is read stays in the cache; four
 * output channels are computed together, so that each input value is read
 * once for four; on ARM the inner loop uses NEON, four pixels at a time. */

#define OB       4          /* output channels computed together */
#define STRIP_H  4          /* lines per strip */

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAVE_NEON 1
#else
#define HAVE_NEON 0
#endif

int sstv_dnn_generic = 0;   /* test programs: 1 = never NEON (to compare) */
volatile int sstv_dnn_progress = 0;

int sstv_dnn_has_neon(void) { return HAVE_NEON; }

/* r[k][x] += 3x3 window of rows a, b, d (padded, starting at x) * q[k] */
static void row4_generic(float *const r[OB], const float *a, const float *b, const float *d,
                         const float q[OB][9], int w)
{
    for (int x = 0; x < w; x++) {
        const float v0 = a[x], v1 = a[x + 1], v2 = a[x + 2];
        const float v3 = b[x], v4 = b[x + 1], v5 = b[x + 2];
        const float v6 = d[x], v7 = d[x + 1], v8 = d[x + 2];
        for (int k = 0; k < OB; k++)
            r[k][x] += q[k][0] * v0 + q[k][1] * v1 + q[k][2] * v2 + q[k][3] * v3 + q[k][4] * v4 +
                       q[k][5] * v5 + q[k][6] * v6 + q[k][7] * v7 + q[k][8] * v8;
    }
}

#if HAVE_NEON
/* The 9 taps one after the other, each into the 4 outputs: 4 independent
 * chains of multiply-adds in flight (the Cortex-A7 is in order, a single
 * chain would wait for every result).  Weights of tap t for the 4 outputs
 * in one register: 9 + 4 accumulators + 1 input = 14 of the 16 q registers. */
static void row4_neon(float *const r[OB], const float *a, const float *b, const float *d,
                      const float q[OB][9], int w)
{
    float32x4_t wq[9];
    for (int t = 0; t < 9; t++) {
        float tmp[4] = { q[0][t], q[1][t], q[2][t], q[3][t] };
        wq[t] = vld1q_f32(tmp);
    }
    float *r0 = r[0], *r1 = r[1], *r2 = r[2], *r3 = r[3];
    int x = 0;
#define TAP(src, t)                                                         \
    do {                                                                    \
        const float32x4_t v = vld1q_f32(src);                               \
        const float32x2_t lo = vget_low_f32(wq[t]), hi = vget_high_f32(wq[t]); \
        acc0 = vmlaq_lane_f32(acc0, v, lo, 0);                              \
        acc1 = vmlaq_lane_f32(acc1, v, lo, 1);                              \
        acc2 = vmlaq_lane_f32(acc2, v, hi, 0);                              \
        acc3 = vmlaq_lane_f32(acc3, v, hi, 1);                              \
    } while (0)
    for (; x + 4 <= w; x += 4) {
        float32x4_t acc0 = vld1q_f32(r0 + x), acc1 = vld1q_f32(r1 + x);
        float32x4_t acc2 = vld1q_f32(r2 + x), acc3 = vld1q_f32(r3 + x);
        TAP(a + x, 0);
        TAP(a + x + 1, 1);
        TAP(a + x + 2, 2);
        TAP(b + x, 3);
        TAP(b + x + 1, 4);
        TAP(b + x + 2, 5);
        TAP(d + x, 6);
        TAP(d + x + 1, 7);
        TAP(d + x + 2, 8);
        vst1q_f32(r0 + x, acc0);
        vst1q_f32(r1 + x, acc1);
        vst1q_f32(r2 + x, acc2);
        vst1q_f32(r3 + x, acc3);
    }
#undef TAP
    if (x < w) {
        float *rt[OB];
        for (int k = 0; k < OB; k++) rt[k] = r[k] + x;
        row4_generic(rt, a + x, b + x, d + x, q, w - x);
    }
}
#endif

/* Lines y0..y1-1 of all the output channels of layer L */
static void conv_strip(const layer_t *L, const float *in, float *out, int w, int h, int y0, int y1)
{
    const int pw = w + 2;
    const size_t plane = (size_t)pw * (h + 2);

    for (int o0 = 0; o0 < L->cout; o0 += OB) {
        const int nb = L->cout - o0 < OB ? L->cout - o0 : OB;
        float *op[OB];
        for (int k = 0; k < OB; k++) op[k] = out + (size_t)(o0 + (k < nb ? k : 0)) * plane;

        /* bias */
        for (int k = 0; k < nb; k++)
            for (int y = y0; y < y1; y++) {
                float *r = op[k] + (size_t)(y + 1) * pw + 1;
                const float bias = L->b[o0 + k];
                for (int x = 0; x < w; x++) r[x] = bias;
            }

        /* a block of fewer than OB channels: the missing ones get weights 0
         * and write into scratch rows, never into a real channel */
        float *scratch = NULL;
        if (nb < OB) {
            scratch = calloc((size_t)(OB - nb) * pw, sizeof(float));
            if (!scratch) return;
        }

        for (int c = 0; c < L->cin; c++) {
            const float *ip = in + (size_t)c * plane;
            float q[OB][9];
            for (int k = 0; k < OB; k++)
                for (int t = 0; t < 9; t++)
                    q[k][t] = k < nb ? L->w[((size_t)(o0 + k) * L->cin + c) * 9 + t] : 0.0f;
            for (int y = y0; y < y1; y++) {
                const float *a = ip + (size_t)y * pw;
                const float *b = a + pw;
                const float *d = b + pw;
                float *r[OB];
                for (int k = 0; k < OB; k++)
                    r[k] = k < nb ? op[k] + (size_t)(y + 1) * pw + 1 : scratch + (size_t)(k - nb) * pw;
#if HAVE_NEON
                if (!sstv_dnn_generic) {
                    row4_neon(r, a, b, d, q, w);
                    continue;
                }
#endif
                row4_generic(r, a, b, d, q, w);
            }
        }
        free(scratch);

        if (L->act == 1)
            for (int k = 0; k < nb; k++)
                for (int y = y0; y < y1; y++) {
                    float *r = op[k] + (size_t)(y + 1) * pw + 1;
                    for (int x = 0; x < w; x++)
                        if (r[x] < 0.0f) r[x] = 0.0f;
                }
    }
}

typedef struct {
    const layer_t *L;
    const float   *in;
    float         *out;
    int            w, h;
    int           *next;            /* next strip to do, shared */
    int           *done;            /* strips done in this layer, shared */
    int            layer, layers;   /* for sstv_dnn_progress */
} job_t;

static void *conv_job(void *arg)
{
    job_t *j = arg;
    for (;;) {
        int s = __atomic_fetch_add(j->next, 1, __ATOMIC_RELAXED);
        int y0 = s * STRIP_H;
        if (y0 >= j->h) break;
        int y1 = y0 + STRIP_H > j->h ? j->h : y0 + STRIP_H;
        conv_strip(j->L, j->in, j->out, j->w, j->h, y0, y1);
        int strips = (j->h + STRIP_H - 1) / STRIP_H;
        int d = __atomic_add_fetch(j->done, 1, __ATOMIC_RELAXED);
        sstv_dnn_progress = (int)((j->layer * (long)strips + d) * 1000L / ((long)j->layers * strips));
    }
    return NULL;
}

static void conv_layer(const layer_t *L, const float *in, float *out, int w, int h, int threads,
                       int layer, int layers)
{
    pthread_t th[MAX_THREADS];
    int started[MAX_THREADS] = { 0 };
    int next = 0, done = 0;
    job_t jb = { L, in, out, w, h, &next, &done, layer, layers };
    for (int t = 1; t < threads; t++)
        started[t] = pthread_create(&th[t], NULL, conv_job, &jb) == 0;
    conv_job(&jb);
    for (int t = 1; t < threads; t++)
        if (started[t]) pthread_join(th[t], NULL);
}

int sstv_dnn_run(const sstv_dnn_t *m, uint8_t *rgb, int w, int h, int threads)
{
    if (!m || !rgb || w < 1 || h < 1) return -1;
    if (threads < 1) threads = 1;
    if (threads > MAX_THREADS) threads = MAX_THREADS;

    const int pw = w + 2;
    const size_t plane = (size_t)pw * (h + 2);
    /* +4 floats: NEON reads up to 2 values past the end of the last row */
    float *a = calloc(plane * m->max_ch + 4, sizeof(float));
    float *b = calloc(plane * m->max_ch + 4, sizeof(float));
    if (!a || !b) {
        free(a);
        free(b);
        return -2;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
                a[(size_t)c * plane + (size_t)(y + 1) * pw + x + 1] = rgb[((size_t)y * w + x) * 3 + c] * (1.0f / 255.0f);

    sstv_dnn_progress = 0;
    float *in = a, *out = b;
    for (int i = 0; i < m->n; i++) {
        conv_layer(&m->l[i], in, out, w, h, threads, i, m->n);
        float *t = in;
        in = out;
        out = t;
    }
    /* in: the last output (3 planes) */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                size_t o = ((size_t)y * w + x) * 3 + c;
                float v = in[(size_t)c * plane + (size_t)(y + 1) * pw + x + 1];
                if (m->residual) v = rgb[o] * (1.0f / 255.0f) - v;
                v = v * 255.0f + 0.5f;
                rgb[o] = v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
            }
    free(a);
    free(b);
    return 0;
}
