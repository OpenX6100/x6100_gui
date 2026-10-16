/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - PSK31 window
 *
 *  Display and control layer only. The receiver (psk_rx.c) and the
 *  transmitter (psk_tx.c) live below; this file feeds them a frequency
 *  and shows what they produce.
 *
 *  Modelled on dialog_ft8.c: a full screen dialog owning a waterfall,
 *  with the received text laid over its lower part so that turning the
 *  encoder fades the text away and reveals the spectrum underneath.
 *
 *  Tuning works the way fldigi does. The radio stays on the standard
 *  PSK31 dial frequency for the band and the encoder moves the decoder
 *  across the audio passband, shown by the finder on the waterfall.
 *  When the encoder stops, the finder snaps onto the signal under it.
 *
 *  Why the snap and the tracking below exist: the receiver's AFC is a
 *  phase-error loop, and a differential BPSK detector cannot tell a
 *  correct lock from one 15.6 Hz (half the baud rate) off - there the
 *  carrier turns 180 degrees per symbol, every bit comes out inverted,
 *  the quality figure still reads high and the text is garbage.
 *  Measured on synthetic signals, the decoder only reads correctly when
 *  started within about 7 Hz of the carrier, and cannot follow a
 *  station that drifts more than 12 Hz. Both are handled here, from
 *  the spectrum, without touching the decoder:
 *
 *   - snap: after the encoder stops, move onto the energy centroid of
 *     the signal under the finder (psk_spec_snap), typically within
 *     1-2 Hz and within 6 Hz even for a weak idle signal;
 *   - guard: if the decoder has locked more than 9 Hz away from the
 *     centroid of the signal it sits on, put it back;
 *   - follow: fold the AFC's pull into the centre frequency, so the
 *     loop never runs out of range on a drifting station. The transmit
 *     frequency moves with it, as it does in fldigi.
 */

#include "dialog_psk.h"

#include "lvgl/lvgl.h"

#include "dialog.h"
#include "styles.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "audio.h"
#include "dsp.h"
#include "events.h"
#include "buttons.h"
#include "main_screen.h"
#include "keyboard.h"
#include "msg.h"
#include "panel.h"
#include "scheduler.h"
#include "textarea_window.h"

#include "waterfall.h"
#include "spectrum.h"
#include "lock_manager.h"
#include "widgets/lv_waterfall.h"
#include "widgets/lv_finder.h"

#include "psk_rx.h"
#include "psk_tx.h"
#include "psk_spec.h"
#include "psk_macro.h"
#include "digi_qso.h"           /* shared QSO state: DX call and RST */
#include "digi_menu.h"
#include "digi_next.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIDTH           771
#define WF_HEIGHT       325

/* The text sits over the waterfall, leaving the top strip visible:
 * 130 lines, about 12 seconds of spectrum history. */
#define TEXT_TOP        130
#define TEXT_PAD        6

/* Finder: roughly the occupied bandwidth of PSK31, 2 x 31.25 Hz. */
#define FINDER_W_HZ     62

/* Encoder: Hz per click, times 3 or 6 when turned quickly. The snap
 * makes the exact landing point unimportant. */
#define ROTARY_HZ       5

#define SNAP_DELAY_MS   400
#define SNAP_SEARCH_HZ  45.0f

/* Guard and follow, see the file header. */
#define TRACK_PERIOD_MS 500
#define GUARD_SEARCH_HZ 18.0f
#define GUARD_ERR_HZ    9.0f
#define GUARD_MIN_Q     65
#define FOLLOW_MIN_Q    70
#define FOLLOW_MIN_HZ   2.0f
#define QUIET_AFTER_MS  1500    /* after turning the knob or after TX */

/* Averaging of the spectrum lines used by snap and guard: 0.05 per
 * line is a time constant of about 2 seconds at 10.8 lines per second.
 * Measured: an idle station at -12 dB in 2.5 kHz is then found with
 * 1.9 Hz rms error, 5.4 Hz worst of 100; at 0.2 the worst case was
 * 21 Hz, well outside what the decoder can lock from. The average is
 * per audio frequency, so retuning does not disturb it. */
#define AVG_ALPHA       0.05f
#define AVG_MIN_LINES   20

/* Received text: a ring of logical lines. LVGL wraps each one to the
 * width of the box; lines longer than this are broken by hand. */
#define TXT_LINES       24
#define TXT_LINE_LEN    96
#define TXT_SHOW        10      /* labels; more than the box can hold */

#define TX_COLOR        0xF5C400    /* same yellow as the panel echo */

/*
 * Squelch: a threshold in dB that the level must reach before decoded
 * characters are shown, 0 = off, 2 dB of hysteresis, "*" on the button
 * while open, live level beside it.
 *
 * The level is the signal to noise ratio in the decoder's passband,
 * taken from the spectrum the window already has, relative to the noise
 * of the whole band - not an absolute level, which would move with every
 * change of AF or RF gain - so a threshold set once holds on every band
 * and at any gain.
 *
 * Measured on synthetic signals (120 s each, 0.2 smoothing, +-25 Hz):
 * noise alone reads 0 dB, extremes -2.3..+2.4; a station at -12 dB in
 * 2.5 kHz reads 5.2 (never below 3.6); -15 dB reads 3.3; -9 dB 7.5.
 * A threshold of 3-4 dB passes anything the decoder can read and
 * keeps noise out.
 */
#define SQL_MAX_DB      30
#define SQL_HYST_DB     2.0f
#define SQL_HALF_HZ     25.0f
#define SQL_ALPHA       0.2f
#define KNOB_HOLD_MS    8000    /* MFK knob goes back to fine tuning */
/*
 * Dial: the radio normally stays on the band's PSK31 frequency and the
 * waterfall shows the 2.9 kHz above it. The Dial button lets the MFK
 * knob move the VFO in 1 kHz steps, up to 50 kHz either way, for a
 * station outside that window. Changing band, or holding Dial, returns
 * to the standard frequency; leaving the window restores whatever the
 * radio was on before it opened.
 */
#define DIAL_STEP_HZ    1000
#define DIAL_MAX_HZ     50000

#ifndef SQL_CONF_PATH
#define SQL_CONF_PATH   "/mnt/psk.conf"
#endif

/* ------------------------------------------------------------------ */
/* Standard PSK31 dial frequencies (USB)                               */

/*
 * PSK31 has no single official list of frequencies. These are where
 * activity is found, cross-checked between several published lists;
 * PSK31 signals sit within the 3 kHz above the dial, which is exactly
 * what the waterfall shows. Region 1 values where regions differ:
 *
 *  - 40 m: 7040 kHz is Region 1. Region 2 uses 7070, Region 3 7035.
 *  - 17 m: 18097 kHz. PSK31 moved down from 18100 in 2019, when FT8
 *    took that frequency.
 *  - 60 m is left out: allocations and permitted modes differ from
 *    country to country and there is no common PSK31 frequency.
 */
typedef struct {
    const char *label;
    uint32_t    dial_hz;
} psk_band_t;

static const psk_band_t psk_bands[] = {
    {"160m",   1838000},
    {"80m",    3580000},
    {"40m",    7040000},
    {"30m",   10140000},
    {"20m",   14070000},
    {"17m",   18097000},
    {"15m",   21070000},
    {"12m",   24920000},
    {"10m",   28120000},
    {"6m",    50290000},
};

#define PSK_BAND_CNT ((int)(sizeof(psk_bands) / sizeof(psk_bands[0])))

static int cur_band = -1;

/* ------------------------------------------------------------------ */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static void band_cb(lv_event_t *e);

static lv_obj_t   *waterfall = NULL;
static lv_obj_t   *finder = NULL;
static lv_obj_t   *text_box = NULL;
static lv_obj_t   *line_lbl[TXT_SHOW];

