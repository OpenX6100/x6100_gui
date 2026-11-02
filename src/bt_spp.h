/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_spp.h - Bluetooth SPP (RFCOMM) frame server.
 *
 * Carries the same WSJT-X datagrams that ft8_udp sends over UDP, so a
 * phone can receive them with no IP network at all.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest payload accepted by bt_spp_send(). WSJT-X messages are far
 * smaller; the limit exists so a corrupt length cannot allocate wildly. */
#define BT_SPP_MAX_FRAME 8192

typedef enum {
    BT_SPP_STOPPED = 0,
    BT_SPP_LISTENING,
    BT_SPP_CONNECTED,
    BT_SPP_ERROR,
} bt_spp_state_t;

/* Opens the RFCOMM listening socket and starts the worker thread.
 * Channel must match the one published in the SDP record by bt_up.sh. */
bool bt_spp_start(uint8_t channel);

/* Closes the client and the listening socket, joins the thread and frees
 * every queued frame. Safe to call when already stopped. */
void bt_spp_stop(void);

bt_spp_state_t bt_spp_state(void);

/* Address of the connected phone, "" when nobody is connected. */
const char *bt_spp_peer(void);

/* Queues one datagram. Never blocks, never writes a partial frame.
 * Returns false if there is no client or the queue is full - a dropped
 * decode is always better than a frozen interface. */
bool bt_spp_send(const void *payload, size_t len);

void bt_spp_stats(uint32_t *sent, uint32_t *dropped);

#ifdef __cplusplus
}
#endif
