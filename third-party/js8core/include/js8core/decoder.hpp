#pragma once

#include <functional>

#include "js8core/decoder_state.hpp"
#include "js8core/engine.hpp"
#include "js8core/protocol/costas.hpp"

namespace js8core {

// Qt-free legacy decoder entry point (moved from JS8.cpp).
std::size_t legacy_decode(DecodeState const& state,
                          std::function<void(events::Variant const&)> emit);

void legacy_encode(int type,
                   protocol::CostasArray const& costas,
                   char const* message,
                   int* tones);

}  // namespace js8core
