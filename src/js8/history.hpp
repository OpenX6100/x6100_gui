/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 station history
 *
 *  Every station you've exchanged messages with, per band, kept for good:
 *  their latest INFO and STATUS, and the text of each QSO, time-stamped.
 *  One SQLite file (desktop JS8Call keeps its inbox the same way). The
 *  caller only decides what a message means (cheap string work); the
 *  database is written by a low-priority thread of its own, a few seconds'
 *  worth in one transaction, so neither the screen nor the decoder waits.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct sqlite3;

namespace x6100::js8 {

/// What one message means for the history.
struct HistoryNote {
    std::string call;           ///< the other station, base call ("VE7ABC" for "VE7ABC/P")
    std::string as_sent;        ///< as it was sent ("VE7ABC/P")
    bool        exchange  = false; ///< between us and them, either way
    bool        heartbeat = false; ///< a heartbeat ACK ("CALL HEARTBEAT SNR -12"): kept, not a QSO line shown
    /// Text with no recipient ("W1ABC: GOOD COPY", or ours): in a long QSO
    /// the calls get dropped. Joins an open QSO with them, never starts one.
    bool        loose     = false;
    std::optional<int> reported_snr; ///< how they hear us, if they said
    int         info_kind = -1;    ///< 0 INFO, 1 STATUS: their own answer, to anyone
    std::string info_text;
    std::string info_to;           ///< who they answered
    std::string grid;              ///< announced in this message (heartbeat, CQ, GRID)
};

/// A message from `from` ("FROM: TO ..."), `to_me` as the classifier
/// decided. nullopt: nothing for the history (from us, or no sender).
std::optional<HistoryNote> history_note_rx(const std::string &from, const std::string &text, bool to_me,
                                           const std::string &my_call);

/// One of ours, "MYCALL: TO ...": an exchange when TO is a station (not a
/// group, not @APRSIS); with no TO at all, loose (its station is the one
/// you have selected, filled in by the caller).
std::optional<HistoryNote> history_note_tx(const std::string &text, const std::string &my_call);

/// A station of the all-time list (one band).
struct HistoryContact {
    std::string  call, grid, band;
    std::int64_t first_ms = 0, last_ms = 0; ///< first and latest exchange
    std::int64_t heard_ms = 0;              ///< heard at all (anything decoded), latest
    std::int64_t heard_us_ms = 0;           ///< they sent us something, latest (0: never)
    int          snr = 0;                   ///< how we heard them, latest
    std::optional<int> reported_snr;        ///< how they heard us, latest
};

struct HistoryInfo {
    std::int64_t ms = 0;
    std::string  text, to, band;
};

struct HistoryQso {
    std::int64_t id = 0, start_ms = 0, end_ms = 0;
    std::string  band;
    int          lines = 0, real_lines = 0; ///< real: not heartbeat ACKs
    bool         logged = false;
};

struct HistoryLine {
    std::int64_t ms = 0;
    bool         tx = false;
    int          snr = 0;
    std::string  text;
    bool         heartbeat = false;
};

/// Counters for the health line, since the last read.
struct HistoryStats {
    unsigned     rows = 0, commits = 0, failed = 0;
    std::int64_t busy_us = 0; ///< inside transactions
};

class History {
public:
    /// A new QSO starts after this long without a message between you
    /// (the log prompt's rule).
    static constexpr std::int64_t QSO_GAP_MS = 30 * 60 * 1000;
    /// Writes wait this long to share a transaction.
    static constexpr int BATCH_MS = 3000;

    History() = default;
    ~History();
    History(const History &) = delete;
    History &operator=(const History &) = delete;

    /// Opens (creates) the file and starts the writer. False if it can't.
    bool open(const std::string &path, bool background = true);
    /// Writes what's waiting and stops the writer.
    void close();
    bool is_open() const { return db_ != nullptr; }

