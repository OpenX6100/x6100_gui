/*
 * Xiegu X6100 Channel List manager
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/*
 * The CHANNELS window can run in two ways.
 *
 * DIALOG mode - no other dialog is open (main screen, RTTY panel).
 *   The window is a normal R1CBU dialog: dialog_construct() /
 *   dialog_destruct() handle the soft keys and MSG_DIALOG_START/STOP.
 *
 * OVERLAY mode - another dialog is open (WEFAX, SSTV, FT8, NAVTEX, ...).
 *   dialog.c keeps a single current dialog, so a second dialog_construct()
 *   would orphan the running one: after closing CHANNELS its Close key does
 *   nothing, its destruct_cb never runs and MSG_DIALOG_STOP gives the MFK
 *   back to the spectrum.  In OVERLAY mode CHANNELS therefore bypasses
 *   dialog.c completely: it saves the soft-key page and restores it on
 *   close, sends no MSG_DIALOG_* message and never touches the running
 *   dialog.
 *
 * In both modes the window lives on lv_layer_top(), above everything else
 * (WEFAX SMALL panel included), and takes the input devices with a private
 * LVGL group: the encoder, the VOL knob and a USB keyboard talk only to
 * CHANNELS, while the focus inside keyboard_group (the running app) is left
 * untouched and is therefore intact after closing.
 *
 * Scrolling: MFK moves one row (fine), the VOL knob COARSE_STEP rows
 * (coarse).  While the window is open the VOL knob does not change the
 * volume.
 *
 * Selection and scrolling are managed here, not by lv_table.  The table
 * has exactly visible_rows rows (no partially visible row, no LVGL
 * scrolling) and shows the channels first_row .. first_row+visible_rows-1:
 * scrolling only changes first_row.  The selected row always keeps
 * SCROLL_MARGIN rows of context, in both directions.
 */

#include "dialog_channels.h"
#include "lock_manager.h"
#include "msg.h"
#include "channels.h"
#include "keyboard.h"
#include "events.h"
#include "voice.h"
#include "pubsub_ids.h"
#include "textarea_window.h"
#include "cfg/cfg_api.h"
#include "styles.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>


#define ROW_NONE                (-1)

#define LIST_W                  700     /* list area (background panel)  */
#define LIST_H                  270
#define LIST_Y                  50
#define TABLE_W                 (LIST_W - 12)   /* room for the scroll bar */
#define SCROLLBAR_W             4
#define SCROLL_MARGIN           1       /* rows kept visible beyond the selection */

/* Column layout, relative to the cell (pixels). */
#define COL_NUM_W               44      /* channel number, right aligned  */
#define COL_NAME_X              64      /* name starts here               */
#define COL_FREQ_X1             335     /* frequency: x2-335 .. x2-110    */
#define COL_FREQ_X2             110
#define COL_MODE_X1             100     /* mode:      x2-100 .. x2-12     */
#define COL_MODE_X2             12
#define COL_NAME_GAP            12      /* free space before frequency    */

#define COLOR_TABLE_BG          0x27313a
#define COLOR_TEXT              0xffffff
#define COLOR_TEXT_MUTED        0x9aa6b0
#define COLOR_SEL_BG            0x4a6a86
#define COLOR_DRAG_BG           0x9a6a1e
#define COLOR_ACTIVE_MARK       0xeac345

#define COARSE_STEP             10      /* VOL knob, PgUp/PgDn            */
#define PAGE_GUARD_MS           100


typedef enum {
    WIN_CLOSED = 0,
    WIN_DIALOG,
    WIN_OVERLAY
} win_mode_t;

typedef enum {
    SCROLL_ENSURE,      /* minimal scroll, keeping SCROLL_MARGIN rows of context */
    SCROLL_CENTER       /* row approximately in the middle                       */
} scroll_t;


static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void close_cb(button_data_t *data);
static void add_cb(button_data_t *data);
static void delete_cb(button_data_t *data);
static void edit_cb(button_data_t *data);
static void drag_cb(button_data_t *data);
static void drop_cb(button_data_t *data);
static bool edit_ok_cb(void);
static bool edit_cancel_cb(void);


static win_mode_t win_mode = WIN_CLOSED;

static lv_obj_t *table = NULL;
static lv_obj_t *scroll_track = NULL;
static lv_obj_t *scroll_thumb = NULL;

/* Window on the database shown by the table. */
static int16_t visible_rows = 1;
static int16_t first_row = 0;
static lv_coord_t row_h = 1;
static lv_coord_t list_top = 0;     /* y of the first row in the list area */
static lv_obj_t *count_label = NULL;
static lv_obj_t *empty_label = NULL;

/* Private input group: only the window itself is in it. */
static lv_group_t *popup_group = NULL;

