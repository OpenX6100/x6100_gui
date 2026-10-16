/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - PSK31 receiver
 *
 *  Chain:  real audio @ 12000 Hz (PSK_RX_RATE, from the DSP)
 *            -> complex mixer (centre freq, AFC corrected)
 *            -> stage A: 133-tap LPF, decimate by 24   (-> 500 Hz)
 *            -> stage B: 24-tap matched filter (Hann, 1.5 symbols)
 *            -> envelope early/late gate for symbol timing
 *            -> differential phase detection (0 / pi)
 *            -> Varicode decode -> panel
 *
 *  Decimation by 24 lands on 500 Hz, 16 samples per symbol, the rate
 *  stage B, the timing loop and every threshold below are tuned for.
 *
 *  Stage A is a plain windowed-sinc FIR computed at init. At these
 *  rates the cost is a few hundred thousand multiplies per second, and
 *  keeping the DSP self-contained means the whole receiver can be tested
 *  on a host against the transmitter in psk_tx.c.
 *
 *  Stage B is a matched filter, not a plain low-pass: a 65-tap low-pass
 *  with a 50 Hz cutoff lets 100 Hz of noise through to a decision that
 *  needs about 31 Hz. A
 *  PSK31 signal is a train of raised-cosine pulses two symbols long, so
 *  the filter that maximises the signal to noise ratio at the sampling
 *  instant has that shape; slightly shorter, 1.5 symbols, trades a
 *  little noise for less overlap between neighbouring symbols.
 *
 *  Measured on synthetic signals, 6 x 100 words per point, words fully
 *  correct at a given S/N in 2.5 kHz:
 *
 *                   -6 dB   -8 dB   -10 dB   -12 dB   -14 dB
 *      65-tap LPF    96.2    72.5     35.8      6.3      0.7
 *      matched 24    99.7    98.5     90.0     55.3     18.7
 *
 *  About 3.5 dB. Ideal DBPSK would reach roughly 99 % at -10 dB and
 *  80 % at -12 dB, so what is left is 1.5-2 dB. Lengths from 16 to 40
 *  taps were tried; 24-26 was best. A smoothed proportional timing loop
 *  was also tried and made no measurable difference, so the early/late
 *  gate stays. Capture range is the same either way: up to 7 Hz off.
 *
 *  Selectivity improved even more. A station 9.5 dB stronger 40 Hz
 *  away, or 20 dB stronger 70 Hz away: the old filter read nothing of
 *  the weaker one (it passed the neighbour at full strength and, 35 Hz
 *  away, decoded the neighbour instead); the matched filter reads the
 *  weaker one 103/120 and 120/120 words. The quality figure on pure
 *  noise is unchanged, mean 50 and highest 66 over ten minutes, so the
 *  thresholds the PSK31 window uses for tracking still hold.
 */

#include "psk_rx.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "dsp.h"
#include "panel.h"
#include "psk_tx.h"          /* varicode table accessor */

#define FS_IN            ((float)PSK_RX_RATE)                        /* 12000 */
#define DECIM2           24
#define FS2              (FS_IN / (float)DECIM2)                     /* 500   */
#define PSK_BAUD_F       31.25f
#define SPB              (FS2 / PSK_BAUD_F)                          /* ~16.0 */

#define NTAPS_A          133
#define NTAPS_B          24     /* 1.5 symbols at FS2 */
#define CUTOFF_A         220.0f     /* anti-alias before decimation */

#define AFC_MAX_HZ       12.0f      /* how far the loop will pull    */
#define AFC_GAIN         0.02f
#define QUAL_ALPHA       0.05f

#define MAX_CODE_LEN     12

/* Statically initialised so it is valid before psk_rx_init() and is
 * never re-initialised while the DSP thread may be holding it. */
static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static bool            active = false;
static bool            ready  = false;

/* DSP subscription, made once and switched on and off with the
 * receiver. dsp_audio_set_active() takes the lock the DSP holds while
 * it calls us, so it is never called with mux held: that order would
 * deadlock against an audio block arriving at the same moment. */
