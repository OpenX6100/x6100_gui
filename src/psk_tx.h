/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - PSK31 transmitter
 *
 *  BPSK31 modulator: Varicode encoding, 31.25 baud, cosine-shaped
 *  envelope through phase reversals (this shaping is what keeps the
 *  occupied bandwidth near 60 Hz). Streams through the same modem TX
 *  path used by the FT8 worker.
 *
 *  Transmit only; the receiver is psk_rx.c. Audio is generated at
 *  48 kHz for the default PulseAudio player.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Centre (audio) frequency of the BPSK carrier, Hz. */
uint16_t psk_tx_get_center(void);
void     psk_tx_set_center(uint16_t hz);

/* Start transmitting text asynchronously. False if already running. */
bool psk_tx_send(const char *text);

/* Request a stop; the current symbol finishes, then the postamble runs. */
void psk_tx_stop(void);

bool psk_tx_is_on(void);

/* Text entry window (USB/BT keyboard), OK transmits. */
void psk_tx_open_input(void);


/* Varicode code string for a character, or NULL. Shared with the
 * receiver so the table exists in exactly one place. */
const char *psk_tx_varicode(unsigned char c);

/* The PSK31 window shows what is sent in its own text area: when set,
 * psk_tx_send() hands the text here. Called on the thread that calls
 * psk_tx_send(), which is the UI thread. */
typedef void (*psk_tx_echo_cb_t)(const char *text);
void psk_tx_set_echo_cb(psk_tx_echo_cb_t cb);

#ifdef __cplusplus
}
#endif
