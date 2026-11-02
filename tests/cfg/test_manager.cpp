// test_manager.cpp
// Integration tests for SettingsManager: init load, band/mode switching
// semantics (explicit/implicit), VFO retention, deferred-write round-trip
// against an in-memory SQLite database.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <chrono>
#include <thread>

#include "db.h"
#include "settings_manager.h"
#include "tests/cfg/mocks/pending_writes_test_access.h"

extern "C" {
#include <aether_radio/x6100_control/control.h>
}

namespace {

// Frequency conversion helper: express band boundaries in kHz for readability.
constexpr int32_t kHz = 1000;

void insert_band(sqlite3 *db, int id, int start, int stop, int type = 1) {
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "INSERT INTO bands(id, name, start_freq, stop_freq, type) "
                  "VALUES(%d, 'test', %d, %d, %d);",
                  id, start, stop, type);
    REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}

// In-memory DB fixture: creates all three params tables and initialises the
// shared prepared statements of every table class used by the storage
// policies. A BAND/MODE round-trip fails with rc=21 (SQLITE_MISUSE) if
// KeyValueTable<BAND>/KeyValueTable<MODE> is not initialised.
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

TEST_CASE("SettingsManager init_load loads global/band/mode params", "[manager]") {
    TestDbGuard db;

    // Pre-populate the DB with known values for band 5 / MODE_GROUP_DIGI.
    {
        REQUIRE(store_save<int32_t>(StorageType::GLOBAL, 0, "vol", 45) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100'000) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 14'300'000) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_B) == SUCCESS);
        // The starting mode is derived from the active VFO (B) -> 3.
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_usb_dig) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_DIGI, "freq_step", 9) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    REQUIRE(mgr.current_band_id() == 5);
    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_DIGI);

    REQUIRE(mgr.p_volume.get() == 45);
    REQUIRE(mgr.p_band_vfoa_freq.get() == 7'100'000);
    REQUIRE(mgr.p_band_vfob_freq.get() == 14'300'000);
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    REQUIRE(mgr.p_mode_freq_step.get() == 9);

    // fg_freq mirrors the active VFO (current_vfo == 1 -> vfob).
    REQUIRE(mgr.cp_fg_freq.get() == 14'300'000);
}

TEST_CASE("SettingsManager deferred writes round-trip after flush", "[manager]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // Change parameters; the new values go to the journal (not the DB yet).
    mgr.p_volume.set(45);
    mgr.p_band_if_shift.set(10);
    REQUIRE(store_load<int32_t>(StorageType::GLOBAL, 0, "vol") != 45);

    mgr.flush_all();

    // Values are now persisted.
    REQUIRE(store_load<int32_t>(StorageType::GLOBAL, 0, "vol") == 45);
    auto if_shift = store_load<int32_t>(StorageType::BAND, 5, "if_shift");
    REQUIRE(if_shift.has_value());
    REQUIRE(*if_shift == 10);

    // A fresh manager loads the persisted values.
    SettingsManager mgr2;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr2.init_load();
    REQUIRE(mgr2.p_volume.get() == 45);
    REQUIRE(mgr2.p_band_if_shift.get() == 10);
}

TEST_CASE("band_id persists globally and is restored on a fresh manager", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 7'000 * kHz, 7'200 * kHz);
    insert_band(db.db, 6, 10'000 * kHz, 10'100 * kHz);

    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfo", X6100_VFO_A) == SUCCESS);
    }

    // Start on band 7, then switch explicitly to band 6.
    SettingsManager mgr;
    mgr.init_load();
    // store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    REQUIRE(mgr.current_band_id() != 6);

    mgr.p_band_id.set(6);
    REQUIRE(mgr.p_band_id.get() == 6);
    REQUIRE(mgr.current_band_id() == 6);

    // The switch enqueued a deferred GLOBAL write for band_id; flush persists it.
    mgr.flush_all();
    REQUIRE(store_load<int32_t>(StorageType::GLOBAL, 0, "band_id").value() == 6);

    // A fresh manager derives its starting band from the persisted global value.
    SettingsManager mgr2;
    mgr2.init_load();
    REQUIRE(mgr2.current_band_id() == 6);
}

