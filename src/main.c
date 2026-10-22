/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/display/drm.h"

#include "globals.h"
#include "cfg/cfg_api.h"
#include "cfg/db.h"

#include "main.h"
#include "main_screen.h"
#include "styles.h"
#include "radio.h"
#include "dsp.h"
#include "util.h"
#include "keyboard.h"
#include "spectrum.h"
#include "waterfall.h"
#include "keypad.h"
#include "audio.h"
#include "cw.h"
#include "panel.h"
#include "app_ports.h"
#include "cat/cat.h"
#include "cat/lan/cat_lan.h"
#include "rtty.h"
#include "display.h"
#include "events.h"
#include "gps.h"
#include "mfk.h"
#include "vol.h"
#include "qso_log.h"
#include "scheduler.h"
#include "wifi.h"
#include "usb_devices.h"
#include "sstv_resources.h"

rotary_t  *vol;
encoder_t *mfk;
lv_obj_t  *overlay_scr;
lv_obj_t  *primary_scr;
volatile sig_atomic_t app_is_running = 1;

static lv_disp_drv_t        disp_drv_primary;
static lv_disp_drv_t        disp_drv_overlay;

static pthread_t            tick_pthread;
static bool                 tick_pthread_started = false;

void * tick_thread (void *args);

static void handle_sigint(int signum) {
    app_is_running = 0;
}


int main(void) {

    // Register signal handler
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    bool drm = true;
    lv_init();
    scheduler_init();

    if (drm) {
        drm_init(&disp_drv_primary, &disp_drv_overlay);
    } else {
        fbdev_init(&disp_drv_primary);
    }
    audio_init();
    event_init();
    usb_devices_monitor_init();

    // disp_drv.full_refresh = true;
    lv_disp_t *disp_primary = NULL;
    lv_disp_t *disp_overlay = NULL;
    if (drm) {
        disp_drv_primary.direct_mode   = false;
        disp_drv_primary.sw_rotate     = 1;
        disp_drv_primary.rotated       = LV_DISP_ROT_90;
        disp_drv_primary.screen_transp = 0;

        disp_drv_overlay.direct_mode   = false;
        disp_drv_overlay.sw_rotate     = 1;
        disp_drv_overlay.rotated       = LV_DISP_ROT_90;
        disp_drv_overlay.screen_transp = 1;

        disp_overlay                   = lv_disp_drv_register(&disp_drv_overlay);
    } else {
        disp_drv_primary.direct_mode = true;
    }

    disp_primary = lv_disp_drv_register(&disp_drv_primary);

    // Init screens
    lv_disp_set_default(disp_primary);
    primary_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(primary_scr, lv_color_black(), 0);
    // lv_obj_remove_style_all(primary_scr);
    lv_obj_set_style_bg_opa(primary_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_disp_set_bg_opa(disp_primary, LV_OPA_COVER);
    lv_scr_load(primary_scr);
    if (drm) {
        // overlay_scr = primary_scr;
        lv_disp_set_default(disp_overlay);
        overlay_scr = lv_obj_create(NULL);
        // lv_obj_remove_style_all(overlay_scr);
        lv_obj_set_style_bg_color(overlay_scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(overlay_scr, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_disp_set_bg_opa(disp_overlay, LV_OPA_TRANSP);
        lv_scr_load(overlay_scr);
    } else {
        overlay_scr = primary_scr;
    }

    keyboard_init();

    keypad_init("/dev/input/event0");
    keypad_init("/dev/input/event4");

    rotary_main_init("/dev/input/event1");

    vol = rotary_init("/dev/input/event2");
    mfk = encoder_init("/dev/input/event3");

    vol->left[VOL_STATE_EDIT] = KEY_VOL_LEFT_EDIT;
    vol->right[VOL_STATE_EDIT] = KEY_VOL_RIGHT_EDIT;

    vol->left[VOL_STATE_SELECT] = KEY_VOL_LEFT_SELECT;
    vol->right[VOL_STATE_SELECT] = KEY_VOL_RIGHT_SELECT;
    vol->state = VOL_STATE_EDIT;

    cfg_init();
    sstv_resources_init();
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    audio_set_rec_vol(param_f_get(cfg.audio.rec_gain_db()));
    mfk_init();
    vol_init();
    styles_init((themes_t)param_i_get(cfg.ui.theme()));

    radio_init();
    audio_mixer_setup(x6100_control_get_base_ver());
    dsp_init();
    main_screen(overlay_scr);

    radio_start();

    cw_init();
    rtty_init();
    wifi_power_setup();
    display_init();
    cat_init(&app_ports);
    cat_lan_init(&app_ports);
    gps_init();
    if (!qso_log_init()) {
        LV_LOG_ERROR("Can't init QSO log");
    }
    qso_log_import_adif("/mnt/incoming_log.adi");

    int rc = pthread_create(&tick_pthread, NULL, tick_thread, NULL);

    if (rc != 0) {
        LV_LOG_ERROR("Can't start tick thread: %d", rc);
    } else {
        tick_pthread_started = true;
    }

#if 0
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_0, 0);
    lv_scr_load_anim(main_obj, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
#else
    // lv_scr_load(main_obj);
#endif

    int64_t next_loop_time, sleep_time, loop_start_time;
    while (app_is_running) {
        loop_start_time = get_time();
        observer_delayed_drain();
        event_obj_check();
        scheduler_work();
        next_loop_time = lv_timer_handler() + loop_start_time;
        spectrum_process();
        waterfall_process();
        drm_flip();
        sleep_time = next_loop_time - get_time();
        if (sleep_time > 0) {
            usleep(sleep_time * 1000);
        }
    }

    // Cleanup
    radio_shutdown();
    usb_devices_monitor_shutdown();
    if (tick_pthread_started) {
        pthread_join(tick_pthread, NULL);
    }
    display_invert(false);
    cat_lan_destruct();
    wifi_cleanup();
    observer_delayed_shutdown();
    cfg_api_flush_all();
    cfg_db_shutdown();
    return 0;
}

void * tick_thread (void *args)
{
      while (app_is_running) {
        usleep(5 * 1000);
        lv_tick_inc(5);
    }

      return NULL;
}
