/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "dialog.h"

#include "styles.h"
#include "main_screen.h"
#include "keyboard.h"
#include "events.h"
#include "waterfall.h"
#include "scheduler.h"

static lv_obj_t     *obj;
static dialog_t     *current_dialog = NULL;

static bool dialog_is_running();

void dialog_construct(dialog_t *dialog, lv_obj_t *parent) {
    if (dialog && !dialog->run) {
        scheduler_msg_send(MSG_DIALOG_START, NULL);
        dialog->prev_page = buttons_get_cur_page();
        buttons_unload_page();
        if (dialog->btn_page) {
            buttons_load_page(dialog->btn_page);
        }
        dialog->construct_cb(parent);

        dialog->run = true;
    }

    current_dialog = dialog;
}

void dialog_destruct() {
    if (current_dialog && current_dialog->run) {
        current_dialog->run = false;

        if (current_dialog->destruct_cb) {
            current_dialog->destruct_cb();
        }

        if (current_dialog->obj) {
            lv_obj_del(current_dialog->obj);
        }
        buttons_unload_page();
        if (current_dialog->prev_page) {
            buttons_load_page(current_dialog->prev_page);
        }
        current_dialog = NULL;
        scheduler_msg_send(MSG_DIALOG_STOP, NULL);
    }
}

void dialog_send(lv_event_code_t event_code, void *param) {
    if (dialog_is_running()) {
        event_send(current_dialog->obj, event_code, param);
    }
}

bool dialog_key(dialog_t *dialog, lv_event_t * e) {
    if (dialog && dialog->key_cb && dialog->run) {
        dialog->key_cb(e);
        return true;
    }

    return false;
}

bool dialog_is_run() {
    return dialog_is_running();
}

bool dialog_type_is_run(dialog_t *dialog) {
    return (current_dialog == dialog) && current_dialog->run;
}

lv_obj_t * dialog_init(lv_obj_t *parent) {
    obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &style.dialog.base, 0);

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    return obj;
}

void dialog_item(dialog_t *dialog, lv_obj_t *obj) {
    lv_obj_add_style(obj, &style.dialog.item, LV_STATE_DEFAULT);
    lv_obj_add_style(obj, &style.dialog.item_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(obj, &style.dialog.item_edited, LV_STATE_EDITED);

    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_add_style(obj, &style.text_base_color, LV_PART_CURSOR);
    lv_obj_set_style_text_color(obj, lv_color_black(), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(obj, 128, LV_PART_CURSOR | LV_STATE_EDITED);

    lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUS_KEY | LV_PART_INDICATOR);
    lv_obj_set_style_border_color(obj, lv_color_white(), LV_STATE_FOCUS_KEY | LV_PART_INDICATOR);

    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    lv_group_add_obj(keyboard_group, obj);

    if (dialog->key_cb) {
        lv_obj_add_event_cb(obj, dialog->key_cb, LV_EVENT_KEY, NULL);
    }
}

void dialog_rotary(int32_t diff, uint16_t dt) {
    if (dialog_is_running() && current_dialog->rotary_cb) {
        current_dialog->rotary_cb(diff, dt);
    }
}


static bool dialog_is_running() {
    return (current_dialog != NULL) && current_dialog->run;
}
