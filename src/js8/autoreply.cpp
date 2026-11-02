/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 auto-reply, heartbeat acks and heartbeat timing
 */

#include "autoreply.hpp"
#include "strutil.hpp"

#include "classify.hpp"
#include "commands.hpp"
#include "directed.hpp"
#include "inbox.hpp"

#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace x6100::js8 {

namespace {

std::string join(const std::vector<std::string> &v, const char *sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); i++) out += (i ? sep : "") + v[i];
    return out;
}

// Desktop's since(): how long ago, as QUERY CALL answers it.
std::string since(std::int64_t heard_ms, std::int64_t now_ms) {
    std::int64_t d = (now_ms - heard_ms) / 1000;
    if (d >= 24 * 3600) return std::to_string(d / (24 * 3600)) + "d";
    if (d >= 3600) return std::to_string(d / 3600) + "h";
    if (d >= 60) return std::to_string(d / 60) + "m";
    if (d >= 15) return std::to_string(d - d % 15) + "s";
    return "now";
}

// "MSG ID 3", with desktop's " +2" when more are waiting after it.
std::string msg_id_text(int id, int more) {
    return "MSG ID " + std::to_string(id) + (more > 0 ? " +" + std::to_string(more) : "");
}

// Desktop's isGroupCallIncluded(), plus @APRSIS as it treats that as one.
bool is_group_call(const std::string &to, const AutoSettings &s) {
    if (to == "@APRSIS") return true;
    return std::find(s.groups.begin(), s.groups.end(), to) != s.groups.end();
}

struct Ctx {
    const Incoming           &in;
    const AutoSettings       &s;
    const std::vector<Heard> &heard;
    const std::string        &last_tx;
    bool                      to_me, allcall, group;
};

AutoReply make(const Ctx &c, std::string text, std::string command, ReplyKind kind) {
    AutoReply r{std::move(text), c.in.from, std::move(command), kind};
    r.allcall = c.allcall;
    return r;
}

Processed with(std::optional<AutoReply> r) {
    Processed p;
    p.reply = std::move(r);
    return p;
}

// Desktop's handling of the "@APRSIS MSG TO:CALL text DE SENDER" an APRS
// gateway sends: for our call, into the inbox from "APRS"; never ACKed.
Processed aprs_to_inbox(const Ctx &c, const std::string &dest, const std::string &text) {
    Processed p;
    if (dest != upper(trim(c.s.my_call))) return p;
    // The gateway's receipt for a message we sent with an id ("{04}"):
    // "ACK04} DE SMS" (or REJ, or APRS 1.1's "ACK04}AB"), not a message.
    static const std::regex receipt_re("^(ACK|REJ)([A-Z0-9]{1,5})(\\}[A-Z0-9]{0,5})?(?:\\s+DE\\s+(\\S+))?$");
    std::smatch             m;
    std::string             t = upper(trim(text));
    if (std::regex_match(t, m, receipt_re)) {
        p.store = {StoreAction::Kind::AprsReceipt, m[4].matched ? m.str(4) : "APRS", c.s.my_call, m.str(1), m.str(2)};
        return p;
    }
    p.store = {StoreAction::Kind::Inbox, "APRS", c.s.my_call, "APRS", trim(text)};
    return p;
}

