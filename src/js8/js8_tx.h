/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 transmit, C interface for the dialog
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "js8_rx.h" /* JS8_RX_TEXT_LEN */
#include "js8_speed.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JS8_TX_ERR_LEN      96
#define JS8_TX_MIN_OFFSET   500 /* the top is js8_speed_max_offset_hz() */

typedef enum {
    JS8_TX_IDLE,
    JS8_TX_WAITING, /* queued; next frame starts at next_ms */
    JS8_TX_KEYING,  /* a frame is on the air */
} js8_tx_state_t;

typedef struct {
    js8_tx_state_t state;
    int            frame;  /* 1-based */
    int            frames;
    int64_t        next_ms; /* wall clock, ms since the epoch */
    float          offset_hz;
    js8_speed_t    speed;
    char           text[JS8_RX_TEXT_LEN];
} js8_tx_status_t;

/* What a message would look like, before sending it. */
typedef struct {
    bool  ok;
    int   frames;
    float seconds;                  /* air time */
    char  preview[JS8_RX_TEXT_LEN]; /* as other stations will see it */
    char  error[JS8_TX_ERR_LEN];
} js8_tx_preview_t;

/* All callbacks run on the transmitter's thread. */
typedef struct {
    /* Key the radio and play one frame (int16 at the rate given to
     * js8_tx_create, full scale x0.8, centred on synth_hz). Block until done;
     * return false if stopped early. The buffer may be modified. */
    bool (*play)(int16_t *samples, unsigned n, int index, int count, void *ctx);
    void (*on_status)(const js8_tx_status_t *status, void *ctx);
    void (*on_done)(const char *text, bool completed, void *ctx);
    void *ctx;
} js8_tx_cb_t;

typedef struct js8_tx js8_tx_t;

void js8_tx_preview(const char *my_call, const char *my_grid, const char *text, js8_speed_t speed,
                    js8_tx_preview_t *out);
bool js8_tx_sendable_char(char c);

/* rate: audio output rate. synth_hz: tone the audio is generated around
 * (0 = at the requested offset). */
js8_tx_t *js8_tx_create(int rate, float synth_hz, const js8_tx_cb_t *cb);

/* Queue a message at offset_hz and speed. On failure returns false with a
 * reason. */
bool js8_tx_send(js8_tx_t *tx, const char *my_call, const char *my_grid, const char *text, float offset_hz,
                 js8_speed_t speed, char *err, unsigned err_len);

/* Abandon the current message. Returns at once; js8_tx_busy() turns false
 * once the current frame's play() has returned. Safe from any thread. */
void js8_tx_stop(js8_tx_t *tx);
bool js8_tx_busy(js8_tx_t *tx);
bool js8_tx_stopping(js8_tx_t *tx);

/* Stops and waits for the transmitter thread. NULL is ignored. */
void js8_tx_destroy(js8_tx_t *tx);

#ifdef __cplusplus
}
#endif
