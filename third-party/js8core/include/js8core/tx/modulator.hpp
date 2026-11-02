#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "js8core/protocol/constants.hpp"

namespace js8core::tx {

class Modulator {
public:
  enum class State { Synchronizing, Active, Idle };

  void start(std::array<int, protocol::kJs8NumSymbols> const& tones,
             int symbol_samples,
             int start_delay_ms,
             int period_ms,
             double audio_frequency_hz,
             double tx_delay_s,
             bool tuning);

  void stop();
  void set_clock_offset_ms(std::int64_t offset_ms) { clock_offset_ms_.store(offset_ms); }
  bool is_idle() const { return state_.load() == State::Idle; }
  bool is_active() const { return state_.load() == State::Active; }
  int milliseconds_until_active() const;

  float next_sample();

private:
  std::array<int, protocol::kJs8NumSymbols> tones_{};
  std::atomic<State> state_{State::Idle};
  bool tuning_ = false;
  int symbol_samples_ = 0;
  int base_rate_ = protocol::kJs8RxSampleRate;
  double tone_spacing_ = 0.0;
  double audio_frequency_ = 0.0;
  double audio_frequency0_ = 0.0;
  double phi_ = 0.0;
  double dphi_ = 0.0;
  double amp_ = 1.0;
  std::atomic<std::int64_t> silent_frames_{0};
  std::atomic<std::int64_t> clock_offset_ms_{0};
  std::uint64_t ic_ = 0;
  std::uint64_t isym0_ = 0;
};

}  // namespace js8core::tx
