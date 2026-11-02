/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * WEFAX test signal generator: a picture as a radio fax broadcast (120 lines
 * per minute, IOC 576), through a simulated HF channel, as the audio of the
 * radio (12000 Hz mono 16-bit WAV, the format of the radio's recordings).
 *
 *   wefax_gen IN.pgm OUT.wav [options]
 *   wefax_pair IN.pgm OUT.pgm [options] [--denoise]
 *
 * wefax_pair is the same program built with the decoder of the firmware
 * (src/wefax/decoder.c, -DWEFAX_PAIR): the audio goes straight into the
 * decoder (Auto RX, as on the radio, or Manual RX with --cont: a weak
 * signal whose start tone is lost still gives a fax) and OUT.pgm is the received fax,
 * without writing the WAV. Used to make training pairs for the denoiser.
 *
 *   IN.pgm         the picture, 8-bit PGM (P5), any size: each row is
 *                  stretched to the whole line, as Northwood sends it
 *   --snr DB       noise, signal to noise ratio in 2500 Hz (default: none)
 *   --paths N      propagation paths: 1 (default) or 2 or 3
 *   --scatter DB   power of each fading path against the steady first
 *                  path (default: all paths fading, the same power)
 *   --delay MS     delay of the 2nd path (3rd: 1.7 times it), default 1.5
 *   --doppler HZ   speed of the fading of each path (default 0.2)
 *   --flat S       slow fading of the whole signal, period S seconds
 *   --depth DB     depth of the slow fading (default 15)
 *   --clicks N     static crashes per minute (default 0)
 *   --jitter PX    timing jitter of each line, standard deviation in
 *                  pixels of the 1809-pixel line (default 0)
 *   --off HZ       tuning error (default 0)
 *   --ppm N        transmitter line period error (default 0)
 *   --seed N
 *
 * With 2 or 3 paths each one fades on its own (complex gains following a
 * slow random walk, Rayleigh-like): the two tones of the fax, 850 Hz apart,
 * then fade at different moments - the selective fading of HF.
 *
 * Broadcast: 5 s start tone (300 Hz black/white), 30 phasing lines (black
 * with the last 5% white), the picture, 5 s stop tone (450 Hz), 10 s black.
 * Tones: white 1925 Hz, black 1075 Hz, as the 1KO125 decoder expects
 * (src/wefax/decoder.c).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WEFAX_PAIR
#include "decoder.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE        12000.0
#define LINE_S      0.5             /* 120 lines per minute */
#define F_WHITE     1925.0
#define F_BLACK     1075.0
#define MAX_DELAY   256             /* samples of path delay history */

static uint64_t rng = 88172645463325252ull;
static double urand(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return ((rng >> 11) + 0.5) / 9007199254740992.0;
}
static double grand(void) { return sqrt(-2 * log(urand())) * cos(2 * M_PI * urand()); }

/* the picture */
static int pw, ph;
static uint8_t *pic;

static int read_pgm(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    char magic[3] = {0};
    int maxv;
    if (fscanf(f, "%2s %d %d %d", magic, &pw, &ph, &maxv) != 4 || strcmp(magic, "P5") || maxv != 255) {
        fprintf(stderr, "%s: not an 8-bit PGM (P5)\n", path);
        fclose(f);
        return -1;
    }
    fgetc(f);
    pic = malloc((size_t)pw * ph);
    if (fread(pic, 1, (size_t)pw * ph, f) != (size_t)pw * ph) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* gray (0 black .. 255 white) of the broadcast at time t (s) */
static double t_start, t_phase, t_pic, t_stop, t_end;
static double line_s;
static double *jit;     /* line timing jitter, picture pixels per row */

static double gray_at(double t)
{
    if (t < t_start) {                              /* start tone: 300 Hz */
        return fmod(t * 300.0, 1.0) < 0.5 ? 0 : 255;
    }
    if (t < t_phase) {                              /* phasing */
        double x = fmod(t - t_start, line_s) / line_s;
        return x >= 0.95 ? 255 : 0;
    }
    if (t < t_pic) {                                /* the picture */
        double u = (t - t_phase) / line_s;
        int row = (int)u;
        if (row >= ph) row = ph - 1;
        double x = (u - row) * pw + (jit ? jit[row] : 0);
        if (x < 0) x = 0;
        if (x > pw - 1) x = pw - 1;
        int c = (int)x;
        double fr = x - c;
        int c2 = c + 1 < pw ? c + 1 : c;
        const uint8_t *r = pic + (size_t)row * pw;
        return r[c] * (1 - fr) + r[c2] * fr;
    }
    if (t < t_stop) {                               /* stop tone: 450 Hz */
        return fmod(t * 450.0, 1.0) < 0.5 ? 0 : 255;
    }
    return 0;                                       /* black */
}

/* a slowly varying complex gain of unit mean power (one fading path): a sum
 * of NSIN rotating phasors with Doppler frequencies spread up to the
 * Doppler limit (Jakes-like), Rayleigh-distributed amplitude */
#define NSIN 12
typedef struct { double f[NSIN], p[NSIN]; } path_t;

static void path_init(path_t *p, double doppler)
{
    for (int k = 0; k < NSIN; k++) {
        p->f[k] = doppler * cos(2 * M_PI * (k + urand()) / NSIN);
        p->p[k] = 2 * M_PI * urand();
    }
}

static void path_gain(const path_t *p, double t, double *re, double *im)
{
    double a = 0, b = 0;
    for (int k = 0; k < NSIN; k++) {
        double ph = 2 * M_PI * p->f[k] * t + p->p[k];
        a += cos(ph);
        b += sin(ph);
    }
    *re = a / sqrt(NSIN);
    *im = b / sqrt(NSIN);
}

#ifdef WEFAX_PAIR
static uint8_t *dimg;
static unsigned int drows;

static void row_cb(const uint8_t *row, unsigned int width, unsigned int nr, void *user)
{
    (void)user;
    if (nr >= WEFAX_MAX_ROWS) return;
    memcpy(dimg + (size_t)nr * WEFAX_WIDTH, row, width < WEFAX_WIDTH ? width : WEFAX_WIDTH);
    if (nr + 1 > drows) drows = nr + 1;
}

/* the audio through the decoder of the firmware, the fax as PGM */
static int write_decoded(const char *path, const int16_t *pcm, long n, int denoise, int cont)
{
    dimg = calloc((size_t)WEFAX_MAX_ROWS * WEFAX_WIDTH, 1);
    wefax_decoder_t *d = wefax_decoder_create();
    wefax_decoder_set_mode(d, cont ? WEFAX_MODE_CONT : WEFAX_MODE_AUTO);
    wefax_decoder_set_row_callback(d, row_cb, NULL);
    wefax_decoder_set_denoise(d, denoise);
    float fb[1200];
    for (long i = 0; i < n; i += 1200) {
        long m = n - i < 1200 ? n - i : 1200;
        for (long k = 0; k < m; k++) fb[k] = pcm[i + k] / 32768.0f;
        wefax_decoder_process(d, fb, (unsigned int)m);
    }
    wefax_decoder_destroy(d);
    if (drows == 0) { fprintf(stderr, "%s: no fax decoded\n", path); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "P5\n%d %u\n255\n", WEFAX_WIDTH, drows);
    fwrite(dimg, WEFAX_WIDTH, drows, f);
    fclose(f);
    free(dimg);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    double snr = 1e9, delay_ms = 1.5, doppler = 0.2, flat = 0, depth = 15, clicks = 0, off = 0, ppm = 0;
    double scatter = 1e9;    /* none: all paths fade */
    double jitter = 0;
    int denoise = 0, cont = 0;
    int paths = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--snr") && i + 1 < argc) snr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--paths") && i + 1 < argc) paths = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc) delay_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--doppler") && i + 1 < argc) doppler = atof(argv[++i]);
        else if (!strcmp(argv[i], "--scatter") && i + 1 < argc) scatter = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jitter") && i + 1 < argc) jitter = atof(argv[++i]);
        else if (!strcmp(argv[i], "--denoise")) denoise = 1;
        else if (!strcmp(argv[i], "--cont")) cont = 1;
        else if (!strcmp(argv[i], "--flat") && i + 1 < argc) flat = atof(argv[++i]);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = atof(argv[++i]);
        else if (!strcmp(argv[i], "--clicks") && i + 1 < argc) clicks = atof(argv[++i]);
        else if (!strcmp(argv[i], "--off") && i + 1 < argc) off = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) rng ^= (uint64_t)atoll(argv[++i]) * 0x9E3779B97F4A7C15ull;
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
    }
    if (!in || !out || paths < 1 || paths > 3) {
        fprintf(stderr, "usage: wefax_gen IN.pgm OUT.wav [--snr DB] [--paths N] [--scatter DB] [--delay MS] [--doppler HZ]\n"
                        "       [--flat S] [--depth DB] [--clicks N] [--jitter PX] [--off HZ] [--ppm N] [--seed N]\n");
        return 2;
    }
    if (read_pgm(in)) return 1;
    for (int k = 0; k < 20; k++) urand();

    line_s = LINE_S * (1 + ppm * 1e-6);
    if (jitter > 0) {
        /* each line on its own, with a little memory from line to line */
        jit = malloc(sizeof(double) * ph);
        double j = 0;
        for (int r = 0; r < ph; r++) {
            j = 0.5 * j + 0.87 * grand();
            jit[r] = j * jitter * pw / 1809.0;
        }
    }
    t_start = 5;
    t_phase = t_start + 30 * line_s;
    t_pic = t_phase + ph * line_s;
    t_stop = t_pic + 5;
    t_end = t_stop + 10;

    long n = (long)(t_end * RATE);
    int16_t *pcm = malloc((size_t)n * sizeof(int16_t));

    /* path delays in samples, gains starting at random */
    int dly[3] = {0, (int)lrint(delay_ms * 1e-3 * RATE), (int)lrint(1.7 * delay_ms * 1e-3 * RATE)};
    if (dly[2] >= MAX_DELAY) { fprintf(stderr, "delay too long\n"); return 2; }
    /* with --scatter the first path is steady (amplitude 1) and the others
     * fade with this power; otherwise all fade, the same power */
    path_t pa[3];
    double pg[3];
    bool steady = scatter < 1e8 && paths > 1;
    for (int k = 0; k < 3; k++) {
        path_init(&pa[k], doppler);
        pg[k] = steady ? (k == 0 ? 1.0 : pow(10, scatter / 20)) : 1.0 / sqrt(paths);
    }

    double hist_re[MAX_DELAY] = {0}, hist_im[MAX_DELAY] = {0};
    int hp = 0;
    double ph_acc = 0, amp = 0.25;
    double sigma = snr > 1e8 ? 0 : sqrt((amp * amp / 2) / pow(10, snr / 10) * (RATE / 2) / 2500);
    double fph = 2 * M_PI * urand();
    double click_left = 0, click_amp = 0;

    for (long i = 0; i < n; i++) {
        double t = i / RATE;
        double g = gray_at(t);
        double f = F_BLACK + (F_WHITE - F_BLACK) * g / 255.0 + off;
        ph_acc += 2 * M_PI * f / RATE;
        if (ph_acc > M_PI) ph_acc -= 2 * M_PI;

        /* analytic transmitted signal and its delayed copies */
        hist_re[hp] = cos(ph_acc);
        hist_im[hp] = sin(ph_acc);
        double re = 0, im = 0;
        for (int k = 0; k < paths; k++) {
            double gr = 1, gi = 0;
            if (paths > 1 && !(steady && k == 0)) path_gain(&pa[k], t, &gr, &gi);
            gr *= pg[k];
            gi *= pg[k];
            int j = (hp - dly[k] + MAX_DELAY) % MAX_DELAY;
            re += gr * hist_re[j] - gi * hist_im[j];
            im += gr * hist_im[j] + gi * hist_re[j];
        }
        hp = (hp + 1) % MAX_DELAY;
        (void)im;

        double v = amp * re;
        if (flat > 0) {
            double d = 0.5 + 0.5 * sin(2 * M_PI * t / flat + fph) * sin(2 * M_PI * t / (2.7 * flat));
            v *= pow(10, -depth * d * d / 20);
        }
        v += sigma * grand();

        /* static crashes: short bursts of strong noise */
        if (clicks > 0) {
            if (click_left <= 0 && urand() < clicks / 60.0 / RATE) {
                click_left = RATE * (0.002 + 0.02 * urand());
                click_amp = amp * (1 + 4 * urand());
            }
            if (click_left > 0) { v += click_amp * grand(); click_left--; }
        }
        pcm[i] = (int16_t)lrint(fmax(-32767, fmin(32767, v * 32767)));
    }

