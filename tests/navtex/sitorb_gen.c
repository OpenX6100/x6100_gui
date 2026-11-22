/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * Test signal generator: a NAVTEX message sent in SITOR-B (ITU-R M.625 FEC
 * mode B), as audio, with noise, frequency offset and clock error.
 *
 *   sitorb_gen OUT.wav [options] < message.txt
 *     --snr DB     signal to noise ratio in 2500 Hz (default: no noise)
 *     --off HZ     frequency offset of the signal (default 0)
 *     --ppm N      transmitter clock error (default 0)
 *     --rate HZ    sample rate (default 12000)
 *     --centre HZ  audio centre (default 1000)
 *     --rev        swap the tones
 *     --fade S     Rayleigh-like fading, period in seconds (default off)
 *     --sfade S    selective fading: the two tones fade independently
 *     --seed N
 *
 * The message is read from standard input (letters, figures, spaces, new
 * lines). Mono 16-bit WAV.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *const code_bits[35] = {
    "BBBYYYB", "YBYYBBB", "BYBBBYY", "BBYYBYB", "YBBYBYB", "BBYBBYY",
    "BYBYBBY", "BYYBYBB", "BYBBYYB", "BBBYBYY", "YBBBBYY", "BYBYYBB",
    "BYYBBBY", "BYYBBYB", "BYYYBBB", "BYBBYBY", "YBBBYBY", "BYBYBYB",
    "BBYBYYB", "YYBYBBB", "YBBBYYB", "YYBBBBY", "BBBYYBY", "YBYBBBY",
    "BBYBYBY", "BBYYYBB",
    "YYYBBBB", "YYBBYBB", "YBYBBYB", "YBBYBBY", "YYBBBYB", "YBYBYBB",
    "BBYYBBY", "BBBBYYY", "YBBYYBB",
};
enum { C_CR = 26, C_LF, C_LTRS, C_FIGS, C_SPACE, C_NUL, C_BETA, C_ALPHA, C_RQ };
static const char figs_of[] = "-?:$3!&#8\0().,9014'57=2/6+";

/* --- the character stream -------------------------------------------------------- */

static int *seq;
static int nseq, capseq;

static void push(int c)
{
    if (nseq == capseq) {
        capseq = capseq ? capseq * 2 : 1024;
        seq = realloc(seq, sizeof(int) * (size_t)capseq);
    }
    seq[nseq++] = c;
}

static void text_to_codes(FILE *f)
{
    int figs = -1, c;
    while ((c = fgetc(f)) != EOF) {
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c == '\n') {
            push(C_CR);
            push(C_LF);
            continue;
        }
        if (c == ' ') {
            push(C_SPACE);
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            if (figs != 0) {
                push(C_LTRS);
                figs = 0;
            }
            push(c - 'A');
            continue;
        }
        for (int i = 0; i < 26; i++) {
            if (i != 9 && figs_of[i] == c) {
                if (figs != 1) {
                    push(C_FIGS);
                    figs = 1;
                }
                push(i);
                break;
            }
        }
    }
}

/* --- noise ---------------------------------------------------------------------------- */

static uint64_t rng = 88172645463325252ull;

static double urand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return ((rng >> 11) + 0.5) / 9007199254740992.0;
}

