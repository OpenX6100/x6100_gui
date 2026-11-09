/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4/FT2: the table of callsigns heard
 */

#pragma once

#include <ft8lib/message.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ft8_lib's hash table, which loops for ever once it is full (on a busy band
 * that takes minutes) and zeroes only a quarter of itself. Same hashes and
 * ages, but a search only ever looks at a limited number of places, and when
 * they're all taken the oldest call there goes. Not thread safe: the caller
 * locks. */
extern ftx_callsign_hash_interface_t ftx_call_table_if;

void ftx_call_table_reset(void);

/* Each slot: calls older than max_age slots go, the rest get a slot older. */
void ftx_call_table_age(uint8_t max_age);

#ifdef __cplusplus
}
#endif