// One command, as processCommandActivity() takes it. `relay_path` is set
// ("K2XYZ>N0XYZ") for a command that came inside a relay ending here.
Processed handle(const Directed &d, const std::string &relay_path, const Ctx &c) {
    namespace vc = js8core::protocol::varicode;
    const std::string &cmd = d.cmd;

    // Desktop answers relayed questions along the path; MSG and QUERY
    // commands work out the asker (the path's last station) themselves.
    std::string reply_to = d.from;
    if (!relay_path.empty() && vc::is_command_autoreply(cmd) && cmd.rfind(" MSG", 0) != 0 &&
        cmd.rfind(" QUERY", 0) != 0)
        reply_to = relay_path;
    std::string who = d.from, back = d.from;
    if (relay_path.find('>') != std::string::npos) {
        who  = relay_path.substr(relay_path.rfind('>') + 1);
        back = relay_path;
    }

    if ((cmd == " SNR?") && !c.allcall) {
        auto text = query_text(Query::SendSnr, reply_to, c.in.snr, "");
        if (text.empty()) return {};
        return with(make(c, text, "SNR?", ReplyKind::Query));
    }
    if (cmd == " INFO?" && !c.allcall) {
        if (trim(c.s.info).empty()) return {};
        return with(make(c, reply_to + " INFO " + upper(trim(c.s.info)), "INFO?", ReplyKind::Query));
    }
    if (cmd == " STATUS?" && !c.allcall) {
        if (trim(c.s.status).empty()) return {};
        return with(make(c, reply_to + " STATUS " + upper(trim(c.s.status)), "STATUS?", ReplyKind::Query));
    }
    if (cmd == " GRID?" && !c.allcall) {
        auto text = query_text(Query::MyGrid, reply_to, 0, c.s.my_grid);
        if (text.empty()) return {};
        return with(make(c, text, "GRID?", ReplyKind::Query));
    }
    if (cmd == " HEARING?" && !c.allcall) {
        // Up to 4 recently heard stations other than the one asking.
        std::string list;
        int         n = 0;
        for (auto &h : c.heard) {
            if (n >= 4) break;
            if (base_callsign(h.call) == base_callsign(who)) continue;
            if (base_callsign(h.call) == base_callsign(c.s.my_call)) continue;
            list += " " + h.call;
            n++;
        }
        if (n == 0) return {};
        return with(make(c, reply_to + " HEARING" + list, "HEARING?", ReplyKind::Query));
    }

    // A relay: on to the next station while the text starts with a
    // callsign, else it ends here: ACK back along the path, or answer the
    // command it carries (instead of the ACK), as desktop does.
    if (cmd == ">" && !c.allcall) {
        if (!c.s.relay) return {};
        if (auto next = relay_next_hop(d.text); next && !c.group)
            return with(make(c, *next + " *DE* " + d.from, ">", ReplyKind::Relay));
        if (d.text.rfind("ACK", 0) == 0) return {};
        auto path = join(relay_path_calls(d.from, d.text), ">");
        if (auto inner = relayed_command(d.text)) {
            Directed rd = d;
            rd.cmd      = inner->first;
            rd.text     = inner->second;
            return handle(rd, path, c);
        }
        return with(make(c, path + " ACK", ">", ReplyKind::MsgAck));
    }

    // MSG TO:CALL text: held here for CALL (desktop's STORE), ACKed.
    if (cmd == " MSG TO:" && !c.allcall) {
        auto w = words(d.text);
        if (w.empty()) return {};
        std::string dest = w[0];
        std::string text = trim(join(std::vector<std::string>(w.begin() + 1, w.end()), " "));
        if (c.in.to == "@APRSIS") return aprs_to_inbox(c, dest, text);
        if (!c.s.relay) return {}; // desktop's relay switch covers holding too
        auto      calls = relay_path_calls(d.from, text);
        Processed p;
        p.store = {StoreAction::Kind::Held, d.from, base_callsign(dest), join(calls, ">"), text};
        p.reply = make(c, (calls.size() > 1 ? join(calls, ">") : d.from) + " ACK", "MSG TO:", ReplyKind::MsgAck);
        return p;
    }

    if (cmd == " AGN?" && !c.allcall && !c.group) {
        auto text = trim(c.last_tx);
        if (text.empty()) return {};
        return with(make(c, text, "AGN?", ReplyKind::Query));
    }

    // MSG: into the inbox (from our group too), ACKed along the path.
    if (cmd == " MSG" && !c.allcall) {
        if (c.in.to == "@APRSIS") {
            // Only a gateway's "MSG TO:CALL text DE SENDER"; desktop ignores
            // any other MSG to @APRSIS (it's for the gateway, not for us).
            static const std::regex to_re("^TO:\\s*(\\S+)\\s+(.*)$");
            std::smatch             m;
            if (std::regex_match(d.text, m, to_re)) return aprs_to_inbox(c, m.str(1), m.str(2));
            return {};
        }
        auto      calls = relay_path_calls(d.from, d.text);
        Processed p;
        p.store = {StoreAction::Kind::Inbox, d.from, c.in.to, join(calls, ">"), trim(d.text)};
        p.reply = make(c, (calls.size() > 1 ? join(calls, ">") : d.from) + " ACK", "MSG", ReplyKind::MsgAck);
        return p;
    }

    // QUERY MSG n: the held message, "NEXT MSG ID n [+k]" if more wait.
    if (cmd == " QUERY" && !c.allcall) {
        auto w = words(d.text);
        if (w.size() < 2 || w[0] != "MSG" || !c.s.held) return {};
        // A bare number as desktop sends it, or "[3]" / "[ID 3]" with
        // desktop's template brackets left in (we're kinder than desktop).
        auto parsed = msg_id_arg(join(std::vector<std::string>(w.begin() + 1, w.end()), " "));
        if (!parsed) return {};
        int  id = *parsed;
        auto m  = c.s.held->get(id);
        if (!m || trim(m->text).empty()) return {};
        // A group message is for anyone who asks; others only for their station.
        bool group_msg = m->is_group();
        if (!group_msg && m->to != who && m->to != base_callsign(who)) return {};
        std::int64_t now     = c.in.when_ms;
        int          pending = c.s.held->count_for(who);
        if (c.group) pending += c.s.held->count_group_for(c.in.to, who, now);
        pending -= 2; // not this one, nor the next we name
        auto next = c.s.held->lookahead_for(who, id);
        if (!next && group_msg) next = c.s.held->lookahead_group_for(c.in.to, who, id, now);
        std::string text = back + " MSG " + trim(m->text) + " FROM " + m->from;
        if (next) text += " NEXT " + msg_id_text(*next, pending);
        AutoReply r = make(c, text, "QUERY MSG " + std::to_string(id), ReplyKind::Stored);
        r.deliver_id = id;
        if (group_msg) r.deliver_group_call = who;
        return with(r);
    }

    // QUERY MSGS: "YES MSG ID n [+k]", or NO unless it was to everyone.
    if (cmd == " QUERY MSGS") {
        const HeldMessages *h     = c.s.held;
        std::int64_t        now   = c.in.when_ms;
        std::string         reply;
        if (h) {
            auto id      = h->next_for(who);
            int  pending = h->count_for(who) + (c.group ? h->count_group_for(c.in.to, d.from, now) : 0) - 1;
            if (!id && c.group) id = h->next_group_for(c.in.to, d.from, now);
            if (id) reply = back + " YES " + msg_id_text(*id, pending);
        }
        if (reply.empty() && !c.allcall) reply = back + " NO";
        if (reply.empty()) return {};
        AutoReply r = make(c, reply, "QUERY MSGS", ReplyKind::Stored);
        r.auto_only = true;
        return with(r);
    }

    // QUERY CALL W1ABC?: "YES -12 (5m)" if we've heard them; else nothing.
    // Only heard through a relay (SNR -64): no SNR, "YES (5m)", as desktop's
    // "%1 (%2)" with an empty formatSNR() comes out after trimmed().
    if (cmd == " QUERY CALL") {
        auto calls = parse_callsigns(d.text);
        if (calls.empty()) return {};
        const std::string &want = calls.front();
        for (auto &h : c.heard) {
            if (h.call != want && base_callsign(h.call) != want) continue;
            std::string snr = desktop_snr(h.snr);
            std::string ago = "(" + since(h.heard_ms, c.in.when_ms) + ")";
            AutoReply   r   = make(c, back + " YES " + (snr.empty() ? ago : snr + " " + ago), "QUERY CALL",
                                   ReplyKind::Stored);
            r.auto_only = true;
            return with(r);
        }
        return {};
    }

    // Our own help, never sent by itself: nothing on desktop answers these.
    if (!c.to_me || !relay_path.empty()) return {};
    if (cmd == " HW CPY?") {
        // "how do you copy?": offer how we hear them; desktop leaves the
        // answer to you.
        auto text = query_text(Query::SendSnr, d.from, c.in.snr, "");
        if (text.empty()) return {};
        return with(make(c, text, "HW CPY?", ReplyKind::Suggest));
    }
    // They hold a message for us ("YES MSG ID 3", a heartbeat ack's "MSG ID
    // 3", or "RETRIEVE MSG 3"): suggest fetching it, as desktop's popup does.
    if (cmd != " ACK") {
        if (auto id = msg_id_offered(cmd + " " + d.text))
            return with(make(c, upper(d.from) + " QUERY MSG " + std::to_string(*id), "MSG ID", ReplyKind::Suggest));
    }
    return {};
}

} // namespace

