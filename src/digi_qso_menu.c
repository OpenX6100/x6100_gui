/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the QSO menu of the keyboard modes
 */

#include "digi_qso_menu.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cfg/cfg_api.h"
#include "digi_menu.h"
#include "digi_qso.h"
#include "digi_text.h"
#include "msg.h"
#include "textarea_window.h"

#define FIELD_MAX 31            /* digi_qso.c keeps 32 bytes */
#define ITEM_LEN  64

enum {
    ITEM_LOG, ITEM_NAME, ITEM_QTH, ITEM_RSTR, ITEM_MYNAME, ITEM_MYQTH, ITEM_PICKUP, ITEM_AUTOLOG,
    ITEM_TEXT, ITEM_COUNT
};

static const digi_qso_menu_t *cur;
static int                    field = -1;   /* the item a text window edits */

/* A QSO whose 73 is queued: logged when the transmission ends. */
#define PENDING_POLL_MS 200

static digi_log_qso_t pending;
static bool         (*pending_tx_on)(void);
static lv_timer_t    *pending_timer;

static void changed(void)
{
    if (cur && cur->changed) {
        cur->changed();
    }
}

static uint64_t freq_now(const digi_qso_menu_t *m)
{
    return (m && m->freq) ? m->freq() : 0;
}

static const char *submode_now(const digi_qso_menu_t *m)
{
    return (m && m->submode) ? m->submode() : NULL;
}

/* ---- text fields ------------------------------------------------------ */

static bool field_ok_cb(void)
{
    const char *t = textarea_window_get();
    char        my[PARAM_TEXT_MAX];

    switch (field) {
        case ITEM_NAME:
            digi_qso_set_name(t);
            msg_update_text_fmt("#FFFFFF DX name: %s", digi_qso_name());
            break;

        case ITEM_QTH:
            digi_qso_set_qth(t);
            msg_update_text_fmt("#FFFFFF DX QTH: %s", digi_qso_qth());
            break;

        case ITEM_MYNAME:
            param_t_set(cfg.op_name(), t ? t : "");
            msg_update_text_fmt("#FFFFFF My name: %s",
                                param_t_get_into(cfg.op_name(), my, sizeof(my)));
            break;

        case ITEM_MYQTH:
            param_t_set(cfg.op_qth(), t ? t : "");
            msg_update_text_fmt("#FFFFFF My QTH: %s",
                                param_t_get_into(cfg.op_qth(), my, sizeof(my)));
            break;
    }
    field = -1;
    if (cur && cur->input_end) {
        cur->input_end();
    }
    changed();
    return true;
}

static bool field_cancel_cb(void)
{
    field = -1;
    if (cur && cur->input_end) {
        cur->input_end();
    }
    return true;
}

static void edit_field(int item)
{
    static char  text[PARAM_TEXT_MAX];
    const char  *label = "";

    if (cur == NULL || (cur->input_begin && !cur->input_begin())) {
        return;
    }
    text[0] = '\0';
    switch (item) {
        case ITEM_NAME:
            label = "DX name:";
            snprintf(text, sizeof(text), "%s", digi_qso_name());
            break;

        case ITEM_QTH:
            label = "DX QTH:";
            snprintf(text, sizeof(text), "%s", digi_qso_qth());
            break;

        case ITEM_MYNAME:
            label = "My name:";
            param_t_get_into(cfg.op_name(), text, sizeof(text));
            break;

        case ITEM_MYQTH:
            label = "My QTH:";
            param_t_get_into(cfg.op_qth(), text, sizeof(text));
            break;
    }
    field = item;
    textarea_window_open_w_label(field_ok_cb, field_cancel_cb, label);
    lv_textarea_set_max_length(textarea_window_text(), FIELD_MAX);
    if (text[0]) {
        textarea_window_set(text);
    }
}

/* ---- DX name / QTH: a word from the text, or typed ---------------------- */

static char words[DIGI_MENU_MAX][16];
static int  words_n;
static int  words_field;

