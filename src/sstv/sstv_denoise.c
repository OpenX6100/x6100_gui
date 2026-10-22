/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */

/* SSTV picture denoiser, see sstv_denoise.h */

#include "sstv_denoise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- parameters ------------------------------------------------------------------ */

typedef struct {
    float gate;         /* below this noise (levels) a plane is left as it is */
    float rep_k;        /* line repair: outlier when beyond rep_k x noise ... */
    float rep_min;      /* ... and beyond rep_min levels */
    float y_k;          /* brightness smoothing strength, x noise */
    float c_k;          /* colour smoothing strength, x noise */
    float c_guide_k;    /* colour smoothing: brightness edge, x brightness noise */
    int   run_long;     /* line repair: a run of outliers this long ... */
    float run_cv;       /* ... this even (spread / mean) is a real line: kept */
    float achro;        /* brightness outlier without colour outlier (below achro x) is real: kept */
    float band_ratio;   /* band repair: a piece of line busier than band_ratio x its neighbours ... */
    float band_abs;     /* ... and by band_abs levels ... */
    int   band_max;     /* ... in a band at most band_max lines tall, is rebuilt */
    float band_floor;   /* band repair: neighbours never taken as calmer than this */
    float band_coh;     /* band repair: a piece differing this evenly (0..1) is real */
} dn_param_t;

static dn_param_t P = {
    .gate      = 3.0f,
    .rep_k     = 1.6f,
    .rep_min   = 24.0f,
    .y_k       = 1.6f,
    .c_k       = 5.0f,
    .c_guide_k = 2.5f,
    .run_long  = 10,
    .run_cv    = 0.35f,
    .achro     = 0.25f,
    .band_ratio = 2.0f,
    .band_abs   = 6.0f,
    .band_max   = 8,
    .band_floor = 4.0f,
    .band_coh   = 0.6f,
};

#ifdef SSTV_DENOISE_TUNING
#include <stdio.h>
/* test program only: SSTV_DN_<NAME>=value in the environment */
static void load_tuning(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *v;
    if ((v = getenv("SSTV_DN_GATE")))    P.gate = (float)atof(v);
    if ((v = getenv("SSTV_DN_REP_K")))   P.rep_k = (float)atof(v);
    if ((v = getenv("SSTV_DN_REP_MIN"))) P.rep_min = (float)atof(v);
    if ((v = getenv("SSTV_DN_Y_K")))     P.y_k = (float)atof(v);
    if ((v = getenv("SSTV_DN_C_K")))     P.c_k = (float)atof(v);
    if ((v = getenv("SSTV_DN_CG_K")))    P.c_guide_k = (float)atof(v);
    if ((v = getenv("SSTV_DN_RUN_LONG"))) P.run_long = atoi(v);
    if ((v = getenv("SSTV_DN_RUN_CV")))  P.run_cv = (float)atof(v);
    if ((v = getenv("SSTV_DN_ACHRO")))   P.achro = (float)atof(v);
    if ((v = getenv("SSTV_DN_BAND_RATIO"))) P.band_ratio = (float)atof(v);
    if ((v = getenv("SSTV_DN_BAND_ABS")))   P.band_abs = (float)atof(v);
    if ((v = getenv("SSTV_DN_BAND_MAX")))   P.band_max = atoi(v);
    if ((v = getenv("SSTV_DN_BAND_FLOOR"))) P.band_floor = (float)atof(v);
    if ((v = getenv("SSTV_DN_BAND_COH")))   P.band_coh = (float)atof(v);
}
#endif

/* ---- helpers --------------------------------------------------------------------- */

/* exp(-x/2) for x = d^2/h^2 in [0, LUT_MAX) */
#define LUT_N     512
#define LUT_MAX   9.0f
static float lut[LUT_N];

static void lut_init(void)
{
    static int done;
    if (done) return;
    for (int i = 0; i < LUT_N; i++) lut[i] = expf(-0.5f * (i + 0.5f) * LUT_MAX / LUT_N);
    done = 1;
}

