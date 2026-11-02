/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sndfile.h>
#include <dirent.h>
#include <pthread.h>

#include <aether_radio/x6100_control/control.h>

#include "radio.h"
#include "audio.h"
#include "bt_audio.h"
#include "dialog.h"
#include "dialog_msg_voice.h"
#include "dsp.h"
#include "styles.h"
#include "cfg/cfg_api.h"
#include "events.h"
#include "util.h"
#include "panel.h"
#include "keyboard.h"
#include "textarea_window.h"
#include "msg.h"
#include "meter.h"
#include "scheduler.h"
#include "diag_log.h"

#define BUF_SIZE 1024

typedef enum {
    VOICE_BEACON_OFF = 0,
    VOICE_BEACON_PLAY,
    VOICE_BEACON_IDLE,
} voice_beacon_t;

static msg_voice_state_t    state = MSG_VOICE_OFF;
static uint32_t             dsp_audio_sub_id = AUDIO_SUB_INVALID;
static voice_beacon_t       beacon = VOICE_BEACON_OFF;
static char                 *path = "/mnt/msg";

static lv_obj_t             *table;
static int16_t              table_rows = 0;
static SNDFILE              *file = NULL;

static char                 *prev_filename;
static pthread_t            thread;
static int16_t              samples_buf[BUF_SIZE];

static void construct_cb(lv_obj_t *parent);
static void destruct_cb();
static void key_cb(lv_event_t * e);
static void rec_stop_cb(button_data_t *btn_data);
static void play_stop_cb(button_data_t *btn_data);
static void send_stop_cb(button_data_t *btn_data);
static void beacon_stop_cb(button_data_t *btn_data);
static void refresh_buttons_on_done(void *page);

static void dialog_msg_voice_send_cb(button_data_t *btn_data);
static void dialog_msg_voice_beacon_cb(button_data_t *btn_data);
static void dialog_msg_voice_period_cb(button_data_t *btn_data);

static void dialog_msg_voice_rec_cb(button_data_t *btn_data);
static void dialog_msg_voice_play_cb(button_data_t *btn_data);
static void dialog_msg_voice_rename_cb(button_data_t *btn_data);
static void dialog_msg_voice_delete_cb(button_data_t *btn_data);

static button_data_t btn_rec_stop = {
    .type  = BTN_TEXT,
    .label = "Rec\nStop",
    .press = rec_stop_cb,
};
static button_data_t btn_play_stop = {
    .type  = BTN_TEXT,
    .label = "Play\nStop",
    .press = play_stop_cb,
};
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

/* Msg Voice */

static button_data_t btn_msg_p1 = {
    .type  = BTN_TEXT,
    .label = "(MSG 1:2)",
    .press = button_next_page_cb,
};
static button_data_t btn_send = {
    .type  = BTN_TEXT,
    .label = "Send",
    .press = dialog_msg_voice_send_cb,
};
static button_data_t btn_beacon = {
    .type  = BTN_TEXT,
    .label = "Beacon",
    .press = dialog_msg_voice_beacon_cb,
};
static button_data_t btn_beacon_period = {
    .type  = BTN_TEXT,
    .label = "Beacon\nPeriod",
    .press = dialog_msg_voice_period_cb,
};

static buttons_page_t page_msg_voice_1 = {
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
static button_data_t btn_rec = {
    .type  = BTN_TEXT,
    .label = "Rec",
    .press = dialog_msg_voice_rec_cb,
};
static button_data_t btn_rename = {
    .type  = BTN_TEXT,
    .label = "Rename",
    .press = dialog_msg_voice_rename_cb,
};
static button_data_t btn_delete = {
    .type  = BTN_TEXT,
    .label = "Delete",
    .press = dialog_msg_voice_delete_cb,
};
static button_data_t btn_play = {
    .type  = BTN_TEXT,
    .label = "Play",
    .press = dialog_msg_voice_play_cb,
};

static buttons_page_t page_msg_voice_2 = {
    {
     &btn_msg_p2,
     &btn_rec,
     &btn_rename,
     &btn_delete,
     &btn_play,
     }
};

static buttons_group_t group_msg_voice = {
    &page_msg_voice_1,
    &page_msg_voice_2,
};

static dialog_t             dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .btn_page = &page_msg_voice_1,
    .key_cb = NULL
};

