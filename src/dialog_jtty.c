/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY window
 *
 *  Laid out as the JS8 window (randal007/x6100-js8): a strip of waterfall
 *  at the top with the transmit offset in red (main knob) and the
 *  selected station in green, a TX bar under it, and the message list
 *  below, one row per transmission (UTC, SNR, audio Hz, text) growing
 *  frame by frame as the receiver decodes it.
 *
 *  Rows: red = sent by us, blue = to us, green = a CQ; white text while
 *  still arriving. The MFK selects a row: its station (the first call in
 *  it that is not ours) is the one Reply answers, and its frequency is
 *  marked in green.
 *
 *  One page of buttons: Next, Reply, Send..., Macros, Menu.
 *
 *  Next (also a press of the MFK) sends what the QSO needs now and says
 *  so on its label, as contest loggers' "enter sends message": CQ with
 *  nothing selected; to a station that called us its exchange, then TU
 *  (which logs it) once its exchange is in, or AGN? while it is not, and
 *  the exchange again when it asks AGN?; to a station calling CQ our
 *  call, then our exchange (which logs it) once its exchange is in.
 *  Macros lists WSJT-X's F1-F8 with what each would send now; Menu holds
 *  the rest (Log QSO, Serial..., Clear). Both are lists over the window,
 *  moved through with the MFK and picked by pressing it.
 *
 *  F1-F8 go out as their native atoms (%M our call, %H and %Q the
 *  selected station, %E "599" and the serial number). Reply opens the
 *  keyboard with the selected call (our offset moves to theirs), Send...
 *  any text (%M %H %Q %E expanded). The serial goes up after TU ends a
 *  QSO and before TU NOW starts the next one; it and our offset are kept
 *  on the card. With nothing selected, a station calling us (our call in
 *  its row, or a call heard near our offset) is selected by itself; CQ
 *  and TU clear the selection for the next caller. ESC closes a list,
 *  stops a transmission, or closes the window.
 *
 *  Log: TU logs the station, TU NOW the one before, once an exchange was
 *  sent to it; Log QSO logs the selected station at any time. QSOs go to
 *  the radio's log, to /mnt/jtty_log.adi with the serials, and over
 *  WSJT-X UDP (HamLogBridge). Stations already worked in JTTY on the band
 *  are shown grey.
 *
 *  JTTY has dedicated dial frequencies: the window moves to the nearest
 *  band's in USB-D and locks frequency, mode and band as FT8 does; the
 *  band keys step through them. Settings > Interface > Decoder text sets
 *  the list's font, also while the window is open.
 */

#include "dialog_jtty.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cfg/subject_api.h"
#include "dialog.h"
#include "digi_menu.h"
#include "dsp.h"
#include "events.h"
#include "ft8_udp.h"
#include "keyboard.h"
#include "lock_manager.h"
#include "main_screen.h"
#include "msg.h"
#include "radio.h"
#include "scheduler.h"
#include "spectrum.h"
#include "styles.h"
#include "textarea_window.h"
#include "waterfall.h"

#include "widgets/lv_finder.h"
#include "widgets/lv_waterfall.h"

#include "jtty/jtty_proto.h"
#include "jtty/jtty_rx.h"
#include "jtty/jtty_source.h"
#include "jtty_log.h"
#include "jtty_tx.h"

#define WIDTH           771
#define WF_HEIGHT       80
#define BAR_HEIGHT      30
#define LIST_TOP        (13 + WF_HEIGHT + BAR_HEIGHT + 2)
#define LIST_HEIGHT     (DIALOG_HEIGHT - LIST_TOP - 6)
#define WF_BINS         512
#define MAX_ROWS        60
#define MARKER_STEP_HZ  10
/* WSJT-X looks for JTTY only between these (tone 0) and around its own
 * Rx frequency, so the main knob keeps our offset here. */
#define WSJTX_LOW_HZ    1200
#define WSJTX_HIGH_HZ   1800
#define TX_ID_FLAG      0x80000000u
#define AUTO_SELECT_HZ  150             /* callers near our offset */

/* Dial frequencies, the WSJT-X 3.3 defaults. */
static const struct {
    const char *label;
    uint32_t    dial_hz;
} bands[] = {
    {"160m", 1839000},  {"80m", 3572000},   {"40m", 7078000},
    {"30m", 10140000},  {"20m", 14090000},  {"17m", 18104000},
    {"15m", 21090000},  {"12m", 24920000},  {"10m", 28090000},
    {"6m", 50160000},
};
#define N_BANDS ((int)(sizeof(bands) / sizeof(bands[0])))

typedef struct {
    uint32_t id;                        /* receiver's id, or TX_ID_FLAG | ours */
    time_t   when;                      /* UTC of its first frame */
    float    freq;
    float    snr;
    bool     done;
    bool     tx;
    bool     dupe;                      /* its station worked on this band */
    char     text[JTTY_MSG_MAX + 24];
} row_t;

/* A QSO in progress: what was sent and received, for the log. */
typedef struct {
    char call[16];
    char sent[24];                      /* "599 007" once sent */
    char rcvd[24];                      /* "599 012" once received */
    bool called;                        /* we answered its CQ (F4) */
    bool agn;                           /* it asked AGN? since our exchange */
} qso_t;

#define MAX_QSOS 8

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff, uint16_t dt);
static void band_cb(lv_event_t *e);
static void clear_cb(button_data_t *b);
static void log_cb(button_data_t *b);
static void macro_cb(button_data_t *b);
static void reply_cb(button_data_t *b);
static void send_cb(button_data_t *b);
static void serial_cb(button_data_t *b);
static void next_cb(button_data_t *b);
static void macros_cb(button_data_t *b);
static void menu_cb(button_data_t *b);
static const char *next_label(void);

