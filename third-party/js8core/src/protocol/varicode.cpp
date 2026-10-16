#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cmath>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "varicode", __VA_ARGS__)
#else
#define LOGD(...) do {} while(0)
#endif
#include <cstring>
#include <functional>
#include <numeric>
#include <regex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#define CRCPP_INCLUDE_ESOTERIC_CRC_DEFINITIONS
#define CRCPP_USE_CPP11
#include <vendor/CRCpp/CRC.h>

#include "js8core/protocol/jsc.hpp"

namespace js8core::protocol::varicode {

namespace {

using namespace std::string_literals;

const std::string kAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";
const std::string kAlphabet72 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?."s;
const std::string kAlphanumeric = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@"s;
const int kNAlphabet = 41;

// Huffman table for core JS8 text
const HuffTable kHuffTable = {
    {" ", "01"},       {"E", "100"},     {"T", "1101"},   {"A", "0011"},
    {"O", "11111"},    {"I", "11100"},   {"N", "10111"},  {"S", "10100"},
    {"H", "00011"},    {"R", "00000"},   {"D", "111011"}, {"L", "110011"},
    {"C", "110001"},   {"U", "101101"},  {"M", "101011"}, {"W", "001011"},
    {"F", "001001"},   {"G", "000101"},  {"Y", "000011"}, {"P", "1111011"},
    {"B", "1111001"},  {".", "1110100"}, {"V", "1100101"},{"K", "1100100"},
    {"-", "1100001"},  {"+", "1100000"}, {"?", "1011001"},{"!", "1011000"},
    {"\"", "1010101"}, {"X", "1010100"}, {"0", "0010101"},{"J", "0010100"},
    {"1", "0010001"},  {"Q", "0010000"}, {"2", "0001001"},{"Z", "0001000"},
    {"3", "0000101"},  {"5", "0000100"}, {"4", "11110101"},{"9","11110100"},
    {"8","11110001"},  {"6","11110000"}, {"7","11101011"},{"/","11101010"},
};

const std::unordered_set<int> kAllowedCmds = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
const std::unordered_set<int> kAutoreplyCmds = {0, 2, 3, 4, 6, 9, 10, 11, 12, 13, 14, 16, 30};
const std::unordered_set<int> kBufferedCmds = {5, 9, 10, 11, 12, 13, 15, 24};
const std::unordered_set<int> kSnrCmds = {25, 29};

const std::unordered_map<int, int> kChecksumCmds = {
    {5, 16},  {9, 16},  {10, 16}, {11, 16},
    {12, 16}, {13, 16}, {15, 0},  {24, 16},
};

struct DirectedCmd {
  std::string key;
  int value;
};

const std::vector<DirectedCmd> kDirectedCmds = {
    {" HEARTBEAT", -1}, {" HB", -1}, {" CQ", -1}, {" SNR?", 0}, {"?", 0},
    {" DIT DIT", 1}, {" HEARING?", 3}, {" GRID?", 4}, {">", 5}, {" STATUS?", 6},
    {" STATUS", 7}, {" HEARING", 8}, {" MSG", 9}, {" MSG TO:", 10}, {" QUERY", 11},
    {" QUERY MSGS", 12}, {" QUERY MSGS?", 12}, {" QUERY CALL", 13},
    {" GRID", 15}, {" INFO?", 16}, {" INFO", 17}, {" FB", 18}, {" HW CPY?", 19},
    {" SK", 20}, {" RR", 21}, {" QSL?", 22}, {" QSL", 23}, {" CMD", 24},
    {" SNR", 25}, {" NO", 26}, {" YES", 27}, {" 73", 28}, {" NACK", 2},
    {" ACK", 14}, {" HEARTBEAT SNR", 29}, {" AGN?", 30}, {"  ", 31}, {" ", 31},
};

const std::unordered_map<std::string, int> kBaseCalls = [] {
  std::unordered_map<std::string, int> out;
  int nbasecall = 37 * 36 * 10 * 27 * 27 * 27;
  std::vector<std::string> keys = {
      "<....>", "@ALLCALL", "@JS8NET",
      "@DX/NA", "@DX/SA", "@DX/EU", "@DX/AS", "@DX/AF", "@DX/OC", "@DX/AN",
      "@REGION/1", "@REGION/2", "@REGION/3",
      "@GROUP/0","@GROUP/1","@GROUP/2","@GROUP/3","@GROUP/4",
      "@GROUP/5","@GROUP/6","@GROUP/7","@GROUP/8","@GROUP/9",
      "@COMMAND","@CONTROL","@NET","@NTS",
      "@RESERVE/0","@RESERVE/1","@RESERVE/2","@RESERVE/3","@RESERVE/4",
      "@APRSIS","@RAGCHEW","@JS8","@EMCOMM","@ARES","@MARS","@AMRRON","@RACES","@RAYNET","@RADAR","@SKYWARN","@CQ","@HB","@QSO","@QSOPARTY","@CONTEST","@FIELDDAY","@SOTA","@IOTA","@POTA","@QRP","@QRO"
  };
  for (std::size_t i = 0; i < keys.size(); ++i) out.emplace(keys[i], nbasecall + static_cast<int>(i) + 1);
  return out;
}();

const std::vector<std::pair<int, std::string>> kCqs = {
    {0, "CQ CQ CQ"}, {1, "CQ DX"}, {2, "CQ QRP"}, {3, "CQ CONTEST"},
    {4, "CQ FIELD"}, {5, "CQ FD"}, {6, "CQ CQ"}, {7, "CQ"},
};

const std::vector<std::pair<int, std::string>> kHbs = {
    {0, "HB"}, {1, "HB"}, {2, "HB"}, {3, "HB"}, {4, "HB"}, {5, "HB"}, {6, "HB"}, {7, "HB"},
};

enum class FrameType : std::uint8_t {
  FrameHeartbeat        = 0,
  FrameCompound         = 1,
  FrameCompoundDirected = 2,
  FrameDirected         = 3,
  FrameData             = 4,
  FrameDataCompressed   = 6,
};

constexpr std::uint16_t nbasegrid = 180 * 180;
constexpr std::uint16_t nusergrid = static_cast<std::uint16_t>(nbasegrid + 10);
constexpr std::uint16_t nmaxgrid = static_cast<std::uint16_t>((1 << 15) - 1);

// Simplified regexes to keep compilation working; behavior parity can be tuned later.
const std::regex kGridPattern(R"(^[A-R]{2}[0-9]{2}([A-X]{2})?$)", std::regex::icase);
// Capture groups: 1=type, 2=grid
const std::regex kHeartbeatRe(
    R"(^\s*(?:[@](?:ALLCALL|HB)\s+)?(CQ CQ CQ|CQ DX|CQ QRP|CQ CONTEST|CQ FIELD|CQ FD|CQ CQ|CQ|HB ALT|HB|HEARTBEAT(?!\s+SNR))(?:\s+([A-R]{2}[0-9]{2}))?.*$)",
    std::regex::icase);
// Capture groups: 1=callsign, 2=grid, 3=cmd, 4=num
const std::regex kCompoundRe(
    R"(^\s*[`]([A-Z0-9@/]+)(?:\s+([A-R]{2}[0-9]{2}(?:[A-X]{2})?))?(\s?(?:AGN[?]|QSL[?]|HW CPY[?]|MSG TO[:]|SNR[?]|INFO[?]|GRID[?]|STATUS[?]|QUERY MSGS[?]|HEARING[?]|(?:(?:STATUS|HEARING|QUERY CALL|QUERY MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT)(?=[ ]|$))|[?> ]))?(\s*[+-]?\d{1,3})?\s*$)",
    std::regex::icase);
// Capture groups: 1=to, 2=cmd, 3=num
const std::regex kDirectedRe(
    R"(^\s*([A-Z0-9@/]+):?(\s?(?:AGN[?]|QSL[?]|HW CPY[?]|MSG TO[:]|SNR[?]|INFO[?]|GRID[?]|STATUS[?]|QUERY MSGS[?]|HEARING[?]|(?:(?:STATUS|HEARING|QUERY CALL|QUERY MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT)(?=[ ]|$))|[?> ]))?(\s*[+-]?\d{1,3})?)",
    std::regex::icase);

inline std::uint8_t pack_num_qtstyle(std::string const& num, bool* ok) {
  try {
    int val = std::stoi(num);
    if (val < -30) val = -30;
    if (val > 31) val = 31;
    if (ok) *ok = true;
    return static_cast<std::uint8_t>(val + 31);
  } catch (...) {
    if (ok) *ok = false;
    return 0;
  }
}

const std::unordered_map<int, std::string> kDbm2mwStr = {
    {0, "1mW"},       {3, "2mW"},       {7, "5mW"},       {10, "10mW"},
    {13, "20mW"},     {17, "50mW"},     {20, "100mW"},    {23, "200mW"},
    {27, "500mW"},    {30, "1W"},       {33, "2W"},       {37, "5W"},
    {40, "10W"},      {43, "20W"},      {47, "50W"},      {50, "100W"},
    {53, "200W"},     {57, "500W"},     {60, "1000W"},
};

int mwattsToDbm(int mwatts) {
  static const std::vector<std::pair<int, int>> dbm2mw = {
      {0, 1},   {3, 2},    {7, 5},    {10, 10},   {13, 20},   {17, 50},
      {20, 100},{23, 200}, {27, 500}, {30, 1000}, {33, 2000}, {37, 5000},
      {40, 10000},{43, 20000},{47, 50000},{50, 100000},{53, 200000},{57, 500000},{60, 1000000},
  };
  for (auto const& p : dbm2mw) {
    if (p.second >= mwatts) return p.first;
  }
  return dbm2mw.back().first;
}

int dbmTomwatts(int dbm) {
  static const std::vector<std::pair<int, int>> dbm2mw = {
      {0, 1},   {3, 2},    {7, 5},    {10, 10},   {13, 20},   {17, 50},
      {20, 100},{23, 200}, {27, 500}, {30, 1000}, {33, 2000}, {37, 5000},
      {40, 10000},{43, 20000},{47, 50000},{50, 100000},{53, 200000},{57, 500000},{60, 1000000},
  };
  for (auto const& p : dbm2mw) {
    if (p.first == dbm) return p.second;
  }
  auto it = std::lower_bound(dbm2mw.begin(), dbm2mw.end(), dbm,
                             [](auto const& pair, int val) { return pair.first < val; });
  if (it == dbm2mw.end()) return dbm2mw.back().second;
  return it->second;
}

// Desktop's Varicode::formatSNR(): "+05", "-08"; empty outside -60..+60
// (local patch 7; this was unpadded and unchecked).
std::string format_snr(int snr) {
  if (snr < -60 || snr > 60) return {};
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%s%0*d", snr >= 0 ? "+" : "", snr < 0 ? 3 : 2, snr);
  return buf;
}

std::string trimmed_left(std::string s) {
  auto it = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
  s.erase(s.begin(), it);
  return s;
}

std::string trimmed_right(std::string s) {
  auto it = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  s.erase(it, s.end());
  return s;
}

}  // namespace

HuffTable default_huff_table() { return kHuffTable; }

std::string escape(std::string const& text) {
  std::string out;
  out.reserve(text.size() * 6);
  for (unsigned char c : text) {
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      out += "\\U";
      char buf[5]{};
      std::snprintf(buf, sizeof(buf), "%04x", c);
      out += buf;
    }
  }
  return out;
}

