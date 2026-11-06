/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - NAVTEX bulletin assembler
 *
 * Turns the character stream of the SITOR-B decoder (with the reliability
 * of every character, see decoder.h) into bulletins:
 *
 *   ZCZC B1B2B3B4      B1 station, B2 subject, B3B4 serial number
 *   ...text...
 *   NNNN
 *
 * Start and end are recognised also when damaged ("ZCZC" with one wrong
 * character, "NNN", the digits of the serial received as letters after a
 * lost FIGS shift).  When they are lost altogether the text is still kept,
 * flagged "start lost" / "end lost", and closed by the next header, by the
 * loss of the signal or by a timeout.  Short noise between bulletins is
 * thrown away.
 *
 * Some coast stations send on HF plain SITOR-B messages, without the
 * NAVTEX frame: no ZCZC and no NNNN, each message starts with its
 * date-time group ("190730 UTC JUN 26") and ends where the next starts.
 * Such a message is marked sitor, complete unless the signal went in the
 * middle of its text.
 *
 * Pure C, no LVGL: fed from any thread, the owner serialises the calls.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_MAX_TEXT     6000        /* characters of one bulletin */

typedef struct {
    char     b1, b2;                /* station, subject; 0 = unknown */
    int      num;                   /* serial 0..99, -1 = unknown */
    bool     lost_head;             /* no ZCZC header */
    bool     lost_tail;             /* no NNNN */
    bool     sitor;                 /* SITOR-B message without ZCZC / NNNN */
    time_t   first_rx, last_rx;
    int      receptions;            /* copies merged into this one */
    int      len;
    char     text[NB_MAX_TEXT + 1]; /* body, lines separated by '\n' */
    uint8_t  q[NB_MAX_TEXT];        /* NAVTEX_Q_* of every character */
} nb_bulletin_t;

typedef void (*nb_done_cb_t)(const nb_bulletin_t *b, void *user);

typedef struct nb_assembler nb_assembler_t;

nb_assembler_t *nb_create(nb_done_cb_t done, void *user);
void nb_destroy(nb_assembler_t *a);

/* one character (quality NAVTEX_Q_*) or event (NAVTEX_Q_EVENT) */
void nb_feed(nb_assembler_t *a, int c, int quality, time_t now);

/* call about once a second: closes a bulletin whose signal is gone */
void nb_tick(nb_assembler_t *a, time_t now);

/* close what is open now (decoder stopped) */
void nb_flush(nb_assembler_t *a);

/* a bulletin (with its ZCZC header) is being received */
bool nb_active(const nb_assembler_t *a);

/* ---- helpers on a finished bulletin -------------------------------------- */

/* characters that count (no spaces / line ends) and how they were received */
void nb_counts(const nb_bulletin_t *b, int *total, int *clean, int *guess, int *lost);

/* 0..100: share of characters received correctly (clean or from FEC copy) */
int  nb_quality(const nb_bulletin_t *b);

/* IEC 61097-6: more than 4% of the characters lost = not received */
bool nb_corrupted(const nb_bulletin_t *b);

/* subject (B2) in words, "" if unknown */
const char *nb_subject_name(char b2);

/* same bulletin received again: improve dst with src, true if merged */
bool nb_merge(nb_bulletin_t *dst, const nb_bulletin_t *src);

/* quality level: OK (<= 4% lost), damaged (<= 15%), unreadable */
typedef enum { NB_LEVEL_OK = 0, NB_LEVEL_DAMAGED, NB_LEVEL_UNREADABLE } nb_level_t;
nb_level_t nb_level(const nb_bulletin_t *b);

/*
 * Lines worth showing in a summary: the station ("ISTANBUL TURK RADYO",
 * "MONDOLFO RADIO"), the date-time group and the first two lines of the
 * message itself.  Each is copied to its buffer ("" if not found).
 */
void nb_summary(const nb_bulletin_t *b, char *station, char *dtg,
                char *title, char *title2, int sz);

/*
 * Positions in the text, for the display: mark[i] = NB_MARK_COORD for the
 * characters of a geographic position, NB_MARK_FIXED where a digit (or
 * . , -) received as a letter after a lost FIGS shift was put back, and
 * text[i] is replaced accordingly in fixed[] (same length as b->text).
 */
#define NB_MARK_NONE    0
#define NB_MARK_COORD   1
#define NB_MARK_FIXED   2
void nb_mark_positions(const nb_bulletin_t *b, char *fixed, uint8_t *mark);

/* file on the SD card (text, readable); false on error */
bool nb_save(const nb_bulletin_t *b, const char *path);
bool nb_load(nb_bulletin_t *b, const char *path);

#ifdef __cplusplus
}
#endif
