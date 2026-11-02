/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#ifndef SSTV_DECODER_H
#define SSTV_DECODER_H

#include <stdint.h>

#define SSTV_SAMPLE_RATE 12000
#define SSTV_MAX_W 320
#define SSTV_MAX_H 256
#define SSTV_ROBOT36_W 320
#define SSTV_ROBOT36_H 240

typedef enum {
    SSTV_MODE_UNKNOWN = 0,
    SSTV_MODE_ROBOT8_BW,
    SSTV_MODE_ROBOT36,
    SSTV_MODE_ROBOT72,
    SSTV_MODE_MARTIN_M1,
    SSTV_MODE_MARTIN_M2,
    SSTV_MODE_SCOTTIE_S1,
    SSTV_MODE_SCOTTIE_S2,
    SSTV_MODE_SCOTTIE_DX
} sstv_mode_t;

typedef struct {
    sstv_mode_t mode;
    int vis;
    int width;
    int height;
    const char *name;
} sstv_info_t;

/* Detect the SSTV VIS header in a mono SSTV_SAMPLE_RATE (12000 Hz) PCM WAV.
 * Returns 0 on success, <0 on error.  The filename is never used to
 * determine the mode.
 */
int sstv_detect_wav(const char *path, sstv_info_t *info);

/* Decode a mono SSTV_SAMPLE_RATE (12000 Hz) PCM WAV (Test RX).  The mode is
 * detected from the VIS header, then the WAV goes through the same RAM
 * decoder used for live RX.  rgb must hold at least SSTV_MAX_W*SSTV_MAX_H*3
 * bytes.  Returns decoded rows, or <0.
 */
int sstv_decode_wav(const char *path, uint8_t *rgb, sstv_info_t *info);

/* Decode an already-captured SSTV_SAMPLE_RATE (12000 Hz) mono sample buffer.
 * No VIS re-detection. */
int sstv_decode_samples(const float *samples, long count, sstv_mode_t mode, uint8_t *rgb, sstv_info_t *info);

/* Manual continuous RX decoder. The capture may start/end anywhere inside a
 * transmission and contains no VIS header. The normal Auto decoder path is
 * intentionally left untouched; this wrapper only supplies a synthetic
 * pre-roll so the existing robust per-mode decoders can lock to the first
 * usable sync found in the arbitrary capture. */
int sstv_decode_cont_samples(const float *samples, long count, sstv_mode_t mode, uint8_t *rgb, sstv_info_t *info);

/* ---- Progressive display support -------------------------------------
 * The dialog draws rows while a picture is being received; the final
 * picture still comes from sstv_decode_samples()/sstv_decode_cont_samples(),
 * which use the very same per-row renderers. */

/* FM front end of the decoders: frequency (Hz) of every sample of x[0..n).
 * Returns a malloc'd array (free() it) or NULL.  May be applied to a slice
 * of the capture: keep ~100 samples of margin on each side of a row. */
float *sstv_fm_demod(const float *x, long n);

/* Width in pixels of a row of mode (160 for Robot 8, else 320; 0 unknown). */
int sstv_row_width(sstv_mode_t mode);

/* Where the anchor sync of picture row 0 lies, in seconds after the end of
 * the VIS (0 for Robot/Martin; Scottie rows are anchored on the sync between
 * B and R).  Negative for unknown modes. */
double sstv_row0_anchor_after_vis(sstv_mode_t mode);

/* Robot 36 keeps the chroma of the previous row between calls. */
typedef struct {
    float chroma[320];
    int   chroma_valid;
    int   chroma_is_cr;
} sstv_row_state_t;

/* Draw one row of mode into rgb_row (sstv_row_width() * 3 bytes).
 * freq[0..n): output of sstv_fm_demod(); base: index in freq of the row's
 * anchor sync (line sync for Robot/Martin, B/R sync for Scottie); scale:
 * measured / nominal line period.  st may be NULL except for Robot 36
 * (zero it before the first row).  Returns 0, or <0 if the row's samples
 * are not all inside freq[0..n) or the mode is unknown. */
int sstv_render_row(sstv_mode_t mode, const float *freq, long n, long base, double scale,
                    sstv_row_state_t *st, uint8_t *rgb_row);

#endif
