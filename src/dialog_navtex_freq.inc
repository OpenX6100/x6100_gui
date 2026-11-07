/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 - NAVTEX frequency list ("Freq."), included by dialog_navtex.c.
 *
 * Sections: NAVTEX MF/HF, SITOR-B coast stations on their own channels,
 * HF MSI worldwide.  The same list as DSC: sections with a coloured heading, one frequency per
 * row, VOL scrolls 10 rows, press / Tune tunes it.  Microphone: V/M opens
 * and closes the list, UP/DOWN step through it (circular, headings
 * skipped; from a frequency not in the list to the first one, 518 kHz).
 *
 * Audio centre 1000 Hz, like the "NAVTEX MF/HF" channels: dial = frequency
 * - 1 kHz, USB-DIG.
 */

#include "cfg/cfg_api.h"

#define NF_AUDIO_HZ     1000

typedef struct { int32_t hz; const char *band; const char *section; int kind; } nf_freq_t;

#define NF_HEAD(t, k)   { 0, NULL, t, k }
#define NF_CH(h, b)     { h, b, NULL, 0 }

static const nf_freq_t nf_freqs[] = {
    NF_HEAD("NAVTEX - MF / HF", 0),
    NF_CH(  518000, "MF international (English)"),
    NF_CH(  490000, "MF national language"),
    NF_CH( 4209500, "HF NAVTEX"),

    /* coast stations on their own SITOR-B channels (centre frequencies,
     * www.dxinfocentre.com, 2026-09; Olympia 16830.5 measured) */
    NF_HEAD("SITOR-B STATIONS", 2),
    NF_CH(  8424000, "SVO Olympia (GRC)"),
    NF_CH( 16830500, "SVO Olympia (GRC)"),
    NF_CH(  6322500, "UDK Murmansk (RUS)"),
    NF_CH(  8424000, "UDK Murmansk (RUS)"),
    NF_CH( 12586000, "UDK Murmansk (RUS)"),
    NF_CH(  8416750, "UAT Moscow (RUS)"),
    NF_CH( 12578250, "UAT Moscow (RUS)"),
    NF_CH(  6425000, "LGI Hammerfest (NOR)"),
    NF_CH(  6326000, "XSG Shanghai (CHN)"),
    NF_CH(  8425500, "XSG Shanghai (CHN)"),
    NF_CH( 12637500, "XSG Shanghai (CHN)"),
    NF_CH( 16898500, "XSG Shanghai (CHN)"),
    NF_CH(  4212500, "XSV Tianjin (CHN)"),
    NF_CH(  4283000, "XSV Tianjin (CHN)"),
    NF_CH(  8417500, "XSV Tianjin (CHN)"),
    NF_CH(  8600000, "XSV Tianjin (CHN)"),
    NF_CH( 12581500, "XSV Tianjin (CHN)"),
    NF_CH( 12969000, "XSV Tianjin (CHN)"),
    NF_CH(  6333500, "XSZ Dalian (CHN)"),
    NF_CH(  8694000, "XSZ Dalian (CHN)"),
    NF_CH(  8515000, "UFL Vladivostok (RUS)"),
    NF_CH( 12955000, "UFL Vladivostok (RUS)"),
    NF_CH(  4255000, "UFH Petropavlovsk (RUS)"),
    NF_CH(  4323000, "UFH Petropavlovsk (RUS)"),
    NF_CH(  6360500, "UFH Petropavlovsk (RUS)"),
    NF_CH(  6405000, "UFH Petropavlovsk (RUS)"),
    NF_CH(  8451000, "UFH Petropavlovsk (RUS)"),
    NF_CH( 12603000, "UFH Petropavlovsk (RUS)"),
    NF_CH( 17045000, "UFH Petropavlovsk (RUS)"),
    NF_CH(  4345000, "UFM Nevelsk (RUS)"),
    NF_CH(  6323000, "UFM Nevelsk (RUS)"),
    NF_CH(  6334500, "UFM Nevelsk (RUS)"),
    NF_CH(  8446000, "UFM Nevelsk (RUS)"),
    NF_CH( 12684500, "UFM Nevelsk (RUS)"),
    NF_CH(  4268000, "PWZ Rio de Janeiro (BRA)"),
    NF_CH(  6450000, "PWZ Rio de Janeiro (BRA)"),
    NF_CH(  8582000, "PWZ Rio de Janeiro (BRA)"),
    NF_CH( 12711000, "PWZ Rio de Janeiro (BRA)"),
    NF_CH( 16976000, "PWZ Rio de Janeiro (BRA)"),
    NF_CH(  4247000, "KPH Pt Reyes (USA) Sat"),
    NF_CH(  6324500, "KPH Pt Reyes (USA) Sat"),
    NF_CH(  6477500, "KPH Pt Reyes (USA) Sat"),
    NF_CH(  8427000, "KPH Pt Reyes (USA) Sat"),
    NF_CH(  8642000, "KPH Pt Reyes (USA) Sat"),
    NF_CH( 12585500, "KPH Pt Reyes (USA) Sat"),
    NF_CH( 12808500, "KPH Pt Reyes (USA) Sat"),
    NF_CH( 17016800, "KPH Pt Reyes (USA) Sat"),
    NF_CH( 22477500, "KPH Pt Reyes (USA) Sat"),
    NF_CH( 12695500, "KFS S.Francisco (USA) Sat"),

    NF_HEAD("HF MSI (SITOR-B) - WORLDWIDE", 1),
    NF_CH( 4210000, "4 MHz"),   NF_CH( 6314000, "6 MHz"),   NF_CH( 8416500, "8 MHz"),
    NF_CH(12579000, "12 MHz"),  NF_CH(16806500, "16 MHz"),  NF_CH(19680500, "19 MHz"),
    NF_CH(22376000, "22 MHz"),  NF_CH(26100500, "26 MHz"),
};

