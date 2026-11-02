/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 one-press messages and heartbeat offsets
 */

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace x6100::js8 {

enum class Query {
    SnrQ,     ///< "CALL SNR?"          how do you hear me?
    SendSnr,  ///< "CALL SNR -12"       how I hear you
    GridQ,    ///< "CALL GRID?"
    MyGrid,   ///< "CALL GRID FN42AB"
    InfoQ,    ///< "CALL INFO?"
    StatusQ,  ///< "CALL STATUS?"
    HearingQ, ///< "CALL HEARING?"      who can you hear?
    AgnQ,     ///< "CALL AGN?"          please repeat
    RR,       ///< "CALL RR"
    SeventyThree, ///< "CALL 73"
};

/// Desktop's Varicode::formatSNR(): "+05", "-12"; empty outside -60..+60.
std::string desktop_snr(int snr);

/// Text for a one-press message to `to_call`. `their_snr` is used by
/// SendSnr, `my_grid` by MyGrid. Returns "" if a needed value is missing.
std::string query_text(Query q, const std::string &to_call, int their_snr, const std::string &my_grid);

/// Desktop JS8Call's heartbeat text: "CALL: HEARTBEAT FN42" (4-character grid).
std::string heartbeat_text(const std::string &my_call, const std::string &my_grid);

/// Recent activity at one audio offset.
struct OffsetActivity {
    float        offset_hz;
    std::int64_t heard_ms; ///< wall clock, ms
};

/// Port of desktop JS8Call's findFreeFreqOffset() / isFreqOffsetFree(): an
/// offset in [fmin, fmax) at least `bw` Hz from anything heard in the last
/// 30 s. Tries random `bw`-wide slots, then random offsets, then gives up
/// and returns fmin. Heartbeats use 500-1000 Hz with bw 50.
int find_free_offset(const std::vector<OffsetActivity> &activity, std::int64_t now_ms, std::mt19937 &rng,
                     int fmin = 500, int fmax = 1000, int bw = 50);

} // namespace x6100::js8
