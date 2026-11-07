/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 Sitor-B/NavTex viewer
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
*/

#include "dialog_navtex.h"
#include "navtex/decoder.h"
#include "dialog.h"
#include "styles.h"
#include "buttons.h"
#include "keyboard.h"
#include "radio.h"
#include "mfk.h"
#include "events.h"
#include "dsp.h"
#include "msg.h"
#include "ui_list.h"
#include "app_info.h"
#include "app_radio.h"  /* 1KO125: radio set up while open */
#include "spectrum.h"
#include "cfg/cfg_api.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NAVTEX_SAMPLE_RATE 12000

/* Live text history (what Save writes).  The panel shows only its newest
 * part that fits in NAVTEX_LABEL_MAX_H, see fit_tail(). */
#define NAVTEX_TEXT_MAX 16384
#define NAVTEX_PENDING_MAX 512
#define NAVTEX_UI_TIMER_MS 50

#define NAVTEX_SCROLL_STEP 36

/* 1KO125: LVGL 8.3 (LV_USE_LARGE_COORD 0) handles objects up to 8191 px
 * (LV_COORD_MAX): a taller label cannot be scrolled to its end, and new
 * text looked frozen until Clear.  The labels never show more than this
 * height of text, with room for the lines of the next updates. */
#define NAVTEX_LABEL_MAX_H  7000
#define NAVTEX_MEASURE_MAX  1024    /* text measured at a time, see text_px() */

/* Saved messages: NAVTEX_SAVE_DIR/sitorb-YYYYMMDD-HHMMSS.txt */
#define NAVTEX_SAVE_DIR     "/mnt/navtex"
#define NAVTEX_SAVE_PREFIX  "sitorb-"
#define NAVTEX_SAVE_EXT     ".txt"
#define NAVTEX_VIEW_MAX     (64 * 1024)     /* largest file shown in the view */
#define NAVTEX_SUMMARY_LEN  48

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void audio_cb(size_t n, float *samples);
static void close_cb(button_data_t *data);
static void clear_cb(button_data_t *data);
static bool reset_decoder(void);
static void ui_timer_cb(lv_timer_t *timer);
static void char_cb(int c, void *userdata);
static void key_cb(lv_event_t *e);
static void save_clear_cb(button_data_t *data);
static void browser_cb(button_data_t *data);
static void browser_build(void);
static void browser_close(bool restore_focus);
static void browser_buttons(void);
static void mode_cb(button_data_t *data);
static void sitor_page2_cb(button_data_t *data);
static void sitor_page1_cb(button_data_t *data);

/* NAVTEX bulletins, see dialog_navtex_bul.inc */
static void nav_char_q_cb(int c, int quality, void *user);
static void nav_construct(lv_obj_t *parent);
static void nav_destruct(void);
static void nav_poll(void);
static void nav_flush(void);
static bool nav_key(uint32_t key);
static lv_obj_t *nav_body_label;     /* text of the open bulletin */

/* frequency list, see dialog_navtex_freq.inc */
static button_data_t btn_freq;
static lv_obj_t *nf_frame;
static void nf_poll(void);
static void nf_update_label(void);
static void nf_destroy(void);

/* scan of the list, see dialog_navtex_scan.inc */
static button_data_t btn_scan, btn_scan_wait;
static void scan_poll(void);
static void scan_stop(const char *why);
static bool scan_running(void);
static void wait_release(void);
static void scan_audio(const float *samples, size_t n);
static void scan_q_cb(int c, int quality, void *user);
static void scan_wait_load(void);
static bool scan_resume;

static navtex_decoder_t *decoder = NULL;
static lv_obj_t *scroll = NULL;
static lv_obj_t *label = NULL;
static lv_timer_t *ui_timer = NULL;
static lv_obj_t *parent_obj = NULL;

/* Saved-messages browser.  It never touches the decoder, the pending FIFO or
 * text_buf, so live decoding continues while the browser or a saved message
 * is shown (the live panel is only hidden). */
typedef struct {
    char *name;
    char summary[NAVTEX_SUMMARY_LEN];   /* first line of the message */
} navtex_file_t;

static lv_obj_t *browser_panel = NULL;  /* same panel as the live text */
static lv_obj_t *browser_list = NULL;
static lv_obj_t *view_scroll = NULL;    /* text of the selected file */
static lv_obj_t *view_label = NULL;
/* 1KO125: the view shows a window of the file that fits in
 * NAVTEX_LABEL_MAX_H, moved along by view_key_cb() at its ends */
static char *view_text = NULL;      /* the whole file, freed with the label */
static size_t view_len, view_start, view_end;
static int view_index = -1;          /* the file open, in browser_files */
static char view_cut = '\0';        /* the character at view_end */
static void view_set_window(size_t start);
static navtex_file_t *browser_files = NULL;
static size_t browser_file_count = 0;
static size_t browser_file_capacity = 0;
static bool browser_open = false;
static uint32_t dsp_audio_sub_id = AUDIO_SUB_INVALID;

/* char_cb() is called from the decoder/audio side while ui_timer_cb() runs
 * in the LVGL thread.  Keep that boundary explicit with a small FIFO. */
static pthread_mutex_t text_mutex = PTHREAD_MUTEX_INITIALIZER;
static char pending[NAVTEX_PENDING_MAX];
static size_t pending_head = 0;
static size_t pending_tail = 0;
static size_t pending_count = 0;
static unsigned long pending_overflow = 0;

/* Protect decoder lifetime as well as navtex_decoder_process().  reset_decoder()
 * can otherwise destroy the decoder while the DSP callback is using it. */