std::string unescape(std::string const& text) {
  std::regex re(R"(([\\][uU])[0-9a-fA-F]{4})");
  std::string out;
  auto begin = std::sregex_iterator(text.begin(), text.end(), re);
  auto end = std::sregex_iterator();
  std::size_t last = 0;
  for (auto i = begin; i != end; ++i) {
    auto m = *i;
    out.append(text.substr(last, m.position() - last));
    auto hex = text.substr(m.position() + 2, 4);
    char16_t code = static_cast<char16_t>(std::stoi(hex, nullptr, 16));
    if (code < 0x80) out.push_back(static_cast<char>(code));
    last = m.position() + m.length();
  }
  out.append(text.substr(last));
  return out;
}

std::string rstrip(std::string const& text) { return trimmed_right(text); }
std::string lstrip(std::string const& text) { return trimmed_left(text); }

std::string checksum16(std::string const& input) {
  auto crc = CRC::Calculate(input.data(), input.size(), CRC::CRC_16_KERMIT());
  auto packed = pack16bits(static_cast<std::uint16_t>(crc));
  if (packed.size() < 3) packed.append(3 - packed.size(), ' ');
  return packed;
}

bool checksum16_valid(std::string const& checksum, std::string const& input) {
  auto crc = CRC::Calculate(input.data(), input.size(), CRC::CRC_16_KERMIT());
  return pack16bits(static_cast<std::uint16_t>(crc)) == checksum;
}

std::string checksum32(std::string const& input) {
  auto crc = CRC::Calculate(input.data(), input.size(), CRC::CRC_32_BZIP2());
  auto packed = pack32bits(static_cast<std::uint32_t>(crc));
  if (packed.size() < 6) packed.append(6 - packed.size(), ' ');
  return packed;
}

bool checksum32_valid(std::string const& checksum, std::string const& input) {
  auto crc = CRC::Calculate(input.data(), input.size(), CRC::CRC_32_BZIP2());
  return pack32bits(static_cast<std::uint32_t>(crc)) == checksum;
}

HuffEncoded huff_encode(HuffTable const& table, std::string const& text) {
  HuffEncoded out;
  // Sort keys longest-first to match original greedy behavior.
  std::vector<std::string> keys;
  keys.reserve(table.size());
  for (auto const& kv : table) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end(), [](auto const& a, auto const& b) {
    if (a.size() != b.size()) return a.size() > b.size();
    return a > b;
  });
  std::size_t i = 0;
  while (i < text.size()) {
    bool found = false;
    for (auto const& key : keys) {
      if (text.compare(i, key.size(), key) == 0) {
        std::vector<bool> bits;
        bits.reserve(table.at(key).size());
        for (char b : table.at(key)) bits.push_back(b == '1');
        out.emplace_back(static_cast<int>(key.size()), std::move(bits));
        i += key.size();
        found = true;
        break;
      }
    }
    if (!found) ++i;
  }
  return out;
}

std::string huff_decode(HuffTable const& table, std::vector<bool> const& bits) {
  // Build reverse map code->char
  std::unordered_map<std::string, std::string> rev;
  for (auto const& kv : table) rev.emplace(kv.second, kv.first);
  std::string bitstr;
  bitstr.reserve(bits.size());
  for (bool b : bits) bitstr.push_back(b ? '1' : '0');
  std::string text;
  std::size_t pos = 0;
  while (pos < bitstr.size()) {
    bool found = false;
    for (auto const& kv : rev) {
      auto const& code = kv.first;
      if (bitstr.compare(pos, code.size(), code) == 0) {
        text += kv.second;
        pos += code.size();
        found = true;
        break;
      }
    }
    if (!found) break;
  }
  return text;
}

std::unordered_set<std::string> huff_valid_chars(HuffTable const& table) {
  std::unordered_set<std::string> out;
  for (auto const& kv : table) out.insert(kv.first);
  return out;
}

std::vector<bool> bytes_to_bits(char* bitvec, int n) {
  std::vector<bool> bits;
  bits.reserve(n);
  for (int i = 0; i < n; ++i) bits.push_back(bitvec[i] == 0x01);
  return bits;
}

std::vector<bool> str_to_bits(std::string const& bitvec) {
  std::vector<bool> bits;
  bits.reserve(bitvec.size());
  for (char c : bitvec) bits.push_back(c == '1');
  return bits;
}

std::string bits_to_str(std::vector<bool> const& bitvec) {
  std::string bits;
  bits.reserve(bitvec.size());
  for (bool b : bitvec) bits.push_back(b ? '1' : '0');
  return bits;
}

std::vector<bool> int_to_bits(std::uint64_t value, int expected) {
  std::vector<bool> bits;
  while (value) {
    bits.insert(bits.begin(), static_cast<bool>(value & 1));
    value >>= 1;
  }
  if (expected) {
    while (static_cast<int>(bits.size()) < expected) bits.insert(bits.begin(), false);
  }
  return bits;
}

std::uint64_t bits_to_int(std::vector<bool> value) {
  std::uint64_t v = 0;
  for (bool bit : value) {
    v = (v << 1) + static_cast<int>(bit);
  }
  return v;
}

std::uint64_t bits_to_int(std::vector<bool>::const_iterator start, int n) {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) {
    v = (v << 1) + static_cast<int>(*(start + i));
  }
  return v;
}

std::vector<bool> bits_list_to_bits(std::vector<std::vector<bool>>& list) {
  std::vector<bool> out;
  std::size_t total = 0;
  for (auto const& v : list) total += v.size();
  out.reserve(total);
  for (auto const& v : list) out.insert(out.end(), v.begin(), v.end());
  return out;
}

