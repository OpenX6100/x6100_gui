/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 WeFax app - stations list and broadcast schedules
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
 * This file is NOT a separate compilation unit: it is #included at the end
 * of dialog_wefax.c and must not be added to CMakeLists.txt.
 *
 * "Freq." (FAX 1:4) opens a window over the WEFAX dialog, the same size
 * and place as the BIG view, so the fax view, its zoom and the Fax Browser
 * are left untouched and reception goes on underneath.
 *
 *   stations list   MFK turn: select   MFK press: tune   VOL press: schedule
 *   schedule        MFK turn: select   MFK press: tune   VOL press: back
 *
 * Data: wefax/stations.c (from weatherfax.com, see make_wefax_db.py).
 * The audio centre of the decoder is 1500 Hz: dial = frequency - 1.5 kHz, USB.
 */

#include "wefax/stations.h"
#include "cfg/cfg_api.h"

#define FREQ_CENTER_HZ      1500
#define FREQ_W              795
#define FREQ_H              350
#define FREQ_TITLE_H        34
#define FREQ_FOOTER_H       60
#define FREQ_FOOTER_COLOR   0x6aaeff      /* light blue */

static lv_obj_t *freq_win = NULL;
static lv_obj_t *freq_title = NULL;
static lv_obj_t *freq_footer = NULL;
static lv_obj_t *freq_list = NULL;          /* stations */
static lv_obj_t *freq_sched = NULL;         /* schedule of one row */
static lv_timer_t *freq_timer = NULL;
static lv_obj_t *freq_prev_focus = NULL;
static bool freq_prev_editing = false;
static int freq_sched_row = -1;
static int freq_last_minute = -1;
static char freq_btn_label[32] = "Freq.";

static void freq_close(void);
static void freq_tune_cb(button_data_t *data);
static void freq_sched_btn_cb(button_data_t *data);
static void freq_back_cb(button_data_t *data);
static void freq_close_cb(button_data_t *data);

static button_data_t btn_fl_title  = { .type = BTN_TEXT, .label = "(Stations)" };
static button_data_t btn_fl_tune   = { .type = BTN_TEXT, .label = "Tune", .press = freq_tune_cb };
static button_data_t btn_fl_sched  = { .type = BTN_TEXT, .label = "Schedule", .press = freq_sched_btn_cb };
static button_data_t btn_fl_close  = { .type = BTN_TEXT, .label = "Close\nList", .press = freq_close_cb };
static button_data_t btn_fs_title  = { .type = BTN_TEXT, .label = "(Schedule)" };
static button_data_t btn_fs_back   = { .type = BTN_TEXT, .label = "Back", .press = freq_back_cb };

static buttons_page_t btn_page_freq_list = {
    .items = { &btn_fl_title, &btn_fl_tune, &btn_fl_sched, NULL, &btn_fl_close }
};

static buttons_page_t btn_page_freq_sched = {
    .items = { &btn_fs_title, &btn_fl_tune, &btn_fs_back, NULL, &btn_fl_close }
};

/* ---- helpers ---------------------------------------------------------------- */

static int freq_now_minute(void)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

static void freq_fmt_khz(char *dst, size_t sz, int32_t hz)
{
    snprintf(dst, sz, "%ld.%ld", (long)(hz / 1000), (long)((hz % 1000) / 100));
}

/* signal centre of the current tuning (USB) */
static int32_t freq_current_hz(void)
{
    return cparam_i_get(cfg.cur.fg_freq()) + FREQ_CENTER_HZ;
}

static void freq_fmt_wait(char *dst, size_t sz, int wait)
{
    if (wait == 0) snprintf(dst, sz, "now");
    else if (wait < 60) snprintf(dst, sz, "in %d min", wait);
    else snprintf(dst, sz, "in %dh%02d", wait / 60, wait % 60);
}

/*
 * "Freq." button: the callsign of the station recalled from the list, until
 * the frequency is changed by hand (VFO); then the frequency in kHz.
 */
