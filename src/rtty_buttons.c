/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - RTTY button page 2
 */

#include "rtty_buttons.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "mfk.h"
#include "msg.h"
#include "digi_menu.h"
#include "digi_next.h"
#include "digi_qso.h"
#include "rtty_sql.h"
#include "rtty_tx.h"

static digi_next_t next_state;          /* where the QSO is, for Next */

/* ---- helpers -------------------------------------------------------- */

static const char *macro_name_at(int idx)
{
    const rtty_macro_t *m = (idx < rtty_macros_count()) ? rtty_macro_get(idx) : NULL;

    return m ? m->name : NULL;
}

static void send_macro(int idx)
{
    const rtty_macro_t *m = rtty_macro_get(idx);

    if (m == NULL) {
        return;
    }
    if (!rtty_macro_send(idx)) {
        msg_update_text_fmt("#FF0000 RTTY TX busy");
        return;
    }
    msg_update_text_fmt("#FFFFFF RTTY TX: %s", m->name);
    digi_next_sent(&next_state, m->name, digi_qso_dx());
    if (digi_next_is_cq(m->name)) {
        digi_qso_clear();               /* a CQ starts a new QSO */
    }
}

/* ---- page button ---------------------------------------------------- */

static button_data_t btn_rtty_p2 = {
    .type  = BTN_TEXT,
    .label = "(RTTY 2:2)",
    .press = button_next_page_cb,
    .hold  = button_prev_page_cb,
};

/* ---- Next ----------------------------------------------------------- */

/* The step the QSO is at: CQ, Answer, Report, TNX or 73, by macro name.
 * While a transmission runs the same button stops it. */
static const char *next_label_getter(void)
{
    static char buf[48];

    if (rtty_tx_is_on()) {
        snprintf(buf, sizeof(buf), "Stop\nTX");
    } else {
        digi_next_label(&next_state, digi_qso_dx(), rtty_macros_count(), macro_name_at, buf,
                        sizeof(buf));
    }
    return buf;
}

static void next_press_cb(button_data_t *b)
{
    int idx;

    digi_menu_close();
    if (rtty_tx_is_on()) {
        rtty_tx_stop();
        buttons_refresh(b);
        return;
    }
    idx = digi_next_pick(&next_state, digi_qso_dx(), rtty_macros_count(), macro_name_at);
    if (idx < 0) {
        msg_update_text_fmt("#FF0000 No CQ/Answer/Report/TNX/73 macro - see Macros");
        return;
    }
    send_macro(idx);
    buttons_refresh(b);
}

static button_data_t btn_rtty_next = {
    .type     = BTN_TEXT_FN,
    .label_fn = next_label_getter,
    .press    = next_press_cb,
};

/* ---- SQL ------------------------------------------------------------ */

/*
 * The threshold alone is useless without seeing the level it is compared
 * with: park on a quiet spot, read the lower number, set the upper one a
 * few dB above it. "*" means the gate is open. The level moves all the
 * time, so this lives on a button, repainted by the timer below, rather
 * than in a list. Press hands the MFK knob to it, like Rate and Shift.
 */
static char sql_label[32];

static const char *sql_label_getter(void)
{
    uint8_t v = rtty_change_squelch(0);
    int     lvl = (int)lroundf(rtty_get_level_db());

    if (v == 0) {
        snprintf(sql_label, sizeof(sql_label), "SQL off\nlvl %d", lvl);
    } else {
        snprintf(sql_label, sizeof(sql_label), "SQL %u%s\nlvl %d", v,
                 rtty_squelch_is_open() ? "*" : "", lvl);
    }
    return sql_label;
}

static void sql_press_cb(button_data_t *b)
{
    (void)b;
    digi_menu_close();
    mfk_set_ctrl(CTRL_RTTY_SQUELCH);
}

static button_data_t btn_rtty_sql = {
    .type     = BTN_TEXT_FN,
    .label_fn = sql_label_getter,
    .press    = sql_press_cb,
    .ctrl     = CTRL_RTTY_SQUELCH,
};

/* ---- Macros --------------------------------------------------------- */

static void macro_edit_pick(int item, void *ctx)
{
    (void)ctx;
    rtty_macros_edit_slot(item);
}

static void macros_pick(int item, void *ctx)
{
    static char  text[DIGI_MENU_MAX][RTTY_MACRO_NAME_LEN + 4];
    const char  *items[DIGI_MENU_MAX];
    int          n = rtty_macros_count();
    int          shown = (n < DIGI_MENU_MAX - 3) ? n : DIGI_MENU_MAX - 3;
    int          k = 0;

    (void)ctx;
    if (item == 0) {
        rtty_tx_open_input();           /* free text */
        return;
    }
    item--;
    if (item < shown) {
        send_macro(item);
        buttons_refresh(&btn_rtty_next);
    } else if (item == shown) {
        if (n < RTTY_MACRO_MAX) {
            rtty_macros_open_menu();    /* a new one, at the end */
        }
    } else {
        for (int i = 0; i < n && k < DIGI_MENU_MAX; i++) {
            snprintf(text[k], sizeof(text[k]), "%s", rtty_macro_get(i)->name);
            items[k] = text[k];
            k++;
        }
        digi_menu_open(lv_scr_act(), "Edit which macro?", items, k, macro_edit_pick, NULL,
                       NULL);
    }
}

