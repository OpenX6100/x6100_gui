/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - PSK31 transmitter
 */

#include "psk_tx.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "main_screen.h"
#include "msg.h"
#include "radio.h"
#include "textarea_window.h"
#include "tx_info.h"

/* Defined in main_screen.c but not declared in its header. */
void main_screen_keys_enable(bool value);

/* PSK31 is a continuous-carrier mode: keep the same thermal cap the FT8
 * worker uses. */
#define MAX_PWR_W        5.0f
#define GAIN_MIN_DB     (-30.0f)
#define GAIN_MAX_DB      0.0f

#define AMPLITUDE        30000.0f
#define CHUNK_SAMPLES    2048       /* 43 ms at 48 kHz */

/* The stock text window takes 64 characters. */
#define INPUT_MAX_LEN    159

/* 31.25 baud exactly (8000/256, the rate the mode is defined at). */
#define PSK_BAUD         31.25
#define PREAMBLE_BITS    32      /* zeros: continuous reversals = idle    */
#define POSTAMBLE_BITS   16      /* ones: unmodulated carrier             */

#define CENTER_MIN       300
#define CENTER_MAX       2700
#define CENTER_DEFAULT   1000

/* Varicode: PSK31 character codes, transmitted left bit first.
 * 0 = phase reversal, 1 = steady carrier; >=2 zeros between chars.
 * Cross-verified: all codes start/end with 1, contain no "00",
 * no duplicates, and the per-length counts match the Fibonacci
 * series (1,1,2,3,5,8,13,21,34,40) given in the specification. */
