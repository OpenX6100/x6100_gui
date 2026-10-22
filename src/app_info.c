/*
 * Xiegu X6100 - "Info" window of the WEFAX, NAVTEX, DSC and SSTV applications
 *
 * Copyright (C) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * See app_info.h.
 */

#include "app_info.h"
#include "buttons.h"
#include "events.h"
#include "keyboard.h"
#include "radio.h"
#include "styles.h"

#include <stdio.h>

#define INFO_W          795
#define INFO_H          350
#define INFO_TITLE_H    40
#define INFO_STEP_LINES 2

#define C_TITLE         0xffffff
#define C_TEXT          0xe8edf2
#define C_LINE          0x343d48

/* section heading (LVGL recolour) */
#define H(t)            "#6aaeff " t "#\n"

/* ---- texts -------------------------------------------------------------------- */

static const char *const titles[] = {
    "WEFAX - INFO", "NAVTEX / SITOR-B - INFO", "DSC - INFO", "SSTV - INFO",
};

static const char *const names[] = {
    "WEFAX", "NAVTEX / SITOR-B", "DSC", "SSTV",
};

static const char *const bodies[] = {
    /* WEFAX */
    H("WEFAX - WEATHER FAX RECEIVER")
    "WEFAX receives the weather charts that meteorological services broadcast on HF as an "
    "analog fax signal: 120 lines per minute, IOC 576, 1500 Hz audio centre, USB.\n\n"
    H("HOW TO USE IT")
    "Press Freq. to open the list of weather-fax stations with their daily schedules. Tune a "
    "station, then press Start Auto RX: the receiver waits for the start tone, draws the fax "
    "line by line and saves it at the end. If the broadcast has already begun, use Start "
    "Manual RX and save with Save and Clear.\n\n"
    H("CORRECTIONS")
    "Align moves the picture sideways, Tilt straightens a slanted picture. The Tilt is "
    "remembered for each station; Auto Slant measures it on the phasing signal of each fax.\n\n"
    H("FILES")
    "Saved faxes: /mnt/wefax (PNG). Open them with Fax Browser.\n\n"
    H("MICROPHONE")
    "V/M opens and closes the station list. UP/DOWN tune the previous/next station.",

    /* NAVTEX */
    H("NAVTEX / SITOR-B RECEIVER")
    "NavTex receives SITOR-B, the 100-baud FEC teleprinter mode used at sea, in two modes:\n"
    "NAVTEX - bulletins (ZCZC ... NNNN) shown as cards, with the quality of reception: OK, "
    "DAMAGED or UNREADABLE. Copies received again are merged.\n"
    "SITORB - the received text as it arrives, for any SITOR-B station.\n"
    "Bulletins are recognised and saved in both modes.\n\n"
    H("HOW TO USE IT")
    "Press Freq. and tune 518 kHz (international NAVTEX, English), 490 kHz (national "
    "languages), 4209.5 kHz (HF), a coast station or an HF MSI channel. The dial is set 1 kHz "
    "below the centre frequency, USB-D.\n\n"
    H("COLOURS IN A BULLETIN")
    "Yellow: uncertain character. Red *: lost character. Light blue: position. "
    "Green: digits repaired.\n\n"
    H("FILES")
    "Bulletins and texts: /mnt/navtex\n\n"
    H("MICROPHONE")
    "V/M opens and closes the frequency list. UP/DOWN tune the previous/next frequency.",

    /* DSC */
    H("DSC - DIGITAL SELECTIVE CALLING RECEIVER")
    "DSC is the digital calling system of the GMDSS. Ships and coast stations call each other "
    "by MMSI to send distress alerts, urgency and safety messages, routine calls and tests. "
    "This application receives DSC on the MF/HF channels and shows every call in plain "
    "language, with the names and flags of the stations.\n\n"
    H("COLOURS")
    "Red DISTRESS, orange URGENCY, yellow SAFETY, green ROUTINE, lilac SELF TEST "
    "(a station testing its own equipment), grey unknown.\n\n"
    H("HOW TO USE IT")
    "Press Freq. and tune a distress and safety channel: 2187.5, 4207.5, 6312, 8414.5, "
    "12577 or 16804.5 kHz. Select a call and press MFK for the detail card. The QR code "
    "opens the ship's page on a phone.\n\n"
    H("RECEIVE ONLY")
    "This is not GMDSS equipment. Do not rely on it for the safety of a vessel.\n\n"
    H("FILES")
    "Calls: /mnt/dsc. Your own station names: /mnt/dsc/mmsi.txt (MMSI;Name).\n\n"
    H("MICROPHONE")
    "V/M opens and closes the channel list. UP/DOWN tune the previous/next channel.",

    /* SSTV */
    H("SSTV - SLOW SCAN TELEVISION")
    "SSTV sends still pictures as audio tones. This application receives Robot 8/36/72, "
    "Martin M1/M2, Scottie S1/S2/DX and transmits all of them except Scottie DX.\n\n"
    H("RECEIVE")
    "Start Auto RX waits for the VIS header, decodes the picture and saves it. Start Manual RX "
    "receives a picture already under way, in the mode chosen with RX Mode or by Autodetect. "
    "With Auto RX Restart ON a new VIS heard during a picture starts a new reception only if, "
    "within 3 seconds, the line syncs of the new picture follow it and those of the picture "
    "under way stop: a part of a picture that sounds like a VIS no longer breaks the reception. "
    "Received pictures: /mnt/sstv, shown by RX Pics Browser.\n\n"
    H("DENOISE")
    "Denoise (page 2) cleans the received picture; the button changes OFF / CLASSIC / ML. "
    "CLASSIC rebuilds the lines spoiled by QRM, removes the coloured dots and dashes and "
    "smooths the grain without blurring the edges. ML uses a small neural network trained on "
    "SSTV receptions: it cleans much more, takes about a second after the picture ends and "
    "leaves a clean picture as it is; very thin lines and small text may come out softer. "
    "With CLASSIC or ML the screen shows the cleaned picture and both are saved: the original "
    "and the cleaned one (_dn.png, marked DN in the browser).\n\n"
    H("TRANSMIT")
    "TX opens the transmit menu: CQ call, reply, replay (reply with a miniature of a received picture, chosen among the saved ones), picture only, "
    "repeat. Your pictures go in /mnt/sstv/tx_pics, text templates in /mnt/sstv/tx_text.\n"
    "While you set up TX the receiver keeps going (the line at the bottom of the picture "
    "tells what it is doing); it stops only when transmission starts, and a picture being "
    "received is saved with the part that has arrived.\n"
    "Transmission starts at the last confirmation: check frequency, power and antenna first.",
};

