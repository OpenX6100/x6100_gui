#include "js8core/dsp/resampler.hpp"

#include <algorithm>
#include <cmath>

namespace js8core::dsp {

namespace {

std::vector<float> make_windowed_sinc_fir(int input_rate, int target_rate) {
  constexpr int kNumTaps = 32;
  constexpr double kPi = 3.14159265358979323846;

  if (input_rate <= 0 || target_rate <= 0) return {};

  double cutoff = 0.5 * static_cast<double>(target_rate) / static_cast<double>(input_rate);
  cutoff = std::clamp(cutoff, 0.0, 0.5);

  std::vector<float> taps(kNumTaps);
  double sum = 0.0;

  for (int i = 0; i < kNumTaps; ++i) {
    double n = i - (kNumTaps - 1) / 2.0;
    double sinc = (n == 0.0)
                      ? 2.0 * cutoff
                      : std::sin(2.0 * kPi * cutoff * n) / (kPi * n);
    double window = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (kNumTaps - 1));
    taps[i] = static_cast<float>(sinc * window);
    sum += taps[i];
  }

  if (sum != 0.0) {
    for (auto& t : taps) t = static_cast<float>(t / sum);
  }

  return taps;
}

std::vector<std::vector<float>> build_polyphase(std::vector<float> const& taps, int factor, bool scale) {
  std::vector<std::vector<float>> phases;
  phases.resize(static_cast<std::size_t>(factor));
  for (int p = 0; p < factor; ++p) {
    for (std::size_t i = static_cast<std::size_t>(p); i < taps.size(); i += static_cast<std::size_t>(factor)) {
      float value = taps[i];
      if (scale) value *= static_cast<float>(factor);
      phases[static_cast<std::size_t>(p)].push_back(value);
    }
  }
  return phases;
}

}  // namespace

std::vector<float> make_js8_fir(int input_rate, int target_rate) {
  if (input_rate == 48000 && target_rate == 12000) {
    static const float kDesktopFIR[] = {
        0.000861074040f,  0.010051920210f,  0.010161983649f,  0.011363155076f,
        0.008706594219f,  0.002613872664f, -0.005202883094f, -0.011720748164f,
        -0.013752163325f, -0.009431602741f,  0.000539063909f,  0.012636767098f,
        0.021494659597f,  0.021951235065f,  0.011564169382f, -0.007656470131f,
        -0.028965787341f, -0.042637874109f, -0.039203309748f, -0.013153301537f,
         0.034320769178f,  0.094717832646f,  0.154224604789f,  0.197758325022f,
         0.213715139513f,  0.197758325022f,  0.154224604789f,  0.094717832646f,
         0.034320769178f, -0.013153301537f, -0.039203309748f, -0.042637874109f,
        -0.028965787341f, -0.007656470131f,  0.011564169382f,  0.021951235065f,
         0.021494659597f,  0.012636767098f,  0.000539063909f, -0.009431602741f,
        -0.013752163325f, -0.011720748164f, -0.005202883094f,  0.002613872664f,
         0.008706594219f,  0.011363155076f,  0.010161983649f,  0.010051920210f,
         0.000861074040f};
    return std::vector<float>(std::begin(kDesktopFIR), std::end(kDesktopFIR));
  }

  return make_windowed_sinc_fir(input_rate, target_rate);
}

void Resampler::configure(int input_rate, int output_rate) {
  reset();
  input_rate_ = input_rate;
  output_rate_ = output_rate;

  if (input_rate_ <= 0 || output_rate_ <= 0) {
    mode_ = Mode::Unconfigured;
    return;
  }

  if (input_rate_ == output_rate_) {
    mode_ = Mode::Passthrough;
    return;
  }

  if (output_rate_ % input_rate_ == 0) {
    mode_ = Mode::Upsample;
    factor_ = output_rate_ / input_rate_;
    int fir_in = std::max(input_rate_, output_rate_);
    int fir_out = std::min(input_rate_, output_rate_);
    taps_ = make_js8_fir(fir_in, fir_out);
    phase_taps_ = build_polyphase(taps_, factor_, true);
    ring_.assign(taps_.size(), 0.0f);
    return;
  }

  if (input_rate_ % output_rate_ == 0) {
    mode_ = Mode::Downsample;
    factor_ = input_rate_ / output_rate_;
    int fir_in = std::max(input_rate_, output_rate_);
    int fir_out = std::min(input_rate_, output_rate_);
    taps_ = make_js8_fir(fir_in, fir_out);
    ring_.assign(taps_.size(), 0.0f);
    return;
  }

  mode_ = Mode::Fractional;
  step_ = static_cast<double>(input_rate_) / static_cast<double>(output_rate_);
}

void Resampler::reset() {
  mode_ = Mode::Unconfigured;
  input_rate_ = 0;
  output_rate_ = 0;
  factor_ = 1;
  taps_.clear();
  phase_taps_.clear();
  ring_.clear();
  ring_pos_ = 0;
  phase_ = 0;
  step_ = 0.0;
  frac_pos_ = 0.0;
  curr_ = 0.0f;
  next_ = 0.0f;
  has_next_ = false;
}

}  // namespace js8core::dsp
