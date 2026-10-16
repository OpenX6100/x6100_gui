/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "dialog_ft8.h"

#include "app_ports.h"
#include "ft8/worker.h"
#include "ft8/qso.h"
#include "ft8/utils.h"
#include "lvgl/lvgl.h"
#include "dialog.h"
#include "styles.h"
#include "cfg/cfg_api.h"
#include "cfg/digital_modes.h"
#include "radio.h"
#include "audio.h"
#include "dsp.h"
#include "keyboard.h"
#include "events.h"
#include "buttons.h"
#include "main_screen.h"
#include "spectrum.h"
#include "waterfall.h"
#include "lock_manager.h"
#include "qth/qth.h"
#include "msg.h"
#include "util.h"
#include "recorder.h"
#include "textarea_window.h"
#include "dsp.h"

#include "ft8/audio_worker.h"
#include "ft8/cq_scheduler.h"
#include "ft8_ui/table_view.h"
#include "ft8/tx_worker.h"
#include "widgets/lv_waterfall.h"
#include "widgets/lv_finder.h"

#include "ft8/worker.h"
#include "adif.h"
#include "ft8_udp.h"
#include "qso_log.h"
#include "dxcc.h"
#include "scheduler.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>

#define WIDTH           771

#define UNKNOWN_SNR     99

#define MAX_PWR         5.0f

#define FT8_WIDTH_HZ    50
#define FT4_WIDTH_HZ    83

#define MAX_TX_START_DELAY 1.5f

typedef enum {
    RX_PROCESS,
    TX_PROCESS,
} ft8_state_t;

/* ft8_cell_type_t and cell_data_t live in ft8_ui/table_view.h */

/* slot_info_t lives in ft8/audio_worker.h */

static ft8_state_t state = RX_PROCESS;
static SubjectInt *tx_enabled;
static SubjectInt *cq_enabled;
static bool        tx_time_slot;

static ftx_tx_msg_t tx_msg;

/* The lv_table widget is owned by ft8_ui/table_view; expose its handle as
 * `table` so existing fade/group/anim call sites need no rename. */
#define table (table_view_obj())

static lv_timer_t *timer = NULL;
static lv_anim_t   fade;
static bool        fade_run        = false;
static bool        disable_buttons = false;

static lv_obj_t *finder;
static lv_obj_t *waterfall;

/* Audio capture, decimation, FFT, decoder thread and stop-flag plumbing
 * all live in ft8/audio_worker.c. The dialog just creates/destroys the
 * worker and exposes a handful of callbacks. */
static audio_worker_t *audio_worker = NULL;

static adif_log         ft8_log;
static FTxQsoProcessor *qso_processor;

static double cur_lat, cur_lon;

static int32_t filter_low, filter_high;

static float base_gain_offset;

static uint32_t dsp_audio_sub_id = AUDIO_SUB_INVALID;

static void construct_cb(lv_obj_t *parent);
static void key_cb(lv_event_t * e);
static void destruct_cb();
static void audio_cb(unsigned int n, float *samples);
static void rotary_cb(int32_t diff, uint16_t dt);

/* audio_worker callbacks (run on worker thread). UI mutations must go
 * through scheduler_put() to land on the LVGL task. */
static void on_message_cb(const char *text, int snr, float freq_hz, float time_sec,
                          const slot_info_t *info, void *ctx);
static void on_psd_cb(const float *psd, uint16_t nfft, float sec_since_slot_start,
                      const slot_info_t *info, void *ctx);
static void on_slot_end_cb(const slot_info_t *info, void *ctx);
static void on_tick_cb(const slot_info_t *info, bool new_slot,
                       float sec_since_slot_start, void *ctx);

static const char * cq_all_label_getter();
static const char * protocol_label_getter();
static const char * tx_cq_label_getter();
static const char * tx_call_label_getter();
static const char * hold_freq_label_getter();
static const char * auto_label_getter();

static void show_cq_all_cb(struct button_data_t *btn_data);
static void mode_ft4_ft8_cb(struct button_data_t *btn_data);
static void tx_cq_en_dis_cb(struct button_data_t *btn_data);
static void tx_call_en_dis_cb(struct button_data_t *btn_data);

static void hold_tx_freq_cb(struct button_data_t *btn_data);
static void mode_auto_cb(struct button_data_t *btn_data);
static void cq_modifier_cb(struct button_data_t *btn_data);
static void time_sync(struct button_data_t *btn_data);

static void force_save_qso(struct button_data_t *btn_data);

static void on_table_press(const cell_data_t *cell_data);
static void on_table_close(void);
static void on_table_vol_change(int32_t delta);

static void keyboard_open();
static bool keyboard_cancel_cb();
static bool keyboard_ok_cb();
static void keyboard_close();

static void add_info(const char * fmt, ...);
static void add_tx_text(const char * text);
static bool get_time_slot(struct timespec now, float *time_since_start);


// button label is current state, press action and name - next state

static buttons_page_t btn_page_1;
static buttons_page_t btn_page_2;
static buttons_page_t btn_page_3;

