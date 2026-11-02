/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

/*********************
 *      INCLUDES
 *********************/
#include "radio.h"

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>

#include <aether_radio/x6100_control/low/flow.h>
#include <aether_radio/x6100_control/low/gpio.h>

#include "audio.h"
#include "cfg/cfg_api.h"
#include "cfg/db.h"
#include "globals.h"
#include "util.h"
#include "voice.h"
#include "dsp.h"
#include "hkey.h"
#include "tx_info.h"
#include "cw.h"
#include "pubsub_ids.h"
#include "scheduler.h"
#include "diag_log.h"

/*********************
 *      DEFINES
 *********************/

#define FLOW_RESTART_TIMEOUT 300
#define IDLE_TIMEOUT        (3 * 1000)

/* Fixed PA bias DAC values applied once at radio_init (not user settings). */
#define BIAS_DRIVE_DEFAULT 450
#define BIAS_FINAL_DEFAULT 650

#define FILTER_2_OFFSET 60
#define FILTER_2_OFFSET_IN 20
#define FILTER_2_OFFSET_OUT (FILTER_2_OFFSET - FILTER_2_OFFSET_IN)

/* Battery voltage in 0.1 V units; LOW + HYSTERESIS = HIGH. */
#define LOW_BATTERY_VBAT_LOW     60
#define LOW_BATTERY_HYSTERESIS   2
#define LOW_BATTERY_VBAT_HIGH    (LOW_BATTERY_VBAT_LOW + LOW_BATTERY_HYSTERESIS)

/**********************
 *      MACROS
 **********************/

#define WITH_RADIO_LOCK(fn) radio_lock(); fn; radio_unlock();

/**********************
 *      TYPEDEFS
 **********************/

typedef enum {
    x6100_flow_fp32 = 0,
    x6100_flow_bf16,
} x6100_flow_fmt_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void on_change_bool(Subject *subj, void *user_data);
static void radio_lock();
static void radio_unlock();

static void on_vfo_freq_change(Subject *subj, void *user_data);
static void on_vfo_mode_change(Subject *subj, void *user_data);
static void on_vfo_agc_change(Subject *subj, void *user_data);
static void on_vfo_att_change(Subject *subj, void *user_data);
static void on_vfo_pre_change(Subject *subj, void *user_data);
static void on_low_filter_change(Subject *subj, void *user_data);
static void on_high_filter_change(Subject *subj, void *user_data);

static void on_if_shift_change(Subject *subj, void *user_data);
static void on_band_change(Subject *subj, void *user_data);
static void update_agc_time(Subject *subj, void *user_data);
static void on_atu_network_change(Subject *subj, void *user_data);
static void on_change_comp_ratio(Subject *subj, void *user_data);
static void base_control_command(Subject *subj, void *user_data);
static void on_fw_zoom_change(Subject *subj, void *user_data);

static void on_change_float(Subject *subj, void *user_data);
static void on_change_uint32(Subject *subj, void *user_data);
static void on_change_int32(Subject *subj, void *user_data);
static void on_change_uint16(Subject *subj, void *user_data);
static void on_change_uint8(Subject *subj, void *user_data);
static void on_change_int8(Subject *subj, void *user_data);
static void on_volume_change(Subject *subj, void *user_data);

static void recover_processing_audio_inputs();
static bool radio_tick();
static void * radio_thread(void *arg);

static void recompute_display_freqs(void);
static void recompute_subj_cb(Subject *subj, void *user_data);

/**********************
 *  STATIC VARIABLES
 **********************/

static pthread_mutex_t  control_mux;
static pthread_t        radio_pthread;
static bool             radio_pthread_started = false;

static x6100_flow_t    *pack;
static x6100_base_ver_t base_ver;
static int8_t           audio_in_lvl;

static radio_state_t    state = RADIO_RX;
static uint64_t         now_time;
static uint64_t         prev_time;
static uint64_t         idle_time;

/* 1KO125 diagnostics (diag_log.h): what the APP asked the BASE for */
static volatile bool    diag_cmd_ptt, diag_cmd_modem, diag_cmd_key, diag_cmd_other;
static char             diag_cmd_src[32] = "none";  /* a copy: the thread may be gone when it is read */
static pthread_mutex_t  diag_src_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t diag_cmd_time;
static uint64_t         diag_tx_start;
static float            diag_tx_max_pwr;
static bool             diag_noresp_logged;
static volatile bool    diag_tx_seen;       /* the BASE went TX since the last ON command */
static bool             diag_tx_logged;     /* this TX is in ptt.log (not CW) */

/* Before the command: what the APP asked for, so the BASE's answer is matched to it */
static void diag_tx_note(bool on, volatile bool *flag) {
    pthread_mutex_lock(&diag_src_mutex);
    diag_source_copy(diag_cmd_src, sizeof(diag_cmd_src));
    pthread_mutex_unlock(&diag_src_mutex);
    *flag = on;
    if (on) diag_tx_seen = false;
    diag_cmd_time = get_time();
    diag_noresp_logged = false;
}

static void diag_cmd_src_get(char *dst, size_t size) {
    pthread_mutex_lock(&diag_src_mutex);
    snprintf(dst, size, "%s", diag_cmd_src);
    pthread_mutex_unlock(&diag_src_mutex);
}

