/**
 * Work with params table on DB (new cfg version).
 * The sqlite3 connection and prepared statements are shared with the
 * template-based cfg_param_load/cfg_param_save defined in db.h.
 *
 * Each statement group lives as inline-static fields of its table class
 * (stmt + guarding mutex + cached :name/:id/:val parameter indices) with a
 * common prefix per group. Indices are resolved once at Init via
 * sqlite3_bind_parameter_index() and reused on every bind, which avoids
 * per-call name lookups.
 */
#include "db.h"

#include "migrations.h"
#include "js8_db.h"

#include "../lvgl/lvgl.h"
#include <cstdio>
#include <pthread.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// KeyValueTable<Type> — shared implementation for all key-value stores
// ---------------------------------------------------------------------------

template <StorageType Type> bool KeyValueTable<Type>::Init(sqlite3 *database) {
    if (db_) {
        LV_LOG_ERROR("Repeated KeyValueTable initialization for %s", KeyValueTableTraits<Type>::table);
        return false;
    }
    db_ = database;

    using Traits = KeyValueTableTraits<Type>;

    // Build the load/save SQL from the table traits: the GLOBAL `params` table
    // is flat (no key column), the band/mode/transverter tables are keyed.
    char load_sql[160];
    char save_sql[192];
    if constexpr (Traits::has_key) {
        snprintf(load_sql, sizeof(load_sql), "SELECT val FROM %s WHERE %s = :id AND name = :name", Traits::table,
                 Traits::key_col);
        snprintf(save_sql, sizeof(save_sql), "INSERT OR REPLACE INTO %s(%s, name, val) VALUES(:id, :name, :val)",
                 Traits::table, Traits::key_col);
    } else {
        snprintf(load_sql, sizeof(load_sql), "SELECT val FROM %s WHERE name = :name", Traits::table);
        snprintf(save_sql, sizeof(save_sql), "INSERT OR REPLACE INTO %s(name, val) VALUES(:name, :val)", Traits::table);
    }

    int rc = sqlite3_prepare_v2(db_, load_sql, -1, &load_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare load statement for %s: %s", Traits::table, sqlite3_errmsg(db_));
        db_ = nullptr;
        return false;
    }
    load_name_param_index_ = sqlite3_bind_parameter_index(load_stmt_, ":name");
    if constexpr (Traits::has_key) {
        load_id_param_index_ = sqlite3_bind_parameter_index(load_stmt_, ":id");
    }

    rc = sqlite3_prepare_v2(db_, save_sql, -1, &save_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare save statement for %s: %s", Traits::table, sqlite3_errmsg(db_));
        sqlite3_finalize(load_stmt_);
        load_stmt_             = nullptr;
        load_id_param_index_   = 0;
        load_name_param_index_ = 0;
        db_                    = nullptr;
        return false;
    }
    save_name_param_index_ = sqlite3_bind_parameter_index(save_stmt_, ":name");
    save_val_param_index_  = sqlite3_bind_parameter_index(save_stmt_, ":val");
    if constexpr (Traits::has_key) {
        save_id_param_index_ = sqlite3_bind_parameter_index(save_stmt_, ":id");
    }
    return true;
}

template <StorageType Type> void KeyValueTable<Type>::Shutdown() {
    if (load_stmt_) {
        sqlite3_finalize(load_stmt_);
        load_stmt_ = nullptr;
    }
    if (save_stmt_) {
        sqlite3_finalize(save_stmt_);
        save_stmt_ = nullptr;
    }
    load_id_param_index_   = 0;
    load_name_param_index_ = 0;
    save_id_param_index_   = 0;
    save_name_param_index_ = 0;
    save_val_param_index_  = 0;
    db_                    = nullptr;
}

// Explicit instantiations: Init/Shutdown are defined in this TU, so every
// KeyValueTable<...> used by the application (and tests) must be listed here.
template class KeyValueTable<StorageType::GLOBAL>;
template class KeyValueTable<StorageType::BAND>;
template class KeyValueTable<StorageType::MODE>;
template class KeyValueTable<StorageType::TRANSVERTER>;

// ---------------------------------------------------------------------------
// BandsTable
// ---------------------------------------------------------------------------