/* Received text font, from Settings > Interface > Decoder text, read when
 * the window opens: Compact sony_28 (about 9 lines in the box), Large
 * sony_36, the dialog font (6 lines). */
static const lv_font_t *txt_font = &sony_28;

static lv_timer_t *status_timer = NULL;
static lv_timer_t *text_timer = NULL;
static lv_timer_t *snap_timer = NULL;
static lv_timer_t *fade_timer = NULL;
static lv_timer_t *focus_timer = NULL;
static lv_timer_t *editor_timer = NULL;

static lv_anim_t   fade;
static bool        fade_run = false;

/* True between construct and destruct. Work the DSP thread queued for
 * the UI thread can still arrive after the window is gone. */
static bool        alive = false;

/* Spectrum: the DSP thread runs psk_spec through the receiver's audio
 * tap; the UI thread changes its range. */
static pthread_mutex_t spec_mux = PTHREAD_MUTEX_INITIALIZER;
static psk_spec_t  spec;
static bool        spec_ok = false;

/* UI thread copies of the displayed span, and the averaged line. */
static float       sp_bin0_hz = 0.0f;
static float       sp_df = 1.0f;
static float       sp_edge_lo = 200.0f;
static float       sp_edge_hi = 2800.0f;
static float       avg_lin[PSK_SPEC_MAXBIN];
static int         avg_nb = 0;
static int         avg_lines = 0;

/* Squelch */
static uint8_t     sql_thr = 0;        /* dB, 0 = off */
static uint8_t     sql_last = 4;       /* restored by hold after "off" */
static bool        sql_open = true;
static float       sql_lin = 1.0f;     /* smoothed (S+N)/N, linear */
static bool        sql_primed = false;
/* What the multifunction knob does: 1 Hz fine tuning normally, or the
 * squelch threshold / the VFO while the SQL / Dial button has it. */
typedef enum { KNOB_FINE, KNOB_SQL, KNOB_DIAL } knob_mode_t;
static knob_mode_t knob = KNOB_FINE;
static lv_timer_t *knob_timer = NULL;
static int32_t     dial_offset_hz = 0; /* from the band's PSK31 dial */
static bool        sql_dirty = false;
static int         sql_refresh_div = 0;
static float       line_lin[PSK_SPEC_MAXBIN];

static uint32_t    quiet_since = 0;    /* lv_tick of last knob or TX end */
static bool        was_tx = false;

/* Text entry state */
static bool        input_active = false;
static int         edit_idx = -1;      /* macro being edited, -1 = new */
static char        edit_name[PSK_MACRO_NAME_LEN];
static digi_next_t next_state;          /* where the QSO is, for Next */

/* ---- Buttons ---------------------------------------------------------- */

static const char *status_label_getter(void);
static const char *dx_label_getter(void);
static const char *rst_label_getter(void);
static const char *sql_label_getter(void);
static void sql_press_cb(button_data_t *b);
static void sql_hold_cb(button_data_t *b);
static const char *dial_label_getter(void);
static void dial_press_cb(button_data_t *b);
static void dial_hold_cb(button_data_t *b);

static void text_press_cb(button_data_t *b);
static void stop_press_cb(button_data_t *b);
static void status_press_cb(button_data_t *b);
static void status_hold_cb(button_data_t *b);
static void clear_press_cb(button_data_t *b);
static void band_up_cb(button_data_t *b);
static void band_down_cb(button_data_t *b);
static void dx_press_cb(button_data_t *b);
static void dx_hold_cb(button_data_t *b);
static void rst_press_cb(button_data_t *b);
static void rst_hold_cb(button_data_t *b);

static void macro_press(int idx);
static void macro_hold(int idx);

/*
 * One page of buttons: Next, Send..., Macros, Status, Menu.
 *
 * Next (also a press of the MFK) sends the macro the QSO needs now and
 * says which on its label (digi_next.c: CQ, Answer, Report, TNX, 73);
 * while transmitting it is Stop TX. Macros lists every macro with what it
 * would send, plus New and Edit; Menu holds squelch, dial, band, the QSO
 * fields and Clear. Both are lists over the window (digi_menu.c): the MFK
 * moves, pressing it picks, ESC closes.
 *
 * The buttons below that are not on the page (squelch, dial, DX, RST,
 * ...) keep their callbacks for the menu, and their labels for the
 * message line while the knob is theirs.
 */
static const char *next_label_getter(void);
static void next_press_cb(button_data_t *b);
static void macros_press_cb(button_data_t *b);
static void menu_press_cb(button_data_t *b);

static buttons_page_t page_1;

static button_data_t btn_next = {
    .type = BTN_TEXT_FN, .label_fn = next_label_getter, .press = next_press_cb,
};
static button_data_t btn_text = {
    .type = BTN_TEXT, .label = "Send...", .press = text_press_cb,
};
static button_data_t btn_macros = {
    .type = BTN_TEXT, .label = "Macros", .press = macros_press_cb,
};
static button_data_t btn_status = {
    .type = BTN_TEXT_FN, .label_fn = status_label_getter,
    .press = status_press_cb, .hold = status_hold_cb,
};
static button_data_t btn_menu = {
    .type = BTN_TEXT, .label = "Menu", .press = menu_press_cb,
};

/* not on the page, see above */
static button_data_t btn_stop = {
    .type = BTN_TEXT, .label = "Stop\nTX", .press = stop_press_cb,
};
static button_data_t btn_sql = {
    .type = BTN_TEXT_FN, .label_fn = sql_label_getter,
    .press = sql_press_cb, .hold = sql_hold_cb,
};
static button_data_t btn_dial = {
    .type = BTN_TEXT_FN, .label_fn = dial_label_getter,
    .press = dial_press_cb, .hold = dial_hold_cb,
};
static button_data_t btn_dx = {
    .type = BTN_TEXT_FN, .label_fn = dx_label_getter,
    .press = dx_press_cb, .hold = dx_hold_cb,
};
static button_data_t btn_rst = {
    .type = BTN_TEXT_FN, .label_fn = rst_label_getter,
    .press = rst_press_cb, .hold = rst_hold_cb,
};

static buttons_page_t page_1 = {
    {&btn_next, &btn_text, &btn_macros, &btn_status, &btn_menu}
};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb,
};

dialog_t *dialog_psk = &dialog;

/* BTN_TEXT_FN without a subject never repaints by itself, and a button
 * that is not on the current page has nothing to repaint. */
static void refresh_btn(button_data_t *b)
{
    if (b && b->disp_btn) {
        buttons_refresh(b);
    } else if (b && b->type == BTN_TEXT_FN &&
               ((b == &btn_sql && knob == KNOB_SQL) || (b == &btn_dial && knob == KNOB_DIAL))) {
        /* the knob's button is not on the page: say it on the message line */
        char buf[48];

        snprintf(buf, sizeof(buf), "%s", b->label_fn());
        for (char *p = buf; *p; p++) {
            if (*p == '\n')
                *p = ' ';
        }
        msg_update_text_fmt("#FFFFFF %s", buf);
    }
}

/* ---- Received text ---------------------------------------------------- */

typedef struct {
    char    s[TXT_LINE_LEN];
    uint8_t len;
    bool    tx;
} txt_line_t;

static txt_line_t txt[TXT_LINES];
static int        txt_head = 0;     /* line being written */
static int        txt_cnt = 1;      /* lines in use, including head */
static bool       txt_dirty = false;

static void txt_clear(void)
{
    memset(txt, 0, sizeof(txt));
    txt_head = 0;
    txt_cnt = 1;
    txt_dirty = true;
}

