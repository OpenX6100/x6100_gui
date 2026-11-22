/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - who hears you, live from PSK Reporter
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Both on the GUI thread. */
typedef struct {
    /* A station reported hearing you: `when` is the report's UTC seconds. */
    void (*on_spot)(const char *call, const char *grid, int snr, int64_t when);
    /* Listening (true), or the feed can't be reached and is being retried. */
    void (*on_state)(bool up);
} psk_reporter_cb_t;

/* The band's name in PSK Reporter ("20m"), NULL outside the amateur bands. */
const char *psk_reporter_band(int32_t hz);

/* As GridTracker: reports of `call` heard on `band` in `mode`, pushed by
 * PSK Reporter's MQTT feed (mqtt.pskreporter.info, plain TCP) as they arrive,
 * on a thread of its own. Only new ones: the feed has no history. A band of
 * NULL stops. GUI thread; returns at once, whatever the network does. */
void psk_reporter_watch(const char *call, const char *band, const char *mode, const psk_reporter_cb_t *cb);

#ifdef __cplusplus
}
#endif
