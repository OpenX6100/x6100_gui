/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 QSO tracking and the ADIF log
 */

#include "qsolog.hpp"
#include "strutil.hpp"

#include "classify.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace x6100::js8 {

namespace {

/// Words after "FROM: TO", e.g. {"SNR", "-12"} for "K2XYZ: N0XYZ SNR -12".
std::vector<std::string> command_words(const std::string &text) {
    auto colon = text.find(':');
    auto w     = words(colon == std::string::npos ? text : text.substr(colon + 1));
    if (!w.empty()) w.erase(w.begin());
    return w;
}

// A sign and up to 3 digits: anyone can send "SNR +9999999999" as free
// text, and std::stoi throws on it.
bool parse_snr(const std::string &w, int *out) {
    if (w.size() < 2 || w.size() > 4 || (w[0] != '+' && w[0] != '-')) return false;
    for (std::size_t i = 1; i < w.size(); i++)
        if (!std::isdigit((unsigned char)w[i])) return false;
    *out = std::stoi(w);
    return true;
}

std::optional<int> find_snr(const std::vector<std::string> &w) {
    for (std::size_t i = 0; i + 1 < w.size(); i++) {
        int snr;
        if (w[i] == "SNR" && parse_snr(w[i + 1], &snr)) return snr;
    }
    return std::nullopt;
}

bool is_hb_ack(const std::vector<std::string> &w) {
    return w.size() >= 2 && w[0] == "HEARTBEAT" && w[1] == "SNR";
}

/// "73", "SK", "TU 73!", "RR73": the QSO is ending.
// 73, SK or RR73 among the last three words ("TNX QSO 73", "73 GL",
// "FB OM 73 ES GL"), not anywhere: "73 DEGREES HERE" mid-QSO isn't its end.
bool ends_qso(const std::vector<std::string> &w) {
    for (std::size_t i = w.size() > 3 ? w.size() - 3 : 0; i < w.size(); i++) {
        auto s = w[i];
        while (!s.empty() && std::ispunct((unsigned char)s.back())) s.pop_back();
        if (s == "73" || s == "SK" || s == "RR73") return true;
    }
    return false;
}

bool is_call(const std::string &s) {
    if (s.size() < 3 || s[0] == '@') return false;
    bool letter = false, digit = false;
    for (char c : s) {
        if (std::isupper((unsigned char)c)) letter = true;
        else if (std::isdigit((unsigned char)c)) digit = true;
        else if (c != '/') return false;
    }
    return letter && digit;
}

} // namespace

Qso &QsoTracker::entry(const std::string &call, std::int64_t now_ms) {
    auto key = base_callsign(call);
    auto it  = qsos_.find(key);
    if (it != qsos_.end() && now_ms - it->second.last_ms > EXPIRE_MS) {
        qsos_.erase(it);
        it = qsos_.end();
    }
    if (it == qsos_.end()) {
        Qso q;
        q.start_ms = now_ms;
        it         = qsos_.emplace(key, q).first;
    }
    it->second.call    = call;
    it->second.last_ms = now_ms;
    return it->second;
}

std::optional<std::string> QsoTracker::maybe_offer(Qso &q, bool ends) {
    if (!ends || !q.we_sent || !q.they_sent || q.offered) return std::nullopt;
    q.offered = true;
    return q.call;
}

std::optional<std::string> QsoTracker::received(const std::string &from, const std::string &text, bool to_me, int snr,
                                                const std::string &my_call, std::int64_t now_ms) {
    if (from.empty() || my_call.empty() || base_callsign(from) == base_callsign(my_call)) return std::nullopt;

    if (!to_me) {
        // Still how we hear them, if we're in a QSO with them.
        auto it = qsos_.find(base_callsign(from));
        if (it != qsos_.end()) it->second.heard_snr = snr;
        return std::nullopt;
    }

    auto w      = command_words(text);
    Qso &q      = entry(from, now_ms);
    q.heard_snr = snr;
    if (auto r = find_snr(w)) q.rcvd_snr = r;
    auto colon  = text.find(':');
    q.grid      = better_grid(q.grid, announced_grid(colon == std::string::npos ? text : text.substr(colon + 1)));
    if (is_hb_ack(w)) return std::nullopt; // an ack isn't a QSO
    q.they_sent = true;
    return maybe_offer(q, ends_qso(w));
}

