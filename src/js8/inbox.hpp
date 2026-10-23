/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 inbox
 *
 *  Desktop JS8Call keeps "CALL MSG text" messages sent to you in an inbox
 *  and answers them with "CALL ACK". Here the inbox is a plain text file on
 *  the SD card's DATA partition, one message per line, readable on a PC.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "directed.hpp"

namespace x6100::js8 {

struct InboxMessage {
    int          id     = 0;
    std::int64_t utc_ms = 0;
    std::string  from, text;
    std::string  to;   ///< our call, or one of our groups
    std::string  path; ///< desktop's PATH: the way back, "K2XYZ>N0XYZ" if relayed
    bool         read = false;
};

class Inbox {
public:
    static constexpr std::size_t  MAX_MESSAGES = 200;          ///< oldest go first
    static constexpr std::int64_t REPEAT_MS    = 30 * 60 * 1000; ///< same message again: a resend

    /// A missing file is an empty inbox. One that can't be read is moved
    /// aside (read_data_file) and the inbox starts empty; if it can't even be
    /// moved, nothing is ever saved over it: false, and save() refuses.
    /// notice() says what happened.
    bool load(const std::string &path);
    /// Write via a temporary file and rename, so a power cut keeps the old one.
    bool save(const std::string &path) const;
    /// What load() had to do with an unreadable file, for the screen; "" normally.
    const std::string &notice() const { return notice_; }

    /// Returns the id. The same text from the same station within REPEAT_MS
    /// is a resend (they didn't get our ACK): the existing id, not a copy,
    /// and `*added` false. `path` empty: the message came straight from `from`.
    int add(const std::string &from, const std::string &text, std::int64_t utc_ms, const std::string &to = "",
            const std::string &path = "", bool *added = nullptr);

    /// Newest first.
    std::vector<InboxMessage> list() const;
    std::optional<InboxMessage> get(int id) const;
    bool mark_read(int id);
    bool remove(int id);
    int  unread() const;
    std::size_t size() const { return msgs_.size(); }

private:
    std::vector<InboxMessage> msgs_; ///< oldest first
    int                       next_id_ = 1;
    bool                      writable_ = true;
    std::string               notice_;
};

/// A message held here for another station, as desktop JS8Call stores
/// "MSG TO:" messages ("STORE") until that station asks with QUERY MSGS /
/// QUERY MSG n (then "DELIVERED"). One per line in a text file, like the inbox.
/// A message for a group ("MSG TO:@GROUP") is never delivered as a whole:
/// any member can fetch it for two days, and `got` lists who has.
struct HeldMessage {
    int          id     = 0;
    std::int64_t utc_ms = 0;
    std::string  from, to, text; ///< `to` is a base call, as desktop stores it, or a @GROUP
    std::string  path;           ///< how it came: "N0XYZ", or "K2XYZ>N0XYZ" relayed
    bool         delivered = false;
    std::int64_t notified_ms = 0;   ///< last "RETRIEVE MSG" sent for it
    std::vector<std::string> got; ///< group message: the stations that fetched it

    bool is_group() const { return !to.empty() && to[0] == '@'; }
};

class HeldMessages {
public:
    static constexpr std::size_t  MAX_MESSAGES    = 100;
    static constexpr std::int64_t GROUP_WINDOW_MS = 2LL * 24 * 60 * 60 * 1000; ///< desktop: group messages, 2 days
    static constexpr std::int64_t PUSH_SEEN_MS    = 15 * 60 * 1000;          ///< heard this recently
    static constexpr std::int64_t PUSH_REPEAT_MS  = 8LL * 60 * 60 * 1000;    ///< once per 8 h per message

    /// As Inbox::load() / save() / notice().
    bool load(const std::string &path);
    bool save(const std::string &path) const;
    const std::string &notice() const { return notice_; }

