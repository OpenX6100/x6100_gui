/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "main_screen.h"

#include <math.h>

#include "globals.h"
#include "styles.h"
#include "spectrum.h"
#include "waterfall.h"
#include "util.h"
#include "radio.h"
#include "events.h"
#include "msg.h"
#include "msg_tiny.h"
#include "dsp.h"
#include "clock.h"
#include "cw_tune_ui.h"
#include "meter.h"
#include "band_info.h"
#include "tx_info.h"
#include "mfk.h"
#include "vol.h"
#include "main.h"
#include "panel.h"
#include "rtty.h"
#include "screenshot.h"
#include "keyboard.h"
#include "dialog.h"
#include "settings/dialog_settings.h"
#include "dialog_freq.h"
#include "dialog_msg_cw.h"
#include "dialog_msg_voice.h"
#include "dialog_swrscan.h"
#include "dialog_ft8.h"
#include "dialog_gps.h"
#include "dialog_qth.h"
#include "dialog_recorder.h"
#include "dialog_callsign.h"
#include "dialog_wifi.h"
#include "dialog_bt.h"
/* The apps on their way in: each one's include below its name */
/* WSPR */
/* JS8 */
#include "dialog_js8.h"
/* PSK31 */
/* Olivia */
/* WeFax */
/* SSTV */
#include "display.h"
#include "buttons.h"
#include "recorder.h"
#include "voice.h"
#include "pubsub_ids.h"
#include "cfg/cfg_api.h"
#include "cfg/memory.h"
#include "knobs.h"
#include "indicators.h"
#include "freq_info.h"
#include "lock_manager.h"
#include "diag_log.h"

#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>


static lv_obj_t     *obj;

static lv_obj_t     *top_container;
static uint32_t     top_container_index;
static lv_obj_t     *spectrum;
static lv_obj_t     *waterfall;
static lv_obj_t     *freq_bounds[2];
static lv_obj_t     *meter;
static lv_obj_t     *tx_info;
static lv_obj_t     *knobs;

static bool dialog_running = false;

// power off on low battery
static lv_timer_t *low_power_timer;

static void low_power_timer_cb(lv_timer_t * timer);

static void freq_shift(int16_t diff, uint16_t dt);
static void next_freq_step(bool up);
static void toggle_atu_enabled();

static void keypad_pre(const event_keypad_t *kp);
static void keypad_band(const event_keypad_t *kp, bool up);
static void keypad_mode(const event_keypad_t *kp);
static void keypad_agc(const event_keypad_t *kp);
static void keypad_fst(const event_keypad_t *kp);
static void keypad_atu(const event_keypad_t *kp);
static void keypad_fkey(const event_keypad_t *kp, uint8_t idx);
static void keypad_group_page(const event_keypad_t *kp, ParamInt *long_action, buttons_group_t group,
                              const char *voice);
static void keypad_msg(const event_keypad_t *kp);
static void keypad_ab(const event_keypad_t *kp);
static void keypad_power(const event_keypad_t *kp);
static void keypad_lock(const event_keypad_t *kp);
static void keypad_ptt(const event_keypad_t *kp);
static void keypad_vm(const event_keypad_t *kp);

// Observers functions

static void on_fg_freq_change(Subject *subj, void *user_data);
static void update_freq_boundaries(Subject *subj, void *user_data);
static void update_zoom_on_if_shift_change(Subject *subj, void *user_data);

static void apply_main_layout(void);
static void on_spectrum_height_change(Subject *subj, void *user_data);

static void lock_freq_cb(void * s, lv_msg_t * msg);
static void on_dialog_start_cb(void *s, lv_msg_t *m);
static void on_dialog_stop_cb(void *s, lv_msg_t *m);

void mem_load(uint16_t id) {
    if (!cfg_memory_load(id)) {
        msg_update_text_fmt("Nothing to load for memory %i", id);
    }
    if (id != MEM_BACKUP_ID) {
        msg_update_text_fmt("Loaded from memory %i", id);
    }
}

void mem_save(uint16_t id) {
    cfg_memory_save(id);

    if (id <= MEM_HKEY_MAX_ID) {
        msg_update_text_fmt("Saved in memory %i", id);
    }
}

static void low_power_timer_cb(lv_timer_t * timer) {
    low_power_timer = NULL;
    msg_update_text_fmt("Power off");
    radio_set_charger(true);
    radio_poweroff();
}

static void toggle_atu_enabled() {
    bool new_atu_enabled = !param_i_get(cfg.atu_enabled());
    param_i_set(cfg.atu_enabled(), new_atu_enabled);
    voice_say_text_fmt("Auto tuner %s", new_atu_enabled ? "On" : "Off");
}

static void next_freq_step(bool up) {
    uint16_t new_step = cfg_mode_change_freq_step(up);
    msg_update_text_fmt("Freq step: %i Hz", new_step);
    voice_say_text_fmt("Frequency step %i herz", new_step);
}

static void apps_disable() {
    dialog_destruct();

    rtty_set_state(RTTY_OFF);
    panel_update_visibility(false);
}

