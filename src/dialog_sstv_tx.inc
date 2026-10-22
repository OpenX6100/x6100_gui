/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 SSTV app
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
 * TX: transmitter thread, ALC, interactive TX wizard (picture, type, template, target, colors).
 *
 * This file is NOT a separate compilation unit: it is #included by
 * dialog_sstv.c and must not be added to CMakeLists.txt.  All SSTV dialog
 * code still forms ONE translation unit, so every "static" keeps its
 * original meaning and no symbol is exported.
 */
/* SSTV TX level control (ALC).
 *
 * Digital modes (USB-D / LSB-D): the app sets the TX audio level itself.
 * Facts measured on the radio (October 2026):
 *   - below the ALC onset the output power follows the audio level
 *     linearly (power in dB = level in dB + constant);
 *   - the radio limits the power to its setting; above that the ALC rises;
 *   - the ALC reading and the power drop it causes release SLOWLY (seconds).
 *     Cutting the level again while the ALC is still falling drove the
 *     level to the floor (v4).
 *   - in the digital modes the radio reports power and ALC unfiltered, and
 *     the SSTV power follows the picture (4..5.5 W at about 10 Hz): the
 *     control uses averages over SSTV_TX_WINDOW_MS, never single readings.
 * Control law (one decision per averaging window):
 *   - ALC > SSTV_TX_ALC_HIGH and not falling: cut by the overdrive estimated
 *     from the ALC value (1..10 dB); while the ALC is falling: wait;
 *   - ALC in (SSTV_TX_ALC_OK, SSTV_TX_ALC_HIGH] and not falling: cut 0.5 dB;
 *   - ALC <= SSTV_TX_ALC_OK: steer the POWER to SSTV_TX_PWR_AIM x target
 *     (target = radio power setting, max 5 W), using the linear relation:
 *     step = 0.7 x 10*log10(aim / power), at most 6 dB.  The aim is just
 *     below the setting: there the radio's own power limit levels all the
 *     SSTV tones (flat power, ALC ~0).  A few tenths of dB lower, the power
 *     follows the TX response of each tone and swings (4..5 W at 5 W);
 *   - the level and the power setting are saved in SSTV_TX_LEVEL_FILE
 *     (average of the readings close to the aim).  The next transmission
 *     starts from it.  With the stock BASE (no true power control) it is
 *     rescaled if the power setting has changed;
 *   - without a saved level the transmission starts at SSTV_TX_START_DB.
 * The total level is the ALSA play volume (fixed, SSTV_TX_PLAY_DB) plus a
 * digital gain applied to the samples.
 *
 * SSB modes (USB / LSB):
 *   - patched BASE (rev >= 3): the operator sets the level with the TX codec
 *     gain slider.  No ALC correction is applied unless SSTV_ALC_IN_SSB is
 *     set to 1 (old behaviour: attenuation only, starting from 0 dB);
 *   - stock BASE: there is no slider, so the automatic control above is
 *     used as in the digital modes. */
#define SSTV_ALC_IN_SSB 0
#define SSTV_TX_LEVEL_FILE      "/mnt/sstv/tx_level_db.txt"  /* "<level dB> <power W>" */
#define SSTV_TX_START_DB        (-40.0f)  /* first transmission, no saved level */
#define SSTV_TX_START_MARGIN_DB   0.0f    /* the saved level is already the right one */
#define SSTV_TX_LEVEL_MIN_DB    (-60.0f)  /* limits of the total level */
#define SSTV_TX_LEVEL_MAX_DB    (-12.0f)
#define SSTV_TX_PLAY_DB         (-12.0f)  /* fixed ALSA play volume in digital modes */
#define SSTV_TX_WINDOW_MS       1000u  /* telemetry averaging window */
#define SSTV_TX_SETTLE_MS        300u  /* readings ignored after a step */
#define SSTV_TX_STEP_UP_MAX_DB    6.0f
#define SSTV_TX_CUT_MAX_DB       10.0f
#define SSTV_TX_ALC_OK            0.3f
#define SSTV_TX_ALC_HIGH          1.0f
#define SSTV_TX_PWR_AIM           0.97f
#define SSTV_TX_AIM_UP_DB         0.10f  /* raise if the average is this much below aim */
#define SSTV_TX_AIM_DOWN_DB       0.40f  /* lower if this much above aim (ALC also limits) */
#define SSTV_TX_MAX_PWR_W         5.0f
/* Digital gain limits for the optional SSB protection. */
#define SSTV_TX_GAIN_MIN_DB (-50.0f)
#define SSTV_TX_GAIN_MAX_DB   0.0f

static bool sstv_tx_level_load(float *level, float *pwr)
{
    float v = 0.0f, w = 0.0f;
    bool ok = false;
    FILE *fp = fopen(SSTV_TX_LEVEL_FILE, "r");
    if (!fp) return false;
    if (fscanf(fp, "%f %f", &v, &w) == 2 &&
        v >= SSTV_TX_LEVEL_MIN_DB && v <= SSTV_TX_LEVEL_MAX_DB &&
        w >= 0.1f && w <= SSTV_TX_MAX_PWR_W) {
        *level = v;
        *pwr = w;
        ok = true;
    }
    fclose(fp);
    return ok;
}

static void sstv_tx_level_save(float level, float pwr)
{
    FILE *fp = fopen(SSTV_TX_LEVEL_FILE, "w");
    if (!fp) {
        fprintf(stderr, "SSTV TX: cannot save %s\n", SSTV_TX_LEVEL_FILE);
        return;
    }
    fprintf(fp, "%.2f %.2f\n", level, pwr);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
}

static uint32_t sstv_tx_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static float sstv_tx_target_pwr(void)
{
    float t = param_f_get(cfg.pwr());
    if (t > SSTV_TX_MAX_PWR_W) t = SSTV_TX_MAX_PWR_W;
    if (t < 0.1f) t = 0.1f;
    return t;
}

static lv_obj_t *tx_picture_table = NULL;
static lv_obj_t *tx_type_table = NULL;
static lv_obj_t *tx_type_help_label = NULL;
static lv_obj_t *tx_template_table = NULL;
static lv_obj_t *tx_target_table = NULL;
static lv_obj_t *tx_target_label = NULL;
static lv_obj_t *tx_target_callsign_label = NULL;
static lv_obj_t *tx_color_table = NULL;
static lv_obj_t *tx_instruction_label = NULL;
static bool tx_type_preview_pending = false;
/* Runtime-only persistence of the selected initial TX action row. */
static int tx_last_type_selected = 0;
static lv_obj_t *tx_target_keyboard_obj = NULL;
static bool tx_target_keyboard_active = false;   /* virtual keyboard window is open */
static sstv_tx_message_type_t tx_message_type = SSTV_TX_MESSAGE_CQ;
static int tx_color_selected = 0;
/* Runtime-only persistence of the selected color-combination row. */
static int tx_last_color_selected = 0;
static char **tx_template_files = NULL;
static size_t tx_template_file_count = 0;
static size_t tx_template_file_capacity = 0;
static char tx_template_middle[256];
static char tx_template_bottom[128];
/* Runtime persistence by design: keep the last target while the app/process
 * remains alive, but do not write it to permanent firmware configuration. */
static char tx_target_callsign[32] = "";
static size_t tx_picture_file_capacity = 0;
static int tx_picture_pending = -1;
static uint64_t tx_picture_pending_since_ms = 0;
static uint8_t tx_picture_rgb[320 * 256 * 3];
static char tx_picture_name[256];
static char tx_last_picture_file[256] = "";
static lv_color_t tx_saved_rx_canvas[SSTV_CANVAS_W * SSTV_CANVAS_H];
static bool tx_saved_rx_canvas_valid = false;


/* TX wizard lists: same look as CHANNELS and the RX browser (ui_list).
 * VOL keeps changing the volume (key_cb is attached to every list). */
#define TX_LIST_NUM_COL     { .x = 4,  .w = 30, .align = UI_LIST_ALIGN_RIGHT, .muted = true }
#define TX_LIST_NAME_COL    { .x = 42, .w = SSTV_TX_LIST_W - 12 - 42 - 4, .align = UI_LIST_ALIGN_LEFT }
#define TX_LIST_MENU_COL    { .x = 10, .w = SSTV_TX_LIST_W - 12 - 10 - 4, .align = UI_LIST_ALIGN_LEFT }

/* Create a TX list, give it key_cb (VOL = volume) and focus it. */
static lv_obj_t *tx_list_create(const ui_list_cfg_t *cfg, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *list=ui_list_create(dialog.obj,cfg);
    lv_obj_set_pos(list,x,y);
    lv_obj_add_event_cb(list,key_cb,LV_EVENT_KEY,NULL);
    return list;
}

static void tx_list_focus(lv_obj_t *list)
{
    lv_group_remove_obj(sstv_canvas);
    lv_group_add_obj(keyboard_group,list);
    lv_group_focus_obj(list);
    lv_group_set_editing(keyboard_group,true);
}

typedef struct { uint8_t r,g,b; } tx_rgb_t;
typedef struct { tx_rgb_t top,middle,bottom; } tx_color_combo_t;

/* Strong, deliberately saturated choices for readability over photographs.
 * #0 and #1 are the only monochromatic sets. Pink occurs only as BOTTOM. */
static const tx_color_combo_t tx_color_combos[20] = {
    { TX_RGB(255,  0,  0), TX_RGB(255,  0,  0), TX_RGB(255,  0,  0) }, /* red */
    { TX_RGB(255,255,255), TX_RGB(255,255,255), TX_RGB(255,255,255) }, /* white */
    { TX_RGB(255,255,  0), TX_RGB(  0,255,  0), TX_RGB( 64,128,255) }, /* yellow green blue */
    { TX_RGB(255,128,  0), TX_RGB(192, 64,255), TX_RGB( 64,192,255) }, /* orange violet light blue */
    { TX_RGB(255,  0,  0), TX_RGB(255,255,  0), TX_RGB(  0,255,  0) }, /* red yellow green */
    { TX_RGB( 64,128,255), TX_RGB(255,128,  0), TX_RGB(255,255,255) }, /* blue orange white */
    { TX_RGB(192, 64,255), TX_RGB(  0,255,  0), TX_RGB(255,255,  0) }, /* violet green yellow */
    { TX_RGB(  0,255,  0), TX_RGB( 64,192,255), TX_RGB(255,128,  0) }, /* green light blue orange */
    { TX_RGB(255,255,  0), TX_RGB(255,  0,  0), TX_RGB(192, 64,255) }, /* yellow red violet */
    { TX_RGB( 64,192,255), TX_RGB(255,255,  0), TX_RGB(255, 64,160) }, /* light blue yellow pink */

    /* Darker palette: still saturated enough to remain useful for SSTV text. */
    { TX_RGB(  0,112, 32), TX_RGB(  0, 64,160), TX_RGB(160,  0,  0) }, /* dark green, blue, red */
    { TX_RGB(128, 64,  0), TX_RGB( 96,  0,128), TX_RGB(  0,112,112) }, /* brown, dark violet, teal */
    { TX_RGB(  0, 80,144), TX_RGB(144, 64,  0), TX_RGB(112,  0, 80) }, /* deep blue, burnt orange, wine */
    { TX_RGB( 72,104,  0), TX_RGB(  0, 72,112), TX_RGB(128, 32, 32) }, /* olive, petrol blue, brick */
    { TX_RGB(112,  0,128), TX_RGB(  0,104, 64), TX_RGB(160, 80,  0) }, /* violet, emerald, amber */
    { TX_RGB(  0, 88,128), TX_RGB(128,  0, 64), TX_RGB( 64,112,  0) }, /* ocean, burgundy, leaf */
    { TX_RGB(144, 48,  0), TX_RGB( 48, 64,144), TX_RGB(  0,120, 96) }, /* rust, indigo, sea green */
    { TX_RGB( 80,  0,128), TX_RGB(144, 80,  0), TX_RGB(  0, 96,128) }, /* purple, ochre, dark cyan-blue */
    { TX_RGB(  0,128, 64), TX_RGB(128,  0, 32), TX_RGB( 64, 64,160) }, /* forest, crimson, royal blue */
    { TX_RGB(128, 48, 96), TX_RGB( 32,112, 80), TX_RGB(144, 96,  0) }  /* plum, jade, dark gold */
};

