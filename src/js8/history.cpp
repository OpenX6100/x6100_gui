/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 station history
 */

#include "history.hpp"
#include "strutil.hpp"

#include "classify.hpp"
#include "directed.hpp"
#include "js8_history.h"
#include "js8_speed.h"
#include "qsolog.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#if defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace x6100::js8 {

namespace {

// A sign and up to 3 digits: anyone can send "SNR +9999999999" as free
// text, and std::stoi throws on it.
bool parse_snr(const std::string &text, int *out) {
    std::string w = trim(text);
    if (auto sp = w.find(' '); sp != std::string::npos) w = w.substr(0, sp);
    if (w.size() < 2 || w.size() > 4 || (w[0] != '+' && w[0] != '-')) return false;
    for (std::size_t i = 1; i < w.size(); i++)
        if (!std::isdigit((unsigned char)w[i])) return false;
    *out = std::stoi(w);
    return true;
}

std::int64_t mono_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Version 1. Calls are base calls ("VE7ABC" for "VE7ABC/P"); lines keep
// the call as sent.
constexpr const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS contacts ("
    " call TEXT NOT NULL, band TEXT NOT NULL, grid TEXT NOT NULL DEFAULT '',"
    " first_ms INTEGER NOT NULL, last_ms INTEGER NOT NULL,"
    " heard_ms INTEGER NOT NULL DEFAULT 0, heard_us_ms INTEGER NOT NULL DEFAULT 0,"
    " snr INTEGER, reported_snr INTEGER,"
    " PRIMARY KEY (call, band));"
    "CREATE TABLE IF NOT EXISTS infos ("
    " id INTEGER PRIMARY KEY, call TEXT NOT NULL, kind INTEGER NOT NULL, ms INTEGER NOT NULL,"
    " text TEXT NOT NULL, to_call TEXT NOT NULL DEFAULT '', band TEXT NOT NULL DEFAULT '');"
    "CREATE INDEX IF NOT EXISTS infos_call ON infos (call, kind, ms);"
    "CREATE TABLE IF NOT EXISTS qsos ("
    " id INTEGER PRIMARY KEY, call TEXT NOT NULL, band TEXT NOT NULL,"
    " start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL,"
    " lines INTEGER NOT NULL DEFAULT 0, real_lines INTEGER NOT NULL DEFAULT 0,"
    " logged INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS qsos_call ON qsos (call, end_ms);"
    "CREATE TABLE IF NOT EXISTS lines ("
    " id INTEGER PRIMARY KEY, qso INTEGER NOT NULL, ms INTEGER NOT NULL, tx INTEGER NOT NULL,"
    " snr INTEGER, freq_hz REAL, speed INTEGER, auto INTEGER NOT NULL DEFAULT 0,"
    " heartbeat INTEGER NOT NULL DEFAULT 0, call_as TEXT NOT NULL DEFAULT '', text TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS lines_qso ON lines (qso, ms);"
    "PRAGMA user_version = 1;";

// A prepared statement for one call, finalized at the end of scope.
struct Stmt {
    sqlite3_stmt *s = nullptr;
    Stmt(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
    Stmt &text(int i, const std::string &v) {
        sqlite3_bind_text(s, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt &i64(int i, std::int64_t v) {
        sqlite3_bind_int64(s, i, v);
        return *this;
    }
    Stmt &real(int i, double v) {
        sqlite3_bind_double(s, i, v);
        return *this;
    }
    Stmt &null(int i) {
        sqlite3_bind_null(s, i);
        return *this;
    }
    bool step() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool run() {
        if (!s) return false;
        int rc = sqlite3_step(s);
        return rc == SQLITE_DONE || rc == SQLITE_ROW;
    }
    std::int64_t col_i64(int i) const { return sqlite3_column_int64(s, i); }
    bool         col_null(int i) const { return sqlite3_column_type(s, i) == SQLITE_NULL; }
    std::string  col_text(int i) const {
        auto *t = sqlite3_column_text(s, i);
        return t ? reinterpret_cast<const char *>(t) : "";
    }
};

} // namespace

std::optional<HistoryNote> history_note_rx(const std::string &from, const std::string &text, bool to_me,
                                           const std::string &my_call) {
    if (from.empty()) return std::nullopt;
    if (!my_call.empty() && base_callsign(from) == base_callsign(my_call)) return std::nullopt; // our own echo
    HistoryNote n;
    n.as_sent = from;
    n.call    = base_callsign(from);
    std::string body = text;
    if (auto colon = body.find(':'); colon != std::string::npos) body = body.substr(colon + 1);
    n.grid = announced_grid(body);
    auto d  = parse_directed(text);
    auto mc = classify(text, my_call); // its recipient, if it's a call or a group
    if (d && (d->cmd == " INFO" || d->cmd == " STATUS") && !trim(d->text).empty()) {
        n.info_kind = d->cmd == " INFO" ? 0 : 1;
        n.info_text = trim(d->text);
        n.info_to   = d->to;
    }
    if (to_me) {
        n.exchange  = true;
        n.heartbeat = d && d->cmd == " HEARTBEAT SNR";
        int snr;
        if (d && (d->cmd == " SNR" || d->cmd == " HEARTBEAT SNR") && parse_snr(d->text, &snr)) n.reported_snr = snr;
    } else if (mc.to.empty() && !mc.heartbeat && !mc.cq) {
        n.loose = true; // "W1ABC: GOOD COPY": to whoever they're talking to
    }
    return n;
}

std::optional<HistoryNote> history_note_tx(const std::string &text, const std::string &my_call) {
    // The recipient as the classifier sees it: a call or a group, never a
    // word ("K2XYZ: GOOD COPY" is to nobody, not to "GOOD").
    auto        mc = classify(text, my_call);
    HistoryNote n;
    if (mc.to.empty()) {
        if (mc.heartbeat || mc.cq) return std::nullopt;
        n.loose = true; // free text: the caller says to whom
        return n;
    }
    if (mc.to[0] == '@') return std::nullopt;
    if (!my_call.empty() && base_callsign(mc.to) == base_callsign(my_call)) return std::nullopt;
    auto d      = parse_directed(text);
    n.as_sent   = mc.to;
    n.call      = base_callsign(mc.to);
    n.exchange  = true;
    n.heartbeat = d && d->cmd == " HEARTBEAT SNR";
    return n;
}

History::~History() {
    close();
}

bool History::open(const std::string &path, bool background) {
    close();
    sqlite3 *db = nullptr;
    auto     try_open = [&]() {
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            db = nullptr;
            return false;
        }
        sqlite3_busy_timeout(db, 2000);
        // A rollback journal truncated, not deleted, after each commit: on
        // the card's FAT partition that's fewer directory writes. Synchronous
        // stays FULL (the default): the radio can lose power any time.
        sqlite3_exec(db, "PRAGMA journal_mode = TRUNCATE;", nullptr, nullptr, nullptr);
        if (sqlite3_exec(db, SCHEMA, nullptr, nullptr, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            db = nullptr;
            return false;
        }
        return true;
    };
    if (!try_open()) {
        // Not a database (an SD card error): kept aside, a new one started,
        // as the other JS8 files are.
        char      stamp[32];
        std::time_t t = std::time(nullptr);
        std::tm   tm{};
        gmtime_r(&t, &tm);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%SZ", &tm);
        std::string aside = path + ".unreadable-" + stamp;
        if (std::rename(path.c_str(), aside.c_str()) != 0 || !try_open()) return false;
    }
    db_ = db;
    {
        Stmt q(db_, "SELECT call, band FROM contacts");
        while (q.step()) known_.insert(q.col_text(0) + " " + q.col_text(1));
        Stmt w(db_, "SELECT DISTINCT call FROM qsos WHERE real_lines > 0");
        while (w.step()) qso_calls_.insert(w.col_text(0));
    }
    stop_       = false;
    background_ = background;
    posted_ = written_ = 0;
    if (background_) thread_ = std::thread([this] { run(); });
    return true;
}

void History::close() {
    if (!db_) return;
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    std::lock_guard<std::mutex> dl(db_mu_);
    sqlite3_close(db_);
    db_ = nullptr;
    known_.clear();
    qso_calls_.clear();
    open_.clear();
    queue_.clear();
}

void History::post(Op op) {
    if (!db_) return;
    if (!background_) {
        std::deque<Op> one{std::move(op)};
        write_all(one);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(op));
        posted_++;
    }
    cv_.notify_all();
}

void History::received(const HistoryNote &n, const std::string &text, const std::string &band, double freq_hz,
                       int snr, int speed, std::int64_t now_ms) {
    Op op;
    op.note    = n;
    op.text    = text;
    op.band    = band;
    op.freq_hz = freq_hz;
    op.snr     = snr;
    op.speed   = speed;
    op.ms      = now_ms;
    if (n.exchange || n.info_kind >= 0) {
        if (n.exchange) {
            std::lock_guard<std::mutex> lk(mu_);
            known_.insert(n.call + " " + band);
            if (!n.heartbeat) qso_calls_.insert(n.call); // now, not when it's written: Sort: QSO shows them at once
            last_exchange_ms_ = now_ms;
        }
        op.kind = Op::Rx;
        post(std::move(op));
    } else if (n.loose && known(n.call, band)) { // their text in a QSO with us, calls dropped
        op.kind = Op::Loose;
        post(std::move(op));
    } else if (known(n.call, band)) { // someone we've talked to here: when and how we hear them
        op.kind = Op::Heard;
        post(std::move(op));
    }
}

void History::sent(const HistoryNote &n, const std::string &text, const std::string &band, double freq_hz, int speed,
                   bool automatic, std::int64_t now_ms) {
    if (n.loose) {
        if (n.call.empty() || !known(n.call, band)) return; // free text joins an open QSO only
    } else if (!n.exchange) {
        return;
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        known_.insert(n.call + " " + band);
        if (!n.heartbeat) qso_calls_.insert(n.call);
        last_exchange_ms_ = now_ms;
    }
    Op op;
    op.kind      = n.loose ? Op::Loose : Op::Tx;
    op.tx        = true;
    op.note      = n;
    op.text      = text;
    op.band      = band;
    op.freq_hz   = freq_hz;
    op.speed     = speed;
    op.automatic = automatic;
    op.ms        = now_ms;
    post(std::move(op));
}

void History::received_callless(const std::string &text, const std::string &band, double freq_hz, double window_hz,
                                int snr, int speed, std::int64_t now_ms) {
    if (now_ms - last_exchange_ms_ > QSO_GAP_MS) return; // no QSO open: nothing to join
    Op op;
    op.kind      = Op::Loose;
    op.text      = text;
    op.band      = band;
    op.freq_hz   = freq_hz;
    op.window_hz = window_hz;
    op.snr       = snr;
    op.speed     = speed;
    op.ms        = now_ms;
    post(std::move(op));
}

void History::logged(const std::string &call, const std::string &band, std::int64_t now_ms) {
    Op op;
    op.kind      = Op::Logged;
    op.note.call = base_callsign(call);
    op.band      = band;
    op.ms        = now_ms;
    post(std::move(op));
}

void History::heard(const std::string &call, const std::string &band, int snr, const std::string &grid,
                    std::int64_t now_ms) {
    if (!known(base_callsign(call), band)) return;
    Op op;
    op.kind      = Op::Heard;
    op.note.call = base_callsign(call);
    op.note.grid = grid;
    op.band      = band;
    op.snr       = snr;
    op.ms        = now_ms;
    post(std::move(op));
}

bool History::had_qso(const std::string &call) const {
    std::lock_guard<std::mutex> lk(mu_);
    return qso_calls_.count(call) != 0;
}

bool History::known(const std::string &call, const std::string &band) const {
    std::lock_guard<std::mutex> lk(mu_);
    return known_.count(call + " " + band) != 0;
}

void History::flush() {
    if (!db_ || !background_) return;
    std::unique_lock<std::mutex> lk(mu_);
    std::uint64_t target = posted_;
    flushes_++;
    cv_.notify_all();
    done_cv_.wait(lk, [&] { return written_ >= target || !thread_.joinable(); });
    flushes_--;
}

void History::clear() {
    if (!db_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        known_.clear(); // from now on nobody is known
        qso_calls_.clear();
    }
    last_exchange_ms_ = 0;
    Op op;
    op.kind = Op::Clear; // after what's waiting, on the writer's thread (it owns the open QSOs)
    post(std::move(op));
    flush();
}

int History::station_count() {
    if (!db_) return 0;
    flush();
    std::lock_guard<std::mutex> dl(db_mu_);
    Stmt q(db_, "SELECT COUNT(DISTINCT call) FROM contacts");
    return q.step() ? (int)q.col_i64(0) : 0;
}

HistoryStats History::take_stats() {
    std::lock_guard<std::mutex> lk(mu_);
    HistoryStats s = stats_;
    stats_         = {};
    return s;
}

void History::run() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "js8-hist");
    // Below everything else: the decoder, the screen and the audio come first.
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cv_.wait(lk, [this] { return stop_ || !queue_.empty() || flushes_ > 0; });
        // A few seconds' worth in one transaction, unless someone waits.
        if (!stop_ && flushes_ == 0)
            cv_.wait_for(lk, std::chrono::milliseconds(BATCH_MS), [this] { return stop_ || flushes_ > 0; });
        std::deque<Op> ops;
        ops.swap(queue_);
        lk.unlock();
        if (!ops.empty()) write_all(ops);
        lk.lock();
        written_ += ops.size();
        done_cv_.notify_all();
        if (stop_ && queue_.empty()) break;
    }
}

