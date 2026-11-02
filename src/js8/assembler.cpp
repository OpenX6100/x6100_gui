/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Copyright (c) the JS8Call-improved authors (ported from their Android app)
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 *
 *  Ported from DecodeViewModel.kt in JS8Call-improved/Android-port.
 */

#include "assembler.hpp"
#include "strutil.hpp"

#include "render.hpp"
#include "speeds.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>

namespace x6100::js8 {

namespace {

// Data frames split mid-word and carry their own spaces. A directed header
// can meet its payload flush: a buffered command strips the separator before
// packing, so that boundary needs the space put back.
bool needs_space_before(const RxFrame &prev, const RxFrame &next) {
    return !prev.is_data() && !std::isspace((unsigned char)prev.text.back()) &&
           !std::isspace((unsigned char)next.text.front());
}

const std::regex &grid_re() {
    static const std::regex re("^[A-R]{2}[0-9]{2}([A-X]{2})?$");
    return re;
}

const std::regex &directed_command_tail_re() {
    static const std::regex re(
        "^(?:AGN\\?|QSL\\?|HW CPY\\?|MSG TO:|SNR\\?|INFO\\?|GRID\\?|STATUS\\?|QUERY MSGS\\?|HEARING\\?|STATUS|HEARING|"
        "QUERY CALL|QUERY MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT|>|\\?)"
        "(?:\\s+[+-]?\\d{1,3})?$");
    return re;
}

const std::regex &directed_placeholder_re() {
    static const std::regex re("^<\\.{4}>:\\s*(.+)$");
    return re;
}

// Kotlin isCallsignLike(): 3-12 chars of [A-Z0-9/] with a letter and a digit.
bool callsign_like_strict(const std::string &token) {
    auto u = upper(trim(token));
    if (u.size() < 3 || u.size() > 12) return false;
    bool letter = false, digit = false;
    for (char c : u) {
        if (std::isalpha((unsigned char)c)) letter = true;
        else if (std::isdigit((unsigned char)c)) digit = true;
        else if (c != '/') return false;
    }
    return letter && digit;
}

bool is_compound_de_helper_frame(const std::string &text) {
    auto parts = words(text);
    if (parts.size() != 2) return false;
    return callsign_like_strict(parts[0]) && std::regex_match(upper(parts[1]), grid_re());
}

bool is_directed_compound_header(const std::string &text) {
    auto tokens = words(text);
    if (tokens.empty() || !callsign_like_strict(tokens[0])) return false;
    if (tokens.size() == 1) return true;
    std::string tail;
    for (std::size_t i = 1; i < tokens.size(); ++i) tail += (i > 1 ? " " : "") + tokens[i];
    return std::regex_match(upper(tail), directed_command_tail_re());
}

// A compound sender's directed message arrives as a "CALL GRID" helper frame
// followed by a "<....>: ..." placeholder frame; fold them into "CALL: ...".
void normalize_compound_directed_helpers(std::vector<RxFrame> &frames) {
    if (frames.size() < 2) return;
    auto &f0 = frames[0];
    auto &f1 = frames[1];
    if (f0.is_data() || f1.is_data()) return;
    if (!f0.is_first() || f0.is_last()) return;
    if (f1.is_first()) return;

    auto first  = trim(f0.text);
    auto second = trim(f1.text);
    if (!is_compound_de_helper_frame(first)) return;

    auto from_call = trim(first.substr(0, first.find(' ')));
    if (from_call.empty()) return;

    std::smatch m;
    if (std::regex_match(second, m, directed_placeholder_re())) {
        auto tail = trim(m[1].str());
        if (!tail.empty()) {
            f0.text = "";
            f1.text = from_call + ": " + tail;
            return;
        }
    }
    if (is_directed_compound_header(second)) {
        f0.text = "";
        f1.text = from_call + ": " + second;
        return;
    }
    // Our renderer already attributes a compound-directed frame to the
    // compound call it heard at this offset ("EA8/K1ABC: G4XYZ"); the helper
    // frame is then redundant. (Not in the Android port, whose output here
    // repeats the sender.)
    if (second.rfind(from_call + ":", 0) == 0) f0.text = "";
}

} // namespace

namespace {
// Message ids for the whole run, not per receiver: the app's message list
// outlives a receiver (JS8 closed and reopened, a retune), and a new
// receiver's message 1 used to update an old message's row.
std::atomic<std::uint32_t> g_next_id{1};

std::uint32_t next_message_id() {
    std::uint32_t id = g_next_id++;
    return id ? id : g_next_id++; // 0 means "none"
}
} // namespace

std::string assemble_multipart_text(const std::vector<RxFrame> &frames) {
    std::vector<const RxFrame *> parts;
    for (auto &f : frames)
        if (!f.text.empty()) parts.push_back(&f);

    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0 && needs_space_before(*parts[i - 1], *parts[i])) out += ' ';
        out += parts[i]->text;
    }
    return out;
}

