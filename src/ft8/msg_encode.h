/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4 message text for any callsign
 */

#pragma once

#include <stdbool.h>

#include <ft8lib/message.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A call FT8/FT4 packs in 28 bits: a plain call, with /P or /R at most.
 * It goes with a grid or a report in one message. Any other call
 * (KF0QGT/6Y, DL/SP2KMO, SP2KMO/MM, SP2KMO/QRP) is sent as plain text only
 * in a "type 4" message, which holds one more call (as a hash) and
 * RRR/RR73/73, but no grid and no report; elsewhere it goes as a hash.
 */
bool ftx_call_is_standard(const char *call);

/* Fits FT8/FT4 at all: standard, or plain text of up to 11 characters. */
bool ftx_call_is_usable(const char *call);

/*
 * ftx_message_encode(), plus the type 4 form WSJT-X sends for a
 * non-standard call: "<K1ABC> KF0QGT/6Y", "<K1ABC> KF0QGT/6Y RR73". The
 * ft8_lib in the image refuses it while the first call is in brackets.
 */
ftx_message_rc_t ftx_encode_text(ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if,
                                 const char *text);

#ifdef __cplusplus
}
#endif