#ifdef WEFAX_PAIR
    if (write_decoded(out, pcm, n, denoise, cont)) return 1;
#else
    (void)denoise;
    (void)cont;
    FILE *fo = fopen(out, "wb");
    if (!fo) { perror(out); return 1; }
    uint32_t data = (uint32_t)n * 2, r = (uint32_t)RATE, v32;
    uint16_t v16;
    fwrite("RIFF", 1, 4, fo); v32 = 36 + data; fwrite(&v32, 4, 1, fo);
    fwrite("WAVEfmt ", 1, 8, fo); v32 = 16; fwrite(&v32, 4, 1, fo);
    v16 = 1; fwrite(&v16, 2, 1, fo); fwrite(&v16, 2, 1, fo);
    fwrite(&r, 4, 1, fo); v32 = r * 2; fwrite(&v32, 4, 1, fo);
    v16 = 2; fwrite(&v16, 2, 1, fo); v16 = 16; fwrite(&v16, 2, 1, fo);
    fwrite("data", 1, 4, fo); fwrite(&data, 4, 1, fo);
    fwrite(pcm, 2, (size_t)n, fo);
    fclose(fo);
#endif
    fprintf(stderr, "%s: %d x %d picture, %.0f s\n", out, pw, ph, t_end);
    return 0;
}