RxFrame MessageAssembler::assemble(const Buffer &buffer) const {
    auto frames = buffer.frames;
    normalize_compound_directed_helpers(frames);

    // Metadata comes from the most recent frame.
    RxFrame msg = buffer.frames.back();
    msg.text    = assemble_multipart_text(frames);
    msg.msg_id  = buffer.id;
    msg.partial = false;
    return msg;
}

void MessageAssembler::show_partial(const Buffer &buffer) const {
    if (!partial_) return;
    RxFrame msg = assemble(buffer);
    msg.partial = true;
    partial_(msg);
}

MessageAssembler::Key MessageAssembler::key_for(const RxFrame &frame) {
    return {frame.mode, (int)std::lround(frame.freq_hz)};
}

std::optional<MessageAssembler::Key> MessageAssembler::find_key(const RxFrame &frame) const {
    const float tolerance = (float)speed_from_varicode(frame.mode).rx_threshold_hz;
    for (auto &[key, buf] : buffers_) {
        if (key.first == frame.mode && std::fabs(frame.freq_hz - (float)key.second) <= tolerance) return key;
    }
    return std::nullopt;
}

void MessageAssembler::add(const RxFrame &frame) {
    flush_stale(frame.timestamp_ms);

    auto match = find_key(frame);

    // Complete single-frame message.
    if (frame.is_first() && frame.is_last()) {
        if (match) buffers_.erase(*match);
        RxFrame msg = frame;
        msg.msg_id  = next_message_id();
        msg.partial = false;
        emit_(msg);
        return;
    }

    // First frame: start a fresh buffer at this offset.
    if (frame.is_first()) {
        if (match) buffers_.erase(*match);
        auto &buf = buffers_[key_for(frame)] = Buffer{{frame}, frame.timestamp_ms, next_message_id()};
        show_partial(buf);
        return;
    }

    // Middle or last frame of a buffer we're tracking.
    if (match) {
        auto key = *match;
        auto &buf = buffers_[key];
        buf.frames.push_back(frame);
        buf.last_timestamp_ms = frame.timestamp_ms;
        if (frame.is_last()) {
            emit_(assemble(buf));
            buffers_.erase(key);
        } else {
            show_partial(buf);
        }
        return;
    }

    // No buffer: probably a missed first frame. Start one anyway.
    Buffer buf{{frame}, frame.timestamp_ms, next_message_id()};
    if (frame.is_last()) {
        emit_(assemble(buf));
        return;
    }
    show_partial(buf);
    buffers_[key_for(frame)] = std::move(buf);
}

bool DuplicateFilter::seen(int mode, const std::string &frame, float freq_hz, std::int64_t now_ms) {
    const Speed &sp = speed_from_varicode(mode);
    // Forget what's older than the longest slot.
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [&](const Entry &e) { return now_ms - e.at_ms >= 30'000; }),
                  recent_.end());
    for (auto &e : recent_) {
        if (e.mode == mode && e.frame == frame && std::fabs(e.freq_hz - freq_hz) <= sp.rx_threshold_hz &&
            now_ms - e.at_ms < sp.period_ms() - 1000) // the next slot's decode can come a little early
            return true;
    }
    recent_.push_back({mode, frame, freq_hz, now_ms});
    return false;
}

void MessageAssembler::flush_stale(std::int64_t now_ms) {
    for (auto it = buffers_.begin(); it != buffers_.end();) {
        if (now_ms - it->second.last_timestamp_ms > IDLE_TIMEOUT_MS) {
            emit_(assemble(it->second));
            it = buffers_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace x6100::js8