/* Free text, every macro with what it sends now, then New and Edit. */
static void macros_press_cb(button_data_t *b)
{
    static char  text[DIGI_MENU_MAX][72];
    const char  *items[DIGI_MENU_MAX];
    int          n = rtty_macros_count();
    int          k = 0;

    (void)b;
    if (digi_menu_is_open()) {
        digi_menu_close();
        return;
    }
    items[k++] = "Free text...";
    for (int i = 0; i < n && k < DIGI_MENU_MAX - 2; i++) {
        char out[RTTY_MACRO_TEXT_LEN + 32];

        rtty_macro_expand(i, out, sizeof(out));
        for (char *p = out; *p; p++) {
            if (*p == '\n' || *p == '\r') {
                *p = ' ';
            }
        }
        snprintf(text[k], sizeof(text[k]), "%-7s %.60s", rtty_macro_get(i)->name, out);
        items[k] = text[k];
        k++;
    }
    items[k++] = (n < RTTY_MACRO_MAX) ? "New macro..." : "(all macro slots used)";
    if (n > 0) {
        items[k++] = "Edit a macro...";
    }
    digi_menu_open(lv_scr_act(), "Macros: MFK to choose, press to send", items, k,
                   macros_pick, NULL, NULL);
}

static void macros_hold_cb(button_data_t *b)
{
    (void)b;
    digi_menu_close();
    rtty_macros_open_menu();
}

static button_data_t btn_rtty_macros = {
    .type  = BTN_TEXT,
    .label = "Macros",
    .press = macros_press_cb,
    .hold  = macros_hold_cb,
};

/* ---- Menu ----------------------------------------------------------- */

/*
 * The reports are set with the knob, like Rate, Shift and Center:
 * picking one hands the MFK knob to it and its value shows on the
 * message line as it turns.
 */
enum {
    MENU_DX, MENU_RST_SENT, MENU_RST_RCVD, MENU_AUTO_DX, MENU_NEW_QSO, MENU_COUNT
};

static void menu_pick(int item, void *ctx)
{
    (void)ctx;
    switch (item) {
        case MENU_DX:
            rtty_qso_open_dx_input();
            break;

        case MENU_RST_SENT:
            mfk_set_ctrl(CTRL_RTTY_RST_SENT);
            break;

        case MENU_RST_RCVD:
            mfk_set_ctrl(CTRL_RTTY_RST_RCVD);
            break;

        case MENU_AUTO_DX:
            digi_qso_toggle_autograb();
            msg_update_text_fmt("#FFFFFF Auto DX %s", digi_qso_autograb() ? "on" : "off");
            break;

        case MENU_NEW_QSO:
            digi_qso_clear();
            memset(&next_state, 0, sizeof(next_state));
            msg_update_text_fmt("#FFFFFF QSO cleared");
            break;
    }
    buttons_refresh(&btn_rtty_next);
}

static void menu_press_cb(button_data_t *b)
{
    static char  text[MENU_COUNT][56];
    const char  *items[MENU_COUNT];
    const char  *dx = digi_qso_dx();

    (void)b;
    if (digi_menu_is_open()) {
        digi_menu_close();
        return;
    }
    snprintf(text[MENU_DX], 56, "DX call...  (%s)", (dx && *dx) ? dx : "none");
    snprintf(text[MENU_RST_SENT], 56, "RST sent with the knob  (%s)", digi_qso_rst_sent());
    snprintf(text[MENU_RST_RCVD], 56, "RST received with the knob  (%s)",
             digi_qso_rst_rcvd());
    snprintf(text[MENU_AUTO_DX], 56, "Auto DX from the text: %s  (press: %s)",
             digi_qso_autograb() ? "on" : "off", digi_qso_autograb() ? "off" : "on");
    snprintf(text[MENU_NEW_QSO], 56, "New QSO (clear DX call, RST)");
    for (int i = 0; i < MENU_COUNT; i++) {
        items[i] = text[i];
    }
    digi_menu_open(lv_scr_act(), "Menu", items, MENU_COUNT, menu_pick, NULL, NULL);
}

static button_data_t btn_rtty_menu = {
    .type  = BTN_TEXT,
    .label = "Menu",
    .press = menu_press_cb,
};

/* ---- page ----------------------------------------------------------- */

buttons_page_t buttons_page_rtty_2 = {
    {&btn_rtty_p2, &btn_rtty_next, &btn_rtty_sql, &btn_rtty_macros, &btn_rtty_menu}
};

void rtty_refresh_macro_labels(void)
{
    if (btn_rtty_next.disp_btn) {
        buttons_refresh(&btn_rtty_next);
    }
}

/* ---- live repaint ---------------------------------------------------- */

/* Next follows the transmitter (Stop TX while it runs) and the DX call,
 * which the decoder may pick up off the air, and SQL shows the live
 * level, so both are repainted here. */
static lv_timer_t *sql_timer = NULL;

static void sql_timer_cb(lv_timer_t *t)
{
    (void)t;
    rtty_refresh_macro_labels();
    if (btn_rtty_sql.disp_btn) {
        buttons_refresh(&btn_rtty_sql);
    }
}

void rtty_sql_timer_start(void)
{
    if (!sql_timer) {
        sql_timer = lv_timer_create(sql_timer_cb, 400, NULL);
    }
}

void rtty_sql_timer_stop(void)
{
    digi_menu_close();
    if (sql_timer) {
        lv_timer_del(sql_timer);
        sql_timer = NULL;
    }
}
