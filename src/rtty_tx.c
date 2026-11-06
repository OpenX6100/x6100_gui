/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - RTTY transmitter
 */

#include "rtty_tx.h"
#include "digi_qso.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "lvgl/lvgl.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "digi_text.h"
#include "digi_txq.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "panel.h"
#include "radio.h"
#include "rtty.h"
#include "rtty_buttons.h"
#include "scheduler.h"
#include "textarea_window.h"
#include "tx_info.h"

/* Defined in main_screen.c but not declared in its header. */
void main_screen_keys_enable(bool value);

/* main_screen_keys_enable(false) releases the spectrum's hold on the
 * keyboard group so the textarea can receive keystrokes; (true) restores.
 *
 * The stock text window takes 64 characters; macros hold up to 159, so
 * the windows opened here are widened to that. */
#define INPUT_MAX_LEN    (RTTY_MACRO_TEXT_LEN - 1)

/* RTTY has a 100% duty cycle - keep the FT8 thermal cap. */
#define MAX_PWR_W        5.0f
#define GAIN_MIN_DB     (-30.0f)
#define GAIN_MAX_DB      0.0f

#define AMPLITUDE        30000.0f
#define RAMP_MS          5.0f
#define PREAMBLE_MS      300.0f     /* carrier (mark) before data          */
#define TAIL_MS          100.0f     /* carrier (mark) after the last stop  */
#define CHUNK_SAMPLES    2048

#define LTRS_CODE        0b11111
#define FIGS_CODE        0b11011
#define CR_CODE          0b01000
#define LF_CODE          0b00010
#define SPACE_CODE       0b00100

/* Same ITA2 tables as the decoder in rtty.c */
static const char tx_letters[32] = {'\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\0', 'D', 'R',
                                    'J',  'N', 'F',  'C', 'K', 'T', 'Z', 'L', 'W',  'H', 'Y',
                                    'P',  'Q', 'O',  'B', 'G', ' ', 'M', 'X', 'V',  ' '};

static const char tx_symbols[32] = {'\0', '3', '\n', '-', ' ', '\0', '8', '7', '\0', '$', '4',
                                    '\'', ',', '!',  ':', '(', '5',  '\"', ')', '2',  '#', '6',
                                    '0',  '1', '9',  '?', '&', ' ',  '.', '/', ';',  ' '};

typedef struct {
    uint8_t *items;     /* 1 = mark, 0 = space, each item = half a bit */
    size_t   size;
    size_t   cap;
} halfbits_t;

static pthread_t       tx_thread;
static digi_txq_t      txq = DIGI_TXQ_INITIALIZER;
static bool            input_active = false;  /* a textarea window is open */

/* --- Baudot encoder ------------------------------------------------- */

static int table_find(const char *table, char c) {
    for (int i = 0; i < 32; i++) {
        if (table[i] == c && table[i] != '\0') {
            return i;
        }
    }
    return -1;
}

static bool hb_push(halfbits_t *hb, uint8_t v, size_t n) {
    if (hb->size + n > hb->cap) {
        size_t new_cap = (hb->cap ? hb->cap * 2 : 1024);
        while (new_cap < hb->size + n) new_cap *= 2;
        uint8_t *p = realloc(hb->items, new_cap);
        if (!p) return false;
        hb->items = p;
        hb->cap   = new_cap;
    }
    memset(hb->items + hb->size, v, n);
    hb->size += n;
    return true;
}

/* start bit (space, 2 half-bits) + 5 data bits LSB first (2 each)
 * + stop (mark, 3 half-bits = 1.5 bit) */
static bool hb_push_code(halfbits_t *hb, uint8_t code) {
    if (!hb_push(hb, 0, 2)) return false;
    for (int b = 0; b < 5; b++) {
        if (!hb_push(hb, (code >> b) & 1, 2)) return false;
    }
    return hb_push(hb, 1, 3);
}

/* One character into half-bits, shifting between LTRS and FIGS as
 * needed. A line break goes out as CR LF; a CR on its own is dropped. */
