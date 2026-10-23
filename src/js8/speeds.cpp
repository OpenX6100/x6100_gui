/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 speeds (submodes)
 */

#include "speeds.hpp"

#include "js8_rx.h"

namespace x6100::js8 {

namespace {

// Desktop JS8Submode.cpp: name, symbol samples, start delay, period, Costas,
// rxThreshold (10 unless given).
const Speed SPEEDS[JS8_SPEED_COUNT] = {
    {JS8_SPEED_NORMAL, "Normal", 'N', 0, 1920, 15, 500, 10, true, true, JS8_SUBMODE_NORMAL},
    {JS8_SPEED_FAST, "Fast", 'F', 1, 1200, 10, 200, 16, false, true, JS8_SUBMODE_FAST},
    {JS8_SPEED_TURBO, "Turbo", 'T', 2, 600, 6, 100, 32, false, false, JS8_SUBMODE_TURBO},
    {JS8_SPEED_SLOW, "Slow", 'S', 4, 3840, 30, 500, 10, false, true, JS8_SUBMODE_SLOW},
    {JS8_SPEED_ULTRA, "Ultra", 'U', 8, 384, 4, 100, 50, false, false, JS8_SUBMODE_ULTRA},  // desktop calls it "JS8 60"
};

} // namespace

const Speed &speed(js8_speed_t id) {
    return (id >= 0 && id < JS8_SPEED_COUNT) ? SPEEDS[id] : SPEEDS[JS8_SPEED_NORMAL];
}

const Speed &speed_from_varicode(int varicode) {
    for (auto &s : SPEEDS)
        if (s.varicode == varicode) return s;
    return SPEEDS[JS8_SPEED_NORMAL];
}

} // namespace x6100::js8

using namespace x6100::js8;

extern "C" const char *js8_speed_name(js8_speed_t s) { return speed(s).name; }
extern "C" char js8_speed_letter(js8_speed_t s) { return speed(s).letter; }
extern "C" int js8_speed_bandwidth_hz(js8_speed_t s) { return speed(s).bandwidth_hz(); }
extern "C" int js8_speed_period_s(js8_speed_t s) { return speed(s).period_s; }
extern "C" int js8_speed_rx_threshold_hz(js8_speed_t s) { return speed(s).rx_threshold_hz; }
extern "C" int js8_speed_max_offset_hz(js8_speed_t s) { return speed(s).max_offset_hz(); }
extern "C" bool js8_speed_heartbeats(js8_speed_t s) { return speed(s).heartbeats; }
extern "C" int js8_speed_rx_mask(js8_speed_t s) { return speed(s).rx_mask; }
extern "C" int js8_speed_submode(js8_speed_t s) { return speed(s).varicode; }
extern "C" js8_speed_t js8_speed_from_submode(int submode) { return speed_from_varicode(submode).id; }