TEST_CASE("band switch via p_band_id loads new band, keeps VFO reference", "[manager]") {
    TestDbGuard db;

    // Band 5: VFO reference = A (0).
    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 7'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", 0) == SUCCESS);
        // Band 6 has different frequencies and no current_vfo row (missing).
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_freq", 10'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_freq", 10'160 * kHz) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A);
    REQUIRE(mgr.cp_fg_freq.get() == 7'100 * kHz);

    // Change VFO reference to B on the current band, then switch explicitly.
    mgr.p_band_current_vfo.set(X6100_VFO_B);
    mgr.p_band_id.set(6);

    REQUIRE(mgr.current_band_id() == 6);
    // The persisted band id tracks the new band.
    REQUIRE(mgr.p_band_id.get() == 6);
    // current_vfo is NOT loaded on explicit switch: it keeps B (1).
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    // Both frequency slots loaded from band 6.
    REQUIRE(mgr.p_band_vfoa_freq.get() == 10'150 * kHz);
    REQUIRE(mgr.p_band_vfob_freq.get() == 10'160 * kHz);
    // fg_freq = active VFO (B) frequency of band 6.
    REQUIRE(mgr.cp_fg_freq.get() == 10'160 * kHz);
}

TEST_CASE("frequency set into new band switches implicitly", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 7'000 * kHz, 7'200 * kHz);
    insert_band(db.db, 6, 3'600 * kHz, 4'000 * kHz);

    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 7'150 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_freq", 3'650 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_freq", 3'800 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfo", X6100_VFO_B) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A);
    REQUIRE(mgr.cp_fg_freq.get() == 7'100 * kHz);

    // Setting the active VFO frequency into band 6 range triggers an implicit
    // band switch; the frequency of the active VFO (A) is preserved.
    mgr.p_band_vfoa_freq.set(3'720 * kHz);

    REQUIRE(mgr.current_band_id() == 6);
    REQUIRE(mgr.p_band_vfoa_freq.get() == 3'720 * kHz); // active VFO freq kept
    REQUIRE(mgr.p_band_vfob_freq.get() == 3'800 * kHz); // inactive VFO loaded from DB
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A);
    REQUIRE(mgr.cp_fg_freq.get() == 3'720 * kHz);
}

TEST_CASE("switch_mode saves and loads mode params", "[manager]") {
    TestDbGuard db;

    // The starting mode group is derived from the active VFO (A) -> 3.
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_usb) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_SSB, "freq_step", 5) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_CW, "freq_step", 15) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.p_mode_freq_step.get() == 5);

    mgr.p_mode_freq_step.set(11);
    mgr.cp_cur_mode.set(x6100_mode_cwr);

    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_CW);
    REQUIRE(mgr.p_mode_freq_step.get() == 15);
}

TEST_CASE("init_load loads vfo modes, cur_mode mirrors active VFO", "[manager]") {
    TestDbGuard db;

    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_usb) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_am) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_usb);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_am);
    // active VFO = A (0) -> cur_mode mirrors vfoa_mode.
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_usb);
}

TEST_CASE("cp_cur_mode.set writes active VFO mode and triggers switch_mode", "[manager]") {
    TestDbGuard db;

    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_usb) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_nfm) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_SSB, "freq_step", 9) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_CW, "freq_step", 15) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_usb);
    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_SSB);

    mgr.cp_cur_mode.set(x6100_mode_cw);

    // Writes back into the active (A) VFO's mode param.
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_cw);
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_cw);
    // switch_mode() was triggered by the cp_cur_mode change.
    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_CW);
    REQUIRE(mgr.p_mode_freq_step.get() == 15);
}

TEST_CASE("VFO toggle changes cur_mode and triggers switch_mode", "[manager]") {
    TestDbGuard db;

    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_lsb) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_usb_dig) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_SSB, "freq_step", 510) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_DIGI, "freq_step", 520) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_lsb);
    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_SSB);

    // Switch active VFO to B: cur_mode now reflects vfob_mode and switch_mode
    // follows the new mode context.
    mgr.p_band_current_vfo.set(X6100_VFO_B);

    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_usb_dig);
    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_DIGI);
    REQUIRE(mgr.p_mode_freq_step.get() == 520);
}

TEST_CASE("band switch via p_band_id loads both vfo modes", "[manager]") {
    TestDbGuard db;

    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_lsb) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_lsb_dig) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfo", X6100_VFO_B) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_mode", x6100_mode_cwr) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_mode", x6100_mode_am) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_lsb);

    mgr.p_band_id.set(6);

    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A); // VFO reference kept
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_cwr);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_am);
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_cwr); // active (A) VFO mode of band 6
}

TEST_CASE("frequency switch loads active VFO mode from DB", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 7'000 * kHz, 7'200 * kHz);
    insert_band(db.db, 6, 14'000 * kHz, 14'350 * kHz);

    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_B) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 7'150 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_cwr) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_usb_dig) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfo", X6100_VFO_A) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_freq", 14'100 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_freq", 14'150 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_mode", x6100_mode_cw) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_mode", x6100_mode_lsb_dig) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_usb_dig);

    // Change active VFO mode on band 5, then cross bands by setting frequency.
    // The implicit switch loads the mode from band 6's DB — the local change is
    // overwritten by the stored band-6 mode.
    mgr.p_band_vfoa_mode.set(x6100_mode_usb);
    mgr.cp_fg_freq.set(14'200 * kHz);

    REQUIRE(mgr.current_band_id() == 6);
    REQUIRE(mgr.p_band_vfob_freq.get() == 14'200 * kHz); // new frequency preserved
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_cw);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_lsb_dig);
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_lsb_dig);
}

// Helpers shared by the VFO-restore tests below.
namespace {

// initialise `mgr` against an empty band_params table for `band_id`.
void fresh_manager(SettingsManager &mgr, TestDbGuard &db, int band_id) {
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", band_id);
    mgr.init_load();
}

} // namespace

