/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* the thread's name and core */
#endif

#include "worker.h"

#include "gfsk.h"
#include "ft2.h"
#include "msg_encode.h"
#include "contest_msg.h"
#include "subtract.h"

#include "lvgl/lvgl.h"
#include <ft8lib/constants.h>
#include <ft8lib/decode.h>
#include <ft8lib/encode.h>
#include <ft8lib/hashtable.h>
#include <ft8lib/message.h>
#include <liquid/liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_CANDIDATES 200
#define MAX_DECODED_MESSAGES 50
#define LDPC_ITERATIONS 25
#define TIME_OSR 4               // Time oversampling rate (symbol subdivision)
#define FREQ_OSR 2               // Frequency oversampling rate (bin subdivision)
#define FTXLIB_MIN_SCORE_DEFAULT 10
#define FTXLIB_MIN_SCORE_PATH "/mnt/ft8lib_min_score.txt"
#define DECODE_BLOCK_STRIDE 2    // Try to decode each N block
#define EARLY_LDPC_ITERATIONS 25 // LDPC iterations on early decoding

/* The spectrum the decoder searches, and what makes it */
typedef struct {
    ftx_waterfall_t *wf;
    float complex   *time_buf;
    float complex   *freq_buf;
    fftplan          fft;
    windowf          window;
} spectrum_t;

static spectrum_t rx_spec;
static float     *rx_window = NULL;

static float symbol_period;
static int   block_size;
static int   subblock_size;
static int   nfft;

static uint8_t n_tones;  // Number of tones for generate message and check minimal length for rx
static uint8_t sync_num; // Length of sync

/* Decode score threshold (minimum candidate score). Initialised from
 * FTXLIB_MIN_SCORE_PATH on first ftx_worker_init() so a power user can
 * tweak sensitivity vs false-positive rate without a rebuild. The file is
 * (re)created with the default value when missing or unparsable. */
static int             ft8lib_min_score = FTXLIB_MIN_SCORE_DEFAULT;

static void ftx_min_score_write_default(void) {
    FILE *wf = fopen(FTXLIB_MIN_SCORE_PATH, "w");
    if (!wf) return;
    fprintf(wf, "%d\n", FTXLIB_MIN_SCORE_DEFAULT);
    fclose(wf);
}

static void ftx_min_score_load_or_create(void) {
    FILE *f = fopen(FTXLIB_MIN_SCORE_PATH, "r");
    if (!f) {
        if (errno == ENOENT) {
            ftx_min_score_write_default();
        }
        ft8lib_min_score = FTXLIB_MIN_SCORE_DEFAULT;
        return;
    }
    int val = FTXLIB_MIN_SCORE_DEFAULT;
    if (fscanf(f, "%d", &val) != 1)
        val = FTXLIB_MIN_SCORE_DEFAULT;
    fclose(f);
    ft8lib_min_score = val;
    if (val == FTXLIB_MIN_SCORE_DEFAULT)
        ftx_min_score_write_default();
}

static int             num_candidates;
static ftx_candidate_t candidate_list[MAX_CANDIDATES];
static ftx_message_t   decoded[MAX_DECODED_MESSAGES];
static ftx_message_t  *decoded_hashtable[MAX_DECODED_MESSAGES];
static ftx_waterfall_t wf;
static int             find_candidates_at;
static char            own_call[12]; /* kept in the hash table, see remember_own_call() */

/* More passes, as WSJT-X: each signal decoded in a slot is taken out of
 * its audio (subtract.c), the spectrum made again and searched again, so
 * stations sitting next to stronger ones come through. FT8 and FT4; FT2
 * stays single-pass.
 *
 * As JS8's decoder: on a thread of their own, with their own copy of the
 * slot, off the core kept for the screen and below the audio's priority, so
 * however long they take they can't touch the receiver or a transmission.
 * What they find is handed back on the audio thread by
 * ftx_worker_decode_more(). */
#define EXTRA_PASSES      2     /* WSJT-X: 3 passes in all */
#define EXTRA_BUDGET      0.8f  /* of a slot: done before the next one's */
#define EXTRA_NICE        5
typedef struct {
    uint8_t tones[FT4_NN];
    float   freq_hz, time_sec;
    bool    subtracted;
} heard_sig_t;
typedef struct {
    heard_sig_t sig[MAX_DECODED_MESSAGES];
    int         n;
} heard_list_t;
typedef struct {
    char  text[FTX_CONTEST_TEXT_LEN];
    int   snr;
    float freq_hz, time_sec;
} extra_found_t;