dialog_t                    *dialog_msg_voice = &dialog;

static void load_table() {
    table_rows = 0;

    DIR             *dp;
    struct dirent   *ep;

    dp = opendir(path);

    if (dp != NULL) {
        while ((ep = readdir(dp)) != NULL) {
            if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) {
                continue;
            }

            lv_table_set_cell_value(table, table_rows++, 0, ep->d_name);
        }

        closedir(dp);
    }
    if (table_rows > 0) {
        lv_table_set_row_cnt(table, table_rows);
    } else {
        lv_table_set_cell_value(table, table_rows++, 0, "");
        lv_table_set_row_cnt(table, 1);
    }
}

static bool create_file() {
    SF_INFO sfinfo;

    memset(&sfinfo, 0, sizeof(sfinfo));

    sfinfo.samplerate = AUDIO_CAPTURE_RATE;
    sfinfo.channels = 1;
    sfinfo.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;

    char        filename[64];
    time_t      now = time(NULL);
    struct tm   *t = localtime(&now);

    snprintf(filename, sizeof(filename),
        "%s/MSG_%04i%02i%02i_%02i%02i%02i.mp3",
        path, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec
    );

    file = sf_open(filename, SFM_WRITE, &sfinfo);

    if (file == NULL) {
        const char* err = sf_strerror(NULL);
        LV_LOG_ERROR("Problem with create file: %s", err);
        return false;
    }

    return true;
}

static void close_file() {
    if (file) {
        sf_close(file);
        file = NULL;
    }
}

static const char* get_item() {
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

/* bt_sink: play the preview on the Bluetooth headphones (listening on),
 * NULL for the codec - always NULL when transmitting. */
static void play_item(const char *bt_sink) {
    const char *item = get_item();

    if (!item) {
        return;
    }

    char filename[64];

    strcpy(filename, path);
    strcat(filename, "/");
    strcat(filename, item);

    SF_INFO sfinfo;

    memset(&sfinfo, 0, sizeof(sfinfo));

    SNDFILE *file = sf_open(filename, SFM_READ, &sfinfo);

    if (!file) {
        return;
    }
    audio_player_t *player = NULL;

    if (bt_sink) {
        player = audio_create_player_on(bt_sink, sfinfo.samplerate, sfinfo.channels);
    }
    if (!player) {
        player = audio_get_player(sfinfo.samplerate, sfinfo.channels);
    }

    state = MSG_VOICE_PLAY;
    while (state == MSG_VOICE_PLAY) {
        int res = sf_read_short(file, samples_buf, BUF_SIZE);

        if (res > 0) {
            audio_player_send(player, samples_buf, res);
        } else {
            state = MSG_VOICE_OFF;
        }
    }

    sf_close(file);
    audio_player_wait(player);
    audio_player_release(player);
}

static void * play_thread(void *arg) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    char bt_sink[96];
    bool bt = bt_audio_playback_begin(bt_sink, sizeof(bt_sink));

    if (bt) {
        play_item(bt_sink);
        bt_audio_playback_end();
    } else {
        audio_set_play_mode(AUDIO_PLAY_ON);
        play_item(NULL);
        audio_set_play_mode(AUDIO_PLAY_OFF);
    }

    void *page_ptr = &page_msg_voice_2;
    scheduler_put(refresh_buttons_on_done, &page_ptr, sizeof(&page_ptr));
}