TEST_CASE("explicit switch ignores stored vfo, cp_fg_freq tracks active VFO", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 7'000 * kHz, 7'200 * kHz);
    insert_band(db.db, 6, 14'000 * kHz, 14'350 * kHz);
    // Band 6 stores vfo=X6100_VFO_B — different from the current VFO reference.
    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 7'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_freq", 14'100 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_freq", 14'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfo", X6100_VFO_B) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A);
    REQUIRE(mgr.cp_fg_freq.get() == 7'100 * kHz);

    // Explicit switch ignores the stored vfo=1: the active VFO reference stays A.
    mgr.p_band_id.set(6);

    REQUIRE(mgr.current_band_id() == 6);
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A);
    REQUIRE(mgr.p_band_vfoa_freq.get() == 14'100 * kHz);
    REQUIRE(mgr.p_band_vfob_freq.get() == 14'150 * kHz);
    // cp_fg_freq mirrors the active VFO (A) -> vfoa_freq of band 6.
    REQUIRE(mgr.cp_fg_freq.get() == 14'100 * kHz);
}

TEST_CASE("cp_fg_freq.set doing band switch but preserve p_band_current_vfo and cp_fg_freq", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 7'000 * kHz, 7'200 * kHz);
    insert_band(db.db, 6, 14'000 * kHz, 14'350 * kHz);
    // Band 6 stores vfo=0 (A) while the current VFO reference is B.
    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 7'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_B) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_freq", 14'100 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_freq", 14'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfo", X6100_VFO_A) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    REQUIRE(mgr.cp_fg_freq.get() == 7'150 * kHz);

    // Implicit switch to band 6
    mgr.cp_fg_freq.set(14'200 * kHz);
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);

    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    REQUIRE(mgr.cp_fg_freq.get() == 14'200 * kHz);
    REQUIRE(mgr.p_band_vfob_freq.get() == 14'200 * kHz); // write target = B VFO
    REQUIRE(mgr.p_band_vfoa_freq.get() == 14'100 * kHz); // A VFO untouched
}

TEST_CASE("Active VFO frequency set doing band switch but preserve p_band_current_vfo and new freq", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 7'000 * kHz, 7'200 * kHz);
    insert_band(db.db, 6, 14'000 * kHz, 14'350 * kHz);
    // Band 6 stores vfo=0 (A) while the current VFO reference is B.
    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 7'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_B) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfoa_freq", 14'100 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfob_freq", 14'150 * kHz) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 6, "vfo", X6100_VFO_A) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    REQUIRE(mgr.cp_fg_freq.get() == 7'150 * kHz);

    // Implicit switch to band 6 (p_band_vfob_freq is active freq)
    mgr.p_band_vfob_freq.set(14'200 * kHz);
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);

    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    REQUIRE(mgr.cp_fg_freq.get() == 14'200 * kHz);
    REQUIRE(mgr.p_band_vfob_freq.get() == 14'200 * kHz); // write target = B VFO
    REQUIRE(mgr.p_band_vfoa_freq.get() == 14'100 * kHz); // A VFO untouched
}

TEST_CASE("VFO restore on an empty high band (> 10 MHz)", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 10'100 * kHz, 10'150 * kHz);

    SettingsManager mgr;
    fresh_manager(mgr, db, 5);

    // NOT_FOUND in band_params -> restore from the band definition.
    REQUIRE(mgr.p_band_vfoa_freq.get() == 10'100 * kHz);
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_usb);
    // vfob copies the restored vfoa freq/mode.
    REQUIRE(mgr.p_band_vfob_freq.get() == 10'100 * kHz);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_usb);
}

TEST_CASE("VFO restore picks LSB on a low band", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 1, 1'400 * kHz, 2'000 * kHz);

    SettingsManager mgr;
    fresh_manager(mgr, db, 1);

    REQUIRE(mgr.p_band_vfoa_freq.get() == 1'400 * kHz);
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_lsb);
    REQUIRE(mgr.p_band_vfob_freq.get() == 1'400 * kHz);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_lsb);
}

TEST_CASE("VFO boundary clamp of a DB value outside all HW ranges", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 10'100 * kHz, 10'150 * kHz);

    // vfoa_freq = 999.999 MHz is above TV1's upper boundary; vfob_freq = 1 kHz is
    // below HF's lower boundary. Both are outside every hardware-usable range,
    // so they clamp to the *nearest* HW-valid boundary (not the band start).
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 999'999 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 1 * kHz) == SUCCESS);

    SettingsManager mgr;
    fresh_manager(mgr, db, 5);

    // 999.999 MHz -> nearest boundary is transverter 1's 'to' (438 MHz).
    REQUIRE(mgr.p_band_vfoa_freq.get() == 438'000'000);
    // 1 kHz -> nearest boundary is HF's lower limit (0.1 MHz).
    REQUIRE(mgr.p_band_vfob_freq.get() == 100'000);
}

TEST_CASE("vfob copies a loaded vfoa freq and restores its mode", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 21'000 * kHz, 21'450 * kHz);

    // Only vfoa_freq is seeded (in-band); vfob_* and vfoa_mode are absent.
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 21'200 * kHz) == SUCCESS);

    SettingsManager mgr;
    fresh_manager(mgr, db, 5);

    REQUIRE(mgr.p_band_vfoa_freq.get() == 21'200 * kHz);
    // vfoa_mode is restored from the loaded freq (14.2 MHz > 10 MHz -> USB).
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_usb);
    // vfob_freq copies the loaded vfoa_freq; vfob_mode copies vfoa_mode.
    REQUIRE(mgr.p_band_vfob_freq.get() == 21'200 * kHz);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_usb);
}

TEST_CASE("VFO restore is quiet (no deferred write enqueued)", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 10'100 * kHz, 10'150 * kHz);

    SettingsManager mgr;
    fresh_manager(mgr, db, 5);

    // Restored VFO values were applied with set_quiet: nothing is pending.
    REQUIRE(PendingWritesTestAccess::peek_int(mgr.pending_writes_, StorageKey{StorageType::BAND, 5, "vfoa_freq"})
                .has_value() == false);
    REQUIRE(PendingWritesTestAccess::peek_int(mgr.pending_writes_, StorageKey{StorageType::BAND, 5, "vfob_freq"})
                .has_value() == false);
    REQUIRE(PendingWritesTestAccess::peek_int(mgr.pending_writes_, StorageKey{StorageType::BAND, 5, "vfoa_mode"})
                .has_value() == false);
    REQUIRE(PendingWritesTestAccess::peek_int(mgr.pending_writes_, StorageKey{StorageType::BAND, 5, "vfob_mode"})
                .has_value() == false);
}

TEST_CASE("VFO restore default on undefined band", "[manager]") {
    TestDbGuard db;
    // No bands row at all: get_by_id fails -> vfoa_freq falls back to the
    // 12000000 default and the mode is derived from it (USB, >= 10 MHz).
    SettingsManager mgr;
    fresh_manager(mgr, db, 7);

    REQUIRE(mgr.p_band_vfoa_freq.get() == 12'000 * kHz);
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_usb);
    REQUIRE(mgr.p_band_vfob_freq.get() == 12'000 * kHz);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_usb);
}

TEST_CASE("frequency switch keeps active VFO, restores inactive", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 10'000 * kHz, 11'000 * kHz);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // Tune active VFO A inside band 5.
    mgr.p_band_vfoa_freq.set(10'200 * kHz);

    // Insert band 6 (non-overlapping) and cross into it by setting the active
    // VFO frequency.
    insert_band(db.db, 6, 14'000 * kHz, 14'350 * kHz);
    mgr.p_band_vfoa_freq.set(14'100 * kHz);

    // Active VFO A is kept (not restored), B copies active VFO; modes default
    // to USB (band ≥ 10 MHz).
    REQUIRE(mgr.current_band_id() == 6);
    REQUIRE(mgr.p_band_vfoa_freq.get() == 14'100 * kHz);
    REQUIRE(mgr.p_band_vfob_freq.get() == 14'100 * kHz);
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_usb);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_usb);
}

TEST_CASE("explicit band switch restores both VFOs", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 10'100 * kHz, 20'000 * kHz);
    insert_band(db.db, 6, 1'400 * kHz, 2'000 * kHz);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", 0) == SUCCESS);

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    mgr.p_band_vfoa_freq.set(10'200 * kHz);
    mgr.p_band_id.set(6);

    // Explicit switch restores both VFOs from the low band 6 definition.
    REQUIRE(mgr.p_band_vfoa_freq.get() == 1'400 * kHz);
    REQUIRE(mgr.p_band_vfoa_mode.get() == x6100_mode_lsb);
    REQUIRE(mgr.p_band_vfob_freq.get() == 1'400 * kHz);
    REQUIRE(mgr.p_band_vfob_mode.get() == x6100_mode_lsb);
    // current_vfo is never touched by a switch.
    REQUIRE(mgr.p_band_current_vfo.get() == 0);
}

TEST_CASE("flush thread lifecycle starts and stops cleanly", "[manager][threads]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    mgr.start_flush_thread();
    REQUIRE(mgr.current_band_id() == 5);

    // Starting twice is a no-op (does not spawn a second thread).
    mgr.start_flush_thread();

    mgr.stop_flush_thread();
    REQUIRE(mgr.current_band_id() == 5);

    // The destructor also calls stop_flush_thread(); joining an already-stopped
    // thread is safe.
    mgr.stop_flush_thread();
}

TEST_CASE("flush thread persists pending writes within the wake timeout", "[manager][threads]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // Queue a write (not yet in the DB).
    mgr.p_volume.set(45);
    REQUIRE(store_load<int32_t>(StorageType::GLOBAL, 0, "vol") != 45);

    mgr.start_flush_thread();

    // Poll the DB for up to ~5 s (the thread wakes on a 3 s timeout).
    bool       persisted = false;
    const auto deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (store_load<int32_t>(StorageType::GLOBAL, 0, "vol") == 45) {
            persisted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(persisted);
    mgr.stop_flush_thread();
}

TEST_CASE("parameter validators clamp out-of-range values on set", "[manager]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // Clamping validators bound the documented ranges.
    mgr.p_volume.set(500);
    REQUIRE(mgr.p_volume.get() == 55);

    mgr.p_squelch.set(-50);
    REQUIRE(mgr.p_squelch.get() == 0);

    mgr.p_mode_zoom.set(1000);
    REQUIRE(mgr.p_mode_zoom.get() == 8);

    mgr.p_band_current_vfo.set(7);
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
}

TEST_CASE("numeric validators clamp to the UI-derived ranges", "[manager][validators]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // Exceptions (ranges NOT taken from the UI clip).
    mgr.p_volume.set(500);                      // CTRL_VOL 0..55
    REQUIRE(mgr.p_volume.get() == 55);
    mgr.p_pwr.set(-5.0f);                       // CTRL_PWR 0.1..10 (UI min was 0.0)
    REQUIRE(mgr.p_pwr.get() == 0.1f);
    mgr.p_imic.set(200);                        // CTRL_IMIC 0..50 (UI max was 35)
    REQUIRE(mgr.p_imic.get() == 50);

    // Numeric ranges from the UI clip.
    mgr.p_rit.set(99999);                       // -1500..1500
    REQUIRE(mgr.p_rit.get() == 1500);
    mgr.p_xit.set(-99999);
    REQUIRE(mgr.p_xit.get() == -1500);
    mgr.p_key_speed.set(100);                   // 5..50
    REQUIRE(mgr.p_key_speed.get() == 50);
    mgr.p_key_vol.set(-5);                      // 0..32
    REQUIRE(mgr.p_key_vol.get() == 0);
    mgr.p_key_tone.set(99999);                  // 400..1200
    REQUIRE(mgr.p_key_tone.get() == 1200);
    mgr.p_band_if_shift.set(99999999);          // -40000..40000
    REQUIRE(mgr.p_band_if_shift.get() == 40000);
    mgr.p_vox_delay.set(0);                     // 100..2000
    REQUIRE(mgr.p_vox_delay.get() == 100);
    mgr.p_comp.set(999);                        // 1..8
    REQUIRE(mgr.p_comp.get() == 8);
    mgr.p_ant_id.set(99);                       // 0..5
    REQUIRE(mgr.p_ant_id.get() == 5);
    mgr.p_nr_level.set(999);                    // 0..60
    REQUIRE(mgr.p_nr_level.get() == 60);

    // Float scalars.
    mgr.p_key_ratio.set(10.0f);                 // 2.5..4.5
    REQUIRE(mgr.p_key_ratio.get() == 4.5f);
    mgr.p_cw_decoder_snr.set(0.0f);             // 5.0..30.0
    REQUIRE(mgr.p_cw_decoder_snr.get() == 5.0f);
}

TEST_CASE("enum validators clamp to the documented bounds", "[manager][validators]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    mgr.p_mic.set(99);                          // 0..2
    REQUIRE(mgr.p_mic.get() == (int32_t)x6100_mic_auto);
    mgr.p_key_mode.set(-1);                     // 0..2
    REQUIRE(mgr.p_key_mode.get() == 0);
    mgr.p_iambic_mode.set(99);                  // 0..1
    REQUIRE(mgr.p_iambic_mode.get() == (int32_t)x6100_iambic_b);
    mgr.p_ft8_protocol.set(99);                 // 0..1
    REQUIRE(mgr.p_ft8_protocol.get() == (int32_t)FTX_PROTOCOL_FT8);
    mgr.p_band_vfoa_att.set(99);                // 0..1
    REQUIRE(mgr.p_band_vfoa_att.get() == (int32_t)x6100_att_on);
    mgr.p_band_vfoa_pre.set(99);                // 0..1
    REQUIRE(mgr.p_band_vfoa_pre.get() == (int32_t)x6100_pre_on);
    mgr.p_band_vfoa_agc.set(99);                // 0..3
    REQUIRE(mgr.p_band_vfoa_agc.get() == (int32_t)x6100_agc_auto);
}

TEST_CASE("boolean validators clamp to 0/1", "[manager][validators]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    mgr.p_vox_en.set(7);
    REQUIRE(mgr.p_vox_en.get() == 1);
    mgr.p_dnf.set(-3);
    REQUIRE(mgr.p_dnf.get() == 0);
    mgr.p_band_split.set(5);
    REQUIRE(mgr.p_band_split.get() == 1);
}

TEST_CASE("set to an already-clamped value does not enqueue a write", "[manager]") {
    TestDbGuard db;

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // 500 clamps to 55; pending write holds 55.
    mgr.p_volume.set(500);
    REQUIRE(
        PendingWritesTestAccess::peek_int(mgr.pending_writes_, StorageKey{StorageType::GLOBAL, 0, "vol"}).value() ==
        55);

    // Setting 55 again is already the clamped value -> no change -> no write.
    mgr.p_volume.set(55);
    REQUIRE(
        PendingWritesTestAccess::peek_int(mgr.pending_writes_, StorageKey{StorageType::GLOBAL, 0, "vol"}).value() ==
        55);
}

TEST_CASE("encoder_bind round-trip via DB", "[manager]") {
    TestDbGuard     db;
    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    auto def = mgr.p_encoder_bind.get();
    REQUIRE(def.size() == static_cast<size_t>(CTRL_FAST_ACCESS_LAST));
    REQUIRE(def[CTRL_VOL] == ENCODER_BIND_VOL);
    REQUIRE(def[CTRL_HMIC] == ENCODER_BIND_VOL);
    REQUIRE(def[CTRL_DNF] == ENCODER_BIND_MFK);
    REQUIRE(def[CTRL_SQL] == ENCODER_BIND_NONE);

    std::string custom(CTRL_FAST_ACCESS_LAST, ENCODER_BIND_NONE);
    custom[CTRL_FILTER_LOW] = ENCODER_BIND_MFK;
    mgr.p_encoder_bind.set(custom);
    mgr.flush_all();

    SettingsManager mgr2;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr2.init_load();
    REQUIRE(mgr2.p_encoder_bind.get() == custom);
}

TEST_CASE("encoder_bind validator pads/truncates to FAST_ACCESS_LAST", "[manager]") {
    TestDbGuard     db;
    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    mgr.p_encoder_bind.set("VV");
    REQUIRE(mgr.p_encoder_bind.get().size() == static_cast<size_t>(CTRL_FAST_ACCESS_LAST));
    REQUIRE(mgr.p_encoder_bind.get()[0] == 'V');
    REQUIRE(mgr.p_encoder_bind.get()[2] == ENCODER_BIND_NONE);

    std::string long_str(100, 'X');
    mgr.p_encoder_bind.set(long_str);
    REQUIRE(mgr.p_encoder_bind.get().size() == static_cast<size_t>(CTRL_FAST_ACCESS_LAST));
}

TEST_CASE("encoder_bind deferred write enqueued on change", "[manager]") {
    TestDbGuard     db;
    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    std::string val(CTRL_FAST_ACCESS_LAST, ENCODER_BIND_NONE);
    val[CTRL_VOL] = ENCODER_BIND_VOL;
    mgr.p_encoder_bind.set(val);

    auto stored =
        PendingWritesTestAccess::peek_text(mgr.pending_writes_, StorageKey{StorageType::GLOBAL, 0, "encoder_bind"});
    REQUIRE(stored.has_value());
    REQUIRE(*stored == val);
}

// Helpers for the mode-filter tests below.
namespace {

// Initialise a fresh manager whose active VFO mode == `mode` and whose MODE
// context is `mode`, with filter_low/filter_high seeded for that mode. This
// mirrors production (cp_cur_mode == mode context) so filter_mode() picks the
// right category and reads the right MODE pair.
void init_filter_manager(SettingsManager &mgr, TestDbGuard &db, int32_t mode, ModeGroup group, int low, int high) {
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", 0) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", mode) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, group, "filter_low", low) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, group, "filter_high", high) == SUCCESS);
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
}

} // namespace

TEST_CASE("freq_step/spectrum_factor default and clamp", "[manager]") {
    TestDbGuard     db;
    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // Test for default values
    REQUIRE(mgr.p_mode_freq_step.get() == 500);
    REQUIRE(mgr.p_mode_zoom.get() == 1);

    mgr.p_mode_freq_step.set(20000);
    REQUIRE(mgr.p_mode_freq_step.get() == 10000);
    mgr.p_mode_freq_step.set(0);
    REQUIRE(mgr.p_mode_freq_step.get() == 1);

    mgr.p_mode_zoom.set(100);
    REQUIRE(mgr.p_mode_zoom.get() == 8);
    mgr.p_mode_zoom.set(0);
    REQUIRE(mgr.p_mode_zoom.get() == 1);
}

TEST_CASE("cur_filter_* per category from DB-seeded filter pair", "[manager]") {
    TestDbGuard db;

    // SSB (usb): low/high mirror the stored pair directly.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_usb, MODE_GROUP_SSB, 100, 3000);
        REQUIRE(mgr.cp_cur_filter_low.get() == 100);
        REQUIRE(mgr.cp_cur_filter_high.get() == 3000);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 2900);
    }
    // AM: low == 0, bw == filter_high.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_am, MODE_GROUP_AM,  500, 5000);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);
        REQUIRE(mgr.cp_cur_filter_high.get() == 5000);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 5000);
    }
    // FM (nfm): low == 0, bw == filter_high.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_nfm, MODE_GROUP_FM, 500, 6000);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);
        REQUIRE(mgr.cp_cur_filter_high.get() == 6000);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 6000);
    }
    // CW: centred on key_tone
    {
        SettingsManager mgr;
        // BW = 400, save low=0, high=bw
        init_filter_manager(mgr, db, x6100_mode_cw,  MODE_GROUP_CW, 0, 400);
        auto key_tone = mgr.p_key_tone.get();
        REQUIRE(mgr.cp_cur_filter_low.get() == key_tone - 200);
        REQUIRE(mgr.cp_cur_filter_high.get() == key_tone + 200);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 400);
    }
}

