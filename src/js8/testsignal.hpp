/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "js8_speed.h"

namespace x6100::js8 {

/// One simulated station for make_test_band().
struct TestStation {
    std::string call;
    std::string grid;
    std::string text;       ///< as typed in JS8Call, e.g. "K2XYZ SNR?" or "CQ CQ CQ FN42"
    double      offset_hz;  ///< audio offset of the lowest tone
    double      snr_db;     ///< in 2500 Hz, as JS8Call reports it
    js8_speed_t speed = JS8_SPEED_NORMAL;
};

/// JS8 audio for a band of stations, each at its own speed. Sample 0 is a
/// slot boundary for every speed (a 30 s boundary is one for all of them);
/// each station sends its frames in consecutive slots of its speed, starting
/// its speed's delay into each slot as JS8Call does. Adds Gaussian noise of
/// RMS `noise_rms` (full scale 1.0). The result is as long as the longest
/// station's transmission, rounded up to a whole 30 s.
std::vector<float> make_test_band(const std::vector<TestStation> &stations, int rate, float noise_rms = 0.02f,
                                  std::uint32_t seed = 1);

} // namespace x6100::js8
