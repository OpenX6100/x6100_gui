/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Broadcast Info pop-up (microphone GENE key) and Save as channel (NW key).
 *  Written by Hany El Imam (1KO125) in main_screen.c, moved here unchanged.
 */

#include "broadcast_overlay.h"

#include <stdio.h>

#include "styles.h"
#include "msg.h"
#include "cfg/cfg_api.h"
#include "dialog_channels.h"
#include "channels.h"
#include "broadcast_db.h"

static lv_obj_t *broadcast_overlay = NULL;
static lv_obj_t *broadcast_overlay_label = NULL;
static lv_obj_t *broadcast_overlay_counter = NULL;
static lv_obj_t *broadcast_overlay_name = NULL;
static lv_timer_t *broadcast_overlay_timer = NULL;
static broadcast_match_t broadcast_matches[BROADCAST_MAX_MATCHES];
static size_t broadcast_match_count = 0;
static size_t broadcast_match_index = 0;
static int32_t broadcast_search_frequency = 0;

static void broadcast_overlay_timer_cb(lv_timer_t *timer) {

    if (broadcast_overlay) {
        lv_obj_del(broadcast_overlay);
        broadcast_overlay = NULL;
        broadcast_overlay_label = NULL;
        broadcast_overlay_counter = NULL;
        broadcast_overlay_name = NULL;
    }

    broadcast_overlay_timer = NULL;
    broadcast_match_count = 0;
    broadcast_match_index = 0;
}


