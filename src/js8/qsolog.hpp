/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 QSO tracking and the ADIF log
 *
 *  Desktop JS8Call logs by hand (Log QSO, F5) into js8call_log.adi; the
 *  dialog does the same, filled in from what went back and forth, and
 *  offers to log when a two-way QSO ends with 73 or SK.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace x6100::js8 {

/// What we know about a QSO with one station.
struct Qso {
    std::string        call;
    std::int64_t       start_ms = 0; ///< first directed message either way
    std::int64_t       last_ms  = 0;
    std::optional<int> sent_snr;     ///< report we gave them ("CALL SNR -12")
    std::optional<int> rcvd_snr;     ///< report they gave us
    std::optional<int> heard_snr;    ///< how we last heard them
    std::string        grid;         ///< a grid they sent us
    bool               we_sent   = false; ///< we sent them something (not an HB ack)
    bool               they_sent = false; ///< they sent us something (not an HB ack)
    bool               offered   = false; ///< the log prompt has been shown
};

/// Follows directed traffic between us and other stations.
class QsoTracker {
public:
    /// A QSO is forgotten after this long without a message.
    static constexpr std::int64_t EXPIRE_MS = 30 * 60 * 1000;

    /// A received message, "FROM: TO ...". Returns the call when this
    /// message ends a two-way QSO (73 or SK) not offered for logging yet.
    std::optional<std::string> received(const std::string &from, const std::string &text, bool to_me, int snr,
                                        const std::string &my_call, std::int64_t now_ms);

    /// One of ours as others see it, "MYCALL: TO ...", when it goes out.
    std::optional<std::string> sent(const std::string &text, const std::string &my_call, std::int64_t now_ms);

    /// The QSO with `call`, if any (base calls match: "EA8/G4ABC" is G4ABC).
    std::optional<Qso> get(const std::string &call, std::int64_t now_ms) const;

    /// Logged: the next message starts a new QSO.
    void logged(const std::string &call);

    void clear() { qsos_.clear(); }

private:
    Qso                       &entry(const std::string &call, std::int64_t now_ms);
    std::optional<std::string> maybe_offer(Qso &q, bool ends);

    std::map<std::string, Qso> qsos_; ///< by base call
};

/// One log entry, as desktop JS8Call writes it plus TX power and the
/// POTA / SOTA activation fields.
struct LogEntry {
    std::string   call, grid, name, comment;
    std::string   rst_sent, rst_rcvd; ///< "-12", or empty
    std::int64_t  on_ms = 0, off_ms = 0;
    std::uint64_t freq_hz = 0;        ///< dial + audio offset
    std::string   my_call, my_grid, op_call;
    float         tx_pwr_w = 0;       ///< 0: not written
    std::string   pota_ref;           ///< MY_SIG POTA / MY_SIG_INFO
    std::string   sota_ref;           ///< MY_SOTA_REF
};

/// "40m", or "" outside the amateur bands.
std::string adif_band(std::uint64_t freq_hz);

/// One record ending "<eor>", fields as desktop JS8Call writes them
/// (MODE MFSK, SUBMODE JS8).
std::string adif_record(const LogEntry &e);

/// Append to an ADIF file, writing the header first if it's new or empty.
bool adif_append(const std::string &path, const LogEntry &e, std::string &err);

/// "+05" / "-12", as JS8 writes reports.
std::string format_snr(int snr);

} // namespace x6100::js8
