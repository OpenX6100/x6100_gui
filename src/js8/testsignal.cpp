/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#include "testsignal.hpp"

#include "speeds.hpp"

#include "js8core/decoder.hpp"
#include "js8core/protocol/costas.hpp"
#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace x6100::js8 {

std::vector<float> make_test_band(const std::vector<TestStation> &stations, int rate, float noise_rms,
                                  std::uint32_t seed) {
    namespace vc = js8core::protocol::varicode;
    using Tones  = std::array<int, js8core::kJs8NumSymbols>;

    std::vector<std::vector<Tones>> tones;
    std::size_t                     seconds = 0;
    for (auto &st : stations) {
        const Speed &sp     = speed(st.speed);
        const auto  &costas = js8core::protocol::costas(sp.original_costas ? js8core::protocol::CostasType::Original
                                                                           : js8core::protocol::CostasType::Modified);
        auto frames = vc::build_message_frames(st.call, st.grid, "", st.text, false, false, sp.varicode);
        tones.emplace_back();
        for (auto &[frame, bits] : frames) {
            Tones t{};
            js8core::legacy_encode(bits, costas, frame.c_str(), t.data());
            tones.back().push_back(t);
        }
        seconds = std::max(seconds, frames.size() * sp.period_s);
    }
    seconds = std::max<std::size_t>((seconds + 29) / 30 * 30, 30);

    std::vector<float> audio(seconds * rate, 0.0f);

    // Tone power relative to noise in 2500 Hz of a band `rate / 2` wide.
    const double noise_2500 = (double)noise_rms * noise_rms * 2500.0 / (rate / 2.0);

    for (std::size_t i = 0; i < stations.size(); i++) {
        const Speed &sp          = speed(stations[i].speed);
        const double sym_samples = sp.symbol_seconds() * rate;
        const double amp         = std::sqrt(2.0 * noise_2500 * std::pow(10.0, stations[i].snr_db / 10.0));
        for (std::size_t k = 0; k < tones[i].size(); k++) {
            const std::size_t start = (k * sp.period_ms() + sp.start_delay_ms) * (std::size_t)rate / 1000;
            double            phi   = 0.0;
            for (int s = 0; s < js8core::kJs8NumSymbols; s++) {
                const double dphi =
                    2.0 * M_PI * (stations[i].offset_hz + tones[i][k][s] * sp.tone_spacing_hz()) / rate;
                const std::size_t a    = start + (std::size_t)std::llround(s * sym_samples);
                const std::size_t b    = start + (std::size_t)std::llround((s + 1) * sym_samples);
                for (std::size_t j = a; j < b && j < audio.size(); j++) {
                    audio[j] += (float)(amp * std::sin(phi));
                    phi += dphi;
                }
            }
        }
    }

    std::mt19937                    rng(seed);
    std::normal_distribution<float> noise(0.0f, noise_rms);
    for (auto &x : audio) x += noise(rng);
    return audio;
}

} // namespace x6100::js8
