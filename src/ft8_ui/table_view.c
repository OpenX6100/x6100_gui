/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 message table view
 *
 *  Copyright (c) 2026
 */

#include "table_view.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../events.h"

/*
 *  Cell data is owned by a fixed-size ring pool. We do NOT use LVGL's
 *  cell user_data API: lv_table_set_cell_user_data() internally calls
 *  lv_mem_free() on the previous user_data pointer (treating it as a
 *  malloc'd buffer), which would crash on our pool indices. Instead we
 *  keep a parallel s_row_cell[] mapping table row -> cell_data_t* in
 *  this module, and never touch LVGL's user_data slot at all.
 *
 *  Size invariant:
 *      CELL_POOL_SIZE  >= MAX_TABLE_MSG + CLEAN_N_ROWS
 *  Worst case, the table holds MAX_TABLE_MSG rows; one pending push
 *  touches the next slot before truncate_oldest() runs. Any slot that
 *  the ring write cursor is about to overwrite is guaranteed to have
 *  been truncated out of the table by that point.
 */

#define MAX_TABLE_MSG               512
#define CLEAN_N_ROWS                 64
#define CELL_POOL_SIZE              576  /* MAX_TABLE_MSG + CLEAN_N_ROWS */
#define TABLE_MAX_ROWS_INTERNAL     576  /* upper bound on row index used */
#define WAIT_SYNC_TEXT              "Wait sync"

static cell_data_t            s_pool[CELL_POOL_SIZE];
static uint16_t               s_pool_write = 0;
static cell_data_t           *s_row_cell[TABLE_MAX_ROWS_INTERNAL]; /* row -> pool entry, NULL if none */

static lv_obj_t              *s_table      = NULL;
static table_view_press_cb_t  s_press_cb   = NULL;
static table_view_actions_t   s_actions    = {0};
static table_view_info_t      s_info       = TABLE_INFO_COUNTRY_KM;

static inline cell_data_t *row_cell(uint16_t r) {
    if (r >= TABLE_MAX_ROWS_INTERNAL) return NULL;
    return s_row_cell[r];
}

static inline void set_row_cell(uint16_t r, cell_data_t *cd) {
    if (r >= TABLE_MAX_ROWS_INTERNAL) return;
    s_row_cell[r] = cd;
}

static cell_data_t *pool_alloc_and_copy(const cell_data_t *src) {
    cell_data_t *c = &s_pool[s_pool_write];
    s_pool_write = (uint16_t)((s_pool_write + 1u) % CELL_POOL_SIZE);
    *c = *src;
    c->text[sizeof(c->text) - 1] = '\0';
    return c;
}

static void truncate_oldest(void) {
    if (!s_table) return;

    uint16_t rows = lv_table_get_row_cnt(s_table);
    if (rows <= MAX_TABLE_MSG) return;

    lv_table_t *tbl = (lv_table_t *)s_table;
    lv_coord_t  removed_height = 0;
    for (size_t i = 0; i < CLEAN_N_ROWS; i++) {
        removed_height += tbl->row_h[i];
    }
    if (tbl->row_act > CLEAN_N_ROWS) {
        tbl->row_act -= CLEAN_N_ROWS;
    } else {
        tbl->row_act = 0;
    }

    /* Shift rows up: LVGL copies cell text, our parallel s_row_cell[]
     * gets memmove'd in lockstep. No allocations, no frees, and we never
     * touch LVGL's user_data slot. */
    for (uint16_t i = CLEAN_N_ROWS; i < rows; i++) {
        const char *val = lv_table_get_cell_value(s_table, i, 0);
        lv_table_set_cell_value(s_table, (uint16_t)(i - CLEAN_N_ROWS), 0, val ? val : "");
    }
    if (rows > CLEAN_N_ROWS) {
        memmove(&s_row_cell[0], &s_row_cell[CLEAN_N_ROWS],
                (size_t)(rows - CLEAN_N_ROWS) * sizeof(s_row_cell[0]));
    }
    /* Zero out the tail entries that were just shifted away. */
    for (uint16_t i = (uint16_t)(rows - CLEAN_N_ROWS); i < rows; i++) {
        set_row_cell(i, NULL);
    }

    rows = (uint16_t)(rows - CLEAN_N_ROWS);
    lv_table_set_row_cnt(s_table, rows);
    lv_obj_scroll_by_bounded(s_table, 0, removed_height, LV_ANIM_OFF);
}

/* Return true if this cell_data_t matches an RX-header row. */
static bool is_rx_header(const cell_data_t *cd) {
    return cd && (cd->cell_type == CELL_RX_INFO);
}