static void txt_new_line(bool tx)
{
    txt_head = (txt_head + 1) % TXT_LINES;
    txt[txt_head].len = 0;
    txt[txt_head].s[0] = '\0';
    txt[txt_head].tx = tx;
    if (txt_cnt < TXT_LINES)
        txt_cnt++;
    txt_dirty = true;
}

static void txt_put(char c, bool tx)
{
    txt_line_t *l = &txt[txt_head];

    /* Received text never continues a transmitted line, and the other
     * way round, so the colours never mix within one line. */
    if (l->tx != tx) {
        if (l->len > 0) {
            txt_new_line(tx);
            l = &txt[txt_head];
        } else {
            l->tx = tx;
        }
    }

    if (c == '\n') {
        txt_new_line(tx);
        return;
    }

    if (l->len + 1 >= TXT_LINE_LEN) {
        txt_new_line(tx);
        l = &txt[txt_head];
    }

    l->s[l->len++] = c;
    l->s[l->len] = '\0';
    txt_dirty = true;
}

/*
 * One label per logical line rather than one label with colour markup:
 * LVGL 8 resets the recolour state at every wrapped row, so a sent line
 * long enough to wrap lost its colour half way and the closing '#' then
 * swallowed the start of the next line. Separate labels need no markup
 * and no escaping, and a '#' off the air is just a character.
 *
 * The labels are stacked upwards from the bottom of the box, which
 * clips whatever no longer fits at the top. Positioned by hand rather
 * than with a flex layout, so nothing depends on LV_USE_FLEX.
 */
static void txt_render(void)
{
    int first = (txt_head - txt_cnt + 1 + TXT_LINES) % TXT_LINES;
    int n = txt_cnt;
    int k;

    /* An empty line being written is the next line, not a blank one:
     * leave it out, or the text would sit one row too high. */
    if (n > 0 && txt[txt_head].len == 0)
        n--;

    /* Only the newest few can be on screen. */
    if (n > TXT_SHOW) {
        first = (first + (n - TXT_SHOW)) % TXT_LINES;
        n = TXT_SHOW;
    }

    for (k = n; k < TXT_SHOW; k++)
        lv_obj_add_flag(line_lbl[k], LV_OBJ_FLAG_HIDDEN);

    /* Newest at the bottom, working upwards. */
    lv_coord_t y = WF_HEIGHT - TEXT_TOP - 2 * TEXT_PAD;

    for (k = n - 1; k >= 0; k--) {
        const txt_line_t *l = &txt[(first + k) % TXT_LINES];
        lv_obj_t *lbl = line_lbl[k];
        lv_point_t size;

        lv_txt_get_size(&size, l->s, txt_font, 0, 0, WIDTH - 2 * TEXT_PAD,
                        LV_TEXT_FLAG_NONE);
        y -= size.y;

        /* A line that no longer fits whole is dropped rather than shown
         * cut in half, and so is everything older. */
        if (y < 0) {
            for (int j = 0; j <= k; j++)
                lv_obj_add_flag(line_lbl[j], LV_OBJ_FLAG_HIDDEN);
            break;
        }

        lv_label_set_text(lbl, l->s);
        lv_obj_set_style_text_color(lbl, l->tx ? lv_color_hex(TX_COLOR)
                                               : lv_color_white(), 0);
        lv_obj_set_pos(lbl, 0, y);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void text_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* The level changes continuously: repaint the button every
     * 400 ms. */
    if (++sql_refresh_div >= 4) {
        sql_refresh_div = 0;
        refresh_btn(&btn_sql);
    }

    if (txt_dirty && text_box) {
        txt_dirty = false;
        txt_render();
    }
}

/* UI thread: one decoded character. */
static void rx_char_ui(void *data)
{
    char c = *(char *)data;
    char before[16];

    if (!alive)
        return;

    /* Control characters are decoding noise, apart from line feed. */
    if (c != '\n' && (unsigned char)c < 32)
        return;
    if ((unsigned char)c == 127)
        return;

    /* Squelch closed: what the decoder makes of noise is not shown and
     * cannot trigger the DX call capture either. */
    if (!sql_open)
        return;

    txt_put(c, false);

    /* Catch the correspondent's call from "DE <call>". */
    snprintf(before, sizeof(before), "%s", digi_qso_dx());
    digi_qso_feed_rx(c);
    if (strcmp(before, digi_qso_dx()) != 0)
        refresh_btn(&btn_dx);
}

/* DSP thread, receiver lock held: hand over and return. */
static void rx_text_cb(char c)
{
    scheduler_put(rx_char_ui, &c, sizeof(c));
}

/* UI thread, from psk_tx_send(). */
static void tx_echo_cb(const char *text)
{
    const char *p = text;
    size_t      n;

    if (!alive || !text)
        return;

    /* The line breaks sent around every transmission are for the other
     * station's screen; here each transmission gets its own line. */
    while (*p == '\n' || *p == '\r')
        p++;
    n = strlen(p);
    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r'))
        n--;

    if (txt[txt_head].len > 0)
        txt_new_line(true);

    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\r')
            txt_put(p[i], true);
    }
    txt_new_line(false);
}

/* ---- Spectrum and waterfall ------------------------------------------- */

/* ---- Squelch ----------------------------------------------------------- */

static float sql_db(void)
{
    return 10.0f * log10f(sql_lin + 1e-6f);
}

static void sql_load(void)
{
    FILE *f = fopen(SQL_CONF_PATH, "r");
    char line[64];

    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        int v;

        if (sscanf(line, "squelch=%d", &v) == 1 && v >= 0 && v <= SQL_MAX_DB)
            sql_thr = (uint8_t)v;
        else if (sscanf(line, "squelch_last=%d", &v) == 1 && v > 0 && v <= SQL_MAX_DB)
            sql_last = (uint8_t)v;
    }
    fclose(f);
}

static void sql_save(void)
{
    FILE *f;

    if (!sql_dirty)
        return;

    f = fopen(SQL_CONF_PATH, "w");
    if (!f) {
        msg_update_text_fmt("#FF0000 Could not write " SQL_CONF_PATH);
        return;
    }
    fprintf(f, "squelch=%u\nsquelch_last=%u\n", (unsigned)sql_thr,
            (unsigned)sql_last);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    sql_dirty = false;
}

/* Called once per spectrum line with that line in linear form. */
static void sql_update(const float *lin, int nbins)
{
    float w = psk_spec_window_lin(lin, nbins, sp_bin0_hz, sp_df,
                                  (float)psk_tx_get_center(), SQL_HALF_HZ);
    float db;

    if (!sql_primed) {
        sql_lin = w;
        sql_primed = true;
    } else {
        sql_lin += SQL_ALPHA * (w - sql_lin);
    }

    if (sql_thr == 0) {
        sql_open = true;
        return;
    }

    db = sql_db();
    if (!sql_open) {
        if (db > (float)sql_thr)
            sql_open = true;
    } else {
        if (db < (float)sql_thr - SQL_HYST_DB)
            sql_open = false;
    }
}

static const char *sql_label_getter(void)
{
    static char buf[32];
    int lvl = (int)lroundf(sql_db());

    if (sql_thr == 0)
        snprintf(buf, sizeof(buf), "SQL off\nS/N %d", lvl);
    else
        snprintf(buf, sizeof(buf), "SQL %u%s\nS/N %d", (unsigned)sql_thr,
                 sql_open ? "*" : "", lvl);
    return buf;
}

static button_data_t *knob_btn(knob_mode_t m)
{
    return (m == KNOB_SQL) ? &btn_sql : &btn_dial;
}

static void knob_mark(knob_mode_t m, bool on)
{
    button_data_t *b;

    if (m == KNOB_FINE)
        return;
    b = knob_btn(m);
    if (b->disp_btn)
        buttons_mark(b, on);
    b->mark = on;
}

