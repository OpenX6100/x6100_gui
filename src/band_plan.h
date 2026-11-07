/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - amateur band plans for the band bar
 *
 *  The HF and 6 m bands of each ITU region with their segments (CW,
 *  digital, phone / all modes), simplified from the IARU regions' band
 *  plans: a guide to where a band starts and stops and what goes where,
 *  not a licence. Countries differ (60 m above all); the operator's own
 *  rules come first.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BAND_PLAN_CW,
    BAND_PLAN_DIGITAL,
    BAND_PLAN_PHONE,       /* phone / all modes */
} band_plan_kind_t;

typedef struct {
    uint32_t         lo_hz;
    uint32_t         hi_hz;
    band_plan_kind_t kind;
} band_plan_seg_t;

/* The region the band plan follows: Settings > Band plan, or with Auto the
 * QTH Grid locator's (itu_region()). 1, 2 or 3. */
int band_plan_region(void);

/* The band hz is in ("40m"), or NULL outside the amateur bands. */
const char *band_plan_band_name(uint32_t hz);

/* The segments of `region` that overlap lo_hz..hi_hz, low to high, at most
 * `max`; returns how many. What lies between them is outside the bands. */
int band_plan_segments(int region, uint32_t lo_hz, uint32_t hi_hz, band_plan_seg_t *out, int max);

#ifdef __cplusplus
}
#endif