static tx_rgb_t tx_text_top    = TX_RGB(255,0,0);
static tx_rgb_t tx_text_middle = TX_RGB(255,0,0);
static tx_rgb_t tx_text_bottom = TX_RGB(255,0,0);

/* ---- TX text rendering (font, text, color combinations) ---- */
#include "dialog_sstv_tx_draw.inc"
static unsigned int sstv_tx_duration_ms(sstv_mode_t mode)
{
    switch (mode) {
        case SSTV_MODE_ROBOT8_BW:   return 8910u;   /* VIS ~910 ms + 120 x 66.667 ms */
        case SSTV_MODE_MARTIN_M1:  return 115200u; /* VIS ~910 ms + 256 x 446.446 ms */
        case SSTV_MODE_MARTIN_M2:  return 58970u;  /* VIS ~910 ms + 256 x 226.798 ms */
        case SSTV_MODE_ROBOT36:    return 36910u;  /* VIS 910 ms + 240 x 150.000 ms */
        case SSTV_MODE_ROBOT72:    return 72910u;  /* VIS 910 ms + 240 x 300.000 ms */
        case SSTV_MODE_SCOTTIE_S1: return 110543u; /* VIS 910 + 9 ms + 256 x 428.220 ms */
        case SSTV_MODE_SCOTTIE_S2: return 72008u;  /* VIS 910 + 9 ms + 256 x 277.692 ms */
        default:                   return 0u;
    }
}

static uint64_t monotonic_ms_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec = (int64_t)now.tv_sec - (int64_t)start->tv_sec;
    int64_t nsec = (int64_t)now.tv_nsec - (int64_t)start->tv_nsec;
    return (uint64_t)(sec * 1000 + nsec / 1000000);
}


static bool save_tx_png(const char *path, const uint8_t *rgb,
                        unsigned int width, unsigned int height)
{
    if (!path || !rgb || width == 0 || height == 0) return false;

    FILE *fp=fopen(path,"wb");
    if (!fp) {
        fprintf(stderr,"SSTV TX: fopen %s failed: %s\n",path,strerror(errno));
        return false;
    }

    png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop info=png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) png_destroy_write_struct(&png,NULL);
        fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png,&info);
        fclose(fp);
        return false;
    }

    png_init_io(png,fp);
    png_set_IHDR(png,info,width,height,8,PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png,info);
    for (unsigned int y=0;y<height;y++)
        png_write_row(png,(png_bytep)(rgb+(size_t)y*width*3));
    png_write_end(png,NULL);
    png_destroy_write_struct(&png,&info);
    fclose(fp);

    fprintf(stderr,"SSTV TX: final WYSIWYG framebuffer written to %s (%ux%u)\n",
            path,width,height);
    return true;
}

static bool sstv_tx_mode_is_digital(void)
{
    int mode = (int)cparam_i_get(cfg.cur.mode());
    return mode == x6100_mode_usb_dig || mode == x6100_mode_lsb_dig;
}

/* Optional SSB protection (SSTV_ALC_IN_SSB): attenuation only, FT8 law.
 * Returns a gain correction in dB, 0 if no new telemetry / nothing to do. */
static float sstv_tx_alc_correction(void)
{
    static uint8_t msg_id = 0;
    float alc = 0.0f;

    if (!tx_info_refresh(&msg_id, &alc, NULL, NULL))
        return 0.0f;
    if (alc <= 0.5f)
        return 0.0f;
    if (alc >= 11.0f)
        alc = 11.0f;
    return log10f(log10f(11.1f - alc)) * 20.0f - 0.38f;
}

/* Telemetry averaging window (digital modes).  The radio reports power and
 * ALC unfiltered in the digital modes, and with SSTV the power follows the
 * picture (about 4..5.5 W at 10 Hz in Robot 36): single readings must not be
 * used for control. */
static struct {
    uint8_t  msg_id;
    double   pwr_sum, alc_sum;
    unsigned n;
    uint32_t start_ms;     /* readings before this time are ignored */
    float    prev_alc;     /* mean ALC of the previous window, -1 = none */
} sstv_tx_win;

static void sstv_tx_win_reset(uint32_t start_ms)
{
    sstv_tx_win.pwr_sum = 0.0;
    sstv_tx_win.alc_sum = 0.0;
    sstv_tx_win.n = 0;
    sstv_tx_win.start_ms = start_ms;
}

/* Digital modes: collects telemetry and, once per SSTV_TX_WINDOW_MS window,
 * decides one control step on the window averages (see the comment at the top
 * of this file).  Returns the level change in dB (0 = none).  *decided is set
 * when a window was closed; *alc_out / *pwr_out then get its averages. */
static float sstv_tx_dig_step(float target, uint32_t now, bool *decided,
                              float *alc_out, float *pwr_out)
{
    float alc = 0.0f, pwr = 0.0f, step = 0.0f;

    *decided = false;
    if (tx_info_refresh(&sstv_tx_win.msg_id, &alc, &pwr, NULL) &&
        (int32_t)(now - sstv_tx_win.start_ms) >= 0) {
        sstv_tx_win.pwr_sum += pwr;
        sstv_tx_win.alc_sum += alc;
        sstv_tx_win.n++;
    }
    if ((int32_t)(now - sstv_tx_win.start_ms) < (int32_t)SSTV_TX_WINDOW_MS ||
        sstv_tx_win.n < 3)
        return 0.0f;

    pwr = (float)(sstv_tx_win.pwr_sum / sstv_tx_win.n);
    alc = (float)(sstv_tx_win.alc_sum / sstv_tx_win.n);
    unsigned n = sstv_tx_win.n;
    sstv_tx_win_reset(now);
    *decided = true;
    *alc_out = alc;
    *pwr_out = pwr;

    bool falling = sstv_tx_win.prev_alc >= 0.0f && alc < sstv_tx_win.prev_alc - 0.03f;
    sstv_tx_win.prev_alc = alc;

    if (alc > SSTV_TX_ALC_HIGH) {
        if (!falling) {
            step = -((alc - 0.2f) / 0.8f + 0.3f);
            if (step < -SSTV_TX_CUT_MAX_DB) step = -SSTV_TX_CUT_MAX_DB;
            if (step > -1.0f) step = -1.0f;
        }
    } else if (alc > SSTV_TX_ALC_OK) {
        if (!falling)
            step = -0.5f;
    } else {
        float aim = target * SSTV_TX_PWR_AIM;
        if (pwr <= 0.1f) {
            step = SSTV_TX_STEP_UP_MAX_DB;
        } else {
            float e = 10.0f * log10f(aim / pwr);
            if (e > SSTV_TX_AIM_UP_DB || e < -SSTV_TX_AIM_DOWN_DB)
                step = 0.7f * e;
            if (step > SSTV_TX_STEP_UP_MAX_DB) step = SSTV_TX_STEP_UP_MAX_DB;
            if (step < -SSTV_TX_STEP_UP_MAX_DB) step = -SSTV_TX_STEP_UP_MAX_DB;
        }
    }
    if (step != 0.0f) {
        fprintf(stderr, "SSTV TX: avg of %u: ALC %.2f%s PWR %.2f aim %.2f -> step %+.2f dB\n",
                n, alc, falling ? " (falling)" : "", pwr, target * SSTV_TX_PWR_AIM, step);
        /* Let the radio settle before collecting the next window. */
        sstv_tx_win_reset(now + SSTV_TX_SETTLE_MS);
    }
    return step;
}

