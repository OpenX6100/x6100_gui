#include "js8core/protocol/jsc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "js8core/protocol/varicode.hpp"
#include "jsc_data.hpp"

#ifdef __ANDROID__
#include <android/log.h>
#define JSC_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "JSC", __VA_ARGS__)
#else
#define JSC_LOGD(...) do {} while(0)
#endif

namespace js8core::protocol::jsc {

Codeword codeword(std::uint32_t index, bool separate, std::uint32_t bytesize, std::uint32_t s, std::uint32_t c) {
  std::vector<Codeword> out;
  std::uint32_t v = ((index % s) << 1) + static_cast<std::uint32_t>(separate);
  out.insert(out.begin(), varicode::int_to_bits(v, bytesize + 1));
  std::uint32_t x = index / s;
  while (x > 0) {
    x -= 1;
    out.insert(out.begin(), varicode::int_to_bits((x % c) + s, bytesize));
    x /= c;
  }
  Codeword word;
  for (auto const& w : out) word.insert(word.end(), w.begin(), w.end());
  return word;
}

std::vector<CodewordPair> compress(std::string const& text) {
  std::vector<CodewordPair> out;
  constexpr std::uint32_t b = 4;
  constexpr std::uint32_t s = 7;
  constexpr std::uint32_t c = (1u << b) - s;

  JSC_LOGD("compress: input text='%s' len=%zu", text.c_str(), text.size());

  std::string space = " ";
  std::vector<std::string> words;
  std::size_t start = 0;
  while (start <= text.size()) {
    auto pos = text.find(' ', start);
    if (pos == std::string::npos) pos = text.size();
    words.push_back(text.substr(start, pos - start));
    if (pos == text.size()) break;
    start = pos + 1;
  }

  JSC_LOGD("compress: split into %zu words", words.size());

  for (std::size_t i = 0; i < words.size(); ++i) {
    auto w = words[i];
    bool isLastWord = (i == words.size() - 1);
    bool isSpaceCharacter = false;

    JSC_LOGD("compress: processing word[%zu]='%s' isLastWord=%d", i, w.c_str(), isLastWord);

    if (w.empty() && !isLastWord) {
      w = space;
      isSpaceCharacter = true;
    }

    while (!w.empty()) {
      bool ok = false;
      auto index = lookup(w, &ok);
      JSC_LOGD("compress: lookup w='%s' ok=%d index=%u", w.c_str(), ok, index);
      if (!ok) {
        JSC_LOGD("compress: lookup failed for '%s', breaking", w.c_str());
        break;
      }
      auto t = JSC_MAP[index];
      JSC_LOGD("compress: JSC_MAP[%u] size=%d str='%.*s'", index, t.size, t.size, t.str);
      w = w.substr(t.size);
      bool isLast = w.empty();
      bool shouldAppendSpace = isLast && !isSpaceCharacter && !isLastWord;
      auto cw = codeword(index, shouldAppendSpace, b, s, c);
      JSC_LOGD("compress: codeword bits=%zu shouldAppendSpace=%d", cw.size(), shouldAppendSpace);
      out.emplace_back(cw, static_cast<std::uint32_t>(t.size + (shouldAppendSpace ? 1 : 0)));
    }
  }

  JSC_LOGD("compress: output %zu codeword pairs", out.size());
  return out;
}

std::string decompress(Codeword const& bitvec) {
  constexpr std::uint32_t b = 4;
  constexpr std::uint32_t s = 7;
  constexpr std::uint32_t c = (1u << b) - s;

  #ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_ERROR, "JSC_DEBUG",
                      "decompress: bitvec.size=%zu, JSC_SIZE=%u", bitvec.size(), JSC_SIZE);
  #endif

  std::vector<std::string> out;

  std::uint32_t base[8];
  base[0] = 0;
  base[1] = s;
  base[2] = base[1] + s * c;
  base[3] = base[2] + s * c * c;
  base[4] = base[3] + s * c * c * c;
  base[5] = base[4] + s * c * c * c * c;
  base[6] = base[5] + s * c * c * c * c * c;
  base[7] = base[6] + s * c * c * c * c * c * c;

  std::vector<std::uint64_t> bytes;
  std::vector<std::uint32_t> separators;

  std::size_t i = 0;
  auto count = bitvec.size();
  while (i < count) {
    if (i + 4 > count) break;
    Codeword bbits(bitvec.begin() + i, bitvec.begin() + i + 4);
    std::uint64_t byte = varicode::bits_to_int(bbits);
    bytes.push_back(byte);
    i += 4;
    if (byte < s) {
      if (count - i > 0 && bitvec.at(i)) {
        separators.push_back(static_cast<std::uint32_t>(bytes.size() - 1));
      }
      i += 1;
    }
  }

  std::uint32_t start = 0;
  while (start < bytes.size()) {
    std::uint32_t k = 0;
    std::uint32_t j = 0;
    while (start + k < bytes.size() && bytes[start + k] >= s) {
      j = j * c + static_cast<std::uint32_t>(bytes[start + k] - s);
      k++;
    }
    if (j >= JSC_SIZE) break;
    if (start + k >= bytes.size()) break;
    j = j * s + static_cast<std::uint32_t>(bytes[start + k]) + base[k];
    if (j >= JSC_SIZE) break;

    auto word = std::string(JSC_MAP[j].str, JSC_MAP[j].size);
    #ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "JSC_DEBUG",
                        "decompress: j=%u, word='%s'", j, word.c_str());
    #endif
    out.push_back(word);
    if (!separators.empty() && separators.front() == start + k) {
      out.push_back(" ");
      separators.erase(separators.begin());
    }

    start = start + (k + 1);
  }

  std::string joined;
  for (auto const& w : out) joined += w;
  return joined;
}

bool exists(std::string const& w, std::uint32_t* pIndex) {
  bool found = false;
  auto idx = lookup(w, &found);
  if (pIndex) *pIndex = idx;
  return found && JSC_MAP[idx].size == static_cast<int>(w.size());
}

std::uint32_t lookup(std::string const& w, bool* ok) {
  return lookup(w.c_str(), ok);
}

std::uint32_t lookup(const char* b, bool* ok) {
  std::uint32_t index = 0;
  std::uint32_t count = 0;
  bool found = false;

  // prefix scan
  for (std::uint32_t i = 0; i < JSC_PREFIX_SIZE; ++i) {
    if (b[0] != JSC_PREFIX[i].str[0]) continue;
    if (JSC_PREFIX[i].size == 1) {
      if (ok) *ok = true;
      return JSC_LIST[JSC_PREFIX[i].index].index;
    }
    index = static_cast<std::uint32_t>(JSC_PREFIX[i].index);
    count = static_cast<std::uint32_t>(JSC_PREFIX[i].size);
    found = true;
    break;
  }
  if (!found) { if (ok) *ok = false; return 0; }

  for (std::uint32_t i = index; i < index + count; ++i) {
    auto len = static_cast<std::uint32_t>(JSC_LIST[i].size);
    if (std::strncmp(b, JSC_LIST[i].str, len) == 0) {
      if (ok) *ok = true;
      return static_cast<std::uint32_t>(JSC_LIST[i].index);
    }
  }
  if (ok) *ok = false;
  return 0;
}

}  // namespace js8core::protocol::jsc
