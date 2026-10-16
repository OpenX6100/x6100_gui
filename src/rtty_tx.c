/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
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
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "panel.h"
#include "radio.h"
#include "rtty.h"
#include "rtty_buttons.h"
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
static volatile bool   tx_on   = false;
static volatile bool   tx_stop_req = false;
static char           *tx_text = NULL;
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

/* Encode ASCII text into a half-bit stream with LTRS/FIGS shifting. */
static bool baudot_encode(const char *text, halfbits_t *hb, float baud) {
    bool figs = false;

    /* preamble: steady mark + LTRS LTRS to arm remote decoders */
    size_t pre_hb = (size_t)(PREAMBLE_MS / 1000.0f * baud * 2.0f + 0.5f);
    if (!hb_push(hb, 1, pre_hb)) return false;
    if (!hb_push_code(hb, LTRS_CODE)) return false;
    if (!hb_push_code(hb, LTRS_CODE)) return false;

    for (const char *p = text; *p; p++) {
        char c = toupper((unsigned char)*p);

        if (c == '\r') continue;
        if (c == '\n') {
            if (!hb_push_code(hb, CR_CODE)) return false;
            if (!hb_push_code(hb, LF_CODE)) return false;
            continue;
        }
        if (c == ' ') {
            if (!hb_push_code(hb, SPACE_CODE)) return false;
            continue;
        }

        int idx = table_find(tx_letters, c);
        if (idx >= 0) {
            if (figs) {
                if (!hb_push_code(hb, LTRS_CODE)) return false;
                figs = false;
            }
            if (!hb_push_code(hb, (uint8_t)idx)) return false;
            continue;
        }
        idx = table_find(tx_symbols, c);
        if (idx >= 0) {
            if (!figs) {
                if (!hb_push_code(hb, FIGS_CODE)) return false;
                figs = true;
            }
            if (!hb_push_code(hb, (uint8_t)idx)) return false;
            continue;
        }
        /* unsupported char -> space keeps timing readable */
        if (!hb_push_code(hb, SPACE_CODE)) return false;
    }

    size_t tail_hb = (size_t)(TAIL_MS / 1000.0f * baud * 2.0f + 0.5f);
    return hb_push(hb, 1, tail_hb);
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
            correction = log10f(log10f(11.1f - alc)) * 20.0f - 0.38f;
        } else if (target_pwr - pwr > 0.5f) {
            correction = log10f(target_pwr / (pwr + 0.01f)) * 10.0f;
        }
    }
    return correction;
}

/* --- Worker thread --------------------------------------------------- */