static int freq_recalled_row = -1;          /* station tuned from the list */
static int32_t freq_recalled_dial = 0;      /* ... and its dial frequency */
static int32_t freq_shown_dial = 0;
static lv_timer_t *freq_watch_timer = NULL;

static const char *freq_label_getter(void)
{
    int32_t dial = cparam_i_get(cfg.cur.fg_freq());
    int32_t hz = dial + FREQ_CENTER_HZ;
    freq_shown_dial = dial;
    if (freq_recalled_row >= 0 && dial != freq_recalled_dial) freq_recalled_row = -1;
    int r = freq_recalled_row;
    if (r >= 0 && wefax_db_row(r)) {
        snprintf(freq_btn_label, sizeof(freq_btn_label), "Freq.\n%s", wefax_db_row(r)->call);
    } else {
        char k[16];
        freq_fmt_khz(k, sizeof(k), hz);
        snprintf(freq_btn_label, sizeof(freq_btn_label), "Freq.\n%s", k);
    }
    return freq_btn_label;
}

static void freq_tune_row(int row)
{
    const wefax_row_t *r = wefax_db_row(row);
    if (!r) return;
    cparam_i_set(cfg.cur.mode(), x6100_mode_usb);
    cparam_i_set(cfg.cur.fg_freq(), r->hz - FREQ_CENTER_HZ);
    freq_recalled_row = row;
    freq_recalled_dial = r->hz - FREQ_CENTER_HZ;
    char k[16];
    freq_fmt_khz(k, sizeof(k), r->hz);
    msg_update_text_fmt("WeFax %s %s kHz", r->call, k);
}

/* the label follows the VFO (and the RX buttons the decoder): 4 times a second */
static void freq_watch_cb(lv_timer_t *t)
{
    (void)t;
    rx_buttons_sync(false);
    if (cparam_i_get(cfg.cur.fg_freq()) != freq_shown_dial && btn_freq.disp_btn)
        buttons_refresh(&btn_freq);
}

static void freq_start(void)
{
    if (!freq_watch_timer) freq_watch_timer = lv_timer_create(freq_watch_cb, 250, NULL);
}

static void freq_stop(void)
{
    if (freq_watch_timer) { lv_timer_del(freq_watch_timer); freq_watch_timer = NULL; }
    freq_recalled_row = -1;
}

/* ---- title and footer ----------------------------------------------------- */

static void freq_update_title(void)
{
    if (!freq_title) return;
    int now = freq_now_minute();
    if (freq_sched && freq_sched_row >= 0) {
        const wefax_row_t *r = wefax_db_row(freq_sched_row);
        char k[16];
        freq_fmt_khz(k, sizeof(k), r->hz);
        lv_label_set_text_fmt(freq_title, "%s %s  %s kHz  -  schedule  (UTC %02d:%02d)",
                              r->call, r->place, k, now / 60, now % 60);
    } else {
        lv_label_set_text_fmt(freq_title, "WEFAX STATIONS  (UTC %02d:%02d)", now / 60, now % 60);
    }
}

static void freq_update_footer(void)
{
    if (!freq_footer) return;
    int now = freq_now_minute();
    char t[256], w[24], k[16], d[16];

    if (freq_sched) {
        const wefax_entry_t *e;
        int n = wefax_db_schedule(freq_sched_row, &e);
        int i = ui_list_get_selected(freq_sched);
        if (i < 0 || i >= n) { lv_label_set_text(freq_footer, ""); return; }
        freq_fmt_wait(w, sizeof(w), wefax_db_wait(&e[i], now));
        snprintf(t, sizeof(t), "%02d:%02d UTC  (%s)  %s", e[i].minute / 60, e[i].minute % 60, w, e[i].text);
        lv_label_set_text(freq_footer, t);
        return;
    }

    int row = freq_list ? ui_list_get_selected(freq_list) : -1;
    const wefax_row_t *r = wefax_db_row(row);
    if (!r) { lv_label_set_text(freq_footer, ""); return; }
    freq_fmt_khz(k, sizeof(k), r->hz);
    freq_fmt_khz(d, sizeof(d), r->hz - FREQ_CENTER_HZ);
    int L = snprintf(t, sizeof(t), "%s %s  %s kHz  (dial %s USB)", r->call, r->place, k, d);
    const wefax_entry_t *e;
    int n = wefax_db_schedule(row, &e);
    int nx = wefax_db_next(e, n, now);
    if (nx >= 0) {
        freq_fmt_wait(w, sizeof(w), wefax_db_wait(&e[nx], now));
        L += snprintf(t + L, sizeof(t) - (size_t)L, "\nNext %02d:%02d %s: %s",
                      e[nx].minute / 60, e[nx].minute % 60, w, e[nx].text);
    } else {
        L += snprintf(t + L, sizeof(t) - (size_t)L, "\nNo schedule available");
    }
    if (r->note[0] && L < (int)sizeof(t) - 4)
        snprintf(t + L, sizeof(t) - (size_t)L, " - %s", r->note);
    lv_label_set_text(freq_footer, t);
}

