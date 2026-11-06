/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
#ifndef X6100_NAVTEX_DECODER_H
#define X6100_NAVTEX_DECODER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct navtex_decoder navtex_decoder_t;
typedef void (*navtex_char_cb_t)(int c, void *userdata);

/* Reliability of a received character */
#define NAVTEX_Q_CLEAN   0      /* received correctly */
#define NAVTEX_Q_REP     1      /* taken from the repeated (FEC) copy */
#define NAVTEX_Q_GUESS   2      /* reconstructed, may be wrong */
#define NAVTEX_Q_LOST    3      /* lost, emitted as '*' */
#define NAVTEX_Q_EVENT  -1      /* c is an event, not a character: */
#define NAVTEX_EV_SYNC_ON   -1  /*   text starts (decoder in sync) */
#define NAVTEX_EV_SYNC_OFF  -2  /*   text stops (sync lost) */

typedef void (*navtex_char_q_cb_t)(int c, int quality, void *userdata);

/*
 * Create the validated NAVTEX/SITOR-B decoder.
 * Input audio is mono float at 12000 Hz, requested from the X6100 DSP audio path.
 * reverse=false matches normal polarity; pass true when RV is required.
 * For this first integration stage decoded characters are written to stdout.
 */
navtex_decoder_t *navtex_decoder_create(int reverse);
void navtex_decoder_destroy(navtex_decoder_t *decoder);
void navtex_decoder_set_char_callback(navtex_decoder_t *decoder, navtex_char_cb_t cb, void *userdata);
void navtex_decoder_set_char_q_callback(navtex_decoder_t *decoder, navtex_char_q_cb_t cb, void *userdata);
void navtex_decoder_process(navtex_decoder_t *decoder,
                            const float *samples,
                            unsigned int n);

#ifdef __cplusplus
}
#endif

#endif