bool BandsTable::Init(sqlite3 *database) {
    if (db_) {
        LV_LOG_ERROR("Repeated BandsTable initialization");
        return false;
    }
    db_ = database;

    int rc;

    rc = sqlite3_prepare_v2(db_, "SELECT name, start_freq, stop_freq, type FROM bands WHERE id = :id", -1,
                            &get_band_by_id_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare get_band_by_id statement: %s", sqlite3_errmsg(db_));
        db_ = nullptr;
        return false;
    }
    get_band_by_id_id_param_index_ = sqlite3_bind_parameter_index(get_band_by_id_stmt_, ":id");

    rc = sqlite3_prepare_v2(
        db_,
        "SELECT id, name, start_freq, stop_freq FROM bands WHERE "
        "   (:freq >= start_freq) AND (:freq <= stop_freq) AND (type = 1) "
        "UNION SELECT * FROM ("
        "   SELECT NULL, NULL, a.stop_freq, b.start_freq FROM ("
        "       SELECT stop_freq FROM bands WHERE :freq > stop_freq AND type = 1 ORDER BY stop_freq DESC LIMIT 1"
        "   ) AS a FULL OUTER JOIN ("
        "       SELECT start_freq FROM bands WHERE :freq < start_freq AND type = 1 ORDER BY start_freq LIMIT 1"
        "   ) AS b"
        ") ORDER BY id DESC NULLS LAST LIMIT 1",
        -1, &get_band_by_freq_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare get_band_by_freq statement: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(get_band_by_id_stmt_);
        get_band_by_id_stmt_ = nullptr;
        db_                  = nullptr;
        return false;
    }
    get_band_by_freq_freq_param_index_ = sqlite3_bind_parameter_index(get_band_by_freq_stmt_, ":freq");

    rc = sqlite3_prepare_v2(db_,
                            "SELECT id, name, start_freq, stop_freq, type FROM bands "
                            "WHERE :freq <= start_freq AND id != :id AND type = 1 ORDER BY start_freq LIMIT 1",
                            -1, &get_band_up_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare get_band_up statement: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(get_band_by_id_stmt_);
        sqlite3_finalize(get_band_by_freq_stmt_);
        get_band_by_id_stmt_   = nullptr;
        get_band_by_freq_stmt_ = nullptr;
        db_                    = nullptr;
        return false;
    }
    get_band_up_freq_param_index_ = sqlite3_bind_parameter_index(get_band_up_stmt_, ":freq");
    get_band_up_id_param_index_   = sqlite3_bind_parameter_index(get_band_up_stmt_, ":id");

    rc = sqlite3_prepare_v2(db_,
                            "SELECT id, name, start_freq, stop_freq, type FROM bands "
                            "WHERE :freq >= stop_freq AND id != :id AND type = 1 ORDER BY start_freq DESC LIMIT 1",
                            -1, &get_band_down_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare get_band_down statement: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(get_band_by_id_stmt_);
        sqlite3_finalize(get_band_by_freq_stmt_);
        sqlite3_finalize(get_band_up_stmt_);
        get_band_by_id_stmt_   = nullptr;
        get_band_by_freq_stmt_ = nullptr;
        get_band_up_stmt_      = nullptr;
        db_                    = nullptr;
        return false;
    }
    get_band_down_freq_param_index_ = sqlite3_bind_parameter_index(get_band_down_stmt_, ":freq");
    get_band_down_id_param_index_   = sqlite3_bind_parameter_index(get_band_down_stmt_, ":id");

    rc = sqlite3_prepare_v2(db_, "SELECT id, name, start_freq, stop_freq, type FROM bands ORDER BY start_freq", -1,
                            &read_all_bands_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare read_all_bands statement: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(get_band_by_id_stmt_);
        sqlite3_finalize(get_band_by_freq_stmt_);
        sqlite3_finalize(get_band_up_stmt_);
        sqlite3_finalize(get_band_down_stmt_);
        get_band_by_id_stmt_   = nullptr;
        get_band_by_freq_stmt_ = nullptr;
        get_band_up_stmt_      = nullptr;
        get_band_down_stmt_    = nullptr;
        db_                    = nullptr;
        return false;
    }
    return true;
}

void BandsTable::Shutdown() {
    if (get_band_by_id_stmt_) {
        sqlite3_finalize(get_band_by_id_stmt_);
        get_band_by_id_stmt_ = nullptr;
    }
    if (get_band_by_freq_stmt_) {
        sqlite3_finalize(get_band_by_freq_stmt_);
        get_band_by_freq_stmt_ = nullptr;
    }
    if (get_band_up_stmt_) {
        sqlite3_finalize(get_band_up_stmt_);
        get_band_up_stmt_ = nullptr;
    }
    if (get_band_down_stmt_) {
        sqlite3_finalize(get_band_down_stmt_);
        get_band_down_stmt_ = nullptr;
    }
    if (read_all_bands_stmt_) {
        sqlite3_finalize(read_all_bands_stmt_);
        read_all_bands_stmt_ = nullptr;
    }
    get_band_by_id_id_param_index_     = 0;
    get_band_by_freq_freq_param_index_ = 0;
    get_band_up_freq_param_index_      = 0;
    get_band_up_id_param_index_        = 0;
    get_band_down_freq_param_index_    = 0;
    get_band_down_id_param_index_      = 0;
    {
        std::lock_guard<std::mutex> cache_lock(last_band_mutex_);
        last_band = BandInfo{};
    }
    db_ = nullptr;
}

