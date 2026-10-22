/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */
#include "buttons.h"

#include "globals.h"
#include "controls.h"
#include "util.h"
#include "format.h"
#include "cfg/cfg_api.h"
#include "cfg/encoder_defaults.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <array>

extern "C" {
    #include "styles.h"
    #include "main_screen.h"
    #include "mfk.h"
    #include "vol.h"
    #include "msg.h"
    #include "panel.h"
    #include "voice.h"
    #include "pubsub_ids.h"
}

#define STATE_ASSIGNED LV_STATE_USER_1

struct _disp_button_t {
    lv_obj_t      *parent;
    lv_obj_t      *label;
    lv_obj_t      *vol_mark;
    lv_obj_t      *mfk_mark;
    button_data_t *data;  // Link to button data;
};

// Array of display buttons (size BUTTONS)
static disp_btn_t      disp_btns[BUTTONS];
static buttons_page_t *cur_page = NULL;
static std::array<char, CTRL_FAST_ACCESS_LAST> fast_binds;
static std::array<char, CTRL_LAST> binds;

static void disp_btn_refresh(disp_btn_t *b);
static void disp_btn_clear(disp_btn_t *b);

static void button_app_page_cb(button_data_t *data);
static void button_encoder_update_cb(button_data_t *data);
static void button_mem_load_cb(button_data_t *data);

static void encoder_binds_change_cb(Subject *subj, void *user_data);
static void label_update_cb(Subject *subj, void *user_data);

static void button_encoder_hold_update_cb(button_data_t *data);
static void button_mem_save_cb(button_data_t *data);

// Label getters

static const char * vol_label_getter();
static const char * sql_label_getter();
static const char * rfg_label_getter();
static const char * tx_power_label_getter();

static const char * filter_low_label_getter();
static const char * filter_high_label_getter();
static const char * filter_bw_label_getter();

static const char * mic_sel_label_getter();
static const char * h_mic_gain_label_getter();
static const char * i_mic_gain_label_getter();
static const char * moni_level_label_getter();

static const char * rit_label_getter();
static const char * xit_label_getter();

static const char * agc_hang_label_getter();
static const char * agc_knee_label_getter();
static const char * agc_slope_label_getter();
static const char * comp_label_getter();
static const char * if_shift_label_getter();

static const char * vox_on_label_getter();
static const char * vox_gain_label_getter();
static const char * vox_ag_label_getter();
static const char * vox_delay_label_getter();

static const char * key_speed_label_getter();
static const char * key_volume_label_getter();
static const char * key_train_label_getter();
static const char * key_tone_label_getter();

static const char * key_mode_label_getter();
static const char * iambic_mode_label_getter();
static const char * qsk_time_label_getter();
static const char * key_ratio_label_getter();

static const char * cw_decoder_label_getter();
static const char * cw_tuner_label_getter();
static const char * cw_snr_label_getter();

static const char * cw_peak_on_label_getter();
static const char * cw_peak_q_label_getter();

static const char * dnf_label_getter();
static const char * dnf_center_label_getter();
static const char * dnf_width_label_getter();
static const char * dnf_auto_label_getter();

static const char * nb_label_getter();
static const char * nb_level_label_getter();
static const char * nb_width_label_getter();

static const char * nr_label_getter();
static const char * nr_level_label_getter();

static void button_action_cb(button_data_t *data);

/* Make VOL/MFK button functions */
static button_data_t make_encoder_btn(const char *name, cfg_ctrl_t ctrl) {
    return button_data_t{.type            = BTN_TEXT,
                         .label           = name,
                         .press           = button_encoder_update_cb,
                         .hold            = button_encoder_hold_update_cb,
                         .ctrl            = ctrl,
                         .encoder_allowed = true};
}

static button_data_t make_encoder_btn(const char *(*label_fn)(), cfg_ctrl_t ctrl, Subject *subj = nullptr) {
    return button_data_t{.type            = BTN_TEXT_FN,
                         .label_fn        = label_fn,
                         .press           = button_encoder_update_cb,
                         .hold            = button_encoder_hold_update_cb,
                         .ctrl            = ctrl,
                         .encoder_allowed = true,
                         .subj            = subj};
}

/* Make MEM buttons functions */
static button_data_t make_mem_btn(const char *name, int32_t ctrl) {
    return button_data_t{
        .type = BTN_TEXT, .label = name, .press = button_mem_load_cb, .hold = button_mem_save_cb, .ctrl = ctrl};
}

static button_data_t make_app_btn(const char *name, press_action_t ctrl) {
    return button_data_t{.type = BTN_TEXT, .label = name, .press = button_app_page_cb, .hold = nullptr, .ctrl = ctrl};
}
static button_data_t make_action_btn(const char *name, press_action_t ctrl) {
    return button_data_t{.type = BTN_TEXT, .label = name, .press = button_action_cb, .hold = nullptr, .ctrl = ctrl};
}

static button_data_t make_page_btn(const char *name, const char *voice) {
    return button_data_t{
        .type = BTN_TEXT, .label = name, .press = button_next_page_cb, .hold = button_prev_page_cb, .voice = voice};
}

/* VOL */

static button_data_t btn_vol = {
    .type            = BTN_TEXT_FN,
    .label_fn        = vol_label_getter,
    .press           = button_encoder_update_cb,
    .ctrl            = CTRL_VOL,
    .encoder_allowed = true,
    .subj            = (Subject*)cfg.volume(),
};