static bool    extra_on;
static bool    extra_realtime = true;
static int     rx_rate;
static float   slot_secs;
/* The audio thread's */
static float        *slot_audio;
static int           slot_len, slot_cap;
static heard_list_t  heard;
/* The passes' own, after extra_submit() */
static float           *extra_audio;
static int              extra_len;
static heard_list_t     extra_heard;
static ftx_waterfall_t  extra_wf;
static spectrum_t       extra_spec;
static ftx_candidate_t  extra_candidates[MAX_CANDIDATES];
static int              extra_num_candidates;
static ftx_message_t    extra_decoded[MAX_DECODED_MESSAGES];
static ftx_message_t   *extra_table[MAX_DECODED_MESSAGES];
static struct timespec  extra_started;
/* Shared, under extra_mux */
static pthread_t        extra_thread;
static bool             extra_thread_up;
static pthread_mutex_t  extra_mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   extra_cv  = PTHREAD_COND_INITIALIZER;
static bool             extra_quit, extra_job, extra_busy;
static extra_found_t    extra_found[MAX_DECODED_MESSAGES];
static int              extra_found_n;
/* ft8_lib's callsign hash table: both threads use it */
static pthread_mutex_t  hash_mux = PTHREAD_MUTEX_INITIALIZER;

static void decode_messages(const ftx_waterfall_t *wf, int *num_candidates, ftx_candidate_t *candidate_list,
                            ftx_message_t *decoded, ftx_message_t **decoded_hashtable, int ldpc_iterations,
                            heard_list_t *heard_list, decoded_msg_cb msg_cb, void *user_data);

static int get_message_snr(const ftx_waterfall_t *wf, const ftx_candidate_t *candidate, ftx_message_t *msg);

/*
 * A station answering a non-standard call (KF0QGT/6Y) sends it as a hash:
 * "<KF0QGT/6Y> W1ABC -05". It decodes only with that hash in the table,
 * and the table forgets a call after a few slots. Encoding "CQ <own call>"
 * puts it there; done every slot, so a call to us decodes even before our
 * first transmission and after a long pause.
 */
static void remember_own_call(void) {
    ftx_message_t msg;
    char          text[20];

    if (own_call[0] == '\0') {
        return;
    }
    snprintf(text, sizeof(text), "CQ %s", own_call);
    ftx_message_init(&msg);
    pthread_mutex_lock(&hash_mux);
    ftx_message_encode(&msg, &hash_if, text);
    pthread_mutex_unlock(&hash_mux);
}

void ftx_worker_set_own_call(const char *call) {
    if (call == NULL || strlen(call) >= sizeof(own_call)) {
        own_call[0] = '\0';
        return;
    }
    strcpy(own_call, call);
}

static bool spectrum_init(spectrum_t *sp, ftx_waterfall_t *w) {
    sp->wf       = w;
    sp->time_buf = (float complex *)malloc(nfft * sizeof(float complex));
    sp->freq_buf = (float complex *)malloc(nfft * sizeof(float complex));
    if (!sp->time_buf || !sp->freq_buf) return false;
    sp->fft    = fft_create_plan(nfft, sp->time_buf, sp->freq_buf, LIQUID_FFT_FORWARD, 0);
    sp->window = windowf_create(nfft);
    return true;
}

static void spectrum_free(spectrum_t *sp) {
    if (sp->window) windowf_destroy(sp->window);
    if (sp->fft) fft_destroy_plan(sp->fft);
    free(sp->time_buf);
    free(sp->freq_buf);
    memset(sp, 0, sizeof(*sp));
}

static void *extra_main(void *arg);

/**
 * Init worker
 */