static bool encode_char(halfbits_t *hb, bool *figs, char ch) {
    char c = (char)toupper((unsigned char)ch);
    int  idx;

    if (c == '\r') return true;
    if (c == '\n') {
        return hb_push_code(hb, CR_CODE) && hb_push_code(hb, LF_CODE);
    }
    if (c == ' ') {
        /* Receivers with unshift-on-space (MMTTY's and fldigi's default)
         * go back to letters here: a figure after a space needs FIGS
         * again, or "599 599" prints "599 TOO". */
        *figs = false;
        return hb_push_code(hb, SPACE_CODE);
    }
    idx = table_find(tx_letters, c);
    if (idx >= 0) {
        if (*figs) {
            if (!hb_push_code(hb, LTRS_CODE)) return false;
            *figs = false;
        }
        return hb_push_code(hb, (uint8_t)idx);
    }
    idx = table_find(tx_symbols, c);
    if (idx >= 0) {
        if (!*figs) {
            if (!hb_push_code(hb, FIGS_CODE)) return false;
            *figs = true;
        }
        return hb_push_code(hb, (uint8_t)idx);
    }
    /* unsupported char -> space keeps timing readable */
    *figs = false;
    return hb_push_code(hb, SPACE_CODE);
}

typedef enum { PH_PREAMBLE, PH_TEXT, PH_TAIL } phase_t;

typedef struct {
    phase_t ph;
    bool    figs;
    float   baud;
} encoder_t;

/*
 * The next few half-bits, taken from the queue as the transmission goes,
 * so that text added meanwhile joins it: steady mark and LTRS LTRS to
 * arm remote decoders, the characters, LTRS while the operator is still
 * typing (the usual RTTY idle), then a tail of mark. Text that arrives
 * during the tail carries on, after the current shift is sent again.
 * Returns false when the transmission is over.
 */
static bool next_halfbits(halfbits_t *hb, encoder_t *e) {
    int c;

    hb->size = 0;
    switch (e->ph) {
        case PH_PREAMBLE:
            e->ph = PH_TEXT;
            e->figs = false;
            return hb_push(hb, 1, (size_t)(PREAMBLE_MS / 1000.0f * e->baud * 2.0f + 0.5f)) &&
                   hb_push_code(hb, LTRS_CODE) && hb_push_code(hb, LTRS_CODE);

        case PH_TEXT:
            c = digi_txq_get(&txq);
            if (c >= 0) {
                return encode_char(hb, &e->figs, (char)c);
            }
            if (digi_txq_held(&txq)) {
                e->figs = false;
                return hb_push_code(hb, LTRS_CODE);
            }
            e->ph = PH_TAIL;
            return hb_push(hb, 1, (size_t)(TAIL_MS / 1000.0f * e->baud * 2.0f + 0.5f));

        case PH_TAIL:
        default:
            if (digi_txq_close_if_empty(&txq)) {
                return false;
            }
            e->ph = PH_TEXT;
            return hb_push_code(hb, e->figs ? FIGS_CODE : LTRS_CODE);
    }
}

/* --- ALC-driven gain correction (same approach as ft8/tx_worker) ---- */

static float get_correction(void) {
    static uint8_t msg_id = 0;
    float correction = 0.0f;
    float pwr        = 0.0f;
    float alc        = 0.0f;

    if (tx_info_refresh(&msg_id, &alc, &pwr, NULL)) {
        float target_pwr = LV_MIN(param_f_get(cfg.pwr()), MAX_PWR_W);
        if (alc > 0.5f) {
            correction = log10f(log10f(11.1f - fminf(alc, 10.0f))) * 20.0f - 0.38f;  /* > 10.1: NaN */
        } else if (target_pwr - pwr > 0.5f) {
            correction = log10f(target_pwr / (pwr + 0.01f)) * 10.0f;
        }
    }
    return correction;
}

/* --- Worker thread --------------------------------------------------- */

/* UI thread: the transmission is over. Unless RTTY was closed meanwhile
 * (RTTY_OFF, which must stay so) or a new transmission has started since,
 * receive again. */
static void tx_done_ui(void *arg) {
    (void)arg;
    if (rtty_get_state() == RTTY_TX && !digi_txq_running(&txq)) {
        rtty_set_state(RTTY_RX);
    }
}

typedef struct {
    audio_player_t *player;
    int16_t         chunk[CHUNK_SAMPLES];
    size_t          n;
    size_t          counter;
    float           fs;
    float           phase;
    float           gain_offset;
    float           prev_gain_offset;
} output_t;

