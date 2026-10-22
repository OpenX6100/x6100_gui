/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#ifndef SSTV_ENCODER_H
#define SSTV_ENCODER_H

#include <stddef.h>
#include <stdint.h>

/* Test encoders: read a PNG, scale it to the native mode geometry, and write
 * an SSTV_SAMPLE_RATE (12000 Hz) mono 16-bit PCM WAV including the
 * appropriate VIS header.
 * Returns 0 on success, <0 on error. */
int sstv_encode_martin_m2_png_wav(const char *png_path, const char *wav_path);
int sstv_encode_robot36_png_wav(const char *png_path, const char *wav_path);

/* Generate Martin M2 directly as signed 16-bit mono PCM for radio TX. */
int sstv_encode_martin_m2_png_pcm(const char *png_path, unsigned sample_rate,
                                  int16_t **samples, size_t *sample_count);

/* Generate Robot 36 directly as signed 16-bit mono PCM.  The caller owns
 * *samples and must free() it.  sample_rate is normally AUDIO_PLAY_RATE. */
int sstv_encode_robot36_png_pcm(const char *png_path, unsigned sample_rate,
                                int16_t **samples, size_t *sample_count);
int sstv_encode_scottie_s1_png_wav(const char *png_path, const char *wav_path);

/* Generate Scottie S1 directly as signed 16-bit mono PCM for radio TX. */
int sstv_encode_scottie_s1_png_pcm(const char *png_path, unsigned sample_rate,
                                   int16_t **samples, size_t *sample_count);


/* Additional direct PCM encoders for SSTV Test TX. */
int sstv_encode_martin_m1_png_pcm(const char *, unsigned, int16_t **, size_t *);
int sstv_encode_scottie_s2_png_pcm(const char *, unsigned, int16_t **, size_t *);
int sstv_encode_robot8_png_pcm(const char *, unsigned, int16_t **, size_t *);
int sstv_encode_robot72_png_pcm(const char *, unsigned, int16_t **, size_t *);

#endif
