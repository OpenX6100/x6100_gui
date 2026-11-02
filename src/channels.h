/*
 * Copyright (C) 2026 Hany El Imam
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"


#define CHANNELS_MAX 300
#define CHANNEL_NAME_MAX 32


typedef struct {
    char name[CHANNEL_NAME_MAX];

    uint32_t freq;

    x6100_mode_t mode;
    x6100_agc_t agc;

    bool pre;
    bool att;
} channel_t;


/*
 * Load channel database from disk.
 *
 * Later:
 * - if channels.json exists -> load it
 * - if it doesn't exist -> create an empty list
 */
bool channels_load(void);


/*
 * Save current channel database to disk.
 */
bool channels_save(void);


/*
 * Number of currently loaded channels.
 */
uint16_t channels_count(void);


/*
 * Return channel at given index.
 *
 * Returns NULL if index is invalid.
 */
const channel_t *channels_get(uint16_t index);

/*
 * Add a new channel using the current radio state.
 *
 * The channel is appended to the end of the database.
 * The name is initially empty.
 *
 * Frequency, mode, AGC, PRE and ATT are captured
 * from the current radio configuration.
 *
 * The database is immediately saved to channels.json.
 */
bool channels_add_current(void);

bool channels_delete(uint16_t index);

bool channels_set_name(uint16_t index, const char *name);

bool channels_move(uint16_t from, uint16_t to);


