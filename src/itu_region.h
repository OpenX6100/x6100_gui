/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - ITU region from a position or a locator
 *
 *  Some calling frequencies and band plans differ between the three ITU
 *  regions (PSK31 on 40 m is 7040 kHz in Region 1, 7070 in Region 2, 7035
 *  in Region 3). The operator's locator says where the station is, so the
 *  apps can pick the right one by themselves.
 *
 *  The regions follow the Radio Regulations, Article 5 (RR 5.3 to 5.9):
 *
 *    Line A  the 40 E meridian from the North Pole to 40 N, a great
 *            circle to 60 E on the Tropic of Cancer, the 60 E meridian
 *            to the South Pole.
 *    Line B  the 10 W meridian from the North Pole to 72 N, great circles
 *            to 50 W 40 N and to 20 W 10 S, the 20 W meridian to the
 *            South Pole.
 *    Line C  a great circle from the North Pole to the Bering Strait at
 *            65 30' N, great circles to 165 E 50 N and to 170 W 10 N,
 *            the 10 N parallel to 120 W, the 120 W meridian to the South
 *            Pole.
 *
 *  Region 2 lies between Lines C and B, Region 1 between B and A, Region 3
 *  between A and C - except that the whole territory of Armenia,
 *  Azerbaijan, Georgia, Kazakhstan, Kyrgyzstan, Mongolia, Russia,
 *  Tajikistan, Turkey, Turkmenistan, Ukraine and Uzbekistan, and the area
 *  north of Russia between Lines A and C, is Region 1, and the whole of
 *  Iran is Region 3. Those borders are outlines a few tens of km close,
 *  which is what a 6-character locator resolves; the lines themselves are
 *  exact.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1, 2 or 3 for a position in degrees (north and east positive). */
int itu_region_from_pos(double lat, double lon);

/* 1, 2 or 3 for a locator of 2, 4, 6 or 8 characters ("JO94",
 * "fn31pr"), taken at the centre of what it names; 0 when it is empty or
 * not a locator. */
int itu_region_from_locator(const char *loc);

/* A value that depends on the region, such as a calling frequency, as
 * {Region 1, Region 2, Region 3}; 0 for Region 2 or 3 means Region 1's.
 * The one for `region` (1..3; anything else gives Region 1's). */
uint32_t itu_region_pick(const uint32_t v[3], int region);

/* Whether such a value differs between the regions. */
bool     itu_region_differs(const uint32_t v[3]);

#ifdef __cplusplus
}
#endif
