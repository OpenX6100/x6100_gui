#if defined(__linux__)
#include <pthread.h>
#endif
#include "js8core/engine.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <deque>
#include "js8core/compat/numbers.hpp"
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "commons.h"
#include "js8core/decoder_state.hpp"
#include "js8core/decoder_bridge.hpp"
#include "js8core/decoder.hpp"
#include "js8core/dsp/resampler.hpp"
#include "js8core/protocol/costas.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/submode.hpp"
#include "js8core/protocol/varicode.hpp"
#include "js8core/types.hpp"
#include "js8core/tx/modulator.hpp"

namespace js8core {

namespace {

std::optional<protocol::Submode> submode_from_varicode(int submode) {
  using protocol::SubmodeId;
  switch (submode) {
    case 0:  return protocol::find(SubmodeId::A); // Normal
    case 1:  return protocol::find(SubmodeId::B); // Fast
    case 2:  return protocol::find(SubmodeId::C); // Turbo
    case 4:  return protocol::find(SubmodeId::E); // Slow
    case 8:  return protocol::find(SubmodeId::I); // Ultra
    default: return protocol::find(SubmodeId::A);
  }
}

protocol::CostasType costas_from_varicode(int submode) {
  return submode == 0 ? protocol::CostasType::Original : protocol::CostasType::Modified;
}

class Js8EngineImpl : public Js8Engine {
public:
  Js8EngineImpl(EngineConfig config, EngineCallbacks callbacks, EngineDependencies deps)
      : config_(std::move(config)),
        callbacks_(std::move(callbacks)),
        deps_(deps) {
    decode_state_.samples.resize(kJs8NtMax * kJs8RxSampleRate);

    // Initialize decoder parameters with sensible defaults
    decode_state_.params.nfa = 200;   // Start frequency (Hz) - avoid low-freq noise
    decode_state_.params.nfb = 2500;  // End frequency (Hz) - typical JS8Call range
    decode_state_.params.nfqso = 1500; // Center frequency for QSO

    // Align the ring buffer position to wall clock so decode windows line up with
    // the desktop timing (cycles relative to UTC within the current minute).
    align_ring_to_clock();

    if (config_.submodes == 0) {
      int mask = 0;
      for (auto const& sm : js8core::protocol::submodes()) {
        if (sm.enabled) mask |= 1 << static_cast<int>(sm.id);
      }
      config_.submodes = mask;
    }
    enabled_submodes_.store(config_.submodes);
    init_schedules();
    start_decode_worker();
    if (config_.spectrum_enabled) start_spectrum_worker();
  }

  ~Js8EngineImpl() override {
    stop_decode_worker();
    stop_spectrum_worker();
  }

  bool start() override {
    running_ = true;

    if (deps_.audio_in) {
      AudioStreamParams params;
      params.format.sample_rate = config_.sample_rate_hz ? config_.sample_rate_hz : kJs8RxSampleRate;
      params.format.channels = 1;
      params.format.sample_type = SampleType::Int16;
      params.frames_per_buffer = 0;

      auto ok = deps_.audio_in->start(
          params,
          [this](AudioInputBuffer const& buf) {
            if (running_) submit_capture(buf);
          },
          [this](std::string_view msg) {
            if (callbacks_.on_error) callbacks_.on_error(msg);
          });

      if (!ok && callbacks_.on_error) {
        callbacks_.on_error("Failed to start audio input");
      }
    }

    if (deps_.rig) {
      deps_.rig->start(
          [](RigState const&) {},
          [this](std::string_view msg) {
            if (callbacks_.on_error) callbacks_.on_error(msg);
          });
    }

    return true;
  }

  void stop() override {
    running_ = false;
    stop_transmit();
    if (deps_.audio_in) deps_.audio_in->stop();
    if (deps_.rig) deps_.rig->stop();
  }

  bool submit_capture(AudioInputBuffer const& buffer) override {
    if (buffer.format.sample_type != SampleType::Int16) {
      if (callbacks_.on_error) callbacks_.on_error("Unsupported sample type");
      return false;
    }

    // Only accept matching sample rate for now.
    if (config_.sample_rate_hz && buffer.format.sample_rate != config_.sample_rate_hz) {
      if (callbacks_.on_error) callbacks_.on_error("Unexpected sample rate");
      return false;
    }

    // Apply a pending drift change here, on the thread that owns the ring state.
    if (drift_realign_pending_.exchange(false)) {
      align_ring_to_clock();
    }

    auto bytes_per_sample = static_cast<std::size_t>(sizeof(std::int16_t) * buffer.format.channels);
    if (bytes_per_sample == 0) return false;
    auto total_samples = buffer.data.size() / bytes_per_sample;

    // Append mono data; if stereo, take left channel for now.
    auto* raw = reinterpret_cast<const std::int16_t*>(buffer.data.data());
    std::size_t frames = total_samples / static_cast<std::size_t>(buffer.format.channels);

    // Calculate RMS power to verify we're getting actual audio data
    static int audio_log_counter = 0;
    if (++audio_log_counter % 100 == 0 && callbacks_.on_log) {
      double sum_squares = 0.0;
      for (std::size_t i = 0; i < frames; ++i) {
        int16_t sample = raw[i * buffer.format.channels];
        sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
      }
      double rms = std::sqrt(sum_squares / static_cast<double>(frames));
      char log_msg[256];
      snprintf(log_msg, sizeof(log_msg),
              "Audio submit: frames=%zu, rms=%.1f, total_samples=%d, kin=%d",
              frames, rms, total_samples_, decode_state_.params.kin);
      callbacks_.on_log(LogLevel::Info, log_msg);
    }

    for (std::size_t i = 0; i < frames; ++i) {
      auto write_index = (static_cast<std::size_t>(decode_state_.params.kin) + i) % decode_state_.samples.size();
      decode_state_.samples[write_index] = raw[i * buffer.format.channels];
    }
    decode_state_.params.kin = (decode_state_.params.kin + static_cast<int>(frames)) %
                               static_cast<int>(decode_state_.samples.size());
    total_samples_ += static_cast<int>(frames);

    // Emit a lightweight spectrum frame for UI consumers at a throttled rate.
    if (config_.spectrum_enabled && callbacks_.on_event && frames > 0) {
      auto const now = std::chrono::steady_clock::now();
      if (now - last_spectrum_time_ >= kSpectrumInterval) {
        last_spectrum_time_ = now;
        enqueue_spectrum(raw, frames, buffer.format.channels, buffer.format.sample_rate);
      }
    }

    // Trigger decode scheduling when we've accumulated enough samples.
    schedule_decodes();
    return true;
  }