BandInfoLoadResult BandsTable::get_by_id(int32_t band_id) {
    if (band_id == BAND_UNDEFINED) {
        return {BandInfo{}, NOT_FOUND};
    }

    // Fast path: serve from cache without touching the DB.
    {
        std::lock_guard<std::mutex> cache_lock(last_band_mutex_);
        if (last_band.id == band_id) {
            return {last_band, SUCCESS};
        }
    }

    LV_LOG_USER("Loading band info for id: %i", band_id);

    int            rc;
    StmtResetGuard guard(get_band_by_id_mutex_, get_band_by_id_stmt_);

    rc = sqlite3_bind_int(get_band_by_id_stmt_, get_band_by_id_id_param_index_, band_id);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind bands_id %i: %s", band_id, sqlite3_errmsg(db_));
        return {BandInfo{}, rc};
    }
    rc = sqlite3_step(get_band_by_id_stmt_);
    if (rc == SQLITE_ROW) {
        BandInfo info;
        info.id                  = band_id;
        const unsigned char *txt = sqlite3_column_text(get_band_by_id_stmt_, 0);
        info.name                = txt ? reinterpret_cast<const char *>(txt) : "";
        info.start_freq          = sqlite3_column_int(get_band_by_id_stmt_, 1);
        info.stop_freq           = sqlite3_column_int(get_band_by_id_stmt_, 2);
        info.type                = static_cast<band_type_t>(sqlite3_column_int(get_band_by_id_stmt_, 3));
        {
            std::lock_guard<std::mutex> cache_lock(last_band_mutex_);
            last_band = info;
        }
        return {info, SUCCESS};
    }
    LV_LOG_USER("No info for band with id: %i", band_id);
    return {BandInfo{}, NOT_FOUND};
}

BandInfoLoadResult BandsTable::get_by_freq(uint32_t freq) {
    {
        std::lock_guard<std::mutex> cache_lock(last_band_mutex_);
        if ((freq >= last_band.start_freq) && (freq <= last_band.stop_freq)) {
            return {last_band, SUCCESS};
        }
    }

    LV_LOG_USER("Loading band info for freq: %u", freq);
    int            rc;
    StmtResetGuard guard(get_band_by_freq_mutex_, get_band_by_freq_stmt_);
    rc = sqlite3_bind_int(get_band_by_freq_stmt_, get_band_by_freq_freq_param_index_, freq);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind freq %u: %s", freq, sqlite3_errmsg(db_));
        return {BandInfo{}, rc};
    }
    rc = sqlite3_step(get_band_by_freq_stmt_);
    if (rc == SQLITE_ROW) {
        BandInfo info;
        if (sqlite3_column_type(get_band_by_freq_stmt_, 0) != SQLITE_NULL) {
            // Found an active band
            info.id                  = sqlite3_column_int(get_band_by_freq_stmt_, 0);
            const unsigned char *txt = sqlite3_column_text(get_band_by_freq_stmt_, 1);
            info.name                = txt ? reinterpret_cast<const char *>(txt) : "";
            info.type                = BAND_ACTIVE;
        } else {
            // Found a gap between bands
            info.id   = BAND_UNDEFINED;
            info.type = BAND_INACTIVE;
        }
        info.start_freq = sqlite3_column_int(get_band_by_freq_stmt_, 2);
        if (sqlite3_column_type(get_band_by_freq_stmt_, 3) == SQLITE_NULL) {
            info.stop_freq = 0xFFFFFFFFU;
        } else {
            info.stop_freq = sqlite3_column_int(get_band_by_freq_stmt_, 3);
        }
        {
            std::lock_guard<std::mutex> cache_lock(last_band_mutex_);
            last_band = info;
        }
        return {info, SUCCESS};
    }
    LV_LOG_WARN("No band info for freq: %u", freq);
    return {BandInfo{}, NOT_FOUND};
}

