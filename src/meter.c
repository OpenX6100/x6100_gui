/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "meter.h"
#include "styles.h"
#include "events.h"
#include "spectrum.h"
#include "util.h"
#include "scheduler.h"
#include "audio.h"
#include "widgets/lv_bar_indicator.h"
#include "cfg/cfg_api.h"

#define METER_PEAK_HOLD 1500
#define METER_PEAK_SPEED 20

#define LEVEL_MIN_DB    (-50.0f)
#define LEVEL_MAX_DB    (0.5f)

static float            meter_db = S1;
static float            meter_db_raw = S1;
static float            noise_level = S_MIN;

static float            meter_peak = S1;
static int64_t          meter_peak_time;
static int64_t          now;

static lv_obj_t         *obj;
static lv_obj_t         *s_bar;
static lv_obj_t         *level_bar;
static lv_obj_t         *db_val_label;

static lv_timer_t       *level_timer;
static float            level_peak = LEVEL_MIN_DB;
static int64_t          level_peak_time;

static meter_mode_t     meter_mode = METER_MODE_S;

static bar_tick_t s_items[] = {
    { .label = "S1",    .val = S1 },
    { .label = "3",     .val = S3 },
    { .label = "5",     .val = S5 },
    { .label = "7",     .val = S7 },
    { .label = "9",     .val = S9 },
    { .label = "+20",   .val = S9_20 },
    { .label = "+40",   .val = S9_40 }
};

static bar_tick_t level_items[] = {
    { .label = "0",     .val = 0 },
    { .label = "-12",   .val = -12 },
    { .label = "-24",   .val = -24 },
    { .label = "-36",   .val = -36 },
    { .label = "-48",   .val = -48 }
};

static void on_show_meter_value_change(Subject *, void *) {
    if (param_i_get(cfg.ui.show_meter_value())) {
        lv_obj_clear_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void meter_scheduled_refresh(void *unused) {
    (void)unused;
    lv_bar_indicator_set_value(s_bar, meter_db);
    lv_bar_indicator_set_peak_value(s_bar, meter_peak);
}

static void update_db_label_cb(lv_timer_t *t) {
    // TODO: add check for visibility
    lv_label_set_text_fmt(db_val_label, "%.1f", meter_db_raw);
}

static void tx_cb(void * s, lv_msg_t * msg) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void rx_cb(void * s, lv_msg_t * msg) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_color_t meter_color_cb(float val) {
    if (val <= noise_level) {
        return style.colors.s_meter.noise;
    } else if (val <= S9) {
        return style.colors.s_meter.low;
    } else if (val <= S9_20) {
        return style.colors.s_meter.mid;
    }
    return style.colors.s_meter.high;
}

static lv_color_t level_color_cb(float val) {
    if (val <= -12.0f) {
        return style.colors.s_meter.low;
    } else if (val <= -6.0f) {
        return style.colors.s_meter.mid;
    }
    return style.colors.s_meter.high;
}

static void level_refresh_cb(lv_timer_t *t) {
    float db = audio_get_peak_db();

    int64_t t_now = get_time();

    if (db > level_peak) {
        level_peak = db;
        level_peak_time = t_now;
    } else if (t_now - level_peak_time > METER_PEAK_HOLD) {
        level_peak -= (t_now - level_peak_time - METER_PEAK_HOLD) * METER_PEAK_SPEED / 1000;
    }

    lv_bar_indicator_set_value(level_bar, db);
    lv_bar_indicator_set_peak_value(level_bar, level_peak);
}


lv_obj_t * meter_init(lv_obj_t * parent) {
    obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &style.s_meter, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);

    // Use pad to align
    lv_coord_t pad = lv_obj_get_style_pad_top(obj, 0);
    lv_obj_update_layout(obj);
    lv_coord_t w = lv_obj_get_content_width(obj);
    lv_coord_t h = lv_obj_get_content_height(obj);

    lv_msg_subscribe(MSG_TX_INFO_SHOW, tx_cb, NULL);
    lv_msg_subscribe(MSG_TX_INFO_HIDE, rx_cb, NULL);

    s_bar = lv_bar_indicator_create(obj);
    lv_obj_set_size(s_bar, w, h);
    lv_obj_center(s_bar);

    lv_bar_indicator_set_range(s_bar, S1, S9_40 + 5, 3.0f);

    lv_bar_indicator_set_ticks(s_bar, s_items, ARRAY_SIZE(s_items));
    lv_bar_indicator_set_font(s_bar, &sony_22);
    lv_bar_indicator_set_default_color(s_bar, style.colors.s_meter.low);
    lv_bar_indicator_set_color_cb(s_bar, meter_color_cb);

    lv_bar_indicator_set_peak_enable(s_bar, true);
    lv_bar_indicator_set_peak_color(s_bar, style.colors.s_meter.peak);

    level_bar = lv_bar_indicator_create(obj);
    lv_obj_set_size(level_bar, w, h);
    lv_obj_center(level_bar);
    lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);

    lv_bar_indicator_set_range(level_bar, LEVEL_MIN_DB, LEVEL_MAX_DB, 1.0f);

    lv_bar_indicator_set_ticks(level_bar, level_items, ARRAY_SIZE(level_items));
    lv_bar_indicator_set_font(level_bar, &sony_22);
    lv_bar_indicator_set_default_color(level_bar, style.colors.s_meter.low);
    lv_bar_indicator_set_color_cb(level_bar, level_color_cb);

    lv_bar_indicator_set_peak_enable(level_bar, true);
    lv_bar_indicator_set_peak_color(level_bar, style.colors.s_meter.peak);

    db_val_label = lv_label_create(obj);
    lv_obj_add_style(db_val_label, &style.text_base_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(db_val_label, &sony_20, 0);
    lv_obj_align(db_val_label, LV_ALIGN_BOTTOM_RIGHT, pad - 3, pad - 2);
    lv_label_set_text(db_val_label, "");

    lv_timer_create(update_db_label_cb, LV_DISP_DEF_REFR_PERIOD * 3, NULL);

    subject_subscribe_delayed_and_notify((Subject *)cfg.ui.show_meter_value(), on_show_meter_value_change, NULL);

    return obj;
}

void meter_set_noise(float val) {
    noise_level = val;
}

void meter_set_mode(meter_mode_t mode) {
    if (mode == meter_mode) {
        return;
    }

    meter_mode = mode;

    if (meter_mode == METER_MODE_LEVEL) {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);

        level_peak = LEVEL_MIN_DB;
        level_peak_time = get_time();

        if (level_timer == NULL) {
            level_timer = lv_timer_create(level_refresh_cb, LV_DISP_DEF_REFR_PERIOD * 2, NULL);
        }
    } else {
        if (level_timer) {
            lv_timer_del(level_timer);
            level_timer = NULL;
        }

        lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void meter_update(float db, float beta) {
    meter_db = meter_db * beta + db * (1.0f - beta);

    meter_db_raw = db;
    now = get_time();
    if (meter_db > meter_peak) {
        meter_peak = meter_db;
        meter_peak_time = now;
    } else if (now - meter_peak_time > METER_PEAK_HOLD) {
        meter_peak -= (now - meter_peak_time - METER_PEAK_HOLD) * METER_PEAK_SPEED / 1000;
    }

    if (meter_mode == METER_MODE_S) {
        scheduler_put_noargs(meter_scheduled_refresh);
    }
}
