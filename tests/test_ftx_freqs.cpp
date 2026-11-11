/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the FT8 window's list of every FT8 and FT4
 *  frequency (src/ftx_freqs.c)
 */

extern "C" {
#include "ftx_freqs.h"
#include "ft8/ft2.h"
}

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<ftx_freq_t> list(int protocol, int region) {
    ftx_freq_t out[32];
    int        n = ftx_freqs(protocol, region, out, 32);
    return std::vector<ftx_freq_t>(out, out + n);
}

bool has(const std::vector<ftx_freq_t> &l, uint32_t hz, bool standard) {
    for (const auto &f : l) {
        if (f.hz == hz && (f.what == nullptr) == standard) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("ftx_freqs: FT8 has the standard and the DXpedition frequencies", "[ftx_freqs]") {
    auto l = list(FTX_PROTOCOL_FT8, 1);

    REQUIRE(l.size() == 20);
    CHECK(has(l, 14074000, true));
    CHECK(has(l, 14090000, false));
    CHECK(has(l, 7056000, false));
    CHECK(has(l, 50323000, false));
    for (size_t i = 1; i < l.size(); i++) {
        CHECK(l[i - 1].hz < l[i].hz);       /* low to high */
    }
}

TEST_CASE("ftx_freqs: FT4's 80 m one for Region 3 only there", "[ftx_freqs]") {
    CHECK_FALSE(has(list(FTX_PROTOCOL_FT4, 1), 3568000, false));
    CHECK_FALSE(has(list(FTX_PROTOCOL_FT4, 2), 3568000, false));
    CHECK(has(list(FTX_PROTOCOL_FT4, 3), 3568000, false));
    CHECK(has(list(FTX_PROTOCOL_FT4, 1), 3575000, true));
}

TEST_CASE("ftx_freqs: none for FT2, and no more than asked", "[ftx_freqs]") {
    ftx_freq_t out[4];

    CHECK(ftx_freqs(FTX_PROTOCOL_FT2, 1, out, 4) == 0);
    CHECK(ftx_freqs(FTX_PROTOCOL_FT8, 1, out, 4) == 4);
}

TEST_CASE("ftx_freqs: the closest one", "[ftx_freqs]") {
    auto l = list(FTX_PROTOCOL_FT8, 1);

    CHECK(l[ftx_freqs_closest(l.data(), (int)l.size(), 14074000)].hz == 14074000);
    CHECK(l[ftx_freqs_closest(l.data(), (int)l.size(), 14089000)].hz == 14090000);
    CHECK(l[ftx_freqs_closest(l.data(), (int)l.size(), 7080000)].hz == 7074000);
    CHECK(ftx_freqs_closest(nullptr, 0, 7074000) == 0);
}

/* The band buttons' frequencies (sql/digital_modes.csv) are all in the
 * list as standard ones, so it can always go back to them. */
TEST_CASE("ftx_freqs: every FT8 and FT4 row of the band table is a standard one", "[ftx_freqs]") {
    std::ifstream f(X6100_SOURCE_DIR "/sql/digital_modes.csv");
    std::string   line;
    int           rows = 0;

    REQUIRE(f.good());
    while (std::getline(f, line)) {
        std::stringstream s(line);
        std::string       id, label, freq, mode, type;

        std::getline(s, id, ',');
        std::getline(s, label, ',');
        std::getline(s, freq, ',');
        std::getline(s, mode, ',');
        std::getline(s, type, ',');
        if (type != "0" && type != "1") {
            continue;
        }
        int protocol = type == "0" ? FTX_PROTOCOL_FT8 : FTX_PROTOCOL_FT4;
        INFO(label);
        CHECK(has(list(protocol, 1), (uint32_t)std::stoul(freq), true));
        rows++;
    }
    CHECK(rows == 20);
}