BandInfoLoadResult BandsTable::next(int32_t cur_band_id, uint32_t cur_freq, bool up) {
    int           rc;
    sqlite3_stmt *stmt;
    std::mutex   *mux;
    int           freq_idx, band_idx;
    if (up) {
        stmt     = get_band_up_stmt_;
        mux      = &get_band_up_mutex_;
        freq_idx = get_band_up_freq_param_index_;
        band_idx = get_band_up_id_param_index_;
    } else {
        stmt     = get_band_down_stmt_;
        mux      = &get_band_down_mutex_;
        freq_idx = get_band_down_freq_param_index_;
        band_idx = get_band_down_id_param_index_;
    }

    StmtResetGuard guard(*mux, stmt);

    rc = sqlite3_bind_int(stmt, freq_idx, cur_freq);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind freq %u to find up/down stmt: %s", cur_freq, sqlite3_errmsg(db_));
        return {BandInfo{}, rc};
    }
    rc = sqlite3_bind_int(stmt, band_idx, cur_band_id);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to current band id %i to find up/down stmt: %s", cur_band_id, sqlite3_errmsg(db_));
        return {BandInfo{}, rc};
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        BandInfo info;
        info.id                  = sqlite3_column_int(stmt, 0);
        const unsigned char *txt = sqlite3_column_text(stmt, 1);
        info.name                = txt ? reinterpret_cast<const char *>(txt) : "";
        info.type                = BAND_ACTIVE;
        info.start_freq          = sqlite3_column_int(stmt, 2);
        info.stop_freq           = sqlite3_column_int(stmt, 3);
        {
            std::lock_guard<std::mutex> cache_lock(last_band_mutex_);
            last_band = info;
        }
        return {info, SUCCESS};
    }
    LV_LOG_INFO("No next band info for freq: %u, cur_id: %i and direction: %i", cur_freq, cur_band_id, up);
    return {BandInfo{}, NOT_FOUND};
}

