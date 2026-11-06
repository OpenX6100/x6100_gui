/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the shared map (see map_widget.h)
 *
 *  Moved out of the JS8 app's Show Map (dialog_js8.c) so that every app
 *  draws the same map.
 */

#include "map_widget.h"

#include "styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_CORNER 11 /* the frame's corners reach this far in (x + y) */
#define MAP_TAKEN  32 /* label boxes placed per redraw */

static lv_obj_t         *parent_obj;
static lv_obj_t         *map_box, *map_canvas;
static map_widget_geom_t geom;
static uint32_t         *map_base, *map_px; /* the base map, and base + what the app drew, as shown */
static js8_map_data_t   *map_data;
static js8_map_view_t    map_view;
static bool              map_view_ok, map_world;
static lv_area_t         map_taken[MAP_TAKEN];
static int               map_ntaken;

/* ---- The frame's corners ---- */

/* The corners as the parent's frame image draws them (over the screen
 * behind it): the map's square corners would stick out past them. Read
 * from the image when the map is made. */
static struct {
    int16_t  x, y; /* on the map */
    uint32_t c;
} map_corner_px[4 * MAP_CORNER * (MAP_CORNER + 1) / 2];
static int map_corner_n;

static bool img_read(lv_img_decoder_dsc_t *dsc, int x, int y, int len, lv_color32_t *out) {
    if (dsc->img_data) { /* an image in memory */
        memcpy(out, dsc->img_data + ((size_t)y * dsc->header.w + x) * sizeof(lv_color32_t), len * sizeof(lv_color32_t));
        return true;
    }
    return lv_img_decoder_read_line(dsc, x, y, len, (uint8_t *)out) == LV_RES_OK; /* a file */
}

static void corners_load(void) {
    map_corner_n = 0;
    const void          *src = lv_obj_get_style_bg_img_src(parent_obj, LV_PART_MAIN);
    lv_img_decoder_dsc_t dsc;
    if (!src || lv_img_decoder_open(&dsc, src, lv_color_white(), 0) != LV_RES_OK) return;
    if (dsc.header.cf == LV_IMG_CF_TRUE_COLOR_ALPHA && sizeof(lv_color_t) == sizeof(lv_color32_t) &&
        geom.x + geom.w < (int)dsc.header.w && geom.y + geom.h < (int)dsc.header.h) {
        /* Behind the window: R1CBU 1.0's other display plane, whose
         * spectrum and waterfall the map's apps turn off (black). Blending
         * the few corner pixels here keeps the map an opaque canvas (cheap
         * to draw). */
        lv_color32_t bg = {.full = lv_color_to32(lv_color_black())};
        for (int corner = 0; corner < 4; corner++) {
            bool right = corner & 1, bottom = corner & 2;
            int  x0    = right ? geom.w - MAP_CORNER : 0;
            for (int dy = 0; dy < MAP_CORNER; dy++) {
                int          y = bottom ? geom.h - 1 - dy : dy;
                lv_color32_t row[MAP_CORNER], mid;
                if (!img_read(&dsc, geom.x + x0, geom.y + y, MAP_CORNER, row) ||
                    !img_read(&dsc, geom.x + geom.w / 2, geom.y + y, 1, &mid))
                    continue;
                for (int dx = 0; dx + dy < MAP_CORNER; dx++) {
                    int          x = right ? geom.w - 1 - dx : dx;
                    lv_color32_t p = row[x - x0];
                    /* As the band in the middle of this row: the map covers it. */
                    if (abs(p.ch.alpha - mid.ch.alpha) <= 8 && abs(p.ch.red - mid.ch.red) <= 40 &&
                        abs(p.ch.green - mid.ch.green) <= 40 && abs(p.ch.blue - mid.ch.blue) <= 40)
                        continue;
                    int a = p.ch.alpha;
                    map_corner_px[map_corner_n].x = x;
                    map_corner_px[map_corner_n].y = y;
                    map_corner_px[map_corner_n].c = 0xFF000000u |
                                                    (uint32_t)((p.ch.red * a + bg.ch.red * (255 - a)) / 255) << 16 |
                                                    (uint32_t)((p.ch.green * a + bg.ch.green * (255 - a)) / 255) << 8 |
                                                    (uint32_t)((p.ch.blue * a + bg.ch.blue * (255 - a)) / 255);
                    map_corner_n++;
                }
            }
        }
    }
    lv_img_decoder_close(&dsc);
}

