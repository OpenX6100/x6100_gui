/*
 * Xiegu X6100 - shared list widget ("CHANNELS" look)
 *
 * A scrollable list of rows with the same graphics as the CHANNELS window:
 * dark panel, sony_24 font, whole rows only, coloured selection bar, thin
 * scroll bar.  It does not use lv_table: the rows are drawn directly, and
 * selection and scrolling are managed here, so wrap-around and direct row
 * selection are exact.
 *
 * The widget is a normal focusable lv_obj: add it to keyboard_group (with
 * the group in editing mode) and the MFK encoder scrolls it.
 *
 *   MFK / arrows     one row, with wrap-around
 *   VOL knob         cfg.coarse_step rows (only if cfg.vol_coarse)
 *   PgUp / PgDn      cfg.coarse_step rows
 *   Home / End       first / last row
 *   MFK press        cfg.activate_cb(user, selected_row)
 *
 * A selection change made with the keys sends LV_EVENT_VALUE_CHANGED to the
 * list object (not ui_list_select() or ui_list_set_count()).
 *
 * The widget stores no data: the application keeps its own array and gives
 * the row contents through cfg.row_cb, which is called only for the visible
 * rows each time the list is drawn (keep it cheap: no file access).
 *
 * Copyright (C) 2026 Hany El Imam
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#define UI_LIST_MAX_COLS    4
#define UI_LIST_CELL_LEN    48

typedef enum {
    UI_LIST_ALIGN_LEFT = 0,
    UI_LIST_ALIGN_CENTER,
    UI_LIST_ALIGN_RIGHT
} ui_list_align_t;

/*
 * One column.  x is the left edge of the column, in pixels from the left
 * edge of the row (x >= 0) or from the right edge of the row (x < 0, e.g.
 * -335 puts the column 335 px left of the right edge).  Text wider than w
 * is shortened with "..".
 */
typedef struct {
    lv_coord_t      x;
    lv_coord_t      w;
    ui_list_align_t align;
    bool            muted;      /* secondary colour (not for the selected row) */
} ui_list_col_t;

/* Contents of one row, filled by cfg.row_cb (zeroed before the call). */
typedef struct {
    char cell[UI_LIST_MAX_COLS][UI_LIST_CELL_LEN];  /* empty string: nothing */
    bool mark;                                      /* yellow mark on the left edge */
    bool muted;                                     /* whole row grey (unavailable item) */
} ui_list_row_t;

typedef void (*ui_list_row_cb_t)(void *user, int index, ui_list_row_t *row);
typedef void (*ui_list_activate_cb_t)(void *user, int index);

/*
 * Optional extra drawing for a row (e.g. colour swatches), called after the
 * selection bar and the text.  area is the row area (scroll bar excluded).
 */
typedef void (*ui_list_draw_cb_t)(void *user, int index, lv_draw_ctx_t *ctx,
                                  const lv_area_t *area, bool selected);

typedef struct {
    lv_coord_t              width;
    lv_coord_t              height;
    const lv_font_t         *font;          /* NULL: sony_24 */

    uint8_t                 col_count;
    ui_list_col_t           cols[UI_LIST_MAX_COLS];

    ui_list_row_cb_t        row_cb;
    ui_list_activate_cb_t   activate_cb;    /* may be NULL */
    ui_list_draw_cb_t       draw_cb;        /* may be NULL */
    void                    *user;

    const char              *empty_text;    /* shown when there are no rows */

    bool                    activate_on_click;  /* false: on press (default) */
    bool                    vol_coarse;         /* VOL knob scrolls coarsely */
    uint8_t                 coarse_step;        /* 0: 10 rows */
    uint8_t                 row_pad;            /* space above and below the text, 0: 5 px */
} ui_list_cfg_t;

/*
 * Create the list inside parent.  Position it with lv_obj_align().  It is
 * freed by lv_obj_del().  The cfg struct is copied, but empty_text must
 * stay valid (use a string literal).
 */
lv_obj_t *ui_list_create(lv_obj_t *parent, const ui_list_cfg_t *cfg);

/*
 * Set the number of rows.  The selection is kept when still valid, else it
 * moves to the last row (so after a delete the next row, or the previous
 * one if the last was deleted, is selected).
 */
void ui_list_set_count(lv_obj_t *list, int count);

int  ui_list_get_count(lv_obj_t *list);

/* Selected row, or -1 when the list is empty. */
int  ui_list_get_selected(lv_obj_t *list);

/* Select a row and scroll just enough to show it. */
void ui_list_select(lv_obj_t *list, int index);

/* Number of rows that fit completely in the list (for paging code). */
int  ui_list_visible_rows(lv_obj_t *list);

/*
 * Split a saved file name "<prefix>YYYYMMDD-HHMMSS.png" into
 * "YYYY-MM-DD" and "HH:MM:SS".  Returns false if the name does not match.
 */
bool ui_list_parse_stamp(const char *name, const char *prefix,
                         char *date, int date_size,
                         char *time, int time_size);

/* Same for "<prefix>YYYYMMDD-HHMMSS<ext>", e.g. ext ".txt". */
bool ui_list_parse_stamp_ext(const char *name, const char *prefix, const char *ext,
                             char *date, int date_size,
                             char *time, int time_size);
