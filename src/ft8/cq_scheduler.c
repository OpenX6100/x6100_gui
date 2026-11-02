/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 CQ scheduler
 *
 *  Copyright (c) 2026
 */

#include "cq_scheduler.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <ft8lib/message.h>

#include "lvgl/lvgl.h"

/* `text` through the encoder: the text as it goes on the air into `out`. */
static bool encode_cq(const char *text, char *out) {
    ftx_message_t msg;
    char          buf[64];

    ftx_message_init(&msg);
    if (ftx_message_encode(&msg, NULL, text) != FTX_MESSAGE_RC_OK) {
        return false;
    }
    if (ftx_message_decode(&msg, NULL, buf) != FTX_MESSAGE_RC_OK) {
        return false;
    }
    strcpy(out, buf);
    return true;
}

/*
 * With a non-standard call (KF0QGT/6Y) a CQ is "CQ KF0QGT/6Y": the encoder
 * leaves the grid out itself, and there is no room for a modifier either,
 * so that goes too. `out` is empty when the call does not fit FT8 at all.
 */
void cq_make_message(const char *callsign,
                     const char *qth,
                     const char *cq_mod,
                     char       *out) {
    char buf[128];

    if (strlen(cq_mod)) {
        snprintf(buf, FTX_MAX_MESSAGE_LENGTH, "CQ_%s %s %.4s", cq_mod, callsign, qth);
    } else {
        snprintf(buf, FTX_MAX_MESSAGE_LENGTH, "CQ %s %.4s", callsign, qth);
    }
    if (encode_cq(buf, out)) {
        return;
    }
    LV_LOG_USER("Cannot encode '%s', trying without the modifier", buf);
    snprintf(buf, FTX_MAX_MESSAGE_LENGTH, "CQ %s", callsign);
    if (encode_cq(buf, out)) {
        return;
    }
    LV_LOG_USER("Cannot encode '%s'", buf);
    out[0] = '\0';
}
