/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4 contest and DXpedition messages
 */

#pragma once

#include <ft8lib/message.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest text ftx_contest_decode() writes, with its '\0'. */
#define FTX_CONTEST_TEXT_LEN 40

/* The 77-bit messages ft8_lib can't unpack, as WSJT-X shows them
 * (lib/77bit/packjt77.f90, unpack77):
 *   0.1  K1ABC RR73; W9XYZ <KH1/KH7Z> -11    DXpedition (Fox/Hound)
 *   0.3  WA9XYZ KA1ABC R 16A EMA             ARRL Field Day (0.4: 17-32 tx)
 *   3    TU; W9XYZ K1ABC R 579 MA            ARRL RTTY Roundup (or a serial)
 *   5    <PA3XYZ> <G4ABC/P> R 590003 IO91NP  EU VHF contest
 * Callsigns go through ft8_lib's own unpacking and hash table. False for
 * anything else or a field out of range. */
bool ftx_contest_decode(const ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, char *text, size_t len);

#ifdef __cplusplus
}
#endif
