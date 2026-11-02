/*
 * Xiegu X6100 - shared list widget ("CHANNELS" look)
 * See ui_list.h.
 *
 * Copyright (C) 2026 Hany El Imam
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "ui_list.h"
#include "keyboard.h"
#include "events.h"
#include "styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define COLOR_BG            0x27313a
#define COLOR_TEXT          0xffffff
#define COLOR_TEXT_MUTED    0x9aa6b0
#define COLOR_SEL_BG        0x4a6a86
#define COLOR_MARK          0xeac345
#define COLOR_TRACK         0x3a4752

#define PAD_V               5       /* above and below the text of a row   */
#define RESERVED_RIGHT      12      /* right strip kept for the scroll bar */
#define SCROLLBAR_W         4
#define SCROLLBAR_MARGIN    4
#define SCROLL_MARGIN       1       /* rows of context around the selection */
#define DEFAULT_COARSE_STEP 10


typedef struct {
    ui_list_cfg_t   cfg;

    int             count;
    int             sel;            /* -1: none            */
    int             first;          /* first visible row   */

    int             visible;        /* whole rows that fit */
    lv_coord_t      pad;            /* space above and below the text */
    lv_coord_t      row_h;
    lv_coord_t      top;            /* y of the first row inside the object */
} ui_list_t;


static ui_list_t *state(lv_obj_t *obj) {
    return obj ? (ui_list_t *)lv_obj_get_user_data(obj) : NULL;
}

static bool row_valid(const ui_list_t *l, int row) {
    return row >= 0 && row < l->count;
}


/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

/*
 * The visible window is rows first .. first+visible-1.  Scrolling moves
 * the window so that the selected row always has SCROLL_MARGIN rows of
 * context on both sides (except at the ends of the list).
 */
static void scroll_to_sel(ui_list_t *l) {
    if (l->count <= l->visible) {
        l->first = 0;
        return;
    }

    int margin = SCROLL_MARGIN;
    if (margin > (l->visible - 1) / 2) {
        margin = (l->visible - 1) / 2;
    }

    if (row_valid(l, l->sel)) {
        if (l->sel < l->first + margin) {
            l->first = l->sel - margin;
        } else if (l->sel > l->first + l->visible - 1 - margin) {
            l->first = l->sel - (l->visible - 1 - margin);
        }
    }

    int max_first = l->count - l->visible;
    if (l->first > max_first) {
        l->first = max_first;
    }
    if (l->first < 0) {
        l->first = 0;
    }
}

static void set_sel(lv_obj_t *obj, ui_list_t *l, int row) {
    if (l->count <= 0) {
        l->sel = -1;
        l->first = 0;
    } else {
        if (row < 0) {
            row = 0;
        }
        if (row >= l->count) {
            row = l->count - 1;
        }
        l->sel = row;
        scroll_to_sel(l);
    }
    lv_obj_invalidate(obj);
}

/* Fine step: one row, wrap-around. */
static void move_fine(lv_obj_t *obj, ui_list_t *l, int delta) {
    if (l->count <= 0) {
        return;
    }
    if (!row_valid(l, l->sel)) {
        set_sel(obj, l, 0);
        return;
    }

    int row = (l->sel + delta) % l->count;
    if (row < 0) {
        row += l->count;
    }
    set_sel(obj, l, row);
}

/*
 * Coarse step: |delta| rows, stopping at the first/last row; one more
 * step from there wraps to the other end.
 */
static void move_coarse(lv_obj_t *obj, ui_list_t *l, int delta) {
    if (l->count <= 0) {
        return;
    }
    if (!row_valid(l, l->sel)) {
        set_sel(obj, l, 0);
        return;
    }

    int row = l->sel + delta;

    if (row >= l->count) {
        row = (l->sel == l->count - 1) ? 0 : l->count - 1;
    } else if (row < 0) {
        row = (l->sel == 0) ? l->count - 1 : 0;
    }
    set_sel(obj, l, row);
}