static inline float range_w(float d2_over_h2)
{
    int i = (int)(d2_over_h2 * (LUT_N / LUT_MAX));
    return i < LUT_N ? lut[i] : 0.0f;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline uint8_t to_u8(float v)
{
    v += 0.5f;
    return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
}

/* Noise of a plane: robust (median) size of the difference between a value
 * and the mean of the lines above and below.  For noise of size s that
 * difference has size s * sqrt(1.5). */
static float plane_noise(const float *p, int w, int h)
{
    enum { BINS = 1024 };               /* 0.25 level per bin */
    unsigned hist[BINS];
    memset(hist, 0, sizeof(hist));
    unsigned n = 0;
    for (int y = 1; y < h - 1; y++) {
        const float *a = p + (size_t)(y - 1) * w, *b = p + (size_t)y * w, *c = p + (size_t)(y + 1) * w;
        for (int x = 0; x < w; x++) {
            float r = fabsf(b[x] - 0.5f * (a[x] + c[x]));
            int i = (int)(r * 4.0f);
            hist[i < BINS ? i : BINS - 1]++;
            n++;
        }
    }
    if (!n) return 0.0f;
    unsigned half = n / 2, acc = 0;
    int i = 0;
    while (i < BINS && (acc += hist[i]) < half) i++;
    float med = (i + 0.5f) / 4.0f;
    return 1.4826f * med / 1.2247449f;
}

static float med5(float a, float b, float c, float d, float e)
{
    float v[5] = { a, b, c, d, e };
    for (int i = 1; i < 5; i++) {       /* insertion sort, 5 values */
        float t = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; }
        v[j + 1] = t;
    }
    return v[2];
}

/* Signed difference from the lines above and below, when the value stands out
 * from both in the same direction by more than thr; else 0 */
static inline float outlier(const float *col_u, const float *col_c, const float *col_d, int x, float thr)
{
    float p = col_c[x], du = p - col_u[x], dd = p - col_d[x];
    if ((du > thr && dd > thr) || (du < -thr && dd < -thr)) return p - 0.5f * (col_u[x] + col_d[x]);
    return 0.0f;
}

/* Line repair of one plane: src -> dst.  Returns the values replaced.
 * e: scratch of w floats.  c1, c2 (may be NULL): the colour planes, for the
 * brightness only: an outlier with no colour outlier is taken as real (white
 * or black text, thin lines), the noise of Martin/Scottie is coloured */
static int line_repair(const float *src, float *dst, int w, int h, float thr, float *e,
                       const float *c1, const float *c2, float cthr)
{
    int n = 0;
    for (int y = 0; y < h; y++) {
        const float *r2u = src + (size_t)clampi(y - 2, 0, h - 1) * w;
        const float *r1u = src + (size_t)clampi(y - 1, 0, h - 1) * w;
        const float *r0  = src + (size_t)y * w;
        const float *r1d = src + (size_t)clampi(y + 1, 0, h - 1) * w;
        const float *r2d = src + (size_t)clampi(y + 2, 0, h - 1) * w;
        float *o = dst + (size_t)y * w;
        size_t yu = (size_t)clampi(y - 1, 0, h - 1) * w, y0 = (size_t)y * w, yd = (size_t)clampi(y + 1, 0, h - 1) * w;

        for (int x = 0; x < w; x++) {
            e[x] = outlier(r1u, r0, r1d, x, thr);
            if (e[x] != 0.0f && c1 && P.achro > 0.0f) {
                float a = fabsf(outlier(c1 + yu, c1 + y0, c1 + yd, x, cthr)) +
                          fabsf(outlier(c2 + yu, c2 + y0, c2 + yd, x, cthr));
                if (a < P.achro * fabsf(e[x])) e[x] = 0.0f;
            }
        }
        /* long, even runs of the same sign are real horizontal lines */
        for (int x = 0; x < w;) {
            if (e[x] == 0.0f) { x++; continue; }
            int x0 = x;
            float sum = 0.0f, sum2 = 0.0f;
            while (x < w && e[x] != 0.0f && (e[x] > 0.0f) == (e[x0] > 0.0f)) {
                sum += e[x];
                sum2 += e[x] * e[x];
                x++;
            }
            int len = x - x0;
            if (len >= P.run_long) {
                float m = sum / len, var = sum2 / len - m * m;
                if (sqrtf(var > 0.0f ? var : 0.0f) < P.run_cv * fabsf(m))
                    for (int i = x0; i < x; i++) e[i] = 0.0f;
            }
        }
        for (int x = 0; x < w; x++) {
            if (e[x] != 0.0f) {
                o[x] = med5(r2u[x], r1u[x], r0[x], r1d[x], r2d[x]);
                n++;
            } else {
                o[x] = r0[x];
            }
        }
    }
    return n;
}