static button_data_t btn_sql = make_encoder_btn(sql_label_getter, CTRL_SQL, (Subject*)cfg.squelch());
static button_data_t btn_rfg = make_encoder_btn(rfg_label_getter, CTRL_RFG, (Subject*)cfg.rfgain());
static button_data_t btn_tx_pwr = make_encoder_btn(tx_power_label_getter, CTRL_PWR, (Subject*)cfg.pwr());
static button_data_t btn_flt_low  = make_encoder_btn(filter_low_label_getter, CTRL_FILTER_LOW, (Subject*)cfg.filter.low());
static button_data_t btn_flt_high = make_encoder_btn(filter_high_label_getter, CTRL_FILTER_HIGH, (Subject*)cfg.filter.high());
static button_data_t btn_flt_bw   = make_encoder_btn(filter_bw_label_getter, CTRL_FILTER_BW, (Subject*)cfg.filter.bw());
static button_data_t btn_mic_sel   = make_encoder_btn(mic_sel_label_getter, CTRL_MIC, (Subject*)cfg.mic());
static button_data_t btn_hmic_gain = make_encoder_btn(h_mic_gain_label_getter, CTRL_HMIC, (Subject*)cfg.hmic());
static button_data_t btn_imic_hain = make_encoder_btn(i_mic_gain_label_getter, CTRL_IMIC, (Subject*)cfg.imic());
static button_data_t btn_moni_lvl  = make_encoder_btn(moni_level_label_getter, CTRL_MONI, (Subject*)cfg.moni());

/* MFK */

static button_data_t btn_zoom      = make_encoder_btn("Spectrum\nZoom", CTRL_SPECTRUM_FACTOR);
static button_data_t btn_ant       = make_encoder_btn("Antenna", CTRL_ANT);
static button_data_t btn_rit       = make_encoder_btn(rit_label_getter, CTRL_RIT, (Subject*)cfg.rit());
static button_data_t btn_xit       = make_encoder_btn(xit_label_getter, CTRL_XIT, (Subject*)cfg.xit());
static button_data_t btn_agc_hang  = {.type            = BTN_TEXT_FN,
                                      .label_fn        = agc_hang_label_getter,
                                      .press           = controls_toggle_agc_hang,
                                      .hold            = button_encoder_hold_update_cb,
                                      .ctrl            = CTRL_AGC_HANG,
                                      .encoder_allowed = true,
                                      .subj            = (Subject*)cfg.agc.hang()};
static button_data_t btn_agc_knee  = make_encoder_btn(agc_knee_label_getter, CTRL_AGC_KNEE, (Subject*)cfg.agc.knee());
static button_data_t btn_agc_slope = make_encoder_btn(agc_slope_label_getter, CTRL_AGC_SLOPE, (Subject*)cfg.agc.slope());
static button_data_t btn_comp      = make_encoder_btn(comp_label_getter, CTRL_COMP, (Subject*)cfg.dsp.comp());
static button_data_t btn_if_shift  = make_encoder_btn(if_shift_label_getter, CTRL_IF_SHIFT, (Subject*)cfg.band.if_shift());

/* VOX */
static button_data_t btn_vox_on    = {.type            = BTN_TEXT_FN,
                                      .label_fn        = vox_on_label_getter,
                                      .press           = controls_toggle_vox,
                                      .hold            = button_encoder_hold_update_cb,
                                      .ctrl            = CTRL_VOX_ON,
                                      .encoder_allowed = true,
                                      .subj            = (Subject*)cfg.vox.on()};
static button_data_t btn_vox_gain  = make_encoder_btn(vox_gain_label_getter, CTRL_VOX_GAIN, (Subject*)cfg.vox.gain());
static button_data_t btn_vox_ag    = make_encoder_btn(vox_ag_label_getter, CTRL_VOX_AG, (Subject*)cfg.vox.ag());
static button_data_t btn_vox_delay = make_encoder_btn(vox_delay_label_getter, CTRL_VOX_DELAY, (Subject*)cfg.vox.delay());

/* MEM */

static button_data_t btn_mem_1 = make_mem_btn("Set 1", 1);
static button_data_t btn_mem_2 = make_mem_btn("Set 2", 2);
static button_data_t btn_mem_3 = make_mem_btn("Set 3", 3);
static button_data_t btn_mem_4 = make_mem_btn("Set 4", 4);
static button_data_t btn_mem_5 = make_mem_btn("Set 5", 5);
static button_data_t btn_mem_6 = make_mem_btn("Set 6", 6);
static button_data_t btn_mem_7 = make_mem_btn("Set 7", 7);
static button_data_t btn_mem_8 = make_mem_btn("Set 8", 8);

/* CW */

static button_data_t btn_key_speed  = make_encoder_btn(key_speed_label_getter, CTRL_KEY_SPEED, (Subject*)cfg.cw.key_speed());
static button_data_t btn_key_volume = make_encoder_btn(key_volume_label_getter, CTRL_KEY_VOL, (Subject*)cfg.cw.key_vol());
static button_data_t btn_key_train  = {.type            = BTN_TEXT_FN,
                                       .label_fn        = key_train_label_getter,
                                       .press           = controls_toggle_key_train,
                                       .hold            = button_encoder_hold_update_cb,
                                       .ctrl            = CTRL_KEY_TRAIN,
                                       .encoder_allowed = true,
                                       .subj            = (Subject*)cfg.cw.key_train()};
static button_data_t btn_key_tone   = make_encoder_btn(key_tone_label_getter, CTRL_KEY_TONE, (Subject*)cfg.cw.key_tone());

static button_data_t btn_key_mode        = make_encoder_btn(key_mode_label_getter, CTRL_KEY_MODE, (Subject*)cfg.cw.key_mode());
static button_data_t btn_key_iambic_mode = {.type            = BTN_TEXT_FN,
                                            .label_fn        = iambic_mode_label_getter,
                                            .press           = controls_toggle_key_iambic_mode,
                                            .hold            = button_encoder_hold_update_cb,
                                            .ctrl            = CTRL_IAMBIC_MODE,
                                            .encoder_allowed = true,
                                            .subj            = (Subject*)cfg.cw.iambic_mode()};
static button_data_t btn_key_qsk_time    = make_encoder_btn(qsk_time_label_getter, CTRL_QSK_TIME, (Subject*)cfg.cw.qsk_time());
static button_data_t btn_key_ratio       = make_encoder_btn(key_ratio_label_getter, CTRL_KEY_RATIO, (Subject*)cfg.cw.key_ratio());

static button_data_t btn_cw_decoder = {.type            = BTN_TEXT_FN,
                                       .label_fn        = cw_decoder_label_getter,
                                       .press           = controls_toggle_cw_decoder,
                                       .hold            = button_encoder_hold_update_cb,
                                       .ctrl            = CTRL_CW_DECODER,
                                       .encoder_allowed = true,
                                       .subj            = (Subject*)cfg.cw.decoder()};