std::uint8_t unpack5bits(std::string const& value) { return static_cast<std::uint8_t>(kAlphabet.find(value.at(0))); }
std::string pack5bits(std::uint8_t packed) { return std::string(1, kAlphabet.at(packed % 32)); }
std::uint8_t unpack6bits(std::string const& value) { return static_cast<std::uint8_t>(kAlphabet.find(value.at(0))); }
std::string pack6bits(std::uint8_t packed) { return std::string(1, kAlphabet.at(packed % 41)); }

std::uint16_t unpack16bits(std::string const& value) {
  if (value.size() < 3) return 0;
  int a = static_cast<int>(kAlphabet.find(value[0]));
  int b = static_cast<int>(kAlphabet.find(value[1]));
  int c = static_cast<int>(kAlphabet.find(value[2]));
  int unpacked = (kNAlphabet * kNAlphabet) * a + kNAlphabet * b + c;
  if (unpacked > ((1 << 16) - 1)) return 0;
  return static_cast<std::uint16_t>(unpacked & ((1 << 16) - 1));
}

std::string pack16bits(std::uint16_t packed) {
  std::string out;
  std::uint16_t tmp = packed / (kNAlphabet * kNAlphabet);
  out.push_back(kAlphabet.at(tmp));
  tmp = (packed - (tmp * (kNAlphabet * kNAlphabet))) / kNAlphabet;
  out.push_back(kAlphabet.at(tmp));
  tmp = packed % kNAlphabet;
  out.push_back(kAlphabet.at(tmp));
  return out;
}

std::uint32_t unpack32bits(std::string const& value) {
  if (value.size() < 6) return 0;
  return (static_cast<std::uint32_t>(unpack16bits(value.substr(0, 3))) << 16) |
         unpack16bits(value.substr(3, 3));
}

std::string pack32bits(std::uint32_t packed) {
  std::uint16_t a = static_cast<std::uint16_t>((packed & 0xFFFF0000) >> 16);
  std::uint16_t b = static_cast<std::uint16_t>(packed & 0xFFFF);
  return pack16bits(a) + pack16bits(b);
}

std::uint64_t unpack64bits(std::string const& value) {
  if (value.size() < 12) return 0;
  return (static_cast<std::uint64_t>(unpack32bits(value.substr(0, 6))) << 32) |
         unpack32bits(value.substr(6, 6));
}

std::string pack64bits(std::uint64_t packed) {
  std::uint32_t a = static_cast<std::uint32_t>((packed & 0xFFFFFFFF00000000ULL) >> 32);
  std::uint32_t b = static_cast<std::uint32_t>(packed & 0xFFFFFFFFULL);
  return pack32bits(a) + pack32bits(b);
}

std::uint64_t unpack72bits(std::string const& value, std::uint8_t* pRem) {
  // Frames are encoded as 12 x 6-bit symbols using the 72-character alphabet.
  // The first 64 bits are returned; the final 8 bits are provided via pRem.
  if (value.size() < 12) return 0;

  std::uint64_t decoded = 0;
  constexpr std::uint8_t mask2 = static_cast<std::uint8_t>((1 << 2) - 1);

  // First 10 symbols carry 60 bits (MSB-first)
  for (int i = 0; i < 10; ++i) {
    auto idx = kAlphabet72.find(value[static_cast<std::size_t>(i)]);
    if (idx == std::string::npos) return 0;
    decoded |= static_cast<std::uint64_t>(idx) << (58 - 6 * i);
  }

  auto remHighIdx = kAlphabet72.find(value[10]);
  auto remLowIdx = kAlphabet72.find(value[11]);
  if (remHighIdx == std::string::npos || remLowIdx == std::string::npos) return 0;

  std::uint8_t remHigh = static_cast<std::uint8_t>(remHighIdx);
  std::uint8_t remLow = static_cast<std::uint8_t>(remLowIdx);

  decoded |= static_cast<std::uint64_t>(remHigh >> 2);
  std::uint8_t rem = static_cast<std::uint8_t>(((remHigh & mask2) << 6) | remLow);

  if (pRem) *pRem = rem;
  return decoded;
}

std::string pack72bits(std::uint64_t value, std::uint8_t rem) {
  // Inverse of unpack72bits: 64-bit value plus 8-bit rem => 12 x 6-bit symbols.
  std::array<char, 12> packed{};
  constexpr std::uint8_t mask4 = static_cast<std::uint8_t>((1 << 4) - 1);
  constexpr std::uint8_t mask6 = static_cast<std::uint8_t>((1 << 6) - 1);

  std::uint8_t remHigh = static_cast<std::uint8_t>(((value & mask4) << 2) | (rem >> 6));
  std::uint8_t remLow = static_cast<std::uint8_t>(rem & mask6);
  value >>= 4;

  packed[11] = kAlphabet72.at(remLow);
  packed[10] = kAlphabet72.at(remHigh);

  for (int i = 0; i < 10; ++i) {
    packed[9 - i] = kAlphabet72.at(static_cast<std::size_t>(value & mask6));
    value >>= 6;
  }

  return std::string(packed.data(), packed.size());
}

std::uint32_t pack_alphanumeric22(std::string const& value, bool is_flag) {
  std::string padded = value;
  while (padded.size() < 4) padded.push_back(' ');
  std::uint32_t packed = 0;
  packed = kAlphanumeric.find(padded[0]);
  packed = 37 * packed + kAlphanumeric.find(padded[1]);
  packed = 27 * packed + kAlphanumeric.find(padded[2]) - 10;
  packed = 27 * packed + kAlphanumeric.find(padded[3]) - 10;
  if (is_flag) packed |= (1u << 21);
  return packed;
}

std::string unpack_alphanumeric22(std::uint32_t packed, bool* is_flag) {
  if (is_flag) *is_flag = (packed & (1u << 21)) != 0;
  packed &= ~(1u << 21);
  char word[4];
  std::uint32_t tmp = packed % 27 + 10;
  word[3] = kAlphanumeric.at(tmp);
  packed /= 27;
  tmp = packed % 27 + 10;
  word[2] = kAlphanumeric.at(tmp);
  packed /= 27;
  tmp = packed % 37;
  word[1] = kAlphanumeric.at(tmp);
  packed /= 37;
  tmp = packed;
  word[0] = kAlphanumeric.at(tmp);
  return std::string(word, 4);
}

std::uint64_t pack_alphanumeric50(std::string const& value) {
  // Match the desktop JS8 mixed-radix packing (38/39 with slash fields).
  auto clean = value;
  clean.erase(std::remove_if(clean.begin(), clean.end(), [](char c) {
                return kAlphanumeric.find(c) == std::string::npos;
              }),
              clean.end());

  // Insert spaces if a slash isn't already at the expected positions.
  if (clean.size() > 3 && clean[3] != '/') clean.insert(clean.begin() + 3, ' ');
  if (clean.size() > 7 && clean[7] != '/') clean.insert(clean.begin() + 7, ' ');

  while (clean.size() < 11) clean.push_back(' ');

  auto idx = [&](std::size_t i) -> std::uint64_t {
    auto pos = kAlphanumeric.find(clean[i]);
    return pos == std::string::npos ? 0ULL : static_cast<std::uint64_t>(pos);
  };

  std::uint64_t a = 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * 38ULL * idx(0);
  std::uint64_t b = 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * idx(1);
  std::uint64_t c = 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * 38ULL * 38ULL * 2ULL * idx(2);
  std::uint64_t d = 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * 38ULL * 38ULL * (clean[3] == '/' ? 1ULL : 0ULL);
  std::uint64_t e = 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * 38ULL * idx(4);
  std::uint64_t f = 38ULL * 38ULL * 38ULL * 2ULL * 38ULL * idx(5);
  std::uint64_t g = 38ULL * 38ULL * 38ULL * 2ULL * idx(6);
  std::uint64_t h = 38ULL * 38ULL * 38ULL * (clean[7] == '/' ? 1ULL : 0ULL);
  std::uint64_t i = 38ULL * 38ULL * idx(8);
  std::uint64_t j = 38ULL * idx(9);
  std::uint64_t k = idx(10);

  return a + b + c + d + e + f + g + h + i + j + k;
}