static pthread_mutex_t decoder_mutex = PTHREAD_MUTEX_INITIALIZER;

static char text_buf[NAVTEX_TEXT_MAX];
static size_t text_len = 0;
static bool follow_tail = true;

static button_data_t btn_page = {
    .type = BTN_TEXT,
    .label = "(SITORB 1:2)",
    .press = sitor_page2_cb
};

static button_data_t btn_page2 = {
    .type = BTN_TEXT,
    .label = "(SITORB 2:2)",
    .press = sitor_page1_cb
};

static button_data_t btn_mode = {
    .type = BTN_TEXT,
    .label = "Mode:\nSITORB",
    .press = mode_cb
};

static button_data_t btn_close = {
    .type = BTN_TEXT,
    .label = "Close\nNavtex",
    .press = close_cb
};

static button_data_t btn_clear = {
    .type = BTN_TEXT,
    .label = "Clear",
    .press = clear_cb
};

static button_data_t btn_save_clear = {
    .type = BTN_TEXT,
    .label = "Save and\nClear",
    .press = save_clear_cb
};

static button_data_t btn_browser = {
    .type = BTN_TEXT,
    .label = "Sitor-B\nBrowser",
    .press = browser_cb
};

static buttons_page_t btn_page_1 = {
    .items = { &btn_page, &btn_mode, &btn_clear, &btn_scan, &btn_freq }
};

/* 1KO125: Info window (both modes, F4 of page 2), see app_info.c */
static void info_cb(button_data_t *data);
static button_data_t btn_info = { .type = BTN_TEXT, .label = "Info", .press = info_cb };

static buttons_page_t btn_page_2 = {
    .items = { &btn_page2, &btn_browser, &btn_save_clear, &btn_info, &btn_close }
};

/* Browser mode: its own buttons only, until Close Browser */
static void br_open_cb(button_data_t *data);
static void br_delete_cb(button_data_t *data);
static void bv_back_cb(button_data_t *data);
static void bv_newer_cb(button_data_t *data);
static void bv_older_cb(button_data_t *data);

static button_data_t btn_br_title = { .type = BTN_TEXT, .label = "(Browser)", .press = NULL };
static button_data_t btn_br_close = { .type = BTN_TEXT, .label = "Close\nBrowser", .press = browser_cb };
static button_data_t btn_br_open  = { .type = BTN_TEXT, .label = "Open", .press = br_open_cb };
static button_data_t btn_br_del   = { .type = BTN_TEXT, .label = "Delete", .press = br_delete_cb };
static button_data_t btn_bv_title = { .type = BTN_TEXT, .label = "(Message)", .press = NULL };
static button_data_t btn_bv_back  = { .type = BTN_TEXT, .label = "Back", .press = bv_back_cb };
static button_data_t btn_bv_newer = { .type = BTN_TEXT, .label = "Newer", .press = bv_newer_cb };
static button_data_t btn_bv_older = { .type = BTN_TEXT, .label = "Older", .press = bv_older_cb };

static buttons_page_t btn_page_browser = {
    .items = { &btn_br_title, &btn_br_close, &btn_br_open, &btn_br_del, NULL }
};
static buttons_page_t btn_page_view = {
    .items = { &btn_bv_title, &btn_bv_back, &btn_bv_newer, &btn_bv_older, &btn_br_del }
};

static void sitor_page2_cb(button_data_t *data) { (void)data; buttons_load_page(&btn_page_2); }
static void sitor_page1_cb(button_data_t *data) { (void)data; buttons_load_page(&btn_page_1); }

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .key_cb = key_cb,
    .btn_page = &btn_page_1
};

dialog_t *dialog_navtex = &dialog;

static void info_cb(button_data_t *data) { (void)data; app_info_open(dialog.obj, APP_INFO_NAVTEX); }

static const lv_font_t *navtex_font(void);

/* Height in pixels of text[0..n) (no newline in it) shown with font in a
 * label width px wide: at least one line. */
static int32_t text_px(const char *text, size_t n, const lv_font_t *font, lv_coord_t width)
{
    char tmp[NAVTEX_MEASURE_MAX + 1];
    int32_t lh = lv_font_get_line_height(font);
    int32_t h = 0;

    do {
        size_t k = n > NAVTEX_MEASURE_MAX ? NAVTEX_MEASURE_MAX : n;
        lv_point_t size;

        memcpy(tmp, text, k);
        tmp[k] = '\0';
        lv_txt_get_size(&size, tmp, font, 0, 0, width, LV_TEXT_FLAG_NONE);
        h += size.y < lh ? lh : size.y;
        text += k;
        n -= k;
    } while (n > 0);
    return h;
}

/* The start of the newest lines of text[0..len) that fit in limit px.  A
 * single line taller than that is shown from the part of it that fits. */
static size_t fit_tail(const char *text, size_t len, const lv_font_t *font, lv_coord_t width,
                       int32_t limit, int32_t *height)
{
    size_t end = len;
    int32_t h = 0;

    while (end > 0) {
        size_t e = end;
        if (text[e - 1] == '\n')
            e--;
        size_t st = e;
        while (st > 0 && text[st - 1] != '\n')
            st--;
        int32_t px = text_px(text + st, e - st, font, width);
        if (h + px > limit) {
            if (h == 0 && px > 0)       /* one very long line */
                end = e - (size_t)((int64_t)(e - st) * limit / px);
            break;
        }
        h += px;
        end = st;
    }
    if (height)
        *height = h;
    return end;
}