void table_view_push_ui(const cell_data_t *src) {
    if (!s_table || !src) return;

    truncate_oldest();

    uint16_t rows = lv_table_get_row_cnt(s_table);
    if ((rows == 1) && (strcmp(lv_table_get_cell_value(s_table, 0, 0), WAIT_SYNC_TEXT) == 0)) {
        /* The single "Wait sync" placeholder is about to be replaced. */
        rows = 0;
    }

    /* Row autoscroll: keep scrolled-to-bottom unless the user moved away. */
    uint16_t selected_row = 0;
    uint16_t selected_col = 0;
    lv_table_get_selected_cell(s_table, &selected_row, &selected_col);
    bool scroll = (rows == (uint16_t)(selected_row + 1));

    /* In-place RX-header collapse: replace the last row text instead of
     * appending another identical "RX ..." marker. */
    if (is_rx_header(src) && (rows > 0)) {
        uint16_t last = (uint16_t)(rows - 1);
        cell_data_t *last_cd = row_cell(last);
        if (last_cd && is_rx_header(last_cd)) {
            strncpy(last_cd->text, src->text, sizeof(last_cd->text) - 1);
            last_cd->text[sizeof(last_cd->text) - 1] = '\0';
            lv_table_set_cell_value(s_table, last, 0, last_cd->text);
            return;
        }
    }

    cell_data_t *stored = pool_alloc_and_copy(src);
    lv_table_set_cell_value(s_table, rows, 0, stored->text);
    set_row_cell(rows, stored);

    if (scroll) {
        static int32_t c = LV_KEY_DOWN;
        lv_event_send(s_table, LV_EVENT_KEY, &c);
    }
}

void table_view_add_msg_cb(void *data) {
    table_view_push_ui((const cell_data_t *)data);
}

void table_view_reset(void) {
    if (!s_table) return;

    /* Clear our parallel row->cell map; LVGL's user_data slot is never
     * touched (it stays NULL), so there is no stale-pointer free path
     * during set_row_cnt() shrink. */
    memset(s_row_cell, 0, sizeof(s_row_cell));
    lv_table_set_row_cnt(s_table, 1);
    lv_table_set_cell_value(s_table, 0, 0, WAIT_SYNC_TEXT);
    s_pool_write = 0;
}

/* -------- LVGL event plumbing ------------------------------------------ */

static void draw_part_begin_cb(lv_event_t *e) {
    lv_obj_t               *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
    uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);
    (void)col;
    cell_data_t *cd = row_cell((uint16_t)row);

    dsc->rect_dsc->bg_opa = LV_OPA_50;

    if (!cd) {
        dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
        dsc->rect_dsc->bg_color = lv_color_hex(0x303030);
    } else {
        switch (cd->cell_type) {
        case CELL_START_QSO:
        case CELL_RX_INFO:
            dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
            dsc->rect_dsc->bg_color = lv_color_hex(0x303030);
            break;
        case CELL_RX_CQ:
            switch (cd->worked_type) {
            case SEARCH_WORKED_NO:
                dsc->rect_dsc->bg_color = lv_color_hex(0x00DD00);
                break;
            case SEARCH_WORKED_YES:
                dsc->label_dsc->opa = LV_OPA_90;
                dsc->rect_dsc->bg_color = lv_color_hex(0x2e5a00);
                break;
            case SEARCH_WORKED_SAME_MODE:
                dsc->label_dsc->decor = LV_TEXT_DECOR_STRIKETHROUGH;
                dsc->label_dsc->opa   = LV_OPA_80;
                dsc->rect_dsc->bg_color = lv_color_hex(0x224400);
                break;
            }
            break;
        case CELL_RX_TO_ME:
            dsc->rect_dsc->bg_color = lv_color_hex(0xFF0000);
            break;
        case CELL_TX_MSG:
            dsc->rect_dsc->bg_color = lv_color_hex(0x0000FF);
            break;
        default:
            dsc->rect_dsc->bg_color = lv_color_black();
            break;
        }
    }

    uint16_t selected_row;
    uint16_t selected_col;
    lv_table_get_selected_cell(obj, &selected_row, &selected_col);
    if (selected_row == row) {
        dsc->rect_dsc->bg_color = lv_color_lighten(dsc->rect_dsc->bg_color, 20);
    }
}

