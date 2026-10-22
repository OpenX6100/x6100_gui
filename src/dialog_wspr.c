/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSPR window
 *
 *  Display layer only. Everything below it - the receiver, the decoder,
 *  the transmit scheduler and the power handling - lives in wspr_app.c
 *  and the modules under it. This file draws what they produce and gives
 *  the operator the controls.
 *
 *  Modelled on dialog_ft8.c: a full screen dialog owning a waterfall
 *  with the table laid over its lower part, so that turning the encoder
 *  fades the table away and reveals the spectrum underneath.
 */

#include "dialog_wspr.h"

#include "lvgl/lvgl.h"

#include "dialog.h"
#include "styles.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "audio.h"
#include "dsp.h"
#include "waterfall.h"
#include "spectrum.h"
#include "lock_manager.h"
#include "events.h"
#include "buttons.h"
#include "main_screen.h"
#include "keyboard.h"
#include "qth/qth.h"
#include "msg.h"
#include "util.h"
#include "scheduler.h"

#include "ft8_ui/table_view.h"
#include "widgets/lv_waterfall.h"
#include "widgets/lv_finder.h"

#include "wspr_app.h"
#include "wspr_demod.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIDTH           771

/* The table sits over the waterfall, leaving a strip of it visible. */
#define WF_HEIGHT       325
#define TABLE_TOP       55

/* Encoder step for our transmit frequency, and the width of its marker.
 * Moving it never touches the receiver, which always decodes the whole
 * 1400-1600 Hz window. */
#define TX_STEP_HZ      5
#define TX_MARK_HZ      6               /* four tones, 1.46 Hz apart */

/* ------------------------------------------------------------------ */
/* Standard WSPR dial frequencies                                      */

/*
 * Source: WSPRnet. These are USB dial frequencies; the 200 Hz WSPR
 * window sits 1400 to 1600 Hz above each one. 80 m is the Region 1
 * value, which replaced the older 3592.6 kHz.
 *
 * 2 m is omitted deliberately: the radio does not reach it, and an
 * entry that cannot be tuned would only be a trap in the band list.
 */
typedef struct {
    const char *label;
    uint32_t    dial_hz;
} wspr_band_t;

static const wspr_band_t wspr_bands[] = {
    {"160m",   1836600},
    {"80m",    3568600},
    {"60m",    5287200},
    {"40m",    7038600},
    {"30m",   10138700},
    {"20m",   14095600},
    {"17m",   18104600},
    {"15m",   21094600},
    {"12m",   24924600},
    {"10m",   28124600},
    {"6m",    50293000},
};

#define WSPR_BAND_CNT ((int)(sizeof(wspr_bands) / sizeof(wspr_bands[0])))

static int cur_band = -1;

/* ------------------------------------------------------------------ */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);

static void band_cb(lv_event_t *e);

static const char *tx_label_getter(void);
static const char *pwr_label_getter(void);
static const char *txf_label_getter(void);
static const char *status_label_getter(void);

static void tx_press_cb(button_data_t *b);
static void tx_hold_cb(button_data_t *b);
static void pwr_press_cb(button_data_t *b);
static void pwr_hold_cb(button_data_t *b);
static void txf_press_cb(button_data_t *b);
static void txf_hold_cb(button_data_t *b);
static void clear_press_cb(button_data_t *b);
static void band_up_cb(button_data_t *b);
static void band_down_cb(button_data_t *b);

#define table (table_view_obj())

static lv_obj_t   *waterfall;
static lv_obj_t   *finder;            /* our transmit frequency */
static lv_obj_t   *window_box;        /* 1400-1600 Hz, decoded */

static lv_timer_t *status_timer = NULL;
static lv_timer_t *fade_timer = NULL;

static lv_anim_t   fade;
static bool        fade_run = false;

static uint16_t    tx_hz = 0;
static double      cur_lat, cur_lon;

/* ---- Buttons ---------------------------------------------------------- */