/* Brightness: bilateral over 5 lines x 3 pixels.  src -> dst */
static void smooth_y(const float *src, float *dst, int w, int h, float hy)
{
    static const float ws[5][3] = {
        /* exp(-(dy^2 / (2*1.5^2) + dx^2 / 2)) */
        { 0.1084f, 0.1690f, 0.1084f },
        { 0.4329f, 0.8007f, 0.4329f },
        { 0.6065f, 1.0000f, 0.6065f },
        { 0.4329f, 0.8007f, 0.4329f },
        { 0.1084f, 0.1690f, 0.1084f },
    };
    float inv = 1.0f / (hy * hy);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float c = src[(size_t)y * w + x], sw = 0.0f, sv = 0.0f;
            for (int dy = -2; dy <= 2; dy++) {
                const float *r = src + (size_t)clampi(y + dy, 0, h - 1) * w;
                for (int dx = -1; dx <= 1; dx++) {
                    float v = r[clampi(x + dx, 0, w - 1)], d = v - c;
                    float wt = ws[dy + 2][dx + 1] * range_w(d * d * inv);
                    sw += wt;
                    sv += wt * v;
                }
            }
            dst[(size_t)y * w + x] = sv / sw;   /* sw >= 1: the centre */
        }
    }
}

/* Colour: bilateral over 5 x 5 guided by the brightness g.  src -> dst */
static void smooth_c(const float *src, const float *g, float *dst, int w, int h, float hc, float hg)
{
    static float ws[5][5];
    static int ws_done;
    if (!ws_done) {
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                ws[dy + 2][dx + 2] = expf(-(dy * dy + dx * dx) / (2.0f * 1.5f * 1.5f));
        ws_done = 1;
    }
    float invc = 1.0f / (hc * hc), invg = 1.0f / (hg * hg);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t o = (size_t)y * w + x;
            float c = src[o], gc = g[o], sw = 0.0f, sv = 0.0f;
            for (int dy = -2; dy <= 2; dy++) {
                size_t ro = (size_t)clampi(y + dy, 0, h - 1) * w;
                for (int dx = -2; dx <= 2; dx++) {
                    size_t i = ro + clampi(x + dx, 0, w - 1);
                    float d = src[i] - c, dg = g[i] - gc;
                    float wt = ws[dy + 2][dx + 2] * range_w(d * d * invc) * range_w(dg * dg * invg);
                    sw += wt;
                    sv += wt * src[i];
                }
            }
            dst[o] = sv / sw;
        }
    }
}

/* ---- band repair ---------------------------------------------------------------------- */

#define SEG_W    32     /* the lines are judged in pieces this wide */
#define BAND_WIN 12     /* compared with the lines up to this far above and below */

static float median_f(float *v, int n)
{
    for (int i = 1; i < n; i++) {
        float t = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; }
        v[j + 1] = t;
    }
    return v[n / 2];
}

/* A burst of QRM or noise spoils a band of a few lines: each piece of line much
 * busier (differences along the line and from the lines around) than the
 * lines near it is rebuilt from the good lines above and below.  Returns the
 * pieces rebuilt.  pl: Y, Cb, Cr. */