static lv_coord_t text_width(const char *text, const lv_draw_label_dsc_t *ld) {
    lv_point_t size;

    lv_txt_get_size(&size, text, ld->font, ld->letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* The sender's country, right-aligned up to `right`: the name when it fits
 * after the message (`text_end`), else its prefix, else nothing. Measured
 * with the table's own font, so it holds for both row sizes (Settings >
 * Decoder text) and for FT4 as for FT8. */
static void draw_country(lv_obj_draw_part_dsc_t *dsc, const cell_data_t *cd, lv_area_t *area,
                         lv_coord_t right, lv_coord_t text_end) {
    const char *choice[2] = { cd->country, cd->cty_prefix };

    for (int i = 0; i < 2; i++) {
        if (choice[i][0] == '\0') continue;

        lv_coord_t w = text_width(choice[i], dsc->label_dsc);
        if (right - w < text_end + 16) continue;

        area->x1 = right - w;
        area->x2 = right;
        lv_draw_label(dsc->draw_ctx, dsc->label_dsc, area, choice[i], NULL);
        return;
    }
}

static void draw_part_end_cb(lv_event_t *e) {
    lv_obj_t               *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
    uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);
    (void)col;
    cell_data_t *cd = row_cell((uint16_t)row);
    if (!cd) return;

    if ((cd->cell_type != CELL_RX_MSG) &&
        (cd->cell_type != CELL_RX_CQ) &&
        (cd->cell_type != CELL_RX_TO_ME)) {
        return;
    }

    char              buf[64];
    const lv_coord_t  cell_top    = lv_obj_get_style_pad_top(obj, LV_PART_ITEMS);
    const lv_coord_t  cell_bottom = lv_obj_get_style_pad_bottom(obj, LV_PART_ITEMS);
    lv_area_t         area;

    dsc->label_dsc->align = LV_TEXT_ALIGN_RIGHT;

    area.y1 = dsc->draw_area->y1 + cell_top;
    area.y2 = dsc->draw_area->y2 - cell_bottom;
    area.x2 = dsc->draw_area->x2 - 15;
    area.x1 = area.x2 - 120;

    snprintf(buf, sizeof(buf), "%i dB", cd->meta.local_snr);
    lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);

    /* Country: left of the distance, or where the distance would be. */
    lv_coord_t right    = area.x1 - 10;
    lv_coord_t text_end = dsc->draw_area->x1 + lv_obj_get_style_pad_left(obj, LV_PART_ITEMS) +
                          text_width(cd->text, dsc->label_dsc);

    bool miles = (s_info == TABLE_INFO_MI) || (s_info == TABLE_INFO_COUNTRY_MI);

    if (s_info != TABLE_INFO_COUNTRY && cd->dist > 0) {
        area.x2 = area.x1 - 10;
        area.x1 = area.x2 - 200;
        if (miles) {
            snprintf(buf, sizeof(buf), "%i mi", (int)(cd->dist * 0.621371f + 0.5f));
        } else {
            snprintf(buf, sizeof(buf), "%i km", cd->dist);
        }
        /* Only where it does not run into the message (a long WSPR line,
         * a big font). */
        lv_coord_t w = text_width(buf, dsc->label_dsc);
        if (area.x2 - w >= text_end + 8) {
            lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);
            right = area.x2 - w - 16;
        }
    }

    if (s_info >= TABLE_INFO_COUNTRY) {
        draw_country(dsc, cd, &area, right, text_end);
    }
}

static void cell_press_cb(lv_event_t *e) {
    (void)e;
    if (!s_press_cb) return;

    uint16_t row, col;
    lv_table_get_selected_cell(s_table, &row, &col);
    (void)col;
    cell_data_t *cd = row_cell(row);
    s_press_cb(cd);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
    case LV_KEY_ESC:
        if (s_actions.on_close) s_actions.on_close();
        break;
    case KEY_VOL_LEFT_EDIT:
    case KEY_VOL_LEFT_SELECT:
        if (s_actions.on_vol_change) s_actions.on_vol_change(-1);
        break;
    case KEY_VOL_RIGHT_EDIT:
    case KEY_VOL_RIGHT_SELECT:
        if (s_actions.on_vol_change) s_actions.on_vol_change(1);
        break;
    default:
        break;
    }
}

void table_view_build(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    s_pool_write = 0;
    s_press_cb   = NULL;
    memset(&s_actions, 0, sizeof(s_actions));
    memset(s_row_cell, 0, sizeof(s_row_cell));

    s_table = lv_table_create(parent);
    lv_obj_remove_style(s_table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_add_event_cb(s_table, cell_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_table, draw_part_begin_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(s_table, draw_part_end_cb,   LV_EVENT_DRAW_PART_END,   NULL);

    lv_obj_set_size(s_table, w, h);
    lv_obj_set_pos(s_table, x, y);

    lv_table_set_col_cnt(s_table, 1);
    lv_table_set_col_width(s_table, 0, w - 2);

    lv_obj_set_style_border_width(s_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_table, 192, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_table, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_table, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_table, 128, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_table, lv_color_hex(0xC0C0C0), LV_PART_ITEMS);
    lv_obj_set_style_pad_top(s_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(s_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(s_table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(s_table, 0, LV_PART_ITEMS);

    lv_table_set_cell_value(s_table, 0, 0, WAIT_SYNC_TEXT);
}

void table_view_destroy(void) {
    if (s_table) {
        /* LVGL will delete its internal cell strings; we never wrote to
         * cell user_data, so its NULL slots have nothing to free. */
        lv_obj_del(s_table);
        s_table = NULL;
    }
    s_press_cb = NULL;
    memset(&s_actions, 0, sizeof(s_actions));
    memset(s_row_cell, 0, sizeof(s_row_cell));
    s_pool_write = 0;
}

lv_obj_t *table_view_obj(void) {
    return s_table;
}

void table_view_set_press_cb(table_view_press_cb_t cb) {
    s_press_cb = cb;
}

void table_view_set_info(table_view_info_t info) {
    s_info = (info <= TABLE_INFO_COUNTRY_MI) ? info : TABLE_INFO_COUNTRY_KM;
}

void table_view_set_actions(const table_view_actions_t *actions) {
    if (actions) {
        s_actions = *actions;
    } else {
        memset(&s_actions, 0, sizeof(s_actions));
    }
}