std::vector<BandInfo> BandsTable::all_bands() {
    int                   rc;
    StmtResetGuard        guard(read_all_bands_mutex_, read_all_bands_stmt_);
    std::vector<BandInfo> result;
    while (1) {
        rc = sqlite3_step(read_all_bands_stmt_);

        if (rc == SQLITE_ROW) {
            BandInfo info;
            info.id                  = sqlite3_column_int(read_all_bands_stmt_, 0);
            const unsigned char *txt = sqlite3_column_text(read_all_bands_stmt_, 1);
            info.name                = txt ? reinterpret_cast<const char *>(txt) : "";
            info.start_freq          = sqlite3_column_int(read_all_bands_stmt_, 2);
            info.stop_freq           = sqlite3_column_int(read_all_bands_stmt_, 3);
            info.type                = static_cast<band_type_t>(sqlite3_column_int(read_all_bands_stmt_, 4));
            result.push_back(info);
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            LV_LOG_ERROR("Error while reading bands rows: %s", sqlite3_errmsg(db_));
            break;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// MemoryTable
// ---------------------------------------------------------------------------

bool MemoryTable::Init(sqlite3 *database) {
    if (db_) {
        LV_LOG_ERROR("Repeated MemoryTable initialization");
        return false;
    }
    db_ = database;

    int rc;

    rc = sqlite3_prepare_v2(db_, "SELECT name, val FROM memory WHERE id = :id", -1, &load_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare MemoryTable::load: %s", sqlite3_errmsg(db_));
        db_ = nullptr;
        return false;
    }
    load_id_param_index_ = sqlite3_bind_parameter_index(load_stmt_, ":id");

    rc = sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO memory(id, name, val) VALUES(:id, :name, :val)", -1,
                            &save_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare MemoryTable::save: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(load_stmt_);
        load_stmt_           = nullptr;
        load_id_param_index_ = 0;
        db_                  = nullptr;
        return false;
    }
    save_id_param_index_   = sqlite3_bind_parameter_index(save_stmt_, ":id");
    save_name_param_index_ = sqlite3_bind_parameter_index(save_stmt_, ":name");
    save_val_param_index_  = sqlite3_bind_parameter_index(save_stmt_, ":val");
    return true;
}

void MemoryTable::Shutdown() {
    if (load_stmt_) {
        sqlite3_finalize(load_stmt_);
        load_stmt_ = nullptr;
    }
    if (save_stmt_) {
        sqlite3_finalize(save_stmt_);
        save_stmt_ = nullptr;
    }
    load_id_param_index_   = 0;
    save_id_param_index_   = 0;
    save_name_param_index_ = 0;
    save_val_param_index_  = 0;
    db_                    = nullptr;
}

int MemoryTable::Save(int32_t id, const char *name, int32_t value) {
    int            rc;
    StmtResetGuard guard(save_mutex_, save_stmt_);

    rc = sqlite3_bind_int(save_stmt_, save_id_param_index_, id);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind mem id %i: %s", id, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_bind_text(save_stmt_, save_name_param_index_, name, strlen(name), 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind mem name %s: %s", name, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_bind_int(save_stmt_, save_val_param_index_, value);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind mem val %i: %s", value, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_step(save_stmt_);
    if (rc != SQLITE_DONE) {
        LV_LOG_ERROR("Failed save memory item %s: %s", name, sqlite3_errmsg(db_));
        return rc;
    }
    return SUCCESS;
}

bool MemoryTable::Load(int32_t id, int32_t &freq, bool &has_freq, int32_t &mode, bool &has_mode, int32_t &agc,
                       bool &has_agc, int32_t &att, bool &has_att, int32_t &pre, bool &has_pre) {
    freq     = 0;
    mode     = 0;
    agc      = 0;
    att      = 0;
    pre      = 0;
    has_freq = false;
    has_mode = false;
    has_agc  = false;
    has_att  = false;
    has_pre  = false;

    int            rc;
    StmtResetGuard guard(load_mutex_, load_stmt_);

    rc = sqlite3_bind_int(load_stmt_, load_id_param_index_, id);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind mem id %i: %s", id, sqlite3_errmsg(db_));
        return false;
    }
    while (1) {
        rc = sqlite3_step(load_stmt_);
        if (rc == SQLITE_ROW) {
            const unsigned char *name_txt = sqlite3_column_text(load_stmt_, 0);
            const char          *name     = name_txt ? reinterpret_cast<const char *>(name_txt) : "";
            const int32_t        val      = sqlite3_column_int(load_stmt_, 1);
            if (strcmp(name, "vfoa_freq") == 0) {
                freq     = val;
                has_freq = true;
            } else if (strcmp(name, "vfoa_mode") == 0) {
                mode     = val;
                has_mode = true;
            } else if (strcmp(name, "vfoa_agc") == 0) {
                agc     = val;
                has_agc = true;
            } else if (strcmp(name, "vfoa_att") == 0) {
                att     = val;
                has_att = true;
            } else if (strcmp(name, "vfoa_pre") == 0) {
                pre     = val;
                has_pre = true;
            }
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            LV_LOG_ERROR("Error while reading memory rows: %s", sqlite3_errmsg(db_));
            return false;
        }
    }
    return has_freq;
}

// ---------------------------------------------------------------------------
// DigitalModesTable
// ---------------------------------------------------------------------------

bool DigitalModesTable::Init(sqlite3 *database) {
    if (db_) {
        LV_LOG_ERROR("Repeated DigitalModesTable initialization");
        return false;
    }
    db_ = database;

    int rc;

    rc = sqlite3_prepare_v2(
        db_,
        "SELECT label, freq, mode FROM digital_modes WHERE type = :type AND freq > :freq ORDER BY freq ASC LIMIT 1", -1,
        &get_next_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare DigitalModesTable::get_next: %s", sqlite3_errmsg(db_));
        db_ = nullptr;
        return false;
    }
    get_next_type_param_index_ = sqlite3_bind_parameter_index(get_next_stmt_, ":type");
    get_next_freq_param_index_ = sqlite3_bind_parameter_index(get_next_stmt_, ":freq");

    rc = sqlite3_prepare_v2(
        db_, "SELECT label, freq, mode FROM digital_modes WHERE type = :type ORDER BY ABS(freq - :freq) ASC LIMIT 1",
        -1, &get_closest_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare DigitalModesTable::get_closest: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(get_next_stmt_);
        get_next_stmt_             = nullptr;
        get_next_type_param_index_ = 0;
        get_next_freq_param_index_ = 0;
        db_                        = nullptr;
        return false;
    }
    get_closest_type_param_index_ = sqlite3_bind_parameter_index(get_closest_stmt_, ":type");
    get_closest_freq_param_index_ = sqlite3_bind_parameter_index(get_closest_stmt_, ":freq");

    rc = sqlite3_prepare_v2(
        db_,
        "SELECT label, freq, mode FROM digital_modes WHERE type = :type AND freq < :freq ORDER BY freq DESC LIMIT 1",
        -1, &get_prev_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare DigitalModesTable::get_prev: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(get_next_stmt_);
        sqlite3_finalize(get_closest_stmt_);
        get_next_stmt_                = nullptr;
        get_closest_stmt_             = nullptr;
        get_next_type_param_index_    = 0;
        get_next_freq_param_index_    = 0;
        get_closest_type_param_index_ = 0;
        get_closest_freq_param_index_ = 0;
        db_                           = nullptr;
        return false;
    }
    get_prev_type_param_index_ = sqlite3_bind_parameter_index(get_prev_stmt_, ":type");
    get_prev_freq_param_index_ = sqlite3_bind_parameter_index(get_prev_stmt_, ":freq");
    return true;
}

void DigitalModesTable::Shutdown() {
    if (get_next_stmt_) {
        sqlite3_finalize(get_next_stmt_);
        get_next_stmt_ = nullptr;
    }
    if (get_closest_stmt_) {
        sqlite3_finalize(get_closest_stmt_);
        get_closest_stmt_ = nullptr;
    }
    if (get_prev_stmt_) {
        sqlite3_finalize(get_prev_stmt_);
        get_prev_stmt_ = nullptr;
    }
    get_next_type_param_index_    = 0;
    get_next_freq_param_index_    = 0;
    get_closest_type_param_index_ = 0;
    get_closest_freq_param_index_ = 0;
    get_prev_type_param_index_    = 0;
    get_prev_freq_param_index_    = 0;
    db_                           = nullptr;
}

DigitalModesTable::LoadResult DigitalModesTable::find_next(int32_t type, int32_t current_freq) {
    int            rc;
    StmtResetGuard guard(get_next_mutex_, get_next_stmt_);

    rc = sqlite3_bind_int(get_next_stmt_, get_next_type_param_index_, type);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind digital type %i to find_next stmt: %s", type, sqlite3_errmsg(db_));
        return {Record{}, rc};
    }
    rc = sqlite3_bind_int(get_next_stmt_, get_next_freq_param_index_, current_freq);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind digital freq %i to find_next stmt: %s", current_freq, sqlite3_errmsg(db_));
        return {Record{}, rc};
    }
    rc = sqlite3_step(get_next_stmt_);
    if (rc == SQLITE_ROW) {
        Record               record;
        const unsigned char *txt = sqlite3_column_text(get_next_stmt_, 0);
        record.label             = txt ? reinterpret_cast<const char *>(txt) : "";
        record.freq              = sqlite3_column_int(get_next_stmt_, 1);
        record.mode              = sqlite3_column_int(get_next_stmt_, 2);
        // record.label is copied into a std::string: sqlite3_reset (run by the
        // guard on scope exit) invalidates the column text pointer.
        return {record, SUCCESS};
    }
    if (rc == SQLITE_DONE) {
        LV_LOG_WARN("No next digital mode for type=%i, freq=%i", type, current_freq);
        return {Record{}, NOT_FOUND};
    }
    LV_LOG_WARN("find_next failed: %s", sqlite3_errmsg(db_));
    return {Record{}, rc};
}

