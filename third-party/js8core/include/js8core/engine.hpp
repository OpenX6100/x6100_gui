#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "js8core/audio.hpp"
#include "js8core/clock.hpp"
#include "js8core/protocol/varicode.hpp"
#include "js8core/logger.hpp"
#include "js8core/network.hpp"
#include "js8core/rig.hpp"
#include "js8core/storage.hpp"
#include "js8core/types.hpp"
#include "js8core/protocol/varicode.hpp"

namespace js8core {

namespace events {

struct DecodeStarted {
  int submodes = 0;
};

struct SyncStart {
  int position = 0;
  int size = 0;
};

struct SyncState {
  enum class Kind { Candidate, Decoded } kind = Kind::Candidate;
  int mode = 0;
  float frequency = 0.0f;
  float dt = 0.0f;
  union {
    int candidate;
    float decoded;
  } sync {0};
};

struct Decoded {
  int utc = 0;
  int snr = 0;
  float xdt = 0.0f;
  float frequency = 0.0f;
  std::string data;
  int type = 0;
  float quality = 0.0f;
  int mode = 0;
  // Suggested total drift (ms) to center this signal's cycle.
  int drift_ms = 0;
  // Drift (ms) the ring was aligned with when this audio was captured; the
  // signal is on time with drift capture_drift_ms - 1000 * xdt.
  int capture_drift_ms = 0;
};

struct DecodeFinished {
  std::size_t decoded = 0;
  int submodes = 0;  // x6100 patch 15: the pass's submodes (which decode thread)
};

struct Spectrum {
  std::vector<float> bins;
  float bin_hz = 0.0f;
  float power_db = 0.0f;
  float peak_db = 0.0f;
};

using Variant = std::variant<DecodeStarted, SyncStart, SyncState, Decoded, DecodeFinished, Spectrum>;

}  // namespace events

struct EngineConfig {
  int sample_rate_hz = 0;
  int submodes = 0;
  int tx_output_rate_hz = 48000;
  float tx_output_gain = 1.0f;
  bool tx_output_gain_boost_enabled = false;
  // Hosts that draw their own waterfall can skip the engine's spectrum
  // thread and its per-buffer FFT.
  bool spectrum_enabled = true;
  // x6100 patch 15: decode submode I (Ultra) on a second decode thread, so
  // its 4 s slots never wait behind a pass of the slower submodes (and
  // theirs never behind it).
  bool ultra_own_thread = false;
};

struct TxMessageRequest {
  std::string text;
  std::string my_call;
  std::string my_grid;
  std::string selected_call;
  int submode = 0;
  double audio_frequency_hz = 0.0;
  double tx_delay_s = 0.0;
  bool force_identify = false;
  bool force_data = false;
};

struct TxFrameRequest {
  std::string frame;
  int bits = 0;
  int submode = 0;
  double audio_frequency_hz = 0.0;
  double tx_delay_s = 0.0;
};

struct EngineCallbacks {
  std::function<void(events::Variant const&)> on_event;
  std::function<void(std::string_view message)> on_error;
  std::function<void(LogLevel level, std::string_view message)> on_log;
  // x6100 patch 15: called on each decode thread as it starts ("main", or
  // "ultra" with EngineConfig::ultra_own_thread), so the host can set its
  // CPU affinity or priority.
  std::function<void(std::string_view lane)> on_decode_thread_start;
};

struct EngineDependencies {
  AudioInput* audio_in = nullptr;
  AudioOutput* audio_out = nullptr;
  RigControl* rig = nullptr;
  Scheduler* scheduler = nullptr;
  Storage* storage = nullptr;
  Logger* logger = nullptr;
  UdpChannel* udp = nullptr;
};

class Js8Engine {
public:
  virtual ~Js8Engine() = default;

  virtual bool start() = 0;
  virtual void stop() = 0;

  virtual bool submit_capture(AudioInputBuffer const& buffer) = 0;

  virtual bool transmit_message(TxMessageRequest const& request) = 0;
  virtual bool transmit_frame(TxFrameRequest const& request) = 0;
  virtual bool start_tune(double audio_frequency_hz, int submode, double tx_delay_s) = 0;
  virtual void stop_transmit() = 0;
  virtual bool is_transmitting() const = 0;
  virtual bool is_transmitting_audio() const = 0;
  virtual int tx_milliseconds_until_audio() const = 0;
  virtual void set_tx_ready(bool ready) = 0;
  virtual void set_tx_boost_enabled(bool enabled) = 0;
  virtual void set_submodes(int submodes) = 0;

  // x6100 patch 9: the audio range searched (desktop: its waterfall filter
  // edges, or 0-5000 Hz) and our own offset, whose neighbours are decoded
  // first (desktop passes freq() as nfqso). Take effect at the next decode.
  virtual void set_decode_range(int low_hz, int high_hz) = 0;
  virtual void set_qso_offset(int offset_hz) = 0;

  // x6100 patch 12: emit events::SyncState for every sync candidate and
  // decode (desktop's "Show decode attempts" sets syncStats the same way).
  // Off by default; takes effect at the next decode.
  virtual void set_sync_stats(bool enabled) = 0;

  // Positive = engine clock ahead of system clock; takes effect at the next captured buffer.
  virtual void set_time_drift_ms(std::int64_t drift_ms) = 0;
  virtual std::int64_t time_drift_ms() const = 0;

  // Re-snap the RX ring to the wall clock at the next captured buffer, e.g.
  // after the host notices its sample count has slipped against real time.
  virtual void request_realign() = 0;
};

std::unique_ptr<Js8Engine> make_engine(EngineConfig const& config,
                                       EngineCallbacks callbacks,
                                       EngineDependencies deps);

}  // namespace js8core
