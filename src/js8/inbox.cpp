/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 inbox
 */

#include "inbox.hpp"
#include "strutil.hpp"

#include "classify.hpp"
#include "datafile.hpp"

#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace x6100::js8 {

namespace {

// File format, one message per line (tabs never appear in JS8 text):
//   id <TAB> utc_ms <TAB> U|R <TAB> from <TAB> to <TAB> path <TAB> text
// (v1 files, without to and path, still load).
const char *const HEADER = "# X6100 JS8 inbox v2: id, UTC ms, U(nread)/R(ead), from, to, path, message\n";

std::string one_line(const std::string &s) {
    std::string out = s;
    for (char &c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

// A line's fields; the message itself is the last one (it never has a tab).
std::vector<std::string> fields(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> out;
    std::size_t              start = 0;
    for (auto tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', start)) {
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    out.push_back(line.substr(start));
    return out;
}

std::string join(const std::vector<std::string> &v, char sep) {
    std::string out;
    for (auto &x : v) out += (out.empty() ? "" : std::string(1, sep)) + x;
    return out;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::stringstream        in(s);
    for (std::string x; std::getline(in, x, sep);)
        if (!x.empty()) out.push_back(x);
    return out;
}

} // namespace

bool Inbox::load(const std::string &path) {
    msgs_.clear();
    next_id_  = 1;
    auto file = read_data_file(path); // missing: empty inbox
    writable_ = file.writable;
    notice_   = file.notice;
    std::istringstream f(file.text);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        // v1: id, utc, U/R, from, text; v2 adds to and path before the text.
        auto fld = fields(line);
        if (fld.size() != 5 && fld.size() != 7) continue;
        InboxMessage m;
        try {
            m.id     = std::stoi(fld[0]);
            m.utc_ms = std::stoll(fld[1]);
        } catch (...) {
            continue;
        }
        m.read = fld[2] == "R";
        m.from = fld[3];
        if (fld.size() == 7) {
            m.to   = fld[4];
            m.path = fld[5];
        }
        if (m.path.empty()) m.path = m.from;
        m.text   = fld.back();
        next_id_ = std::max(next_id_, m.id + 1);
        msgs_.push_back(m);
    }
    return writable_;
}

bool Inbox::save(const std::string &path) const {
    if (!writable_) return false; // an unreadable file we couldn't move aside: never write over it
    std::string out = HEADER;
    for (auto &m : msgs_) {
        out += std::to_string(m.id) + '\t' + std::to_string(m.utc_ms) + '\t' + (m.read ? "R" : "U") + '\t' +
               one_line(m.from) + '\t' + one_line(m.to) + '\t' + one_line(m.path) + '\t' + one_line(m.text) + '\n';
    }
    return write_data_file(path, out);
}

int Inbox::add(const std::string &from, const std::string &text, std::int64_t utc_ms, const std::string &to,
               const std::string &path, bool *added) {
    if (added) *added = false;
    for (auto &m : msgs_)
        if (m.from == from && m.text == text && utc_ms - m.utc_ms < REPEAT_MS) return m.id;
    if (added) *added = true;

    InboxMessage m;
    m.id     = next_id_++;
    m.utc_ms = utc_ms;
    m.from   = one_line(from);
    m.text   = one_line(text);
    m.to     = one_line(to);
    m.path   = one_line(path.empty() ? from : path);
    msgs_.push_back(m);

    // Full: drop the oldest read message, else the oldest.
    while (msgs_.size() > MAX_MESSAGES) {
        auto it = std::find_if(msgs_.begin(), msgs_.end(), [](const InboxMessage &x) { return x.read; });
        msgs_.erase(it != msgs_.end() ? it : msgs_.begin());
    }
    return m.id;
}

std::vector<InboxMessage> Inbox::list() const {
    return std::vector<InboxMessage>(msgs_.rbegin(), msgs_.rend());
}

std::optional<InboxMessage> Inbox::get(int id) const {
    for (auto &m : msgs_)
        if (m.id == id) return m;
    return std::nullopt;
}

bool Inbox::mark_read(int id) {
    for (auto &m : msgs_)
        if (m.id == id) {
            bool changed = !m.read;
            m.read       = true;
            return changed;
        }
    return false;
}

bool Inbox::remove(int id) {
    auto it = std::find_if(msgs_.begin(), msgs_.end(), [id](const InboxMessage &m) { return m.id == id; });
    if (it == msgs_.end()) return false;
    msgs_.erase(it);
    return true;
}

int Inbox::unread() const {
    return (int)std::count_if(msgs_.begin(), msgs_.end(), [](const InboxMessage &m) { return !m.read; });
}

namespace {
const char *const HELD_HEADER =
    "# X6100 JS8 held messages v2: id, UTC ms, H(eld)/D(elivered), from, for, path, RETRIEVE MSG sent (ms), "
    "fetched by (group), message\n";
} // namespace

bool HeldMessages::load(const std::string &path) {
    msgs_.clear();
    next_id_  = 1;
    auto file = read_data_file(path);
    writable_ = file.writable;
    notice_   = file.notice;
    std::istringstream f(file.text);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        // v1: id, utc, H/D, from, for, text; v2 adds path, notified, got.
        auto fld = fields(line);
        if (fld.size() != 6 && fld.size() != 9) continue;
        HeldMessage m;
        try {
            m.id     = std::stoi(fld[0]);
            m.utc_ms = std::stoll(fld[1]);
            if (fld.size() == 9) m.notified_ms = fld[6].empty() ? 0 : std::stoll(fld[6]);
        } catch (...) {
            continue;
        }
        m.delivered = fld[2] == "D";
        m.from      = fld[3];
        m.to        = fld[4];
        if (fld.size() == 9) {
            m.path = fld[5];
            m.got  = split(fld[7], ',');
        }
        if (m.path.empty()) m.path = m.from;
        m.text   = fld.back();
        next_id_ = std::max(next_id_, m.id + 1);
        msgs_.push_back(m);
    }
    return writable_;
}

bool HeldMessages::save(const std::string &path) const {
    if (!writable_) return false; // as Inbox::save()
    std::string out = HELD_HEADER;
    for (auto &m : msgs_) {
        out += std::to_string(m.id) + '\t' + std::to_string(m.utc_ms) + '\t' + (m.delivered ? "D" : "H") + '\t' +
               one_line(m.from) + '\t' + one_line(m.to) + '\t' + one_line(m.path) + '\t' +
               std::to_string(m.notified_ms) + '\t' + join(m.got, ',') + '\t' + one_line(m.text) + '\n';
    }
    return write_data_file(path, out);
}

int HeldMessages::add(const std::string &from, const std::string &to, const std::string &text, std::int64_t utc_ms,
                      const std::string &path, bool *added) {
    const std::string dest = base_callsign(to);
    if (added) *added = false;
    for (auto &m : msgs_)
        if (m.from == from && m.to == dest && m.text == text && utc_ms - m.utc_ms < Inbox::REPEAT_MS) return m.id;
    if (added) *added = true;
    HeldMessage m;
    m.id     = next_id_++;
    m.utc_ms = utc_ms;
    m.from   = one_line(from);
    m.to     = one_line(dest);
    m.path   = one_line(path.empty() ? from : path);
    m.text   = one_line(text);
    msgs_.push_back(m);
    // Full: drop a delivered one (or a group message past its two days),
    // else the oldest.
    while (msgs_.size() > MAX_MESSAGES) {
        auto it = std::find_if(msgs_.begin(), msgs_.end(), [&](const HeldMessage &x) {
            return x.delivered || (x.is_group() && utc_ms - x.utc_ms > GROUP_WINDOW_MS);
        });
        msgs_.erase(it != msgs_.end() ? it : msgs_.begin());
    }
    return m.id;
}

bool HeldMessages::is_for(int id, const std::string &call) const {
    auto m = get(id);
    return m && (m->to == call || m->to == base_callsign(call));
}

std::optional<int> HeldMessages::next_for(const std::string &call) const {
    const std::string base = base_callsign(call);
    for (auto &m : msgs_)
        if (!m.delivered && !m.text.empty() && (m.to == call || m.to == base)) return m.id;
    return std::nullopt;
}

std::optional<int> HeldMessages::lookahead_for(const std::string &call, int id) const {
    // Desktop looks for the call as heard, then its base call.
    for (const std::string &c : {call, base_callsign(call)})
        for (auto &m : msgs_)
            if (m.id != id && !m.delivered && !m.text.empty() && m.to == c) return m.id;
    return std::nullopt;
}

int HeldMessages::count_for(const std::string &call) const {
    // Desktop counts the call exactly as heard, while messages are stored
    // under base calls, so "W1ABC/P" would count none; count both.
    const std::string base = base_callsign(call);
    return (int)std::count_if(msgs_.begin(), msgs_.end(), [&](const HeldMessage &m) {
        return !m.delivered && !m.text.empty() && (m.to == call || m.to == base);
    });
}

namespace {
bool group_waiting(const HeldMessage &m, const std::string &group, const std::string &call, std::int64_t now_ms) {
    return m.to == group && !m.text.empty() && now_ms - m.utc_ms < HeldMessages::GROUP_WINDOW_MS &&
           std::find(m.got.begin(), m.got.end(), call) == m.got.end();
}
} // namespace

std::optional<int> HeldMessages::next_group_for(const std::string &group, const std::string &call,
                                                std::int64_t now_ms) const {
    for (auto &m : msgs_)
        if (group_waiting(m, group, call, now_ms)) return m.id;
    return std::nullopt;
}

std::optional<int> HeldMessages::lookahead_group_for(const std::string &group, const std::string &call, int id,
                                                     std::int64_t now_ms) const {
    for (const std::string &c : {call, base_callsign(call)})
        for (auto &m : msgs_)
            if (m.id != id && group_waiting(m, group, c, now_ms)) return m.id;
    return std::nullopt;
}

int HeldMessages::count_group_for(const std::string &group, const std::string &call, std::int64_t now_ms) const {
    return (int)std::count_if(msgs_.begin(), msgs_.end(),
                              [&](const HeldMessage &m) { return group_waiting(m, group, call, now_ms); });
}

bool HeldMessages::mark_group_delivered(int id, const std::string &call) {
    for (auto &m : msgs_)
        if (m.id == id && m.is_group()) {
            if (std::find(m.got.begin(), m.got.end(), call) != m.got.end()) return false;
            m.got.push_back(one_line(call));
            return true;
        }
    return false;
}

// Only finds it: marked told (notified()) once the notice has been queued,
// or a notice that couldn't go out waited 8 hours (bug hunt S4).
std::optional<std::pair<int, std::string>> HeldMessages::push_due(const std::vector<Heard> &heard,
                                                                  std::int64_t              now_ms) const {
    for (auto &m : msgs_) {
        if (m.delivered || m.text.empty() || m.to.empty() || m.is_group()) continue;
        bool seen = false;
        for (auto &h : heard)
            if ((h.call == m.to || base_callsign(h.call) == m.to) && now_ms - h.heard_ms <= PUSH_SEEN_MS) seen = true;
        if (!seen) continue;
        if (m.notified_ms && now_ms - m.notified_ms < PUSH_REPEAT_MS) continue;
        return std::make_pair(m.id, m.to + " RETRIEVE MSG " + std::to_string(m.id));
    }
    return std::nullopt;
}

bool HeldMessages::notified(int id, std::int64_t now_ms) {
    for (auto &m : msgs_)
        if (m.id == id) {
            m.notified_ms = now_ms;
            return true;
        }
    return false;
}

std::optional<HeldMessage> HeldMessages::get(int id) const {
    for (auto &m : msgs_)
        if (m.id == id) return m;
    return std::nullopt;
}

bool HeldMessages::mark_delivered(int id) {
    for (auto &m : msgs_)
        if (m.id == id && !m.delivered) {
            m.delivered = true;
            return true;
        }
    return false;
}

bool HeldMessages::remove(int id) {
    auto it = std::find_if(msgs_.begin(), msgs_.end(), [id](const HeldMessage &m) { return m.id == id; });
    if (it == msgs_.end()) return false;
    msgs_.erase(it);
    return true;
}

std::vector<HeldMessage> HeldMessages::list() const {
    return std::vector<HeldMessage>(msgs_.rbegin(), msgs_.rend());
}

int HeldMessages::waiting() const {
    return (int)std::count_if(msgs_.begin(), msgs_.end(), [](const HeldMessage &m) { return !m.delivered; });
}

std::optional<std::pair<std::string, std::string>> msg_to_body(const std::string &text, const std::string &my_call) {
    if (my_call.empty()) return std::nullopt;
    auto colon = text.find(':');
    if (colon == std::string::npos) return std::nullopt;
    auto w = words(text.substr(colon + 1));
    if (w.size() < 4 || base_callsign(w[0]) != base_callsign(my_call) || w[1] != "MSG" || w[2].rfind("TO:", 0) != 0)
        return std::nullopt;
    std::size_t i    = 3;
    std::string dest = w[2].substr(3);
    if (dest.empty()) dest = w[i++]; // "MSG TO: W1ABC ..."
    if (dest.empty() || dest[0] == '@' || i >= w.size()) return std::nullopt;
    std::string body;
    for (; i < w.size(); i++) body += (body.empty() ? "" : " ") + w[i];
    return std::make_pair(dest, body);
}

std::optional<int> msg_id_arg(const std::string &arg) {
    std::string s = arg;
    for (auto &c : s)
        if (c == '[' || c == ']') c = ' ';
    auto w = words(s);
    if (w.empty()) return std::nullopt;
    std::size_t i = 0;
    if (w[0] == "ID") i = 1;
    else if (w[0].rfind("ID", 0) == 0) w[0] = w[0].substr(2);
    if (i >= w.size()) return std::nullopt;
    const auto &n = w[i];
    if (n.empty() || n.size() > 6 || !std::all_of(n.begin(), n.end(), ::isdigit)) return std::nullopt;
    return std::stoi(n);
}

std::optional<int> query_msg_id(const std::string &text) {
    auto w = words(text);
    for (std::size_t i = 0; i + 2 < w.size(); i++) {
        if (w[i] != "QUERY" || w[i + 1] != "MSG") continue;
        std::string rest;
        for (std::size_t k = i + 2; k < w.size(); k++) rest += w[k] + " ";
        return msg_id_arg(rest);
    }
    return std::nullopt;
}

std::optional<std::string> msg_body(const std::string &text, const std::string &my_call) {
    if (my_call.empty()) return std::nullopt;
    auto colon = text.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string rest = text.substr(colon + 1);
    auto        w    = words(rest);
    if (w.size() < 3 || base_callsign(w[0]) != base_callsign(my_call) || w[1] != "MSG") return std::nullopt;
    if (w[2].rfind("TO:", 0) == 0) return std::nullopt;

    // Everything after "MSG ", spacing kept.
    auto pos = rest.find(" MSG ");
    if (pos == std::string::npos) return std::nullopt;
    std::string body = rest.substr(pos + 5);
    while (!body.empty() && body.front() == ' ') body.erase(body.begin());
    while (!body.empty() && body.back() == ' ') body.pop_back();
    if (body.empty()) return std::nullopt;
    return body;
}

std::optional<int> msg_id_offered(const std::string &text) {
    auto w = words(text);
    for (std::size_t i = 0; i + 2 < w.size(); i++) {
        bool id       = w[i] == "MSG" && w[i + 1] == "ID";
        bool retrieve = w[i] == "RETRIEVE" && w[i + 1] == "MSG";
        if (!id && !retrieve) continue;
        const auto &n = w[i + 2];
        if (n.empty() || n.size() > 6 || !std::all_of(n.begin(), n.end(), ::isdigit)) continue;
        return std::stoi(n);
    }
    return std::nullopt;
}

std::optional<Signature> delivered_signature(const std::string &text) {
    // Desktop's MessagePanel: (?:^| )FROM (callsign)(?: NEXT MSG ID \d+(?: \+\d+)?)?$
    static const std::regex re("(?:^| )FROM (\\S+)(?: NEXT MSG ID (\\d+)(?: \\+\\d+)?)?$");
    std::smatch             m;
    std::string             t = text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (!std::regex_search(t, m, re)) return std::nullopt;
    Signature s;
    s.from = m.str(1);
    // "I'M AWAY FROM HOME" isn't signed by "HOME": desktop also requires a
    // valid callsign.
    if (!js8core::protocol::varicode::is_valid_callsign(s.from, nullptr)) return std::nullopt;
    // Up to 6 digits, as msg_id_arg(): a longer run (std::stoi would throw)
    // reads as no next id, as desktop's toInt() gives 0.
    if (m[2].matched && m[2].length() <= 6) s.next_id = std::stoi(m.str(2));
    return s;
}

} // namespace x6100::js8
