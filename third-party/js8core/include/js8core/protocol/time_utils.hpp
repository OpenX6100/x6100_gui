#pragma once

#include <cstdint>

namespace js8core::protocol {

inline int code_time(int hour, int minute, int second) {
  return hour * 10000 + minute * 100 + second;
}

struct HourMinuteSecond {
  int hour;
  int minute;
  int second;
};

inline HourMinuteSecond decode_time(int nutc) {
  HourMinuteSecond result{};
  result.hour = nutc / 10000;
  result.minute = nutc % 10000 / 100;
  result.second = nutc % 100;
  return result;
}

}  // namespace js8core::protocol