/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void key_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    ui_list_t *l = state(obj);

    if (!l) {
        return;
    }

    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    int step = l->cfg.coarse_step;
    int old_sel = l->sel;

    switch (key) {
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            move_fine(obj, l, +1);
            break;

        case LV_KEY_LEFT:
        case LV_KEY_UP:
            move_fine(obj, l, -1);
            break;

        case KEYBOARD_PGDN:
            move_coarse(obj, l, +step);
            break;

        case KEYBOARD_PGUP:
            move_coarse(obj, l, -step);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            if (l->cfg.vol_coarse) {
                move_coarse(obj, l, +step);
            }
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            if (l->cfg.vol_coarse) {
                move_coarse(obj, l, -step);
            }
            break;

        case LV_KEY_HOME:
            set_sel(obj, l, 0);
            break;

        case LV_KEY_END:
            set_sel(obj, l, l->count - 1);
            break;

        default:
            break;
    }

    if (l->sel != old_sel) {
        lv_event_send(obj, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

static void activate_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    ui_list_t *l = state(obj);

    if (!l || !l->cfg.activate_cb || !row_valid(l, l->sel)) {
        return;
    }
    l->cfg.activate_cb(l->cfg.user, l->sel);
}

static void delete_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    ui_list_t *l = state(obj);

    if (l) {
        lv_obj_set_user_data(obj, NULL);
        free(l);
    }
}


/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Copy text into buf, shortened with ".." so that it fits max_w pixels.
 * UTF-8 safe: never cuts inside a multi-byte character.
 */
static const char *fit_text(const lv_font_t *font, const char *text,
                            lv_coord_t max_w, char *buf, size_t buf_size) {
    if (lv_txt_get_width(text, strlen(text), font, 0, LV_TEXT_FLAG_NONE) <= max_w) {
        return text;
    }

    size_t len = strlen(text);
    if (len >= buf_size - 3) {
        len = buf_size - 3;
    }

    while (len > 0) {
        len--;
        while (len > 0 && (text[len] & 0xC0) == 0x80) {
            len--;
        }
        memcpy(buf, text, len);
        strcpy(buf + len, "..");
        if (lv_txt_get_width(buf, strlen(buf), font, 0, LV_TEXT_FLAG_NONE) <= max_w) {
            return buf;
        }
    }
    strcpy(buf, "..");
    return buf;
}

static lv_text_align_t to_lv_align(ui_list_align_t a) {
    switch (a) {
        case UI_LIST_ALIGN_CENTER:  return LV_TEXT_ALIGN_CENTER;
        case UI_LIST_ALIGN_RIGHT:   return LV_TEXT_ALIGN_RIGHT;
        default:                    return LV_TEXT_ALIGN_LEFT;
    }
}

static void draw_empty(lv_draw_ctx_t *ctx, const ui_list_t *l, const lv_area_t *box) {
    if (!l->cfg.empty_text || !l->cfg.empty_text[0]) {
        return;
    }

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = l->cfg.font;
    label.color = lv_color_hex(COLOR_TEXT_MUTED);
    label.opa = LV_OPA_COVER;
    label.align = LV_TEXT_ALIGN_CENTER;

    lv_coord_t text_h = lv_font_get_line_height(l->cfg.font);
    lv_area_t area;
    area.x1 = box->x1;
    area.x2 = box->x2;
    area.y1 = box->y1 + (box->y2 - box->y1 + 1 - text_h) / 2;
    area.y2 = area.y1 + text_h;
    lv_draw_label(ctx, &label, &area, l->cfg.empty_text, NULL);
}