static button_data_t button_page_1 = { .type=BTN_TEXT, .label = "(Page: 1:3)", .press = button_next_page_cb, .next=&btn_page_2};
static button_data_t button_show_cq_all = { .type=BTN_TEXT_FN, .label_fn = cq_all_label_getter, .press = show_cq_all_cb};
static button_data_t button_mode_ft4_ft8 = { .type=BTN_TEXT_FN, .label_fn = protocol_label_getter, .press = mode_ft4_ft8_cb};
static button_data_t button_tx_cq_en_dis = { .type=BTN_TEXT_FN, .label_fn = tx_cq_label_getter, .press = tx_cq_en_dis_cb };
static button_data_t button_tx_call_en_dis = { .type=BTN_TEXT_FN, .label_fn = tx_call_label_getter, .press = tx_call_en_dis_cb};

static button_data_t button_page_2 = { .type=BTN_TEXT, .label = "(Page: 2:3)", .press = button_next_page_cb, .next=&btn_page_3};
static button_data_t button_hold_freq = { .type=BTN_TEXT_FN, .label_fn = hold_freq_label_getter, .press = hold_tx_freq_cb };
static button_data_t button_auto_en_dis = { .type=BTN_TEXT_FN, .label_fn = auto_label_getter, .press = mode_auto_cb };
static button_data_t button_force_save = { .type=BTN_TEXT, .label = "Force QSO\nsave", .press = force_save_qso };

static button_data_t button_page_3 = { .type=BTN_TEXT, .label = "(Page: 3:3)", .press = button_next_page_cb, .next=&btn_page_1};
static button_data_t button_cq_mod = { .type=BTN_TEXT, .label = "CQ\nModifier", .press = cq_modifier_cb };
static button_data_t button_time_sync = { .type=BTN_TEXT, .label = "Time\nSync", .press = time_sync };

static buttons_page_t btn_page_1 = {
    {&button_page_1, &button_show_cq_all, &button_mode_ft4_ft8, &button_tx_cq_en_dis, &button_tx_call_en_dis}
};

static buttons_page_t btn_page_2 = {
    {&button_page_2, &button_hold_freq, &button_auto_en_dis, &button_force_save}
};

static buttons_page_t btn_page_3 = {
    {&button_page_3, &button_cq_mod, &button_time_sync}
};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb,
};

dialog_t *dialog_ft8 = &dialog;

/* ---- WSJT-X datagrams over UDP (ft8_udp.c) -----------------------------
 *
 * Status goes out whenever something a logger shows has changed - RX/TX,
 * the audio offset, the dial, the station being worked - and with every
 * heartbeat. One 250 ms timer compares and sends, so no existing code
 * path has to call it and turning the finder cannot burst packets.
 * Every ft8_udp_send_* returns at once when UDP is off. */

#define UDP_POLL_MS        250
#define UDP_HEARTBEAT_POLLS 60      /* 15 s, as WSJT-X */

static lv_timer_t *udp_timer = NULL;
static uint32_t    udp_polls = 0;
static int         udp_last_state = -1;
static int32_t     udp_last_df = -1;
static int64_t     udp_last_freq = -1;
static char        udp_last_dx[16];

static const char *udp_mode_str(void) {
    return param_i_get(cfg.ft8.protocol()) == FTX_PROTOCOL_FT8 ? "FT8" : "FT4";
}

/* The QSO partner is not exposed by the qso processor, but the pending TX
 * message always starts with it ("DXCALL MYCALL ..."), except for CQ. */
static void udp_current_dx_call(char *out, size_t out_sz) {
    size_t i = 0;

    out[0] = '\0';
    if (tx_msg.msg[0] == '\0') return;

    while (tx_msg.msg[i] && tx_msg.msg[i] != ' ' && i < out_sz - 1) {
        out[i] = tx_msg.msg[i];
        i++;
    }
    out[i] = '\0';

    if (strcmp(out, "CQ") == 0) out[0] = '\0';
}

static void udp_status_send(void) {
    char dx_call[16];
    char my_call[PARAM_TEXT_MAX];
    char my_grid[PARAM_TEXT_MAX];
    uint32_t df = (uint32_t)param_i_get(cfg.ft8.tx_freq());

    udp_current_dx_call(dx_call, sizeof(dx_call));
    param_t_get_into(cfg.callsign(), my_call, sizeof(my_call));
    param_t_get_into(cfg.qth(), my_grid, sizeof(my_grid));

    ft8_udp_send_status((uint64_t)cparam_i_get(cfg.cur.fg_freq()), udp_mode_str(),
                        my_call, my_grid, dx_call,
                        df,                         /* rx df (audio offset) */
                        df,                         /* tx df                */
                        state == TX_PROCESS,        /* transmitting */
                        state != TX_PROCESS);       /* decoding     */

    udp_last_state = (int)state;
    udp_last_df = (int32_t)df;
    udp_last_freq = (int64_t)cparam_i_get(cfg.cur.fg_freq());
    snprintf(udp_last_dx, sizeof(udp_last_dx), "%s", dx_call);
}

