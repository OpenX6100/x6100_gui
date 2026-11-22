/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4/FT2: the table of callsigns heard
 */

#include "call_table.h"

#include <string.h>

#define TABLE_SIZE 1024
#define PROBES     64           /* places a search looks at */
#define HASH_MASK  0x3FFFFFu    /* 22 bits */

typedef struct {
    char     call[12];          /* "" free */
    uint32_t hash;
    uint8_t  age;
} entry_t;

static entry_t table[TABLE_SIZE];

/* Where a call with these top 10 hash bits starts its search. */
static int first_place(uint32_t hash10) {
    return (int)((hash10 & 0x3FFu) * 23 % TABLE_SIZE);
}

static void call_save(const char *call, uint32_t hash) {
    int      start = first_place(hash >> 12);
    entry_t *use   = NULL;

    for (int i = 0; i < PROBES; i++) {
        entry_t *e = &table[(start + i) % TABLE_SIZE];
        if (e->call[0] && e->hash == hash && strcmp(e->call, call) == 0) {
            e->age = 0;
            return;
        }
        if (!e->call[0]) {
            if (!use || use->call[0]) use = e;      /* the first free place */
        } else if (!use || (use->call[0] && e->age > use->age)) {
            use = e;                                /* else the oldest */
        }
    }
    strncpy(use->call, call, sizeof(use->call) - 1);
    use->call[sizeof(use->call) - 1] = '\0';
    use->hash = hash;
    use->age  = 0;
}

static bool call_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char *call) {
    int shift = type == FTX_CALLSIGN_HASH_10_BITS ? 12 : (type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    int start = first_place(hash >> (12 - shift));

    for (int i = 0; i < PROBES; i++) {
        const entry_t *e = &table[(start + i) % TABLE_SIZE];
        if (e->call[0] && (e->hash & HASH_MASK) >> shift == hash) {
            strcpy(call, e->call);
            return true;
        }
    }
    call[0] = '\0';
    return false;
}

ftx_callsign_hash_interface_t ftx_call_table_if = {
    .lookup_hash = call_lookup,
    .save_hash   = call_save,
};

void ftx_call_table_reset(void) {
    memset(table, 0, sizeof(table));
}

void ftx_call_table_age(uint8_t max_age) {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (!table[i].call[0]) continue;
        if (table[i].age > max_age) table[i].call[0] = '\0';
        else table[i].age++;
    }
}
