/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY transmitter
 *
 *  A queue of messages sent one after the other on their own thread:
 *  each is packed (jtty/jtty_pack.c), modulated and played at 12 kHz with
 *  the power capped at 5 W and the drive corrected from the ALC, as the
 *  FT8 window does. The radio stays keyed while messages are queued.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "jtty/jtty_pack.h"
#include "jtty/jtty_proto.h"
#include "jtty/jtty_source.h"

typedef struct {
    uint32_t id;                        /* from jtty_tx_queue() */
    float    freq;                      /* audio Hz of tone 0 */
    int      frame;                     /* being sent, 0-based */
    int      frames;
    bool     done;                      /* last frame gone, or stopped */
    bool     stopped;                   /* cut short (ESC, window closed) */
    bool     idle;                      /* unkeyed, the queue is empty */
    char     text[JTTY_MSG_MAX + 1];    /* as the receivers will show it */
} jtty_tx_status_t;

/* Called from the transmitter's thread at the start of every frame, when
 * a message ends and, with `idle`, once the radio is unkeyed. */
typedef void (*jtty_tx_cb_t)(const jtty_tx_status_t *st, void *ctx);

bool jtty_tx_start(jtty_tx_cb_t cb, void *ctx);
void jtty_tx_stop(void);                /* stops sending, joins the thread */

/* Queues text (normalized, packed) at `freq`. Returns its id, 0 if empty
 * or the queue is full. `norm` (may be NULL) gets the text as sent. */
uint32_t jtty_tx_queue(const char *text, float freq, char *norm, int norm_size);

/* Queues frames already packed (a native function key); `text` is how
 * the receivers will show them. Returns its id, 0 if the queue is full. */
uint32_t jtty_tx_queue_frames(const jtty_payload_t *payload, int frames, const char *text,
                              float freq);

/* Stops the message on the air at the end of the current chunk and
 * drops the queue. */
void jtty_tx_abort(void);

/* Message `id` is still queued or being sent. */
bool jtty_tx_pending(uint32_t id);

/* JTTY never transmits above this, whatever the radio is set to. */
#define JTTY_TX_MAX_PWR_W 5.0f              /* as FT8 and JS8 */

/* Keyed (or about to be) for JTTY. */
bool jtty_tx_busy(void);
