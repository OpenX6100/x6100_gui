/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - DSC ship names database (see make_ships_db.py)
 *
 * The file is memory mapped read-only: lookups cost no RAM and no parsing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;       /* point into the mapped file */
    const char *call;       /* "" if unknown */
    const char *type;       /* "" if unknown */
} dsc_ship_t;

/* Maps the first database found; returns the number of ships (0 = none). */
int  dsc_ships_open(void);
void dsc_ships_close(void);

/* Ship with this 9-digit MMSI ('?' digits never match). */
bool dsc_ship_lookup(const char *mmsi, dsc_ship_t *out);

#ifdef __cplusplus
}
#endif
