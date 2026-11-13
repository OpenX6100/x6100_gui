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
 * thin bars below, in the stock meter's colours (seg_color). The box
 * takes the meter and frequency boxes' width (the dial hides during TX),
 * in the small top bar's height, so it never hangs over the spectrum or an
 * app's waterfall. Bar positions are in the box's content area (620 x 58). */
#define ROWS        3       /* PWR, SWR, ALC */
#define PEAK_HOLD_MS 1000
#define PEAK_FALL    20      /* segments a second the peak falls after the hold */
#define CW_HOLD_MS   1000    /* CW: TX and RX at every pause in the keying, so the
                              * meter stays up this long after the last element */

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
static int16_t   peak_held;     /* where it was when the hold started */
static uint32_t  peak_time;
static bool      swr_gray;      /* the SWR colour setting, read once a redraw */
static lv_timer_t *cw_hold;

static int16_t segments(int row) {
    return bar_geo[row].w / (bar_geo[row].seg + bar_geo[row].gap);
}

static void on_cur_mode_change(Subject *subj, void *user_data);
static void update_labels_visibility_cb(Subject *, void *);

/* The S-meter and the dial follow these (meter.c, freq_info.cpp) */
static void hide(void) {
    lv_timer_pause(cw_hold);
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_msg_send(MSG_TX_INFO_HIDE, NULL);
    }
}

static void cw_hold_cb(lv_timer_t *t) {
    hide();
}

static void tx_cb(void * s, lv_msg_t * msg) {
    lv_timer_pause(cw_hold);
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;                 /* CW: the next element within the hold */
    }
    pwr  = 0.0f;
    vswr = 0.0f;
    alc  = 0.0f;
    peak = 0;
    memset(lit, 0, sizeof(lit));  /* not the last transmission's bars */

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_msg_send(MSG_TX_INFO_SHOW, NULL);
}

static void rx_cb(void * s, lv_msg_t * msg) {
    x6100_mode_t mode = cparam_i_get(cfg.cur.mode());   /* cur_mode is set only on a change */

    if (mode == x6100_mode_cw || mode == x6100_mode_cwr) {
        lv_timer_reset(cw_hold);
        lv_timer_resume(cw_hold);
    } else {
        hide();
    }
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
    for (int row = 0; row < ROWS; row++) {
        int16_t now = (int16_t)(segments(row) * LV_CLAMP(0.0f, frac[row], 1.0f) + 0.5f);

        if (now != lit[row]) {
            lit[row] = now;
            lv_obj_invalidate(bars[row]);
        }
    }

    /* The peak holds, then falls by time (not by packet, whose rate varies) */
    int16_t new_peak = peak;
    if (lit[0] >= peak) {
        new_peak  = lit[0];
        peak_held = lit[0];
        peak_time = lv_tick_get();
    } else {
        uint32_t t = lv_tick_elaps(peak_time);
        if (t > PEAK_HOLD_MS) {
            new_peak = LV_MAX(lit[0], peak_held - (int16_t)((t - PEAK_HOLD_MS) * PEAK_FALL / 1000));
        }
    }
    if (new_peak != peak) {
        peak = new_peak;
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
}

static void on_dialog_stop(void *s, lv_msg_t *msg) {
    dialog_run = false;
}

/* The stock meter's colours: PWR (and ALC, which had no bar) grey, SWR
 * green up to 2, yellow up to 3, red above. */
static lv_color_t seg_color(int row, float f) {
    if (row != 1) return lv_color_hex(0xAAAAAA);
    if (f > 0.5f) return lv_color_hex(0xAA0000);                 /* SWR 3 */
    if (f > 0.25f) return lv_color_hex(0xAAAA00);                /* SWR 2 */
    return swr_gray ? lv_color_hex(0xAAAAAA) : lv_color_hex(0x00CC00);
}

static void bar_draw_cb(lv_event_t *e) {
    int                row      = (int)(intptr_t)lv_event_get_user_data(e);
    lv_draw_ctx_t     *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t          coords;
    lv_draw_rect_dsc_t dsc;
    lv_coord_t         step = bar_geo[row].seg + bar_geo[row].gap;

    int16_t            n    = segments(row);

    lv_obj_get_coords(bars[row], &coords);
    swr_gray = param_i_get(cfg.ui.swr_color()) == SWR_GRAY;

    lv_draw_rect_dsc_init(&dsc);
    for (int16_t i = 0; i < n; i++) {
        lv_color_t c = seg_color(row, (float)i / n);
        lv_area_t  a;

        a.x1 = coords.x1 + i * step;
        a.x2 = a.x1 + bar_geo[row].seg - 1;
        a.y1 = coords.y1;
        a.y2 = coords.y2;
        dsc.bg_opa = LV_OPA_80;         /* lit, as the stock bars */
        if (row == 0 && peak > 0 && i == peak - 1 && peak > lit[0]) {
            c = lv_color_white();
        } else if (i >= lit[row]) {
            c = lv_color_darken(c, 212);
            dsc.bg_opa = LV_OPA_COVER;
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

    cw_hold = lv_timer_create(cw_hold_cb, CW_HOLD_MS, NULL);
    lv_timer_pause(cw_hold);

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
