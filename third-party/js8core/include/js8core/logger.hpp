#pragma once

#include <functional>
#include <string_view>

#include "js8core/types.hpp"

namespace js8core {

using LogSink = std::function<void(LogLevel level, std::string_view message)>;

class Logger {
public:
  virtual ~Logger() = default;
  virtual void log(LogLevel level, std::string_view message) = 0;
};

}  // namespace js8core