static buttons_page_t btn_page_1;
static buttons_page_t btn_page_2;

static button_data_t button_page_1 = {
    .type = BTN_TEXT, .label = "(WSPR 1:2)",
    .press = button_next_page_cb, .next = &btn_page_2,
};
static button_data_t button_tx = {
    .type = BTN_TEXT_FN, .label_fn = tx_label_getter,
    .press = tx_press_cb, .hold = tx_hold_cb,
};
static button_data_t button_pwr = {
    .type = BTN_TEXT_FN, .label_fn = pwr_label_getter,
    .press = pwr_press_cb, .hold = pwr_hold_cb,
};
static button_data_t button_status = {
    .type = BTN_TEXT_FN, .label_fn = status_label_getter,
};
static button_data_t button_clear = {
    .type = BTN_TEXT, .label = "Clear", .press = clear_press_cb,
};

static button_data_t button_page_2 = {
    .type = BTN_TEXT, .label = "(WSPR 2:2)",
    .press = button_next_page_cb, .next = &btn_page_1,
};
static button_data_t button_band_down = {
    .type = BTN_TEXT, .label = "Band\ndown", .press = band_down_cb,
};
static button_data_t button_band_up = {
    .type = BTN_TEXT, .label = "Band\nup", .press = band_up_cb,
};
static button_data_t button_center = {
    .type = BTN_TEXT_FN, .label_fn = txf_label_getter,
    .press = txf_press_cb, .hold = txf_hold_cb,
};

static buttons_page_t btn_page_1 = {
    {&button_page_1, &button_tx, &button_pwr, &button_status, &button_clear}
};

static buttons_page_t btn_page_2 = {
    {&button_page_2, &button_band_down, &button_band_up, &button_center}
};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb,
};

dialog_t *dialog_wspr = &dialog;

/* ---- Band selection --------------------------------------------------- */

