/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - PSK31 receiver
 *
 *  BPSK31 demodulator: complex downconversion to baseband, narrow
 *  low-pass, decimation to 8 samples per symbol, symbol timing recovery
 *  from the envelope dip that PSK31's shaping produces at every phase
 *  reversal, differential phase detection and Varicode decoding.
 *
 *  Decoded text goes to the PSK31 window through psk_rx_set_text_cb(),
 *  or to the panel when no callback is set.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Rate the receiver asks the DSP for. 12000 / 24 = 500 Hz after the
 * first decimation, 16.0 samples per symbol, the working point the
 * filters and the timing loop are tuned for. The PSK31 window feeds its
 * waterfall from the same audio, so it uses this rate too.
 */
#define PSK_RX_RATE 12000

#ifdef __cplusplus
extern "C" {
#endif

void psk_rx_init(void);

/* Enable/disable the receiver. While disabled it consumes nothing. */
void psk_rx_set_active(bool on);
bool psk_rx_is_active(void);

/* Audio at PSK_RX_RATE. psk_rx_init() subscribes this to the DSP; it is
 * public only so a host test can drive the receiver directly. */
void psk_rx_put_audio_samples(size_t n, float *samples);

/* Re-read the centre frequency (called when the operator retunes). */
void psk_rx_update_center(void);

/* Measured signal quality, for display: estimated frequency error in Hz
 * and a 0..100 lock indicator. */
float psk_rx_freq_error(void);
uint8_t psk_rx_quality(void);

/*
 * Hooks for the PSK31 window. Both are called from the DSP audio thread
 * with the receiver's lock (and the DSP's subscriber lock) held, so they must be short and must not call
 * back into this module; hand the work to the UI thread instead.
 *
 * text_cb: when set, decoded characters go here instead of the panel.
 * audio_tap: when set, sees every block of audio the receiver gets,
 *            before it is processed, while the receiver is active.
 *
 * After psk_rx_set_active(false) returns neither is called again.
 */
typedef void (*psk_rx_text_cb_t)(char c);
typedef void (*psk_rx_audio_tap_t)(unsigned int n, const float *samples);

void psk_rx_set_text_cb(psk_rx_text_cb_t cb);
void psk_rx_set_audio_tap(psk_rx_audio_tap_t tap);

#ifdef __cplusplus
}
#endif
