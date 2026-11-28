/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - every FT8 and FT4 frequency
 */

#include "ftx_freqs.h"

#include <stddef.h>

#include "ft8/ft2.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    uint32_t    hz;
    uint8_t     region;     /* 0: all three */
    const char *what;
} row_t;

/* WSJT-X 3.3 (models/FrequencyList.cpp), 160 m to 6 m. */
static const row_t ft8[] = {
    { 1840000, 0, NULL},
    { 3567000, 0, "DXpedition"},
    { 3573000, 0, NULL},
    { 5357000, 0, NULL},
    { 7056000, 0, "DXpedition"},
    { 7074000, 0, NULL},
    {10131000, 0, "DXpedition"},
    {10136000, 0, NULL},
    {14074000, 0, NULL},
    {14090000, 0, "DXpedition"},
    {18095000, 0, "DXpedition"},
    {18100000, 0, NULL},
    {21074000, 0, NULL},
    {21091000, 0, "DXpedition"},
    {24911000, 0, "DXpedition"},
    {24915000, 0, NULL},
    {28074000, 0, NULL},
    {28091000, 0, "DXpedition"},
    {50313000, 0, NULL},
    {50323000, 0, "intercontinental"},
};

static const row_t ft4[] = {
    { 3568000, 3, "Region 3"},
    { 3575000, 0, NULL},
    { 5357000, 0, NULL},
    { 7047500, 0, NULL},
    {10140000, 0, NULL},
    {14080000, 0, NULL},
    {18104000, 0, NULL},
    {21140000, 0, NULL},
    {24919000, 0, NULL},
    {28180000, 0, NULL},
    {50318000, 0, NULL},
};

int ftx_freqs(int protocol, int region, ftx_freq_t *out, int max)
{
    const row_t *rows;
    size_t       n;
    int          k = 0;

    if (protocol == (int)FTX_PROTOCOL_FT8) {
        rows = ft8;
        n = N(ft8);
    } else if (protocol == (int)FTX_PROTOCOL_FT4) {
        rows = ft4;
        n = N(ft4);
    } else {
        return 0;
    }
    for (size_t i = 0; i < n && k < max; i++) {
        if (rows[i].region != 0 && rows[i].region != region) {
            continue;
        }
        out[k].hz = rows[i].hz;
        out[k].what = rows[i].what;
        k++;
    }
    return k;
}

int ftx_freqs_closest(const ftx_freq_t *list, int n, uint32_t hz)
{
    int      best = 0;
    uint32_t best_d = UINT32_MAX;

    for (int i = 0; i < n; i++) {
        uint32_t d = list[i].hz > hz ? list[i].hz - hz : hz - list[i].hz;

        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}