static int band_nearest(uint32_t freq)
{
    int best = 0;
    int64_t best_d = -1;
    int i;

    for (i = 0; i < WSPR_BAND_CNT; i++) {
        int64_t d = (int64_t)freq - (int64_t)wspr_bands[i].dial_hz;

        if (d < 0)
            d = -d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/*
 * dir 0 picks the band closest to where the radio already is, which is
 * what opening the window does; +-1 steps through the list.
 */
static void load_band(int8_t dir)
{
    int idx;

    if (dir == 0 || cur_band < 0) {
        idx = band_nearest((uint32_t)cparam_i_get(cfg.cur.fg_freq()));
    } else {
        idx = cur_band + dir;
        if (idx < 0)
            idx = WSPR_BAND_CNT - 1;
        if (idx >= WSPR_BAND_CNT)
            idx = 0;
    }

    cur_band = idx;

    /* Frequency first, then mode - the order cfg_digital_load() uses. */
    cparam_i_set(cfg.cur.fg_freq(), (int32_t)wspr_bands[idx].dial_hz);
    cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);

    msg_update_text_fmt("WSPR %s  %u.%03u MHz", wspr_bands[idx].label,
                        (unsigned)(wspr_bands[idx].dial_hz / 1000000u),
                        (unsigned)((wspr_bands[idx].dial_hz / 1000u) % 1000u));
}

static void restart_rx(void)
{
    /* A band change rebuilds the receive chain: the collected baseband
     * no longer belongs to the frequency being displayed. */
    wspr_app_set_active(false);
    wspr_app_set_active(true);
}

static void clean_screen(void)
{
    table_view_reset();
    lv_waterfall_clear_data(waterfall);
}

static void change_band(int8_t dir)
{
    load_band(dir);
    restart_rx();
    clean_screen();
    buttons_refresh(&button_status);
}

static void band_cb(lv_event_t *e)
{
    change_band(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1);
}

static void band_up_cb(button_data_t *b)
{
    (void)b;
    change_band(1);
}

static void band_down_cb(button_data_t *b)
{
    (void)b;
    change_band(-1);
}

/* ---- Waterfall -------------------------------------------------------- */

struct waterfall_data {
    float *psd;
    size_t size;
};

static void waterfall_add_data(void *data)
{
    struct waterfall_data *wf = (struct waterfall_data *)data;

    lv_waterfall_add_data(waterfall, wf->psd, wf->size);
    free(wf->psd);
}

static void on_psd(const float *mag_db, int nbins, void *ctx)
{
    struct waterfall_data wf;

    (void)ctx;

    if (!mag_db || nbins <= 0)
        return;

    wf.size = (size_t)nbins;
    wf.psd = (float *)malloc(sizeof(float) * wf.size);
    if (!wf.psd)
        return;

    memcpy(wf.psd, mag_db, wf.size * sizeof(float));
    scheduler_put(waterfall_add_data, &wf, sizeof(wf));
}

/* ---- Spots ------------------------------------------------------------ */

/*
 * The timestamp a spot carries is the start of the slot it was heard
 * in, not the moment decoding finished. Decoding runs after the window
 * closes, roughly 116 s in, so rounding the current time down to the
 * previous even minute gives the right label.
 */
static void slot_time_str(char *out, size_t out_sz)
{
    time_t     t = time(NULL) - 60;
    struct tm  tm_utc;

    t -= t % 120;
    gmtime_r(&t, &tm_utc);
    snprintf(out, out_sz, "%02d%02d", tm_utc.tm_hour, tm_utc.tm_min);
}

/* Settings > Interface > Decoder text, read when the window opens. */
static bool compact;

static void on_spot(const wspr_spot_t *spot, void *ctx)
{
    cell_data_t cd;
    char        when[8];

    (void)ctx;

    memset(&cd, 0, sizeof(cd));
    cd.cell_type = CELL_RX_MSG;

    slot_time_str(when, sizeof(when));

    /*
     * SNR and distance are drawn separately by table_view, right
     * aligned, so they must not be repeated in the text.
     */
    if (compact) {
        snprintf(cd.text, sizeof(cd.text), "%s  %-6s %-4s %2d dBm  %.1f Hz",
                 when, spot->callsign, spot->locator, spot->power_dbm,
                 (double)spot->freq_hz);
    } else {
        /* Large: the audio offset does not fit before the distance. */
        snprintf(cd.text, sizeof(cd.text), "%s %-6s %-4s %2d dBm",
                 when, spot->callsign, spot->locator, spot->power_dbm);
    }

    cd.meta.local_snr = (int16_t)lroundf(spot->snr_db);

    if (strlen(spot->locator) >= 4) {
        double lat, lon;

        qth_str_to_pos(spot->locator, &lat, &lon);
        cd.dist = (int16_t)qth_pos_dist(lat, lon, cur_lat, cur_lon);
    }

    scheduler_put(table_view_add_msg_cb, &cd, sizeof(cd));
}

static void on_slot_end(void *ctx)
{
    (void)ctx;
}

static void on_note(const char *text, void *ctx)
{
    (void)ctx;
    msg_schedule_text_fmt("%s", text);
}

static wspr_app_ui_cb_t ui_cb = {
    .on_spot     = on_spot,
    .on_psd      = on_psd,
    .on_slot_end = on_slot_end,
    .on_note     = on_note,
    .ctx         = NULL,
};

/* ---- Button labels ---------------------------------------------------- */

static const char *tx_label_getter(void)
{
    static char buf[32];
    uint8_t n = wspr_app_get_tx_period();

    if (n == 0) {
        snprintf(buf, sizeof(buf), "TX:\noff");
    } else if (n == 1) {
        snprintf(buf, sizeof(buf), "TX:\nevery");
    } else {
        snprintf(buf, sizeof(buf), "TX:\n1 in %u", (unsigned)n);
    }
    return buf;
}

static const char *pwr_label_getter(void)
{
    static char buf[32];
    uint16_t mw = wspr_app_get_tx_pwr_mw();

    if (mw < 1000) {
        snprintf(buf, sizeof(buf), "TX pwr:\n%u mW", (unsigned)mw);
    } else {
        snprintf(buf, sizeof(buf), "TX pwr:\n%u.%u W",
                 (unsigned)(mw / 1000), (unsigned)((mw % 1000) / 100));
    }
    return buf;
}

static const char *txf_label_getter(void)
{
    static char buf[32];

    snprintf(buf, sizeof(buf), "TX freq:\n%u Hz", (unsigned)tx_hz);
    return buf;
}

static const char *status_label_getter(void)
{
    static char buf[40];
    const char *blocked;

    if (wspr_app_is_transmitting()) {
        snprintf(buf, sizeof(buf), "TX\nsending");
        return buf;
    }

    blocked = wspr_app_tx_blocked_reason();
    if (blocked && wspr_app_get_tx_period() > 0) {
        /* Say why we will not key up, rather than silently not doing
         * it, which looks exactly like a broken transmitter. */
        snprintf(buf, sizeof(buf), "%s", blocked);
        return buf;
    }

    snprintf(buf, sizeof(buf), "%03d/120 s\n%u spots",
             (int)wspr_app_slot_progress(),
             (unsigned)wspr_app_spot_count());
    return buf;
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* BTN_TEXT_FN without a subject never repaints by itself. */
    if (button_status.disp_btn)
        buttons_refresh(&button_status);
}

/* ---- Button actions --------------------------------------------------- */

static const uint8_t tx_periods[] = {0, 10, 5, 3, 2, 1};

static void tx_press_cb(button_data_t *b)
{
    uint8_t cur = wspr_app_get_tx_period();
    uint8_t next;
    size_t  i;

    for (i = 0; i < sizeof(tx_periods) / sizeof(tx_periods[0]); i++) {
        if (tx_periods[i] == cur)
            break;
    }
    i = (i + 1) % (sizeof(tx_periods) / sizeof(tx_periods[0]));
    next = tx_periods[i];

    wspr_app_set_tx_period(next);

    if (next == 0) {
        msg_update_text_fmt("WSPR transmit disabled");
    } else if (next == 1) {
        msg_update_text_fmt("WSPR TX every slot - test only");
    } else {
        msg_update_text_fmt("WSPR TX one slot in %u", (unsigned)next);
    }
    buttons_refresh(b);
}

static void tx_hold_cb(button_data_t *b)
{
    wspr_app_set_tx_period(0);
    msg_update_text_fmt("WSPR transmit disabled");
    buttons_refresh(b);
}

static void pwr_press_cb(button_data_t *b)
{
    uint16_t mw = wspr_app_get_tx_pwr_mw() + WSPR_PWR_STEP_MW;

    if (mw > WSPR_PWR_MAX_MW)
        mw = WSPR_PWR_MIN_MW;

    wspr_app_set_tx_pwr_mw(mw);
    msg_update_text_fmt("WSPR TX %u mW, reported %d dBm",
                        (unsigned)wspr_app_get_tx_pwr_mw(),
                        wspr_app_tx_dbm());
    buttons_refresh(b);
}

static void pwr_hold_cb(button_data_t *b)
{
    wspr_app_set_tx_pwr_mw(WSPR_PWR_MIN_MW);
    msg_update_text_fmt("WSPR TX %u mW, reported %d dBm",
                        (unsigned)wspr_app_get_tx_pwr_mw(),
                        wspr_app_tx_dbm());
    buttons_refresh(b);
}

static void apply_tx_freq(void);

static void txf_press_cb(button_data_t *b)
{
    uint16_t f = tx_hz + 10;

    if (f > 1600)
        f = 1400;
    tx_hz = f;
    buttons_refresh(b);
    apply_tx_freq();
}

static void txf_hold_cb(button_data_t *b)
{
    tx_hz = 1500;
    buttons_refresh(b);
    apply_tx_freq();
}

static void clear_press_cb(button_data_t *b)
{
    (void)b;
    clean_screen();
}

/* ---- Transmit frequency ---------------------------------------------- */

#define WF_LOW_HZ   (WSPR_APP_RX_CENTER - WSPR_WORK_RATE / 2)
#define WF_HIGH_HZ  (WSPR_APP_RX_CENTER + WSPR_WORK_RATE / 2)

/*
 * The waterfall spans one working rate, 375 Hz, around the WSPR window.
 * The whole window, 1400 to 1600 Hz, is always decoded and outlined; the
 * red marker is our own transmission, about 6 Hz wide, as in WSJT-X.
 */
static void update_finder(void)
{
    lv_finder_set_range(finder, (int16_t)WF_LOW_HZ, (int16_t)WF_HIGH_HZ);
    lv_finder_set_width(finder, TX_MARK_HZ);
    lv_finder_set_value(finder, (int16_t)(tx_hz - TX_MARK_HZ / 2));   /* left edge */
    lv_obj_invalidate(finder);
}

static void apply_tx_freq(void)
{
    wspr_app_set_tx_freq(tx_hz);
    update_finder();
}

/* ---- Fade ------------------------------------------------------------- */

static void fade_anim(void *obj, int32_t v)
{
    lv_obj_set_style_opa_layered((lv_obj_t *)obj, v, 0);
}

static void fade_ready(lv_anim_t *a)
{
    (void)a;
    fade_run = false;
}

static void fade_back_cb(lv_timer_t *t)
{
    (void)t;
    fade_timer = NULL;
    lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0),
                       LV_OPA_COVER);
    lv_anim_start(&fade);
}

