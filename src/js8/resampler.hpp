/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#pragma once

#include <cstddef>
#include <vector>

namespace x6100::js8 {

/// Rational L/M polyphase resampler (Kaiser-windowed sinc).
///
/// Used to take the radio's 11025 Hz dialog audio to the 12000 Hz that the
/// JS8 decoder expects (L/M = 160/147). The prototype filter cuts off below
/// the lower of the two Nyquist rates, so images of the input spectrum are
/// suppressed before they can fold back into the passband.
class RationalResampler {
public:
    RationalResampler(int interp, int decim, int taps_per_phase = 24, double stopband_db = 80.0);

    /// Append resampled output for `n` input samples to `out`.
    void process(const float *in, std::size_t n, std::vector<float> &out);

    void reset();

    int interp() const { return l_; }
    int decim() const { return m_; }

private:
    int                             l_;
    int                             m_;
    int                             taps_per_phase_;
    std::vector<std::vector<float>> phases_; // [l_][taps_per_phase_], newest-sample-first
    std::vector<float>              history_; // circular, 2x length to avoid wrap on read
    std::size_t                     head_ = 0;
    int                             phase_ = 0; // position in the L-rate output grid
};

} // namespace x6100::js8
