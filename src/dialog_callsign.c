/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO (suffix list)
 */

#include "dialog_callsign.h"

#include <stdio.h>
#include <string.h>

#include "radio.h"
#include "textarea_window.h"
#include "cfg/cfg_api.h"
#include "main_screen.h"
#include "dialog.h"
#include "digi_menu.h"
#include "keyboard.h"
#include "events.h"
#include "msg.h"
#include "voice.h"
#include "ft8/msg_encode.h"

#define CALLSIGN_MAX_LEN 15

/*
 * The callsign is typed bare, then a suffix is picked from a list. The
 * whole call (SP2KMO/P) is what is saved, so every mode that sends the
 * callsign (FT8, the logs, the digital-mode macros) uses it as it is.
 * A call with a suffix of its own (SP2KMO/HA, KF0QGT/6Y) is saved as
 * typed, without the list: a suffix on top of it (SP2KMO/HA/P) makes no
 * sense. A call with a prefix (HA/SP2KMO) gets the list. Mobile suffixes are left out, digital modes are not worked on
 * the move.
 */
typedef struct {
    const char *suffix;
    const char *what;
} suffix_t;

static const suffix_t suffixes[] = {
    {"",     "no suffix"},
    {"/P",   "portable"},
    {"/QRP", "low power"},
};

#define SUFFIX_COUNT ((int)(sizeof(suffixes) / sizeof(suffixes[0])))

static char base_call[CALLSIGN_MAX_LEN + 1];

static void construct_cb(lv_obj_t *parent);
static void destruct_cb();
static void key_cb(lv_event_t * e);

static dialog_t             dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .key_cb = key_cb
};

dialog_t                    *dialog_callsign = &dialog;

/* The listed suffix `call` ends in, 0 for none. */
static int suffix_of(const char *call) {
    size_t len = strlen(call);

    for (int i = 1; i < SUFFIX_COUNT; i++) {
        size_t n = strlen(suffixes[i].suffix);

        if (len > n && strcmp(call + len - n, suffixes[i].suffix) == 0) {
            return i;
        }
    }
    return 0;
}

/* `call` without its listed suffix into `out`. */
static void strip_suffix(const char *call, char *out, size_t out_size) {
    size_t len = strlen(call) - strlen(suffixes[suffix_of(call)].suffix);

    snprintf(out, out_size, "%.*s", (int)len, call);
}

/* After its last slash a call has the home call (HA/SP2KMO, the longest
 * part) or a suffix of its own (SP2KMO/HA). */
static bool has_own_suffix(const char *call) {
    const char *last = strrchr(call, '/');

    return last != NULL && strlen(last + 1) < (size_t)(last - call);
}

static void save_call(const char *full) {
    if (strlen(full) > CALLSIGN_MAX_LEN) {
        msg_schedule_text_fmt("%s is too long, not saved", full);
        return;
    }
    param_t_set(cfg.callsign(), full);

    /* Only FT8/FT4 have no room for the grid beside such a call; the other
     * modes send it as typed. */
    if (ftx_call_is_standard(full)) {
        msg_schedule_text_fmt("Callsign: %s", full);
    } else if (ftx_call_is_usable(full)) {
        msg_schedule_text_fmt("Callsign: %s (FT8/FT4: QTH not sent)", full);
    } else {
        msg_schedule_text_fmt("Callsign: %s (too long for FT8/FT4)", full);
    }
    voice_say_text_fmt("Callsign %s", full);
}

static void suffix_picked(int item, void *ctx) {
    char full[CALLSIGN_MAX_LEN + 8];

    (void)ctx;
    if (item < 0 || item >= SUFFIX_COUNT) {
        return;
    }
    snprintf(full, sizeof(full), "%s%s", base_call, suffixes[item].suffix);
    save_call(full);
}

static void open_suffix_list(int now) {
    static char        text[SUFFIX_COUNT][CALLSIGN_MAX_LEN + 48];
    static const char *items[SUFFIX_COUNT];

    for (int i = 0; i < SUFFIX_COUNT; i++) {
        snprintf(text[i], sizeof(text[i]), "%s%s   %s%s", base_call, suffixes[i].suffix, suffixes[i].what,
                 i == now ? "   (now)" : "");
        items[i] = text[i];
    }
    digi_menu_open(lv_scr_act(), "Suffix: MFK to choose, press to save", items, SUFFIX_COUNT, suffix_picked,
                   NULL, NULL);
    /* The cursor on the one saved now: the list has the focus and moves
     * as the MFK moves it. */
    for (int i = 0; i < now; i++) {
        uint32_t key = LV_KEY_DOWN;

        lv_event_send(lv_group_get_focused(keyboard_group), LV_EVENT_KEY, &key);
    }
    voice_say_text_fmt("Suffix list");
}

static bool edit_ok() {
    char typed[CALLSIGN_MAX_LEN + 1];
    int  now;

    snprintf(typed, sizeof(typed), "%s", textarea_window_get());
    if (typed[0] == '\0') {
        param_t_set(cfg.callsign(), "");
        msg_schedule_text_fmt("Callsign cleared");
        dialog_destruct(&dialog);
        return true;
    }

    /* SP2KMO/P typed in full picks /P in the list; otherwise the list
     * marks the suffix saved now. */
    now = suffix_of(typed);
    if (now == 0) {
        now = suffix_of(PARAM_T_GET(cfg.callsign()));
    }
    strip_suffix(typed, base_call, sizeof(base_call));

    dialog_destruct(&dialog);
    if (has_own_suffix(base_call)) {
        save_call(typed);
        return true;
    }
    open_suffix_list(now);
    return true;
}

static bool edit_cancel() {
    dialog_destruct(&dialog);
    return true;
}

/* Callsigns are capitals only: take lowercase (a USB or Bluetooth keyboard
 * without Caps Lock) as capitals instead of dropping it silently. LVGL
 * inserts the replacement as if typed, so the accepted list still applies. */
static void insert_cb(lv_event_t *e) {
    const char *in = lv_event_get_param(e);
    static char up[8];
    size_t      n = strlen(in);
    if (n >= sizeof(up)) return;
    bool lower = false;
    for (size_t i = 0; i <= n; i++) {
        up[i] = in[i] >= 'a' && in[i] <= 'z' ? (char)(in[i] - 'a' + 'A') : in[i];
        lower |= up[i] != in[i];
    }
    if (lower) lv_textarea_set_insert_replace(lv_event_get_target(e), up);
}

static void construct_cb(lv_obj_t *parent) {
    char call[CALLSIGN_MAX_LEN + 8];

    digi_menu_close();
    dialog.obj = textarea_window_open(edit_ok, edit_cancel);

    lv_obj_t *text = textarea_window_text();

    lv_textarea_set_accepted_chars(text,
        "0123456789/"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    );

    lv_textarea_set_max_length(text, CALLSIGN_MAX_LEN);
    lv_textarea_set_placeholder_text(text, "Callsign");
    lv_obj_add_event_cb(text, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(text, insert_cb, LV_EVENT_INSERT, NULL);

    /* The bare call: the suffix is picked again from the list. */
    strip_suffix(PARAM_T_GET(cfg.callsign()), call, sizeof(call));
    textarea_window_set(call);
}

static void destruct_cb() {
    textarea_window_close();
    dialog.obj = NULL;
}

/* ENTER and ESC are the text window's own (edit_ok, edit_cancel). */
static void key_cb(lv_event_t * e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}