static void draw_scrollbar(lv_draw_ctx_t *ctx, const ui_list_t *l, const lv_area_t *box) {
    if (l->count <= l->visible) {
        return;
    }

    lv_coord_t track_h = l->visible * l->row_h;
    lv_coord_t thumb_h = (lv_coord_t)((int32_t)track_h * l->visible / l->count);
    if (thumb_h < 16) {
        thumb_h = 16;
    }
    lv_coord_t thumb_y = (lv_coord_t)((int32_t)(track_h - thumb_h) * l->first /
                                      (l->count - l->visible));

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = 2;

    lv_area_t a;
    a.x2 = box->x2 - SCROLLBAR_MARGIN;
    a.x1 = a.x2 - SCROLLBAR_W + 1;

    rect.bg_color = lv_color_hex(COLOR_TRACK);
    a.y1 = box->y1 + l->top;
    a.y2 = a.y1 + track_h - 1;
    lv_draw_rect(ctx, &rect, &a);

    rect.bg_color = lv_color_hex(COLOR_TEXT_MUTED);
    a.y1 = box->y1 + l->top + thumb_y;
    a.y2 = a.y1 + thumb_h - 1;
    lv_draw_rect(ctx, &rect, &a);
}

/*
 * Draw the visible rows, the selection bar and the scroll bar.  The panel
 * background is the object's own style.
 */
static void draw_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    ui_list_t *l = state(obj);

    if (!l) {
        return;
    }

    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);

    if (l->count <= 0) {
        draw_empty(ctx, l, &box);
        return;
    }

    const lv_font_t *font = l->cfg.font;

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = font;
    label.opa = LV_OPA_COVER;

    int rows = l->count - l->first;
    if (rows > l->visible) {
        rows = l->visible;
    }

    for (int i = 0; i < rows; i++) {
        int index = l->first + i;
        bool selected = (index == l->sel);

        lv_area_t row;
        row.x1 = box.x1;
        row.x2 = box.x2 - RESERVED_RIGHT;
        row.y1 = box.y1 + l->top + i * l->row_h;
        row.y2 = row.y1 + l->row_h - 1;

        if (selected) {
            lv_draw_rect_dsc_t rect;
            lv_draw_rect_dsc_init(&rect);
            rect.bg_color = lv_color_hex(COLOR_SEL_BG);
            rect.bg_opa = LV_OPA_COVER;
            rect.radius = 6;

            lv_area_t a = row;
            a.x1 += 2;
            a.x2 -= 2;
            a.y1 += 1;
            a.y2 -= 1;
            lv_draw_rect(ctx, &rect, &a);
        }

        ui_list_row_t content;
        memset(&content, 0, sizeof(content));
        if (l->cfg.row_cb) {
            l->cfg.row_cb(l->cfg.user, index, &content);
        }

        if (content.mark) {
            lv_draw_rect_dsc_t mark;
            lv_draw_rect_dsc_init(&mark);
            mark.bg_color = lv_color_hex(COLOR_MARK);
            mark.bg_opa = LV_OPA_COVER;
            mark.radius = 2;

            lv_area_t a;
            a.x1 = row.x1 + 4;
            a.x2 = row.x1 + 7;
            a.y1 = row.y1 + 7;
            a.y2 = row.y2 - 7;
            lv_draw_rect(ctx, &mark, &a);
        }

        for (int c = 0; c < l->cfg.col_count; c++) {
            const ui_list_col_t *col = &l->cfg.cols[c];
            const char *text = content.cell[c];

            if (text[0] == '\0') {
                continue;
            }

            lv_area_t area;
            area.y1 = row.y1 + l->pad;
            area.y2 = row.y2 - l->pad;
            area.x1 = (col->x >= 0) ? row.x1 + col->x : row.x2 + 1 + col->x;
            area.x2 = area.x1 + col->w - 1;

            char buf[UI_LIST_CELL_LEN + 4];
            text = fit_text(font, text, col->w, buf, sizeof(buf));

            bool muted = content.muted || (col->muted && !selected);

            label.align = to_lv_align(col->align);
            label.color = lv_color_hex(muted ? COLOR_TEXT_MUTED : COLOR_TEXT);
            lv_draw_label(ctx, &label, &area, text, NULL);
        }

        if (l->cfg.draw_cb) {
            l->cfg.draw_cb(l->cfg.user, index, ctx, &row, selected);
        }
    }

    draw_scrollbar(ctx, l, &box);
}


