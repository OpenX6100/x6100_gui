/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#include "resampler.hpp"

#include <algorithm>
#include <cmath>

namespace x6100::js8 {

namespace {

// Zeroth-order modified Bessel function of the first kind, for the Kaiser window.
double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

double kaiser_beta(double stopband_db) {
    if (stopband_db > 50.0) return 0.1102 * (stopband_db - 8.7);
    if (stopband_db >= 21.0) return 0.5842 * std::pow(stopband_db - 21.0, 0.4) + 0.07886 * (stopband_db - 21.0);
    return 0.0;
}

} // namespace

RationalResampler::RationalResampler(int interp, int decim, int taps_per_phase, double stopband_db)
    : l_(interp), m_(decim), taps_per_phase_(taps_per_phase) {
    const int    n_taps = l_ * taps_per_phase_;
    // Cutoff relative to the L-times-upsampled rate: half of the narrower of
    // the input and output bands, pulled in slightly for the transition band.
    const double cutoff = 0.5 / std::max(l_, m_) * 0.92;
    const double beta   = kaiser_beta(stopband_db);
    const double centre = (n_taps - 1) / 2.0;

    std::vector<double> proto(n_taps);
    for (int i = 0; i < n_taps; ++i) {
        double t    = i - centre;
        double sinc = (t == 0.0) ? 2.0 * cutoff : std::sin(2.0 * M_PI * cutoff * t) / (M_PI * t);
        double r    = t / centre;
        double w    = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel_i0(beta);
        // Gain of L restores the amplitude lost to zero-stuffing.
        proto[i] = sinc * w * l_;
    }

    // Split into L phases. Phase p holds taps p, p+L, p+2L, ...; storing
    // them reversed lets process() walk history oldest-to-newest.
    phases_.assign(l_, std::vector<float>(taps_per_phase_));
    for (int p = 0; p < l_; ++p)
        for (int k = 0; k < taps_per_phase_; ++k)
            phases_[p][taps_per_phase_ - 1 - k] = (float)proto[p + k * l_];

    reset();
}

void RationalResampler::reset() {
    history_.assign(2 * taps_per_phase_, 0.0f);
    head_  = 0;
    phase_ = 0;
}

void RationalResampler::process(const float *in, std::size_t n, std::vector<float> &out) {
    out.reserve(out.size() + n * l_ / m_ + 2);

    for (std::size_t i = 0; i < n; ++i) {
        // Push one input sample; keep a mirrored copy so the most recent
        // taps_per_phase_ samples are always contiguous.
        history_[head_]                   = in[i];
        history_[head_ + taps_per_phase_] = in[i];
        head_                             = (head_ + 1) % taps_per_phase_;

        // Emit every output whose L-rate position lands within this input period.
        while (phase_ < l_) {
            const float *h    = &history_[head_];
            const auto  &taps = phases_[phase_];
            float        acc  = 0.0f;
            for (int k = 0; k < taps_per_phase_; ++k) acc += taps[k] * h[k];
            out.push_back(acc);
            phase_ += m_;
        }
        phase_ -= l_;
    }
}

} // namespace x6100::js8