void ftx_worker_init(int sample_rate, ftx_protocol_t protocol) {
    ftx_min_score_load_or_create();
    float slot_period;

    switch ((int)protocol) {
    case FTX_PROTOCOL_FT2:
        /* FT4 at twice the speed (ft2.h) */
        slot_period = FT2_SLOT_TIME;
        symbol_period = FT2_SYMBOL_PERIOD;
        n_tones = FT4_NN;
        sync_num = FT4_NUM_SYNC;
        break;
    case FTX_PROTOCOL_FT8:
        slot_period = FT8_SLOT_TIME;
        symbol_period = FT8_SYMBOL_PERIOD;
        n_tones = FT8_NN;
        sync_num = FT8_NUM_SYNC;
        break;
    case FTX_PROTOCOL_FT4:
        slot_period = FT4_SLOT_TIME;
        symbol_period = FT4_SYMBOL_PERIOD;
        n_tones = FT4_NN;
        sync_num = FT4_NUM_SYNC;
        break;
    default:
        LV_LOG_ERROR("Unsupported protocol: %lu", protocol);
        return;
    }
    int num_samples = slot_period * sample_rate;
    rx_rate   = sample_rate;
    slot_secs = slot_period;

    hashtable_init(256);
    remember_own_call();

    /* FT8 decoder */

    block_size = (int)(sample_rate * symbol_period); // samples corresponding to one FSK symbol
    subblock_size = block_size / TIME_OSR;

    const int max_blocks = (int)(slot_period / symbol_period);
    // TODO: skip bins outside filter
    const int num_bins = sample_rate * symbol_period / 2;

    const size_t mag_stride = (size_t)TIME_OSR * (size_t)FREQ_OSR * (size_t)num_bins * sizeof(WF_ELEM_T);
    if (mag_stride == 0 || (size_t)max_blocks > SIZE_MAX / mag_stride) {
        LV_LOG_ERROR("FT8 worker: mag_size calculation overflow");
        goto error_cleanup;
    }

    size_t mag_size = (size_t)max_blocks * mag_stride;

    wf.max_blocks = max_blocks;
    wf.num_bins = num_bins;
    wf.time_osr = TIME_OSR;
    wf.freq_osr = FREQ_OSR;
    wf.block_stride = TIME_OSR * FREQ_OSR * num_bins;
    wf.mag = (uint8_t *)malloc(mag_size);
    if (!wf.mag) {
        LV_LOG_ERROR("FT8 worker: wf.mag malloc failed");
        goto error_cleanup;
    }
    wf.protocol = ftx_lib_protocol(protocol);   /* FT2 decodes as FT4 */

    find_candidates_at = n_tones - sync_num;

    /* FT8 DSP */
    nfft = block_size * FREQ_OSR;
    float fft_norm = 2.0f / nfft;
    if (!spectrum_init(&rx_spec, &wf)) {
        LV_LOG_ERROR("FT8 worker: time_buf/freq_buf malloc failed");
        goto error_cleanup;
    }

    rx_window = malloc(nfft * sizeof(float));
    if (!rx_window) {
        LV_LOG_ERROR("FT8 worker: rx_window malloc failed");
        goto error_cleanup;
    }
    float window_norm = 2.0f / nfft;

    for (uint16_t i = 0; i < nfft; i++) {
        rx_window[i] = liquid_hann(i, nfft) * window_norm;
    }

    /* More passes: FT8/FT4 only */
    extra_on = false;
    if ((int)protocol != (int)FTX_PROTOCOL_FT2) {
        slot_cap     = (int)(slot_period * sample_rate);
        slot_audio   = malloc(sizeof(float) * slot_cap);
        extra_audio  = malloc(sizeof(float) * slot_cap);
        extra_wf     = wf;
        extra_wf.mag = (uint8_t *)malloc(mag_size);
        extra_on     = slot_audio && extra_audio && extra_wf.mag && spectrum_init(&extra_spec, &extra_wf) &&
                   ftx_subtract_init(block_size, n_tones,
                                     protocol == FTX_PROTOCOL_FT8 ? FT8_SYMBOL_BT : FT4_SYMBOL_BT);
        if (extra_on) {
            extra_quit      = false;
            extra_job       = false;
            extra_busy      = false;
            extra_found_n   = 0;
            extra_thread_up = pthread_create(&extra_thread, NULL, extra_main, NULL) == 0;
            extra_on        = extra_thread_up;
        }
        if (!extra_on) LV_LOG_WARN("FT8 worker: no memory for more passes, one pass only");
    }

    ftx_worker_reset();
    return;

error_cleanup:
    ftx_worker_free();
}

