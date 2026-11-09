/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - ADIF records for the logs and for UDP
 *
 *  One writer for every ADIF record the apps make: the FT8/FT4/FT2 UDP
 *  "Logged ADIF" message, JTTY's /mnt/jtty_log.adi and the keyboard
 *  modes' /mnt/digi_log.adi, so the fields, the dates and times, the
 *  band names and the modes are written the same way everywhere.
 *
 *    char       buf[1024];
 *    adif_rec_t r;
 *
 *    adif_rec_begin(&r, buf, sizeof(buf));
 *    adif_rec_str(&r, "call", "SP2KMO");
 *    adif_rec_mode(&r, MODE_FT4, NULL);       // <mode:4>MFSK <submode:3>FT4
 *    adif_rec_times(&r, start, end);
 *    adif_rec_freq(&r, 14080000);             // band and freq
 *    if (adif_rec_end(&r) > 0) ...            // buf holds the record
 *
 *  A field with an empty value is left out: "<name:0>" is not valid ADIF
 *  and makes some loggers drop the whole record. A record that does not
 *  fit the buffer is not cut short: adif_rec_end() returns 0.
 *
 *  The modes are one table (qso_log_mode_t, the name the apps show, the
 *  ADIF MODE and SUBMODE), which adif.c reads and writes the FT8 log by
 *  too.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "qso_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *out;
    size_t size;
    size_t len;
    bool   overflow;
} adif_rec_t;

void   adif_rec_begin(adif_rec_t *r, char *out, size_t size);

/* "<name:len>value " - nothing when value is NULL or "". */
void   adif_rec_str(adif_rec_t *r, const char *name, const char *value);
void   adif_rec_int(adif_rec_t *r, const char *name, int value);

/* QSO_DATE and TIME_ON from `on`, QSO_DATE_OFF and TIME_OFF from `off`,
 * UTC, to the second. */
void   adif_rec_times(adif_rec_t *r, time_t on, time_t off);

/* BAND (when the frequency is in an amateur band) and FREQ in MHz. */
void   adif_rec_freq(adif_rec_t *r, uint64_t hz);

/* MODE and SUBMODE; `submode` replaces the table's (Olivia's
 * "OLIVIA 32/1000"), NULL keeps it. */
void   adif_rec_mode(adif_rec_t *r, qso_log_mode_t mode, const char *submode);

/* MODE and SUBMODE of a mode the apps name ("FT4"): from the table, or a
 * name it does not know as MODE itself, so a record never goes out
 * without one. */
void   adif_rec_mode_name(adif_rec_t *r, const char *name);

/* "<eor>\n". The record's length, 0 when it did not fit. */
size_t adif_rec_end(adif_rec_t *r);

/* The header that starts a file and the WSJT-X "Logged ADIF" message. */
#define ADIF_HEADER "<adif_ver:5>3.1.4 <programid:5>X6100 <eoh>\n"

/* Append `record` to the file at `path`, starting a new or empty file
 * with `title` and the header. Written through to the card (fsync): a
 * logged QSO must survive the power switch. */
bool   adif_rec_append(const char *path, const char *title, const char *record);

/* The ADIF band ("40m") of a frequency, "" outside the amateur bands. */
const char *adif_band_name(uint64_t hz);

/* ---- modes ---------------------------------------------------------- */

/* The ADIF MODE and SUBMODE of a mode (SUBMODE NULL when it has none;
 * MODE NULL for MODE_OTHER). */
void   adif_mode_of(qso_log_mode_t mode, const char **adif_mode, const char **adif_submode);

/* The mode of an ADIF MODE and SUBMODE (either may be NULL), MODE_OTHER
 * when unknown. Also takes the pre-3.0 forms where the submode stood as
 * the mode ("PSK31", "FT4", "JS8"). */
qso_log_mode_t adif_mode_parse(const char *adif_mode, const char *adif_submode);

/* The mode of the name the apps use ("FT4"), MODE_OTHER when unknown. */
qso_log_mode_t adif_mode_by_name(const char *name);

#ifdef __cplusplus
}
#endif