static button_data_t btn_cw_tuner   = {.type            = BTN_TEXT_FN,
                                       .label_fn        = cw_tuner_label_getter,
                                       .press           = controls_toggle_cw_tuner,
                                       .hold            = button_encoder_hold_update_cb,
                                       .ctrl            = CTRL_CW_TUNE,
                                       .encoder_allowed = true,
                                       .subj            = (Subject*)cfg.cw.tune()};
static button_data_t btn_cw_snr = make_encoder_btn(cw_snr_label_getter, CTRL_CW_DECODER_SNR, (Subject*)cfg.cw.decoder_snr());

static button_data_t btn_cw_peak_on = {.type            = BTN_TEXT_FN,
                                       .label_fn        = cw_peak_on_label_getter,
                                       .press           = controls_toggle_cw_peak,
                                       .hold            = button_encoder_hold_update_cb,
                                       .ctrl            = CTRL_CW_PEAK_ON,
                                       .encoder_allowed = true,
                                       .subj            = (Subject*)cfg.cw.peak_on()};
static button_data_t btn_cw_peak_q = make_encoder_btn(cw_peak_q_label_getter, CTRL_CW_PEAK_Q, (Subject*)cfg.cw.peak_q());

static button_data_t btn_cw_zap = {
    .type = BTN_TEXT, .label = "CW\nZAP", .press = controls_cw_zap, .hold = NULL, .ctrl = CTRL_CW_ZAP};

/* DSP */

static button_data_t btn_dnf        = {.type            = BTN_TEXT_FN,
                                       .label_fn        = dnf_label_getter,
                                       .press           = controls_toggle_dnf,
                                       .hold            = button_encoder_hold_update_cb,
                                       .ctrl            = CTRL_DNF,
                                       .encoder_allowed = true,
                                       .subj            = (Subject*)cfg.dsp.dnf()};
static button_data_t btn_dnf_center = make_encoder_btn(dnf_center_label_getter, CTRL_DNF_CENTER, (Subject*)cfg.dsp.dnf_center());
static button_data_t btn_dnf_width  = make_encoder_btn(dnf_width_label_getter, CTRL_DNF_WIDTH, (Subject*)cfg.dsp.dnf_width());
static button_data_t btn_dnf_auto   = {.type            = BTN_TEXT_FN,
                                       .label_fn        = dnf_auto_label_getter,
                                       .press           = controls_toggle_dnf_auto,
                                       .hold            = button_encoder_hold_update_cb,
                                       .ctrl            = CTRL_DNF_AUTO,
                                       .encoder_allowed = true,
                                       .subj            = (Subject*)cfg.dsp.dnf_auto()};

static button_data_t btn_nb       = {.type            = BTN_TEXT_FN,
                                     .label_fn        = nb_label_getter,
                                     .press           = controls_toggle_nb,
                                     .hold            = button_encoder_hold_update_cb,
                                     .ctrl            = CTRL_NB,
                                     .encoder_allowed = true,
                                     .subj            = (Subject*)cfg.dsp.nb()};
static button_data_t btn_nb_level = make_encoder_btn(nb_level_label_getter, CTRL_NB_LEVEL, (Subject*)cfg.dsp.nb_level());
static button_data_t btn_nb_width = make_encoder_btn(nb_width_label_getter, CTRL_NB_WIDTH, (Subject*)cfg.dsp.nb_width());

static button_data_t btn_nr       = {.type            = BTN_TEXT_FN,
                                     .label_fn        = nr_label_getter,
                                     .press           = controls_toggle_nr,
                                     .hold            = button_encoder_hold_update_cb,
                                     .ctrl            = CTRL_NR,
                                     .encoder_allowed = true,
                                     .subj            = (Subject*)cfg.dsp.nr()};
static button_data_t btn_nr_level = make_encoder_btn(nr_level_label_getter, CTRL_NR_LEVEL, (Subject*)cfg.dsp.nr_level());

/* APP */

static button_data_t btn_rtty = make_app_btn("RTTY", ACTION_APP_RTTY);
static button_data_t btn_ft8  = make_app_btn("FT8", ACTION_APP_FT8);
static button_data_t btn_swr  = make_app_btn("SWR\nScan", ACTION_APP_SWRSCAN);
static button_data_t btn_gps  = make_app_btn("GPS", ACTION_APP_GPS);

static button_data_t btn_rec      = make_app_btn("Recorder", ACTION_APP_RECORDER);
static button_data_t btn_qth      = make_action_btn("QTH", ACTION_APP_QTH);
static button_data_t btn_callsign = make_action_btn("Callsign", ACTION_APP_CALLSIGN);
static button_data_t btn_settings = make_app_btn("Settings", ACTION_APP_SETTINGS);

static button_data_t  btn_wifi   = make_app_btn("WiFi", ACTION_APP_WIFI);
static button_data_t  btn_bt     = make_app_btn("Bluetooth", ACTION_APP_BT);
/* The apps on their way in declare their button below their own name, one
 * app per line, so they can be merged in any order without touching each
 * other's lines. Their places on the pages are fixed below. */
/* WSPR */
/* JS8 */
/* PSK31 */
/* Olivia */
/* WeFax */
static button_data_t  btn_wefax  = make_app_btn("WeFax", ACTION_APP_WEFAX);
/* SSTV */
static button_data_t  btn_sstv   = make_app_btn("SSTV", ACTION_APP_SSTV);

/* RTTY */
static button_data_t btn_rtty_p1 = {
    .type  = BTN_TEXT,
    .label = "(RTTY 1:1)",
    .press = NULL,
};
static button_data_t btn_rtty_rate = {
    .type  = BTN_TEXT,
    .label = "Rate",
    .press = button_encoder_update_cb,
    .ctrl  = CTRL_RTTY_RATE,
    .encoder_allowed = true,
};
static button_data_t btn_rtty_shift = {
    .type  = BTN_TEXT,
    .label = "Shift",
    .press = button_encoder_update_cb,
    .ctrl  = CTRL_RTTY_SHIFT,
    .encoder_allowed = true,
};
static button_data_t btn_rtty_center = {
    .type  = BTN_TEXT,
    .label = "Center",
    .press = button_encoder_update_cb,
    .ctrl  = CTRL_RTTY_CENTER,
    .encoder_allowed = true,
};
static button_data_t btn_rtty_reverse = {
    .type  = BTN_TEXT,
    .label = "Reverse",
    .press = button_encoder_update_cb,
    .ctrl  = CTRL_RTTY_REVERSE,
    .encoder_allowed = true,
};


