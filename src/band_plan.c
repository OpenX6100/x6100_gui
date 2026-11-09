/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - amateur band plans for the band bar
 */

#include "band_plan.h"

#include "cfg/cfg_api.h"
#include "itu_region.h"

#define C BAND_PLAN_CW
#define D BAND_PLAN_DIGITAL
#define P BAND_PLAN_PHONE

/* kHz x 10, so 60 m's half kHz fit */
typedef struct {
    uint32_t lo, hi;
    uint8_t  kind;
} seg_t;

/* Simplified from the IARU Region 1 HF band plan and its 6 m plan. */
static const seg_t region1[] = {
    {18100, 18380, C}, {18380, 18430, D}, {18430, 20000, P},
    {35000, 35700, C}, {35700, 36000, D}, {36000, 38000, P},
    {53515, 53540, C}, {53540, 53660, P}, {53660, 53665, D},
    {70000, 70400, C}, {70400, 70500, D}, {70500, 72000, P},
    {101000, 101300, C}, {101300, 101500, D},
    {140000, 140700, C}, {140700, 141120, D}, {141120, 143500, P},
    {180680, 180950, C}, {180950, 181110, D}, {181110, 181680, P},
    {210000, 210700, C}, {210700, 211510, D}, {211510, 214500, P},
    {248900, 249150, C}, {249150, 249310, D}, {249310, 249900, P},
    {280000, 280700, C}, {280700, 282250, D}, {282250, 297000, P},
    {500000, 501000, C}, {501000, 503000, P}, {503000, 504000, D}, {504000, 520000, P},
};

/* Simplified from the IARU Region 2 band plan. */
static const seg_t region2[] = {
    {18000, 18100, D}, {18100, 18400, C}, {18400, 18500, D}, {18500, 20000, P},
    {35000, 35700, C}, {35700, 36000, D}, {36000, 40000, P},
    {53515, 53540, C}, {53540, 53660, P}, {53660, 53665, D},
    {70000, 70400, C}, {70400, 70600, D}, {70600, 73000, P},
    {101000, 101300, C}, {101300, 101500, D},
    {140000, 140700, C}, {140700, 141120, D}, {141120, 143500, P},
    {180680, 180950, C}, {180950, 181110, D}, {181110, 181680, P},
    {210000, 210700, C}, {210700, 211500, D}, {211500, 214500, P},
    {248900, 249150, C}, {249150, 249310, D}, {249310, 249900, P},
    {280000, 280700, C}, {280700, 283000, D}, {283000, 297000, P},
    {500000, 501000, C}, {501000, 503000, P}, {503000, 504000, D}, {504000, 540000, P},
};

/* Simplified from the IARU Region 3 band plan. */
static const seg_t region3[] = {
    {18000, 18300, C}, {18300, 18400, D}, {18400, 20000, P},
    {35000, 35700, C}, {35700, 36000, D}, {36000, 39000, P},
    {53515, 53540, C}, {53540, 53660, P}, {53660, 53665, D},
    {70000, 70300, C}, {70300, 70400, D}, {70400, 72000, P},
    {101000, 101300, C}, {101300, 101500, D},
    {140000, 140700, C}, {140700, 141120, D}, {141120, 143500, P},
    {180680, 180950, C}, {180950, 181110, D}, {181110, 181680, P},
    {210000, 210700, C}, {210700, 211510, D}, {211510, 214500, P},
    {248900, 249150, C}, {249150, 249310, D}, {249310, 249900, P},
    {280000, 280700, C}, {280700, 282250, D}, {282250, 297000, P},
    {500000, 501000, C}, {501000, 503000, P}, {503000, 504000, D}, {504000, 540000, P},
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The bands' names, by their widest edges in any region (kHz x 10). */
static const struct {
    uint32_t    lo, hi;
    const char *name;
} band_names[] = {
    {18000, 20000, "160m"}, {35000, 40000, "80m"},  {53515, 53665, "60m"},  {70000, 73000, "40m"},
    {101000, 101500, "30m"}, {140000, 143500, "20m"}, {180680, 181680, "17m"}, {210000, 214500, "15m"},
    {248900, 249900, "12m"}, {280000, 297000, "10m"}, {500000, 540000, "6m"},
};

const char *band_plan_band_name(uint32_t hz) {
    for (int i = 0; i < N(band_names); i++) {
        if (hz >= band_names[i].lo * 100 && hz < band_names[i].hi * 100) {
            return band_names[i].name;
        }
    }
    return NULL;
}

int band_plan_region(void) {
    char loc[PARAM_TEXT_MAX];
    int  r = param_i_get(cfg.ui.band_plan_region());

    if (r >= 1 && r <= 3) {
        return r;
    }
    param_t_get_into(cfg.qth(), loc, sizeof(loc));
    r = itu_region_from_locator(loc);
    return r ? r : 1;                   /* no locator: Region 1, as before */
}

int band_plan_segments(int region, uint32_t lo_hz, uint32_t hi_hz, band_plan_seg_t *out, int max) {
    const seg_t *t = region == 2 ? region2 : region == 3 ? region3 : region1;
    int          n = region == 2 ? N(region2) : region == 3 ? N(region3) : N(region1);
    int          k = 0;

    for (int i = 0; i < n && k < max; i++) {
        uint32_t a = t[i].lo * 100, b = t[i].hi * 100;
        if (b <= lo_hz || a >= hi_hz) {
            continue;
        }
        out[k].lo_hz = a;
        out[k].hi_hz = b;
        out[k].kind  = (band_plan_kind_t)t[i].kind;
        k++;
    }
    return k;
}