/* The end (after a newline) of the lines of text[start..len) that fit in
 * limit px; at least part of the first one. */
static size_t fit_head(const char *text, size_t start, size_t len, const lv_font_t *font,
                       lv_coord_t width, int32_t limit, int32_t *height)
{
    size_t st = start;
    int32_t h = 0;

    while (st < len) {
        const char *nl = memchr(text + st, '\n', len - st);
        size_t e = nl ? (size_t)(nl - text) : len;
        size_t next = nl ? e + 1 : len;
        int32_t px = text_px(text + st, e - st, font, width);
        if (h + px > limit) {
            if (h == 0 && px > 0)       /* one very long line */
                st += (size_t)((int64_t)(e - st) * limit / px);
            break;
        }
        h += px;
        st = next;
    }
    if (height)
        *height = h;
    return st;
}

/* Text width of a label filling obj (its content width, after a layout) */
static lv_coord_t label_width(lv_obj_t *obj)
{
    lv_coord_t w = lv_obj_get_content_width(obj);
    if (w <= 0) {
        lv_obj_update_layout(obj);
        w = lv_obj_get_content_width(obj);
    }
    return w > 0 ? w : 1;
}

/* The live label: the newest part of the history that fits */
static void live_label_update(void)
{
    if (!label || !scroll)
        return;
    size_t from = fit_tail(text_buf, text_len, navtex_font(), label_width(scroll),
                           NAVTEX_LABEL_MAX_H, NULL);
    lv_label_set_text_static(label, text_buf + from);
}

static size_t drop_oldest_complete_line(void)
{
    if (text_len == 0)
        return 0;

    char *newline = memchr(text_buf, '\n', text_len);
    if (!newline)
        return 0;

    size_t drop = (size_t)(newline - text_buf) + 1;
    memmove(text_buf, text_buf + drop, text_len - drop);
    text_len -= drop;
    text_buf[text_len] = '\0';
    return drop;
}

static void append_text(const char *src, size_t n)
{
    if (!src || n == 0)
        return;

    /*
     * History is bounded by BYTES, not by line count.  When more room is
     * needed, discard complete oldest NAVTEX lines until the incoming text
     * fits.  This keeps rollover independent of line length and leaves LVGL
     * responsible for visual wrapping.
     */
    while (n > 0) {
        size_t free_bytes = (NAVTEX_TEXT_MAX - 1) - text_len;

        if (free_bytes == 0) {
            size_t dropped = drop_oldest_complete_line();

            if (dropped > 0) {
                fprintf(stderr,
                        "NAVTEX: history rollover, dropped %zu bytes, remaining %zu bytes\n",
                        dropped, text_len);
                continue;
            }

            /*
             * Pathological case: the whole 2 KB history is one unterminated
             * line.  A byte-bounded buffer must still make progress.  Drop
             * only the minimum oldest bytes required and report it clearly.
             */
            size_t need = n;
            if (need > NAVTEX_TEXT_MAX - 1)
                need = NAVTEX_TEXT_MAX - 1;

            if (need > text_len)
                need = text_len;

            if (need == 0)
                break;

            memmove(text_buf, text_buf + need, text_len - need);
            text_len -= need;
            text_buf[text_len] = '\0';

            fprintf(stderr,
                    "NAVTEX: history rollover without newline, dropped %zu bytes, remaining %zu bytes\n",
                    need, text_len);
            continue;
        }

        size_t chunk = n;
        if (chunk > free_bytes)
            chunk = free_bytes;

        memcpy(text_buf + text_len, src, chunk);
        text_len += chunk;
        text_buf[text_len] = '\0';
        src += chunk;
        n -= chunk;

        /*
         * If more input remains, force another rollover before copying it.
         * No incoming block is silently truncated.
         */
    }
}

static void char_cb(int c, void *userdata)
{
    (void)userdata;

    if (c == '\r')
        return;

    pthread_mutex_lock(&text_mutex);

    if (pending_count < NAVTEX_PENDING_MAX) {
        pending[pending_head] = (char)c;
        pending_head = (pending_head + 1) % NAVTEX_PENDING_MAX;
        ++pending_count;
    } else {
        /* Never print from the decoder/audio path.  Count here and report
         * later from ui_timer_cb(). */
        ++pending_overflow;
    }

    pthread_mutex_unlock(&text_mutex);
}

static bool reset_decoder(void)
{
    bool ok = false;

    pthread_mutex_lock(&decoder_mutex);

    if (decoder) {
        navtex_decoder_destroy(decoder);
        decoder = NULL;
    }

    decoder = navtex_decoder_create(0);
    if (decoder) {
        navtex_decoder_set_char_callback(decoder, char_cb, NULL);
        navtex_decoder_set_char_q_callback(decoder, scan_q_cb, NULL);   /* -> nav_char_q_cb */
        ok = true;
    }

    pthread_mutex_unlock(&decoder_mutex);

    if (!ok)
        fprintf(stderr, "NAVTEX: unable to create decoder\n");

    return ok;
}


static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* NAVTEX mode: characters -> bulletins (also while SITORB is shown) */
    nav_poll();
    nf_poll();
    scan_poll();

    char local[NAVTEX_PENDING_MAX];
    size_t n = 0;
    unsigned long overflow = 0;

    pthread_mutex_lock(&text_mutex);

    while (pending_count > 0 && n < sizeof(local)) {
        local[n++] = pending[pending_tail];
        pending_tail = (pending_tail + 1) % NAVTEX_PENDING_MAX;
        --pending_count;
    }

    overflow = pending_overflow;
    pending_overflow = 0;

    pthread_mutex_unlock(&text_mutex);

    if (overflow > 0)
        fprintf(stderr, "NAVTEX: pending FIFO overflow, lost %lu characters\n",
                overflow);

    if (n == 0 || !label)
        return;

    append_text(local, n);

    /* text_buf has static lifetime, like the stock RTTY panel buffer */
    live_label_update();

    if (scroll && follow_tail)
        lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