std::string unpack_alphanumeric50(std::uint64_t packed) {
  char word[11];

  auto next = [&](std::uint64_t base, bool slash_field = false) -> char {
    std::uint64_t tmp = packed % base;
    packed /= base;
    if (slash_field) return tmp ? '/' : ' ';
    return kAlphanumeric.at(static_cast<std::size_t>(tmp % kAlphanumeric.size()));
  };

  word[10] = next(38);
  word[9]  = next(38);
  word[8]  = next(38);
  word[7]  = next(2, true);
  word[6]  = next(38);
  word[5]  = next(38);
  word[4]  = next(38);
  word[3]  = next(2, true);
  word[2]  = next(38);
  word[1]  = next(38);
  word[0]  = next(39);  // first symbol uses 39

  std::string out(word, 11);
  out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
  return out;
}

std::uint32_t pack_callsign(std::string const& value, bool* p_portable) {
  std::string callsign = value;
  auto base_it = kBaseCalls.find(callsign);
  if (base_it != kBaseCalls.end()) return static_cast<std::uint32_t>(base_it->second);
  if (!callsign.empty()) {
    if (callsign.size() > 2 && callsign.substr(callsign.size() - 2) == "/P") {
      callsign = callsign.substr(0, callsign.size() - 2);
      if (p_portable) *p_portable = true;
    }
  }
  // Swaziland
  if (callsign.rfind("3DA0", 0) == 0) callsign = "3D0" + callsign.substr(4);
  if (callsign.rfind("3X", 0) == 0 && callsign.size() > 2 && std::isalpha(static_cast<unsigned char>(callsign[2]))) {
    callsign = "Q" + callsign.substr(2);
  }
  if (callsign.size() < 2 || callsign.size() > 6) return 0;
  std::vector<std::string> permutations = {callsign};
  if (callsign.size() == 2) permutations.push_back(" " + callsign + "   ");
  if (callsign.size() == 3) { permutations.push_back(" " + callsign + "  "); permutations.push_back(callsign + "   "); }
  if (callsign.size() == 4) { permutations.push_back(" " + callsign + " "); permutations.push_back(callsign + "  "); }
  if (callsign.size() == 5) { permutations.push_back(" " + callsign); permutations.push_back(callsign + " "); }

  std::regex re(R"(([0-9A-Z ])([0-9A-Z])([0-9])([A-Z ])([A-Z ])([A-Z ]))");
  std::string matched;
  for (auto const& perm : permutations) {
    if (perm.size() < 6) continue;
    std::smatch m;
    if (std::regex_match(perm, m, re)) {
      matched = m[0];
      break;
    }
  }
  if (matched.size() < 6) return 0;
  auto idx = [&matched](int pos) { return static_cast<int>(kAlphanumeric.find(matched[static_cast<std::size_t>(pos)])); };
  std::uint32_t packed = idx(0);
  packed = 36 * packed + idx(1);
  packed = 10 * packed + idx(2);
  packed = 27 * packed + idx(3) - 10;
  packed = 27 * packed + idx(4) - 10;
  packed = 27 * packed + idx(5) - 10;
  return packed;
}

std::string unpack_callsign(std::uint32_t value, bool portable) {
  for (auto const& kv : kBaseCalls) {
    if (kv.second == static_cast<int>(value)) return kv.first;
  }
  char word[6];
  std::uint32_t tmp = value % 27 + 10;
  word[5] = kAlphanumeric.at(tmp);
  value /= 27;
  tmp = value % 27 + 10;
  word[4] = kAlphanumeric.at(tmp);
  value /= 27;
  tmp = value % 27 + 10;
  word[3] = kAlphanumeric.at(tmp);
  value /= 27;
  tmp = value % 10;
  word[2] = kAlphanumeric.at(tmp);
  value /= 10;
  tmp = value % 36;
  word[1] = kAlphanumeric.at(tmp);
  value /= 36;
  tmp = value;
  word[0] = kAlphanumeric.at(tmp);
  std::string callsign(word, 6);
  if (callsign.rfind("3D0", 0) == 0) callsign = "3DA0" + callsign.substr(3);
  if (callsign.rfind("Q", 0) == 0 && callsign.size() > 1 && std::isalpha(static_cast<unsigned char>(callsign[1]))) {
    callsign = "3X" + callsign.substr(1);
  }
  auto trimmed = trimmed_left(trimmed_right(callsign));
  if (portable) trimmed += "/P";
  return trimmed;
}

std::string deg2grid(float dlong, float dlat) {
  char grid[6];
  if (dlong < -180) dlong += 360;
  if (dlong > 180) dlong -= 360;
  int nlong = static_cast<int>(60.0 * (180.0 - dlong) / 5);
  int n1 = nlong / 240;
  int n2 = (nlong - 240 * n1) / 24;
  int n3 = (nlong - 240 * n1 - 24 * n2);
  grid[0] = static_cast<char>('A' + n1);
  grid[2] = static_cast<char>('0' + n2);
  grid[4] = static_cast<char>('a' + n3);
  int nlat = static_cast<int>(60.0 * (dlat + 90) / 2.5);
  n1 = nlat / 240;
  n2 = (nlat - 240 * n1) / 24;
  n3 = (nlat - 240 * n1 - 24 * n2);
  grid[1] = static_cast<char>('A' + n1);
  grid[3] = static_cast<char>('0' + n2);
  grid[5] = static_cast<char>('a' + n3);
  return std::string(grid, 6);
}

std::pair<float, float> grid2deg(std::string const& grid) {
  std::string g = grid;
  if (g.size() < 6) g = grid.substr(0, 4) + "mm";
  g = std::string(g.begin(), g.end());
  if (g.size() < 6) g.resize(6, 'm');
  g[0] = static_cast<char>(std::toupper(g[0]));
  g[1] = static_cast<char>(std::toupper(g[1]));
  g[2] = g[2];
  g[3] = g[3];
  g[4] = static_cast<char>(std::tolower(g[4]));
  g[5] = static_cast<char>(std::tolower(g[5]));
  int nlong = 180 - 20 * (g[0] - 'A');
  int n20d = 2 * (g[2] - '0');
  float xminlong = 5 * (g[4] - 'a' + 0.5f);
  float dlong = nlong - n20d - xminlong / 60.0f;
  int nlat = -90 + 10 * (g[1] - 'A') + g[3] - '0';
  float xminlat = 2.5f * (g[5] - 'a' + 0.5f);
  float dlat = nlat + xminlat / 60.0f;
  return {dlong, dlat};
}

std::uint16_t pack_grid(std::string const& value) {
  auto grid = trimmed_left(trimmed_right(value));
  if (grid.size() < 4) return (1 << 15) - 1;
  auto pair = grid2deg(grid.substr(0, 4));
  int ilong = static_cast<int>(pair.first);
  int ilat = static_cast<int>(pair.second + 90);
  return static_cast<std::uint16_t>(((ilong + 180) / 2) * 180 + ilat);
}

std::string unpack_grid(std::uint16_t value) {
  int nbasegrid = 180 * 180;
  if (value > nbasegrid) return {};
  float dlat = value % 180 - 90;
  float dlong = value / 180 * 2 - 180 + 2;
  return deg2grid(dlong, dlat).substr(0, 4);
}

std::uint8_t pack_num(std::string const& num, bool* ok) {
  try {
    int val = std::stoi(num);
    if (val < -31 || val > 31) throw std::out_of_range("num");
    if (ok) *ok = true;
    return static_cast<std::uint8_t>((val + 64) & 0x7F);
  } catch (...) {
    if (ok) *ok = false;
    return 0;
  }
}

std::uint8_t pack_pwr(std::string const& pwr, bool* ok) {
  try {
    int val = dbmTomwatts(std::stoi(pwr));
    if (ok) *ok = true;
    return static_cast<std::uint8_t>(val);
  } catch (...) {
    if (ok) *ok = false;
    return 0;
  }
}

// As desktop's Varicode::packCmd(): an SNR command is [1][type][6-bit
// number] (type 1 = HEARTBEAT SNR), any other command its 7-bit value.
// js8core kept 2 bits of the number and shifted every command, so a
// compound frame's command and SNR didn't match desktop's (local patch 16).
std::uint8_t pack_cmd(std::uint8_t cmd, std::uint8_t num, bool* p_packed_num) {
  if (kSnrCmds.count(cmd) > 0) {
    if (p_packed_num) *p_packed_num = true;
    std::uint8_t value = static_cast<std::uint8_t>(((1 << 1) | (cmd == 29 ? 1 : 0)) << 6);
    return static_cast<std::uint8_t>(value + (num & ((1 << 6) - 1)));
  }
  if (p_packed_num) *p_packed_num = false;
  return static_cast<std::uint8_t>(cmd & ((1 << 7) - 1));
}