void main_screen_start_app(press_action_t app_action) {
    apps_disable();

    switch (app_action) {
        case ACTION_APP_RTTY:
            buttons_load_page(&buttons_page_rtty);
            rtty_set_state(RTTY_RX);
            panel_update_visibility(true);
            voice_say_text_fmt("Teletype window");
            break;

        case ACTION_APP_SETTINGS:
            dialog_construct(dialog_settings, obj);
            voice_say_text_fmt("Settings window");
            break;

        case ACTION_APP_SWRSCAN:
            dialog_construct(dialog_swrscan, obj);
            voice_say_text_fmt("SWR scan window");
            break;

        case ACTION_APP_FT8:
            dialog_construct(dialog_ft8, obj);
            voice_say_text_fmt("FT8 window");
            break;

        case ACTION_APP_GPS:
            dialog_construct(dialog_gps, obj);
            voice_say_text_fmt("GPS window");
            break;

        case ACTION_APP_RECORDER:
            dialog_construct(dialog_recorder, obj);
            voice_say_text_fmt("Audio recorder window");
            break;

        case ACTION_APP_WIFI:
            dialog_construct(dialog_wifi, obj);
            voice_say_text_fmt("Wi-Fi window");
            break;

        case ACTION_APP_BT:
            dialog_construct(dialog_bt, obj);
            voice_say_text_fmt("Bluetooth window");
            break;

        /* The apps on their way in: each one's case below its name */
        /* WSPR */
        /* JS8 */
        case ACTION_APP_JS8:
            dialog_construct(dialog_js8, obj);
            voice_say_text_fmt("JS8 window");
            break;

        /* PSK31 */
        /* Olivia */
        /* WeFax */
        /* SSTV */
        default:
            break;
    }
}

void main_screen_action(press_action_t action) {
    bool b;
    switch (action) {
        case ACTION_NONE:
            break;

        case ACTION_SCREENSHOT:
            screenshot_take();
            break;

        case ACTION_RECORDER:
            if (recorder_is_on()) {
                recorder_set_on(false);
                voice_say_text_fmt("Audio recorder off");
            } else {
                voice_say_text_fmt("Audio recorder on");
                recorder_set_on(true);
            }
            break;

        case ACTION_MUTE:
            radio_change_mute();
            break;

        case ACTION_VOICE_MODE:
            voice_change_mode();
            break;

        case ACTION_BAT_INFO:
            clock_say_bat_info();
            break;

        case ACTION_STEP_UP:
            next_freq_step(true);
            break;

        case ACTION_STEP_DOWN:
            next_freq_step(false);
            break;

        case ACTION_NR_TOGGLE:
            b = param_i_get(cfg.dsp.nr());
            b = !b;
            param_i_set(cfg.dsp.nr(), b);
            msg_update_text_fmt("#FFFFFF NR: %s", b ? "On" : "Off");
            break;

        case ACTION_NB_TOGGLE:
            b = param_i_get(cfg.dsp.nb());
            b = !b;
            param_i_set(cfg.dsp.nb(), b);
            msg_update_text_fmt("#FFFFFF NB: %s", b ? "On" : "Off");
            break;

        case ACTION_APP_RTTY:
        case ACTION_APP_FT8:
        case ACTION_APP_SWRSCAN:
        case ACTION_APP_GPS:
        case ACTION_APP_SETTINGS:
        case ACTION_APP_RECORDER:
        case ACTION_APP_WIFI:
        case ACTION_APP_BT:
        /* WSPR */
        /* JS8 */
        case ACTION_APP_JS8:
        /* PSK31 */
        /* Olivia */
        /* WeFax */
        /* SSTV */
            main_screen_start_app(action);
            break;

        case ACTION_APP_QTH:
            dialog_construct(dialog_qth, obj);
            voice_say_text_fmt("QTH window");
            break;

        case ACTION_APP_CALLSIGN:
            dialog_construct(dialog_callsign, obj);
            voice_say_text_fmt("Callsign window");
            break;
    }
}

static x6100_mode_t get_next_mode_am_fm(bool long_press) {
    x6100_mode_t    mode = cparam_i_get(cfg.cur.mode());
    switch (mode) {
        case x6100_mode_am:
            mode = x6100_mode_nfm;
            break;
        case x6100_mode_nfm:
        default:
            mode = x6100_mode_am;
            break;
    }
    return mode;
}

static x6100_mode_t get_next_mode_cw(bool long_press) {
    x6100_mode_t    mode = cparam_i_get(cfg.cur.mode());
    switch (mode) {
        case x6100_mode_cw:
            mode = x6100_mode_cwr;
            break;
        case x6100_mode_cwr:
        default:
            mode = x6100_mode_cw;
            break;
    }
    return mode;
}

static x6100_mode_t get_next_mode_ssb(bool long_press) {
    x6100_mode_t    mode = cparam_i_get(cfg.cur.mode());
    switch (mode) {
        case x6100_mode_lsb_dig:
            if (long_press) {
                mode = x6100_mode_lsb;
            } else {
                mode = x6100_mode_usb_dig;
            }
            break;
        case x6100_mode_usb_dig:
            if (long_press) {
                mode = x6100_mode_usb;
            } else {
                mode = x6100_mode_lsb_dig;
            }
            break;
        case x6100_mode_lsb:
            if (long_press) {
                mode = x6100_mode_lsb_dig;
            } else {
                mode = x6100_mode_usb;
            }
            break;
        case x6100_mode_usb:
            if (long_press) {
                mode = x6100_mode_usb_dig;
            } else {
                mode = x6100_mode_lsb;
            }
            break;
        default:
            mode = x6100_mode_lsb;
            break;
    }
    return mode;
}

