/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - DSC (Digital Selective Calling) decoder, MF/HF
 *
 * ITU-R M.493: 100 baud FSK, 170 Hz shift, 10-bit symbols (7 information
 * bits LSB first + 3 check bits = number of B "0" bits, MSB first), the
 * low tone is Y ("1").  Every character is sent twice (DX and, a few
 * positions later, RX); a call starts with a dot pattern and the phasing
 * sequence (DX 125, RX 111..104).
 *
 * Input: mono float audio at any rate >= 8000 Hz (12000 Hz from the X6100
 * DSP path).  The tones are searched around DSC_CENTER_HZ +/- 60 Hz, so a
 * small mistuning or transmitter drift is tolerated.
 *
 * Thread model: dsc_decoder_process() and the callback run in the caller's
 * thread (the DSP audio thread); the callback must only copy the message.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_CENTER_HZ    1000.0f   /* audio centre: dial = frequency - 1 kHz, USB */
#define DSC_MAX_SYMBOLS  64        /* merged message, format specifier .. ECC */

typedef struct {
    int   sym[DSC_MAX_SYMBOLS];    /* merged DX/RX characters, -1 = unreadable */
    int   n;
    int   raw_total;               /* received character positions */
    int   raw_valid;               /* ... of which with a valid check */
    int   repaired;                /* characters recovered from the RX copy */
    float center_hz;               /* audio centre of the demodulator that decoded it */
    bool  ecc_ok;                  /* error check character verified (n trimmed after it) */
    int   unsure;                  /* characters not received twice the same (soft decisions,
                                      filled in): with more than one, two wrong characters can
                                      cancel out in the error check, so it does not verify */
    bool  verified;                /* the call can be trusted: error check right, at most one
                                      unsure character, fields of the right kind */
    double start_t;                /* seconds of audio processed when the call began */
} dsc_message_t;

typedef void (*dsc_message_cb_t)(const dsc_message_t *msg, void *user);

typedef struct dsc_decoder dsc_decoder_t;

dsc_decoder_t *dsc_decoder_create(int sample_rate, dsc_message_cb_t cb, void *user);
void dsc_decoder_destroy(dsc_decoder_t *d);
void dsc_decoder_process(dsc_decoder_t *d, const float *samples, int n);
/* Emit a call still waiting for the de-duplication window (end of a WAV). */
void dsc_decoder_flush(dsc_decoder_t *d);

#ifdef __cplusplus
}
#endif