static void *tx_thread_cb(void *arg)
{
    (void)arg;
    int16_t *samples = NULL;
    size_t count = 0;

    int rc;
    switch (tx_mode) {
        case SSTV_MODE_ROBOT8_BW:
            rc = sstv_encode_robot8_png_pcm("/mnt/sstv-tx.png", AUDIO_PLAY_RATE, &samples, &count);
            break;
        case SSTV_MODE_MARTIN_M1:
            rc = sstv_encode_martin_m1_png_pcm("/mnt/sstv-tx.png", AUDIO_PLAY_RATE, &samples, &count);
            break;
        case SSTV_MODE_MARTIN_M2:
            rc = sstv_encode_martin_m2_png_pcm("/mnt/sstv-tx.png", AUDIO_PLAY_RATE, &samples, &count);
            break;
        case SSTV_MODE_SCOTTIE_S1:
            rc = sstv_encode_scottie_s1_png_pcm("/mnt/sstv-tx.png", AUDIO_PLAY_RATE, &samples, &count);
            break;
        case SSTV_MODE_ROBOT36:
            rc = sstv_encode_robot36_png_pcm("/mnt/sstv-tx.png", AUDIO_PLAY_RATE, &samples, &count);
            break;
        case SSTV_MODE_ROBOT72:
            rc = sstv_encode_robot72_png_pcm("/mnt/sstv-tx.png", AUDIO_PLAY_RATE, &samples, &count);
            break;
        case SSTV_MODE_SCOTTIE_S2:
            rc = sstv_encode_scottie_s2_png_pcm("/mnt/sstv-tx.png", AUDIO_PLAY_RATE, &samples, &count);
            break;
        default:
            fprintf(stderr,"SSTV TX: encoder not implemented for %s\n", cont_mode_name(tx_mode));
            rc = -40;
            break;
    }
    if (rc < 0) {
        fprintf(stderr,"SSTV TX: PCM generation failed rc=%d\n",rc);
        tx_result = rc;
        tx_running = false;
        return NULL;
    }
    if (tx_abort) {
        free(samples);
        tx_result = -1;
        tx_running = false;
        return NULL;
    }

    /* Level setup (see the comment at the top of this file). */
    /* Automatic level control: always in the digital modes; in USB/LSB only
     * with the stock BASE, which has no TX codec gain slider (rev < 3, the
     * same test used by the settings page to show the slider). */
    const bool dig_mode = sstv_tx_mode_is_digital();
    const bool patched_base = x6100_control_get_base_ver().rev >= 3;
    const bool dig = dig_mode || !patched_base;
    fprintf(stderr, "SSTV TX: radio mode %d, digital=%d, BASE %s, ALC control %s\n",
            (int)cparam_i_get(cfg.cur.mode()), dig_mode ? 1 : 0,
            patched_base ? "patched" : "stock",
            dig ? "two-way" : (SSTV_ALC_IN_SSB ? "attenuation only" : "off (TX codec gain slider)"));
    float play_gain_offset = 0.0f;   /* ALSA part of the total level */
    float tx_gain_db = 0.0f;         /* digital part */
    float prev_tx_gain_db = 0.0f;
    float dig_gain_min = SSTV_TX_GAIN_MIN_DB;
    float dig_gain_max = SSTV_TX_GAIN_MAX_DB;
    const float target_pwr = sstv_tx_target_pwr();
    if (dig) {
        float start = SSTV_TX_START_DB, saved_level = 0.0f, saved_pwr = 0.0f;
        bool saved = sstv_tx_level_load(&saved_level, &saved_pwr);
        if (saved) {
            /* Patched BASE (rev >= 3) has a true power control: the same
             * audio level gives the set power at any setting (measured:
             * -35.6 dB at 5 W, -35.8 dB at 2 W).  The stock BASE does not,
             * so there the level is rescaled with the power, as FT8 does. */
            start = saved_level - SSTV_TX_START_MARGIN_DB;
            if (!patched_base)
                start += 10.0f * log10f(target_pwr / saved_pwr);
            if (start < SSTV_TX_LEVEL_MIN_DB) start = SSTV_TX_LEVEL_MIN_DB;
            if (start > SSTV_TX_LEVEL_MAX_DB) start = SSTV_TX_LEVEL_MAX_DB;
        }

        play_gain_offset = audio_set_play_vol(SSTV_TX_PLAY_DB);
        tx_gain_db = start - play_gain_offset;
        dig_gain_min = SSTV_TX_LEVEL_MIN_DB - play_gain_offset;
        dig_gain_max = SSTV_TX_LEVEL_MAX_DB - play_gain_offset;
        if (dig_gain_max > 0.0f) dig_gain_max = 0.0f;
        if (tx_gain_db < dig_gain_min) tx_gain_db = dig_gain_min;
        if (tx_gain_db > dig_gain_max) tx_gain_db = dig_gain_max;
        prev_tx_gain_db = tx_gain_db;
        if (saved)
            fprintf(stderr, "SSTV TX: saved level %.2f dB at %.1f W, target %.1f W -> start %.2f dB\n",
                    saved_level, saved_pwr, target_pwr, play_gain_offset + tx_gain_db);
        else
            fprintf(stderr, "SSTV TX: no saved level, target %.1f W -> start %.2f dB\n",
                    target_pwr, play_gain_offset + tx_gain_db);
        fprintf(stderr, "SSTV TX: ALSA play vol %.2f dB, digital gain %.2f dB\n",
                play_gain_offset, tx_gain_db);
    }
    const bool alc_active = dig || SSTV_ALC_IN_SSB;
    sstv_tx_win.prev_alc = -1.0f;
    sstv_tx_win_reset(sstv_tx_now_ms());
    double level_sum = 0.0;     /* readings close to the power aim */
    unsigned level_n = 0;

    fprintf(stderr,"SSTV TX: keying modem for %s (%zu samples)\n", cont_mode_name(tx_mode), count);
    radio_set_modem(true);
    clock_gettime(CLOCK_MONOTONIC, &tx_key_time);
    tx_key_time_valid = true;
    tx_started = true;

    /* Give the TX/codec path time to settle before the SSTV VIS leader starts.
     * Keep this out of the encoder so generated WAV files remain standard. */
    enum { TX_PREROLL_CHUNK = 2048 };
    const size_t preroll_count = ((size_t)AUDIO_PLAY_RATE * SSTV_TX_PREROLL_MS) / 1000u;
    static int16_t silence[TX_PREROLL_CHUNK] = {0};
    size_t preroll_sent = 0;
    while (preroll_sent < preroll_count && !tx_abort) {
        size_t part = preroll_count - preroll_sent;
        if (part > TX_PREROLL_CHUNK) part = TX_PREROLL_CHUNK;
        if (audio_play(silence, part) < 0) {
            rc = -30;
            break;
        }
        preroll_sent += part;
    }

    fprintf(stderr,"SSTV TX: preroll %zu/%zu silent samples\n",
            preroll_sent, preroll_count);

    size_t sent = 0;
    size_t alc_counter = 0;

    while (rc >= 0 && sent < count && !tx_abort) {
        size_t part = count - sent;
        if (part > 2048u) part = 2048u;

        /* Follow the FT8 cadence: allow the TX chain to settle first
         * (30 blocks, about 1.3 s), then consult the telemetry. */
        if (alc_active && alc_counter > 30) {
            if (dig) {
                uint32_t now = sstv_tx_now_ms();
                bool decided = false;
                float alc = 0.0f, pwr = 0.0f;
                float step = sstv_tx_dig_step(target_pwr, now, &decided, &alc, &pwr);
                if (step != 0.0f) {
                    tx_gain_db += step;
                    if (tx_gain_db < dig_gain_min) tx_gain_db = dig_gain_min;
                    if (tx_gain_db > dig_gain_max) tx_gain_db = dig_gain_max;
                }
                /* Remember the level of the windows where the radio is
                 * where we want it (level of that window = before the step). */
                if (decided && alc <= SSTV_TX_ALC_OK && pwr > 0.1f &&
                    fabsf(10.0f * log10f(pwr / (target_pwr * SSTV_TX_PWR_AIM))) < 0.3f) {
                    level_sum += play_gain_offset + prev_tx_gain_db;
                    level_n++;
                }
            } else {
                float correction = sstv_tx_alc_correction();
                if (correction < 0.0f) {
                    tx_gain_db += correction * 0.4f;
                    if (tx_gain_db < SSTV_TX_GAIN_MIN_DB)
                        tx_gain_db = SSTV_TX_GAIN_MIN_DB;
                    if (tx_gain_db > SSTV_TX_GAIN_MAX_DB)
                        tx_gain_db = SSTV_TX_GAIN_MAX_DB;
                }
            }
        }

        if (tx_gain_db == prev_tx_gain_db) {
            if (tx_gain_db != 0.0f)
                audio_gain_db(samples + sent, part, tx_gain_db, samples + sent);
        } else {
            audio_gain_db_transition(samples + sent, part,
                                     prev_tx_gain_db, tx_gain_db,
                                     samples + sent);
            fprintf(stderr, "SSTV TX: level %.2f dB (digital gain %.2f dB)\n",
                    play_gain_offset + tx_gain_db, tx_gain_db);
            prev_tx_gain_db = tx_gain_db;
        }

        if (audio_play(samples + sent, part) < 0) {
            rc = -30;
            break;
        }
        sent += part;
        alc_counter++;
    }

    /* Drain everything already accepted by PulseAudio before unkeying.  On an
     * abort this is at most the small amount already queued, not the image. */
    audio_play_wait();
    radio_set_modem(false);
    if (dig) {
        /* Save the average level of the good readings (radio at the power
         * aim, ALC low).  Without good readings the old value is kept. */
        if (level_n >= 2) {
            float level = (float)(level_sum / level_n);
            sstv_tx_level_save(level, target_pwr);
            fprintf(stderr, "SSTV TX: level %.2f dB at %.1f W saved (average of %u readings)\n",
                    level, target_pwr, level_n);
        } else {
            fprintf(stderr, "SSTV TX: level not saved (only %u good readings)\n", level_n);
        }
        audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    }
    tx_started = false;
    tx_key_time_valid = false;
    free(samples);

    if (tx_abort) {
        fprintf(stderr,"SSTV TX: aborted at %zu/%zu samples\n",sent,count);
        tx_result = -1;
    } else if (rc < 0) {
        fprintf(stderr,"SSTV TX: audio_play failed rc=%d\n",rc);
        tx_result = rc;
    } else {
        fprintf(stderr,"SSTV TX: %s complete\n", cont_mode_name(tx_mode));
        tx_progress = 1000;
        tx_result = 1;
    }
    tx_running = false;
    return NULL;
}


static uint64_t tx_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int tx_picture_name_compare(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

static void tx_picture_free_files(void)
{
    for (size_t i = 0; i < tx_picture_file_count; ++i)
        free(tx_picture_files[i]);
    free(tx_picture_files);
    tx_picture_files = NULL;
    tx_picture_file_count = 0;
    tx_picture_file_capacity = 0;
}

static void tx_picture_selector_refresh(void)
{
    tx_picture_free_files();

    DIR *dir = opendir(SSTV_TX_PICS_DIR);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            size_t len = strlen(name);

            /* Ignore hidden files, including macOS AppleDouble metadata (._*). */
            if (name[0] == '.')
                continue;

            if (len < 5 || strcmp(name + len - 4, ".png") != 0)
                continue;

            if (tx_picture_file_count == tx_picture_file_capacity) {
                size_t new_capacity = tx_picture_file_capacity ?
                                      tx_picture_file_capacity * 2 : 32;
                char **p = realloc(tx_picture_files,
                                   new_capacity * sizeof(*tx_picture_files));
                if (!p)
                    break;
                tx_picture_files = p;
                tx_picture_file_capacity = new_capacity;
            }

            char *copy = strdup(name);
            if (copy)
                tx_picture_files[tx_picture_file_count++] = copy;
        }
        closedir(dir);
    }

    qsort(tx_picture_files, tx_picture_file_count,
          sizeof(tx_picture_files[0]), tx_picture_name_compare);
}

static void tx_picture_native_size(sstv_mode_t mode,
                                   unsigned int *width,
                                   unsigned int *height)
{
    switch (mode) {
        case SSTV_MODE_ROBOT8_BW:
            *width = 160; *height = 120; break;
        case SSTV_MODE_ROBOT36:
        case SSTV_MODE_ROBOT72:
            *width = 320; *height = 240; break;
        case SSTV_MODE_MARTIN_M1:
        case SSTV_MODE_MARTIN_M2:
        case SSTV_MODE_SCOTTIE_S1:
        case SSTV_MODE_SCOTTIE_S2:
        default:
            *width = 320; *height = 256; break;
    }
}

static bool tx_picture_decode_png(const char *path,
                                  uint8_t **out_rgb,
                                  unsigned int *out_w,
                                  unsigned int *out_h)
{
    *out_rgb = NULL;
    *out_w = *out_h = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                                  NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(fp);
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 w = png_get_image_width(png_ptr, info_ptr);
    png_uint_32 h = png_get_image_height(png_ptr, info_ptr);
    int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);

    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    /* 1KO125: interlaced (Adam7) PNG: the rows come in several passes */
    int passes = png_set_interlace_handling(png_ptr);

    png_read_update_info(png_ptr, info_ptr);
    int channels = png_get_channels(png_ptr, info_ptr);
    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    if (channels != 3 && channels != 4) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    uint8_t *rgb = malloc((size_t)w * h * 3);
    /* interlaced: the whole picture, the passes fill it in; else one row */
    uint8_t *raw = malloc(passes > 1 ? rowbytes * h : rowbytes);
    if (!rgb || !raw) {
        free(rgb); free(raw);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    for (int pass = 0; pass < passes; ++pass)
    for (png_uint_32 y = 0; y < h; ++y) {
        uint8_t *row = passes > 1 ? raw + (size_t)y * rowbytes : raw;
        png_read_row(png_ptr, row, NULL);
        if (pass != passes - 1) continue;
        for (png_uint_32 x = 0; x < w; ++x) {
            const uint8_t *p = row + (size_t)x * channels;
            uint8_t *d = rgb + ((size_t)y * w + x) * 3;
            if (channels == 4 && p[3] != 255) {
                unsigned int a = p[3];
                d[0] = (uint8_t)((p[0] * a + 255u * (255u - a)) / 255u);
                d[1] = (uint8_t)((p[1] * a + 255u * (255u - a)) / 255u);
                d[2] = (uint8_t)((p[2] * a + 255u * (255u - a)) / 255u);
            } else {
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
            }
        }
    }

    png_read_end(png_ptr, NULL);
    free(raw);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    *out_rgb = rgb;
    *out_w = (unsigned int)w;
    *out_h = (unsigned int)h;
    return true;
}

static void tx_picture_aspect_fit(const uint8_t *src,
                                  unsigned int sw, unsigned int sh,
                                  uint8_t *dst,
                                  unsigned int dw, unsigned int dh)
{
    memset(dst, 0, (size_t)dw * dh * 3);

    double scale_x = (double)dw / (double)sw;
    double scale_y = (double)dh / (double)sh;
    double scale = scale_x < scale_y ? scale_x : scale_y;
    unsigned int rw = (unsigned int)floor(sw * scale + 0.5);
    unsigned int rh = (unsigned int)floor(sh * scale + 0.5);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (rw > dw) rw = dw;
    if (rh > dh) rh = dh;

    unsigned int x0 = (dw - rw) / 2;
    unsigned int y0 = (dh - rh) / 2;

    /* Bilinear resize.  This is run only after the 300 ms selection debounce,
     * never on every MFK detent. */
    for (unsigned int y = 0; y < rh; ++y) {
        double sy = ((double)y + 0.5) * sh / rh - 0.5;
        if (sy < 0.0) sy = 0.0;
        unsigned int y1 = (unsigned int)sy;
        unsigned int y2 = y1 + 1 < sh ? y1 + 1 : y1;
        double fy = sy - y1;

        for (unsigned int x = 0; x < rw; ++x) {
            double sx = ((double)x + 0.5) * sw / rw - 0.5;
            if (sx < 0.0) sx = 0.0;
            unsigned int x1 = (unsigned int)sx;
            unsigned int x2 = x1 + 1 < sw ? x1 + 1 : x1;
            double fx = sx - x1;

            for (unsigned int c = 0; c < 3; ++c) {
                double p11 = src[((size_t)y1 * sw + x1) * 3 + c];
                double p12 = src[((size_t)y1 * sw + x2) * 3 + c];
                double p21 = src[((size_t)y2 * sw + x1) * 3 + c];
                double p22 = src[((size_t)y2 * sw + x2) * 3 + c];
                double top = p11 + (p12 - p11) * fx;
                double bot = p21 + (p22 - p21) * fx;
                double v = top + (bot - top) * fy;
                dst[((size_t)(y0 + y) * dw + (x0 + x)) * 3 + c] =
                    (uint8_t)(v + 0.5);
            }
        }
    }
}

static bool tx_picture_load_and_prepare(const char *filename)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SSTV_TX_PICS_DIR, filename);

    uint8_t *src = NULL;
    unsigned int sw = 0, sh = 0;
    if (!tx_picture_decode_png(path, &src, &sw, &sh)) {
        fprintf(stderr, "SSTV TX UI: cannot load picture %s\n", path);
        return false;
    }

    unsigned int dw, dh;
    tx_picture_native_size(cont_mode, &dw, &dh);
    tx_picture_aspect_fit(src, sw, sh, tx_picture_rgb, dw, dh);
    free(src);

    tx_picture_width = dw;
    tx_picture_height = dh;
    tx_build_final_frame(false);
    snprintf(tx_picture_name, sizeof(tx_picture_name), "%s", filename);
    fprintf(stderr, "SSTV TX UI: prepared %s (%ux%u -> %ux%u, %s)\n",
            filename, sw, sh, dw, dh, cont_mode_name(cont_mode));
    return true;
}