TEST_CASE("cp_cur_filter_low/high.set reverse into MODE filter params", "[manager]") {
    TestDbGuard db;

    // SSB: writes the matching filter param.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_usb, MODE_GROUP_SSB, 200, 3000);
        mgr.cp_cur_filter_low.set(300);
        REQUIRE(mgr.cp_cur_filter_low.get() == 300);
        REQUIRE(mgr.cp_cur_filter_high.get() == 3000);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 2700);

        mgr.cp_cur_filter_high.set(3300);
        REQUIRE(mgr.cp_cur_filter_high.get() == 3300);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 3000);
    }
    // AM: low is a no-op; high writes filter_high.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_am, MODE_GROUP_AM, 500, 3000);
        mgr.cp_cur_filter_low.set(200);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0); // unchanged (always 0)
        mgr.cp_cur_filter_high.set(2500);
        REQUIRE(mgr.cp_cur_filter_high.get() == 2500);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 2500);
    }
    // CW: low/high map back to filter_high = 2*(key_tone -/+ v).
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_cw, MODE_GROUP_CW, 100, 300);
        mgr.p_key_tone.set(600);
        mgr.cp_cur_filter_low.set(200);
        REQUIRE(mgr.cp_cur_filter_low.get() == 200);
        REQUIRE(mgr.cp_cur_filter_high.get() == 1000);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 800);

        mgr.cp_cur_filter_high.set(700);
        REQUIRE(mgr.cp_cur_filter_high.get() == 700);
        REQUIRE(mgr.cp_cur_filter_low.get() == 500);

        mgr.p_key_tone.set(800);
        REQUIRE(mgr.cp_cur_filter_high.get() == 900);
        REQUIRE(mgr.cp_cur_filter_low.get() == 700);
    }
    // CW: check, when half bw > key tone
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_cw, MODE_GROUP_CW, 100, 300);
        mgr.p_key_tone.set(600);
        mgr.cp_cur_filter_high.set(1400);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);
        REQUIRE(mgr.cp_cur_filter_high.get() == 1400);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 1400);

        mgr.cp_cur_filter_high.set(1300);
        REQUIRE(mgr.cp_cur_filter_high.get() == 1300);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);

        mgr.cp_cur_filter_low.set(100);
        REQUIRE(mgr.cp_cur_filter_high.get() == 1100);
        REQUIRE(mgr.cp_cur_filter_low.get() == 100);
    }
}

