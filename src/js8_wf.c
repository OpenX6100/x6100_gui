/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 waterfall on the display's lower plane
 */

#include "js8_wf.h"

#include "lv_drivers/display/drm.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Repaint this often while nothing changes (no rows while transmitting), in
 * case something else drew on the plane: the main screen's spectrum and
 * waterfall redraw there when the frequency moves (js8_wf_repaint_soon). */
#define KEEPALIVE_US 500000

static struct {
    lv_obj_t         *box;
    const lv_color_t *palette;
    float             min, max;
    int               w, h;
    /* Column-major ring: each x has 2 x h pixels, newest first from `head`,
     * the same pixels again h further on. Column x is then one contiguous
     * run of h pixels, which is one line of the plane: the panel is portrait
     * and the screen turned 90 degrees, so a screen column is a plane line. */
    lv_color_t       *ring;
    int               head;
    uint32_t          rows;
    bool              dirty;   /* changed since last put on the plane */
    bool              shown;   /* on the plane and visible since */
    int               again;   /* main-loop passes still to put it there (js8_wf_repaint_soon) */
    int64_t           again_us;
    int64_t           last_us; /* when last put there */
} wf;

static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* The plane's area under the box: screen (x, y) is plane (y, hor_res-1-x),
 * as LVGL's LV_DISP_ROT_90 turns what it draws. */
static bool plane_area(lv_area_t *a) {
    lv_area_t c;
    lv_obj_get_coords(wf.box, &c);
    if (lv_area_get_width(&c) != wf.w || lv_area_get_height(&c) != wf.h) return false;
    lv_coord_t hres = lv_disp_get_hor_res(lv_obj_get_disp(wf.box));
    a->x1           = c.y1;
    a->x2           = c.y2;
    a->y1           = hres - 1 - c.x2;
    a->y2           = hres - 1 - c.x1;
    return a->x1 >= 0 && a->y1 >= 0;
}

/* Plane line j is screen column w-1-j (the plane runs right to left). */
static bool present(void) {
    lv_area_t a;
    if (!plane_area(&a)) return false;
    drm_direct_ctx_t ctx;
    if (!drm_primary_begin_direct(&ctx, (uint32_t)wf.w * wf.h)) return false; /* full this frame: next tick */
    for (int j = 0; j < wf.w; j++)
        memcpy(ctx.buf + (size_t)j * wf.h, wf.ring + (size_t)(wf.w - 1 - j) * 2 * wf.h + wf.head,
               (size_t)wf.h * sizeof(lv_color_t));
    drm_primary_end_direct(&a);
    return true;
}

static void black_out(void) {
    lv_area_t        a;
    drm_direct_ctx_t ctx;
    if (!plane_area(&a) || !drm_primary_begin_direct(&ctx, (uint32_t)wf.w * wf.h)) return;
    lv_color_t black = lv_color_black();
    for (size_t i = 0; i < (size_t)wf.w * wf.h; i++) ctx.buf[i] = black;
    drm_primary_end_direct(&a);
}

/* LVGL may start drawing at the box: it paints all of itself (the hole). */
static void box_cover_cb(lv_event_t *e) {
    lv_cover_check_info_t *info = lv_event_get_param(e);
    if (info->res == LV_COVER_RES_MASKED) return;
    lv_obj_t *obj = lv_event_get_target(e);
    if (_lv_area_is_in(info->area, &obj->coords, 0)) info->res = LV_COVER_RES_COVER;
}

/* Before the box draws itself (a border while keyed, its children after):
 * its pixels fully transparent, whatever the dialog's background left. */
static void box_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *dc  = lv_event_get_draw_ctx(e);
    lv_obj_t      *obj = lv_event_get_target(e);
    lv_area_t      a;
    if (!_lv_area_intersect(&a, dc->clip_area, &obj->coords)) return;
    lv_coord_t  stride = lv_area_get_width(dc->buf_area);
    lv_color_t *buf    = dc->buf;
    size_t      n      = (size_t)lv_area_get_width(&a) * sizeof(lv_color_t);
    for (lv_coord_t y = a.y1; y <= a.y2; y++)
        memset(buf + (size_t)(y - dc->buf_area->y1) * stride + (a.x1 - dc->buf_area->x1), 0, n);
}