    /// Received / sent: `text` as the list shows it ("W1ABC: K2XYZ HW
    /// CPY?"), `band` "20m", `freq_hz` dial + offset.
    void received(const HistoryNote &n, const std::string &text, const std::string &band, double freq_hz, int snr,
                  int speed, std::int64_t now_ms);
    void sent(const HistoryNote &n, const std::string &text, const std::string &band, double freq_hz, int speed,
              bool automatic, std::int64_t now_ms);
    /// The QSO with `call` on `band` went into the log.
    void logged(const std::string &call, const std::string &band, std::int64_t now_ms);
    /// A message with no callsign at all (its first frame missed): it joins
    /// the open QSO whose station sends on that offset (`freq_hz` dial +
    /// offset, within `window_hz`).
    void received_callless(const std::string &text, const std::string &band, double freq_hz, double window_hz,
                           int snr, int speed, std::int64_t now_ms);
    /// Anything decoded from a known contact: when and how we last heard them.
    void heard(const std::string &call, const std::string &band, int snr, const std::string &grid,
               std::int64_t now_ms);

    /// Waits until everything sent so far is in the file.
    void flush();
    /// Everything forgotten, every band (Settings). Waits until it's done.
    void clear();
    /// Stations in the history, every band.
    int  station_count();
    HistoryStats take_stats();

    /// Reading (any thread; the writer's pending rows are flushed first).
    std::vector<HistoryContact> contacts(const std::string &band);
    std::optional<HistoryContact> contact(const std::string &call, const std::string &band);
    std::optional<HistoryInfo>  latest_info(const std::string &call, int kind);
    std::vector<HistoryQso>     qsos(const std::string &call, bool with_heartbeat_only = false);
    std::vector<HistoryLine>    lines(std::int64_t qso_id);
    /// A QSO with `call` in the history (any band; heartbeat-only
    /// exchanges don't count). Quick: no file access.
    bool                        had_qso(const std::string &call) const;
    /// Exchanged messages with `call` (a base call) on `band`.
    bool                        known(const std::string &call, const std::string &band) const;

private:
    struct Op {
        enum Kind { Rx, Tx, Logged, Heard, Loose, Clear } kind = Rx;
        HistoryNote  note;
        std::string  text, band;
        double       freq_hz   = 0;
        int          snr       = 0;
        int          speed     = 0;
        bool         automatic = false;
        bool         tx        = false; ///< Loose: ours
        double       window_hz = 0;     ///< Loose without a call: how near their offset
        std::int64_t ms        = 0;
    };
    void post(Op op);
    void run();
    void apply(const Op &op);
    void exchange(const Op &op);
    void loose(const Op &op);
    void write_all(std::deque<Op> &ops);

    sqlite3                *db_ = nullptr;
    std::thread             thread_;
    mutable std::mutex      mu_;
    std::condition_variable cv_, done_cv_;
    std::deque<Op>          queue_;
    std::uint64_t           posted_ = 0, written_ = 0;
    bool                    stop_ = false, background_ = true;
    int                     flushes_ = 0; ///< callers waiting in flush()
    std::unordered_set<std::string> known_;    ///< "CALL BAND" of every contact row
    std::unordered_set<std::string> qso_calls_; ///< calls with a QSO that isn't heartbeats only
    HistoryStats            stats_;
    std::mutex              db_mu_; ///< the connection: writer and readers take turns
    /// The QSO still open per call: id, band, last message, their offset.
    struct Open {
        std::int64_t id = 0, end_ms = 0;
        std::string  band;
        double       rx_freq = 0; ///< dial + offset they last sent on (0: not known)
    };
    std::unordered_map<std::string, Open> open_;
    Open &open_for(const std::string &call);
    void  add_line(Open &o, const std::string &call, const Op &op, bool rx, bool heartbeat, const std::string &call_as);
    bool  is_open(const Open &o, const Op &op) const {
        return o.id && o.band == op.band && op.ms - o.end_ms <= QSO_GAP_MS;
    }
    std::atomic<std::int64_t> last_exchange_ms_{0}; ///< no QSO this recent: call-less messages skipped at once
};

} // namespace x6100::js8