static void udp_timer_cb(lv_timer_t *t) {
    char dx_call[16];

    (void)t;

    if (++udp_polls >= UDP_HEARTBEAT_POLLS) {
        udp_polls = 0;
        ft8_udp_send_heartbeat();
        udp_status_send();
        return;
    }

    udp_current_dx_call(dx_call, sizeof(dx_call));
    if ((int)state != udp_last_state ||
        param_i_get(cfg.ft8.tx_freq()) != udp_last_df ||
        (int64_t)cparam_i_get(cfg.cur.fg_freq()) != udp_last_freq ||
        strcmp(dx_call, udp_last_dx) != 0) {
        udp_status_send();
    }
}

static void save_qso(const char *remote_callsign, const char *remote_grid, const int r_snr, const int s_snr) {
    time_t now = time(NULL);

    char * canonized_call = util_canonize_callsign(remote_callsign, false);
    qso_log_record_t qso = qso_log_record_create(
        PARAM_T_GET(cfg.callsign()),
        canonized_call,
        now, param_i_get(cfg.ft8.protocol()) == FTX_PROTOCOL_FT8 ? MODE_FT8 : MODE_FT4,
        s_snr, r_snr, cparam_i_get(cfg.cur.fg_freq()), NULL, NULL,
        PARAM_T_GET(cfg.qth()), remote_grid
    );
    free(canonized_call);

    adif_add_qso(ft8_log, qso);

    // Save QSO to sqlite log
    qso_log_record_save(qso);

    /* Tell UDP listeners: the structured QSO Logged and the ADIF form, a
     * valid record (band from frequency, empty fields omitted). */
    {
        char my_call[PARAM_TEXT_MAX], my_grid[PARAM_TEXT_MAX];

        param_t_get_into(cfg.callsign(), my_call, sizeof(my_call));
        param_t_get_into(cfg.qth(), my_grid, sizeof(my_grid));
        ft8_udp_log_qso(now, qso.remote_call, remote_grid,
                        (uint64_t)cparam_i_get(cfg.cur.fg_freq()), udp_mode_str(),
                        s_snr, r_snr, (int)param_f_get(cfg.pwr()),
                        my_call, my_grid);
    }

    if (strlen(remote_grid) >= 4) {
        double lat, lon, dist;
        qth_str_to_pos(remote_grid, &lat, &lon);
        dist = qth_pos_dist(lat, lon, cur_lat, cur_lon);
        msg_schedule_long_text_fmt("Saved QSO de %s %d %d (%.0f km)", remote_callsign, s_snr, r_snr, dist);
    } else {
        msg_schedule_long_text_fmt("Saved QSO de %s %d %d", remote_callsign, s_snr, r_snr);
    }

    lv_finder_clear_cursor(finder);
}

static void worker_init() {
    qso_processor = ftx_qso_processor_init(PARAM_T_GET(cfg.callsign()), PARAM_T_GET(cfg.qth()),
                                           save_qso,
                                           param_i_get(cfg.ft8.max_repeats()));

    audio_worker_cb_t cb = {
        .on_message  = on_message_cb,
        .on_psd      = on_psd_cb,
        .on_slot_end = on_slot_end_cb,
        .on_tick     = on_tick_cb,
        .ctx         = NULL,
    };
    audio_worker = audio_worker_create(
        FTX_CAPTURE_RATE,
        param_i_get(cfg.ft8.protocol()),
        filter_low, filter_high,
        &cb);
    if (audio_worker) {
        audio_worker_start(audio_worker);
    }
}

static void worker_done() {
    state = RX_PROCESS;

    if (audio_worker) {
        audio_worker_destroy(audio_worker);
        audio_worker = NULL;
    }
    radio_set_modem(false);

    if (qso_processor) {
        ftx_qso_processor_delete(qso_processor);
        qso_processor = NULL;
    }
    lv_finder_clear_cursor(finder);
    tx_msg.msg[0] = '\0';
}

/* Table widget lifecycle, draw, scroll and message insertion all live
 * in ft8_ui/table_view.c. The dialog only owns the surrounding state. */