/* keyboard_group state saved on open, restored on close. */
static lv_obj_t *saved_kb_focus = NULL;
static bool saved_kb_editing = false;

/* OVERLAY mode: soft-key page of the running app, and the page guard. */
static buttons_page_t *overlay_prev_page = NULL;
static lv_timer_t *page_guard_timer = NULL;
static bool msg_subscribed = false;

/* Selected row (index in the channel database). */
static int16_t sel_row = ROW_NONE;

/*
 * Last channel actually recalled (from the window or with the microphone
 * UP/DOWN keys).  RAM only: it survives closing the window, not a restart.
 */
static int16_t last_recalled_row = ROW_NONE;

/* Name editing. */
static bool edit_active = false;
static int16_t edit_row = ROW_NONE;

/* Drag & Drop. */
static bool drag_active = false;

/* Add a new channel immediately after the window is opened. */

/* Deferred recall + close (scheduled from the CLICKED event). */
static bool recall_pending = false;


/*
 * Soft keys
 */
static button_data_t btn_close = {
    .type = BTN_TEXT,
    .label = "Close",
    .press = close_cb
};

static button_data_t btn_add = {
    .type = BTN_TEXT,
    .label = "Add",
    .press = add_cb
};

static button_data_t btn_delete = {
    .type = BTN_TEXT,
    .label = "Delete",
    .press = delete_cb
};

static button_data_t btn_edit = {
    .type = BTN_TEXT,
    .label = "Edit",
    .press = edit_cb
};

static button_data_t btn_drag = {
    .type = BTN_TEXT,
    .label = "Drag",
    .press = drag_cb
};

/* F5 while dragging */
static button_data_t btn_drop = {
    .type = BTN_TEXT,
    .label = "Drop",
    .press = drop_cb
};

static buttons_page_t btn_page = {
    .items = {
        &btn_close,
        &btn_add,
        &btn_delete,
        &btn_edit,
        &btn_drag
    }
};


static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .key_cb = NULL,
    .btn_page = &btn_page
};

dialog_t *dialog_channels = &dialog;


/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool is_open(void) {
    return win_mode != WIN_CLOSED && dialog.obj != NULL;
}

static bool row_valid(int16_t row) {
    return row >= 0 && row < (int16_t)channels_count();
}

static const char *mode_to_string(x6100_mode_t mode) {
    switch (mode) {
        case x6100_mode_lsb:        return "LSB";
        case x6100_mode_usb:        return "USB";
        case x6100_mode_lsb_dig:    return "LSB-D";
        case x6100_mode_usb_dig:    return "USB-D";
        case x6100_mode_cw:         return "CW";
        case x6100_mode_cwr:        return "CWR";
        case x6100_mode_am:         return "AM";
        case x6100_mode_nfm:        return "NFM";
        default:                    return "?";
    }
}

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


/* ------------------------------------------------------------------ */
/* Input routing                                                       */
/* ------------------------------------------------------------------ */

/*
 * Route the encoder, the VOL knob and a USB keyboard to the window.
 * keyboard_group (and the object focused in it) is not modified, apart
 * from editing mode: hkey.c sends the microphone keys (V/M, UP, DOWN) as
 * EVENT_HKEY only while keyboard_group is in editing mode.
 */
static void input_attach(void) {
    if (!popup_group) {
        popup_group = lv_group_create();
    }

    lv_group_remove_all_objs(popup_group);
    lv_group_add_obj(popup_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(popup_group, true);

    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (indev->group == keyboard_group) {
            lv_indev_set_group(indev, popup_group);
        }
        indev = lv_indev_get_next(indev);
    }

    lv_group_set_editing(keyboard_group, true);
}

/* Give the input devices back to keyboard_group. */
static void input_detach(void) {
    if (!popup_group) {
        return;
    }

    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (indev->group == popup_group) {
            lv_indev_set_group(indev, keyboard_group);
        }
        indev = lv_indev_get_next(indev);
    }

    lv_group_remove_all_objs(popup_group);
}

static void save_kb_state(void) {
    saved_kb_focus = lv_group_get_focused(keyboard_group);
    saved_kb_editing = lv_group_get_editing(keyboard_group);
}

static void restore_kb_state(void) {
    if (saved_kb_focus &&
        lv_obj_is_valid(saved_kb_focus) &&
        lv_obj_get_group(saved_kb_focus) == keyboard_group) {

        lv_group_focus_obj(saved_kb_focus);
    }
    lv_group_set_editing(keyboard_group, saved_kb_editing);
    saved_kb_focus = NULL;
}


/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

static int16_t table_rows(void) {
    int16_t count = (int16_t)channels_count();
    return count < visible_rows ? count : visible_rows;
}