static void change_mode(keypad_key_t key, keypad_state_t state) {
    switch (state) {
        case KEYPAD_LONG:
        case KEYPAD_RELEASE:
            break;
        default:
            return;
    }

    // Define mode->text struct
    typedef struct {
        x6100_mode_t    mode;
        const char*     msg;
    } mode_text_t;

    mode_text_t modes_text[] = {
        {.mode=x6100_mode_nfm, .msg="N F M modulation"},
        {.mode=x6100_mode_am, .msg="A M modulation"},
        {.mode=x6100_mode_cwr, .msg="CWR modulation"},
        {.mode=x6100_mode_cw, .msg="CW modulation"},
        {.mode=x6100_mode_lsb_dig, .msg="LSB digital modulation"},
        {.mode=x6100_mode_lsb, .msg="LSB modulation"},
        {.mode=x6100_mode_usb_dig, .msg="USB digital modulation"},
        {.mode=x6100_mode_usb, .msg="USB modulation"},
    };

    // find next mode
    x6100_mode_t    next_mode;
    switch (key) {
        case KEYPAD_MODE_AM:
            next_mode = get_next_mode_am_fm(state == KEYPAD_LONG);
            break;
        case KEYPAD_MODE_CW:
            next_mode = get_next_mode_cw(state == KEYPAD_LONG);
            break;
        case KEYPAD_MODE_SSB:
            next_mode = get_next_mode_ssb(state == KEYPAD_LONG);
            break;
        default:
            break;
    }

    for (size_t i = 0; i < sizeof(modes_text)/sizeof(modes_text[0]); i++) {
        if (modes_text[i].mode == next_mode) {
            voice_say_text_fmt(modes_text[i].msg);
            break;
        }
    }
    cparam_i_set(cfg.cur.mode(), next_mode);
}

static void keypad_pre(const event_keypad_t *kp) {
    int32_t pre = cparam_i_get(cfg.cur.pre());
    int32_t att = cparam_i_get(cfg.cur.att());

    if (kp->state == KEYPAD_RELEASE) {
        pre = !pre;
        cparam_i_set(cfg.cur.pre(), pre);
        voice_say_text_fmt("Preamplifier %s", pre ? "On" : "Off");

        if (param_i_get(cfg.ui.mag_info())) {
            msg_tiny_set_text_fmt("Pre: %s", pre ? "On" : "Off");
        }
    } else if (kp->state == KEYPAD_LONG) {
        att = !att;
        cparam_i_set(cfg.cur.att(), att);
        voice_say_text_fmt("Attenuator %s", att ? "On" : "Off");

        if (param_i_get(cfg.ui.mag_info())) {
            msg_tiny_set_text_fmt("Att: %s", att ? "On" : "Off");
        }
    }
}

static void keypad_band(const event_keypad_t *kp, bool up) {
    if (kp->state == KEYPAD_RELEASE) {
        if (!lm_get_band()) {
            cfg_band_load_next(up);
        }
        dialog_send(up ? EVENT_BAND_UP : EVENT_BAND_DOWN, NULL);
    }
}

static void keypad_mode(const event_keypad_t *kp) {
    if (!lm_get_mode()) {
        change_mode(kp->key, kp->state);
    }
}

static void keypad_agc(const event_keypad_t *kp) {
    if (kp->state == KEYPAD_RELEASE) {
        x6100_agc_t agc      = cparam_i_get(cfg.cur.agc());
        const char *msg_text = NULL;

        switch (agc) {
            case x6100_agc_off:
                agc = x6100_agc_slow;
                voice_say_text_fmt("Auto gain slow mode");
                msg_text = "AGC: Slow";
                break;

            case x6100_agc_slow:
                agc = x6100_agc_fast;
                voice_say_text_fmt("Auto gain fast mode");
                msg_text = "AGC: Fast";
                break;

            case x6100_agc_fast:
                agc = x6100_agc_auto;
                voice_say_text_fmt("Auto gain auto mode");
                msg_text = "AGC: Auto";
                break;

            case x6100_agc_auto:
                agc = x6100_agc_off;
                voice_say_text_fmt("Auto gain off");
                msg_text = "AGC: Off";
                break;
        }
        cparam_i_set(cfg.cur.agc(), agc);

        if (msg_text != NULL && param_i_get(cfg.ui.mag_info())) {
            msg_tiny_set_text_fmt(msg_text);
        }
    } else if (kp->state == KEYPAD_LONG) {
        bool new_split = !param_i_get(cfg.band.split());
        param_i_set(cfg.band.split(), new_split);
        voice_say_text_fmt("Split %s", new_split ? "On" : "Off");

        spectrum_clear();

        if (param_i_get(cfg.ui.mag_info())) {
            msg_tiny_set_text_fmt("Split: %s", new_split ? "On" : "Off");
        }
    }
}