static const char *const varicode[128] = {
    /*   0         NUL */ "1010101011",
    /*   1         SOH */ "1011011011",
    /*   2         STX */ "1011101101",
    /*   3         ETX */ "1101110111",
    /*   4         EOT */ "1011101011",
    /*   5         ENQ */ "1101011111",
    /*   6         ACK */ "1011101111",
    /*   7         BEL */ "1011111101",
    /*   8          BS */ "1011111111",
    /*   9          HT */ "11101111",
    /*  10          LF */ "11101",
    /*  11          VT */ "1101101111",
    /*  12          FF */ "1011011101",
    /*  13          CR */ "11111",
    /*  14          SO */ "1101110101",
    /*  15          SI */ "1110101011",
    /*  16         DLE */ "1011110111",
    /*  17         DC1 */ "1011110101",
    /*  18         DC2 */ "1110101101",
    /*  19         DC3 */ "1110101111",
    /*  20         DC4 */ "1101011011",
    /*  21         NAK */ "1101101011",
    /*  22         SYN */ "1101101101",
    /*  23         ETB */ "1101010111",
    /*  24         CAN */ "1101111011",
    /*  25          EM */ "1101111101",
    /*  26         SUB */ "1110110111",
    /*  27         ESC */ "1101010101",
    /*  28          FS */ "1101011101",
    /*  29          GS */ "1110111011",
    /*  30          RS */ "1011111011",
    /*  31          US */ "1101111111",
    /*  32          SP */ "1",
    /*  33         '!' */ "111111111",
    /*  34         '"' */ "101011111",
    /*  35         '#' */ "111110101",
    /*  36         '$' */ "111011011",
    /*  37         '%' */ "1011010101",
    /*  38         '&' */ "1010111011",
    /*  39        '\'' */ "101111111",
    /*  40         '(' */ "11111011",
    /*  41         ')' */ "11110111",
    /*  42         '*' */ "101101111",
    /*  43         '+' */ "111011111",
    /*  44         ',' */ "1110101",
    /*  45         '-' */ "110101",
    /*  46         '.' */ "1010111",
    /*  47         '/' */ "110101111",
    /*  48         '0' */ "10110111",
    /*  49         '1' */ "10111101",
    /*  50         '2' */ "11101101",
    /*  51         '3' */ "11111111",
    /*  52         '4' */ "101110111",
    /*  53         '5' */ "101011011",
    /*  54         '6' */ "101101011",
    /*  55         '7' */ "110101101",
    /*  56         '8' */ "110101011",
    /*  57         '9' */ "110110111",
    /*  58         ':' */ "11110101",
    /*  59         ';' */ "110111101",
    /*  60         '<' */ "111101101",
    /*  61         '=' */ "1010101",
    /*  62         '>' */ "111010111",
    /*  63         '?' */ "1010101111",
    /*  64         '@' */ "1010111101",
    /*  65         'A' */ "1111101",
    /*  66         'B' */ "11101011",
    /*  67         'C' */ "10101101",
    /*  68         'D' */ "10110101",
    /*  69         'E' */ "1110111",
    /*  70         'F' */ "11011011",
    /*  71         'G' */ "11111101",
    /*  72         'H' */ "101010101",
    /*  73         'I' */ "1111111",
    /*  74         'J' */ "111111101",
    /*  75         'K' */ "101111101",
    /*  76         'L' */ "11010111",
    /*  77         'M' */ "10111011",
    /*  78         'N' */ "11011101",
    /*  79         'O' */ "10101011",
    /*  80         'P' */ "11010101",
    /*  81         'Q' */ "111011101",
    /*  82         'R' */ "10101111",
    /*  83         'S' */ "1101111",
    /*  84         'T' */ "1101101",
    /*  85         'U' */ "101010111",
    /*  86         'V' */ "110110101",
    /*  87         'W' */ "101011101",
    /*  88         'X' */ "101110101",
    /*  89         'Y' */ "101111011",
    /*  90         'Z' */ "1010101101",
    /*  91         '[' */ "111110111",
    /*  92   backslash */ "111101111",
    /*  93         ']' */ "111111011",
    /*  94         '^' */ "1010111111",
    /*  95         '_' */ "101101101",
    /*  96         '`' */ "1011011111",
    /*  97         'a' */ "1011",
    /*  98         'b' */ "1011111",
    /*  99         'c' */ "101111",
    /* 100         'd' */ "101101",
    /* 101         'e' */ "11",
    /* 102         'f' */ "111101",
    /* 103         'g' */ "1011011",
    /* 104         'h' */ "101011",
    /* 105         'i' */ "1101",
    /* 106         'j' */ "111101011",
    /* 107         'k' */ "10111111",
    /* 108         'l' */ "11011",
    /* 109         'm' */ "111011",
    /* 110         'n' */ "1111",
    /* 111         'o' */ "111",
    /* 112         'p' */ "111111",
    /* 113         'q' */ "110111111",
    /* 114         'r' */ "10101",
    /* 115         's' */ "10111",
    /* 116         't' */ "101",
    /* 117         'u' */ "110111",
    /* 118         'v' */ "1111011",
    /* 119         'w' */ "1101011",
    /* 120         'x' */ "11011111",
    /* 121         'y' */ "1011101",
    /* 122         'z' */ "111010101",
    /* 123         '{' */ "1010110111",
    /* 124         '|' */ "110111011",
    /* 125         '}' */ "1010110101",
    /* 126         '~' */ "1011010111",
    /* 127         DEL */ "1110110101",
};

typedef struct {
    uint8_t *bits;      /* 1 = steady carrier, 0 = phase reversal */
    size_t   size;
    size_t   cap;
} bitstream_t;

static pthread_t     tx_thread;
static volatile bool tx_on = false;
static volatile bool tx_stop_req = false;
static char         *tx_text = NULL;
static bool          input_active = false;
static uint16_t      center_hz = CENTER_DEFAULT;
static psk_tx_echo_cb_t echo_cb = NULL;

const char *psk_tx_varicode(unsigned char c) {
    return (c < 128) ? varicode[c] : NULL;
}

void psk_tx_set_echo_cb(psk_tx_echo_cb_t cb) {
    echo_cb = cb;
}

/* --- Varicode encoder ------------------------------------------------- */