/* VOL pages */
static button_data_t btn_vol_p1 = make_page_btn("(VOL 1:2)", "Volume|page 1");
static button_data_t btn_vol_p2 = make_page_btn("(VOL 2:2)", "Volume|page 2");

buttons_page_t buttons_page_vol_1 = {
    {&btn_vol_p1, &btn_vol, &btn_sql, &btn_rfg, &btn_tx_pwr}
};
static buttons_page_t page_vol_2 = {
    {&btn_vol_p2, &btn_mic_sel, &btn_hmic_gain, &btn_imic_hain, &btn_moni_lvl}
};

/* MFK pages */
static button_data_t btn_mfk_p1 = make_page_btn("(MFK 1:4)", "MFK|page 1");
static button_data_t btn_mfk_p2 = make_page_btn("(MFK 2:4)", "MFK|page 2");
static button_data_t btn_mfk_p3 = make_page_btn("(MFK 3:4)", "MFK|page 3");
static button_data_t btn_mfk_p4 = make_page_btn("(MFK 4:4)", "MFK|page 4");

static buttons_page_t page_mfk_1 = {
    {&btn_mfk_p1, &btn_rit, &btn_xit, &btn_zoom, &btn_ant}
};
static buttons_page_t page_mfk_2 = {
    {&btn_mfk_p2, &btn_agc_hang, &btn_agc_knee, &btn_agc_slope, &btn_comp}
};
static buttons_page_t page_mfk_3 = {
    {&btn_mfk_p3, &btn_if_shift}
};
static buttons_page_t page_mfk_4 = {
    {&btn_mfk_p4, &btn_vox_on, &btn_vox_gain, &btn_vox_ag, &btn_vox_delay},
};

/* MEM pages */

static button_data_t btn_mem_p1 = make_page_btn("(MEM 1:2)", "Memory|page 1");
static button_data_t btn_mem_p2 = make_page_btn("(MEM 2:2)", "Memory|page 2");

static buttons_page_t page_mem_1 = {
    {&btn_mem_p1, &btn_mem_1, &btn_mem_2, &btn_mem_3, &btn_mem_4}
};
static buttons_page_t page_mem_2 = {
    {&btn_mem_p2, &btn_mem_5, &btn_mem_6, &btn_mem_7, &btn_mem_8}
};

/* KEY pages */
static button_data_t btn_key_p1 = make_page_btn("(KEY 1:2)", "Key|page 1");
static button_data_t btn_key_p2 = make_page_btn("(KEY 2:2)", "Key|page 2");
static button_data_t btn_cw_p1  = make_page_btn("(CW 1:2)", "CW|page 1");
static button_data_t btn_cw_p2  = make_page_btn("(CW 2:2)", "CW|page 2");

static buttons_page_t page_key_1 = {
    {&btn_key_p1, &btn_key_speed, &btn_key_volume, &btn_key_train, &btn_key_tone}
};
static buttons_page_t page_key_2 = {
    {&btn_key_p2, &btn_key_mode, &btn_key_iambic_mode, &btn_key_qsk_time, &btn_key_ratio}
};
static buttons_page_t page_cw_decoder_1 = {
    {&btn_cw_p1, &btn_cw_zap, &btn_cw_peak_on, &btn_cw_peak_q}
};
static buttons_page_t page_cw_decoder_2 = {
    {&btn_cw_p2, &btn_cw_decoder, &btn_cw_tuner, &btn_cw_snr}
};

/* DFN pages */
static button_data_t btn_dfn_p1 = make_page_btn("(DFN 1:3)", "DNF page");
static button_data_t btn_dfn_p2 = make_page_btn("(DFN 2:3)", "NB page");
static button_data_t btn_dfn_p3 = make_page_btn("(DFN 3:3)", "NR page");

static buttons_page_t page_dfn_1 = {
    {&btn_dfn_p1, &btn_dnf, &btn_dnf_center, &btn_dnf_width, &btn_dnf_auto}
};
static buttons_page_t page_dfn_2 = {
    {&btn_dfn_p2, &btn_nb, &btn_nb_level, &btn_nb_width}
};
static buttons_page_t page_dfn_3 = {
    {&btn_dfn_p3, &btn_nr, &btn_nr_level}
};

/* DFL pages */
static buttons_page_t page_dfl_1 = {
    {&btn_flt_low, &btn_flt_high, &btn_flt_bw}
};

/* App pages */
static button_data_t btn_app_p1 = make_page_btn("(APP 1:4)", "Application|page 1");
static button_data_t btn_app_p2 = make_page_btn("(APP 2:4)", "Application|page 2");
static button_data_t btn_app_p3 = make_page_btn("(APP 3:4)", "Application|page 3");
static button_data_t btn_app_p4 = make_page_btn("(APP 4:4)", "Application|page 4");

static buttons_page_t page_app_1 = {
    {&btn_app_p1, &btn_rtty, &btn_ft8, &btn_swr, &btn_gps}
};
static buttons_page_t page_app_2 = {
    {&btn_app_p2, &btn_rec, &btn_qth, &btn_callsign, &btn_settings}
};
/* Pages 3 and 4: every app has its fixed place, kept empty (NULL) until the
 * app comes in. Each app only replaces its own NULL, so the order on the
 * radio never changes whichever app is merged first. */
static buttons_page_t page_app_3 = {{
    &btn_app_p3,
    &btn_wifi,
    &btn_bt,
    /* WSPR */
    NULL,
    /* JS8 */
    NULL,
}};
static buttons_page_t page_app_4 = {{
    &btn_app_p4,
    /* PSK31 */
    NULL,
    /* Olivia */
    NULL,
    /* WeFax */
    &btn_wefax,
    /* SSTV */
    &btn_sstv,
}};