static int band_repair(float *pl[3], int w, int h)
{
    int nseg = (w + SEG_W - 1) / SEG_W;
    float *b = malloc(sizeof(float) * (size_t)nseg * h * 2);
    unsigned char *bad = calloc((size_t)nseg * h, 1);
    if (!b || !bad) { free(b); free(bad); return 0; }
    float *coh = b + (size_t)nseg * h;

    for (int y = 0; y < h; y++) {
        int yu = clampi(y - 1, 0, h - 1), yd = clampi(y + 1, 0, h - 1);
        for (int sg = 0; sg < nseg; sg++) {
            int x0 = sg * SEG_W, x1 = x0 + SEG_W > w ? w : x0 + SEG_W;
            float q = 0.0f;
            /* how much the line differs from the lines above and below; the
             * colour counts double: noise is coloured, text and drawings
             * mostly are not, and stay the same from one line to the next */
            float qs = 0.0f;
            for (int k = 0; k < 3; k++) {
                const float *r = pl[k] + (size_t)y * w, *u = pl[k] + (size_t)yu * w, *d = pl[k] + (size_t)yd * w;
                float qk = 0.0f, sk = 0.0f;
                for (int x = x0; x < x1; x++) {
                    float e = r[x] - 0.5f * (u[x] + d[x]);
                    qk += fabsf(e);
                    sk += e;
                }
                float wk = k ? 2.0f : 1.0f;
                q  += wk * qk;
                qs += wk * fabsf(sk);
            }
            b[(size_t)y * nseg + sg] = q / (x1 - x0);
            /* a real line or the edge of a drawing differs the same way all
             * along; noise changes sign all the time */
            coh[(size_t)y * nseg + sg] = q > 0.0f ? qs / q : 1.0f;
        }
    }
    float win[2 * BAND_WIN + 1];
    for (int sg = 0; sg < nseg; sg++)
        for (int y = 0; y < h; y++) {
            int n = 0;
            for (int j = y - BAND_WIN; j <= y + BAND_WIN; j++)
                if (j >= 0 && j < h) win[n++] = b[(size_t)j * nseg + sg];
            float ref = fmaxf(median_f(win, n), P.band_floor), v = b[(size_t)y * nseg + sg];
            if (v > P.band_ratio * ref + P.band_abs && coh[(size_t)y * nseg + sg] < P.band_coh)
                bad[(size_t)y * nseg + sg] = 1;
        }

    int fixed = 0;
    for (int sg = 0; sg < nseg; sg++) {
        int x0 = sg * SEG_W, x1 = x0 + SEG_W > w ? w : x0 + SEG_W;
        for (int y = 0; y < h;) {
            if (!bad[(size_t)y * nseg + sg]) { y++; continue; }
            int ya = y;
            while (y < h && bad[(size_t)y * nseg + sg]) y++;
            int yb = y;                     /* bad lines ya..yb-1 */
            if (yb - ya > P.band_max) continue;
            int above = ya - 1, below = yb;   /* good lines, -1 / h: none */
            if (above < 0 && below >= h) continue;
            for (int k = 0; k < 3; k++)
                for (int yy = ya; yy < yb; yy++) {
                    float t = (above < 0) ? 1.0f : (below >= h) ? 0.0f : (float)(yy - above) / (below - above);
                    float *o = pl[k] + (size_t)yy * w;
                    const float *ra = pl[k] + (size_t)(above < 0 ? below : above) * w;
                    const float *rb = pl[k] + (size_t)(below >= h ? above : below) * w;
                    for (int x = x0; x < x1; x++) o[x] = (1.0f - t) * ra[x] + t * rb[x];
                }
            fixed += yb - ya;
        }
    }
    free(b);
    free(bad);
    return fixed;
}

/* ---- public ------------------------------------------------------------------------ */