// As desktop's Varicode::unpackCmd().
std::uint8_t unpack_cmd(std::uint8_t value, std::uint8_t* p_num) {
  if (value & (1 << 7)) {
    if (p_num) *p_num = value & ((1 << 6) - 1);
    return (value & (1 << 6)) ? 29 /* HEARTBEAT SNR */ : 25 /* SNR */;
  }
  if (p_num) *p_num = 0;
  return value & ((1 << 7) - 1);
}

bool is_snr_command(std::string const& cmd) {
  for (auto const& dc : kDirectedCmds) {
    if (dc.key == cmd) return kSnrCmds.count(dc.value) > 0;
  }
  return false;
}

bool is_command_allowed(std::string const& cmd) {
  for (auto const& dc : kDirectedCmds) {
    if (dc.key == cmd) return kAllowedCmds.count(dc.value) > 0;
  }
  return false;
}

bool is_command_buffered(std::string const& cmd) {
  for (auto const& dc : kDirectedCmds) {
    if (dc.key == cmd) return kBufferedCmds.count(dc.value) > 0;
  }
  return false;
}

int is_command_checksummed(std::string const& cmd) {
  for (auto const& dc : kDirectedCmds) {
    if (dc.key == cmd) {
      auto it = kChecksumCmds.find(dc.value);
      if (it != kChecksumCmds.end()) return it->second;
    }
  }
  return 0;
}

bool is_command_autoreply(std::string const& cmd) {
  for (auto const& dc : kDirectedCmds) {
    if (dc.key == cmd) return kAutoreplyCmds.count(dc.value) > 0;
  }
  return false;
}

// Port of desktop JS8Call's Varicode::isValidCallsign() and
// isValidCompoundCallsign() (local patch 6). The earlier version here accepted
// any short alphanumeric word ("JUST", "HELLO") as a callsign; desktop requires
// a letter-digit pair so ordinary words aren't mistaken for callsigns.
namespace {
bool has_letter_digit_pair(std::string const& s) {
  for (std::size_t i = 1; i < s.size(); ++i) {
    bool a = std::isdigit((unsigned char)s[i - 1]), b = std::isdigit((unsigned char)s[i]);
    bool la = std::isupper((unsigned char)s[i - 1]), lb = std::isupper((unsigned char)s[i]);
    if ((a && lb) || (la && b)) return true;
  }
  return false;
}

bool is_valid_compound_callsign(std::string const& callsign) {
  // Compound calls cannot be > 9 characters after removing the slashes.
  auto slashes = std::count(callsign.begin(), callsign.end(), '/');
  if ((long)callsign.size() - slashes > 9) return false;

  // Valid when it is: an actual compound call (containing /) whose prefix is
  // not a base call; a group call (@...); or longer than two characters with
  // a letter-digit pair, so arbitrary short words aren't coded as callsigns.
  if (auto index = callsign.find('/'); index != std::string::npos) {
    return kBaseCalls.find(callsign.substr(0, index)) == kBaseCalls.end();
  }
  if (!callsign.empty() && callsign.front() == '@') return true;
  return callsign.size() > 2 && has_letter_digit_pair(callsign);
}
}  // namespace

bool is_valid_callsign(std::string const& callsign, bool* p_is_compound) {
  if (kBaseCalls.find(callsign) != kBaseCalls.end()) {
    if (p_is_compound) *p_is_compound = false;
    return true;
  }

  static const std::regex base_re(R"(\b(([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)([/][P])?\b)");
  if (std::regex_match(callsign, base_re)) {
    if (p_is_compound) *p_is_compound = false;
    return callsign.size() > 2 && has_letter_digit_pair(callsign);
  }

  static const std::regex compound_re(R"((?:[@]?|\b)([A-Z0-9\/@][A-Z0-9\/]{0,2}[\/]?[A-Z0-9\/]{0,3}[\/]?[A-Z0-9\/]{0,3})\b)");
  if (std::regex_match(callsign, compound_re)) {
    bool valid = is_valid_compound_callsign(callsign);
    if (p_is_compound) *p_is_compound = valid;
    return valid;
  }

  if (p_is_compound) *p_is_compound = false;
  return false;
}

bool is_compound_callsign(std::string const& callsign) {
  bool is_compound = false;
  return is_valid_callsign(callsign, &is_compound) && is_compound;
}

bool is_group_allowed(std::string const& group) {
  return kBaseCalls.find(group) != kBaseCalls.end();
}

static Backend g_backend;

void set_backend(Backend const& backend) { g_backend = backend; }

// Message packing/unpacking currently delegate to an injected backend (e.g., Qt adapter)
// until the full core implementation is completed.
std::string pack_heartbeat_message(std::string const& text, std::string const& callsign, int* n) {
  std::smatch match;
  if (!std::regex_match(text, match, kHeartbeatRe)) {
    if (g_backend.pack_heartbeat_message) return g_backend.pack_heartbeat_message(text, callsign, n);
    if (n) *n = 0;
    return {};
  }

  auto type = match[1].str();
  auto grid = match.size() > 2 ? match[2].str() : std::string{};
  bool isAlt = type.rfind("CQ", 0) == 0;

  if (callsign.empty()) {
    if (n) *n = 0;
    return {};
  }

  std::uint16_t packed_extra = static_cast<std::uint16_t>(nmaxgrid);
  if (!grid.empty() && std::regex_match(grid, kGridPattern)) {
    packed_extra = pack_grid(grid);
  }

  std::uint8_t cqNumber = 0;
  for (auto const& pair : (isAlt ? kCqs : kHbs)) {
    if (pair.second == type) {
      cqNumber = static_cast<std::uint8_t>(pair.first);
      break;
    }
  }
  if (isAlt) packed_extra |= (1 << 15);

  auto frame = pack_compound_frame(callsign, static_cast<std::uint8_t>(FrameType::FrameHeartbeat), packed_extra, cqNumber);
  if (n) *n = static_cast<int>(match.length(0));
  return frame;
}

std::vector<std::string> unpack_heartbeat_message(std::string const& text, std::uint8_t* pType, bool* isAlt, std::uint8_t* pBits3) {
  std::uint8_t type = static_cast<std::uint8_t>(FrameType::FrameHeartbeat);
  std::uint16_t num = static_cast<std::uint16_t>(nmaxgrid);
  std::uint8_t bits3 = 0;

  auto unpacked = unpack_compound_frame(text, &type, &num, &bits3);
  if (unpacked.empty() || type != static_cast<std::uint8_t>(FrameType::FrameHeartbeat)) return {};

  unpacked.push_back(unpack_grid(num & ((1 << 15) - 1)));
  if (isAlt) *isAlt = (num & (1 << 15)) != 0;
  if (pType) *pType = type;
  if (pBits3) *pBits3 = bits3;
  return unpacked;
}

std::string pack_compound_message(std::string const& text, int* n) {
  std::smatch match;
  if (!std::regex_match(text, match, kCompoundRe)) {
    if (g_backend.pack_compound_message) return g_backend.pack_compound_message(text, n);
    if (n) *n = 0;
    return {};
  }

  auto callsign = match[1].str();
  auto grid = match.size() > 2 ? match[2].str() : std::string{};
  auto cmd = match.size() > 3 ? match[3].str() : std::string{};
  auto num = trimmed_left(match.size() > 4 ? match[4].str() : std::string{});

  if (callsign.empty()) {
    if (n) *n = 0;
    return {};
  }

  bool validGrid = !grid.empty() && std::regex_match(grid, kGridPattern);
  auto cmd_it = std::find_if(kDirectedCmds.begin(), kDirectedCmds.end(), [&](auto const& dc) { return dc.key == cmd; });
  bool validCmd = (cmd_it != kDirectedCmds.end()) && is_command_allowed(cmd);

  std::uint8_t type = static_cast<std::uint8_t>(FrameType::FrameCompound);
  std::uint16_t extra = static_cast<std::uint16_t>(nmaxgrid);

  if (validCmd) {
    bool packedNum = false;
    auto inum = pack_num_qtstyle(num, nullptr);
    extra = static_cast<std::uint16_t>(nusergrid + pack_cmd(static_cast<std::uint8_t>(cmd_it->value), static_cast<std::uint8_t>(inum), &packedNum));
    type = static_cast<std::uint8_t>(FrameType::FrameCompoundDirected);
  } else if (validGrid) {
    extra = pack_grid(grid);
  }

  auto frame = pack_compound_frame(callsign, type, extra, 0);
  if (n) *n = static_cast<int>(match.length(0));
  return frame;
}
std::vector<std::string> unpack_compound_message(std::string const& text, std::uint8_t* pType, std::uint16_t* pNum, std::uint8_t* pBits3) {
  std::uint8_t type = static_cast<std::uint8_t>(FrameType::FrameCompound);
  std::uint16_t extra = static_cast<std::uint16_t>(nmaxgrid);
  std::uint8_t bits3 = 0;

  auto unpacked = unpack_compound_frame(text, &type, &extra, &bits3);
  if (unpacked.empty() || (type != static_cast<std::uint8_t>(FrameType::FrameCompound) && type != static_cast<std::uint8_t>(FrameType::FrameCompoundDirected))) {
    return {};
  }

  if (extra <= nbasegrid) {
    unpacked.push_back(" " + unpack_grid(extra));
  } else if (nusergrid <= extra && extra < nmaxgrid) {
    std::uint8_t num = 0;
    auto cmd = unpack_cmd(static_cast<std::uint8_t>(extra - nusergrid), &num);
    auto it = std::find_if(kDirectedCmds.begin(), kDirectedCmds.end(), [&](auto const& dc) { return dc.value == cmd; });
    if (it != kDirectedCmds.end()) {
      unpacked.push_back(it->key);
      if (is_snr_command(it->key)) {
        unpacked.push_back(format_snr(static_cast<int>(num) - 31));
      }
    }
  }

  if (pType) *pType = type;
  if (pBits3) *pBits3 = bits3;
  if (pNum) *pNum = extra;
  return unpacked;
}