static void broadcast_overlay_render(void) {

    if (!broadcast_overlay) {
        /*
         * Build the Broadcast Info panel exactly like the common
         * RTTY/NAVTEX panel: a label using panel_style.
         *
         * It lives on lv_layer_top(), so it is shown above everything,
         * including app dialogs (WEFAX, SSTV, ...) and the CHANNELS window.
         * It never takes input, so it does not disturb the app below.
         */
        broadcast_overlay = lv_label_create(lv_layer_top());
        lv_label_set_text(broadcast_overlay, "");
        lv_obj_add_style(broadcast_overlay, &style.panels.base, 0);
        lv_obj_add_flag(broadcast_overlay, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_clear_flag(broadcast_overlay, LV_OBJ_FLAG_SCROLLABLE);

        /* Result counter, only shown when more than one match exists. */
        broadcast_overlay_counter = lv_label_create(broadcast_overlay);
        lv_obj_set_pos(broadcast_overlay_counter, 8, 2);
        lv_obj_set_style_text_color(broadcast_overlay_counter, lv_color_white(), 0);
        lv_obj_set_style_text_font(broadcast_overlay_counter, &sony_28, 0);

        /* Station name. */
        broadcast_overlay_name = lv_label_create(broadcast_overlay);
        lv_obj_set_width(broadcast_overlay_name, 735);
        lv_obj_set_pos(broadcast_overlay_name, 20, 4);
        lv_label_set_long_mode(broadcast_overlay_name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(broadcast_overlay_name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(broadcast_overlay_name, lv_color_white(), 0);
        lv_obj_set_style_text_font(broadcast_overlay_name, &sony_38, 0);

        /* Broadcast details, fitted inside the standard 795 x 182 panel. */
        broadcast_overlay_label = lv_label_create(broadcast_overlay);
        lv_obj_set_width(broadcast_overlay_label, 755);
        lv_obj_set_pos(broadcast_overlay_label, 10, 48);
        lv_label_set_long_mode(broadcast_overlay_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(broadcast_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(broadcast_overlay_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(broadcast_overlay_label, &sony_28, 0);
    }

    if (broadcast_match_count > 0) {
        broadcast_match_t *m = &broadcast_matches[broadcast_match_index];
        int sh = m->start_minute / 60;
        int sm = m->start_minute % 60;
        int eh = m->stop_minute / 60;
        int em = m->stop_minute % 60;
        char lang[64];
        char target[80];

        snprintf(lang, sizeof(lang), "%s", m->language[0] ? m->language : "-");
        snprintf(target, sizeof(target), "%s", m->target[0] ? m->target : "-");

        if (broadcast_match_count > 1) {
            lv_label_set_text_fmt(
                broadcast_overlay_counter,
                "%u/%u",
                (unsigned)(broadcast_match_index + 1),
                (unsigned)broadcast_match_count
            );
        } else {
            lv_label_set_text(broadcast_overlay_counter, "");
        }

        lv_label_set_text(broadcast_overlay_name, m->station);

        lv_label_set_text_fmt(
            broadcast_overlay_label,
            "%.3f kHz     %02d:%02d-%02d:%02d UTC\n"
            "Language: %s     Target: %s\n"
            "ITU: %s     Days: %s",
            m->freq_khz, sh, sm, eh, em,
            lang, target,
            m->itu[0] ? m->itu : "-",
            m->days[0] ? m->days : "Daily"
        );
    } else {
        lv_label_set_text(broadcast_overlay_counter, "");
        lv_label_set_text(broadcast_overlay_name, "NO STATION SCHEDULED");
        lv_label_set_text_fmt(
            broadcast_overlay_label,
            "\n%.3f kHz",
            (double)broadcast_search_frequency / 1000.0
        );
    }

    lv_obj_move_foreground(broadcast_overlay);
}

void broadcast_overlay_show(void) {

    /* If the panel is already visible, GENE means NEXT.  Do not repeat
     * the search: cycle the result set captured by the first press. */
    if (broadcast_overlay) {
        if (broadcast_match_count > 1) {
            broadcast_match_index = (broadcast_match_index + 1) % broadcast_match_count;
            broadcast_overlay_render();
        }
    } else {
        broadcast_search_frequency = cparam_i_get(cfg.cur.fg_freq());
        broadcast_match_count = broadcast_db_find_matches(
            broadcast_search_frequency,
            2000,
            broadcast_matches,
            BROADCAST_MAX_MATCHES
        );
        broadcast_match_index = 0;
        broadcast_overlay_render();
    }

    if (broadcast_overlay_timer) {
        lv_timer_reset(broadcast_overlay_timer);
    } else {
        broadcast_overlay_timer = lv_timer_create(broadcast_overlay_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(broadcast_overlay_timer, 1);
    }
}


static void broadcast_overlay_save_error(const char *detail) {
    if (broadcast_overlay) {
        lv_label_set_text(broadcast_overlay_counter, "");
        lv_label_set_text(broadcast_overlay_name, "SAVE ERROR");
        lv_label_set_text(broadcast_overlay_label, detail);
        lv_obj_move_foreground(broadcast_overlay);
    } else {
        msg_schedule_text_fmt("Channel save error: %s", detail);
    }
}

void broadcast_overlay_save_channel(void) {

    broadcast_match_t match;
    bool have_match = false;

    /* If Broadcast Info is visible, save the station currently selected
     * in the panel.  Otherwise perform a fresh lookup at the current VFO
     * frequency and use the first matching station. */
    if (broadcast_overlay && broadcast_match_count > 0) {
        match = broadcast_matches[broadcast_match_index];
        have_match = true;
    } else if (!broadcast_overlay) {
        broadcast_match_t matches[BROADCAST_MAX_MATCHES];
        size_t count = broadcast_db_find_matches(
            cparam_i_get(cfg.cur.fg_freq()),
            2000,
            matches,
            BROADCAST_MAX_MATCHES
        );

        if (count > 0) {
            match = matches[0];
            have_match = true;
        }
    }

    /* With the CHANNELS window open, the window holds the database: do not
     * reload it (a Drag in progress would be lost) and do not save while a
     * Drag or an Edit is in progress. */
    bool channels_open = dialog_channels_is_open();

    if (channels_open && dialog_channels_busy()) {
        broadcast_overlay_save_error("Finish Drag or Edit first");
        return;
    }

    /* Otherwise reload the persistent channel database before modifying it.
     * After boot the in-memory channel list may still be empty if the
     * Channels dialog has never been opened. */
    if (!channels_open && !channels_load()) {
        if (broadcast_overlay) {
            lv_label_set_text(broadcast_overlay_counter, "");
            lv_label_set_text(broadcast_overlay_name, "SAVE ERROR");
            lv_label_set_text(broadcast_overlay_label, "Unable to load channels");
        } else {
            msg_schedule_text_fmt("Channel save error");
        }
        return;
    }

    if (channels_count() >= CHANNELS_MAX) {
        broadcast_overlay_save_error("Channel list full");
        return;
    }

    if (!channels_add_current()) {
        if (broadcast_overlay) {
            lv_label_set_text(broadcast_overlay_counter, "");
            lv_label_set_text(broadcast_overlay_name, "SAVE ERROR");
            lv_label_set_text(broadcast_overlay_label, "Unable to add channel");
        } else {
            msg_schedule_text_fmt("Channel save error");
        }
        return;
    }

    uint16_t count = channels_count();

    if (have_match && count > 0) {
        if (!channels_set_name(count - 1, match.station)) {
            /* channels_add_current() has already saved the new record with
             * an empty name. Remove it again if assigning the station name
             * fails, so we don't leave a bogus channel behind. */
            channels_delete(count - 1);

            if (broadcast_overlay) {
                lv_label_set_text(broadcast_overlay_counter, "");
                lv_label_set_text(broadcast_overlay_name, "SAVE ERROR");
                lv_label_set_text(broadcast_overlay_label, "Unable to set channel name");
            } else {
                msg_schedule_text_fmt("Channel save error");
            }
            return;
        }
    }

    /* Show the new channel in the open CHANNELS window. */
    if (channels_open) {
        dialog_channels_show_added();
    }

    if (broadcast_overlay) {
        lv_label_set_text(broadcast_overlay_counter, "");
        lv_label_set_text(broadcast_overlay_name, "CHANNEL SAVED");
        lv_label_set_text(
            broadcast_overlay_label,
            have_match ? match.station : ""
        );

        if (broadcast_overlay_timer) {
            lv_timer_reset(broadcast_overlay_timer);
        } else {
            broadcast_overlay_timer = lv_timer_create(broadcast_overlay_timer_cb, 1000, NULL);
            lv_timer_set_repeat_count(broadcast_overlay_timer, 1);
        }

        lv_obj_move_foreground(broadcast_overlay);
    } else {
        char message[96];

        if (have_match && match.station[0]) {
            snprintf(message, sizeof(message), "Saved channel %s", match.station);
        } else {
            snprintf(message, sizeof(message), "Saved channel");
        }

        msg_schedule_text_fmt("%s", message);
    }
}