/* RTTY */

buttons_page_t buttons_page_rtty = {
    {&btn_rtty_p1, &btn_rtty_rate, &btn_rtty_shift, &btn_rtty_center, &btn_rtty_reverse}
};

buttons_group_t buttons_group_gen = {
    &buttons_page_vol_1,
    &page_vol_2,
    &page_mfk_1,
    &page_mfk_2,
    &page_mfk_3,
    &page_mfk_4,
};

buttons_group_t buttons_group_app = {
    &page_app_1,
    &page_app_2,
    &page_app_3,
    &page_app_4,
};

buttons_group_t buttons_group_key = {
    &page_key_1,
    &page_key_2,
    &page_cw_decoder_1,
    &page_cw_decoder_2,
};

buttons_group_t buttons_group_dfn = {
    &page_dfn_1,
    &page_dfn_2,
    &page_dfn_3,
};

buttons_group_t buttons_group_dfl = {
    &page_dfl_1,
};

buttons_group_t buttons_group_vm = {
    &page_mem_1,
    &page_mem_2,
};

static struct {
    buttons_page_t **group;
    size_t           size;
} groups[] = {
    {buttons_group_gen, ARRAY_SIZE(buttons_group_gen)},
    {buttons_group_app, ARRAY_SIZE(buttons_group_app)},
    {buttons_group_key, ARRAY_SIZE(buttons_group_key)},
    {buttons_group_dfn, ARRAY_SIZE(buttons_group_dfn)},
    {buttons_group_dfl, ARRAY_SIZE(buttons_group_dfl)},
    {buttons_group_vm,  ARRAY_SIZE(buttons_group_vm) },
};

