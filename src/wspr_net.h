/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSPR: spots to a file, to WSPRnet, and who heard you
 *
 *  Every spot is appended to /mnt/wspr_spots.txt in wsprd's wspr_spots.txt
 *  layout, whether or not the radio is online, so a session can be looked
 *  at or uploaded later.
 *
 *  With WSPRnet on (page 3, saved), the spots of each receive slot are
 *  posted to wsprnet.org the way WSJT-X does (one form POST per spot to
 *  http://wsprnet.org/post/, or a "wsprstat" when the slot had none), and
 *  every 5 minutes the stations that heard you on this band in the last
 *  hour are fetched from wspr.live (a copy of WSPRnet's database) for the
 *  map's blue "heard you" lines.
 *
 *  Plain HTTP over a socket, on a thread of its own: the image's libcurl
 *  has no TLS, and neither service needs it. Nothing here blocks the
 *  caller. Uploads that fail stay queued (up to 64 spots) and go with the
 *  next slot.
 */

#pragma once

#include "wspr_worker.h"

#include <stdbool.h>
#include <stdint.h>

/* Who we are and what we send, captured on the caller's thread. */
typedef struct {
    char     call[16];  /* rcall: as set, prefixes and suffixes allowed */
    char     grid[8];   /* rgrid: 4 or 6 characters                     */
    uint32_t dial_hz;   /* USB dial frequency                           */
    uint16_t tx_hz;     /* our audio frequency                          */
    int      dbm;       /* our power, dBm                               */
    int      tpct;      /* how often we transmit, %                     */
} wspr_net_station_t;

/* A spot decoded in the slot that just ended (the worker thread). */
void wspr_net_spot(const wspr_spot_t *spot, const wspr_net_station_t *me);

/* The slot's decoding finished: post its spots, or a wsprstat if there
 * were none. `transmitted`: we sent in this slot, so there is nothing to
 * report. */
void wspr_net_slot_end(const wspr_net_station_t *me, bool transmitted);

/* Ask wspr.live who heard `call` on the band of `dial_hz` in the last
 * hour. The answers arrive on the LVGL thread as wspr_map_heard_you(). A
 * newer request (or wspr_net_forget()) makes older answers be dropped. */
void wspr_net_fetch_heard(const char *call, uint32_t dial_hz);

/* A new band or Clear: drop answers still on their way. */
void wspr_net_forget(void);

/* WSPRnet switch (cfg, saved). */
bool wspr_net_enabled(void);
void wspr_net_set_enabled(bool on);

/* For the button: "off", "on", "12 sent", "no net", ... */
const char *wspr_net_state(void);