TEST_CASE("cp_cur_filter_bw.set reverse per category", "[manager]") {
    TestDbGuard db;

    // AM: filter_high == bw.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_am, MODE_GROUP_AM, 500, 3000);
        mgr.cp_cur_filter_bw.set(2500);
        REQUIRE(mgr.cp_cur_filter_high.get() == 2500);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 2500);
    }
    // SSB symmetric: edges recentred about (low+high)/2, width preserved.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_usb, MODE_GROUP_SSB, 500, 3000);
        mgr.cp_cur_filter_bw.set(1000);
        REQUIRE(mgr.cp_cur_filter_low.get() == 1250);  // 1750 - 500
        REQUIRE(mgr.cp_cur_filter_high.get() == 2250); // 1750 + 500
        REQUIRE(mgr.cp_cur_filter_bw.get() == 1000);
    }
    // SSB low<0 fallback: low -> 0, high -> bw.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_usb, MODE_GROUP_SSB, 2000, 3000);
        mgr.cp_cur_filter_bw.set(6000);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);
        REQUIRE(mgr.cp_cur_filter_high.get() == 6000);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 6000);
    }
    // CW: filter_high == bw (no symmetric low/high rewrite). The cur_* edges
    // are then centred on key_tone with offset filter_high/2.
    {
        SettingsManager mgr;
        init_filter_manager(mgr, db, x6100_mode_cw, MODE_GROUP_CW,  500, 3000);
        mgr.p_key_tone.set(700);
        mgr.cp_cur_filter_bw.set(2000);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);
        REQUIRE(mgr.cp_cur_filter_high.get() == 2000);
        REQUIRE(mgr.cp_cur_filter_bw.get() == 2000);

        mgr.p_key_tone.set(900);
        REQUIRE(mgr.cp_cur_filter_low.get() == 0);
        REQUIRE(mgr.cp_cur_filter_high.get() == 2000);

        mgr.p_key_tone.set(1100);
        REQUIRE(mgr.cp_cur_filter_low.get() == 100);
        REQUIRE(mgr.cp_cur_filter_high.get() == 2100);
    }
}