int sstv_denoise(uint8_t *rgb, int w, int h, sstv_denoise_stats_t *st)
{
    sstv_denoise_stats_t s = { 0 };
    if (st) *st = s;
    if (!rgb || w < 3 || h < 5) return 0;

#ifdef SSTV_DENOISE_TUNING
    load_tuning();
#endif
    lut_init();

    /* black rows at the bottom: the picture was not complete */
    int rows = h;
    while (rows > 0) {
        const uint8_t *r = rgb + (size_t)(rows - 1) * w * 3;
        int sum = 0;
        for (int i = 0; i < w * 3; i++) sum += r[i];
        if (sum > 3 * w * 3) break;
        rows--;
    }
    s.rows = rows;
    if (rows < 5) {
        if (st) *st = s;
        return 0;
    }

    size_t n = (size_t)w * rows;
    float *buf = malloc((n * 6 + (size_t)w) * sizeof(float));
    if (!buf) return -1;
    float *Y = buf, *Cb = buf + n, *Cr = buf + 2 * n, *t1 = buf + 3 * n, *t2 = buf + 4 * n, *Yf = buf + 5 * n;
    float *ebuf = buf + 6 * n;

    for (size_t i = 0; i < n; i++) {
        float r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        Y[i]  =  0.299f * r + 0.587f * g + 0.114f * b;
        Cb[i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
        Cr[i] =  0.5f * r - 0.418688f * g - 0.081312f * b;
    }

    /* band repair only on a noisy picture: on a clean one, text and drawings
     * can look like a spoiled band */
    float *pl[3] = { Y, Cb, Cr };
    if (plane_noise(Y, w, rows) >= P.gate) s.bands = band_repair(pl, w, rows);

    s.sigma_y  = plane_noise(Y, w, rows);
    s.sigma_cb = plane_noise(Cb, w, rows);
    s.sigma_cr = plane_noise(Cr, w, rows);

    /* brightness */
    if (s.sigma_y >= P.gate) {
        float thr = fmaxf(P.rep_min, P.rep_k * s.sigma_y);
        float cthr = 0.5f * fmaxf(P.rep_min, P.rep_k * 0.5f * (s.sigma_cb + s.sigma_cr));
        /* the colour test only when the noise is coloured too (Martin, Scottie;
         * in Robot the colour is averaged over two lines and nearly clean) */
        int coloured = 0.5f * (s.sigma_cb + s.sigma_cr) > 0.5f * s.sigma_y;
        s.repaired += line_repair(Y, t1, w, rows, thr, ebuf, coloured ? Cb : NULL, Cr, cthr);
        smooth_y(t1, Yf, w, rows, P.y_k * s.sigma_y);
    } else {
        memcpy(Yf, Y, n * sizeof(float));
    }
    float hg = P.c_guide_k * fmaxf(s.sigma_y, P.gate);

    /* colour */
    float *C[2] = { Cb, Cr };
    float sig[2] = { s.sigma_cb, s.sigma_cr };
    for (int k = 0; k < 2; k++) {
        if (sig[k] < P.gate) continue;
        float thr = fmaxf(P.rep_min * 0.5f, P.rep_k * sig[k]);
        s.repaired += line_repair(C[k], t1, w, rows, thr, ebuf, NULL, NULL, 0.0f);
        smooth_c(t1, Yf, t2, w, rows, P.c_k * sig[k], hg);
        memcpy(C[k], t2, n * sizeof(float));
    }

    for (size_t i = 0; i < n; i++) {
        float y = Yf[i], cb = Cb[i], cr = Cr[i];
        rgb[i * 3]     = to_u8(y + 1.402f * cr);
        rgb[i * 3 + 1] = to_u8(y - 0.344136f * cb - 0.714136f * cr);
        rgb[i * 3 + 2] = to_u8(y + 1.772f * cb);
    }

    free(buf);
    if (st) *st = s;
    return 0;
}

/* 1KO125: noise only (for the ML denoiser: it runs only on a noisy picture) */
int sstv_denoise_measure(const uint8_t *rgb, int w, int h, sstv_denoise_stats_t *st)
{
    sstv_denoise_stats_t s = { 0 };
    if (st) *st = s;
    if (!rgb || w < 3 || h < 5) return 0;
#ifdef SSTV_DENOISE_TUNING
    load_tuning();
#endif
    int rows = h;
    while (rows > 0) {
        const uint8_t *r = rgb + (size_t)(rows - 1) * w * 3;
        int sum = 0;
        for (int i = 0; i < w * 3; i++) sum += r[i];
        if (sum > 3 * w * 3) break;
        rows--;
    }
    s.rows = rows;
    if (rows < 5) {
        if (st) *st = s;
        return 0;
    }
    size_t n = (size_t)w * rows;
    float *Y = malloc(n * 3 * sizeof(float));
    if (!Y) return -1;
    float *Cb = Y + n, *Cr = Y + 2 * n;
    for (size_t i = 0; i < n; i++) {
        float r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        Y[i]  =  0.299f * r + 0.587f * g + 0.114f * b;
        Cb[i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
        Cr[i] =  0.5f * r - 0.418688f * g - 0.081312f * b;
    }
    s.sigma_y  = plane_noise(Y, w, rows);
    s.sigma_cb = plane_noise(Cb, w, rows);
    s.sigma_cr = plane_noise(Cr, w, rows);
    free(Y);
    if (st) *st = s;
    return s.sigma_y >= P.gate || s.sigma_cb >= P.gate || s.sigma_cr >= P.gate;
}
