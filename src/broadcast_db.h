/*
 * Copyright (C) 2026 Hany El Imam
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef BROADCAST_DB_H
#define BROADCAST_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BROADCAST_MAX_MATCHES 16

typedef struct {
    double freq_khz;
    int start_minute;
    int stop_minute;
    char station[202];
    char language[50];
    char target[63];
    char days[60];
    char itu[50];
    char remarks[136];
} broadcast_match_t;

/* Load /mnt/broadcast.csv if the user put one there, otherwise the
 * firmware's /usr/share/x6100/broadcast.csv, into memory. Called by the
 * first broadcast_db_find_matches(), so nothing is loaded until the
 * Channel List's GENE or NW is used. */
bool broadcast_db_init(void);

/* Find all stations active now within tolerance_hz of freq_hz. */
size_t broadcast_db_find_matches(int32_t freq_hz, int tolerance_hz,
                                 broadcast_match_t *matches, size_t max_matches);


#endif