static lv_obj_t   *waterfall;
static lv_obj_t   *finder;
static lv_obj_t   *status_label;
static lv_obj_t   *tx_bar;
static lv_obj_t   *table;
static lv_timer_t *status_timer;
static Observer   *font_obs;

static row_t       rows[MAX_ROWS];
static int         nrows;
static uint32_t    sel_id;              /* selected row's message, 0 = none */
static char        sel_call[16];
static int         cur_band = -1;
static int32_t     filter_low;
static int32_t     filter_high;
static int32_t     marker_hz = 1500;
static bool        keyboard_open;
static bool        keyboard_serial;     /* the keyboard edits the serial */
static uint32_t    total;
static uint32_t    serial = 1;          /* %E, cfg.jtty.serial() */
static qso_t       qsos[MAX_QSOS];
static int         nqsos;
static char        last_partner[16];    /* last station we sent an exchange to */

static jtty_rx_t  *rx;
static uint32_t    audio_sub = AUDIO_SUB_INVALID;
static volatile bool transmitting;

/* ---- Buttons ----------------------------------------------------------- */

static buttons_page_t btn_page_1;

static button_data_t button_next = {
    .type = BTN_TEXT_FN, .label_fn = next_label, .press = next_cb,
};
static button_data_t button_reply = {
    .type = BTN_TEXT, .label = "Reply", .press = reply_cb,
};
static button_data_t button_send = {
    .type = BTN_TEXT, .label = "Send...", .press = send_cb,
};
static button_data_t button_macros = {
    .type = BTN_TEXT, .label = "Macros", .press = macros_cb,
};
static button_data_t button_menu = {
    .type = BTN_TEXT, .label = "Menu", .press = menu_cb,
};

static buttons_page_t btn_page_1 = {
    {&button_next, &button_reply, &button_send, &button_macros, &button_menu}
};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb,
    .btn_page = &btn_page_1,
};

dialog_t *dialog_jtty = &dialog;

/* ---- Helpers ----------------------------------------------------------- */

static void my_call(char *out, size_t size)
{
    param_t_get_into(cfg.callsign(), out, size);
    for (char *p = out; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 'a' + 'A');
        }
    }
}

/* Is `word` a whole word of `text`? */
static bool has_word(const char *text, const char *word)
{
    size_t      n = strlen(word);
    const char *p = text;

    if (n == 0) {
        return false;
    }
    while ((p = strstr(p, word)) != NULL) {
        bool start = (p == text) || p[-1] == ' ';
        bool end = p[n] == '\0' || p[n] == ' ';

        if (start && end) {
            return true;
        }
        p += n;
    }
    return false;
}

/* The station of a row: the first standard call in it that is not ours. */
static void station_of(const char *text, char *out, size_t size)
{
    char mine[16];
    char buf[JTTY_MSG_MAX + 24];
    char *save = NULL;

    out[0] = '\0';
    my_call(mine, sizeof(mine));
    snprintf(buf, sizeof(buf), "%s", text);
    for (char *t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        uint32_t n28;
        char     back[16];

        (void)n28;
        (void)back;
        if (strcmp(t, mine) != 0 && jtty_call_standard(t)) {
            snprintf(out, size, "%s", t);
            return;
        }
    }
}

/* The station of row r: from its own text, else from the newest earlier
 * row heard on the same frequency (an exchange carries no call). */
static void station_of_row(int r, char *out, size_t size)
{
    station_of(rows[r].text, out, size);
    for (int i = r - 1; i >= 0 && out[0] == '\0'; i--) {
        if (!rows[i].tx && fabsf(rows[i].freq - rows[r].freq) < 20.0f) {
            station_of(rows[i].text, out, size);
        }
    }
}

static int row_index(uint32_t id)
{
    for (int i = 0; i < nrows; i++) {
        if (rows[i].id == id) {
            return i;
        }
    }
    return -1;
}

static uint64_t dial_hz(void)
{
    return (uint64_t)cparam_i_get(cfg.cur.fg_freq());
}

static void set_serial(uint32_t v)
{
    if (v < 1 || v > JTTY_SERIAL_MAX) {
        v = 1;
    }
    serial = v;
    param_i_set(cfg.jtty.serial(), (int32_t)v);
}

/* ---- QSOs and the log -------------------------------------------------- */

