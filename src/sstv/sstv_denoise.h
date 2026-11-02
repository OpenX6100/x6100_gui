/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */

/*
 * SSTV picture denoiser.  Plain C, no dependencies: the same file is built
 * into the firmware and into the test program for the computer.
 *
 * The noise of a received SSTV picture is not like camera noise: each line is
 * sent in turn, and the FM demodulator smooths along the line, so the errors
 * are short horizontal dashes, independent from one line to the next, and a
 * burst of QRM or a fade spoils a whole line or a piece of it.  In Martin and
 * Scottie each colour has its own scan (coloured speckle); in Robot the
 * brightness carries most of the noise.  So the picture is worked in
 * brightness and colour (Y, Cb, Cr), mostly across the lines:
 *
 *  1. Noise level of each plane, measured on the picture itself (robust
 *     median of the difference between a pixel and the lines above/below).
 *  2. Band repair: a burst of QRM spoils a band of a few lines.  Each piece
 *     of line (32 pixels) much busier than the lines near it, in a band at
 *     most 8 lines tall, is rebuilt from the good lines above and below.
 *  3. Line repair: a value that stands out from the line above AND the one
 *     below, in the same direction, by more than a few times the noise, is
 *     replaced by the median of the 5 values of its column (y-2..y+2).  Fixes
 *     dashes, spoiled pieces of a line and whole spoiled lines.
 *  4. Edge-preserving smoothing (cross bilateral filter): each value becomes
 *     the weighted mean of its neighbours that look like it, the strength
 *     following the measured noise.  Brightness lightly, over 5 lines x 3
 *     pixels; colour more, over 5 x 5, guided by the brightness so that the
 *     colours do not run over the edges.  On a clean picture the measured
 *     noise is low and almost nothing is changed.
 *
 * Rows that are all black at the bottom (picture not complete) are left out.
 */
#ifndef SSTV_DENOISE_H
#define SSTV_DENOISE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float sigma_y, sigma_cb, sigma_cr;  /* measured noise, 0..255 levels */
    int   rows;                          /* rows worked (black rows at the bottom left out) */
    int   repaired;                      /* values replaced by the line repair */
    int   bands;                         /* pieces of line (32 pixels) rebuilt by the band repair */
} sstv_denoise_stats_t;

/* Cleans rgb (width x height, 3 bytes per pixel, rows packed) in place.
 * stats may be NULL.  Returns 0, or <0 if out of memory (rgb untouched). */
int sstv_denoise(uint8_t *rgb, int width, int height, sstv_denoise_stats_t *stats);

/* Only measures the noise (sigma_* and rows of stats; rgb untouched).
 * Returns 1 if the picture is noisy (the denoiser would work on it), 0 if it
 * is clean, <0 if out of memory. */
int sstv_denoise_measure(const uint8_t *rgb, int width, int height, sstv_denoise_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