static uint32_t           dsp_sub   = AUDIO_SUB_INVALID;

static psk_rx_text_cb_t   text_cb   = NULL;
static psk_rx_audio_tap_t audio_tap = NULL;

/* mixer */
static double mix_phase = 0.0;
static double mix_w     = 0.0;      /* radians per input sample */
static float  center_hz = 1000.0f;
static float  afc_hz    = 0.0f;

/* stage A: FIR + decimator */
static float          taps_a[NTAPS_A];
static float complex  hist_a[NTAPS_A];
static unsigned       hist_a_pos = 0;
static unsigned       decim_cnt  = 0;

/* stage B: FIR at FS2 */
static float          taps_b[NTAPS_B];
static float complex  hist_b[NTAPS_B];
static unsigned       hist_b_pos = 0;

/* symbol timing: we keep a short history at FS2 so the early/late gate
 * can look a quarter symbol either side of the sampling instant. */
#define TB_LEN 64
static float complex tbuf[TB_LEN];
static float         tmag[TB_LEN];
static unsigned      tb_pos = 0;
static double        sym_acc = 0.0;      /* counts down to next symbol */
static bool          sym_primed = false;

/* differential detection */
static float complex prev_sym = 0.0f;
static bool          have_prev = false;

/* varicode assembly */
static char   code_buf[MAX_CODE_LEN + 1];
static int    code_len = 0;
static int    zero_run = 0;

/* quality / AFC measurement */
static float  phase_err_avg = 0.0f;
static float  qual_avg      = 0.0f;

/* ---- FIR design ------------------------------------------------------ */

static void design_lpf(float *taps, int n, float cutoff, float fs) {
    double fc = (double)cutoff / (double)fs;      /* normalised */
    int    m  = n - 1;
    double sum = 0.0;

    for (int i = 0; i < n; i++) {
        double k = (double)i - (double)m / 2.0;
        double sinc = (fabs(k) < 1e-9) ? (2.0 * fc)
                                       : sin(2.0 * M_PI * fc * k) / (M_PI * k);
        /* Hamming window */
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)m);
        taps[i] = (float)(sinc * w);
        sum += taps[i];
    }
    /* unity DC gain */
    if (sum != 0.0) {
        for (int i = 0; i < n; i++) taps[i] = (float)(taps[i] / sum);
    }
}

/* Hann pulse, unity DC gain. The half-sample offset keeps it symmetric
 * without zero end taps, so all NTAPS_B taps do work. */
static void design_matched(float *taps, int n) {
    double sum = 0.0;

    for (int i = 0; i < n; i++) {
        double t = ((double)i + 0.5) / (double)n;
        taps[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * t));
        sum += taps[i];
    }
    for (int i = 0; i < n; i++) taps[i] = (float)(taps[i] / sum);
}

static inline float complex fir_exec(const float *taps, int n,
                                    float complex *hist, unsigned pos) {
    float complex acc = 0.0f;
    unsigned idx = pos;
    for (int i = 0; i < n; i++) {
        acc += hist[idx] * taps[i];
        idx = (idx == 0) ? (unsigned)(n - 1) : idx - 1;
    }
    return acc;
}

/* ---- varicode decode ------------------------------------------------- */

static void emit_code(void) {
    if (code_len == 0) return;
    code_buf[code_len] = '\0';

    for (int c = 0; c < 128; c++) {
        const char *v = psk_tx_varicode((unsigned char)c);
        if (v && strcmp(v, code_buf) == 0) {
            if (c == '\r') break;                  /* swallow CR */
            if (text_cb) {
                text_cb((char)c);
            } else {
                char s[2] = { (char)c, 0 };
                panel_add_text(s);
            }
            break;
        }
    }
    code_len = 0;
}

static void push_bit(int bit) {
    if (bit) {
        zero_run = 0;
        if (code_len < MAX_CODE_LEN) code_buf[code_len++] = '1';
        return;
    }

    zero_run++;
    if (zero_run >= 2) {
        /* two zeros in a row terminate a character; the first zero was
         * already appended, so strip it before decoding */
        if (zero_run == 2 && code_len > 0 && code_buf[code_len - 1] == '0') {
            code_len--;
        }
        emit_code();
        return;
    }
    /* a single zero can be inside a code */
    if (code_len < MAX_CODE_LEN) code_buf[code_len++] = '0';
}

