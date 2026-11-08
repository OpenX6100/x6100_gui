/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - tests for the ITU region of a position or a
 *  locator (src/itu_region.c)
 */

extern "C" {
#include "itu_region.h"
#include "qth/qth.h"
}

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

namespace {
struct Place {
    const char *name;
    double      lat, lon;
    int         region;
};

/* Each line and each territory, on both sides of it. */
const Place places[] = {
    /* Line B: Iceland, the Azores, Cape Verde are Region 1 */
    {"Reykjavik", 64.15, -21.94, 1},      {"Ponta Delgada, Azores", 37.74, -25.67, 1},
    {"Praia, Cape Verde", 14.93, -23.51, 1}, {"Jan Mayen", 70.98, -8.50, 1},
    {"Ascension", -7.93, -14.42, 1},      {"Tristan da Cunha", -37.07, -12.31, 1},
    {"Nuuk, Greenland", 64.18, -51.72, 2}, {"Tasiilaq, Greenland", 65.61, -37.64, 2},
    {"NE Greenland", 81.60, -16.70, 2},   {"Recife", -8.05, -34.90, 2},
    {"Fernando de Noronha", -3.85, -32.42, 2}, {"South Georgia", -54.28, -36.50, 2},
    /* Line C: all of Alaska is Region 2, the Pacific south of 10 N
     * between 170 W and 120 W Region 3 */
    {"Adak, Aleutians", 51.88, -176.66, 2}, {"Attu, Aleutians", 52.93, 172.93, 2},
    {"Little Diomede", 65.75, -168.92, 2}, {"Big Diomede", 65.78, -169.05, 1},
    {"Uelen, Chukotka", 66.16, -169.81, 1}, {"Wrangel Island", 71.20, -179.40, 1},
    {"Hawaii", 21.31, -157.86, 2},        {"Clipperton", 10.30, -109.22, 2},
    {"Easter Island", -27.11, -109.35, 2}, {"Tahiti", -17.53, -149.57, 3},
    {"Cook Islands", -21.21, -159.78, 3}, {"Niue", -19.05, -169.87, 3},
    {"Kiritimati", 1.87, -157.36, 3},     {"Pitcairn", -25.07, -130.10, 3},
    {"Samoa", -13.83, -171.76, 3},
    /* Line A, and the territories either side of it */
    {"Baghdad", 33.31, 44.36, 1},         {"Sulaymaniyah", 35.56, 45.43, 1},
    {"Muscat", 23.59, 58.41, 1},          {"Van, Turkey", 38.50, 43.38, 1},
    {"Yerevan", 40.18, 44.51, 1},         {"Baku", 40.41, 49.87, 1},
    {"Tehran", 35.69, 51.39, 3},          {"Urmia, Iran", 37.55, 45.07, 3},
    {"Bandar Abbas, Iran", 27.18, 56.27, 3}, {"Mauritius", -20.16, 57.50, 1},
    {"Rodrigues", -19.70, 63.42, 3},
    /* Region 1 / 3: Russia, Central Asia, Mongolia against their
     * neighbours */
    {"Ashgabat", 37.95, 58.38, 1},        {"Termez", 37.22, 67.28, 1},
    {"Hairatan, Afghanistan", 37.23, 67.42, 3}, {"Dushanbe", 38.56, 68.77, 1},
    {"Mazar-i-Sharif", 36.70, 67.10, 3},  {"Bishkek", 42.87, 74.59, 1},
    {"Kashgar", 39.47, 75.99, 3},         {"Almaty", 43.24, 76.89, 1},
    {"Urumqi", 43.83, 87.62, 3},          {"Ulaanbaatar", 47.92, 106.92, 1},
    {"Erenhot", 43.65, 111.98, 3},        {"Zabaykalsk", 49.64, 117.33, 1},
    {"Manzhouli", 49.59, 117.43, 3},      {"Blagoveshchensk", 50.27, 127.53, 1},
    {"Heihe", 50.24, 127.49, 3},          {"Vladivostok", 43.12, 131.89, 1},
    {"Yuzhno-Sakhalinsk", 46.96, 142.74, 1}, {"Wakkanai", 45.41, 141.67, 3},
    {"Yuzhno-Kurilsk", 44.03, 145.86, 1}, {"Nemuro", 43.33, 145.58, 3},
    {"Paramushir", 50.68, 156.12, 1},     {"Bering Island", 55.20, 165.98, 1},
    /* well inside */
    {"Berlin", 52.52, 13.40, 1},          {"Johannesburg", -26.20, 28.05, 1},
    {"Vancouver", 49.28, -123.12, 2},     {"Sao Paulo", -23.55, -46.63, 2},
    {"Tokyo", 35.68, 139.69, 3},          {"Sydney", -33.87, 151.21, 3},
};
} // namespace

TEST_CASE("ITU region of real places", "[itu_region]") {
    for (const auto &p : places) {
        char grid[9];

        INFO(p.name);
        CHECK(itu_region_from_pos(p.lat, p.lon) == p.region);

        /* and of their 6- and 8-character locators */
        qth_pos_to_str(p.lat, p.lon, grid);
        CHECK(itu_region_from_locator(grid) == p.region);
        grid[6] = '\0';
        CHECK(itu_region_from_locator(grid) == p.region);
    }
}

TEST_CASE("ITU region of 4-character locators", "[itu_region]") {
    CHECK(itu_region_from_locator("HP94") == 1);    /* Reykjavik */
    CHECK(itu_region_from_locator("HM77") == 1);    /* Ponta Delgada */
    CHECK(itu_region_from_locator("HK84") == 1);    /* Praia */
    CHECK(itu_region_from_locator("BH52") == 3);    /* Tahiti */
    CHECK(itu_region_from_locator("AO11") == 2);    /* Adak */
    CHECK(itu_region_from_locator("NN33") == 3);    /* Urumqi */
    CHECK(itu_region_from_locator("QN16") == 1);    /* Yuzhno-Sakhalinsk */
    CHECK(itu_region_from_locator("CN89") == 2);    /* Vancouver */
    CHECK(itu_region_from_locator("JO62") == 1);    /* Berlin */
    CHECK(itu_region_from_locator("FN31") == 2);
    CHECK(itu_region_from_locator("PM95") == 3);
}

TEST_CASE("ITU region of what is not a locator", "[itu_region]") {
    CHECK(itu_region_from_locator(nullptr) == 0);
    CHECK(itu_region_from_locator("") == 0);
    CHECK(itu_region_from_locator("J") == 0);
    CHECK(itu_region_from_locator("JO9") == 0);
    CHECK(itu_region_from_locator("ZZ99") == 0);
    CHECK(itu_region_from_locator("JO94ab12x") == 0);
    CHECK(itu_region_from_locator("jo94ab") == 1);  /* case and spaces do not matter */
    CHECK(itu_region_from_locator(" JO94 ") == 1);
}