TEST_CASE("cur_filter set chain terminates and stays consistent", "[manager]") {
    TestDbGuard     db;
    SettingsManager mgr;
    init_filter_manager(mgr, db, x6100_mode_usb, MODE_GROUP_SSB, 500, 3000);

    // Repeated bw sets (bounded width client) must not recurse/overflow and
    // must keep cur_* mutually consistent each time.
    int last_bw = -1;
    for (int bw = 100; bw <= 6000; bw += 977) {
        mgr.cp_cur_filter_bw.set(bw);
        REQUIRE(mgr.cp_cur_filter_bw.get() >= 10);
        REQUIRE(mgr.cp_cur_filter_high.get() - mgr.cp_cur_filter_low.get() > 0);
        last_bw = mgr.cp_cur_filter_bw.get();
    }
    REQUIRE(last_bw > 0);
    REQUIRE(mgr.cp_cur_filter_high.get() == mgr.cp_cur_filter_low.get() + last_bw);
}

TEST_CASE("switch_mode recomputes cur_filter_* for the new mode's pair", "[manager]") {
    TestDbGuard db;

    // Seed both the SSB (usb) and CW mode pairs, plus the VFO mode.
    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_usb) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_SSB, "filter_low", 500) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_SSB, "filter_high", 3000) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_CW, "filter_low", 0) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_CW, "filter_high", 200) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();
    REQUIRE(mgr.cp_cur_filter_low.get() == 500);
    REQUIRE(mgr.cp_cur_filter_high.get() == 3000);

    // Switch to CW via cp_cur_mode: writes the VFO mode, triggers switch_mode(),
    // loads the CW filter pair, recomputes cur_* for the CW category.
    mgr.cp_cur_mode.set(x6100_mode_cw);
    mgr.p_key_tone.set(700);

    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_CW);
    REQUIRE(mgr.cp_cur_filter_low.get() == 600);  // key_tone - high/2
    REQUIRE(mgr.cp_cur_filter_high.get() == 800); // key_tone + high/2
    REQUIRE(mgr.cp_cur_filter_bw.get() == 200);
}