/* After the command: its line in ptt.log (queued, never waits for the SD card) */
static void diag_tx_log(const char *what, bool on) {
    char src[32];
    diag_source_copy(src, sizeof(src));
    diag_ptt_log("CMD  %-10s %-3s  src=%-14s  freq=%d mode=%d pwr=%.1f", what, on ? "ON" : "OFF", src,
                 (int)cparam_i_get(cfg.cur.fg_freq()), (int)cparam_i_get(cfg.cur.mode()), param_f_get(cfg.pwr()));
}
static bool             mute = false;
static bool             low_power = false;

static pthread_mutex_t  swrscan_cb_mux = PTHREAD_MUTEX_INITIALIZER;
static radio_swrscan_cb_t swrscan_cb = NULL;

static pthread_mutex_t  power_cb_mux = PTHREAD_MUTEX_INITIALIZER;
static radio_power_cb_t power_cb = NULL;

static cfloat           samples_buf[RADIO_SAMPLES*2];

static uint32_t min_tx;
static uint32_t max_tx;

SubjectInt *radio_fg_freq_subj = NULL;
SubjectInt *radio_bg_freq_subj = NULL;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void radio_bb_reset() {
    x6100_gpio_set(x6100_pin_bb_reset, 1);
    usleep(100000);
    x6100_gpio_set(x6100_pin_bb_reset, 0);
}

static void recompute_display_freqs(void) {
    bool split   = param_i_get(cfg.band.split());
    int32_t fg   = cparam_i_get(cfg.cur.fg_freq());
    int32_t bg   = cparam_i_get(cfg.cur.bg_freq());
    bool is_tx   = state == RADIO_TX;

    if (split && is_tx) {
        subject_i_set(radio_fg_freq_subj, bg);
        subject_i_set(radio_bg_freq_subj, fg);
    } else {
        subject_i_set(radio_fg_freq_subj, fg);
        subject_i_set(radio_bg_freq_subj, bg);
    }
}

static void recompute_subj_cb(Subject *subj, void *user_data) {
    (void)subj; (void)user_data;
    recompute_display_freqs();
}

static void init_display_freqs(void) {
    radio_fg_freq_subj = subject_i_create(14100000);
    radio_bg_freq_subj = subject_i_create(14150000);

    subject_subscribe((Subject*)cfg.cur.fg_freq(), recompute_subj_cb, NULL);
    subject_subscribe((Subject*)cfg.cur.bg_freq(), recompute_subj_cb, NULL);
    subject_subscribe((Subject*)cfg.band.split(), recompute_subj_cb, NULL);

    recompute_display_freqs();
}

void radio_init() {
    init_display_freqs();

    if (!x6100_gpio_init())
        return;

    x6100_gpio_set(x6100_pin_morse_key, 1);     /* Morse key off */

    if (!x6100_flow_init())
        return;
    uint8_t cnt = 0;
    while (!x6100_control_init()) {
        usleep(100000 << cnt);
        if (cnt < 8) cnt++;
        if (cnt == 6) {
            LV_LOG_ERROR("Reset BB");
            radio_bb_reset();
            usleep(1000000);
        }
    }
}