static void tx_instruction_hide(void)
{
    if (tx_instruction_label) {
        lv_obj_del(tx_instruction_label);
        tx_instruction_label = NULL;
    }
}

static void tx_instruction_show(const char *text)
{
    tx_instruction_hide();

    tx_instruction_label = lv_label_create(dialog.obj);
    lv_obj_set_pos(tx_instruction_label, SSTV_TX_LIST_X, 8);
    lv_obj_set_width(tx_instruction_label, SSTV_TX_LIST_W);
    lv_obj_set_style_text_font(tx_instruction_label, &sony_28, 0);
    lv_obj_set_style_text_color(tx_instruction_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(tx_instruction_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(tx_instruction_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(tx_instruction_label, text);
}

static void tx_save_rx_canvas(void)
{
    memcpy(tx_saved_rx_canvas, sstv_buffer, sizeof(tx_saved_rx_canvas));
    tx_saved_rx_canvas_valid = true;
}

/* 1KO125: the snapshot of the RX picture taken when TX was pressed, shown on
 * the TX canvas (the RX canvas under the TX panel is never touched by TX) */
static void tx_restore_rx_canvas(void)
{
    if (!tx_saved_rx_canvas_valid || !tx_canvas) return;
    memcpy(tx_buffer, tx_saved_rx_canvas, sizeof(tx_saved_rx_canvas));
    lv_obj_invalidate(tx_canvas);
}

static void tx_rebuild_current_overlay(void)
{
    if (!tx_picture_width || !tx_picture_height) return;

    switch (tx_phase) {
        case SSTV_TX_PHASE_SELECT_PICTURE:
            memcpy(tx_final_rgb,tx_picture_rgb,
                   (size_t)tx_picture_width*tx_picture_height*3);
            break;
        case SSTV_TX_PHASE_SELECT_TYPE:
            /* Initial action phase: never replace the received image. */
            tx_restore_rx_canvas();
            return;
        case SSTV_TX_PHASE_SELECT_TEMPLATE:
        case SSTV_TX_PHASE_SELECT_TARGET:
            tx_template_render();
            break;
        case SSTV_TX_PHASE_SELECT_COLORS:
            if (tx_message_type == SSTV_TX_MESSAGE_CQ) tx_build_final_frame(true);
            else tx_template_render();
            break;
        default:
            memcpy(tx_final_rgb,tx_picture_rgb,
                   (size_t)tx_picture_width*tx_picture_height*3);
            break;
    }
    tx_picture_render_preview();
}

static void tx_build_final_frame(bool cq_overlay)
{
    if (!tx_picture_width || !tx_picture_height) return;
    size_t bytes=(size_t)tx_picture_width*tx_picture_height*3;
    memcpy(tx_final_rgb, tx_picture_rgb, bytes);

    if (cq_overlay) {
        int scale=(tx_picture_width<=160)?2:3;
        int text_h=7*scale;
        tx_draw_text_aligned_color(tx_final_rgb,tx_picture_width,tx_picture_height,
                                   5,"CQ CQ SSTV",scale,TX_TEXT_ALIGN_LEFT,
                                   tx_text_top);
        /* Middle line intentionally empty. */
        tx_draw_text_aligned_color(tx_final_rgb,tx_picture_width,tx_picture_height,
                                   (int)tx_picture_height-text_h-5,PARAM_T_GET(cfg.callsign()),
                                   scale,TX_TEXT_ALIGN_RIGHT,tx_text_bottom);
    }
}

static void tx_picture_render_preview(void)
{
    if (!tx_canvas || tx_picture_width == 0 || tx_picture_height == 0)
        return;

    lv_canvas_fill_bg(tx_canvas, lv_color_black(), LV_OPA_COVER);

    int out_w = SSTV_IMAGE_W;
    int out_h = (int)((uint64_t)tx_picture_height * out_w / tx_picture_width);
    if (out_h > SSTV_IMAGE_H) {
        out_h = SSTV_IMAGE_H;
        out_w = (int)((uint64_t)tx_picture_width * out_h / tx_picture_height);
    }
    int x0 = (SSTV_IMAGE_W - out_w) / 2;
    int y0 = (SSTV_IMAGE_H - out_h) / 2;

    for (int y = 0; y < out_h; ++y) {
        unsigned int sy = (unsigned int)((uint64_t)y * tx_picture_height / out_h);
        for (int x = 0; x < out_w; ++x) {
            unsigned int sx = (unsigned int)((uint64_t)x * tx_picture_width / out_w);
            size_t o = ((size_t)sy * tx_picture_width + sx) * 3;
            lv_canvas_set_px(tx_canvas, x0 + x, y0 + y,
                             lv_color_make(tx_final_rgb[o],
                                           tx_final_rgb[o + 1],
                                           tx_final_rgb[o + 2]));
        }
    }
    lv_obj_invalidate(tx_canvas);
}

static void tx_picture_schedule_preview(void)
{
    if (!tx_picture_table || tx_picture_file_count == 0)
        return;

    int row = ui_list_get_selected(tx_picture_table);
    if (row < 0 || (size_t)row >= tx_picture_file_count)
        return;

    tx_picture_pending = row;
    tx_picture_pending_since_ms = tx_now_ms();
}

static void tx_picture_selection_changed_cb(lv_event_t *e)
{
    (void)e;
    if (tx_ui_state == SSTV_TX_UI_SETUP &&
        tx_phase == SSTV_TX_PHASE_SELECT_PICTURE)
        tx_picture_schedule_preview();
}

static void tx_picture_activate_cb(void *user, int row)
{
    (void)user;
    if (tx_ui_state != SSTV_TX_UI_SETUP ||
        tx_phase != SSTV_TX_PHASE_SELECT_PICTURE ||
        tx_picture_file_count == 0)
        return;

    if (row < 0 || (size_t)row >= tx_picture_file_count)
        return;

    /* A quick rotate+press must confirm the picture currently highlighted,
     * even if the 300 ms preview debounce has not expired yet. */
    tx_picture_pending = row;
    if (tx_picture_selected != row) {
        if (!tx_picture_load_and_prepare(tx_picture_files[row]))
            return;
        tx_picture_selected = row;
        tx_picture_render_preview();
    }

    snprintf(tx_last_picture_file,sizeof(tx_last_picture_file),"%s",
             tx_picture_files[row]);

    lv_group_remove_obj(tx_picture_table);
    lv_obj_add_flag(tx_picture_table, LV_OBJ_FLAG_HIDDEN);

    fprintf(stderr, "SSTV TX UI: picture confirmed: %s\n", tx_picture_name);
    if (tx_message_type == SSTV_TX_MESSAGE_IMAGE_ONLY) {
        size_t bytes=(size_t)tx_picture_width*tx_picture_height*3;
        memcpy(tx_final_rgb,tx_picture_rgb,bytes);
        tx_picture_render_preview();
        tx_picture_selector_destroy();
        tx_instruction_hide();
        lv_group_add_obj(keyboard_group,sstv_canvas);
        lv_group_focus_obj(sstv_canvas);
        lv_group_set_editing(keyboard_group,true);
        if (tx_status()) lv_obj_clear_flag(tx_status(),LV_OBJ_FLAG_HIDDEN);
        fprintf(stderr,"SSTV TX UI: IMAGE ONLY picture confirmed -> RF\n");
        tx_start_final_rf();
        return;
    }
    if (tx_message_type == SSTV_TX_MESSAGE_CQ) {
        tx_build_final_frame(true);
        tx_picture_render_preview();
        tx_phase = SSTV_TX_PHASE_SELECT_COLORS;
        tx_color_selector_build();
        fprintf(stderr, "SSTV TX UI: CQ picture confirmed -> colors\n");
    } else {
        tx_phase = SSTV_TX_PHASE_SELECT_TEMPLATE;
        tx_template_selector_build();
        fprintf(stderr, "SSTV TX UI: REPLY picture confirmed -> templates\n");
    }
}

static void tx_picture_load_pending_preview(void)
{
    if (tx_picture_pending < 0 ||
        tx_picture_pending == tx_picture_selected ||
        (size_t)tx_picture_pending >= tx_picture_file_count)
        return;

    if (tx_now_ms() - tx_picture_pending_since_ms <
        SSTV_TX_PICTURE_DEBOUNCE_MS)
        return;

    int row = tx_picture_pending;
    if (tx_picture_load_and_prepare(tx_picture_files[row])) {
        tx_picture_selected = row;
        tx_picture_render_preview();
    }
}



/* ---- 1KO125: REPLAY - choose the received picture for the miniature ----
 * Same list as the RX Pics Browser (newest first, DN = denoised copy); the
 * TX canvas shows the highlighted picture.  The miniature is made from the
 * PNG file, not from the screen. */
static lv_obj_t *tx_rxpic_table = NULL;
static char **tx_rxpic_files = NULL;
static size_t tx_rxpic_count = 0, tx_rxpic_cap = 0;
static int tx_rxpic_selected = -1, tx_rxpic_pending = -1;
static uint64_t tx_rxpic_pending_since_ms = 0;
static uint8_t *tx_rxpic_rgb = NULL;            /* picture on the TX canvas (row tx_rxpic_selected) */
static unsigned int tx_rxpic_w = 0, tx_rxpic_h = 0;
static uint8_t *tx_replay_rgb = NULL;           /* confirmed picture: the miniature comes from here */
static unsigned int tx_replay_w = 0, tx_replay_h = 0;

static int tx_rxpic_compare(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sb, *sa);   /* newest timestamped filename first, as the browser */
}

static void tx_rxpic_free_files(void)
{
    for (size_t i = 0; i < tx_rxpic_count; ++i) free(tx_rxpic_files[i]);
    free(tx_rxpic_files);
    tx_rxpic_files = NULL;
    tx_rxpic_count = tx_rxpic_cap = 0;
}

static void tx_rxpic_refresh(void)
{
    tx_rxpic_free_files();
    DIR *dir = opendir(SSTV_SAVE_DIR);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        const char *name = e->d_name;
        size_t len = strlen(name);
        if (name[0] == '.' || len < 9 || strncmp(name, "sstv-", 5) != 0 ||
            strcmp(name + len - 4, ".png") != 0)
            continue;
        if (tx_rxpic_count == tx_rxpic_cap) {
            size_t cap = tx_rxpic_cap ? tx_rxpic_cap * 2 : 128;
            char **p = realloc(tx_rxpic_files, cap * sizeof(*p));
            if (!p) break;
            tx_rxpic_files = p;
            tx_rxpic_cap = cap;
        }
        char *copy = strdup(name);
        if (copy) tx_rxpic_files[tx_rxpic_count++] = copy;
    }
    closedir(dir);
    qsort(tx_rxpic_files, tx_rxpic_count, sizeof(tx_rxpic_files[0]), tx_rxpic_compare);
}

/* Any RGB picture on the TX canvas, fitted and centred on black */
static void tx_canvas_draw_rgb(const uint8_t *rgb, unsigned int w, unsigned int h)
{
    if (!tx_canvas) return;
    lv_canvas_fill_bg(tx_canvas, lv_color_black(), LV_OPA_COVER);
    if (rgb && w && h) {
        int out_w = SSTV_IMAGE_W;
        int out_h = (int)((uint64_t)h * out_w / w);
        if (out_h > SSTV_IMAGE_H) {
            out_h = SSTV_IMAGE_H;
            out_w = (int)((uint64_t)w * out_h / h);
        }
        int x0 = (SSTV_IMAGE_W - out_w) / 2, y0 = (SSTV_IMAGE_H - out_h) / 2;
        for (int y = 0; y < out_h; ++y) {
            unsigned int sy = (unsigned int)((uint64_t)y * h / out_h);
            for (int x = 0; x < out_w; ++x) {
                unsigned int sx = (unsigned int)((uint64_t)x * w / out_w);
                size_t o = ((size_t)sy * w + sx) * 3;
                lv_canvas_set_px(tx_canvas, x0 + x, y0 + y, lv_color_make(rgb[o], rgb[o + 1], rgb[o + 2]));
            }
        }
    }
    lv_obj_invalidate(tx_canvas);
}

static bool tx_rxpic_show(int row)
{
    if (row < 0 || (size_t)row >= tx_rxpic_count) return false;
    char path[256];
    snprintf(path, sizeof(path), SSTV_SAVE_DIR "/%s", tx_rxpic_files[row]);
    uint8_t *rgb = NULL;
    unsigned int w = 0, h = 0;
    if (!tx_picture_decode_png(path, &rgb, &w, &h)) {
        fprintf(stderr, "SSTV TX UI: cannot read received picture %s\n", path);
        return false;
    }
    free(tx_rxpic_rgb);
    tx_rxpic_rgb = rgb;
    tx_rxpic_w = w;
    tx_rxpic_h = h;
    tx_rxpic_selected = row;
    tx_canvas_draw_rgb(rgb, w, h);
    return true;
}

/* Row: "n | date | time [DN]", as the RX Pics Browser */
static void tx_rxpic_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    if (index < 0 || (size_t)index >= tx_rxpic_count) return;
    const char *name = tx_rxpic_files[index];
    snprintf(row->cell[0], UI_LIST_CELL_LEN, "%d", index + 1);
    if (ui_list_parse_stamp_ext(name, "sstv-", "_dn.png", row->cell[1], UI_LIST_CELL_LEN,
                                row->cell[2], UI_LIST_CELL_LEN)) {
        strncat(row->cell[2], " DN", UI_LIST_CELL_LEN - strlen(row->cell[2]) - 1);
    } else if (!ui_list_parse_stamp(name, "sstv-", row->cell[1], UI_LIST_CELL_LEN,
                                    row->cell[2], UI_LIST_CELL_LEN)) {
        snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", name);
        row->cell[2][0] = '\0';
    }
}