DigitalModesTable::LoadResult DigitalModesTable::find_closest(int32_t type, int32_t current_freq) {
    int            rc;
    StmtResetGuard guard(get_closest_mutex_, get_closest_stmt_);

    rc = sqlite3_bind_int(get_closest_stmt_, get_closest_type_param_index_, type);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind digital type %i to find_closest stmt: %s", type, sqlite3_errmsg(db_));
        return {Record{}, rc};
    }
    rc = sqlite3_bind_int(get_closest_stmt_, get_closest_freq_param_index_, current_freq);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind digital freq %i to find_closest stmt: %s", current_freq, sqlite3_errmsg(db_));
        return {Record{}, rc};
    }
    rc = sqlite3_step(get_closest_stmt_);
    if (rc == SQLITE_ROW) {
        Record               record;
        const unsigned char *txt = sqlite3_column_text(get_closest_stmt_, 0);
        record.label             = txt ? reinterpret_cast<const char *>(txt) : "";
        record.freq              = sqlite3_column_int(get_closest_stmt_, 1);
        record.mode              = sqlite3_column_int(get_closest_stmt_, 2);
        return {record, SUCCESS};
    }
    if (rc == SQLITE_DONE) {
        LV_LOG_WARN("No closest digital mode for type=%i, freq=%i", type, current_freq);
        return {Record{}, NOT_FOUND};
    }
    LV_LOG_WARN("find_closest failed: %s", sqlite3_errmsg(db_));
    return {Record{}, rc};
}

DigitalModesTable::LoadResult DigitalModesTable::find_prev(int32_t type, int32_t current_freq) {
    int            rc;
    StmtResetGuard guard(get_prev_mutex_, get_prev_stmt_);

    rc = sqlite3_bind_int(get_prev_stmt_, get_prev_type_param_index_, type);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind digital type %i to find_prev stmt: %s", type, sqlite3_errmsg(db_));
        return {Record{}, rc};
    }
    rc = sqlite3_bind_int(get_prev_stmt_, get_prev_freq_param_index_, current_freq);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind digital freq %i to find_prev stmt: %s", current_freq, sqlite3_errmsg(db_));
        return {Record{}, rc};
    }
    rc = sqlite3_step(get_prev_stmt_);
    if (rc == SQLITE_ROW) {
        Record               record;
        const unsigned char *txt = sqlite3_column_text(get_prev_stmt_, 0);
        record.label             = txt ? reinterpret_cast<const char *>(txt) : "";
        record.freq              = sqlite3_column_int(get_prev_stmt_, 1);
        record.mode              = sqlite3_column_int(get_prev_stmt_, 2);
        return {record, SUCCESS};
    }
    if (rc == SQLITE_DONE) {
        LV_LOG_WARN("No prev digital mode for type=%i, freq=%i", type, current_freq);
        return {Record{}, NOT_FOUND};
    }
    LV_LOG_WARN("find_prev failed: %s", sqlite3_errmsg(db_));
    return {Record{}, rc};
}

