/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 text helpers
 *
 *  The small string helpers the JS8 code shares (each file used to keep
 *  its own copy).
 */

#pragma once

#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace x6100::js8 {

/// Upper case (ASCII).
inline std::string upper(std::string_view s) {
    std::string out(s);
    for (auto &c : out) c = (char)std::toupper((unsigned char)c);
    return out;
}

/// Without whitespace at either end. Decoded text only ever has spaces;
/// files can have tabs and line ends.
inline std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return std::string(s.substr(a, b - a));
}

/// The words, split at any whitespace.
inline std::vector<std::string> words(std::string_view s) {
    std::istringstream       in{std::string(s)};
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

} // namespace x6100::js8