static void keypad_fst(const event_keypad_t *kp) {
    if (kp->state == KEYPAD_RELEASE) {
        next_freq_step(true);
    } else if (kp->state == KEYPAD_LONG) {
        next_freq_step(false);
    }
}

static void keypad_atu(const event_keypad_t *kp) {
    if (kp->state == KEYPAD_RELEASE) {
        toggle_atu_enabled();

        if (param_i_get(cfg.ui.mag_info())) {
            msg_tiny_set_text_fmt("ATU: %s", param_i_get(cfg.atu_enabled()) ? "On" : "Off");
        }
    } else if (kp->state == KEYPAD_LONG) {
        radio_start_atu();
    }
}

static void keypad_fkey(const event_keypad_t *kp, uint8_t idx) {
    if (kp->state == KEYPAD_RELEASE) {
        buttons_press(idx, false);
    } else if (kp->state == KEYPAD_LONG) {
        buttons_press(idx, true);
    }
}

static void keypad_group_page(const event_keypad_t *kp, ParamInt *long_action, buttons_group_t group,
                              const char *voice) {
    if (kp->state == KEYPAD_RELEASE) {
        apps_disable();
        buttons_load_page_group(group);
        if (voice) {
            voice_say_text_fmt(voice);
        }
    } else if (kp->state == KEYPAD_LONG) {
        main_screen_action(param_i_get(long_action));
    }
}

static void keypad_msg(const event_keypad_t *kp) {
    if (kp->state == KEYPAD_RELEASE) {
        switch (cparam_i_get(cfg.cur.mode())) {
            case x6100_mode_cw:
            case x6100_mode_cwr:
                if (!dialog_type_is_run(dialog_msg_cw)) {
                    apps_disable();
                }

                panel_hide();
                dialog_construct(dialog_msg_cw, obj);
                voice_say_text_fmt("CW messages window");
                break;

            case x6100_mode_lsb:
            case x6100_mode_usb:
            case x6100_mode_am:
            case x6100_mode_nfm:
                if (!dialog_type_is_run(dialog_msg_voice)) {
                    apps_disable();
                }

                panel_hide();
                dialog_construct(dialog_msg_voice, obj);
                voice_say_text_fmt("Voice messages window");
                break;

            default:
                msg_tiny_set_text_fmt("Not used in this mode");
                break;
        }
    } else if (kp->state == KEYPAD_LONG) {
        main_screen_action(param_i_get(cfg.keys.long_msg()));
    }
}

static void keypad_ab(const event_keypad_t *kp) {
    if (!lm_get_ab()) {
        if (kp->state == KEYPAD_RELEASE) {
            x6100_vfo_t new_vfo = radio_toggle_vfo();

            spectrum_clear();

            if (param_i_get(cfg.ui.mag_info())) {
                const char *prefix     = param_i_get(cfg.band.split()) ? "SPL" : "VFO";
                const char *vfo_id_str = new_vfo == X6100_VFO_A ? "A" : "B";
                msg_tiny_set_text_fmt("%s: %s", prefix, vfo_id_str);
            }
        } else if (kp->state == KEYPAD_LONG) {
            x6100_vfo_t cur_vfo = param_i_get(cfg.band.current_vfo());
            cfg_band_vfo_copy();
            // radio_vfo_set();
            msg_update_text_fmt("Clone VFO %s", cur_vfo == X6100_VFO_A ? "A->B" : "B->A");
            voice_say_text_fmt("V F O cloned %s", cur_vfo == X6100_VFO_A ? "from A to B" : "from B to A");
        }
    }
}

static void keypad_power(const event_keypad_t *kp) {
    if (kp->state == KEYPAD_RELEASE) {
        display_power_toggle();
    } else if (kp->state == KEYPAD_LONG) {
        voice_say_text_fmt("Power off");
        msg_update_text_fmt("Power off");
        radio_poweroff();
    }
}

static void keypad_lock(const event_keypad_t *kp) {
    if (kp->state == KEYPAD_RELEASE) {
        lm_toggle_freq();
        voice_say_text_fmt("Frequency %s", lm_get_freq() ? "locked" : "unlocked");
    } else if (kp->state == KEYPAD_LONG) {
        radio_bb_reset();
        // Stop app
        app_is_running = false;
    }
}

static void keypad_ptt(const event_keypad_t *kp) {
    switch (kp->state) {
        case KEYPAD_PRESS:
            diag_set_source("front PTT key");
            radio_set_ptt(true);
            diag_set_source("UI");

            switch (cparam_i_get(cfg.cur.mode())) {
                case x6100_mode_cw:
                case x6100_mode_cwr:
                    radio_set_morse_key(true);
                    break;
            }
            break;

        case KEYPAD_RELEASE:
        case KEYPAD_LONG_RELEASE:
            switch (cparam_i_get(cfg.cur.mode())) {
                case x6100_mode_cw:
                case x6100_mode_cwr:
                    radio_set_morse_key(false);
                    break;
            }

            diag_set_source("front PTT key");
            radio_set_ptt(false);
            diag_set_source("UI");
            break;
        default:
            break;
    }
}

