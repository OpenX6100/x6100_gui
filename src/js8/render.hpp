/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Copyright (c) the JS8Call-improved authors (ported from their Android app)
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 *
 *  Frame-to-text rendering. Ported from render_decoded_text() in
 *  JS8Call-improved/Android-port adapters/android/jni/js8_engine_jni.cpp,
 *  which mirrors desktop JS8Call's DecodedText unpack order.
 */

#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace x6100::js8 {

/// Frame-type bits carried by every JS8 frame (Varicode::TransmissionType).
enum FrameBits : int {
    FRAME_FIRST = 0b001,
    FRAME_LAST  = 0b010,
    FRAME_DATA  = 0b100,
};

/// Below this decoder quality, desktop JS8Call shows text in [brackets].
constexpr float LOW_CONFIDENCE_QUALITY = 0.17f;

/// Turns decoded 12-character frames into display text.
///
/// Stateful: a compound-callsign frame (e.g. "EA8/G4ABC") is remembered by
/// audio offset for 60 s so a following compound-directed frame at the same
/// offset can be attributed to it. Not thread-safe; use from one thread.
class FrameRenderer {
public:
    using Clock = std::chrono::steady_clock;

    /// @param frame   12-character frame from the decoder
    /// @param type    frame-type bits; FRAME_DATA is added when the payload
    ///                (rather than the frame header) marked it as data
    /// @param freq_hz audio offset of the decode
    std::string render(const std::string &frame, int *type, float freq_hz,
                       Clock::time_point now = Clock::now());

private:
    struct CompoundCall {
        std::string       callsign;
        float             freq_hz;
        Clock::time_point received_at;
    };
    std::vector<CompoundCall> compound_calls_;
};

bool is_callsign_like(const std::string &token);

} // namespace x6100::js8
