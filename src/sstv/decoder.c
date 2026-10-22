/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 SSTV decoder
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
*/

#include "decoder.h"
#include "vis.h"

#include <sndfile.h>
#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR SSTV_SAMPLE_RATE
#define PI 3.14159265358979323846f
/* FM front end (see fm_demod()). */
#define FM_TAPS     61        /* complex band-pass, linear phase          */
#define FM_CENTER   1750.0    /* centre of 1200..2300 Hz                   */
#define FM_HALF_BW  900.0     /* -6 dB half width: about 850..2650 Hz      */
#define FM_POST     15        /* post-detection Hann low-pass, taps (odd)  */
#define FM_FMIN     1000.0f   /* per-sample clamp against FM clicks        */
#define FM_FMAX     2500.0f
#define MAX_SYNCS 2000   /* was 300: noisy pictures overflowed it before the last lines */

typedef struct { int vis; sstv_mode_t mode; const char *name; int w, h; } mode_desc_t;
static const mode_desc_t modes[] = {
    { 2, SSTV_MODE_ROBOT8_BW, "Robot 8 BW", 160, 120 },
    { 8, SSTV_MODE_ROBOT36,   "Robot 36",  320, 240 },
    {12, SSTV_MODE_ROBOT72,   "Robot 72",  320, 240 },
    {40, SSTV_MODE_MARTIN_M2, "Martin M2", 320, 256 },
    {44, SSTV_MODE_MARTIN_M1, "Martin M1", 320, 256 },
    {56, SSTV_MODE_SCOTTIE_S2,"Scottie S2",320, 256 },
    {60, SSTV_MODE_SCOTTIE_S1,"Scottie S1",320, 256 },
    {76, SSTV_MODE_SCOTTIE_DX,"Scottie DX",320, 256 },
};

static uint8_t clamp8(float v)
{
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

/* Common FM front end for all decoders.
 *
 * 1. Complex linear-phase band-pass (about 850..2650 Hz, 61-tap Hamming):
 *    gives a clean analytic signal and removes the noise outside the SSTV
 *    band.  The old 9-tap Hilbert transformer left an image only 16-21 dB
 *    down at 1200-1500 Hz, i.e. a +/-250..370 Hz ripple on the demodulated
 *    frequency.
 *    The band is kept fairly wide on purpose: in FM, band-limiting BEFORE
 *    the discriminator destroys picture detail, noise is better removed
 *    after it.
 * 2. Phase-difference discriminator: freq[i] is the mean frequency over the
 *    interval [i-1, i] (sample units), because it is the phase advance
 *    between samples i-1 and i.
 * 3. Post-detection Hann low-pass (FM_POST taps, ~1.2 ms): removes the
 *    noise the SSB channel cannot carry as picture detail anyway.  Chosen on
 *    a simulated 300-2700 Hz SSB channel: same or lower noise than the old
 *    11-sample moving average at every SNR (2-9% lower at 6 dB), same or
 *    better detail except the very finest Martin M1 detail.
 * 4. Both filters are symmetric and their delay is removed, so freq[] is
 *    aligned with the audio and with the sync positions found on the audio
 *    (the old front end shifted every picture 2-3 pixels to the right).
 * Returns a malloc'd array of n frequencies in Hz, or NULL. */
static float complex fm_taps[FM_TAPS];
static pthread_once_t fm_once = PTHREAD_ONCE_INIT;

static void fm_init(void)
{
    const int D = FM_TAPS / 2;
    const double fc = FM_HALF_BW / (double)SR;
    double sum = 0.0;
    double lp[FM_TAPS];
    for (int k = 0; k < FM_TAPS; ++k) {
        int m = k - D;
        double h = (m == 0) ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        h *= 0.54 - 0.46 * cos(2.0 * M_PI * k / (FM_TAPS - 1));   /* Hamming */
        lp[k] = h;
        sum += h;
    }
    /* shift the low-pass to FM_CENTER: complex band-pass, phase reference at
     * the centre tap so the output is not delayed after re-alignment */
    for (int k = 0; k < FM_TAPS; ++k)
        fm_taps[k] = (float)(lp[k] / sum) * cexpf(I * (float)(2.0 * M_PI * FM_CENTER * (k - D) / (double)SR));
}

static float *fm_demod(const float *x, long n)
{
    if (!x || n <= 1) return NULL;
    float *freq = malloc((size_t)n * sizeof(float));
    if (!freq) return NULL;
    pthread_once(&fm_once, fm_init);
    const int D = FM_TAPS / 2;
    const float k2hz = (float)SR / (2.0f * (float)M_PI);
    float complex prev = 0.0f;
    for (long i = 0; i < n; ++i) {
        /* y[i] = sum_k x[i + D - k] * taps[k]  (centred: no delay) */
        float complex acc = 0.0f;
        long j0 = i + D;
        int k0 = 0, k1 = FM_TAPS;
        if (j0 >= n) k0 = (int)(j0 - n + 1);
        if (j0 - (FM_TAPS - 1) < 0) k1 = (int)(j0 + 1);
        for (int k = k0; k < k1; ++k) acc += x[j0 - k] * fm_taps[k];
        float f = cargf(acc * conjf(prev)) * k2hz;
        if (!(f >= FM_FMIN)) f = FM_FMIN;          /* also catches NaN */
        if (f > FM_FMAX) f = FM_FMAX;
        freq[i] = f;
        prev = acc;
    }
    if (n > 1) freq[0] = freq[1];

    /* centred post-detection low-pass, in place: keep the last H raw values
     * in a small ring, since the output overwrites them */
    {
        const int P = FM_POST, H = FM_POST / 2;
        float w[FM_POST], raw[FM_POST];
        double ws = 0.0;
        for (int k = 0; k < P; ++k) {
            w[k] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)(k + 1) / (float)(P + 1));
            ws += w[k];
        }
        for (int k = 0; k < P; ++k) w[k] = (float)(w[k] / ws);
        /* raw[(j) % P] holds the unfiltered freq[j] for j in [i-H, i+H] */
        for (long j = 0; j < H && j < n; ++j) raw[j % P] = freq[j];
        for (long i = 0; i < n; ++i) {
            if (i + H < n) raw[(i + H) % P] = freq[i + H];
            float a = 0.0f, wsum = 0.0f;
            for (int k = -H; k <= H; ++k) {
                long j = i + k;
                if (j < 0 || j >= n) continue;
                a += w[k + H] * raw[j % P];
                wsum += w[k + H];
            }
            freq[i] = a / wsum;
        }
    }
    return freq;
}