static void word_pick(int item, void *ctx)
{
    (void)ctx;
    if (item == 0 || item > words_n) {
        edit_field(words_field);        /* "Type it..." */
        return;
    }
    if (words_field == ITEM_NAME) {
        digi_qso_set_name(words[item - 1]);
        msg_update_text_fmt("#FFFFFF DX name: %s", digi_qso_name());
    } else {
        digi_qso_set_qth(words[item - 1]);
        msg_update_text_fmt("#FFFFFF DX QTH: %s", digi_qso_qth());
    }
    changed();
}

/* The pick-up from the text (digi_qso_feed_rx) usually fills these in;
 * when it did not, the words received lately are offered, so a name
 * that went past is one press away, and typing is the last resort. */
static void pick_or_type(int item)
{
    const char *items[DIGI_MENU_MAX];

    words_field = item;
    words_n = digi_qso_recent_words(words, DIGI_MENU_MAX - 1);
    if (words_n == 0) {
        edit_field(item);
        return;
    }
    items[0] = "Type it...";
    for (int i = 0; i < words_n; i++) {
        items[i + 1] = words[i];
    }
    digi_menu_open(cur && cur->parent ? cur->parent() : lv_scr_act(),
                   item == ITEM_NAME ? "DX name: a word from the text" : "DX QTH: a word from the text",
                   items, words_n + 1, word_pick, NULL, NULL);
}

/* ---- log when the 73 has gone out ---------------------------------------- */

static bool pending_for(const char *call)
{
    return pending_timer != NULL && strcmp(pending.call, call) == 0;
}

static void pending_drop(bool say)
{
    if (pending_timer == NULL) {
        return;
    }
    lv_timer_del(pending_timer);
    pending_timer = NULL;
    if (say) {
        msg_update_text_fmt("#FF0000 TX stopped: %s not logged (Menu > QSO > Log QSO)",
                            pending.call);
    }
}

static void pending_log(void)
{
    if (pending_timer != NULL) {
        lv_timer_del(pending_timer);
        pending_timer = NULL;
    }
    if (digi_qso_logged() && strcmp(digi_qso_dx(), pending.call) == 0) {
        return;                         /* logged by hand meanwhile */
    }
    pending.end = time(NULL);
    digi_log_qso(&pending);
    changed();
}

static void pending_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (pending_tx_on && pending_tx_on()) {
        return;
    }
    pending_log();
}

/* ---- the list --------------------------------------------------------- */

static void menu_pick(int item, void *ctx)
{
    const digi_qso_menu_t *m = (const digi_qso_menu_t *)ctx;

    cur = m;
    switch (item) {
        case ITEM_LOG:
            digi_qso_menu_log(m);
            break;

        case ITEM_NAME:
        case ITEM_QTH:
            pick_or_type(item);
            break;

        case ITEM_MYNAME:
        case ITEM_MYQTH:
            edit_field(item);
            break;

        case ITEM_RSTR:
            digi_qso_change_rst_rcvd(1);
            changed();
            digi_qso_menu_open(m);      /* stays open for the next press, */
            digi_menu_select(ITEM_RSTR);   /* on this row, not on Log QSO */
            break;

        case ITEM_PICKUP:
            digi_qso_toggle_autograb();
            msg_update_text_fmt("#FFFFFF Call, name and QTH from the text: %s",
                                digi_qso_autograb() ? "on" : "off");
            break;

        case ITEM_AUTOLOG:
            param_i_set(cfg.digi_auto_log(), !param_i_get(cfg.digi_auto_log()));
            msg_update_text_fmt("#FFFFFF Log when 73 goes out: %s",
                                param_i_get(cfg.digi_auto_log()) ? "on" : "off");
            break;

        case ITEM_TEXT:
            digi_text_set_enabled(!digi_text_enabled());
            if (digi_text_enabled() && digi_text_path()[0]) {
                msg_update_text_fmt("#FFFFFF Saving the text to %s", digi_text_path());
            } else {
                msg_update_text_fmt("#FFFFFF Text not saved");
            }
            break;
    }
}

