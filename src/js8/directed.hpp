/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 directed commands and relay paths
 *
 *  The pieces desktop JS8Call's processCommandActivity() works from: a
 *  decoded "FROM: TO CMD text" split the way its CommandDetail holds it, and
 *  the relay helpers (callToPattern, parseRelayPathCallsigns) with the same
 *  patterns, so relays go on, stop and answer where desktop's do.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace x6100::js8 {

/// A station heard lately: HEARING?, QUERY CALL and RETRIEVE MSG look here.
struct Heard {
    std::string  call;
    int          snr      = 0;
    std::int64_t heard_ms = 0;

    Heard() = default;
    Heard(const char *c) : call(c) {}
    Heard(std::string c, int s = 0, std::int64_t ms = 0) : call(std::move(c)), snr(s), heard_ms(ms) {}
};

/// A directed message as desktop's CommandDetail holds it. `cmd` is in
/// desktop's form, with its leading space (" MSG", " MSG TO:", " QUERY
/// MSGS", " SNR?"), ">" for a relay, or " " for plain text. `text` is what
/// follows the command, spaces on the left trimmed.
struct Directed {
    std::string from, to, cmd, text;
};

/// "N0XYZ: K2XYZ MSG TO: W1ABC HI" -> {"N0XYZ", "K2XYZ", " MSG TO:", "W1ABC HI"};
/// "N0XYZ: K2XYZ > W1ABC HI" (a relay) -> {..., ">", "W1ABC HI"}. The
/// command is found the way desktop's directed pattern finds it. nullopt
/// unless the text is "FROM: TO ...".
std::optional<Directed> parse_directed(const std::string &text);

/// The next hop of a relay, or nullopt where it ends. Desktop relays on when
/// the text after '>' starts with a callsign followed by ' ' or '>'
/// (callToPattern): "W1ABC HELLO" -> "W1ABC>HELLO", sent on with " *DE* FROM".
std::optional<std::string> relay_next_hop(const std::string &text);

/// Desktop's parseRelayPathCallsigns(): `from`, then each station named by
/// "*DE* CALL" or "VIA CALL" in `text`, the last named first. Joined with
/// '>' it's the way back: "K2XYZ>N0XYZ".
std::vector<std::string> relay_path_calls(const std::string &from, const std::string &text);

/// A relay that ends here starting with a command desktop answers (its
/// isCommandAutoreply set: SNR?, MSG, MSG TO:, QUERY MSGS, QUERY MSG n, ...):
/// that command and the rest of the text, e.g. "MSG TO:W1ABC HI *DE* N0XYZ"
/// -> {" MSG TO:", "W1ABC HI *DE* N0XYZ"}.
std::optional<std::pair<std::string, std::string>> relayed_command(const std::string &text);

/// A QUERY CALL as desktop's menu sends it, "K2XYZ QUERY CALL [CALLSIGN]?":
/// the '?' added when it's left off after the one call ("K2XYZ QUERY CALL
/// W1ABC", "@ALLCALL QUERY CALL W1ABC"). Anything else comes back as it is.
std::string query_call_question(const std::string &text);

/// Desktop's parseCallsigns(): the valid callsigns in `text`, grids left out.
std::vector<std::string> parse_callsigns(const std::string &text);

/// Desktop's isAllCallIncluded(): @ALLCALL, and @HB (heartbeats go there).
bool is_allcall(const std::string &to);

/// Desktop's buffered commands with a 16-bit checksum (MSG, MSG TO:, QUERY,
/// QUERY MSGS, QUERY CALL, CMD, relay): with text after them, they only
/// count when the checksum checks out.
bool is_checksummed_command(const std::string &cmd);

/// "K2XYZ>N0XYZ" -> "N0XYZ via K2XYZ", as desktop's inbox shows a path.
std::string path_display(const std::string &path);

} // namespace x6100::js8