static inline float hz_to_level(float f)
{
    float v = (f - 1500.0f) * (255.0f / 800.0f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
}

/* Mean frequency over the continuous interval [t0, t1] (sample units),
 * using freq[i] = mean over [i-1, i] (see fm_demod()). */
static float mean_freq(const float *freq, long n, double t0, double t1)
{
    if (t0 < 0.0) t0 = 0.0;
    if (t1 > (double)(n - 1)) t1 = (double)(n - 1);
    if (t1 <= t0) {
        long i = (long)ceil(t0);
        if (i < 1) i = 1;
        if (i > n - 1) i = n - 1;
        return freq[i];
    }
    long i0 = (long)floor(t0), i1 = (long)floor(t1);
    double acc;
    if (i0 == i1) {
        acc = freq[i0 + 1] * (t1 - t0);
    } else {
        acc = freq[i0 + 1] * ((double)(i0 + 1) - t0);        /* partial first */
        for (long i = i0 + 2; i <= i1; ++i) acc += freq[i];   /* whole samples */
        if (t1 > (double)i1) acc += freq[i1 + 1 < n ? i1 + 1 : n - 1] * (t1 - (double)i1);
    }
    return (float)(acc / (t1 - t0));
}

/* Pixel values of one scan segment [a, b): every pixel is the mean frequency
 * over its own time slot (integrate-and-dump), i.e. the matched filter of
 * the mode, with no fixed smoothing length. */
static void segment(const float *freq, long n, long a, long b, float *dst, int count)
{
    double step = (double)(b - a) / (double)count;
    for (int i = 0; i < count; ++i) {
        double t0 = (double)a + (double)i * step;
        dst[i] = hz_to_level(mean_freq(freq, n, t0, t0 + step));
    }
}

/* Robust horizontal-sync detector shared by all RAM decoders.
 *
 * The old detector required the phase discriminator to remain continuously
 * inside 1080..1320 Hz for several milliseconds.  Real RF can punch tiny
 * holes into an otherwise perfectly usable 1200-Hz sync pulse; one such
 * recording reduced every Martin sync to ~1-ms fragments and yielded zero
 * candidates.
 *
 * Detect the sync directly in the audio instead.  For short overlapping
 * windows, measure normalized coherent energy at 1200 Hz.  This tolerates
 * brief frequency-estimator glitches, fading and impulse noise without
 * changing the protocol timing rules.  Adjacent positive windows are merged
 * and only the strongest position is emitted.  The existing robust timing
 * grid then decides whether those candidates form a physically credible
 * line-period chain, so isolated 1200-Hz picture content cannot establish
 * timing by itself.
 *
 * pulse_ms is mode-specific (Martin 4.862 ms, Robot/Scottie about 9-10 ms);
 * the detection mechanism and threshold are intentionally common. */
static int detect_sync_1200_robust(const float *samples, long n,
                                   float pulse_ms, long *cand, int max_cand)
{
    if (!samples || n <= 0 || !cand || max_cand <= 0 || pulse_ms <= 0.0f)
        return 0;

    int win = (int)lroundf(pulse_ms * (float)SR / 1000.0f);
    if (win < 24) win = 24;
    if (win > 160) win = 160;

    /* ~1 ms hop.  Small enough to locate a 4.862-ms Martin sync accurately,
     * while keeping the scan inexpensive on the X6100. */
    const int hop = (SR + 500) / 1000;
    const float threshold = 0.45f;

    float cs[160], sn[160];
    for (int k=0; k<win; ++k) {
        float ph = 2.0f * PI * 1200.0f * (float)k / (float)SR;
        cs[k] = cosf(ph);
        sn[k] = sinf(ph);
    }

    int nc=0;
    int in_group=0;
    float best_score=0.0f;
    long best_pos=0;

    for (long a=SR/2; a+win<n; a+=hop) {
        double re=0.0, im=0.0, energy=0.0;
        for (int k=0; k<win; ++k) {
            float v=samples[a+k];
            re += (double)v * (double)cs[k];
            im -= (double)v * (double)sn[k];
            energy += (double)v * (double)v;
        }

        float score=0.0f;
        if (energy > 1e-12) {
            double mag=sqrt(re*re+im*im);
            score=(float)(mag/sqrt(energy*(double)win));
        }

        if (score >= threshold) {
            if (!in_group) {
                in_group=1;
                best_score=score;
                best_pos=a;
            } else if (score > best_score) {
                best_score=score;
                best_pos=a;
            }
        } else if (in_group) {
            if (nc < max_cand) cand[nc++]=best_pos;
            in_group=0;
            best_score=0.0f;
            if (nc >= max_cand) break;
        }
    }

    if (in_group && nc < max_cand)
        cand[nc++]=best_pos;

    return nc;
}

/* Second-pass global slant refinement.
 * The first robust chain gives us a safe clock/phase estimate.  Once that is
 * known, look at every detected horizontal-sync candidate again and assign it
 * to the nearest predicted image row.  This can extend a short but good chain
 * over a much longer baseline, which makes the samples/line estimate markedly
 * more accurate.  One candidate per row is retained (the closest one).
 *
 * This is deliberately conservative: late chains are left to the existing
 * late-chain anchor recovery, and a new fit is accepted only when it extends
 * the timing baseline by at least 12 rows without producing a large clock
 * jump. */
static int global_sync_refine(const char *tag, const long *cand, int nc, int height,
                              double nominal_spl, int old_points, int old_span,
                              double *intercept_io, double *spl_io)
{
    if (!cand || nc <= 0 || !intercept_io || !spl_io || *spl_io <= 0.0) return 0;

    double intercept=*intercept_io, spl=*spl_io;
    if (intercept > 5.0*(double)SR) return 0; /* late sparse chain: anchor first */

    int have[300]={0};
    long pos[300]={0};
    double err[300];
    for(int r=0;r<300;++r) err[r]=1e30;

    /* Tight gate: wide enough for the residual drift we want to correct, but
     * much narrower than a line so random 1200-Hz bursts are unlikely to win. */
    const double gate=nominal_spl*0.020;
    for(int k=0;k<nc;++k){
        double rf=((double)cand[k]-intercept)/spl;
        long row=lround(rf);
        if(row<0 || row>=height) continue;
        double predicted=intercept+spl*(double)row;
        double e=fabs((double)cand[k]-predicted);
        if(e<=gate && e<err[row]){have[row]=1;pos[row]=cand[k];err[row]=e;}
    }

    int count=0,first=-1,last=-1;
    double sx=0,sy=0,sxx=0,sxy=0;
    for(int r=0;r<height;++r) if(have[r]){
        if(first<0) first=r; last=r; ++count;
        double x=(double)r,y=(double)pos[r];
        sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
    }
    if(count<12 || first<0 || last-first < old_span+12) return 0;

    double den=(double)count*sxx-sx*sx;
    if(fabs(den)<1e-9) return 0;
    double nspl=((double)count*sxy-sx*sy)/den;
    double nintercept=(sy-nspl*sx)/(double)count;

    /* Never let this optional refinement make a dramatic change.  Its job is
     * sub-line slant correction, not mode/timing recovery. */
    double jump_ppm=fabs(nspl/spl-1.0)*1000000.0;
    if(jump_ppm>400.0) {
        fprintf(stderr,"SSTV: %s: GLOBAL sync refinement rejected: clock jump %.1f ppm (points=%d span=%d)\\n",
                tag,jump_ppm,count,last-first+1);
        return 0;
    }

    fprintf(stderr,"SSTV: %s: GLOBAL sync refinement accepted: points %d -> %d, span %d -> %d rows\\n",
            tag,old_points,count,old_span+1,last-first+1);
    fprintf(stderr,"SSTV: %s: GLOBAL refined line %.6f -> %.6f ms, clock %+.1f -> %+.1f ppm\\n",
            tag,spl*1000.0/(double)SR,nspl*1000.0/(double)SR,
            (spl/nominal_spl-1.0)*1000000.0,(nspl/nominal_spl-1.0)*1000000.0);
    *intercept_io=nintercept;
    *spl_io=nspl;
    return count;
}

/* ---------------------------------------------------------------------
 * Per-row renderers.  Each one draws ONE picture row from the position of
 * its anchor sync (base, sample index into freq[0..n)) and the timing
 * scale (fitted / nominal line period).  They are used by the batch
 * decoders below and by sstv_render_row() for the progressive display, so
 * the preview and the final picture are drawn by the same code.  The
 * arithmetic and the bounds checks are exactly those of the original
 * per-mode loops (the batch output is byte-for-byte unchanged).
 * They return false when the row's samples are not all available.
 * --------------------------------------------------------------------- */

/* Martin: base = line sync; G, B, R follow.  out: 320*3 RGB. */
static bool martin_row(const float *freq, long n, long base, double timing_scale,
                       float channel_ms, uint8_t *out)
{
    const float sync_ms = 4.862f;
    const float porch_ms = 0.572f;
    const float sep_ms = 0.572f;
    const float g0_ms = sync_ms + porch_ms;
    const float b0_ms = g0_ms + channel_ms + sep_ms;
    const float r0_ms = b0_ms + channel_ms + sep_ms;
    float G[320], B[320], R[320];
    long ga=base+(long)llround(g0_ms*SR/1000.0*timing_scale);
    long gb=base+(long)llround((g0_ms+channel_ms)*SR/1000.0*timing_scale);
    long ba=base+(long)llround(b0_ms*SR/1000.0*timing_scale);
    long bb=base+(long)llround((b0_ms+channel_ms)*SR/1000.0*timing_scale);
    long ra=base+(long)llround(r0_ms*SR/1000.0*timing_scale);
    long rb=base+(long)llround((r0_ms+channel_ms)*SR/1000.0*timing_scale);
    if (rb >= n) return false;
    segment(freq,n,ga,gb,G,320);
    segment(freq,n,ba,bb,B,320);
    segment(freq,n,ra,rb,R,320);
    for (int px=0; px<320; ++px) {
        out[px*3+0]=clamp8(R[px]);
        out[px*3+1]=clamp8(G[px]);
        out[px*3+2]=clamp8(B[px]);
    }
    return true;
}

/* Robot 72: base = line sync; Y, R-Y, B-Y on every line.  out: 320*3. */
static bool robot72_row(const float *freq, long n, long base, double scale, uint8_t *out)
{
    const int width=320;
    float Y[320],Cr[320],Cb[320];
    long ya=base+(long)llround(.012*SR*scale), yb=base+(long)llround(.150*SR*scale);
    long cra=base+(long)llround(.156*SR*scale), crb=base+(long)llround(.225*SR*scale);
    long cba=base+(long)llround(.231*SR*scale), cbb=base+(long)llround(.300*SR*scale);
    if(base<0||cbb>=n) return false;
    segment(freq,n,ya,yb,Y,width);segment(freq,n,cra,crb,Cr,width);segment(freq,n,cba,cbb,Cb,width);
    for(int px=0;px<width;++px){float cr=Cr[px]-128.0f,cb=Cb[px]-128.0f,y=Y[px];out[px*3]=clamp8(y+1.402f*cr);out[px*3+1]=clamp8(y-.714136f*cr-.344136f*cb);out[px*3+2]=clamp8(y+1.772f*cb);}
    return true;
}

/* Robot 8 B/W line: 1200 Hz sync, then luminance up to the next sync.
 * The published timing (SSTV Handbook, table 4.1) is a 10 ms sync and the
 * rest of the 66.667 ms line as video; some encoders (1KO125 up to beta 3)
 * send a 5 ms sync.  Both are found by the 5 ms sync detector, which may
 * place base anywhere inside a 10 ms pulse, so the row measures the real
 * edges of the pulse around base and renders from the end of the sync to
 * the start of the next one (sync start + one line).
 * Returns false if the edges are not credible (caller falls back). */
#define R8_LINE_MS   (1000.0 / 15.0)
#define R8_EDGE_HZ   1350.0f
/* mean of freq[i .. i + len) */
static double r8_avg(const float *freq, long i, long len)
{
    double sum = 0.0;
    for (long k = i; k < i + len; ++k) sum += freq[k];
    return sum / (double)len;
}

static bool robot8bw_sync_edges(const float *freq, long n, long base, double scale,
                                double *start_ms, double *end_ms)
{
    const long ms1 = (long)llround(SR / 1000.0);
    const long back = (long)llround(12.0 * SR / 1000.0 * scale);
    const long fwd  = (long)llround(14.0 * SR / 1000.0 * scale);
    if (base - back - ms1 < 0 || base + fwd + ms1 >= n) return false;

    /* base may sit just before the pulse: look up to 3 ms ahead for it */
    long in = base;
    const long ahead = (long)llround(3.0 * SR / 1000.0 * scale);
    while (in < base + ahead && r8_avg(freq, in, ms1) > R8_EDGE_HZ) ++in;
    if (r8_avg(freq, in, ms1) > R8_EDGE_HZ) return false;             /* no sync near base */
    long st = in, en = in;
    while (st > base - back && r8_avg(freq, st - 1, ms1) <= R8_EDGE_HZ) --st;
    while (en < base + fwd && r8_avg(freq, en + 1, ms1) <= R8_EDGE_HZ) ++en;
    /* A 1 ms mean starting at i crosses the threshold when half of it is in
     * the sync: the edges are 0.5 ms after st and en. */
    double s_ms = (double)(st - base) * 1000.0 / SR + 0.5;
    double e_ms = (double)(en - base) * 1000.0 / SR + 0.5;
    double len = (e_ms - s_ms) / scale;
    if (len < 2.5 || len > 12.5) return false;
    /* The threshold crossings depend on the picture next to the sync (a
     * white edge makes the pulse look up to ~1 ms shorter on each side),
     * but its centre does not: keep the centre and snap the length to the
     * two timings in use, 5 or 10 ms. */
    double mid = 0.5 * (s_ms + e_ms);
    double nom = (len < 7.5 ? 5.0 : 10.0) * scale;
    *start_ms = mid - 0.5 * nom;
    *end_ms = mid + 0.5 * nom;
    return true;
}

/* edges: sync start/end in ms relative to base (NULL = measure this row). */
static bool robot8bw_row_at(const float *freq, long n, long base, double timing_scale,
                            const double *edges, uint8_t *out)
{
    const int width = 160;
    double s_ms, e_ms;
    if (edges) {
        s_ms = edges[0]; e_ms = edges[1];
    } else if (!robot8bw_sync_edges(freq, n, base, timing_scale, &s_ms, &e_ms)) {
        s_ms = 0.0; e_ms = 10.0 * timing_scale;           /* nominal: 10 ms sync */
    }
    float Y[160];
    long a = base + (long)llround(e_ms * SR / 1000.0);
    long b = base + (long)llround((s_ms + R8_LINE_MS * timing_scale) * SR / 1000.0);
    if (a < 0 || b >= n || b <= a) return false;
    segment(freq,n,a,b,Y,width);
    for(int px=0;px<width;++px){uint8_t y=clamp8(Y[px]);out[px*3]=out[px*3+1]=out[px*3+2]=y;}
    return true;
}

static bool robot8bw_row(const float *freq, long n, long base, double timing_scale, uint8_t *out)
{
    return robot8bw_row_at(freq, n, base, timing_scale, NULL, out);
}

/* Robot 36, first half: luminance Y[320] and the row's own chroma C[320]
 * (R-Y or B-Y, centred on 0) of the line whose sync is at base. */
static bool robot36_row_extract(const float *freq, long n, long base, double scale, float *Yr, float *Cr)
{
    const int width=320;
    float c160[160];
    long ya=base+llround(.012*SR*scale),yb=base+llround(.100*SR*scale),ca=base+llround(.106*SR*scale),cb=base+llround(.150*SR*scale);
    if(base<0||cb>=n) return false;
    segment(freq,n,ya,yb,Yr,width);segment(freq,n,ca,cb,c160,160);
    for(int p=0;p<width;++p){float u=p*159.0f/319.0f;int j=(int)u;float t=u-j;Cr[p]=((j<159)?c160[j]+(c160[j+1]-c160[j])*t:c160[159])-128;}
    return true;
}

/* Robot 36 separator after Y: >0 for R-Y (1500 Hz), <0 for B-Y (2300 Hz),
 * clipped to +/-400 Hz.  Returns false if not measurable. */
static bool robot36_separator(const float *freq, long n, long base, double scale, double *d_out)
{
    long sa=base+llround(.1005*SR*scale), sb=base+llround(.1040*SR*scale);
    if(sa<0||sb>=n||sb<=sa) return false;
    double f=0.0;
    for(long i=sa;i<sb;++i) f+=freq[i];
    f/=(double)(sb-sa);
    double d=1900.0-f;              /* >0: R-Y separator (1500 Hz) */
    if(d>400.0) d=400.0;
    if(d<-400.0) d=-400.0;
    *d_out=d;
    return true;
}

/* Robot 36, second half: RGB of one row from its Y, its own chroma, and the
 * chroma of the neighbouring rows (prev/next may be NULL). */
static void robot36_row_rgb(const float *Yr, const float *Ccur, const float *Cprev, const float *Cnext,
                            bool cur_is_cr, uint8_t *out)
{
    for(int p=0;p<320;++p){
        float cr,cb;
        if(cur_is_cr){cr=Ccur[p];cb=(Cprev&&Cnext)?.5f*(Cprev[p]+Cnext[p]):(Cprev?Cprev[p]:Ccur[p]);}
        else{cb=Ccur[p];cr=(Cprev&&Cnext)?.5f*(Cprev[p]+Cnext[p]):(Cnext?Cnext[p]:Ccur[p]);}
        float y=Yr[p];
        out[p*3]=clamp8(y+1.402f*cr);out[p*3+1]=clamp8(y-.714136f*cr-.344136f*cb);out[p*3+2]=clamp8(y+1.772f*cb);
    }
}

/* Scottie: base = the regular sync between B and R; G and B precede it,
 * R follows.  out: 320*3. */
static bool scottie_row(const float *freq, long n, long base, double scale, float channel_ms, uint8_t *out)
{
    const int width=320;
    const float sync_ms=9.0f,sep_ms=1.5f;
    float G[320],B[320],R[320];
    long ba=base-llround(channel_ms*SR/1000.0*scale),bb=base;long gb=ba-llround(sep_ms*SR/1000.0*scale),ga=gb-llround(channel_ms*SR/1000.0*scale);long ra=base+llround((sync_ms+sep_ms)*SR/1000.0*scale),rb=ra+llround(channel_ms*SR/1000.0*scale);
    if(ga<0||rb>=n) return false;
    segment(freq,n,ga,gb,G,width);segment(freq,n,ba,bb,B,width);segment(freq,n,ra,rb,R,width);
    for(int px=0;px<width;++px){out[px*3]=clamp8(R[px]);out[px*3+1]=clamp8(G[px]);out[px*3+2]=clamp8(B[px]);}
    return true;
}

/* Sync candidates before the end of the VIS cannot be picture lines: VIS
 * bit transitions (1100 <-> 1300 Hz) look like 1200-Hz pulses.  When they
 * entered the chain they bent the regression (a real Martin M1 recording
 * came out with a slant the line syncs do not have). */
static int drop_before(long *cand, int nc, long limit)
{
    int k = 0;
    for (int i = 0; i < nc; ++i) if (cand[i] >= limit) cand[k++] = cand[i];
    return k;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Iterative robust line fit position = intercept + spl * line.  Points
 * farther than max(3 ms, 3 x median residual) are dropped and the line is
 * refitted, until nothing changes (at most 5 passes, at least 8 points
 * kept).  intercept and spl must hold an initial fit.  Returns points used. */
static int robust_refit(const double *lx, const double *py, int npts, double *intercept, double *spl)
{
    if (npts < 8 || npts > 300) return npts;
    double res[300];
    bool keep[300];
    int used = npts;
    for (int i = 0; i < npts; ++i) keep[i] = true;
    for (int pass = 0; pass < 5; ++pass) {
        int m = 0;
        for (int i = 0; i < npts; ++i)
            if (keep[i]) res[m++] = fabs(py[i] - (*intercept + *spl * lx[i]));
        qsort(res, (size_t)m, sizeof(double), cmp_double);
        double med = res[m / 2];
        double lim = 3.0 * med;
        if (lim < 0.003 * SR) lim = 0.003 * SR;
        double sx=0, sy=0, sxx=0, sxy=0;
        int n2 = 0;
        bool changed = false;
        for (int i = 0; i < npts; ++i) {
            bool k = fabs(py[i] - (*intercept + *spl * lx[i])) <= lim;
            if (k != keep[i]) changed = true;
            keep[i] = k;
            if (!k) continue;
            sx += lx[i]; sy += py[i]; sxx += lx[i] * lx[i]; sxy += lx[i] * py[i]; ++n2;
        }
        if (n2 < 8) break;
        double den = (double)n2 * sxx - sx * sx;
        if (fabs(den) < 1e-9) break;
        *spl = ((double)n2 * sxy - sx * sy) / den;
        *intercept = (sy - *spl * sx) / (double)n2;
        used = n2;
        if (!changed && pass > 0) break;
    }
    return used;
}

/* Local refinement of a fitted (straight) line grid.
 *
 * One straight line for the whole picture assumes a constant transmitter
 * clock.  Real recordings break that: in one Martin M1 the line period was
 * ~+1000 ppm for the first 90 lines and nominal afterwards, so the best
 * single line (+618 ppm) slanted both halves.  Each row is moved by the
 * MEDIAN offset between the grid and the syncs actually received within
 * +/-12 rows (at least 5 of them, each within a gate of 10% of a line); rows
 * without enough syncs nearby take the correction interpolated from their
 * neighbours.  On a regular signal the corrections are ~0. */
static void local_grid_refine(const char *tag, const long *cand, int nc, long *sync, int height, double spl)
{
    enum { W = 12, MIN_PTS = 5 };
    if (!cand || !sync || nc < MIN_PTS || height <= 0 || height > 300 || spl <= 0.0) return;
    double gate = 0.10 * spl;
    if (gate < 0.003 * SR) gate = 0.003 * SR;
    double res[300], corr[300], win[2 * W + 1];
    bool have[300], valid[300];
    for (int r = 0; r < height; ++r) { have[r] = false; valid[r] = false; corr[r] = 0.0; }
    for (int k = 0; k < nc; ++k) {
        long r = lround((double)(cand[k] - sync[0]) / spl);
        if (r < 0 || r >= height) continue;
        double e = (double)(cand[k] - sync[r]);
        if (fabs(e) <= gate && (!have[r] || fabs(e) < fabs(res[r]))) { res[r] = e; have[r] = true; }
    }
    int nvalid = 0;
    for (int r = 0; r < height; ++r) {
        int m = 0;
        for (int j = r - W; j <= r + W; ++j) if (j >= 0 && j < height && have[j]) win[m++] = res[j];
        if (m < MIN_PTS) continue;
        qsort(win, (size_t)m, sizeof(double), cmp_double);
        corr[r] = (m & 1) ? win[m / 2] : 0.5 * (win[m / 2 - 1] + win[m / 2]);
        valid[r] = true;
        ++nvalid;
    }
    if (nvalid == 0) return;
    /* fill rows without enough syncs: interpolate / extend */
    int prev = -1;
    for (int r = 0; r < height; ++r) {
        if (!valid[r]) continue;
        if (prev < 0) { for (int j = 0; j < r; ++j) corr[j] = corr[r]; }
        else for (int j = prev + 1; j < r; ++j)
            corr[j] = corr[prev] + (corr[r] - corr[prev]) * (double)(j - prev) / (double)(r - prev);
        prev = r;
    }
    for (int j = prev + 1; j < height; ++j) corr[j] = corr[prev];
    double mx = 0.0;
    for (int r = 0; r < height; ++r) {
        sync[r] += lround(corr[r]);
        if (fabs(corr[r]) > mx) mx = fabs(corr[r]);
    }
    fprintf(stderr, "SSTV: %s: local timing refinement: %d/%d rows from nearby syncs, max correction %.2f ms\n",
            tag, nvalid, height, mx * 1000.0 / SR);
}

/* Fix the integer row numbering of a fitted timing grid using the VIS.
 *
 * The sync chain gives phase and line period (slant) very precisely, but
 * which chain member is image row 0 is only a guess: a VIS bit transition
 * (1100 <-> 1300 Hz averages to 1200 Hz) can look like an extra sync one or
 * two lines early, and the first real sync can merge with the 1200-Hz VIS
 * stop bit and be lost.  The end of the VIS tells where row 0 is, so shift
 * the whole grid by the integer number of lines that brings row 0 nearest
 * to it.  Phase and slant from the syncs are left untouched.
 * expected_row0 is the sample where sync[0] should lie for image row 0. */
static void anchor_grid_to_vis(const char *tag, long *sync, int height,
                               double spl, double expected_row0)
{
    if (!sync || height <= 0 || spl <= 0.0 || expected_row0 < 0.0) return;
    long k = lround(((double)sync[0] - expected_row0) / spl);
    double resid_ms = ((double)sync[0] - (double)k * spl - expected_row0) * 1000.0 / (double)SR;
    if (labs(k) >= height) {
        fprintf(stderr, "SSTV: %s: VIS anchor ignored: grid is %ld lines away\n", tag, k);
        return;
    }
    if (fabs(resid_ms) > 0.25 * spl * 1000.0 / (double)SR)
        fprintf(stderr, "SSTV: %s: VIS anchor: sync phase differs from VIS end by %.1f ms\n", tag, resid_ms);
    if (k == 0) {
        fprintf(stderr, "SSTV: %s: row 0 confirmed by VIS (residual %+.1f ms)\n", tag, resid_ms);
        return;
    }
    long shift = llround((double)k * spl);
    for (int row = 0; row < height; ++row) sync[row] -= shift;
    fprintf(stderr, "SSTV: %s: row numbering shifted by %+ld line(s) to match VIS end (residual %+.1f ms)\n",
            tag, -k, resid_ms);
}

/* Martin family decoder. Martin M1/M2 use the same scan order:
 * 1200 Hz sync, short 1500 Hz porch, then G, B, R with 1500 Hz
 * separators. Only the channel time differs between M1 and M2. */
static int martin_decode_samples(const float *x, long n, uint8_t *rgb, sstv_mode_t mode, long vis_end)
{
    /* A Martin line starts with its sync right after the VIS stop bit. */
    const double vis_row0 = (vis_end >= 0) ? (double)vis_end : -1.0;
    const float channel_ms = (mode == SSTV_MODE_MARTIN_M1) ? 146.432f : 73.216f;
    const float line_ms    = (mode == SSTV_MODE_MARTIN_M1) ? 446.446f : 226.798f;
    const int height = 256;

    fprintf(stderr, "SSTV: Martin decoder start from RAM: %s, samples=%ld, channel=%.3f ms line=%.3f ms\n",
            mode == SSTV_MODE_MARTIN_M1 ? "M1" : "M2", n, channel_ms, line_ms);
    if (!x || n <= 0 || !rgb) return -1;
    float *freq = fm_demod(x, n);
    if (!freq) return -3;

    /* Martin horizontal sync is nominally 4.862 ms at 1200 Hz. */
    long cand[MAX_SYNCS];
    int nc = detect_sync_1200_robust(x, n, 4.862f, cand, MAX_SYNCS);
    if (vis_end >= 0) nc = drop_before(cand, nc, vis_end - (long)(0.010 * SR));
    fprintf(stderr, "SSTV: Martin: %d sync candidates\n", nc);

    /* Robust Martin timing recovery, adapted from QSSTV's syncProcessor
     * strategy: consecutive detected syncs may be separated by more than one
     * image line.  Assign an integer line spacing to each accepted sync, then
     * fit sample_position = intercept + samples_per_line * line_number.
     * Missing syncs are reconstructed from that fitted timing grid. */
    const double nominal_spl = (double)line_ms * (double)SR / 1000.0;
    const double link_tol = 0.035;       /* fractional line error for initial chain */
    const int max_gap_lines = 8;         /* tolerate several lost horizontal syncs */
    int chain_idx[256];
    int chain_line[256];
    int best_idx[256];
    int best_line[256];
    int best_count = 0, best_span = 0;

    for (int st = 0; st < nc; ++st) {
        int cc = 1;
        chain_idx[0] = st;
        chain_line[0] = 0;
        long last_pos = cand[st];
        int last_line = 0;

        for (int j = st + 1; j < nc && cc < height; ++j) {
            double delta = (double)(cand[j] - last_pos);
            int gap = (int)floor(delta / nominal_spl + 0.5);
            if (gap < 1) continue;       /* duplicate/noise before next expected line */
            if (gap > max_gap_lines) break;
            double frac = fabs((double)gap - delta / nominal_spl);
            if (frac < link_tol) {
                last_line += gap;
                if (last_line >= height) break;
                chain_idx[cc] = j;
                chain_line[cc] = last_line;
                ++cc;
                last_pos = cand[j];
            }
        }

        int span = chain_line[cc - 1];
        /* Prefer a chain covering most of the image; candidate count breaks ties. */
        if (span > best_span || (span == best_span && cc > best_count)) {
            best_span = span;
            best_count = cc;
            memcpy(best_idx, chain_idx, (size_t)cc * sizeof(best_idx[0]));
            memcpy(best_line, chain_line, (size_t)cc * sizeof(best_line[0]));
        }
    }

    if (best_count < 4 || best_span < 4) {
        /* VIS already told us the mode.  With badly damaged syncs, do not
         * discard the picture: synthesize two timing anchors at the nominal
         * period.  This deliberately sacrifices slant correction but still
         * lets the demodulator render a degraded 256-line image. */
        double first = (vis_row0 >= 0.0) ? vis_row0 : 1.93 * (double)SR;
        if (nc > 0) {
            long best=cand[0]; double bd=fabs((double)best-first);
            for (int k=1;k<nc;++k) { double d=fabs((double)cand[k]-first); if(d<bd){bd=d;best=cand[k];} }
            if (bd <= nominal_spl*0.75) first=(double)best;
        }
        cand[0]=(long)llround(first);
        cand[1]=(long)llround(first+nominal_spl);
        best_idx[0]=0; best_line[0]=0; best_idx[1]=1; best_line[1]=1;
        best_count=2; best_span=1;
        fprintf(stderr, "SSTV: Martin: FALLBACK nominal timing; rendering degraded image (sync points=%d)\n", nc);
    } else if (best_count < 20 || best_span < 20) {
        fprintf(stderr, "SSTV: Martin: weak sync chain accepted (points=%d span=%d); rendering anyway\n", best_count,best_span);
    }

    fprintf(stderr, "SSTV: Martin: robust chain points=%d, line span=%d, first=%.3f s\n",
            best_count, best_span + 1, (float)cand[best_idx[0]]/(float)SR);

    /* Linear regression exactly in the spirit of QSSTV slantAdjust(): use
     * detected sync position as Y and assigned image line as X. */
    double sx=0.0, sy=0.0, sxx=0.0, sxy=0.0;
    for (int k=0; k<best_count; ++k) {
        double lx=(double)best_line[k];
        double py=(double)cand[best_idx[k]];
        sx += lx; sy += py; sxx += lx*lx; sxy += lx*py;
    }
    double den=(double)best_count*sxx-sx*sx;
    if (fabs(den) < 1e-9) { free(freq); return -6; }
    double fitted_spl=((double)best_count*sxy-sx*sy)/den;
    double intercept=(sy-fitted_spl*sx)/(double)best_count;

    /* One robust refinement pass: reject syncs whose residual is too large,
     * then refit. This prevents an occasional 1200-Hz noise burst from
     * influencing the slant estimate. */
    const double residual_limit = nominal_spl * 0.08;
    sx=sy=sxx=sxy=0.0;
    int used=0;
    double max_resid=0.0;
    for (int k=0; k<best_count; ++k) {
        double lx=(double)best_line[k];
        double py=(double)cand[best_idx[k]];
        double resid=fabs(py-(intercept+fitted_spl*lx));
        if (resid > max_resid) max_resid=resid;
        if (resid > residual_limit) continue;
        sx += lx; sy += py; sxx += lx*lx; sxy += lx*py; ++used;
    }
    if (used >= 12) {
        den=(double)used*sxx-sx*sx;
        if (fabs(den) > 1e-9) {
            fitted_spl=((double)used*sxy-sx*sy)/den;
            intercept=(sy-fitted_spl*sx)/(double)used;
        }
        /* then the iterative robust pass (3 ms / 3 x median residual) */
        double lxa[256], pya[256];
        for (int k=0; k<best_count; ++k) { lxa[k]=(double)best_line[k]; pya[k]=(double)cand[best_idx[k]]; }
        used = robust_refit(lxa, pya, best_count, &intercept, &fitted_spl);
    }

    /* Optional second pass over all sync candidates.  This does not replace
     * the robust chain: it only lengthens its baseline when the initial lock
     * is early and trustworthy. */
    global_sync_refine("Martin",cand,nc,height,nominal_spl,best_count,best_span,
                       &intercept,&fitted_spl);

    double fitted_ms=fitted_spl*1000.0/(double)SR;
    double clock_err_ppm=(fitted_spl/nominal_spl-1.0)*1000000.0;

    /* A very short chain can fit beautifully while still describing noise.
     * In that case do not let it invent a large clock correction.  Keep its
     * phase/anchor information, but use the VIS mode's nominal line period. */
    if ((best_count < 10 || best_span < 10) && fabs(clock_err_ppm) > 500.0) {
        double mean_intercept=0.0;
        fprintf(stderr, "SSTV: Martin: weak-chain clock %+.1f ppm is unreliable; using nominal line period\n",
                clock_err_ppm);
        fitted_spl=nominal_spl;
        for (int k=0;k<best_count;++k)
            mean_intercept += (double)cand[best_idx[k]] - fitted_spl*(double)best_line[k];
        intercept=mean_intercept/(double)best_count;
        fitted_ms=fitted_spl*1000.0/(double)SR;
        clock_err_ppm=0.0;
    }

    fprintf(stderr, "SSTV: Martin: nominal line=%.3f ms, fitted line=%.6f ms\n",
            line_ms, fitted_ms);
    fprintf(stderr, "SSTV: Martin: clock error=%+.1f ppm, regression syncs=%d/%d, max initial residual=%.2f samples\n",
            clock_err_ppm, used, best_count, max_resid);

    /* Reconstruct the complete 256-line timing grid. The chain's first sync
     * is image line zero; lines whose sync pulse was lost are still decoded
     * from the regression prediction. */
    long sync[256];
    int ns=height;
    for (int row=0; row<height; ++row)
        sync[row]=(long)llround(intercept+fitted_spl*(double)row);

    fprintf(stderr, "SSTV: Martin: reconstructed timing grid rows=%d, first=%.3f s, last=%.3f s\n",
            ns, (float)sync[0]/(float)SR, (float)sync[height-1]/(float)SR);

    /* Same late-sparse-chain recovery proven by Scottie: a chain found in
     * the second half of the picture still gives excellent clock/slant, but
     * its first member is not image row zero.  Preserve the fitted period and
     * shift the complete grid backwards by an integer number of lines. */
    {
        const double expected_first_sec = (mode == SSTV_MODE_MARTIN_M1) ? 2.35 : 2.20;
        const double expected_first = expected_first_sec*(double)SR;
        const double late_limit = 5.0*(double)SR;
        if (vis_row0 >= 0.0) {
            anchor_grid_to_vis("Martin", sync, height, fitted_spl, vis_row0);
        } else if ((double)sync[0] > late_limit && fitted_spl > 0.0) {
            long shift_lines=(long)llround(((double)sync[0]-expected_first)/fitted_spl);
            if (shift_lines>0 && shift_lines<height) {
                long shift_samples=(long)llround((double)shift_lines*fitted_spl);
                double old_first=(double)sync[0]/(double)SR;
                for(int row=0;row<height;++row) sync[row]-=shift_samples;
                fprintf(stderr,"SSTV: Martin: late sparse sync chain: first grid point %.3f s corresponds to about image row %ld\n",
                        old_first,shift_lines);
                fprintf(stderr,"SSTV: Martin: extrapolated timing grid backward %ld lines -> first=%.3f s last=%.3f s\n",
                        shift_lines,(double)sync[0]/(double)SR,(double)sync[height-1]/(double)SR);
            }
        }
    }

    local_grid_refine("Martin", cand, nc, sync, height, fitted_spl);
    for (int row=0; row<ns; ++row) {
        double timing_scale=fitted_spl/nominal_spl;
        if (!martin_row(freq, n, sync[row], timing_scale, channel_ms, rgb + (size_t)row*320*3)) {
            fprintf(stderr, "SSTV: Martin: EOF at row %d\n", row); ns=row; break;
        }
        if ((row % 32) == 31 || row == ns-1)
            fprintf(stderr, "SSTV: Martin: decoded row %d/%d\n", row+1, ns);
    }
    if (ns < height)
        memset(rgb+(size_t)ns*320*3,0,(size_t)(height-ns)*320*3);

    free(freq);
    fprintf(stderr, "SSTV: Martin image decode finished, rows=%d\n", ns);
    return ns;
}


/* Recover a complete line timing grid from imperfect horizontal-sync candidates.
 * This follows the useful part of QSSTV syncProcessor/slantAdjust: allow integer
 * multiples of the nominal line spacing, assign line numbers, then regress real
 * sync position against line number. Missing syncs are reconstructed by the fit. */
static int robust_sync_grid(const char *tag, const long *cand, int nc, int height,
                            double nominal_spl, int max_gap_lines,
                            long *grid, double *fitted_spl_out)
{
    int chain_idx[300], chain_line[300], best_idx[300], best_line[300];
    int best_count=0, best_span=0;
    const double link_tol=0.040;
    if (height > 300) return -1;

    for (int st=0; st<nc; ++st) {
        int cc=1, last_line=0;
        long last_pos=cand[st];
        chain_idx[0]=st; chain_line[0]=0;
        for (int j=st+1; j<nc && cc<height; ++j) {
            double delta=(double)(cand[j]-last_pos);
            int gap=(int)floor(delta/nominal_spl+0.5);
            if (gap<1) continue;
            if (gap>max_gap_lines) break;
            double frac=fabs((double)gap-delta/nominal_spl);
            if (frac<link_tol) {
                last_line += gap;
                if (last_line>=height) break;
                chain_idx[cc]=j; chain_line[cc]=last_line; ++cc;
                last_pos=cand[j];
            }
        }
        int span=chain_line[cc-1];
        if (span>best_span || (span==best_span && cc>best_count)) {
            best_span=span; best_count=cc;
            memcpy(best_idx,chain_idx,(size_t)cc*sizeof(int));
            memcpy(best_line,chain_line,(size_t)cc*sizeof(int));
        }
    }
    /* Do not throw an SSTV picture away merely because RF damaged most of the
     * horizontal syncs.  A sparse but coherent chain is still useful: it gives
     * us phase and (usually) enough information to estimate slant, while the
     * known VIS mode supplies the nominal line period for all missing rows. */
    if (best_count<4 || best_span<4) {
        fprintf(stderr,"SSTV: %s: robust sync chain unavailable (points=%d span=%d)\n",tag,best_count,best_span);
        return -2;
    }
    if (best_count<10 || best_span<10)
        fprintf(stderr,"SSTV: %s: weak sync chain accepted (points=%d span=%d); image will be reconstructed anyway\n",
                tag,best_count,best_span);

    double sx=0,sy=0,sxx=0,sxy=0;
    for(int k=0;k<best_count;++k){double x=best_line[k],y=cand[best_idx[k]];sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;}
    double den=(double)best_count*sxx-sx*sx;
    if(fabs(den)<1e-9) return -3;
    double spl=((double)best_count*sxy-sx*sy)/den;
    double intercept=(sy-spl*sx)/(double)best_count;

    /* A tiny number of noisy syncs must not be allowed to invent an absurd
     * clock.  If the fitted period is implausible, keep the phase information
     * from the chain but fall back to the VIS mode's nominal line period. */
    if (fabs(spl/nominal_spl - 1.0) > 0.015) {
        double mean_intercept=0.0;
        fprintf(stderr,"SSTV: %s: sparse fit line period implausible (%.6f ms); using nominal timing\n",
                tag,spl*1000.0/(double)SR);
        spl=nominal_spl;
        for(int k=0;k<best_count;++k)
            mean_intercept += (double)cand[best_idx[k]] - spl*(double)best_line[k];
        intercept=mean_intercept/(double)best_count;
    }

    const double residual_limit=nominal_spl*0.08;
    sx=sy=sxx=sxy=0; int used=0; double max_resid=0;
    for(int k=0;k<best_count;++k){
        double x=best_line[k],y=cand[best_idx[k]];
        double r=fabs(y-(intercept+spl*x)); if(r>max_resid)max_resid=r;
        if(r>residual_limit)continue; sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;++used;
    }
    if(used>=8){den=(double)used*sxx-sx*sx;if(fabs(den)>1e-9){spl=((double)used*sxy-sx*sy)/den;intercept=(sy-spl*sx)/(double)used;}}

    /* Four or five noisy pulses can form a mathematically neat chain but give
     * a useless slant estimate (we observed this on degraded RF Scottie S2).
     * For a weak chain only, reject clock corrections above 500 ppm while
     * retaining the chain's phase. Strong chains are never changed here. */
    if(best_count<10 && fabs(spl/nominal_spl-1.0)*1000000.0 > 500.0){
        double mean_intercept=0.0;
        fprintf(stderr,"SSTV: %s: weak-chain clock %+.1f ppm is unreliable; using nominal line period\n",
                tag,(spl/nominal_spl-1.0)*1000000.0);
        spl=nominal_spl;
        for(int k=0;k<best_count;++k) mean_intercept+=(double)cand[best_idx[k]]-spl*(double)best_line[k];
        intercept=mean_intercept/(double)best_count;
    }

    /* Same conservative global refinement for Robot/Scottie users of this
     * common timing engine.  Strong full-span chains normally remain exactly
     * as they are because there is no meaningful span to extend. */
    global_sync_refine(tag,cand,nc,height,nominal_spl,best_count,best_span,
                       &intercept,&spl);

    for(int row=0;row<height;++row) grid[row]=(long)llround(intercept+spl*(double)row);
    fprintf(stderr,"SSTV: %s: robust chain points=%d span=%d, regression syncs=%d/%d\n",tag,best_count,best_span+1,used,best_count);
    fprintf(stderr,"SSTV: %s: nominal line=%.6f ms, fitted line=%.6f ms, clock error=%+.1f ppm\n",
            tag,nominal_spl*1000.0/(double)SR,spl*1000.0/(double)SR,(spl/nominal_spl-1.0)*1000000.0);
    fprintf(stderr,"SSTV: %s: reconstructed timing grid rows=%d, first=%.3f s, last=%.3f s\n",
            tag,height,(double)grid[0]/SR,(double)grid[height-1]/SR);
    if(fitted_spl_out)*fitted_spl_out=spl;
    return height;
}

/* Robot 72 RAM decoder.  Uses the same timing philosophy as Scottie/Martin:
 * robust regression when sync is usable, late-chain back extrapolation when
 * the useful syncs occur deep in the picture, and nominal timing as the final
 * fallback after a valid VIS. */
/* Robot 72 sends a complete Y, R-Y and B-Y set on every line.
 * Nominal line timing is exactly 300 ms:
 *   sync 9, porch 3, Y 138,
 *   separator 4.5 + porch 1.5, R-Y 69,
 *   separator 4.5 + porch 1.5, B-Y 69. */
static int robot72_decode_samples(const float *x,long n,uint8_t *rgb,long vis_end)
{
    /* A Robot line starts with its sync right after the VIS stop bit. */
    const double vis_row0 = (vis_end >= 0) ? (double)vis_end : -1.0;
    const int width=320,height=240;
    const float line_ms=300.000f;
    if(!x||n<=0||!rgb)return -1;
    fprintf(stderr,"SSTV: Robot 72 decoder start from RAM: samples=%ld, 320x240, line=%.3f ms\n",n,line_ms);

    float *freq = fm_demod(x, n);
    if (!freq) return -3;

    long cand[MAX_SYNCS];
    int nc=detect_sync_1200_robust(x,n,9.0f,cand,MAX_SYNCS);
    if(vis_end>=0) nc=drop_before(cand,nc,vis_end-(long)(0.010*SR));
    fprintf(stderr,"SSTV: Robot 72: %d sync candidates\n",nc);

    long sync[240];double fitted_spl=0.0,nominal_spl=(double)line_ms*(double)SR/1000.0;
    int grid_rc=robust_sync_grid("Robot 72",cand,nc,height,nominal_spl,8,sync,&fitted_spl);
    if(grid_rc<0){
        double first=(vis_row0>=0.0)?vis_row0:2.0*(double)SR; fitted_spl=nominal_spl;
        if(nc>0){long best=cand[0];double bd=fabs((double)best-first);for(int k=1;k<nc;++k){double d=fabs((double)cand[k]-first);if(d<bd){bd=d;best=cand[k];}}if(bd<=nominal_spl*0.75)first=(double)best;}
        for(int row=0;row<height;++row)sync[row]=(long)llround(first+nominal_spl*(double)row);
        fprintf(stderr,"SSTV: Robot 72: FALLBACK nominal timing grid rows=%d, first=%.3f s; rendering degraded image\n",height,(double)sync[0]/SR);
    } else if (vis_row0 >= 0.0) {
        anchor_grid_to_vis("Robot 72",sync,height,fitted_spl,vis_row0);
    } else if ((double)sync[0] > 5.0*(double)SR && fitted_spl>0.0) {
        const double expected_first=2.0*(double)SR;
        long shift_lines=(long)llround(((double)sync[0]-expected_first)/fitted_spl);
        if(shift_lines>0&&shift_lines<height){long ds=(long)llround((double)shift_lines*fitted_spl);double old=(double)sync[0]/SR;for(int r=0;r<height;++r)sync[r]-=ds;fprintf(stderr,"SSTV: Robot 72: late sparse sync chain: first %.3f s corresponds to about row %ld\n",old,shift_lines);fprintf(stderr,"SSTV: Robot 72: extrapolated backward %ld lines -> first=%.3f s last=%.3f s\n",shift_lines,(double)sync[0]/SR,(double)sync[height-1]/SR);}
    }

    double scale=fitted_spl/nominal_spl;
    local_grid_refine("Robot 72",cand,nc,sync,height,fitted_spl);
    int ns=height;
    for(int row=0;row<height;++row){
        if(!robot72_row(freq,n,sync[row],scale,rgb+(size_t)row*width*3)){ns=row;fprintf(stderr,"SSTV: Robot 72: bounds/EOF at row %d\n",row);break;}
        if((row%32)==31||row==height-1)fprintf(stderr,"SSTV: Robot 72: decoded row %d/%d\n",row+1,height);
    }
    if(ns<height)memset(rgb+(size_t)ns*width*3,0,(size_t)(height-ns)*width*3);
    free(freq);fprintf(stderr,"SSTV: Robot 72 image decode finished, rows=%d\n",ns);return ns;
}

/* Robot B/W 8: 160x120 monochrome, VIS 2, 900 lines/minute.
 * One line is 66.667 ms: 1200 Hz sync immediately followed by luminance
 * (1500 Hz black .. 2300 Hz white), no porch.  Published sync: 10 ms
 * (SSTV Handbook, table 4.1); 1KO125 up to beta 3 sent 5 ms.  Syncs are
 * searched with a 5 ms window (it fits both); the sync edges are then
 * measured on every row and their median is used for the whole picture,
 * so both variants render with the video starting right after the sync. */
static int robot8bw_decode_samples(const float *x, long n, uint8_t *rgb, long vis_end)
{
    /* A Robot line starts with its sync right after the VIS stop bit. */
    const double vis_row0 = (vis_end >= 0) ? (double)vis_end : -1.0;
    const int width = 160, height = 120;
    const float line_ms = 1000.0f / 15.0f; /* 900 lpm */
    const float sync_ms = 5.0f;           /* detector window: fits 5 and 10 ms syncs */
    if (!x || n <= 0 || !rgb) return -1;
    fprintf(stderr, "SSTV: Robot 8 BW decoder start from RAM: samples=%ld, 160x120, line=%.3f ms\n", n, line_ms);

    float *freq = fm_demod(x, n);
    if (!freq) return -3;

    long cand[MAX_SYNCS];
    int nc=detect_sync_1200_robust(x,n,sync_ms,cand,MAX_SYNCS);
    if(vis_end>=0) nc=drop_before(cand,nc,vis_end-(long)(0.010*SR));
    fprintf(stderr,"SSTV: Robot 8 BW: %d sync candidates\n",nc);

    long sync[120]; double fitted_spl=0.0;
    double nominal_spl=(double)line_ms*(double)SR/1000.0;
    int grid_rc=robust_sync_grid("Robot 8 BW",cand,nc,height,nominal_spl,8,sync,&fitted_spl);
    if(grid_rc<0){
        double first=(vis_row0>=0.0)?vis_row0:2.0*(double)SR; fitted_spl=nominal_spl;
        if(nc>0){long best=cand[0];double bd=fabs((double)best-first);for(int k=1;k<nc;++k){double d=fabs((double)cand[k]-first);if(d<bd){bd=d;best=cand[k];}}if(bd<=nominal_spl)first=(double)best;}
        for(int row=0;row<height;++row)sync[row]=(long)llround(first+nominal_spl*(double)row);
        fprintf(stderr,"SSTV: Robot 8 BW: FALLBACK nominal timing grid; rendering degraded image\n");
    } else if (vis_row0 >= 0.0) {
        anchor_grid_to_vis("Robot 8 BW",sync,height,fitted_spl,vis_row0);
    } else if ((double)sync[0] > 5.0*(double)SR && fitted_spl>0.0) {
        const double expected_first=2.0*(double)SR;
        long shift_lines=(long)llround(((double)sync[0]-expected_first)/fitted_spl);
        if(shift_lines>0 && shift_lines<height){long ds=(long)llround((double)shift_lines*fitted_spl);double old=(double)sync[0]/SR;for(int r=0;r<height;++r)sync[r]-=ds;fprintf(stderr,"SSTV: Robot 8 BW: late sparse sync chain %.3f s -> shift back %ld lines, first=%.3f s\n",old,shift_lines,(double)sync[0]/SR);}
    }
    local_grid_refine("Robot 8 BW",cand,nc,sync,height,fitted_spl);
    int ns=height;
    double timing_scale=fitted_spl/nominal_spl;

    /* Sync edges relative to each row's anchor: median over the picture. */
    double edges[2] = { 0.0, 10.0 * timing_scale };
    {
        double st_ms[120], en_ms[120], len_ms[120];
        int ne = 0;
        for (int row = 0; row < height; ++row) {
            double s0, e0;
            if (robot8bw_sync_edges(freq, n, sync[row], timing_scale, &s0, &e0)) {
                st_ms[ne] = s0; en_ms[ne] = e0; len_ms[ne] = e0 - s0; ++ne;
            }
        }
        if (ne >= 10) {
            qsort(st_ms, ne, sizeof(double), cmp_double);
            qsort(en_ms, ne, sizeof(double), cmp_double);
            qsort(len_ms, ne, sizeof(double), cmp_double);
            edges[0] = st_ms[ne / 2];
            edges[1] = en_ms[ne / 2];
            fprintf(stderr, "SSTV: Robot 8 BW: sync %.1f ms (median of %d rows), video from %+.2f ms\n",
                    len_ms[ne / 2] / timing_scale, ne, edges[1]);
        } else {
            fprintf(stderr, "SSTV: Robot 8 BW: sync edges not measurable (%d rows), nominal 10 ms\n", ne);
        }
    }

    /* Each row is rendered from its own measured pulse when that agrees with
     * the median (the 5 ms detector can wander inside a 10 ms pulse under
     * noise); otherwise from the median edges. */
    for(int row=0;row<ns;++row){
        double re[2];
        const double *use = edges;
        if (robot8bw_sync_edges(freq, n, sync[row], timing_scale, &re[0], &re[1]) &&
            fabs((re[1] - re[0]) - (edges[1] - edges[0])) < 1.0 &&
            fabs(0.5 * (re[0] + re[1]) - 0.5 * (edges[0] + edges[1])) < 6.0)
            use = re;
        if(!robot8bw_row_at(freq,n,sync[row],timing_scale,use,rgb+(size_t)row*width*3)){ns=row;break;}
        if((row%20)==19 || row==ns-1) fprintf(stderr,"SSTV: Robot 8 BW: decoded row %d/%d\n",row+1,ns);
    }
    if(ns<height) memset(rgb+(size_t)ns*width*3,0,(size_t)(height-ns)*width*3);
    free(freq);
    fprintf(stderr,"SSTV: Robot 8 BW image decode finished, rows=%d\n",ns);
    return ns;
}


static int robot36_decode_samples(const float *x,long n,uint8_t *rgb,long vis_end)
{
    /* A Robot line starts with its sync right after the VIS stop bit. */
    const double vis_row0 = (vis_end >= 0) ? (double)vis_end : -1.0;
    const int width=320,height=240; const float line_ms=150.0f;
    if(!x||n<=0||!rgb)return -1;
    fprintf(stderr,"SSTV: Robot 36 decoder start from RAM: samples=%ld, 320x240, line=%.3f ms\n",n,line_ms);
    float *freq = fm_demod(x, n);
    if (!freq) return -3;
    long cand[MAX_SYNCS];
    int nc=detect_sync_1200_robust(x,n,9.0f,cand,MAX_SYNCS);
    if(vis_end>=0) nc=drop_before(cand,nc,vis_end-(long)(0.010*SR));
    fprintf(stderr,"SSTV: Robot 36: %d sync candidates\n",nc);
    long sync[240];double fitted_spl=0,nominal_spl=(double)line_ms*SR/1000.0;
    int grid_rc=robust_sync_grid("Robot 36",cand,nc,height,nominal_spl,8,sync,&fitted_spl);
    if(grid_rc<0){
        double first=(vis_row0>=0.0)?vis_row0:2.0*(double)SR; fitted_spl=nominal_spl;
        if(nc>0){long best=cand[0];double bd=fabs((double)best-first);for(int k=1;k<nc;++k){double d=fabs((double)cand[k]-first);if(d<bd){bd=d;best=cand[k];}}if(bd<=nominal_spl)first=(double)best;}
        for(int row=0;row<height;++row)sync[row]=(long)llround(first+nominal_spl*(double)row);
        fprintf(stderr,"SSTV: Robot 36: FALLBACK nominal timing grid; rendering degraded image\n");
    } else if (vis_row0 >= 0.0) {
        anchor_grid_to_vis("Robot 36",sync,height,fitted_spl,vis_row0);
    } else if ((double)sync[0] > 5.0*(double)SR && fitted_spl>0.0) {
        const double expected_first=2.0*(double)SR;
        long shift_lines=(long)llround(((double)sync[0]-expected_first)/fitted_spl);
        if(shift_lines>0 && shift_lines<height){long ds=(long)llround((double)shift_lines*fitted_spl);double old=(double)sync[0]/SR;for(int r=0;r<height;++r)sync[r]-=ds;fprintf(stderr,"SSTV: Robot 36: late sparse sync chain %.3f s -> shift back %ld lines, first=%.3f s\n",old,shift_lines,(double)sync[0]/SR);}
    }
    double scale=fitted_spl/nominal_spl; float *Y=calloc((size_t)height*width,sizeof(float)),*C=calloc((size_t)height*width,sizeof(float));if(!Y||!C){free(Y);free(C);free(freq);return -6;}
    local_grid_refine("Robot 36",cand,nc,sync,height,fitted_spl);
    int ns=height;
    for(int r=0;r<height;++r){if(!robot36_row_extract(freq,n,sync[r],scale,&Y[r*width],&C[r*width])){ns=r;break;}}
    /* Chroma order from the 4.5-ms separator after Y: 1500 Hz announces R-Y
     * ("even" line), 2300 Hz announces B-Y ("odd" line).  The row index is
     * NOT reliable for this: a Manual capture can start on any line, and the
     * timing grid itself may be offset by a line.  So the phase of the
     * alternation is measured over ALL rows: sum the separator offset from
     * 1900 Hz with alternating sign (clipped to +/-400 Hz against impulse
     * noise).  This is a coherent integration over ~240 rows, far more robust
     * than per-row decisions, and strict alternation is then imposed so
     * noisy separators cannot flip single rows.  If nothing can be measured,
     * keep the previous convention (odd rows = R-Y), see below. */
    int cr_phase=1;  /* row r carries R-Y when ((r+cr_phase)&1)==0 */
    {
        double acc=0.0; int used=0;
        for(int r=0;r<ns;++r){
            double d;
            if(!robot36_separator(freq,n,sync[r],scale,&d)) continue;
            acc += (r&1) ? -d : d;          /* >0: R-Y on even rows */
            ++used;
        }
        /* Some transmitters do not alternate the separator at all (a real
         * recording gave +2 / -0 Hz per row): then the sign is noise and
         * must not choose the colours.  Below 100 Hz per row on average (pure
         * noise gave 56; real separators 280, synthetic 300-400) the
         * previous convention is kept; it is also what a recording with a
         * clear separator (-280 Hz per row) showed. */
        bool measurable = used>0 && fabs(acc/used) >= 100.0;
        if(measurable && acc>0.0) cr_phase=0;
        fprintf(stderr,"SSTV: Robot 36: separator phase %+.0f Hz/row over %d rows -> R-Y on %s rows%s\n",
                used?acc/used:0.0,used,cr_phase?"odd":"even",
                measurable?"":" (not measurable: previous convention)");
    }
    for(int r=0;r<ns;++r)robot36_row_rgb(&Y[r*width],&C[r*width],r>0?&C[(r-1)*width]:NULL,r+1<ns?&C[(r+1)*width]:NULL,((r+cr_phase)&1)==0,rgb+(size_t)r*width*3);
    if(ns<height)memset(rgb+(size_t)ns*width*3,0,(size_t)(height-ns)*width*3);
    fprintf(stderr,"SSTV: Robot 36 image decode finished, rows=%d\n",ns);free(Y);free(C);free(freq);return ns;
}

/* Scottie family decoder.  Unlike Martin, the regular 1200 Hz sync pulse
 * sits between Blue and Red.  For S1 a regular line is:
 *   1500 sep, G, 1500 sep, B, 1200 sync, 1500 porch, R
 * The image begins with one extra 9 ms starting sync before the first G.
 * We anchor each decoded row on the regular sync between B and R: G/B are
 * immediately before it and R is immediately after it. */
static int scottie_decode_samples(const float *x,long n,uint8_t *rgb,sstv_mode_t mode,long vis_end)
{
    const int width=320,height=256;
    const float sync_ms=9.0f,sep_ms=1.5f;
    float channel_ms; const char *mode_name;
    switch(mode){
        case SSTV_MODE_SCOTTIE_S1: channel_ms=138.240f; mode_name="Scottie S1"; break;
        case SSTV_MODE_SCOTTIE_S2: channel_ms=88.064f; mode_name="Scottie S2"; break;
        case SSTV_MODE_SCOTTIE_DX: channel_ms=345.600f; mode_name="Scottie DX"; break;
        default: return -20;
    }
    const float line_ms=sync_ms+3.0f*sep_ms+3.0f*channel_ms;
    /* Rows are anchored on the regular sync between B and R.  After the VIS
     * come the 9-ms starting sync, separator, G, separator, B: that is where
     * the sync of row 0 lies. */
    const double vis_row0 = (vis_end >= 0)
        ? (double)vis_end + (double)(sync_ms+2.0f*sep_ms+2.0f*channel_ms)*SR/1000.0 : -1.0;
    if(!x||n<=0||!rgb)return -1;fprintf(stderr,"SSTV: %s decoder start from RAM: samples=%ld, line=%.3f ms\n",mode_name,n,line_ms);
    float *freq = fm_demod(x, n);
    if (!freq) return -3;
    long cand[MAX_SYNCS];
    int nc=detect_sync_1200_robust(x,n,sync_ms,cand,MAX_SYNCS);
    if(vis_end>=0) nc=drop_before(cand,nc,vis_end-(long)(0.010*SR));
    fprintf(stderr,"SSTV: %s: %d sync candidates\n",mode_name,nc);
    long sync[256];
    double fitted_spl=0,nominal_spl=(double)line_ms*SR/1000.0;
    if(robust_sync_grid(mode_name,cand,nc,height,nominal_spl,8,sync,&fitted_spl)<0){
        /* Last-resort live-RX fallback.  The capture contains a two-second
         * prebuffer and starts immediately after a valid VIS code, so for the
         * Scottie family the first regular B/R sync is normally about 2.23 s
         * into this RAM block (2.229 s in our validated S1 reception).  Even
         * with no usable horizontal-sync chain, render all 256 rows using the
         * nominal VIS timing.  The result may be noisy/slanted/mis-phased, but
         * the user gets a picture instead of an empty screen. */
        double first=(vis_row0>=0.0)?vis_row0:(2.229*(double)SR);
        fitted_spl=nominal_spl;
        if(nc>0){
            /* Prefer a real 1200-Hz candidate near the expected first sync,
             * but never jump by more than half a line. */
            long best=cand[0]; double bd=fabs((double)best-first);
            for(int k=1;k<nc;++k){double d=fabs((double)cand[k]-first);if(d<bd){bd=d;best=cand[k];}}
            if(bd <= nominal_spl*0.5) first=(double)best;
        }
        for(int row=0;row<height;++row) sync[row]=(long)llround(first+nominal_spl*(double)row);
        fprintf(stderr,"SSTV: %s: FALLBACK nominal timing grid rows=%d, first=%.3f s; rendering degraded image\n",
                mode_name,height,(double)sync[0]/(double)SR);
    } else {
        /* A sparse Scottie chain can begin well inside the picture.
         * robust_sync_grid() correctly recovers its line period but numbers
         * the first member of that chain as row 0.  Do not disturb normal
         * early/strong locks; only when the reconstructed row 0 is clearly
         * late, shift the whole timing grid backwards by an integer number
         * of fitted lines.  This preserves phase and fitted clock/slant. */
        const double expected_first = 2.229 * (double)SR;
        const double late_limit = 5.0 * (double)SR;
        if (vis_row0 >= 0.0) {
            anchor_grid_to_vis(mode_name, sync, height, fitted_spl, vis_row0);
        } else if ((double)sync[0] > late_limit && fitted_spl > 0.0) {
            long shift_lines = (long)llround(((double)sync[0] - expected_first) / fitted_spl);
            if (shift_lines > 0 && shift_lines < height) {
                long shift_samples = (long)llround((double)shift_lines * fitted_spl);
                double old_first = (double)sync[0] / (double)SR;
                for (int row=0; row<height; ++row) sync[row] -= shift_samples;
                fprintf(stderr,"SSTV: %s: late sparse sync chain: first grid point %.3f s corresponds to about image row %ld\n",
                        mode_name,old_first,shift_lines);
                fprintf(stderr,"SSTV: %s: extrapolated timing grid backward %ld lines -> first=%.3f s last=%.3f s\n",
                        mode_name,shift_lines,(double)sync[0]/(double)SR,(double)sync[height-1]/(double)SR);
            }
        }
    }
    local_grid_refine(mode_name,cand,nc,sync,height,fitted_spl);
    double scale=fitted_spl/nominal_spl;int ns=height;
    for(int row=0;row<height;++row){if(!scottie_row(freq,n,sync[row],scale,channel_ms,rgb+(size_t)row*width*3)){ns=row;break;}if((row%32)==31||row==height-1)fprintf(stderr,"SSTV: %s: decoded row %d/%d\n",mode_name,row+1,height);}
    if(ns<height)memset(rgb+(size_t)ns*width*3,0,(size_t)(height-ns)*width*3);fprintf(stderr,"SSTV: %s image decode finished, rows=%d\n",mode_name,ns);free(freq);return ns;
}

/* ---- Progressive display support (see decoder.h) ---------------------- */

float *sstv_fm_demod(const float *x, long n)
{
    return fm_demod(x, n);
}

int sstv_row_width(sstv_mode_t mode)
{
    switch (mode) {
    case SSTV_MODE_ROBOT8_BW: return 160;
    case SSTV_MODE_ROBOT36: case SSTV_MODE_ROBOT72:
    case SSTV_MODE_MARTIN_M1: case SSTV_MODE_MARTIN_M2:
    case SSTV_MODE_SCOTTIE_S1: case SSTV_MODE_SCOTTIE_S2: case SSTV_MODE_SCOTTIE_DX: return 320;
    default: return 0;
    }
}

static float scottie_channel_ms(sstv_mode_t mode)
{
    switch (mode) {
    case SSTV_MODE_SCOTTIE_S1: return 138.240f;
    case SSTV_MODE_SCOTTIE_S2: return 88.064f;
    case SSTV_MODE_SCOTTIE_DX: return 345.600f;
    default: return 0.0f;
    }
}

double sstv_row0_anchor_after_vis(sstv_mode_t mode)
{
    float ch = scottie_channel_ms(mode);
    if (ch > 0.0f) return (9.0 + 2.0 * 1.5 + 2.0 * ch) / 1000.0;   /* as scottie_decode_samples() */
    return sstv_row_width(mode) > 0 ? 0.0 : -1.0;
}

int sstv_render_row(sstv_mode_t mode, const float *freq, long n, long base, double scale,
                    sstv_row_state_t *st, uint8_t *rgb_row)
{
    if (!freq || n <= 0 || !rgb_row || scale <= 0.0) return -1;
    switch (mode) {
    case SSTV_MODE_ROBOT8_BW:
        return robot8bw_row(freq, n, base, scale, rgb_row) ? 0 : -2;
    case SSTV_MODE_ROBOT72:
        return robot72_row(freq, n, base, scale, rgb_row) ? 0 : -2;
    case SSTV_MODE_MARTIN_M1:
        return martin_row(freq, n, base, scale, 146.432f, rgb_row) ? 0 : -2;
    case SSTV_MODE_MARTIN_M2:
        return martin_row(freq, n, base, scale, 73.216f, rgb_row) ? 0 : -2;
    case SSTV_MODE_SCOTTIE_S1: case SSTV_MODE_SCOTTIE_S2: case SSTV_MODE_SCOTTIE_DX:
        return scottie_row(freq, n, base, scale, scottie_channel_ms(mode), rgb_row) ? 0 : -2;
    case SSTV_MODE_ROBOT36: {
        if (!st) return -1;
        float Y[320], C[320];
        if (!robot36_row_extract(freq, n, base, scale, Y, C)) return -2;
        /* Live: the row's own separator says R-Y or B-Y (the final decode
         * uses the phase measured over the whole picture instead); the
         * missing component comes from the previous row. */
        double d;
        bool is_cr;
        if (robot36_separator(freq, n, base, scale, &d) && d != 0.0) is_cr = d > 0.0;
        else is_cr = st->chroma_valid ? !st->chroma_is_cr : true;
        const float *other = (st->chroma_valid && (st->chroma_is_cr != 0) != is_cr) ? st->chroma : NULL;
        /* robot36_row_rgb() takes the missing component of an R-Y row from
         * the previous row and of a B-Y row from the next one; live, the
         * only neighbour available is the previous row, so pass it in the
         * slot that the row type uses. */
        robot36_row_rgb(Y, C, is_cr ? other : NULL, is_cr ? NULL : other, is_cr, rgb_row);
        memcpy(st->chroma, C, sizeof(C));
        st->chroma_valid = 1;
        st->chroma_is_cr = is_cr;
        return 0;
    }
    default:
        return -1;
    }
}

static int decode_dispatch(const float *samples, long count, sstv_mode_t mode, uint8_t *rgb,
                           sstv_info_t *info, long vis_end)
{
    if (!samples || count <= 0 || !rgb) return -1;
    const mode_desc_t *m=NULL;
    for (unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i) if (modes[i].mode==mode) { m=&modes[i]; break; }
    if (!m) return -2;
    if (info) { info->mode=m->mode; info->vis=m->vis; info->name=m->name; info->width=m->w; info->height=m->h; }
    fprintf(stderr,"SSTV LIVE: direct RAM decoder dispatch -> %s (%ld samples @ %d Hz)\n",m->name,count,SR);
    if (mode==SSTV_MODE_ROBOT8_BW)
        return robot8bw_decode_samples(samples,count,rgb,vis_end);
    if (mode==SSTV_MODE_ROBOT36)
        return robot36_decode_samples(samples,count,rgb,vis_end);
    if (mode==SSTV_MODE_ROBOT72)
        return robot72_decode_samples(samples,count,rgb,vis_end);
    if (mode==SSTV_MODE_MARTIN_M1 || mode==SSTV_MODE_MARTIN_M2)
        return martin_decode_samples(samples,count,rgb,mode,vis_end);
    if (mode==SSTV_MODE_SCOTTIE_S1 || mode==SSTV_MODE_SCOTTIE_S2 || mode==SSTV_MODE_SCOTTIE_DX)
        return scottie_decode_samples(samples,count,rgb,mode,vis_end);
    fprintf(stderr,"SSTV LIVE: direct RAM decoder for %s not enabled yet\n",m->name);
    return -20;
}


int sstv_decode_samples(const float *samples, long count, sstv_mode_t mode, uint8_t *rgb, sstv_info_t *info)
{
    if (!samples || count <= 0 || !rgb) return -1;
    int vis = -1;
    for (unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i) if (modes[i].mode==mode) { vis=modes[i].vis; break; }
    if (vis < 0) return -2;
    /* Auto RX and Test RX buffers contain the VIS header: locate it so the
     * decoders can number the image rows exactly (see anchor_grid_to_vis). */
    long vis_end = sstv_vis_find_end(samples, count, vis, 5L * SR);
    if (vis_end >= 0)
        fprintf(stderr, "SSTV: VIS %d located in capture: picture starts at %.3f s\n", vis, (double)vis_end / SR);
    else
        fprintf(stderr, "SSTV: VIS %d not located in capture: row 0 taken from the sync chain\n", vis);
    return decode_dispatch(samples, count, mode, rgb, info, vis_end);
}

int sstv_decode_cont_samples(const float *samples, long count, sstv_mode_t mode, uint8_t *rgb, sstv_info_t *info)
{
    if (!samples || count <= 0 || !rgb) return -1;

    /* Keep Auto RX completely unchanged. Its RAM decoders expect the first
     * useful image sync roughly two seconds into the block because normal
     * Auto captures include the VIS/prebuffer area. A Cont capture begins at
     * an arbitrary point, so prepend silence up to the normal per-family
     * anchor. Real sync candidates are merely shifted in time; robust sync,
     * clock regression and global refinement then run exactly as before.
     * If Start happened in the middle of a line, at worst the first rendered
     * row is partial; subsequent rows lock to the real sync grid. */
    double preroll_s;
    switch (mode) {
        case SSTV_MODE_MARTIN_M1: preroll_s = 2.350; break;
        case SSTV_MODE_MARTIN_M2: preroll_s = 2.200; break;
        case SSTV_MODE_SCOTTIE_S1:
        case SSTV_MODE_SCOTTIE_S2:
        case SSTV_MODE_SCOTTIE_DX: preroll_s = 2.229; break;
        case SSTV_MODE_ROBOT8_BW:
        case SSTV_MODE_ROBOT36:
        case SSTV_MODE_ROBOT72: preroll_s = 2.000; break;
        default: return -2;
    }

    long pad = (long)llround(preroll_s * (double)SR);
    long total = pad + count;
    float *work = calloc((size_t)total, sizeof(float));
    if (!work) return -3;
    memcpy(work + pad, samples, (size_t)count * sizeof(float));

    fprintf(stderr,
            "SSTV CONT: manual mode=%d capture: %ld samples, synthetic pre-roll %.3f s; dispatching unchanged RAM decoder\n",
            (int)mode, count, preroll_s);
    /* Manual capture: no VIS, so row 0 comes from the sync chain as before. */
    int rows = decode_dispatch(work, total, mode, rgb, info, -1);
    free(work);
    return rows;
}


static float goertzel_power(const float *x, int n, int sr, float hz)
{
    int k = (int)(0.5f + (float)n * hz / (float)sr);
    float w = 2.0f * PI * (float)k / (float)n;
    float coeff = 2.0f * cosf(w), s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { s0 = x[i] + coeff*s1 - s2; s2=s1; s1=s0; }
    return s1*s1 + s2*s2 - coeff*s1*s2;
}

static float vis_tone_power(const float *x, long n, float sec, float dur, int center_hz)
{
    long a=(long)(sec*SR), cnt=(long)(dur*SR);
    if (a < 0 || a+cnt > n || cnt < 32) return -1.0f;
    return goertzel_power(x+a,(int)cnt,SR,(float)center_hz);
}

static int classify_tone(const float *x, long n, float sec, float dur)
{
    static const int hz[] = {1100,1200,1300,1900};
    float best=-1, second=-1; int tone=0;
    for (unsigned i=0;i<sizeof(hz)/sizeof(hz[0]);++i) {
        float p=vis_tone_power(x,n,sec,dur,hz[i]);
        if (p>best) {second=best; best=p; tone=hz[i];}
        else if (p>second) second=p;
    }
    /* Do not reject a VIS symbol merely because the runner-up is close.
     * With 15-18 ms symbol windows, 1100 and 1300 Hz are intrinsically
     * difficult to separate by a large power ratio on noisy RF.  Frame-level
     * validation (leader + start + parity + stop + supported VIS) provides
     * the rejection against false triggers instead. */
    return tone;
}


static int classify_tone_offset(const float *x, long n, float sec, float dur, int offset_hz)
{
    static const int hz[] = {1100,1200,1300,1900};
    float best=-1.0f; int tone=0;
    for (unsigned i=0;i<sizeof(hz)/sizeof(hz[0]);++i) {
        float p=vis_tone_power(x,n,sec,dur,hz[i]+offset_hz);
        if (p>best) { best=p; tone=hz[i]; }
    }
    return tone;
}

static int vis_break_near_offset(const float *x, long n, float center, int offset_hz)
{
    for (float d=-0.020f; d<=0.0201f; d+=0.004f)
        if (classify_tone_offset(x,n,center+d,0.010f,offset_hz)==1200) return 1;
    return 0;
}


/* V5: derive frequency offset from the two long VIS leaders before decoding.
 * This prevents broadband noise from freely choosing an offset that happens
 * to make a supported VIS code fit. */
static int measure_vis_leader(const float *x, long n, float sec, float dur,
                              int *freq_hz, float *quality)
{
    float best=-1.0f, runner=-1.0f; int best_hz=0;
    for (int hz=1740; hz<=2060; hz+=10) {
        float q=vis_tone_power(x,n,sec,dur,hz);
        if (q>best) { runner=best; best=q; best_hz=hz; }
        else if (q>runner) runner=q;
    }
    if (best <= 0.0f) return 0;
    runner=-1.0f;
    for (int hz=1740; hz<=2060; hz+=10) {
        if (abs(hz-best_hz) < 40) continue;
        float q=vis_tone_power(x,n,sec,dur,hz);
        if (q>runner) runner=q;
    }
    float qratio=best/(runner>1.0e-20f?runner:1.0e-20f);
    if (freq_hz) *freq_hz=best_hz;
    if (quality) *quality=qratio;
    return qratio>=1.8f;
}

static const mode_desc_t *find_mode(int vis)
{
    for (unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i)
        if (modes[i].vis==vis) return &modes[i];
    return NULL;
}

static int vis_break_near(const float *x, long n, float center)
{
    for (float d=-0.020f; d<=0.0201f; d+=0.004f)
        if (classify_tone(x,n,center+d,0.010f)==1200) return 1;
    return 0;
}

int sstv_detect_wav(const char *path, sstv_info_t *info)
{
    if (!info) return -1;
    memset(info,0,sizeof(*info));
    fprintf(stderr,"SSTV: opening %s\n",path);
    SF_INFO si={0}; SNDFILE *sf=sf_open(path,SFM_READ,&si);
    if (!sf) { fprintf(stderr,"SSTV: open failed: %s\n",sf_strerror(NULL)); return -2; }
    fprintf(stderr,"SSTV: WAV sr=%d channels=%d frames=%lld\n",si.samplerate,si.channels,(long long)si.frames);
    if (si.channels!=1 || si.samplerate!=SR) { sf_close(sf); fprintf(stderr,"SSTV: unsupported WAV format\n"); return -3; }
    float *x=malloc((size_t)si.frames*sizeof(float));
    if (!x) {sf_close(sf); return -4;}
    sf_count_t got=sf_readf_float(sf,x,si.frames); sf_close(sf);
    if (got!=si.frames) {free(x); return -5;}

    float limit=fminf(5.0f,(float)si.frames/SR-0.95f);

    /* V3: preserve the historical detector as the first pass.  This makes
     * the robust detector an extension of behaviour already validated on RF,
     * rather than a replacement for it. */
    int legacy_candidate=0;
    for (float header=0.0f; header<limit; header+=0.005f) {
        if (!(classify_tone(x,si.frames,header+0.10f,0.020f)==1900 &&
              classify_tone(x,si.frames,header+0.295f,0.010f)==1200 &&
              classify_tone(x,si.frames,header+0.42f,0.020f)==1900)) continue;
        ++legacy_candidate;
        int vis=0, valid=1;
        for (int i=0;i<7;++i) {
            int f=classify_tone(x,si.frames,header+0.640f+i*0.030f+0.005f,0.020f);
            if (f==1100) vis|=1<<i;
            else if (f!=1300) { valid=0; break; }
        }
        if (!valid) continue;
        const mode_desc_t *m=find_mode(vis);
        if (!m) continue;
        fprintf(stderr,"SSTV: legacy VIS accepted at %.3f s\n",header);
        info->vis=vis; info->mode=m->mode; info->name=m->name; info->width=m->w; info->height=m->h;
        fprintf(stderr,"SSTV: VIS=%d -> %s (%dx%d)\n",vis,m->name,m->w,m->h);
        free(x); return 0;
    }

    /* Only if the historical path fails, try the tolerant clock/phase path. */
    int robust_candidate=0;
    for (float header=0.0f; header<limit; header+=0.005f) {
        if (classify_tone(x,si.frames,header+0.10f,0.025f)!=1900 ||
            !vis_break_near(x,si.frames,header+0.295f) ||
            classify_tone(x,si.frames,header+0.42f,0.025f)!=1900) continue;
        ++robust_candidate;

        for (float bit=0.028f; bit<=0.0321f; bit+=0.001f) {
            for (float phase=-0.006f; phase<=0.0061f; phase+=0.003f) {
                float start=header+0.610f+phase;
                float win=bit*0.55f, inset=(bit-win)*0.5f;
                if (classify_tone(x,si.frames,start+inset,win)!=1200) continue;
                int vis=0, ones=0, valid=1;
                for (int i=0;i<7;++i) {
                    int f=classify_tone(x,si.frames,start+(i+1)*bit+inset,win);
                    if (f==1100) {vis|=1<<i; ++ones;}
                    else if (f!=1300) {valid=0; break;}
                }
                if (!valid) continue;
                int parity=classify_tone(x,si.frames,start+8*bit+inset,win);
                if (parity != ((ones&1)?1100:1300)) continue;
                if (classify_tone(x,si.frames,start+9*bit+inset,win)!=1200) continue;
                const mode_desc_t *m=find_mode(vis);
                if (!m) continue;
                fprintf(stderr,"SSTV: robust VIS fallback accepted at %.3f s (bit %.1f ms, phase %+.1f ms)\n",header,bit*1000.0f,phase*1000.0f);
                info->vis=vis; info->mode=m->mode; info->name=m->name; info->width=m->w; info->height=m->h;
                fprintf(stderr,"SSTV: VIS=%d -> %s (%dx%d)\n",vis,m->name,m->w,m->h);
                free(x); return 0;
            }
        }
    }
    /* V5 final fallback: measure the offset from both long leader tones,
     * require them to agree, then freeze that offset for the whole VIS. */
    int offset_candidate=0;
    for (float header=0.0f; header<limit; header+=0.005f) {
        int lf1=0, lf2=0; float lq1=0.0f, lq2=0.0f;
        if (!measure_vis_leader(x,si.frames,header+0.055f,0.140f,&lf1,&lq1) ||
            !measure_vis_leader(x,si.frames,header+0.375f,0.140f,&lf2,&lq2)) continue;
        if (abs(lf1-lf2)>20) continue;
        int off=((lf1+lf2+1)/2)-1900;
        off=(int)lroundf((float)off/10.0f)*10;
        if (off < -150 || off > 150) continue;
        if (off>=-20 && off<=20) continue;
        if (!vis_break_near_offset(x,si.frames,header+0.295f,off)) continue;
        ++offset_candidate;

        for (float bit=0.028f; bit<=0.0321f; bit+=0.001f) {
            for (float phase=-0.006f; phase<=0.0061f; phase+=0.003f) {
                float start=header+0.610f+phase;
                float win=bit*0.55f, inset=(bit-win)*0.5f;
                if (classify_tone_offset(x,si.frames,start+inset,win,off)!=1200) continue;
                int vis=0, ones=0, valid=1;
                for (int i=0;i<7;++i) {
                    int f=classify_tone_offset(x,si.frames,start+(i+1)*bit+inset,win,off);
                    if (f==1100) {vis|=1<<i; ++ones;}
                    else if (f!=1300) {valid=0; break;}
                }
                if (!valid) continue;
                int parity=classify_tone_offset(x,si.frames,start+8*bit+inset,win,off);
                if (parity != ((ones&1)?1100:1300)) continue;
                if (classify_tone_offset(x,si.frames,start+9*bit+inset,win,off)!=1200) continue;
                const mode_desc_t *m=find_mode(vis);
                if (!m) continue;
                fprintf(stderr,"SSTV: V5 leader-locked VIS fallback accepted at %.3f s (leaders %d/%d Hz, offset %+d Hz, quality %.2f/%.2f, bit %.1f ms, phase %+.1f ms)\n",header,lf1,lf2,off,lq1,lq2,bit*1000.0f,phase*1000.0f);
                info->vis=vis; info->mode=m->mode; info->name=m->name; info->width=m->w; info->height=m->h;
                fprintf(stderr,"SSTV: VIS=%d -> %s (%dx%d)\n",vis,m->name,m->w,m->h);
                free(x); return 0;
            }
        }
    }
    fprintf(stderr,"SSTV: no valid supported VIS found (legacy candidates=%d, robust candidates=%d, offset candidates=%d)\n",legacy_candidate,robust_candidate,offset_candidate);
    free(x); return -6;
}

int sstv_decode_wav(const char *path, uint8_t *rgb, sstv_info_t *info)
{
    sstv_info_t local;
    if (!info) info=&local;
    int rc=sstv_detect_wav(path,info);
    if (rc<0) return rc;

    fprintf(stderr,"SSTV TEST: loading WAV into RAM so TEST uses the same decoder path as LIVE RF\n");
    SF_INFO si={0};
    SNDFILE *sf=sf_open(path,SFM_READ,&si);
    if (!sf) { fprintf(stderr,"SSTV TEST: cannot reopen %s: %s\n",path,sf_strerror(NULL)); return -2; }
    if (si.channels!=1 || si.samplerate!=SR) { sf_close(sf); return -3; }
    float *samples=malloc((size_t)si.frames*sizeof(float));
    if (!samples) { sf_close(sf); return -4; }
    sf_count_t got=sf_readf_float(sf,samples,si.frames);
    sf_close(sf);
    if (got!=si.frames) { free(samples); return -5; }

    fprintf(stderr,"SSTV TEST: decoder dispatch -> %s through common RAM decoder (%lld samples @ %d Hz)\n",
            info->name,(long long)si.frames,SR);

    int rows=sstv_decode_samples(samples,(long)si.frames,info->mode,rgb,info);
    free(samples);
    fprintf(stderr,"SSTV TEST: common RAM image decode returned rows=%d\n",rows);
    return rows;
}

