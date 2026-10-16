#pragma once

#include <array>
#include <cstddef>

namespace js8core::protocol {

enum class CostasType { Original, Modified };

using CostasArray = std::array<std::array<int, 7>, 3>;

CostasArray const& costas(CostasType type);

}  // namespace js8core::protocol