/* Give the knob back to fine tuning. */
static void knob_release(bool say)
{
    if (knob_timer) {
        lv_timer_del(knob_timer);
        knob_timer = NULL;
    }
    if (knob != KNOB_FINE) {
        knob_mark(knob, false);
        knob = KNOB_FINE;
        if (say)
            msg_update_text_fmt("#FFFFFF Knob: fine tuning");
    }
    sql_save();
}

static void knob_timer_cb(lv_timer_t *t)
{
    (void)t;
    knob_timer = NULL;      /* one-shot: LVGL deletes it */
    if (alive)
        knob_release(true);
}

static void knob_timer_restart(void)
{
    if (knob_timer) {
        lv_timer_reset(knob_timer);
    } else {
        knob_timer = lv_timer_create(knob_timer_cb, KNOB_HOLD_MS, NULL);
        lv_timer_set_repeat_count(knob_timer, 1);
    }
}

/* Pressing the same button again releases the knob; pressing the other
 * one hands it over directly. */
static void knob_take(knob_mode_t m, const char *text)
{
    if (knob == m) {
        knob_release(true);
        return;
    }
    knob_mark(knob, false);
    knob = m;
    knob_mark(m, true);
    knob_timer_restart();
    msg_update_text_fmt("#FFFFFF %s", text);
}

/* Press: the multifunction knob sets the threshold; press again, or
 * leave the knob for 8 s, to get fine tuning back. */
static void sql_press_cb(button_data_t *b)
{
    (void)b;
    knob_take(KNOB_SQL, "Knob: squelch - noise reads about 0, try 3-4");
}

/* Hold: off, and back to the last setting. */
static void sql_hold_cb(button_data_t *b)
{
    if (sql_thr > 0) {
        sql_last = sql_thr;
        sql_thr = 0;
    } else {
        sql_thr = sql_last ? sql_last : 4;
    }
    sql_dirty = true;
    sql_save();
    msg_update_text_fmt("#FFFFFF PSK31 squelch %s", sql_thr ? "on" : "off");
    buttons_refresh(b);
}

static void sql_change(int d)
{
    int v = (int)sql_thr + d;

    if (v < 0)
        v = 0;
    if (v > SQL_MAX_DB)
        v = SQL_MAX_DB;
    if (v == sql_thr)
        return;

    sql_thr = (uint8_t)v;
    if (sql_thr > 0)
        sql_last = sql_thr;
    sql_dirty = true;
    knob_timer_restart();
    refresh_btn(&btn_sql);
}

/*
 * Each new line is averaged with the ones before it (in dB, exponentially,
 * half of the new line), as desktop waterfalls do: every line moves the
 * whole picture down a pixel and the screen dims for a moment while that
 * many pixels change, a flicker at the line rate that a calmer picture
 * makes much smaller. Display only: the squelch, snap and guard still get
 * the line as it came. Restarted on a clear, a band, dial or range change
 * and after transmitting.
 */
#define WF_AVG_NEW  0.5f

static float       wf_avg[PSK_SPEC_MAXBIN];     /* DSP thread */
static int         wf_avg_n = 0;                /* DSP thread; 0 = none yet */
static atomic_bool wf_avg_restart;

/* While we transmit the receiver hears only our own signal, so the
 * waterfall stops (a red frame says why) instead of filling with it. */
static lv_obj_t   *tx_frame = NULL;
static bool        wf_tx = false;               /* UI thread */

static void wf_restart(void)
{
    atomic_store(&wf_avg_restart, true);
}

struct wf_line {
    float *db;          /* n values as they came, then n averaged */
    int    n;
};

