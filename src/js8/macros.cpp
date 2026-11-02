/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 message macros (see macros.hpp)
 */

#include "macros.hpp"
#include "strutil.hpp"

#include "qsolog.hpp" // format_snr(): "+05", "-12", as desktop's Varicode::formatSNR()

#include <cctype>
#include <cstdio>

namespace x6100::js8 {

namespace {

/// QString::replace(): every occurrence, left to right, no rescan.
void replace_all(std::string &s, const std::string &from, const std::string &to) {
    if (from.empty()) return;
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

/// Desktop's prune: QRegularExpression("[<](?:[^>]+)[>]") removed.
std::string prune_rest(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            std::size_t close = s.find('>', i + 1);
            if (close != std::string::npos && close > i + 1) {
                i = close + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// Desktop's own CQ, heartbeat and reply texts (Configuration.cpp defaults);
// ours aren't settings, and our buttons send the same.
constexpr const char *MY_CQ    = "CQ CQ CQ <MYGRID4>";
constexpr const char *MY_HB    = "HB <MYGRID4>";
constexpr const char *MY_REPLY = "HW CPY?";

} // namespace

std::string idle_text(std::int64_t idle_ms) {
    std::int64_t secs = idle_ms > 0 ? idle_ms / 60000 * 60 : 0;
    char         buf[24];
    if (secs >= 24 * 3600) std::snprintf(buf, sizeof(buf), "%lldD", (long long)(secs / (24 * 3600)));
    else if (secs >= 3600) std::snprintf(buf, sizeof(buf), "%lldH", (long long)(secs / 3600));
    else if (secs >= 60) std::snprintf(buf, sizeof(buf), "%lldM", (long long)(secs / 60));
    else std::snprintf(buf, sizeof(buf), "0M"); // desktop: "now" -> "0M"
    return buf;
}

std::map<std::string, std::string> macro_values(const MacroInput &in) {
    std::map<std::string, std::string> v = {
        {"<MYCALL>", in.my_call},
        {"<MYGRID4>", in.my_grid.substr(0, 4)},
        {"<MYGRID12>", in.my_grid.substr(0, 12)},
        {"<MYINFO>", in.my_info},
        {"<MYHB>", MY_HB},
        {"<MYCQ>", MY_CQ},
        {"<MYREPLY>", MY_REPLY},
        {"<MYSTATUS>", in.my_status},
        {"<MYVERSION>", in.version},
        {"<MYIDLE>", idle_text(in.idle_ms)},
    };
    if (!in.call.empty()) {
        v["<CALL>"] = in.call;
        if (in.tdelta_ms) v["<TDELTA>"] = std::to_string(*in.tdelta_ms) + " ms";
        if (in.snr && *in.snr > -31) v["<SNR>"] = format_snr(*in.snr);
    }
    // These can hold macros themselves; desktop fills them in this order.
    for (const char *k : {"<MYINFO>", "<MYSTATUS>", "<MYCQ>", "<MYHB>", "<MYREPLY>"})
        v[k] = replace_macros(v[k], v, false);
    return v;
}

std::string replace_macros(const std::string &text, const std::map<std::string, std::string> &values, bool prune) {
    std::string out = text;
    for (const auto &[key, value] : values) replace_all(out, key, upper(value));
    return prune ? prune_rest(out) : out;
}

bool macros_need_station(const std::string &text) {
    return text.find("<CALL>") != std::string::npos || text.find("<SNR>") != std::string::npos ||
           text.find("<TDELTA>") != std::string::npos;
}

} // namespace x6100::js8