void History::write_all(std::deque<Op> &ops) {
    std::lock_guard<std::mutex> dl(db_mu_);
    std::int64_t t0 = mono_us();
    bool         ok = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok) {
        for (auto &op : ops) apply(op);
        ok = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
        if (!ok) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            open_.clear(); // the QSO ids it remembered may not exist
        }
    }
    std::int64_t dt = mono_us() - t0;
    std::lock_guard<std::mutex> lk(mu_);
    stats_.busy_us += dt;
    if (ok) {
        stats_.rows += (unsigned)ops.size();
        stats_.commits++;
    } else {
        stats_.failed += (unsigned)ops.size();
    }
}

void History::apply(const Op &op) {
    const HistoryNote &n = op.note;
    switch (op.kind) {
    case Op::Rx:
        if (n.exchange) exchange(op);
        if (n.info_kind >= 0)
            Stmt(db_, "INSERT INTO infos (call, kind, ms, text, to_call, band) VALUES (?1, ?2, ?3, ?4, ?5, ?6)")
                .text(1, n.call)
                .i64(2, n.info_kind)
                .i64(3, op.ms)
                .text(4, n.info_text)
                .text(5, n.info_to)
                .text(6, op.band)
                .run();
        break;
    case Op::Tx: exchange(op); break;
    case Op::Loose: loose(op); break;
    case Op::Clear:
        sqlite3_exec(db_, "DELETE FROM lines; DELETE FROM qsos; DELETE FROM infos; DELETE FROM contacts;", nullptr, nullptr,
                     nullptr);
        open_.clear();
        break;
    case Op::Logged:
        Stmt(db_, "UPDATE qsos SET logged = 1 WHERE id = (SELECT id FROM qsos WHERE call = ?1 AND band = ?2"
                  " ORDER BY end_ms DESC LIMIT 1)")
            .text(1, n.call)
            .text(2, op.band)
            .run();
        break;
    case Op::Heard:
        Stmt(db_, "UPDATE contacts SET heard_ms = MAX(heard_ms, ?3), snr = ?4,"
                  " grid = CASE WHEN ?5 <> '' THEN ?5 ELSE grid END WHERE call = ?1 AND band = ?2")
            .text(1, n.call)
            .text(2, op.band)
            .i64(3, op.ms)
            .i64(4, op.snr)
            .text(5, n.grid)
            .run();
        break;
    }
}

