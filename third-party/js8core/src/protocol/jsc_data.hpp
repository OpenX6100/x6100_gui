#pragma once

#include <cstdint>

struct JscTuple {
  char const* str;
  int size;
  int index;
};

extern JscTuple const JSC_MAP[262144];
extern JscTuple const JSC_LIST[262144];
extern int const JSC_PREFIX_SIZE;
extern JscTuple const JSC_PREFIX[103];
extern std::uint32_t const JSC_SIZE;