static void output_flush(output_t *o) {
    if (o->n == 0) return;

    if (o->counter > 30) {
        o->gain_offset += get_correction() * 0.4f;
        if (o->gain_offset > GAIN_MAX_DB) o->gain_offset = GAIN_MAX_DB;
        if (o->gain_offset < GAIN_MIN_DB) o->gain_offset = GAIN_MIN_DB;
    }
    if (o->gain_offset == o->prev_gain_offset) {
        if (o->gain_offset != 0.0f) {
            audio_gain_db(o->chunk, o->n, o->gain_offset, o->chunk);
        }
    } else {
        audio_gain_db_transition(o->chunk, o->n, o->prev_gain_offset, o->gain_offset, o->chunk);
        o->prev_gain_offset = o->gain_offset;
    }
    audio_player_send(o->player, o->chunk, o->n);
    o->n = 0;
    o->counter++;
}

/* One sample of tone f, continuous phase, scaled by env (the ramps). */
static void output_sample(output_t *o, float f, float env) {
    o->phase += 2.0f * (float)M_PI * f / o->fs;
    if (o->phase > 2.0f * (float)M_PI) o->phase -= 2.0f * (float)M_PI;

    o->chunk[o->n++] = (int16_t)(sinf(o->phase) * AMPLITUDE * env);
    if (o->n == CHUNK_SAMPLES) {
        output_flush(o);
    }
}

/* One transmission, until the queue closes. False when it could not
 * start. */
static bool transmit(void) {
    const float fs   = (float)AUDIO_PLAY_RATE;
    const float baud = (float)param_i_get(cfg.rtty.rate()) / 100.0f;

    /* Tone mapping mirrors the RX polarity logic in rtty.c:
     * in USB (non-reverse) mark is the upper audio tone. */
    x6100_mode_t mode = (x6100_mode_t)cparam_i_get(cfg.cur.mode());
    bool usb = (mode == x6100_mode_usb) || (mode == x6100_mode_usb_dig);
    bool mark_upper = usb != (param_i_get(cfg.rtty.reverse()) != 0);

    const float center = (float)param_i_get(cfg.rtty.center());
    const float shift  = (float)param_i_get(cfg.rtty.shift());

    float f_mark  = center + (mark_upper ? 1.0f : -1.0f) * shift / 2.0f;
    float f_space = center - (mark_upper ? 1.0f : -1.0f) * shift / 2.0f;

    /* The default 48 kHz mono player is shared and never released, so
     * asking for it costs nothing. */
    output_t out;

    memset(&out, 0, sizeof(out));
    out.fs = fs;
    out.player = audio_get_player(AUDIO_PLAY_RATE, 1);

    if (!out.player) {
        return false;
    }

    /* Power cap and gain baseline - same scheme as the FT8 worker.
     * radio_set_pwr() only touches the hardware, so putting the stored
     * setting back afterwards undoes the cap. */
    const float saved_pwr  = param_f_get(cfg.pwr());
    const float target_pwr = LV_MIN(saved_pwr, MAX_PWR_W);

    if (saved_pwr > MAX_PWR_W) {
        radio_set_pwr(MAX_PWR_W);
    }

    float base_gain_offset;
    if (x6100_control_get_base_ver().rev >= 3) {
        base_gain_offset = -9.4f;
    } else {
        base_gain_offset = -16.4f + log10f(target_pwr) * 10.0f;
    }

    out.gain_offset       = base_gain_offset + param_f_get(cfg.ft8.output_gain_offset());
    float play_gain_offset = audio_set_play_vol(out.gain_offset + 6.0f);
    out.gain_offset      -= play_gain_offset;
    out.prev_gain_offset  = out.gain_offset;

    radio_set_modem(true);

    const double spb_samples = (double)fs / ((double)baud * 2.0); /* samples per half-bit */
    const size_t ramp_n      = (size_t)(RAMP_MS / 1000.0f * fs);

    halfbits_t hb = {0};
    encoder_t  enc = {.ph = PH_PREAMBLE, .figs = false, .baud = baud};
    size_t     sample_idx = 0;
    double     edge_acc   = 0.0;

    while (next_halfbits(&hb, &enc)) {
        for (size_t h = 0; h < hb.size; h++) {
            float  f   = hb.items[h] ? f_mark : f_space;
            size_t end;

            edge_acc += spb_samples;
            end = (size_t)(edge_acc + 0.5);
            for (; sample_idx < end; sample_idx++) {
                float env = 1.0f;

                if (sample_idx < ramp_n) {
                    env = 0.5f - 0.5f * cosf((float)M_PI * (float)sample_idx / (float)ramp_n);
                }
                output_sample(&out, f, env);
            }
        }
    }
    /* key down gently, on the mark tone */
    for (size_t k = 0; k < ramp_n; k++) {
        output_sample(&out, f_mark, 0.5f + 0.5f * cosf((float)M_PI * (float)k / (float)ramp_n));
    }
    output_flush(&out);

    audio_player_wait(out.player);
    radio_set_modem(false);
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    if (saved_pwr > MAX_PWR_W) {
        radio_set_pwr(saved_pwr);
    }
    audio_player_release(out.player);

    free(hb.items);
    return true;
}

