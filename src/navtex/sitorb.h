/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * SITOR-B (FEC mode B) receiver for NAVTEX, written from ITU-R M.625 and
 * M.476 (7-unit code with 4 B and 3 Y, 100 Bd, 170 Hz shift, each character
 * sent twice: DX, then RX 280 ms later).
 *
 * Input: mono float audio. Output: one callback per character, with the
 * quality codes and the sync events of decoder.h (NAVTEX_Q_*, NAVTEX_EV_*).
 */
#ifndef X6100_NAVTEX_SITORB_H
#define X6100_NAVTEX_SITORB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sitorb sitorb_t;

/* c: an ASCII character ('\r' and '\n' included, '*' when lost), or a
 * NAVTEX_EV_* event when quality is NAVTEX_Q_EVENT */
typedef void (*sitorb_cb_t)(int c, int quality, void *user);

/* sample_rate: any rate from 8000 Hz up (the X6100 gives 12000 Hz).
 * centre_hz: audio frequency between the two tones (1000 Hz in 1KO125).
 * reverse: 0 when the higher tone is B (upper sideband), 1 to swap. */
sitorb_t *sitorb_create(float sample_rate, float centre_hz, int reverse);
void sitorb_destroy(sitorb_t *s);
void sitorb_set_callback(sitorb_t *s, sitorb_cb_t cb, void *user);
void sitorb_process(sitorb_t *s, const float *samples, unsigned int n);

/* state, for displays and tests */
int sitorb_in_sync(const sitorb_t *s);
float sitorb_freq_offset(const sitorb_t *s);   /* AFC correction, Hz */

#ifdef __cplusplus
}
#endif

#endif
