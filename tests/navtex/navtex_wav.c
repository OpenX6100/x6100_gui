/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * Decodes a NAVTEX recording with the firmware decoder, on a computer.
 *
 *   navtex_wav IN.wav [--rev] [--centre HZ] [--quality]
 *
 * Any sample rate, 16-bit PCM or 32-bit float, mono or stereo (the left channel): the
 * audio is resampled to 12000 Hz, the rate of the X6100. The text goes to
 * standard output; with --quality each character is followed by its
 * quality (0 clean, 1 from the repetition, 2 guessed, 3 lost). Statistics
 * go to standard error.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "sitorb.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int show_q;
static long count_q[4], syncs_on, syncs_off;

static void on_char(int c, int q, void *user)
{
    (void)user;
    if (q == NAVTEX_Q_EVENT) {
        if (c == NAVTEX_EV_SYNC_ON) syncs_on++;
        if (c == NAVTEX_EV_SYNC_OFF) syncs_off++;
        if (show_q) printf("%s", c == NAVTEX_EV_SYNC_ON ? "{ON}" : "{OFF}");
        return;
    }
    if (q >= 0 && q <= 3) count_q[q]++;
    if (c == '\r') return;
    putchar(c);
    if (show_q && c != '\n') putchar('0' + (q < 0 ? 3 : q));
}

/* ---- WAV ---------------------------------------------------------------------------- */

static float *read_wav(const char *path, unsigned *rate, long *frames)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        fclose(f);
        return NULL;
    }
    unsigned ch = 0, bits = 0, fmt = 0;
    *rate = 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[40] = {0};
            if (fread(fm, 1, len < 40 ? len : 40, f) == 0) break;
            if (len > 40) fseek(f, len - 40, SEEK_CUR);
            fmt = fm[0] | fm[1] << 8;
            if (fmt == 0xFFFE && len >= 26)
                fmt = fm[24] | fm[25] << 8;    /* WAVE_FORMAT_EXTENSIBLE */
            ch = fm[2] | fm[3] << 8;
            *rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
            bits = fm[14] | fm[15] << 8;
        } else if (!memcmp(c, "data", 4)) {
            int pcm16 = fmt == 1 && bits == 16, flt32 = fmt == 3 && bits == 32;
            if (!(pcm16 || flt32) || ch < 1) {
                fprintf(stderr, "%s: only 16-bit PCM or 32-bit float\n", path);
                break;
            }
            unsigned bps = bits / 8;
            long n = len / (bps * ch);
            uint8_t *raw = malloc((size_t)len);
            float *x = malloc(sizeof(float) * (size_t)n);
            n = (long)fread(raw, bps * ch, (size_t)n, f);
            for (long i = 0; i < n; i++) {
                const uint8_t *q = raw + (size_t)i * bps * ch;
                if (pcm16) {
                    int16_t v;
                    memcpy(&v, q, 2);
                    x[i] = v / 32768.0f;
                } else {
                    memcpy(&x[i], q, 4);
                }
            }
            free(raw);
            fclose(f);
            *frames = n;
            return x;
        } else {
            fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
    fclose(f);
    return NULL;
}

/* windowed-sinc resampler */
static float *resample(const float *x, long n, double from, double to, long *out_n)
{
    double ratio = to / from, cut = 0.45 * fmin(1.0, ratio);
    int half = 32;
    long m = (long)floor(n * ratio);
    float *y = malloc(sizeof(float) * (size_t)m);
    for (long j = 0; j < m; j++) {
        double t = j / ratio;
        long c = (long)floor(t);
        double acc = 0, ws = 0;
        for (long i = c - half + 1; i <= c + half; i++) {
            if (i < 0 || i >= n) continue;
            double u = t - i;
            double s = u == 0 ? 2 * cut : sin(2 * M_PI * cut * u) / (M_PI * u);
            double w = 0.5 + 0.5 * cos(M_PI * u / half);
            acc += x[i] * s * w;
            ws += s * w;
        }
        y[j] = (float)(ws != 0 ? acc / ws * (2 * cut) / (2 * cut) : 0);
    }
    *out_n = m;
    return y;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: navtex_wav IN.wav [--rev] [--centre HZ] [--quality]\n");
        return 2;
    }
    int rev = 0;
    double centre = 1000;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--rev")) rev = 1;
        else if (!strcmp(argv[i], "--quality")) show_q = 1;
        else if (!strcmp(argv[i], "--centre") && i + 1 < argc) centre = atof(argv[++i]);
    }
    unsigned rate;
    long n;
    float *x = read_wav(argv[1], &rate, &n);
    if (!x) return 1;
    /* 2 s of silence at the end: the decoder prints a character 1.5 s
     * after it is decided */
    x = realloc(x, sizeof(float) * (size_t)(n + 24000));
    memset(x + n, 0, sizeof(float) * 24000);
    n += 24000;
    if (rate != 12000) {
        long m;
        float *y = resample(x, n, rate, 12000, &m);
        free(x);
        x = y;
        n = m;
    }

    sitorb_t *d = sitorb_create(12000, (float)centre, rev);
    sitorb_set_callback(d, on_char, NULL);
    for (long i = 0; i < n; i += 512)
        sitorb_process(d, x + i, (unsigned)(n - i < 512 ? n - i : 512));
    fprintf(stderr, "afc %.1f Hz, ", sitorb_freq_offset(d));
    sitorb_destroy(d);
    fflush(stdout);
    fprintf(stderr, "clean %ld rep %ld guess %ld lost %ld, sync on %ld off %ld\n",
            count_q[0], count_q[1], count_q[2], count_q[3], syncs_on, syncs_off);
    free(x);
    return 0;
}
