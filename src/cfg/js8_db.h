/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8's part of params.db
 */

#pragma once

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Before the migrations: a card from the JS8 test builds (betas 1-4,
 * version 4 or 5, which only added JS8's frequency lists) goes back to
 * version 3, so 1.0's migration 4 converts its scaled settings (pwr 4 =
 * 0.4 W). Done once per card: the table js8_db marks it. 0 on success. */
int js8_db_before_migrations(sqlite3 *db);

/* After them: JS8's and GhostNet's frequency lists, added where missing
 * (every start; INSERT OR IGNORE). 0 on success. */
int js8_db_after_migrations(sqlite3 *db);

#ifdef __cplusplus
}
#endif
