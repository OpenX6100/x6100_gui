/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * Test signal generator: DSC calls (ITU-R M.493, HF/MF, 100 Bd, 170 Hz)
 * as audio, with noise, frequency offset and fading.
 *
 *   dsc_gen OUT.wav [options] SYMBOLS...
 *     SYMBOLS      the call from the format specifier to the end of
 *                  sequence (the error check character is added): e.g.
 *                  120 0 22 41 2 20 108 22 97 16 0 0 118 126 126 126 126 126 126 126 117
 *     --calls N    the call N times, 6 s apart (default 1)
 *     --snr DB     signal to noise ratio in 2500 Hz (default: no noise)
 *     --off HZ     frequency offset (default 0)
 *     --fade S     fading, period in seconds
 *     --sfade S    selective fading (the two tones fade independently)
 *     --seed N
 *
 * 12000 Hz mono 16-bit WAV, tones at 1000 Hz -/+ 85 Hz (B = higher = "0").
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint64_t rng = 88172645463325252ull;
static double urand(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return ((rng >> 11) + 0.5) / 9007199254740992.0;
}
static double grand(void) { return sqrt(-2 * log(urand())) * cos(2 * M_PI * urand()); }

static int bits[200000];
static int nb;

static void put_char(int v)
{
    int ones = 0;
    for (int k = 0; k < 7; k++) { int b = (v >> k) & 1; bits[nb++] = b; ones += b; }
    int chk = 7 - ones;
    for (int k = 2; k >= 0; k--) bits[nb++] = (chk >> k) & 1;
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    double snr = 1e9, off = 0, fade = 0, sfade = 0, rate = 12000;
    int calls = 1, sym[64], ns = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--snr") && i + 1 < argc) snr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--off") && i + 1 < argc) off = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fade") && i + 1 < argc) fade = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sfade") && i + 1 < argc) sfade = atof(argv[++i]);
        else if (!strcmp(argv[i], "--calls") && i + 1 < argc) calls = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) rng ^= (uint64_t)atoll(argv[++i]) * 0x9E3779B97F4A7C15ull;
        else if (!out) out = argv[i];
        else if (ns < 60) sym[ns++] = atoi(argv[i]);
    }
    if (!out || ns < 3) {
        fprintf(stderr, "usage: dsc_gen OUT.wav [--calls N] [--snr DB] [--off HZ] [--fade S] [--sfade S] [--seed N] SYMBOLS...\n");
        return 2;
    }
    /* error check: XOR of the format specifier and everything up to the EOS */
    int ecc = 0;
    for (int i = 0; i < ns; i++) ecc ^= sym[i];

    /* DX stream: format x2, message (from index 1), EOS, ECC, EOS, EOS.
     * DX position k is followed by RX = DX of k - 2 (5 positions later);
     * before the message the RX positions carry the phasing 111..104. */
    int dx[128], ndx = 0;
    dx[ndx++] = sym[0];
    for (int i = 0; i < ns - 1; i++) dx[ndx++] = sym[i];
    dx[ndx++] = sym[ns - 1];
    dx[ndx++] = ecc;
    dx[ndx++] = sym[ns - 1];
    dx[ndx++] = sym[ns - 1];

    for (int c = 0; c < calls; c++) {
        /* silence */
        for (int i = 0; i < 200; i++) bits[nb++] = -1;
        /* dot pattern, 200 bits */
        for (int i = 0; i < 200; i++) bits[nb++] = i & 1;
        /* phasing: 6 DX 125 with RX 111..106, then the message */
        for (int k = 0; k < 6; k++) { put_char(125); put_char(111 - k); }
        for (int k = 0; k < ndx + 2; k++) {
            put_char(k < ndx ? dx[k] : 117);
            int r = k - 2;
            put_char(r < 0 ? 105 - (r + 2) : (r < ndx ? dx[r] : 117));
        }
        for (int i = 0; i < 200; i++) bits[nb++] = -1;
    }

    double spb = rate / 100.0, amp = 0.25;
    double sigma = snr > 1e8 ? 0 : sqrt((amp * amp / 2) / pow(10, snr / 10) * (rate / 2) / 2500);
    long n = (long)(nb * spb) + 1;
    int16_t *pcm = calloc((size_t)n, sizeof(int16_t));
    double ph = 0, fph = urand() * 6.28;
    for (long t = 0; t < n; t++) {
        int b = bits[(long)(t / spb) < nb ? (long)(t / spb) : nb - 1];
        double v = sigma * grand();
        if (b >= 0) {
            int hi = b == 0;                    /* B ("0") is the higher tone */
            double f = 1000 + off + (hi ? 85 : -85);
            ph += 2 * M_PI * f / rate;
            double g = 1;
            double a = 2 * M_PI * t / rate;
            if (fade > 0) g = fmax(fabs(0.6 * cos(a / fade + fph) + 0.55 * cos(1.7 * a / fade)), 0.05) * 1.3;
            if (sfade > 0) {
                double k = hi ? 1.0 : 1.37, p0 = hi ? fph : fph + 2.1;
                g *= fmax(fabs(0.6 * cos(a * k / sfade + p0) + 0.55 * cos(1.7 * a * k / sfade + 1.3 * p0)), 0.05) * 1.3;
            }
            v += amp * g * sin(ph);
        }
        pcm[t] = (int16_t)lrint(fmax(-32767, fmin(32767, v * 32767)));
    }
    FILE *f = fopen(out, "wb");
    uint32_t data = (uint32_t)n * 2, r = (uint32_t)rate, v32;
    uint16_t v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + data; fwrite(&v32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f); fwrite(&v16, 2, 1, f);
    fwrite(&r, 4, 1, f); v32 = r * 2; fwrite(&v32, 4, 1, f);
    v16 = 2; fwrite(&v16, 2, 1, f); v16 = 16; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, (size_t)n, f);
    fclose(f);
    return 0;
}
