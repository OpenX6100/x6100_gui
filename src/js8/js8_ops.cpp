/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 one-press messages, heartbeats and the
 *  station list; C interface for the dialog.
 */

#include "js8_ops.h"

#include "alerts.hpp"
#include "autoreply.hpp"
#include "classify.hpp"
#include "commands.hpp"
#include "datafile.hpp"
#include "directed.hpp"
#include "geo.hpp"
#include "inbox.hpp"
#include "macros.hpp"
#include "qsolog.hpp"
#include "stations.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <cstring>
#include <new>
#include <random>
#include <vector>

using namespace x6100::js8;

namespace {

void copy_str(char *dst, std::size_t cap, const std::string &src) {
    if (!dst || cap == 0) return;
    std::size_t n = std::min(cap - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

const char *const LABELS[JS8_Q_COUNT] = {
    "SNR?", "Send SNR", "GRID?", "My grid", "INFO?", "STATUS?", "HEARING?", "AGN?", "RR", "73",
};

} // namespace

struct js8_stations {
    StationList  list;
    std::int64_t pruned_ms = 0; ///< expired stations last forgotten
};

struct js8_held {
    HeldMessages held;
    std::string  path;
    bool         unsaved = false; ///< a change the card hasn't got yet
};

struct js8_inbox {
    Inbox       box;
    std::string path;
    bool        unsaved = false;
};

namespace {
// Write the store to the card; remember a failure so the next change or
// resend tries again.
bool sync(js8_inbox *b) {
    b->unsaved = !b->box.save(b->path);
    return !b->unsaved;
}
bool sync(js8_held *h) {
    h->unsaved = !h->held.save(h->path);
    return !h->unsaved;
}
} // namespace

extern "C" const char *js8_query_label(js8_query_t q) {
    return (unsigned)q < JS8_Q_COUNT ? LABELS[q] : "";
}

extern "C" bool js8_query_text(js8_query_t q, const char *to_call, int their_snr, const char *my_grid, char *out,
                               unsigned out_len) {
    if ((unsigned)q >= JS8_Q_COUNT) return false;
    auto text = query_text((Query)q, to_call ? to_call : "", their_snr, my_grid ? my_grid : "");
    copy_str(out, out_len, text);
    return !text.empty();
}

extern "C" bool js8_is_grid(const char *text) {
    if (!text) return false;
    std::string g = text;
    auto        b = g.find_first_not_of(' '), e = g.find_last_not_of(' ');
    if (b == std::string::npos) return false;
    g = g.substr(b, e - b + 1);
    for (auto &c : g) c = (char)std::toupper((unsigned char)c);
    return is_grid(g);
}

extern "C" void js8_heartbeat_text(const char *my_call, const char *my_grid, char *out, unsigned out_len) {
    copy_str(out, out_len, heartbeat_text(my_call ? my_call : "", my_grid ? my_grid : ""));
}

extern "C" int js8_heartbeat_offset(const float *offsets_hz, const int64_t *heard_ms, unsigned n, int64_t now_ms) {
    static std::mt19937          rng{std::random_device{}()};
    std::vector<OffsetActivity> activity;
    for (unsigned i = 0; i < n; i++) activity.push_back({offsets_hz[i], heard_ms[i]});
    return find_free_offset(activity, now_ms, rng);
}

namespace {
std::int64_t g_station_expire_ms = StationList::EXPIRE_MS;

void to_c(const Station &st, js8_station_t &o) {
    o = js8_station_t{};
    copy_str(o.call, sizeof(o.call), st.call);
    copy_str(o.grid, sizeof(o.grid), st.grid);
    o.heard_ms         = st.heard_ms;
    o.snr              = (int16_t)st.snr;
    o.freq_hz          = st.freq_hz;
    o.submode          = (uint8_t)st.mode;
    o.heard_me         = st.heard_me;
    o.aprs_gate        = st.aprs_gate;
    o.heard_me_ms      = st.heard_me_ms;
    o.has_reported_snr = st.reported_snr.has_value();
    o.reported_snr     = (int16_t)st.reported_snr.value_or(0);
    copy_str(o.via, sizeof(o.via), st.via);
}
} // namespace

extern "C" js8_stations_t *js8_stations_create(void) {
    return new (std::nothrow) js8_stations;
}

extern "C" void js8_stations_destroy(js8_stations_t *s) {
    delete s;
}

extern "C" void js8_stations_add(js8_stations_t *s, const js8_rx_msg_t *m, const char *my_call, int64_t now_ms) {
    if (!s || !m || m->tx) return;
    StationEvent ev;
    ev.from    = m->from;
    ev.to      = m->to;
    ev.text    = m->text;
    ev.to_me   = m->to_me;
    ev.snr     = m->snr;
    ev.freq_hz = m->freq_hz;
    ev.mode    = m->submode;
    ev.when_ms = now_ms;
    s->list.add(ev, my_call ? my_call : "");
    // Forget expired stations once a minute: they were only hidden, and
    // every read copied them all (days on a busy band: thousands).
    if (now_ms - s->pruned_ms >= 60'000) {
        s->list.expire(now_ms, g_station_expire_ms);
        s->pruned_ms = now_ms;
    }
}

extern "C" void js8_stations_set_expire_ms(int64_t ms) {
    g_station_expire_ms = ms;
}

extern "C" void js8_stations_add_via(js8_stations_t *s, const char *call, const char *via, float freq_hz,
                                     uint8_t submode, int64_t now_ms) {
    if (!s || !call || !via) return;
    s->list.add_via(call, via, freq_hz, submode, now_ms, g_station_expire_ms);
}

extern "C" int js8_relay_stations(const js8_rx_msg_t *msg, const char *my_call, char (*out)[JS8_RX_CALL_LEN],
                                  int max, char *via, unsigned via_len) {
    if (!msg || msg->tx || !my_call || !my_call[0] || !msg->to_me || msg->checksum != 1) return 0;
    auto d = parse_directed(msg->text);
    if (!d || d->cmd != ">" || relay_next_hop(d->text) || d->text.rfind("ACK", 0) == 0) return 0;
    auto calls = relay_path_calls(d->from, d->text);
    copy_str(via, via_len, d->from);
    int n = 0;
    for (std::size_t i = 1; i < calls.size() && n < max; i++) {
        if (base_callsign(calls[i]) == base_callsign(my_call)) continue;
        copy_str(out[n++], JS8_RX_CALL_LEN, calls[i]);
    }
    return n;
}

extern "C" void js8_stations_sort(js8_station_t *st, int n, js8_st_sort_t order, const char *my_grid) {
    if (!st || n < 2) return;
    switch (order) {
    case JS8_ST_SORT_SNR:
        std::stable_sort(st, st + n, [](const js8_station_t &a, const js8_station_t &b) { return a.snr > b.snr; });
        break;
    case JS8_ST_SORT_TIME:
    case JS8_ST_SORT_QSO:
        std::stable_sort(st, st + n,
                         [](const js8_station_t &a, const js8_station_t &b) { return a.heard_ms > b.heard_ms; });
        break;
    case JS8_ST_SORT_DISTANCE: {
        auto home = geo::grid_center(my_grid ? my_grid : "");
        std::vector<std::pair<double, int>> key((std::size_t)n); // km (-1: unknown), place in the list
        for (int i = 0; i < n; i++) {
            auto there = home ? geo::grid_center(st[i].grid) : std::nullopt;
            key[(std::size_t)i] = {there ? geo::distance_km(*home, *there) : -1.0, i};
        }
        std::stable_sort(key.begin(), key.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::vector<js8_station_t> sorted((std::size_t)n);
        for (int i = 0; i < n; i++) sorted[(std::size_t)i] = st[key[(std::size_t)i].second];
        std::copy(sorted.begin(), sorted.end(), st);
        break;
    }
    default: // JS8_ST_SORT_HEARD_ME: as the list comes
        break;
    }
}

extern "C" void js8_macros_expand(const char *text, const js8_macro_values_t *v, bool prune, char *out,
                                  unsigned out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!text) return;
    MacroInput in;
    if (v) {
        in.my_call   = v->my_call ? v->my_call : "";
        in.my_grid   = v->my_grid ? v->my_grid : "";
        in.my_info   = v->my_info ? v->my_info : "";
        in.my_status = v->my_status ? v->my_status : "";
        in.version   = v->version ? v->version : "";
        in.idle_ms   = v->idle_ms;
        in.call      = v->call ? v->call : "";
        if (v->has_snr) in.snr = v->snr;
        if (v->has_tdelta) in.tdelta_ms = v->tdelta_ms;
    }
    copy_str(out, out_len, replace_macros(text, macro_values(in), prune));
}

extern "C" bool js8_macros_need_station(const char *text) {
    return text && macros_need_station(text);
}

extern "C" bool js8_command_span(const char *text, unsigned *start, unsigned *len) {
    if (!text) return false;
    std::string t = text;
    auto        d = parse_directed(t);
    if (!d) return false;
    // After "FROM: TO": the command, or a CQ's "CQ CQ CQ".
    auto pos = t.find(": ");
    pos      = t.find(d->to, pos == std::string::npos ? 0 : pos + 2);
    if (pos == std::string::npos) return false;
    pos += d->to.size();
    std::string cmd = d->cmd == ">" ? ">" : d->cmd.size() > 1 ? d->cmd.substr(1) : "";
    std::size_t at  = std::string::npos, n = 0;
    if (!cmd.empty()) {
        at = t.find(cmd, pos);
        n  = cmd.size();
    } else if (d->text.rfind("CQ", 0) == 0) { // "@ALLCALL CQ CQ CQ FN42"
        at = t.find("CQ", pos);
        n  = 2;
        while (at != std::string::npos && t.compare(at + n, 3, " CQ") == 0) n += 3;
    }
    if (at == std::string::npos || at > pos + 2) return false; // right after the target only
    if (start) *start = (unsigned)at;
    if (len) *len = (unsigned)n;
    return true;
}

extern "C" int js8_stations_list(js8_stations_t *s, int64_t now_ms, js8_station_t *out, int max) {
    if (!s || !out || max <= 0) return 0;
    auto list = s->list.sorted(now_ms, g_station_expire_ms);
    int  n    = std::min<int>(max, (int)list.size());
    for (int i = 0; i < n; i++) to_c(list[i], out[i]);
    return n;
}

extern "C" int js8_stations_recent(js8_stations_t *s, int64_t now_ms, js8_station_t *out, int max) {
    if (!s || !out || max <= 0) return 0;
    auto list = s->list.recent(now_ms, (std::size_t)max, g_station_expire_ms);
    for (std::size_t i = 0; i < list.size(); i++) to_c(list[i], out[i]);
    return (int)list.size();
}

extern "C" bool js8_stations_find(js8_stations_t *s, const char *call, int64_t now_ms, js8_station_t *out) {
    if (!s || !call) return false;
    const Station *st = s->list.find(call, now_ms, g_station_expire_ms);
    if (st && out) to_c(*st, *out);
    return st != nullptr;
}

extern "C" bool js8_stations_heard_before(js8_stations_t *s, const char *call) {
    return s && call && s->list.heard_before(call);
}

extern "C" void js8_stations_clear(js8_stations_t *s) {
    if (s) s->list.clear();
}

extern "C" void js8_stations_reset(js8_stations_t *s) {
    if (s) {
        s->list.reset();
        s->pruned_ms = 0;
    }
}

/* ---- T4 -------------------------------------------------------------- */

struct js8_auto {
    AutoPolicy policy;
};

namespace {
Incoming to_incoming(const js8_rx_msg_t *m) {
    Incoming in;
    in.from           = m->from;
    in.to             = m->to;
    in.text           = m->text;
    in.to_me          = m->to_me;
    in.to_group       = m->to_group;
    in.heartbeat      = m->heartbeat;
    in.low_confidence = m->low_confidence;
    in.checksum_ok    = m->checksum == 1;
    in.snr            = m->snr;
    return in;
}
} // namespace

extern "C" js8_auto_t *js8_auto_create(void) {
    return new (std::nothrow) js8_auto;
}

extern "C" void js8_auto_destroy(js8_auto_t *a) {
    delete a;
}

namespace {
std::vector<std::string> group_list(const char *groups) {
    std::vector<std::string> out;
    std::string              g = groups ? groups : "";
    for (auto &c : g) c = (char)std::toupper((unsigned char)c);
    std::size_t start = 0;
    while (start < g.size()) {
        auto end = g.find_first_of(" ,", start);
        if (end == std::string::npos) end = g.size();
        if (end > start) out.push_back(g.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::vector<Heard> heard_list(const js8_heard_t *heard, unsigned n) {
    std::vector<Heard> out;
    for (unsigned i = 0; heard && i < n; i++)
        if (heard[i].call) out.emplace_back(heard[i].call, heard[i].snr, heard[i].heard_ms);
    return out;
}

// Keep what desktop keeps: the inbox for us, held for someone else.
void keep(const StoreAction &a, std::int64_t now_ms, js8_inbox_t *inbox, js8_held_t *held, js8_stored_t *out) {
    // New messages count as new even in a full store (the oldest makes room;
    // comparing sizes took every one for a resend). A new one, or any change
    // an earlier save didn't get onto the card, is written now; id -1 = not
    // on the card.
    js8_stored_t st{};
    bool         added = false;
    if (a.kind == StoreAction::Kind::Inbox && inbox) {
        st.kind   = JS8_STORED_INBOX;
        st.id     = inbox->box.add(a.from, a.text, now_ms, a.to, a.path, &added);
        st.resend = !added;
        if ((added || inbox->unsaved) && !sync(inbox)) st.id = -1;
    } else if (a.kind == StoreAction::Kind::Held && held) {
        st.kind   = JS8_STORED_HELD;
        st.id     = held->held.add(a.from, a.to, a.text, now_ms, a.path, &added);
        st.resend = !added;
        if ((added || held->unsaved) && !sync(held)) st.id = -1;
    } else if (a.kind == StoreAction::Kind::AprsReceipt) {
        st.kind     = JS8_STORED_APRS_RECEIPT; /* nothing kept: the dialog marks its sent message */
        st.rejected = a.path == "REJ";
        copy_str(st.from, sizeof(st.from), a.from);
        copy_str(st.to, sizeof(st.to), a.to);
        copy_str(st.text, sizeof(st.text), a.text);
        if (out) *out = st;
        return;
    } else {
        return;
    }
    copy_str(st.from, sizeof(st.from), a.from);
    copy_str(st.to, sizeof(st.to), a.to);
    copy_str(st.path, sizeof(st.path), a.path);
    copy_str(st.text, sizeof(st.text), a.text);
    if (out) *out = st;
}

js8_reply_kind_t c_kind(ReplyKind k) {
    switch (k) {
    case ReplyKind::Query: return JS8_REPLY_QUERY;
    case ReplyKind::HeartbeatAck: return JS8_REPLY_HB_ACK;
    case ReplyKind::MsgAck: return JS8_REPLY_ACK;
    case ReplyKind::Suggest: return JS8_REPLY_SUGGEST;
    case ReplyKind::Relay: return JS8_REPLY_RELAY;
    case ReplyKind::Stored: return JS8_REPLY_STORED;
    }
    return JS8_REPLY_QUERY;
}

ReplyKind cpp_kind(js8_reply_kind_t k) {
    switch (k) {
    case JS8_REPLY_QUERY: return ReplyKind::Query;
    case JS8_REPLY_HB_ACK: return ReplyKind::HeartbeatAck;
    case JS8_REPLY_ACK: return ReplyKind::MsgAck;
    case JS8_REPLY_SUGGEST: return ReplyKind::Suggest;
    case JS8_REPLY_RELAY: return ReplyKind::Relay;
    case JS8_REPLY_STORED: return ReplyKind::Stored;
    }
    return ReplyKind::Query;
}
} // namespace

extern "C" void js8_process(js8_auto_t *a, const js8_rx_msg_t *msg, const js8_auto_settings_t *s,
                            const js8_heard_t *heard, unsigned n_heard, const char *last_tx, int64_t now_ms,
                            js8_inbox_t *inbox, js8_stored_t *stored, js8_auto_result_t *out) {
    if (out) *out = js8_auto_result_t{};
    if (stored) *stored = js8_stored_t{};
    if (!a || !msg || !s || msg->tx || msg->partial) return;

    AutoSettings settings;
    settings.autoreply = s->autoreply;
    settings.heartbeat = s->heartbeat;
    settings.hb_ack    = s->hb_ack;
    settings.relay     = s->relay;
    settings.my_call   = s->my_call ? s->my_call : "";
    settings.my_grid   = s->my_grid ? s->my_grid : "";
    settings.info      = s->info ? s->info : "";
    settings.status    = s->status ? s->status : "";
    settings.groups    = group_list(s->groups);
    settings.held      = s->held ? &s->held->held : nullptr;

    Incoming in = to_incoming(msg);
    in.when_ms  = now_ms;
    auto         p = process(in, settings, heard_list(heard, n_heard), last_tx ? last_tx : "");
    js8_stored_t kept{};
    keep(p.store, now_ms, inbox, s->held, &kept);
    if (stored) *stored = kept;

    if (!p.reply || !out) return;
    auto &r = *p.reply;
    // An ACK says "I have it": not for a message the card didn't take, so
    // their station knows it didn't arrive and can send it again.
    if (r.kind == ReplyKind::MsgAck && kept.kind != JS8_STORED_NONE && kept.id < 0) return;
    auto  act   = a->policy.decide(r, settings, now_ms);
    out->action = act == AutoPolicy::Action::Send    ? JS8_AUTO_SEND
                  : act == AutoPolicy::Action::Offer ? JS8_AUTO_OFFER
                                                     : JS8_AUTO_IGNORE;
    out->kind    = c_kind(r.kind);
    out->hb_ack    = r.kind == ReplyKind::HeartbeatAck;
    out->allcall   = r.allcall;
    out->auto_only = r.auto_only;
    copy_str(out->text, sizeof(out->text), r.text);
    copy_str(out->to, sizeof(out->to), r.to);
    copy_str(out->command, sizeof(out->command), r.command);
    out->deliver_id = r.deliver_id;
    copy_str(out->deliver_group_call, sizeof(out->deliver_group_call), r.deliver_group_call);
}

extern "C" js8_auto_action_t js8_auto_decide(js8_auto_t *a, const js8_auto_result_t *r,
                                             const js8_auto_settings_t *s, int64_t now_ms) {
    if (!a || !r || !s) return JS8_AUTO_IGNORE;
    AutoReply reply{r->text, r->to, r->command, cpp_kind(r->kind)};
    reply.allcall   = r->allcall;
    reply.auto_only = r->auto_only;
    AutoSettings settings;
    settings.autoreply = s->autoreply;
    settings.heartbeat = s->heartbeat;
    settings.hb_ack    = s->hb_ack;
    auto act           = a->policy.decide(reply, settings, now_ms);
    return act == AutoPolicy::Action::Send    ? JS8_AUTO_SEND
           : act == AutoPolicy::Action::Offer ? JS8_AUTO_OFFER
                                              : JS8_AUTO_IGNORE;
}

extern "C" void js8_auto_sent(js8_auto_t *a, const js8_auto_result_t *r, int64_t now_ms) {
    if (!a || !r) return;
    AutoReply reply{r->text, r->to, r->command, cpp_kind(r->kind)};
    reply.allcall = r->allcall;
    a->policy.sent(reply, now_ms);
}

extern "C" void js8_auto_user_activity(js8_auto_t *a, int64_t now_ms) {
    if (a) a->policy.user_activity(now_ms);
}

extern "C" bool js8_auto_idle(js8_auto_t *a, int64_t now_ms) {
    return a && a->policy.idle(now_ms);
}

extern "C" int64_t js8_auto_last_activity(js8_auto_t *a) {
    return a ? a->policy.last_user_ms() : 0;
}

extern "C" bool js8_starts_qso(const js8_rx_msg_t *msg) {
    return msg && !msg->tx && starts_qso(to_incoming(msg));
}

extern "C" int64_t js8_next_heartbeat_ms(int64_t now_ms, int interval_min, int period_s) {
    return next_heartbeat_ms(now_ms, interval_min, (int64_t)period_s * 1000);
}

extern "C" int64_t js8_following_heartbeat_ms(int64_t scheduled_ms, int64_t now_ms, int interval_min) {
    return following_heartbeat_ms(scheduled_ms, now_ms, interval_min);
}

extern "C" bool js8_latlon_to_grid(double lat, double lon, int chars, char *out, unsigned size) {
    if (!out || chars < 2 || chars > 10 || chars % 2 || size < (unsigned)chars + 1) return false;
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return false;
    // Each pair subdivides the last: fields 18, squares 10, subsquares 24,
    // then 10 and 24 again. Work in fractions of the whole range.
    static const int base[5] = {18, 10, 24, 10, 24};
    double x = (lon + 180.0) / 360.0, y = (lat + 90.0) / 180.0;
    for (int i = 0; i < chars / 2; i++) {
        x *= base[i];
        y *= base[i];
        int xi = std::min((int)x, base[i] - 1), yi = std::min((int)y, base[i] - 1); // lat 90 / lon 180
        char first = (i % 2) ? '0' : 'A';
        out[2 * i]     = (char)(first + xi);
        out[2 * i + 1] = (char)(first + yi);
        x -= xi;
        y -= yi;
    }
    out[chars] = '\0';
    return true;
}

// ---- QSO log ------------------------------------------------------------

struct js8_qsos {
    QsoTracker tracker;
};

extern "C" js8_qsos_t *js8_qsos_create(void) {
    return new (std::nothrow) js8_qsos;
}

extern "C" void js8_qsos_destroy(js8_qsos_t *q) {
    delete q;
}

static bool offer(const std::optional<std::string> &call, char *ended, unsigned ended_len) {
    if (!call) return false;
    copy_str(ended, ended_len, *call);
    return true;
}

extern "C" bool js8_qsos_received(js8_qsos_t *q, const js8_rx_msg_t *msg, const char *my_call, int64_t now_ms,
                                  char *ended, unsigned ended_len) {
    if (!q || !msg || msg->tx || msg->low_confidence || !my_call) return false;
    return offer(q->tracker.received(msg->from, msg->text, msg->to_me, msg->snr, my_call, now_ms), ended, ended_len);
}

extern "C" bool js8_qsos_sent(js8_qsos_t *q, const char *text, const char *my_call, int64_t now_ms, char *ended,
                              unsigned ended_len) {
    if (!q || !text || !my_call) return false;
    return offer(q->tracker.sent(text, my_call, now_ms), ended, ended_len);
}

extern "C" bool js8_qsos_get(js8_qsos_t *q, const char *call, int64_t now_ms, js8_qso_t *out) {
    if (!q || !call || !out) return false;
    auto qso = q->tracker.get(call, now_ms);
    if (!qso) return false;
    std::memset(out, 0, sizeof(*out));
    copy_str(out->call, sizeof(out->call), qso->call);
    copy_str(out->grid, sizeof(out->grid), qso->grid);
    out->start_ms      = qso->start_ms;
    out->has_sent_snr  = qso->sent_snr.has_value();
    out->sent_snr      = (int16_t)qso->sent_snr.value_or(0);
    out->has_rcvd_snr  = qso->rcvd_snr.has_value();
    out->rcvd_snr      = (int16_t)qso->rcvd_snr.value_or(0);
    out->has_heard_snr = qso->heard_snr.has_value();
    out->heard_snr     = (int16_t)qso->heard_snr.value_or(0);
    out->two_way       = qso->we_sent && qso->they_sent;
    return true;
}

extern "C" void js8_qsos_logged(js8_qsos_t *q, const char *call) {
    if (q && call) q->tracker.logged(call);
}

extern "C" void js8_qsos_clear(js8_qsos_t *q) {
    if (q) q->tracker.clear();
}

extern "C" bool js8_log_append(const char *path, const js8_log_entry_t *e, char *err, unsigned err_len) {
    if (!path || !e) return false;
    LogEntry le;
    le.call     = e->call;
    le.grid     = e->grid;
    le.name     = e->name;
    le.comment  = e->comment;
    le.rst_sent = e->rst_sent;
    le.rst_rcvd = e->rst_rcvd;
    le.on_ms    = e->on_ms;
    le.off_ms   = e->off_ms;
    le.freq_hz  = e->freq_hz;
    le.my_call  = e->my_call;
    le.op_call  = e->op_call[0] ? e->op_call : e->my_call; /* desktop: the operator, else the station */
    le.my_grid  = e->my_grid;
    le.tx_pwr_w = e->tx_pwr_w;
    le.pota_ref = e->pota_ref;
    le.sota_ref = e->sota_ref;
    std::string msg;
    if (adif_append(path, le, msg)) return true;
    copy_str(err, err_len, msg);
    return false;
}

extern "C" bool js8_operator_call_valid(const char *typed, char *out, unsigned out_len) {
    std::string c;
    for (const char *p = typed ? typed : ""; *p; p++)
        if (*p != ' ') c += (char)std::toupper((unsigned char)*p);
    bool digit = false, ok = c.size() >= 3 && c.size() < JS8_RX_CALL_LEN;
    for (char ch : c) {
        digit |= std::isdigit((unsigned char)ch) != 0;
        ok &= std::isalnum((unsigned char)ch) || ch == '/';
    }
    if (!ok || !digit) return false;
    copy_str(out, out_len, c);
    return true;
}

extern "C" const char *js8_log_band(uint64_t freq_hz) {
    static thread_local std::string band;
    band = adif_band(freq_hz);
    return band.c_str();
}

// ---- Inbox --------------------------------------------------------------

static void fill_msg(const InboxMessage &m, js8_inbox_msg_t *out) {
    std::memset(out, 0, sizeof(*out));
    out->id     = m.id;
    out->utc_ms = m.utc_ms;
    copy_str(out->from, sizeof(out->from), m.from);
    copy_str(out->to, sizeof(out->to), m.to);
    copy_str(out->path, sizeof(out->path), m.path);
    copy_str(out->text, sizeof(out->text), m.text);
    out->read = m.read;
}

extern "C" js8_inbox_t *js8_inbox_open(const char *path) {
    auto *b = new (std::nothrow) js8_inbox;
    if (!b) return nullptr;
    b->path = path ? path : "";
    b->box.load(b->path); // an unreadable file is moved aside (or never written): notice()
    return b;
}

extern "C" const char *js8_inbox_notice(js8_inbox_t *b) {
    return b ? b->box.notice().c_str() : "";
}

extern "C" void js8_inbox_close(js8_inbox_t *b) {
    delete b;
}

extern "C" int js8_inbox_add(js8_inbox_t *b, const char *from, const char *text, int64_t utc_ms) {
    if (!b || !from || !text) return -1;
    int id = b->box.add(from, text, utc_ms);
    return sync(b) ? id : -1;
}

extern "C" int js8_inbox_list(js8_inbox_t *b, js8_inbox_msg_t *out, int max) {
    if (!b || !out) return 0;
    int n = 0;
    for (auto &m : b->box.list()) {
        if (n >= max) break;
        fill_msg(m, &out[n++]);
    }
    return n;
}

extern "C" bool js8_inbox_get(js8_inbox_t *b, int id, js8_inbox_msg_t *out) {
    if (!b || !out) return false;
    auto m = b->box.get(id);
    if (!m) return false;
    fill_msg(*m, out);
    return true;
}

extern "C" void js8_inbox_mark_read(js8_inbox_t *b, int id) {
    if (b && b->box.mark_read(id)) sync(b);
}

extern "C" void js8_inbox_delete(js8_inbox_t *b, int id) {
    if (b && b->box.remove(id)) sync(b);
}

extern "C" int js8_inbox_unread(js8_inbox_t *b) {
    return b ? b->box.unread() : 0;
}

extern "C" int js8_inbox_count(js8_inbox_t *b) {
    return b ? (int)b->box.size() : 0;
}

extern "C" bool js8_msg_for_me(const js8_rx_msg_t *msg, const char *my_call, char *out, unsigned out_len) {
    if (!msg || msg->tx || msg->checksum != 1 || !my_call) return false;
    auto body = msg_body(msg->text, my_call);
    if (!body) return false;
    copy_str(out, out_len, *body);
    return true;
}

// ---- Alerts -------------------------------------------------------------

extern "C" void js8_alert_words_normalise(const char *typed, char *out, unsigned out_len) {
    copy_str(out, out_len, format_alert_words(parse_alert_words(typed ? typed : "")));
}

extern "C" bool js8_alert_hit(const char *text, const char *from, const char *words, char *hit, unsigned hit_len) {
    if (!text || !words || !words[0]) return false;
    auto w = alert_word_hit(text, from ? from : "", parse_alert_words(words));
    if (w.empty()) return false;
    copy_str(hit, hit_len, w);
    return true;
}

// ---- Held messages ------------------------------------------------------

static void fill_held(const HeldMessage &m, js8_held_msg_t *out) {
    std::memset(out, 0, sizeof(*out));
    out->id        = m.id;
    out->utc_ms    = m.utc_ms;
    out->delivered = m.delivered;
    copy_str(out->from, sizeof(out->from), m.from);
    copy_str(out->to, sizeof(out->to), m.to);
    copy_str(out->path, sizeof(out->path), m.path);
    copy_str(out->text, sizeof(out->text), m.text);
    out->group = m.is_group();
    out->got   = (int)m.got.size();
}

extern "C" js8_held_t *js8_held_open(const char *path) {
    auto *h = new (std::nothrow) js8_held;
    if (!h) return nullptr;
    h->path = path ? path : "";
    h->held.load(h->path); // as js8_inbox_open()
    return h;
}

extern "C" const char *js8_held_notice(js8_held_t *h) {
    return h ? h->held.notice().c_str() : "";
}

extern "C" void js8_held_close(js8_held_t *h) {
    delete h;
}

extern "C" int js8_held_add(js8_held_t *h, const char *from, const char *to, const char *text, int64_t utc_ms) {
    if (!h || !from || !to || !text) return -1;
    int id = h->held.add(from, to, text, utc_ms);
    return sync(h) ? id : -1;
}

extern "C" int js8_held_list(js8_held_t *h, js8_held_msg_t *out, int max) {
    if (!h || !out) return 0;
    int n = 0;
    for (auto &m : h->held.list()) {
        if (n >= max) break;
        fill_held(m, &out[n++]);
    }
    return n;
}

extern "C" bool js8_held_get(js8_held_t *h, int id, js8_held_msg_t *out) {
    if (!h || !out) return false;
    auto m = h->held.get(id);
    if (!m) return false;
    fill_held(*m, out);
    return true;
}

extern "C" void js8_held_delivered(js8_held_t *h, int id) {
    if (h && h->held.mark_delivered(id)) sync(h);
}

extern "C" void js8_held_group_delivered(js8_held_t *h, int id, const char *call) {
    if (h && call && h->held.mark_group_delivered(id, call)) sync(h);
}

extern "C" bool js8_held_push_due(js8_held_t *h, const js8_heard_t *heard, unsigned n_heard, int64_t now_ms,
                                  char *text, unsigned text_len, int *id) {
    if (!h) return false;
    auto due = h->held.push_due(heard_list(heard, n_heard), now_ms);
    if (!due) return false;
    copy_str(text, text_len, due->second);
    if (id) *id = due->first;
    return true;
}

extern "C" void js8_held_push_sent(js8_held_t *h, int id, int64_t now_ms) {
    if (h && h->held.notified(id, now_ms)) sync(h);
}

extern "C" void js8_held_delete(js8_held_t *h, int id) {
    if (h && h->held.remove(id)) sync(h);
}

extern "C" int js8_held_waiting(js8_held_t *h) {
    return h ? h->held.waiting() : 0;
}

extern "C" int js8_held_count(js8_held_t *h) {
    return h ? (int)h->held.size() : 0;
}

extern "C" bool js8_msg_to_for_me(const js8_rx_msg_t *msg, const char *my_call, char *to, unsigned to_len, char *text,
                                  unsigned text_len) {
    if (!msg || msg->tx || msg->checksum != 1 || !my_call) return false;
    auto r = msg_to_body(msg->text, my_call);
    if (!r) return false;
    copy_str(to, to_len, r->first);
    copy_str(text, text_len, r->second);
    return true;
}

// ---- Paths, signatures, groups -----------------------------------------------

extern "C" void js8_path_display(const char *path, char *out, unsigned out_len) {
    copy_str(out, out_len, path_display(path ? path : ""));
}

extern "C" bool js8_delivered_signature(const char *text, char *from, unsigned from_len, int *next_id) {
    auto sig = delivered_signature(text ? text : "");
    if (!sig) return false;
    copy_str(from, from_len, sig->from);
    if (next_id) *next_id = sig->next_id;
    return true;
}

extern "C" void js8_groups_normalise(const char *typed, char *out, unsigned out_len) {
    std::vector<std::string> groups;
    for (auto g : group_list(typed)) {
        if (g[0] != '@') g = "@" + g;
        if (g.size() < 2 || std::find(groups.begin(), groups.end(), g) != groups.end()) continue;
        if (groups.size() >= 10) break;
        groups.push_back(g);
    }
    // Whole groups only: one cut short would match nothing (or another group).
    std::string joined;
    for (auto &g : groups) {
        std::string next = joined + (joined.empty() ? "" : " ") + g;
        if (next.size() + 1 > out_len) break;
        joined = next;
    }
    copy_str(out, out_len, joined);
}

// ---- Data files (js8_texts.txt) --------------------------------------------

extern "C" bool js8_file_read(const char *path, char *buf, unsigned len, char *notice, unsigned notice_len) {
    if (buf && len) buf[0] = '\0';
    if (notice && notice_len) notice[0] = '\0';
    if (!path) return false;
    auto file = read_data_file(path);
    copy_str(buf, len, file.text);
    copy_str(notice, notice_len, file.notice);
    return file.writable;
}

extern "C" bool js8_file_write(const char *path, const char *text) {
    return path && text && write_data_file(path, text);
}