/**
 * Cleanup worker. Safe to call multiple times or on a half-initialised
 * worker (partial allocations are skipped via NULL checks).
 */
void ftx_worker_free() {
    if (extra_thread_up) {
        pthread_mutex_lock(&extra_mux);
        extra_quit = true;
        pthread_cond_signal(&extra_cv);
        pthread_mutex_unlock(&extra_mux);
        pthread_join(extra_thread, NULL);
        extra_thread_up = false;
    }
    ftx_subtract_free();
    free(slot_audio);
    slot_audio = NULL;
    free(extra_audio);
    extra_audio = NULL;
    free(extra_wf.mag);
    extra_wf.mag = NULL;
    spectrum_free(&extra_spec);
    extra_on   = false;
    if (wf.mag) {
        free(wf.mag);
        wf.mag = NULL;
    }
    spectrum_free(&rx_spec);
    if (rx_window) {
        free(rx_window);
        rx_window = NULL;
    }
    hashtable_delete();
}

/**
 * Reset worker
 */
static void forget_slot(void) {
    slot_len = 0;
    heard.n  = 0;
    // Initialize hash table pointers
    for (int i = 0; i < MAX_DECODED_MESSAGES; ++i) {
        decoded_hashtable[i] = NULL;
    }
}

/* The slot just ended goes to the passes: its audio by swapping buffers, its
 * decodes copied. If they're still on the last one this slot goes without. */
static void extra_submit(void) {
    pthread_mutex_lock(&extra_mux);
    if (extra_job || extra_busy) {
        pthread_mutex_unlock(&extra_mux);
        LV_LOG_USER("FT8/FT4 extra passes: still busy, this slot skipped");
        return;
    }
    float *t    = extra_audio;
    extra_audio = slot_audio;
    slot_audio  = t;
    extra_len   = slot_len;
    extra_heard = heard;
    memcpy(extra_decoded, decoded, sizeof(decoded));
    for (int i = 0; i < MAX_DECODED_MESSAGES; i++)
        extra_table[i] = decoded_hashtable[i] ? &extra_decoded[decoded_hashtable[i] - decoded] : NULL;
    extra_found_n = 0;
    extra_job     = true;
    pthread_cond_signal(&extra_cv);
    pthread_mutex_unlock(&extra_mux);
}

void ftx_worker_reset() {
    pthread_mutex_lock(&hash_mux);
    hashtable_cleanup(10);
    pthread_mutex_unlock(&hash_mux);
    remember_own_call();
    wf.num_blocks = 0;
    num_candidates = 0;
    if (extra_on && slot_len >= wf.max_blocks * block_size && heard.n > 0) extra_submit();
    forget_slot();
}

bool ftx_worker_generate_tx_samples(const char *text, const uint16_t signal_freq, const uint32_t sample_rate,
                                    int16_t **samples, uint32_t *n_samples) {
    ftx_message_t msg;
    ftx_message_init(&msg);
    pthread_mutex_lock(&hash_mux);
    ftx_message_rc_t rc = ftx_encode_text(&msg, &hash_if, text);
    pthread_mutex_unlock(&hash_mux);

    if (rc != FTX_MESSAGE_RC_OK) {
        LV_LOG_ERROR("Cannot parse message %i", rc);
        return false;
    }

    uint8_t tones[n_tones];
    float   symbol_bt;

    switch (wf.protocol) {
    case FTX_PROTOCOL_FT8:
        ft8_encode(msg.payload, tones);
        symbol_bt = FT8_SYMBOL_BT;
        break;
    case FTX_PROTOCOL_FT4:
        ft4_encode(msg.payload, tones);
        symbol_bt = FT4_SYMBOL_BT;
        break;
    }

    int16_t *out = gfsk_synth(tones, n_tones, signal_freq, symbol_bt, symbol_period, sample_rate, n_samples);
    if (!out) {
        LV_LOG_ERROR("gfsk_synth allocation failed");
        return false;
    }
    *samples = out;
    return true;
}

static void spectrum_block(spectrum_t *sp, const float *samples);