/* 1KO125: the text font follows Settings > Interface > Decoder text, as the
 * RTTY panel (panel.cpp): Compact sony_28, Large sony_38; live */
static ObserverDelayed *text_size_obs = NULL;

static const lv_font_t *navtex_font(void)
{
    return param_i_get(cfg.ui.digi_compact()) ? &sony_28 : &sony_38;
}

static void text_size_cb(Subject *subj, void *user_data)
{
    (void)subj;
    (void)user_data;
    if (label) {
        lv_obj_set_style_text_font(label, navtex_font(), 0);
        if (text_len) live_label_update();     /* another height per line */
    }
    if (nav_body_label)                 /* the open NAVTEX bulletin */
        lv_obj_set_style_text_font(nav_body_label, navtex_font(), 0);
    if (view_label) {
        lv_obj_set_style_text_font(view_label, navtex_font(), 0);
        view_set_window(view_start);
    }
    if (scroll && follow_tail) lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void construct_cb(lv_obj_t *parent)
{
    app_radio_begin();      /* 1KO125: USB-D locked, zoom 4x, DSP off while open */
    spectrum_set_markers(915, 1085);   /* tuning markers: 1000 Hz -/+ 85 Hz */
    pthread_mutex_lock(&text_mutex);
    pending_head = 0;
    pending_tail = 0;
    pending_count = 0;
    pending_overflow = 0;
    pthread_mutex_unlock(&text_mutex);
    text_len = 0;
    text_buf[0] = '\0';
    follow_tail = true;

    parent_obj = parent;
    browser_open = false;

    reset_decoder();

    if (dsp_audio_sub_id == AUDIO_SUB_INVALID)
        dsp_audio_sub_id = dsp_audio_subscribe_float(audio_cb, NAVTEX_SAMPLE_RATE);
    if (dsp_audio_sub_id != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_audio_sub_id, true);

    /*
     * Use exactly the same visual container as the built-in RTTY/CW panel.
     * panel_style supplies the same geometry, panel image for the selected
     * theme, padding and sony_24 font used by panel.cpp.
     *
     * Unlike the RTTY panel this object remains vertically scrollable,
     * because NAVTEX keeps a much longer text history.
     */
    scroll = lv_obj_create(parent);
    lv_obj_remove_style_all(scroll);
    lv_obj_add_style(scroll, &style.panels.base, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    dialog.obj = scroll;

    label = lv_label_create(scroll);
    /* panel_style has just been attached and LVGL may not have run a
     * layout pass yet.  lv_obj_get_content_width(scroll) can therefore
     * still be 0 here, making the label effectively invisible.
     * 100% means the panel's content width (same padding as RTTY).
     */
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    /* 1KO125: the font of Settings > Interface > Decoder text */
    lv_obj_set_style_text_font(label, navtex_font(), 0);
    if (!text_size_obs)
        text_size_obs = subject_subscribe_delayed((Subject *)cfg.ui.digi_compact(), text_size_cb, NULL);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, "Awaiting NAVTEX signal...\n");

    /* MFK target.  LEFT scrolls towards older text, RIGHT towards
     * the newest text.  The volume encoder is handled in key_cb too.
     */
    lv_group_add_obj(keyboard_group, scroll);
    lv_obj_add_event_cb(scroll, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(scroll);
    lv_group_set_editing(keyboard_group, true);

    /* NAVTEX bulletins: their frame, the mode saved last time */
    nav_construct(parent);
    nf_update_label();
    scan_wait_load();
    scan_resume = false;    /* the first Scan from the lowest frequency */

    ui_timer = lv_timer_create(ui_timer_cb, NAVTEX_UI_TIMER_MS, NULL);
}

static void destruct_cb(void)
{
    scan_stop(NULL);
    app_radio_end();        /* 1KO125: mode, zoom and DSP of before */
    if (text_size_obs) {
        param_unsubscribe((Observer *)text_size_obs);
        text_size_obs = NULL;
    }
    spectrum_clear_markers();
    app_info_abort();
    /* The browser panel is not a child of dialog.obj: delete it here. */
    browser_close(false);

    if (dsp_audio_sub_id != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_audio_sub_id, false);

    if (ui_timer) {
        lv_timer_del(ui_timer);
        ui_timer = NULL;
    }

    pthread_mutex_lock(&decoder_mutex);
    if (decoder) {
        navtex_decoder_destroy(decoder);
        decoder = NULL;
    }
    pthread_mutex_unlock(&decoder_mutex);

    pthread_mutex_lock(&text_mutex);
    pending_head = 0;
    pending_tail = 0;
    pending_count = 0;
    pending_overflow = 0;
    pthread_mutex_unlock(&text_mutex);

    nf_destroy();
    nav_destruct();

    label = NULL;
    scroll = NULL;
    parent_obj = NULL;
}

static void audio_cb(size_t n, float *samples)
{
    scan_audio(samples, n);     /* the tones, while the scan measures */

    pthread_mutex_lock(&decoder_mutex);
    if (decoder)
        navtex_decoder_process(decoder, samples, n);
    pthread_mutex_unlock(&decoder_mutex);
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    /* While a dialog is open the main-screen handler is disabled,
     * therefore NAVTEX must handle the volume encoder itself.
     */
    /* frequency list open: the list handles MFK and VOL (10 rows) itself */
    if (nf_frame)
        return;

    switch (key) {
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            return;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            return;

        default:
            break;
    }

    /* a saved message open: the MFK is its own (view_key_cb), it must not
     * scroll the live text hidden behind it */
    if (view_scroll)
        return;

    /* NAVTEX mode: the MFK moves through the bulletins / scrolls one */
    if (nav_key(key))
        return;

    if (!scroll)
        return;

    switch (key) {
        case LV_KEY_LEFT: {
            /* MFK left: browse older text and stop following the tail. */
            lv_coord_t y = lv_obj_get_scroll_y(scroll);
            follow_tail = false;
            lv_obj_scroll_to_y(scroll, y - NAVTEX_SCROLL_STEP, LV_ANIM_OFF);
            break;
        }

        case LV_KEY_RIGHT: {
            /* MFK right: browse newer text.  Once another step cannot
             * move farther, we are at the tail and automatic following
             * is enabled again.
             */
            lv_coord_t old_y = lv_obj_get_scroll_y(scroll);
            lv_obj_scroll_to_y(scroll, old_y + NAVTEX_SCROLL_STEP, LV_ANIM_OFF);
            lv_coord_t new_y = lv_obj_get_scroll_y(scroll);

            if (new_y == old_y) {
                follow_tail = true;
                lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
            }
            break;
        }

        default:
            break;
    }
}

static void close_cb(button_data_t *data)
{
    (void)data;
    dialog_destruct();
}

static void clear_cb(button_data_t *data)
{
    (void)data;

    pthread_mutex_lock(&text_mutex);
    pending_head = 0;
    pending_tail = 0;
    pending_count = 0;
    pending_overflow = 0;
    pthread_mutex_unlock(&text_mutex);

    text_len = 0;
    text_buf[0] = '\0';
    follow_tail = true;

    if (label)
        lv_label_set_text_static(label, text_buf);

    if (scroll)
        lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF);
}


/* ------------------------------------------------------------------ */
/* Save and Clear                                                      */
/* ------------------------------------------------------------------ */

/*
 * Write the text currently shown in the panel to
 * NAVTEX_SAVE_DIR/sitorb-YYYYMMDD-HHMMSS.txt.  Returns false (and leaves
 * the text untouched) on any error.
 */
static bool save_text(char *name, size_t name_size)
{
    if (mkdir(NAVTEX_SAVE_DIR, 0775) != 0 && errno != EEXIST) {
        fprintf(stderr, "NAVTEX SAVE: mkdir %s failed: %s\n",
                NAVTEX_SAVE_DIR, strerror(errno));
        return false;
    }

    /* 1KO125: file name in UTC, the time of the broadcast schedules */
    time_t now = time(NULL);
    struct tm tm_now;

    if (!gmtime_r(&now, &tm_now)) {
        fprintf(stderr, "NAVTEX SAVE: gmtime failed\n");
        return false;
    }

    snprintf(name, name_size,
             NAVTEX_SAVE_PREFIX "%04d%02d%02d-%02d%02d%02d" NAVTEX_SAVE_EXT,
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", NAVTEX_SAVE_DIR, name);

    /* "x": never overwrite an existing file (two saves in the same second). */
    FILE *f = fopen(path, "wx");
    if (!f) {
        fprintf(stderr, "NAVTEX SAVE: fopen %s failed: %s\n", path, strerror(errno));
        return false;
    }

    bool ok = fwrite(text_buf, 1, text_len, f) == text_len;

    if (ok && text_buf[text_len - 1] != '\n')
        ok = fputc('\n', f) != EOF;

    if (fflush(f) != 0)
        ok = false;
    if (ok)
        fsync(fileno(f));
    if (fclose(f) != 0)
        ok = false;

    if (!ok) {
        fprintf(stderr, "NAVTEX SAVE: write %s failed\n", path);
        unlink(path);
        return false;
    }

    fprintf(stderr, "NAVTEX SAVE: %s (%zu bytes)\n", path, text_len);
    return true;
}

static void browser_refresh(void);

static void save_clear_cb(button_data_t *data)
{
    (void)data;

    if (text_len == 0) {
        msg_schedule_text_fmt("Nothing to save");
        return;
    }

    char name[64];

    if (!save_text(name, sizeof(name))) {
        msg_schedule_text_fmt("NAVTEX: save failed");
        return;
    }

    /* Clear only what was saved (what is shown).  Characters still in the
     * pending FIFO were not displayed yet: they stay and appear next. */
    text_len = 0;
    text_buf[0] = '\0';
    follow_tail = true;

    if (label)
        lv_label_set_text_static(label, text_buf);
    if (scroll)
        lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF);

    msg_schedule_text_fmt("Saved %s", name);

    /* A browser already open shows the new file at once. */
    if (browser_open && browser_list && !view_scroll) {
        browser_refresh();
        ui_list_set_count(browser_list, (int)browser_file_count);
    }
}


/* ------------------------------------------------------------------ */
/* Saved-messages browser                                              */
/* ------------------------------------------------------------------ */

static void browser_free_files(void)
{
    for (size_t i = 0; i < browser_file_count; ++i)
        free(browser_files[i].name);

    free(browser_files);
    browser_files = NULL;
    browser_file_count = 0;
    browser_file_capacity = 0;
}

static int browser_name_compare(const void *a, const void *b)
{
    const navtex_file_t *fa = a;
    const navtex_file_t *fb = b;
    return strcmp(fb->name, fa->name);     /* newest timestamped name first */
}

/* First non-empty line of the file, for the browser's last column. */
static void browser_read_summary(const char *name, char *out, size_t out_size)
{
    out[0] = '\0';

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", NAVTEX_SAVE_DIR, name);

    FILE *f = fopen(path, "rb");
    if (!f)
        return;

    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            ++p;
        if (!*p)
            break;

        size_t len = strcspn(p, "\r\n");
        if (len >= out_size)
            len = out_size - 1;

        /* Keep printable ASCII only (decoder errors can produce odd bytes). */
        size_t o = 0;
        for (size_t i = 0; i < len; ++i)
            out[o++] = ((unsigned char)p[i] >= 32 && (unsigned char)p[i] < 127) ? p[i] : ' ';
        out[o] = '\0';
        return;
    }
}

