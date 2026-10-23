/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 message macros
 *
 *  Desktop JS8Call's macros, as JS8Call-improved (44fa092) has them in
 *  mainwindow.cpp buildMacroValues() and mainwindow.h replaceMacros():
 *  <MYCALL>, <MYGRID4>, <MYGRID12>, <MYINFO>, <MYSTATUS>, <MYCQ>, <MYHB>,
 *  <MYREPLY>, <MYVERSION>, <MYIDLE>, and for the selected station <CALL>,
 *  <SNR> and <TDELTA>. Desktop fills them in saved messages, typed
 *  messages and the INFO and STATUS answers.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace x6100::js8 {

/// What the macros stand for: our station, and the selected one if any.
struct MacroInput {
    std::string my_call, my_grid, my_info, my_status, version;
    std::int64_t idle_ms = 0; ///< since the operator last touched the radio
    std::string  call;        ///< the selected station, "" if none
    std::optional<int> snr;   ///< how we hear it, dB
    std::optional<int> tdelta_ms; ///< its time offset (DT)
};

/// Desktop's macro table: "<MYCALL>" -> "VE7NHW" ... Keys that don't apply
/// (no station selected, an SNR of -31 or less) are left out, as desktop
/// does; <MYINFO>, <MYSTATUS>, <MYCQ>, <MYHB> and <MYREPLY> have their own
/// macros filled in.
std::map<std::string, std::string> macro_values(const MacroInput &in);

/// Desktop's replaceMacros(): each macro replaced (values in capitals);
/// `prune` then drops whatever <...> is left (a macro that doesn't apply),
/// as desktop does when a saved message is used. Without it they stay as
/// typed (desktop's menu labels, and what it sends from the text box).
std::string replace_macros(const std::string &text, const std::map<std::string, std::string> &values, bool prune);

/// Desktop's "since" for <MYIDLE>, counted in whole minutes as desktop's
/// idle timer is: "0M", "5M", "2H", "1D".
std::string idle_text(std::int64_t idle_ms);

/// The text uses <CALL>, <SNR> or <TDELTA>, which need a selected station.
bool macros_need_station(const std::string &text);

} // namespace x6100::js8