TEST_CASE("init_load preserves transverter frequency inside HW range", "[manager][transverter]") {
    TestDbGuard db;

    // Band 7 = 20m SSB (14.07-14.35 MHz). The stored VFO frequency (145 MHz) is
    // outside the band but within the first transverter's hardware range
    // (144-150 MHz), so it must be preserved, not clamped to the band start.
    insert_band(db.db, 7, 14'070'000, 14'350'000);

    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 7, "vfo", X6100_VFO_A) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 7, "vfoa_freq", 145'000'000) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 7, "vfob_freq", 145'000'000) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 7, "vfoa_mode", x6100_mode_usb) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 7, "vfob_mode", x6100_mode_usb) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 7, "vfoa_agc", x6100_agc_auto) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 7, "vfob_agc", x6100_agc_auto) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 7);
    mgr.init_load();

    // 145 MHz is hardware-usable (transverter 0: 144-150 MHz), so it survives
    // the band clamp unchanged.
    REQUIRE(mgr.is_valid_hw_freq(145'000'000));
    REQUIRE(mgr.p_band_vfoa_freq.get() == 145'000'000);
    REQUIRE(mgr.cp_fg_freq.get() == 145'000'000);
}

TEST_CASE("init_load clamps frequency outside HW ranges to nearest boundary", "[manager][transverter]") {
    TestDbGuard db;

    // Band 5 = 30m (10.1-10.15 MHz). The stored VFO frequency (65 MHz) is
    // outside both HF (0.5-55 MHz) and all transverter ranges. The nearest
    // hardware-usable boundary is the HF upper limit: 55 MHz.
    insert_band(db.db, 5, 10'100'000, 10'150'000);

    {
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 65'000'000) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 65'000'000) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_lsb) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_lsb) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_agc", x6100_agc_auto) == SUCCESS);
        REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_agc", x6100_agc_auto) == SUCCESS);
    }

    SettingsManager mgr;
    store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5);
    mgr.init_load();

    // 65 MHz is not hardware-usable; it clamps to the nearest boundary (55 MHz).
    REQUIRE_FALSE(mgr.is_valid_hw_freq(65'000'000));
    REQUIRE(mgr.is_valid_hw_freq(55'000'000));
    REQUIRE(mgr.p_band_vfoa_freq.get() == 55'000'000);
    REQUIRE(mgr.cp_fg_freq.get() == 55'000'000);
}

