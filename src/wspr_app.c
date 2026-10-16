/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - WSPR application layer
 */

#include "wspr_app.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "dsp.h"
#include "radio.h"
#include "tx_info.h"

#include "wspr_demod.h"
#include "wspr_encode.h"
#include "wspr_mod.h"
#include "wspr_worker.h"

#define GAIN_MIN_DB   (-30.0f)
#define GAIN_MAX_DB     0.0f

#define AMPLITUDE       30000.0f

/*
 * Transmit audio is generated at the decoder's rate and handed to a
 * PulseAudio player of that rate, as the FT8 transmitter does at 6000 Hz;
 * PulseAudio converts to the card's 48 kHz. 12 kHz keeps the per-sample
 * work a quarter of what 48 kHz would cost, and 512 samples is 43 ms per
 * chunk - the same abort latency and ALC cadence as before.
 */
#define TX_RATE         WSPR_DEMOD_RATE
#define CHUNK_SAMPLES   512

/*
 * The receiver always decodes the whole 200 Hz WSPR window, 1400 to
 * 1600 Hz, as WSJT-X does. Only the transmit frequency is set by the
 * operator, anywhere in that window.
 */
#define RX_CENTER       WSPR_APP_RX_CENTER
#define TX_FREQ_MIN     1400
#define TX_FREQ_MAX     1600
#define TX_FREQ_DEFAULT 1500

static pthread_mutex_t  mux;
static bool             ready = false;
static bool             active = false;

static wspr_worker_t   *worker = NULL;
static uint16_t         cfg_tx_freq = TX_FREQ_DEFAULT;

/* 0 = receive only, N = one slot in N. */
static uint8_t          cfg_tx_period = 0;
static uint16_t         cfg_tx_mw = WSPR_PWR_DEFAULT_MW;

static volatile bool    transmitting = false;

/* Audio in: one DSP subscription for the life of the process, switched
 * on and off with the application. */
static uint32_t         audio_sub = AUDIO_SUB_INVALID;

/* Audio out, created when reception starts. */
static audio_player_t  *player = NULL;

/*
 * Raised when the transmission has to end early, because the operator
 * left the application or changed a setting that rebuilds the chain.
 * Without it wspr_worker_destroy() would join a thread sitting in the
 * middle of a 110 s transmission: the interface freezes for that long
 * and the radio stays keyed. Olivia solves it the same way.
 */
static volatile bool    tx_abort = false;

static uint32_t         spot_count = 0;
static uint32_t         slot_spots = 0;     /* spots in the slot just decoded */

static wspr_app_ui_cb_t ui;

void wspr_app_set_ui_cb(const wspr_app_ui_cb_t *cb)
{
    if (cb) {
        ui = *cb;
    } else {
        memset(&ui, 0, sizeof(ui));
    }
}

/* Short human readable notice, e.g. why a slot was skipped. */
static void note(const char *text)
{
    if (ui.on_note)
        ui.on_note(text, ui.ctx);
}

/* ---- Slot scheduling --------------------------------------------------- */

/*
 * A private generator rather than rand(). Two reasons. The library one
 * is never seeded here, so every boot produced the identical sequence,
 * and at a low duty cycle its first two dozen draws all missed - the
 * radio sat silent for over half an hour, every time, in exactly the
 * same way. And seeding the shared generator would change the behaviour
 * of anything else in the process that draws from it.
 */
static uint32_t rng_state = 0;

