/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the band bar between the 3D spectrum and the
 *  waterfall
 *
 *  A thin coloured stripe along its top is the band plan of the station's
 *  region (CW blue, digital green, phone / all modes amber); outside the
 *  bands it is shaded with a red line at the band edge. Below it, the
 *  frequency scale: ticks and labels at round frequencies, four or five
 *  across. It is drawn by LVGL only when the frequency, the zoom or the
 *  band plan change, not with the spectrum's frames.
 *
 *  Tuning into another segment shows its band and kind ("40m  CW") in its
 *  colour at the top left of the 3D view, fading after TAG_MS: what the
 *  colours mean, without a legend.
 */

#include "band_bar.h"

#include "band_plan.h"
#include "styles.h"

#include <stdio.h>

#define STRIPE_H  5
#define MAX_SEGS  24
#define TAG_MS    2000  /* the segment tag shows this long, then fades */
#define TAG_FADE  500

static lv_obj_t *bar;
static int32_t   lo_hz, hi_hz;

static lv_obj_t   *tag;
static lv_timer_t *tag_timer;
static int32_t     dial_hz;
static const char *tag_band; /* the segment last shown: its band, and its kind (-1 out of band) */
static int         tag_kind = -2;

static const lv_color32_t kind_color[] = {
    [BAND_PLAN_CW]      = {.ch = {.red = 40, .green = 110, .blue = 200, .alpha = 255}},
    [BAND_PLAN_DIGITAL] = {.ch = {.red = 40, .green = 170, .blue = 90, .alpha = 255}},
    [BAND_PLAN_PHONE]   = {.ch = {.red = 210, .green = 150, .blue = 40, .alpha = 255}},
};

static lv_color_t col(int r, int g, int b) {
    return lv_color_make(r, g, b);
}

static void rect(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_color_t c) {
    lv_draw_rect_dsc_t d;
    lv_area_t          a = {x1, y1, x2, y2};

    if (x2 < x1 || y2 < y1) {
        return;
    }
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa   = LV_OPA_COVER;
    lv_draw_rect(ctx, &d, &a);
}

static void line(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_color_t c, int w) {
    lv_draw_line_dsc_t d;
    lv_point_t         p1 = {x1, y1}, p2 = {x2, y2};

    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    lv_draw_line(ctx, &d, &p1, &p2);
}

