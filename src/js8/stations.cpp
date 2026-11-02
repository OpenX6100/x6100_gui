/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 station list (desktop JS8Call's Call Activity)
 */

#include "stations.hpp"
#include "strutil.hpp"

#include "classify.hpp"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <sstream>

namespace x6100::js8 {

namespace {

// A sign and up to 3 digits: anyone can send "SNR +9999999999" as free
// text, and std::stoi throws on it.
bool parse_snr(const std::string &w, int *out) {
    if (w.size() < 2 || w.size() > 4 || (w[0] != '+' && w[0] != '-')) return false;
    for (std::size_t i = 1; i < w.size(); i++)
        if (!std::isdigit((unsigned char)w[i])) return false;
    *out = std::stoi(w);
    return true;
}

} // namespace

void StationList::add(const StationEvent &ev, const std::string &my_call) {
    if (ev.from.empty()) return;
    if (!my_call.empty() && base_callsign(ev.from) == base_callsign(my_call)) return; // our own echo

    heard_.insert(ev.from);
    Station &st = stations_[ev.from];
    st.call     = ev.from;
    st.via.clear(); // heard directly
    st.heard_ms = ev.when_ms;
    st.snr      = ev.snr;
    st.freq_hz  = ev.freq_hz;
    st.mode     = ev.mode;

    // Text after "FROM:", e.g. "@HB HEARTBEAT FN42", "K2XYZ SNR -12",
    // "K2XYZ HEARTBEAT SNR -08", "@ALLCALL CQ CQ CQ FN03", "K2XYZ GRID EM48AB".
    std::string body = ev.text;
    if (auto colon = body.find(':'); colon != std::string::npos) body = body.substr(colon + 1);
    auto w = words(body);

    // "@APRSIS MSG TO:CALL text DE SENDER" is what a JS8Call-improved
    // station relaying inbound APRS sends (an Echo test's answer, an SMS):
    // a gateway both ways. Only these show up on JS8; one-way gateways
    // are silent here.
    if (w.size() >= 3 && w[0] == "@APRSIS" && w[1] == "MSG" && w[2].rfind("TO:", 0) == 0) st.aprs_gate = true;

    // Grid: only from a heartbeat, a CQ or a GRID command, as desktop.
    st.grid = better_grid(st.grid, announced_grid(body));

    // Anything addressed to us means they hear us (desktop sets the ★ the
    // same way). "... SNR -12" or "... HEARTBEAT SNR -12" to us is how they
    // hear us.
    if (ev.to_me) {
        st.heard_me    = true;
        st.heard_me_ms = ev.when_ms;
        for (std::size_t i = 0; i + 1 < w.size(); i++) {
            int snr;
            if (w[i] == "SNR" && parse_snr(w[i + 1], &snr)) st.reported_snr = snr;
        }
    }
}

void StationList::add_via(const std::string &call, const std::string &via, float freq_hz, int mode,
                          std::int64_t when_ms, std::int64_t expire_ms) {
    if (call.empty() || via.empty() || call == via) return;
    auto it = stations_.find(call);
    if (it != stations_.end() && it->second.via.empty() && (expire_ms <= 0 || when_ms - it->second.heard_ms < expire_ms))
        return; // heard directly: keep that
    Station &st = stations_[call];
    st.call     = call;
    st.via      = via;
    st.heard_ms = when_ms;
    st.snr      = -64; // desktop's value for a station only heard through a relay
    st.freq_hz  = freq_hz;
    st.mode     = mode;
}

std::vector<Station> StationList::sorted(std::int64_t now_ms, std::int64_t expire_ms) const {
    std::vector<Station> out;
    for (auto &[call, st] : stations_)
        if (expire_ms <= 0 || now_ms - st.heard_ms < expire_ms) out.push_back(st);

    std::stable_sort(out.begin(), out.end(), [](const Station &a, const Station &b) {
        if (a.heard_me != b.heard_me) return a.heard_me;
        if (a.heard_me) return a.heard_me_ms > b.heard_me_ms;
        return a.heard_ms > b.heard_ms;
    });
    return out;
}

std::vector<Station> StationList::recent(std::int64_t now_ms, std::size_t max, std::int64_t expire_ms) const {
    std::vector<const Station *> live;
    for (auto &[call, st] : stations_)
        if (expire_ms <= 0 || now_ms - st.heard_ms < expire_ms) live.push_back(&st);
    std::size_t n     = std::min(max, live.size());
    auto        newer = [](const Station *a, const Station *b) { return a->heard_ms > b->heard_ms; };
    std::partial_sort(live.begin(), live.begin() + (std::ptrdiff_t)n, live.end(), newer);
    std::vector<Station> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; i++) out.push_back(*live[i]);
    return out;
}

const Station *StationList::find(const std::string &call, std::int64_t now_ms, std::int64_t expire_ms) const {
    auto it = stations_.find(call);
    if (it == stations_.end() || (expire_ms > 0 && now_ms - it->second.heard_ms >= expire_ms)) return nullptr;
    return &it->second;
}

void StationList::expire(std::int64_t now_ms, std::int64_t expire_ms) {
    if (expire_ms <= 0) return;
    for (auto it = stations_.begin(); it != stations_.end();)
        it = now_ms - it->second.heard_ms >= expire_ms ? stations_.erase(it) : std::next(it);
}

} // namespace x6100::js8