/* ---- the two lists ------------------------------------------------------------ */

static void freq_list_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    const wefax_row_t *r = wefax_db_row(index);
    if (!r) return;
    if (r->first) snprintf(row->cell[0], UI_LIST_CELL_LEN, "%s", r->country);
    snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s  %s", r->call, r->place);
    freq_fmt_khz(row->cell[2], UI_LIST_CELL_LEN, r->hz);

    const wefax_entry_t *e;
    int n = wefax_db_schedule(index, &e);
    int now = freq_now_minute();
    int nx = wefax_db_next(e, n, now);
    if (nx >= 0) snprintf(row->cell[3], UI_LIST_CELL_LEN, "%02d:%02d", e[nx].minute / 60, e[nx].minute % 60);
    else row->muted = true;
    row->mark = (r->hz == freq_current_hz());
}

static void freq_sched_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    const wefax_entry_t *e;
    int n = wefax_db_schedule(freq_sched_row, &e);
    if (index < 0 || index >= n) return;
    int now = freq_now_minute();
    int nx = wefax_db_next(e, n, now);
    snprintf(row->cell[0], UI_LIST_CELL_LEN, "%02d:%02d", e[index].minute / 60, e[index].minute % 60);
    snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", e[index].text);
    if (index == nx) {
        row->mark = true;
        freq_fmt_wait(row->cell[2], UI_LIST_CELL_LEN, wefax_db_wait(&e[index], now));
    } else if (e[index].minute < now) {
        row->muted = true;          /* already sent today */
    }
}

static void freq_list_activate_cb(void *user, int index)
{
    (void)user;
    freq_tune_row(index);
    freq_close();
}

static void freq_sched_activate_cb(void *user, int index)
{
    (void)user;
    (void)index;
    freq_tune_row(freq_sched_row);
    freq_close();
}

static void freq_open_schedule(void);
static void freq_close_schedule(void);

/* VOL press arrives as LV_KEY_ESC */
static void freq_key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    if (key != LV_KEY_ESC) return;
    if (freq_sched) freq_close_schedule();
    else freq_open_schedule();
}

static void freq_changed_cb(lv_event_t *e)
{
    (void)e;
    freq_update_footer();
}

static void freq_focus(lv_obj_t *obj)
{
    lv_group_add_obj(keyboard_group, obj);
    lv_group_focus_obj(obj);
    lv_group_set_editing(keyboard_group, true);
}