static void tx_rxpic_selection_changed_cb(lv_event_t *e)
{
    (void)e;
    if (tx_ui_state != SSTV_TX_UI_SETUP || tx_phase != SSTV_TX_PHASE_SELECT_RX_PICTURE || !tx_rxpic_table)
        return;
    tx_rxpic_pending = ui_list_get_selected(tx_rxpic_table);
    tx_rxpic_pending_since_ms = tx_now_ms();
}

static void tx_rxpic_load_pending_preview(void)
{
    if (tx_rxpic_pending < 0 || tx_rxpic_pending == tx_rxpic_selected) return;
    if (tx_now_ms() - tx_rxpic_pending_since_ms < SSTV_TX_PICTURE_DEBOUNCE_MS) return;
    int row = tx_rxpic_pending;
    if (!tx_rxpic_show(row)) tx_rxpic_pending = tx_rxpic_selected;   /* do not retry an unreadable file */
}

static void tx_rxpic_selector_destroy(void)
{
    if (tx_rxpic_table) {
        lv_group_remove_obj(tx_rxpic_table);
        lv_obj_del(tx_rxpic_table);
        tx_rxpic_table = NULL;
    }
    tx_rxpic_free_files();
    free(tx_rxpic_rgb);
    tx_rxpic_rgb = NULL;
    tx_rxpic_w = tx_rxpic_h = 0;
    tx_rxpic_selected = tx_rxpic_pending = -1;
}

static void tx_rxpic_activate_cb(void *user, int row)
{
    (void)user;
    if (tx_ui_state != SSTV_TX_UI_SETUP || tx_phase != SSTV_TX_PHASE_SELECT_RX_PICTURE ||
        row < 0 || (size_t)row >= tx_rxpic_count)
        return;
    /* a quick rotate+press confirms the highlighted row even before its preview */
    if (tx_rxpic_selected != row && !tx_rxpic_show(row)) return;

    free(tx_replay_rgb);
    tx_replay_rgb = tx_rxpic_rgb;            /* the selector gives its picture away */
    tx_replay_w = tx_rxpic_w;
    tx_replay_h = tx_rxpic_h;
    tx_rxpic_rgb = NULL;
    fprintf(stderr, "SSTV TX UI: REPLAY miniature from %s (%ux%u) -> pictures\n",
            tx_rxpic_files[row], tx_replay_w, tx_replay_h);

    tx_rxpic_selector_destroy();
    tx_phase = SSTV_TX_PHASE_SELECT_PICTURE;
    tx_picture_selector_build();
}

static void tx_rxpic_selector_build(void)
{
    tx_rxpic_selector_destroy();
    tx_rxpic_refresh();

    if (tx_status()) lv_obj_add_flag(tx_status(), LV_OBJ_FLAG_HIDDEN);
    tx_instruction_show("Rotate MFK to select the received picture for the miniature. Press MFK to confirm.");

    ui_list_cfg_t cfg = {
        .width = SSTV_TX_LIST_W,
        .height = SSTV_TX_LIST_H,
        .col_count = 3,
        .cols = {
            { .x = 4,   .w = 30,  .align = UI_LIST_ALIGN_RIGHT, .muted = true },
            { .x = 42,  .w = 130, .align = UI_LIST_ALIGN_LEFT },
            { .x = 176, .w = 108, .align = UI_LIST_ALIGN_LEFT },
        },
        .row_cb = tx_rxpic_row_cb,
        .activate_cb = tx_rxpic_activate_cb,
        .empty_text = "No received pictures",
    };
    tx_rxpic_table = tx_list_create(&cfg, SSTV_TX_LIST_X, SSTV_TX_LIST_Y);
    ui_list_set_count(tx_rxpic_table, (int)tx_rxpic_count);

    if (tx_rxpic_count == 0) {
        lv_group_add_obj(keyboard_group, sstv_canvas);
        lv_group_focus_obj(sstv_canvas);
        lv_group_set_editing(keyboard_group, true);
        tx_canvas_draw_rgb(NULL, 0, 0);
        return;
    }
    lv_obj_add_event_cb(tx_rxpic_table, tx_rxpic_selection_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    tx_list_focus(tx_rxpic_table);
    ui_list_select(tx_rxpic_table, 0);       /* the newest received picture */
    tx_rxpic_pending = 0;
    tx_rxpic_pending_since_ms = tx_now_ms() - SSTV_TX_PICTURE_DEBOUNCE_MS;
}

static void tx_type_selector_destroy(void)
{
    tx_type_preview_pending = false;
    if (tx_type_help_label) {
        lv_obj_del(tx_type_help_label);
        tx_type_help_label = NULL;
    }
    if (tx_type_table) {
        lv_group_remove_obj(tx_type_table);
        lv_obj_del(tx_type_table);
        tx_type_table = NULL;
    }
}


static void tx_template_free_files(void)
{
    for (size_t i=0;i<tx_template_file_count;i++) free(tx_template_files[i]);
    free(tx_template_files);
    tx_template_files=NULL;
    tx_template_file_count=0;
    tx_template_file_capacity=0;
}

static int tx_template_name_compare(const void *a,const void *b)
{
    return strcasecmp(*(const char * const *)a,*(const char * const *)b);
}

static void tx_template_refresh(void)
{
    tx_template_free_files();
    DIR *dir=opendir(SSTV_TX_TEXT_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry=readdir(dir))!=NULL) {
        const char *name=entry->d_name;
        size_t len=strlen(name);
        if (name[0]=='.' || len<9 || strcmp(name+len-8,".sstvtxt")!=0) continue;
        if (tx_template_file_count==tx_template_file_capacity) {
            size_t nc=tx_template_file_capacity?tx_template_file_capacity*2:16;
            char **p=realloc(tx_template_files,nc*sizeof(*p));
            if (!p) break;
            tx_template_files=p; tx_template_file_capacity=nc;
        }
        char *copy=strdup(name);
        if (copy) tx_template_files[tx_template_file_count++]=copy;
    }
    closedir(dir);
    qsort(tx_template_files,tx_template_file_count,sizeof(tx_template_files[0]),
          tx_template_name_compare);
}

static bool tx_json_string(const char *json,const char *key,char *out,size_t outsz)
{
    out[0]='\0';
    char pat[64];
    snprintf(pat,sizeof(pat),"\"%s\"",key);
    const char *p=strstr(json,pat);
    if (!p) return true; /* missing is allowed */
    p=strchr(p+strlen(pat),':');
    if (!p) return false;
    p++;
    while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
    if (!strncmp(p,"null",4)) return true;
    if (*p!='"') return false;
    p++;
    size_t n=0;
    while (*p && *p!='"') {
        char c=*p++;
        if (c=='\\' && *p) {
            char e=*p++;
            if (e=='n') c=' ';
            else if (e=='t') c=' ';
            else if (e=='"'||e=='\\'||e=='/') c=e;
            else c=e;
        }
        if (n+1<outsz) out[n++]=c;
    }
    if (*p!='"') return false;
    out[n]='\0';
    return true;
}

static bool tx_template_load(const char *filename)
{
    char path[512];
    snprintf(path,sizeof(path),"%s/%s",SSTV_TX_TEXT_DIR,filename);
    FILE *fp=fopen(path,"rb");
    if (!fp) return false;
    if (fseek(fp,0,SEEK_END)!=0) { fclose(fp); return false; }
    long sz=ftell(fp);
    if (sz<0 || sz>16384) { fclose(fp); return false; }
    rewind(fp);
    char *json=malloc((size_t)sz+1);
    if (!json) { fclose(fp); return false; }
    size_t got=fread(json,1,(size_t)sz,fp);
    fclose(fp);
    json[got]='\0';
    bool ok=tx_json_string(json,"middle",tx_template_middle,sizeof(tx_template_middle)) &&
            tx_json_string(json,"bottom",tx_template_bottom,sizeof(tx_template_bottom));
    free(json);
    return ok;
}

static void tx_replay_draw_rx_thumbnail(uint8_t *rgb,
                                        unsigned int w, unsigned int h)
{
    /* 1KO125: from the received picture chosen in the REPLAY phase */
    if (!rgb || !w || !h || !tx_replay_rgb || !tx_replay_w || !tx_replay_h) return;
    const unsigned int sw = tx_replay_w, sh = tx_replay_h;

    /* About 96 px wide on a 320x256 TX frame, aspect of the received picture */
    int thumb_w=(int)w*96/320;
    if (thumb_w < 48) thumb_w=48;
    int thumb_h=(int)((int64_t)thumb_w*sh/sw);

    /* Leave room for the bottom line. */
    int bottom_reserved=(w<=160)?24:38;
    /* Place the replay thumbnail 15 px from the left edge on a 320 px frame.
     * Move it 3 px upward to make room for the 3 px black border below. */
    int x0=(int)w*15/320;
    int y0=(int)h*135/256 - 3;
    if (y0+thumb_h+3 > (int)h-bottom_reserved)
        y0=(int)h-bottom_reserved-thumb_h-3;
    if (y0<3) y0=3;

    /* each miniature pixel = mean of the picture pixels it covers */
    for (int y=0;y<thumb_h;y++) {
        unsigned int sy0=(unsigned int)((int64_t)y*sh/thumb_h);
        unsigned int sy1=(unsigned int)((int64_t)(y+1)*sh/thumb_h);
        if (sy1<=sy0) sy1=sy0+1;
        for (int x=0;x<thumb_w;x++) {
            unsigned int sx0=(unsigned int)((int64_t)x*sw/thumb_w);
            unsigned int sx1=(unsigned int)((int64_t)(x+1)*sw/thumb_w);
            if (sx1<=sx0) sx1=sx0+1;
            unsigned int r=0,g=0,b=0,n=0;
            for (unsigned int yy=sy0;yy<sy1 && yy<sh;yy++)
                for (unsigned int xx=sx0;xx<sx1 && xx<sw;xx++) {
                    const uint8_t *p=tx_replay_rgb+((size_t)yy*sw+xx)*3;
                    r+=p[0]; g+=p[1]; b+=p[2]; n++;
                }
            if (!n) continue;
            tx_rgb_pixel(rgb,w,h,x0+x,y0+y,(uint8_t)(r/n),(uint8_t)(g/n),(uint8_t)(b/n));
        }
    }

    /* Solid 3 px black border around the replay thumbnail. */
    for (int b=1;b<=3;b++) {
        for (int x=-b;x<thumb_w+b;x++) {
            tx_rgb_pixel(rgb,w,h,x0+x,y0-b,0,0,0);
            tx_rgb_pixel(rgb,w,h,x0+x,y0+thumb_h-1+b,0,0,0);
        }
        for (int y=-b;y<thumb_h+b;y++) {
            tx_rgb_pixel(rgb,w,h,x0-b,y0+y,0,0,0);
            tx_rgb_pixel(rgb,w,h,x0+thumb_w-1+b,y0+y,0,0,0);
        }
    }
}