// A message between us and them: their contact on this band, the QSO it
// belongs to (a new one after QSO_GAP_MS or on another band), its line.
void History::exchange(const Op &op) {
    const HistoryNote &n  = op.note;
    const bool         rx = op.kind == Op::Rx;
    {
        Stmt s(db_, "INSERT INTO contacts (call, band, grid, first_ms, last_ms, heard_ms, heard_us_ms, snr, reported_snr)"
                    " VALUES (?1, ?2, ?3, ?4, ?4, ?5, ?6, ?7, ?8)"
                    " ON CONFLICT (call, band) DO UPDATE SET last_ms = MAX(last_ms, excluded.last_ms),"
                    " heard_ms = MAX(heard_ms, excluded.heard_ms), heard_us_ms = MAX(heard_us_ms, excluded.heard_us_ms),"
                    " grid = CASE WHEN excluded.grid <> '' THEN excluded.grid ELSE grid END,"
                    " snr = COALESCE(excluded.snr, snr), reported_snr = COALESCE(excluded.reported_snr, reported_snr)");
        s.text(1, n.call).text(2, op.band).text(3, n.grid).i64(4, op.ms);
        s.i64(5, rx ? op.ms : 0).i64(6, rx ? op.ms : 0);
        if (rx) s.i64(7, op.snr);
        else s.null(7);
        if (n.reported_snr) s.i64(8, *n.reported_snr);
        else s.null(8);
        s.run();
    }

    Open &o = open_for(n.call);
    if (!is_open(o, op)) {
        Stmt s(db_, "INSERT INTO qsos (call, band, start_ms, end_ms, lines, real_lines) VALUES (?1, ?2, ?3, ?3, 0, 0)");
        s.text(1, n.call).text(2, op.band).i64(3, op.ms).run();
        o = {sqlite3_last_insert_rowid(db_), op.ms, op.band, 0};
    }
    add_line(o, n.call, op, rx, n.heartbeat, n.as_sent);
}