/* ---- symbol processing ---------------------------------------------- */

static inline float mag_at(int back) {
    int idx = (int)tb_pos - back;
    while (idx < 0) idx += TB_LEN;
    return tmag[idx % TB_LEN];
}

static inline float complex sym_at(int back) {
    int idx = (int)tb_pos - back;
    while (idx < 0) idx += TB_LEN;
    return tbuf[idx % TB_LEN];
}

static void process_symbol(void) {
    /* Early/late gate: PSK31's envelope dips at every phase reversal, so
     * the correct sampling instant sits on an envelope peak. Compare the
     * magnitude a quarter symbol either side and nudge the clock. */
    int q = (int)(SPB / 4.0f + 0.5f);
    float early = mag_at(q * 2);          /* earlier in time  */
    float late  = mag_at(0);              /* most recent      */
    float here  = mag_at(q);

    if (early > here && early > late) {
        sym_acc -= SPB * 0.02;            /* we are late   */
    } else if (late > here && late > early) {
        sym_acc += SPB * 0.02;            /* we are early  */
    }

    float complex s = sym_at(q);          /* sample at the peak */

    if (!have_prev) {
        prev_sym  = s;
        have_prev = true;
        return;
    }

    /* differential detection: same phase -> 1, opposite -> 0 */
    float complex d = s * conjf(prev_sym);
    prev_sym = s;

    float ang = atan2f(cimagf(d), crealf(d));
    int   bit;
    float err;

    if (fabsf(ang) < (float)M_PI / 2.0f) {
        bit = 1;
        err = ang;                        /* should be 0    */
    } else {
        bit = 0;
        err = (ang > 0.0f) ? (ang - (float)M_PI) : (ang + (float)M_PI);
    }

    /* Only trust the error when the symbol has real energy. */
    float m = cabsf(s);
    if (m > 1e-4f) {
        phase_err_avg += (err - phase_err_avg) * AFC_GAIN;

        /* residual offset in Hz: radians per symbol -> Hz */
        float f_err = phase_err_avg * PSK_BAUD_F / (2.0f * (float)M_PI);
        afc_hz += f_err * AFC_GAIN;
        if (afc_hz >  AFC_MAX_HZ) afc_hz =  AFC_MAX_HZ;
        if (afc_hz < -AFC_MAX_HZ) afc_hz = -AFC_MAX_HZ;
        mix_w = 2.0 * M_PI * (double)(center_hz + afc_hz) / (double)FS_IN;

        /* quality: how tightly the decisions cluster on 0 / pi */
        float q01 = 1.0f - fabsf(err) / ((float)M_PI / 2.0f);
        if (q01 < 0.0f) q01 = 0.0f;
        qual_avg += (q01 - qual_avg) * QUAL_ALPHA;
    }

    push_bit(bit);
}

/* ---- public API ------------------------------------------------------ */

/* Caller holds mux, or the receiver is not running yet. */
static void update_center_locked(void) {
    center_hz = (float)psk_tx_get_center();
    afc_hz    = 0.0f;
    mix_w     = 2.0 * M_PI * (double)center_hz / (double)FS_IN;
}

void psk_rx_init(void) {
    design_lpf(taps_a, NTAPS_A, CUTOFF_A, FS_IN);
    design_matched(taps_b, NTAPS_B);

    memset(hist_a, 0, sizeof(hist_a));
    memset(hist_b, 0, sizeof(hist_b));
    memset(tbuf, 0, sizeof(tbuf));
    memset(tmag, 0, sizeof(tmag));

    hist_a_pos = hist_b_pos = tb_pos = 0;
    decim_cnt = 0;
    mix_phase = 0.0;
    afc_hz = 0.0f;
    sym_acc = 0.0;
    sym_primed = false;
    have_prev = false;
    code_len = 0;
    zero_run = 0;
    phase_err_avg = 0.0f;
    qual_avg = 0.0f;

    update_center_locked();
    ready = true;

    if (dsp_sub == AUDIO_SUB_INVALID) {
        dsp_sub = dsp_audio_subscribe_float(psk_rx_put_audio_samples, PSK_RX_RATE);
        dsp_audio_set_active(dsp_sub, false);
    }
}