static void * send_thread(void *arg) {
    diag_set_source("voice message");
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    msg_update_text_fmt("Sending message");

    radio_set_modem(true);
    play_item(NULL);
    radio_set_modem(false);

    void *page_ptr = &page_msg_voice_1;
    scheduler_put(refresh_buttons_on_done, &page_ptr, sizeof(&page_ptr));
}

static void * beacon_thread(void *arg) {
    diag_set_source("voice beacon");
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (true) {
        switch (beacon) {
            case VOICE_BEACON_OFF:
                void *page_ptr = &page_msg_voice_1;
                scheduler_put(refresh_buttons_on_done, &page_ptr, sizeof(&page_ptr));
                return NULL;

            case VOICE_BEACON_PLAY:
                msg_update_text_fmt("Sending message");
                radio_set_modem(true);
                play_item(NULL);
                radio_set_modem(false);
                break;

            case VOICE_BEACON_IDLE:
                msg_update_text_fmt("Beacon pause: %i s", param_i_get(cfg.voice.msg_period()));
                sleep(param_i_get(cfg.voice.msg_period()));
                break;
        }

        switch (beacon) {
            case VOICE_BEACON_PLAY:
                beacon = VOICE_BEACON_IDLE;
                break;

            case VOICE_BEACON_IDLE:
                beacon = VOICE_BEACON_PLAY;
                break;
        }
    }
    return NULL;
}

static bool textarea_window_close_cb() {
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    free(prev_filename);
    prev_filename = NULL;
    return true;
}

static bool textarea_window_edit_ok_cb() {
    const char *new_filename = textarea_window_get();

    if (strcmp(prev_filename, new_filename) != 0) {
        char prev[64];
        char new[64];

        snprintf(prev, sizeof(prev), "%s/%s", path, prev_filename);
        snprintf(new, sizeof(new), "%s/%s", path, new_filename);

        if (rename(prev, new) == 0) {
            load_table();
            textarea_window_close_cb();
        }
    } else {
        free(prev_filename);
        prev_filename = NULL;
    }
    return true;
}

static void tx_cb(void * s, lv_msg_t * msg) {
    if (beacon == VOICE_BEACON_IDLE) {
        pthread_cancel(thread);
        pthread_join(thread, NULL);
        beacon = VOICE_BEACON_OFF;

        buttons_unload_page();
        buttons_load_page(&page_msg_voice_1);
    }
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    page_msg_voice_1.items[0]->next = &page_msg_voice_2;
    page_msg_voice_1.items[0]->prev = &page_msg_voice_2;
    page_msg_voice_2.items[0]->next = &page_msg_voice_1;
    page_msg_voice_2.items[0]->prev = &page_msg_voice_1;

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

    mkdir(path, 0755);
    load_table();
}

static void destruct_cb() {
    audio_set_play_mode(AUDIO_PLAY_OFF);
    bt_audio_playback_end();

    if (beacon == VOICE_BEACON_IDLE) {
        pthread_cancel(thread);
        pthread_join(thread, NULL);
    }

    if (state == MSG_VOICE_RECORD) {
        dsp_audio_set_active(dsp_audio_sub_id, false);
        close_file();
    }

    meter_set_mode(METER_MODE_S);
    beacon = VOICE_BEACON_OFF;
    state = MSG_VOICE_OFF;
    textarea_window_close();
}

