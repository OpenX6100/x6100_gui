/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8's part of params.db
 *
 *  The JS8 test builds (randal007/x6100-js8, betas 1-4 on R1CBU 0.34)
 *  numbered their two migrations 4 and 5:
 *  4 added JS8Call's frequency list, 5 GhostNet's. R1CBU 1.0 has its own
 *  migration 4, which converts settings stored as scaled integers into real
 *  numbers (pwr 4 -> 0.4 W, output_gain / 5, dac_offset / 10 ...). A card
 *  from those builds has version 5 and would skip it, and read pwr 4 as
 *  4 W.
 *
 *  So JS8 follows the GUI's migration numbering exactly from 1.0 on, and
 *  its frequency lists take no number:
 *  - before the migrations, a card at version 4 or 5 that has JS8's
 *    rows ran those builds (nothing else adds them): it goes back to 3,
 *    so the conversion runs. The js8_db table records that this was done,
 *    so it never happens twice (the params table stores whole numbers as
 *    INTEGER, so a second conversion would divide the user's 5 W by 10);
 *  - after them, the two lists are inserted where missing, every start.
 *  On any other card the first part only creates the js8_db table.
 */

#include "js8_db.h"

#include "digital_modes.h"

#include <stdbool.h>
#include <stdio.h>

static int scalar(sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 1;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *out = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : 1;
}

int js8_db_before_migrations(sqlite3 *db) {
    int done = 0;
    if (scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'js8_db'", &done) != 0) {
        printf("JS8 db: cannot read the schema: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    if (done)
        return 0;

    int ver = -1, js8_rows = 0;
    scalar(db, "SELECT id FROM version", &ver); /* no table or row: a new card */
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM digital_modes WHERE type IN (%d, %d)", CFG_DIG_TYPE_JS8,
             CFG_DIG_TYPE_JS8_GHOSTNET);
    scalar(db, sql, &js8_rows); /* no digital_modes table yet: 0 */

    bool from_beta = (ver == 4 || ver == 5) && js8_rows > 0;
    snprintf(sql, sizeof(sql),
             "BEGIN;"
             "%s"
             "CREATE TABLE js8_db(from_version INT, realigned INT);"
             "INSERT INTO js8_db VALUES(%d, %d);"
             "COMMIT",
             from_beta ? "UPDATE version SET id = 3;" : "", ver, from_beta);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        printf("JS8 db: cannot record the realignment: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return 1;
    }
    if (from_beta)
        printf("JS8 db: card from a JS8 beta (version %d): back to 3 for R1CBU 1.0's migrations\n", ver);
    return 0;
}

int js8_db_after_migrations(sqlite3 *db) {
    /* JS8Call's dial frequencies (its FrequencyList.cpp) and GhostNet's nets
     * (s2underground/GhostNet 1.5: 7.107 MHz main), USB-D like the FT8/FT4
     * rows. OR IGNORE keeps the rows a card already has (freq + type is
     * unique). */
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT OR IGNORE INTO digital_modes(label, freq, mode, type) "
             "SELECT column1, column2, 3, %d FROM (VALUES "
             "('JS8 160m', 1842000),"
             "('JS8 80m', 3578000),"
             "('JS8 40m', 7078000),"
             "('JS8 30m', 10130000),"
             "('JS8 20m', 14078000),"
             "('JS8 17m', 18104000),"
             "('JS8 15m', 21078000),"
             "('JS8 12m', 24922000),"
             "('JS8 10m', 28078000),"
             "('JS8 6m', 50318000));"
             "INSERT OR IGNORE INTO digital_modes(label, freq, mode, type) "
             "SELECT column1, column2, 3, %d FROM (VALUES "
             "('GhostNet 80m', 3575000),"
             "('GhostNet 40m', 7107000),"
             "('GhostNet 20m', 14107000))",
             CFG_DIG_TYPE_JS8, CFG_DIG_TYPE_JS8_GHOSTNET);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        printf("JS8 db: cannot add the JS8 frequency lists: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}
