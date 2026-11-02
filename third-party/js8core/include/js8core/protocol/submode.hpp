#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace js8core::protocol {

enum class SubmodeId { A, B, C, E, I };

struct Submode {
  SubmodeId id;
  std::string_view name;
  int symbol_samples;
  int tx_seconds;
  int start_delay_ms;
  bool enabled;
};

// Returns the known submodes with compile-time defaults.
std::vector<Submode> submodes();

std::optional<Submode> find(SubmodeId id);
std::optional<Submode> find(std::string_view name);

}  // namespace js8core::protocol