static lv_obj_t *freq_make_list(const ui_list_cfg_t *cfg)
{
    lv_obj_t *l = ui_list_create(freq_win, cfg);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, FREQ_TITLE_H);
    lv_obj_add_event_cb(l, freq_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(l, freq_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return l;
}

static void freq_open_schedule(void)
{
    if (freq_sched || !freq_list) return;
    int row = ui_list_get_selected(freq_list);
    const wefax_entry_t *e;
    int n = wefax_db_schedule(row, &e);
    if (n <= 0) {
        msg_schedule_text_fmt("No schedule for this station");
        return;
    }
    freq_sched_row = row;
    ui_list_cfg_t cfg = {
        .width = FREQ_W - 16, .height = FREQ_H - FREQ_TITLE_H - FREQ_FOOTER_H,
        .font = &sony_24,
        .col_count = 3,
        .cols = {
            { .x = 14,   .w = 74,  .align = UI_LIST_ALIGN_LEFT },
            { .x = 100,  .w = 520, .align = UI_LIST_ALIGN_LEFT },
            { .x = -140, .w = 124, .align = UI_LIST_ALIGN_RIGHT },
        },
        .row_cb = freq_sched_row_cb,
        .activate_cb = freq_sched_activate_cb,
        .empty_text = "No schedule",
        .vol_coarse = true,
    };
    lv_group_remove_obj(freq_list);
    lv_obj_add_flag(freq_list, LV_OBJ_FLAG_HIDDEN);
    freq_sched = freq_make_list(&cfg);
    ui_list_set_count(freq_sched, n);
    int nx = wefax_db_next(e, n, freq_now_minute());
    ui_list_select(freq_sched, nx >= 0 ? nx : 0);
    freq_focus(freq_sched);
    buttons_load_page(&btn_page_freq_sched);
    freq_update_title();
    freq_update_footer();
}

static void freq_close_schedule(void)
{
    if (!freq_sched) return;
    lv_group_remove_obj(freq_sched);
    lv_obj_del(freq_sched);
    freq_sched = NULL;
    freq_sched_row = -1;
    lv_obj_clear_flag(freq_list, LV_OBJ_FLAG_HIDDEN);
    freq_focus(freq_list);
    buttons_load_page(&btn_page_freq_list);
    freq_update_title();
    freq_update_footer();
}

/* once a second: clock in the title, "next" markers every minute */
static void freq_timer_cb(lv_timer_t *t)
{
    (void)t;
    int now = freq_now_minute();
    if (now == freq_last_minute) return;
    freq_last_minute = now;
    freq_update_title();
    freq_update_footer();
    if (freq_list) lv_obj_invalidate(freq_list);
    if (freq_sched) lv_obj_invalidate(freq_sched);
}

/* ---- open / close -------------------------------------------------------------- */

static void freq_open(void)
{
    if (freq_win || !dialog.obj) return;
    if (wefax_db_load() == 0) {
        msg_schedule_text_fmt("No WeFax stations file");
        return;
    }

    freq_prev_focus = lv_group_get_focused(keyboard_group);
    freq_prev_editing = lv_group_get_editing(keyboard_group);
    if (freq_prev_focus) lv_group_remove_obj(freq_prev_focus);

    /* same frame as the BIG dialog, on the main-screen parent */
    freq_win = lv_obj_create(lv_obj_get_parent(dialog.obj));
    lv_obj_remove_style_all(freq_win);
    lv_obj_add_style(freq_win, &style.dialog.base, 0);
    lv_obj_set_size(freq_win, FREQ_W, FREQ_H);
    lv_obj_center(freq_win);
    lv_obj_set_style_translate_y(freq_win, 4, 0);
    lv_obj_clear_flag(freq_win, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    freq_title = lv_label_create(freq_win);
    lv_obj_set_style_text_font(freq_title, &sony_22, 0);
    lv_obj_set_style_text_color(freq_title, lv_color_white(), 0);
    lv_obj_align(freq_title, LV_ALIGN_TOP_LEFT, 18, 8);

    ui_list_cfg_t cfg = {
        .width = FREQ_W - 16, .height = FREQ_H - FREQ_TITLE_H - FREQ_FOOTER_H,
        .font = &sony_24,
        .col_count = 4,
        .cols = {
            { .x = 14,   .w = 140, .align = UI_LIST_ALIGN_LEFT, .muted = true },
            { .x = 165,  .w = 300, .align = UI_LIST_ALIGN_LEFT },
            { .x = 470,  .w = 130, .align = UI_LIST_ALIGN_RIGHT },
            { .x = -120, .w = 104, .align = UI_LIST_ALIGN_RIGHT, .muted = true },
        },
        .row_cb = freq_list_row_cb,
        .activate_cb = freq_list_activate_cb,
        .empty_text = "No stations",
        .vol_coarse = true,         /* VOL turn: fast scroll, VOL press: schedule */
    };
    freq_list = freq_make_list(&cfg);
    ui_list_set_count(freq_list, wefax_db_rows());
    int cur = wefax_db_find(freq_current_hz());
    ui_list_select(freq_list, cur >= 0 ? cur : 0);

    freq_footer = lv_label_create(freq_win);
    lv_obj_set_width(freq_footer, FREQ_W - 36);
    lv_label_set_long_mode(freq_footer, LV_LABEL_LONG_DOT);
    lv_obj_set_height(freq_footer, FREQ_FOOTER_H - 8);
    lv_obj_set_style_text_font(freq_footer, &sony_24, 0);
    lv_obj_set_style_text_color(freq_footer, lv_color_hex(FREQ_FOOTER_COLOR), 0);
    lv_obj_align(freq_footer, LV_ALIGN_BOTTOM_LEFT, 18, -6);

    freq_focus(freq_list);
    buttons_load_page(&btn_page_freq_list);
    freq_last_minute = -1;
    freq_timer = lv_timer_create(freq_timer_cb, 1000, NULL);
    freq_timer_cb(NULL);
}

static void freq_close(void)
{
    if (!freq_win) return;
    if (freq_timer) { lv_timer_del(freq_timer); freq_timer = NULL; }
    if (freq_sched) { lv_group_remove_obj(freq_sched); }
    if (freq_list) { lv_group_remove_obj(freq_list); }
    lv_obj_del(freq_win);
    freq_win = freq_title = freq_footer = freq_list = freq_sched = NULL;
    freq_sched_row = -1;

    if (freq_prev_focus) {
        lv_group_add_obj(keyboard_group, freq_prev_focus);
        lv_group_focus_obj(freq_prev_focus);
        lv_group_set_editing(keyboard_group, freq_prev_editing);
        freq_prev_focus = NULL;
    }
    if (dialog.run) {
        buttons_load_page(&btn_page_1);
    }
}

/* ---- microphone UP / DOWN ---------------------------------------------------------- */

/*
 * While WEFAX is open the microphone UP/DOWN keys step through the stations
 * list instead of the CHANNEL LIST: from a station recalled from the list
 * to the previous/next one (wrapping at both ends); on any other frequency
 * (the "Freq." button shows kHz) to the first station of the list.
 */
bool dialog_wefax_mic_step(int dir)
{
    if (!dialog.run || dir == 0) return false;
    int n = wefax_db_load();
    if (n <= 0) return false;

    int dial = cparam_i_get(cfg.cur.fg_freq());
    int row;
    if (freq_recalled_row >= 0 && dial == freq_recalled_dial)
        row = ((freq_recalled_row + (dir > 0 ? 1 : -1)) % n + n) % n;
    else
        row = 0;

    freq_tune_row(row);
    if (btn_freq.disp_btn) buttons_refresh(&btn_freq);
    if (freq_list && !freq_sched) {
        ui_list_select(freq_list, row);
        freq_update_footer();
        lv_obj_invalidate(freq_list);
    }
    return true;
}

/* ---- buttons -------------------------------------------------------------------- */

static void freq_open_cb(button_data_t *data)
{
    (void)data;
    freq_open();
}

/* Microphone V/M while WEFAX is open: open / close the stations list. */
bool dialog_wefax_vm_toggle(void)
{
    if (!dialog.run) return false;
    app_info_close();
    if (freq_win) freq_close();
    else freq_open();
    return true;
}

static void freq_tune_cb(button_data_t *data)
{
    (void)data;
    int row = freq_sched ? freq_sched_row : (freq_list ? ui_list_get_selected(freq_list) : -1);
    if (row < 0) return;
    freq_tune_row(row);
    freq_close();
}

static void freq_sched_btn_cb(button_data_t *data)
{
    (void)data;
    freq_open_schedule();
}

static void freq_back_cb(button_data_t *data)
{
    (void)data;
    freq_close_schedule();
}

static void freq_close_cb(button_data_t *data)
{
    (void)data;
    freq_close();
}
