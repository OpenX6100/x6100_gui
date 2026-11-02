/*
 * Xiegu X6100 - "Info" window of the WEFAX, NAVTEX, DSC and SSTV applications
 *
 * Copyright (C) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * A short description of the application, then the credits and the legal
 * notices, in a scrollable window over the application (the same frame as
 * the BIG view).  Reception goes on underneath.
 *
 *   MFK turn: scroll (2 lines)   MFK press / Close Info: close   VOL: volume
 */
#pragma once

#include "lvgl/lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_INFO_WEFAX = 0,
    APP_INFO_NAVTEX,
    APP_INFO_DSC,
    APP_INFO_SSTV,
} app_info_id_t;

/* Opens the window over dialog_obj (the dialog.obj of the application).
 * The current soft-key page and focus are given back on close. */
void app_info_open(lv_obj_t *dialog_obj, app_info_id_t id);

/* Closes the window and gives back the soft keys and the focus. */
void app_info_close(void);

/* Deletes the window without touching soft keys or focus: from destruct_cb. */
void app_info_abort(void);

bool app_info_is_open(void);

#ifdef __cplusplus
}
#endif
