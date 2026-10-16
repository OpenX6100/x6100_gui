#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace js8core {

// Core decoder constants mirrored from legacy commons.h
constexpr int kJs8Nsps = 6192;
constexpr int kJs8NsMax = 6827;
constexpr int kJs8NtMax = 60;
constexpr int kJs8RxSampleRate = 12000;
constexpr int kJs8NumSymbols = 79;

struct DecodeParams {
  int utc = 0;
  int nfqso = 0;
  bool newdat = false;
  int nfa = 0;
  int nfb = 0;
  bool syncStats = false;
  int kin = 0;
  int kposA = 0;
  int kposB = 0;
  int kposC = 0;
  int kposE = 0;
  int kposI = 0;
  int kszA = 0;
  int kszB = 0;
  int kszC = 0;
  int kszE = 0;
  int kszI = 0;
  int nsubmodes = 0;
};

struct DecodeState {
  std::vector<std::int16_t> samples;  // size kJs8NtMax * kJs8RxSampleRate
  DecodeParams params;
  // Drift the ring alignment used at snapshot time; estimates must use this, not current drift.
  std::int64_t drift_ms_at_capture = 0;
};

struct SpectrumState {
  float savg[kJs8NsMax]{};
  float slin[kJs8NsMax]{};
};

}  // namespace js8core
