/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - WSPR application layer
 *
 *  Ties the protocol modules to the radio: audio in from a DSP
 *  subscription, decoded spots out to the window that owns it, and
 *  scheduled transmissions.
 *
 *  WSPR is a beacon mode. Stations transmit in the 120 s slot aligned to
 *  even UTC minutes, starting one second in, and listen the rest of the
 *  time. The operator chooses how often to transmit and with how much
 *  power.
 *
 *  Two separate settings:
 *
 *    - the transmit period, "one slot in N", which decides how often
 *    - the transmit power in milliwatts, which decides how hard
 *
 *  The power is applied to the radio for the duration of the
 *  transmission only, and the operator's normal setting is put back
 *  afterwards.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wspr_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where decoded spots, spectrum lines and short notices go. All three
 * fire on the worker thread, so a handler that touches LVGL must hand
 * off with scheduler_put() rather than poke the widget directly.
 *
 * Set to NULL and the application decodes silently, which is what
 * happens between closing the window and opening it again.
 */
typedef struct {
    void (*on_spot)(const wspr_spot_t *spot, void *ctx);
    void (*on_psd)(const float *mag_db, int nbins, void *ctx);
    void (*on_slot_end)(void *ctx);
    void (*on_note)(const char *text, void *ctx);
    void *ctx;
} wspr_app_ui_cb_t;

void wspr_app_set_ui_cb(const wspr_app_ui_cb_t *cb);

/*
 * Transmit power limits. WSPR is a weak signal beacon mode: 100 mW
 * routinely crosses oceans and running more is mostly a waste of
 * battery, so the useful range on this radio ends well below its full
 * output. The step matches what the operator can meaningfully hear the
 * difference of, and every value maps onto a reportable dBm figure.
 */
#define WSPR_PWR_MIN_MW     100
#define WSPR_PWR_MAX_MW     2000
#define WSPR_PWR_STEP_MW    100
#define WSPR_PWR_DEFAULT_MW 500

void wspr_app_init(void);

/* Enable and disable reception. Disabled it consumes nothing. */
void wspr_app_set_active(bool on);
bool wspr_app_is_active(void);

/* Audio from the DSP subscription, at WSPR_DEMOD_RATE (12000 Hz). The
 * application subscribes itself in wspr_app_init(). */
void wspr_app_put_audio_samples(size_t n, float *samples);

/* The receiver decodes the whole WSPR window, 1400 to 1600 Hz, centred
 * here, as WSJT-X does. */
#define WSPR_APP_RX_CENTER  1500

/* Audio frequency of our transmission (the middle of its four tones),
 * 1400 to 1600 Hz. */
uint16_t wspr_app_get_tx_freq(void);
void     wspr_app_set_tx_freq(uint16_t hz);

/*
 * Transmit period: 0 means receive only, N means one slot in N. Which
 * slot within each group of N is picked at random, so that stations
 * sharing a period do not lock into the same minutes and permanently
 * hide each other, while the wait still stays bounded - a purely random
 * draw at a low duty cycle could go for half an hour without keying up,
 * which looks exactly like a broken transmitter.
 *
 * A period of 1 transmits in every slot. That leaves no time to hear
 * anyone and is meant for testing the transmit path, not for operating.
 */
uint8_t wspr_app_get_tx_period(void);
void    wspr_app_set_tx_period(uint8_t n);

/* Transmit power in milliwatts, WSPR_PWR_MIN_MW..WSPR_PWR_MAX_MW. */
uint16_t wspr_app_get_tx_pwr_mw(void);
void     wspr_app_set_tx_pwr_mw(uint16_t mw);

/*
 * The power figure that goes out in the message, in dBm. WSPR can only
 * carry values ending in 0, 3 or 7, so this is the nearest legal one to
 * the actual output, which is what every other implementation reports.
 */
int wspr_app_tx_dbm(void);

/* True while a transmission is in progress. */
bool wspr_app_is_transmitting(void);

/*
 * Why transmission is currently not possible, or NULL when it is.
 * Returns a short reason for display: no callsign, no locator, the
 * clock is not synchronised, or the radio is busy elsewhere.
 */
const char *wspr_app_tx_blocked_reason(void);

/* Seconds into the current 120 s slot, for a progress indicator. */
float wspr_app_slot_progress(void);

/* Number of spots decoded since the application was started. */
uint32_t wspr_app_spot_count(void);

#ifdef __cplusplus
}
#endif