void radio_start() {

    base_ver = x6100_control_get_base_ver();

    // Enable center mode by default (old behavior)
    if ((util_compare_version(base_ver, (x6100_base_ver_t){1, 1, 9, 0}) >= 0) || (base_ver.rev >= 8)) {
        x6100_control_if_shift_set(false);
    }

    pack = malloc(sizeof(x6100_flow_t));

    subject_subscribe_and_notify((Subject*)cfg.band.vfoa_freq(), on_vfo_freq_change, (void*)X6100_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg.band.vfob_freq(), on_vfo_freq_change, (void*)X6100_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg.band.vfoa_mode(), on_vfo_mode_change, (void*)X6100_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg.band.vfob_mode(), on_vfo_mode_change, (void*)X6100_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg.band.vfoa_agc(), on_vfo_agc_change, (void*)X6100_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg.band.vfob_agc(), on_vfo_agc_change, (void*)X6100_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg.band.vfoa_att(), on_vfo_att_change, (void*)X6100_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg.band.vfob_att(), on_vfo_att_change, (void*)X6100_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg.band.vfoa_pre(), on_vfo_pre_change, (void*)X6100_VFO_A);
    subject_subscribe_and_notify((Subject*)cfg.band.vfob_pre(), on_vfo_pre_change, (void*)X6100_VFO_B);

    subject_subscribe_and_notify((Subject*)cfg.band.current_vfo(), on_change_uint32, x6100_control_vfo_set);
    subject_subscribe_and_notify((Subject*)cfg.band.split(), on_change_uint8, x6100_control_split_set);
    subject_subscribe_and_notify((Subject*)cfg.rfgain(), on_change_uint8, x6100_control_rfg_set);

    subject_subscribe_and_notify((Subject*)cfg.band.if_shift(), on_if_shift_change, NULL);

    subject_subscribe_and_notify((Subject*)cfg.band.tx_i_offset(), on_change_int32, x6100_control_tx_i_offset_set);
    subject_subscribe_and_notify((Subject*)cfg.band.tx_q_offset(), on_change_int32, x6100_control_tx_q_offset_set);

    subject_subscribe((Subject*)cfg.band_id(), on_band_change, NULL);

    subject_subscribe((Subject*)cfg.cur.agc(), update_agc_time, NULL);
    subject_subscribe_and_notify((Subject*)cfg.cur.mode(), update_agc_time, NULL);

    subject_subscribe((Subject*)cfg.filter.low(), on_low_filter_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg.cur.mode(), on_low_filter_change, NULL);
    subject_subscribe((Subject*)cfg.filter.high(), on_high_filter_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg.cur.mode(), on_high_filter_change, NULL);

    subject_subscribe_and_notify((Subject*)cfg.volume(), on_volume_change, NULL);
    subject_subscribe_and_notify((Subject*)cfg.squelch(), on_change_uint8, x6100_control_sql_set);
    subject_subscribe_and_notify((Subject*)cfg.pwr(), on_change_float, x6100_control_txpwr_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.output_gain(), on_change_float, x6100_control_adc_dac_gain_set);
    subject_subscribe_and_notify((Subject*)cfg.band.dac_offset(), on_change_float, x6100_control_dac_gain_set);
    subject_subscribe_and_notify((Subject*)cfg.atu_enabled(), on_change_uint8, x6100_control_atu_set);
    cfg_atu_network_subscribe(on_atu_network_change, NULL);
    on_atu_network_change(NULL, NULL);

    /* Compressor */
    subject_subscribe_and_notify((Subject*)cfg.dsp.comp(), on_change_comp_ratio, NULL);
    subject_subscribe_and_notify((Subject*)cfg.dsp.comp_threshold_offset(), on_change_float, x6100_control_comp_threshold_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.comp_makeup_offset(), on_change_float, x6100_control_comp_makeup_set);

    /* VOX */
    subject_subscribe_and_notify((Subject*)cfg.vox.on(), on_change_uint8, x6100_control_vox_set);
    subject_subscribe_and_notify((Subject*)cfg.vox.gain(), on_change_uint8, x6100_control_vox_gain_set);
    subject_subscribe_and_notify((Subject*)cfg.vox.ag(), on_change_uint8, x6100_control_vox_ag_set);
    subject_subscribe_and_notify((Subject*)cfg.vox.delay(), on_change_uint16, x6100_control_vox_delay_set);

    subject_subscribe_and_notify((Subject*)cfg.mic(), on_change_uint8, x6100_control_mic_set);
    subject_subscribe_and_notify((Subject*)cfg.hmic(), on_change_uint8, x6100_control_hmic_set);
    subject_subscribe_and_notify((Subject*)cfg.imic(), on_change_uint8, x6100_control_imic_set);
    subject_subscribe_and_notify((Subject*)cfg.moni(), on_change_uint8, x6100_control_moni_set);

    subject_subscribe_and_notify((Subject*)cfg.rit(), base_control_command, (void*)x6100_rit);
    subject_subscribe_and_notify((Subject*)cfg.xit(), base_control_command, (void*)x6100_xit);
    subject_subscribe_and_notify((Subject*)cfg.dsp.fm_emphasis(), on_change_bool, x6100_control_fm_emp);

    subject_subscribe_and_notify((Subject*)cfg.dsp.tx_filter_low(), on_change_uint16, x6100_control_tx_filter_low_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.tx_filter_high(), on_change_uint16, x6100_control_tx_filter_high_set);

    subject_subscribe_and_notify((Subject*)cfg.dsp.cessb_on(), on_change_bool, x6100_control_cessb_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.cessb_power_up(), on_change_float, x6100_control_cessb_power_up_set);

    subject_subscribe_and_notify((Subject*)cfg.cw.key_tone(), on_change_uint16, x6100_control_key_tone_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.key_speed(), on_change_uint8, x6100_control_key_speed_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.key_mode(), on_change_uint8, x6100_control_key_mode_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.iambic_mode(), on_change_uint8, x6100_control_iambic_mode_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.key_vol(), on_change_uint16, x6100_control_key_vol_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.key_train(), on_change_uint8, x6100_control_key_train_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.qsk_time(), on_change_uint16, x6100_control_qsk_time_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.key_ratio(), on_change_float, x6100_control_key_ratio_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.peak_on(), on_change_bool, x6100_control_cw_peak_set);
    subject_subscribe_and_notify((Subject*)cfg.cw.peak_q(), on_change_uint8, x6100_control_cw_peak_q_set);

    subject_subscribe_and_notify((Subject*)cfg.agc.hang(), on_change_uint8, x6100_control_agc_hang_set);
    subject_subscribe_and_notify((Subject*)cfg.agc.knee(), on_change_int8, x6100_control_agc_knee_set);
    subject_subscribe_and_notify((Subject*)cfg.agc.slope(), on_change_uint8, x6100_control_agc_slope_set);

    subject_subscribe_and_notify((Subject*)cfg.dsp.dnf(), on_change_uint8, x6100_control_dnf_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.dnf_center(), on_change_uint16, x6100_control_dnf_center_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.dnf_width(), on_change_uint16, x6100_control_dnf_width_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.dnf_auto(), on_change_uint16, x6100_control_dnf_update_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.nb(), on_change_uint8, x6100_control_nb_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.nb_level(), on_change_uint8, x6100_control_nb_level_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.nb_width(), on_change_uint8, x6100_control_nb_width_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.nr(), on_change_uint8, x6100_control_nr_set);
    subject_subscribe_and_notify((Subject*)cfg.dsp.nr_level(), on_change_uint8, x6100_control_nr_level_set);

    if ((util_compare_version(base_ver, (x6100_base_ver_t){1, 1, 9, 0}) >= 0) || (base_ver.rev >= 8)) {
        subject_subscribe_and_notify((Subject*)cfg.mode.zoom(), on_fw_zoom_change, NULL);
    }

    x6100_control_charger_set(param_i_get(cfg.radio.charger()) == RADIO_CHARGER_ON);
    x6100_control_bias_drive_set(BIAS_DRIVE_DEFAULT);
    x6100_control_bias_final_set(BIAS_FINAL_DEFAULT);

    subject_subscribe_and_notify((Subject*)cfg.radio.spmode(), on_change_bool, x6100_control_spmode_set);
    subject_subscribe_and_notify((Subject*)cfg.radio.line_in(), on_change_uint8, x6100_control_linein_set);
    subject_subscribe_and_notify((Subject*)cfg.radio.line_out(), on_change_uint8, x6100_control_lineout_set);

    if (base_ver.rev >= 8) {
        x6100_control_bf16_flow_set(true);
    }

    prev_time = get_time();
    idle_time = prev_time;

    pthread_mutex_init(&control_mux, NULL);

    int rc = pthread_create(&radio_pthread, NULL, radio_thread, NULL);

    if (rc != 0) {
        LV_LOG_ERROR("Can't start radio thread: %d", rc);
    } else {
        radio_pthread_started = true;
    }
}

