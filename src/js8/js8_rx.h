/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, C interface for the dialog
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JS8_RX_CALL_LEN 16
/* A whole message: 20 frames carry up to ~425 characters (desktop sends
 * them; 256 cut them short), plus "CALL: " in front. */
#define JS8_RX_TEXT_LEN 640

/* Submode bits for js8_rx_create(). */
#define JS8_SUBMODE_NORMAL (1 << 0)
#define JS8_SUBMODE_FAST   (1 << 1)
#define JS8_SUBMODE_TURBO  (1 << 2)
#define JS8_SUBMODE_SLOW   (1 << 3)
#define JS8_SUBMODE_ULTRA  (1 << 4) /* Ultra */

/* Frame-type bits in js8_rx_msg_t.type. */
#define JS8_FRAME_FIRST 0x1
#define JS8_FRAME_LAST  0x2
#define JS8_FRAME_DATA  0x4

/* One decoded frame or one assembled message. Plain value type: safe to
 * memcpy, e.g. through scheduler_put(). */
typedef struct {
    int32_t utc;      /* HHMMSS */
    int16_t snr;
    float   dt;
    int32_t drift_ms; /* JS8 drift that would put this signal on time: the
                         drift its audio was captured with, less its DT (so
                         it stays right if the drift changed since) */
    float   freq_hz;  /* audio offset */
    uint8_t type;     /* JS8_FRAME_* bits of the (last) frame */
    uint8_t submode;  /* 0 Normal, 1 Fast, 2 Turbo, 4 Slow */
    bool    low_confidence;
    bool    heartbeat;
    bool    snr_report; /* "CALL SNR -12", mostly answers to heartbeats */
    bool    cq;
    bool    to_me;
    bool    to_group;
    int8_t  checksum; /* buffered command (MSG etc.): 0 none, 1 valid, -1 bad */
    bool    tx;       /* set by the app for its own transmissions */
    bool    alert;    /* set by the app: matched an alert word */
    bool     partial; /* a message still arriving: the text so far (on_message) */
    uint32_t msg_id;  /* partials and the final message share it */
    char    from[JS8_RX_CALL_LEN];
    char    to[JS8_RX_CALL_LEN];
    char    text[JS8_RX_TEXT_LEN];
} js8_rx_msg_t;

/* All callbacks run on receiver worker threads, never the caller's. */
/* A decode attempt worth marking on the waterfall, as desktop JS8Call's
 * "Show decode attempts" draws them: a sync candidate the decoder is trying,
 * coloured by its sync strength, or a decode. */
#define JS8_MARK_WEAK    0 /* sync below 10 (desktop: dark cyan) */
#define JS8_MARK_MEDIUM  1 /* 10-15 (cyan) */
#define JS8_MARK_STRONG  2 /* 16-21 (white) */
#define JS8_MARK_DECODED 3 /* decoded (desktop: red) */

typedef struct {
    float   freq_hz; /* audio offset of the lowest tone */
    uint8_t submode; /* 0 Normal, 1 Fast, 2 Turbo, 4 Slow */
    uint8_t level;   /* JS8_MARK_* */
} js8_rx_mark_t;

/* Desktop's rule: a decode always; a candidate only within 2 s of the slot
 * (|dt|) and with sync 21 or less (stronger ones decode). -1: not marked. */
int js8_mark_level(bool decoded, int sync, float dt);

/* What a time search found (js8_rx_search_start). */
typedef struct {
    bool    found;    /* false: time ran out with nothing decoded */
    int32_t drift_ms; /* the drift that puts it on time */
    int16_t snr;
    float   freq_hz;
    char    text[JS8_RX_TEXT_LEN]; /* the frame, e.g. "W1ABC: @HB HEARTBEAT" */
} js8_rx_search_t;

