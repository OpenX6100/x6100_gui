/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#pragma once

#include <string>

namespace x6100::js8 {

/// What a rendered JS8 message is, for filtering and highlighting.
struct MessageClass {
    std::string from;          ///< sender, if the text starts "CALL: "
    std::string to;            ///< first token after the sender: callsign or @GROUP
    bool        heartbeat = false;
    bool        snr_report = false; ///< "CALL SNR -12": an answer to a heartbeat or SNR?
    bool        cq        = false;
    bool        to_me     = false; ///< addressed to my_call (or its base call)
    bool        to_group  = false; ///< addressed to an @GROUP (including @ALLCALL)
};

/// Classify rendered message text such as "K1ABC: W2XYZ SNR -12" or
/// "K1ABC: @HB HEARTBEAT FN42". `my_call` may be empty.
MessageClass classify(const std::string &text, const std::string &my_call);

enum class Checksum { None, Valid, Invalid };

/// For a complete message carrying a buffered, checksummed command (MSG,
/// MSG TO:, QUERY, relay ">", ...), verify the checksum the sender appended,
/// as desktop JS8Call does before acting on it. On Valid the checksum token
/// is removed from `text`; otherwise `text` is left unchanged.
Checksum verify_command_checksum(std::string &text);

/// A Maidenhead locator of 4, 6, 8 or 10 characters ("DN17", "CN89KG12AB").
/// "RR73" is the FT8/JS8 sign-off, not a grid in the Arctic Ocean.
bool is_grid(const std::string &word);

/// The grid in a message body, or "": the word after "GRID" if that's a
/// grid, else the last grid-looking word. Cut to 6 characters (subsquare,
/// a few km): what logs and the Stations list use.
std::string find_grid(const std::string &body);

/// The sender's own grid, as desktop takes one: from a heartbeat or a CQ
/// ("@HB HEARTBEAT FN42", "@ALLCALL CQ CQ CQ FN03") or "GRID xxxx" (the
/// GRID command, "@APRSIS GRID CN89", or "RR73 GRID EN34KS"), cut to 6
/// characters; "" otherwise. "MY DAUGHTER LIVES IN EM12" isn't theirs.
std::string announced_grid(const std::string &body);

/// Which grid to keep: `heard` replaces `known` unless it's a less precise
/// form of it ("DN17" after "DN17AB" keeps "DN17AB").
std::string better_grid(const std::string &known, const std::string &heard);

/// "EA8/G4ABC/P" -> "G4ABC": the longest slash-separated part containing a digit.
std::string base_callsign(const std::string &call);

} // namespace x6100::js8
