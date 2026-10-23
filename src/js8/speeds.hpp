/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 speeds (submodes)
 *
 *  One table for everything that depends on the speed: frame building,
 *  audio, slot timing, offsets and receive matching. Numbers from desktop
 *  JS8Call's JS8Submode.cpp (79 symbols, 12 kHz); see docs/T6_PLAN.md.
 */

#pragma once

#include "js8_speed.h"

#include <cstdint>

namespace x6100::js8 {

struct Speed {
    js8_speed_t id;
    const char *name;
    char        letter;
    int         varicode;         ///< desktop's submode number: 0, 1, 2, 4, 8
    int         symbol_samples;   ///< at 12 kHz
    int         period_s;         ///< slot length
    int         start_delay_ms;   ///< frames start this far into a slot
    int         rx_threshold_hz;  ///< offsets this close are the same station
    bool        original_costas;  ///< Normal only; the others use the modified array
    bool        heartbeats;       ///< desktop sends none in Turbo or Ultra
    int         rx_mask;          ///< JS8_SUBMODE_* bit for the receiver

    double symbol_seconds() const { return symbol_samples / 12000.0; }
    double tone_spacing_hz() const { return 12000.0 / symbol_samples; }
    int    bandwidth_hz() const { return 8 * 12000 / symbol_samples; }
    int    period_ms() const { return period_s * 1000; }
    /// Highest audio offset whose signal stays below 3000 Hz, the top of
    /// the filters the app sets while open.
    int max_offset_hz() const { return 3000 - bandwidth_hz(); }
    /// Seconds of audio in one frame (79 symbols).
    double frame_seconds() const { return 79 * symbol_seconds(); }
};

const Speed &speed(js8_speed_t id);
/// A decode's submode number (0/1/2/4/8); unknown ones are Normal.
const Speed &speed_from_varicode(int varicode);

} // namespace x6100::js8