std::string pack_compound_frame(std::string const& callsign, std::uint8_t type, std::uint16_t num, std::uint8_t bits3) {
  if (type == static_cast<std::uint8_t>(FrameType::FrameData) || type == static_cast<std::uint8_t>(FrameType::FrameDirected)) return {};
  auto packed_callsign = pack_alphanumeric50(callsign);
  if (packed_callsign == 0) return {};

  std::uint16_t mask11 = static_cast<std::uint16_t>(((1 << 11) - 1) << 5);
  std::uint8_t mask5 = static_cast<std::uint8_t>((1 << 5) - 1);

  std::uint16_t packed_11 = (num & mask11) >> 5;
  std::uint8_t packed_5 = static_cast<std::uint8_t>(num & mask5);
  std::uint8_t packed_8 = static_cast<std::uint8_t>((packed_5 << 3) | bits3);

  auto bits = int_to_bits(type, 3);
  auto call_bits = int_to_bits(packed_callsign, 50);
  auto bits11 = int_to_bits(packed_11, 11);
  bits.insert(bits.end(), call_bits.begin(), call_bits.end());
  bits.insert(bits.end(), bits11.begin(), bits11.end());

  return pack72bits(bits_to_int(bits), packed_8);
}
std::vector<std::string> unpack_compound_frame(std::string const& text, std::uint8_t* pType, std::uint16_t* pNum, std::uint8_t* pBits3) {
  std::vector<std::string> unpacked;
  if (text.size() < 12 || text.find(' ') != std::string::npos) return unpacked;

  std::uint8_t packed_8 = 0;
  auto bits64 = int_to_bits(unpack72bits(text, &packed_8), 64);
  std::uint8_t packed_5 = packed_8 >> 3;
  std::uint8_t packed_3 = packed_8 & ((1 << 3) - 1);
  std::uint8_t packed_flag = static_cast<std::uint8_t>(bits_to_int(std::vector<bool>(bits64.begin(), bits64.begin() + 3)));

  if (packed_flag == static_cast<std::uint8_t>(FrameType::FrameData) || packed_flag == static_cast<std::uint8_t>(FrameType::FrameDirected)) return unpacked;

  auto call_bits = std::vector<bool>(bits64.begin() + 3, bits64.begin() + 53);
  auto packed_callsign = bits_to_int(call_bits);
  auto bits11 = std::vector<bool>(bits64.begin() + 53, bits64.begin() + 64);
  std::uint16_t packed_11 = static_cast<std::uint16_t>(bits_to_int(bits11));

  auto callsign = unpack_alphanumeric50(packed_callsign);
  std::uint16_t num = static_cast<std::uint16_t>((packed_11 << 5) | packed_5);

  if (pType) *pType = packed_flag;
  if (pNum) *pNum = num;
  if (pBits3) *pBits3 = packed_3;

  unpacked.push_back(callsign);
  unpacked.push_back("");
  return unpacked;
}

std::string pack_directed_message(std::string const& text, std::string const& mycall, std::string* pTo, bool* pToCompound, std::string* pCmd, std::string* pNum, int* n) {
  std::smatch match;
  if (!std::regex_search(text, match, kDirectedRe)) {
    if (g_backend.pack_directed_message) return g_backend.pack_directed_message(text, mycall, pTo, pToCompound, pCmd, pNum, n);
    if (n) *n = 0;
    return {};
  }

  std::string from = mycall;
  bool portable_from = false;
  if (is_compound_callsign(from)) {
    from = "<....>";
  }
  auto packed_from = pack_callsign(from, &portable_from);

  auto to = match[1].str();
  auto cmd = match.size() > 2 ? match[2].str() : std::string{};

  if (cmd.empty()) { if (n) *n = 0; return {}; }

  // Desktop's number: only right after SNR (its optional_num_pattern,
  // (?<=SNR)\s?[-+]?(?:3[01]|[0-2]?[0-9]); std::regex has no lookbehind).
  // Taking digits after any command dropped them from the text: "RR 73"
  // went out as RR with a number, a relay to 2E0ABC as one to E0ABC.
  // (Local patch 11, see UPSTREAM.md.)
  std::string num;
  std::size_t consumed = static_cast<std::size_t>(match.position(2) + match.length(2));
  if (cmd.size() >= 3 && cmd.compare(cmd.size() - 3, 3, "SNR") == 0) {
    static const std::regex kSnrNum(R"(^\s?[-+]?(?:3[01]|[0-2]?[0-9]))");
    std::smatch num_match;
    auto rest = text.substr(consumed);
    if (std::regex_search(rest, num_match, kSnrNum)) {
      num = num_match.str(0);
      consumed += static_cast<std::size_t>(num_match.length(0));
    }
  }

  bool isToCompound = false;
  bool validTo = (to != mycall) && is_valid_callsign(to, &isToCompound);
  if (!validTo) { if (n) *n = 0; return {}; }

  if (pTo) *pTo = to;
  if (pToCompound) *pToCompound = isToCompound;

  if (isToCompound) {
    to = "<....>";
  }

  // Validate command
  if (!is_command_allowed(cmd) && !is_command_allowed(trimmed_left(cmd))) {
    if (n) *n = 0;
    return {};
  }

  bool portable_to = false;
  auto packed_to = pack_callsign(to, &portable_to);
  if (packed_from == 0 || packed_to == 0) { if (n) *n = 0; return {}; }

  // Find command code
  std::uint8_t packed_cmd = 0;
  for (auto const& dc : kDirectedCmds) {
    if (dc.key == cmd || dc.key == (" " + cmd)) {
      packed_cmd = static_cast<std::uint8_t>(dc.value);
      break;
    }
  }

  bool numOK = false;
  std::uint8_t inum = pack_num_qtstyle(trimmed_left(num), &numOK);
  if (pCmd) *pCmd = cmd;
  if (numOK && pNum) *pNum = num;

  std::uint8_t packed_flag = static_cast<std::uint8_t>(FrameType::FrameDirected);
  std::uint8_t packed_extra = static_cast<std::uint8_t>(((portable_from ? 1 : 0) << 7) |
                                                        ((portable_to ? 1 : 0) << 6) |
                                                        inum);

  // [3][28][28][5],[2][6] = 72
  auto bits = int_to_bits(packed_flag, 3);
  auto from_bits = int_to_bits(packed_from, 28);
  auto to_bits = int_to_bits(packed_to, 28);
  auto cmd_bits = int_to_bits(static_cast<std::uint8_t>(packed_cmd % 32), 5);
  bits.insert(bits.end(), from_bits.begin(), from_bits.end());
  bits.insert(bits.end(), to_bits.begin(), to_bits.end());
  bits.insert(bits.end(), cmd_bits.begin(), cmd_bits.end());

  if (n) *n = static_cast<int>(consumed);
  return pack72bits(bits_to_int(bits), packed_extra);
}
std::vector<std::string> unpack_directed_message(std::string const& text, std::uint8_t* pType) {
  std::vector<std::string> out;
  if (text.size() < 12 || text.find(' ') != std::string::npos) return out;

  std::uint8_t extra = 0;
  auto bits64 = int_to_bits(unpack72bits(text, &extra), 64);

  std::uint8_t packed_flag = static_cast<std::uint8_t>(bits_to_int(std::vector<bool>(bits64.begin(), bits64.begin() + 3)));
  if (packed_flag != static_cast<std::uint8_t>(FrameType::FrameDirected)) return out;

  std::uint32_t packed_from = static_cast<std::uint32_t>(bits_to_int(std::vector<bool>(bits64.begin() + 3, bits64.begin() + 31)));
  std::uint32_t packed_to   = static_cast<std::uint32_t>(bits_to_int(std::vector<bool>(bits64.begin() + 31, bits64.begin() + 59)));
  std::uint8_t packed_cmd   = static_cast<std::uint8_t>(bits_to_int(std::vector<bool>(bits64.begin() + 59, bits64.begin() + 64)));

  bool portable_from = ((extra >> 7) & 1) == 1;
  bool portable_to   = ((extra >> 6) & 1) == 1;
  extra = static_cast<std::uint8_t>(extra % 64);

  auto from = unpack_callsign(packed_from, portable_from);
  auto to   = unpack_callsign(packed_to, portable_to);

  std::string cmd;
  auto it = std::find_if(kDirectedCmds.begin(), kDirectedCmds.end(), [&](auto const& dc) { return dc.value == packed_cmd % 32; });
  if (it != kDirectedCmds.end()) cmd = it->key;

  if (!from.empty()) out.push_back(from);
  if (!to.empty())   out.push_back(to);
  if (!cmd.empty())  out.push_back(cmd);

  if (extra != 0) {
    if (!cmd.empty() && is_snr_command(cmd)) {
      out.push_back(format_snr(static_cast<int>(extra) - 31));
    } else {
      out.push_back(std::to_string(static_cast<int>(extra) - 31));
    }
  }

  if (pType) *pType = packed_flag;
  return out;
}

