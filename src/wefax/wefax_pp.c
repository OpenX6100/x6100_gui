/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */

/* WEFAX postprocessing network, see wefax_pp.h */

#include "wefax_pp.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONVS        15
#define MAX_THREADS   8
#define MAX_BASE     64

typedef struct {
    int    cin, cout, k;
    float *w;           /* [cout][cin][k][k] */
    float *b;           /* [cout] */
} layer_t;

struct wefax_pp {
    int     base;
    layer_t l[CONVS];
};

volatile int wefax_pp_progress = 0;
volatile int wefax_pp_cancel = 0;
int wefax_pp_generic = 0;

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

void wefax_pp_free(wefax_pp_t *m)
{
    if (!m) return;
    for (int i = 0; i < CONVS; i++) {
        free(m->l[i].w);
        free(m->l[i].b);
    }
    free(m);
}

/* the shape of convolution i for a given base */
static void shape(int base, int i, int *cin, int *cout, int *k)
{
    const int c1 = base, c2 = 2 * base, c3 = 4 * base, c4 = 4 * base;
    /* blocks: e1 (1->c1), e2 (c1->c2), e3 (c2->c3), mid (c3->c4),
     * d3 (c4+c3->c3), d2 (c3+c2->c2), d1 (c2+c1->c1), then out (c1->1, 1x1) */
    const int bin[7]  = { 1, c1, c2, c3, c4 + c3, c3 + c2, c2 + c1 };
    const int bout[7] = { c1, c2, c3, c4, c3, c2, c1 };
    *k = 3;
    if (i == CONVS - 1) {
        *cin = c1;
        *cout = 1;
        *k = 1;
        return;
    }
    const int b = i / 2;
    *cin = (i % 2 == 0) ? bin[b] : bout[b];
    *cout = bout[b];
}

wefax_pp_t *wefax_pp_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "wefax_pp: cannot open %s\n", path);
        return NULL;
    }
    char magic[8];
    uint32_t base;
    wefax_pp_t *m = calloc(1, sizeof(*m));
    if (!m || fread(magic, 1, 8, f) != 8 || memcmp(magic, "WFXUNET1", 8) != 0 || rd_u32(f, &base) ||
        base < 4 || base > MAX_BASE || base % 4 != 0) {
        fprintf(stderr, "wefax_pp: %s is not a model file\n", path);
        goto fail;
    }
    m->base = (int)base;
    for (int i = 0; i < CONVS; i++) {
        uint32_t cin, cout, k;
        int ci, co, kk;
        shape(m->base, i, &ci, &co, &kk);
        if (rd_u32(f, &cin) || rd_u32(f, &cout) || rd_u32(f, &k)) goto bad;
        if ((int)cin != ci || (int)cout != co || (int)k != kk) goto bad;
        layer_t *L = &m->l[i];
        L->cin = ci;
        L->cout = co;
        L->k = kk;
        L->w = malloc(sizeof(float) * co * ci * kk * kk);
        L->b = malloc(sizeof(float) * co);
        if (!L->w || !L->b || rd_f32(f, L->w, (size_t)co * ci * kk * kk) || rd_f32(f, L->b, co)) goto bad;
    }
    fclose(f);
    return m;
bad:
    fprintf(stderr, "wefax_pp: %s: bad layer data\n", path);
fail:
    fclose(f);
    wefax_pp_free(m);
    return NULL;
}

int wefax_pp_base(const wefax_pp_t *m)
{
    return m ? m->base : 0;
}

/* multiply-adds of one tile of w x h (multiples of 8) */
static double tile_macs(const wefax_pp_t *m, int w, int h)
{
    double s = 0;
    for (int i = 0; i < CONVS; i++) {
        const int level = i < 8 ? i / 2 : (i < 14 ? 3 - (i - 6) / 2 : 0);
        const double px = (double)(w >> level) * (h >> level);
        s += px * m->l[i].cin * m->l[i].cout * m->l[i].k * m->l[i].k;
    }
    return s;
}

/* ---- tensors --------------------------------------------------------------------- */

/* One plane per channel, (w+2) x (h+2) with a border of zeros (the zero
 * padding of the convolutions), so the inner loops have no tests.  +4
 * floats: NEON reads up to 2 values past the end of the last row. */
typedef struct {
    int    c, w, h;
    float *d;
} tensor_t;

static int t_new(tensor_t *t, int c, int w, int h)
{
    t->c = c;
    t->w = w;
    t->h = h;
    t->d = calloc((size_t)(w + 2) * (h + 2) * c + 4, sizeof(float));
    return t->d ? 0 : -1;
}