static bool bs_push(bitstream_t *bs, uint8_t v, size_t n) {
    if (bs->size + n > bs->cap) {
        size_t cap = bs->cap ? bs->cap * 2 : 1024;
        while (cap < bs->size + n) cap *= 2;
        uint8_t *p = realloc(bs->bits, cap);
        if (!p) return false;
        bs->bits = p;
        bs->cap  = cap;
    }
    memset(bs->bits + bs->size, v, n);
    bs->size += n;
    return true;
}

static bool bs_push_code(bitstream_t *bs, const char *code) {
    for (const char *p = code; *p; p++) {
        if (!bs_push(bs, (*p == '1') ? 1 : 0, 1)) return false;
    }
    return true;
}

/* text -> bit stream: preamble (zeros), then each character's varicode
 * separated by two zeros, then postamble (ones). */
static bool varicode_encode(const char *text, bitstream_t *bs) {
    if (!bs_push(bs, 0, PREAMBLE_BITS)) return false;

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char c = *p;

        /* Normalise CRLF to a single CR+LF pair; skip stray CR. */
        if (c == '\r') continue;
        if (c > 127) c = '?';           /* varicode is 7-bit */

        if (c == '\n') {
            if (!bs_push_code(bs, varicode[13])) return false;  /* CR */
            if (!bs_push(bs, 0, 2)) return false;
            if (!bs_push_code(bs, varicode[10])) return false;  /* LF */
            if (!bs_push(bs, 0, 2)) return false;
            continue;
        }

        if (!bs_push_code(bs, varicode[c])) return false;
        if (!bs_push(bs, 0, 2)) return false;   /* inter-character gap */
    }

    return bs_push(bs, 1, POSTAMBLE_BITS);
}

/* --- ALC-driven gain correction (same scheme as the FT8 worker) ------ */