void buttons_init(lv_obj_t *parent) {

    if (x6100_control_get_base_ver().rev < 3) {
        // Hide DNF auto button
        page_dfn_1.items[4] = NULL;
    }

    /* Fill prev/next pointers */
    for (size_t i = 0; i < ARRAY_SIZE(groups); i++) {
        buttons_page_t **group = groups[i].group;
        for (size_t j = 0; j < groups[i].size; j++) {
            if (group[j]->items[0]->press == button_next_page_cb) {
                uint16_t next_id = (j + 1) % groups[i].size;
                group[j]->items[0]->next = group[next_id];
            } else {
                LV_LOG_USER("First button in page=%u, group=%u press cb is not next", j, i);
            }
            if (group[j]->items[0]->hold == button_prev_page_cb) {
                uint16_t prev_id = (groups[i].size + j - 1) % groups[i].size;
                group[j]->items[0]->prev = group[prev_id];
            } else {
                LV_LOG_USER("First button in page=%u, group=%u hold cb is not prev", j, i);
            }
        }
    }

    /* Update default binds */
    binds.fill(ENCODER_BIND_MFK);
    for (cfg_ctrl_t ctrl : kEncoderVolDefaults) {
        binds[ctrl] = ENCODER_BIND_VOL;
    }

    uint16_t y = SCREEN_HEIGHT - BTN_HEIGHT;
    uint16_t x = 0;

    for (uint8_t i = 0; i < 5; i++) {
        lv_obj_t *f = lv_obj_create(parent);
        disp_btns[i].parent = f;
        lv_obj_add_flag(f, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        lv_obj_remove_style_all(f);
        lv_obj_add_style(f, &style.btn.base, 0);
        lv_obj_add_style(f, &style.btn.active, LV_STATE_CHECKED);
        lv_obj_add_style(f, &style.btn.disabled, LV_STATE_DISABLED);
        // lv_obj_add_style(f, &style.btn.mark_assigned, STATE_ASSIGNED);

        lv_obj_set_pos(f, x, y);
        lv_obj_set_size(f, BTN_WIDTH, BTN_HEIGHT);
        x += BTN_WIDTH;

        /* Encoder marks */
        lv_obj_t *enc_mark;

        enc_mark = lv_obj_create(f);
        lv_obj_set_pos(enc_mark, 5, 5);
        lv_obj_clear_flag(enc_mark, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(enc_mark, &style.btn.mark, 0);
        lv_obj_add_style(enc_mark, &style.btn.mark_assigned, STATE_ASSIGNED);

        disp_btns[i].vol_mark = enc_mark;


        enc_mark = lv_obj_create(f);
        lv_obj_set_pos(enc_mark, 5, BTN_HEIGHT - 5 - 24);
        lv_obj_clear_flag(enc_mark, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_add_style(enc_mark, &style.btn.mark, 0);
        lv_obj_add_style(enc_mark, &style.btn.mark_assigned, STATE_ASSIGNED);
        disp_btns[i].mfk_mark = enc_mark;

        /* Label */
        lv_obj_t *label = lv_label_create(f);

        lv_obj_center(label);
        lv_obj_set_user_data(f, label);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

        disp_btns[i].label = label;
    }

    cfg.encoder.bind()->subscribe_delayed_and_notify(encoder_binds_change_cb);
}

void buttons_refresh(button_data_t *data) {
    if (data->disp_btn) {
        disp_btn_refresh(data->disp_btn);
    } else {
        LV_LOG_WARN("Button data label obj is null");
    }
}

void buttons_mark(button_data_t *data, bool val) {
    if (!data) {
        LV_LOG_INFO("Button data is null, skip mark");
        return;
    }
    data->mark = val;
    if (data->disp_btn) {
        disp_btn_refresh(data->disp_btn);
    }
}

void buttons_disabled(button_data_t *data, bool val) {
    data->disabled = val;
    if (data->disp_btn) {
        disp_btn_refresh(data->disp_btn);
    }
}

void buttons_load(uint8_t n, button_data_t *data) {
    button_data_t *prev_data = disp_btns[n].data;
    disp_btn_clear(disp_btns + n);

    lv_obj_t *label = disp_btns[n].label;
    disp_btns[n].data = data;
    if (data) {
        data->disp_btn = disp_btns + n;
        disp_btn_refresh(data->disp_btn);
    } else {
        lv_label_set_text(label, "");
    }
}

void buttons_load_page(buttons_page_t *page) {
    if (!page) {
        LV_LOG_ERROR("NULL pointer to buttons page");
        return;
    }
    if (cur_page) {
        buttons_unload_page();
    }
    cur_page = page;
    for (uint8_t i = 0; i < BUTTONS; i++) {
        buttons_load(i, page->items[i]);
    }
    if (page->items[0]->voice) {
        voice_say_text_fmt("%s", page->items[0]->voice);
    }
}

void buttons_unload_page() {
    cur_page = NULL;
    for (uint8_t i = 0; i < BUTTONS; i++) {
        disp_btn_clear(&disp_btns[i]);
    }
}

void button_next_page_cb(button_data_t *data) {
    buttons_unload_page();
    buttons_load_page(data->next);
}

void button_prev_page_cb(button_data_t *data) {
    buttons_unload_page();
    buttons_load_page(data->prev);
}

static void button_app_page_cb(button_data_t *data) {
    main_screen_start_app((press_action_t)data->ctrl);
}

static void button_action_cb(button_data_t *data) {
    main_screen_action((press_action_t)data->ctrl);
}


static void button_encoder_update_cb(button_data_t *data) {
    // set corresponding encoder
    // if already bind - use corresponding encoder. If no - you default
    cfg_ctrl_t ctrl = (cfg_ctrl_t)data->ctrl;
    std::string binds_str = cfg.encoder.bind()->get();

    void (*set_fn)(cfg_ctrl_t) = NULL;
    if (binds_str.length() > ctrl){
        switch (binds[ctrl]) {
            case ENCODER_BIND_VOL:
                set_fn = vol_set_ctrl;
                break;
            case ENCODER_BIND_MFK:
                set_fn = mfk_set_ctrl;
                break;
        default: ;
            if (std::find(kEncoderVolDefaults.begin(), kEncoderVolDefaults.end(), ctrl)
                != kEncoderVolDefaults.end()) {
                set_fn = vol_set_ctrl;
            } else if (std::find(kEncoderMfkDefaults.begin(), kEncoderMfkDefaults.end(), ctrl)
                       != kEncoderMfkDefaults.end()) {
                set_fn = mfk_set_ctrl;
            }
            break;
        }
    } else {
        set_fn = mfk_set_ctrl;
    }
    if (set_fn) {
        set_fn(ctrl);
    }
}

static void button_encoder_hold_update_cb(button_data_t *data) {
    cfg_ctrl_t ctrl = (cfg_ctrl_t)data->ctrl;
    std::string binds = cfg.encoder.bind()->get();
    if (binds.length() > ctrl){
        switch (binds[ctrl]) {
            case ENCODER_BIND_NONE:
                msg_update_text_fmt("Added to VOL encoder");
                voice_say_text_fmt("Added to volume encoder");
                binds[ctrl] = ENCODER_BIND_VOL;
                break;
            case ENCODER_BIND_VOL:
                msg_update_text_fmt("Moved to MFK encoder");
                voice_say_text_fmt("Moved to MFK encoder");
                binds[ctrl] = ENCODER_BIND_MFK;
                break;
            case ENCODER_BIND_MFK:
                msg_update_text_fmt("Removed from MFK encoder");
                voice_say_text_fmt("Removed from MFK encoder");
                binds[ctrl] = ENCODER_BIND_NONE;
                break;
            default:
                binds[ctrl] = ENCODER_BIND_NONE;
                LV_LOG_WARN("Unexpected mode: %c for ctrl %d", binds[ctrl], ctrl);
                break;
        }
        cfg.encoder.bind()->set(binds);
    } else {
        LV_LOG_ERROR("Binds is too short (%d) for ctrl %c", binds.length(), binds.c_str());
    }
}

static void button_mem_load_cb(button_data_t *data) {
    mem_load(data->ctrl);
    voice_say_text_fmt("Memory %i loaded", data->ctrl);
}

static void button_mem_save_cb(button_data_t *data) {
    mem_save(data->ctrl);
    voice_say_text_fmt("Memory %i stored", data->ctrl);
}

void buttons_press(uint8_t n, bool hold) {
    button_data_t *btn_data = disp_btns[n].data;
    if (btn_data == NULL) {
        LV_LOG_WARN("Button %u is NULL", n);
        return;
    }
    if (btn_data->disabled) {
        LV_LOG_USER("Button %s disabled", lv_label_get_text(btn_data->disp_btn->label));
        return;
    }
    if (hold) {
        if (btn_data->hold) {
            btn_data->hold(btn_data);
        } else {
            LV_LOG_USER("Button %u hold action is NULL", n);
        }
    } else {
        if (btn_data->press) {
            btn_data->press(btn_data);
        } else {
            LV_LOG_USER("Button %u press action is NULL", n);
        }
    }
}

void buttons_load_page_group(buttons_group_t group) {
    size_t group_size = 0;
    for (size_t i = 0; i < ARRAY_SIZE(groups); i++) {
        if (groups[i].group == group) {
            group_size = groups[i].size;
            break;
        }
    }
    if (group_size <= 0) {
        return;
    }
    for (size_t i = 0; i < group_size; i++) {
        if ((group[i] == cur_page) && (cur_page->items[0]->next)) {
            // load next
            cur_page->items[0]->press(cur_page->items[0]);
            return;
        }
    }
    // Load first
    buttons_load_page(group[0]);
}

buttons_page_t *buttons_get_cur_page() {
    return cur_page;
}

static const char * vol_label_getter() {
    static char buf[16];
    sprintf(buf, "Volume:\n%i", cfg.volume()->get());
    return buf;
}

static const char * sql_label_getter() {
    static char buf[16];
    sprintf(buf, "Squelch:\n%i", cfg.squelch()->get());
    return buf;
}

static const char * rfg_label_getter() {
    static char buf[16];
    sprintf(buf, "RF gain:\n%i", cfg.rfgain()->get());
    return buf;
}

static const char * tx_power_label_getter() {
    static char buf[20];
    sprintf(buf, "TX power:\n%0.1f W", cfg.pwr()->get());
    return buf;
}

static const char * filter_low_label_getter() {
    static char buf[22];
    sprintf(buf, "Filter low:\n%i Hz", cfg.filter.low()->get());
    return buf;
}
static const char * filter_high_label_getter() {
    static char buf[22];
    sprintf(buf, "Filter high:\n%i Hz", cfg.filter.high()->get());
    return buf;
}

static const char * filter_bw_label_getter() {
    static char buf[22];
    sprintf(buf, "Filter BW:\n%i Hz", cfg.filter.bw()->get());
    return buf;
}


static const char * mic_sel_label_getter() {
    static char buf[22];
    sprintf(buf, "MIC Sel:\n%s", format_mic_str_get((x6100_mic_sel_t)cfg.mic()->get()));
    return buf;
}


static const char * h_mic_gain_label_getter() {
    static char buf[22];
    sprintf(buf, "H-Mic gain:\n%i", cfg.hmic()->get());
    return buf;
}

static const char * i_mic_gain_label_getter() {
    static char buf[22];
    sprintf(buf, "I-Mic gain:\n%i", cfg.imic()->get());
    return buf;
}

static const char * moni_level_label_getter() {
    static char buf[22];
    sprintf(buf, "Moni level:\n%i", cfg.moni()->get());
    return buf;
}


static const char * rit_label_getter() {
    static char buf[22];
    sprintf(buf, "RIT:\n%+i", cfg.rit()->get());
    return buf;
}

static const char * xit_label_getter() {
    static char buf[22];
    sprintf(buf, "XIT:\n%+i", cfg.xit()->get());
    return buf;
}

static const char * agc_hang_label_getter() {
    static char buf[22];
    sprintf(buf, "AGC hang:\n%s", cfg.agc.hang()->get() ? "On": "Off");
    return buf;
}

static const char * agc_knee_label_getter() {
    static char buf[22];
    sprintf(buf, "AGC knee:\n%i dB", cfg.agc.knee()->get());
    return buf;
}

static const char * agc_slope_label_getter() {
    static char buf[22];
    sprintf(buf, "AGC slope:\n%i dB", cfg.agc.slope()->get());
    return buf;
}

static const char * comp_label_getter() {
    static char buf[22];
    sprintf(buf, "Comp:\n%s", format_comp_str_get(cfg.dsp.comp()->get()));
    return buf;
}

static const char * if_shift_label_getter() {
    static char buf[22];
    sprintf(buf, "IF shift:\n%i", cfg.band.if_shift()->get());
    return buf;
}

const char *vox_on_label_getter() {
    static char buf[22];
    sprintf(buf, "VOX:\n%s", cfg.vox.on()->get() ? "On": "Off");
    return buf;
}

const char *vox_gain_label_getter() {
    static char buf[22];
    sprintf(buf, "VOX gain:\n%i", cfg.vox.gain()->get());
    return buf;
}

const char *vox_ag_label_getter() {
    static char buf[22];
    sprintf(buf, "VOX a-gain:\n%i", cfg.vox.ag()->get());
    return buf;
}

const char *vox_delay_label_getter() {
    static char buf[22];
    sprintf(buf, "VOX delay:\n%i ms", cfg.vox.delay()->get());
    return buf;
}

static const char * key_speed_label_getter() {
    static char buf[22];
    sprintf(buf, "Speed:\n%i wpm", cfg.cw.key_speed()->get());
    return buf;
}

static const char * key_volume_label_getter() {
    static char buf[22];
    sprintf(buf, "Volume:\n%i", cfg.cw.key_vol()->get());
    return buf;
}

static const char * key_train_label_getter() {
    static char buf[22];
    sprintf(buf, "Train:\n%s", cfg.cw.key_train()->get() ? "On": "Off");
    return buf;
}

static const char * key_tone_label_getter() {
    static char buf[22];
    sprintf(buf, "Tone:\n%i Hz", cfg.cw.key_tone()->get());
    return buf;
}

static const char * key_mode_label_getter() {
    static char buf[22];
    sprintf(buf, "Mode:\n%s", format_key_mode_str_get((x6100_key_mode_t)cfg.cw.key_mode()->get()));
    return buf;
}

static const char * iambic_mode_label_getter() {
    static char buf[22];
    sprintf(buf, "Iambic:\n%s mode", format_iambic_mode_str_get((x6100_iambic_mode_t)cfg.cw.iambic_mode()->get()));
    return buf;
}

static const char * qsk_time_label_getter() {
    static char buf[22];
    sprintf(buf, "QSK time:\n%i ms", cfg.cw.qsk_time()->get());
    return buf;
}

static const char * key_ratio_label_getter() {
    static char buf[22];
    sprintf(buf, "Ratio:\n%0.1f", cfg.cw.key_ratio()->get());
    return buf;
}

static const char * cw_decoder_label_getter() {
    static char buf[22];
    sprintf(buf, "Decoder:\n%s", cfg.cw.decoder()->get() ? "On": "Off");
    return buf;
}

static const char * cw_tuner_label_getter() {
    static char buf[22];
    sprintf(buf, "Tuner:\n%s", cfg.cw.tune()->get() ? "On": "Off");
    return buf;
}

static const char * cw_snr_label_getter() {
    static char buf[22];
    sprintf(buf, "Dec SNR:\n%0.1f dB", cfg.cw.decoder_snr()->get());
    return buf;
}

static const char * cw_peak_on_label_getter() {
    static char buf[22];
    sprintf(buf, "CW peak:\n%s", cfg.cw.peak_on()->get() ? "On": "Off");
    return buf;
}

static const char * cw_peak_q_label_getter() {
    static char buf[22];
    sprintf(buf, "CW peak Q:\n%i", cfg.cw.peak_q()->get());
    return buf;
}

static const char * dnf_label_getter() {
    static char buf[22];
    sprintf(buf, "DNF:\n%s", cfg.dsp.dnf()->get() ? "On": "Off");
    return buf;
}

static const char * dnf_center_label_getter() {
    static char buf[22];
    sprintf(buf, "DNF freq:\n%i Hz", cfg.dsp.dnf_center()->get());
    return buf;
}

static const char * dnf_width_label_getter() {
    static char buf[22];
    sprintf(buf, "DNF width:\n%i Hz", cfg.dsp.dnf_width()->get());
    return buf;
}

static const char * dnf_auto_label_getter() {
    static char buf[22];
    sprintf(buf, "DNF auto:\n%s", cfg.dsp.dnf_auto()->get() ? "On": "Off");
    return buf;
}

static const char * nb_label_getter() {
    static char buf[22];
    sprintf(buf, "NB:\n%s", cfg.dsp.nb()->get() ? "On": "Off");
    return buf;
}

static const char * nb_level_label_getter() {
    static char buf[22];
    sprintf(buf, "NB level:\n%i", cfg.dsp.nb_level()->get());
    return buf;
}

static const char * nb_width_label_getter() {
    static char buf[22];
    sprintf(buf, "NB width:\n%i Hz", cfg.dsp.nb_width()->get());
    return buf;
}

static const char * nr_label_getter() {
    static char buf[22];
    sprintf(buf, "NR:\n%s", cfg.dsp.nr()->get() ? "On": "Off");
    return buf;
}

static const char * nr_level_label_getter() {
    static char buf[22];
    sprintf(buf, "NR level:\n%i", cfg.dsp.nr_level()->get());
    return buf;
}


static void encoder_binds_change_cb(Subject *subj, void *user_data) {
    std::string binds = cfg.encoder.bind()->get();

    for (size_t i = 0; i < binds.length(); i++) {
        fast_binds[i] = binds[i];
    }

    for (size_t i = 0; i < BUTTONS; i++)
    {
        disp_btn_t *b = disp_btns + i;
        disp_btn_refresh(b);
    }
}

static void label_update_cb(Subject *subj, void *user_data) {
    button_data_t *btn_data = (button_data_t*)user_data;
    if (btn_data->disp_btn) {
        lv_label_set_text(btn_data->disp_btn->label, btn_data->label_fn());
    } else {
        LV_LOG_WARN("Can't update label: it's NULL");
    }
}


static void disp_btn_refresh(disp_btn_t *b) {
    button_data_t *btn_data = b->data;
    if (!btn_data) {
        LV_LOG_WARN("Button has no assigned data");
        return;
    }

    /* Label */

    if (btn_data->type == BTN_TEXT) {
        // Copy label
        lv_label_set_text(b->label, btn_data->label);
    } else if (btn_data->type == BTN_TEXT_FN) {
        // Update label with btn_data::label_fn
        lv_label_set_text(b->label, btn_data->label_fn());
        if (btn_data->subj) {
            btn_data->observer = btn_data->subj->subscribe_delayed(label_update_cb, btn_data);
        } else {
            lv_obj_set_user_data(b->label, (void *)btn_data->label_fn);
        }
    } else {
        lv_label_set_text(b->label, "");
    }

    /* Marked button */

    if (btn_data->mark) {
        lv_obj_add_state(b->parent, LV_STATE_CHECKED);

    } else {
        lv_obj_clear_state(b->parent, LV_STATE_CHECKED);
    }

    /* Disabled button */

    if (btn_data->disabled) {
        lv_obj_add_state(b->parent, LV_STATE_DISABLED);

    } else {
        lv_obj_clear_state(b->parent, LV_STATE_DISABLED);
    }

    /* Encoder */

    if ((btn_data->ctrl >= 0) && btn_data->encoder_allowed) {
        cfg_ctrl_t ctrl = (cfg_ctrl_t)btn_data->ctrl;
        encoder_binds_t encoder      = ENCODER_BIND_NONE,
                        fast_encoder = ENCODER_BIND_NONE;

        if (ctrl < std::size(fast_binds)) {
            fast_encoder = (encoder_binds_t)fast_binds[ctrl];
        }
        if (fast_encoder != ENCODER_BIND_NONE) {
            encoder = fast_encoder;
            lv_obj_add_state(b->vol_mark, STATE_ASSIGNED);
            lv_obj_add_state(b->mfk_mark, STATE_ASSIGNED);
        } else {
            lv_obj_clear_state(b->vol_mark, STATE_ASSIGNED);
            lv_obj_clear_state(b->mfk_mark, STATE_ASSIGNED);
            if ((ctrl < std::size(binds)) && (btn_data->press == button_encoder_update_cb)) {
                encoder = (encoder_binds_t)binds[ctrl];
            }
        }
        lv_obj_t *active_encoder_marker = NULL;
        switch (encoder) {
            case ENCODER_BIND_VOL:
                lv_obj_clear_flag(b->vol_mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(b->mfk_mark, LV_OBJ_FLAG_HIDDEN);
                break;
            case ENCODER_BIND_MFK:
                lv_obj_add_flag(b->vol_mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(b->mfk_mark, LV_OBJ_FLAG_HIDDEN);
                break;
            default:
                lv_obj_add_flag(b->vol_mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(b->mfk_mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_state(b->vol_mark, STATE_ASSIGNED);
                lv_obj_clear_state(b->mfk_mark, STATE_ASSIGNED);
                break;
        }
    } else {
        lv_obj_add_flag(b->vol_mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(b->mfk_mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(b->vol_mark, STATE_ASSIGNED);
        lv_obj_clear_state(b->mfk_mark, STATE_ASSIGNED);
    }
}


static void disp_btn_clear(disp_btn_t *btn) {
    lv_obj_t *label = btn->label;

    lv_label_set_text(label, "");
    lv_obj_set_user_data(label, NULL);
    lv_obj_clear_state(btn->parent, LV_STATE_CHECKED);
    lv_obj_clear_state(btn->parent, LV_STATE_DISABLED);

    lv_obj_clear_state(btn->vol_mark, STATE_ASSIGNED);
    lv_obj_clear_state(btn->mfk_mark, STATE_ASSIGNED);
    lv_obj_add_flag(btn->vol_mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn->mfk_mark, LV_OBJ_FLAG_HIDDEN);

    if (btn->data) {
        // Unbind data from display button
        btn->data->disp_btn = NULL;
        if (btn->data->observer) {
            // unsubscribe() releases the Subject's reference; the observer is
            // destroyed here because no other reference is held. It must not be
            // deleted again.
            btn->data->observer->unsubscribe();
            btn->data->observer = NULL;
        }
        btn->data = NULL;
    }
}
