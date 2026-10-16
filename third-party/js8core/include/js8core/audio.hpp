#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "js8core/types.hpp"

namespace js8core {

struct AudioDeviceInfo {
  std::string id;
  std::string name;
  std::vector<AudioFormat> supported_formats;
};

struct AudioStreamParams {
  AudioFormat format;
  std::size_t frames_per_buffer = 0;
};

// Buffers are expected to use interleaved channel samples in the native
// platform endianness (little-endian for Android and desktop targets).
struct AudioInputBuffer {
  std::span<const std::byte> data;
  AudioFormat format;
  SteadyTimePoint captured_at;
};

struct AudioOutputBuffer {
  std::span<std::byte> data;
  AudioFormat format;
  SteadyTimePoint playback_at;
};

using AudioInputHandler = std::function<void(AudioInputBuffer const&)>;
using AudioOutputFill = std::function<std::size_t(AudioOutputBuffer&)>;
using AudioErrorHandler = std::function<void(std::string_view message)>;

class AudioInput {
public:
  virtual ~AudioInput() = default;
  virtual bool start(AudioStreamParams const& params,
                     AudioInputHandler on_frames,
                     AudioErrorHandler on_error) = 0;
  virtual void stop() = 0;
};

class AudioOutput {
public:
  virtual ~AudioOutput() = default;
  virtual bool start(AudioStreamParams const& params,
                     AudioOutputFill fill,
                     AudioErrorHandler on_error) = 0;
  virtual void stop() = 0;
};

}  // namespace js8core