static void browser_refresh(void)
{
    browser_free_files();

    DIR *dir = opendir(NAVTEX_SAVE_DIR);
    if (!dir)
        return;

    struct dirent *entry;
    size_t plen = strlen(NAVTEX_SAVE_PREFIX);
    size_t elen = strlen(NAVTEX_SAVE_EXT);

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);

        if (len <= plen + elen ||
            strncmp(name, NAVTEX_SAVE_PREFIX, plen) != 0 ||
            strcmp(name + len - elen, NAVTEX_SAVE_EXT) != 0)
            continue;

        if (browser_file_count == browser_file_capacity) {
            size_t nc = browser_file_capacity ? browser_file_capacity * 2 : 64;
            navtex_file_t *p = realloc(browser_files, nc * sizeof(*p));
            if (!p)
                break;
            browser_files = p;
            browser_file_capacity = nc;
        }

        char *copy = strdup(name);
        if (!copy)
            continue;

        browser_files[browser_file_count].name = copy;
        browser_files[browser_file_count].summary[0] = '\0';
        browser_file_count++;
    }
    closedir(dir);

    qsort(browser_files, browser_file_count, sizeof(browser_files[0]),
          browser_name_compare);

    for (size_t i = 0; i < browser_file_count; ++i)
        browser_read_summary(browser_files[i].name, browser_files[i].summary,
                             sizeof(browser_files[i].summary));
}