static double grand(void)
{
    return sqrt(-2 * log(urand())) * cos(2 * M_PI * urand());
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sitorb_gen OUT.wav [--snr DB] [--off HZ] [--ppm N] "
                        "[--rate HZ] [--centre HZ] [--rev] [--fade S] [--seed N] < text\n");
        return 2;
    }
    const char *out = argv[1];
    double snr = 1e9, off = 0, ppm = 0, rate = 12000, centre = 1000, fade = 0, sfade = 0;
    int rev = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--snr") && i + 1 < argc) snr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--off") && i + 1 < argc) off = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--centre") && i + 1 < argc) centre = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fade") && i + 1 < argc) fade = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sfade") && i + 1 < argc) sfade = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) rng = 88172645463325252ull ^ (uint64_t)atoll(argv[++i]) * 0x9E3779B97F4A7C15ull;
        else if (!strcmp(argv[i], "--rev")) rev = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    /* DX stream: phasing (RQ), the message, then idle alpha */
    int phasing = 24;
    for (int i = 0; i < phasing; i++)
        push(C_RQ);
    push(C_CR);
    push(C_LF);
    text_to_codes(stdin);
    push(C_CR);
    push(C_LF);
    for (int i = 0; i < 30; i++)
        push(C_ALPHA);

    /* slots: DX m, then RX = DX of m - 2 (280 ms later); alpha during phasing */
    int nslots = 2 * nseq;
    double spb = rate / 100.0 * (1 + ppm * 1e-6);
    double lead = 1.5 * rate, tail = 1.5 * rate;
    long n = (long)(lead + nslots * 7 * spb + tail);
    int16_t *pcm = calloc((size_t)n, sizeof(int16_t));
    if (!pcm)
        return 1;

    double amp = 0.25;
    /* noise power in 2500 Hz -> per-sample sigma over rate/2 */
    double sigma = snr > 1e8 ? 0 : sqrt((amp * amp / 2) / pow(10, snr / 10) * (rate / 2) / 2500);
    double ph = 0, fph = urand() * 2 * M_PI;
    long t = 0;
    for (; t < (long)lead; t++)
        pcm[t] = (int16_t)lrint(fmax(-32767, fmin(32767, sigma * grand() * 32767)));
    double tb = lead;
    for (int sl = 0; sl < nslots; sl++) {
        int m = sl / 2, code;
        if ((sl & 1) == 0)
            code = seq[m];
        else
            code = (m - 2 >= phasing) ? seq[m - 2] : C_ALPHA;
        for (int b = 0; b < 7; b++) {
            int is_b = code_bits[code][b] == 'B';
            double f = centre + off + ((is_b ^ rev) ? 85.0 : -85.0);
            double end = tb + spb;
            for (; t < (long)end && t < n; t++) {
                double g = 1;
                if (fade > 0) {
                    /* two slowly beating paths */
                    double a = 2 * M_PI * t / (rate * fade);
                    g = fabs(0.6 * cos(a + fph) + 0.55 * cos(1.7 * a));
                    g = fmax(g, 0.05) * 1.3;
                }
                if (sfade > 0) {
                    /* each tone has its own fading (different phases and
                     * rates): one can be deep while the other is not */
                    double a = 2 * M_PI * t / (rate * sfade) * (is_b ? 1.0 : 1.37);
                    double p0 = is_b ? fph : fph + 2.1;
                    g *= fmax(fabs(0.6 * cos(a + p0) + 0.55 * cos(1.7 * a + 1.3 * p0)), 0.05) * 1.3;
                }
                ph += 2 * M_PI * f / rate;
                double v = amp * g * sin(ph) + sigma * grand();
                pcm[t] = (int16_t)lrint(fmax(-32767, fmin(32767, v * 32767)));
            }
            tb = end;
        }
    }
    for (; t < n; t++)
        pcm[t] = (int16_t)lrint(fmax(-32767, fmin(32767, sigma * grand() * 32767)));

    FILE *f = fopen(out, "wb");
    if (!f)
        return 1;
    uint32_t data = (uint32_t)n * 2, r = (uint32_t)rate;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t v = 36 + data; memcpy(hdr + 4, &v, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    v = 16; memcpy(hdr + 16, &v, 4);
    uint16_t w = 1; memcpy(hdr + 20, &w, 2);    /* PCM */
    w = 1; memcpy(hdr + 22, &w, 2);             /* mono */
    memcpy(hdr + 24, &r, 4);
    v = r * 2; memcpy(hdr + 28, &v, 4);
    w = 2; memcpy(hdr + 32, &w, 2);
    w = 16; memcpy(hdr + 34, &w, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data, 4);
    fwrite(hdr, 1, 44, f);
    fwrite(pcm, 2, (size_t)n, f);
    fclose(f);
    return 0;
}