static void text(lv_draw_ctx_t *ctx, lv_coord_t cx, lv_coord_t cy, const char *s, lv_color_t c, bool left) {
    lv_draw_label_dsc_t d;
    lv_point_t          sz;
    lv_area_t           a;

    lv_draw_label_dsc_init(&d);
    d.font  = &sony_18;
    d.color = c;
    lv_txt_get_size(&sz, s, d.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    a.x1 = left ? cx : cx - sz.x / 2;
    a.y1 = cy - sz.y / 2;
    a.x2 = a.x1 + sz.x;
    a.y2 = a.y1 + sz.y;
    lv_draw_label(ctx, &d, &a, s, NULL);
}

/* A round label step: 1, 2, 2.5 or 5 x 10^n Hz, the smallest with at most
 * five across (the two at the very edges are left out). */
static int32_t label_step(int32_t span) {
    static const int32_t m[] = {1, 2, 25, 5};
    static const int32_t div[] = {1, 1, 10, 1};
    for (int32_t p = 1000; p <= 10000000; p *= 10) {
        for (int i = 0; i < 4; i++) {
            int32_t s = m[i] * p / div[i];
            if (span / s <= 5) {
                return s;
            }
        }
    }
    return 10000000;
}

/* hatched shading, only between x1 and x2 (out of band) */
static void hatch(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t x2, lv_coord_t y0, lv_coord_t y1) {
    lv_area_t clip = {x1, y0, x2, y1}, *old = (lv_area_t *)ctx->clip_area, cut;

    if (x2 <= x1 || !_lv_area_intersect(&cut, &clip, old)) {
        return;
    }
    ctx->clip_area = &cut;
    for (lv_coord_t x = x1 - (y1 - y0) - ((x1 - (y1 - y0)) % 7 + 7) % 7; x < x2; x += 7) {
        line(ctx, x, y1, x + (y1 - y0), y0, col(62, 30, 30), 1);
    }
    ctx->clip_area = old;
}

static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t  *ctx = lv_event_get_draw_ctx(e);
    lv_area_t       c;
    band_plan_seg_t seg[MAX_SEGS];
    int             region, n;
    lv_coord_t      w, y0, y1;

    lv_obj_get_coords(bar, &c);
    w  = lv_area_get_width(&c);
    y0 = c.y1;
    y1 = c.y2;
    if (hi_hz <= lo_hz || w <= 0) {
        return;
    }
#define X(f) ((lv_coord_t)(c.x1 + ((int64_t)(f) - lo_hz) * w / (hi_hz - lo_hz)))

    /* out of band underneath: dark, a red-brown hatch where no band is */
    rect(ctx, c.x1, y0, c.x2, y1, col(30, 26, 28));
    region = band_plan_region();
    n      = band_plan_segments(region, lo_hz, hi_hz, seg, MAX_SEGS);
    {
        lv_coord_t x = c.x1;
        for (int i = 0; i < n; i++) {
            hatch(ctx, x, LV_MAX(X(seg[i].lo_hz), c.x1), y0, y1);
            x = LV_MAX(x, LV_MIN(X(seg[i].hi_hz), c.x2));
        }
        hatch(ctx, x, c.x2, y0, y1);
    }
    for (int i = 0; i < n; i++) {
        lv_coord_t   a = LV_MAX(X(seg[i].lo_hz), c.x1), b = LV_MIN(X(seg[i].hi_hz), c.x2);
        lv_color32_t k = kind_color[seg[i].kind];
        rect(ctx, a, y0, b, y1, col(18 + k.ch.red / 8, 18 + k.ch.green / 8, 18 + k.ch.blue / 8));
        rect(ctx, a, y0, b, y0 + STRIPE_H - 1, col(k.ch.red, k.ch.green, k.ch.blue));
    }
    /* band edges inside the view, where in-band meets out-of-band */
    for (int i = 0; i < n; i++) {
        bool lo_edge = (i == 0 || seg[i - 1].hi_hz != seg[i].lo_hz) && (int32_t)seg[i].lo_hz > lo_hz;
        bool hi_edge = (i == n - 1 || seg[i + 1].lo_hz != seg[i].hi_hz) && (int32_t)seg[i].hi_hz < hi_hz;
        if (lo_edge) {
            line(ctx, X(seg[i].lo_hz), y0, X(seg[i].lo_hz), y1, col(255, 60, 60), 2);
        }
        if (hi_edge) {
            line(ctx, X(seg[i].hi_hz), y0, X(seg[i].hi_hz), y1, col(255, 60, 60), 2);
        }
    }

    /* the scale */
    int32_t step  = label_step(hi_hz - lo_hz);
    int32_t minor = step / 5;
    for (int32_t f = (lo_hz / minor + 1) * minor; f < hi_hz; f += minor) {
        lv_coord_t x   = X(f);
        bool       big = f % step == 0;
        line(ctx, x, y0 + STRIPE_H, x, y0 + STRIPE_H + (big ? 6 : 3), big ? col(170, 170, 170) : col(105, 105, 105), 1);
        if (big && x > c.x1 + 40 && x < c.x2 - 40) {
            char s[16];
            if (step % 1000 == 0) {
                snprintf(s, sizeof(s), "%d.%03d", (int)(f / 1000000), (int)(f / 1000 % 1000));
            } else {
                snprintf(s, sizeof(s), "%d.%04d", (int)(f / 1000000), (int)(f / 100 % 10000));
            }
            text(ctx, x, (y0 + STRIPE_H + y1) / 2 + 2, s, col(220, 220, 220), false);
        }
    }

    /* the region, small at the left */
    char r[4];
    snprintf(r, sizeof(r), "R%d", region);
    text(ctx, c.x1 + 4, (y0 + STRIPE_H + y1) / 2 + 2, r, col(140, 140, 140), true);
#undef X
}

