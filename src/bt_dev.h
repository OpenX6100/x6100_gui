/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_dev.h - known Bluetooth devices, for the table in dialog_bt.
 *
 * Everything goes to BlueZ over D-Bus on a worker thread (pairing can
 * take many seconds), and the window reads a cached snapshot.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_DEV_MAX 12

typedef struct {
    char addr[18];
    char name[32];
    bool connected;
    bool audio;      /* headset or speaker the radio can play to (never a phone) */
    bool input;      /* keyboard or other input device (HID) */
} bt_dev_t;

void bt_dev_init(void);
void bt_dev_deinit(void);

/* Asynchronous refresh of the snapshot. Call from the dialog timer. */
void bt_dev_refresh(void);

int  bt_dev_count(void);
bool bt_dev_get(int idx, bt_dev_t *out);

/* One device at a time: Connect blocks every other paired device
 * (BlueZ "Blocked", shown as "(off)") and lets this one in. A headset is
 * then connected (paired first when needed); a phone only unblocked, it
 * connects SPP from its app. Disconnect blocks the device, so a phone
 * cannot come straight back. Keyboards are left out of this: connecting
 * one blocks nothing, and others never block it. A keyboard that is not
 * paired yet is paired the moment it shows up after its pairing key is
 * pressed; a code to type on it shows in bt_dev_activity(). */
void bt_dev_connect(const char *addr);
void bt_dev_disconnect(const char *addr);

/* Disconnect and remove the pairing (Adapter1.RemoveDevice). The next
 * Connect pairs again, so the headset must be in pairing mode then. */
void bt_dev_forget(const char *addr);

/* Runs an inquiry for a fixed period, then refreshes the snapshot. It also
 * opens pairing for a few minutes (bt_dev_visible_s()): the radio is
 * visible, and a phone can pair from its side. The rest of the time it is
 * hidden and pairing requests are refused. */
void bt_dev_scan(void);
bool bt_dev_scanning(void);

/* What the worker is doing, "" when idle. For the status panel. */
const char *bt_dev_activity(void);

/* Seconds left in the pairing window, 0 when closed. */
int bt_dev_visible_s(void);

#ifdef __cplusplus
}
#endif