static void fill_black(void) {
    lv_color_t black = lv_color_black();
    for (size_t i = 0; i < (size_t)wf.w * 2 * wf.h; i++) wf.ring[i] = black;
}

bool js8_wf_create(lv_obj_t *box, const lv_color_t *palette, int min, int max) {
    lv_obj_update_layout(box);
    wf.w    = lv_obj_get_width(box);
    wf.h    = lv_obj_get_height(box);
    wf.ring = malloc((size_t)wf.w * 2 * wf.h * sizeof(lv_color_t));
    if (!wf.ring) return false;
    wf.box     = box;
    wf.palette = palette;
    wf.min     = (float)min;
    wf.max     = (float)max;
    wf.head    = 0;
    wf.rows    = 0;
    wf.dirty   = true;
    wf.shown   = false;
    wf.again   = 3; /* opening JS8 retuned: the main screen redraws this frame */
    fill_black();
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(box, box_cover_cb, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(box, box_draw_cb, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    lv_obj_invalidate(box);
    return true;
}

void js8_wf_destroy(void) {
    if (!wf.box) return;
    black_out();
    free(wf.ring);
    memset(&wf, 0, sizeof(wf));
}

void js8_wf_add_row(const float *data, uint16_t cnt) {
    if (!wf.ring || !cnt) return;
    wf.head = (wf.head + wf.h - 1) % wf.h;
    wf.rows++;
    for (int x = 0; x < wf.w; x++) {
        float v = (data[(uint32_t)x * cnt / wf.w] - wf.min) / (wf.max - wf.min);
        if (v < 0.0f) v = 0.0f;
        else if (v > 1.0f) v = 1.0f;
        lv_color_t *col = wf.ring + (size_t)x * 2 * wf.h;
        col[wf.head] = col[wf.head + wf.h] = wf.palette[(uint8_t)(v * 255)];
    }
    wf.dirty = true;
}

void js8_wf_fill_rect(int x1, int y1, int x2, int y2, lv_color_t color) {
    if (!wf.ring) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > wf.w - 1) x2 = wf.w - 1;
    if (y2 > wf.h - 1) y2 = wf.h - 1;
    if (x1 > x2 || y1 > y2) return;
    for (int x = x1; x <= x2; x++) {
        lv_color_t *col = wf.ring + (size_t)x * 2 * wf.h;
        for (int y = y1; y <= y2; y++) {
            int i  = (wf.head + y) % wf.h;
            col[i] = col[i + wf.h] = color;
        }
    }
    wf.dirty = true;
}

void js8_wf_clear(void) {
    if (!wf.ring) return;
    fill_black();
    wf.dirty = true;
}

void js8_wf_repaint_soon(void) {
    __atomic_store_n(&wf.again, 3, __ATOMIC_RELAXED);
}

uint32_t js8_wf_rows(void) {
    return wf.rows;
}

void js8_wf_tick(void) {
    if (!wf.ring) return;
    /* Hidden (under the map): nothing to show, and the map covers it. */
    if (!lv_obj_is_visible(wf.box)) {
        wf.shown = false;
        return;
    }
    int64_t now   = mono_us();
    int     again = __atomic_load_n(&wf.again, __ATOMIC_RELAXED);
    if (!wf.dirty && wf.shown && !again && now - wf.last_us < KEEPALIVE_US) return;
    if (!present()) return;
    /* The main screen's redraw comes after this timer in the pass it
     * happens, so the next passes put it right; counted by time, as this
     * runs twice in a pass that adds a row. */
    if (again && now - wf.again_us >= 4000) {
        __atomic_store_n(&wf.again, again - 1, __ATOMIC_RELAXED);
        wf.again_us = now;
    }
    wf.dirty   = false;
    wf.shown   = true;
    wf.last_us = now;
}
