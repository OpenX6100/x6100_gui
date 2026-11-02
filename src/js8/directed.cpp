/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 directed commands and relay paths
 */

#include "directed.hpp"
#include "strutil.hpp"

#include "js8core/protocol/varicode.hpp"

#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>

namespace x6100::js8 {

namespace {

std::string ltrim(const std::string &s) {
    auto b = s.find_first_not_of(' ');
    return b == std::string::npos ? "" : s.substr(b);
}

// Desktop's directed pattern (Varicode optional_cmd_pattern), in its order:
// first the commands ending in '?' or ':', then the word commands, which
// must be followed by a space or the end.
const char *const FIXED_CMDS[] = {
    "AGN?", "QSL?", "HW CPY?", "MSG TO:", "SNR?", "INFO?", "GRID?", "STATUS?", "QUERY MSGS?", "HEARING?",
};
const char *const WORD_CMDS[] = {
    "STATUS", "HEARING", "QUERY CALL", "QUERY MSGS", "QUERY", "CMD", "MSG", "NACK", "ACK", "73", "YES",
    "NO",     "HEARTBEAT SNR", "SNR", "QSL", "RR", "SK", "FB", "INFO", "GRID", "DIT DIT",
};

// A callsign as desktop's relay patterns spell it: optional prefix, a base
// with one digit, optional suffix.
const char *const CALL_RE = "(?:[A-Z0-9]{1,4}/)?[0-9A-Z]?[0-9A-Z][0-9][A-Z]?[A-Z]?[A-Z]?(?:/[A-Z0-9]{1,4})?";

bool is_grid(const std::string &s) {
    static const std::regex grid("^[A-R]{2}[0-9]{2}([A-X]{2})?$");
    return std::regex_match(s, grid);
}

} // namespace

std::optional<Directed> parse_directed(const std::string &text) {
    auto colon = text.find(": ");
    if (colon == std::string::npos || colon == 0) return std::nullopt;
    Directed d;
    d.from = upper(text.substr(0, colon));

    std::string rest = upper(text.substr(colon + 2));
    rest             = ltrim(rest);
    std::size_t n    = 0;
    while (n < rest.size() && (std::isalnum((unsigned char)rest[n]) || rest[n] == '/' || rest[n] == '@')) n++;
    if (n == 0) return std::nullopt;
    d.to = rest.substr(0, n);
    rest = rest.substr(n);

    // Our renderer spaces a relay out ("TO > W1ABC ..."); desktop has "TO>".
    if (!rest.empty() && (rest[0] == '>' || rest.rfind(" >", 0) == 0)) {
        d.cmd  = ">";
        d.text = ltrim(rest.substr(rest.find('>') + 1));
        return d;
    }
    if (!rest.empty() && rest[0] == '?') {
        d.cmd  = " SNR?"; // "K2XYZ?": the old form of SNR?
        d.text = ltrim(rest.substr(1));
        return d;
    }
    std::string s = ltrim(rest);
    for (auto c : FIXED_CMDS) {
        if (s.rfind(c, 0) == 0) {
            d.cmd  = std::string(" ") + c;
            d.text = ltrim(s.substr(std::strlen(c)));
            if (d.cmd == " QUERY MSGS?") d.cmd = " QUERY MSGS";
            return d;
        }
    }
    for (auto c : WORD_CMDS) {
        std::size_t len = std::strlen(c);
        if (s.rfind(c, 0) == 0 && (s.size() == len || s[len] == ' ')) {
            d.cmd  = std::string(" ") + c;
            d.text = ltrim(s.substr(len));
            return d;
        }
    }
    d.cmd  = " ";
    d.text = s;
    return d;
}

std::optional<std::string> relay_next_hop(const std::string &text) {
    // callToPattern: ^\b(callsign)([> ])\b
    static const std::regex re(std::string("^(") + CALL_RE + ")([> ])\\b");
    std::smatch m;
    if (!std::regex_search(text, m, re)) return std::nullopt;
    return m.str(1) + ">" + text.substr(m.position(2) + 1);
}

std::vector<std::string> relay_path_calls(const std::string &from, const std::string &text) {
    // callDePattern: \s(*DE*|VIA)\s(callsign)\b, each match put first.
    static const std::regex re(std::string("\\s(\\*DE\\*|VIA)\\s(") + CALL_RE + ")\\b");
    std::vector<std::string> calls;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it)
        calls.insert(calls.begin(), (*it)[2].str());
    calls.insert(calls.begin(), from);
    return calls;
}