static void *tx_thread_fn(void *arg) {
    (void)arg;

    const float fs   = (float)AUDIO_PLAY_RATE;
    const float baud = (float)param_i_get(cfg.rtty.rate()) / 100.0f;

    halfbits_t hb = {0};
    if (!baudot_encode(tx_text, &hb, baud)) {
        free(hb.items);
        free(tx_text);
        tx_text = NULL;
        tx_on = false;
        return NULL;
    }

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
    audio_player_t *player = audio_get_player(AUDIO_PLAY_RATE, 1);

    if (!player) {
        free(hb.items);
        free(tx_text);
        tx_text = NULL;
        tx_on = false;
        return NULL;
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

    float gain_offset      = base_gain_offset + param_f_get(cfg.ft8.output_gain_offset());
    float play_gain_offset = audio_set_play_vol(gain_offset + 6.0f);
    gain_offset           -= play_gain_offset;

    rtty_set_state(RTTY_TX);
    radio_set_modem(true);

    const double spb_samples = (double)fs / ((double)baud * 2.0); /* samples per half-bit */
    const size_t ramp_n      = (size_t)(RAMP_MS / 1000.0f * fs);
    const size_t total_samples = (size_t)((double)hb.size * spb_samples);

    int16_t chunk[CHUNK_SAMPLES];
    float   phase            = 0.0f;
    float   prev_gain_offset = gain_offset;
    size_t  sample_idx       = 0;
    size_t  counter          = 0;
    double  edge_acc         = 0.0;
    size_t  hb_idx           = 0;
    size_t  next_edge        = 0;

    while (sample_idx < total_samples && !tx_stop_req) {
        if (counter > 30) {
            gain_offset += get_correction() * 0.4f;
            if (gain_offset > GAIN_MAX_DB) gain_offset = GAIN_MAX_DB;
            if (gain_offset < GAIN_MIN_DB) gain_offset = GAIN_MIN_DB;
        }

        size_t part = LV_MIN(CHUNK_SAMPLES, total_samples - sample_idx);

        for (size_t i = 0; i < part; i++, sample_idx++) {
            while (sample_idx >= next_edge && hb_idx < hb.size) {
                edge_acc += spb_samples;
                next_edge = (size_t)(edge_acc + 0.5);
                hb_idx++;
            }
            uint8_t sym  = (hb_idx > 0 && hb_idx <= hb.size) ? hb.items[hb_idx - 1] : 1;
            float   f    = sym ? f_mark : f_space;

            phase += 2.0f * (float)M_PI * f / fs;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;

            float env = 1.0f;
            if (sample_idx < ramp_n) {
                env = 0.5f - 0.5f * cosf((float)M_PI * (float)sample_idx / (float)ramp_n);
            } else if (total_samples - sample_idx <= ramp_n) {
                env = 0.5f - 0.5f * cosf((float)M_PI * (float)(total_samples - sample_idx) / (float)ramp_n);
            }

            chunk[i] = (int16_t)(sinf(phase) * AMPLITUDE * env);
        }

        if (gain_offset == prev_gain_offset) {
            if (gain_offset != 0.0f) {
                audio_gain_db(chunk, part, gain_offset, chunk);
            }
        } else {
            audio_gain_db_transition(chunk, part, prev_gain_offset, gain_offset, chunk);
            prev_gain_offset = gain_offset;
        }

        audio_player_send(player, chunk, part);
        counter++;
    }

    audio_player_wait(player);
    radio_set_modem(false);
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    if (saved_pwr > MAX_PWR_W) {
        radio_set_pwr(saved_pwr);
    }
    audio_player_release(player);

    /* Back to receive - unless the application was closed meanwhile,
     * in which case the state is RTTY_OFF and must stay so. */
    if (rtty_get_state() == RTTY_TX) {
        rtty_set_state(RTTY_RX);
    }

    free(hb.items);
    free(tx_text);
    tx_text = NULL;
    tx_stop_req = false;
    tx_on = false;
    return NULL;
}

/* --- Public API ------------------------------------------------------ */

bool rtty_tx_send(const char *text) {
    if (tx_on || !text || !*text) {
        return false;
    }

    tx_text = strdup(text);
    if (!tx_text) {
        return false;
    }

    tx_on       = true;
    tx_stop_req = false;

    /* Show it in the panel so sent and received text read as one QSO. */
    panel_add_tx_text(text);

    if (pthread_create(&tx_thread, NULL, tx_thread_fn, NULL) != 0) {
        free(tx_text);
        tx_text = NULL;
        tx_on   = false;
        return false;
    }
    pthread_detach(tx_thread);
    return true;
}

void rtty_tx_stop(void) {
    if (tx_on) {
        tx_stop_req = true;
    }
}

bool rtty_tx_is_on(void) {
    return tx_on;
}

/* --- Simple text entry UI -------------------------------------------- */

static bool input_ok_cb(void) {
    const char *text = textarea_window_get();

    if (text && *text) {
        if (rtty_tx_send(text)) {
            msg_update_text_fmt("#FFFFFF RTTY TX...");
        } else {
            msg_update_text_fmt("#FF0000 RTTY TX busy");
        }
    }
    input_active = false;
    main_screen_keys_enable(true);
    return true;
}

static bool input_cancel_cb(void) {
    input_active = false;
    main_screen_keys_enable(true);
    return true;
}

void rtty_tx_open_input(void) {
    if (tx_on) {
        msg_update_text_fmt("#FF0000 RTTY TX busy");
        return;
    }
    if (input_active) return;              /* a window is already open */
    input_active = true;
    /* release the spectrum's grip on the input group, then open */
    main_screen_keys_enable(false);
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
static bool         editing_name = false;  /* two-step add/edit state       */
static char         edit_name_buf[RTTY_MACRO_NAME_LEN];

static void seed_default_macros(void) {
    macro_count = 0;
    rtty_macro_add("CQ",     "CQ CQ CQ DE <MYCALL> <MYCALL> <MYCALL> PSE K");
    rtty_macro_add("Report", "<CALL> DE <MYCALL> UR RST <RST> <RST> QTH ");
    rtty_macro_add("73",     "TNX FER QSO 73 73 DE <MYCALL> SK");
    rtty_macro_add("RY",     "RYRYRYRYRYRYRYRY");
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

/* Replace <CALL>/<MYCALL>/<RST> tokens with current values. */
static void expand_macro(const char *in, char *out, size_t out_sz) {
    const char *mycall = PARAM_T_GET(cfg.callsign());
    const char *call = digi_qso_dx();
    const char *rst  = digi_qso_rst_sent();

    size_t o = 0;
    for (const char *p = *in ? in : ""; *p && o + 1 < out_sz; ) {
        if (strncmp(p, "<CALL>", 6) == 0) {
            for (const char *s = call; *s && o + 1 < out_sz; s++) out[o++] = *s;
            p += 6;
        } else if (strncmp(p, "<MYCALL>", 8) == 0) {
            for (const char *s = mycall ? mycall : ""; *s && o + 1 < out_sz; s++) out[o++] = *s;
            p += 8;
        } else if (strncmp(p, "<RSTR>", 6) == 0) {
            for (const char *s = digi_qso_rst_rcvd(); *s && o + 1 < out_sz; s++) {
                out[o++] = *s;
            }
            p += 6;
        } else if (strncmp(p, "<RST>", 5) == 0) {
            for (const char *s = rst; *s && o + 1 < out_sz; s++) out[o++] = *s;
            p += 5;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

void rtty_macro_expand(int idx, char *out, size_t size) {
    rtty_macro_t *m = rtty_macro_get(idx);

    if (size == 0) return;
    out[0] = '\0';
    if (m) expand_macro(m->text, out, size);
}

bool rtty_macro_send(int idx) {
    rtty_macro_t *m = rtty_macro_get(idx);
    if (!m) return false;

    char expanded[RTTY_MACRO_TEXT_LEN + 32];
    expand_macro(m->text, expanded, sizeof(expanded));
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
    if (macro_edit_idx >= 0) {
        rtty_macro_set(macro_edit_idx, NULL, text);
    } else {
        rtty_macro_add(edit_name_buf, text ? text : "");
    }
    rtty_macros_save();
    rtty_refresh_macro_labels();      /* update button captions live */
    macro_edit_idx = -1;
    editing_name = false;
    input_active = false;
    main_screen_keys_enable(true);
    msg_update_text_fmt("#FFFFFF Macro saved");
    return true;   /* framework closes this window cleanly */
}

static bool macro_edit_cancel_cb(void) {
    macro_edit_idx = -1;
    editing_name = false;
    input_active = false;
    main_screen_keys_enable(true);
    return true;
}

static bool macro_name_ok_cb(void) {
    const char *name = textarea_window_get();
    strncpy(edit_name_buf, name ? name : "", sizeof(edit_name_buf) - 1);
    edit_name_buf[sizeof(edit_name_buf) - 1] = '\0';

    editing_name = false;
    /* Return true so the framework closes THIS window; then a one-shot
     * timer opens the text window once this one is gone. Opening it here
     * inline would leave two textareas in the group and misdirect keys.
     * input_active stays true across the chain. */
    defer_open_text_window();
    return true;
}

/* Open editor: idx < 0 to create new, >= 0 to edit existing. */
static void macro_edit_open(int idx) {
    if (tx_on) { msg_update_text_fmt("#FF0000 RTTY TX busy"); return; }
    if (input_active) return;

    input_active = true;
    macro_edit_idx = idx;
    main_screen_keys_enable(false);

    if (idx >= 0) {
        /* editing existing: skip straight to text, keep the name */
        editing_name = false;
        defer_open_text_window();
    } else {
        editing_name = true;
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
    input_active = false;
    main_screen_keys_enable(true);
    return true;
}

void rtty_qso_open_dx_input(void) {
    if (input_active) return;
    input_active = true;
    main_screen_keys_enable(false);
    textarea_window_open_w_label(dx_ok_cb, input_cancel_cb, "DX call:");
    if (digi_qso_dx()[0]) textarea_window_set(digi_qso_dx());
}