static void key_cb(lv_event_t * e) {
    uint32_t key = *((uint32_t *) lv_event_get_param(e));

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

static void destruct_cb() {
    dsp_audio_set_active(dsp_audio_sub_id, false);
    keyboard_close();
    worker_done();

    /* After worker_done(): the decoder thread, which also sends, has
     * stopped, so the socket can go. */
    if (udp_timer) {
        lv_timer_del(udp_timer);
        udp_timer = NULL;
    }
    ft8_udp_deinit();

    table_view_destroy();

    waterfall_set_enabled(true);
    spectrum_set_enabled(true);

    mem_load(MEM_BACKUP_ID);

    lm_set_mode(false);
    lm_set_ab(false);
    lm_set_freq(false);
    lm_set_band(false);

    radio_set_pwr(param_f_get(cfg.pwr()));
    adif_log_close(ft8_log);

    tx_worker_destruct();
}

static void load_band(int8_t dir) {
    cfg_digital_type_t type;
    switch (param_i_get(cfg.ft8.protocol())) {
        case FTX_PROTOCOL_FT8:
            type = CFG_DIG_TYPE_FT8;
            lv_finder_set_width(finder, FT8_WIDTH_HZ);
            break;

        case FTX_PROTOCOL_FT4:
            type = CFG_DIG_TYPE_FT4;
            lv_finder_set_width(finder, FT4_WIDTH_HZ);
            break;
    }
    bool res = cfg_digital_load(dir, type);
    if (res) {
        msg_update_text_fmt("%s", cfg_digital_label_get());
    }
}

/// @brief Clean waterfall and table
static void clean_screen() {
    table_view_reset();
    lv_waterfall_clear_data(waterfall);

    int32_t *c = malloc(sizeof(int32_t));
    *c = LV_KEY_UP;

    lv_event_send(table, LV_EVENT_KEY, c);
}

static void band_cb(lv_event_t * e) {
    int8_t dir;

    if (lv_event_get_code(e) == EVENT_BAND_UP) {
        dir = 1;
    } else {
        dir = -1;
    }

    load_band(dir);

    worker_done();
    worker_init();
    clean_screen();
}

static void msg_timer(lv_timer_t *t) {
    lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0), LV_OPA_COVER);
    lv_anim_start(&fade);
    timer = NULL;
}

static void fade_anim(void * obj, int32_t v) {
    lv_obj_set_style_opa_layered(obj, v, 0);
}

static void fade_ready(lv_anim_t * a) {
    fade_run = false;
}

static void set_freq(uint32_t freq) {
    if (freq > filter_high) {
        freq = filter_high;
    }

    if (freq < filter_low) {
        freq = filter_low;
    }

    param_i_set(cfg.ft8.tx_freq(), freq);

    lv_finder_set_value(finder, freq);
    lv_obj_invalidate(finder);
}

static void rotary_cb(int32_t diff, uint16_t dt) {
    /* Faster turns move further, as before the knob handed over each click
     * on its own (then 4-5 clicks in 30 ms moved 5 Hz each, 6 or more
     * 10 Hz). dt is the time per click in ms, 0 when unknown. */
    if (dt > 0 && dt <= 5) {
        diff *= 10;
    } else if (dt > 0 && dt < 8) {
        diff *= 5;
    }
    uint32_t f = param_i_get(cfg.ft8.tx_freq()) + diff;
    f = limit(f, filter_low, filter_high - (param_i_get(cfg.ft8.protocol()) == FTX_PROTOCOL_FT8 ? FT8_WIDTH_HZ : FT4_WIDTH_HZ));

    set_freq(f);

    if (!fade_run) {
        fade_run = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0), LV_OPA_TRANSP);
        lv_anim_start(&fade);
    }

    if (timer) {
        lv_timer_reset(timer);
    } else {
        timer = lv_timer_create(msg_timer, 1000, NULL);
        lv_timer_set_repeat_count(timer, 1);
    }

}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    /* FT8 dialog owns the full screen + audio pipe; main_screen's spectrum
     * and waterfall are not visible, so skip their DSP cost entirely. The
     * companion lv_obj_invalidate() in tx_info handles the side effect of
     * losing the spectrum redraw that previously refreshed PWR/SWR bars. */
    waterfall_set_enabled(false);
    spectrum_set_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    // Fill subjects for buttons
    button_show_cq_all.subj = (Subject*)cfg.ft8.show_all();
    button_mode_ft4_ft8.subj = (Subject*)cfg.ft8.protocol();
    button_hold_freq.subj = (Subject*)cfg.ft8.hold_freq();
    button_auto_en_dis.subj = (Subject*)cfg.ft8.auto_mode();

    if (!cq_enabled) {
        cq_enabled = subject_i_create(false);
        button_tx_cq_en_dis.subj = (Subject*)cq_enabled;
    } else {
        subject_i_set(cq_enabled, false);
    }
    if (!tx_enabled) {
        tx_enabled = subject_i_create(true);
        button_tx_call_en_dis.subj = (Subject*)tx_enabled;
    }
    buttons_load_page(&btn_page_1);

    /* Audio pipeline (cbuffer/firdecim/spgramcf/decoder thread) is created
     * lazily inside worker_init() -> audio_worker_create(). */

    /* Waterfall */

    waterfall = lv_waterfall_create(dialog.obj);

    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);

    lv_waterfall_set_palette(waterfall, (lv_color_t*)style.wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, 325);
    lv_waterfall_set_min(waterfall, -60);

    lv_obj_set_pos(waterfall, 13, 13);

    /* Freq finder */

    finder = lv_finder_create(waterfall);

    lv_finder_set_width(finder, 50);
    lv_finder_set_value(finder, param_i_get(cfg.ft8.tx_freq()));

    lv_obj_set_size(finder, WIDTH, 325);
    lv_obj_set_pos(finder, 0, 0);

    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(finder, style.colors.mark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    /* Table */

    table_view_build(dialog.obj, 13, 13 + 55, WIDTH, 325 - 55);

    /* Settings > Interface > Decoder text. Compact: 12 rows, sony_28
     * (line height 20) with 1 px padding = 22 px rows in the 270 px
     * table. Large: 8 rows, the dialog font as it is (sony_36 + 3 + 3 =
     * 34 px). Read when the window opens. */
    if (param_i_get(cfg.ui.digi_compact())) {
        lv_obj_set_style_text_font(table, &sony_28, LV_PART_ITEMS);
        lv_obj_set_style_pad_top(table, 1, LV_PART_ITEMS);
        lv_obj_set_style_pad_bottom(table, 1, LV_PART_ITEMS);
    }
    table_view_set_press_cb(on_table_press);
    table_view_actions_t tv_actions = {
        .on_close      = on_table_close,
        .on_vol_change = on_table_vol_change,
    };
    table_view_set_actions(&tv_actions);
    /* Settings > Interface > FT8/FT4 info: distance, country or both. */
    table_view_set_info((table_view_info_t)param_i_get(cfg.ui.ft8_info()));

    /* Fade */

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, table);
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, fade_anim);
    lv_anim_set_ready_cb(&fade, fade_ready);

    /* * */

    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    mem_save(MEM_BACKUP_ID);
    load_band(0);

    filter_low = cparam_i_get(cfg.filter.low());
    filter_high = cparam_i_get(cfg.filter.high());

    lv_finder_set_range(finder, filter_low, filter_high);

    qth_str_to_pos(PARAM_T_GET(cfg.qth()), &cur_lat, &cur_lon);

    lm_set_ab(true);
    lm_set_mode(true);
    lm_set_freq(true);
    lm_set_band(true);

    worker_init();

    if (dsp_audio_sub_id == AUDIO_SUB_INVALID) {
        dsp_audio_sub_id = dsp_audio_subscribe_float(audio_cb, FTX_CAPTURE_RATE);
    }
    dsp_audio_set_active(dsp_audio_sub_id, true);

    /* Logger */
    ft8_log = adif_log_init("/mnt/ft_log.adi");

    /* WSJT-X compatible datagrams over UDP (GridTracker, JTAlert, ...). */
    ft8_udp_init();
    udp_polls = 0;
    udp_last_state = -1;
    ft8_udp_send_heartbeat();
    udp_status_send();
    udp_timer = lv_timer_create(udp_timer_cb, UDP_POLL_MS, NULL);
    if (ft8_udp_is_enabled()) {
        msg_schedule_text_fmt("UDP -> %s:%u", ft8_udp_host(), ft8_udp_port());
    }

    if (param_f_get(cfg.pwr()) > MAX_PWR) {
        radio_set_pwr(MAX_PWR);
        msg_schedule_text_fmt("Power was limited to %0.0fW", MAX_PWR);
    }

    // setup gain offset
    float target_pwr = LV_MIN(param_f_get(cfg.pwr()), MAX_PWR);
    if (x6100_control_get_base_ver().rev >= 3) {
        // patched firmware has a true power control
        base_gain_offset = -9.4f;
    } else {
        base_gain_offset = -16.4f + log10f(target_pwr) * 10.0f;
    }

    // setup tx_worker (with lower sample rate)
    tx_worker_construct(&app_ports, AUDIO_PLAY_RATE / 8);
}

