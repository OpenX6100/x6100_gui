#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace js8core::protocol::varicode {

// Extra information from build_message_frames
struct MessageInfo {
  std::string dir_to;
  std::string dir_cmd;
  std::string dir_num;
};

// Huffman helpers
using HuffTable = std::unordered_map<std::string, std::string>;
using HuffEncoded = std::vector<std::pair<int, std::vector<bool>>>;

// Tables
HuffTable default_huff_table();

// Basic string utilities
std::string escape(std::string const& text);
std::string unescape(std::string const& text);
std::string rstrip(std::string const& text);
std::string lstrip(std::string const& text);

// Checksums
std::string checksum16(std::string const& input);
bool checksum16_valid(std::string const& checksum, std::string const& input);
std::string checksum32(std::string const& input);
bool checksum32_valid(std::string const& checksum, std::string const& input);

// Huffman encoding/decoding
HuffEncoded huff_encode(HuffTable const& table, std::string const& text);
std::string huff_decode(HuffTable const& table, std::vector<bool> const& bits);
std::unordered_set<std::string> huff_valid_chars(HuffTable const& table);

// Bit helpers
std::vector<bool> bytes_to_bits(char* bitvec, int n);
std::vector<bool> str_to_bits(std::string const& bitvec);
std::string bits_to_str(std::vector<bool> const& bitvec);

std::vector<bool> int_to_bits(std::uint64_t value, int expected = 0);
std::uint64_t bits_to_int(std::vector<bool> value);
std::uint64_t bits_to_int(std::vector<bool>::const_iterator start, int n);
std::vector<bool> bits_list_to_bits(std::vector<std::vector<bool>>& list);

// Packing helpers
std::uint8_t unpack5bits(std::string const& value);
std::string pack5bits(std::uint8_t packed);
std::uint8_t unpack6bits(std::string const& value);
std::string pack6bits(std::uint8_t packed);
std::uint16_t unpack16bits(std::string const& value);
std::string pack16bits(std::uint16_t packed);
std::uint32_t unpack32bits(std::string const& value);
std::string pack32bits(std::uint32_t packed);
std::uint64_t unpack64bits(std::string const& value);
std::string pack64bits(std::uint64_t packed);
std::uint64_t unpack72bits(std::string const& value, std::uint8_t* pRem);
std::string pack72bits(std::uint64_t value, std::uint8_t rem);

std::uint32_t pack_alphanumeric22(std::string const& value, bool is_flag);
std::string unpack_alphanumeric22(std::uint32_t packed, bool* is_flag);
std::uint64_t pack_alphanumeric50(std::string const& value);
std::string unpack_alphanumeric50(std::uint64_t packed);

std::uint32_t pack_callsign(std::string const& value, bool* p_portable);
std::string unpack_callsign(std::uint32_t value, bool portable);

std::string deg2grid(float dlong, float dlat);
std::pair<float, float> grid2deg(std::string const& grid);
std::uint16_t pack_grid(std::string const& value);
std::string unpack_grid(std::uint16_t value);

std::uint8_t pack_num(std::string const& num, bool* ok);
std::uint8_t pack_pwr(std::string const& pwr, bool* ok);
std::uint8_t pack_cmd(std::uint8_t cmd, std::uint8_t num, bool* p_packed_num);
std::uint8_t unpack_cmd(std::uint8_t value, std::uint8_t* p_num);

bool is_snr_command(std::string const& cmd);
bool is_command_allowed(std::string const& cmd);
bool is_command_buffered(std::string const& cmd);
int is_command_checksummed(std::string const& cmd);
bool is_command_autoreply(std::string const& cmd);
bool is_valid_callsign(std::string const& callsign, bool* p_is_compound);
bool is_compound_callsign(std::string const& callsign);
bool is_group_allowed(std::string const& group);

// Message helpers
std::string pack_heartbeat_message(std::string const& text, std::string const& callsign, int* n);
std::vector<std::string> unpack_heartbeat_message(std::string const& text, std::uint8_t* pType, bool* isAlt, std::uint8_t* pBits3);

std::string pack_compound_message(std::string const& text, int* n);
std::vector<std::string> unpack_compound_message(std::string const& text, std::uint8_t* pType, std::uint16_t* pNum, std::uint8_t* pBits3);

std::string pack_compound_frame(std::string const& callsign, std::uint8_t type, std::uint16_t num, std::uint8_t bits3);
std::vector<std::string> unpack_compound_frame(std::string const& text, std::uint8_t* pType, std::uint16_t* pNum, std::uint8_t* pBits3);

std::string pack_directed_message(std::string const& text, std::string const& mycall, std::string* pTo, bool* pToCompound, std::string* pCmd, std::string* pNum, int* n);
std::vector<std::string> unpack_directed_message(std::string const& text, std::uint8_t* pType);

std::string pack_data_message(std::string const& text, int* n);
std::string unpack_data_message(std::string const& text);

std::string pack_fast_data_message(std::string const& text, int* n);
std::string unpack_fast_data_message(std::string const& text);

std::vector<std::pair<std::string, int>> build_message_frames(std::string const& mycall,
                                                              std::string const& mygrid,
                                                              std::string const& selectedCall,
                                                              std::string const& text,
                                                              bool forceIdentify,
                                                              bool forceData,
                                                              int submode,
                                                              MessageInfo* pInfo = nullptr);

// Optional backend to allow platform adapters to provide a reference implementation
// (e.g., reusing legacy Qt Varicode) until full core parity is achieved.
struct Backend {
  std::function<std::string(std::string const&, std::string const&, int*)> pack_heartbeat_message;
  std::function<std::vector<std::string>(std::string const&, std::uint8_t*, bool*, std::uint8_t*)> unpack_heartbeat_message;

  std::function<std::string(std::string const&, int*)> pack_compound_message;
  std::function<std::vector<std::string>(std::string const&, std::uint8_t*, std::uint16_t*, std::uint8_t*)> unpack_compound_message;

  std::function<std::string(std::string const&, std::uint8_t, std::uint16_t, std::uint8_t)> pack_compound_frame;
  std::function<std::vector<std::string>(std::string const&, std::uint8_t*, std::uint16_t*, std::uint8_t*)> unpack_compound_frame;

  std::function<std::string(std::string const&, std::string const&, std::string*, bool*, std::string*, std::string*, int*)> pack_directed_message;
  std::function<std::vector<std::string>(std::string const&, std::uint8_t*)> unpack_directed_message;

  std::function<std::string(std::string const&, int*)> pack_data_message;
  std::function<std::string(std::string const&)> unpack_data_message;

  std::function<std::string(std::string const&, int*)> pack_fast_data_message;
  std::function<std::string(std::string const&)> unpack_fast_data_message;

  std::function<std::vector<std::pair<std::string, int>>(std::string const&, std::string const&, std::string const&, std::string const&, bool, bool, int, MessageInfo*)> build_message_frames;
};

void set_backend(Backend const& backend);

}  // namespace js8core::protocol::varicode
