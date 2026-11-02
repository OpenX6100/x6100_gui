/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#include "classify.hpp"
#include "strutil.hpp"

#include <regex>

#include "render.hpp"

#include "js8core/protocol/varicode.hpp"

#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace x6100::js8 {

namespace {

bool starts_with(const std::string &s, const char *prefix) {
    return s.rfind(prefix, 0) == 0;
}

} // namespace

std::string base_callsign(const std::string &call) {
    std::string best;
    std::istringstream in(upper(call));
    for (std::string part; std::getline(in, part, '/');) {
        bool has_digit = false;
        for (char c : part) has_digit |= std::isdigit((unsigned char)c) != 0;
        if (has_digit && part.size() > best.size()) best = part;
    }
    return best.empty() ? upper(call) : best;
}

MessageClass classify(const std::string &text, const std::string &my_call) {
    MessageClass mc;

    std::string rest = text;
    auto        colon = text.find(':');
    if (colon != std::string::npos && colon > 0) {
        auto from = text.substr(0, colon);
        if (is_callsign_like(from)) {
            mc.from = upper(from);
            rest    = text.substr(colon + 1);
        }
    }

    std::istringstream       in(upper(rest));
    std::vector<std::string> tokens;
    for (std::string t; in >> t;) tokens.push_back(t);
    if (tokens.empty()) return mc;

    const std::string &first = tokens[0];

    if (first == "@HB" || (tokens.size() > 1 && tokens[1] == "HEARTBEAT")) mc.heartbeat = true;
    // "CQ" itself, not a call starting with it (Portugal's CQ0-CQ9 special
    // calls: "CT1ABC: CQ7ABC HELLO" is to CQ7ABC). CQ frames render as
    // "@ALLCALL CQ ...".
    if (first == "CQ" || (first == "@ALLCALL" && tokens.size() > 1 && tokens[1] == "CQ")) mc.cq = true;

    if (first[0] == '@') {
        mc.to       = first;
        mc.to_group = true;
    } else if (is_callsign_like(first)) {
        mc.to = first;
    }

    if (!mc.to.empty() && !mc.to_group && tokens.size() > 2 && tokens[1] == "SNR") mc.snr_report = true;

    // As desktop: our call or our base call, exactly. VE7NHW/P is another
    // station (a portable, a club's /P) when we're VE7NHW.
    if (!my_call.empty() && !mc.to.empty() && !mc.to_group)
        mc.to_me = mc.to == upper(my_call) || mc.to == base_callsign(my_call);

    return mc;
}

Checksum verify_command_checksum(std::string &text) {
    namespace vc = js8core::protocol::varicode;

    // "FROM: TO<cmd>payload". The target is a callsign or @GROUP.
    auto colon = text.find(": ");
    if (colon == std::string::npos) return Checksum::None;
    std::size_t to_start = colon + 2;
    std::size_t to_end   = to_start;
    while (to_end < text.size() && text[to_end] != ' ' && text[to_end] != '>') ++to_end;
    if (to_end == to_start) return Checksum::None;

    // Buffered commands, longest first so " MSG TO:" wins over " MSG".
    static const char *const BUFFERED[] = {
        " QUERY MSGS?", " QUERY MSGS", " QUERY CALL", " MSG TO:", " QUERY", " GRID", " MSG", " CMD", ">",
    };
    std::string rest = text.substr(to_end);
    const char *cmd  = nullptr;
    for (auto c : BUFFERED) {
        if (rest.rfind(c, 0) == 0) {
            cmd = c;
            break;
        }
    }
    // The renderer spaces out a relay: "TO > VIA ...".
    if (!cmd && rest.rfind(" >", 0) == 0) {
        rest.erase(0, 1);
        cmd = ">";
    }
    if (!cmd || !vc::is_command_buffered(cmd)) return Checksum::None;

    const int bits = vc::is_command_checksummed(cmd);
    if (bits != 16 && bits != 32) return Checksum::None;

    // Payload with surrounding whitespace removed, then " CHECKSUM" at the end.
    std::string payload = rest.substr(std::strlen(cmd));
    auto        b       = payload.find_first_not_of(' ');
    auto        e       = payload.find_last_not_of(' ');
    // A command sent on its own (e.g. "QUERY MSGS") buffers no payload.
    if (b == std::string::npos) return Checksum::None;
    payload = payload.substr(b, e - b + 1);

    // Desktop sends @APRSIS MSG / MSG TO: (an APRS gateway passing an APRS
    // message on to a JS8 station) without a checksum: take it as it is.
    const bool aprs_unchecked =
        text.compare(to_start, to_end - to_start, "@APRSIS") == 0 && (std::strcmp(cmd, " MSG") == 0 ||
                                                                       std::strcmp(cmd, " MSG TO:") == 0);
    const Checksum bad = aprs_unchecked ? Checksum::None : Checksum::Invalid;

    const std::size_t len = bits == 32 ? 6 : 3;
    if (payload.size() < len + 2 || payload[payload.size() - len - 1] != ' ') return bad;
    std::string checksum = payload.substr(payload.size() - len);
    std::string message  = payload.substr(0, payload.size() - len - 1);

    bool ok = bits == 32 ? vc::checksum32_valid(checksum, message) : vc::checksum16_valid(checksum, message);
    if (!ok) return bad;

    // Drop " CHECKSUM" from the end of the displayed text.
    auto last = text.find_last_not_of(' ');
    text.erase(last + 1 - len);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return Checksum::Valid;
}

bool is_grid(const std::string &w) {
    static const std::regex re("^[A-R]{2}[0-9]{2}([A-X]{2}([0-9]{2}([A-X]{2})?)?)?$");
    return w != "RR73" && std::regex_match(w, re);
}

std::string find_grid(const std::string &body) {
    std::istringstream       in(body);
    std::vector<std::string> w;
    for (std::string t; in >> t;) w.push_back(t);
    for (std::size_t i = 0; i + 1 < w.size(); i++)
        if (w[i] == "GRID" && is_grid(w[i + 1])) return w[i + 1].substr(0, 6);
    for (auto it = w.rbegin(); it != w.rend(); ++it)
        if (is_grid(*it)) return it->substr(0, 6);
    return "";
}

std::string announced_grid(const std::string &body) {
    std::istringstream       in(upper(body));
    std::vector<std::string> w;
    for (std::string t; in >> t;) w.push_back(t);
    if (w.empty()) return "";
    // "GRID xxxx": a GRID command, or said in so many words ("RR73 GRID
    // EN34KS"; desktop only reads the command, but this is just as plain).
    for (std::size_t i = 0; i + 1 < w.size(); i++)
        if (w[i] == "GRID" && is_grid(w[i + 1])) return w[i + 1].substr(0, 6);
    bool hb = w[0] == "@HB" || (w.size() > 1 && w[1] == "HEARTBEAT");
    bool cq = w[0] == "CQ" || (w[0] == "@ALLCALL" && w.size() > 1 && w[1] == "CQ");
    if ((hb || cq) && w.size() > 1 && is_grid(w.back())) return w.back().substr(0, 6);
    return "";
}

std::string better_grid(const std::string &known, const std::string &heard) {
    if (heard.empty()) return known;
    if (heard.size() < known.size() && known.compare(0, heard.size(), heard) == 0) return known;
    return heard;
}

} // namespace x6100::js8