// The latest QSO with `call`: remembered, or read from the file once.
History::Open &History::open_for(const std::string &call) {
    auto it = open_.find(call);
    if (it == open_.end()) {
        Stmt q(db_, "SELECT id, end_ms, band FROM qsos WHERE call = ?1 ORDER BY end_ms DESC LIMIT 1");
        q.text(1, call);
        Open o;
        if (q.step()) o = {q.col_i64(0), q.col_i64(1), q.col_text(2), 0};
        it = open_.emplace(call, o).first;
    }
    return it->second;
}

void History::add_line(Open &o, const std::string &call, const Op &op, bool rx, bool heartbeat,
                       const std::string &call_as) {
    if (!heartbeat) {
        std::lock_guard<std::mutex> lk(mu_);
        qso_calls_.insert(call);
    }
    Stmt(db_, "UPDATE qsos SET end_ms = MAX(end_ms, ?2), lines = lines + 1, real_lines = real_lines + ?3"
              " WHERE id = ?1")
        .i64(1, o.id)
        .i64(2, op.ms)
        .i64(3, heartbeat ? 0 : 1)
        .run();
    o.end_ms = std::max(o.end_ms, op.ms);
    if (rx) o.rx_freq = op.freq_hz;
    Stmt s(db_, "INSERT INTO lines (qso, ms, tx, snr, freq_hz, speed, auto, heartbeat, call_as, text)"
                " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    s.i64(1, o.id).i64(2, op.ms).i64(3, rx ? 0 : 1);
    if (rx) s.i64(4, op.snr);
    else s.null(4);
    s.real(5, op.freq_hz).i64(6, op.speed).i64(7, op.automatic ? 1 : 0).i64(8, heartbeat ? 1 : 0);
    s.text(9, call_as).text(10, op.text).run();
}

// Text without the calls: into the open QSO with its station (the sender
// named, or the one sending on that offset), else nowhere.
void History::loose(const Op &op) {
    const HistoryNote &n = op.note;
    if (!n.call.empty()) {
        Open &o = open_for(n.call);
        if (is_open(o, op)) add_line(o, n.call, op, !op.tx, false, n.as_sent);
        return;
    }
    Open       *best = nullptr;
    std::string best_call;
    double      best_d = op.window_hz;
    for (auto &[call, o] : open_) {
        if (!is_open(o, op) || o.rx_freq <= 0) continue;
        double d = std::fabs(o.rx_freq - op.freq_hz);
        if (d <= best_d) {
            best      = &o;
            best_call = call;
            best_d    = d;
        }
    }
    if (best) add_line(*best, best_call, op, true, false, "");
}

namespace {
constexpr const char *CONTACT_COLS = "SELECT call, grid, band, first_ms, last_ms, heard_ms, heard_us_ms, snr, reported_snr"
                                     " FROM contacts ";
} // namespace

std::optional<HistoryContact> History::contact(const std::string &call, const std::string &band) {
    if (!db_) return std::nullopt;
    auto all = contacts(band); // a few hundred at most: no index needed
    for (auto &c : all)
        if (c.call == base_callsign(call)) return c;
    return std::nullopt;
}

std::vector<HistoryContact> History::contacts(const std::string &band) {
    std::vector<HistoryContact> out;
    if (!db_) return out;
    flush();
    std::lock_guard<std::mutex> dl(db_mu_);
    Stmt q(db_, (std::string(CONTACT_COLS) + "WHERE band = ?1 ORDER BY last_ms DESC").c_str());
    q.text(1, band);
    while (q.step()) {
        HistoryContact c;
        c.call        = q.col_text(0);
        c.grid        = q.col_text(1);
        c.band        = q.col_text(2);
        c.first_ms    = q.col_i64(3);
        c.last_ms     = q.col_i64(4);
        c.heard_ms    = q.col_i64(5);
        c.heard_us_ms = q.col_i64(6);
        c.snr         = (int)q.col_i64(7);
        if (!q.col_null(8)) c.reported_snr = (int)q.col_i64(8);
        out.push_back(std::move(c));
    }
    return out;
}

std::optional<HistoryInfo> History::latest_info(const std::string &call, int kind) {
    if (!db_) return std::nullopt;
    flush();
    std::lock_guard<std::mutex> dl(db_mu_);
    Stmt q(db_, "SELECT ms, text, to_call, band FROM infos WHERE call = ?1 AND kind = ?2 ORDER BY ms DESC LIMIT 1");
    q.text(1, base_callsign(call)).i64(2, kind);
    if (!q.step()) return std::nullopt;
    return HistoryInfo{q.col_i64(0), q.col_text(1), q.col_text(2), q.col_text(3)};
}

std::vector<HistoryQso> History::qsos(const std::string &call, bool with_heartbeat_only) {
    std::vector<HistoryQso> out;
    if (!db_) return out;
    flush();
    std::lock_guard<std::mutex> dl(db_mu_);
    Stmt q(db_, "SELECT id, start_ms, end_ms, band, lines, real_lines, logged FROM qsos WHERE call = ?1"
                " AND (?2 OR real_lines > 0) ORDER BY end_ms DESC");
    q.text(1, base_callsign(call)).i64(2, with_heartbeat_only ? 1 : 0);
    while (q.step())
        out.push_back({q.col_i64(0), q.col_i64(1), q.col_i64(2), q.col_text(3), (int)q.col_i64(4), (int)q.col_i64(5),
                       q.col_i64(6) != 0});
    return out;
}

std::vector<HistoryLine> History::lines(std::int64_t qso_id) {
    std::vector<HistoryLine> out;
    if (!db_) return out;
    flush();
    std::lock_guard<std::mutex> dl(db_mu_);
    Stmt q(db_, "SELECT ms, tx, snr, text, heartbeat FROM lines WHERE qso = ?1 ORDER BY ms, id");
    q.i64(1, qso_id);
    while (q.step())
        out.push_back({q.col_i64(0), q.col_i64(1) != 0, (int)q.col_i64(2), q.col_text(3), q.col_i64(4) != 0});
    return out;
}

} // namespace x6100::js8

