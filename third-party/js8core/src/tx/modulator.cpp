#include "js8core/tx/modulator.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include "js8core/compat/numbers.hpp"

namespace js8core::tx {

namespace {
constexpr double kTau = 2.0 * std::numbers::pi;
}

void Modulator::start(std::array<int, protocol::kJs8NumSymbols> const& tones,
                      int symbol_samples,
                      int start_delay_ms,
                      int period_ms,
                      double audio_frequency_hz,
                      double tx_delay_s,
                      bool tuning) {
  if (symbol_samples <= 0 || period_ms <= 0) {
    stop();
    return;
  }

  tones_ = tones;
  tuning_ = tuning;
  symbol_samples_ = symbol_samples;
  base_rate_ = protocol::kJs8RxSampleRate;
  tone_spacing_ = static_cast<double>(base_rate_) / static_cast<double>(symbol_samples_);
  audio_frequency_ = audio_frequency_hz;
  audio_frequency0_ = 0.0;
  phi_ = 0.0;
  dphi_ = 0.0;
  amp_ = 1.0;
  silent_frames_.store(0);
  ic_ = 0;
  isym0_ = std::numeric_limits<std::uint64_t>::max();

  if (!tuning_) {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() +
                  clock_offset_ms_.load();
    auto period_offset = static_cast<std::int64_t>(((now_ms % period_ms) + period_ms) % period_ms);
    auto tx_delay_ms = std::max<std::int64_t>(0, static_cast<std::int64_t>(tx_delay_s * 1000.0));

    // tx_delay_s decides whether this request belongs in the next period; it
    // must not move a submode's fixed on-air offset (Turbo is always 100 ms).
    std::int64_t wait_ms = 0;
    if (period_offset + tx_delay_ms >= period_ms) {
      wait_ms = period_ms - period_offset + start_delay_ms;
    } else if (period_offset < start_delay_ms) {
      wait_ms = start_delay_ms - period_offset;
    } else {
      // JS8 frames are valid only from their slot boundary. Starting partway
      // through a frame sends a partial waveform that receivers cannot decode.
      wait_ms = period_ms - period_offset + start_delay_ms;
    }
    silent_frames_.store(wait_ms * base_rate_ / 1000);
  }

  state_.store(silent_frames_.load() > 0 ? State::Synchronizing : State::Active);
}

void Modulator::stop() {
  state_.store(State::Idle);
  silent_frames_.store(0);
  ic_ = 0;
  phi_ = 0.0;
}

float Modulator::next_sample() {
  State state = state_.load();
  if (state == State::Idle) return 0.0f;

  if (state == State::Synchronizing) {
    auto const remaining = silent_frames_.load();
    if (remaining > 0) {
      if (silent_frames_.fetch_sub(1) == 1) {
        state_.store(State::Active);
      }
      return 0.0f;
    }
    state_.store(State::Active);
  }

  std::int64_t i0 = tuning_ ? std::numeric_limits<std::int64_t>::max()
                            : static_cast<std::int64_t>((protocol::kJs8NumSymbols - 0.017) * symbol_samples_);
  std::int64_t i1 = tuning_ ? std::numeric_limits<std::int64_t>::max()
                            : static_cast<std::int64_t>(protocol::kJs8NumSymbols * symbol_samples_);

  if (static_cast<std::int64_t>(ic_) >= i1) {
    state_.store(State::Idle);
    phi_ = 0.0;
    return 0.0f;
  }

  std::uint64_t isym = tuning_ ? 0 : (ic_ / static_cast<std::uint64_t>(symbol_samples_));
  if (isym != isym0_ || audio_frequency_ != audio_frequency0_) {
    double tone_frequency = audio_frequency_ + static_cast<double>(tones_[isym]) * tone_spacing_;
    dphi_ = kTau * tone_frequency / static_cast<double>(base_rate_);
    isym0_ = isym;
    audio_frequency0_ = audio_frequency_;
  }

  phi_ += dphi_;
  if (phi_ > kTau) phi_ -= kTau;

  if (static_cast<std::int64_t>(ic_) > i0) amp_ *= 0.98;

  float sample = static_cast<float>(amp_ * std::sin(phi_));
  ++ic_;

  if (amp_ <= 0.0) {
    state_.store(State::Idle);
    phi_ = 0.0;
  }

  return sample;
}

int Modulator::milliseconds_until_active() const {
  auto const state = state_.load();
  if (state == State::Idle) return -1;
  if (state == State::Active) return 0;

  auto const frames = silent_frames_.load();
  if (frames <= 0) return 0;
  return static_cast<int>((frames * 1000 + protocol::kJs8RxSampleRate - 1) /
                          protocol::kJs8RxSampleRate);
}

}  // namespace js8core::tx