void ftx_worker_put_rx_samples(float *samples, uint32_t n_samples) {
    if (wf.num_blocks >= wf.max_blocks) {
        LV_LOG_ERROR("FT8 wf is full");
        return;
    }

    if (n_samples != block_size) {
        LV_LOG_ERROR("n_samples(%llu) is not equal expected block size(%llu)", n_samples, block_size);
        return;
    }

    if (extra_on && slot_len + (int)n_samples <= slot_cap) {
        memcpy(slot_audio + slot_len, samples, n_samples * sizeof(float));
        slot_len += n_samples;
    }
    spectrum_block(&rx_spec, samples);
}

/* One block (a symbol) of audio into the spectrum the decoder searches. */
static void spectrum_block(spectrum_t *sp, const float *samples) {
    ftx_waterfall_t *w = sp->wf;
    float *frame_ptr;
    int    offset    = w->num_blocks * w->block_stride;
    int    frame_pos = 0;

    for (int time_sub = 0; time_sub < w->time_osr; time_sub++) {
        windowf_write(sp->window, (float *)&samples[frame_pos], subblock_size);
        frame_pos += subblock_size;

        windowf_read(sp->window, &frame_ptr);

        for (size_t i = 0; i < nfft; i++) {
            sp->time_buf[i] = rx_window[i] * frame_ptr[i];
        }

        fft_execute(sp->fft);

        for (int freq_sub = 0; freq_sub < w->freq_osr; freq_sub++)
            for (int bin = 0; bin < w->num_bins; bin++) {
                int           src_bin = (bin * w->freq_osr) + freq_sub;
                complex float freq = sp->freq_buf[src_bin];
                float         mag2 = crealf(freq * conjf(freq));
                float         db = 10.0f * log10f(mag2);
                int           scaled = (int16_t)(db * 2.0f + 240.0f);

                if (scaled < 0) {
                    scaled = 0;
                } else if (scaled > 255) {
                    scaled = 255;
                }

                w->mag[offset] = scaled;
                offset++;
            }
    }
    w->num_blocks++;
}

void ftx_worker_decode(decoded_msg_cb msg_cb, bool last, void *user_data) {
    if (wf.num_blocks >= find_candidates_at) {
        if (num_candidates == 0) {
            num_candidates = ftx_find_candidates(&wf, MAX_CANDIDATES, candidate_list, ft8lib_min_score);
        } else if (last) {
            // Last decoding
            decode_messages(&wf, &num_candidates, candidate_list, decoded, decoded_hashtable, LDPC_ITERATIONS,
                            extra_on ? &heard : NULL, msg_cb, user_data);
        } else if (wf.num_blocks % DECODE_BLOCK_STRIDE == 0) {
            // incremental decoding
            decode_messages(&wf, &num_candidates, candidate_list, decoded, decoded_hashtable, EARLY_LDPC_ITERATIONS,
                            extra_on ? &heard : NULL, msg_cb, user_data);
        }
    }
}

int ftx_worker_get_block_size() {
    return block_size;
}

bool ftx_worker_is_full() {
    return wf.max_blocks <= wf.num_blocks;
}

