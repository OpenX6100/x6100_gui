/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "band_info.h"

#include "display.h"

extern "C" {
    #include "events.h"
    #include "pubsub_ids.h"
    #include "styles.h"
    #include "lvgl/lvgl.h"
}

// C++ headers
#include "cfg/cfg_api.h"
#include "cfg/db.h"
#include "cfg/subject.h"


#include <memory>
#include <vector>

#define BANDS_REFRESH_PERIOD_MS 3 * 1000

static lv_obj_t    *obj;

static int32_t      width_hz         = 100000;
static uint64_t     freq;
static lv_anim_t    fade;
static bool         fade_run = false;
static uint8_t      zoom     = 1;
static int32_t      if_shift = 0;

static lv_timer_t *timer = NULL;

static std::vector<BandInfo> bands;
static Subscription  zoom_sub;
static Subscription  freq_sub;
static Subscription  if_shift_sub;
static Subscription  view_3d_sub;

static void on_zoom_changed(Subject *subj, void *user_data);
static void on_freq_changed(Subject *subj, void *user_data);
static void on_if_shift_changed(Subject *subj, void *user_data);
static void on_view_3d_changed(Subject *subj, void *user_data);

static void band_info_timer(lv_timer_t *t) {
    lv_anim_set_values(&fade, lv_obj_get_style_opa(obj, 0), LV_OPA_TRANSP);
    lv_anim_start(&fade);
    timer = NULL;
}

static void band_info_draw_cb(lv_event_t *e) {
    lv_event_code_t code     = lv_event_get_code(e);
    lv_obj_t       *obj      = lv_event_get_target(e);
    lv_draw_ctx_t  *draw_ctx = lv_event_get_draw_ctx(e);

    if (bands.empty()) {
        return;
    }

    uint8_t current_zoom = zoom;

    lv_coord_t x1 = obj->coords.x1;
    lv_coord_t y1 = obj->coords.y1;

    lv_coord_t w = lv_obj_get_width(obj);
    lv_coord_t h = lv_obj_get_height(obj) - 1;

    for (const BandInfo &band : bands) {
        /* Rect */

        lv_border_side_t border_side = LV_BORDER_SIDE_NONE;

        int32_t start = (int64_t)(band.start_freq - freq + if_shift) * w / width_hz * current_zoom;
        int32_t stop  = (int64_t)(band.stop_freq - freq + if_shift) * w / width_hz * current_zoom;

        start += w / 2;
        stop += w / 2;

        if (start < 0) {
            start = 0;
        } else if (start > w) {
            continue;
        } else {
            border_side |= LV_BORDER_SIDE_LEFT;
        }

        if (stop < 0) {
            continue;
        } else if (stop > w) {
            stop = w;
        } else {
            border_side |= LV_BORDER_SIDE_RIGHT;
        }

        lv_draw_rect_dsc_t rect_dsc;
        lv_area_t          area;

        lv_draw_rect_dsc_init(&rect_dsc);

        rect_dsc.bg_color     = style.colors.mark;
        rect_dsc.bg_opa       = LV_OPA_50;
        rect_dsc.border_width = 2;
        rect_dsc.border_color = lv_color_white();
        rect_dsc.border_opa   = LV_OPA_50;
        rect_dsc.border_side  = border_side;

        area.x1 = x1 + start + 2;
        area.y1 = y1;
        area.x2 = x1 + stop - 2;
        area.y2 = y1 + h;

        lv_draw_rect(draw_ctx, &rect_dsc, &area);

        /* Label */

        lv_draw_label_dsc_t dsc_label;
        lv_draw_label_dsc_init(&dsc_label);

        dsc_label.color = colors.base_text_color;
        dsc_label.font  = &sony_22;

        lv_point_t label_size;
        lv_txt_get_size(&label_size, band.name.c_str(), dsc_label.font, 0, 0, LV_COORD_MAX, 0);

        if (stop - start > label_size.x) {
            area.x1 = x1 + (start + stop) / 2 - label_size.x / 2;
            area.y1 = y1 + (h - label_size.y) / 2;
            area.x2 = x1 + (start + stop / 2 + label_size.x / 2);
            area.y2 = y1 + h;

            lv_draw_label(draw_ctx, &dsc_label, &area, band.name.c_str(), NULL);
        }
    }
}

static void fade_anim(void *obj, int32_t v) {
    lv_obj_set_style_opa_layered((_lv_obj_t*)obj, v, 0);
}

static void fade_ready(lv_anim_t *a) {
    fade_run = false;
}

extern "C" lv_obj_t *band_info_init(lv_obj_t *parent) {
    bands = BandsTable::all_bands();
    obj   = lv_obj_create(parent);

    lv_obj_set_size(obj, LV_PCT(100), BAND_INFO_HEIGHT);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, BAND_INFO_OFFSET_Y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_0, 0);

    lv_obj_add_event_cb(obj, band_info_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, obj);
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, fade_anim);
    lv_anim_set_ready_cb(&fade, fade_ready);

    zoom_sub = Subscription(cfg.mode.zoom()->subscribe_and_notify(on_zoom_changed));
    freq_sub = Subscription(cfg.cur.fg_freq()->subscribe_delayed_and_notify(on_freq_changed));
    if_shift_sub = Subscription(cfg.band.if_shift()->subscribe_delayed_and_notify(on_if_shift_changed));
    view_3d_sub = Subscription(cfg.ui.spectrum_3d()->subscribe_delayed(on_view_3d_changed));

    return obj;
}

static void band_info_update(int32_t f) {
    freq = f;

    /* 3D: the band bar under it shows the bands, and its tag the segment,
     * so the banner goes at once (it starts shown) */
    if (cfg.ui.spectrum_3d()->get()) {
        if (timer) {
            lv_timer_del(timer);
            timer = NULL;
        }
        lv_anim_del(obj, fade_anim);
        fade_run = false;
        lv_obj_set_style_opa_layered(obj, LV_OPA_TRANSP, 0);
        return;
    }

    lv_obj_invalidate(obj);

    if (!fade_run) {
        fade_run = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa(obj, 0), LV_OPA_COVER);
        lv_anim_start(&fade);
    }

    if (timer) {
        lv_timer_reset(timer);
    } else {
        timer = lv_timer_create(band_info_timer, 2000, NULL);
        lv_timer_set_repeat_count(timer, 1);
    }
}

static void on_zoom_changed(Subject *subj, void *user_data) {
    zoom = static_cast<ParamInt *>(subj)->get();
    lv_obj_invalidate(obj);
}

static void on_freq_changed(Subject *subj, void *user_data) {
    band_info_update(static_cast<ComputedParamInt *>(subj)->get());
}

static void on_if_shift_changed(Subject *subj, void *user_data) {
    if_shift = static_cast<ParamInt *>(subj)->get();
    lv_obj_invalidate(obj);
}

static void on_view_3d_changed(Subject *subj, void *user_data) {
    band_info_update(freq);
}