/* Buttons */

const char *cq_all_label_getter() {
    static char buf[32];
    sprintf(buf, "Show:\n%s", param_i_get(cfg.ft8.show_all()) ? "All": "CQ");
    return buf;
}

const char *protocol_label_getter() {
    static char buf[32];
    sprintf(buf, "Mode:\n%s", param_i_get(cfg.ft8.protocol()) == FTX_PROTOCOL_FT8 ? "FT8": "FT4");
    return buf;
}

const char *tx_cq_label_getter() {
    static char buf[32];
    sprintf(buf, "TX CQ:\n%s", subject_i_get(cq_enabled) ? "Enabled": "Disabled");
    return buf;
}

const char *tx_call_label_getter() {
    static char buf[32];
    sprintf(buf, "TX Call:\n%s", subject_i_get(tx_enabled) ? "Enabled": "Disabled");
    return buf;
}

const char *hold_freq_label_getter() {
    static char buf[32];
    sprintf(buf, "Hold Freq:\n%s", param_i_get(cfg.ft8.hold_freq()) ? "Enabled": "Disabled");
    return buf;
}

const char *auto_label_getter() {
    static char buf[32];
    sprintf(buf, "Auto:\n%s", param_i_get(cfg.ft8.auto_mode()) ? "Enabled": "Disabled");
    return buf;
}

static void show_cq_all_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    param_i_set(cfg.ft8.show_all(), !param_i_get(cfg.ft8.show_all()));
}

static void mode_ft4_ft8_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;

    ftx_protocol_t proto = param_i_get(cfg.ft8.protocol());
    if (proto == FTX_PROTOCOL_FT8)
        proto = FTX_PROTOCOL_FT4;
    else {
        proto = FTX_PROTOCOL_FT8;
    }
    param_i_set(cfg.ft8.protocol(), proto);
    subject_i_set(cq_enabled, false);

    worker_done();
    worker_init();
    clean_screen();
    load_band(0);
}