/* One transmission after another while text keeps coming in as one
 * ends (digi_txq_finish); text that cannot go out is dropped. */
static void *tx_thread_fn(void *arg) {
    (void)arg;

    while (transmit()) {
        if (!digi_txq_finish(&txq)) break;
    }
    if (digi_txq_running(&txq)) {
        digi_txq_abort(&txq);
        digi_txq_finish(&txq);
    }

    /* Back to receive, on the UI thread where RTTY is closed too: checked
     * and set here, a close in between could be undone (RTTY back on,
     * decoding, with the app closed). Not between transmissions that
     * follow each other: the state stays TX. */
    scheduler_put_noargs(tx_done_ui);
    return NULL;
}

/* --- Public API ------------------------------------------------------ */

bool rtty_tx_send(const char *text) {
    if (!text || !*text) {
        return false;
    }
    /* RTTY closed (a text window left open, say): nothing is sent */
    if (rtty_get_state() == RTTY_OFF) {
        return false;
    }

    switch (digi_txq_put(&txq, text)) {
        case DIGI_TXQ_ADDED:
            break;

        case DIGI_TXQ_STARTED:
            rtty_set_state(RTTY_TX);    /* here, on the UI thread, not in the worker */
            if (pthread_create(&tx_thread, NULL, tx_thread_fn, NULL) != 0) {
                digi_txq_abort(&txq);
                digi_txq_finish(&txq);
                rtty_set_state(RTTY_RX);
                return false;
            }
            pthread_detach(tx_thread);
            break;

        default:                        /* no room */
            return false;
    }

    /* Show it in the panel so sent and received text read as one QSO. */
    panel_add_tx_text(text);
    digi_text_tx(text);
    return true;
}

void rtty_tx_stop(void) {
    digi_txq_abort(&txq);
}

bool rtty_tx_is_on(void) {
    return digi_txq_running(&txq);
}

/* --- Simple text entry UI -------------------------------------------- */

bool rtty_tx_input_begin(void) {
    if (input_active) return false;        /* a window is already open */
    input_active = true;
    /* release the spectrum's grip on the input group */
    main_screen_keys_enable(false);
    return true;
}

void rtty_tx_input_end(void) {
    input_active = false;
    main_screen_keys_enable(true);
}

/* OK: the text joins the transmission if one is running, and only then
 * is the hold let go, so the transmitter never drops in between. */
static bool input_ok_cb(void) {
    const char *text = textarea_window_get();
    bool        was_on = rtty_tx_is_on();

    if (text && *text) {
        if (rtty_tx_send(text)) {
            msg_update_text_fmt(was_on ? "#FFFFFF RTTY: added to the transmission"
                                       : "#FFFFFF RTTY TX...");
        } else {
            msg_update_text_fmt("#FF0000 RTTY: the TX buffer is full");
        }
    }
    digi_txq_hold(&txq, false);
    rtty_tx_input_end();
    return true;
}

static bool input_cancel_cb(void) {
    digi_txq_hold(&txq, false);
    rtty_tx_input_end();
    return true;
}

void rtty_tx_close_input(void) {
    digi_txq_hold(&txq, false);
    if (input_active) {
        textarea_window_close();
        input_active = false;
        main_screen_keys_enable(true);
    }
}

/* Free text. Opened while transmitting, the transmitter idles (LTRS)
 * until the text is sent or the window cancelled. */
void rtty_tx_open_input(void) {
    if (!rtty_tx_input_begin()) return;
    digi_txq_hold(&txq, true);
    textarea_window_open_w_label(input_ok_cb, input_cancel_cb, "RTTY msg:");
    lv_textarea_set_max_length(textarea_window_text(), INPUT_MAX_LEN);
}

/* ==================================================================== *
 *  Macros
 * ==================================================================== */

#define MACROS_PATH "/mnt/rtty_macros.txt"