  bool transmit_message(TxMessageRequest const& request) override {
    auto sm = submode_from_varicode(request.submode);
    if (!sm) return false;

    protocol::varicode::MessageInfo info;
    auto frames = protocol::varicode::build_message_frames(
        request.my_call,
        request.my_grid,
        request.selected_call,
        request.text,
        request.force_identify,
        request.force_data,
        request.submode,
        &info);

    if (frames.empty()) return false;

    std::deque<TxFrame> built;
    built.resize(frames.size());
    auto costas = protocol::costas(costas_from_varicode(request.submode));

    for (std::size_t i = 0; i < frames.size(); ++i) {
      auto const& frame = frames[i].first;
      if (frame.size() < 12) continue;
      TxFrame tx_frame;
      tx_frame.bits = frames[i].second;
      tx_frame.frame = frame.substr(0, 12);
      legacy_encode(tx_frame.bits, costas, tx_frame.frame.c_str(), tx_frame.tones.data());
      built[i] = std::move(tx_frame);
    }

    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_queue_.clear();
      for (auto& frame : built) {
        if (!frame.frame.empty()) {
          tx_queue_.push_back(std::move(frame));
        }
      }
      if (tx_queue_.empty()) return false;

      tx_settings_.submode = request.submode;
      tx_settings_.audio_frequency_hz = request.audio_frequency_hz;
      tx_settings_.tx_delay_s = request.tx_delay_s;
      tx_settings_.tuning = false;
      tx_active_ = true;

      tx_modulator_.stop();
      tx_resampler_.reset();
      start_next_frame_locked();
    }

    if (!start_tx_output()) {
      stop_transmit();
      return false;
    }
    return true;
  }

  bool transmit_frame(TxFrameRequest const& request) override {
    auto sm = submode_from_varicode(request.submode);
    if (!sm) return false;
    if (request.frame.size() < 12) return false;

    TxFrame frame;
    frame.bits = request.bits;
    frame.frame = request.frame.substr(0, 12);
    auto costas = protocol::costas(costas_from_varicode(request.submode));
    legacy_encode(frame.bits, costas, frame.frame.c_str(), frame.tones.data());

    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_queue_.clear();
      tx_queue_.push_back(std::move(frame));
      tx_settings_.submode = request.submode;
      tx_settings_.audio_frequency_hz = request.audio_frequency_hz;
      tx_settings_.tx_delay_s = request.tx_delay_s;
      tx_settings_.tuning = false;
      tx_active_ = true;

      tx_modulator_.stop();
      tx_resampler_.reset();
      start_next_frame_locked();
    }

    if (!start_tx_output()) {
      stop_transmit();
      return false;
    }
    return true;
  }

  bool start_tune(double audio_frequency_hz, int submode, double tx_delay_s) override {
    auto sm = submode_from_varicode(submode);
    if (!sm) return false;

    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_queue_.clear();
      tx_settings_.submode = submode;
      tx_settings_.audio_frequency_hz = audio_frequency_hz;
      tx_settings_.tx_delay_s = tx_delay_s;
      tx_settings_.tuning = true;
      tx_active_ = true;

      tx_resampler_.reset();
      std::array<int, protocol::kJs8NumSymbols> tones{};
      tx_modulator_.start(tones,
                          sm->symbol_samples,
                          sm->start_delay_ms,
                          sm->tx_seconds * 1000,
                          tx_settings_.audio_frequency_hz,
                          tx_settings_.tx_delay_s,
                          true);
    }

