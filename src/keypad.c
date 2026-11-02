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
#include <fcntl.h>
#include <linux/input.h>

#include "keypad.h"
#include "main.h"
#include "display.h"
#include "keyboard.h"

#define KEYPAD_LONG_TIME 1000

static event_keypad_t   event;
static lv_timer_t       *timer = NULL;
static uint32_t         long_time = KEYPAD_LONG_TIME;

void keypad_set_long_time(uint32_t ms) {
    long_time = ms ? ms : KEYPAD_LONG_TIME;
}

static void keypad_timer(lv_timer_t *t) {
    event.state = KEYPAD_LONG;
    lv_event_send(lv_scr_act(), EVENT_KEYPAD, (void*) &event);
    timer = NULL;
}

static void keypad_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    struct input_event  in;
    keypad_t            *keypad = (keypad_t*) drv->user_data;

    if (read(keypad->fd, &in, sizeof(struct input_event)) > 0) {
        if (in.type == EV_KEY) {
            display_tick();

            switch (in.code) {
                /* Rotary VOL */

                case BTN_TRIGGER_HAPPY21:
                    data->key = LV_KEY_ESC;
                    data->state = (in.value) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
                    keypad->evdev_key = data->key;
                    keypad->evdev_state = data->state;
                    return;

                /* Rotary MFK */

                case BTN_TRIGGER_HAPPY27:
                    mfk->pressed = (in.value != 0);
                    return;

                /* Front side */

                case KEY_POWER:
                    event.key = KEYPAD_POWER;
                    break;

                case BTN_TRIGGER_HAPPY1:
                    event.key = KEYPAD_GEN;
                    break;

                case BTN_TRIGGER_HAPPY7:
                    event.key = KEYPAD_APP;
                    break;

                case BTN_TRIGGER_HAPPY2:
                    event.key = KEYPAD_KEY;
                    break;

                case BTN_TRIGGER_HAPPY8:
                    event.key = KEYPAD_MSG;
                    break;

                case BTN_TRIGGER_HAPPY3:
                    event.key = KEYPAD_DFN;
                    break;

                case BTN_TRIGGER_HAPPY9:
                    event.key = KEYPAD_DFL;
                    break;

                case BTN_TRIGGER_HAPPY13:
                    event.key = KEYPAD_F1;
                    break;

                case BTN_TRIGGER_HAPPY14:
                    event.key = KEYPAD_F2;
                    break;

                case BTN_TRIGGER_HAPPY15:
                    event.key = KEYPAD_F3;
                    break;

                case BTN_TRIGGER_HAPPY19:
                    event.key = KEYPAD_F4;
                    break;

                case BTN_TRIGGER_HAPPY20:
                    event.key = KEYPAD_F5;
                    break;

                case BTN_TRIGGER_HAPPY25:
                    event.key = KEYPAD_LOCK;
                    break;

                /* Top side */

                case BTN_TRIGGER_HAPPY4:
                    event.key = KEYPAD_PTT;
                    break;

                case BTN_TRIGGER_HAPPY5:
                    event.key = KEYPAD_BAND_DOWN;
                    break;

                case BTN_TRIGGER_HAPPY6:
                    event.key = KEYPAD_BAND_UP;
                    break;

                case BTN_TRIGGER_HAPPY10:
                    event.key = KEYPAD_MODE_AM;
                    break;

                case BTN_TRIGGER_HAPPY11:
                    event.key = KEYPAD_MODE_CW;
                    break;

                case BTN_TRIGGER_HAPPY12:
                    event.key = KEYPAD_MODE_SSB;
                    break;

                case BTN_TRIGGER_HAPPY16:
                    event.key = KEYPAD_AB;
                    break;

                case BTN_TRIGGER_HAPPY17:
                    event.key = KEYPAD_PRE;
                    break;

                case BTN_TRIGGER_HAPPY18:
                    event.key = KEYPAD_ATU;
                    break;

                case BTN_TRIGGER_HAPPY22:
                    event.key = KEYPAD_VM;
                    break;

                case BTN_TRIGGER_HAPPY23:
                    event.key = KEYPAD_AGC;
                    break;

                case BTN_TRIGGER_HAPPY24:
                    event.key = KEYPAD_FST;
                    break;

                default:
                    event.key = KEYPAD_UNKNOWN;
                    LV_LOG_WARN("Unknown key");
                    break;
            }

            if (timer) {
                lv_timer_del(timer);
                timer = NULL;
            }

            if (in.value == 1) {
                event.state = KEYPAD_PRESS;
                lv_event_send(lv_scr_act(), EVENT_KEYPAD, (void*) &event);

                timer = lv_timer_create(keypad_timer, long_time, NULL);
                lv_timer_set_repeat_count(timer, 1);
            } else {
                if (event.state == KEYPAD_PRESS) {
                    event.state = KEYPAD_RELEASE;
                    lv_event_send(lv_scr_act(), EVENT_KEYPAD, (void*) &event);
                } else if (event.state == KEYPAD_LONG) {
                    event.state = KEYPAD_LONG_RELEASE;
                    lv_event_send(lv_scr_act(), EVENT_KEYPAD, (void*) &event);
                }
            }
        }
    }

    data->key = keypad->evdev_key;
    data->state = keypad->evdev_state;
}

keypad_t * keypad_init(char *dev_name) {
    int fd = open(dev_name, O_RDWR | O_NOCTTY | O_NDELAY);

    if (fd == -1) {
        perror("unable to open keypad interface:");

        return NULL;
    }

    fcntl(fd, F_SETFL, O_ASYNC | O_NONBLOCK);

    keypad_t *keypad = malloc(sizeof(keypad_t));

    keypad->fd = fd;

    lv_indev_drv_init(&keypad->indev_drv);

    keypad->indev_drv.type = LV_INDEV_TYPE_KEYPAD;
    keypad->indev_drv.read_cb = keypad_input_read;
    keypad->indev_drv.user_data = keypad;
    keypad->indev_drv.long_press_time = 1000;

    keypad->indev = lv_indev_drv_register(&keypad->indev_drv);


    lv_indev_set_group(keypad->indev, keyboard_group);
    keypad->evdev_state = LV_INDEV_STATE_RELEASED;

    return keypad;
}
