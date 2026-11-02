/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 station history (C API)
 *
 *  Every station you've exchanged messages with (heartbeat ACKs included),
 *  per band, kept in one SQLite file: their INFO and STATUS, and the text
 *  of each QSO, time-stamped. Written by a low-priority thread of its own,
 *  a few seconds' worth at a time.
 */

#pragma once

#include "js8_ops.h"
#include "js8_rx.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js8_history js8_history_t;

/* NULL if the file can't be opened or made (it's then not recorded). */
js8_history_t *js8_history_open(const char *path);
/* Writes what's waiting, then closes. */
void           js8_history_close(js8_history_t *h);

/* A complete received message (partials, our own and low-confidence ones
 * are skipped), on the dial frequency `dial_hz`. `stations` (or NULL): the
 * Stations list, for a grid they announced before you exchanged anything.
 * Text without calls joins an open QSO: theirs ("W1ABC: GOOD COPY"), or,
 * with no sender at all, the one whose station sends on that offset. */
void js8_history_rx(js8_history_t *h, const js8_rx_msg_t *m, const char *my_call, uint64_t dial_hz,
                    js8_stations_t *stations, int64_t now_ms);
/* One of ours as it goes out ("MYCALL: W1ABC HW CPY?"); `automatic`: an
 * automatic reply or ACK. Free text ("MYCALL: GOOD COPY") joins the open
 * QSO with `partner` (the station you have selected), if there is one. */
void js8_history_tx(js8_history_t *h, const char *text, const char *my_call, uint64_t dial_hz, float offset_hz,
                    uint8_t submode, bool automatic, const char *partner, js8_stations_t *stations, int64_t now_ms);
/* The QSO with `call` went into the log (`freq_hz` its frequency). */
void js8_history_logged(js8_history_t *h, const char *call, uint64_t freq_hz, int64_t now_ms);

/* You've exchanged messages with `call` on `freq_hz`'s band. Quick (no
 * file access). */
bool js8_history_known(js8_history_t *h, const char *call, uint64_t freq_hz);

/* You've had a QSO with `call` (any band; heartbeat ACKs alone don't
 * count). Quick (no file access). */
bool js8_history_had_qso(js8_history_t *h, const char *call);

/* Forget everything, every band (Settings). Waits until it's done. */
void js8_history_clear(js8_history_t *h);
/* Stations in the history, every band. */
int  js8_history_station_count(js8_history_t *h);

/* Waits until everything so far is in the file. */
void js8_history_flush(js8_history_t *h);
/* Since the last call: rows written, transactions, rows lost to errors,
 * time inside transactions. False if nothing happened. */
bool js8_history_stats(js8_history_t *h, unsigned *rows, unsigned *commits, unsigned *failed, int64_t *busy_us);

/* ---- Reading ---------------------------------------------------------- */

typedef struct {
    char    call[JS8_RX_CALL_LEN];
    char    grid[8];
    char    band[8];
    int64_t first_ms, last_ms; /* first and latest exchange */
    int64_t heard_ms;          /* heard at all, latest */
    int64_t heard_us_ms;       /* they sent us something, latest (0: never) */
    int16_t snr;               /* how we heard them, latest */
    bool    has_reported_snr;
    int16_t reported_snr;      /* how they heard us, latest */
} js8_hist_contact_t;

typedef struct {
    int64_t ms;
    char    text[JS8_RX_TEXT_LEN];
    char    to[JS8_RX_CALL_LEN];
} js8_hist_info_t;

typedef struct {
    int64_t id, start_ms, end_ms;
    char    band[8];
    int     lines, real_lines; /* real: not heartbeat ACKs */
    bool    logged;
} js8_hist_qso_t;

typedef struct {
    int64_t ms;
    bool    tx;
    bool    heartbeat;
    int16_t snr;
    char    text[JS8_RX_TEXT_LEN];
} js8_hist_line_t;

/* The stations of `band` ("20m"), latest exchange first; returns how many
 * (up to max). */
int  js8_history_contacts(js8_history_t *h, const char *band, js8_hist_contact_t *out, int max);
/* `call` on `band`, if you've exchanged messages there. */
bool js8_history_contact(js8_history_t *h, const char *call, const char *band, js8_hist_contact_t *out);
/* Their latest INFO (kind 0) or STATUS (kind 1). */
bool js8_history_info(js8_history_t *h, const char *call, int kind, js8_hist_info_t *out);
/* Their QSOs, newest first, heartbeat-only exchanges left out. */
int  js8_history_qsos(js8_history_t *h, const char *call, js8_hist_qso_t *out, int max);
/* A QSO's messages, oldest first. */
int  js8_history_lines(js8_history_t *h, int64_t qso_id, js8_hist_line_t *out, int max);

#ifdef __cplusplus
}
#endif