bool starts_qso(const Incoming &in) {
    if (!in.to_me) return false;
    return in.text.find(" HEARTBEAT SNR") == std::string::npos;
}

Processed process(const Incoming &in, const AutoSettings &s, const std::vector<Heard> &heard,
                  const std::string &last_tx) {
    if (s.my_call.empty() || in.from.empty() || in.low_confidence) return {};
    if (base_callsign(in.from) == base_callsign(s.my_call)) return {};

    Ctx c{in, s, heard, last_tx, in.to_me, is_allcall(in.to), is_group_call(in.to, s)};

    // Someone else's heartbeat: acknowledge with how we hear them, and tell
    // them about messages held here ("MSG ID 3 +1").
    if (in.heartbeat && !in.to_me && in.text.find(" HEARTBEAT SNR") == std::string::npos &&
        in.text.find("@HB HEARTBEAT") != std::string::npos) {
        auto text = query_text(Query::SendSnr, in.from, in.snr, "");
        if (text.empty()) return {};
        // Desktop: "%1 HEARTBEAT SNR %2 %3", i.e. "CALL HEARTBEAT SNR -08".
        text.replace(text.find(" SNR "), 5, " HEARTBEAT SNR ");
        if (s.held) {
            auto id      = s.held->next_for(in.from);
            int  pending = s.held->count_for(in.from) +
                          (c.group ? s.held->count_group_for(in.to, in.from, in.when_ms) : 0) - 1;
            if (!id && c.group) id = s.held->next_group_for(in.to, in.from, in.when_ms);
            if (id) text += " " + msg_id_text(*id, pending);
        }
        AutoReply r = make(c, text, "HEARTBEAT", ReplyKind::HeartbeatAck);
        r.allcall   = true;
        return with(r);
    }

    // Only what's to us, to everyone, or to one of our groups.
    if (!c.allcall && !c.to_me && !c.group) return {};

    auto d = parse_directed(in.text);
    if (!d) return {};
    // A buffered command with text counts only once its checksum checks out
    // (desktop sends an APRS gateway's @APRSIS MSG / MSG TO: without one).
    const bool aprs_unchecked = in.to == "@APRSIS" && (d->cmd == " MSG" || d->cmd == " MSG TO:");
    if (is_checksummed_command(d->cmd) && !d->text.empty() && !in.checksum_ok && !aprs_unchecked) return {};
    return handle(*d, "", c);
}