/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

lv_obj_t *ui_list_create(lv_obj_t *parent, const ui_list_cfg_t *cfg) {
    if (!parent || !cfg) {
        return NULL;
    }

    ui_list_t *l = calloc(1, sizeof(*l));
    if (!l) {
        return NULL;
    }

    l->cfg = *cfg;
    if (!l->cfg.font) {
        l->cfg.font = &sony_24;
    }
    if (l->cfg.col_count > UI_LIST_MAX_COLS) {
        l->cfg.col_count = UI_LIST_MAX_COLS;
    }
    if (l->cfg.coarse_step == 0) {
        l->cfg.coarse_step = DEFAULT_COARSE_STEP;
    }
    l->sel = -1;

    l->pad = l->cfg.row_pad ? l->cfg.row_pad : PAD_V;
    l->row_h = lv_font_get_line_height(l->cfg.font) + 2 * l->pad;
    l->visible = l->cfg.height / l->row_h;
    if (l->visible < 1) {
        l->visible = 1;
    }
    /* Centre the whole rows vertically: no partly visible row. */
    l->top = (l->cfg.height - l->visible * l->row_h) / 2;

    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) {
        free(l);
        return NULL;
    }

    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(obj, cfg->width, cfg->height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_user_data(obj, l);

    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(obj, activate_cb,
                        cfg->activate_on_click ? LV_EVENT_CLICKED : LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, NULL);

    return obj;
}

void ui_list_set_count(lv_obj_t *list, int count) {
    ui_list_t *l = state(list);

    if (!l) {
        return;
    }

    l->count = count > 0 ? count : 0;

    if (l->count == 0) {
        l->sel = -1;
        l->first = 0;
        lv_obj_invalidate(list);
        return;
    }

    set_sel(list, l, l->sel < 0 ? 0 : l->sel);
}

int ui_list_get_count(lv_obj_t *list) {
    ui_list_t *l = state(list);
    return l ? l->count : 0;
}

int ui_list_get_selected(lv_obj_t *list) {
    ui_list_t *l = state(list);
    return (l && row_valid(l, l->sel)) ? l->sel : -1;
}

void ui_list_select(lv_obj_t *list, int index) {
    ui_list_t *l = state(list);

    if (l) {
        set_sel(list, l, index);
    }
}

int ui_list_visible_rows(lv_obj_t *list) {
    ui_list_t *l = state(list);
    return l ? l->visible : 0;
}

static bool all_digits(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

bool ui_list_parse_stamp(const char *name, const char *prefix,
                         char *date, int date_size,
                         char *time, int time_size) {
    return ui_list_parse_stamp_ext(name, prefix, ".png", date, date_size, time, time_size);
}

bool ui_list_parse_stamp_ext(const char *name, const char *prefix, const char *ext,
                             char *date, int date_size,
                             char *time, int time_size) {
    if (!name || !prefix || !ext || !date || !time || date_size < 11 || time_size < 9) {
        return false;
    }

    size_t plen = strlen(prefix);

    if (strncmp(name, prefix, plen) != 0) {
        return false;
    }

    const char *s = name + plen;

    /* YYYYMMDD-HHMMSS<ext> */
    if (strlen(s) != 15 + strlen(ext) || !all_digits(s, 8) || s[8] != '-' ||
        !all_digits(s + 9, 6) || strcmp(s + 15, ext) != 0) {
        return false;
    }

    snprintf(date, (size_t)date_size, "%.4s-%.2s-%.2s", s, s + 4, s + 6);
    snprintf(time, (size_t)time_size, "%.2s:%.2s:%.2s", s + 9, s + 11, s + 13);
    return true;
}