// Helper function to convert string to uppercase for JSC encoding
// JSC dictionary only contains uppercase letters
static std::string to_upper(std::string const& s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

namespace {
// Desktop's packHuffMessage(): the default Huffman table with a [1][0]
// prefix, only when every character has a code; chars packed into *n.
std::string pack_huff_message(std::string const& text, int* n) {
  constexpr int frameSize = 72;
  auto table = default_huff_table();
  auto valid = huff_valid_chars(table);
  for (char ch : text) {
    if (!valid.count(std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))))) {
      if (n) *n = 0;
      return {};
    }
  }
  std::vector<bool> frameBits{true, false};
  int chars_used = 0;
  for (auto const& [chars, bits] : huff_encode(table, text)) {
    if (static_cast<int>(frameBits.size() + bits.size()) < frameSize) {
      frameBits.insert(frameBits.end(), bits.begin(), bits.end());
      chars_used += chars;
      continue;
    }
    break;
  }
  int pad = frameSize - static_cast<int>(frameBits.size());
  for (int i = 0; i < pad; ++i) frameBits.push_back(i == 0 ? false : true);
  auto value = bits_to_int(std::vector<bool>(frameBits.begin(), frameBits.begin() + 64));
  auto rem = static_cast<std::uint8_t>(bits_to_int(std::vector<bool>(frameBits.begin() + 64, frameBits.end())));
  if (n) *n = chars_used;
  return pack72bits(static_cast<std::uint64_t>(value), rem);
}
} // namespace

std::string pack_jsc_data_message(std::string const& text, int* n);

// Desktop's packDataMessage(): Huffman or JSC, whichever fits more
// characters (JSC on a tie). JSC alone put callsigns in more frames than
// desktop does (local patch 11).
std::string pack_data_message(std::string const& text, int* n) {
  int huff_chars = 0, jsc_chars = 0;
  auto huff = pack_huff_message(to_upper(text), &huff_chars);
  auto jsc  = pack_jsc_data_message(text, &jsc_chars);
  if (huff_chars > jsc_chars) {
    if (n) *n = huff_chars;
    return huff;
  }
  if (n) *n = jsc_chars;
  return jsc;
}

std::string pack_jsc_data_message(std::string const& text, int* n) {
  // Legacy data frames use a 2-bit prefix: [data=1][compressed=1] + payload.
  // JSC dictionary only contains uppercase, so convert input first
  std::string upperText = to_upper(text);
  LOGD("pack_data_message: input='%s' upperText='%s'", text.c_str(), upperText.c_str());
  
  constexpr int frameSize = 72;
  std::vector<bool> frameBits;
  frameBits.reserve(frameSize);
  frameBits.push_back(true);  // data flag
  frameBits.push_back(true);  // compressed (JSC)

  int chars_used = 0;
  for (auto const& pair : jsc::compress(upperText)) {
    auto const& bits = pair.first;
    auto chars = static_cast<int>(pair.second);
    if (static_cast<int>(frameBits.size() + bits.size()) < frameSize) {
      frameBits.insert(frameBits.end(), bits.begin(), bits.end());
      chars_used += chars;
      continue;
    }
    break;
  }

  int pad = frameSize - static_cast<int>(frameBits.size());
  for (int i = 0; i < pad; ++i) {
    frameBits.push_back(i == 0 ? false : true);
  }

  auto value = bits_to_int(std::vector<bool>(frameBits.begin(), frameBits.begin() + 64));
  auto rem = static_cast<std::uint8_t>(bits_to_int(std::vector<bool>(frameBits.begin() + 64, frameBits.end())));
  auto frame = pack72bits(static_cast<std::uint64_t>(value), rem);

  LOGD("pack_data_message: frame='%s' chars_used=%d", frame.c_str(), chars_used);
  if (n) *n = chars_used;
  return frame;
}
std::string unpack_data_message(std::string const& text) {
  if (text.size() < 12 || text.find(' ') != std::string::npos) return {};

  std::uint8_t rem = 0;
  auto value = unpack72bits(text, &rem);
  auto bits64 = int_to_bits(value, 64);
  auto remBits = int_to_bits(rem, 8);
  bits64.insert(bits64.end(), remBits.begin(), remBits.end());

  if (bits64.empty() || !bits64.front()) return {};
  bits64.erase(bits64.begin());
  if (bits64.empty()) return {};

  bool compressed = bits64.front();
  bits64.erase(bits64.begin());

  int lastZero = -1;
  for (int i = static_cast<int>(bits64.size()) - 1; i >= 0; --i) {
    if (!bits64[static_cast<std::size_t>(i)]) { lastZero = i; break; }
  }
  if (lastZero >= 0) {
    bits64.erase(bits64.begin() + lastZero, bits64.end());
  }

  if (bits64.empty()) return {};
  if (compressed) return jsc::decompress(bits64);
  return huff_decode(default_huff_table(), bits64);
}

std::string pack_fast_data_message(std::string const& text, int* n) {
  // Compress using JSC into a 72-bit frame with pad scheme: first pad bit 0, remaining pad bits 1.
  // JSC dictionary only contains uppercase, so convert input first
  std::string upperText = to_upper(text);
  LOGD("pack_fast_data_message: input text='%s' upperText='%s'", text.c_str(), upperText.c_str());

  constexpr int frameSize = 72;
  std::vector<bool> frameBits;
  int chars_used = 0;

  auto compressed = jsc::compress(upperText);
  LOGD("pack_fast_data_message: jsc::compress returned %zu codeword pairs", compressed.size());

  for (std::size_t i = 0; i < compressed.size(); ++i) {
    auto const& pair = compressed[i];
    auto const& bits = pair.first;
    auto chars = static_cast<int>(pair.second);
    LOGD("pack_fast_data_message: pair[%zu] bits=%zu chars=%d frameBits.size=%zu", 
         i, bits.size(), chars, frameBits.size());
    if (static_cast<int>(frameBits.size() + bits.size()) < frameSize) {
      frameBits.insert(frameBits.end(), bits.begin(), bits.end());
      chars_used += chars;
      LOGD("pack_fast_data_message: added, frameBits.size now=%zu chars_used=%d", frameBits.size(), chars_used);
      continue;
    }
    LOGD("pack_fast_data_message: breaking, would exceed frameSize");
    break;
  }

  LOGD("pack_fast_data_message: before padding frameBits.size=%zu chars_used=%d", frameBits.size(), chars_used);

  int pad = frameSize - static_cast<int>(frameBits.size());
  for (int i = 0; i < pad; ++i) {
    frameBits.push_back(i == 0 ? false : true);
  }

  LOGD("pack_fast_data_message: after padding frameBits.size=%zu pad=%d", frameBits.size(), pad);

  auto value = bits_to_int(std::vector<bool>(frameBits.begin(), frameBits.begin() + 64));
  auto rem = static_cast<std::uint8_t>(bits_to_int(std::vector<bool>(frameBits.begin() + 64, frameBits.begin() + 72)));
  auto frame = pack72bits(static_cast<std::uint64_t>(value), rem);

  LOGD("pack_fast_data_message: frame='%s' chars_used=%d", frame.c_str(), chars_used);

  if (n) *n = chars_used;
  return frame;
}
std::string unpack_fast_data_message(std::string const& text) {
  if (text.size() < 12 || text.find(' ') != std::string::npos) return {};

  std::uint8_t rem = 0;
  auto value = unpack72bits(text, &rem);
  auto bits64 = int_to_bits(value, 64);
  auto remBits = int_to_bits(rem, 8);
  bits64.insert(bits64.end(), remBits.begin(), remBits.end());

  // Find last zero (pad start)
  int lastZero = -1;
  for (int i = static_cast<int>(bits64.size()) - 1; i >= 0; --i) {
    if (!bits64[static_cast<std::size_t>(i)]) { lastZero = i; break; }
  }
  if (lastZero >= 0) {
    bits64.erase(bits64.begin() + lastZero, bits64.end());
  }

  return jsc::decompress(bits64);
}