static rtty_macro_t macros[RTTY_MACRO_MAX];
static int          macro_count = 0;
static int          macro_edit_idx = -1;   /* which macro an edit targets  */
static char         edit_name_buf[RTTY_MACRO_NAME_LEN];

static void seed_default_macros(void) {
    macro_count = 0;
    rtty_macro_add("CQ",     "CQ CQ CQ DE <MYCALL> <MYCALL> <MYCALL> PSE K");
    rtty_macro_add("Report", "<CALL> DE <MYCALL> UR RST <RST> <RST> QTH ");
    rtty_macro_add("73",     "TNX FER QSO 73 73 DE <MYCALL> SK");
    rtty_macro_add("RY",     "RYRYRYRYRYRYRYRY");
    rtty_macro_add("Info",   "<CALL> DE <MYCALL> NAME <MYNAME> QTH <MYQTH> LOC <MYLOC> <CALL> DE <MYCALL> K");
}

void rtty_macros_load(void) {
    FILE *f = fopen(MACROS_PATH, "r");
    macro_count = 0;

    if (!f) {
        seed_default_macros();
        rtty_macros_save();
        return;
    }

    /* format per line: name<TAB>text */
    char line[RTTY_MACRO_NAME_LEN + RTTY_MACRO_TEXT_LEN + 4];
    while (macro_count < RTTY_MACRO_MAX && fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;

        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';

        rtty_macro_t *m = &macros[macro_count];
        strncpy(m->name, line, RTTY_MACRO_NAME_LEN - 1);
        m->name[RTTY_MACRO_NAME_LEN - 1] = '\0';
        strncpy(m->text, tab + 1, RTTY_MACRO_TEXT_LEN - 1);
        m->text[RTTY_MACRO_TEXT_LEN - 1] = '\0';
        macro_count++;
    }
    fclose(f);

    if (macro_count == 0) {
        seed_default_macros();
        rtty_macros_save();
    }
}

bool rtty_macros_save(void) {
    FILE *f = fopen(MACROS_PATH, "w");
    if (!f) return false;
    for (int i = 0; i < macro_count; i++) {
        fprintf(f, "%s\t%s\n", macros[i].name, macros[i].text);
    }
    fclose(f);
    return true;
}

int rtty_macros_count(void) {
    return macro_count;
}

rtty_macro_t *rtty_macro_get(int idx) {
    if (idx < 0 || idx >= macro_count) return NULL;
    return &macros[idx];
}

int rtty_macro_add(const char *name, const char *text) {
    if (macro_count >= RTTY_MACRO_MAX) return -1;
    rtty_macro_t *m = &macros[macro_count];
    strncpy(m->name, name ? name : "", RTTY_MACRO_NAME_LEN - 1);
    m->name[RTTY_MACRO_NAME_LEN - 1] = '\0';
    strncpy(m->text, text ? text : "", RTTY_MACRO_TEXT_LEN - 1);
    m->text[RTTY_MACRO_TEXT_LEN - 1] = '\0';
    return macro_count++;
}

void rtty_macro_set(int idx, const char *name, const char *text) {
    rtty_macro_t *m = rtty_macro_get(idx);
    if (!m) return;
    if (name) {
        strncpy(m->name, name, RTTY_MACRO_NAME_LEN - 1);
        m->name[RTTY_MACRO_NAME_LEN - 1] = '\0';
    }
    if (text) {
        strncpy(m->text, text, RTTY_MACRO_TEXT_LEN - 1);
        m->text[RTTY_MACRO_TEXT_LEN - 1] = '\0';
    }
}

void rtty_macro_delete(int idx) {
    if (idx < 0 || idx >= macro_count) return;
    for (int i = idx; i < macro_count - 1; i++) {
        macros[i] = macros[i + 1];
    }
    macro_count--;
    rtty_macros_save();
}

void rtty_macro_expand(int idx, char *out, size_t size) {
    rtty_macro_t *m = rtty_macro_get(idx);

    if (size == 0) return;
    out[0] = '\0';
    if (m) digi_qso_expand(m->text, out, size, NULL);
}

bool rtty_macro_send(int idx, const char **missing) {
    rtty_macro_t *m = rtty_macro_get(idx);
    char          expanded[RTTY_MACRO_TEXT_LEN + 128];

    if (missing) *missing = NULL;
    if (!m) return false;

    /* the tokens are the same in every keyboard mode: digi_qso.c */
    if (!digi_qso_expand(m->text, expanded, sizeof(expanded), missing)) {
        return false;
    }
    return rtty_tx_send(expanded);
}

