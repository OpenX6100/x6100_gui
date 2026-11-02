#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace js8core::dsp {

// Shared FIR design helper (matches desktop 48k->12k taps when applicable).
std::vector<float> make_js8_fir(int input_rate, int target_rate);

class Resampler {
public:
  void configure(int input_rate, int output_rate);
  void reset();

  int input_rate() const { return input_rate_; }
  int output_rate() const { return output_rate_; }

  template <typename InputFn>
  void process(std::span<float> output, InputFn next_input) {
    switch (mode_) {
      case Mode::Unconfigured:
        for (auto& v : output) v = 0.0f;
        return;
      case Mode::Passthrough:
        for (auto& v : output) v = next_input();
        return;
      case Mode::Upsample:
        upsample(output, next_input);
        return;
      case Mode::Downsample:
        downsample(output, next_input);
        return;
      case Mode::Fractional:
        fractional(output, next_input);
        return;
    }
  }

private:
  enum class Mode { Unconfigured, Passthrough, Upsample, Downsample, Fractional };

  template <typename InputFn>
  void upsample(std::span<float> output, InputFn next_input) {
    if (phase_taps_.empty() || ring_.empty()) {
      for (auto& v : output) v = 0.0f;
      return;
    }

    for (auto& v : output) {
      if (phase_ == 0) {
        ring_[ring_pos_] = next_input();
        ring_pos_ = (ring_pos_ + 1) % static_cast<int>(ring_.size());
      }

      auto const& taps = phase_taps_[phase_];
      double acc = 0.0;
      int read_pos = (ring_pos_ - 1 + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
      for (std::size_t j = 0; j < taps.size(); ++j) {
        int idx = (read_pos - static_cast<int>(j) + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
        acc += static_cast<double>(taps[j]) * static_cast<double>(ring_[idx]);
      }
      v = static_cast<float>(acc);

      phase_ = (phase_ + 1) % factor_;
    }
  }

  template <typename InputFn>
  void downsample(std::span<float> output, InputFn next_input) {
    if (taps_.empty() || ring_.empty()) {
      for (auto& v : output) v = 0.0f;
      return;
    }

    for (auto& v : output) {
      for (int i = 0; i < factor_; ++i) {
        ring_[ring_pos_] = next_input();
        ring_pos_ = (ring_pos_ + 1) % static_cast<int>(ring_.size());
      }

      double acc = 0.0;
      int read_pos = (ring_pos_ - 1 + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
      for (std::size_t j = 0; j < taps_.size(); ++j) {
        int idx = (read_pos - static_cast<int>(j) + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
        acc += static_cast<double>(taps_[j]) * static_cast<double>(ring_[idx]);
      }
      v = static_cast<float>(acc);
    }
  }

  template <typename InputFn>
  void fractional(std::span<float> output, InputFn next_input) {
    if (step_ <= 0.0) {
      for (auto& v : output) v = 0.0f;
      return;
    }

    if (!has_next_) {
      curr_ = next_input();
      next_ = next_input();
      has_next_ = true;
      frac_pos_ = 0.0;
    }

    for (auto& v : output) {
      v = curr_ + static_cast<float>(frac_pos_) * (next_ - curr_);
      frac_pos_ += step_;
      while (frac_pos_ >= 1.0) {
        curr_ = next_;
        next_ = next_input();
        frac_pos_ -= 1.0;
      }
    }
  }

  Mode mode_ = Mode::Unconfigured;
  int input_rate_ = 0;
  int output_rate_ = 0;
  int factor_ = 1;
  std::vector<float> taps_;
  std::vector<std::vector<float>> phase_taps_;
  std::vector<float> ring_;
  int ring_pos_ = 0;
  int phase_ = 0;
  double step_ = 0.0;
  double frac_pos_ = 0.0;
  float curr_ = 0.0f;
  float next_ = 0.0f;
  bool has_next_ = false;
};

}  // namespace js8core::dsp