#undef NF_HEAD
#undef NF_CH
#define NF_N ((int)(sizeof(nf_freqs) / sizeof(nf_freqs[0])))

static char nf_label[24] = "Freq.";
static int nf_recalled = -1;
static int32_t nf_shown_dial = 0;
static lv_obj_t *nf_frame = NULL;
static lv_obj_t *nf_list = NULL;
static lv_obj_t *nf_prev_focus = NULL;

static void nf_open_cb(button_data_t *d);
static void nf_tune_cb(button_data_t *d);
static void nf_close_cb(button_data_t *d);

/* on the first page of both modes (declared in dialog_navtex.c) */
static button_data_t btn_freq      = { .type = BTN_TEXT, .label = nf_label, .press = nf_open_cb };
static button_data_t btn_nf_title  = { .type = BTN_TEXT, .label = "(NAVTEX Freq.)", .press = NULL };
static button_data_t btn_nf_tune   = { .type = BTN_TEXT, .label = "Tune", .press = nf_tune_cb };
static button_data_t btn_nf_close  = { .type = BTN_TEXT, .label = "Close\nList", .press = nf_close_cb };

/* Scan: the list from its lowest frequency up (see
 * dialog_navtex_scan.inc); Wait: while its best value is being found */
static buttons_page_t nf_page = { .items = { &btn_nf_title, &btn_nf_tune, &btn_scan, &btn_scan_wait, &btn_nf_close } };

/* RF frequency of the 1000 Hz audio centre */
static int32_t nf_channel_hz(void)
{
    int32_t f = cparam_i_get(cfg.cur.fg_freq());
    int mode = cparam_i_get(cfg.cur.mode());
    return (mode == x6100_mode_lsb || mode == x6100_mode_lsb_dig) ? f - NF_AUDIO_HZ : f + NF_AUDIO_HZ;
}

/* "8424.0", "8416.75" (two decimals only when needed) */
static void nf_fmt_khz(char *dst, size_t sz, int32_t hz)
{
    if (hz % 100) snprintf(dst, sz, "%ld.%02ld", (long)(hz / 1000), (long)((hz % 1000) / 10));
    else snprintf(dst, sz, "%ld.%ld", (long)(hz / 1000), (long)((hz % 1000) / 100));
}