static void decode_messages(const ftx_waterfall_t *wf, int *num_candidates, ftx_candidate_t *candidate_list,
                            ftx_message_t *decoded, ftx_message_t **decoded_hashtable, int ldpc_iterations,
                            heard_list_t *heard_list, decoded_msg_cb msg_cb, void *user_data) {
    // Go over candidates and attempt to decode messages

    if (!num_candidates || *num_candidates <= 0) return;

    /* Use fixed-size array sized to MAX_CANDIDATES instead of a VLA based
     * on *num_candidates so a zero value cannot create a zero-length VLA
     * (undefined behaviour). The early-return guard above makes this safe. */
    int to_delete_idx[MAX_CANDIDATES];
    int to_delete_size = 0;

    for (int idx = 0; idx < *num_candidates; ++idx) {
        const ftx_candidate_t *cand = &candidate_list[idx];

        // Skip candidates, that are not fully received
        if ((cand->time_offset + n_tones - sync_num) >= wf->num_blocks) {
            continue;
        }

        to_delete_idx[to_delete_size++] = idx;

        ftx_message_t       message;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(wf, cand, ldpc_iterations, &message, &status)) {
            if (status.ldpc_errors > 0) {
                LV_LOG_INFO("LDPC decode: %d errors", status.ldpc_errors);
            } else if (status.crc_calculated != status.crc_extracted) {
                LV_LOG_INFO("CRC mismatch!");
            }
            continue;
        }

        float freq_hz = (cand->freq_offset + (float)cand->freq_sub / FREQ_OSR) / symbol_period;
        float time_sec = (cand->time_offset + (float)cand->time_sub / TIME_OSR) * symbol_period;

        LV_LOG_INFO("Checking hash table for %4.1fs / %4.1fHz [%d]...", time_sec, freq_hz, cand->score);
        int  idx_hash = message.hash % MAX_DECODED_MESSAGES;
        bool found_empty_slot = false;
        bool found_duplicate = false;
        int  probes = 0;
        do {
            if (decoded_hashtable[idx_hash] == NULL) {
                LV_LOG_INFO("Found an empty slot");
                found_empty_slot = true;
            } else if ((decoded_hashtable[idx_hash]->hash == message.hash) &&
                       (0 == memcmp(decoded_hashtable[idx_hash]->payload, message.payload, sizeof(message.payload)))) {
                LV_LOG_INFO("Found a duplicate!");
                found_duplicate = true;
            } else {
                LV_LOG_INFO("Hash table clash!");
                idx_hash = (idx_hash + 1) % MAX_DECODED_MESSAGES;
                probes++;
            }
        } while (!found_empty_slot && !found_duplicate && probes < MAX_DECODED_MESSAGES);

        if (found_empty_slot) {
            // Fill the empty hashtable slot
            memcpy(&decoded[idx_hash], &message, sizeof(message));
            decoded_hashtable[idx_hash] = &decoded[idx_hash];

            char             text[FTX_CONTEST_TEXT_LEN];
            pthread_mutex_lock(&hash_mux);
            ftx_message_rc_t unpack_status = ftx_message_decode(&message, &hash_if, text);
            /* Contest and DXpedition messages ft8_lib can't unpack */
            if (unpack_status != FTX_MESSAGE_RC_OK && ftx_contest_decode(&message, &hash_if, text, sizeof(text)))
                unpack_status = FTX_MESSAGE_RC_OK;
            pthread_mutex_unlock(&hash_mux);
            if (unpack_status != FTX_MESSAGE_RC_OK) {
                LV_LOG_INFO("Error [%d] while unpacking!", (int)unpack_status);
            } else {
                int snr = get_message_snr(wf, cand, &message);
                msg_cb(text, snr, freq_hz, time_sec, user_data);
            }
            if (heard_list && heard_list->n < MAX_DECODED_MESSAGES) { /* to take out for the next pass */
                heard_sig_t *h = &heard_list->sig[heard_list->n++];
                if (wf->protocol == FTX_PROTOCOL_FT8) ft8_encode(message.payload, h->tones);
                else ft4_encode(message.payload, h->tones);
                h->freq_hz    = freq_hz;
                h->time_sec   = time_sec;
                h->subtracted = false;
            }
        }
    }
    // Remove decoded candidate;
    ftx_delete_candidates(to_delete_idx, to_delete_size, candidate_list, num_candidates);
}

static int get_message_snr(const ftx_waterfall_t *wf, const ftx_candidate_t *candidate, ftx_message_t *msg) {
    uint8_t n_tones = (wf->protocol == FTX_PROTOCOL_FT4) ? FT4_NN : FT8_NN;
    uint8_t tones[n_tones];

    if (wf->protocol == FTX_PROTOCOL_FT4) {
        ft4_encode(msg->payload, tones);
    } else {
        ft8_encode(msg->payload, tones);
    }
    return ftx_get_snr(wf, candidate, tones, n_tones);
}

void ftx_worker_set_realtime(bool on) {
    extra_realtime = on;
}

/* Its time is up, or the window is closing. */
static bool extra_stop(void) {
    pthread_mutex_lock(&extra_mux);
    bool quit = extra_quit;
    pthread_mutex_unlock(&extra_mux);
    if (quit) return true;
    if (!extra_realtime) return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - extra_started.tv_sec) + (now.tv_nsec - extra_started.tv_nsec) / 1e9f >=
           slot_secs * EXTRA_BUDGET;
}