/* Row contents: "n | date | time | first line". */
static void browser_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;

    if (index < 0 || (size_t)index >= browser_file_count)
        return;

    const navtex_file_t *f = &browser_files[index];

    snprintf(row->cell[0], UI_LIST_CELL_LEN, "%d", index + 1);

    if (!ui_list_parse_stamp_ext(f->name, NAVTEX_SAVE_PREFIX, NAVTEX_SAVE_EXT,
                                 row->cell[1], UI_LIST_CELL_LEN,
                                 row->cell[2], UI_LIST_CELL_LEN)) {
        snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", f->name);
        row->cell[2][0] = '\0';
    }

    snprintf(row->cell[3], UI_LIST_CELL_LEN, "%s", f->summary);
}

/* The buttons of browser mode: the list, or the message open */
static void browser_buttons(void)
{
    if (!browser_open)
        return;
    if (view_scroll) {
        buttons_load_page(&btn_page_view);
        buttons_disabled(&btn_bv_newer, view_index <= 0);
        buttons_disabled(&btn_bv_older, (size_t)view_index + 1 >= browser_file_count);
        buttons_disabled(&btn_br_del, false);
    } else {
        buttons_load_page(&btn_page_browser);
        buttons_disabled(&btn_br_open, browser_file_count == 0);
        buttons_disabled(&btn_br_del, browser_file_count == 0);
    }
}

static void focus_obj(lv_obj_t *obj)
{
    if (lv_obj_get_group(obj) != keyboard_group)
        lv_group_add_obj(keyboard_group, obj);
    lv_group_focus_obj(obj);
    lv_group_set_editing(keyboard_group, true);
}

/* --- text view ---------------------------------------------------- */

static void view_close(void)
{
    if (!view_scroll)
        return;

    lv_obj_t *obj = view_scroll;
    view_scroll = NULL;
    view_label = NULL;

    /* Called from the view's own PRESSED event; LVGL 8.3 allows deleting
     * the target there (the SSTV TX lists do the same).  The text is freed
     * by view_delete_cb(). */
    lv_group_remove_obj(obj);
    lv_obj_del(obj);

    if (browser_list) {
        lv_obj_clear_flag(browser_list, LV_OBJ_FLAG_HIDDEN);

        /* Files may have been saved meanwhile: rescan, keep the row. */
        browser_refresh();
        ui_list_set_count(browser_list, (int)browser_file_count);
        focus_obj(browser_list);
    }
    view_index = -1;
    browser_buttons();
}

static void view_delete_cb(lv_event_t *e)
{
    /* The label shows the text as static text: free it with the label. */
    if (lv_event_get_user_data(e) == view_text) {
        view_text = NULL;
        view_len = view_start = view_end = 0;
    }
    free(lv_event_get_user_data(e));
}

/* Shows view_text from start, as much as fits */
static void view_set_window(size_t start)
{
    if (!view_label || !view_text || !view_len)
        return;
    view_text[view_end] = view_cut;     /* the whole text again */
    view_end = fit_head(view_text, start, view_len, navtex_font(), label_width(view_scroll),
                        NAVTEX_LABEL_MAX_H, NULL);
    if (view_end <= start)
        view_end = view_len;            /* never an empty window */
    view_cut = view_text[view_end];
    view_text[view_end] = '\0';
    view_start = start;
    lv_label_set_text_static(view_label, view_text + start);
    lv_obj_update_layout(view_scroll);
}

