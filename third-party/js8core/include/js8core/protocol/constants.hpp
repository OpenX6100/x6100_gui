#pragma once

namespace js8core::protocol {

// Sample rate and symbol constants (mirrors legacy commons.h defines).
inline constexpr int kJs8Nsps = 6192;
inline constexpr int kJs8NsMax = 6827;
inline constexpr int kJs8NtMax = 60;
inline constexpr int kJs8RxSampleRate = 12000;
inline constexpr int kJs8NumSymbols = 79;

// Submode-specific parameters.
inline constexpr int kJs8aSymbolSamples = 1920;
inline constexpr int kJs8aTxSeconds = 15;
inline constexpr int kJs8aStartDelayMs = 500;

inline constexpr int kJs8bSymbolSamples = 1200;
inline constexpr int kJs8bTxSeconds = 10;
inline constexpr int kJs8bStartDelayMs = 200;

inline constexpr int kJs8cSymbolSamples = 600;
inline constexpr int kJs8cTxSeconds = 6;
inline constexpr int kJs8cStartDelayMs = 100;

inline constexpr int kJs8eSymbolSamples = 3840;
inline constexpr int kJs8eTxSeconds = 30;
inline constexpr int kJs8eStartDelayMs = 500;

inline constexpr int kJs8iSymbolSamples = 384;
inline constexpr int kJs8iTxSeconds = 4;
inline constexpr int kJs8iStartDelayMs = 100;

}  // namespace js8core::protocol