// ---- C API ---------------------------------------------------------------

struct js8_history {
    x6100::js8::History h;
};

namespace {

void copy_str(char *dst, std::size_t len, const std::string &src) {
    if (!dst || !len) return;
    std::size_t n = std::min(len - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// A grid from the Stations list when the message itself has none: most
// stations announce theirs in a heartbeat before you ever exchange a word.
void grid_from(js8_stations_t *stations, x6100::js8::HistoryNote &n, std::int64_t now_ms) {
    js8_station_t st;
    if (n.grid.empty() && stations && js8_stations_find(stations, n.as_sent.c_str(), now_ms, &st) && st.grid[0])
        n.grid = st.grid;
}

} // namespace

extern "C" js8_history_t *js8_history_open(const char *path) {
    if (!path) return nullptr;
    auto *h = new (std::nothrow) js8_history;
    if (h && !h->h.open(path)) {
        delete h;
        return nullptr;
    }
    return h;
}

extern "C" void js8_history_close(js8_history_t *h) {
    delete h;
}

extern "C" void js8_history_rx(js8_history_t *h, const js8_rx_msg_t *m, const char *my_call, uint64_t dial_hz,
                               js8_stations_t *stations, int64_t now_ms) {
    if (!h || !m || m->tx || m->partial || m->low_confidence) return;
    if (!m->from[0]) { // its first frame (the sender) missed: by its offset
        double freq = (double)dial_hz + m->freq_hz;
        h->h.received_callless(m->text, x6100::js8::adif_band((std::uint64_t)freq), freq,
                               js8_speed_rx_threshold_hz(js8_speed_from_submode(m->submode)), m->snr, m->submode,
                               now_ms);
        return;
    }
    auto n = x6100::js8::history_note_rx(m->from, m->text, m->to_me, my_call ? my_call : "");
    if (!n) return;
    if (n->exchange) grid_from(stations, *n, now_ms);
    double freq = (double)dial_hz + m->freq_hz;
    h->h.received(*n, m->text, x6100::js8::adif_band((std::uint64_t)freq), freq, m->snr, m->submode, now_ms);
}

extern "C" void js8_history_tx(js8_history_t *h, const char *text, const char *my_call, uint64_t dial_hz,
                               float offset_hz, uint8_t submode, bool automatic, const char *partner,
                               js8_stations_t *stations, int64_t now_ms) {
    if (!h || !text) return;
    auto n = x6100::js8::history_note_tx(text, my_call ? my_call : "");
    if (!n) return;
    if (n->loose) { // free text: to the station you have selected
        if (!partner || !partner[0]) return;
        n->as_sent = partner;
        n->call    = x6100::js8::base_callsign(partner);
    }
    grid_from(stations, *n, now_ms);
    double freq = (double)dial_hz + offset_hz;
    h->h.sent(*n, text, x6100::js8::adif_band((std::uint64_t)freq), freq, submode, automatic, now_ms);
}

extern "C" void js8_history_logged(js8_history_t *h, const char *call, uint64_t freq_hz, int64_t now_ms) {
    if (!h || !call) return;
    h->h.logged(call, x6100::js8::adif_band(freq_hz), now_ms);
}

extern "C" bool js8_history_had_qso(js8_history_t *h, const char *call) {
    return h && call && h->h.had_qso(x6100::js8::base_callsign(call));
}

extern "C" bool js8_history_known(js8_history_t *h, const char *call, uint64_t freq_hz) {
    return h && call && h->h.known(x6100::js8::base_callsign(call), x6100::js8::adif_band(freq_hz));
}

extern "C" void js8_history_clear(js8_history_t *h) {
    if (h) h->h.clear();
}

extern "C" int js8_history_station_count(js8_history_t *h) {
    return h ? h->h.station_count() : 0;
}

extern "C" void js8_history_flush(js8_history_t *h) {
    if (h) h->h.flush();
}

extern "C" bool js8_history_stats(js8_history_t *h, unsigned *rows, unsigned *commits, unsigned *failed,
                                  int64_t *busy_us) {
    if (!h) return false;
    auto s = h->h.take_stats();
    if (rows) *rows = s.rows;
    if (commits) *commits = s.commits;
    if (failed) *failed = s.failed;
    if (busy_us) *busy_us = s.busy_us;
    return s.rows || s.commits || s.failed;
}

static void to_c(const x6100::js8::HistoryContact &c, js8_hist_contact_t &o) {
    o = js8_hist_contact_t{};
    copy_str(o.call, sizeof(o.call), c.call);
    copy_str(o.grid, sizeof(o.grid), c.grid);
    copy_str(o.band, sizeof(o.band), c.band);
    o.first_ms         = c.first_ms;
    o.last_ms          = c.last_ms;
    o.heard_ms         = c.heard_ms;
    o.heard_us_ms      = c.heard_us_ms;
    o.snr              = (int16_t)c.snr;
    o.has_reported_snr = c.reported_snr.has_value();
    o.reported_snr     = (int16_t)c.reported_snr.value_or(0);
}

extern "C" bool js8_history_contact(js8_history_t *h, const char *call, const char *band, js8_hist_contact_t *out) {
    if (!h || !call || !band || !out) return false;
    auto c = h->h.contact(call, band);
    if (!c) return false;
    to_c(*c, *out);
    return true;
}

extern "C" int js8_history_contacts(js8_history_t *h, const char *band, js8_hist_contact_t *out, int max) {
    if (!h || !band || !out || max <= 0) return 0;
    int n = 0;
    for (auto &c : h->h.contacts(band)) {
        if (n >= max) break;
        to_c(c, out[n++]);
    }
    return n;
}

extern "C" bool js8_history_info(js8_history_t *h, const char *call, int kind, js8_hist_info_t *out) {
    if (!h || !call || !out) return false;
    auto i = h->h.latest_info(call, kind);
    if (!i) return false;
    *out    = js8_hist_info_t{};
    out->ms = i->ms;
    copy_str(out->text, sizeof(out->text), i->text);
    copy_str(out->to, sizeof(out->to), i->to);
    return true;
}

extern "C" int js8_history_qsos(js8_history_t *h, const char *call, js8_hist_qso_t *out, int max) {
    if (!h || !call || !out || max <= 0) return 0;
    int n = 0;
    for (auto &q : h->h.qsos(call)) {
        if (n >= max) break;
        js8_hist_qso_t &o = out[n++];
        o                 = js8_hist_qso_t{};
        o.id              = q.id;
        o.start_ms        = q.start_ms;
        o.end_ms          = q.end_ms;
        copy_str(o.band, sizeof(o.band), q.band);
        o.lines      = q.lines;
        o.real_lines = q.real_lines;
        o.logged     = q.logged;
    }
    return n;
}

extern "C" int js8_history_lines(js8_history_t *h, int64_t qso_id, js8_hist_line_t *out, int max) {
    if (!h || !out || max <= 0) return 0;
    int n = 0;
    for (auto &l : h->h.lines(qso_id)) {
        if (n >= max) break;
        js8_hist_line_t &o = out[n++];
        o                  = js8_hist_line_t{};
        o.ms               = l.ms;
        o.tx               = l.tx;
        o.heartbeat        = l.heartbeat;
        o.snr              = (int16_t)l.snr;
        copy_str(o.text, sizeof(o.text), l.text);
    }
    return n;
}