static const char *const kind_name[] = {
    [BAND_PLAN_CW]      = "CW",
    [BAND_PLAN_DIGITAL] = "Digital",
    [BAND_PLAN_PHONE]   = "SSB",
};

static void tag_opa_cb(void *o, int32_t v) {
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

static void tag_timer_cb(lv_timer_t *t) {
    lv_anim_t a;

    tag_timer = NULL;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tag);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, TAG_FADE);
    lv_anim_set_exec_cb(&a, tag_opa_cb);
    lv_anim_start(&a);
}

/* The dial's segment, at the top left of the 3D view, then the fade. */
static void tag_show(void) {
    if (!dial_hz) {
        return; /* start-up: no frequency yet */
    }
    band_plan_seg_t seg;
    const char     *band = band_plan_band_name((uint32_t)dial_hz);
    lv_color32_t    k    = {.ch = {.red = 255, .green = 60, .blue = 60, .alpha = 255}};

    if (band && band_plan_segments(band_plan_region(), (uint32_t)dial_hz, (uint32_t)dial_hz + 1, &seg, 1) == 1) {
        k = kind_color[seg.kind];
        lv_label_set_text_fmt(tag, "%s  %s", band, kind_name[seg.kind]);
    } else {
        lv_label_set_text(tag, band ? "Out of the band plan" : "Out of band");
    }
    lv_obj_set_style_text_color(tag, lv_color_make(k.ch.red, k.ch.green, k.ch.blue), 0);
    lv_anim_del(tag, tag_opa_cb);
    lv_obj_set_style_opa(tag, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_HIDDEN);
    if (tag_timer) {
        lv_timer_reset(tag_timer);
    } else {
        tag_timer = lv_timer_create(tag_timer_cb, TAG_MS, NULL);
        lv_timer_set_repeat_count(tag_timer, 1);
    }
}

static void tag_hide(void) {
    if (tag_timer) {
        lv_timer_del(tag_timer);
        tag_timer = NULL;
    }
    lv_anim_del(tag, tag_opa_cb);
    lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *band_bar_create(lv_obj_t *parent) {
    bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, LV_HOR_RES, BAND_BAR_HEIGHT);
    lv_obj_add_event_cb(bar, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    tag = lv_label_create(parent);
    lv_obj_set_style_text_font(tag, &sony_22, 0);
    lv_obj_set_style_bg_color(tag, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_60, 0);
    lv_obj_set_style_radius(tag, 4, 0);
    lv_obj_set_style_pad_hor(tag, 8, 0);
    lv_obj_set_style_pad_ver(tag, 2, 0);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
    return bar;
}

void band_bar_set_geometry(lv_coord_t y, bool shown, lv_coord_t view_y) {
    if (!bar) {
        return;
    }
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_pos(tag, 8, view_y + 8);
    if (shown) {
        bool was_hidden = lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
        if (was_hidden) {
            tag_show(); /* into 3D: where the dial is */
        }
    } else {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        tag_hide();
    }
}

void band_bar_set_range(int32_t lo, int32_t hi) {
    if (lo == lo_hz && hi == hi_hz) {
        return;
    }
    lo_hz = lo;
    hi_hz = hi;
    if (bar && !lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_invalidate(bar);
    }
}

void band_bar_refresh(void) {
    if (bar && !lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_invalidate(bar);
        tag_show(); /* the new plan's segment */
    }
}

void band_bar_set_dial(int32_t hz) {
    band_plan_seg_t seg;
    const char     *band;
    int             kind;

    dial_hz = hz;
    band    = band_plan_band_name((uint32_t)hz);
    kind    = band_plan_segments(band_plan_region(), (uint32_t)hz, (uint32_t)hz + 1, &seg, 1) == 1 ? (int)seg.kind : -1;
    if (band == tag_band && kind == tag_kind) {
        return;
    }
    tag_band = band;
    tag_kind = kind;
    if (bar && !lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN)) {
        tag_show();
    }
}
