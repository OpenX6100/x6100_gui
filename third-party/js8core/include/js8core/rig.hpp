#pragma once

#include <functional>
#include <string_view>

#include "js8core/types.hpp"

namespace js8core {

using RigStateHandler = std::function<void(RigState const&)>;
using RigErrorHandler = std::function<void(std::string_view message)>;

class RigControl {
public:
  virtual ~RigControl() = default;
  virtual bool start(RigStateHandler on_state, RigErrorHandler on_error) = 0;
  virtual void stop() = 0;
  // apply should be non-blocking; updates are surfaced via on_state callbacks.
  virtual void apply(RigState const& desired, unsigned sequence_number) = 0;
  virtual void request_status(unsigned sequence_number) = 0;
};

}  // namespace js8core
