/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Copyright (c) the JS8Call-improved authors (ported from their Android app)
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 *
 *  Ported from render_decoded_text() in JS8Call-improved/Android-port
 *  adapters/android/jni/js8_engine_jni.cpp.
 */

#include "render.hpp"

#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>

namespace x6100::js8 {

using namespace js8core::protocol::varicode;

bool is_callsign_like(const std::string &token) {
    if (token.size() < 3 || token.size() > 12) return false;
    if (token.front() == '@') return false;
    bool has_digit = false;
    for (char c : token) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isdigit(uc)) {
            has_digit = true;
            continue;
        }
        if (std::isalpha(uc) || c == '/') continue;
        return false;
    }
    return has_digit;
}

namespace {

std::string ltrim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
    return s;
}

// A first data frame that starts "CALL1 CALL2 ..." is shown "CALL1: CALL2 ...".
// Continuation frames are plain payload.
std::string maybe_insert_callsign_prefix(const std::string &text, bool first_frame) {
    if (!first_frame) return text;

    auto first_sep = std::find_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    if (first_sep == text.end()) return text;
    std::string first_token(text.begin(), first_sep);
    if (first_token.find(':') != std::string::npos) return text;

    std::vector<std::string> tokens;
    std::string              current;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
                if (tokens.size() >= 2) break;
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty() && tokens.size() < 2) tokens.push_back(current);
    if (tokens.size() < 2) return text;
    if (!is_callsign_like(tokens[0]) || !is_callsign_like(tokens[1])) return text;

    return tokens[0] + ": " + text.substr(first_sep - text.begin() + 1);
}

std::string build_compound(const std::vector<std::string> &parts) {
    std::string a = parts.size() > 0 ? parts[0] : "";
    std::string b = parts.size() > 1 ? parts[1] : "";
    if (!a.empty() && !b.empty()) return a + "/" + b;
    return a.empty() ? b : a;
}

const char *const CQ_STRINGS[] = {
    "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD", "CQ CQ", "CQ",
};

constexpr auto  COMPOUND_MEMORY     = std::chrono::seconds(60);
constexpr float COMPOUND_MATCH_HZ   = 10.0f;

} // namespace

std::string FrameRenderer::render(const std::string &frame, int *type, float freq_hz, Clock::time_point now) {
    if (frame.size() < 12 || frame.find(' ') != std::string::npos) return frame;

    const bool is_data_flag   = (*type & FRAME_DATA) != 0;
    const bool is_first_frame = (*type & FRAME_FIRST) != 0;

    // Data payloads first, as desktop JS8Call does.
    if (is_data_flag) {
        auto data = unpack_fast_data_message(frame);
        if (!data.empty()) return maybe_insert_callsign_prefix(data, is_first_frame);
        // Fast-data frames are never heartbeat/compound/directed.
        return frame;
    } else {
        auto data = unpack_data_message(frame);
        if (!data.empty()) {
            // Normal and Slow carry the data flag in the payload rather than
            // the frame type; normalise so callers see one convention.
            *type |= FRAME_DATA;
            return maybe_insert_callsign_prefix(data, is_first_frame);
        }
    }

    // Heartbeat / CQ
    {
        std::uint8_t hb_type  = 0;
        bool         hb_alt   = false;
        std::uint8_t hb_bits3 = 0;
        auto         parts    = unpack_heartbeat_message(frame, &hb_type, &hb_alt, &hb_bits3);
        if (!parts.empty()) {
            auto        callsign = build_compound(parts);
            std::string grid     = parts.size() > 2 ? parts[2] : std::string{};
            std::string text     = callsign;
            if (!text.empty()) text += ": ";
            if (hb_alt) {
                text += std::string("@ALLCALL ") + (hb_bits3 < 8 ? CQ_STRINGS[hb_bits3] : "CQ");
            } else {
                // Every heartbeat status value maps to "HB" in JS8Call >= 2.2.
                text += "@HB HEARTBEAT";
            }
            if (!grid.empty()) text += " " + grid;
            return text;
        }
    }

    // Compound frames: a lone compound callsign, or compound-directed.
    {
        std::uint8_t  ctype = 0;
        std::uint16_t num   = 0;
        std::uint8_t  bits3 = 0;
        auto          parts = unpack_compound_message(frame, &ctype, &num, &bits3);
        if (!parts.empty()) {
            std::string out;
            for (auto part : parts) {
                part = ltrim(part);
                if (part.empty()) continue;
                if (!out.empty()) out += " ";
                out += part;
            }

            if (ctype == 1 && !parts.front().empty()) {
                compound_calls_.erase(std::remove_if(compound_calls_.begin(), compound_calls_.end(),
                                                     [&](const CompoundCall &c) {
                                                         return now - c.received_at > COMPOUND_MEMORY;
                                                     }),
                                      compound_calls_.end());
                compound_calls_.push_back({parts.front(), freq_hz, now});
            }

            if (ctype == 2 && !parts.front().empty() && !out.empty()) {
                auto  closest      = compound_calls_.end();
                float closest_dist = std::numeric_limits<float>::max();
                for (auto it = compound_calls_.begin(); it != compound_calls_.end(); ++it) {
                    float d = std::fabs(freq_hz - it->freq_hz);
                    if (d <= COMPOUND_MATCH_HZ && d < closest_dist) {
                        closest      = it;
                        closest_dist = d;
                    }
                }
                if (closest != compound_calls_.end()) {
                    out = closest->callsign + ": " + out;
                    compound_calls_.erase(closest);
                }
            }
            if (!out.empty()) return out;
        }
    }

    // Directed frames: "FROM: TO CMD [NUM]" or directed free text.
    {
        std::uint8_t dtype = 0;
        auto         parts = unpack_directed_message(frame, &dtype);
        if (!parts.empty()) {
            std::vector<std::string> tokens;
            for (auto part : parts) {
                part = ltrim(part);
                if (!part.empty()) tokens.push_back(part);
            }
            if (!tokens.empty()) {
                std::string out = tokens[0];
                if (tokens.size() >= 2) {
                    out += ": " + tokens[1];
                    for (std::size_t i = 2; i < tokens.size(); ++i) out += " " + tokens[i];
                }
                return out;
            }
        }
    }

    return frame;
}

} // namespace x6100::js8