/* ---- Dialog callbacks ------------------------------------------------- */

static void rotary_cb(int32_t diff)
{
    int32_t f = (int32_t)tx_hz + diff * TX_STEP_HZ;

    if (f < 1400)
        f = 1400;
    if (f > 1600)
        f = 1600;
    tx_hz = (uint16_t)f;

    buttons_refresh(&button_center);
    apply_tx_freq();

    /* Reveal the waterfall while tuning, exactly as the FT8 window
     * does, and bring the table back a second after the last click. */
    if (!fade_run) {
        fade_run = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0),
                           LV_OPA_TRANSP);
        lv_anim_start(&fade);
    }

    if (fade_timer) {
        lv_timer_reset(fade_timer);
    } else {
        fade_timer = lv_timer_create(fade_back_cb, 1000, NULL);
        lv_timer_set_repeat_count(fade_timer, 1);
    }
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
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

static void on_table_close(void)
{
    dialog_destruct();
}

static void on_table_vol_change(int32_t delta)
{
    radio_change_vol(delta > 0 ? 1 : -1);
}

static void construct_cb(lv_obj_t *parent)
{
    dialog.obj = dialog_init(parent);

    /* The window covers the main screen entirely, so the spectrum and
     * waterfall behind it would be computed for nothing. */
    waterfall_set_enabled(false);
    spectrum_set_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    buttons_load_page(&btn_page_1);

    /* Waterfall */

    waterfall = lv_waterfall_create(dialog.obj);

    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);

    lv_waterfall_set_palette(waterfall, (lv_color_t *)style.wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, WF_HEIGHT);

    /*
     * The spectrum module hands over dB above the noise floor of each
     * line, so these limits are absolute and never need adjusting when
     * the operator changes AF or RF gain.
     */
    lv_waterfall_set_min(waterfall, -3);
    lv_waterfall_set_max(waterfall, 25);

    lv_obj_set_pos(waterfall, 13, 13);

    /* The decoded window, 1400-1600 Hz: its edges */

    window_box = lv_obj_create(waterfall);
    lv_obj_remove_style_all(window_box);
    lv_obj_set_size(window_box, WIDTH * 200 / (WF_HIGH_HZ - WF_LOW_HZ), WF_HEIGHT);
    lv_obj_set_pos(window_box, WIDTH * (1400 - WF_LOW_HZ) / (WF_HIGH_HZ - WF_LOW_HZ), 0);
    lv_obj_set_style_border_side(window_box, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(window_box, 1, 0);
    lv_obj_set_style_border_color(window_box, lv_color_white(), 0);
    lv_obj_set_style_border_opa(window_box, LV_OPA_40, 0);
    lv_obj_clear_flag(window_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Transmit frequency marker */

    finder = lv_finder_create(waterfall);

    lv_obj_set_size(finder, WIDTH, WF_HEIGHT);
    lv_obj_set_pos(finder, 0, 0);

    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(finder, style.colors.mark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    /* Table of spots */

    table_view_build(dialog.obj, 13, 13 + TABLE_TOP, WIDTH,
                     WF_HEIGHT - TABLE_TOP);

    /*
     * Settings > Interface > Decoder text, as in FT8.
     * Large: the dialog font (sony_36, 34 px rows), 8 rows in the 270 px
     * table; the row leaves out the audio offset, so a normal line
     * (about 455 px) ends before even a 5-digit distance (468 px).
     * Compact: sony_28 with 1 px cell padding, 22 px rows, 12 rows, the
     * full line. table_view leaves the distance out of a row whose text
     * would run into it.
     */
    compact = param_i_get(cfg.ui.digi_compact()) != 0;
    if (compact) {
        lv_obj_set_style_text_font(table, &sony_28, LV_PART_ITEMS);
        lv_obj_set_style_pad_top(table, 1, LV_PART_ITEMS);
        lv_obj_set_style_pad_bottom(table, 1, LV_PART_ITEMS);
    }
    table_view_set_press_cb(NULL);
    {
        table_view_actions_t tv_actions = {
            .on_close      = on_table_close,
            .on_vol_change = on_table_vol_change,
        };
        table_view_set_actions(&tv_actions);
    }

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, table);
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, fade_anim);
    lv_anim_set_ready_cb(&fade, fade_ready);

    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    /* Frequency: remember where the operator was, then move to the
     * standard dial for whichever band is nearest. mem_load() in the
     * destructor puts it all back on the way out. */
    mem_save(MEM_BACKUP_ID);
    load_band(0);

    qth_str_to_pos(PARAM_T_GET(cfg.qth()), &cur_lat, &cur_lon);

    lm_set_ab(true);
    lm_set_mode(true);
    lm_set_freq(true);
    lm_set_band(true);

    /* Receiver */

    wspr_app_init();
    wspr_app_set_ui_cb(&ui_cb);
    wspr_app_set_active(true);

    tx_hz = wspr_app_get_tx_freq();
    update_finder();

    status_timer = lv_timer_create(status_timer_cb, 1000, NULL);
}

static void destruct_cb(void)
{
    if (status_timer) {
        lv_timer_del(status_timer);
        status_timer = NULL;
    }
    if (fade_timer) {
        lv_timer_del(fade_timer);
        fade_timer = NULL;
    }

    /*
     * The beacon is switched off with the window: opening WSPR again
     * must never start transmitting until the operator asks for it.
     * Not in wspr_app_set_active(false), which a band change also
     * calls.
     */
    wspr_app_set_tx_period(0);

    /*
     * Order matters: stop the application first. It aborts any
     * transmission in progress and joins the worker thread, so no
     * callback can arrive after the table has been destroyed.
     */
    wspr_app_set_active(false);
    wspr_app_set_ui_cb(NULL);

    table_view_destroy();

    waterfall_set_enabled(true);
    spectrum_set_enabled(true);

    mem_load(MEM_BACKUP_ID);

    lm_set_mode(false);
    lm_set_ab(false);
    lm_set_freq(false);
    lm_set_band(false);
}