static void wf_line_ui(void *data)
{
    struct wf_line *l = (struct wf_line *)data;
    bool            tx = psk_tx_is_on();

    if (!alive || !waterfall) {
        free(l->db);
        return;
    }

    if (tx != wf_tx) {
        wf_tx = tx;
        if (tx_frame) {
            if (tx)
                lv_obj_clear_flag(tx_frame, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(tx_frame, LV_OBJ_FLAG_HIDDEN);
        }
        if (!tx)
            wf_restart();           /* what came in meanwhile was us */
    }

    if (!tx)
        lv_waterfall_add_data(waterfall, l->db + l->n, (uint16_t)l->n);

    if (l->n > PSK_SPEC_MAXBIN) {
        free(l->db);
        return;
    }
    for (int i = 0; i < l->n; i++)
        line_lin[i] = powf(10.0f, l->db[i] * 0.1f);

    sql_update(line_lin, l->n);

    /* Averaged copy for snap and guard. Not while transmitting: what
     * comes back then is our own signal. */
    if (!psk_tx_is_on()) {
        if (l->n != avg_nb) {
            avg_nb = l->n;
            avg_lines = 0;
        }
        for (int i = 0; i < l->n; i++) {
            float v = line_lin[i];

            if (avg_lines == 0)
                avg_lin[i] = v;
            else
                avg_lin[i] += AVG_ALPHA * (v - avg_lin[i]);
        }
        avg_lines++;
    }

    free(l->db);
}

/* DSP thread, from inside the tap. */
static void on_spec_line(const float *db, int nbins, void *ctx)
{
    struct wf_line l;

    (void)ctx;

    if (!db || nbins <= 0)
        return;

    if (nbins > PSK_SPEC_MAXBIN)
        nbins = PSK_SPEC_MAXBIN;

    if (atomic_exchange(&wf_avg_restart, false) || nbins != wf_avg_n) {
        memcpy(wf_avg, db, sizeof(float) * (size_t)nbins);
        wf_avg_n = nbins;
    } else {
        for (int i = 0; i < nbins; i++)
            wf_avg[i] += WF_AVG_NEW * (db[i] - wf_avg[i]);
    }

    l.n = nbins;
    l.db = (float *)malloc(sizeof(float) * 2 * (size_t)nbins);
    if (!l.db)
        return;

    memcpy(l.db, db, sizeof(float) * (size_t)nbins);
    memcpy(l.db + nbins, wf_avg, sizeof(float) * (size_t)nbins);
    scheduler_put(wf_line_ui, &l, sizeof(l));
}

/* DSP thread, receiver lock held. */
static void rx_tap(unsigned int n, const float *samples)
{
    pthread_mutex_lock(&spec_mux);
    if (spec_ok)
        psk_spec_process(&spec, samples, n, on_spec_line, NULL);
    pthread_mutex_unlock(&spec_mux);
}

static int center_min(void)
{
    return (int)ceilf(sp_edge_lo) + FINDER_W_HZ / 2;
}

static int center_max(void)
{
    return (int)floorf(sp_edge_hi) - FINDER_W_HZ / 2;
}

static void update_finder(void)
{
    int c = (int)psk_tx_get_center();

    lv_finder_set_range(finder, (int16_t)lroundf(sp_edge_lo),
                        (int16_t)lroundf(sp_edge_hi));
    lv_finder_set_width(finder, FINDER_W_HZ);
    lv_finder_set_value(finder, (int16_t)(c - FINDER_W_HZ / 2));
    lv_obj_invalidate(finder);
}

static void set_center(int hz)
{
    int lo = center_min();
    int hi = center_max();

    if (hz < lo)
        hz = lo;
    if (hz > hi)
        hz = hi;

    psk_tx_set_center((uint16_t)hz);    /* clamps to its own 300..2700 */
    psk_rx_update_center();
    update_finder();
    refresh_btn(&btn_status);
}

/* Waterfall span follows the receive filter of the current mode, as
 * it does in the FT8 window. */
static void apply_range(void)
{
    float lo = (float)cparam_i_get(cfg.filter.low());
    float hi = (float)cparam_i_get(cfg.filter.high());

    if (lo < 100.0f)
        lo = 100.0f;
    if (hi > 3000.0f)
        hi = 3000.0f;
    if (hi - lo < 400.0f) {
        lo = 200.0f;
        hi = 2800.0f;
    }

    pthread_mutex_lock(&spec_mux);
    psk_spec_set_range(&spec, lo, hi);
    psk_spec_reset(&spec);
    atomic_store(&wf_avg_restart, true);
    sp_bin0_hz = psk_spec_bin0_hz(&spec);
    sp_df = psk_spec_df(&spec);
    sp_edge_lo = psk_spec_edge_lo_hz(&spec);
    sp_edge_hi = psk_spec_edge_hi_hz(&spec);
    pthread_mutex_unlock(&spec_mux);

    avg_nb = 0;
    avg_lines = 0;

    /* Keep the decoder inside the span. */
    set_center((int)psk_tx_get_center());
}

/* ---- Band selection --------------------------------------------------- */

static int band_nearest(uint32_t freq)
{
    int best = 0;
    int64_t best_d = -1;

    for (int i = 0; i < PSK_BAND_CNT; i++) {
        int64_t d = (int64_t)freq - (int64_t)psk_bands[i].dial_hz;

        if (d < 0)
            d = -d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* dir 0 picks the band closest to where the radio already is, which is
 * what opening the window does; +-1 steps through the list. */
static void load_band(int8_t dir)
{
    int idx;

    if (dir == 0 || cur_band < 0) {
        idx = band_nearest((uint32_t)cparam_i_get(cfg.cur.fg_freq()));
    } else {
        idx = cur_band + dir;
        if (idx < 0)
            idx = PSK_BAND_CNT - 1;
        if (idx >= PSK_BAND_CNT)
            idx = 0;
    }

    cur_band = idx;
    dial_offset_hz = 0;
    refresh_btn(&btn_dial);

    /* Frequency first, then mode - the order cfg_digital_load() uses. */
    cparam_i_set(cfg.cur.fg_freq(), (int32_t)psk_bands[idx].dial_hz);
    cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);

    msg_update_text_fmt("PSK31 %s  %u.%03u MHz", psk_bands[idx].label,
                        (unsigned)(psk_bands[idx].dial_hz / 1000000u),
                        (unsigned)((psk_bands[idx].dial_hz / 1000u) % 1000u));
}

static void restart_rx(void)
{
    psk_rx_set_active(false);
    psk_rx_set_active(true);
}

static void change_band(int8_t dir)
{
    if (input_active)
        return;

    if (psk_tx_is_on()) {
        msg_update_text_fmt("#FF0000 PSK31: stop TX first");
        return;
    }

    load_band(dir);
    apply_range();
    restart_rx();
    sql_primed = false;
    lv_waterfall_clear_data(waterfall);
    wf_restart();
    txt_clear();
    quiet_since = lv_tick_get();
}

/* ---- Dial ------------------------------------------------------------- */

static const char *dial_label_getter(void)
{
    static char buf[32];

    if (dial_offset_hz == 0)
        snprintf(buf, sizeof(buf), "Dial\nstandard");
    else
        snprintf(buf, sizeof(buf), "Dial\n%+d kHz", (int)(dial_offset_hz / 1000));
    return buf;
}

static void dial_apply(void)
{
    uint32_t f;

    if (cur_band < 0)
        return;

    f = (uint32_t)((int32_t)psk_bands[cur_band].dial_hz + dial_offset_hz);
    cparam_i_set(cfg.cur.fg_freq(), (int32_t)f);

    /* What the waterfall and the averages hold belongs to the old dial.
     * The received text stays: it is still the same conversation. */
    lv_waterfall_clear_data(waterfall);
    wf_restart();
    avg_lines = 0;
    sql_primed = false;
    restart_rx();
    quiet_since = lv_tick_get();

    if (dial_offset_hz == 0)
        msg_update_text_fmt("PSK31 %s  %u.%03u MHz", psk_bands[cur_band].label,
                            (unsigned)(f / 1000000u), (unsigned)((f / 1000u) % 1000u));
    else
        msg_update_text_fmt("PSK31 %s  %u.%03u MHz (%+d kHz)", psk_bands[cur_band].label,
                            (unsigned)(f / 1000000u), (unsigned)((f / 1000u) % 1000u),
                            (int)(dial_offset_hz / 1000));
    refresh_btn(&btn_dial);
}

static void dial_change(int d)
{
    int32_t v = dial_offset_hz + d * DIAL_STEP_HZ;

    if (psk_tx_is_on()) {
        msg_update_text_fmt("#FF0000 PSK31: stop TX first");
        return;
    }

    if (v < -DIAL_MAX_HZ)
        v = -DIAL_MAX_HZ;
    if (v > DIAL_MAX_HZ)
        v = DIAL_MAX_HZ;
    knob_timer_restart();
    if (v == dial_offset_hz)
        return;

    dial_offset_hz = v;
    dial_apply();
}

/* Press: the multifunction knob moves the VFO in 1 kHz steps. */
static void dial_press_cb(button_data_t *b)
{
    (void)b;
    knob_take(KNOB_DIAL, "Knob: dial, 1 kHz steps");
}

/* Hold: back to the band's PSK31 frequency. */
static void dial_hold_cb(button_data_t *b)
{
    (void)b;

    if (psk_tx_is_on()) {
        msg_update_text_fmt("#FF0000 PSK31: stop TX first");
        return;
    }
    if (dial_offset_hz != 0) {
        dial_offset_hz = 0;
        dial_apply();
    }
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

/* ---- Snap, guard and follow ------------------------------------------- */

static bool snap_near(float hz, float search, float *out)
{
    if (avg_lines < AVG_MIN_LINES || avg_nb <= 0)
        return false;
    return psk_spec_snap(avg_lin, avg_nb, sp_bin0_hz, sp_df, hz, search, out);
}

static void snap_timer_cb(lv_timer_t *t)
{
    float hz;

    (void)t;
    snap_timer = NULL;

    if (!alive)
        return;

    if (snap_near((float)psk_tx_get_center(), SNAP_SEARCH_HZ, &hz)) {
        set_center((int)lroundf(hz));
        restart_rx();       /* start clean on the new station */
    }
    quiet_since = lv_tick_get();
}

static void track(void)
{
    float afc, rx_hz, hz;
    uint8_t q;

    if (psk_tx_is_on() || snap_timer || input_active)
        return;
    if (lv_tick_elaps(quiet_since) < QUIET_AFTER_MS)
        return;

    q = psk_rx_quality();
    afc = psk_rx_freq_error();
    rx_hz = (float)psk_tx_get_center() + afc;

    /* Guard first: a false lock also reads high quality. The quality
     * gate keeps it from being dragged towards a strong neighbour,
     * which pulls the quality down instead. */
    if (q >= GUARD_MIN_Q && snap_near(rx_hz, GUARD_SEARCH_HZ, &hz) &&
        fabsf(hz - rx_hz) > GUARD_ERR_HZ) {
        set_center((int)lroundf(hz));
        return;
    }

    if (q >= FOLLOW_MIN_Q && fabsf(afc) >= FOLLOW_MIN_HZ)
        set_center((int)psk_tx_get_center() + (int)lroundf(afc));
}

/* ---- Button labels ---------------------------------------------------- */

static const char *status_label_getter(void)
{
    static char buf[32];

    if (psk_tx_is_on()) {
        snprintf(buf, sizeof(buf), "TX\n%u Hz", (unsigned)psk_tx_get_center());
    } else {
        snprintf(buf, sizeof(buf), "%u Hz\nQ %u%%",
                 (unsigned)psk_tx_get_center(), (unsigned)psk_rx_quality());
    }
    return buf;
}

static const char *dx_label_getter(void)
{
    static char buf[32];
    const char *c = digi_qso_dx();

    snprintf(buf, sizeof(buf), "DX call\n%s", (c && *c) ? c : "---");
    return buf;
}

static const char *rst_label_getter(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "RST snt\n%s", digi_qso_rst_sent());
    return buf;
}

static void status_timer_cb(lv_timer_t *t)
{
    bool tx = psk_tx_is_on();

    (void)t;

    /* When a transmission ends, the averaged spectrum and the decoder's
     * loops have seen our own signal; start both afresh. */
    if (was_tx && !tx) {
        avg_lines = 0;
        quiet_since = lv_tick_get();
    }
    was_tx = tx;

    track();
    refresh_btn(&btn_status);
    refresh_btn(&btn_next);
}

static void refresh_macro_labels(void)
{
    refresh_btn(&btn_next);     /* its label names a macro */
}

/* ---- Keyboard focus around text entry --------------------------------- */

/*
 * While a text window is open it must be the only thing in the
 * keyboard group, and afterwards the text box has to get the focus
 * back. main_screen_keys_enable() is not usable here - it hands the
 * keyboard to the main screen's spectrum, which is behind this window.
 */
static void input_begin(void)
{
    input_active = true;
    if (text_box)
        lv_group_remove_obj(text_box);
}

static void focus_timer_cb(lv_timer_t *t)
{
    (void)t;
    focus_timer = NULL;

    if (!alive || !text_box || input_active)
        return;

    lv_group_add_obj(keyboard_group, text_box);
    lv_group_focus_obj(text_box);
    lv_group_set_editing(keyboard_group, true);
}

/* Called from an OK or Cancel callback, which returns true so the text
 * window closes itself; the focus comes back once it has gone. */
static void input_end(void)
{
    input_active = false;
    if (!focus_timer) {
        focus_timer = lv_timer_create(focus_timer_cb, 100, NULL);
        lv_timer_set_repeat_count(focus_timer, 1);
    }
}

/* textarea_window_set() takes a plain char pointer; hand it a copy so
 * the stored macro can never be written through it. */
static void textarea_set_copy(const char *text)
{
    static char buf[PSK_MACRO_TEXT_LEN];

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    textarea_window_set(buf);
}

static bool input_cancel_cb(void)
{
    input_end();
    return true;
}

/* ---- Transmit --------------------------------------------------------- */

static bool send_text(const char *text)
{
    char buf[PSK_MACRO_TEXT_LEN + 400];

    if (!text || !*text)
        return false;

    if (psk_tx_is_on()) {
        msg_update_text_fmt("#FF0000 PSK31 TX busy");
        return false;
    }

    /* Start and finish on a fresh line on the other station's screen. */
    snprintf(buf, sizeof(buf), "\n%s\n", text);

    if (psk_tx_send(buf)) {
        msg_update_text_fmt("#FFFFFF PSK31 TX...");
        was_tx = true;
        refresh_btn(&btn_status);
        refresh_btn(&btn_next);
        return true;
    }
    msg_update_text_fmt("#FF0000 PSK31 TX busy");
    return false;
}

static bool free_text_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (alive && t && *t)
        send_text(t);
    input_end();
    return true;
}

static void text_press_cb(button_data_t *b)
{
    (void)b;

    if (input_active)
        return;
    if (psk_tx_is_on()) {
        msg_update_text_fmt("#FF0000 PSK31 TX busy");
        return;
    }

    input_begin();
    textarea_window_open_w_label(free_text_ok_cb, input_cancel_cb, "PSK31 msg:");
    /* The stock window takes 64 characters. */
    lv_textarea_set_max_length(textarea_window_text(), PSK_MACRO_TEXT_LEN - 1);
}

static void stop_press_cb(button_data_t *b)
{
    (void)b;
    psk_tx_stop();
}

static void status_press_cb(button_data_t *b)
{
    (void)b;
    restart_rx();
    msg_update_text_fmt("#FFFFFF PSK31 RX resync");
}

/* Hold: clear the text and the waterfall. */
static void status_hold_cb(button_data_t *b)
{
    clear_press_cb(b);
}

static void clear_press_cb(button_data_t *b)
{
    (void)b;
    txt_clear();
    lv_waterfall_clear_data(waterfall);
    wf_restart();
}

/* ---- Macros ----------------------------------------------------------- */

static void macro_press(int idx)
{
    const psk_macro_t *m;
    char out[PSK_MACRO_TEXT_LEN + 128];
    const char *missing = NULL;

    if (input_active)
        return;

    m = psk_macro_get(idx);
    if (!m) {
        msg_update_text_fmt("#FFFFFF Empty slot - hold to create a macro");
        return;
    }

    if (!psk_macro_expand(m->text, out, sizeof(out), &missing)) {
        if (missing && strcmp(missing, "<CALL>") == 0)
            msg_update_text_fmt("#FF0000 No DX call - set it on page 2");
        else
            msg_update_text_fmt("#FF0000 %s is empty", missing ? missing : "?");
        return;
    }

    if (send_text(out)) {
        digi_next_sent(&next_state, m->name, digi_qso_dx());
        if (digi_next_is_cq(m->name)) {
            digi_qso_clear();           /* a CQ starts a new QSO */
        }
        refresh_btn(&btn_next);
    }
}

/* Editor, two steps: name, then text. Both windows
 * open pre-filled when an existing macro is edited, so a macro can be
 * renamed. Saving an empty text deletes the macro. */

static bool editor_text_ok_cb(void)
{
    const char *t = textarea_window_get();
    bool empty = (!t || !*t);

    if (!alive) {
        input_end();
        return true;
    }

    if (edit_idx >= 0) {
        if (empty) {
            psk_macro_delete(edit_idx);
            msg_update_text_fmt("#FFFFFF Macro deleted");
        } else {
            psk_macro_set(edit_idx, edit_name, t);
            msg_update_text_fmt("#FFFFFF Macro saved");
        }
    } else if (!empty) {
        if (psk_macro_add(edit_name, t) < 0)
            msg_update_text_fmt("#FF0000 All %d macro slots are used", PSK_MACRO_MAX);
        else
            msg_update_text_fmt("#FFFFFF Macro saved");
    }

    if (!psk_macros_save())
        msg_update_text_fmt("#FF0000 Could not write /mnt/psk_macros.txt");

    refresh_macro_labels();
    edit_idx = -1;
    input_end();
    return true;
}

static void editor_timer_cb(lv_timer_t *t)
{
    const psk_macro_t *m;

    (void)t;
    editor_timer = NULL;

    if (!alive) {
        input_active = false;
        return;
    }

    textarea_window_open_w_label(editor_text_ok_cb, input_cancel_cb, "Macro text:");
    lv_textarea_set_max_length(textarea_window_text(), PSK_MACRO_TEXT_LEN - 1);

    m = (edit_idx >= 0) ? psk_macro_get(edit_idx) : NULL;
    if (m)
        textarea_set_copy(m->text);
}

static bool editor_name_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (!alive) {
        input_end();
        return true;
    }

    if (t && *t) {
        snprintf(edit_name, sizeof(edit_name), "%s", t);
    } else {
        const psk_macro_t *m = (edit_idx >= 0) ? psk_macro_get(edit_idx) : NULL;

        if (m)
            snprintf(edit_name, sizeof(edit_name), "%s", m->name);
        else
            snprintf(edit_name, sizeof(edit_name), "M%d", psk_macros_count() + 1);
    }

    /* Let this window close before the next one opens, or two text
     * areas end up in the keyboard group. input_active stays set. */
    if (!editor_timer) {
        editor_timer = lv_timer_create(editor_timer_cb, 50, NULL);
        lv_timer_set_repeat_count(editor_timer, 1);
    }
    return true;
}