static void corners_apply(void) {
    for (int i = 0; i < map_corner_n; i++) map_px[map_corner_px[i].y * geom.w + map_corner_px[i].x] = map_corner_px[i].c;
}

/* ---- Making, showing ---- */

bool map_widget_create(lv_obj_t *parent, const map_widget_geom_t *g, const char *data_path, const char *cty_sd,
                       const char *cty) {
    if (map_box) return true;
    geom       = *g;
    parent_obj = parent;
    map_base   = malloc((size_t)geom.w * geom.h * sizeof(uint32_t));
    map_px     = malloc((size_t)geom.w * geom.h * sizeof(uint32_t));
    if (!map_base || !map_px) {
        free(map_base);
        free(map_px);
        map_base = map_px = NULL;
        return false;
    }
    if (!map_data) map_data = js8_map_data_load(data_path);
    if (!js8_map_countries_loaded() && !js8_map_load_countries(cty_sd)) js8_map_load_countries(cty);
    if (!map_data) LV_LOG_USER("Map: no map data at %s", data_path);

    /* An opaque box, as the waterfall's: LVGL starts drawing there and
     * never redraws what is behind it. */
    map_box = lv_obj_create(parent);
    lv_obj_remove_style_all(map_box);
    lv_obj_set_style_bg_color(map_box, lv_color_hex(MAP_WIDGET_OCEAN), 0);
    lv_obj_set_style_bg_opa(map_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(map_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(map_box, geom.w, geom.h);
    lv_obj_set_pos(map_box, geom.x, geom.y);
    lv_obj_add_flag(map_box, LV_OBJ_FLAG_HIDDEN);

    map_canvas = lv_canvas_create(map_box);
    lv_canvas_set_buffer(map_canvas, map_px, geom.w, geom.h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(map_canvas, 0, 0);
    corners_load();
    map_view_ok = false;
    return true;
}

bool map_widget_made(void) {
    return map_box != NULL;
}

void map_widget_free(void) {
    /* The objects go with the parent window. */
    map_box = map_canvas = NULL;
    parent_obj           = NULL;
    free(map_base);
    free(map_px);
    map_base = map_px = NULL;
    js8_map_data_free(map_data);
    map_data    = NULL;
    map_view_ok = false;
}

lv_obj_t *map_widget_box(void) {
    return map_box;
}

lv_obj_t *map_widget_canvas(void) {
    return map_canvas;
}

const map_widget_geom_t *map_widget_geom(void) {
    return &geom;
}

void map_widget_show(bool on) {
    if (!map_box) return;
    if (on) {
        lv_obj_clear_flag(map_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(map_box);
    } else {
        lv_obj_add_flag(map_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void map_widget_view_reset(void) {
    map_view_ok = false;
}

/* ---- The view ---- */

bool map_widget_fit(js8_map_mode_t mode, double my_lat, double my_lon, const char *my_continent,
                    const js8_map_point_t *pts, unsigned n, bool *world) {
    js8_map_view_t v;
    bool           w = js8_map_choose_view(mode, my_lat, my_lon, my_continent, pts, n, geom.w, geom.fit_h, &v);
    /* Drawn over the whole area; the fitted part sits above the legend. */
    v.merc_c -= (geom.h / 2.0 - geom.fit_h / 2.0) / v.px_deg;
    v.height = geom.h;
    bool base_new = !map_view_ok || v.lon_c != map_view.lon_c || v.merc_c != map_view.merc_c ||
                    v.px_deg != map_view.px_deg;
    if (base_new) {
        if (!map_data || !js8_map_render_base(map_data, &v, map_base, geom.w))
            for (size_t i = 0; i < (size_t)geom.w * geom.h; i++) map_base[i] = 0xFF000000u | MAP_WIDGET_OCEAN;
        map_view    = v;
        map_view_ok = true;
        map_world   = w;
        LV_LOG_USER("Map: view lon %.1f merc %.1f, %.2f px/deg, %u stations%s", v.lon_c, v.merc_c, v.px_deg, n,
                    w ? ", world" : "");
    }
    if (world) *world = map_world;
    return base_new;
}

const js8_map_view_t *map_widget_view(void) {
    return &map_view;
}

bool map_widget_world(void) {
    return map_world;
}

uint32_t map_widget_hash(uint32_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* ---- Drawing ---- */

void map_widget_begin(void) {
    memcpy(map_px, map_base, (size_t)geom.w * geom.h * sizeof(uint32_t));
    map_ntaken = 0;
}

void map_widget_end(void) {
    corners_apply();
    lv_obj_invalidate(map_canvas);
}

void map_widget_rect_ex(int x, int y, int w, int h, uint32_t fill, lv_opa_t fill_opa, uint32_t border,
                        lv_opa_t border_opa, int bw, lv_coord_t radius) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color     = lv_color_hex(fill);
    d.bg_opa       = fill_opa;
    d.border_color = lv_color_hex(border);
    d.border_width = bw;
    d.border_opa   = bw ? border_opa : LV_OPA_TRANSP;
    d.radius       = radius;
    lv_canvas_draw_rect(map_canvas, x, y, w, h, &d);
}

void map_widget_rect(int x, int y, int w, int h, uint32_t fill, lv_opa_t fill_opa, uint32_t border, int bw,
                     lv_coord_t radius) {
    map_widget_rect_ex(x, y, w, h, fill, fill_opa, border, LV_OPA_COVER, bw, radius);
}

void map_widget_text(int x, int y, int w, const lv_font_t *font, uint32_t color, const char *text) {
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(color);
    ld.font  = font;
    lv_canvas_draw_text(map_canvas, x, y, w, &ld, text);
}

bool map_widget_overlaps(const lv_area_t *a) {
    for (int i = 0; i < map_ntaken; i++)
        if (a->x1 < map_taken[i].x2 && map_taken[i].x1 < a->x2 && a->y1 < map_taken[i].y2 && map_taken[i].y1 < a->y2)
            return true;
    return false;
}

void map_widget_take(lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2) {
    if (map_ntaken < MAP_TAKEN) map_taken[map_ntaken++] = (lv_area_t){x1, y1, x2, y2};
}

bool map_widget_label(int x, int y, const char *text, uint32_t fg, uint32_t bg, lv_opa_t bg_opa, int dx, bool force) {
    lv_point_t sz;
    lv_txt_get_size(&sz, text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int places[4][2] = {{x + dx, y - sz.y / 2},
                              {x - dx - sz.x, y - sz.y / 2},
                              {x - sz.x / 2, y - dx - sz.y},
                              {x - sz.x / 2, y + dx}};
    lv_area_t box;
    int       bx = 0, by = 0;
    bool      found = false;
    /* Never over its own station (moved back onto the map, a label can
     * land on it). */
    const lv_area_t own = {x - 8, y - 8, x + 8, y + 8};
#define LABEL_FREE(b)                                                                                                  \
    (!map_widget_overlaps(&(b)) && !((b).x1 < own.x2 && own.x1 < (b).x2 && (b).y1 < own.y2 && own.y1 < (b).y2))
    for (int k = 0; k < 4 && !found; k++) {
        bx    = LV_CLAMP(4, places[k][0], geom.w - sz.x - 4);
        by    = LV_CLAMP(2, places[k][1], geom.label_bottom - sz.y - 2);
        box   = (lv_area_t){bx - 4, by - 2, bx + sz.x + 4, by + sz.y + 2};
        found = LABEL_FREE(box);
        if (force && !found && k == 0) { /* forced: keep the first place unless another is free */
            lv_area_t first = box;
            int       fx = bx, fy = by;
            for (int j = 1; j < 4 && !found; j++) {
                bx    = LV_CLAMP(4, places[j][0], geom.w - sz.x - 4);
                by    = LV_CLAMP(2, places[j][1], geom.label_bottom - sz.y - 2);
                box   = (lv_area_t){bx - 4, by - 2, bx + sz.x + 4, by + sz.y + 2};
                found = LABEL_FREE(box);
            }
            if (!found) {
                box   = first;
                bx    = fx;
                by    = fy;
                found = true;
            }
        }
    }
#undef LABEL_FREE
    if (!found) return false;
    map_widget_take(box.x1, box.y1, box.x2, box.y2);
    map_widget_rect(box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1, bg, bg_opa, 0, 0, 3);
    map_widget_text(bx, by, sz.x + 4, &sony_18, fg, text);
    return true;
}

int map_widget_station_mark(const char *grid, const js8_map_place_t *pl, uint32_t color, lv_opa_t opa,
                            uint32_t border, lv_opa_t line_opa, int grow, int *cx, int *cy) {
    float x0, y0, x1, y1;
    char  g4[5] = "";
    if (!pl->approx && strlen(grid) >= 4) snprintf(g4, sizeof(g4), "%.4s", grid);
    if (g4[0] && js8_map_grid_rect(&map_view, g4, &x0, &y0, &x1, &y1)) {
        float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        int   side = (int)((x1 - x0 + y1 - y0) * 0.375f + 0.5f); /* a little smaller than the square (VE7NHW) */
        side       = LV_CLAMP(6, side, 20) + grow;
        side       = LV_CLAMP(5, side, 22);
        *cx        = (int)(mx + 0.5f);
        *cy        = (int)(my + 0.5f);
        map_widget_rect_ex(*cx - side / 2, *cy - side / 2, side, side, color, opa, border, line_opa, 1, 0);
        return side;
    }
    float x, y;
    int   side = LV_CLAMP(7, 9 + grow, 12);
    js8_map_project(&map_view, pl->lat, pl->lon, &x, &y);
    map_widget_rect_ex((int)x - side / 2, (int)y - side / 2, side, side, color, LV_OPA_TRANSP,
                       border == color ? color : border, line_opa, 2, 0);
    *cx = (int)x;
    *cy = (int)y;
    return side;
}

void map_widget_path_points(double lat1, double lon1, double lat2, double lon2, lv_point_t *p) {
    float xs[MAP_WIDGET_PATH_N + 1], ys[MAP_WIDGET_PATH_N + 1];
    js8_map_path(&map_view, lat1, lon1, lat2, lon2, MAP_WIDGET_PATH_N, xs, ys);
    for (int k = 0; k <= MAP_WIDGET_PATH_N; k++) {
        p[k].x = (lv_coord_t)LV_CLAMP(-4000, (int)xs[k], 4000);
        p[k].y = (lv_coord_t)LV_CLAMP(-4000, (int)ys[k], 4000);
    }
}

void map_widget_line(const lv_point_t *p, int n, const lv_draw_line_dsc_t *line) {
    lv_canvas_draw_line(map_canvas, p, n, line);
}

bool map_widget_badge_box_at(int x, int y, int side, const char *text, bool below, lv_area_t *box) {
    lv_point_t sz;
    lv_txt_get_size(&sz, text, &sony_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int h  = LV_MAX(side / 2, 6);
    int bx = LV_CLAMP(2, x + h - 3, geom.w - sz.x - 8);
    int by = LV_CLAMP(2, below ? y + h - 3 : y - h + 3 - sz.y - 2, geom.label_bottom - sz.y - 4);
    *box   = (lv_area_t){bx, by, bx + sz.x + 6, by + sz.y + 2};
    if (map_widget_overlaps(box)) return false;
    map_widget_take(box->x1, box->y1, box->x2, box->y2);
    return true;
}

bool map_widget_badge_box(int x, int y, int side, const char *text, lv_area_t *box) {
    return map_widget_badge_box_at(x, y, side, text, false, box);
}

void map_widget_badge_ex(const lv_area_t *box, const char *text, uint32_t bg, uint32_t fg) {
    map_widget_rect(box->x1, box->y1, box->x2 - box->x1, box->y2 - box->y1, bg, 235, 0x000000, 1, 3);
    map_widget_text(box->x1 + 3, box->y1 + 1, box->x2 - box->x1, &sony_14, fg, text);
}

void map_widget_badge(const lv_area_t *box, const char *text) {
    map_widget_badge_ex(box, text, MAP_WIDGET_CQ_BG, 0x000000);
}

/* ---- The legend ---- */

void map_widget_legend(const map_legend_item_t *items, unsigned n, const char *right, int y0, int h) {
    /* See-through, as the status line: a dark box only behind the text,
     * the map all around (VE7NHW: "makes the screen look bigger"). */
    int cy = y0 + h / 2;
    int x = 8, w = 0;
    for (unsigned i = 0; i < n; i++) {
        lv_point_t sz;
        lv_txt_get_size(&sz, items[i].text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        w += 16 + sz.x + (i + 1 < n ? 16 : 0);
    }
    map_widget_rect(x - 4, y0 + 2, w + 8, h - 3, 0x000000, LV_OPA_50, 0, 0, 0);
    map_widget_take(x - 4, y0 + 2, x + w + 4, y0 + h);
    for (unsigned i = 0; i < n; i++) {
        if (items[i].kind == MAP_LEGEND_SQUARE)
            map_widget_rect(x, cy - 5, 10, 10, items[i].color, MAP_WIDGET_SQUARE_OPA + 60, items[i].color, 1, 0);
        else if (items[i].kind == MAP_LEGEND_HOLLOW)
            map_widget_rect(x, cy - 5, 10, 10, items[i].color, LV_OPA_TRANSP, items[i].color, 2, 0);
        else
            map_widget_rect(x, cy - 1, 12, 3, items[i].color, LV_OPA_COVER, 0, 0, 0);
        lv_point_t sz;
        lv_txt_get_size(&sz, items[i].text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        map_widget_text(x + 16, cy - sz.y / 2, sz.x + 4, &sony_18, 0xDDDDDD, items[i].text);
        x += 16 + sz.x + 16;
    }
    if (!right || !right[0]) return;
    lv_point_t sz;
    lv_txt_get_size(&sz, right, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    map_widget_rect(geom.w - 12 - sz.x, y0 + 2, sz.x + 8, h - 3, 0x000000, LV_OPA_50, 0, 0, 0);
    map_widget_take(geom.w - 12 - sz.x, y0 + 2, geom.w, y0 + h);
    map_widget_text(geom.w - 8 - sz.x, cy - sz.y / 2, sz.x + 4, &sony_18, 0xFFFFFF, right);
}

const char *map_widget_continent_name(const char *code) {
    return strcmp(code, "NA") == 0   ? "North America"
           : strcmp(code, "SA") == 0 ? "South America"
           : strcmp(code, "EU") == 0 ? "Europe"
           : strcmp(code, "AF") == 0 ? "Africa"
           : strcmp(code, "AS") == 0 ? "Asia"
           : strcmp(code, "OC") == 0 ? "Oceania"
                                     : "Your continent";
}
