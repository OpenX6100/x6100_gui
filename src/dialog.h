/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "lvgl/lvgl.h"
#include <liquid/liquid.h>

#include "helpers.h"
#include "buttons.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef void (*dialog_construct_cb_t)(lv_obj_t *);
typedef void (*dialog_destruct_cb_t)(void);
typedef void (*dialog_rotary_cb_t)(int32_t diff, uint16_t dt);

typedef struct {
    lv_obj_t                *obj;
    dialog_construct_cb_t   construct_cb;
    dialog_destruct_cb_t    destruct_cb;
    dialog_rotary_cb_t      rotary_cb;
    buttons_page_t          *btn_page;
    buttons_page_t          *prev_page;
    lv_event_cb_t           key_cb;
    bool                    run;
} dialog_t;


void dialog_construct(dialog_t *dialog, lv_obj_t *parent);
void dialog_destruct();

bool dialog_key(dialog_t *dialog, lv_event_t * e);
void dialog_send(lv_event_code_t event_code, void *param);
bool dialog_is_run() __attribute__((deprecated("Use subscription on MSG_DIALOG_START and MSG_DIALOG_STOP")));
bool dialog_type_is_run(dialog_t *dialog);

lv_obj_t * dialog_init(lv_obj_t *parent);
void dialog_item(dialog_t *dialog, lv_obj_t *obj);

void dialog_rotary(int32_t diff, uint16_t dt);

#ifdef __cplusplus
}
#endif