void radio_shutdown() {
    if (!radio_pthread_started) {
        return;
    }

    pthread_join(radio_pthread, NULL);
    radio_pthread_started = false;
}

radio_state_t radio_get_state() {
    return state;
}

void radio_set_freq(int32_t freq) {
    if (!radio_check_freq(freq)) {
        LV_LOG_ERROR("Freq %i incorrect", freq);
        return;
    }
    x6100_vfo_t vfo = param_i_get(cfg.band.current_vfo());
    int32_t shift = cfg_transverter_shift_for(freq);
    WITH_RADIO_LOCK(x6100_control_vfo_freq_set(vfo, freq - shift));
}

bool radio_check_freq(int32_t freq) {
    return cfg_is_valid_hw_freq(freq);
}

/* Bluetooth listening: see radio_set_volume_sink() in radio.h. The
 * mutex keeps a volume change and the hand-over from crossing, so the
 * speaker can never be left at a volume while the headphones play. */
static pthread_mutex_t   vol_sink_mux = PTHREAD_MUTEX_INITIALIZER;
static radio_vol_sink_t  vol_sink = NULL;

static void on_volume_change(Subject *subj, void *user_data) {
    int32_t new_val = subject_i_get((SubjectInt*)subj);

    (void)user_data;
    pthread_mutex_lock(&vol_sink_mux);
    if (vol_sink) {
        vol_sink(new_val);
        new_val = 0;
    }
    WITH_RADIO_LOCK(x6100_control_rxvol_set(new_val));
    pthread_mutex_unlock(&vol_sink_mux);
}

void radio_set_volume_sink(radio_vol_sink_t fn) {
    int32_t vol;

    pthread_mutex_lock(&vol_sink_mux);
    vol_sink = fn;
    vol = mute ? 0 : param_i_get(cfg.volume());
    if (fn) {
        fn(vol);
        vol = 0;
    }
    WITH_RADIO_LOCK(x6100_control_rxvol_set(vol));
    pthread_mutex_unlock(&vol_sink_mux);
}

uint16_t radio_change_vol(int16_t df) {
    int32_t vol = param_i_get(cfg.volume());
    if (df == 0) {
        return vol;
    }

    mute = false;

    uint16_t new_val = limit(vol + df, 0, 55);

    if (new_val != vol) {
        param_i_set(cfg.volume(), new_val);
    };

    return new_val;
}

void radio_change_mute() {
    pthread_mutex_lock(&vol_sink_mux);
    mute = !mute;
    if (vol_sink) {
        /* Bluetooth listening: mute the headphones, speaker stays 0. */
        vol_sink(mute ? 0 : param_i_get(cfg.volume()));
    } else {
        x6100_control_rxvol_set(mute ? 0 : param_i_get(cfg.volume()));
    }
    pthread_mutex_unlock(&vol_sink_mux);
}

void radio_start_atu() {
    if (state == RADIO_RX) {
        state = RADIO_ATU_START;
    }
}

bool radio_start_swrscan() {
    if (state != RADIO_RX) {
        return false;
    }

    cparam_i_set(cfg.cur.mode(), x6100_mode_am);
    diag_tx_note(true, &diag_cmd_other);
    radio_lock();
    x6100_control_txpwr_set(5.0f);
    x6100_control_swrscan_set(true);
    radio_unlock();
    diag_tx_log("SWR SCAN", true);
    state = RADIO_SWRSCAN;

    return true;
}

void radio_stop_swrscan() {
    if (state == RADIO_SWRSCAN) {
        state = RADIO_RX;
        diag_tx_note(false, &diag_cmd_other);
        radio_lock();
        x6100_control_swrscan_set(false);
        x6100_control_txpwr_set(param_f_get(cfg.pwr()));
        radio_unlock();
        diag_tx_log("SWR SCAN", false);
    }
}

void radio_swrscan_set_cb(radio_swrscan_cb_t cb) {
    pthread_mutex_lock(&swrscan_cb_mux);
    swrscan_cb = cb;
    pthread_mutex_unlock(&swrscan_cb_mux);
}

void radio_power_set_cb(radio_power_cb_t cb) {
    pthread_mutex_lock(&power_cb_mux);
    power_cb = cb;
    pthread_mutex_unlock(&power_cb_mux);
}

void radio_set_pwr(float d) {
    WITH_RADIO_LOCK(x6100_control_txpwr_set(d));
}

void radio_set_tx_filter(uint16_t low, uint16_t high) {
    radio_lock();
    x6100_control_tx_filter_low_set(low);
    x6100_control_tx_filter_high_set(high);
    radio_unlock();
}

