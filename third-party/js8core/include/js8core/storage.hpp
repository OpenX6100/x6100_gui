#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace js8core {

class Storage {
public:
  virtual ~Storage() = default;
  virtual bool put(std::string_view key, std::span<const std::byte> value) = 0;
  virtual bool get(std::string_view key, std::vector<std::byte>& out) const = 0;
  virtual bool erase(std::string_view key) = 0;
};

}  // namespace js8core
