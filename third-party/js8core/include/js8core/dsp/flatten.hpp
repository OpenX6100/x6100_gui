#pragma once

#include <cstddef>
#include <memory>

namespace js8core::dsp {

// Non-reentrant spectrum flattener; reuse serially.
class Flatten {
public:
  explicit Flatten(bool enabled = false);
  ~Flatten();

  void operator()(bool value);
  void operator()(float* data, std::size_t size);

  explicit operator bool() const noexcept { return !!impl_; }
  bool live() const noexcept { return !!impl_; }

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace js8core::dsp