static void tx_template_render(void)
{
    if (!tx_picture_width || !tx_picture_height) return;
    memcpy(tx_final_rgb,tx_picture_rgb,
           (size_t)tx_picture_width*tx_picture_height*3);

    int outer_scale=(tx_picture_width<=160)?2:3;
    tx_draw_text_aligned_color(tx_final_rgb,tx_picture_width,tx_picture_height,5,
                               tx_target_callsign[0] ? tx_target_callsign : "<TARGET CALLSIGN>",
                               outer_scale,TX_TEXT_ALIGN_LEFT,tx_text_top);

    tx_draw_middle_wrapped(tx_final_rgb,tx_picture_width,tx_picture_height,
                           tx_template_middle);

    if (tx_message_type == SSTV_TX_MESSAGE_REPLAY)
        tx_replay_draw_rx_thumbnail(tx_final_rgb,tx_picture_width,tx_picture_height);

    char bottom[256];
    if (tx_template_bottom[0])
        snprintf(bottom,sizeof(bottom),"%s%s",tx_template_bottom,PARAM_T_GET(cfg.callsign()));
    else
        snprintf(bottom,sizeof(bottom),"%s",PARAM_T_GET(cfg.callsign()));
    int scale=(tx_picture_width<=160)?2:3;
    tx_draw_text_aligned_color(tx_final_rgb,tx_picture_width,tx_picture_height,
                               (int)tx_picture_height-7*scale-5,bottom,
                               scale,TX_TEXT_ALIGN_RIGHT,tx_text_bottom);
}

static void tx_template_render_selected(void)
{
    if (!tx_template_table || !tx_template_file_count) return;
    int row=ui_list_get_selected(tx_template_table);
    if (row<0 || (size_t)row>=tx_template_file_count) return;
    if (!tx_template_load(tx_template_files[row])) {
        fprintf(stderr,"SSTV TX UI: invalid template %s\n",tx_template_files[row]);
        return;
    }
    tx_template_render();
    tx_picture_render_preview();
}

static void tx_template_selection_changed_cb(lv_event_t *e)
{
    (void)e;
    if (tx_ui_state==SSTV_TX_UI_SETUP &&
        tx_phase==SSTV_TX_PHASE_SELECT_TEMPLATE)
        tx_template_preview_pending=true;
}

static void tx_template_activate_cb(void *user, int row)
{
    (void)user;
    if (tx_ui_state!=SSTV_TX_UI_SETUP ||
        tx_phase!=SSTV_TX_PHASE_SELECT_TEMPLATE || !tx_template_file_count) return;
    tx_template_preview_pending=false;
    tx_template_render_selected();

    if (row<0 || (size_t)row>=tx_template_file_count) return;
    char shown[256];
    snprintf(shown,sizeof(shown),"%s",tx_template_files[row]);
    size_t n=strlen(shown); if(n>=8) shown[n-8]='\0';

    fprintf(stderr,"SSTV TX UI: template confirmed: %s\n",shown);
    tx_template_selector_destroy();
    /* Preserve REPLAY; normal non-CQ traffic remains REPLY. */
    if (tx_message_type != SSTV_TX_MESSAGE_REPLAY)
        tx_message_type=SSTV_TX_MESSAGE_REPLY;
    tx_phase=SSTV_TX_PHASE_SELECT_COLORS;
    tx_color_selector_build();
}

static void tx_template_selector_destroy(void)
{
    tx_template_preview_pending=false;
    if (tx_template_table) {
        lv_group_remove_obj(tx_template_table);
        lv_obj_del(tx_template_table);
        tx_template_table=NULL;
    }
    tx_template_free_files();
}

static void tx_template_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    if (index<0 || (size_t)index>=tx_template_file_count) return;
    snprintf(row->cell[0],UI_LIST_CELL_LEN,"%d",index+1);
    snprintf(row->cell[1],UI_LIST_CELL_LEN,"%s",tx_template_files[index]);
    size_t n=strlen(row->cell[1]);
    if (n>=8 && strcmp(row->cell[1]+n-8,".sstvtxt")==0) row->cell[1][n-8]='\0';
}

static void tx_template_selector_build(void)
{
    tx_template_selector_destroy();
    tx_template_refresh();
    if(tx_status()) lv_obj_add_flag(tx_status(),LV_OBJ_FLAG_HIDDEN);
    tx_instruction_show("Rotate MFK to select a TX text template. Press MFK to confirm.");

    ui_list_cfg_t cfg = {
        .width = SSTV_TX_LIST_W,
        .height = SSTV_TX_LIST_H,
        .col_count = 2,
        .cols = { TX_LIST_NUM_COL, TX_LIST_NAME_COL },
        .row_cb = tx_template_row_cb,
        .activate_cb = tx_template_activate_cb,
        .empty_text = "No TX templates found",
    };
    tx_template_table=tx_list_create(&cfg,SSTV_TX_LIST_X,SSTV_TX_LIST_Y);
    ui_list_set_count(tx_template_table,(int)tx_template_file_count);

    if (!tx_template_file_count) {
        lv_group_add_obj(keyboard_group,sstv_canvas);
        lv_group_focus_obj(sstv_canvas);
        lv_group_set_editing(keyboard_group,true);
        return;
    }

    lv_obj_add_event_cb(tx_template_table,tx_template_selection_changed_cb,
                        LV_EVENT_VALUE_CHANGED,NULL);
    tx_list_focus(tx_template_table);
    tx_template_preview_pending=true;
}


static void tx_target_selector_destroy(void)
{
    /* The virtual keyboard is a separate window on the screen: it is not a
     * child of the SSTV dialog, so it must be closed explicitly whenever the
     * target selector disappears (Cancel TX, dialog closed, phase change). */
    if (tx_target_keyboard_active) {
        tx_target_keyboard_active=false;
        textarea_window_close();
    }
    tx_target_keyboard_open_pending=false;
    tx_target_keyboard_obj=NULL;
    tx_target_keyboard_force_until=0;
    if (tx_target_table) {
        lv_group_remove_obj(tx_target_table);
        lv_obj_del(tx_target_table);
        tx_target_table=NULL;
    }
    if (tx_target_label) {
        lv_obj_del(tx_target_label);
        tx_target_label=NULL;
    }
    if (tx_target_callsign_label) {
        lv_obj_del(tx_target_callsign_label);
        tx_target_callsign_label=NULL;
    }
}

static bool tx_target_keyboard_cancel_cb(void)
{
    tx_target_keyboard_active=false;
    textarea_window_close();
    tx_type_selector_build();
    return true;
}

static bool tx_target_keyboard_ok_cb(void)
{
    const char *value=textarea_window_get();
    if (value) {
        while (*value==' ' || *value=='\t') value++;
        snprintf(tx_target_callsign,sizeof(tx_target_callsign),"%s",value);
        size_t n=strlen(tx_target_callsign);
        while (n && (tx_target_callsign[n-1]==' ' ||
                     tx_target_callsign[n-1]=='\t')) tx_target_callsign[--n]='\0';
        for (size_t i=0;tx_target_callsign[i];i++) {
            if (tx_target_callsign[i]>='a' && tx_target_callsign[i]<='z')
                tx_target_callsign[i]-=('a'-'A');
        }
    }
    tx_target_keyboard_active=false;
    textarea_window_close();
    tx_type_selector_build();
    return true;
}

/* Called by main_screen on every radio / microphone key press other than MFK
 * (and the microphone ENTER-like FINP key, which confirms the keyboard).
 * If the target-callsign keyboard is open it is closed exactly as ESC would
 * do, so it can never stay on screen without focus.  The key itself is then
 * processed normally. */
void dialog_sstv_keyboard_dismiss(void)
{
    if (!tx_target_keyboard_active) return;
    tx_target_keyboard_active=false;
    textarea_window_close();
    if (tx_ui_state==SSTV_TX_UI_SETUP &&
        tx_phase==SSTV_TX_PHASE_SELECT_TARGET)
        tx_type_selector_build();
}

static lv_obj_t *tx_find_keyboard_recursive(lv_obj_t *parent)
{
    if (!parent) return NULL;
    if (lv_obj_check_type(parent,&lv_keyboard_class)) return parent;

    uint32_t count=lv_obj_get_child_cnt(parent);
    for (uint32_t i=0;i<count;i++) {
        lv_obj_t *found=tx_find_keyboard_recursive(lv_obj_get_child(parent,i));
        if (found) return found;
    }
    return NULL;
}

static void tx_target_keyboard_force_lower(void)
{
    if (!tx_target_keyboard_obj)
        tx_target_keyboard_obj=tx_find_keyboard_recursive(lv_scr_act());

    if (tx_target_keyboard_obj &&
        lv_obj_is_valid(tx_target_keyboard_obj)) {
        lv_keyboard_set_mode(tx_target_keyboard_obj,
                             LV_KEYBOARD_MODE_TEXT_LOWER);
    } else {
        tx_target_keyboard_obj=NULL;
    }
}

