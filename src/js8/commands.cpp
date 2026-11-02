/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 one-press messages and heartbeat offsets
 */

#include "commands.hpp"
#include "strutil.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace x6100::js8 {

namespace {

bool offset_free(const std::vector<OffsetActivity> &activity, std::int64_t now_ms, int f, int bw) {
    for (auto &a : activity) {
        if (now_ms - a.heard_ms >= 30'000) continue;
        if (std::fabs(a.offset_hz - f) < bw) return false;
    }
    return true;
}

} // namespace

std::string desktop_snr(int snr) {
    if (snr < -60 || snr > 60) return "";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%02d", snr >= 0 ? '+' : '-', std::abs(snr));
    return buf;
}

std::string query_text(Query q, const std::string &to_call, int their_snr, const std::string &my_grid) {
    const std::string to = upper(to_call);
    if (to.empty()) return "";
    switch (q) {
    case Query::SnrQ: return to + " SNR?";
    case Query::SendSnr: {
        auto snr = desktop_snr(their_snr);
        return snr.empty() ? "" : to + " SNR " + snr;
    }
    case Query::GridQ: return to + " GRID?";
    case Query::MyGrid: return my_grid.empty() ? "" : to + " GRID " + upper(my_grid);
    case Query::InfoQ: return to + " INFO?";
    case Query::StatusQ: return to + " STATUS?";
    case Query::HearingQ: return to + " HEARING?";
    case Query::AgnQ: return to + " AGN?";
    case Query::RR: return to + " RR";
    case Query::SeventyThree: return to + " 73";
    }
    return "";
}

std::string heartbeat_text(const std::string &my_call, const std::string &my_grid) {
    std::string text = upper(my_call) + ": HEARTBEAT";
    if (my_grid.size() >= 4) text += " " + upper(my_grid.substr(0, 4));
    return text;
}

int find_free_offset(const std::vector<OffsetActivity> &activity, std::int64_t now_ms, std::mt19937 &rng, int fmin,
                     int fmax, int bw) {
    const int nslots = (fmax - fmin) / bw;
    if (nslots <= 0) return fmin;

    for (int i = 0; i < nslots; i++) {
        int f = fmin + bw * (int)(rng() % (unsigned)nslots);
        if (offset_free(activity, now_ms, f, bw)) return f;
    }
    for (int i = 0; i < nslots; i++) {
        int f = fmin + (int)(rng() % (unsigned)(fmax - fmin));
        if (offset_free(activity, now_ms, f, bw)) return f;
    }
    return fmin;
}

} // namespace x6100::js8