void radio_set_rx_dsp_off(bool off) {
    radio_lock();
    x6100_control_nr_set(!off && param_i_get(cfg.dsp.nr()));
    x6100_control_nb_set(!off && param_i_get(cfg.dsp.nb()));
    x6100_control_dnf_set(!off && param_i_get(cfg.dsp.dnf()));
    x6100_control_dnf_update_set(!off && param_i_get(cfg.dsp.dnf_auto()));
    radio_unlock();
}

x6100_vfo_t radio_toggle_vfo() {
    x6100_vfo_t new_vfo = (param_i_get(cfg.band.current_vfo()) == X6100_VFO_A) ? X6100_VFO_B : X6100_VFO_A;

    param_i_set(cfg.band.current_vfo(), new_vfo);
    // TODO: move to another file
    voice_say_text_fmt("V F O %s", (new_vfo == X6100_VFO_A) ? "A" : "B");

    return new_vfo;
}

void radio_poweroff() {
    if (param_i_get(cfg.radio.charger()) == RADIO_CHARGER_SHADOW) {
        WITH_RADIO_LOCK(x6100_control_charger_set(true));
    }
    state = RADIO_POWEROFF;
}

void radio_set_charger(bool on) {
    WITH_RADIO_LOCK(x6100_control_charger_set(on));
}

void radio_set_ptt(bool tx) {
    diag_tx_note(tx, &diag_cmd_ptt);
    WITH_RADIO_LOCK(x6100_control_ptt_set(tx));
    diag_tx_log("PTT", tx);
}

void radio_set_modem(bool tx) {
    diag_tx_note(tx, &diag_cmd_modem);
    WITH_RADIO_LOCK(x6100_control_modem_set(tx));
    diag_tx_log("MODEM", tx);
}

void radio_speaker_play(bool on) {
    WITH_RADIO_LOCK(audio_set_play_mode(on ? AUDIO_PLAY_ON : AUDIO_PLAY_OFF));
}