std::optional<std::string> QsoTracker::sent(const std::string &text, const std::string &my_call, std::int64_t now_ms) {
    auto mc = classify(text, my_call);
    if (!is_call(mc.to) || base_callsign(mc.to) == base_callsign(my_call)) return std::nullopt;

    auto w = command_words(text);
    Qso &q = entry(mc.to, now_ms);
    if (auto r = find_snr(w)) q.sent_snr = r;
    if (is_hb_ack(w)) return std::nullopt;
    q.we_sent = true;
    return maybe_offer(q, ends_qso(w));
}

std::optional<Qso> QsoTracker::get(const std::string &call, std::int64_t now_ms) const {
    auto it = qsos_.find(base_callsign(call));
    if (it == qsos_.end() || now_ms - it->second.last_ms > EXPIRE_MS) return std::nullopt;
    return it->second;
}

void QsoTracker::logged(const std::string &call) {
    qsos_.erase(base_callsign(call));
}

std::string format_snr(int snr) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%+03d", snr);
    return buf;
}

std::string adif_band(std::uint64_t freq_hz) {
    static const struct {
        std::uint64_t lo, hi;
        const char   *band;
    } bands[] = {
        {1800000, 2000000, "160m"},   {3500000, 4000000, "80m"},    {5060000, 5450000, "60m"},
        {7000000, 7300000, "40m"},    {10100000, 10150000, "30m"},  {14000000, 14350000, "20m"},
        {18068000, 18168000, "17m"},  {21000000, 21450000, "15m"},  {24890000, 24990000, "12m"},
        {28000000, 29700000, "10m"},  {50000000, 54000000, "6m"},   {144000000, 148000000, "2m"},
    };
    for (auto &b : bands)
        if (freq_hz >= b.lo && freq_hz <= b.hi) return b.band;
    return "";
}

namespace {

void field(std::string &out, const char *name, const std::string &value) {
    if (value.empty()) return;
    std::string v;
    for (char c : value) v += (c == '\r' || c == '\n') ? ' ' : c;
    if (!out.empty()) out += ' ';
    out += '<';
    out += name;
    out += ':' + std::to_string(v.size()) + '>' + v;
}

std::string utc(std::int64_t ms, const char *fmt) {
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm     tm;
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

} // namespace

std::string adif_record(const LogEntry &e) {
    std::string r;
    field(r, "call", e.call);
    field(r, "gridsquare", e.grid);
    field(r, "mode", "MFSK");
    field(r, "submode", "JS8");
    field(r, "rst_sent", e.rst_sent);
    field(r, "rst_rcvd", e.rst_rcvd);
    field(r, "qso_date", utc(e.on_ms, "%Y%m%d"));
    field(r, "time_on", utc(e.on_ms, "%H%M%S"));
    field(r, "qso_date_off", utc(e.off_ms, "%Y%m%d"));
    field(r, "time_off", utc(e.off_ms, "%H%M%S"));
    field(r, "band", adif_band(e.freq_hz));
    char freq[24];
    std::snprintf(freq, sizeof(freq), "%.6f", e.freq_hz / 1e6);
    field(r, "freq", freq);
    field(r, "station_callsign", e.my_call);
    field(r, "my_gridsquare", e.my_grid);
    field(r, "comment", e.comment);
    field(r, "name", e.name);
    field(r, "operator", e.op_call);
    if (e.tx_pwr_w > 0) {
        char pwr[16];
        std::snprintf(pwr, sizeof(pwr), "%g", e.tx_pwr_w);
        field(r, "tx_pwr", pwr);
    }
    if (!e.pota_ref.empty()) {
        field(r, "my_sig", "POTA");
        field(r, "my_sig_info", e.pota_ref);
    }
    field(r, "my_sota_ref", e.sota_ref);
    return r + " <eor>\n";
}

bool adif_append(const std::string &path, const LogEntry &e, std::string &err) {
    FILE *f = std::fopen(path.c_str(), "a");
    if (!f) {
        err = "can't open " + path;
        return false;
    }
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    if (ok && std::ftell(f) == 0) {
        ok = std::fputs("X6100 JS8 log, desktop JS8Call format\n<adif_ver:5>3.1.4 <programid:9>X6100_JS8 <eoh>\n",
                        f) >= 0;
    }
    ok = ok && std::fputs(adif_record(e).c_str(), f) >= 0;
    ok = ok && std::fflush(f) == 0;
    fsync(fileno(f)); // the SD card: a power cut shouldn't lose it
    ok = std::fclose(f) == 0 && ok;
    if (!ok) err = "can't write " + path;
    return ok;
}

} // namespace x6100::js8
