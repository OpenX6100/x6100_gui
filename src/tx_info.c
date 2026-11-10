/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "tx_info.h"

#include <stdio.h>
#include <string.h>

#include "events.h"
#include "msg_tiny.h"
#include "cfg/cfg_api.h"
#include "scheduler.h"
#include "styles.h"
#include "util.h"

#define UPDATE_UI_MS 40

static const float min_pwr = 0.0f;
static const float max_pwr = 10.0f;

static const float min_swr = 1.0f;
static const float max_swr = 5.0f;

static float pwr  = 0.0f;
static float vswr = 0.0f;
static float alc;

static uint8_t msg_id;

static uint64_t prev_ui_update = 0;

static x6100_mode_t cur_mode;

static bool dialog_run = false;

static lv_obj_t     *obj;
static lv_obj_t     *pwr_label;
static lv_obj_t     *vswr_label;
static lv_obj_t     *alc_label;
static lv_obj_t     *leds;

/* Three LED rows, PWR / SWR / ALC, in the small top bar's height, so the
 * box never hangs over the spectrum or an app's waterfall. */
#define ROWS        3
#define ROW_H       14
#define ROW_STEP    22
#define NAME_W      44
#define VALUE_W     70
#define SEG_W       4
#define SEG_GAP     2

static const float max_alc = 10.0f;

static int16_t lit[ROWS];       /* segments lit in each row, as last drawn */

static void on_cur_mode_change(Subject *subj, void *user_data);
static void update_labels_visibility_cb(Subject *, void *);

static void tx_cb(void * s, lv_msg_t * msg) {
    pwr  = 0.0f;
    vswr = 0.0f;
    alc  = 0.0f;

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void rx_cb(void * s, lv_msg_t * msg) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void update_tx_info(void *arg) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    int16_t n   = (int16_t)(lv_obj_get_width(leds) / (SEG_W + SEG_GAP));
    int16_t now[ROWS] = {
        (int16_t)(n * LV_CLAMP(0.0f, (pwr - min_pwr) / (max_pwr - min_pwr), 1.0f) + 0.5f),
        (int16_t)(n * LV_CLAMP(0.0f, (vswr - min_swr) / (max_swr - min_swr), 1.0f) + 0.5f),
        (int16_t)(n * LV_CLAMP(0.0f, alc / max_alc, 1.0f) + 0.5f),
    };
    if (memcmp(now, lit, sizeof(lit)) != 0) {
        memcpy(lit, now, sizeof(lit));
        lv_obj_invalidate(leds);
    }

    if (param_i_get(cfg.ui.mag_alc()) && !dialog_run) {
        msg_tiny_set_text_fmt("ALC: %.1f", alc);
    }
}

static void update_labels_cb(lv_timer_t *t) {
    lv_label_set_text_fmt(alc_label, "%.1f", alc);
    lv_label_set_text_fmt(vswr_label, "%.2f", vswr);
    lv_label_set_text_fmt(pwr_label, "%.1f W", pwr);
}

static void on_dialog_start(void *s, lv_msg_t *msg) {
    dialog_run = true;
    update_labels_visibility_cb(NULL, NULL);
}

static void on_dialog_stop(void *s, lv_msg_t *msg) {
    dialog_run = false;
    update_labels_visibility_cb(NULL, NULL);
}

/* Green, amber, red by where the segment sits on its scale. */
static lv_color_t seg_color(int row, float f) {
    static const float amber_from[ROWS] = {0.6f, 0.25f, 0.3f};   /* 6 W, SWR 2, ALC 3 */
    static const float red_from[ROWS]   = {0.85f, 0.5f, 0.6f};   /* 8.5 W, SWR 3, ALC 6 */

    if (f >= red_from[row]) return lv_color_hex(0xE62820);
    if (f >= amber_from[row]) return lv_color_hex(0xFFB400);
    if (row == 1 && param_i_get(cfg.ui.swr_color()) == SWR_GRAY) return lv_color_hex(0xAAAAAA);
    return lv_color_hex(0x00C83C);
}

