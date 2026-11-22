/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#include "radio.h"
#include "dialog.h"
#include "dialog_msg_cw.h"
#include "styles.h"
#include "cfg/cfg_api.h"
#include "events.h"
#include "util.h"
#include "panel.h"
#include "keyboard.h"
#include "textarea_window.h"
#include "msg_cw_store.h"
#include "cw_encoder.h"
#include "msg.h"
#include "buttons.h"
#include "main_screen.h"
#include "lock_manager.h"
#include "pubsub_ids.h"

static uint32_t         *ids = NULL;

static lv_obj_t         *table;
static int16_t          table_rows = 0;


static void init();
static void construct_cb(lv_obj_t *parent);
static void destruct_cb();
static void key_cb(lv_event_t * e);
static void send_stop_cb(button_data_t *btn_data);
static void beacon_stop_cb(button_data_t *btn_data);

static void dialog_msg_cw_send_cb(button_data_t *btn_data);
static void dialog_msg_cw_beacon_cb(button_data_t *btn_data);
static void dialog_msg_cw_period_cb(button_data_t *btn_data);

static void dialog_msg_cw_new_cb(button_data_t *btn_data);
static void dialog_msg_cw_edit_cb(button_data_t *btn_data);
static void dialog_msg_cw_delete_cb(button_data_t *btn_data);

static button_data_t btn_send_stop = {
    .type  = BTN_TEXT,
    .label = "Send\nStop",
    .press = send_stop_cb,
};
static button_data_t btn_beacon_stop = {
    .type  = BTN_TEXT,
    .label = "Beacon\nStop",
    .press = beacon_stop_cb,
};

static button_data_t btn_msg_p1 = {
    .type  = BTN_TEXT,
    .label = "(MSG 1:2)",
    .press = button_next_page_cb,
};
static button_data_t btn_send = {
    .type  = BTN_TEXT,
    .label = "Send",
    .press = dialog_msg_cw_send_cb,
};
static button_data_t btn_beacon = {
    .type  = BTN_TEXT,
    .label = "Beacon",
    .press = dialog_msg_cw_beacon_cb,
};
static button_data_t btn_beacon_period = {
    .type  = BTN_TEXT,
    .label = "Beacon\nPeriod",
    .press = dialog_msg_cw_period_cb,
};
buttons_page_t buttons_page_msg_cw_1 = {
    {
     &btn_msg_p1,
     &btn_send,
     &btn_beacon,
     &btn_beacon_period,
     }
};

static button_data_t btn_msg_p2 = {
    .type  = BTN_TEXT,
    .label = "(MSG 2:2)",
    .press = button_next_page_cb,
};
static button_data_t btn_new = {
    .type  = BTN_TEXT,
    .label = "New",
    .press = dialog_msg_cw_new_cb,
};
static button_data_t btn_edit = {
    .type  = BTN_TEXT,
    .label = "Edit",
    .press = dialog_msg_cw_edit_cb,
};
static button_data_t btn_delete = {
    .type  = BTN_TEXT,
    .label = "Delete",
    .press = dialog_msg_cw_delete_cb,
};

buttons_page_t buttons_page_msg_cw_2 = {
    {
     &btn_msg_p2,
     &btn_new,
     &btn_edit,
     &btn_delete,
     }
};

buttons_group_t group_msg_cw = {
    &buttons_page_msg_cw_1,
    &buttons_page_msg_cw_2,
};

static dialog_t             dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .btn_page = &buttons_page_msg_cw_1,
    .key_cb = NULL
};

dialog_t                    *dialog_msg_cw = &dialog;

static void reset() {
    free(ids);

    ids = NULL;
    table_rows = 0;
    lv_table_set_row_cnt(table, 1);
}

static void tx_cb(void * s, lv_msg_t * msg) {
    if (cw_encoder_state() == CW_ENCODER_BEACON_IDLE) {
        cw_encoder_stop();
        buttons_unload_page();
        buttons_load_page(&buttons_page_msg_cw_1);
    }
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    buttons_page_msg_cw_1.items[0]->next = &buttons_page_msg_cw_2;
    buttons_page_msg_cw_1.items[0]->prev = &buttons_page_msg_cw_2;
    buttons_page_msg_cw_2.items[0]->next = &buttons_page_msg_cw_1;
    buttons_page_msg_cw_2.items[0]->prev = &buttons_page_msg_cw_1;

    lv_msg_subscribe(MSG_RADIO_TX, tx_cb, NULL);

    table = lv_table_create(dialog.obj);

    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);

    lv_obj_set_size(table, 775, 325);

    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, 770);

    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);

    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_add_style(table, &style.text_base_color, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 0, LV_PART_ITEMS);

    lv_obj_set_style_text_color(table, lv_color_black(), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_color(table, lv_color_white(), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, 128, LV_PART_ITEMS | LV_STATE_EDITED);

    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    lv_obj_center(table);

    table_rows = 0;
    ids = NULL;

    msg_cw_store_load(dialog_msg_cw_append);
    lm_set_mode(true);
}