    if (!start_tx_output()) {
      stop_transmit();
      return false;
    }
    return true;
  }

  void stop_transmit() override {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    tx_active_ = false;
    tx_queue_.clear();
    tx_settings_.tuning = false;
    tx_modulator_.stop();
    tx_resampler_.reset();
    if (deps_.audio_out && tx_output_started_) {
      deps_.audio_out->stop();
      tx_output_started_ = false;
    }
  }

  bool is_transmitting() const override {
    return tx_active_;
  }

  bool is_transmitting_audio() const override {
    return tx_modulator_.is_active() && tx_ready_.load();
  }

  int tx_milliseconds_until_audio() const override {
    return tx_modulator_.milliseconds_until_active();
  }

  void set_tx_ready(bool ready) override {
    tx_ready_.store(ready);
  }

  void set_tx_boost_enabled(bool enabled) override {
    config_.tx_output_gain_boost_enabled = enabled;
  }

  void set_decode_range(int low_hz, int high_hz) override {
    if (low_hz < 0) low_hz = 0;
    if (high_hz <= low_hz) return;
    nfa_.store(low_hz);
    nfb_.store(high_hz);
  }

  void set_qso_offset(int offset_hz) override { nfqso_.store(offset_hz); }
  void set_sync_stats(bool enabled) override { sync_stats_.store(enabled); }

  void set_submodes(int submodes) override {
    constexpr int knownSubmodes = (1 << static_cast<int>(protocol::SubmodeId::A)) |
                                 (1 << static_cast<int>(protocol::SubmodeId::B)) |
                                 (1 << static_cast<int>(protocol::SubmodeId::C)) |
                                 (1 << static_cast<int>(protocol::SubmodeId::E)) |
                                 (1 << static_cast<int>(protocol::SubmodeId::I));
    enabled_submodes_.store(submodes & knownSubmodes);
  }

  void set_time_drift_ms(std::int64_t drift_ms) override {
    if (time_drift_ms_.exchange(drift_ms) == drift_ms) return;
    tx_modulator_.set_clock_offset_ms(drift_ms);
    drift_realign_pending_.store(true);
    if (callbacks_.on_log) {
      char log_msg[128];
      snprintf(log_msg, sizeof(log_msg),
               "Time drift set to %lld ms", static_cast<long long>(drift_ms));
      callbacks_.on_log(LogLevel::Info, log_msg);
    }
  }

  std::int64_t time_drift_ms() const override {
    return time_drift_ms_.load();
  }

  void request_realign() override {
    drift_realign_pending_.store(true);
  }

 private:
    struct SubmodeSchedule {
      protocol::SubmodeId id;
      int period_samples;
      int start_delay_samples;
      int samples_needed;
      int retry_samples;
      int start_offset_samples;
      int current_decode_start = -1;  // Absolute position in buffer for current decode window
      int next_decode_start = -1;     // Absolute position in buffer for next decode window
      int last_decode_sample = -1;
      int next_start = 0;  // Keep for compatibility (unused now)
    };

    struct TxFrame {
      std::array<int, protocol::kJs8NumSymbols> tones{};
      int bits = 0;
      std::string frame;
    };

    struct TxSettings {
      int submode = 0;
      double audio_frequency_hz = 0.0;
      double tx_delay_s = 0.0;
      bool tuning = false;
    };

    std::chrono::system_clock::time_point drifted_now() const {
      return std::chrono::system_clock::now() +
             std::chrono::milliseconds(time_drift_ms_.load());
    }

    // Runs at construction and, after a drift change, on the audio thread (see submit_capture).
    void align_ring_to_clock() {
      auto const sample_rate = config_.sample_rate_hz ? config_.sample_rate_hz : kJs8RxSampleRate;
      ring_drift_ms_ = time_drift_ms_.load();
      auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
          drifted_now().time_since_epoch());
      // JS8 RX buffer spans 60 seconds; use milliseconds into the current minute.
      auto ms_in_minute = ms_since_epoch.count() % (kJs8NtMax * 1000);
      int aligned = static_cast<int>((ms_in_minute * sample_rate) / 1000);
      decode_state_.params.kin = aligned;
      total_samples_ = aligned;
      k0_ = aligned;
      // Force every schedule to re-snap its decode window to the new phase.
      for (auto& sch : schedules_) {
        sch.current_decode_start = -1;
        sch.next_decode_start = -1;
        sch.last_decode_sample = -1;
      }
      if (callbacks_.on_log) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "Ring buffer aligned to UTC minute: ms_in_minute=%lld, offset_samples=%d, sample_rate=%d, drift_ms=%lld",
                 static_cast<long long>(ms_in_minute), aligned, sample_rate,
                 static_cast<long long>(time_drift_ms_.load()));
        callbacks_.on_log(LogLevel::Info, log_msg);
      }
    }

    void init_schedules() {
      // Get current UTC time to synchronize decode windows
      using clock = std::chrono::system_clock;
      auto now = drifted_now();
      auto t = clock::to_time_t(now);
      std::tm utc_tm{};
#if defined(_WIN32)
      gmtime_s(&utc_tm, &t);
#else
      gmtime_r(&t, &utc_tm);
#endif

      // Get milliseconds within current second
      auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
      int ms_in_second = ms_since_epoch.count() % 1000;
      int total_ms = utc_tm.tm_sec * 1000 + ms_in_second;

      auto modes = protocol::submodes();
      for (auto const& sm : modes) {
        if ((config_.submodes & (1 << static_cast<int>(sm.id))) == 0) continue;
        int sample_rate = config_.sample_rate_hz ? config_.sample_rate_hz : kJs8RxSampleRate;
        int period = sm.tx_seconds * sample_rate;
        int period_ms = sm.tx_seconds * 1000;

        // Calculate time until next period boundary
        // JS8 periods start at multiples of the period (0, 15, 30, 45 sec for 15s periods)
        // with the start_delay_ms offset (e.g., A starts at 0.5, 15.5, 30.5, 45.5)
        int ms_into_period = total_ms % period_ms;
        int ms_until_next = (period_ms - ms_into_period + sm.start_delay_ms) % period_ms;
        if (ms_until_next == 0) ms_until_next = period_ms;

        // Convert to samples
        int samples_until_next = (ms_until_next * sample_rate) / 1000;
        int start_delay_samples = (sm.start_delay_ms * sample_rate) / 1000;
        int offset_samples = 0;  // Keep windows simple; no additional phase offset

        if (callbacks_.on_log) {
          char log_msg[256];
          snprintf(log_msg, sizeof(log_msg),
                  "Submode %.*s: period=%ds, delay=%dms, current=%d.%03ds, next_in=%dms (%d samples)",
                  static_cast<int>(sm.name.length()), sm.name.data(),
                  sm.tx_seconds, sm.start_delay_ms,
                  utc_tm.tm_sec, ms_in_second, ms_until_next, samples_until_next);
          callbacks_.on_log(LogLevel::Info, log_msg);
        }

        // Turbo can decode as soon as its 79 symbols are captured. Keep the
        // slot available for later retries, matching the desktop scheduler.
        // Ultra (I) keeps the full threshold, start delay and 0.5 s guard
        // included, one decode a slot: desktop's samplesNeeded() for its
        // "JS8 60" (patch 14, which gave it Turbo's path, was withdrawn).
        int samples_needed = (sm.symbol_samples * JS8_NUM_SYMBOLS) +
                            static_cast<int>((0.5 + sm.start_delay_ms / 1000.0) * sample_rate);
        int retry_samples = period;
        if (sm.id == protocol::SubmodeId::C) {
          samples_needed = sm.symbol_samples * JS8_NUM_SYMBOLS;
          retry_samples = sample_rate;
        }

        schedules_.push_back(SubmodeSchedule{sm.id, period, start_delay_samples, samples_needed,
                                             retry_samples, offset_samples, -1, -1, -1,
                                             samples_until_next});
      }
    }

    void set_submode_window(protocol::SubmodeId id, int start, int size) {
      switch (id) {
        case protocol::SubmodeId::A:
          decode_state_.params.kposA = start;
          decode_state_.params.kszA = size;
          break;
        case protocol::SubmodeId::B:
          decode_state_.params.kposB = start;
          decode_state_.params.kszB = size;
          break;
        case protocol::SubmodeId::C:
          decode_state_.params.kposC = start;
          decode_state_.params.kszC = size;
          break;
        case protocol::SubmodeId::E:
          decode_state_.params.kposE = start;
          decode_state_.params.kszE = size;
          break;
        case protocol::SubmodeId::I:
          decode_state_.params.kposI = start;
          decode_state_.params.kszI = size;
          break;
      }
    }

    void populate_decode_metadata() {
      using clock = std::chrono::system_clock;
      auto now = drifted_now();
      auto t = clock::to_time_t(now);
      std::tm utc_tm{};
#if defined(_WIN32)
      gmtime_s(&utc_tm, &t);
#else
      gmtime_r(&t, &utc_tm);
#endif
      decode_state_.params.utc = utc_tm.tm_hour * 10000 + utc_tm.tm_min * 100 + utc_tm.tm_sec;
      decode_state_.params.newdat = true;
      decode_state_.params.syncStats = sync_stats_.load(); // patch 12
      decode_state_.params.nfa = nfa_.load();
      decode_state_.params.nfb = nfb_.load();
      decode_state_.params.nfqso = nfqso_.load();
    }

    // Port of isDecodeReady() from mainwindow.cpp
    // Returns true if decode should happen, sets start and size for decode window
    bool isDecodeReady(SubmodeSchedule& sch, int k, int k0, int* start, int* size) {
      int const cycleFrames = sch.period_samples;
      int const framesNeeded = sch.samples_needed;
      int const maxFrames = kJs8NtMax * kJs8RxSampleRate;

      // Compute which cycle we're in based on current position
      int const currentCycle = (k / cycleFrames) % (maxFrames / cycleFrames);
      int const delta = std::abs(k - k0);

      // Check for buffer loop or discontinuity (deadAir condition)
      bool const deadAir = (k < sch.current_decode_start &&
                           k < std::max(0, sch.current_decode_start - cycleFrames + framesNeeded));

      // DEBUG: Log the state before reset check
      static int debug_counter = 0;
      if (++debug_counter % 50 == 0 && callbacks_.on_log) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg),
                "isDecodeReady DEBUG: submode=%d, k=%d, k0=%d, cycleFrames=%d, framesNeeded=%d, "
                "currentCycle=%d, delta=%d, deadAir=%d, current_decode_start=%d, next_decode_start=%d",
                static_cast<int>(sch.id), k, k0, cycleFrames, framesNeeded,
                currentCycle, delta, deadAir, sch.current_decode_start, sch.next_decode_start);
        callbacks_.on_log(LogLevel::Info, log_msg);
      }

      // Reset decode window on buffer loop, init, or discontinuity
      if (deadAir ||
          (k < k0) ||
          (delta > cycleFrames) ||
          (sch.current_decode_start == -1) ||
          (sch.next_decode_start == -1))
      {
        // Set to UTC-synchronized position using the phase offset (desktop behavior).
        int aligned_start = sch.start_offset_samples + currentCycle * cycleFrames;
        sch.current_decode_start = aligned_start;
        sch.next_decode_start = sch.current_decode_start + cycleFrames;
        sch.last_decode_sample = -1;

        if (callbacks_.on_log) {
          char log_msg[512];
          snprintf(log_msg, sizeof(log_msg),
                  "isDecodeReady RESET: submode=%d, new current_decode_start=%d, new next_decode_start=%d",
                  static_cast<int>(sch.id), sch.current_decode_start, sch.next_decode_start);
          callbacks_.on_log(LogLevel::Info, log_msg);
        }
      }

      // Advance past slots that have elapsed. Turbo retries stay in the
      // current slot until the next six-second boundary.
      while (k >= sch.next_decode_start) {
        sch.current_decode_start = sch.next_decode_start;
        sch.next_decode_start += cycleFrames;
        sch.last_decode_sample = -1;
      }

      // Check if we have enough samples for this decode window. Turbo retries
      // once per second so late/degraded frames get the same opportunities as
      // the desktop scheduler.
      bool const ready = sch.current_decode_start + framesNeeded <= k &&
                         (sch.last_decode_sample < sch.current_decode_start ||
                          k - sch.last_decode_sample >= sch.retry_samples);

      // DEBUG: Log ready check
      if (callbacks_.on_log && (debug_counter % 50 == 0 || ready)) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg),
                "isDecodeReady CHECK: submode=%d, ready=%d, need=%d, have=%d, "
                "current_decode_start=%d, framesNeeded=%d, k=%d",
                static_cast<int>(sch.id), ready, sch.current_decode_start + framesNeeded, k,
                sch.current_decode_start, framesNeeded, k);
        callbacks_.on_log(LogLevel::Info, log_msg);
      }

      if (ready) {
        *start = sch.current_decode_start;  // Absolute position in buffer
        *size = std::max(framesNeeded, k - sch.current_decode_start);

        sch.last_decode_sample = k;
      }

      return ready;
    }

    void schedule_decodes() {
      if (!callbacks_.on_event) return;

      int const k = decode_state_.params.kin;
      int const k0 = k0_;

      // DEBUG: Confirm schedule_decodes() is being called
      static int sched_call_counter = 0;
      if (++sched_call_counter % 100 == 0 && callbacks_.on_log) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                "schedule_decodes CALLED: count=%d, k=%d, k0=%d, total_samples=%d, schedules_size=%zu",
                sched_call_counter, k, k0, total_samples_, schedules_.size());
        callbacks_.on_log(LogLevel::Info, log_msg);
      }

      static int drift_log_counter = 0;
      if (++drift_log_counter % 200 == 0 && callbacks_.on_log) {
        auto const sample_rate = config_.sample_rate_hz ? config_.sample_rate_hz : kJs8RxSampleRate;
        auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
            drifted_now().time_since_epoch());
        int ms_in_minute = static_cast<int>(ms_since_epoch.count() % (kJs8NtMax * 1000));
        int k_ms = static_cast<int>((static_cast<long long>(k) * 1000) / sample_rate);
        int delta_ms = ms_in_minute - k_ms;
        while (delta_ms > 30000) delta_ms -= 60000;
        while (delta_ms < -30000) delta_ms += 60000;
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                "Timing drift: ms_in_minute=%d, k_ms=%d, delta_ms=%d, k=%d, sample_rate=%d",
                ms_in_minute, k_ms, delta_ms, k, sample_rate);
        callbacks_.on_log(LogLevel::Info, log_msg);
      }

      bool any = false;
      decode_state_.params.nsubmodes = 0;

      // Use isDecodeReady() to determine if each submode should decode
      // This replaces the old rolling window approach with UTC-synchronized fixed windows
      for (auto& sch : schedules_) {
        if ((enabled_submodes_.load() & (1 << static_cast<int>(sch.id))) == 0) continue;
        int start = 0;
        int size = 0;

        // isDecodeReady() returns true when we have a complete decode window ready
        // It sets start to the absolute position in the circular buffer
        bool ready_result = isDecodeReady(sch, k, k0, &start, &size);

        // DEBUG: Log the result
        static int result_counter = 0;
        if (++result_counter % 50 == 0 && callbacks_.on_log) {
          char log_msg[256];
          snprintf(log_msg, sizeof(log_msg),
                  "isDecodeReady RESULT: submode=%d, returned=%d, start=%d, size=%d",
                  static_cast<int>(sch.id), ready_result, start, size);
          callbacks_.on_log(LogLevel::Info, log_msg);
        }

        if (ready_result) {
#ifdef __ANDROID__
          __android_log_print(ANDROID_LOG_ERROR, "JS8Core",
                             "IF CONDITION IS TRUE: ready_result=%d (submode=%d)",
                             ready_result, static_cast<int>(sch.id));

          __android_log_print(ANDROID_LOG_ERROR, "JS8Core", "STEP 1: About to create char buffer");
#endif
          // ALWAYS log this - no conditions!
          char log_msg_ifblock[256];
#ifdef __ANDROID__
          __android_log_print(ANDROID_LOG_ERROR, "JS8Core", "STEP 2: Created char buffer, about to snprintf");
#endif
          snprintf(log_msg_ifblock, sizeof(log_msg_ifblock),
                  "INSIDE IF BLOCK: submode=%d, start=%d, size=%d",
                  static_cast<int>(sch.id), start, size);
#ifdef __ANDROID__
          __android_log_print(ANDROID_LOG_ERROR, "JS8Core", "STEP 3: snprintf done, start=%d, size=%d", start, size);
#endif
          if (callbacks_.on_log) {
            callbacks_.on_log(LogLevel::Info, log_msg_ifblock);
          }
#ifdef __ANDROID__
          __android_log_print(ANDROID_LOG_ERROR, "JS8Core",
                             "!!! INSIDE IF BLOCK: submode=%d, start=%d, size=%d !!!",
                             static_cast<int>(sch.id), start, size);
#endif

          // Take kpos modulo buffer size to handle circular buffer wrapping
          int const buffer_size = kJs8NtMax * kJs8RxSampleRate;  // 720,000 samples
          int const wrapped_start = start % buffer_size;

          set_submode_window(sch.id, wrapped_start, size);
          decode_state_.params.nsubmodes |= (1 << static_cast<int>(sch.id));
          any = true;

          if (callbacks_.on_log) {
            // Get UTC time for logging
            using clock = std::chrono::system_clock;
            auto now = clock::now();
            auto t = clock::to_time_t(now);
            std::tm utc_tm{};
#if defined(_WIN32)
            gmtime_s(&utc_tm, &t);
#else
            gmtime_r(&t, &utc_tm);
#endif
            auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            int ms_in_second = ms_since_epoch.count() % 1000;

            char log_msg[512];
            snprintf(log_msg, sizeof(log_msg),
                    "Decode ready at UTC %02d:%02d:%02d.%03d, submode_id=%d, kpos_abs=%d, kpos_wrapped=%d, ksz=%d, k=%d, k0=%d, total_samples=%d",
                    utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec, ms_in_second,
                    static_cast<int>(sch.id), start, wrapped_start, size, k, k0, total_samples_);
            callbacks_.on_log(LogLevel::Info, log_msg);
          }
        }
      }

      // Update k0 for next call
      k0_ = k;

      if (!any) return;

      populate_decode_metadata();

      DecodeState snapshot;
      snapshot.params = decode_state_.params;
      snapshot.samples = decode_state_.samples;
      snapshot.drift_ms_at_capture = ring_drift_ms_;
      enqueue_decode(std::move(snapshot));
    }

    bool start_tx_output() {
      if (!deps_.audio_out || tx_output_started_) return true;

      AudioStreamParams params;
      params.format.sample_rate = config_.tx_output_rate_hz;
      params.format.channels = 1;
      params.format.sample_type = SampleType::Int16;

      bool ok = deps_.audio_out->start(
          params,
          [this](AudioOutputBuffer& buffer) { return render_tx_audio(buffer); },
          [this](std::string_view msg) {
            if (callbacks_.on_error) callbacks_.on_error(msg);
          });

      if (!ok && callbacks_.on_error) {
        callbacks_.on_error("Failed to start audio output");
      }
      tx_output_started_ = ok;
      return ok;
    }

    std::size_t render_tx_audio(AudioOutputBuffer& buffer) {
      std::size_t bytes_per_sample = buffer.format.sample_type == SampleType::Float32
                                         ? sizeof(float)
                                         : sizeof(std::int16_t);
      if (bytes_per_sample == 0 || buffer.format.channels <= 0) return 0;

      std::size_t frames = buffer.data.size() / (bytes_per_sample * static_cast<std::size_t>(buffer.format.channels));
      if (frames == 0) return 0;

      int output_rate = buffer.format.sample_rate;
      if (output_rate <= 0) {
        std::fill(buffer.data.begin(), buffer.data.end(), std::byte{0});
        return buffer.data.size();
      }

      std::lock_guard<std::mutex> lock(tx_mutex_);

      if (!tx_output_logged_ && callbacks_.on_log) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "TX output format: rate=%d Hz, channels=%d, type=%s",
                 output_rate,
                 buffer.format.channels,
                 buffer.format.sample_type == SampleType::Float32 ? "float" : "int16");
        callbacks_.on_log(LogLevel::Info, log_msg);
        tx_output_logged_ = true;
      }

      if (tx_resampler_.input_rate() != protocol::kJs8RxSampleRate ||
          tx_resampler_.output_rate() != output_rate) {
        tx_resampler_.configure(protocol::kJs8RxSampleRate, output_rate);
      }

      if (tx_float_buffer_.size() < frames) {
        tx_float_buffer_.resize(frames);
      }

      tx_resampler_.process(std::span<float>(tx_float_buffer_.data(), frames),
                            [this]() { return next_tx_sample_locked(); });

      float gain = std::clamp(config_.tx_output_gain, 0.0f, 1.0f);
      if (config_.tx_output_gain_boost_enabled) {
        gain *= 3.1623f;
      }
      if (gain != 1.0f) {
        for (std::size_t i = 0; i < frames; ++i) {
          tx_float_buffer_[i] *= gain;
        }
      }

      if (callbacks_.on_log && (++tx_log_counter_ % 250 == 0)) {
        double sum_squares = 0.0;
        float peak = 0.0f;
        std::size_t soft_limited = 0;
        for (std::size_t i = 0; i < frames; ++i) {
          double v = static_cast<double>(tx_float_buffer_[i]);
          sum_squares += v * v;
          peak = std::max(peak, std::abs(tx_float_buffer_[i]));
          if (std::abs(tx_float_buffer_[i]) > 1.0f) ++soft_limited;
        }
        double rms = frames > 0 ? std::sqrt(sum_squares / static_cast<double>(frames)) : 0.0;
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "TX audio: rate=%d, channels=%d, frames=%zu, gain=%.3f, rms=%.4f, peak=%.4f, soft_limited=%zu, active=%d, tuning=%d, queue=%zu",
                 output_rate, buffer.format.channels, frames, gain, rms, peak, soft_limited,
                 tx_active_ ? 1 : 0, tx_settings_.tuning ? 1 : 0, tx_queue_.size());
        callbacks_.on_log(LogLevel::Info, log_msg);
      }

      if (buffer.format.sample_type == SampleType::Float32) {
        auto* out = reinterpret_cast<float*>(buffer.data.data());
        for (std::size_t i = 0; i < frames; ++i) {
          float v = tx_float_buffer_[i];
          for (int ch = 0; ch < buffer.format.channels; ++ch) {
            *out++ = v;
          }
        }
        return frames * bytes_per_sample * static_cast<std::size_t>(buffer.format.channels);
      }

      auto* out = reinterpret_cast<std::int16_t*>(buffer.data.data());
      for (std::size_t i = 0; i < frames; ++i) {
        float v = tx_float_buffer_[i];
        if (config_.tx_output_gain_boost_enabled) {
          v = std::tanh(v);
        } else {
          v = std::clamp(v, -1.0f, 1.0f);
        }
        auto sample = static_cast<std::int16_t>(std::lround(v * 32767.0f));
        for (int ch = 0; ch < buffer.format.channels; ++ch) {
          *out++ = sample;
        }
      }
      return frames * bytes_per_sample * static_cast<std::size_t>(buffer.format.channels);
    }

    float next_tx_sample_locked() {
      if (!tx_active_) return 0.0f;

      if (tx_modulator_.is_idle()) {
        if (!tx_queue_.empty()) {
          start_next_frame_locked();
        } else if (!tx_settings_.tuning) {
          tx_active_ = false;
          return 0.0f;
        }
      }

      if (tx_modulator_.is_active() && !tx_ready_.load()) return 0.0f;

      return tx_modulator_.next_sample();
    }

    void start_next_frame_locked() {
      if (tx_queue_.empty()) return;
      auto sm = submode_from_varicode(tx_settings_.submode);
      if (!sm) {
        tx_queue_.clear();
        tx_active_ = false;
        return;
      }

      TxFrame frame = std::move(tx_queue_.front());
      tx_queue_.pop_front();

      tx_modulator_.start(frame.tones,
                          sm->symbol_samples,
                          sm->start_delay_ms,
                          sm->tx_seconds * 1000,
                          tx_settings_.audio_frequency_hz,
                          tx_settings_.tx_delay_s,
                          tx_settings_.tuning);
    }

    static std::size_t next_pow2(std::size_t v) {
      if (v == 0) return 1;
      --v;
      v |= v >> 1;
      v |= v >> 2;
      v |= v >> 4;
      v |= v >> 8;
      v |= v >> 16;
#if INTPTR_MAX == INT64_MAX
      v |= v >> 32;
#endif
      return v + 1;
    }

    bool compute_spectrum(const std::int16_t* data,
                          std::size_t frames,
                          int channels,
                          int sample_rate,
                          events::Spectrum& spec) {
      if (!data || frames == 0 || sample_rate <= 0) return false;

      // Limit FFT size for cost; keep it power of two for simplicity.
      // Clamp to the largest power-of-two that fits in the provided frame count
      // to avoid reading past the input buffer.
      std::size_t max_n = std::min<std::size_t>(frames, 4096);
      std::size_t n = max_n;
      // Round down to the nearest power of two (<= max_n).
      if (n > 0) {
        n = next_pow2(n);
        if (n > max_n) n >>= 1;
      }
      if (n < 64) return false;

      constexpr double two_pi = 2.0 * std::numbers::pi;
      if (n != spectrum_n_) {
        spectrum_n_ = n;
        spectrum_window_.assign(n, 0.0);
        spectrum_samples_.assign(n, 0.0);
        spectrum_fft_.assign(n, std::complex<double>{});
        for (std::size_t i = 0; i < n; ++i) {
          spectrum_window_[i] =
              0.5 * (1.0 - std::cos(two_pi * static_cast<double>(i) / static_cast<double>(n - 1)));
        }
      }

      // Mono mix: pick first channel.
      double power_sum = 0.0;
      double peak = 0.0;
      for (std::size_t i = 0; i < n; ++i) {
        auto v = static_cast<double>(data[i * channels]);
        power_sum += v * v;
        peak = std::max(peak, std::abs(v));
        spectrum_samples_[i] = v * spectrum_window_[i];
      }

      // In-place iterative radix-2 Cooley-Tukey FFT
      for (std::size_t i = 0; i < n; ++i) {
        spectrum_fft_[i] = std::complex<double>(spectrum_samples_[i], 0.0);
      }

      // Bit reversal
      for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(spectrum_fft_[i], spectrum_fft_[j]);
      }

      for (std::size_t len = 2; len <= n; len <<= 1) {
        double ang = -two_pi / static_cast<double>(len);
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
          std::complex<double> w(1.0, 0.0);
          for (std::size_t j = 0; j < len / 2; ++j) {
            auto u = spectrum_fft_[i + j];
            auto v = spectrum_fft_[i + j + len / 2] * w;
            spectrum_fft_[i + j] = u + v;
            spectrum_fft_[i + j + len / 2] = u - v;
            w *= wlen;
          }
        }
      }

      // Resample the spectrum to the bin count expected by the UI to avoid narrow plots.
      constexpr std::size_t kTargetBins = JS8_NSMAX;
      spec.bins.resize(kTargetBins);
      double scale = 1.0 / static_cast<double>(n * n);
      std::size_t source_bins = n / 2;
      double source_bin_hz = static_cast<double>(sample_rate) / static_cast<double>(n);
      double target_bin_hz = (static_cast<double>(sample_rate) / 2.0) / static_cast<double>(kTargetBins);

      for (std::size_t i = 0; i < kTargetBins; ++i) {
        double freq = static_cast<double>(i) * target_bin_hz;
        double pos = freq / source_bin_hz;
        std::size_t idx = static_cast<std::size_t>(pos);
        double frac = pos - static_cast<double>(idx);
        float v0 = idx < source_bins
                       ? static_cast<float>(std::norm(spectrum_fft_[idx]) * scale)
                       : 0.0f;
        float v1 = (idx + 1 < source_bins)
                       ? static_cast<float>(std::norm(spectrum_fft_[idx + 1]) * scale)
                       : v0;
        spec.bins[i] = v0 + static_cast<float>(frac) * (v1 - v0);
      }

      // Display axis: reflect the actual bin spacing of the resampled spectrum.
      spec.bin_hz = static_cast<float>(target_bin_hz);
      spec.power_db = power_sum > 0.0 ? static_cast<float>(10.0 * std::log10(power_sum / n)) : 0.0f;
      spec.peak_db = peak > 0.0 ? static_cast<float>(20.0 * std::log10(peak)) : 0.0f;
      return true;
    }

    EngineConfig config_;
    EngineCallbacks callbacks_;
    EngineDependencies deps_;
    DecodeState decode_state_;
    SpectrumState spectrum_state_{};
    std::vector<SubmodeSchedule> schedules_;
    int total_samples_{0};
    int k0_{0};  // Previous sample position for isDecodeReady logic
    std::atomic<std::int64_t> time_drift_ms_{0};
    std::atomic<int> enabled_submodes_{0};
    std::atomic<int> nfa_{200};    // patch 9: set_decode_range()
    std::atomic<int> nfb_{2500};
    std::atomic<int> nfqso_{1500}; // set_qso_offset()
    std::atomic<bool> sync_stats_{false}; // patch 12: set_sync_stats()
    std::atomic<bool> drift_realign_pending_{false};
    // Written only on the audio thread; may lag time_drift_ms_ by one capture buffer.
    std::int64_t ring_drift_ms_{0};
    bool running_{false};
    std::mutex tx_mutex_;
    std::deque<TxFrame> tx_queue_;
    TxSettings tx_settings_{};
    tx::Modulator tx_modulator_;
    dsp::Resampler tx_resampler_;
    std::vector<float> tx_float_buffer_;
    int tx_log_counter_ = 0;
    bool tx_output_logged_ = false;
    std::atomic<bool> tx_active_{false};
    bool tx_output_started_{false};
    std::atomic<bool> tx_ready_{true};
    static constexpr auto kSpectrumInterval = std::chrono::milliseconds(100);
    std::chrono::steady_clock::time_point last_spectrum_time_{};
    std::size_t spectrum_n_{0};
    std::vector<double> spectrum_window_;
    std::vector<double> spectrum_samples_;
    std::vector<std::complex<double>> spectrum_fft_;
    events::Variant spectrum_event_{events::Spectrum{}};
    std::mutex event_mutex_;

    // A decode thread and its queue. lanes_[0] decodes everything, or
    // everything but Ultra with ultra_own_thread (patch 15); lanes_[1] then
    // decodes Ultra only.
    struct DecodeLane {
      const char* name = "main";
      const char* thread_name = "js8-decode";
      const char* log_prefix = "";
      std::thread thread;
      std::mutex mutex;
      std::condition_variable cv;
      std::deque<DecodeState> queue;
      bool stop{false};
    };
    DecodeLane lanes_[2];
    static constexpr int kUltraBit = 1 << static_cast<int>(protocol::SubmodeId::I);

    std::thread spectrum_thread_;
    std::mutex spectrum_mutex_;
    std::condition_variable spectrum_cv_;
    struct SpectrumTask {
      std::vector<std::int16_t> samples;
      std::size_t frames = 0;
      int channels = 1;
      int sample_rate = 0;
    };
    std::deque<SpectrumTask> spectrum_queue_;
    bool spectrum_stop_{false};

    // Desktop auto-sync math (mainwindow.cpp, events::Decoded handler).
    int compute_drift_estimate(DecodeState const& task, events::Decoded const& d) const {
      auto sm = submode_from_varicode(d.mode);
      if (!sm) return static_cast<int>(task.drift_ms_at_capture);

      int kpos = 0;
      switch (sm->id) {
        case protocol::SubmodeId::A: kpos = task.params.kposA; break;
        case protocol::SubmodeId::B: kpos = task.params.kposB; break;
        case protocol::SubmodeId::C: kpos = task.params.kposC; break;
        case protocol::SubmodeId::E: kpos = task.params.kposE; break;
        case protocol::SubmodeId::I: kpos = task.params.kposI; break;
      }

      auto const sample_rate = config_.sample_rate_hz ? config_.sample_rate_hz : kJs8RxSampleRate;
      int const period_ms = sm->tx_seconds * 1000;

      float signal_time = static_cast<float>(kpos) / static_cast<float>(sample_rate);
      signal_time -= static_cast<float>(sm->start_delay_ms) / 1000.0f;
      signal_time += d.xdt;
      if (signal_time < 0.0f) signal_time += 60.0f;
      else if (signal_time > 60.0f) signal_time -= 60.0f;

      int const signal_ms = static_cast<int>(1000.0f * signal_time);
      int drift = (signal_ms / period_ms) * period_ms - signal_ms;
      // Take the shorter way around the cycle (-14s becomes +1s, etc).
      if (drift + period_ms < std::abs(drift)) drift += period_ms;
      else if (std::abs(drift - period_ms) < drift) drift -= period_ms;

      auto total = task.drift_ms_at_capture + drift;
      total %= period_ms;
      return static_cast<int>(total);
    }

    void emit_event(events::Variant const& ev) {
      if (!callbacks_.on_event) return;
      std::lock_guard<std::mutex> lock(event_mutex_);
      callbacks_.on_event(ev);
    }

    void start_decode_worker() {
      lanes_[1].name = "ultra";
      lanes_[1].thread_name = "js8-decode-u";
      lanes_[1].log_prefix = "[ultra] ";
      for (int i = 0; i < (config_.ultra_own_thread ? 2 : 1); ++i) {
        DecodeLane& lane = lanes_[i];
        lane.stop = false;
        lane.thread = std::thread([this, &lane]() { decode_worker_loop(lane); });
      }
    }

    void stop_decode_worker() {
      for (auto& lane : lanes_) {
        {
          std::lock_guard<std::mutex> lock(lane.mutex);
          lane.stop = true;
          lane.queue.clear();
        }
        lane.cv.notify_one();
      }
      for (auto& lane : lanes_)
        if (lane.thread.joinable()) lane.thread.join();
    }

    void start_spectrum_worker() {
      spectrum_thread_ = std::thread([this]() { spectrum_worker_loop(); });
    }

    void stop_spectrum_worker() {
      {
        std::lock_guard<std::mutex> lock(spectrum_mutex_);
        spectrum_stop_ = true;
        spectrum_queue_.clear();
      }
      spectrum_cv_.notify_one();
      if (spectrum_thread_.joinable()) spectrum_thread_.join();
    }

    // x6100 patch 8: never drop a ready decode window. Upstream returned
    // here while a decode was running, but isDecodeReady() has already
    // marked the window done, so with several speeds on, a Normal or Fast
    // slot falling due during a decode (Turbo retries every second) was
    // never decoded. Desktop JS8Call queues its ready windows instead. Keep
    // at most one snapshot waiting behind the running decode: a newer one
    // takes over the waiting one (fresher audio; the ring still holds the
    // older windows) along with any speed's window only the older one had.
    static void merge_windows(DecodeParams& into, DecodeParams const& older) {
      struct Win { int bit; int DecodeParams::*pos; int DecodeParams::*sz; };
      static constexpr Win wins[] = {
          {1 << static_cast<int>(protocol::SubmodeId::A), &DecodeParams::kposA, &DecodeParams::kszA},
          {1 << static_cast<int>(protocol::SubmodeId::B), &DecodeParams::kposB, &DecodeParams::kszB},
          {1 << static_cast<int>(protocol::SubmodeId::C), &DecodeParams::kposC, &DecodeParams::kszC},
          {1 << static_cast<int>(protocol::SubmodeId::E), &DecodeParams::kposE, &DecodeParams::kszE},
          {1 << static_cast<int>(protocol::SubmodeId::I), &DecodeParams::kposI, &DecodeParams::kszI},
      };
      for (auto const& w : wins) {
        if ((older.nsubmodes & w.bit) == 0 || (into.nsubmodes & w.bit) != 0) continue;
        into.*w.pos = older.*w.pos;
        into.*w.sz  = older.*w.sz;
        into.nsubmodes |= w.bit;
      }
    }

    void enqueue_decode(DecodeState snapshot) {
      // Patch 15: Ultra's window goes to its own thread, the rest as before.
      if (config_.ultra_own_thread && (snapshot.params.nsubmodes & kUltraBit)) {
        if ((snapshot.params.nsubmodes & ~kUltraBit) == 0) {
          enqueue_lane(lanes_[1], std::move(snapshot));
          return;
        }
        DecodeState ultra;
        ultra.params = snapshot.params;
        ultra.params.nsubmodes = kUltraBit;
        ultra.samples = snapshot.samples;
        ultra.drift_ms_at_capture = snapshot.drift_ms_at_capture;
        snapshot.params.nsubmodes &= ~kUltraBit;
        enqueue_lane(lanes_[1], std::move(ultra));
      }
      enqueue_lane(lanes_[0], std::move(snapshot));
    }

    void enqueue_lane(DecodeLane& lane, DecodeState snapshot) {
      {
        std::lock_guard<std::mutex> lock(lane.mutex);
        if (!lane.queue.empty()) {
          merge_windows(snapshot.params, lane.queue.back().params);
          lane.queue.back() = std::move(snapshot);
          // x6100 patch 13: how often the decoder is still busy when the
          // next window is ready (the app counts these).
          if (callbacks_.on_log) {
            std::string m = std::string(lane.log_prefix) + "decode window merged: the decoder was busy";
            callbacks_.on_log(LogLevel::Info, m);
          }
        } else {
          lane.queue.push_back(std::move(snapshot));
        }
      }
      lane.cv.notify_one();
    }

    void enqueue_spectrum(const std::int16_t* data,
                          std::size_t frames,
                          int channels,
                          int sample_rate) {
      if (!data || frames == 0 || sample_rate <= 0) return;
      std::size_t const max_frames = std::min<std::size_t>(frames, 4096);
      SpectrumTask task;
      task.frames = max_frames;
      task.channels = 1;
      task.sample_rate = sample_rate;
      task.samples.resize(max_frames);
      for (std::size_t i = 0; i < max_frames; ++i) {
        task.samples[i] = data[i * channels];
      }
      {
        std::lock_guard<std::mutex> lock(spectrum_mutex_);
        spectrum_queue_.push_back(std::move(task));
      }
      spectrum_cv_.notify_one();
    }

    void decode_worker_loop(DecodeLane& lane) {
#if defined(__linux__)
      // x6100 patch 13: named, so per-thread CPU tools can tell it apart.
      pthread_setname_np(pthread_self(), lane.thread_name);
#endif
      // x6100 patch 15: the host may pin or deprioritise this thread.
      if (callbacks_.on_decode_thread_start) callbacks_.on_decode_thread_start(lane.name);
      for (;;) {
        DecodeState task;
        {
          std::unique_lock<std::mutex> lock(lane.mutex);
          lane.cv.wait(lock, [&]() { return lane.stop || !lane.queue.empty(); });
          if (lane.stop) return;
          task = std::move(lane.queue.front());
          lane.queue.pop_front();
        }

        if (callbacks_.on_log) {
          char log_msg[512];
          snprintf(log_msg, sizeof(log_msg),
                   "%sCalling legacy_decode: nsubmodes=0x%x, freq_range=%d-%d Hz, nfqso=%d Hz, sample_rate=%d, buffer_size=%zu, callback=%s",
                   lane.log_prefix, task.params.nsubmodes, task.params.nfa, task.params.nfb, task.params.nfqso,
                   config_.sample_rate_hz, task.samples.size(),
                   callbacks_.on_event ? "SET" : "NULL");
          callbacks_.on_log(LogLevel::Info, log_msg);
        }

        std::size_t decode_count = legacy_decode(task, [this, &task](events::Variant const& ev) {
          if (auto const* d = std::get_if<events::Decoded>(&ev)) {
            auto out = *d;
            out.drift_ms = compute_drift_estimate(task, out);
            out.capture_drift_ms = static_cast<int>(task.drift_ms_at_capture);
            emit_event(events::Variant{std::move(out)});
            return;
          }
          emit_event(ev);
        });

        if (callbacks_.on_log) {
          char log_msg[256];
          snprintf(log_msg, sizeof(log_msg),
                   "%slegacy_decode returned: %zu decodes", lane.log_prefix, decode_count);
          callbacks_.on_log(LogLevel::Info, log_msg);
        }
      }
    }

    void spectrum_worker_loop() {
      for (;;) {
        SpectrumTask task;
        {
          std::unique_lock<std::mutex> lock(spectrum_mutex_);
          spectrum_cv_.wait(lock, [&]() { return spectrum_stop_ || !spectrum_queue_.empty(); });
          if (spectrum_stop_) return;
          task = std::move(spectrum_queue_.front());
          spectrum_queue_.pop_front();
        }

        auto& spec = std::get<events::Spectrum>(spectrum_event_);
        if (compute_spectrum(task.samples.data(),
                             task.frames,
                             task.channels,
                             task.sample_rate,
                             spec)) {
          emit_event(spectrum_event_);
        }
      }
    }
  };

}  // namespace

std::unique_ptr<Js8Engine> make_engine(EngineConfig const& config,
                                       EngineCallbacks callbacks,
                                       EngineDependencies deps) {
  return std::make_unique<Js8EngineImpl>(config, std::move(callbacks), deps);
}

}  // namespace js8core