/* ---- Macro editor UI (two-step: name, then text) -------------------- */

static bool macro_text_ok_cb(void);        /* forward decl */
static bool macro_edit_cancel_cb(void);    /* forward decl */

/* Deferred opener: runs after the previous window has fully closed, so
 * only one textarea is ever in the keyboard group at a time. */
static void open_text_window_cb(lv_timer_t *t) {
    (void)t;
    textarea_window_open_w_label(macro_text_ok_cb, macro_edit_cancel_cb, "Macro text:");
    lv_textarea_set_max_length(textarea_window_text(), INPUT_MAX_LEN);
    if (macro_edit_idx >= 0) {
        rtty_macro_t *m = rtty_macro_get(macro_edit_idx);
        if (m) textarea_window_set(m->text);
    }
}

static void defer_open_text_window(void) {
    lv_timer_t *timer = lv_timer_create(open_text_window_cb, 50, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

static bool macro_text_ok_cb(void) {
    const char *text = textarea_window_get();
    bool        empty = !text || !*text;

    /* Saving an empty text deletes the macro, as in PSK31: otherwise a
     * full set of slots could never be freed. */
    if (macro_edit_idx >= 0 && empty) {
        rtty_macro_delete(macro_edit_idx);
    } else if (macro_edit_idx >= 0) {
        rtty_macro_set(macro_edit_idx, NULL, text);
    } else if (!empty) {
        rtty_macro_add(edit_name_buf, text);
    }
    rtty_macros_save();
    rtty_refresh_macro_labels();      /* update button captions live */
    msg_update_text_fmt("#FFFFFF %s", macro_edit_idx >= 0 && empty ? "Macro deleted" :
                                       empty ? "Empty macro not saved" : "Macro saved");
    macro_edit_idx = -1;
    input_active = false;
    main_screen_keys_enable(true);
    return true;   /* framework closes this window cleanly */
}

static bool macro_edit_cancel_cb(void) {
    macro_edit_idx = -1;
    input_active = false;
    main_screen_keys_enable(true);
    return true;
}

static bool macro_name_ok_cb(void) {
    const char *name = textarea_window_get();
    strncpy(edit_name_buf, name ? name : "", sizeof(edit_name_buf) - 1);
    edit_name_buf[sizeof(edit_name_buf) - 1] = '\0';

    /* Return true so the framework closes THIS window; then a one-shot
     * timer opens the text window once this one is gone. Opening it here
     * inline would leave two textareas in the group and misdirect keys.
     * input_active stays true across the chain. */
    defer_open_text_window();
    return true;
}

/* Open editor: idx < 0 to create new, >= 0 to edit existing. */
static void macro_edit_open(int idx) {
    if (input_active) return;

    input_active = true;
    macro_edit_idx = idx;
    main_screen_keys_enable(false);

    if (idx >= 0) {
        /* editing existing: skip straight to text, keep the name */
        defer_open_text_window();
    } else {
        textarea_window_open_w_label(macro_name_ok_cb, macro_edit_cancel_cb, "Macro name:");
    }
}

void rtty_macros_open_menu(void) {
    /* "New macro" button: create a new one (two-step name then text). */
    macro_edit_open(-1);
}

/* Hold on a macro button edits that slot; on an empty slot, creates new. */
void rtty_macros_edit_slot(int idx) {
    if (idx >= 0 && idx < macro_count) {
        macro_edit_open(idx);
    } else {
        macro_edit_open(-1);
    }
}


/* ==================================================================== *
 *  QSO state
 * ==================================================================== */

/* The state itself lives in digi_qso.c, shared with other keyboard
 * mode windows. */

/* ---- manual entry of the callsign ---------------------------------- */

static bool dx_ok_cb(void) {
    const char *t = textarea_window_get();
    if (t && *t) digi_qso_set_dx(t);
    rtty_tx_input_end();
    return true;
}

static bool dx_cancel_cb(void) {
    rtty_tx_input_end();
    return true;
}

void rtty_qso_open_dx_input(void) {
    static char buf[16];

    if (!rtty_tx_input_begin()) return;
    textarea_window_open_w_label(dx_ok_cb, dx_cancel_cb, "DX call:");
    if (digi_qso_dx()[0]) {
        /* a copy: the window may write through the pointer it gets */
        snprintf(buf, sizeof(buf), "%s", digi_qso_dx());
        textarea_window_set(buf);
    }
}
