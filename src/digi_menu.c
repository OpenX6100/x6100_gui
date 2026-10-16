/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - on-screen list for the digital-mode windows
 */

#include "digi_menu.h"

#include <stdio.h>
#include <string.h>

#include "cfg/cfg_api.h"
#include "keyboard.h"
#include "styles.h"

/* Sized by the "Decoder text" setting, as the received text is:
 * Compact keeps the window behind in view, Large reads at arm's length. */
#define MENU_WIDTH_COMPACT  560
#define MENU_WIDTH_LARGE    740
#define ITEM_LEN    64

static lv_obj_t       *box;
static lv_obj_t       *list;
static lv_obj_t       *back_obj;
static lv_obj_t       *prev_focus;      /* focused before, when no `back` */
static bool            prev_editing;
static digi_menu_cb_t  pick_cb;
static void           *pick_ctx;
static int             nitems;

static void restore_focus(void)
{
    if (back_obj != NULL) {
        lv_group_add_obj(keyboard_group, back_obj);
        lv_group_focus_obj(back_obj);
        lv_group_set_editing(keyboard_group, true);
    } else if (prev_focus != NULL) {
        lv_group_focus_obj(prev_focus);
        lv_group_set_editing(keyboard_group, prev_editing);
    }
    prev_focus = NULL;
}

void digi_menu_close(void)
{
    if (box == NULL) {
        return;
    }
    lv_group_remove_obj(list);
    lv_obj_del(box);
    box = NULL;
    list = NULL;
    restore_focus();
    back_obj = NULL;
}

bool digi_menu_is_open(void)
{
    return box != NULL;
}

static void key_cb(lv_event_t *e)
{
    uint32_t       key = *((uint32_t *)lv_event_get_param(e));
    uint16_t       row;
    uint16_t       col;
    digi_menu_cb_t cb = pick_cb;
    void          *ctx = pick_ctx;

    switch (key) {
        case LV_KEY_ENTER:
            lv_table_get_selected_cell(list, &row, &col);
            digi_menu_close();
            if (cb != NULL && row < nitems) {
                cb((int)row, ctx);
            }
            break;

        case LV_KEY_ESC:
            digi_menu_close();
            break;
    }
}

/* A long list scrolls: keep the picked row in view. */
static void changed_cb(lv_event_t *e)
{
    lv_table_t *t = (lv_table_t *)list;
    lv_coord_t  y = 0;
    lv_coord_t  h = lv_obj_get_content_height(list);
    lv_coord_t  top = lv_obj_get_scroll_y(list);

    (void)e;
    for (uint16_t r = 0; r < t->row_act && r < t->row_cnt; r++) {
        y += t->row_h[r];
    }
    if (y < top) {
        lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
    } else if (t->row_act < t->row_cnt && y + t->row_h[t->row_act] > top + h) {
        lv_obj_scroll_to_y(list, y + t->row_h[t->row_act] - h, LV_ANIM_OFF);
    }
}

/* The picked row, highlighted as in the window's list. */
static void draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    uint16_t                row;
    uint16_t                col;

    if (dsc->part != LV_PART_ITEMS || dsc->rect_dsc == NULL) {
        return;
    }
    lv_table_get_selected_cell(list, &row, &col);
    if (dsc->id == row) {
        dsc->rect_dsc->bg_color = lv_color_hex(0x2060C0);
        dsc->rect_dsc->bg_opa = LV_OPA_COVER;
    }
}

void digi_menu_open(lv_obj_t *parent, const char *title, const char *const *items, int n,
                    digi_menu_cb_t cb, void *ctx, lv_obj_t *back)
{
    lv_obj_t        *label;
    bool             compact = param_i_get(cfg.ui.digi_compact()) != 0;
    lv_coord_t       width = compact ? MENU_WIDTH_COMPACT : MENU_WIDTH_LARGE;
    const lv_font_t *font = compact ? &sony_28 : &sony_36;
    const lv_font_t *title_font = compact ? &sony_24 : &sony_28;
    lv_coord_t       avail;
    lv_coord_t       y_ofs = 0;

    digi_menu_close();
    if (n <= 0) {
        return;
    }
    if (n > DIGI_MENU_MAX) {
        n = DIGI_MENU_MAX;
    }
    pick_cb = cb;
    pick_ctx = ctx;
    nitems = n;
    back_obj = back;

    box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, width);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_border_color(box, lv_color_white(), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_60, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);

    label = lv_label_create(box);
    lv_obj_set_style_text_font(label, title_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xA0A0A0), 0);
    lv_label_set_text(label, title);

    list = lv_table_create(box);
    lv_obj_set_width(list, width - 20);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(list, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(list, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(list, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(list, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(list, 8, LV_PART_ITEMS);
    /* our own highlight, not LVGL's edited-cell frame */
    lv_obj_set_style_border_width(list, 0, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_border_width(list, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(list, 0, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(list, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(list, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(list, 0, LV_PART_MAIN | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_table_set_col_cnt(list, 1);
    lv_table_set_col_width(list, 0, width - 20);
    lv_table_set_row_cnt(list, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        lv_table_set_cell_value(list, (uint16_t)i, 0, items[i]);
    }
    ((lv_table_t *)list)->row_act = 0;
    ((lv_table_t *)list)->col_act = 0;
    lv_obj_add_event_cb(list, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(list, draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(list, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* No taller than the window, nor over the button row when the list
     * opens on the main screen: the rest scrolls. */
    lv_obj_update_layout(box);
    avail = lv_obj_get_height(parent) - 20;
    if (parent == lv_scr_act()) {
        avail -= BTN_HEIGHT;
        y_ofs = -BTN_HEIGHT / 2;
    }
    if (lv_obj_get_height(box) > avail) {
        lv_obj_set_height(list, lv_obj_get_height(list) - (lv_obj_get_height(box) - avail));
    }
    lv_obj_align(box, LV_ALIGN_CENTER, 0, y_ofs);

    if (back != NULL) {
        lv_group_remove_obj(back);
    } else {
        prev_focus = lv_group_get_focused(keyboard_group);
        prev_editing = lv_group_get_editing(keyboard_group);
    }
    lv_group_add_obj(keyboard_group, list);
    lv_group_focus_obj(list);
    lv_group_set_editing(keyboard_group, true);
}