static float get_correction(void) {
    static uint8_t msg_id = 0;
    float correction = 0.0f, pwr = 0.0f, alc = 0.0f;

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

/* --- Worker thread ---------------------------------------------------- */

static void *tx_thread_fn(void *arg) {
    (void)arg;

    const double fs = (double)AUDIO_PLAY_RATE;

    bitstream_t bs = {0};
    if (!varicode_encode(tx_text, &bs)) {
        free(bs.bits);
        free(tx_text);
        tx_text = NULL;
        tx_on = false;
        return NULL;
    }

    /* The default 48 kHz mono player is shared and never released. */
    audio_player_t *player = audio_get_player(AUDIO_PLAY_RATE, 1);

    if (!player) {
        free(bs.bits);
        free(tx_text);
        tx_text = NULL;
        tx_on = false;
        return NULL;
    }

    /* radio_set_pwr() only touches the hardware, so putting the stored
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

    radio_set_modem(true);

    const double spb = fs / PSK_BAUD;        /* samples per bit (fractional) */
    int16_t chunk[CHUNK_SAMPLES];

    double phase   = 0.0;                    /* carrier phase, never reset  */
    double w       = 2.0 * M_PI * (double)center_hz / fs;
    float  polarity = 1.0f;                  /* current carrier polarity    */
    float  prev_gain_offset = gain_offset;

    size_t out_n   = 0;                      /* samples emitted in chunk    */
    size_t counter = 0;
    double bit_acc = 0.0;                    /* fractional bit boundary     */
    size_t sample_idx = 0;

    for (size_t i = 0; i < bs.size; i++) {
        uint8_t bit = bs.bits[i];

        /* Number of samples for this bit, tracking the fractional rate. */
        bit_acc += spb;
        size_t next_boundary = (size_t)(bit_acc + 0.5);
        size_t n = next_boundary - sample_idx;

        for (size_t k = 0; k < n; k++) {
            /* Amplitude: constant for a steady bit; a cosine sweeping from
             * +1 to -1 for a reversal, which both tapers the envelope to
             * zero and applies the 180 degree flip. This shaping is what
             * keeps PSK31 narrow. */
            float env;
            if (bit) {
                env = polarity;
            } else {
                env = polarity * (float)cos(M_PI * (double)k / (double)n);
            }

            phase += w;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;

            chunk[out_n++] = (int16_t)(env * AMPLITUDE * (float)sin(phase));

            if (out_n == CHUNK_SAMPLES) {
                if (counter > 30) {
                    gain_offset += get_correction() * 0.4f;
                    if (gain_offset > GAIN_MAX_DB) gain_offset = GAIN_MAX_DB;
                    if (gain_offset < GAIN_MIN_DB) gain_offset = GAIN_MIN_DB;
                }
                if (gain_offset == prev_gain_offset) {
                    if (gain_offset != 0.0f) {
                        audio_gain_db(chunk, out_n, gain_offset, chunk);
                    }
                } else {
                    audio_gain_db_transition(chunk, out_n, prev_gain_offset,
                                             gain_offset, chunk);
                    prev_gain_offset = gain_offset;
                }
                audio_player_send(player, chunk, out_n);
                out_n = 0;
                counter++;
            }
        }

        sample_idx = next_boundary;
        if (!bit) polarity = -polarity;      /* reversal completed */

        /* Stop requested: break out, but let the postamble ones finish so
         * the receiver sees a clean end of transmission. */
        if (tx_stop_req && i + POSTAMBLE_BITS < bs.size) {
            i = bs.size - POSTAMBLE_BITS - 1;
            tx_stop_req = false;
        }
    }

    if (out_n > 0) {
        if (gain_offset != 0.0f) {
            audio_gain_db(chunk, out_n, gain_offset, chunk);
        }
        audio_player_send(player, chunk, out_n);
    }

    audio_player_wait(player);
    radio_set_modem(false);
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    if (saved_pwr > MAX_PWR_W) {
        radio_set_pwr(saved_pwr);
    }
    audio_player_release(player);

    free(bs.bits);
    free(tx_text);
    tx_text = NULL;
    tx_stop_req = false;
    tx_on = false;
    return NULL;
}

/* --- Public API ------------------------------------------------------- */

uint16_t psk_tx_get_center(void) {
    return center_hz;
}

void psk_tx_set_center(uint16_t hz) {
    if (hz < CENTER_MIN) hz = CENTER_MIN;
    if (hz > CENTER_MAX) hz = CENTER_MAX;
    center_hz = hz;
}

bool psk_tx_send(const char *text) {
    if (tx_on || !text || !*text) return false;

    tx_text = strdup(text);
    if (!tx_text) return false;

    tx_on = true;
    tx_stop_req = false;

    if (echo_cb) {
        echo_cb(text);
    }

    if (pthread_create(&tx_thread, NULL, tx_thread_fn, NULL) != 0) {
        free(tx_text);
        tx_text = NULL;
        tx_on = false;
        return false;
    }
    pthread_detach(tx_thread);
    return true;
}

void psk_tx_stop(void) {
    if (tx_on) tx_stop_req = true;
}

bool psk_tx_is_on(void) {
    return tx_on;
}

/* --- Text entry ------------------------------------------------------- */

static bool input_ok_cb(void) {
    const char *text = textarea_window_get();

    if (text && *text) {
        if (psk_tx_send(text)) {
            msg_update_text_fmt("#FFFFFF PSK31 TX...");
        } else {
            msg_update_text_fmt("#FF0000 PSK31 TX busy");
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

void psk_tx_open_input(void) {
    if (tx_on) {
        msg_update_text_fmt("#FF0000 PSK31 TX busy");
        return;
    }
    if (input_active) return;
    input_active = true;
    main_screen_keys_enable(false);
    textarea_window_open_w_label(input_ok_cb, input_cancel_cb, "PSK31 msg:");
    lv_textarea_set_max_length(textarea_window_text(), INPUT_MAX_LEN);
}