/* One MFK step scrolls the saved text by this many whole lines. */
#define NAVTEX_VIEW_STEP_LINES 2

static void view_key_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    lv_coord_t lh = lv_font_get_line_height(navtex_font());
    lv_coord_t step = NAVTEX_VIEW_STEP_LINES * lh;

    /* Scroll by whole lines: the view height is a whole number of lines,
     * so no line is ever shown cut at the top or bottom edge. */
    lv_coord_t y = lv_obj_get_scroll_y(obj);
    y = (y + lh / 2) / lh * lh;

    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            y -= step;
            break;

        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            y += step;
            break;

        default:
            return;     /* VOL: handled by key_cb (volume) */
    }

    lv_coord_t max_y = lv_obj_get_scroll_y(obj) + lv_obj_get_scroll_bottom(obj);
    lv_coord_t w = label_width(obj);

    /* past the end of the window: move it on by half its height, the text
     * on the screen staying where it is */
    if (y > max_y && view_end < view_len) {
        int32_t gone;
        size_t start = fit_head(view_text, view_start, view_end, navtex_font(), w,
                                NAVTEX_LABEL_MAX_H / 2, &gone);
        if (start > view_start) {
            lv_coord_t old_y = lv_obj_get_scroll_y(obj);
            view_set_window(start);
            lv_obj_scroll_to_y(obj, old_y - gone, LV_ANIM_OFF);
            y -= gone;
            max_y = lv_obj_get_scroll_y(obj) + lv_obj_get_scroll_bottom(obj);
        }
    } else if (y < 0 && view_start > 0) {
        int32_t added;
        size_t start = fit_tail(view_text, view_start, navtex_font(), w,
                                NAVTEX_LABEL_MAX_H / 2, &added);
        if (start < view_start) {
            lv_coord_t old_y = lv_obj_get_scroll_y(obj);
            view_set_window(start);
            lv_obj_scroll_to_y(obj, old_y + added, LV_ANIM_OFF);
            y += added;
            max_y = lv_obj_get_scroll_y(obj) + lv_obj_get_scroll_bottom(obj);
        }
    }

    if (y > max_y)
        y = max_y;
    if (y < 0)
        y = 0;

    lv_obj_scroll_to_y(obj, y, LV_ANIM_OFF);
}

/* Before deleting the focused object from its own key / press handler: wait
 * for the release, or LVGL takes the held key as a long press on the next
 * focused object (as digi_menu.c does) */
static void wait_release(void)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev)
        lv_indev_wait_release(indev);
}

static void view_pressed_cb(lv_event_t *e)
{
    (void)e;
    /* MFK press closes the text and returns to the list. */
    wait_release();
    view_close();
}

/* Read a saved file into a malloc'd, NUL-terminated string without '\r'. */
static char *view_load(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", NAVTEX_SAVE_DIR, name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "NAVTEX BROWSER: unable to open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    char *buf = malloc(NAVTEX_VIEW_MAX + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, NAVTEX_VIEW_MAX, f);
    bool truncated = !feof(f) && fgetc(f) != EOF;
    fclose(f);

    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = buf[i];
        if (c == '\r' || c == '\0')
            continue;
        buf[o++] = c;
    }
    buf[o] = '\0';

    if (truncated)
        fprintf(stderr, "NAVTEX BROWSER: %s shown truncated to %d bytes\n",
                path, NAVTEX_VIEW_MAX);

    return buf;
}