static qso_t *find_qso(const char *call, bool create)
{
    if (call == NULL || call[0] == '\0') {
        return NULL;
    }
    for (int i = 0; i < nqsos; i++) {
        if (strcmp(qsos[i].call, call) == 0) {
            return &qsos[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (nqsos == MAX_QSOS) {
        memmove(qsos, qsos + 1, sizeof(qso_t) * (MAX_QSOS - 1));
        nqsos--;
    }
    memset(&qsos[nqsos], 0, sizeof(qso_t));
    snprintf(qsos[nqsos].call, sizeof(qsos[nqsos].call), "%s", call);
    return &qsos[nqsos++];
}

static void drop_qso(const char *call)
{
    for (int i = 0; i < nqsos; i++) {
        if (strcmp(qsos[i].call, call) == 0) {
            memmove(qsos + i, qsos + i + 1, sizeof(qso_t) * (size_t)(nqsos - i - 1));
            nqsos--;
            return;
        }
    }
}

/* "... 599 012 ..." -> "599 012": an exchange in a received row. */
static bool exchange_in(const char *text, char *out, size_t size)
{
    const char *p = text;

    while ((p = strstr(p, "599 ")) != NULL) {
        if (p == text || p[-1] == ' ') {
            const char *q = p + 4;
            size_t      n = strcspn(q, " ");

            if (n > 0 && n < size - 4) {
                snprintf(out, size, "599 %.*s", (int)n, q);
                return true;
            }
        }
        p += 4;
    }
    return false;
}

static void refresh_dupes(void)
{
    for (int i = 0; i < nrows; i++) {
        char call[16];

        if (!rows[i].tx) {
            station_of(rows[i].text, call, sizeof(call));
            rows[i].dupe = call[0] && jtty_log_worked(call, dial_hz());
        }
    }
}

static void redraw(void);

/* Logs `call`; `need_exchange`: only once we sent it an exchange. */
static bool log_qso(const char *call, bool need_exchange)
{
    qso_t     *q = find_qso(call, false);
    jtty_qso_t l;

    if (call == NULL || call[0] == '\0' || (need_exchange && (q == NULL || q->sent[0] == '\0'))) {
        return false;
    }
    memset(&l, 0, sizeof(l));
    snprintf(l.call, sizeof(l.call), "%s", call);
    l.when = time(NULL);
    l.dial_hz = dial_hz();
    if (q != NULL) {
        snprintf(l.sent, sizeof(l.sent), "%s", q->sent);
        snprintf(l.rcvd, sizeof(l.rcvd), "%s", q->rcvd);
    }
    if (!jtty_log_save(&l)) {
        msg_update_text_fmt("Not logged: the radio's log refused %s (callsign set in Settings?)", call);
        return false;
    }
    msg_update_text_fmt("Logged %s  %s / %s", l.call, l.sent[0] ? l.sent : "-",
                        l.rcvd[0] ? l.rcvd : "-");
    drop_qso(call);
    if (strcmp(last_partner, call) == 0) {
        last_partner[0] = '\0';
    }
    refresh_dupes();
    redraw();
    return true;
}

static void log_cb(button_data_t *b)
{
    const char *call = sel_call[0] ? sel_call : last_partner;

    (void)b;
    if (call[0] == '\0') {
        msg_update_text_fmt("JTTY: select a station first (MFK)");
        return;
    }
    log_qso(call, false);
}

/* ---- TX bar and status ----------------------------------------------- */

static jtty_tx_status_t tx_now;         /* UI thread copy */

static void update_tx_bar(void)
{
    char buf[160];

    if (tx_bar == NULL) {
        return;
    }
    if (transmitting && tx_now.frames > 0) {
        snprintf(buf, sizeof(buf), "TX %d Hz  sending %d/%d  %s", (int)tx_now.freq,
                 tx_now.frame + 1, tx_now.frames, tx_now.text);
        lv_obj_set_style_bg_color(tx_bar, lv_color_hex(0xA00000), 0);
    } else {
        const char *hint = (marker_hz < WSJTX_LOW_HZ || marker_hz > WSJTX_HIGH_HZ)
                               ? "  (WSJT-X: click on it)" : "";

        if (sel_call[0] || sel_id) {
            bool dupe = sel_call[0] && jtty_log_worked(sel_call, dial_hz());

            snprintf(buf, sizeof(buf), "TX %d Hz%s  nr %03u  selected: %s%s", (int)marker_hz, hint,
                     (unsigned)serial, sel_call[0] ? sel_call : "no call", dupe ? " (dupe)" : "");
        } else {
            snprintf(buf, sizeof(buf), "TX %d Hz%s  nr %03u", (int)marker_hz, hint,
                     (unsigned)serial);
        }
        lv_obj_set_style_bg_color(tx_bar, lv_color_hex(0x202020), 0);
    }
    lv_label_set_text(tx_bar, buf);
    buttons_refresh(&button_next);
}

static void status_timer_cb(lv_timer_t *t)
{
    time_t    now = time(NULL);
    struct tm tm;
    char      buf[64];

    (void)t;
    if (now % 15 == 0) {
        ft8_udp_send_heartbeat();       /* loggers find us (no-op when UDP is off) */
    }
    gmtime_r(&now, &tm);
    snprintf(buf, sizeof(buf), "JTTY %s  %02d:%02d:%02dZ  total %u",
             cur_band >= 0 ? bands[cur_band].label : "", tm.tm_hour, tm.tm_min, tm.tm_sec,
             (unsigned)total);
    lv_label_set_text(status_label, buf);
}

/* ---- Message list ------------------------------------------------------ */

static void select_row(int r)
{
    lv_table_t *t = (lv_table_t *)table;

    if (r < 0 || r >= nrows) {
        sel_id = 0;
        sel_call[0] = '\0';
        lv_finder_clear_cursor(finder);
    } else {
        sel_id = rows[r].id;
        station_of_row(r, sel_call, sizeof(sel_call));
        lv_finder_set_cursor(finder, (int16_t)lroundf(rows[r].freq));
        t->row_act = (uint16_t)r;
        t->col_act = 0;
    }
    lv_obj_invalidate(finder);
    update_tx_bar();
}

static void redraw(void)
{
    int sel;

    if (table == NULL) {
        return;
    }
    lv_table_set_row_cnt(table, (uint16_t)(nrows > 0 ? nrows : 1));
    if (nrows == 0) {
        lv_table_set_cell_value(table, 0, 0, "");
    }
    for (int i = 0; i < nrows; i++) {
        const row_t *r = &rows[i];
        struct tm    tm;
        char         buf[160];

        gmtime_r(&r->when, &tm);
        if (r->tx) {
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d  TX  %4.0f  %s", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, r->freq, r->text);
        } else {
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d %+3.0f  %4.0f  %s", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, r->snr, r->freq, r->text);
        }
        lv_table_set_cell_value(table, (uint16_t)i, 0, buf);
    }

    /* The selection stays on its message; with none, or on the newest,
     * the list follows the newest. */
    sel = sel_id ? row_index(sel_id) : -1;
    if (sel >= 0) {
        ((lv_table_t *)table)->row_act = (uint16_t)sel;
        ((lv_table_t *)table)->col_act = 0;
    }
    if (sel < 0 || sel == nrows - 2 || sel == nrows - 1) {
        lv_obj_scroll_to_y(table, LV_COORD_MAX, LV_ANIM_OFF);
    }
    lv_obj_invalidate(table);
}

static void add_or_update(const row_t *m)
{
    int i = row_index(m->id);

    if (i < 0) {
        if (nrows == MAX_ROWS) {
            memmove(rows, rows + 1, sizeof(row_t) * (MAX_ROWS - 1));
            nrows--;
        }
        i = nrows++;
        if (!m->tx) {
            total++;
        }
    }
    rows[i] = *m;
    if (!m->tx) {
        char call[16];

        station_of(m->text, call, sizeof(call));
        rows[i].dupe = call[0] && jtty_log_worked(call, dial_hz());
    }
    redraw();
}

/* Row colours, as the JS8 window: ours red, to us blue, CQ green. */
static void draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    const row_t            *r;
    char                    mine[16];
    lv_color_t              bg;
    bool                    has_bg = true;

    if (dsc->part != LV_PART_ITEMS) {
        return;
    }
    if ((int)dsc->id >= nrows) {
        return;
    }
    r = &rows[dsc->id];
    my_call(mine, sizeof(mine));

    if (r->tx) {
        bg = lv_color_hex(0x8B0000);
    } else if (mine[0] && has_word(r->text, mine)) {
        bg = lv_color_hex(0x0000A0);
    } else if (has_word(r->text, "CQ")) {
        bg = lv_color_hex(0x2E5A00);
    } else {
        bg = lv_color_hex(0x000000);
        has_bg = false;
    }
    if (r->id == sel_id) {
        bg = lv_color_lighten(has_bg ? bg : lv_color_hex(0x303030), 40);
        has_bg = true;
    }
    if (dsc->rect_dsc != NULL && has_bg) {
        dsc->rect_dsc->bg_color = bg;
        dsc->rect_dsc->bg_opa = LV_OPA_70;
    }
    if (dsc->label_dsc != NULL && !r->done) {
        dsc->label_dsc->color = lv_color_white();
    } else if (dsc->label_dsc != NULL && r->dupe) {
        dsc->label_dsc->color = lv_color_hex(0x707070);   /* worked before */
    }
}

static void table_changed_cb(lv_event_t *e)
{
    uint16_t row;
    uint16_t col;

    (void)e;
    lv_table_get_selected_cell(table, &row, &col);
    if (row < nrows) {
        select_row(row);
        lv_obj_invalidate(table);
    }
}

/* UI thread: a received message changed. */
/*
 * With no station selected, a station that calls us is selected by
 * itself: a row with our call in it, or one with a call heard near our
 * offset (a caller answering our CQ sends just its own call). A choice
 * made with the MFK is never overridden. F1 and F3 (CQ) clear the
 * selection so the next caller is picked up.
 */
static void auto_select(const row_t *m)
{
    char mine[16];
    char call[16];
    bool near;

    if (sel_id != 0 || m->tx) {
        return;
    }
    station_of(m->text, call, sizeof(call));
    if (call[0] == '\0') {
        return;
    }
    my_call(mine, sizeof(mine));
    near = fabsf(m->freq - (float)marker_hz) <= AUTO_SELECT_HZ;
    if ((mine[0] && has_word(m->text, mine)) || near) {
        int r = row_index(m->id);

        if (r >= 0) {
            select_row(r);
            redraw();
        }
    }
}

/* An exchange received from a station we are working, for the log. */
static void note_exchange(const row_t *m)
{
    char   exch[24];
    char   call[16];
    int    r = row_index(m->id);
    qso_t *q;

    if (m->tx || r < 0) {
        return;
    }
    station_of_row(r, call, sizeof(call));
    q = find_qso(call, false);
    if (q == NULL) {
        return;
    }
    if (m->done && has_word(m->text, "AGN?") && q->sent[0]) {
        q->agn = true;                  /* our exchange again */
    }
    if (exchange_in(m->text, exch, sizeof(exch))) {
        snprintf(q->rcvd, sizeof(q->rcvd), "%s", exch);
    }
    update_tx_bar();
}

static void msg_ui_cb(void *arg)
{
    if (table != NULL) {
        add_or_update((const row_t *)arg);
        note_exchange((const row_t *)arg);
        auto_select((const row_t *)arg);
    }
}

/* Receiver thread. */
static void rx_cb(const jtty_msg_t *msg, void *ctx)
{
    row_t    r;
    uint64_t now = jtty_rx_samples(rx);
    double   ago = (now > msg->start) ? (double)(now - msg->start) / JTTY_RATE : 0.0;

    (void)ctx;
    memset(&r, 0, sizeof(r));
    r.id = msg->id;
    r.when = time(NULL) - (time_t)lround(ago);
    r.freq = msg->freq;
    r.snr = msg->snr;
    r.done = msg->done;
    snprintf(r.text, sizeof(r.text), "%s", msg->text);
    scheduler_put(msg_ui_cb, &r, sizeof(r));
}

/* ---- Transmit ---------------------------------------------------------- */

/* UI thread: progress of our transmission. */
static void tx_ui_cb(void *arg)
{
    const jtty_tx_status_t *st = arg;
    int                     i;

    if (st->idle) {
        /* unkeyed: messages dropped from the queue by ESC never went out
         * (one queued since, or already being sent, is still on its way) */
        transmitting = jtty_tx_busy();
        if (table != NULL) {
            for (i = 0; i < nrows; i++) {
                if (rows[i].tx && !rows[i].done && !jtty_tx_pending(rows[i].id & ~TX_ID_FLAG)) {
                    size_t len = strlen(rows[i].text);

                    rows[i].done = true;
                    snprintf(rows[i].text + len, sizeof(rows[i].text) - len, " (not sent)");
                }
            }
            redraw();
        }
        if (!transmitting && rx != NULL) {
            jtty_rx_reset(rx);          /* what was heard meanwhile was ourselves */
        }
        update_tx_bar();
        return;
    }

    tx_now = *st;
    transmitting = true;
    if (table == NULL) {
        return;
    }
    i = row_index(TX_ID_FLAG | st->id);
    if (i >= 0) {
        rows[i].done = st->done;
        if (st->stopped) {
            size_t len = strlen(rows[i].text);

            snprintf(rows[i].text + len, sizeof(rows[i].text) - len, " (stopped %d/%d)",
                     st->frame + 1, st->frames);
        }
        redraw();
    }
    update_tx_bar();
}

/* Transmitter thread. */
static void tx_cb(const jtty_tx_status_t *st, void *ctx)
{
    (void)ctx;
    if (!st->idle) {
        transmitting = true;
    }
    scheduler_put(tx_ui_cb, (void *)st, sizeof(*st));
}

/* A row for what we queued, white until it is on the air. */
static void add_tx_row(uint32_t id, const char *text)
{
    row_t r;

    memset(&r, 0, sizeof(r));
    r.id = TX_ID_FLAG | id;
    r.when = time(NULL);
    r.freq = (float)marker_hz;
    r.tx = true;
    snprintf(r.text, sizeof(r.text), "%s", text);
    add_or_update(&r);
    transmitting = true;
    update_tx_bar();
}

/* %M %H %Q %E in typed text, as WSJT-X expands its edited macros */
static void expand(const char *in, char *out, size_t size)
{
    char   mine[16];
    char   exch[16];
    size_t o = 0;

    my_call(mine, sizeof(mine));
    snprintf(exch, sizeof(exch), "599 %03u", (unsigned)serial);
    for (const char *p = in; *p && o + 1 < size; p++) {
        const char *sub = NULL;

        if (p[0] == '%' && p[1]) {
            switch (p[1] & ~0x20) {
                case 'M': sub = mine; break;
                case 'H':
                case 'Q': sub = sel_call; break;
                case 'E': sub = exch; break;
            }
        }
        if (sub != NULL) {
            o += (size_t)snprintf(out + o, size - o, "%s", sub);
            if (o >= size) {
                o = size - 1;
            }
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static void queue_text(const char *text)
{
    char     full[JTTY_MSG_MAX * 2];
    char     norm[JTTY_MSG_MAX + 1];
    uint32_t id;

    expand(text, full, sizeof(full));
    id = jtty_tx_queue(full, (float)marker_hz, norm, sizeof(norm));
    if (id == 0) {
        if (norm[0]) {
            msg_update_text_fmt("JTTY: queue full");
        }
        return;
    }
    add_tx_row(id, norm);
}

/* Our offset onto the selected station, as the JS8 window's Reply. */
static void tune_to_selected(void)
{
    int r = sel_id ? row_index(sel_id) : -1;

    if (r >= 0 && !rows[r].tx) {
        marker_hz = (int32_t)lroundf(rows[r].freq);
        lv_finder_set_value(finder, (int16_t)marker_hz);
        lv_obj_invalidate(finder);
    }
}

/* F1-F8: WSJT-X's templates, sent as native atoms. */
static void macro_cb(button_data_t *b)
{
    int            key = (int)b->ctrl;
    char           mine[16];
    char           norm[JTTY_MSG_MAX + 1];
    jtty_payload_t pay[JTTY_MAX_FRAMES];
    uint32_t       use_serial = serial;
    int            frames;
    uint32_t       id;

    my_call(mine, sizeof(mine));
    if (strstr(jtty_macro_template(key), "%M") && mine[0] == '\0') {
        msg_update_text_fmt("Set your callsign first (APP > Callsign)");
        return;
    }
    if ((strstr(jtty_macro_template(key), "%H") || strstr(jtty_macro_template(key), "%Q")) &&
        sel_call[0] == '\0') {
        msg_update_text_fmt(sel_id ? "JTTY: no other station's call in that row"
                                   : "JTTY: select a station first (MFK)");
        return;
    }
    if (key == 6) {
        use_serial = serial + 1;        /* the QSO before is over */
    }
    frames = jtty_pack_macro(key, mine, sel_call, use_serial, pay, norm, sizeof(norm));
    if (frames == 0) {
        msg_update_text_fmt("JTTY: F%d needs standard calls (%s)", key, jtty_macro_template(key));
        return;
    }
    if (key == 4) {
        tune_to_selected();             /* answering a CQ */
    }
    id = jtty_tx_queue_frames(pay, frames, norm, (float)marker_hz);
    if (id == 0) {
        msg_update_text_fmt("JTTY: queue full");
        return;
    }
    if (key == 4 && sel_call[0]) {
        find_qso(sel_call, true)->called = true;    /* answering its CQ */
    }
    /* the exchange sent, for the log */
    if ((key == 2 || key == 6 || key == 8) && sel_call[0]) {
        qso_t *q;
        char   prev[16];

        snprintf(prev, sizeof(prev), "%s", last_partner);
        q = find_qso(sel_call, true);
        snprintf(q->sent, sizeof(q->sent), "599 %03u", (unsigned)use_serial);
        q->agn = false;
        if (key == 6 && strcmp(prev, sel_call) != 0) {
            log_qso(prev, true);        /* TU NOW: the one before is done */
        }
        snprintf(last_partner, sizeof(last_partner), "%s", sel_call);
        if (key == 8 && q->called && q->rcvd[0]) {
            /* answering a CQ: both exchanges done; the serial moves on
             * only with a QSO in the log */
            if (log_qso(sel_call, true)) {
                set_serial(serial + 1);
            }
            select_row(-1);
        }
    }
    bool tu_logged = key == 3 && log_qso(sel_call, true);   /* TU: done */

    if (key == 6) {
        set_serial(use_serial);
    } else if (tu_logged) {
        set_serial(serial + 1);         /* TU, the next QSO gets the next number */
    }
    if (key == 1 || key == 3) {
        select_row(-1);                 /* calling CQ: the next caller is selected */
    }
    add_tx_row(id, norm);
}

/* ---- Next, Macros, Menu ------------------------------------------------- */

/* What Next sends now: a function key, 0 for nothing. */
static int next_key(void)
{
    int    r = sel_id ? row_index(sel_id) : -1;
    qso_t *q = find_qso(sel_call, false);

    if (sel_call[0] == '\0') {
        return 1;                                       /* CQ */
    }
    if (q != NULL && q->called) {                       /* we answered its CQ */
        return q->rcvd[0] ? 8 : 4;                      /* our exchange / call again */
    }
    if (q == NULL || q->sent[0] == '\0') {
        if (r >= 0 && has_word(rows[r].text, "CQ")) {
            return 4;                                   /* answer its CQ */
        }
        return 2;                                       /* it called us: exchange */
    }
    if (q->agn) {
        return 2;                                       /* it asked again */
    }
    return q->rcvd[0] ? 3 : 7;                          /* TU and log / AGN? */
}

static const char *next_label(void)
{
    static char buf[40];
    static const char *const what[9] = {"", "CQ", "Exch", "TU", "Call", "", "", "AGN?", "Exch"};
    int key = next_key();

    if (key == 1 || sel_call[0] == '\0') {
        snprintf(buf, sizeof(buf), "Next:\nCQ");
    } else {
        snprintf(buf, sizeof(buf), "%s\n%s", what[key], sel_call);
    }
    return buf;
}

static void send_key(int key)
{
    button_data_t b = {.ctrl = key};

    macro_cb(&b);
    update_tx_bar();
}

static void next_cb(button_data_t *b)
{
    (void)b;
    digi_menu_close();
    send_key(next_key());
}

static void macros_pick(int item, void *ctx)
{
    (void)ctx;
    send_key(item + 1);
}

static void macros_cb(button_data_t *b)
{
    static char  text[JTTY_MACROS][JTTY_MSG_MAX + 8];
    const char  *items[JTTY_MACROS];
    char         mine[16];

    (void)b;
    if (digi_menu_is_open()) {
        digi_menu_close();
        return;
    }
    my_call(mine, sizeof(mine));
    for (int k = 1; k <= JTTY_MACROS; k++) {
        jtty_payload_t pay[JTTY_MAX_FRAMES];
        char           norm[JTTY_MSG_MAX + 1];
        uint32_t       nr = (k == 6) ? serial + 1 : serial;

        if (jtty_pack_macro(k, mine, sel_call, nr, pay, norm, sizeof(norm)) > 0) {
            snprintf(text[k - 1], sizeof(text[k - 1]), "F%d  %s", k, norm);
        } else {
            snprintf(text[k - 1], sizeof(text[k - 1]), "F%d  %s", k, jtty_macro_template(k));
        }
        items[k - 1] = text[k - 1];
    }
    digi_menu_open(dialog.obj, "Macros: MFK to choose, press to send", items, JTTY_MACROS,
                   macros_pick, NULL, table);
}

static void menu_pick(int item, void *ctx)
{
    (void)ctx;
    switch (item) {
        case 0: log_cb(NULL); break;
        case 1: serial_cb(NULL); break;
        case 2: clear_cb(NULL); break;
        default: break;
    }
    update_tx_bar();
}

static void menu_cb(button_data_t *b)
{
    static char log_item[40];
    static char serial_item[40];
    const char *call = sel_call[0] ? sel_call : last_partner;
    const char *items[4];

    (void)b;
    if (digi_menu_is_open()) {
        digi_menu_close();
        return;
    }
    snprintf(log_item, sizeof(log_item), "Log QSO%s%s", call[0] ? "  " : "", call);
    snprintf(serial_item, sizeof(serial_item), "Serial...  (nr %03u)", (unsigned)serial);
    items[0] = log_item;
    items[1] = serial_item;
    items[2] = "Clear the list";
    items[3] = "Close";
    digi_menu_open(dialog.obj, "Menu", items, 4, menu_pick, NULL, table);
}

/* ---- Keyboard ---------------------------------------------------------- */

static void keyboard_close(void)
{
    textarea_window_close();
    keyboard_open = false;
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);
}

static bool keyboard_ok_cb(void)
{
    char text[JTTY_MSG_MAX * 2];

    snprintf(text, sizeof(text), "%s", textarea_window_get());
    keyboard_close();
    if (keyboard_serial) {
        char *end;
        long  v = strtol(text, &end, 10);

        keyboard_serial = false;
        if (end != text && v >= 1 && v <= JTTY_SERIAL_MAX) {
            set_serial((uint32_t)v);
        } else {
            msg_update_text_fmt("JTTY: serial 1 .. %d", JTTY_SERIAL_MAX);
        }
        update_tx_bar();
        return true;
    }
    queue_text(text);
    return true;
}

static bool keyboard_cancel_cb(void)
{
    keyboard_close();
    keyboard_serial = false;
    return true;
}

static void keyboard(const char *prefill)
{
    lv_obj_t *ta;

    if (keyboard_open) {
        return;
    }
    keyboard_open = true;
    lv_group_remove_obj(table);
    textarea_window_open(keyboard_ok_cb, keyboard_cancel_cb);
    ta = textarea_window_text();
    lv_textarea_set_max_length(ta, JTTY_MSG_MAX);
    if (prefill != NULL && prefill[0]) {
        textarea_window_set(prefill);
    }
}

static void reply_cb(button_data_t *b)
{
    char prefill[24];

    (void)b;
    tune_to_selected();                 /* answer on their frequency */
    snprintf(prefill, sizeof(prefill), "%s%s", sel_call, sel_call[0] ? " " : "");
    keyboard(prefill);
    update_tx_bar();
}

static void send_cb(button_data_t *b)
{
    (void)b;
    keyboard(NULL);
}

static void serial_cb(button_data_t *b)
{
    char buf[16];

    (void)b;
    if (keyboard_open) {
        return;
    }
    snprintf(buf, sizeof(buf), "%u", (unsigned)serial);
    keyboard(buf);
    keyboard_serial = true;
}

/* ---- Waterfall --------------------------------------------------------- */

/* The line travels inside the scheduler's copy: one the scheduler drops
 * (queue full) leaves nothing behind to free. */
typedef struct {
    float db[WF_BINS];
    int   n;
} wf_line_t;

static void wf_ui_cb(void *arg)
{
    wf_line_t *l = arg;

    if (waterfall != NULL) {
        lv_waterfall_add_data(waterfall, l->db, (uint16_t)l->n);
    }
}

/* Receiver thread. */
static void psd_cb(const float *db, int nbins, void *ctx)
{
    wf_line_t l;

    (void)ctx;
    l.n = nbins < WF_BINS ? nbins : WF_BINS;
    memcpy(l.db, db, sizeof(float) * (size_t)l.n);
    scheduler_put(wf_ui_cb, &l, sizeof(l));
}

/* DSP thread: 12 kHz audio. Our own transmission is not decoded. */
static void audio_cb(size_t n, float *samples)
{
    if (rx != NULL && !transmitting) {
        jtty_rx_feed(rx, samples, (int)n);
    }
}

/* The waterfall, the markers and the decoder follow the radio's filter. */
static void apply_filter(void)
{
    filter_low = cparam_i_get(cfg.filter.low());
    filter_high = cparam_i_get(cfg.filter.high());
    if (filter_high - filter_low < 400) {
        filter_low = 200;
        filter_high = 2800;
    }
    lv_finder_set_range(finder, (int16_t)filter_low, (int16_t)filter_high);
    if (marker_hz < filter_low) {
        marker_hz = filter_low;
    }
    if (marker_hz > filter_high - (int32_t)JTTY_BW_HZ) {
        marker_hz = filter_high - (int32_t)JTTY_BW_HZ;
    }
    lv_finder_set_value(finder, (int16_t)marker_hz);    /* left edge = tone 0 */
    lv_obj_invalidate(finder);

    if (rx != NULL) {
        jtty_rx_set_band(rx, (float)filter_low, (float)filter_high - JTTY_BW_HZ);
        jtty_rx_set_psd(rx, psd_cb, NULL, (float)filter_low, (float)filter_high, WF_BINS);
    }
    update_tx_bar();
}

/* ---- Font (Settings > Interface > Decoder text) ------------------------ */

static void apply_font(void)
{
    bool compact = param_i_get(cfg.ui.digi_compact()) != 0;

    lv_obj_set_style_text_font(table, compact ? &sony_28 : &sony_36, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, compact ? 1 : 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, compact ? 1 : 3, LV_PART_ITEMS);
    redraw();
}

static void font_cb(Subject *subj, void *user_data)
{
    (void)subj;
    (void)user_data;
    if (table != NULL) {
        apply_font();
    }
}

/* ---- Bands ------------------------------------------------------------- */

static void load_band(int dir)
{
    int idx;

    if (dir == 0 || cur_band < 0) {
        uint32_t freq = (uint32_t)cparam_i_get(cfg.cur.fg_freq());
        int64_t  best = -1;

        idx = 0;
        for (int i = 0; i < N_BANDS; i++) {
            int64_t d = llabs((int64_t)freq - (int64_t)bands[i].dial_hz);

            if (best < 0 || d < best) {
                best = d;
                idx = i;
            }
        }
    } else {
        idx = (cur_band + dir + N_BANDS) % N_BANDS;
    }
    cur_band = idx;

    /* frequency first, then mode, as cfg_digital_load() does */
    cparam_i_set(cfg.cur.fg_freq(), (int32_t)bands[idx].dial_hz);
    cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);

    msg_update_text_fmt("JTTY %s  %u.%03u MHz", bands[idx].label,
                        (unsigned)(bands[idx].dial_hz / 1000000u),
                        (unsigned)((bands[idx].dial_hz / 1000u) % 1000u));
}

static void change_band(int dir)
{
    if (transmitting) {
        msg_update_text_fmt("JTTY: stop transmitting first (ESC)");
        return;
    }
    load_band(dir);
    if (rx != NULL) {
        jtty_rx_reset(rx);
    }
    nrows = 0;
    sel_id = 0;
    sel_call[0] = '\0';
    redraw();
    lv_waterfall_clear_data(waterfall);
    apply_filter();
}

static void band_cb(lv_event_t *e)
{
    change_band(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1);
}

static void clear_cb(button_data_t *b)
{
    (void)b;
    nrows = 0;
    select_row(-1);
    redraw();
}

/* Main knob: our transmit offset. */
static void rotary_cb(int32_t diff, uint16_t dt)
{
    /* The knob keeps to WSJT-X's 1200-1800 Hz, but F4 and Reply may have
     * put the marker outside it: from there a click moves one step, it
     * doesn't jump to the edge (the limit then is where the marker is). */
    int low = marker_hz < WSJTX_LOW_HZ ? marker_hz : WSJTX_LOW_HZ;
    int high = marker_hz > WSJTX_HIGH_HZ ? marker_hz : WSJTX_HIGH_HZ;

    marker_hz += diff * MARKER_STEP_HZ;
    if (marker_hz < low) {
        marker_hz = low;
    }
    if (marker_hz > high) {
        marker_hz = high;
    }
    apply_filter();             /* clamps to the filter and redraws it */
}

/* ---- Window ------------------------------------------------------------ */

static void construct_cb(lv_obj_t *parent)
{
    dialog.obj = dialog_init(parent);

    /* The window covers the main screen, whose spectrum and waterfall
     * would be computed for nothing. */
    waterfall_set_enabled(false);
    spectrum_set_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);


    /* Waterfall strip: dB above each line's median */
    waterfall = lv_waterfall_create(dialog.obj);
    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);
    lv_waterfall_set_palette(waterfall, (lv_color_t *)style.wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, WF_HEIGHT);
    lv_waterfall_set_min(waterfall, -3);
    lv_waterfall_set_max(waterfall, 25);
    lv_obj_set_pos(waterfall, 13, 13);

    /* Red: our offset; green (the finder's cursor): the selected station */
    finder = lv_finder_create(waterfall);
    lv_finder_set_width(finder, (uint16_t)JTTY_BW_HZ);
    lv_obj_set_size(finder, WIDTH, WF_HEIGHT);
    lv_obj_set_pos(finder, 0, 0);
    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);

    status_label = lv_label_create(dialog.obj);
    lv_obj_set_style_text_font(status_label, &sony_22, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(status_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_50, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_RIGHT, -16, 15);
    lv_label_set_text(status_label, "");

    /* TX bar */
    tx_bar = lv_label_create(dialog.obj);
    lv_obj_set_pos(tx_bar, 13, 13 + WF_HEIGHT);
    lv_obj_set_size(tx_bar, WIDTH, BAR_HEIGHT);
    lv_obj_set_style_text_font(tx_bar, &sony_24, 0);
    lv_obj_set_style_text_color(tx_bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tx_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(tx_bar, 6, 0);
    lv_obj_set_style_pad_top(tx_bar, 2, 0);
    lv_label_set_long_mode(tx_bar, LV_LABEL_LONG_DOT);

    /* Messages: one column, so the MFK steps through rows */
    table = lv_table_create(dialog.obj);
    lv_obj_set_pos(table, 13, LIST_TOP);
    lv_obj_set_size(table, WIDTH, LIST_HEIGHT);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(table, 128, LV_PART_MAIN);
    lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, lv_color_hex(0xC0C0C0), LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 5, LV_PART_ITEMS);
    /* our own row highlight replaces LVGL's selected-cell look */
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(table, 0, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(table, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(table, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(table, 0, LV_PART_MAIN | LV_STATE_EDITED);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, WIDTH - 4);
    lv_obj_add_event_cb(table, draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    nrows = 0;
    sel_id = 0;
    sel_call[0] = '\0';
    total = 0;
    transmitting = false;
    keyboard_open = false;
    keyboard_serial = false;
    apply_font();
    font_obs = subject_subscribe((Subject *)cfg.ui.digi_compact(), font_cb, NULL);

    /* Dedicated frequency, locked as in FT8; back on the way out */
    mem_save(MEM_BACKUP_ID);
    load_band(0);
    lm_set_ab(true);
    lm_set_mode(true);
    lm_set_freq(true);
    lm_set_band(true);

    /* kept on the card */
    serial = (uint32_t)param_i_get(cfg.jtty.serial());
    marker_hz = param_i_get(cfg.jtty.tx_freq());
    nqsos = 0;
    last_partner[0] = '\0';
    jtty_log_open();

    rx = jtty_rx_create(rx_cb, NULL);
    apply_filter();
    select_row(-1);
    if (rx != NULL) {
        jtty_rx_start(rx);
        audio_sub = dsp_audio_subscribe_float(audio_cb, JTTY_RATE);
        dsp_audio_set_active(audio_sub, true);
    } else {
        msg_update_text_fmt("JTTY: out of memory");
    }
    jtty_tx_start(tx_cb, NULL);

    status_timer = lv_timer_create(status_timer_cb, 1000, NULL);
    status_timer_cb(NULL);
}

static void destruct_cb(void)
{
    /* nothing goes on the air once the window is gone */
    jtty_tx_stop();
    transmitting = false;
    param_i_set(cfg.jtty.tx_freq(), marker_hz);
    jtty_log_close();

    digi_menu_close();
    if (keyboard_open) {
        textarea_window_close();
        keyboard_open = false;
    }
    if (audio_sub != AUDIO_SUB_INVALID) {
        dsp_audio_set_active(audio_sub, false);
        dsp_audio_unsubscribe(audio_sub);
        audio_sub = AUDIO_SUB_INVALID;
    }
    if (rx != NULL) {
        jtty_rx_stop(rx);
        jtty_rx_destroy(rx);
        rx = NULL;
    }
    if (font_obs != NULL) {
        param_unsubscribe(font_obs);
        font_obs = NULL;
    }
    if (status_timer) {
        lv_timer_del(status_timer);
        status_timer = NULL;
    }
    table = NULL;
    waterfall = NULL;
    finder = NULL;
    tx_bar = NULL;
    status_label = NULL;

    waterfall_set_enabled(true);
    spectrum_set_enabled(true);

    mem_load(MEM_BACKUP_ID);
    lm_set_ab(false);
    lm_set_mode(false);
    lm_set_freq(false);
    lm_set_band(false);
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ENTER:
            next_cb(NULL);              /* the MFK sends what Next says */
            break;

        case LV_KEY_ESC:
            if (transmitting || jtty_tx_busy()) {
                jtty_tx_abort();
                msg_update_text_fmt("JTTY: transmission stopped");
            } else {
                dialog_destruct();
            }
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