    /// Hold `text` from `from` for `to` (stored as its base call). A resend
    /// within Inbox::REPEAT_MS returns the first one's id, `*added` false.
    int add(const std::string &from, const std::string &to, const std::string &text, std::int64_t utc_ms,
            const std::string &path = "", bool *added = nullptr);

    /// The oldest undelivered message for `call` (or its base call), as
    /// desktop's getNextMessageIdForCallsign().
    std::optional<int> next_for(const std::string &call) const;
    /// The next one after `id` (desktop's lookahead): "NEXT MSG ID n".
    std::optional<int> lookahead_for(const std::string &call, int id) const;
    /// Undelivered messages for `call`: desktop's countUnreadForCallsign().
    int count_for(const std::string &call) const;

    /// Group messages to `group` from the last two days that `call` hasn't
    /// fetched yet: the oldest, the next after `id`, how many.
    std::optional<int> next_group_for(const std::string &group, const std::string &call, std::int64_t now_ms) const;
    std::optional<int> lookahead_group_for(const std::string &group, const std::string &call, int id,
                                           std::int64_t now_ms) const;
    int count_group_for(const std::string &group, const std::string &call, std::int64_t now_ms) const;

    std::optional<HeldMessage> get(int id) const;
    /// Is message `id` for `call`? (desktop: TO equals the caller or its base)
    bool is_for(int id, const std::string &call) const;
    bool mark_delivered(int id);
    /// A group message fetched by `call`.
    bool mark_group_delivered(int id, const std::string &call);
    bool remove(int id);
    std::vector<HeldMessage> list() const; ///< newest first
    int  waiting() const;                  ///< not yet delivered
    std::size_t size() const { return msgs_.size(); }

    /// Desktop's pushNotificationHandler(): the first message whose station
    /// was heard in the last 15 minutes and hasn't been told in 8 hours, as
    /// "W1ABC RETRIEVE MSG 3"; marks it told. Not for group messages.
    std::optional<std::pair<int, std::string>> push_due(const std::vector<Heard> &heard, std::int64_t now_ms) const;
    /// "RETRIEVE MSG" for `id` went out: not again for PUSH_REPEAT_MS.
    bool notified(int id, std::int64_t now_ms);

private:
    std::vector<HeldMessage> msgs_; ///< oldest first
    int                      next_id_ = 1;
    bool                     writable_ = true;
    std::string              notice_;
};

/// "FROM: MYCALL MSG TO:W1ABC HELLO" (or "MSG TO: W1ABC HELLO") to my_call:
/// {"W1ABC", "HELLO"}.
std::optional<std::pair<std::string, std::string>> msg_to_body(const std::string &text, const std::string &my_call);

/// "FROM: MYCALL QUERY MSG 3": 3.
std::optional<int> query_msg_id(const std::string &text);

/// The number in what follows "QUERY MSG": "3", and as people type it from
/// desktop's "QUERY MSG [ID]" template with the brackets left in: "[3]",
/// "[ID3]", "[ID 3]", "ID 3". Desktop itself takes only a bare number.
std::optional<int> msg_id_arg(const std::string &arg);

/// The message in "FROM: MYCALL MSG HELLO THERE" when it's to my_call
/// (or its base call): "HELLO THERE". Not "MSG TO:" (stored for others).
std::optional<std::string> msg_body(const std::string &text, const std::string &my_call);

/// "N0XYZ: K2XYZ YES MSG ID 3", a heartbeat ack ending "MSG ID 3", or
/// "RETRIEVE MSG 3": they hold message 3 for us.
std::optional<int> msg_id_offered(const std::string &text);

/// A delivered message's signature, as desktop's inbox reads it: the
/// original sender in "... FROM N0XYZ" (then maybe "NEXT MSG ID 4 +2"),
/// and the next id if there is one.
struct Signature {
    std::string from;
    int         next_id = 0;
};
std::optional<Signature> delivered_signature(const std::string &text);

} // namespace x6100::js8