/* Called from the UI thread while the DSP thread may be mid-block, so
 * take the lock: mix_w is a double and a torn write would be a jump. */
void psk_rx_update_center(void) {
    pthread_mutex_lock(&mux);
    update_center_locked();
    pthread_mutex_unlock(&mux);
}

void psk_rx_set_text_cb(psk_rx_text_cb_t cb) {
    pthread_mutex_lock(&mux);
    text_cb = cb;
    pthread_mutex_unlock(&mux);
}

void psk_rx_set_audio_tap(psk_rx_audio_tap_t tap) {
    pthread_mutex_lock(&mux);
    audio_tap = tap;
    pthread_mutex_unlock(&mux);
}

void psk_rx_set_active(bool on) {
    /* Off: stop the feed first; once this returns no audio block is
     * inside the receiver, which is what lets the caller drop its hooks. */
    if (!on && dsp_sub != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_sub, false);

    pthread_mutex_lock(&mux);
    if (on && !active) {
        /* fresh start: clear the loops so old state cannot leak in */
        memset(hist_a, 0, sizeof(hist_a));
        memset(hist_b, 0, sizeof(hist_b));
        have_prev = false;
        code_len = 0;
        zero_run = 0;
        sym_primed = false;
        afc_hz = 0.0f;
        phase_err_avg = 0.0f;
        qual_avg = 0.0f;
        update_center_locked();     /* already holding mux */
    }
    active = on;
    pthread_mutex_unlock(&mux);

    if (on && dsp_sub != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_sub, true);
}

bool psk_rx_is_active(void) {
    return active;
}

float psk_rx_freq_error(void) {
    return afc_hz;
}

uint8_t psk_rx_quality(void) {
    float q = qual_avg * 100.0f;
    if (q < 0.0f) q = 0.0f;
    if (q > 100.0f) q = 100.0f;
    return (uint8_t)(q + 0.5f);
}

void psk_rx_put_audio_samples(size_t n, float *samples) {
    if (!ready) return;

    pthread_mutex_lock(&mux);
    if (!active) {
        pthread_mutex_unlock(&mux);
        return;
    }

    if (audio_tap) {
        audio_tap((unsigned int)n, samples);
    }

    for (size_t i = 0; i < n; i++) {
        /* --- complex downconversion --- */
        mix_phase += mix_w;
        if (mix_phase > 2.0 * M_PI) mix_phase -= 2.0 * M_PI;
        float complex z = samples[i] * cexpf(-I * (float)mix_phase);

        /* --- stage A: filter, then take every DECIM2'th output --- */
        hist_a_pos = (hist_a_pos + 1) % NTAPS_A;
        hist_a[hist_a_pos] = z;

        if (++decim_cnt < DECIM2) continue;
        decim_cnt = 0;

        float complex a = fir_exec(taps_a, NTAPS_A, hist_a, hist_a_pos);

        /* --- stage B: narrow band --- */
        hist_b_pos = (hist_b_pos + 1) % NTAPS_B;
        hist_b[hist_b_pos] = a;
        float complex b = fir_exec(taps_b, NTAPS_B, hist_b, hist_b_pos);

        /* --- timing history --- */
        tb_pos = (tb_pos + 1) % TB_LEN;
        tbuf[tb_pos] = b;
        tmag[tb_pos] = cabsf(b);

        /* --- symbol clock --- */
        if (!sym_primed) {
            sym_acc = SPB;
            sym_primed = true;
        }
        sym_acc -= 1.0;
        if (sym_acc <= 0.0) {
            sym_acc += SPB;
            process_symbol();
        }
    }

    pthread_mutex_unlock(&mux);
}
