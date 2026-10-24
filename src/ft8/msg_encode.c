/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4 message text for any callsign
 */

#include "msg_encode.h"

#include <stdio.h>
#include <string.h>

#define FTX_CALL_MAX 11 /* plain text in a type 4 message */

/* `text` goes through FT8 unchanged: encoded, decoded, the same text. */
static bool round_trip(const char *text, ftx_message_type_t *type) {
    ftx_message_t msg;
    char          out[64];

    ftx_message_init(&msg);
    if (ftx_message_encode(&msg, NULL, text) != FTX_MESSAGE_RC_OK) {
        return false;
    }
    if (ftx_message_decode(&msg, NULL, out) != FTX_MESSAGE_RC_OK) {
        return false;
    }
    if (type) {
        *type = ftx_message_get_type(&msg);
    }
    return strcmp(text, out) == 0;
}

static bool call_ok(const char *call) {
    return call && call[0] && strlen(call) <= FTX_CALL_MAX && !strchr(call, ' ');
}

bool ftx_call_is_standard(const char *call) {
    char               text[40];
    ftx_message_type_t type;

    if (!call_ok(call)) {
        return false;
    }
    snprintf(text, sizeof(text), "CQ %s AA00", call);
    return round_trip(text, &type) && type == FTX_MESSAGE_TYPE_STANDARD;
}

bool ftx_call_is_usable(const char *call) {
    char text[40];

    if (!call_ok(call)) {
        return false;
    }
    if (ftx_call_is_standard(call)) {
        return true;
    }
    snprintf(text, sizeof(text), "CQ %s", call);
    return round_trip(text, NULL);
}

static bool is_short_ack(const char *extra) {
    return !strcmp(extra, "RRR") || !strcmp(extra, "RR73") || !strcmp(extra, "73");
}

ftx_message_rc_t ftx_encode_text(ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if,
                                 const char *text) {
    char   to[16], de[16], extra[16], rest[4];
    int    n = sscanf(text, "%15s %15s %15s %3s", to, de, extra, rest);
    size_t len = n >= 1 ? strlen(to) : 0;

    if ((n == 2 || (n == 3 && is_short_ack(extra))) && len > 2 && to[0] == '<' && to[len - 1] == '>' &&
        !ftx_call_is_standard(de)) {
        char bare[16];

        snprintf(bare, sizeof(bare), "%.*s", (int)(len - 2), to + 1);
        /* The ft8_lib in the image wants the hashed call bare; a fixed one
         * may want it in brackets, as the text has it. */
        ftx_message_init(msg);
        if (ftx_message_encode_nonstd(msg, hash_if, bare, de, n == 3 ? extra : "") == FTX_MESSAGE_RC_OK) {
            return FTX_MESSAGE_RC_OK;
        }
        ftx_message_init(msg);
        if (ftx_message_encode_nonstd(msg, hash_if, to, de, n == 3 ? extra : "") == FTX_MESSAGE_RC_OK) {
            return FTX_MESSAGE_RC_OK;
        }
    }
    ftx_message_init(msg);
    return ftx_message_encode(msg, hash_if, text);
}