static void key_cb(lv_event_t * e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct(&dialog);
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

void dialog_msg_voice_send_cb(button_data_t *btn_data) {
    if (state == MSG_VOICE_OFF) {
        pthread_create(&thread, NULL, send_thread, NULL);

        buttons_unload_page();
        buttons_load(1, &btn_send_stop);
    }
}

static void send_stop_cb(button_data_t *btn_datae) {
    state = MSG_VOICE_OFF;
}

void dialog_msg_voice_beacon_cb(button_data_t *btn_data) {
    if (state == MSG_VOICE_OFF) {
        if (get_item()) {
            beacon = VOICE_BEACON_PLAY;
            pthread_create(&thread, NULL, beacon_thread, NULL);

            buttons_unload_page();
            buttons_load(2, &btn_beacon_stop);
        }
    }
}

static void beacon_stop_cb(button_data_t *btn_data) {
    switch (state) {
        case MSG_VOICE_OFF:
            pthread_cancel(thread);
            pthread_join(thread, NULL);
            beacon = VOICE_BEACON_OFF;

            buttons_unload_page();
            buttons_load_page(&page_msg_voice_1);
            break;

        case MSG_VOICE_PLAY:
            beacon = VOICE_BEACON_OFF;
            state = MSG_VOICE_OFF;
            break;

        default:
            break;
    }
}

void dialog_msg_voice_period_cb(button_data_t *btn_data) {
    int32_t period;

    switch (param_i_get(cfg.voice.msg_period())) {
        case 10:  period = 30;  break;
        case 30:  period = 60;  break;
        case 60:  period = 120; break;
        case 120: period = 10;  break;
        default:  period = 10;  break;
    }

    param_i_set(cfg.voice.msg_period(), period);
    msg_update_text_fmt("Beacon period: %i s", param_i_get(cfg.voice.msg_period()));
}

void dialog_msg_voice_rec_cb(button_data_t *btn_data) {
    if (state == MSG_VOICE_OFF) {
        if (create_file()) {
            audio_set_play_mode(AUDIO_PLAY_VOICE_REC);
            state = MSG_VOICE_RECORD;

            if (dsp_audio_sub_id == AUDIO_SUB_INVALID) {
                dsp_audio_sub_id = dsp_audio_subscribe_raw(dialog_msg_voice_put_audio_samples, true);
            }
            dsp_audio_set_active(dsp_audio_sub_id, true);
            meter_set_mode(METER_MODE_LEVEL);

            buttons_unload_page();
            buttons_load(1, &btn_rec_stop);
        }
    }
}

static void rec_stop_cb(button_data_t *btn_data) {
    buttons_unload_page();
    buttons_load_page(&page_msg_voice_2);

    dsp_audio_set_active(dsp_audio_sub_id, false);
    audio_set_play_mode(AUDIO_PLAY_OFF);
    meter_set_mode(METER_MODE_S);
    state = MSG_VOICE_OFF;
    close_file();
    load_table();
}

void dialog_msg_voice_play_cb(button_data_t *btn_data) {
    if (state == MSG_VOICE_OFF) {
        pthread_create(&thread, NULL, play_thread, NULL);

        buttons_unload_page();
        buttons_load(4, &btn_play_stop);
    }
}

void play_stop_cb(button_data_t *btn_data) {
    state = MSG_VOICE_OFF;
}

void dialog_msg_voice_rename_cb(button_data_t *btn_data) {
    prev_filename = strdup(get_item());

    if (prev_filename) {
        lv_group_remove_obj(table);
        textarea_window_open(textarea_window_edit_ok_cb, textarea_window_close_cb);
        textarea_window_set(prev_filename);
    }
}

void dialog_msg_voice_delete_cb(button_data_t *btn_data) {
    const char *name = get_item();

    if (name) {
        char filename[64];

        strcpy(filename, path);
        strcat(filename, "/");
        strcat(filename, name);

        unlink(filename);
        load_table();
    }
}

msg_voice_state_t dialog_msg_voice_get_state() {
    return state;
}

void dialog_msg_voice_put_audio_samples(size_t nsamples, int16_t *samples) {
    sf_write_short(file, samples, nsamples);
}


/**
 * Helper function to refresh buttons from threads to prevent LVGL errors
 */
static void refresh_buttons_on_done(void *page) {
    if (!page) {
        return;
    }

    if (dialog.run) {
        buttons_unload_page();
        buttons_load_page(*(buttons_page_t**)page);
    }
}
