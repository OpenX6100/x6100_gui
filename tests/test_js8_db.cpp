// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Randal Warren VE7NHW
// Part of OpenX6100 (github.com/OpenX6100)
//
// params.db across the move to R1CBU 1.0 (src/cfg/js8_db.c): every kind of
// card gets 1.0's settings conversion exactly once, and JS8's frequency lists.
// Also checked against copies of testers' real cards before it went in.

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <string>

extern "C" {
#include "js8_db.h"
#include "migrations.h"
}

static sqlite3 *g_db;
extern "C" sqlite3 *cfg_db_get(void) { return g_db; }

namespace {

// The tables migrations 2-4 and js8_db touch, as sql/params.sql makes them.
void make_card(int version, bool js8_rows) {
    REQUIRE(sqlite3_open(":memory:", &g_db) == SQLITE_OK);
    std::string sql =
        "CREATE TABLE version(id INT NOT NULL DEFAULT 0);"
        "INSERT INTO version VALUES(" + std::to_string(version) + ");"
        "CREATE TABLE params(name TEXT PRIMARY KEY ON CONFLICT REPLACE, val INTEGER);"
        "CREATE TABLE band_params(bands_id INTEGER, name TEXT, val INTEGER, UNIQUE(bands_id, name) ON CONFLICT REPLACE);"
        "CREATE TABLE atu(freq INTEGER);"
        "CREATE TABLE digital_modes(id INTEGER PRIMARY KEY AUTOINCREMENT, label varchar(64) NOT NULL,"
        " freq INTEGER NOT NULL, mode INTEGER NOT NULL DEFAULT 3, type INTEGER NOT NULL,"
        " CONSTRAINT freq_type_uniq UNIQUE(freq, type));";
    if (js8_rows) sql += "INSERT INTO digital_modes(label, freq, mode, type) VALUES('JS8 40m', 7078000, 3, 2);";
    REQUIRE(sqlite3_exec(g_db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
}

void exec(const std::string &sql) { REQUIRE(sqlite3_exec(g_db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK); }

double value(const char *sql) {
    sqlite3_stmt *st;
    REQUIRE(sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    double v = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return v;
}

double pwr() { return value("SELECT val FROM params WHERE name = 'pwr'"); }

// The version every card ends at: the last migration, whichever that is
// (other features add theirs after 1.0's 4).
int latest_version() {
    sqlite3 *card = g_db;
    make_card(1, false);
    REQUIRE(migrations_apply() == 0);
    int v = (int)value("SELECT id FROM version");
    sqlite3_close(g_db);
    g_db = card;
    return v;
}

// What the GUI does at every start (cfg_db_open).
void start_ours() {
    REQUIRE(js8_db_before_migrations(g_db) == 0);
    REQUIRE(migrations_apply() == 0);
    REQUIRE(js8_db_after_migrations(g_db) == 0);
}

} // namespace

TEST_CASE("every card gets R1CBU 1.0's settings conversion exactly once", "[js8][db]") {
    const int latest = latest_version();
    REQUIRE(latest >= 4);
    SECTION("a card from the JS8 test builds 2-4 (version 5): converted, never twice") {
        make_card(5, true);
        exec("INSERT INTO params VALUES('pwr', 3), ('output_gain', 44), ('js8_speed', 2);"
             "INSERT INTO band_params VALUES(1, 'dac_offset', -52);");
        start_ours();
        CHECK(pwr() == 0.3); // not 3 W
        CHECK(value("SELECT val FROM params WHERE name = 'output_gain'") == 8.8);
        CHECK(value("SELECT val FROM band_params WHERE name = 'dac_offset'") == -5.2);
        CHECK(value("SELECT val FROM params WHERE name = 'js8_speed'") == 2); // ours kept as they are
        CHECK(value("SELECT id FROM version") == latest);
        exec("UPDATE params SET val = 5.0 WHERE name = 'pwr'"); // stored as INTEGER 5
        start_ours();
        CHECK(pwr() == 5.0);
    }
    SECTION("a card from JS8 test build 1 (version 4, JS8's list only)") {
        make_card(4, true);
        exec("INSERT INTO params VALUES('pwr', 50)");
        start_ours();
        CHECK(pwr() == 5.0);
    }
    SECTION("a card from stock R1CBU 1.0.x: already converted, left alone") {
        make_card(3, false);
        exec("INSERT INTO params VALUES('pwr', 50)");
        REQUIRE(migrations_apply() == 0); // what stock 1.0 did
        REQUIRE(pwr() == 5.0);
        start_ours();
        CHECK(pwr() == 5.0); // not 0.5 W
        CHECK(value("SELECT COUNT(*) FROM digital_modes WHERE type = 2") == 10); // JS8's list added
        CHECK(value("SELECT COUNT(*) FROM digital_modes WHERE type = 3") == 3);  // and GhostNet's
    }
    SECTION("a card from stock R1CBU 0.34 or 1KO125 (version 3)") {
        make_card(3, false);
        exec("INSERT INTO params VALUES('pwr', 50)");
        start_ours();
        CHECK(pwr() == 5.0);
        start_ours();
        CHECK(pwr() == 5.0);
    }
    SECTION("a new card (version 1): the lists, nothing to convert") {
        make_card(1, true);
        start_ours();
        CHECK(value("SELECT id FROM version") == latest);
        CHECK(value("SELECT COUNT(*) FROM digital_modes WHERE type IN (2, 3)") == 13);
    }
    sqlite3_close(g_db);
    g_db = nullptr;
}