// ---------------------------------------------------------------------------
// AtuTable
// ---------------------------------------------------------------------------

bool AtuTable::Init(sqlite3 *database) {
    if (db_) {
        LV_LOG_ERROR("Repeated AtuTable initialization");
        return false;
    }
    db_ = database;

    int rc;

    rc = sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO atu(ant, freq, val) VALUES(:ant, :freq, :val)", -1,
                            &save_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare AtuTable::save: %s", sqlite3_errmsg(db_));
        db_ = nullptr;
        return false;
    }
    save_ant_param_index_  = sqlite3_bind_parameter_index(save_stmt_, ":ant");
    save_freq_param_index_ = sqlite3_bind_parameter_index(save_stmt_, ":freq");
    save_val_param_index_  = sqlite3_bind_parameter_index(save_stmt_, ":val");

    rc = sqlite3_prepare_v2(
        db_, "DELETE FROM atu WHERE ant = :ant AND (freq BETWEEN :freq - :step AND :freq + :step) AND (:freq != freq)",
        -1, &delete_adjacent_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare AtuTable::delete_adjacent: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(save_stmt_);
        save_stmt_             = nullptr;
        save_ant_param_index_  = 0;
        save_freq_param_index_ = 0;
        save_val_param_index_  = 0;
        db_                    = nullptr;
        return false;
    }
    delete_adjacent_ant_param_index_  = sqlite3_bind_parameter_index(delete_adjacent_stmt_, ":ant");
    delete_adjacent_freq_param_index_ = sqlite3_bind_parameter_index(delete_adjacent_stmt_, ":freq");
    delete_adjacent_step_param_index_ = sqlite3_bind_parameter_index(delete_adjacent_stmt_, ":step");

    rc =
        sqlite3_prepare_v2(db_, "SELECT freq, val FROM atu WHERE ant = :ant ORDER BY freq ASC", -1, &load_all_stmt_, 0);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed prepare AtuTable::load_all: %s", sqlite3_errmsg(db_));
        sqlite3_finalize(save_stmt_);
        sqlite3_finalize(delete_adjacent_stmt_);
        save_stmt_                        = nullptr;
        delete_adjacent_stmt_             = nullptr;
        save_ant_param_index_             = 0;
        save_freq_param_index_            = 0;
        save_val_param_index_             = 0;
        delete_adjacent_ant_param_index_  = 0;
        delete_adjacent_freq_param_index_ = 0;
        delete_adjacent_step_param_index_ = 0;
        db_                               = nullptr;
        return false;
    }
    load_all_ant_param_index_ = sqlite3_bind_parameter_index(load_all_stmt_, ":ant");
    return true;
}

void AtuTable::Shutdown() {
    if (save_stmt_) {
        sqlite3_finalize(save_stmt_);
        save_stmt_ = nullptr;
    }
    if (delete_adjacent_stmt_) {
        sqlite3_finalize(delete_adjacent_stmt_);
        delete_adjacent_stmt_ = nullptr;
    }
    if (load_all_stmt_) {
        sqlite3_finalize(load_all_stmt_);
        load_all_stmt_ = nullptr;
    }
    save_ant_param_index_             = 0;
    save_freq_param_index_            = 0;
    save_val_param_index_             = 0;
    delete_adjacent_ant_param_index_  = 0;
    delete_adjacent_freq_param_index_ = 0;
    delete_adjacent_step_param_index_ = 0;
    load_all_ant_param_index_         = 0;
    db_                               = nullptr;
}

int AtuTable::Save(int32_t ant, int32_t freq, uint32_t network) {
    int            rc;
    StmtResetGuard guard(save_mutex_, save_stmt_);

    rc = sqlite3_bind_int(save_stmt_, save_ant_param_index_, ant);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind atu ant %i: %s", ant, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_bind_int(save_stmt_, save_freq_param_index_, freq);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind atu freq %i: %s", freq, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_bind_int(save_stmt_, save_val_param_index_, static_cast<int32_t>(network));
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind atu val %u: %s", network, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_step(save_stmt_);
    if (rc != SQLITE_DONE) {
        LV_LOG_ERROR("Failed save atu row for ant %i, freq %i: %s", ant, freq, sqlite3_errmsg(db_));
        return rc;
    }
    return SUCCESS;
}

