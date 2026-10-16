#include "js8core/protocol/submode.hpp"

#include <algorithm>

namespace js8core::protocol {

namespace {

constexpr Submode kDefaults[] = {
    {SubmodeId::A, "A", 1920, 15, 500, true},
    {SubmodeId::B, "B", 1200, 10, 200, true},
    {SubmodeId::C, "C", 600, 6, 100, true},
    {SubmodeId::E, "E", 3840, 30, 500, true},
    {SubmodeId::I, "I", 384, 4, 100, false},  // disabled by default
};

}  // namespace

std::vector<Submode> submodes() {
  return std::vector<Submode>(std::begin(kDefaults), std::end(kDefaults));
}

std::optional<Submode> find(SubmodeId id) {
  auto modes = submodes();
  auto it = std::find_if(modes.begin(), modes.end(), [id](Submode const& m) { return m.id == id; });
  if (it != modes.end()) return *it;
  return std::nullopt;
}

std::optional<Submode> find(std::string_view name) {
  auto modes = submodes();
  auto it = std::find_if(modes.begin(), modes.end(),
                         [name](Submode const& m) { return m.name == name; });
  if (it != modes.end()) return *it;
  return std::nullopt;
}

}  // namespace js8core::protocol