static void destruct_cb() {
    if (!ids) {
        free(ids);
    }

    cw_encoder_stop();
    textarea_window_close();
    lm_set_mode(false);
}

static void key_cb(lv_event_t * e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            /* The RFG knob, as JS8: the first stops TX, the next closes. */
            if (cw_encoder_state() != CW_ENCODER_IDLE) {
                send_stop_cb(NULL);
            } else {
                dialog_destruct(&dialog);
            }
            break;

        case KEYBOARD_F4:
            dialog_msg_cw_edit_cb(NULL);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static bool textarea_window_close_cb() {
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);
    return true;
}

static bool textarea_window_new_ok_cb() {
    const char *val = textarea_window_get();
    uint32_t    id  = msg_cw_store_new(val);

    dialog_msg_cw_append(id, val);
    return textarea_window_close_cb();
}

static bool textarea_window_edit_ok_cb() {
    const char *val = textarea_window_get();
    int16_t     row = 0;
    int16_t     col = 0;

    lv_table_get_selected_cell(table, &row, &col);
    lv_table_set_cell_value(table, row, col, val);
    msg_cw_store_edit(ids[row], val);
    return textarea_window_close_cb();
}

static const char* get_msg() {
    if (table_rows == 0) {
        return NULL;
    }

    int16_t     row = 0;
    int16_t     col = 0;

    lv_table_get_selected_cell(table, &row, &col);

    if (row == LV_TABLE_CELL_NONE) {
        return NULL;
    }

    return lv_table_get_cell_value(table, row, col);
}

void dialog_msg_cw_append(uint32_t id, const char *val) {
    ids = realloc(ids, sizeof(uint32_t) * (table_rows + 1));

    ids[table_rows] = id;
    lv_table_set_cell_value(table, table_rows, 0, val);

    table_rows++;
}

void dialog_msg_cw_send_cb(button_data_t *btn_data) {
    const char *msg = get_msg();

    cw_encoder_send(msg, false);
    buttons_unload_page();
    buttons_load(1, &btn_send_stop);
}

static void send_stop_cb(button_data_t *btn_data) {
    cw_encoder_stop();
    buttons_unload_page();
    buttons_load_page(&buttons_page_msg_cw_1);
}

void dialog_msg_cw_beacon_cb(button_data_t *btn_data) {
    const char *msg = get_msg();

    cw_encoder_send(msg, true);
    buttons_unload_page();
    buttons_load(2, &btn_beacon_stop);
}

static void beacon_stop_cb(button_data_t *btn_data) {
    cw_encoder_stop();
    buttons_unload_page();
    buttons_load_page(&buttons_page_msg_cw_1);
}

void dialog_msg_cw_period_cb(button_data_t *btn_data) {
    int32_t period;

    switch (param_i_get(cfg.cw.encoder_period())) {
        case 10:  period = 30;  break;
        case 30:  period = 60;  break;
        case 60:  period = 120; break;
        case 120: period = 10;  break;
        default:  period = 10;  break;
    }

    param_i_set(cfg.cw.encoder_period(), period);
    msg_update_text_fmt("Beacon period: %i s", param_i_get(cfg.cw.encoder_period()));
}

void dialog_msg_cw_new_cb(button_data_t *btn_data) {
    lv_group_remove_obj(table);
    textarea_window_open(textarea_window_new_ok_cb, textarea_window_close_cb);
}

void dialog_msg_cw_edit_cb(button_data_t *btn_data) {
    const char *msg = get_msg();

    if (msg) {
        lv_group_remove_obj(table);
        textarea_window_open(textarea_window_edit_ok_cb, textarea_window_close_cb);
        textarea_window_set(msg);
    }
}

void dialog_msg_cw_delete_cb(button_data_t *btn_data) {
    if (table_rows == 0) {
        return;
    }

    int16_t     row = 0;
    int16_t     col = 0;

    lv_table_get_selected_cell(table, &row, &col);

    if (row != LV_TABLE_CELL_NONE) {
        msg_cw_store_delete(ids[row]);
        reset();
        msg_cw_store_load(dialog_msg_cw_append);
    }
}
