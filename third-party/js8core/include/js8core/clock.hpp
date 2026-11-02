#pragma once

#include <functional>

#include "js8core/types.hpp"

namespace js8core {

using TimerHandle = std::uint64_t;

class Scheduler {
public:
  virtual ~Scheduler() = default;
  virtual SteadyTimePoint now() const = 0;
  virtual TimerHandle call_after(SteadyDuration delay, std::function<void()> fn) = 0;
  virtual TimerHandle call_every(SteadyDuration period, std::function<void()> fn) = 0;
  virtual void cancel(TimerHandle handle) = 0;
};

}  // namespace js8core