static void t_free(tensor_t *t)
{
    free(t->d);
    t->d = NULL;
}

static inline size_t t_plane(const tensor_t *t)
{
    return (size_t)(t->w + 2) * (t->h + 2);
}

/* row y (0-based) of channel c, at column 0 (inside the border) */
static inline float *t_row(const tensor_t *t, int c, int y)
{
    return t->d + (size_t)c * t_plane(t) + (size_t)(y + 1) * (t->w + 2) + 1;
}

/* ---- 3x3 convolution ------------------------------------------------------------- */

/* Speed (as in sstv_dnn.c): the work is done in strips of STRIP_H lines, all
 * the channels of a strip before the next one, so that what is read stays in
 * the cache; four output channels are computed together, so that each input
 * value is read once for four; on ARM the inner loop uses NEON, four pixels at
 * a time. */

#define OB       4          /* output channels computed together */
#define STRIP_H  4          /* lines per strip */

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAVE_NEON 1
#else
#define HAVE_NEON 0
#endif

int wefax_pp_has_neon(void) { return HAVE_NEON; }

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
 * chains of multiply-adds in flight (the Cortex-A7 is in order). */
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

/* The input of a convolution: one tensor, or two concatenated (decoder) */
typedef struct {
    const tensor_t *t[2];
    int             n;
} input_t;

static const float *in_plane_row(const input_t *in, int c, int y)
{
    if (c >= in->t[0]->c) {
        c -= in->t[0]->c;
        return t_row(in->t[1], c, y);
    }
    return t_row(in->t[0], c, y);
}

/* Lines y0..y1-1 of all the output channels of layer L, then ReLU */
static void conv_strip(const layer_t *L, const input_t *in, const tensor_t *out, int y0, int y1,
                       float *scratch)
{
    const int w = out->w;
    const int pw = w + 2;

    for (int o0 = 0; o0 < L->cout; o0 += OB) {
        const int nb = L->cout - o0 < OB ? L->cout - o0 : OB;

        for (int k = 0; k < nb; k++)
            for (int y = y0; y < y1; y++) {
                float *r = t_row(out, o0 + k, y);
                const float bias = L->b[o0 + k];
                for (int x = 0; x < w; x++) r[x] = bias;
            }

        for (int c = 0; c < L->cin; c++) {
            float q[OB][9];
            for (int k = 0; k < OB; k++)
                for (int t = 0; t < 9; t++)
                    q[k][t] = k < nb ? L->w[((size_t)(o0 + k) * L->cin + c) * 9 + t] : 0.0f;
            for (int y = y0; y < y1; y++) {
                /* rows y-1, y, y+1 of the padded plane, from column -1 */
                const float *b = in_plane_row(in, c, y) - 1;
                const float *a = b - pw;
                const float *d = b + pw;
                float *r[OB];
                for (int k = 0; k < OB; k++)
                    r[k] = k < nb ? t_row(out, o0 + k, y) : scratch + (size_t)(k - nb) * pw;
#if HAVE_NEON
                if (!wefax_pp_generic) {
                    row4_neon(r, a, b, d, q, w);
                    continue;
                }
#endif
                row4_generic(r, a, b, d, q, w);
            }
        }

        for (int k = 0; k < nb; k++)
            for (int y = y0; y < y1; y++) {
                float *r = t_row(out, o0 + k, y);
                for (int x = 0; x < w; x++)
                    if (r[x] < 0.0f) r[x] = 0.0f;
            }
    }
}

typedef struct {
    const layer_t  *L;
    const input_t  *in;
    const tensor_t *out;
    int            *next;           /* next strip to do, shared */
} job_t;

static void *conv_job(void *arg)
{
    job_t *j = arg;
    float *scratch = calloc((size_t)OB * (j->out->w + 2) + 4, sizeof(float));
    if (!scratch) return NULL;
    for (;;) {
        if (wefax_pp_cancel) break;
        int s = __atomic_fetch_add(j->next, 1, __ATOMIC_RELAXED);
        int y0 = s * STRIP_H;
        if (y0 >= j->out->h) break;
        int y1 = y0 + STRIP_H > j->out->h ? j->out->h : y0 + STRIP_H;
        conv_strip(j->L, j->in, j->out, y0, y1, scratch);
    }
    free(scratch);
    return NULL;
}