static void keypad_vm(const event_keypad_t *kp) {
    if ((kp->state == KEYPAD_RELEASE) && !dialog_running) {
        buttons_load_page_group(buttons_group_vm);
        voice_say_text_fmt("VM parameters");
    }
}

static void main_screen_keypad_cb(lv_event_t *e) {
    const event_keypad_t *kp = lv_event_get_param(e);

    switch (kp->key) {
        case KEYPAD_PRE:
            keypad_pre(kp);
            break;

        case KEYPAD_BAND_UP:
            keypad_band(kp, true);
            break;

        case KEYPAD_BAND_DOWN:
            keypad_band(kp, false);
            break;

        case KEYPAD_MODE_AM:
        case KEYPAD_MODE_CW:
        case KEYPAD_MODE_SSB:
            keypad_mode(kp);
            break;

        case KEYPAD_AGC:
            keypad_agc(kp);
            break;

        case KEYPAD_FST:
            keypad_fst(kp);
            break;

        case KEYPAD_ATU:
            keypad_atu(kp);
            break;

        case KEYPAD_F1:
            keypad_fkey(kp, 0);
            break;

        case KEYPAD_F2:
            keypad_fkey(kp, 1);
            break;

        case KEYPAD_F3:
            keypad_fkey(kp, 2);
            break;

        case KEYPAD_F4:
            keypad_fkey(kp, 3);
            break;

        case KEYPAD_F5:
            keypad_fkey(kp, 4);
            break;

        case KEYPAD_GEN:
            keypad_group_page(kp, cfg.keys.long_gen(), buttons_group_gen, "General menu keys");
            break;

        case KEYPAD_APP:
            keypad_group_page(kp, cfg.keys.long_app(), buttons_group_app, "Application menu keys");
            break;

        case KEYPAD_KEY:
            keypad_group_page(kp, cfg.keys.long_key(), buttons_group_key, "CW parameters");
            break;

        case KEYPAD_MSG:
            keypad_msg(kp);
            break;

        case KEYPAD_DFN:
            keypad_group_page(kp, cfg.keys.long_dfn(), buttons_group_dfn, "DNF parameters");
            break;

        case KEYPAD_DFL:
            keypad_group_page(kp, cfg.keys.long_dfl(), buttons_group_dfl, "DFL parameters");
            break;

        case KEYPAD_AB:
            keypad_ab(kp);
            break;

        case KEYPAD_POWER:
            keypad_power(kp);
            break;

        case KEYPAD_LOCK:
            keypad_lock(kp);
            break;

        case KEYPAD_PTT:
            keypad_ptt(kp);
            break;

        case KEYPAD_VM:
            keypad_vm(kp);
            break;

        default:
            LV_LOG_WARN("Unsuported key: %u", kp->key);
            break;
    }
}

static void main_screen_hkey_cb(lv_event_t * e) {
    event_hkey_t *hkey = lv_event_get_param(e);
    switch (hkey->key) {
        case HKEY_1:
        case HKEY_2:
        case HKEY_3:
        case HKEY_4:
        case HKEY_5:
        case HKEY_6:
        case HKEY_7:
        case HKEY_8:
        case HKEY_9:
            if (hkey->state == HKEY_RELEASE) {
                mem_load(hkey->key - HKEY_1 + 1);
                voice_say_text_fmt("Memory %i loaded", hkey->key - HKEY_1 + 1);
            } else if (hkey->state == HKEY_LONG) {
                mem_save(hkey->key - HKEY_1 + 1);
                voice_say_text_fmt("Memory %i stored", hkey->key - HKEY_1 + 1);
            }
            break;

        case HKEY_SPCH:
            if (hkey->state == HKEY_RELEASE) {
                lm_toggle_freq();
                voice_say_text_fmt("Frequency %s", lm_get_freq() ? "locked" : "unlocked");
            }
            break;

        case HKEY_TUNER:
            if (hkey->state == HKEY_RELEASE) {
                toggle_atu_enabled();

            } else if (hkey->state == HKEY_LONG) {
                radio_start_atu();
            }
            break;

        case HKEY_XFC:
            if (hkey->state == HKEY_RELEASE) {
                radio_toggle_vfo();

                spectrum_clear();
            }
            break;

        case HKEY_UP:
            if (hkey->state == HKEY_RELEASE) {
                if (!lm_get_freq()) {
                    freq_shift(+1, 0);
                }
            } else if (hkey->state == HKEY_LONG) {
                if (!lm_get_band()) {
                    cfg_band_load_next(true);
                }
                dialog_send(EVENT_BAND_UP, NULL);
            }
            break;

        case HKEY_DOWN:
            if (hkey->state == HKEY_RELEASE) {
                if (!lm_get_freq()) {
                    freq_shift(-1, 0);
                }
            } else if (hkey->state == HKEY_LONG) {
                if (!lm_get_band()) {
                    cfg_band_load_next(false);
                }
                dialog_send(EVENT_BAND_DOWN, NULL);
            }
            break;

        case HKEY_F1:
            if (hkey->state == HKEY_RELEASE) {
                main_screen_action(param_i_get(cfg.keys.press_f1()));
            } else if (hkey->state == HKEY_LONG) {
                main_screen_action(param_i_get(cfg.keys.long_f1()));
            }
            break;

        case HKEY_F2:
            if (hkey->state == HKEY_RELEASE) {
                main_screen_action(param_i_get(cfg.keys.press_f2()));
            } else if (hkey->state == HKEY_LONG) {
                main_screen_action(param_i_get(cfg.keys.long_f2()));
            }
            break;

        default:
            LV_LOG_WARN("Unsupported key: %u", hkey->key);
            break;
    }
}