static void update_scrollbar(void) {
    if (!scroll_track || !scroll_thumb) {
        return;
    }

    int16_t count = (int16_t)channels_count();

    if (count <= visible_rows) {
        lv_obj_add_flag(scroll_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scroll_thumb, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_coord_t track_h = visible_rows * row_h;
    lv_coord_t thumb_h = (lv_coord_t)((int32_t)track_h * visible_rows / count);
    if (thumb_h < 16) {
        thumb_h = 16;
    }
    lv_coord_t y = (lv_coord_t)((int32_t)(track_h - thumb_h) * first_row / (count - visible_rows));

    lv_obj_clear_flag(scroll_track, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(scroll_thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(scroll_thumb, thumb_h);
    lv_obj_set_y(scroll_thumb, list_top + y);
}

/* Move the window so that the selected row is visible. */
static void scroll_to_row(int16_t row, scroll_t how) {
    int16_t count = (int16_t)channels_count();
    int16_t max_first = count > visible_rows ? count - visible_rows : 0;

    if (row_valid(row)) {
        if (how == SCROLL_CENTER) {
            first_row = row - visible_rows / 2;
        } else {
            /* margin cannot exceed what fits around the selected row */
            int16_t margin = SCROLL_MARGIN;
            if (margin > (visible_rows - 1) / 2) {
                margin = (visible_rows - 1) / 2;
            }
            if (row < first_row + margin) {
                first_row = row - margin;
            } else if (row > first_row + visible_rows - 1 - margin) {
                first_row = row - (visible_rows - 1 - margin);
            }
        }
    }

    if (first_row > max_first) {
        first_row = max_first;
    }
    if (first_row < 0) {
        first_row = 0;
    }
    update_scrollbar();
}

static void set_selection(int16_t row, scroll_t how) {
    if (!table) {
        return;
    }

    sel_row = row_valid(row) ? row : (channels_count() > 0 ? 0 : ROW_NONE);
    scroll_to_row(sel_row, how);
    lv_obj_invalidate(table);
}

/*
 * Sync the table with the database: one blank cell per channel, the
 * visible contents are drawn by channel_draw_cb().
 */
static void refresh_table(void) {
    if (!table) {
        return;
    }

    uint16_t count = channels_count();
    uint16_t rows = (uint16_t)table_rows();

    lv_table_set_row_cnt(table, rows);
    for (uint16_t i = 0; i < rows; i++) {
        lv_table_set_cell_value(table, i, 0, " ");
    }

    if (count_label) {
        lv_label_set_text_fmt(count_label, "%u / %u", (unsigned)count, (unsigned)CHANNELS_MAX);
    }
    if (empty_label) {
        if (count == 0) {
            lv_obj_clear_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!row_valid(sel_row)) {
        sel_row = (count > 0) ? (int16_t)count - 1 : ROW_NONE;
    }
    scroll_to_row(sel_row, SCROLL_ENSURE);
    lv_obj_invalidate(table);
}

/*
 * Draw one row:  number | name | frequency | mode
 * Selected row: coloured bar (amber while dragging).
 * Last recalled channel: small yellow mark on the left edge.
 */
static void channel_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (!dsc || dsc->part != LV_PART_ITEMS) {
        return;
    }

    lv_obj_t *obj = lv_event_get_target(e);
    uint32_t col_count = lv_table_get_col_cnt(obj);

    if (col_count == 0) {
        return;
    }

    int16_t row = first_row + (int16_t)(dsc->id / col_count);
    const channel_t *channel = row_valid(row) ? channels_get((uint16_t)row) : NULL;

    if (!channel) {
        return;
    }

    const lv_area_t *cell = dsc->draw_area;
    bool selected = (row == sel_row);

    if (selected) {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_color = lv_color_hex(drag_active ? COLOR_DRAG_BG : COLOR_SEL_BG);
        rect.bg_opa = LV_OPA_COVER;
        rect.radius = 6;

        lv_area_t a = *cell;
        a.x1 += 2;
        a.x2 -= 2;
        a.y1 += 1;
        a.y2 -= 1;
        lv_draw_rect(dsc->draw_ctx, &rect, &a);
    }

    if (row == last_recalled_row) {
        lv_draw_rect_dsc_t mark;
        lv_draw_rect_dsc_init(&mark);
        mark.bg_color = lv_color_hex(COLOR_ACTIVE_MARK);
        mark.bg_opa = LV_OPA_COVER;
        mark.radius = 2;

        lv_area_t a;
        a.x1 = cell->x1 + 4;
        a.x2 = cell->x1 + 7;
        a.y1 = cell->y1 + 7;
        a.y2 = cell->y2 - 7;
        lv_draw_rect(dsc->draw_ctx, &mark, &a);
    }

    lv_draw_label_dsc_t label = *dsc->label_dsc;
    label.opa = LV_OPA_COVER;

    const lv_coord_t pad_top = lv_obj_get_style_pad_top(obj, LV_PART_ITEMS);
    const lv_coord_t pad_bottom = lv_obj_get_style_pad_bottom(obj, LV_PART_ITEMS);
    const lv_coord_t pad_left = lv_obj_get_style_pad_left(obj, LV_PART_ITEMS);
    const lv_color_t text = lv_color_hex(COLOR_TEXT);
    const lv_color_t muted = selected ? text : lv_color_hex(COLOR_TEXT_MUTED);

    lv_area_t area;
    area.y1 = cell->y1 + pad_top;
    area.y2 = cell->y2 - pad_bottom;

    /* Number */
    char num[8];
    snprintf(num, sizeof(num), "%d", row + 1);

    area.x1 = cell->x1 + pad_left;
    area.x2 = area.x1 + COL_NUM_W;
    label.align = LV_TEXT_ALIGN_RIGHT;
    label.color = muted;
    lv_draw_label(dsc->draw_ctx, &label, &area, num, NULL);

    /* Name */
    char name_buf[CHANNEL_NAME_MAX + 4];
    area.x1 = cell->x1 + pad_left + COL_NAME_X;
    area.x2 = cell->x2 - COL_FREQ_X1 - COL_NAME_GAP;

    const char *name = channel->name[0] ? channel->name : "-";
    name = fit_text(label.font, name, area.x2 - area.x1, name_buf, sizeof(name_buf));

    label.align = LV_TEXT_ALIGN_LEFT;
    label.color = channel->name[0] ? text : muted;
    lv_draw_label(dsc->draw_ctx, &label, &area, name, NULL);

    /* Frequency */
    char freq[24];
    snprintf(freq, sizeof(freq), "%u.%03u.%03u",
             (unsigned)(channel->freq / 1000000),
             (unsigned)((channel->freq / 1000) % 1000),
             (unsigned)(channel->freq % 1000));

    area.x1 = cell->x2 - COL_FREQ_X1;
    area.x2 = cell->x2 - COL_FREQ_X2;
    label.align = LV_TEXT_ALIGN_RIGHT;
    label.color = text;
    lv_draw_label(dsc->draw_ctx, &label, &area, freq, NULL);

    /* Mode */
    area.x1 = cell->x2 - COL_MODE_X1;
    area.x2 = cell->x2 - COL_MODE_X2;
    label.color = muted;
    lv_draw_label(dsc->draw_ctx, &label, &area, mode_to_string(channel->mode), NULL);
}


/* ------------------------------------------------------------------ */
/* Recall                                                              */
/* ------------------------------------------------------------------ */

/*
 * Tune the radio to a channel.  This happens whatever the running app is
 * doing (WEFAX/SSTV reception included).
 */
static bool recall_row(int16_t row) {
    if (!row_valid(row)) {
        return false;
    }

    const channel_t *channel = channels_get((uint16_t)row);

    if (!channel) {
        return false;
    }

    /* A channel sets frequency and mode: not while either is locked
     * (the microphone keys already check the frequency lock). */
    if (lm_get_freq() || lm_get_mode()) {
        msg_update_text_fmt("Frequency locked");
        voice_say_text_fmt("Frequency locked");
        return false;
    }

    last_recalled_row = row;

    cparam_i_set(cfg.cur.fg_freq(), channel->freq);
    cparam_i_set(cfg.cur.mode(), channel->mode);
    cparam_i_set(cfg.cur.agc(), channel->agc);
    cparam_i_set(cfg.cur.pre(), channel->pre ? x6100_pre_on : x6100_pre_off);
    cparam_i_set(cfg.cur.att(), channel->att ? x6100_att_on : x6100_att_off);

    if (channel->name[0] != '\0') {
        voice_say_text_fmt("Channel %s", channel->name);
    } else {
        voice_say_text_fmt("Channel");
    }
    return true;
}


/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

static void close_window(void);

/*
 * End a Drag: the order changed in RAM is written to channels.json.
 * If saving fails the last valid database is reloaded, so RAM and file
 * stay consistent.
 */
static bool commit_drag(void) {
    if (!drag_active) {
        return true;
    }

    drag_active = false;
    btn_page.items[4] = &btn_drag;

    if (!channels_save()) {
        channels_load();
        last_recalled_row = ROW_NONE;
        voice_say_text_fmt("Unable to save channel order");
        return false;
    }
    return true;
}

static void move_selection(int16_t delta);
static void move_selection_coarse(int16_t delta);

static void key_cb(lv_event_t *e) {
    if (edit_active) {
        return;
    }

    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            move_selection(+1);
            break;

        case LV_KEY_LEFT:
        case LV_KEY_UP:
            move_selection(-1);
            break;

        /* Coarse: VOL knob and PgUp/PgDn */
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
        case KEYBOARD_PGDN:
            move_selection_coarse(+COARSE_STEP);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
        case KEYBOARD_PGUP:
            move_selection_coarse(-COARSE_STEP);
            break;

        case LV_KEY_HOME:
            if (!drag_active) {
                set_selection(0, SCROLL_ENSURE);
            }
            break;

        case LV_KEY_END:
            if (!drag_active) {
                set_selection((int16_t)channels_count() - 1, SCROLL_ENSURE);
            }
            break;

        case LV_KEY_ESC:
            close_window();
            break;

        default:
            break;
    }
}

/*
 * Recall and close only after the click has been fully processed by LVGL,
 * so that no part of it (release) reaches the app below.
 */
static void recall_and_close_async(void *data) {
    (void)data;

    if (!recall_pending) {
        return;
    }
    recall_pending = false;

    if (!is_open() || drag_active || edit_active) {
        return;
    }

    if (recall_row(sel_row)) {
        close_window();
    }
}

static void clicked_cb(lv_event_t *e) {
    (void)e;

    if (drag_active || edit_active || recall_pending || !row_valid(sel_row)) {
        return;
    }
    recall_pending = true;
    lv_async_call(recall_and_close_async, NULL);
}

/*
 * Number of rows that fit completely in the list area: the table gets
 * exactly that many rows and is centred vertically, the scroll bar has
 * the same height as the rows.
 */
static void apply_row_geometry(lv_coord_t height) {
    row_h = height > 0 ? height : 1;
    visible_rows = LIST_H / row_h;
    if (visible_rows < 1) {
        visible_rows = 1;
    }

    list_top = (LIST_H - visible_rows * row_h) / 2;

    lv_coord_t x = LIST_W - SCROLLBAR_W - 4;

    lv_obj_set_pos(table, 0, list_top);
    lv_obj_set_size(scroll_track, SCROLLBAR_W, visible_rows * row_h);
    lv_obj_set_pos(scroll_track, x, list_top);
    lv_obj_set_pos(scroll_thumb, x, list_top);
}

static void build_window(void) {
    dialog.obj = dialog_init(lv_layer_top());

    /* Lowest object of the top layer: above every app window, below the
     * short messages (msg/msg_tiny), e.g. the name shown by mic UP/DOWN. */
    lv_obj_move_background(dialog.obj);

    /* Native standard dialog frame, shared with FT8/SSTV and WEFAX BIG/FULL. */
    lv_obj_set_size(dialog.obj, 795, 350);
    lv_obj_center(dialog.obj);
    lv_obj_set_style_translate_y(dialog.obj, 4, 0);

    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(dialog.obj);
    lv_label_set_text(title, "CHANNELS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    count_label = lv_label_create(dialog.obj);
    lv_obj_set_style_text_font(count_label, &sony_20, 0);
    lv_obj_set_style_text_color(count_label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(count_label, LV_ALIGN_TOP_RIGHT, -(795 - LIST_W) / 2, 16);

    lv_obj_t *list = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(list);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, LIST_Y);
    lv_obj_set_style_bg_color(list, lv_color_hex(COLOR_TABLE_BG), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);

    /* Table: drawing only, it never receives input and never scrolls. */
    table = lv_table_create(list);
    lv_group_remove_obj(table);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(table, TABLE_W, LV_SIZE_CONTENT);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, TABLE_W);

    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_24, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 12, LV_PART_ITEMS);

    lv_obj_add_event_cb(table, channel_draw_cb, LV_EVENT_DRAW_PART_END, NULL);

    /* Scroll position indicator, shown when the list is longer than the window. */
    scroll_track = lv_obj_create(list);
    lv_obj_remove_style_all(scroll_track);
    lv_obj_clear_flag(scroll_track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(scroll_track, lv_color_hex(0x3a4752), 0);
    lv_obj_set_style_bg_opa(scroll_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scroll_track, 2, 0);

    scroll_thumb = lv_obj_create(list);
    lv_obj_remove_style_all(scroll_thumb);
    lv_obj_clear_flag(scroll_thumb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(scroll_thumb, SCROLLBAR_W, 16);
    lv_obj_set_style_bg_color(scroll_thumb, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_bg_opa(scroll_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scroll_thumb, 2, 0);

    empty_label = lv_label_create(dialog.obj);
    lv_obj_set_style_text_color(empty_label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(empty_label, "No channels\nPress Add to store the current frequency");
    lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 20);

    /*
     * Row height: estimate from the font, then measure the real table and
     * redo the layout if LVGL uses a different height, so that the window
     * always holds a whole number of rows.
     */
    first_row = 0;
    apply_row_geometry(lv_font_get_line_height(&sony_24) + 5 + 5);   /* pad top + bottom */
    refresh_table();

    int16_t rows = table_rows();
    if (rows > 0) {
        lv_obj_update_layout(table);
        lv_coord_t measured = (lv_obj_get_height(table) + rows / 2) / rows;
        if (measured > 0 && measured != row_h) {
            apply_row_geometry(measured);
            refresh_table();
        }
    }
}

/* Common part of opening, after the window is in place (both modes). */
static void open_common(void) {
    edit_active = false;
    edit_row = ROW_NONE;
    drag_active = false;
    recall_pending = false;
    btn_page.items[4] = &btn_drag;

    channels_load();

    save_kb_state();
    build_window();
    input_attach();

    set_selection(row_valid(last_recalled_row) ? last_recalled_row : 0, SCROLL_CENTER);
}

/* Reset everything that belongs to an open window (both modes). */
static void close_common(void) {
    if (edit_active) {
        edit_active = false;
        edit_row = ROW_NONE;
        textarea_window_close();
        if (dialog.obj) {
            lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    commit_drag();
    input_detach();

    recall_pending = false;
    table = NULL;
    scroll_track = NULL;
    scroll_thumb = NULL;
    count_label = NULL;
    empty_label = NULL;
    btn_page.items[4] = &btn_drag;
}


/* DIALOG mode: called by dialog_construct() / dialog_destruct(). */
static void construct_cb(lv_obj_t *parent) {
    (void)parent;

    win_mode = WIN_DIALOG;
    open_common();
}

static void destruct_cb(void) {
    close_common();
    restore_kb_state();
    win_mode = WIN_CLOSED;
    /* dialog_destruct() deletes dialog.obj and restores the soft keys. */
}


/*
 * OVERLAY mode.
 *
 * Page guard: an app may reload its own soft-key page from a timer while
 * CHANNELS is open (e.g. SSTV when the receiver state changes).  The newly
 * loaded page becomes the page to restore, and CHANNELS takes the keys back.
 */
static void page_guard_cb(lv_timer_t *t) {
    (void)t;

    if (win_mode != WIN_OVERLAY || edit_active) {
        return;
    }

    buttons_page_t *cur = buttons_get_cur_page();

    if (cur != &btn_page) {
        if (cur) {
            overlay_prev_page = cur;
        }
        buttons_load_page(&btn_page);
    }
}

static void overlay_close(bool restore) {
    if (win_mode != WIN_OVERLAY) {
        return;
    }

    if (page_guard_timer) {
        lv_timer_del(page_guard_timer);
        page_guard_timer = NULL;
    }

    close_common();

    if (dialog.obj) {
        lv_obj_del(dialog.obj);
        dialog.obj = NULL;
    }
    win_mode = WIN_CLOSED;

    if (restore) {
        buttons_unload_page();
        if (overlay_prev_page) {
            buttons_load_page(overlay_prev_page);
        }
        restore_kb_state();
    } else {
        saved_kb_focus = NULL;
    }
    overlay_prev_page = NULL;
}

/*
 * The app below was closed (MSG_DIALOG_STOP, e.g. GEN/APP keys) or a new
 * dialog is starting: close CHANNELS too.  After a STOP dialog.c has
 * already restored the soft keys, so they must not be touched.
 */
static void dialog_msg_cb(void *s, lv_msg_t *m) {
    (void)s;

    if (win_mode != WIN_OVERLAY) {
        return;
    }
    overlay_close(m->id != MSG_DIALOG_STOP);
}

static void overlay_open(void) {
    if (!msg_subscribed) {
        lv_msg_subscribe(MSG_DIALOG_START, dialog_msg_cb, NULL);
        lv_msg_subscribe(MSG_DIALOG_STOP, dialog_msg_cb, NULL);
        msg_subscribed = true;
    }

    overlay_prev_page = buttons_get_cur_page();
    buttons_unload_page();
    buttons_load_page(&btn_page);

    win_mode = WIN_OVERLAY;
    open_common();

    page_guard_timer = lv_timer_create(page_guard_cb, PAGE_GUARD_MS, NULL);
}


static void close_window(void) {
    if (win_mode == WIN_DIALOG) {
        if (dialog_type_is_run(&dialog)) {
            dialog_destruct();
        }
    } else if (win_mode == WIN_OVERLAY) {
        overlay_close(true);
    }
}


/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

/*
 * Normal mode: wrap-around (last -> first, first -> last).
 * Drag mode: the selected channel moves; it stops at the first/last place.
 */
static void move_selection(int16_t delta) {
    int16_t count = (int16_t)channels_count();

    if (!table || count <= 0) {
        return;
    }
    if (!row_valid(sel_row)) {
        set_selection(0, SCROLL_ENSURE);
        return;
    }

    if (!drag_active) {
        int16_t row = (int16_t)((sel_row + delta) % count);
        if (row < 0) {
            row += count;
        }
        set_selection(row, SCROLL_ENSURE);
        return;
    }

    int16_t from = sel_row;
    int16_t to = from + delta;

    if (to < 0) {
        to = 0;
    }
    if (to >= count) {
        to = count - 1;
    }
    if (to == from || !channels_move((uint16_t)from, (uint16_t)to)) {
        return;
    }

    /* Keep last_recalled_row attached to the same channel. */
    if (last_recalled_row != ROW_NONE) {
        if (last_recalled_row == from) {
            last_recalled_row = to;
        } else if (from < to && last_recalled_row > from && last_recalled_row <= to) {
            last_recalled_row--;
        } else if (from > to && last_recalled_row >= to && last_recalled_row < from) {
            last_recalled_row++;
        }
    }

    set_selection(to, SCROLL_ENSURE);
}


/*
 * Coarse step (VOL knob): jump |delta| rows, stopping at the first/last
 * channel; one more step from there wraps to the other end.  In Drag mode
 * the channel moves by |delta| places and stops at the ends.
 */
static void move_selection_coarse(int16_t delta) {
    int16_t count = (int16_t)channels_count();

    if (!table || count <= 0) {
        return;
    }
    if (!row_valid(sel_row) || drag_active) {
        move_selection(delta);
        return;
    }

    int16_t row = sel_row + delta;

    if (row >= count) {
        row = (sel_row == count - 1) ? 0 : count - 1;
    } else if (row < 0) {
        row = (sel_row == 0) ? count - 1 : 0;
    }
    set_selection(row, SCROLL_ENSURE);
}


/* ------------------------------------------------------------------ */
/* Soft keys                                                           */
/* ------------------------------------------------------------------ */

/* F1 - Close */
static void close_cb(button_data_t *data) {
    (void)data;

    if (edit_active) {
        return;
    }
    close_window();
}

/* F2 - Add the current radio settings at the end of the list */
static void add_cb(button_data_t *data) {
    (void)data;

    if (drag_active || edit_active) {
        return;
    }

    if (channels_count() >= CHANNELS_MAX) {
        voice_say_text_fmt("Channel list full");
        return;
    }

    if (!channels_add_current()) {
        voice_say_text_fmt("Unable to add channel");
        return;
    }

    refresh_table();
    set_selection((int16_t)channels_count() - 1, SCROLL_ENSURE);
    voice_say_text_fmt("Channel added");
}

/* F3 - Delete */
static void delete_cb(button_data_t *data) {
    (void)data;

    if (drag_active || edit_active || !row_valid(sel_row)) {
        return;
    }

    int16_t row = sel_row;

    if (!channels_delete((uint16_t)row)) {
        voice_say_text_fmt("Unable to delete channel");
        return;
    }

    if (last_recalled_row == row) {
        last_recalled_row = ROW_NONE;
    } else if (last_recalled_row > row) {
        last_recalled_row--;
    }

    refresh_table();
    set_selection(row > 0 ? row - 1 : 0, SCROLL_ENSURE);
    voice_say_text_fmt("Channel deleted");
}

/*
 * F4 - Edit the name with the standard R1CBU text window.
 *
 * textarea_window lives on the screen, below lv_layer_top(), and uses
 * keyboard_group: while editing, the CHANNELS window is hidden and the
 * input devices go back to keyboard_group with the editor focused.
 */
static lv_obj_t *find_onscreen_keyboard(void) {
    lv_obj_t *scr = lv_scr_act();
    uint32_t n = lv_obj_get_child_cnt(scr);

    for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
        if (lv_obj_check_type(child, &lv_keyboard_class)) {
            return child;
        }
    }
    return NULL;
}

static void edit_cb(button_data_t *data) {
    (void)data;

    if (drag_active || edit_active || !row_valid(sel_row)) {
        return;
    }

    const channel_t *channel = channels_get((uint16_t)sel_row);

    if (!channel) {
        return;
    }

    edit_active = true;
    edit_row = sel_row;

    lv_obj_add_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
    input_detach();

    textarea_window_open(edit_ok_cb, edit_cancel_cb);

    lv_obj_t *text = textarea_window_text();
    lv_textarea_set_max_length(text, CHANNEL_NAME_MAX - 1);
    lv_textarea_set_placeholder_text(text, "Channel name");
    textarea_window_set(channel->name);

    /* Without a USB keyboard the encoder drives the on-screen keyboard. */
    lv_obj_t *kb = keyboard_ready() ? NULL : find_onscreen_keyboard();
    lv_group_focus_obj(kb ? kb : text);
    lv_group_set_editing(keyboard_group, true);
}

/* Runs after textarea_window has closed itself. */
static void edit_end_async(void *data) {
    (void)data;

    if (!edit_active) {
        return;
    }

    int16_t row = edit_row;

    edit_active = false;
    edit_row = ROW_NONE;

    if (!is_open()) {
        return;
    }

    if (saved_kb_focus &&
        lv_obj_is_valid(saved_kb_focus) &&
        lv_obj_get_group(saved_kb_focus) == keyboard_group) {

        lv_group_focus_obj(saved_kb_focus);
    }

    lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
    input_attach();
    refresh_table();
    set_selection(row, SCROLL_CENTER);
}

static bool edit_ok_cb(void) {
    if (!edit_active) {
        return true;
    }

    if (row_valid(edit_row) && !channels_set_name((uint16_t)edit_row, textarea_window_get())) {
        voice_say_text_fmt("Unable to save channel");
        return false;
    }

    lv_async_call(edit_end_async, NULL);
    voice_say_text_fmt("Channel saved");
    return true;
}

static bool edit_cancel_cb(void) {
    lv_async_call(edit_end_async, NULL);
    return true;
}

/* F5 - Drag: rotate the MFK to move the selected channel */
static void drag_cb(button_data_t *data) {
    (void)data;

    if (drag_active || edit_active || !row_valid(sel_row)) {
        return;
    }

    drag_active = true;
    btn_page.items[4] = &btn_drop;
    buttons_load(4, &btn_drop);
    lv_obj_invalidate(table);
    voice_say_text_fmt("Drag channel");
}

/* F5 - Drop: save the new order */
static void drop_cb(button_data_t *data) {
    (void)data;

    if (!drag_active) {
        return;
    }

    bool saved = commit_drag();

    buttons_load(4, &btn_drag);
    refresh_table();
    set_selection(sel_row, SCROLL_ENSURE);

    if (saved) {
        voice_say_text_fmt("Channel moved");
    }
}


/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void dialog_channels_toggle(lv_obj_t *parent, bool other_dialog_running) {
    if (win_mode != WIN_CLOSED) {
        if (!edit_active) {
            close_window();
        }
        return;
    }

    if (other_dialog_running) {
        overlay_open();
    } else {
        dialog_construct(&dialog, parent);
    }
}

bool dialog_channels_empty(void) {
    return channels_count() == 0;
}

bool dialog_channels_is_open(void) {
    return win_mode != WIN_CLOSED;
}

bool dialog_channels_busy(void) {
    return win_mode != WIN_CLOSED && (drag_active || edit_active);
}

void dialog_channels_show_added(void) {
    if (!is_open() || edit_active) {
        return;
    }
    refresh_table();
    set_selection((int16_t)channels_count() - 1, SCROLL_ENSURE);
}

const char *dialog_channels_last_recalled_name(void) {
    static char fallback_name[32];

    if (!row_valid(last_recalled_row)) {
        return "";
    }

    const channel_t *channel = channels_get((uint16_t)last_recalled_row);

    if (!channel) {
        return "";
    }
    if (channel->name[0] != '\0') {
        return channel->name;
    }

    snprintf(fallback_name, sizeof(fallback_name), "CHANNEL %d", last_recalled_row + 1);
    return fallback_name;
}

/*
 * Microphone UP/DOWN: recall the previous/next channel, with wrap-around.
 * If no channel has been recalled yet, UP starts from the last channel and
 * DOWN from the first.  If the window is open, the selection follows.
 */
bool dialog_channels_recall_relative(int8_t direction) {
    if (direction == 0) {
        return false;
    }

    if (is_open()) {
        /* The open window already holds the database; never reload it
         * here, a Drag in progress would be lost. */
        if (drag_active || edit_active) {
            return false;
        }
    } else if (!channels_load()) {
        /* Reload: works before the window has ever been opened and sees
         * manual changes to channels.json. */
        return false;
    }

    int16_t count = (int16_t)channels_count();

    if (count <= 0) {
        /* No channels: the main screen tunes a step, as without this app */
        return false;
    }

    int16_t row;

    if (!row_valid(last_recalled_row)) {
        row = (direction < 0) ? count - 1 : 0;
    } else {
        row = last_recalled_row + (direction < 0 ? -1 : 1);
        if (row < 0) {
            row = count - 1;
        } else if (row >= count) {
            row = 0;
        }
    }

    if (!recall_row(row)) {
        return false;
    }

    if (is_open()) {
        set_selection(row, SCROLL_CENTER);
    }
    return true;
}