std::optional<AutoReply> build_reply(const Incoming &in, const AutoSettings &s, const std::vector<Heard> &heard,
                                     const std::string &last_tx) {
    return process(in, s, heard, last_tx).reply;
}

AutoPolicy::Action AutoPolicy::decide(const AutoReply &r, const AutoSettings &s, std::int64_t now_ms) {
    const bool auto_ok = s.autoreply && !idle(now_ms);

    if (r.kind == ReplyKind::Suggest) return Action::Offer;
    if (r.kind == ReplyKind::HeartbeatAck) {
        // Desktop: HB ACK only with AUTO and heartbeat networking both on.
        if (!(auto_ok && s.heartbeat && s.hb_ack)) return Action::Ignore;
    } else if (!auto_ok) {
        // Desktop fills its outgoing box with the answer; not for @ALLCALL,
        // QUERY MSGS or QUERY CALL, which it answers only with AUTO on.
        return r.auto_only || r.allcall ? Action::Ignore : Action::Offer;
    }
    // Desktop's @ALLCALL cooldown: one answer per station per 55 min to
    // anything sent to everyone, heartbeats included.
    if (r.allcall || r.kind == ReplyKind::HeartbeatAck) {
        auto it = last_sent_.find("@ALLCALL|" + r.to);
        return it != last_sent_.end() && now_ms - it->second < HB_ACK_REPEAT_MS ? Action::Ignore : Action::Send;
    }
    // Everything else every time, as on desktop: a question asked again
    // gets its answer again, a resend its ACK, a relay or a held message
    // asked for again (it didn't get there) goes again.
    return Action::Send;
}

void AutoPolicy::sent(const AutoReply &r, std::int64_t now_ms) {
    // Forget what's past the cooldown: a station left running for days
    // would otherwise keep every station it ever answered.
    for (auto it = last_sent_.begin(); it != last_sent_.end();)
        it = now_ms - it->second >= HB_ACK_REPEAT_MS ? last_sent_.erase(it) : std::next(it);
    if (r.allcall || r.kind == ReplyKind::HeartbeatAck) last_sent_["@ALLCALL|" + r.to] = now_ms;
}

namespace {
std::int64_t interval_ms(int interval_min) {
    if (interval_min < HB_MIN_INTERVAL) interval_min = HB_MIN_INTERVAL;
    if (interval_min > HB_MAX_INTERVAL) interval_min = HB_MAX_INTERVAL;
    return (std::int64_t)interval_min * 60'000;
}
} // namespace

// Desktop's TxLoop::onTxLoopPeriodChangeStart(): now + the period, rounded
// up to the speed's slot.
std::int64_t next_heartbeat_ms(std::int64_t now_ms, int interval_min, std::int64_t period_ms) {
    if (period_ms <= 0) period_ms = 15'000;
    std::int64_t earliest = now_ms + interval_ms(interval_min);
    std::int64_t rem      = earliest % period_ms;
    return rem ? earliest + period_ms - rem : earliest;
}

// TxLoop::onTimer(): the last one + the period (whole minutes, so still on
// the slot grid), pushed on by whole periods while it's already past.
std::int64_t following_heartbeat_ms(std::int64_t scheduled_ms, std::int64_t now_ms, int interval_min) {
    std::int64_t step = interval_ms(interval_min);
    std::int64_t next = scheduled_ms + step;
    if (next <= now_ms) next += ((now_ms - next) / step + 1) * step;
    return next;
}

} // namespace x6100::js8
