#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace js8core::protocol::jsc {

using Codeword = std::vector<bool>;
using CodewordPair = std::pair<Codeword, std::uint32_t>;  // bits, size

Codeword codeword(std::uint32_t index, bool separate, std::uint32_t bytesize, std::uint32_t s, std::uint32_t c);
std::vector<CodewordPair> compress(std::string const& text);
std::string decompress(Codeword const& bits);

bool exists(std::string const& w, std::uint32_t* pIndex);
std::uint32_t lookup(std::string const& w, bool* ok);
std::uint32_t lookup(const char* b, bool* ok);

}  // namespace js8core::protocol::jsc
