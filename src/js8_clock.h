/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8: the system clock's sync, and the battery clock
 *
 *  Kept apart from dialog_js8.c so the UI harness can stand in for both
 *  (it runs on a PC whose own clock must never be written).
 */

#pragma once

#include <stdbool.h>

/* The kernel says the system clock is disciplined (ntpd synchronized it;
 * on the radio, with no network, that means the GPS). Read-only. */
bool js8_clock_synced(void);

/* Writes the system time into the battery-backed clock (rtc1, the PCF8563
 * the kernel reads at boot): `hwclock -w -u -f /dev/rtc1`, once. rtc1 is
 * on i2c-0, the bus that also carries the amplifier's band data: never
 * call this in a loop. Blocks ~0.1 s: not on the GUI thread. */
bool js8_clock_save_rtc(void);