static void mode_auto_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    bool new_val = !param_i_get(cfg.ft8.auto_mode());
    param_i_set(cfg.ft8.auto_mode(), new_val);
    ftx_qso_processor_set_auto(qso_processor, new_val);
}

static void hold_tx_freq_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    param_i_set(cfg.ft8.hold_freq(), !param_i_get(cfg.ft8.hold_freq()));
}

static void tx_cq_en_dis_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;

    if (!subject_i_get(cq_enabled)){
        if (strlen(PARAM_T_GET(cfg.callsign())) == 0) {
            msg_schedule_text_fmt("Call sign required");
            return;
        }
        subject_i_set(cq_enabled, true);
        subject_i_set(tx_enabled, true);

        cq_make_message(PARAM_T_GET(cfg.callsign()), PARAM_T_GET(cfg.qth()), PARAM_T_GET(cfg.ft8.cq_modifier()), tx_msg.msg);

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        float time_since_slot_start;
        tx_time_slot = !get_time_slot(now, &time_since_slot_start);
        if (time_since_slot_start < MAX_TX_START_DELAY) {
            tx_time_slot = !tx_time_slot;
        }

        if (tx_msg.msg[2] == '_') {
            msg_schedule_text_fmt("Next TX: CQ %s", tx_msg.msg + 3);
        } else {
            msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
        }
        tx_msg.repeats = param_i_get(cfg.ft8.max_repeats());
        ftx_qso_processor_reset(qso_processor);
        lv_finder_clear_cursor(finder);
    } else {
        if (state == TX_PROCESS) {
            state = RX_PROCESS;
        }
        subject_i_set(cq_enabled, false);
        tx_msg.msg[0] = '\0';
    }
}

static void tx_call_en_dis_cb(struct button_data_t *btn_data) {
    if (disable_buttons)
        return;

    if (!subject_i_get(tx_enabled)) {
        if (strlen(PARAM_T_GET(cfg.callsign())) == 0) {
            msg_schedule_text_fmt("Call sign required");
            return;
        }
        subject_i_set(tx_enabled, true);
    } else {
        if (state == TX_PROCESS) {
            state = RX_PROCESS;
        }
        subject_i_set(tx_enabled, false);
    }
}

static void tx_call_off() {
    state = RX_PROCESS;
    subject_i_set(tx_enabled, false);
}

static void cq_modifier_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    keyboard_open();
}

static void time_sync(struct button_data_t *btn_data) {
    time_t now = time(NULL);
    uint8_t sec = now % 60;
    float drift, slot_time;
    switch (param_i_get(cfg.ft8.protocol())) {
        case FTX_PROTOCOL_FT4:
            slot_time = FT4_SLOT_TIME;
            break;

        case FTX_PROTOCOL_FT8:
            slot_time = FT8_SLOT_TIME;
            break;
    }
    drift = fmodf(sec + slot_time / 2, slot_time) - slot_time / 2;
    struct timespec tp;

    now -= (int) drift;
    tp.tv_sec = now;
    tp.tv_nsec = 0;

    int res = clock_settime(CLOCK_REALTIME, &tp);
    if (res != 0)
    {
        LV_LOG_ERROR("Can't set system time: %s\n", strerror(errno));
        return;
    }
}

static void force_save_qso(struct button_data_t *btn_data) {
    if (ftx_qso_processor_can_save_qso(qso_processor)) {
        ftx_qso_processor_force_save_qso(qso_processor);
    } else {
        msg_schedule_text_fmt("Can't save incomplete QSO");
    }
}

static void on_table_press(const cell_data_t *cell_data) {
    if (state == TX_PROCESS) {
        tx_call_off();
        return;
    }

    if ((cell_data == NULL) ||
        (cell_data->cell_type == CELL_TX_MSG) ||
        (cell_data->cell_type == CELL_RX_INFO) ||
        (cell_data->cell_type == CELL_START_QSO)
    ) {
        msg_schedule_text_fmt("What should I do about it?");
        return;
    }

    ftx_qso_processor_start_qso(qso_processor, (ftx_msg_meta_t *)&cell_data->meta, &tx_msg);
    if (strlen(tx_msg.msg) > 0) {
        lv_finder_set_cursor(finder, cell_data->meta.freq_hz);
        if (!param_i_get(cfg.ft8.hold_freq())) {
            set_freq(cell_data->meta.freq_hz);
        }
        tx_time_slot = !cell_data->odd;
        subject_i_set(tx_enabled, true);
        {
            cell_data_t cd;
            cd.cell_type = CELL_START_QSO;
            snprintf(cd.text, sizeof(cd.text), "Start QSO with %s", cell_data->meta.call_de);
            scheduler_put(table_view_add_msg_cb, &cd, sizeof(cell_data_t));
        }
        msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
    } else {
        msg_schedule_text_fmt("Invalid message");
        tx_call_off();
    }
}

static void on_table_close(void) {
    dialog_destruct(&dialog);
}

static void on_table_vol_change(int32_t delta) {
    radio_change_vol(delta);
}

