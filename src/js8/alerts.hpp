/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 alerts
 *
 *  Desktop JS8Call has notifications (a sound for CQ, heartbeats, ACKs,
 *  directed messages, inbox messages, new and worked callsigns) and
 *  highlight words: callsigns or words typed in Settings, matched against
 *  whole words of each decode (split at ':', '>' and spaces). Here both
 *  end in a beep and, for alert words, a highlighted row.
 */

#pragma once

#include <string>
#include <vector>

namespace x6100::js8 {

/// Alert words as typed ("ve7abc, @pota  sota") -> {"VE7ABC", "@POTA", "SOTA"}:
/// upper case, split at commas and spaces, duplicates dropped, at most 20.
std::vector<std::string> parse_alert_words(const std::string &typed);

/// "VE7ABC @POTA SOTA": how they're saved and shown.
std::string format_alert_words(const std::vector<std::string> &words);

/// The first alert word in a decode, or "". A word matches a whole word of
/// the text, as on desktop; a callsign also matches the sender's other
/// forms ("VE7ABC" matches "VE7ABC/P" and "VA7/VE7ABC").
std::string alert_word_hit(const std::string &text, const std::string &from, const std::vector<std::string> &words);

} // namespace x6100::js8