static void macro_hold(int idx)
{
    const psk_macro_t *m;

    if (input_active)
        return;

    if (idx >= psk_macros_count()) {
        if (psk_macros_count() >= PSK_MACRO_MAX) {
            msg_update_text_fmt("#FF0000 All %d macro slots are used", PSK_MACRO_MAX);
            return;
        }
        idx = -1;       /* an empty slot: new macro, added at the end */
    }

    edit_idx = idx;
    m = (idx >= 0) ? psk_macro_get(idx) : NULL;

    input_begin();
    textarea_window_open_w_label(editor_name_ok_cb, input_cancel_cb, "Macro name:");
    if (m)
        textarea_set_copy(m->name);
}

/* ---- Next, Macros, Menu ------------------------------------------------ */

static const char *macro_name_at(int idx)
{
    const psk_macro_t *m = psk_macro_get(idx);

    return (m && idx < psk_macros_count()) ? m->name : NULL;
}

static const char *next_label_getter(void)
{
    static char buf[48];

    if (psk_tx_is_on()) {
        snprintf(buf, sizeof(buf), "Stop\nTX");
    } else {
        digi_next_label(&next_state, digi_qso_dx(), psk_macros_count(), macro_name_at, buf,
                        sizeof(buf));
    }
    return buf;
}