static void rx_cb(void * s, lv_msg_t * msg) {
    if (!dialog_running) {
        indicators_left_show(true);
    }
    // Show left freq boundary
    lv_obj_clear_flag(freq_bounds[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(top_container, top_container_index);
}

static void tx_cb(void * s, lv_msg_t * msg) {
    indicators_left_show(false);
    if (dialog_running) {
        lv_obj_move_foreground(top_container);
    }
    // Hide left freq boundary
    lv_obj_add_flag(freq_bounds[0], LV_OBJ_FLAG_HIDDEN);
}

static void low_power_cb(void * s, lv_msg_t * msg) {
    bool is_low = (bool)(uintptr_t)lv_msg_get_payload(msg);
    if (is_low) {
        if (!low_power_timer) {
            low_power_timer = lv_timer_create(low_power_timer_cb, 30000, NULL);
            lv_timer_set_repeat_count(low_power_timer, 1);
            msg_schedule_long_text_fmt("Low battery! Turning off in 30s.");
        }
    } else {
        if (low_power_timer) {
            lv_timer_del(low_power_timer);
            low_power_timer = NULL;
        }
    }
}

static void lock_freq_cb(void * s, lv_msg_t * msg) {
    bool lock = *(bool*)lv_msg_get_payload(msg);
    if (lock) {
        lv_obj_add_state(freq_bounds[0], LV_STATE_DISABLED);
        lv_obj_add_state(freq_bounds[1], LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(freq_bounds[0], LV_STATE_DISABLED);
        lv_obj_clear_state(freq_bounds[1], LV_STATE_DISABLED);
    }
}

static void main_screen_update_cb(lv_event_t * e) {

    spectrum_clear();
}

static uint16_t freq_accel(uint16_t dt) {
    if (dt == 0) {
        return 1;
    }

    float speed;

    switch (param_i_get(cfg.radio.freq_accel())) {
        case FREQ_ACCEL_NONE:
            return 1;

        case FREQ_ACCEL_LITE:
            speed = 20.0f / dt;
            return LV_MIN(exp2f(speed), 10);

        case FREQ_ACCEL_STRONG:
            speed = 40.0f / dt;
            return LV_MIN(exp2f(speed), 30);
    }
    return 1;
}

static void freq_shift(int16_t diff, uint16_t dt) {
    if (lm_get_freq()) {
        return;
    }

    int32_t freq = cparam_i_get(cfg.cur.fg_freq());
    int32_t df = diff * param_i_get(cfg.mode.freq_step()) * freq_accel(dt);
    freq = align_int(freq + df, abs(df));
    cparam_i_set(cfg.cur.fg_freq(), freq);

    voice_say_freq(freq);
}

static void main_screen_rotary_cb(lv_event_t * e) {
    rotary_data_t *data = (rotary_data_t *) lv_event_get_param(e);

    freq_shift(data->diff, data->dt);
    dialog_rotary(data->diff, data->dt);
    free(data);
}

static void spectrum_key_cb(lv_event_t * e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case '-':
            if (!lm_get_freq()) {
                freq_shift(-1, 0);
            }
            break;

        case '=':
            if (!lm_get_freq()) {
                freq_shift(+1, 0);
            }
            break;

        case '_':
            next_freq_step(false);
            break;

        case '+':
            next_freq_step(true);
            break;

        case KEY_VOL_LEFT_EDIT:
        case '[':
            vol_update(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case ']':
            vol_update(+1);
            break;

        case KEY_VOL_LEFT_SELECT:
        case '{':
            vol_change_ctrl(-1);
            break;

        case KEY_VOL_RIGHT_SELECT:
        case '}':
            vol_change_ctrl(+1);
            break;

        case KEYBOARD_F9:
            dialog_construct(dialog_settings, obj);
            break;

        case LV_KEY_LEFT:
            switch (mfk_state) {
                case MFK_STATE_EDIT:
                    mfk_update(-1);
                    break;

                case MFK_STATE_SELECT:
                    mfk_change_ctrl(-1);
                    break;
            }
            break;

        case LV_KEY_RIGHT:
            switch (mfk_state) {
                case MFK_STATE_EDIT:
                    mfk_update(+1);
                    break;

                case MFK_STATE_SELECT:
                    mfk_change_ctrl(+1);
                    break;
            }
            break;

        case LV_KEY_ESC:
            // VOL press also
            if (!dialog_running) {
                switch (vol->state) {
                    case VOL_STATE_EDIT:
                        vol->state = VOL_STATE_SELECT;
                        knobs_set_vol_state(false);
                        voice_say_text_fmt("Selection mode");
                        break;

                    case VOL_STATE_SELECT:
                        vol->state = VOL_STATE_EDIT;
                        knobs_set_vol_state(true);
                        voice_say_text_fmt("Edit mode");
                        break;
                }
                vol_update(0);
            }
            break;

        case KEYBOARD_PRINT:
        case KEYBOARD_PRINT_SCR:
            screenshot_take();
            break;

        case KEYBOARD_SCRL_LOCK:
            lm_toggle_freq();
            break;

        case KEYBOARD_PGUP:
            if (!lm_get_band()) {
                cfg_band_load_next(true);
            }
            dialog_send(EVENT_BAND_UP, NULL);
            break;

        case KEYBOARD_PGDN:
            if (!lm_get_band()) {
                cfg_band_load_next(false);
            }
            dialog_send(EVENT_BAND_DOWN, NULL);
            break;

        case HKEY_FINP:
        case 'f':
            if (!lm_get_freq()) {
                voice_say_text_fmt("Enter frequency");
                dialog_construct(dialog_freq, obj);
            }
            break;

        default:
            break;
    }
}

static void spectrum_pressed_cb(lv_event_t * e) {
    switch (mfk_state) {
        case MFK_STATE_EDIT:
            mfk_state = MFK_STATE_SELECT;
            voice_say_text_fmt("Selection mode");
            knobs_set_mfk_state(false);
            break;

        case MFK_STATE_SELECT:
            mfk_state = MFK_STATE_EDIT;
            voice_say_text_fmt("Edit mode");
            knobs_set_mfk_state(true);
            break;
    }
    mfk_update(0);
}

static void keys_enable_cb(lv_timer_t *t) {
    lv_group_add_obj(keyboard_group, spectrum);
    lv_group_set_editing(keyboard_group, true);
}

void main_screen_keys_enable(bool value) {
    if (value) {
        lv_timer_t *timer = lv_timer_create(keys_enable_cb, 100, NULL);
        lv_timer_set_repeat_count(timer, 1);
    } else {
        lv_group_remove_obj(spectrum);
        lv_group_set_editing(keyboard_group, false);
    }
}

void main_screen_set_freq(uint64_t freq) {
    cparam_i_set(cfg.cur.fg_freq(), freq);
    event_send(lv_scr_act(), EVENT_SCREEN_UPDATE, NULL);
}

lv_obj_t * main_screen(lv_obj_t *overlay_scr) {
    uint16_t y = 0;
    uint16_t spectrum_height = (uint16_t)param_i_get(cfg.ui.spectrum_height());

    obj = overlay_scr;

    lv_obj_add_event_cb(obj, main_screen_rotary_cb, EVENT_ROTARY, NULL);
    lv_obj_add_event_cb(obj, main_screen_keypad_cb, EVENT_KEYPAD, NULL);
    lv_obj_add_event_cb(obj, main_screen_hkey_cb, EVENT_HKEY, NULL);
    lv_obj_add_event_cb(obj, main_screen_update_cb, EVENT_SCREEN_UPDATE, NULL);

    lv_msg_subscribe(MSG_RADIO_RX, rx_cb, NULL);
    lv_msg_subscribe(MSG_RADIO_TX, tx_cb, NULL);
    lv_msg_subscribe(MSG_LOW_POWER, low_power_cb, NULL);

    // lv_obj_add_style(obj, &style.background, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    /* Indicators block */
    // width from meter style for correct padding
    indicators_init(obj, INDICATORS_HEIGHT, METER_WIDTH);
    y += INDICATORS_HEIGHT;

    /* Spectrum */
    spectrum = spectrum_init(overlay_scr, INDICATORS_HEIGHT, spectrum_height);
    main_screen_keys_enable(true);

    lv_obj_add_event_cb(spectrum, spectrum_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(spectrum, spectrum_pressed_cb, LV_EVENT_PRESSED, NULL);

    y += spectrum_height;

    /* Freq boundary (left, right) */
    lv_obj_t *f;

    f = lv_label_create(spectrum);
    lv_obj_add_style(f, &style.freq_bounds, LV_PART_MAIN);
    lv_obj_add_style(f, &style.text_muted_color, LV_STATE_DISABLED);
    lv_obj_align(f, LV_ALIGN_TOP_LEFT, 5, TOP_BLOCK_SMALL_HEIGHT + 10);
    freq_bounds[0] = f;

    f = lv_label_create(spectrum);
    lv_obj_add_style(f, &style.freq_bounds, LV_PART_MAIN);
    lv_obj_add_style(f, &style.text_muted_color, LV_STATE_DISABLED);
    lv_obj_align(f, LV_ALIGN_TOP_RIGHT, -5, TOP_BLOCK_SMALL_HEIGHT + 10);
    freq_bounds[1] = f;

    /* Waterfall */
    waterfall = waterfall_init(overlay_scr, y, SCREEN_HEIGHT - y);

    /* Konbs */
    knobs_init(obj);

    /* Buttons */
    buttons_init(obj);
    buttons_load_page(&buttons_page_vol_1);

    /* Top container (meter, clock, freq) */
    top_container = lv_obj_create(obj);
    lv_obj_remove_style_all(top_container);
    lv_obj_clear_flag(top_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(top_container, 0, 0);
    // height from tx_info style for correct padding
    lv_obj_set_size(top_container, SCREEN_WIDTH, TOP_BLOCK_BIG_HEIGHT);
    lv_obj_set_layout(top_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(top_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    top_container_index = lv_obj_get_index(top_container);

    meter = meter_init(top_container);
    tx_info = tx_info_init(top_container);
    freq_info_init(top_container);
    clock_init(top_container);

    /* Panel (CW/RTTY) */
    panel_init(waterfall);
    msg_init(waterfall);
    msg_tiny_init(spectrum);

    /* CW tune */
    cw_tune_init(spectrum);

    msg_schedule_text_fmt("X6100 de R1CBU es Others " VERSION);

    subject_subscribe_delayed_and_notify((Subject*)radio_fg_freq_subj, on_fg_freq_change, NULL);

    lv_msg_subscribe(MSG_LOCK_FREQ, lock_freq_cb, NULL);
    lv_msg_subscribe(MSG_DIALOG_START, on_dialog_start_cb, NULL);
    lv_msg_subscribe(MSG_DIALOG_STOP, on_dialog_stop_cb, NULL);
    subject_subscribe_delayed((Subject*)radio_fg_freq_subj, update_freq_boundaries, NULL);
    subject_subscribe_delayed((Subject*)cfg.band.if_shift(), update_freq_boundaries, NULL);
    subject_subscribe_delayed_and_notify((Subject*)cfg.mode.zoom(), update_freq_boundaries, NULL);

    subject_subscribe_delayed((Subject*)cfg.band.if_shift(), update_zoom_on_if_shift_change, NULL);
    subject_subscribe_delayed((Subject*)cfg.mode.zoom(), update_zoom_on_if_shift_change, NULL);

    subject_subscribe_delayed_and_notify((Subject*)cfg.ui.spectrum_height(), on_spectrum_height_change, NULL);

    return obj;
}

static void apply_main_layout(void) {
    lv_coord_t h = (lv_coord_t)param_i_get(cfg.ui.spectrum_height());
    lv_coord_t y = INDICATORS_HEIGHT;

    spectrum_set_geometry(y, h);
    y += h;

    lv_coord_t wf_h = SCREEN_HEIGHT - y;

    waterfall_set_geometry(y, wf_h);

    lv_coord_t panel_h = wf_h - (BAND_INFO_OFFSET_Y * 2 + BAND_INFO_HEIGHT) - (BTN_HEIGHT + PANEL_GAP_BOTTOM);
    panel_set_height(LV_MAX(panel_h, 1));

    msg_align();
    msg_tiny_align();
}

static void on_spectrum_height_change(Subject *subj, void *user_data) {
    apply_main_layout();
}

static void on_fg_freq_change(Subject *subj, void *user_data) {
    if (param_i_get(cfg.ui.mag_freq())) {
        int32_t f = subject_i_get(radio_fg_freq_subj);

        uint16_t mhz, khz, hz;

        split_freq(f, &mhz, &khz, &hz);

        if (mhz < 100) {
            msg_tiny_set_text_fmt("%i.%03i.%03i", mhz, khz, hz);
        } else {
            msg_tiny_set_text_fmt("%i.%03i", mhz, khz);
        }
    }
}

static void update_freq_boundaries(Subject *subj, void *user_data) {
    int32_t f = subject_i_get(radio_fg_freq_subj) - param_i_get(cfg.band.if_shift());

    uint16_t mhz, khz, hz;

    int32_t zoom = param_i_get(cfg.mode.zoom());
    uint32_t half_width = 50000 / zoom;

    split_freq(f - half_width, &mhz, &khz, &hz);
    lv_label_set_text_fmt(freq_bounds[0], "%i.%03i", mhz, khz);

    split_freq(f + half_width, &mhz, &khz, &hz);
    lv_label_set_text_fmt(freq_bounds[1], "%i.%03i", mhz, khz);
}

static void update_zoom_on_if_shift_change(Subject *subj, void *user_data) {
    int32_t half_width = 40000;
    int32_t new_if_shift = param_i_get(cfg.band.if_shift());
    uint32_t zoom = param_i_get(cfg.mode.zoom());
    uint32_t new_zoom = zoom;
    while ((abs(new_if_shift) * new_zoom / half_width) && (new_zoom > 1))
    {
        new_zoom >>= 1;
    }
    if (new_zoom != zoom) {
        param_i_set(cfg.mode.zoom(), new_zoom);
    }
}


static void on_dialog_start_cb(void *s, lv_msg_t *m) {
    // Move meter/freq/clock to top
    lv_obj_set_flex_align(top_container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    indicators_show(false);
    main_screen_keys_enable(false);
    dialog_running = true;
}

static void on_dialog_stop_cb(void *s, lv_msg_t *m) {
    // Restore align
    lv_obj_set_flex_align(top_container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    indicators_show(true);
    main_screen_keys_enable(true);
    dialog_running = false;
}