std::optional<std::pair<std::string, std::string>> relayed_command(const std::string &text) {
    namespace vc = js8core::protocol::varicode;
    // Desktop splits on single spaces and takes the first word as the
    // command, with its leading space unless it's one of the bare ones.
    std::vector<std::string> words;
    {
        std::size_t start = 0;
        while (true) {
            auto sp = text.find(' ', start);
            words.push_back(text.substr(start, sp == std::string::npos ? std::string::npos : sp - start));
            if (sp == std::string::npos) break;
            start = sp + 1;
        }
    }
    if (words.empty()) return std::nullopt;
    std::string first = words[0];
    bool        valid = vc::is_command_allowed(first);
    if (!valid) {
        first = " " + first;
        valid = vc::is_command_allowed(first);
        if (valid) words.erase(words.begin());
    }
    // "MSG TO:" and "QUERY MSGS" / "QUERY CALL" have a space in them.
    if (!words.empty()) {
        if (first == " MSG") {
            auto &second = words[0];
            if (second == "TO:") {
                first = " MSG TO:";
                words.erase(words.begin());
            } else if (second.rfind("TO:", 0) == 0) {
                first  = " MSG TO:";
                second = second.substr(3);
            }
        } else if (first == " QUERY") {
            auto &second = words[0];
            if (second == "MSGS" || second == "MSGS?") {
                first = " QUERY MSGS";
                words.erase(words.begin());
            } else if (second == "CALL") {
                first = " QUERY CALL";
                words.erase(words.begin());
            }
        }
    }
    if (!vc::is_command_allowed(first) || !vc::is_command_autoreply(first)) return std::nullopt;
    std::string rest;
    for (std::size_t i = 0; i < words.size(); i++) rest += (i ? " " : "") + words[i];
    if (first == "?") first = " SNR?";
    return std::make_pair(first, rest);
}

std::string query_call_question(const std::string &text) {
    auto d = parse_directed("X: " + text);
    if (!d || d->cmd != " QUERY CALL") return text;
    auto        e    = d->text.find_last_not_of(' ');
    std::string word = e == std::string::npos ? "" : d->text.substr(0, e + 1);
    if (word.empty() || word.find(' ') != std::string::npos || word.back() == '?') return text;
    return text.substr(0, text.find_last_not_of(' ') + 1) + "?";
}

std::vector<std::string> parse_callsigns(const std::string &text) {
    namespace vc = js8core::protocol::varicode;
    std::vector<std::string> out;
    std::string              tok;
    auto                     flush = [&] {
        if (!tok.empty() && vc::is_valid_callsign(tok, nullptr) && !is_grid(tok)) out.push_back(tok);
        tok.clear();
    };
    for (char c : upper(text)) {
        if (std::isalnum((unsigned char)c) || c == '/' || c == '@') tok += c;
        else flush();
    }
    flush();
    return out;
}

bool is_allcall(const std::string &to) {
    return to.find("@ALLCALL") != std::string::npos || to.find("@HB") != std::string::npos;
}

bool is_checksummed_command(const std::string &cmd) {
    return js8core::protocol::varicode::is_command_checksummed(cmd) == 16;
}

std::string path_display(const std::string &path) {
    std::vector<std::string> segs;
    std::stringstream        in(path);
    for (std::string s; std::getline(in, s, '>');) segs.push_back(s);
    std::string out;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) out += (out.empty() ? "" : " via ") + *it;
    return out;
}

} // namespace x6100::js8