void digi_qso_menu_open(const digi_qso_menu_t *m)
{
    static char  text[ITEM_COUNT][ITEM_LEN];
    const char  *items[ITEM_COUNT];
    const char  *dx = digi_qso_dx();
    char         my[PARAM_TEXT_MAX];

    if (m == NULL) {
        return;
    }
    cur = m;
    if (dx[0] == '\0') {
        snprintf(text[ITEM_LOG], ITEM_LEN, "Log QSO  (no DX call yet)");
    } else {
        snprintf(text[ITEM_LOG], ITEM_LEN, "Log QSO%s  (%s %s/%s%s)",
                 digi_qso_logged() ? " again" : " now", dx, digi_qso_rst_sent(),
                 digi_qso_rst_rcvd(), pending_for(dx) ? ", after this TX" : "");
    }
    snprintf(text[ITEM_NAME], ITEM_LEN, "DX name...  (%s)",
             digi_qso_name()[0] ? digi_qso_name() : "none");
    snprintf(text[ITEM_QTH], ITEM_LEN, "DX QTH...  (%s)",
             digi_qso_qth()[0] ? digi_qso_qth() : "none");
    snprintf(text[ITEM_RSTR], ITEM_LEN, "RST received: %s  (press: next)", digi_qso_rst_rcvd());
    param_t_get_into(cfg.op_name(), my, sizeof(my));
    snprintf(text[ITEM_MYNAME], ITEM_LEN, "My name...  (%s)", my[0] ? my : "not set");
    param_t_get_into(cfg.op_qth(), my, sizeof(my));
    snprintf(text[ITEM_MYQTH], ITEM_LEN, "My QTH...  (%s)", my[0] ? my : "not set");
    snprintf(text[ITEM_PICKUP], ITEM_LEN, "Call, name, QTH from the text: %s",
             digi_qso_autograb() ? "on" : "off");
    snprintf(text[ITEM_AUTOLOG], ITEM_LEN, "Log when 73 goes out: %s",
             param_i_get(cfg.digi_auto_log()) ? "on" : "off");
    snprintf(text[ITEM_TEXT], ITEM_LEN, "Save the text to the card: %s",
             digi_text_enabled() ? "on" : "off");
    for (int i = 0; i < ITEM_COUNT; i++) {
        items[i] = text[i];
    }
    digi_menu_open(m->parent ? m->parent() : lv_scr_act(), "QSO", items, ITEM_COUNT, menu_pick,
                   (void *)m, NULL);
}

void digi_qso_menu_log(const digi_qso_menu_t *m)
{
    if (m == NULL) {
        return;
    }
    if (pending_for(digi_qso_dx())) {
        pending_drop(false);            /* logged now, not again after TX */
    }
    digi_log_current(m->mode, submode_now(m), freq_now(m));
    if (cur == m) {
        changed();
    }
}

void digi_qso_menu_sent(const digi_qso_menu_t *m, const char *macro_name)
{
    if (m == NULL || macro_name == NULL || strcasecmp(macro_name, "73") != 0) {
        return;
    }
    if (digi_qso_dx()[0] == '\0' || digi_qso_logged() || !param_i_get(cfg.digi_auto_log()) ||
        pending_for(digi_qso_dx())) {
        return;
    }
    if (pending_timer != NULL) {
        pending_log();                  /* an earlier 73, to another call, queued ahead */
    }
    digi_log_from_qso(&pending, m->mode, submode_now(m), freq_now(m));
    pending_tx_on = m->tx_on;
    if (pending_tx_on == NULL || !pending_tx_on()) {
        pending_log();                  /* nothing to wait for */
        return;
    }
    pending_timer = lv_timer_create(pending_timer_cb, PENDING_POLL_MS, NULL);
    msg_update_text_fmt("#FFFFFF %s: logging when the 73 has gone out", pending.call);
}

void digi_qso_menu_tx_stopped(void)
{
    pending_drop(true);
}

void digi_qso_menu_close(void)
{
    pending_drop(true);
    digi_menu_close();
    cur = NULL;
    field = -1;
}