static void next_press_cb(button_data_t *b)
{
    int idx;

    (void)b;
    digi_menu_close();
    if (input_active)
        return;
    if (psk_tx_is_on()) {
        stop_press_cb(NULL);
        refresh_btn(&btn_next);
        return;
    }
    idx = digi_next_pick(&next_state, digi_qso_dx(), psk_macros_count(), macro_name_at);
    if (idx < 0) {
        msg_update_text_fmt("#FF0000 No CQ/Answer/Report/TNX/73 macro - see Macros");
        return;
    }
    macro_press(idx);
}

/* Macros: every macro with what it sends now, then New and Edit. */
static void macro_edit_pick(int item, void *ctx)
{
    (void)ctx;
    macro_hold(item);
}

static void macros_pick(int item, void *ctx)
{
    int n = psk_macros_count();

    (void)ctx;
    if (item < n) {
        macro_press(item);
    } else if (item == n) {
        macro_hold(n);                  /* a new one, at the end */
    } else {
        static char  text[PSK_MACRO_MAX][PSK_MACRO_NAME_LEN + 8];
        const char  *items[PSK_MACRO_MAX];

        for (int i = 0; i < n; i++) {
            snprintf(text[i], sizeof(text[i]), "%s", psk_macro_get(i)->name);
            items[i] = text[i];
        }
        digi_menu_open(dialog.obj, "Edit which macro?", items, n, macro_edit_pick, NULL, NULL);
    }
}

static void macros_press_cb(button_data_t *b)
{
    static char  text[PSK_MACRO_MAX][72];
    const char  *items[PSK_MACRO_MAX + 2];
    int          n = psk_macros_count();
    int          k = 0;

    (void)b;
    if (input_active)
        return;
    if (digi_menu_is_open()) {
        digi_menu_close();
        return;
    }
    for (int i = 0; i < n && k < DIGI_MENU_MAX - 2; i++) {
        const psk_macro_t *m = psk_macro_get(i);
        char               out[PSK_MACRO_TEXT_LEN + 128];
        const char        *missing = NULL;

        if (!psk_macro_expand(m->text, out, sizeof(out), &missing))
            snprintf(out, sizeof(out), "%s", m->text);
        snprintf(text[k], sizeof(text[k]), "%-7s %.60s", m->name, out);
        items[k] = text[k];
        k++;
    }
    items[k++] = (n < PSK_MACRO_MAX) ? "New macro..." : "(all macro slots used)";
    if (n > 0)
        items[k++] = "Edit a macro...";
    digi_menu_open(dialog.obj, "Macros: MFK to choose, press to send", items, k, macros_pick,
                   NULL, NULL);
}

enum {
    MENU_SQL, MENU_SQL_ONOFF, MENU_DIAL, MENU_DIAL_BACK, MENU_BAND_UP, MENU_BAND_DOWN,
    MENU_DX, MENU_RST, MENU_NEW_QSO, MENU_CLEAR, MENU_COUNT
};

static void menu_pick(int item, void *ctx)
{
    (void)ctx;
    switch (item) {
        case MENU_SQL:       sql_press_cb(&btn_sql); break;
        case MENU_SQL_ONOFF: sql_hold_cb(&btn_sql); break;
        case MENU_DIAL:      dial_press_cb(&btn_dial); break;
        case MENU_DIAL_BACK: dial_hold_cb(&btn_dial); break;
        case MENU_BAND_UP:   band_up_cb(NULL); break;
        case MENU_BAND_DOWN: band_down_cb(NULL); break;
        case MENU_DX:        dx_press_cb(&btn_dx); break;
        case MENU_RST:       rst_press_cb(&btn_rst); break;
        case MENU_NEW_QSO:
            dx_hold_cb(&btn_dx);
            memset(&next_state, 0, sizeof(next_state));
            break;
        case MENU_CLEAR:     clear_press_cb(NULL); break;
    }
    refresh_btn(&btn_next);
}

static void menu_press_cb(button_data_t *b)
{
    static char  text[MENU_COUNT][48];
    const char  *items[MENU_COUNT];
    const char  *dx = digi_qso_dx();

    (void)b;
    if (input_active)
        return;
    if (digi_menu_is_open()) {
        digi_menu_close();
        return;
    }
    snprintf(text[MENU_SQL], 48, "Squelch with the knob  (%s)", sql_thr ? "on" : "off");
    snprintf(text[MENU_SQL_ONOFF], 48, "Squelch %s", sql_thr ? "off" : "on");
    snprintf(text[MENU_DIAL], 48, "Dial with the knob  (%+d kHz)", (int)(dial_offset_hz / 1000));
    snprintf(text[MENU_DIAL_BACK], 48, "Dial back to the band's");
    snprintf(text[MENU_BAND_UP], 48, "Band up");
    snprintf(text[MENU_BAND_DOWN], 48, "Band down");
    snprintf(text[MENU_DX], 48, "DX call...  (%s)", (dx && *dx) ? dx : "none");
    snprintf(text[MENU_RST], 48, "RST sent: %s  (press: next)", digi_qso_rst_sent());
    snprintf(text[MENU_NEW_QSO], 48, "New QSO (clear DX call, RST)");
    snprintf(text[MENU_CLEAR], 48, "Clear the text");
    for (int i = 0; i < MENU_COUNT; i++)
        items[i] = text[i];
    digi_menu_open(dialog.obj, "Menu", items, MENU_COUNT, menu_pick, NULL, NULL);
}

/* ---- QSO fields ------------------------------------------------------- */

static bool dx_ok_cb(void)
{
    const char *t = textarea_window_get();

    if (alive && t && *t) {
        digi_qso_set_dx(t);
        refresh_btn(&btn_dx);
    }
    input_end();
    return true;
}

static void dx_press_cb(button_data_t *b)
{
    const char *c = digi_qso_dx();

    (void)b;
    if (input_active)
        return;

    input_begin();
    textarea_window_open_w_label(dx_ok_cb, input_cancel_cb, "DX call:");
    if (c && *c)
        textarea_set_copy(c);
}

static void dx_hold_cb(button_data_t *b)
{
    digi_qso_clear();
    msg_update_text_fmt("#FFFFFF QSO cleared");
    buttons_refresh(b);
    refresh_btn(&btn_rst);
}

/* Only the strength digit changes; readability and tone stay 5 and 9. */
static void rst_press_cb(button_data_t *b)
{
    digi_qso_change_rst_sent(1);
    buttons_refresh(b);
}