static void leds_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t      coords;
    lv_draw_rect_dsc_t dsc;

    lv_obj_get_coords(leds, &coords);
    int16_t n = (int16_t)(lv_area_get_width(&coords) / (SEG_W + SEG_GAP));

    lv_draw_rect_dsc_init(&dsc);
    for (int row = 0; row < ROWS; row++) {
        for (int16_t i = 0; i < n; i++) {
            lv_color_t c = seg_color(row, (float)i / n);
            lv_area_t  a;

            a.x1 = coords.x1 + i * (SEG_W + SEG_GAP);
            a.x2 = a.x1 + SEG_W - 1;
            a.y1 = coords.y1 + row * ROW_STEP;
            a.y2 = a.y1 + ROW_H - 1;
            dsc.bg_color = i < lit[row] ? c : lv_color_darken(c, 212);
            lv_draw_rect(draw_ctx, &dsc, &a);
        }
    }
}

static lv_obj_t *row_label(lv_coord_t x, lv_coord_t w, int row, lv_text_align_t align, const char *text) {
    lv_obj_t *l = lv_label_create(obj);

    lv_obj_add_style(l, &style.text_base_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &sony_16, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, row * ROW_STEP + ROW_H / 2 - 9);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *tx_info_init(lv_obj_t *parent) {
    obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &style.tx_info, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(obj);
    lv_coord_t w = lv_obj_get_content_width(obj);

    leds = lv_obj_create(obj);
    lv_obj_remove_style_all(leds);
    lv_obj_set_pos(leds, NAME_W, 0);
    lv_obj_set_size(leds, w - NAME_W - VALUE_W, ROW_STEP * (ROWS - 1) + ROW_H);
    lv_obj_clear_flag(leds, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(leds, leds_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    row_label(0, NAME_W, 0, LV_TEXT_ALIGN_LEFT, "PWR");
    row_label(0, NAME_W, 1, LV_TEXT_ALIGN_LEFT, "SWR");
    row_label(0, NAME_W, 2, LV_TEXT_ALIGN_LEFT, "ALC");
    pwr_label  = row_label(w - VALUE_W, VALUE_W, 0, LV_TEXT_ALIGN_RIGHT, "");
    vswr_label = row_label(w - VALUE_W, VALUE_W, 1, LV_TEXT_ALIGN_RIGHT, "");
    alc_label  = row_label(w - VALUE_W, VALUE_W, 2, LV_TEXT_ALIGN_RIGHT, "");

    lv_msg_subscribe(MSG_RADIO_TX, tx_cb, NULL);
    lv_msg_subscribe(MSG_RADIO_RX, rx_cb, NULL);
    lv_msg_subscribe(MSG_DIALOG_START, on_dialog_start, NULL);
    lv_msg_subscribe(MSG_DIALOG_STOP, on_dialog_stop, NULL);

    subject_subscribe((Subject*)cfg.cur.mode(), on_cur_mode_change, NULL);

    subject_subscribe_delayed((Subject*)cfg.ui.mag_alc(), update_labels_visibility_cb, NULL);
    subject_subscribe_delayed_and_notify((Subject*)cfg.ui.show_meter_value(), update_labels_visibility_cb, NULL);

    lv_timer_create(update_labels_cb, LV_DISP_DEF_REFR_PERIOD * 3, NULL);

    return obj;
}

void tx_info_update(float p, float s, float a) {
    const float beta = 0.9f;

    a = 10.f - a;
    s = LV_MIN(max_swr, s);

    switch (cur_mode) {
        case x6100_mode_lsb_dig:
        case x6100_mode_usb_dig:
            pwr  = p;
            alc  = a;
            vswr = s;
            break;
        default:
            lpf(&pwr, p, beta, 0.0f);
            lpf(&alc, a, beta, 0.0f);
            lpf(&vswr, s, beta, 0.0f);
    }
    msg_id++;
    scheduler_put_noargs(update_tx_info);
}

bool tx_info_refresh(uint8_t *prev_msg_id, float *alc_p, float *pwr_p, float *vswr_p) {
    if (*prev_msg_id == msg_id) {
        return false;
    }
    if (alc_p)
        *alc_p = alc;
    if (pwr_p)
        *pwr_p = pwr;
    if (vswr_p)
        *vswr_p = vswr;
    *prev_msg_id = msg_id;
    return true;
}


static void on_cur_mode_change(Subject *subj, void *user_data) {
    cur_mode = cparam_i_get(cfg.cur.mode());
}

static void update_labels_visibility_cb(Subject *, void *) {
    /* Each row has its value on the right, ALC included */
    bool show = param_i_get(cfg.ui.show_meter_value());

    lv_obj_t *labels[] = {pwr_label, vswr_label, alc_label};
    for (int i = 0; i < ROWS; i++) {
        if (show) {
            lv_obj_clear_flag(labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
