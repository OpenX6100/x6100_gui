/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - ITU region from the Maidenhead locator
 *
 *  Some calling frequencies differ between the three ITU regions (PSK31
 *  on 40 m is 7040 kHz in Region 1, 7070 in Region 2, 7035 in Region 3).
 *  The operator's locator (QTH Grid app) says where the station is, so
 *  the apps can pick the right one by themselves.
 *
 *  The borders are followed closely enough for a band plan, not to the
 *  kilometre: the Americas and Greenland are Region 2; Europe, Africa,
 *  the Middle East, the former USSR and Mongolia are Region 1; the rest
 *  of Asia and the Pacific are Region 3. A station within a grid square
 *  (about 100 km) of a border may get its neighbour's region, which the
 *  apps' dial offset puts right.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 1, 2 or 3 for a locator of 2, 4 or more characters ("JO94", "fn31pr"),
 * 0 when it is empty or not a locator. */
int itu_region_from_locator(const char *loc);

/* The region of the locator set in the QTH Grid app; Region 1 when none
 * is set (the apps' long-standing defaults). */
int itu_region(void);

#ifdef __cplusplus
}
#endif