/* Their finds wait for the audio thread, which owns what happens next. */
static void extra_found_cb(const char *text, int snr, float freq_hz, float time_sec, void *user_data) {
    (void)user_data;
    pthread_mutex_lock(&extra_mux);
    if (extra_found_n < MAX_DECODED_MESSAGES) {
        extra_found_t *f = &extra_found[extra_found_n++];
        snprintf(f->text, sizeof(f->text), "%s", text);
        f->snr      = snr;
        f->freq_hz  = freq_hz;
        f->time_sec = time_sec;
    }
    pthread_mutex_unlock(&extra_mux);
}

static void extra_passes_run(void) {
    for (int pass = 0; pass < EXTRA_PASSES; pass++) {
        int done = 0;
        for (int i = 0; i < extra_heard.n; i++) {
            heard_sig_t *h = &extra_heard.sig[i];
            if (h->subtracted) continue;
            if (extra_stop()) return;
            /* ft8_lib's time is that of the spectrum frame, two symbols long: the
             * signal starts a symbol before it (fitted within a symbol by ftx_subtract) */
            ftx_subtract(extra_audio, extra_len, rx_rate, h->tones, h->freq_hz,
                         (int)lroundf(h->time_sec * rx_rate) - block_size);
            h->subtracted = true;
            done++;
        }
        if (!done || extra_stop()) return;

        /* The spectrum again, from what's left, and a fresh search */
        int before = extra_heard.n;
        windowf_reset(extra_spec.window);
        extra_wf.num_blocks = 0;
        for (int b = 0; b + block_size <= extra_len && extra_wf.num_blocks < extra_wf.max_blocks; b += block_size)
            spectrum_block(&extra_spec, extra_audio + b);
        extra_num_candidates = ftx_find_candidates(&extra_wf, MAX_CANDIDATES, extra_candidates, ft8lib_min_score);
        if (extra_stop()) return;
        decode_messages(&extra_wf, &extra_num_candidates, extra_candidates, extra_decoded, extra_table, LDPC_ITERATIONS,
                        &extra_heard, extra_found_cb, NULL);
        if (extra_heard.n == before) return; /* nothing new: no more passes */
    }
}

/* As JS8's decode threads (js8/cpu_fence): off the last core, which is left
 * for the GUI thread, and below the rest in priority. */
static void extra_fence(void) {
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), EXTRA_NICE);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 3) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (long c = 0; c < cores - 1; c++) CPU_SET(c, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *extra_main(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "ftx-extra");
    extra_fence();
    for (;;) {
        pthread_mutex_lock(&extra_mux);
        while (!extra_job && !extra_quit) pthread_cond_wait(&extra_cv, &extra_mux);
        if (extra_quit) {
            pthread_mutex_unlock(&extra_mux);
            return NULL;
        }
        extra_job  = false;
        extra_busy = true;
        pthread_mutex_unlock(&extra_mux);

        int before = extra_heard.n;
        clock_gettime(CLOCK_MONOTONIC, &extra_started);
        extra_passes_run();
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        /* Each slot, in the app log: what they found and how long they took */
        LV_LOG_USER("FT8/FT4 extra passes: %d more decoded (of %d), %ld ms%s", extra_heard.n - before, extra_heard.n,
                    (long)((t1.tv_sec - extra_started.tv_sec) * 1000 + (t1.tv_nsec - extra_started.tv_nsec) / 1000000),
                    extra_stop() ? ", stopped at the limit" : "");

        pthread_mutex_lock(&extra_mux);
        extra_busy = false;
        pthread_mutex_unlock(&extra_mux);
    }
}

void ftx_worker_decode_more(decoded_msg_cb msg_cb, void *user_data) {
    if (!extra_on) return;
    extra_found_t found[MAX_DECODED_MESSAGES];
    pthread_mutex_lock(&extra_mux);
    int n = extra_found_n;
    memcpy(found, extra_found, sizeof(found[0]) * n);
    extra_found_n = 0;
    pthread_mutex_unlock(&extra_mux);
    for (int i = 0; i < n; i++) msg_cb(found[i].text, found[i].snr, found[i].freq_hz, found[i].time_sec, user_data);
}

bool ftx_worker_extra_busy(void) {
    pthread_mutex_lock(&extra_mux);
    bool busy = extra_job || extra_busy;
    pthread_mutex_unlock(&extra_mux);
    return busy;
}