static void rst_hold_cb(button_data_t *b)
{
    for (int i = 0; i < 9 && digi_qso_change_rst_sent(0) != 9; i++)
        digi_qso_change_rst_sent(1);
    buttons_refresh(b);
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

    if (!alive || !text_box)
        return;

    lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(text_box, 0),
                       LV_OPA_COVER);
    lv_anim_start(&fade);
}

static void reveal_waterfall(void)
{
    if (!fade_run) {
        fade_run = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(text_box, 0),
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

/* ---- Dialog callbacks ------------------------------------------------- */

/* Main encoder: coarse, then snap onto the signal once it stops. */
static void rotary_cb(int32_t diff)
{
    int32_t mag = diff < 0 ? -diff : diff;
    int32_t step = ROTARY_HZ * (mag > 6 ? 6 : (mag > 3 ? 3 : 1));

    set_center((int)psk_tx_get_center() + (int)(diff * step));
    quiet_since = lv_tick_get();
    reveal_waterfall();

    if (snap_timer) {
        lv_timer_reset(snap_timer);
    } else {
        snap_timer = lv_timer_create(snap_timer_cb, SNAP_DELAY_MS, NULL);
        lv_timer_set_repeat_count(snap_timer, 1);
    }
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;

        case LV_KEY_ENTER:
            next_press_cb(NULL);        /* the MFK sends what Next says */
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;

        /* Multifunction knob: squelch or VFO while the SQL or Dial
         * button has it, otherwise 1 Hz fine tuning, no snap. */
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT: {
            int d = (key == LV_KEY_RIGHT) ? 1 : -1;

            if (knob == KNOB_SQL) {
                sql_change(d);
            } else if (knob == KNOB_DIAL) {
                dial_change(d);
            } else {
                set_center((int)psk_tx_get_center() + d);
                quiet_since = lv_tick_get();
            }
            break;
        }
    }
}

static void construct_cb(lv_obj_t *parent)
{
    dialog.obj = dialog_init(parent);

    /* The window covers the main screen entirely, so its spectrum and
     * waterfall would be computed for nothing. */
    waterfall_set_enabled(false);
    spectrum_set_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    buttons_load_page(&page_1);

    /* Waterfall */

    waterfall = lv_waterfall_create(dialog.obj);

    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);

    lv_waterfall_set_palette(waterfall, (lv_color_t *)style.wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, WF_HEIGHT);

    /* dB above the noise floor of each line, so fixed limits hold
     * whatever the AF and RF gain are. */
    lv_waterfall_set_min(waterfall, -3);
    lv_waterfall_set_max(waterfall, 25);

    lv_obj_set_pos(waterfall, 13, 13);

    /* Decoder position */

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

    lv_finder_clear_cursor(finder);

    /* Red frame while transmitting, the waterfall stopped */

    tx_frame = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(tx_frame);
    lv_obj_set_pos(tx_frame, 13, 13);
    lv_obj_set_size(tx_frame, WIDTH, WF_HEIGHT);
    lv_obj_set_style_border_width(tx_frame, 3, 0);
    lv_obj_set_style_border_color(tx_frame, lv_color_hex(0xFF1010), 0);
    lv_obj_set_style_border_opa(tx_frame, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tx_frame, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tx_frame, LV_OBJ_FLAG_HIDDEN);
    wf_tx = false;
    wf_restart();

    /* Received text, over the lower part of the waterfall */

    text_box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(text_box);
    lv_obj_set_pos(text_box, 13, 13 + TEXT_TOP);
    lv_obj_set_size(text_box, WIDTH, WF_HEIGHT - TEXT_TOP);
    lv_obj_set_style_bg_color(text_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(text_box, TEXT_PAD, 0);
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(tx_frame);    /* the red frame over the text, too */

    txt_font = param_i_get(cfg.ui.digi_compact()) ? &sony_28 : &sony_36;

    for (int i = 0; i < TXT_SHOW; i++) {
        lv_obj_t *lbl = lv_label_create(text_box);

        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, WIDTH - 2 * TEXT_PAD);
        lv_obj_set_style_text_font(lbl, txt_font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_text(lbl, "");
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        line_lbl[i] = lbl;
    }

    lv_obj_add_event_cb(text_box, key_cb, LV_EVENT_KEY, NULL);

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, text_box);
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, fade_anim);
    lv_anim_set_ready_cb(&fade, fade_ready);
    fade_run = false;

    lv_group_add_obj(keyboard_group, text_box);
    lv_group_focus_obj(text_box);
    lv_group_set_editing(keyboard_group, true);

    /* Frequency: remember where the operator was, then move to the
     * PSK31 dial for whichever band is nearest. mem_load() in the
     * destructor puts it all back on the way out. */
    mem_save(MEM_BACKUP_ID);
    load_band(0);

    lm_set_ab(true);
    lm_set_mode(true);
    lm_set_freq(true);
    lm_set_band(true);

    psk_macros_load();
    refresh_macro_labels();

    sql_load();
    sql_open = (sql_thr == 0);
    sql_primed = false;
    knob = KNOB_FINE;
    sql_dirty = false;

    txt_clear();
    input_active = false;
    edit_idx = -1;
    was_tx = false;

    pthread_mutex_lock(&spec_mux);
    spec_ok = (psk_spec_init(&spec, (float)PSK_RX_RATE) == 0);
    pthread_mutex_unlock(&spec_mux);

    alive = true;
    apply_range();

    /* Receiver and transmitter, routed to this window */

    psk_rx_init();
    psk_rx_set_text_cb(rx_text_cb);
    psk_rx_set_audio_tap(rx_tap);
    psk_tx_set_echo_cb(tx_echo_cb);
    psk_rx_set_active(true);

    quiet_since = lv_tick_get();

    status_timer = lv_timer_create(status_timer_cb, TRACK_PERIOD_MS, NULL);
    text_timer = lv_timer_create(text_timer_cb, 100, NULL);
}

static void del_timer(lv_timer_t **t)
{
    if (*t) {
        lv_timer_del(*t);
        *t = NULL;
    }
}

static void destruct_cb(void)
{
    digi_menu_close();
    alive = false;
    tx_frame = NULL;            /* deleted with the window */

    del_timer(&status_timer);
    del_timer(&text_timer);
    del_timer(&snap_timer);
    del_timer(&fade_timer);
    del_timer(&focus_timer);
    del_timer(&editor_timer);
    del_timer(&knob_timer);

    knob = KNOB_FINE;
    btn_sql.mark = false;
    btn_dial.mark = false;
    sql_save();

    if (input_active) {
        textarea_window_close();
        input_active = false;
    }

    /*
     * Let a transmission finish its postamble before the frequency is
     * put back, or the radio would retune while keyed. The stop request
     * cuts it to 16 bits, about half a second.
     */
    psk_tx_stop();
    for (int i = 0; i < 100 && psk_tx_is_on(); i++)
        usleep(20000);

    /* After set_active(false) returns, neither hook runs again. */
    psk_rx_set_active(false);
    psk_rx_set_text_cb(NULL);
    psk_rx_set_audio_tap(NULL);
    psk_tx_set_echo_cb(NULL);

    pthread_mutex_lock(&spec_mux);
    if (spec_ok)
        psk_spec_free(&spec);
    spec_ok = false;
    pthread_mutex_unlock(&spec_mux);

    waterfall = NULL;
    finder = NULL;
    text_box = NULL;
    memset(line_lbl, 0, sizeof(line_lbl));

    waterfall_set_enabled(true);
    spectrum_set_enabled(true);

    mem_load(MEM_BACKUP_ID);

    lm_set_ab(false);
    lm_set_mode(false);
    lm_set_freq(false);
    lm_set_band(false);

    /* The receiver is off now, so this hides the main screen's text
     * panel, which would otherwise stay up with nothing to show. */
    panel_update_visibility(true);
}