static void tx_target_keyboard_open_now(void)
{
    /* Same opening path used by the stock CALLSIGN dialog.
     * textarea_window_open() itself selects LV_KEYBOARD_MODE_TEXT_UPPER. */
    textarea_window_open(tx_target_keyboard_ok_cb,tx_target_keyboard_cancel_cb);
    tx_target_keyboard_active=true;

    /* Keep the actual LVGL keyboard object.  Some front-panel events arrive
     * after the window is created and can change its page, so Phase 4 keeps
     * forcing TEXT_LOWER for the first 500 ms after opening. */
    tx_target_keyboard_obj=tx_find_keyboard_recursive(lv_scr_act());
    tx_target_keyboard_force_until=lv_tick_get()+500;
    tx_target_keyboard_force_lower();

    lv_obj_t *text=textarea_window_text();
    lv_textarea_set_accepted_chars(text,
        "0123456789/"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz");
    lv_textarea_set_max_length(text,sizeof(tx_target_callsign)-1);
    lv_textarea_set_placeholder_text(text,"Target callsign");
    textarea_window_set(tx_target_callsign);
}



static void tx_color_apply(int row)
{
    if (row<0 || row>=20) row=0;
    tx_color_selected=row;
    tx_last_color_selected=row;
    tx_text_top=tx_color_combos[row].top;
    tx_text_middle=tx_color_combos[row].middle;
    tx_text_bottom=tx_color_combos[row].bottom;
}

static void tx_color_render_selected(void)
{
    if (!tx_color_table) return;
    int row=ui_list_get_selected(tx_color_table);
    if (row<0 || row>=20) return;
    tx_color_apply(row);
    if (tx_message_type == SSTV_TX_MESSAGE_CQ) tx_build_final_frame(true);
    else tx_template_render();
    tx_picture_render_preview();
}

static void tx_color_selection_changed_cb(lv_event_t *e)
{
    (void)e;
    if (tx_ui_state==SSTV_TX_UI_SETUP &&
        tx_phase==SSTV_TX_PHASE_SELECT_COLORS) {
        tx_color_preview_pending=true;
        tx_color_preview_since_ms=lv_tick_get();
    }
}


static void tx_lock_radio(void)
{
    if (tx_locks_held) return;
    tx_saved_lock_freq = lm_get_freq();
    tx_saved_lock_mode = lm_get_mode();
    tx_saved_lock_band = lm_get_band();
    lm_set_freq(true);
    lm_set_mode(true);
    lm_set_band(true);
    tx_locks_held = true;
}

static void tx_unlock_radio(void)
{
    if (!tx_locks_held) return;
    lm_set_freq(tx_saved_lock_freq);
    lm_set_mode(tx_saved_lock_mode);
    lm_set_band(tx_saved_lock_band);
    tx_locks_held = false;
}


static void tx_return_to_auto_rx(void)
{
    tx_unlock_radio();
    tx_ui_state=SSTV_TX_UI_RX;
    tx_phase=SSTV_TX_PHASE_NONE;
    btn_tx.label="TX";
    snprintf(tx_mode_label,sizeof(tx_mode_label),"RX Mode\n%s",cont_rx_mode_name());
    btn_cont_mode.label=tx_mode_label;

    pthread_mutex_lock(&live_mutex);
    live_enabled=true;
    live_capturing=false;
    clock_gettime(CLOCK_MONOTONIC, &live_dbg_listen_ts);
    live_dbg_listen_ts_valid = true;
    live_dbg_vis_checks = 0;
    live_dbg_vis_check_samples = 0;
    fprintf(stderr, "SSTV DBG: LISTEN START SR=%d\n", SSTV_SAMPLE_RATE);
    live_ring_pos=live_ring_count=0;
    live_wait_samples=live_wait_log_mark=0;
    if (!live_decode_pending) {          /* 1KO125: a partial picture may still wait for the decoder */
        free(live_samples);
        live_samples=NULL;
        live_written=live_target=0;
    }
    pthread_mutex_unlock(&live_mutex);

    tx_panel_show(false);                /* 1KO125: the RX panel, as the receiver left it */
    rx_ui_resync();
    if (btn_tx.disp_btn) buttons_refresh(&btn_tx);
    if (btn_cont_mode.disp_btn) buttons_refresh(&btn_cont_mode);
    update_rx_controls();
}

/* 1KO125: the receivers run during the whole TX setup and stop only here,
 * when RF starts.  A picture being received is not thrown away: what has
 * arrived is decoded and saved like a complete one (the decoder fills the
 * missing rows with black).  The decoder itself does not need the audio, so a
 * decode/denoise in course goes on during the transmission. */
static void tx_stop_receivers_for_rf(void)
{
    pthread_mutex_lock(&live_mutex);
    if (live_capturing) {
        live_capturing = false;
        if (live_samples && live_written > SSTV_SAMPLE_RATE * 3) {
            live_decode_pending = true;          /* decoded as soon as the decoder is free */
            fprintf(stderr, "SSTV LIVE: TX - %s capture closed after %.1f s, partial picture decoded\n",
                    live_info.name, (double)live_written / SSTV_SAMPLE_RATE);
        } else {
            free(live_samples); live_samples = NULL; live_written = live_target = 0;
            fprintf(stderr, "SSTV LIVE: TX - capture too short, discarded\n");
        }
    }
    live_enabled = false;
    live_ring_pos = live_ring_count = 0;
    live_wait_samples = live_wait_log_mark = 0;

    if (cont_capturing) {
        cont_capturing = false;
        bool no_mode = (cont_rx_mode == SSTV_MODE_UNKNOWN && !cont_locked);
        if (cont_written > 0 && !no_mode) {
            cont_decode_mode = (cont_rx_mode == SSTV_MODE_UNKNOWN) ? cont_detected : cont_rx_mode;
            cont_decode_pending = true;
            fprintf(stderr, "SSTV CONT: TX - Manual capture closed after %.1f s, decoded as %s\n",
                    (double)cont_written / SSTV_SAMPLE_RATE, cont_mode_name(cont_decode_mode));
        } else {
            free(cont_samples); cont_samples = NULL; cont_capacity = cont_written = 0;
            cont_event = SSTV_CONT_EV_NONE;
            fprintf(stderr, "SSTV CONT: TX - Manual capture discarded (no SSTV mode)\n");
        }
        btn_cont_start.label = "Start\nManual RX";
    }
    pthread_mutex_unlock(&live_mutex);
}

static bool tx_start_final_rf_mode(sstv_mode_t mode)
{
    if (!tx_picture_width || !tx_picture_height) {
        tx_set_status("SSTV TX: no final image.");
        return false;
    }

    /* The encoder consumes this exact composed framebuffer.  It is already at
     * the selected mode's native geometry, so the encoder's PNG import does
     * not change the WYSIWYG composition. */
    if (!save_tx_png("/mnt/sstv-tx.png",tx_final_rgb,
                     tx_picture_width,tx_picture_height)) {
        tx_set_status("SSTV TX: cannot prepare image.");
        return false;
    }

    if (tx_thread_valid) {
        pthread_join(tx_thread,NULL);
        tx_thread_valid=false;
        tx_unlock_radio();
    }

    tx_stop_receivers_for_rf();

    /* Snapshot the requested mode now.  Normal TX passes cont_mode;
     * REPEAT LAST TX passes the mode stored with the successful backup. */
    tx_mode=mode;
    tx_abort=false;
    tx_started=false;
    tx_key_time_valid=false;
    tx_progress=0;
    tx_result=0;
    tx_running=true;

    tx_ui_state=SSTV_TX_UI_RF;
    tx_phase=SSTV_TX_PHASE_NONE;
    btn_tx.label="Halt TX";
    snprintf(tx_mode_label,sizeof(tx_mode_label),"TX Mode\n%s",cont_mode_name(tx_mode));
    btn_cont_mode.label=tx_mode_label;

    if (tx_status()) lv_obj_clear_flag(tx_status(),LV_OBJ_FLAG_HIDDEN);
    {
        char msg[96];
        snprintf(msg,sizeof(msg),"Preparing %s TX...",cont_mode_name(tx_mode));
        tx_set_status(msg);
    }
    if (tx_bar()) {
        lv_bar_set_value(tx_bar(),0,LV_ANIM_OFF);
        lv_obj_clear_flag(tx_bar(),LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_tx.disp_btn) buttons_refresh(&btn_tx);
    if (btn_cont_mode.disp_btn) buttons_refresh(&btn_cont_mode);
    update_rx_controls();

    tx_lock_radio();
    int rc=pthread_create(&tx_thread,NULL,tx_thread_cb,NULL);
    if (rc != 0) {
        fprintf(stderr,"SSTV TX: pthread_create failed: %d\n",rc);
        tx_running=false;
        tx_result=-31;
        tx_set_status("SSTV TX: thread creation failed.");
        if (tx_bar()) lv_obj_add_flag(tx_bar(),LV_OBJ_FLAG_HIDDEN);
        tx_return_to_auto_rx();
        return false;
    }

    tx_thread_valid=true;
    fprintf(stderr,"SSTV TX: interactive RF start %s, colors=%d, type=%s\n",
            cont_mode_name(tx_mode),tx_color_selected+1,
            tx_message_type == SSTV_TX_MESSAGE_CQ ? "CQ" :
            (tx_message_type == SSTV_TX_MESSAGE_REPLAY ? "REPLAY" :
             (tx_message_type == SSTV_TX_MESSAGE_IMAGE_ONLY ? "IMAGE ONLY" :
              (tx_message_type == SSTV_TX_MESSAGE_REPEAT_LAST ? "REPEAT LAST" : "REPLY"))));
    return true;
}

static bool tx_start_final_rf(void)
{
    return tx_start_final_rf_mode(cont_mode);
}

static void tx_color_activate_cb(void *user, int index)
{
    (void)user;
    (void)index;
    if (tx_ui_state!=SSTV_TX_UI_SETUP ||
        tx_phase!=SSTV_TX_PHASE_SELECT_COLORS || !tx_color_table) return;

    /* Confirm exactly the highlighted row even if deferred preview has not run. */
    tx_color_preview_pending=false;
    tx_color_preview_since_ms=0;
    tx_color_render_selected();

    tx_color_selector_destroy();
    tx_instruction_hide();
    lv_group_add_obj(keyboard_group,sstv_canvas);
    lv_group_focus_obj(sstv_canvas);
    lv_group_set_editing(keyboard_group,true);
    if (tx_status()) lv_obj_clear_flag(tx_status(),LV_OBJ_FLAG_HIDDEN);

    /* Phase 5 is the single RF launch point for both CQ and REPLY. */
    tx_start_final_rf();
}

static void tx_color_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    (void)index;
    (void)row;      /* no text: tx_color_draw_cb paints the swatches */
}

static void tx_color_draw_cb(void *user, int index, lv_draw_ctx_t *ctx,
                             const lv_area_t *area, bool selected)
{
    (void)user;
    (void)selected;
    if (index<0 || index>=20) return;

    /* Three colour swatches: top, middle and bottom text colour. */
    const tx_color_combo_t *c=&tx_color_combos[index];
    tx_rgb_t colors[3]={c->top,c->middle,c->bottom};
    int box=22, gap=10;
    int total=3*box+2*gap;
    int x=area->x1+(lv_area_get_width(area)-total)/2;
    int y=area->y1+(lv_area_get_height(area)-box)/2;

    for (int i=0;i<3;i++) {
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_opa=LV_OPA_COVER;
        rd.bg_color=lv_color_make(colors[i].r,colors[i].g,colors[i].b);
        rd.border_width=2;
        rd.border_opa=LV_OPA_COVER;
        rd.border_color=lv_color_black();
        rd.radius=3;
        lv_area_t r={x+i*(box+gap),y,x+i*(box+gap)+box-1,y+box-1};
        lv_draw_rect(ctx,&rd,&r);
    }
}

static void tx_color_selector_destroy(void)
{
    tx_color_preview_pending=false;
    tx_color_preview_since_ms=0;
    if (tx_color_table) {
        lv_group_remove_obj(tx_color_table);
        lv_obj_del(tx_color_table);
        tx_color_table=NULL;
    }
}

static void tx_color_selector_build(void)
{
    tx_color_selector_destroy();
    if (tx_status()) lv_obj_add_flag(tx_status(),LV_OBJ_FLAG_HIDDEN);

    tx_instruction_show("Rotate MFK to select the text colors. Press MFK to confirm and start SSTV transmission.");

    /* Phase 5 needs only the three swatches: use a compact 150 px list,
     * centered in the same 300 px left-hand column. */
    ui_list_cfg_t cfg = {
        .width = 150,
        .height = SSTV_TX_LIST_H,
        .row_pad = 4,
        .col_count = 0,
        .row_cb = tx_color_row_cb,
        .activate_cb = tx_color_activate_cb,
        .draw_cb = tx_color_draw_cb,
    };
    tx_color_table=tx_list_create(&cfg,SSTV_TX_LIST_X+75,SSTV_TX_LIST_Y);
    ui_list_set_count(tx_color_table,20);
    lv_obj_add_event_cb(tx_color_table,tx_color_selection_changed_cb,
                        LV_EVENT_VALUE_CHANGED,NULL);
    tx_list_focus(tx_color_table);

    /* Restore the last color-combination row used in this runtime. */
    int initial_row=tx_last_color_selected;
    if (initial_row<0 || initial_row>=20) initial_row=0;
    ui_list_select(tx_color_table,initial_row);
    tx_color_apply(initial_row);
    tx_color_preview_pending=true;
    tx_color_preview_since_ms=lv_tick_get()-300;
}

static void tx_type_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    const char *text="";
    switch (index) {
        case 0: text="CALL CQ"; break;
        case 1: case 2:
            if (tx_target_callsign[0])
                snprintf(row->cell[0],UI_LIST_CELL_LEN,"%s TO %s",
                         index==1 ? "REPLY" : "REPLAY",tx_target_callsign);
            else
                snprintf(row->cell[0],UI_LIST_CELL_LEN,"%s TO...",
                         index==1 ? "REPLY" : "REPLAY");
            row->muted=!tx_target_callsign[0];
            return;
        case 3: text="TX IMAGE ONLY"; break;
        case 4: text="REPEAT LAST TX"; row->muted=!tx_last_valid; break;
        case 5: text="SET TARGET CALLSIGN"; break;
        default: break;
    }
    snprintf(row->cell[0],UI_LIST_CELL_LEN,"%s",text);
}

static void tx_type_update_help(void)
{
    if (!tx_type_table || !tx_type_help_label) return;

    int row=ui_list_get_selected(tx_type_table);

    const char *text="";
    switch (row) {
        case 0:
            text="Choose an image and call CQ using the default text template";
            break;
        case 1:
            text=tx_target_callsign[0]
                ? "Choose an image and a text template to reply to the target callsign"
                : "You must set a target callsign before using this action";
            break;
        case 2:
            text=tx_target_callsign[0]
                ? "Choose a received picture for the miniature, then an image and a text template to reply to the target callsign"
                : "You must set a target callsign before using this action";
            break;
        case 3:
            text="Choose an image and transmit without any added text";
            break;
        case 4:
            text=tx_last_valid
                ? "Repeat the last SSTV transmission using the same image and mode"
                : "Last TX not found";
            break;
        case 5:
            text="The target callsign will be automatically included in your text when replying or replaying";
            break;
        default:
            break;
    }

    lv_label_set_text(tx_type_help_label,text);
}

