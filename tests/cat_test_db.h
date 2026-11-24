#pragma once

// An in-memory settings database for the CAT tests (test_civ, test_cat_lan):
// the tables the cfg layer reads, opened and closed with the test.

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include "cfg/db.h"
#include "cfg/settings_internal.h"
#include "cfg/settings_manager.h"

// ---- TestDbGuard ------------------------------------------------------------
namespace {

struct TestDbGuard {
    sqlite3 *db = nullptr;

    TestDbGuard() {
        REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
        char *err = nullptr;
        int   rc  = sqlite3_exec(db,
                                 "CREATE TABLE IF NOT EXISTS params ("
                                     "  name TEXT PRIMARY KEY,"
                                     "  val  INTEGER"
                                     ");"
                                     "CREATE TABLE IF NOT EXISTS band_params("
                                     "  bands_id INTEGER,"
                                     "  name     TEXT,"
                                     "  val      INTEGER,"
                                     "  UNIQUE (bands_id, name) ON CONFLICT REPLACE"
                                     ");"
                                     "CREATE TABLE IF NOT EXISTS mode_params("
                                     "  mode INTEGER,"
                                     "  name TEXT,"
                                     "  val  INTEGER,"
                                     "  UNIQUE (mode, name) ON CONFLICT REPLACE"
                                     ");"
                                     "CREATE TABLE IF NOT EXISTS bands("
                                     "  id         INTEGER PRIMARY KEY,"
                                     "  name       TEXT,"
                                     "  start_freq INTEGER,"
                                     "  stop_freq  INTEGER,"
                                     "  type       INTEGER"
                                     ");"
                                     "CREATE TABLE IF NOT EXISTS transverter("
                                     "  id   INTEGER,"
                                     "  name TEXT,"
                                     "  val  INTEGER,"
                                     "  UNIQUE(id, name) ON CONFLICT REPLACE"
                                     ");",
                                 nullptr, nullptr, &err);
        REQUIRE(rc == SQLITE_OK);
        if (err)
            sqlite3_free(err);
        KeyValueTable<StorageType::GLOBAL>::Init(db);
        KeyValueTable<StorageType::BAND>::Init(db);
        KeyValueTable<StorageType::MODE>::Init(db);
        BandsTable::Init(db);
        KeyValueTable<StorageType::TRANSVERTER>::Init(db);
    }

    ~TestDbGuard() {
        KeyValueTable<StorageType::TRANSVERTER>::Shutdown();
        BandsTable::Shutdown();
        KeyValueTable<StorageType::GLOBAL>::Shutdown();
        KeyValueTable<StorageType::BAND>::Shutdown();
        KeyValueTable<StorageType::MODE>::Shutdown();
        if (db) {
            sqlite3_close(db);
        }
    }
};

} // namespace