static void view_open(int index)
{
    if (!browser_panel || index < 0 || (size_t)index >= browser_file_count || view_scroll)
        return;

    char *text = view_load(browser_files[index].name);
    if (!text) {
        msg_schedule_text_fmt("Unable to open %s", browser_files[index].name);
        return;
    }

    /* Same font, colours and padding as the live NAVTEX text. */
    view_scroll = lv_obj_create(browser_panel);
    lv_obj_remove_style_all(view_scroll);

    /* Height: a whole number of text lines (see view_key_cb). */
    lv_coord_t lh = lv_font_get_line_height(navtex_font());
    lv_coord_t vh = lv_obj_get_content_height(browser_panel) / lh * lh;
    lv_obj_set_size(view_scroll, lv_pct(100), vh);
    lv_obj_align(view_scroll, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_scroll_dir(view_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(view_scroll, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    view_label = lv_label_create(view_scroll);
    lv_obj_set_width(view_label, lv_pct(100));
    lv_label_set_long_mode(view_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(view_label, navtex_font(), 0);
    lv_obj_set_style_text_color(view_label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(view_label, 0, 0);
    view_text = text;
    view_len = strlen(text);
    view_start = 0;
    view_end = view_len;                /* view_set_window puts view_cut back there */
    view_cut = '\0';
    if (view_len)
        view_set_window(0);             /* as much of it as fits */
    else
        lv_label_set_text_static(view_label, "(empty file)");

    lv_obj_add_event_cb(view_scroll, view_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(view_scroll, key_cb, LV_EVENT_KEY, NULL);   /* VOL */
    lv_obj_add_event_cb(view_scroll, view_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(view_label, view_delete_cb, LV_EVENT_DELETE, text);

    if (browser_list) {
        lv_group_remove_obj(browser_list);
        lv_obj_add_flag(browser_list, LV_OBJ_FLAG_HIDDEN);
    }
    focus_obj(view_scroll);
    view_index = index;
    browser_buttons();

    fprintf(stderr, "NAVTEX BROWSER: opened %s\n", browser_files[index].name);
}

static void browser_activate_cb(void *user, int index)
{
    (void)user;
    view_open(index);
}

/* --- browser ------------------------------------------------------ */

static void browser_build(void)
{
    if (browser_open || !parent_obj || !scroll)
        return;

    if (scan_running()) {                    /* it could be neither seen nor stopped */
        scan_stop("Scan stopped");
        scan_resume = true;                  /* Scan goes on from here, as the other stops */
    }
    browser_refresh();

    /* Exactly the panel used by the live NAVTEX text (same geometry and
     * background), shown in its place. */
    browser_panel = lv_obj_create(parent_obj);
    lv_obj_remove_style_all(browser_panel);
    lv_obj_add_style(browser_panel, &style.panels.base, 0);
    lv_obj_clear_flag(browser_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(browser_panel);

    lv_coord_t w = lv_obj_get_content_width(browser_panel);
    lv_coord_t h = lv_obj_get_content_height(browser_panel);

    /* Same columns as the WEFAX/SSTV browsers, plus the first text line. */
    ui_list_cfg_t cfg = {
        .width = w,
        .height = h,
        .col_count = 4,
        .cols = {
            { .x = 12,  .w = 44,  .align = UI_LIST_ALIGN_RIGHT, .muted = true },
            { .x = 76,  .w = 190, .align = UI_LIST_ALIGN_LEFT },
            { .x = 290, .w = 140, .align = UI_LIST_ALIGN_LEFT },
            { .x = 440, .w = w - 12 - 440 - 8, .align = UI_LIST_ALIGN_LEFT, .muted = true },
        },
        .row_cb = browser_row_cb,
        .activate_cb = browser_activate_cb,
        .empty_text = "No saved messages",
        .vol_coarse = true,
    };

    browser_list = ui_list_create(browser_panel, &cfg);
    lv_obj_center(browser_list);
    ui_list_set_count(browser_list, (int)browser_file_count);

    /* The live text keeps being updated while hidden. */
    lv_group_remove_obj(scroll);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_HIDDEN);
    focus_obj(browser_list);

    browser_open = true;
    browser_buttons();
}

static void browser_close(bool restore_focus)
{
    if (browser_panel) {
        if (view_scroll)
            lv_group_remove_obj(view_scroll);
        if (browser_list)
            lv_group_remove_obj(browser_list);
        lv_obj_del(browser_panel);      /* also frees the view text (DELETE event) */
    }

    browser_panel = NULL;
    browser_list = NULL;
    view_scroll = NULL;
    view_label = NULL;
    view_index = -1;
    browser_free_files();

    bool was_open = browser_open;
    browser_open = false;

    if (was_open && scroll) {
        lv_obj_clear_flag(scroll, LV_OBJ_FLAG_HIDDEN);

        /* Give the keys back explicitly: when the browser left the group
         * the live panel was still hidden and could not take the focus. */
        if (restore_focus)
            focus_obj(scroll);

        if (follow_tail) {
            lv_obj_update_layout(scroll);
            lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
        }
    }

    if (restore_focus && was_open)
        buttons_load_page(&btn_page_2);     /* where Sitor-B Browser is */
}

static void browser_cb(button_data_t *data)
{
    (void)data;

    if (browser_open)
        browser_close(true);
    else
        browser_build();
}

/* --- browser mode buttons ------------------------------------------- */

static void br_open_cb(button_data_t *data)
{
    (void)data;
    if (browser_list && !view_scroll)
        view_open(ui_list_get_selected(browser_list));
}

/* another message of the list, from the one open */
static void bv_goto(int index)
{
    if (!view_scroll || index < 0 || (size_t)index >= browser_file_count)
        return;
    view_close();
    ui_list_select(browser_list, index);
    view_open(index);
}

static void bv_back_cb(button_data_t *data)  { (void)data; view_close(); }
static void bv_newer_cb(button_data_t *data) { (void)data; bv_goto(view_index - 1); }  /* newest first */
static void bv_older_cb(button_data_t *data) { (void)data; bv_goto(view_index + 1); }

/* Delete, no confirmation (as the NAVTEX bulletins): from the list the next
 * row is selected; from a message open the next (older) one opens, back to
 * the list after the last */
static void br_delete_cb(button_data_t *data)
{
    (void)data;
    if (!browser_list)
        return;
    bool open = view_scroll != NULL;
    int index = open ? view_index : ui_list_get_selected(browser_list);
    if (index < 0 || (size_t)index >= browser_file_count)
        return;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", NAVTEX_SAVE_DIR, browser_files[index].name);
    if (unlink(path) != 0) {
        fprintf(stderr, "NAVTEX BROWSER: unable to delete %s: %s\n", path, strerror(errno));
        msg_schedule_text_fmt("Unable to delete %s", browser_files[index].name);
        return;
    }
    fprintf(stderr, "NAVTEX BROWSER: deleted %s\n", path);

    if (open)
        view_close();                       /* rescans the folder */
    else {
        browser_refresh();
        ui_list_set_count(browser_list, (int)browser_file_count);
    }
    if (browser_file_count == 0) {
        browser_buttons();
        return;
    }
    if ((size_t)index < browser_file_count) {
        ui_list_select(browser_list, index);
        if (open)
            view_open(index);
    } else {
        ui_list_select(browser_list, (int)browser_file_count - 1);
    }
    browser_buttons();
}

#include "dialog_navtex_bul.inc"
#include "dialog_navtex_freq.inc"
#include "dialog_navtex_scan.inc"