typedef struct {
    void (*on_frame)(const js8_rx_msg_t *msg, void *ctx);   /* every decode */
    /* Assembled messages; also, with msg->partial set, the text so far of a
     * multi-frame message after each of its frames (the final one follows
     * with the same msg_id). */
    void (*on_message)(const js8_rx_msg_t *msg, void *ctx);
    void (*on_cycle_done)(unsigned decodes, void *ctx);
    /* Input-rate audio off the audio thread, e.g. for a waterfall. */
    void (*on_audio)(const float *samples, unsigned n, void *ctx);
    /* Decode attempts, only while js8_rx_set_sync_marks(rx, true): many per
     * decode pass, from the decoder's thread. */
    void (*on_mark)(const js8_rx_mark_t *mark, void *ctx);
    /* Health lines for the app's log, a few a minute at most: the decoder's
     * load each minute, audio missing or thrown away, clock realigns. From
     * the receiver's and the decoder's threads. */
    void (*on_report)(const char *line, void *ctx);
    /* Automatic time sync (js8_rx_set_auto_sync): after a decode pass with
     * Normal or Slow frames, the drift desktop would set (its 60-frame
     * average) and how many frames came in that pass. Setting it
     * (js8_set_drift_ms) is the caller's: not while sending. */
    void (*on_auto_drift)(int64_t drift_ms, unsigned frames, void *ctx);
    /* A time search ended (js8_rx_search_start), from its own thread. */
    void (*on_search)(const js8_rx_search_t *result, void *ctx);
    void *ctx;
} js8_rx_cb_t;

typedef struct js8_rx js8_rx_t;

/* Returns NULL on failure. `my_call` may be NULL or empty. */
js8_rx_t *js8_rx_create(int input_rate, int submodes, const char *my_call, const js8_rx_cb_t *cb);

/* Queue float audio in [-1, 1] at input_rate. Safe from the audio thread. */
void js8_rx_feed(js8_rx_t *rx, const float *samples, unsigned n);

/* Drop partially received messages, e.g. after a band change. */
void js8_rx_clear(js8_rx_t *rx);
/* Change which speeds are decoded (JS8_SUBMODE_* bits), from any thread. */
void js8_rx_set_submodes(js8_rx_t *rx, int submodes);
/* The audio range searched (low..high Hz) and our TX offset, whose
 * neighbours are decoded first. From any thread. */
void js8_rx_set_decode_range(js8_rx_t *rx, int low_hz, int high_hz);
void js8_rx_set_qso_offset(js8_rx_t *rx, int offset_hz);
/* Report decode attempts through on_mark, from the next decode pass. */
void js8_rx_set_sync_marks(js8_rx_t *rx, bool on);

/* Automatic time sync, as desktop JS8Call's Automatic Time Drift (and the
 * Android app's Auto time sync): every decoded Normal or Slow frame,
 * heartbeats included, feeds a 60-frame average reported through
 * on_auto_drift after each pass. Off until switched on. */
void js8_rx_set_auto_sync(js8_rx_t *rx, bool on);
/* The drift was set another way: the average goes on from `drift_ms`,
 * counted as one frame (`keep`: a search's find), or starts afresh (a
 * reset: the next frame sets it outright). */
void js8_rx_auto_sync_restart(js8_rx_t *rx, int64_t drift_ms, bool keep);
/* For when the clock is too far off for anything to decode (more than
 * about 2.5 s): decode the latest 15 s of audio every 4 s, wherever the
 * slots fall, for up to `max_s` seconds; the first Normal decode gives the
 * drift (on_search). Starting again restarts it. */
void js8_rx_search_start(js8_rx_t *rx, unsigned max_s);
void js8_rx_search_stop(js8_rx_t *rx);
bool js8_rx_searching(js8_rx_t *rx);

/* Test mode: play a 16-bit PCM WAV (any rate; resampled as needed) into the
 * decoder in real time, starting at the next 30 s boundary (a slot start for
 * every speed), so a file whose first sample is a slot start decodes like
 * live audio. Live audio from js8_rx_feed() is ignored while it plays.
 * Returns the seconds until playback starts, or -1 with a message in err. */
float js8_rx_play_wav(js8_rx_t *rx, const char *path, char *err, unsigned err_len);
void  js8_rx_stop_wav(js8_rx_t *rx);
bool  js8_rx_wav_active(js8_rx_t *rx);

/* Stops threads; no callbacks fire after this returns. NULL is ignored. */
void js8_rx_destroy(js8_rx_t *rx);

/* JS8's time, as desktop JS8Call's time drift: the system clock plus a
 * drift (ms) that Time Sync sets. Receive windows and transmit slots go by
 * it; the system clock is never changed. The drift is process-wide and
 * starts at 0 (so it lasts until the radio restarts). */
int64_t js8_wall_ms(void);
void    js8_set_drift_ms(int64_t ms);
int64_t js8_drift_ms(void);

#ifdef __cplusplus
}
#endif
