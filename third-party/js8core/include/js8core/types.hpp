#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace js8core {

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using SteadyTimePoint = SteadyClock::time_point;
using SteadyDuration = SteadyClock::duration;
using WallTimePoint = SystemClock::time_point;

using FrequencyHz = std::uint64_t;
using FrequencyDeltaHz = std::int64_t;

enum class SampleType { Int16, Float32 };

struct AudioFormat {
  int sample_rate = 0;
  int channels = 0;
  SampleType sample_type = SampleType::Int16;
};

enum class Mode { Unk, CW, CW_R, USB, LSB, FSK, FSK_R, DIG_U, DIG_L, AM, FM, DIG_FM };

enum class Split { Unknown, Off, On };

struct RigState {
  bool online = false;
  FrequencyHz rx_frequency = 0;
  FrequencyHz tx_frequency = 0;
  Mode mode = Mode::Unk;
  Split split = Split::Unknown;
  bool ptt = false;
};

enum class LogLevel { Trace, Debug, Info, Warn, Error };

}  // namespace js8core