static bool nf_is_head(int i)
{
    return i >= 0 && i < NF_N && nf_freqs[i].hz == 0;
}

/* row of the current frequency, -1 when not in the list */
static int nf_current_row(void)
{
    int32_t hz = nf_channel_hz();
    if (nf_recalled >= 0 && nf_recalled < NF_N && nf_freqs[nf_recalled].hz == hz) return nf_recalled;
    for (int i = 0; i < NF_N; ++i) if (nf_freqs[i].hz == hz) return i;
    return -1;
}

static void nf_update_label(void)
{
    char k[16];
    nf_shown_dial = cparam_i_get(cfg.cur.fg_freq());
    nf_fmt_khz(k, sizeof(k), nf_channel_hz());
    snprintf(nf_label, sizeof(nf_label), "Freq.\n%s", k);
    if (btn_freq.disp_btn) buttons_refresh(&btn_freq);
}

/* every UI tick: the label follows the VFO */
static void nf_poll(void)
{
    if (cparam_i_get(cfg.cur.fg_freq()) != nf_shown_dial) nf_update_label();
}

/* ---- the list ------------------------------------------------------------------- */

static void nf_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    if (index < 0 || index >= NF_N) return;
    const nf_freq_t *f = &nf_freqs[index];
    if (f->hz == 0) {
        snprintf(row->cell[0], UI_LIST_CELL_LEN, "%s", f->section);
        return;
    }
    char k[16];
    snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", f->band);
    nf_fmt_khz(k, sizeof(k), f->hz);
    snprintf(row->cell[2], UI_LIST_CELL_LEN, "%s kHz", k);
    nf_fmt_khz(k, sizeof(k), f->hz - NF_AUDIO_HZ);
    snprintf(row->cell[3], UI_LIST_CELL_LEN, "dial %s USB-D", k);
    row->mark = (index == nf_current_row());
}

/* coloured bar under each section heading */
static void nf_draw_cb(void *user, int index, lv_draw_ctx_t *ctx, const lv_area_t *a, bool sel)
{
    (void)user; (void)sel;
    if (!nf_is_head(index)) return;
    int kd = nf_freqs[index].kind;
    uint32_t c = kd == 0 ? NB_C_SELECT : (kd == 2 ? NB_C_WARN : NB_C_OK);
    nav_rect(ctx, a->x1 + 4, a->y1 + 4, a->x1 + 9, a->y2 - 4, c, 2, 0, 0);
    nav_rect(ctx, a->x1 + 14, a->y2 - 3, a->x2 - 14, a->y2 - 2, c, 0, 0, 0);
}

static void nf_tune_row(int index)
{
    if (index < 0 || index >= NF_N || nf_is_head(index)) return;
    cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);
    cparam_i_set(cfg.cur.fg_freq(), nf_freqs[index].hz - NF_AUDIO_HZ);
    nf_recalled = index;
    nf_update_label();
    char k[16];
    nf_fmt_khz(k, sizeof(k), nf_freqs[index].hz);
    /* the section a frequency belongs to is the nearest heading above it */
    int h = index;
    while (h > 0 && !nf_is_head(h)) h--;
    /* update, not schedule: stepping fast replaces the text instead of queueing */
    if (nf_freqs[h].kind == 2)          /* a coast station: its name */
        msg_update_text_fmt("%s %s kHz", nf_freqs[index].band, k);
    else
        msg_update_text_fmt("%s %s kHz", nf_freqs[h].kind == 0 ? "NAVTEX" : "SITOR-B", k);
}

static void nf_close(void)
{
    if (!nf_frame) return;
    if (nf_list && lv_obj_get_group(nf_list)) lv_group_remove_obj(nf_list);
    lv_obj_del(nf_frame);
    nf_frame = NULL;
    nf_list = NULL;
    /* back to the view of the current mode */
    if (nf_prev_focus) {
        focus_obj(nf_prev_focus);
        nf_prev_focus = NULL;
    }
    if (nav_mode) {
        if (nav_detail) buttons_load_page(&nav_page_detail);
        else buttons_load_page(&nav_page_1);
    } else {
        buttons_load_page(&btn_page_1);
    }
}