/* third-party parts named in the credits, "" = none */
static const char *const extras[] = {
    "",
    "SITOR-B demodulator from fldigi (W1HKJ and contributors, after JNX by Paul Lutus), "
    "as adapted by Franco Venturi.\n",
    "QR code generator by Project Nayuki (MIT License).\n",
    "",
};

/* credits and licence notice */
static const char credits_fmt[] =
    H("CREDITS")
    "%s application for the Xiegu X6100 by Hany El Imam\n"
    "www.hanynet.com - github.com/TheMurusTeam\n"
    "Originally from the 1KO125 firmware, part of OpenX6100, based on R1CBU x6100_gui.\n"
    "%s"
    "\n"
    "Copyright (C) 2026 Hany El Imam.\n"
    "Portions Copyright (C) 2022-2023 Belousov Oleg aka R1CBU and contributors.\n"
    "This program is free software under the GNU Lesser General Public License, version "
    "2.1 or later, and comes with ABSOLUTELY NO WARRANTY. "
    "Source: github.com/OpenX6100/x6100_gui";

/* ---- window --------------------------------------------------------------------- */

static lv_obj_t *win = NULL;
static lv_obj_t *body = NULL;
static lv_obj_t *prev_focus = NULL;
static bool prev_editing = false;
static buttons_page_t *prev_page = NULL;
static char credits[1024];

static void close_btn_cb(button_data_t *d) { (void)d; app_info_close(); }