void radio_set_morse_key(bool on) {
    diag_cmd_key = on;          /* not logged: one line per dot and dash */
    x6100_gpio_set(x6100_pin_morse_key, on ? 0 : 1);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

 static void radio_lock() {
    pthread_mutex_lock(&control_mux);
}

static void radio_unlock() {
    idle_time = get_time();
    pthread_mutex_unlock(&control_mux);
}

static void on_change_bool(Subject *subj, void *user_data) {
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    void (*fn)(bool) = (void (*)(bool))user_data;
    if (fn == x6100_control_spmode_set) {
        // function expect true for phone mode (not speaker)
        new_val = !new_val;
    }
    WITH_RADIO_LOCK(fn(new_val));
}

static void on_change_int8(Subject *subj, void *user_data) {
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    void (*fn)(int8_t) = (void (*)(int8_t))user_data;
    WITH_RADIO_LOCK(fn(new_val));
}

static void on_change_uint8(Subject *subj, void *user_data) {
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    void (*fn)(uint8_t) = (void (*)(uint8_t))user_data;
    WITH_RADIO_LOCK(fn(new_val));
}


static void on_change_uint16(Subject *subj, void *user_data) {
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    void (*fn)(uint16_t) = (void (*)(uint16_t))user_data;
    WITH_RADIO_LOCK(fn(new_val));
}

static void on_change_uint32(Subject *subj, void *user_data) {
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    void (*fn)(uint32_t) = (void (*)(uint32_t))user_data;
    WITH_RADIO_LOCK(fn(new_val));
}

static void on_change_int32(Subject *subj, void *user_data) {
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    void (*fn)(int32_t) = (void (*)(int32_t))user_data;
    WITH_RADIO_LOCK(fn(new_val));
}

static void on_change_float(Subject *subj, void *user_data) {
    float new_val = subject_f_get((SubjectFloat*)subj);
    void (*fn)(float) = (void (*)(float))user_data;
    WITH_RADIO_LOCK(fn(new_val));
}

static void on_vfo_freq_change(Subject *subj, void *user_data) {
    x6100_vfo_t vfo = (x6100_vfo_t )user_data;
    int32_t new_val;
    if (vfo == X6100_VFO_A) {
        new_val = param_i_get(cfg.band.vfoa_freq());
    } else {
        new_val = param_i_get(cfg.band.vfob_freq());
    }
    int32_t shift = cfg_transverter_shift_for(new_val);
    shift += param_i_get(cfg.band.if_shift());
    WITH_RADIO_LOCK(x6100_control_vfo_freq_set(vfo, new_val - shift));
    LV_LOG_USER("Radio set vfo %i freq=%i (%i)", vfo, new_val, new_val - shift);
}

static void on_vfo_mode_change(Subject *subj, void *user_data) {
    x6100_vfo_t vfo = (x6100_vfo_t )user_data;
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    WITH_RADIO_LOCK(x6100_control_vfo_mode_set(vfo, new_val));
    LV_LOG_USER("Radio set vfo %i mode=%i", vfo, new_val);;
}

static void on_vfo_agc_change(Subject *subj, void *user_data) {
    x6100_vfo_t vfo = (x6100_vfo_t )user_data;
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    WITH_RADIO_LOCK(x6100_control_vfo_agc_set(vfo, new_val));
    LV_LOG_USER("Radio set vfo %i agc=%i", vfo, new_val);
}

static void update_agc_time(Subject *subj, void *user_data) {
    x6100_agc_t     agc = cparam_i_get(cfg.cur.agc());
    x6100_mode_t    mode = cparam_i_get(cfg.cur.mode());
    uint16_t        agc_time = 500;

    switch (agc) {
        case x6100_agc_off:
            agc_time = 1000;
            break;

        case x6100_agc_slow:
            agc_time = 1000;
            break;

        case x6100_agc_fast:
            agc_time = 100;
            break;

        case x6100_agc_auto:
            switch (mode) {
                case x6100_mode_lsb:
                case x6100_mode_lsb_dig:
                case x6100_mode_usb:
                case x6100_mode_usb_dig:
                    agc_time = 500;
                    break;

                case x6100_mode_cw:
                case x6100_mode_cwr:
                    agc_time = 100;
                    break;

                case x6100_mode_am:
                case x6100_mode_nfm:
                    agc_time = 1000;
                    break;
            }
            break;
    }
    WITH_RADIO_LOCK(x6100_control_agc_time_set(agc_time));
    LV_LOG_USER("Radio set agc time=%u for agc: %i\n", agc_time, agc);
}

static void on_vfo_att_change(Subject *subj, void *user_data) {
    x6100_vfo_t vfo = (x6100_vfo_t )user_data;
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    WITH_RADIO_LOCK(x6100_control_vfo_att_set(vfo, new_val));
    LV_LOG_USER("Radio set vfo %i att=%i", vfo, new_val);
}

static void on_vfo_pre_change(Subject *subj, void *user_data) {
    x6100_vfo_t vfo = (x6100_vfo_t )user_data;
    int32_t new_val = subject_i_get((SubjectInt*)subj);
    WITH_RADIO_LOCK(x6100_control_vfo_pre_set(vfo, new_val));
    LV_LOG_USER("Radio set vfo %i pre=%i", vfo, new_val);
}

static void on_atu_network_change(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    uint32_t new_val = cfg_atu_get_network();
    WITH_RADIO_LOCK(x6100_control_cmd(x6100_atu_network, new_val));
    LV_LOG_USER("Radio set atu network=%u", new_val);
}

static void on_low_filter_change(Subject *subj, void *user_data) {
    x6100_mode_t mode = cparam_i_get(cfg.cur.mode());
    if ((mode == x6100_mode_am) || (mode == x6100_mode_nfm)) {
        // No update low on AM/FM, low should be -high
        return;
    }
    int32_t low = cparam_i_get(cfg.filter.low());
    int32_t low2 = LV_MAX(0, low - FILTER_2_OFFSET_OUT);
    radio_lock();
    LV_LOG_USER("Radio set filters: low=%i, low2=%i", low, low2);
    x6100_control_cmd(x6100_filter1_low, low);
    x6100_control_cmd(x6100_filter2_low, low2);
    radio_unlock();
}

static void on_high_filter_change(Subject *subj, void *user_data) {
    int32_t high = cparam_i_get(cfg.filter.high());
    int32_t high2 = high + FILTER_2_OFFSET_OUT;
    switch (cparam_i_get(cfg.cur.mode())) {
        case x6100_mode_am:
        case x6100_mode_nfm:
            // For AM BASE uses absolute low and high values to choose LPF filters for I and Q
            radio_lock();
            LV_LOG_USER("Radio set filters: low=%i, low2=%i", -high, -high2);
            x6100_control_cmd(x6100_filter1_low, -high);
            x6100_control_cmd(x6100_filter2_low, -high2);
            radio_unlock();
            break;
        default:
            break;
    }
    radio_lock();
    LV_LOG_USER("Radio set filters: high=%i, high2=%i", high, high2);
    x6100_control_cmd(x6100_filter1_high, high);
    x6100_control_cmd(x6100_filter2_high, high2);
    radio_unlock();
}

static void on_change_comp_ratio(Subject *subj, void *user_data) {
    uint8_t ratio = subject_i_get((SubjectInt*)subj);
    if (ratio < 1) {
        ratio = 1;
    }
    if (ratio == 1) {
        // invert
        WITH_RADIO_LOCK(x6100_control_comp_set(true));
    } else {
        radio_lock();
        x6100_control_comp_set(false);
        x6100_control_comp_level_set((x6100_comp_level_t)(ratio - 2));
        radio_unlock();
    }
}

static void on_fw_zoom_change(Subject *subj, void *user_data) {
    uint8_t zoom = subject_i_get((SubjectInt*)subj);
    uint8_t val = 0;
    while (zoom > 1) {
        val++;
        zoom >>= 1;
    }
    WITH_RADIO_LOCK(x6100_control_fftdec_set(val));
}

static void on_if_shift_change(Subject *subj, void *user_data) {
    int32_t shift = subject_i_get((SubjectInt*)subj);
    int32_t cur_freq = cparam_i_get(cfg.cur.fg_freq());

    LV_LOG_USER("Shift: %d, cur_freq: %d\n", shift, cur_freq);
    if (shift != 0) {
        radio_lock();
        x6100_control_if_shift_freq_set(shift);
        x6100_control_if_shift_set(true);
        radio_unlock();
    } else {
        WITH_RADIO_LOCK(x6100_control_if_shift_set(false));
    }
    x6100_vfo_t vfo = param_i_get(cfg.band.current_vfo());
    on_vfo_freq_change(NULL, (void*)vfo);
}

static void on_band_change(Subject *subj, void *user_data) {
    // Workaround for bug with ignored IQ offset on band change from BASE
    int32_t i_val = param_i_get(cfg.band.tx_i_offset());
    int32_t q_val = param_i_get(cfg.band.tx_q_offset());
    if (i_val == (int32_t)x6100_control_get(x6100_txiofs)) {
        i_val++;
    }
    radio_lock();
    x6100_control_tx_i_offset_set(i_val);
    radio_unlock();
    if (q_val == (int32_t)x6100_control_get(x6100_txqofs)) {
        q_val ++;
    }
    radio_lock();
    x6100_control_tx_q_offset_set(q_val);
    radio_unlock();
}

static void base_control_command(Subject *subj, void *user_data) {
    uint32_t val = subject_i_get((SubjectInt*)subj);
    x6100_cmd_enum_t cmd = (x6100_cmd_enum_t)user_data;
    WITH_RADIO_LOCK(x6100_control_cmd(cmd, val));
}

/**
 * Restore "listening" of main board and USB soundcard after ATU
 */
static void recover_processing_audio_inputs() {
    usleep(10000);
    x6100_vfo_t vfo = param_i_get(cfg.band.current_vfo());
    diag_ptt_log("CMD  ATU end: MODEM pulse 50 ms at 0.1 W (audio inputs recovery)");
    radio_lock();
    x6100_control_vfo_mode_set(vfo, x6100_mode_usb_dig);
    x6100_control_txpwr_set(0.1f);
    x6100_control_modem_set(true);
    usleep(50000);
    x6100_control_modem_set(false);
    x6100_control_txpwr_set(param_f_get(cfg.pwr()));
    x6100_control_vfo_mode_set(vfo, cparam_i_get(cfg.cur.mode()));
    radio_unlock();
}

static bool radio_tick() {
    if (now_time < prev_time) {
        prev_time = now_time;
    }

    int32_t d = now_time - prev_time;

    if (x6100_flow_read(pack)) {
        prev_time = now_time;

        static uint8_t delay;

        if (delay++ > 10) {
            delay = 0;

            pthread_mutex_lock(&power_cb_mux);
            if (power_cb) {
                radio_power_t power = {
                    .vext     = pack->vext * 0.1f,
                    .vbat     = pack->vbat * 0.1f,
                    .cap      = pack->batcap,
                    .charging = pack->flag.charging,
                };
                power_cb(&power);
            }
            pthread_mutex_unlock(&power_cb_mux);

            bool no_vext    = !pack->flag.vext;
            bool enter_low  = no_vext && (pack->vbat <= LOW_BATTERY_VBAT_LOW);
            bool leave_low  = pack->flag.vext || (pack->vbat >= LOW_BATTERY_VBAT_HIGH);

            if (!low_power && enter_low) {
                low_power = true;
                scheduler_msg_send(MSG_LOW_POWER, (void*)(uintptr_t)low_power);
            } else if (low_power && leave_low) {
                low_power = false;
                scheduler_msg_send(MSG_LOW_POWER, (void*)(uintptr_t)low_power);
            }
        }
        flow_info_t flow_info = pack->flow_info;

        uint32_t base_freq = 0;
        uint8_t fft_dec = 0;
        /*
        On patched:
            RX:
                SSB: base_freq = fg_freq - if_shift + rit
                CW: base_freq = fg_freq - key_tone - if_shift + rit
                CWR: base_freq = fg_freq + key_tone - if_shift + rit
                AM: base_freq = fg_freq - 100 + rit
                FM: base_freq = fg_freq - 200 + rit
            TX:
                SSB: base_freq = fg_freq + xit
                CW: base_freq = fg_freq + xit

            On OEM: base_freq = 0
        */
        if (base_ver.rev >= 8) {
            base_freq = flow_info.lo_freq;
            fft_dec = 1U << flow_info.fft_dec;
        }

        cfloat *flow_samples = (cfloat*)((char *)pack + offsetof(x6100_flow_t, samples));
        cfloat *samples;
        size_t n_samples;

        if (base_ver.rev >= 8) {
            if (flow_info.flow_fmt == x6100_flow_fp32) {
                samples = flow_samples;
                n_samples = RADIO_SAMPLES;
            } else if (flow_info.flow_fmt == x6100_flow_bf16) {
                n_samples = RADIO_SAMPLES * 2;
                uint16_t *u16_flow_samples = (uint16_t*)flow_samples;
                uint32_t *u32_samples_buf = (uint32_t*)samples_buf;
                for (size_t i = 0; i < n_samples * 2; i+=4) {
                    u32_samples_buf[i] = (uint32_t)u16_flow_samples[i] << 16;
                    u32_samples_buf[i+1] = (uint32_t)u16_flow_samples[i+1] << 16;
                    u32_samples_buf[i+2] = (uint32_t)u16_flow_samples[i+2] << 16;
                    u32_samples_buf[i+3] = (uint32_t)u16_flow_samples[i+3] << 16;
                }
                samples = samples_buf;
            }
        } else {
            samples = flow_samples;
            n_samples = RADIO_SAMPLES;
        }
        // printf("%d from %d, freq: %d, vary_freq: %d\n", flow_info->flow_seq_n, seq_total, base_freq, vary_freq);
        // union {
        //     uint32_t i;
        //     float f;
        // } fuint = {flow_info->_pad2};
        // printf("max signal: %g\n", fuint.f);
        // printf("audio mul: %.*g\n", fuint.f);
        dsp_samples(samples, n_samples, pack->flag.tx, base_freq, flow_info.vary_freq, fft_dec);

        switch (state) {
            case RADIO_RX:
                if (pack->flag.tx) {
                    bool cmd = diag_cmd_ptt || diag_cmd_modem || diag_cmd_key || diag_cmd_other;
                    int  m   = (int)cparam_i_get(cfg.cur.mode());
                    char src[32];
                    diag_cmd_src_get(src, sizeof(src));
                    diag_tx_seen    = true;
                    diag_tx_start   = now_time;
                    diag_tx_max_pwr = 0.0f;
                    /* CW is not logged: a TX for every element with QSK, keyer or paddle */
                    diag_tx_logged  = !(m == x6100_mode_cw || m == x6100_mode_cwr || diag_cmd_key);
                    if (!diag_tx_logged) {
                        /* nothing */
                    } else if (cmd) {
                        diag_ptt_log("BASE TX ON   commanded by %s (%.1f s before)  freq=%d mode=%d",
                                     src, (now_time - diag_cmd_time) / 1000.0f,
                                     (int)cparam_i_get(cfg.cur.fg_freq()), (int)cparam_i_get(cfg.cur.mode()));
                    } else {
                        diag_ptt_log("BASE TX ON   NOT COMMANDED by the APP (mic PTT, PTT jack or BASE)  "
                                     "freq=%d mode=%d pwr=%.1f  last cmd: %s %.1f s ago",
                                     (int)cparam_i_get(cfg.cur.fg_freq()), (int)cparam_i_get(cfg.cur.mode()),
                                     param_f_get(cfg.pwr()), src, (now_time - diag_cmd_time) / 1000.0f);
                    }
                    state = RADIO_TX;
                    scheduler_msg_send(MSG_RADIO_TX, NULL);
                    recompute_display_freqs();
                }
                break;

            case RADIO_TX:
                if (!pack->flag.tx) {
                    if (!diag_tx_logged) {
                        /* CW: not logged */
                    } else if (diag_cmd_ptt || diag_cmd_modem || diag_cmd_other) {
                        char src[32];
                        diag_cmd_src_get(src, sizeof(src));
                        diag_ptt_log("BASE TX OFF  after %.1f s, max power %.1f W, while TX was still commanded "
                                     "by %s (SWR protection or the BASE)",
                                     (now_time - diag_tx_start) / 1000.0f, diag_tx_max_pwr, src);
                    } else {
                        diag_ptt_log("BASE TX OFF  after %.1f s, max power %.1f W", (now_time - diag_tx_start) / 1000.0f,
                                     diag_tx_max_pwr);
                    }
                    state = RADIO_RX;
                    scheduler_msg_send(MSG_RADIO_RX, NULL);
                    recompute_display_freqs();
                } else {
                    // printf("%d, %d\n", pack->tx_power, pack->alc_level);
                    if (pack->tx_power * 0.1f > diag_tx_max_pwr) diag_tx_max_pwr = pack->tx_power * 0.1f;
                    tx_info_update(pack->tx_power * 0.1f, pack->vswr * 0.1f, pack->alc_level * 0.1f);
                }
                break;

            case RADIO_ATU_START:
                diag_tx_note(true, &diag_cmd_other);
                WITH_RADIO_LOCK(x6100_control_atu_tune(true));
                diag_tx_log("ATU TUNE", true);
                state = RADIO_ATU_WAIT;
                break;

            case RADIO_ATU_WAIT:
                if (pack->flag.tx) {
                    scheduler_msg_send(MSG_RADIO_TX, NULL);
                    state = RADIO_ATU_RUN;
                }
                break;

            case RADIO_ATU_RUN:
                if (pack->flag.atu_status && !pack->flag.tx) {
                    cfg_atu_save_network(pack->atu_params);
                    diag_tx_note(false, &diag_cmd_other);
                    WITH_RADIO_LOCK(x6100_control_atu_tune(false));
                    diag_tx_log("ATU TUNE", false);
                    param_i_set(cfg.atu_enabled(), true);
                    recover_processing_audio_inputs();
                    scheduler_msg_send(MSG_RADIO_RX, NULL);

                    // TODO: change with observer on atu->loaded change
                    WITH_RADIO_LOCK(x6100_control_cmd(x6100_atu_network, pack->atu_params));
                    state = RADIO_RX;
                    recompute_display_freqs();
                } else if (pack->flag.tx) {
                    tx_info_update(pack->tx_power * 0.1f, pack->vswr * 0.1f, pack->alc_level * 0.1f);
                }
                break;

            case RADIO_SWRSCAN:
                pthread_mutex_lock(&swrscan_cb_mux);
                if (swrscan_cb) {
                    swrscan_cb(pack->vswr * 0.1f);
                }
                pthread_mutex_unlock(&swrscan_cb_mux);
                break;

            case RADIO_POWEROFF:
                x6100_control_poweroff();
                state = RADIO_OFF;
                break;

            case RADIO_OFF:
                break;
        }

        /* TX asked for, but the BASE never went TX (relay without TX?). Not after the
         * BASE dropped a TX itself (SWR protection): that has its own TX OFF line. */
        if ((diag_cmd_ptt || diag_cmd_modem) && state == RADIO_RX && !diag_tx_seen && !diag_noresp_logged &&
            now_time - diag_cmd_time > 1500) {
            char src[32];
            diag_cmd_src_get(src, sizeof(src));
            diag_ptt_log("BASE did NOT go TX 1.5 s after the command of %s", src);
            diag_noresp_logged = true;
        }

        hkey_put(pack->hkey);
    } else {
        if (d > FLOW_RESTART_TIMEOUT) {
            LV_LOG_WARN("Flow reset");
            prev_time = now_time;
            x6100_flow_restart();
            dsp_reset();
        }
        return true;
    }
    return false;
}

static void * radio_thread(void *arg) {
    while (app_is_running) {
        now_time = get_time();

        radio_tick();

        int32_t idle = now_time - idle_time;

        if (idle > IDLE_TIMEOUT && state == RADIO_RX) {
            WITH_RADIO_LOCK(x6100_control_idle());

            idle_time = now_time;
        }
    }

    return NULL;
}
