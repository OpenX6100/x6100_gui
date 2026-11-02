/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_ctl.h - power and discoverability for the built-in Bluetooth adapter.
 *
 * The dialog only ever calls bt_ctl_request(). Bringing the adapter up
 * takes seconds - the Realtek part has its firmware pushed over a UART -
 * so it happens on a worker thread and the dialog reads bt_ctl_state()
 * from its refresh timer.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_CTL_OFF = 0,
    BT_CTL_STARTING,
    BT_CTL_ON,          /* adapter up, discoverable, SPP listening */
    BT_CTL_STOPPING,
    BT_CTL_FAILED,
} bt_ctl_state_t;

/* Reads /mnt/bt.conf and, if enabled=1, brings the adapter up. */
void bt_ctl_init(void);

/* Stops the worker thread and powers the adapter down. */
void bt_ctl_deinit(void);

/* Asynchronous. Returns immediately; watch bt_ctl_state(). */
void bt_ctl_request(bool on);

bt_ctl_state_t bt_ctl_state(void);

/* Adapter address as "AA:BB:CC:DD:EE:FF", or "" when down. */
const char *bt_ctl_addr(void);

/* Name the radio advertises. Persisted to /mnt/bt.conf. */
const char *bt_ctl_name(void);
void        bt_ctl_set_name(const char *name);

/* True once the adapter is up and inquiry scan is enabled, which is the
 * only state in which the phone can find us. */
bool bt_ctl_discoverable(void);

#ifdef __cplusplus
}
#endif