static void tx_type_selection_changed_cb(lv_event_t *e)
{
    (void)e;
    /* Remember the selected initial TX action for the next TX entry. */
    if (tx_type_table) {
        int row=ui_list_get_selected(tx_type_table);
        if (row>=0 && row<=5) tx_last_type_selected=row;
    }
    tx_type_update_help();
    /* Phase 1 intentionally leaves the received SSTV canvas untouched. */
}

static void tx_type_activate_cb(void *user, int row)
{
    (void)user;
    if (tx_ui_state != SSTV_TX_UI_SETUP ||
        tx_phase != SSTV_TX_PHASE_SELECT_TYPE || !tx_type_table)
        return;

    if (row<0 || row>5) return;

    if (row==0) {
        tx_message_type=SSTV_TX_MESSAGE_CQ;
        tx_type_selector_destroy();
        tx_phase=SSTV_TX_PHASE_SELECT_PICTURE;
        tx_picture_selector_build();
        fprintf(stderr,"SSTV TX UI: CALL CQ -> pictures\n");
        return;
    }

    /* REPLY and REPLAY both require a target callsign. */
    if (row==1 || row==2) {
        if (!tx_target_callsign[0]) return;
        tx_message_type=(row==2) ? SSTV_TX_MESSAGE_REPLAY : SSTV_TX_MESSAGE_REPLY;
        tx_type_selector_destroy();
        if (row==2) {                    /* 1KO125: first the received picture for the miniature */
            tx_phase=SSTV_TX_PHASE_SELECT_RX_PICTURE;
            tx_rxpic_selector_build();
            fprintf(stderr,"SSTV TX UI: REPLAY TO %s -> received pictures\n",tx_target_callsign);
            return;
        }
        tx_phase=SSTV_TX_PHASE_SELECT_PICTURE;
        tx_picture_selector_build();
        fprintf(stderr,"SSTV TX UI: %s TO %s -> pictures\n",
                row==2 ? "REPLAY" : "REPLY",tx_target_callsign);
        return;
    }

    if (row==3) {
        tx_message_type=SSTV_TX_MESSAGE_IMAGE_ONLY;
        tx_type_selector_destroy();
        tx_phase=SSTV_TX_PHASE_SELECT_PICTURE;
        tx_picture_selector_build();
        fprintf(stderr,"SSTV TX UI: TX IMAGE ONLY -> pictures\n");
        return;
    }

    if (row==4) {
        if (!tx_last_valid) return;
        tx_message_type=SSTV_TX_MESSAGE_REPEAT_LAST;
        tx_type_selector_destroy();
        tx_instruction_hide();
        tx_picture_width=tx_last_width;
        tx_picture_height=tx_last_height;
        memcpy(tx_final_rgb,tx_last_rgb,(size_t)tx_last_width*tx_last_height*3);
        tx_picture_render_preview();
        lv_group_add_obj(keyboard_group,sstv_canvas);
        lv_group_focus_obj(sstv_canvas);
        lv_group_set_editing(keyboard_group,true);
        if (tx_status()) lv_obj_clear_flag(tx_status(),LV_OBJ_FLAG_HIDDEN);
        fprintf(stderr,"SSTV TX UI: REPEAT LAST TX -> RF (%s)\n",
                cont_mode_name(tx_last_mode));
        tx_start_final_rf_mode(tx_last_mode);
        return;
    }

    /* SET TARGET CALLSIGN */
    lv_group_remove_obj(tx_type_table);
    tx_target_keyboard_open_pending=true;
    return;
}

static void tx_type_selector_build(void)
{
    tx_type_selector_destroy();
    tx_phase=SSTV_TX_PHASE_SELECT_TYPE;

    if (tx_status()) lv_obj_add_flag(tx_status(),LV_OBJ_FLAG_HIDDEN);

    /* This is now the FIRST TX phase.  Keep the received image visible so
     * the operator can read the remote callsign while choosing the action. */
    tx_restore_rx_canvas();
    tx_instruction_show("Rotate and press MFK to choose SSTV TX action");

    /* Six actions in 6 x 24 px rows. */
    ui_list_cfg_t cfg = {
        .width = SSTV_TX_LIST_W,
        .height = 145,
        .font = &sony_28,
        .row_pad = 2,
        .col_count = 1,
        .cols = { TX_LIST_MENU_COL },
        .row_cb = tx_type_row_cb,
        .activate_cb = tx_type_activate_cb,
    };
    tx_type_table=tx_list_create(&cfg,SSTV_TX_LIST_X,65); // TABELLA TX
    ui_list_set_count(tx_type_table,6);

    /* Context-sensitive help for the currently selected Phase 1 action. */
    tx_type_help_label=lv_label_create(dialog.obj);
    lv_obj_set_pos(tx_type_help_label,SSTV_TX_LIST_X,225);
    lv_obj_set_width(tx_type_help_label,SSTV_TX_LIST_W);
    lv_obj_set_style_text_font(tx_type_help_label,&sony_24,0);
    lv_obj_set_style_text_color(tx_type_help_label,lv_color_white(),0);
    lv_obj_set_style_text_align(tx_type_help_label,LV_TEXT_ALIGN_LEFT,0);
    lv_label_set_long_mode(tx_type_help_label,LV_LABEL_LONG_WRAP);

    lv_obj_add_event_cb(tx_type_table,tx_type_selection_changed_cb,LV_EVENT_VALUE_CHANGED,NULL);

    /* With no target, REPLY/REPLAY remain selectable for navigation but pressing them
     * is deliberately a no-op; the grey text makes the unavailable state clear. */
    tx_list_focus(tx_type_table);

    /* Restore the last action row selected during this runtime.  If REPLY/REPLAY was
     * remembered but there is currently no target callsign, fall back to
     * SET TARGET CALLSIGN because both are disabled in that state. */
    int initial_row=tx_last_type_selected;
    if (initial_row<0 || initial_row>5) initial_row=0;
    if ((initial_row==1 || initial_row==2) && !tx_target_callsign[0])
        initial_row=5;
    if (initial_row==4 && !tx_last_valid)
        initial_row=0;

    ui_list_select(tx_type_table,initial_row);
    tx_last_type_selected=initial_row;
    tx_type_update_help();

    fprintf(stderr,"SSTV TX UI: initial action selector; target=%s\n",
            tx_target_callsign[0] ? tx_target_callsign : "<not set>");
}

static void tx_picture_selector_destroy(void)
{
    if (tx_picture_table) {
        lv_group_remove_obj(tx_picture_table);
        lv_obj_del(tx_picture_table);
        tx_picture_table = NULL;
    }
    tx_picture_free_files();
    tx_picture_selected = -1;
    tx_picture_pending = -1;
    tx_picture_pending_since_ms = 0;
//    tx_picture_width = tx_picture_height = 0;
    tx_picture_name[0] = '\0';
}

/* Picture name without ".png"; the list shortens it with ".." if needed. */
static void tx_picture_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    if (index<0 || (size_t)index>=tx_picture_file_count) return;

    const char *name=tx_picture_files[index];
    size_t len=strlen(name);
    if (len>=4 && strcmp(name+len-4,".png")==0) len-=4;

    snprintf(row->cell[0],UI_LIST_CELL_LEN,"%d",index+1);
    snprintf(row->cell[1],UI_LIST_CELL_LEN,"%.*s",(int)len,name);
}

static void tx_picture_selector_build(void)
{
    tx_picture_selector_destroy();
    tx_picture_selector_refresh();

    if (tx_status())
        lv_obj_add_flag(tx_status(), LV_OBJ_FLAG_HIDDEN);

    tx_instruction_show("Rotate MFK to select a TX image. Press MFK to confirm.");

    ui_list_cfg_t cfg = {
        .width = SSTV_TX_LIST_W,
        .height = SSTV_TX_LIST_H,
        .col_count = 2,
        .cols = { TX_LIST_NUM_COL, TX_LIST_NAME_COL },
        .row_cb = tx_picture_row_cb,
        .activate_cb = tx_picture_activate_cb,
        .empty_text = "No TX pictures found",
    };
    tx_picture_table = tx_list_create(&cfg, SSTV_TX_LIST_X, SSTV_TX_LIST_Y);
    ui_list_set_count(tx_picture_table, (int)tx_picture_file_count);

    if (tx_picture_file_count == 0) {
        buttons_disabled(&btn_cont_mode, false);
        lv_group_add_obj(keyboard_group, sstv_canvas);
        lv_group_focus_obj(sstv_canvas);
        lv_group_set_editing(keyboard_group, true);
        tx_set_status("No TX pictures found");
        if (tx_status())
            lv_obj_clear_flag(tx_status(), LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_event_cb(tx_picture_table, tx_picture_selection_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    tx_list_focus(tx_picture_table);

    int initial_row=0;
    if (tx_last_picture_file[0]) {
        for (size_t i=0;i<tx_picture_file_count;i++) {
            if (strcmp(tx_picture_files[i],tx_last_picture_file)==0) {
                initial_row=(int)i;
                break;
            }
        }
    }

    ui_list_select(tx_picture_table,initial_row);

    tx_picture_pending=initial_row;
    tx_picture_pending_since_ms=tx_now_ms()-SSTV_TX_PICTURE_DEBOUNCE_MS;
    tx_picture_load_pending_preview();

    fprintf(stderr,"SSTV TX UI: %zu TX pictures found in %s; initial=%s\n",
            tx_picture_file_count,SSTV_TX_PICS_DIR,tx_picture_files[initial_row]);
}

static void tx_ui_cb(button_data_t *data)
{
    (void)data;

    if (tx_ui_state == SSTV_TX_UI_RF) {
        tx_abort = true;
        tx_set_status("Stopping SSTV TX...");
        return;
    }

    if (tx_ui_state == SSTV_TX_UI_SETUP) {
        tx_color_selector_destroy();
        tx_target_selector_destroy();
        tx_template_selector_destroy();
        tx_type_selector_destroy();
        tx_picture_selector_destroy();
        tx_rxpic_selector_destroy();   /* 1KO125 */
        tx_instruction_hide();
        tx_phase = SSTV_TX_PHASE_NONE;
        if (tx_status())
            lv_obj_clear_flag(tx_status(), LV_OBJ_FLAG_HIDDEN);
        tx_ui_state = SSTV_TX_UI_RX;

        /* TX selectors own keyboard focus while the TX setup is open.
         * On Cancel TX restore the normal RX focus, otherwise VOL events
         * no longer reach key_cb() even though the RX screen is visible. */
        lv_group_add_obj(keyboard_group, sstv_canvas);
        lv_group_focus_obj(sstv_canvas);
        lv_group_set_editing(keyboard_group, true);

        btn_tx.label = "TX";
        snprintf(tx_mode_label, sizeof(tx_mode_label), "RX Mode\n%s", cont_rx_mode_name());
        btn_cont_mode.label = tx_mode_label;

        /* 1KO125: the receivers were never stopped: back to the RX panel */
        tx_panel_show(false);
        rx_ui_resync();
        if (btn_tx.disp_btn) buttons_refresh(&btn_tx);
        if (btn_cont_mode.disp_btn) buttons_refresh(&btn_cont_mode);
        update_rx_controls();
        fprintf(stderr, "SSTV TX UI: setup cancelled; RX panel shown again\n");
        return;
    }

    /* RX pixels for the REPLAY miniature and for the first TX phase. */
    tx_save_rx_canvas();

    /* 1KO125: the receivers keep going under the TX panel; they stop only
     * when RF starts (tx_stop_receivers_for_rf) */
    tx_restore_auto_rx = false;

    tx_ui_state = SSTV_TX_UI_SETUP;
    tx_restore_rx_canvas();
    tx_panel_show(true);
    tx_phase = SSTV_TX_PHASE_SELECT_TYPE;
    btn_tx.label = "Cancel TX";
    snprintf(tx_mode_label, sizeof(tx_mode_label), "TX Mode\n%s", cont_mode_name(cont_mode));
    btn_cont_mode.label = tx_mode_label;
    tx_set_status("TX setup\n\nChoose TX action");
    if (tx_bar()) {
        lv_bar_set_value(tx_bar(), 0, LV_ANIM_OFF);
        lv_obj_add_flag(tx_bar(), LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_tx.disp_btn) buttons_refresh(&btn_tx);
    if (btn_cont_mode.disp_btn) buttons_refresh(&btn_cont_mode);
    update_rx_controls();
    tx_type_selector_build();
    fprintf(stderr, "SSTV TX UI: entered setup at action selector; receivers keep running\n");
}
