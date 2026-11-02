/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 - WEFAX stations and broadcast schedules
 *
 * Data file (see make_wefax_db.py), one record per line, '|' separated:
 *   S|country|place|callsign|freq[:call] freq[:call] ...|note
 *   G|freq freq ...           the following entries are sent on these
 *   E|HHMM|description        one broadcast, UTC
 * Frequencies are in kHz (the published "assigned" frequency).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFAX_DB_FILE           "/mnt/wefax_stations.txt"
#define WEFAX_DB_DEFAULT_FILE   "/usr/share/x6100/wefax_stations.txt"

typedef struct {
    const char *country;
    const char *place;
    const char *call;       /* callsign of this frequency */
    const char *note;       /* "" if none */
    int32_t     hz;         /* assigned frequency */
    int         station;    /* station number */
    bool        first;      /* first row of its country */
} wefax_row_t;

typedef struct {
    int         minute;     /* 0..1439, UTC */
    const char *text;
} wefax_entry_t;

/* Load the first data file found; returns the number of rows (0 = none). */
int  wefax_db_load(void);
void wefax_db_free(void);

int  wefax_db_rows(void);
const wefax_row_t *wefax_db_row(int i);

/* Row whose frequency is hz (exact), -1 if none. */
int  wefax_db_find(int32_t hz);

/*
 * Broadcasts of a row, sorted by time (duplicates removed).  The array
 * stays valid until the next call or wefax_db_free().
 */
int  wefax_db_schedule(int row, const wefax_entry_t **out);

/* Index of the next broadcast at or after minute now (wraps), -1 if none. */
int  wefax_db_next(const wefax_entry_t *e, int n, int now);

/* Minutes from now to the start of e (0..1439). */
int  wefax_db_wait(const wefax_entry_t *e, int now);

#ifdef __cplusplus
}
#endif