static void nf_activate_cb(void *user, int index)
{
    (void)user;
    if (nf_is_head(index)) return;              /* a heading: nothing to tune */
    nf_tune_row(index);
    nf_close();
}

static void nf_open(void)
{
    if (nf_frame || !parent_obj) return;
    browser_close(false);                       /* the Sitor-B browser shares the panel */

    nf_prev_focus = lv_group_get_focused(keyboard_group);
    if (nf_prev_focus) lv_group_remove_obj(nf_prev_focus);
    if (!nav_mode && scroll) nf_prev_focus = scroll;

    /* the WEFAX / DSC big frame, over the text or the bulletins */
    nf_frame = lv_obj_create(parent_obj);
    lv_obj_remove_style_all(nf_frame);
    lv_obj_add_style(nf_frame, &style.dialog.base, 0);
    lv_obj_set_size(nf_frame, NB_W, NB_H);
    lv_obj_center(nf_frame);
    lv_obj_set_style_translate_y(nf_frame, 4, 0);
    lv_obj_clear_flag(nf_frame, LV_OBJ_FLAG_SCROLLABLE);

    ui_list_cfg_t cfg = {
        .width = NB_W - 20, .height = NB_H - 24, .font = &sony_24,
        .col_count = 4,
        .cols = {
            { .x = 18,  .w = 730, .align = UI_LIST_ALIGN_LEFT },                /* heading */
            { .x = 44,  .w = 300, .align = UI_LIST_ALIGN_LEFT, .muted = true },  /* band */
            { .x = 360, .w = 180, .align = UI_LIST_ALIGN_LEFT },                /* kHz */
            { .x = 550, .w = 210, .align = UI_LIST_ALIGN_LEFT, .muted = true },  /* dial */
        },
        .row_cb = nf_row_cb,
        .activate_cb = nf_activate_cb,
        .draw_cb = nf_draw_cb,
        .empty_text = "",
        .vol_coarse = true,                     /* VOL: 10 rows at a time */
    };
    nf_list = ui_list_create(nf_frame, &cfg);
    lv_obj_center(nf_list);
    ui_list_set_count(nf_list, NF_N);
    int cur = nf_current_row();
    ui_list_select(nf_list, cur >= 0 ? cur : 1);
    focus_obj(nf_list);
    buttons_load_page(&nf_page);
}

static void nf_open_cb(button_data_t *d) { (void)d; nf_open(); }

/* dialog closing: no page or focus to give back */
static void nf_destroy(void)
{
    if (nf_list && lv_obj_get_group(nf_list)) lv_group_remove_obj(nf_list);
    if (nf_frame) lv_obj_del(nf_frame);
    nf_frame = NULL;
    nf_list = NULL;
    nf_prev_focus = NULL;
}
static void nf_close_cb(button_data_t *d) { (void)d; nf_close(); }

static void nf_tune_cb(button_data_t *d)
{
    (void)d;
    if (nf_list) nf_activate_cb(NULL, ui_list_get_selected(nf_list));
}

/* ---- microphone ---------------------------------------------------------------------- */

bool dialog_navtex_vm_toggle(void)
{
    if (!dialog.run) return false;
    app_info_close();
    if (nf_frame) nf_close();
    else nf_open();
    return true;
}

bool dialog_navtex_mic_step(int dir)
{
    if (!dialog.run || dir == 0) return false;
    int row = nf_current_row();
    if (row < 0) {
        row = 0;
        while (nf_is_head(row)) row++;
    } else {
        int step = dir > 0 ? 1 : -1;
        do {
            row = (row + step + NF_N) % NF_N;
        } while (nf_is_head(row));
    }
    nf_tune_row(row);
    if (nf_list) {
        ui_list_select(nf_list, row);
        lv_obj_invalidate(nf_list);
    }
    return true;
}