static uint32_t rnd32(void)
{
    /* xorshift32; more than enough for choosing a slot. */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void rng_seed(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    rng_state = (uint32_t)ts.tv_sec ^ ((uint32_t)ts.tv_nsec << 8) ^
                (uint32_t)lv_tick_get();
    if (rng_state == 0)
        rng_state = 0x1234567u;      /* xorshift cannot start from zero */
}

/* Which slot of the current group transmits, and where we are in it. */
static uint8_t slot_index = 0;
static uint8_t slot_chosen = 0;

static void schedule_reset(void)
{
    slot_index = 0;
    slot_chosen = (cfg_tx_period > 1)
                      ? (uint8_t)(rnd32() % cfg_tx_period)
                      : 0;
}

/*
 * Called once per slot. Returns true when this is the slot to use, and
 * advances to the next group when the current one runs out.
 */
static bool schedule_take_slot(void)
{
    bool mine;

    if (cfg_tx_period == 0)
        return false;

    mine = (slot_index == slot_chosen);

    slot_index++;
    if (slot_index >= cfg_tx_period)
        schedule_reset();

    return mine;
}

/* ---- Power ------------------------------------------------------------- */

/*
 * WSPR carries the power as dBm and only values ending in 0, 3 or 7 are
 * legal, those being the round 1, 2 and 5 milliwatt multiples. Pick the
 * nearest one to what is actually being radiated; reporting a figure
 * the message cannot hold would simply be decoded as something else by
 * every receiver.
 */
static int dbm_from_mw(uint16_t mw)
{
    float exact;
    float best_diff = 1.0e9f;
    int best = 30;
    int d;

    if (mw < 1)
        mw = 1;

    exact = 10.0f * log10f((float)mw);

    for (d = 0; d <= 60; d++) {
        int last = d % 10;
        float diff;

        if (last != 0 && last != 3 && last != 7)
            continue;

        diff = fabsf((float)d - exact);
        if (diff < best_diff) {
            best_diff = diff;
            best = d;
        }
    }

    return best;
}

int wspr_app_tx_dbm(void)
{
    return dbm_from_mw(cfg_tx_mw);
}

/* ---- Message fields ---------------------------------------------------- */

/*
 * WSPR carries a 4 character locator. The station QTH is normally a 6
 * character grid, so only the first four are used; that is what every
 * other WSPR implementation does with a type 1 message.
 */
static void get_locator(char *out, size_t out_sz)
{
    char qth[PARAM_TEXT_MAX];
    size_t i;

    param_t_get_into(cfg.qth(), qth, sizeof(qth));

    out[0] = '\0';
    for (i = 0; i < 4 && i + 1 < out_sz; i++) {
        char c = qth[i];

        if (c == '\0')
            break;
        out[i] = c;
    }
    out[i] = '\0';
}

static void get_callsign(char *out, size_t out_sz)
{
    param_t_get_into(cfg.callsign(), out, out_sz);
}

const char *wspr_app_tx_blocked_reason(void)
{
    char loc[8];
    char call[PARAM_TEXT_MAX];

    get_callsign(call, sizeof(call));
    if (call[0] == '\0')
        return "no callsign";

    get_locator(loc, sizeof(loc));
    if (strlen(loc) != 4)
        return "no locator";

    /*
     * Without a synchronised clock a transmission lands in the wrong
     * window, which is simply interference, and any spot carries a
     * wrong timestamp into the global database.
     */
    if (!wspr_worker_time_is_synced())
        return "clock not synced";

    /*
     * Something else may already hold the transmitter: the operator on
     * the PTT, an ATU cycle, an SWR sweep. Keying on top of that would
     * fight it for the radio.
     */
    if (radio_get_state() != RADIO_RX)
        return "radio busy";

    return NULL;
}

/* ---- Receive ----------------------------------------------------------- */

static void on_spot(const wspr_spot_t *spot, void *ctx)
{
    (void)ctx;
    spot_count++;
    slot_spots++;

    if (ui.on_spot)
        ui.on_spot(spot, ui.ctx);
}

static void on_slot_end(void *ctx)
{
    char text[64];

    (void)ctx;

    snprintf(text, sizeof(text), "Slot decoded: %u spot%s in %.1f s",
             (unsigned)slot_spots, slot_spots == 1 ? "" : "s",
             (double)wspr_worker_last_decode_s());
    note(text);
    slot_spots = 0;

    if (ui.on_slot_end)
        ui.on_slot_end(ui.ctx);
}

static void on_psd(const float *mag_db, int nbins, void *ctx)
{
    (void)ctx;

    if (ui.on_psd)
        ui.on_psd(mag_db, nbins, ui.ctx);
}

/* ---- Transmit ---------------------------------------------------------- */

/*
 * ALC correction, as the other digital modes do it. The target is the
 * power WSPR was told to use, not the operator's normal setting, since
 * that is what the radio has been put on for the duration.
 */
static float get_correction(float target_pwr)
{
    static uint8_t msg_id = 0;
    float correction = 0.0f, pwr = 0.0f, alc = 0.0f;

    if (tx_info_refresh(&msg_id, &alc, &pwr, NULL)) {
        if (alc > 0.5f) {
            correction = log10f(log10f(11.1f - alc)) * 20.0f - 0.38f;
        } else if (target_pwr - pwr > 0.5f) {
            correction = log10f(target_pwr / (pwr + 0.01f)) * 10.0f;
        }
    }
    return correction;
}

/*
 * Send one whole WSPR transmission. This blocks for 110.6 s, which is
 * what the worker's on_tick contract allows. Audio arriving meanwhile
 * is discarded at the next slot boundary.
 *
 * The transmission is generated one symbol at a time. Producing it in
 * one go would need 1.3 million samples at 12 kHz, over 5 MB as floats,
 * which this radio should not spend on a beacon.
 */
static void transmit_message(void)
{
    char loc[8];
    char call[PARAM_TEXT_MAX];
    uint8_t symbols[WSPR_SYMBOLS];
    wspr_mod_t mod;
    float *sym_buf;
    int16_t chunk[CHUNK_SAMPLES];
    size_t out_n = 0;
    size_t counter = 0;
    float gain_offset, play_gain_offset, prev_gain_offset;
    float base_gain_offset;
    float target_pwr;
    float saved_pwr;
    long sym_cap;
    int k;
    int dbm;
    char text[64];

    if (!player) {
        note("TX skipped: no audio player");
        return;
    }

    get_locator(loc, sizeof(loc));
    get_callsign(call, sizeof(call));
    dbm = dbm_from_mw(cfg_tx_mw);

    if (wspr_encode(call, loc, dbm, symbols) != 0)
        return;

    if (wspr_mod_init(&mod, TX_RATE, (double)cfg_tx_freq -
                      1.5 * WSPR_TONE_SPACING_HZ) != 0) {
        return;
    }

    sym_cap = wspr_mod_max_symbol_samples(&mod);
    sym_buf = (float *)malloc(sizeof(float) * sym_cap);
    if (!sym_buf)
        return;

    /*
     * Put the radio on the WSPR power for the duration and remember what
     * it was on before. radio_set_pwr() only touches the hardware, so
     * the operator's stored setting survives untouched and putting it
     * back is enough.
     */
    target_pwr = (float)cfg_tx_mw / 1000.0f;
    saved_pwr = param_f_get(cfg.pwr());
    radio_set_pwr(target_pwr);

    if (x6100_control_get_base_ver().rev >= 3) {
        base_gain_offset = -9.4f;
    } else {
        base_gain_offset = -16.4f + log10f(target_pwr) * 10.0f;
    }

    gain_offset = base_gain_offset + param_f_get(cfg.ft8.output_gain_offset());
    play_gain_offset = audio_set_play_vol(gain_offset + 6.0f);
    gain_offset -= play_gain_offset;
    prev_gain_offset = gain_offset;

    snprintf(text, sizeof(text), "TX %s %s %d dBm (%u mW)",
             call, loc, dbm, (unsigned)cfg_tx_mw);
    note(text);

    transmitting = true;
    radio_set_modem(true);

    for (k = 0; k < WSPR_SYMBOLS && !tx_abort; k++) {
        long n = wspr_mod_symbol(&mod, k, symbols[k], sym_buf);
        long i;

        for (i = 0; i < n; i++) {
            float v = sym_buf[i] * AMPLITUDE;

            if (v > 32767.0f)
                v = 32767.0f;
            if (v < -32768.0f)
                v = -32768.0f;
            chunk[out_n++] = (int16_t)v;

            if (out_n == CHUNK_SAMPLES) {
                if (counter > 30) {
                    gain_offset += get_correction(target_pwr) * 0.4f;
                    if (gain_offset > GAIN_MAX_DB)
                        gain_offset = GAIN_MAX_DB;
                    if (gain_offset < GAIN_MIN_DB)
                        gain_offset = GAIN_MIN_DB;
                }
                if (gain_offset == prev_gain_offset) {
                    if (gain_offset != 0.0f)
                        audio_gain_db(chunk, out_n, gain_offset, chunk);
                } else {
                    audio_gain_db_transition(chunk, out_n, prev_gain_offset,
                                             gain_offset, chunk);
                    prev_gain_offset = gain_offset;
                }
                audio_player_send(player, chunk, out_n);
                out_n = 0;
                counter++;

                /* Checked here rather than per sample: one chunk is
                 * 43 ms, which is a short enough wait for a join. */
                if (tx_abort)
                    break;
            }
        }
    }

    if (out_n > 0 && !tx_abort) {
        if (gain_offset != 0.0f)
            audio_gain_db(chunk, out_n, gain_offset, chunk);
        audio_player_send(player, chunk, out_n);
    }

    audio_player_wait(player);
    radio_set_modem(false);
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    radio_set_pwr(saved_pwr);
    transmitting = false;

    free(sym_buf);
}

static void on_tick(bool tx_window, float sec_into_slot, void *ctx)
{
    const char *blocked;

    (void)sec_into_slot;
    (void)ctx;

    if (!tx_window || cfg_tx_period == 0 || tx_abort)
        return;

    /*
     * The slot is consumed whether or not it turns out to be usable, so
     * that a period of five really means one slot in five rather than
     * one in five of the slots that happened to be clear.
     */
    if (!schedule_take_slot())
        return;

    blocked = wspr_app_tx_blocked_reason();
    if (blocked != NULL) {
        char text[64];

        /* Say why the beacon stayed quiet instead of just staying
         * quiet, which is indistinguishable from a broken radio. */
        snprintf(text, sizeof(text), "TX skipped: %s", blocked);
        note(text);
        return;
    }

    transmit_message();
}

/* ---- Chain setup ------------------------------------------------------- */

static void teardown(void)
{
    if (worker) {
        /* Break off any transmission first: destroy joins the worker
         * thread, and that thread may be 100 s into keying the radio. */
        tx_abort = true;
        wspr_worker_destroy(worker);
        worker = NULL;
        tx_abort = false;
    }
}

static bool setup(void)
{
    wspr_worker_cb_t cb = {
        .on_spot = on_spot,
        .on_slot_end = on_slot_end,
        .on_psd = on_psd,
        .on_tick = on_tick,
        .ctx = NULL,
    };

    teardown();

    worker = wspr_worker_create(WSPR_DEMOD_RATE, (double)RX_CENTER, &cb);
    if (!worker)
        return false;

    if (wspr_worker_start(worker) != 0) {
        teardown();
        return false;
    }

    return true;
}

/* ---- Public API -------------------------------------------------------- */

void wspr_app_init(void)
{
    /* Called every time the window opens; the mutex must be created
     * once and only once. */
    if (ready)
        return;

    pthread_mutex_init(&mux, NULL);
    rng_seed();
    schedule_reset();

    /* The DSP delivers 48 kHz divided by an integer; 12000 is exactly 4.
     * Created inactive, so nothing is spent until the window opens. */
    audio_sub = dsp_audio_subscribe_float(wspr_app_put_audio_samples,
                                          WSPR_DEMOD_RATE);
    dsp_audio_set_active(audio_sub, false);

    ready = true;
}

/*
 * Stop the audio feed before the worker goes away. dsp_audio_set_active()
 * takes the lock the audio callbacks run under, so once it returns no
 * callback is inside wspr_worker_feed() any more and the worker can be
 * freed safely. Never called from the audio callback itself.
 */
static void audio_feed(bool on)
{
    if (audio_sub != AUDIO_SUB_INVALID)
        dsp_audio_set_active(audio_sub, on);
}

void wspr_app_set_active(bool on)
{
    if (!ready)
        return;

    pthread_mutex_lock(&mux);

    if (on && !active) {
        if (!setup()) {
            pthread_mutex_unlock(&mux);
            return;
        }
        if (!player)
            player = audio_get_player(TX_RATE, 1);
        spot_count = 0;
        slot_spots = 0;
        schedule_reset();
        audio_feed(true);
    } else if (!on && active) {
        audio_feed(false);
        teardown();
        if (player) {
            audio_player_release(player);
            player = NULL;
        }
    }

    active = on;
    pthread_mutex_unlock(&mux);
}

bool wspr_app_is_active(void)
{
    return active;
}

void wspr_app_put_audio_samples(size_t n, float *samples)
{
    if (!ready || !active || !worker)
        return;

    /* Never feed our own transmission back into the decoder. */
    if (transmitting)
        return;

    wspr_worker_feed(worker, (unsigned int)n, samples);
}

uint16_t wspr_app_get_tx_freq(void)
{
    return cfg_tx_freq;
}

/* Used at the next transmission; the receiver is not touched. */
void wspr_app_set_tx_freq(uint16_t hz)
{
    if (hz < TX_FREQ_MIN)
        hz = TX_FREQ_MIN;
    if (hz > TX_FREQ_MAX)
        hz = TX_FREQ_MAX;

    pthread_mutex_lock(&mux);
    cfg_tx_freq = hz;
    pthread_mutex_unlock(&mux);
}

uint8_t wspr_app_get_tx_period(void)
{
    return cfg_tx_period;
}

void wspr_app_set_tx_period(uint8_t n)
{
    cfg_tx_period = n;
    schedule_reset();
}

uint16_t wspr_app_get_tx_pwr_mw(void)
{
    return cfg_tx_mw;
}

void wspr_app_set_tx_pwr_mw(uint16_t mw)
{
    if (mw < WSPR_PWR_MIN_MW)
        mw = WSPR_PWR_MIN_MW;
    if (mw > WSPR_PWR_MAX_MW)
        mw = WSPR_PWR_MAX_MW;

    /* Snap to the step, so the reported dBm cannot drift off the grid
     * the button walks along. */
    mw = (uint16_t)((mw / WSPR_PWR_STEP_MW) * WSPR_PWR_STEP_MW);
    if (mw < WSPR_PWR_MIN_MW)
        mw = WSPR_PWR_MIN_MW;

    cfg_tx_mw = mw;
}

bool wspr_app_is_transmitting(void)
{
    return transmitting;
}

float wspr_app_slot_progress(void)
{
    return wspr_worker_slot_progress();
}

uint32_t wspr_app_spot_count(void)
{
    return spot_count;
}