static void keyboard_open() {
    lv_group_remove_obj(table);
    textarea_window_open(keyboard_ok_cb, keyboard_cancel_cb);
    lv_obj_t *text = textarea_window_text();

    lv_textarea_set_accepted_chars(text,
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    );

    if (strlen(PARAM_T_GET(cfg.ft8.cq_modifier())) > 0) {
        textarea_window_set(PARAM_T_GET(cfg.ft8.cq_modifier()));
    } else {
        lv_obj_t *text = textarea_window_text();
        lv_textarea_set_placeholder_text(text, " CQ modifier");
    }
    disable_buttons = true;
}

static void keyboard_close() {
    textarea_window_close();
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);
    disable_buttons = false;
}

static bool keyboard_cancel_cb() {
    keyboard_close();
    return true;
}

static bool keyboard_ok_cb() {
    char *cq_mod = (char *)textarea_window_get();
    if ((strlen(cq_mod) > 0) && !is_cq_modifier(cq_mod)) {
        msg_schedule_text_fmt("Unsupported CQ modifier");
        return false;
    }
    param_t_set(cfg.ft8.cq_modifier(), cq_mod);
    keyboard_close();
    return true;
}

static void audio_cb(size_t n, float *samples) {
    if (state == RX_PROCESS) {
        audio_worker_feed(audio_worker, n, samples);
    }
}

static bool get_time_slot(struct timespec now, float *sec_since_start) {
    bool cur_odd;
    float sec = (now.tv_sec % 60) + now.tv_nsec / 1.0e9f;

    switch (param_i_get(cfg.ft8.protocol())) {
    case FTX_PROTOCOL_FT4:
        cur_odd = (int)(sec / FT4_SLOT_TIME) % 2;
        *sec_since_start = fmodf(sec, FT4_SLOT_TIME);
        break;

        case FTX_PROTOCOL_FT8:
        cur_odd = (int)(sec / FT8_SLOT_TIME) % 2;
        *sec_since_start = fmodf(sec, FT8_SLOT_TIME);
        break;
    }
    return cur_odd;
}


/* CQ message composition has moved to ft8/cq_scheduler.c
 * (see cq_make_message()). */

/* TX waveform synthesis + per-block ALC gain correction live in
 * ft8/tx_worker.c. The dialog only owns the per-session abort flag. */
static bool tx_should_abort_cb(void *ctx) {
    (void)ctx;
    return state != TX_PROCESS;
}

/**
 * Add INFO message to the table
 */
static void add_info(const char * fmt, ...) {
    va_list     args;
    cell_data_t  cell_data;
    cell_data.cell_type = CELL_RX_INFO;

    va_start(args, fmt);
    vsnprintf(cell_data.text, sizeof(cell_data.text), fmt, args);
    va_end(args);

    scheduler_put(table_view_add_msg_cb, &cell_data, sizeof(cell_data_t));
}

/**
 * Add TX message to the table
 */
static void add_tx_text(const char * text) {
    cell_data_t  cell_data;
    cell_data.cell_type = CELL_TX_MSG;

    strncpy(cell_data.text, text, sizeof(cell_data.text) - 1);
    if (strncmp(cell_data.text, "CQ_", 3) == 0) {
        cell_data.text[2] = ' ';
    }
    scheduler_put(table_view_add_msg_cb, &cell_data, sizeof(cell_data_t));
}

/**
 * Parse and add RX messages to the table
 */
static void add_rx_text(int16_t snr, const char * text, slot_info_t *s_info, float freq_hz, float time_sec) {

    ftx_msg_meta_t meta;
    /* The parser fills call_de only for the messages it knows. */
    memset(&meta, 0, sizeof(meta));
    meta.freq_hz = freq_hz;
    meta.time_sec = time_sec;
    char * old_msg = strdup(tx_msg.msg);
    ftx_qso_processor_add_rx_text(qso_processor, text, snr, &meta, &tx_msg);

    if ((strlen(tx_msg.msg) > 0) && (strcmp(old_msg, tx_msg.msg) != 0)) {
        lv_finder_set_cursor(finder, meta.freq_hz);
        if (!param_i_get(cfg.ft8.hold_freq())) {
            set_freq(freq_hz);
        }
        tx_time_slot = !s_info->odd;
        msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
        if (subject_i_get(cq_enabled)) {
            subject_i_set(cq_enabled, false);
        }
    }
    free(old_msg);

    ft8_cell_type_t cell_type;
    if (meta.to_me) {
        cell_type = CELL_RX_TO_ME;
    } else if (meta.type == FTX_MSG_TYPE_CQ) {
        cell_type = CELL_RX_CQ;
    } else if (!param_i_get(cfg.ft8.show_all())) {
        return;
    } else {
        cell_type = CELL_RX_MSG;
    }

    cell_data_t  cell_data;
    if (meta.type == FTX_MSG_TYPE_CQ) {
        cell_data.worked_type = qso_log_search_worked(
            meta.call_de,
            param_i_get(cfg.ft8.protocol()) == FTX_PROTOCOL_FT8 ? MODE_FT8 : MODE_FT4,
            qso_log_freq_to_band(cparam_i_get(cfg.cur.fg_freq()))
        );
    }

    cell_data.cell_type = cell_type;
    strncpy(cell_data.text, text, sizeof(cell_data.text) - 1);
    cell_data.meta = meta;
    /* Country of the sender, from the call (cty.dat), as in JTDX. */
    cell_data.country[0]    = '\0';
    cell_data.cty_prefix[0] = '\0';
    if (meta.type != FXT_MSG_TYPE_OTHER && meta.call_de[0] != '\0') {
        const char *name, *prefix;
        if (dxcc_lookup(meta.call_de, &name, &prefix)) {
            snprintf(cell_data.country, sizeof(cell_data.country), "%s", name);
            snprintf(cell_data.cty_prefix, sizeof(cell_data.cty_prefix), "%s", prefix);
        }
    }
    cell_data.odd = s_info->odd;
    if (PARAM_T_GET(cfg.qth())[0] != 0) {
        if (strlen(meta.grid) > 0) {
            double lat, lon;
            qth_str_to_pos(meta.grid, &lat, &lon);
            cell_data.dist = qth_pos_dist(lat, lon, cur_lat, cur_lon);
        } else {
            cell_data.dist = 0;
        }
    } else {
        cell_data.dist = 0;
    }
    scheduler_put(table_view_add_msg_cb, (void*)&cell_data, sizeof(cell_data_t));
}