int AtuTable::DeleteAdjacent(int32_t ant, int32_t freq, int32_t step) {
    int            rc;
    StmtResetGuard guard(delete_adjacent_mutex_, delete_adjacent_stmt_);

    rc = sqlite3_bind_int(delete_adjacent_stmt_, delete_adjacent_ant_param_index_, ant);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind atu delete ant %i: %s", ant, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_bind_int(delete_adjacent_stmt_, delete_adjacent_freq_param_index_, freq);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind atu delete freq %i: %s", freq, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_bind_int(delete_adjacent_stmt_, delete_adjacent_step_param_index_, step);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind atu delete step %i: %s", step, sqlite3_errmsg(db_));
        return rc;
    }
    rc = sqlite3_step(delete_adjacent_stmt_);
    if (rc != SQLITE_DONE) {
        LV_LOG_ERROR("Failed delete adjacent atu rows for ant %i, freq %i: %s", ant, freq, sqlite3_errmsg(db_));
        return rc;
    }
    return SUCCESS;
}

int AtuTable::LoadAll(int32_t ant, std::vector<AtuEntry> &out) {
    int            rc;
    StmtResetGuard guard(load_all_mutex_, load_all_stmt_);

    out.clear();
    rc = sqlite3_bind_int(load_all_stmt_, load_all_ant_param_index_, ant);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Failed to bind atu load ant %i: %s", ant, sqlite3_errmsg(db_));
        return rc;
    }
    while (1) {
        rc = sqlite3_step(load_all_stmt_);
        if (rc == SQLITE_ROW) {
            AtuEntry entry;
            entry.freq    = sqlite3_column_int(load_all_stmt_, 0);
            entry.network = static_cast<uint32_t>(sqlite3_column_int(load_all_stmt_, 1));
            out.push_back(entry);
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            LV_LOG_ERROR("Error while reading atu rows: %s", sqlite3_errmsg(db_));
            return rc;
        }
    }
    return SUCCESS;
}

// ---------------------------------------------------------------------------
// Global database entry points
// ---------------------------------------------------------------------------

// Process-wide settings connection (owned by cfg, never closed until exit).
static sqlite3 *g_db = nullptr;

static void db_log_callback(void * /*pArg*/, int err_code, const char *msg) {
    LV_LOG_ERROR("(%d) %s\n", err_code, msg);
}

extern "C" bool cfg_db_open(const char *path) {
    if (g_db) {
        LV_LOG_ERROR("Repeated cfg_db_open");
        return false;
    }

    sqlite3_config(SQLITE_CONFIG_LOG, db_log_callback, NULL);
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);

    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Can't open params.db: %s", g_db ? sqlite3_errmsg(g_db) : "out of memory");
        if (g_db) {
            sqlite3_close(g_db);
            g_db = nullptr;
        }
        return false;
    }

    // JS8: a card from the JS8 test builds (betas 1-4, on 0.34) is realigned
    // first, or 1.0's settings conversion would be skipped (js8_db.c). Any
    // other card is left as it is.
    if (js8_db_before_migrations(g_db) != 0 || migrations_apply() != 0) {
        LV_LOG_ERROR("Can't apply DB migrations");
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }

    js8_db_after_migrations(g_db); // JS8's frequency lists; logs its own error

    // Some optimizations
    sqlite3_exec(g_db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA cache_size = -4096;", NULL, NULL, NULL);
    return true;
}

extern "C" sqlite3 *cfg_db_get(void) {
    return g_db;
}

extern "C" void cfg_db_init(sqlite3 *database) {
    bool ok;
    ok = KeyValueTable<StorageType::GLOBAL>::Init(database);
    if (!ok)
        exit(1);
    ok = KeyValueTable<StorageType::BAND>::Init(database);
    if (!ok)
        exit(1);
    ok = KeyValueTable<StorageType::MODE>::Init(database);
    if (!ok)
        exit(1);
    ok = KeyValueTable<StorageType::TRANSVERTER>::Init(database);
    if (!ok)
        exit(1);
    ok = BandsTable::Init(database);
    if (!ok)
        exit(1);
    ok = MemoryTable::Init(database);
    if (!ok)
        exit(1);
    ok = DigitalModesTable::Init(database);
    if (!ok)
        exit(1);
    ok = AtuTable::Init(database);
    if (!ok)
        exit(1);
}

void cfg_db_shutdown() {
    KeyValueTable<StorageType::GLOBAL>::Shutdown();
    KeyValueTable<StorageType::BAND>::Shutdown();
    KeyValueTable<StorageType::MODE>::Shutdown();
    KeyValueTable<StorageType::TRANSVERTER>::Shutdown();
    BandsTable::Shutdown();
    MemoryTable::Shutdown();
    DigitalModesTable::Shutdown();
    AtuTable::Shutdown();
}