static button_data_t btn_title = { .type = BTN_TEXT, .label = "(Info)", .press = NULL };
static button_data_t btn_close = { .type = BTN_TEXT, .label = "Close\nInfo", .press = close_btn_cb };
static buttons_page_t page_info = { .items = { &btn_title, NULL, NULL, NULL, &btn_close } };

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    int dir = 0;
    switch (key) {
    case KEY_VOL_LEFT_EDIT:
    case KEY_VOL_LEFT_SELECT:   radio_change_vol(-1); return;
    case KEY_VOL_RIGHT_EDIT:
    case KEY_VOL_RIGHT_SELECT:  radio_change_vol(1); return;
    case LV_KEY_LEFT:
    case LV_KEY_UP:             dir = -1; break;
    case LV_KEY_RIGHT:
    case LV_KEY_DOWN:           dir = 1; break;
    case LV_KEY_ESC:            app_info_close(); return;
    default:                    return;
    }
    lv_coord_t step = INFO_STEP_LINES * lv_font_get_line_height(&sony_22);
    lv_coord_t room = dir > 0 ? lv_obj_get_scroll_bottom(body) : lv_obj_get_scroll_y(body);
    if (room > step) room = step;
    if (room > 0) lv_obj_scroll_by(body, 0, -dir * room, LV_ANIM_OFF);
}

static void pressed_cb(lv_event_t *e)
{
    (void)e;
    app_info_close();
}

static lv_obj_t *text_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(l, true);
    lv_obj_set_style_text_font(l, &sony_22, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_label_set_text_static(l, text);
    return l;
}

void app_info_open(lv_obj_t *dialog_obj, app_info_id_t id)
{
    if (win || !dialog_obj || id < APP_INFO_WEFAX || id > APP_INFO_SSTV) return;

    prev_page = buttons_get_cur_page();
    prev_focus = lv_group_get_focused(keyboard_group);
    prev_editing = lv_group_get_editing(keyboard_group);
    if (prev_focus) lv_group_remove_obj(prev_focus);

    /* same frame as the BIG dialog, on the main-screen parent */
    win = lv_obj_create(lv_obj_get_parent(dialog_obj));
    lv_obj_remove_style_all(win);
    lv_obj_add_style(win, &style.dialog.base, 0);
    lv_obj_set_size(win, INFO_W, INFO_H);
    lv_obj_center(win);
    lv_obj_set_style_translate_y(win, 4, 0);
    lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(win);
    lv_obj_set_style_text_font(title, &sony_22, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 18, 8);
    lv_label_set_text_static(title, titles[id]);

    body = lv_obj_create(win);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, INFO_W - 16, INFO_H - INFO_TITLE_H - 10);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, INFO_TITLE_H);
    lv_obj_set_style_pad_hor(body, 12, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(body, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(body, pressed_cb, LV_EVENT_PRESSED, NULL);

    text_label(body, bodies[id]);

    lv_obj_t *line = lv_obj_create(body);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    snprintf(credits, sizeof(credits), credits_fmt, names[id], extras[id]);
    text_label(body, credits);

    lv_group_add_obj(keyboard_group, body);
    lv_group_focus_obj(body);
    lv_group_set_editing(keyboard_group, true);
    buttons_load_page(&page_info);
}

static void destroy(void)
{
    if (body && lv_obj_get_group(body)) lv_group_remove_obj(body);
    if (win) lv_obj_del(win);
    win = body = NULL;
}

void app_info_close(void)
{
    if (!win) return;
    destroy();
    if (prev_focus) {
        lv_group_add_obj(keyboard_group, prev_focus);
        lv_group_focus_obj(prev_focus);
        lv_group_set_editing(keyboard_group, prev_editing);
    }
    prev_focus = NULL;
    if (prev_page) buttons_load_page(prev_page);
    prev_page = NULL;
}

void app_info_abort(void)
{
    destroy();
    prev_focus = NULL;
    prev_page = NULL;
}

bool app_info_is_open(void)
{
    return win != NULL;
}
