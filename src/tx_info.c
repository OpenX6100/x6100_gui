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

/* One thick PWR bar with its 0-10 W scale and a peak tick, SWR and ALC as
 * thin bars below, green / amber / red by where each segment sits. The box
 * takes the meter and frequency boxes' width (the dial hides during TX),
 * in the small top bar's height, so it never hangs over the spectrum or an
 * app's waterfall. Bar positions are in the box's content area (620 x 58). */
#define ROWS        3       /* PWR, SWR, ALC */
#define PEAK_HOLD_MS 1000

static const struct {
    lv_coord_t x, y, w, h;  /* the bar */
    lv_coord_t seg, gap;    /* one LED segment, the gap after it */
} bar_geo[ROWS] = {
    { 50, 14, 480, 22, 6, 2 },
    { 50, 43, 168, 12, 4, 2 },
    {376, 43, 156, 12, 4, 2 },
};

static const float max_alc = 10.0f;

static lv_obj_t *bars[ROWS];
static int16_t   lit[ROWS];     /* segments lit in each bar, as last drawn */
static int16_t   peak;          /* PWR's peak segment, held PEAK_HOLD_MS */
static uint32_t  peak_time;

static void on_cur_mode_change(Subject *subj, void *user_data);
static void update_labels_visibility_cb(Subject *, void *);

static void tx_cb(void * s, lv_msg_t * msg) {
    pwr  = 0.0f;
    vswr = 0.0f;
    alc  = 0.0f;
    peak = 0;

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void rx_cb(void * s, lv_msg_t * msg) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void update_tx_info(void *arg) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    float frac[ROWS] = {
        (pwr - min_pwr) / (max_pwr - min_pwr),
        (vswr - min_swr) / (max_swr - min_swr),
        alc / max_alc,
    };
    bool redraw = false;

    for (int row = 0; row < ROWS; row++) {
        int16_t n   = bar_geo[row].w / (bar_geo[row].seg + bar_geo[row].gap);
        int16_t now = (int16_t)(n * LV_CLAMP(0.0f, frac[row], 1.0f) + 0.5f);

        if (now != lit[row]) {
            lit[row] = now;
            lv_obj_invalidate(bars[row]);
        }
    }
    if (lit[0] >= peak) {
        redraw = lit[0] != peak;
        peak = lit[0];
        peak_time = lv_tick_get();
    } else if (lv_tick_elaps(peak_time) > PEAK_HOLD_MS) {
        peak--;                 /* falls one segment an update after the hold */
        redraw = true;
    }
    if (redraw) {
        lv_obj_invalidate(bars[0]);
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

static void bar_draw_cb(lv_event_t *e) {
    int                row      = (int)(intptr_t)lv_event_get_user_data(e);
    lv_draw_ctx_t     *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t          coords;
    lv_draw_rect_dsc_t dsc;
    lv_coord_t         step = bar_geo[row].seg + bar_geo[row].gap;

    lv_obj_get_coords(bars[row], &coords);
    int16_t n = (int16_t)(lv_area_get_width(&coords) / step);

    lv_draw_rect_dsc_init(&dsc);
    for (int16_t i = 0; i < n; i++) {
        lv_color_t c = seg_color(row, (float)i / n);
        lv_area_t  a;

        a.x1 = coords.x1 + i * step;
        a.x2 = a.x1 + bar_geo[row].seg - 1;
        a.y1 = coords.y1;
        a.y2 = coords.y2;
        if (row == 0 && peak > 0 && i == peak - 1 && peak > lit[0]) {
            c = lv_color_white();
        } else if (i >= lit[row]) {
            c = lv_color_darken(c, 212);
        }
        dsc.bg_color = c;
        lv_draw_rect(draw_ctx, &dsc, &a);
    }
}

static lv_obj_t *add_label(lv_coord_t x, lv_coord_t y, lv_coord_t w, const lv_font_t *font,
                           lv_text_align_t align, const char *text) {
    lv_obj_t *l = lv_label_create(obj);

    lv_obj_add_style(l, &style.text_base_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *tx_info_init(lv_obj_t *parent) {
    obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &style.tx_info, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);

    for (int row = 0; row < ROWS; row++) {
        bars[row] = lv_obj_create(obj);
        lv_obj_remove_style_all(bars[row]);
        lv_obj_set_pos(bars[row], bar_geo[row].x, bar_geo[row].y);
        lv_obj_set_size(bars[row], bar_geo[row].w, bar_geo[row].h);
        lv_obj_clear_flag(bars[row], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(bars[row], bar_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)row);
    }

    /* PWR's scale, 0-10 W over the thick bar */
    for (int w = 0; w <= 10; w += 2) {
        lv_coord_t x = bar_geo[0].x + bar_geo[0].w * w / 10;
        lv_obj_t  *l = add_label(x - 15, -4, 30, &sony_14, LV_TEXT_ALIGN_CENTER, "");

        lv_label_set_text_fmt(l, "%d", w);
        lv_obj_add_style(l, &style.text_muted_color, LV_PART_MAIN);
    }

    /* SWR and ALC numbers as big as the power (sony_16 was too small to
     * read on the radio), the names sony_22, each on its number's base
     * line (sony_22 and sony_26 lines put it 15 and 16 px down) */
    add_label(0, 10, 48, &sony_22, LV_TEXT_ALIGN_LEFT, "PWR");
    add_label(0, 40, 48, &sony_22, LV_TEXT_ALIGN_LEFT, "SWR");
    add_label(326, 40, 48, &sony_22, LV_TEXT_ALIGN_LEFT, "ALC");
    pwr_label  = add_label(535, 9, 85, &sony_26, LV_TEXT_ALIGN_RIGHT, "");
    vswr_label = add_label(222, 39, 78, &sony_26, LV_TEXT_ALIGN_RIGHT, "");
    alc_label  = add_label(536, 39, 84, &sony_26, LV_TEXT_ALIGN_RIGHT, "");

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
