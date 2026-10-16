#include "js8core/protocol/costas.hpp"

namespace js8core::protocol {

namespace {

constexpr auto kCostas = std::array{
    // Original
    CostasArray{{
        {{4, 2, 5, 6, 1, 3, 0}},
        {{4, 2, 5, 6, 1, 3, 0}},
        {{4, 2, 5, 6, 1, 3, 0}},
    }},
    // Modified
    CostasArray{{
        {{0, 6, 2, 3, 5, 4, 1}},
        {{1, 5, 0, 2, 3, 6, 4}},
        {{2, 5, 0, 6, 4, 1, 3}},
    }},
};

}  // namespace

CostasArray const& costas(CostasType type) {
  return kCostas[static_cast<std::size_t>(type)];
}

}  // namespace js8core::protocol