static void conv_layer(const layer_t *L, const input_t *in, const tensor_t *out, int threads)
{
    pthread_t th[MAX_THREADS];
    int started[MAX_THREADS] = { 0 };
    int next = 0;
    job_t jb = { L, in, out, &next };
    for (int t = 1; t < threads; t++)
        started[t] = pthread_create(&th[t], NULL, conv_job, &jb) == 0;
    conv_job(&jb);
    for (int t = 1; t < threads; t++)
        if (started[t]) pthread_join(th[t], NULL);
}

/* ---- the network on one tile ----------------------------------------------------- */

/* two convolutions with ReLU: in -> out (out allocated here) */
static int block(const wefax_pp_t *m, int i, const tensor_t *a, const tensor_t *b, tensor_t *out,
                 int threads)
{
    const layer_t *L1 = &m->l[i], *L2 = &m->l[i + 1];
    tensor_t mid;
    input_t in = { { a, b }, b ? 2 : 1 };
    if (t_new(&mid, L1->cout, a->w, a->h)) return -2;
    conv_layer(L1, &in, &mid, threads);
    if (wefax_pp_cancel) {
        t_free(&mid);
        return -3;
    }
    if (t_new(out, L2->cout, a->w, a->h)) {
        t_free(&mid);
        return -2;
    }
    input_t in2 = { { &mid, NULL }, 1 };
    conv_layer(L2, &in2, out, threads);
    t_free(&mid);
    return wefax_pp_cancel ? -3 : 0;
}

static int pool(const tensor_t *in, tensor_t *out)
{
    if (t_new(out, in->c, in->w / 2, in->h / 2)) return -2;
    for (int c = 0; c < in->c; c++)
        for (int y = 0; y < out->h; y++) {
            const float *a = t_row(in, c, 2 * y), *b = t_row(in, c, 2 * y + 1);
            float *r = t_row(out, c, y);
            for (int x = 0; x < out->w; x++) {
                float v = a[2 * x];
                if (a[2 * x + 1] > v) v = a[2 * x + 1];
                if (b[2 * x] > v) v = b[2 * x];
                if (b[2 * x + 1] > v) v = b[2 * x + 1];
                r[x] = v;
            }
        }
    return 0;
}

static int upsample(const tensor_t *in, tensor_t *out)
{
    if (t_new(out, in->c, in->w * 2, in->h * 2)) return -2;
    for (int c = 0; c < in->c; c++)
        for (int y = 0; y < out->h; y++) {
            const float *a = t_row(in, c, y / 2);
            float *r = t_row(out, c, y);
            for (int x = 0; x < out->w; x++) r[x] = a[x / 2];
        }
    return 0;
}

#define TRY(e)                  \
    do {                        \
        rc = (e);               \
        if (rc) goto done;      \
    } while (0)

/* x: the tile (1 channel, 0 white .. 1 black); p: probability of black,
 * w x h floats */
static int net(const wefax_pp_t *m, const tensor_t *x, float *p, int threads)
{
    tensor_t e1 = { 0 }, e2 = { 0 }, e3 = { 0 }, mi = { 0 }, t = { 0 }, u = { 0 }, d = { 0 };
    int rc;

    TRY(block(m, 0, x, NULL, &e1, threads));
    TRY(pool(&e1, &t));
    TRY(block(m, 2, &t, NULL, &e2, threads));
    t_free(&t);
    TRY(pool(&e2, &t));
    TRY(block(m, 4, &t, NULL, &e3, threads));
    t_free(&t);
    TRY(pool(&e3, &t));
    TRY(block(m, 6, &t, NULL, &mi, threads));
    t_free(&t);

    TRY(upsample(&mi, &u));
    t_free(&mi);
    TRY(block(m, 8, &u, &e3, &d, threads));
    t_free(&u);
    t_free(&e3);

    TRY(upsample(&d, &u));
    t_free(&d);
    TRY(block(m, 10, &u, &e2, &d, threads));
    t_free(&u);
    t_free(&e2);

    TRY(upsample(&d, &u));
    t_free(&d);
    TRY(block(m, 12, &u, &e1, &d, threads));
    t_free(&u);
    t_free(&e1);

    /* out: 1x1 convolution, then the sigmoid */
    {
        const layer_t *L = &m->l[CONVS - 1];
        for (int y = 0; y < d.h; y++)
            for (int xx = 0; xx < d.w; xx++) {
                float s = L->b[0];
                for (int c = 0; c < L->cin; c++) s += L->w[c] * t_row(&d, c, y)[xx];
                p[(size_t)y * d.w + xx] = 1.0f / (1.0f + expf(-s));
            }
    }
    rc = 0;
done:
    t_free(&e1);
    t_free(&e2);
    t_free(&e3);
    t_free(&mi);
    t_free(&t);
    t_free(&u);
    t_free(&d);
    return rc;
}