std::vector<std::pair<std::string, int>> build_message_frames(std::string const& mycall,
                                                              std::string const& mygrid,
                                                              std::string const& selectedCall,
                                                              std::string const& text,
                                                              bool forceIdentify,
                                                              bool forceData,
                                                              int submode,
                                                              MessageInfo* pInfo) {
  // This is a direct std translation of the legacy buildMessageFrames logic, but without JSC compression.
  bool mycallCompound = is_compound_callsign(mycall);
  std::vector<std::pair<std::string, int>> allFrames;

  std::vector<std::string> lines = {text};

  for (auto line : lines) {
    std::vector<std::pair<std::string, int>> lineFrames;
    bool hasDirected = false;
    bool hasData = forceData;

    // Data frames must preserve the caller's payload exactly. Desktop disables
    // automatic identification here so it cannot prepend the sender callsign.
    if (forceData) forceIdentify = false;

    // Remove own callsign prefix
    auto mycallWithSep = mycall + ":";
    if (line.rfind(mycallWithSep, 0) == 0 || line.rfind(mycall + " ", 0) == 0) {
      line = lstrip(line.substr(mycall.size() + 1));
    }

    // Auto-append selected call if needed
    if (!selectedCall.empty() && line.rfind(selectedCall, 0) != 0 && line.rfind("`", 0) != 0 && !forceData) {
      bool lineStartsWithBase = (line.rfind("@ALLCALL", 0) == 0) || (line.rfind("CQ", 0) == 0) || (line.rfind("HB", 0) == 0);
      auto const separator = line.find_first_of(" :");
      auto const firstToken = line.substr(0, separator);
      bool isCompound = false;
      bool lineStartsWithCallsign = is_valid_callsign(firstToken, &isCompound) && firstToken.size() > 3;
      if (!lineStartsWithBase && !lineStartsWithCallsign) {
        auto sep = line.rfind(" ", 0) == 0 ? "" : " ";
        line = selectedCall + sep + line;
      }
    }

    while (!line.empty()) {
      std::string frame;
      bool useBcn = false;
      bool useCmp = false;
      bool useDir = false;
      bool useDat = false;

      int l = 0;
      auto bcnFrame = pack_heartbeat_message(line, mycall, &l);

      int o = 0;
      auto cmpFrame = pack_compound_message(line, &o);

      int nlen = 0;
      std::string dirCmd, dirTo, dirNum;
      bool dirToCompound = false;
      auto dirFrame = pack_directed_message(line, mycall, &dirTo, &dirToCompound, &dirCmd, &dirNum, &nlen);
      LOGD("build_message_frames: line='%s' mycall='%s' dirTo='%s' dirCmd='%s' nlen=%d", 
           line.c_str(), mycall.c_str(), dirTo.c_str(), dirCmd.c_str(), nlen);

      if (forceIdentify && lineFrames.empty() && selectedCall.empty() && dirTo.empty() && l == 0 && o == 0 && line.find(mycall) == std::string::npos) {
        line = mycall + ": " + line;
        LOGD("build_message_frames: prepended mycall, line now='%s'", line.c_str());
      }

      int m = 0;
      bool fastDataFrame = submode != 0;
      auto datFrame = fastDataFrame ? pack_fast_data_message(line, &m) : pack_data_message(line, &m);
      LOGD("build_message_frames: l=%d o=%d nlen=%d m=%d hasDirected=%d hasData=%d", l, o, nlen, m, hasDirected, hasData);

      if (!hasDirected && !hasData && l > 0) {
        useBcn = true;
        frame = bcnFrame;
        LOGD("build_message_frames: using BCN frame");
      } else if (!hasDirected && !hasData && nlen > 0) {
        // Check directed BEFORE compound - directed messages like "CALL CMD" are more specific
        useDir = true;
        hasDirected = true;
        frame = dirFrame;
        LOGD("build_message_frames: using DIR frame='%s'", frame.c_str());
      } else if (!hasDirected && !hasData && o > 0) {
        useCmp = true;
        frame = cmpFrame;
        LOGD("build_message_frames: using CMP frame");
      } else if (m > 0) {
        useDat = true;
        hasData = true;
        frame = datFrame;
        LOGD("build_message_frames: using DAT frame='%s' m=%d", frame.c_str(), m);
      }

      if (useBcn) {
        lineFrames.push_back({frame, 0});
        line = line.substr(l);
      }
      if (useCmp) {
        lineFrames.push_back({frame, 0});
        line = line.substr(o);
      }
      if (useDir) {
        bool shouldUseStandardFrame = true;

        if (mycallCompound || dirToCompound) {
          auto deCompoundMessage = "`" + mycall + " " + mygrid;
          auto deCompoundFrame = pack_compound_message(deCompoundMessage, nullptr);
          if (!deCompoundFrame.empty()) lineFrames.push_back({deCompoundFrame, 0});
        }

        if (mycallCompound || dirToCompound) {
          auto dirCompoundMessage = "`" + dirTo + dirCmd + dirNum;
          auto dirCompoundFrame = pack_compound_message(dirCompoundMessage, nullptr);
          if (!dirCompoundFrame.empty()) {
            lineFrames.push_back({dirCompoundFrame, 0});
            shouldUseStandardFrame = false;
          }
        }

        if (shouldUseStandardFrame) {
          lineFrames.push_back({frame, 0});
        }
        line = line.substr(nlen);
        // Desktop's isCommandBuffered() here also counts any command with a
        // space in it (" RR", " YES", " HEARTBEAT SNR" ...), so the text
        // after it loses its leading space: "RR 73" sends "73", not " 73"
        // (local patch 11).
        bool desktopBuffered = is_command_buffered(dirCmd) ||
                               (dirCmd.find(' ') != std::string::npos && is_command_allowed(dirCmd));
        if (desktopBuffered && !line.empty()) {
          line = lstrip(line);
          // Desktop sends @APRSIS MSG / MSG TO: without a checksum.
          bool skipAprsChecksum = to_upper(dirTo) == "@APRSIS" && (dirCmd == " MSG" || dirCmd == " MSG TO:");
          int checksumSize = skipAprsChecksum ? 0 : is_command_checksummed(dirCmd);
          if (checksumSize == 32) {
            line = line + " " + checksum32(line);
          } else if (checksumSize == 16) {
            line = line + " " + checksum16(line);
          }
        }
        if (pInfo) {
          pInfo->dir_cmd = dirCmd;
          pInfo->dir_to = dirTo;
          pInfo->dir_num = dirNum;
        }
      }
      if (useDat) {
        lineFrames.push_back({frame, fastDataFrame ? 4 : 0});
        line = line.substr(m);
      }
    }

    if (!lineFrames.empty()) {
      lineFrames.front().second |= 1;  // First flag
      lineFrames.back().second |= 2;   // Last flag
    }

    allFrames.insert(allFrames.end(), lineFrames.begin(), lineFrames.end());
  }

  return allFrames;
}

}  // namespace js8core::protocol::varicode
