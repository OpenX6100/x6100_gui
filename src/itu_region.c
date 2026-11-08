/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - ITU region from a position or a locator
 */

#include "itu_region.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "qth/qth.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define RAD  (M_PI / 180.0)

/* Longitudes here run east from 0 to 360, so that Line C, which crosses
 * the 180th meridian, is one continuous line: 170 W is 190. */
static double east(double lon)
{
    while (lon < 0.0) {
        lon += 360.0;
    }
    while (lon >= 360.0) {
        lon -= 360.0;
    }
    return lon;
}

/* ---- the lines (RR 5.9) ------------------------------------------------- */

/* The latitude of the great circle through (lon1, lat1) and (lon2, lat2)
 * where it crosses meridian `lon`. */
static double gc_lat(double lon1, double lat1, double lon2, double lat2, double lon)
{
    double t = tan(lat1 * RAD) * sin((lon2 - lon) * RAD) + tan(lat2 * RAD) * sin((lon - lon1) * RAD);

    return atan(t / sin((lon2 - lon1) * RAD)) / RAD;
}

/* Where between lon1 and lon2 that great circle reaches `lat`; the arc is
 * one way in latitude between its ends, so halving finds it. */
static double gc_lon(double lon1, double lat1, double lon2, double lat2, double lat)
{
    double lo = lon1, hi = lon2;
    bool   up = lat2 > lat1;

    for (int i = 0; i < 40; i++) {
        double mid = (lo + hi) / 2.0;

        if ((gc_lat(lon1, lat1, lon2, lat2, mid) < lat) == up) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (lo + hi) / 2.0;
}

/* Each line as its longitude at a latitude. */
static double line_a(double lat)
{
    if (lat >= 40.0) {
        return 40.0;
    }
    if (lat <= 23.5) {
        return 60.0;
    }
    return gc_lon(40.0, 40.0, 60.0, 23.5, lat);
}

static double line_b(double lat)
{
    double lon;

    if (lat >= 72.0) {
        lon = -10.0;
    } else if (lat >= 40.0) {
        lon = gc_lon(-10.0, 72.0, -50.0, 40.0, lat);
    } else if (lat >= -10.0) {
        lon = gc_lon(-50.0, 40.0, -20.0, -10.0, lat);
    } else {
        lon = -20.0;
    }
    return east(lon);
}

/* The Bering Strait boundary meets 65 30' N at 168 58' 49" W. A great
 * circle from the pole is a meridian. */
#define BERING_E (360.0 - (168.0 + 58.0 / 60.0 + 49.0 / 3600.0))

static double line_c(double lat)
{
    if (lat >= 65.5) {
        return BERING_E;
    }
    if (lat >= 50.0) {
        return gc_lon(165.0, 50.0, BERING_E, 65.5, lat);
    }
    if (lat > 10.0) {
        return gc_lon(165.0, 50.0, 190.0, 10.0, lat);
    }
    return 240.0;                       /* along 10 N to 120 W, then south */
}

/* ---- the territories ---------------------------------------------------- */

typedef struct {
    float lon, lat;                     /* lon east, 0..360 */
} pt_t;

/* East of Line A, Region 1: Turkey's east, the Caucasus, Central Asia,
 * Mongolia and Russia, and the sea north of Russia. The outline follows
 * their southern borders from the Mediterranean to the Pacific, then the
 * Kuril Islands, Kamchatka and the Commander Islands out past Line C,
 * and closes over the pole. */
static const pt_t region1_east[] = {
    {36.0f, 36.0f},                     /* west of Line A: closes the outline */
    {39.0f, 36.7f}, {42.4f, 37.1f},     /* Turkey - Syria */
    {44.8f, 37.15f},                    /* Turkey - Iraq */
    {44.6f, 37.7f}, {44.2f, 38.3f}, {44.3f, 38.9f}, {44.0f, 39.4f},
    {44.8f, 39.7f},                     /* Turkey - Iran */
    {45.5f, 38.95f}, {46.0f, 38.87f}, {46.54f, 38.87f}, {47.0f, 39.2f},
    {47.8f, 39.65f}, {48.35f, 39.38f}, {48.0f, 38.9f}, {48.88f, 38.43f},
                                        /* Nakhchivan, Armenia, Azerbaijan - Iran */
    {53.92f, 37.33f},                   /* across the Caspian */
    {55.4f, 38.0f}, {57.0f, 38.2f}, {58.4f, 37.65f}, {59.4f, 37.5f},
    {60.4f, 36.65f}, {61.15f, 36.65f}, {61.27f, 35.6f}, /* Turkmenistan - Iran */
    {62.3f, 35.15f}, {63.1f, 35.85f}, {64.5f, 36.25f}, {65.6f, 37.3f},
    {66.54f, 37.37f}, {67.25f, 37.19f}, {67.45f, 37.245f}, {67.78f, 37.19f},
                                        /* Turkmenistan, Uzbekistan - Afghanistan */
    {68.3f, 37.1f}, {69.3f, 37.1f}, {69.5f, 37.6f}, {70.2f, 37.95f},
    {70.95f, 38.45f}, {71.4f, 37.9f}, {71.5f, 37.0f}, {72.6f, 37.0f},
    {73.6f, 37.4f}, {74.9f, 37.24f},   /* Tajikistan - Afghanistan */
    {75.1f, 37.6f}, {74.9f, 38.5f}, {73.8f, 38.6f}, {73.6f, 39.45f},
                                        /* Tajikistan - China */
    {73.9f, 39.7f}, {74.9f, 40.5f}, {75.7f, 40.3f}, {76.6f, 40.4f},
    {77.7f, 41.0f}, {78.4f, 41.4f}, {79.8f, 42.0f}, {80.26f, 42.2f},
                                        /* Kyrgyzstan - China */
    {80.2f, 42.8f}, {80.8f, 43.2f}, {80.35f, 44.1f}, {80.5f, 45.0f},
    {81.7f, 45.3f}, {82.6f, 45.4f}, {82.3f, 46.0f}, {83.0f, 47.2f},
    {84.5f, 47.0f}, {85.5f, 47.1f}, {85.7f, 47.4f}, {85.6f, 48.4f},
    {86.6f, 48.6f}, {87.75f, 49.17f},  /* Kazakhstan, Russia - China */
    {88.0f, 48.6f}, {88.6f, 48.2f}, {90.1f, 47.9f}, {90.9f, 46.9f},
    {91.0f, 46.0f}, {90.7f, 45.4f}, {91.6f, 45.1f}, {93.5f, 45.0f},
    {95.4f, 44.3f}, {96.4f, 42.7f}, {97.2f, 42.8f}, {100.8f, 42.7f},
    {101.8f, 42.5f}, {103.7f, 41.8f}, {105.0f, 41.6f}, {106.8f, 42.3f},
    {109.3f, 42.4f}, {110.6f, 42.8f}, {111.9f, 43.7f}, {111.6f, 44.4f},
    {112.6f, 44.9f}, {113.6f, 44.7f}, {114.5f, 45.4f}, {115.7f, 45.5f},
    {116.6f, 46.3f}, {117.4f, 46.6f}, {118.3f, 46.7f}, {119.7f, 46.6f},
    {119.9f, 47.0f}, {119.1f, 47.7f}, {118.5f, 47.9f}, {117.4f, 47.6f},
    {116.2f, 47.8f}, {115.6f, 47.9f}, {116.71f, 49.85f}, /* Mongolia - China */
    {117.0f, 49.7f}, {117.37f, 49.615f}, {118.5f, 49.95f}, {119.2f, 50.35f}, {119.4f, 50.9f},
    {120.0f, 51.6f}, {120.8f, 52.6f}, {122.0f, 53.3f}, {123.5f, 53.5f},
    {125.0f, 53.2f}, {126.0f, 52.8f}, {126.5f, 52.1f}, {127.0f, 51.0f},
    {127.5f, 50.255f}, {128.5f, 49.6f}, {129.5f, 49.4f}, {130.6f, 48.9f},
    {131.0f, 47.7f}, {132.5f, 47.7f}, {134.0f, 48.3f}, {134.7f, 48.27f},
    {134.2f, 47.4f}, {133.9f, 46.3f}, {133.1f, 45.0f}, {132.0f, 45.25f},
    {131.1f, 44.8f}, {131.3f, 44.0f}, {131.0f, 43.0f}, {130.4f, 42.7f},
    {130.64f, 42.42f}, {130.7f, 42.3f}, /* Russia - China, North Korea */
    {136.0f, 42.8f}, {139.5f, 45.3f}, {141.0f, 45.8f}, {142.0f, 45.7f},
                                        /* Sea of Japan, La Perouse Strait */
    {144.5f, 44.9f}, {145.3f, 44.5f}, {145.45f, 44.0f}, {145.6f, 43.6f},
    {146.0f, 43.55f}, {146.5f, 43.3f}, {147.5f, 43.3f},
                                        /* Hokkaido - Kunashir, Habomai */
    {149.0f, 44.0f}, {151.0f, 45.2f}, {153.5f, 47.2f}, {155.5f, 49.0f},
    {157.0f, 50.5f}, {158.8f, 52.5f}, {160.5f, 54.0f}, {163.0f, 55.0f},
    {165.0f, 54.3f}, {168.5f, 54.3f},  /* Kurils, Kamchatka, Commander Is. */
    {200.0f, 60.0f}, {200.0f, 90.0f},  /* past Line C: Region 2 there anyway */
    {36.0f, 90.0f},
};

/* Iran, all of it Region 3 (west of Line A too). */
static const pt_t iran[] = {
    {44.8f, 37.15f}, {44.6f, 37.7f}, {44.2f, 38.3f}, {44.3f, 38.9f},
    {44.0f, 39.4f}, {44.8f, 39.7f}, {45.5f, 38.95f}, {46.0f, 38.87f},
    {46.54f, 38.87f}, {47.0f, 39.2f}, {47.8f, 39.65f}, {48.35f, 39.38f},
    {48.0f, 38.9f}, {48.88f, 38.43f}, {53.92f, 37.33f}, {55.4f, 38.0f},
    {57.0f, 38.2f}, {58.4f, 37.65f}, {59.4f, 37.5f}, {60.4f, 36.65f},
    {61.15f, 36.65f}, {61.27f, 35.6f},  /* the north, as above */
    {60.9f, 34.3f}, {60.5f, 33.7f}, {60.9f, 33.5f}, {60.6f, 32.0f},
    {60.85f, 31.5f}, {61.7f, 31.4f}, {61.85f, 30.85f}, /* Afghanistan */
    {62.8f, 29.4f}, {61.5f, 29.0f}, {61.0f, 28.1f}, {62.8f, 27.2f},
    {63.3f, 26.7f}, {61.6f, 25.2f},     /* Pakistan */
    {57.5f, 25.0f}, {56.6f, 26.0f}, {56.5f, 26.6f}, {54.5f, 26.4f},
    {52.5f, 27.0f}, {50.5f, 28.6f}, {49.0f, 29.6f}, {48.5f, 29.9f},
                                        /* the Gulf of Oman and the Gulf */
    {48.0f, 30.5f}, {47.7f, 31.0f}, {47.7f, 32.3f}, {46.1f, 33.0f},
    {45.4f, 33.9f}, {45.6f, 34.6f}, {45.9f, 35.8f}, {45.4f, 36.0f},
                                        /* Iraq */
};

/* Even-odd rule, in lon (0..360) / lat. */
static bool inside(const pt_t *p, size_t n, double lon, double lat)
{
    bool in = false;

    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if ((p[i].lat > lat) != (p[j].lat > lat) &&
            lon < (p[j].lon - p[i].lon) * (lat - p[i].lat) / (p[j].lat - p[i].lat) + p[i].lon) {
            in = !in;
        }
    }
    return in;
}

/* ---- the regions (RR 5.3 to 5.8) ---------------------------------------- */

int itu_region_from_pos(double lat, double lon)
{
    double e = east(lon);
    double a = line_a(lat), c = line_c(lat), b = line_b(lat);

    if (inside(iran, N(iran), e, lat)) {
        return 3;
    }
    if (e >= c && e < b) {
        return 2;
    }
    if (e >= a && e < c) {
        return inside(region1_east, N(region1_east), e, lat) ? 1 : 3;
    }
    return 1;
}

int itu_region_from_locator(const char *loc)
{
    char   grid[9];
    size_t n = 0;
    double lat, lon;

    for (const char *p = loc ? loc : ""; *p; p++) {
        if (*p == ' ') {
            continue;
        }
        if (n + 1 >= sizeof(grid)) {
            return 0;
        }
        grid[n++] = *p;
    }
    grid[n] = '\0';
    if (!qth_grid_check(grid)) {
        return 0;
    }
    qth_str_to_pos(grid, &lat, &lon);
    return itu_region_from_pos(lat, lon);
}