namespace {

struct NotifyCounter {
    int         count = 0;
    static void cb(Subject * /*subj*/, void *user_data) { static_cast<NotifyCounter *>(user_data)->count++; }
};

} // namespace

TEST_CASE("init_load is idempotent: repeated call resets and reloads", "[manager]") {
    TestDbGuard db;
    insert_band(db.db, 5, 7'000 * kHz, 7'200 * kHz);

    REQUIRE(store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_usb) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::MODE, MODE_GROUP_SSB, "freq_step", 9) == SUCCESS);

    SettingsManager mgr;
    mgr.init_load();

    REQUIRE(mgr.current_band_id() == 5);
    REQUIRE(mgr.current_mode_group_id() == MODE_GROUP_SSB);
    REQUIRE(mgr.p_band_vfoa_freq.get() == 7'100 * kHz);
    REQUIRE(mgr.p_mode_freq_step.get() == 9);
    REQUIRE(mgr.cp_fg_freq.get() == 7'100 * kHz);

    // Observe cp_fg_freq to prove a repeated init_load does not accumulate
    // duplicate source subscriptions: each source change must notify once.
    NotifyCounter counter;
    Subscription  obs(mgr.cp_fg_freq.subscribe(NotifyCounter::cb, &counter));

    // Mutate in memory without persisting (p_volume enqueues a deferred write),
    // then re-run init_load: it must reset to defaults and reload the DB values.
    mgr.p_volume.set(45);
    mgr.p_band_vfoa_freq.set_quiet(21'000 * kHz);
    mgr.p_mode_freq_step.set_quiet(5000);

    mgr.init_load();

    REQUIRE(mgr.current_band_id() == 5);
    REQUIRE(mgr.p_band_vfoa_freq.get() == 7'100 * kHz);
    REQUIRE(mgr.p_mode_freq_step.get() == 9);
    REQUIRE(mgr.cp_fg_freq.get() == 7'100 * kHz);
    // The reset cleared the pending deferred write, so the DB is untouched.
    REQUIRE_FALSE(store_load<int32_t>(StorageType::GLOBAL, 0, "vol").has_value());

    // Exactly one notification for one source change (no duplicated observers).
    counter.count = 0;
    mgr.p_band_vfoa_freq.set_quiet(7'050 * kHz);
    REQUIRE(counter.count == 1);
}

namespace {

struct ValueNotifyCounter {
    int         count = 0;
    int32_t     last  = -1;
    static void cb(Subject *subj, void *user_data) {
        auto *self = static_cast<ValueNotifyCounter *>(user_data);
        self->count++;
        self->last = static_cast<SubjectT<int32_t> *>(subj)->get();
    }
};

} // namespace

TEST_CASE("init_load coalesces reset and reload into one notification with DB value", "[manager]") {
    TestDbGuard db;

    // DB value (45) differs from the construction-time default (30).
    REQUIRE(store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::GLOBAL, 0, "vol", 45) == SUCCESS);

    SettingsManager mgr;

    // Subscribe before the first init_load: the reset-to-default must not be
    // delivered as a separate notification before the reloaded DB value.
    ValueNotifyCounter counter;
    Subscription       obs(mgr.p_volume.subscribe(ValueNotifyCounter::cb, &counter));

    mgr.init_load();

    REQUIRE(mgr.p_volume.get() == 45);
    REQUIRE(counter.count == 1);
    REQUIRE(counter.last == 45);
}
