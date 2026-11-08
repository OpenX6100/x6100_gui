/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - ITU region from the Maidenhead locator
 */

#include "itu_region.h"

#include <ctype.h>

#include "cfg/cfg_api.h"

/* Centre of the locator's field (2 characters) or square (4 or more). */
static int locator_pos(const char *loc, double *lon, double *lat)
{
    int a, b;

    if (loc == NULL) {
        return -1;
    }
    a = toupper((unsigned char)loc[0]) - 'A';
    b = toupper((unsigned char)loc[1]) - 'A';
    if (a < 0 || a > 17 || b < 0 || b > 17) {
        return -1;
    }
    *lon = a * 20.0 - 180.0 + 10.0;
    *lat = b * 10.0 - 90.0 + 5.0;
    if (isdigit((unsigned char)loc[2]) && isdigit((unsigned char)loc[3])) {
        *lon = a * 20.0 - 180.0 + (loc[2] - '0') * 2.0 + 1.0;
        *lat = b * 10.0 - 90.0 + (loc[3] - '0') * 1.0 + 0.5;
    }
    return 0;
}

int itu_region_from_locator(const char *loc)
{
    double lon, lat;

    if (locator_pos(loc, &lon, &lat) != 0) {
        return 0;
    }

    /* The Americas, Greenland, Hawaii */
    if (lon >= -170.0 && lon < -20.0) {
        return 2;
    }
    /* West of 170 W: Chukotka, else Samoa, Tonga and the Pacific */
    if (lon < -170.0) {
        return lat >= 60.0 ? 1 : 3;
    }
    if (lon < 61.0) {
        return 1;                       /* Europe, Africa, Middle East, Iran */
    }

    /* East of Iran: Region 1 is the former USSR and Mongolia, the rest
     * of Asia and the Pacific Region 3. */
    if (lat >= 50.0) {
        return 1;                       /* Siberia, Kazakhstan's north */
    }
    if (lon < 75.0) {
        return lat >= 37.0 ? 1 : 3;     /* Central Asia / Afghanistan, Pakistan */
    }
    if (lon < 87.0) {
        return lat >= 40.0 ? 1 : 3;     /* Kyrgyzstan, Kazakhstan / Xinjiang, India */
    }
    if (lon < 120.0) {
        return lat >= 42.0 ? 1 : 3;     /* Mongolia / China */
    }
    if (lon >= 130.0 && lon < 140.0 && lat >= 42.5) {
        return 1;                       /* Primorye (Vladivostok) */
    }
    return 3;                           /* China, Korea, Japan, Australia... */
}

int itu_region(void)
{
    char loc[PARAM_TEXT_MAX];
    int  r;

    param_t_get_into(cfg.qth(), loc, sizeof(loc));
    r = itu_region_from_locator(loc);
    return r ? r : 1;
}