/* ---- the whole fax --------------------------------------------------------------- */

double wefax_pp_macs(const wefax_pp_t *m, int w, int h, int tile)
{
    if (!m) return 0;
    if (tile <= 0) tile = WEFAX_PP_TILE;
    double s = 0;
    for (int y0 = 0; y0 < h; y0 += tile)
        for (int x0 = 0; x0 < w; x0 += tile) {
            const int ya = y0 - WEFAX_PP_PAD > 0 ? y0 - WEFAX_PP_PAD : 0;
            const int xa = x0 - WEFAX_PP_PAD > 0 ? x0 - WEFAX_PP_PAD : 0;
            const int yb = y0 + tile + WEFAX_PP_PAD < h ? y0 + tile + WEFAX_PP_PAD : h;
            const int xb = x0 + tile + WEFAX_PP_PAD < w ? x0 + tile + WEFAX_PP_PAD : w;
            s += tile_macs(m, (xb - xa + 7) / 8 * 8, (yb - ya + 7) / 8 * 8);
        }
    return s;
}

int wefax_pp_run(const wefax_pp_t *m, const uint8_t *gray, uint8_t *out, int w, int h,
                 int threads, int tile)
{
    if (!m || !gray || !out || w < 8 || h < 8) return -1;
    if (tile <= 0) tile = WEFAX_PP_TILE;
    if (tile % 8) return -1;
    if (threads < 1) threads = 1;
    if (threads > MAX_THREADS) threads = MAX_THREADS;

    /* the result goes to a buffer of its own: out may be gray itself, and
     * the tiles still to do read the received fax around them */
    uint8_t *res = malloc((size_t)w * h);
    if (!res) return -2;

    const double total = wefax_pp_macs(m, w, h, tile);
    double done_macs = 0;
    wefax_pp_progress = 0;
    int rc = 0;

    for (int y0 = 0; y0 < h && !rc; y0 += tile)
        for (int x0 = 0; x0 < w && !rc; x0 += tile) {
            /* as run_full() of model.py: the tile with WEFAX_PP_PAD of
             * context, cut at the edges of the fax; the size rounded up to
             * a multiple of 8 repeating the last row/column */
            const int ya = y0 - WEFAX_PP_PAD > 0 ? y0 - WEFAX_PP_PAD : 0;
            const int xa = x0 - WEFAX_PP_PAD > 0 ? x0 - WEFAX_PP_PAD : 0;
            const int yb = y0 + tile + WEFAX_PP_PAD < h ? y0 + tile + WEFAX_PP_PAD : h;
            const int xb = x0 + tile + WEFAX_PP_PAD < w ? x0 + tile + WEFAX_PP_PAD : w;
            const int tw = (xb - xa + 7) / 8 * 8, th = (yb - ya + 7) / 8 * 8;

            tensor_t x;
            float *p = malloc(sizeof(float) * tw * th);
            if (!p || t_new(&x, 1, tw, th)) {
                free(p);
                rc = -2;
                break;
            }
            for (int y = 0; y < th; y++) {
                const int sy = ya + (y < yb - ya ? y : yb - ya - 1);
                float *r = t_row(&x, 0, y);
                for (int xx = 0; xx < tw; xx++) {
                    const int sx = xa + (xx < xb - xa ? xx : xb - xa - 1);
                    r[xx] = (255 - gray[(size_t)sy * w + sx]) * (1.0f / 255.0f);
                }
            }
            rc = net(m, &x, p, threads);
            t_free(&x);
            if (!rc) {
                const int y1 = y0 + tile < h ? y0 + tile : h;
                const int x1 = x0 + tile < w ? x0 + tile : w;
                for (int y = y0; y < y1; y++)
                    for (int xx = x0; xx < x1; xx++) {
                        const float v = 255.0f * (1.0f - p[(size_t)(y - ya) * tw + (xx - xa)]) + 0.5f;
                        res[(size_t)y * w + xx] = v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
                    }
            }
            free(p);
            done_macs += tile_macs(m, tw, th);
            wefax_pp_progress = (int)(done_macs * 1000.0 / total);
        }

    if (!rc) memcpy(out, res, (size_t)w * h);
    free(res);
    return rc;
}
