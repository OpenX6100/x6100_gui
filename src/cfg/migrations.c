/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#define _GNU_SOURCE

#include "migrations.h"

#include "db.h"
#include "digital_modes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZEOF_ARRAY(arr) (sizeof(arr) > 0 ? sizeof(arr) / sizeof(arr[0]) : 0)

static int get_current_version(int * ver);
static int set_current_version(int ver);

/* Migrations functions */
static int _0_init_migrations() {
    sqlite3 *db = cfg_db_get();
    int rc;
    rc = sqlite3_exec(db, "INSERT INTO version(id) VALUES(0)", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot insert 0 version: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int _1_create_ftx_table() {
    sqlite3 *db = cfg_db_get();
    int rc;
    char *query;
    rc = asprintf(&query,
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS digital_modes("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "label varchar(64) NOT NULL, "
            "freq INTEGER NOT NULL CHECK(freq > 0), "
            "mode INTEGER NOT NULL DEFAULT 3 CHECK(mode >= 0 AND mode <= 7), "
            "type INTEGER NOT NULL, "
            "CONSTRAINT freq_type_uniq UNIQUE(freq, type) "
        ");"
        "CREATE INDEX IF NOT EXISTS digital_modes_type_idx ON digital_modes (type);"
        "CREATE INDEX IF NOT EXISTS digital_modes_freq_idx ON digital_modes (freq);"
        "INSERT INTO digital_modes(label, freq, mode, type) "
        "SELECT m1.val, m2.val, m3.val, %u  "
        "FROM memory AS m1  "
        "INNER JOIN memory AS m2 ON m1.id == m2.id  "
        "INNER JOIN memory AS m3 ON m1.id == m3.id  "
        "WHERE m1.name='label' AND m2.name='vfoa_freq' AND m3.name = 'vfoa_mode' AND m1.id >= 100 AND m1.id < 200; "
        "INSERT INTO digital_modes(label, freq, mode, type) "
        "SELECT m1.val, m2.val, m3.val, %u  "
        "FROM memory AS m1  "
        "INNER JOIN memory AS m2 ON m1.id == m2.id  "
        "INNER JOIN memory AS m3 ON m1.id == m3.id  "
        "WHERE m1.name='label' AND m2.name='vfoa_freq' AND m3.name = 'vfoa_mode' AND m1.id >= 200; "
        "COMMIT",
        CFG_DIG_TYPE_FT8, CFG_DIG_TYPE_FT4
    );
    if (rc == -1) {
        printf("Cannot allocate SQL query\n");
        return 1;
    }
    rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    free(query);
    if (rc != SQLITE_OK) {
        printf("Cannot migrate: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int _2_update_atu_freq() {
    sqlite3 *db = cfg_db_get();
    int rc;
    rc = sqlite3_exec(db, "UPDATE atu SET freq=freq * 50000 + 25000 WHERE freq < 500000", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot update ATU frequencies: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int _3_update_spectrum_peak_hold() {
    sqlite3 *db = cfg_db_get();
    int rc;
    // spectrum peak hold from 1ms to 1s
    rc = sqlite3_exec(db, "UPDATE params SET val = val / 1000 WHERE name = 'spectrum_peak_hold'", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot update spectrum peak hold: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int _4_convert_scaled_params_to_real() {
    sqlite3 *db = cfg_db_get();
    int rc;
    /* Floats used to be stored as int * Scale. They are now REAL in engineering
       units; divide legacy integer rows by the old scale. Rows already stored as
       REAL (written by the buggy float path) are left untouched. */
    rc = sqlite3_exec(db,
        "UPDATE params SET val = CAST(val AS REAL) / 10.0 WHERE name='pwr' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 2.0  WHERE name='auto_level_offset' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 10.0 WHERE name='key_ratio' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 10.0 WHERE name='cw_decoder_snr_2' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 10.0 WHERE name='cw_decoder_snr_gist' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 5.0  WHERE name='output_gain' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 2.0  WHERE name='comp_threshold_offset' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 2.0  WHERE name='comp_makeup_offset' AND typeof(val)='integer';"
        "UPDATE params SET val = CAST(val AS REAL) / 10.0 WHERE name='cessb_power_up' AND typeof(val)='integer';"
        "UPDATE band_params SET val = CAST(val AS REAL) / 10.0 WHERE name='dac_offset' AND typeof(val)='integer';",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot convert scaled params to REAL: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

/* FT2 dial frequencies (IU8LMC's Decodium list, mode 3 = USB-D). For new
   and existing databases alike: the seed database starts at version 1. */
static int _5_add_ft2_frequencies() {
    sqlite3 *db = cfg_db_get();
    int rc;
    char *query;
    rc = asprintf(&query,
        "INSERT OR IGNORE INTO digital_modes(label, freq, mode, type) VALUES "
        "('FT2 160m', 1843000, 3, %u), ('FT2 80m', 3568000, 3, %u), "
        "('FT2 60m', 5360000, 3, %u), ('FT2 40m', 7062000, 3, %u), "
        "('FT2 30m', 10144000, 3, %u), ('FT2 20m', 14084000, 3, %u), "
        "('FT2 17m', 18108000, 3, %u), ('FT2 15m', 21144000, 3, %u), "
        "('FT2 12m', 24923000, 3, %u), ('FT2 10m', 28184000, 3, %u), "
        "('FT2 6m', 50316000, 3, %u)",
        CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2,
        CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2,
        CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2, CFG_DIG_TYPE_FT2
    );
    if (rc == -1) {
        printf("Cannot allocate SQL query\n");
        return 1;
    }
    rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    free(query);
    if (rc != SQLITE_OK) {
        printf("Cannot add FT2 frequencies: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

/* Migrations array */
static int (*migrations[])() = {
    _0_init_migrations,
    _1_create_ftx_table,
    _2_update_atu_freq,
    _3_update_spectrum_peak_hold,
    _4_convert_scaled_params_to_real,
    _5_add_ft2_frequencies,
};

int migrations_apply(void) {
    int rc = 0;
    int ver;
    if (cfg_db_get() == NULL) {
        printf("Database is not opened\n");
        return 1;
    }

    rc = get_current_version(&ver);
    if (rc != 0) {
        printf("Cannot get current version\n");
        return 1;
    }
    for (size_t i = ver+1; i < SIZEOF_ARRAY(migrations); i++){
        printf("Apply migration: %i ...\n", i);
        rc = (*migrations[i])();
        if (rc != 0) {
            printf("Can't apply %i migration\n", i);
            break;
        }
        set_current_version(i);
    }
    return rc;
}


static int get_current_version(int * ver) {
    sqlite3 *db = cfg_db_get();
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS version(id INT NOT NULL DEFAULT 0)", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot create versions table: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    rc = sqlite3_prepare_v2(db, "SELECT id from version", -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        printf("Failed prepare statement: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
        *ver = sqlite3_column_int(stmt, 0);
    } else {
        *ver = -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int set_current_version(int ver) {
    sqlite3 *db = cfg_db_get();
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "UPDATE version SET id=?", -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        printf("Failed prepare statement: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_bind_int(stmt, 1, ver);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return 0;
}