/* ---- audio_worker callbacks (run on the worker thread) ---------------- */

static void on_message_cb(const char *text, int snr, float freq_hz, float time_sec,
                          const slot_info_t *info, void *ctx) {
    (void)ctx;

    /* Mirror the decode to WSJT-X compatible listeners before the UI
     * work, so they see it promptly. Worker thread; the emitter only
     * builds a datagram on the stack and hands it to a non-blocking
     * socket. */
    ft8_udp_send_decode(true,
                        param_i_get(cfg.ft8.protocol()) == FTX_PROTOCOL_FT8 ? "~" : "+",
                        text, (int32_t)snr, time_sec, (uint32_t)(freq_hz + 0.5f));

    add_rx_text((int16_t)snr, text, (slot_info_t *)info, freq_hz, time_sec);
}

/**
 * Helper function to update UI in main thread
 */

struct waterfall_data {
    float *psd;
    size_t size;
};

static void waterfall_add_data(void *data) {
    struct waterfall_data *wf_data = (struct waterfall_data *)data;
    lv_waterfall_add_data(waterfall, wf_data->psd, wf_data->size);
    free(wf_data->psd);
}

static void on_psd_cb(const float *psd, uint16_t nfft, float sec_since_slot_start,
                      const slot_info_t *info, void *ctx) {
    (void)sec_since_slot_start;
    (void)info;
    (void)ctx;
    if (!psd || !nfft) return;

    uint32_t low_bin  = (uint32_t)nfft / 2u + (uint32_t)nfft * filter_low  / FTX_CAPTURE_RATE;
    uint32_t high_bin = (uint32_t)nfft / 2u + (uint32_t)nfft * filter_high / FTX_CAPTURE_RATE;
    if (high_bin > nfft) high_bin = nfft;
    if (low_bin >= high_bin) return;

    // Schedule waterfall update in main thread
    struct waterfall_data wf_data;
    wf_data.size = high_bin - low_bin;
    wf_data.psd = (float *)calloc(sizeof(float), wf_data.size);
    memcpy((void*)wf_data.psd, (void*)&psd[low_bin], wf_data.size * sizeof(float));
    scheduler_put(waterfall_add_data, &wf_data, sizeof(wf_data));
}

static void on_slot_end_cb(const slot_info_t *info, void *ctx) {
    (void)info;
    (void)ctx;
    if (qso_processor) {
        ftx_qso_processor_start_new_slot(qso_processor);
    }
}

static void on_tick_cb(const slot_info_t *info, bool new_slot,
                       float sec_since_slot_start, void *ctx) {
    (void)ctx;

    bool have_tx_msg = tx_msg.msg[0] != '\0';

    if ((sec_since_slot_start < MAX_TX_START_DELAY) && have_tx_msg) {
        if ((tx_time_slot == info->odd) && subject_i_get(tx_enabled)) {
            state = TX_PROCESS;
            add_tx_text(tx_msg.msg);
            tx_worker_run(tx_msg.msg, base_gain_offset,
                          tx_should_abort_cb, NULL);
            state = RX_PROCESS;
            if (tx_msg.repeats > 0) {
                tx_msg.repeats--;
            }
            if (tx_msg.repeats == 0) {
                if (strncmp(tx_msg.msg, "CQ", 2) == 0) {
                    subject_i_set(cq_enabled, false);
                }
                tx_msg.msg[0] = '\0';
            }
            return;
        }
    }

    if (new_slot) {
        state = RX_PROCESS;
        if (!have_tx_msg || !subject_i_get(tx_enabled)) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            struct tm *ts = localtime(&now.tv_sec);
            add_info("RX %s %02i:%02i:%02i", cfg_digital_label_get(),
                     ts->tm_hour, ts->tm_min, ts->tm_sec);
        }
    }
}
