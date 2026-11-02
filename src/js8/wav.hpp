/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x6100::js8 {

/// A 16-bit PCM WAV file, reduced to mono float in [-1, 1].
struct WavAudio {
    int                rate = 0;
    std::vector<float> samples;
};

/// Read a RIFF/WAVE file with 16-bit PCM samples (mono, or the first channel
/// of a multi-channel file). Chunks other than "fmt " and "data" are
/// skipped, wherever they appear. On failure returns false and sets `error`.
bool read_wav(const std::string &path, WavAudio &out, std::string &error);

/// Write mono 16-bit PCM, full scale 1.0 (values beyond it are clipped).
bool write_wav(const std::string &path, int rate, const std::vector<float> &samples);

} // namespace x6100::js8
