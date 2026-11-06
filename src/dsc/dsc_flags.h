/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - country flags for the DSC detail card (see make_flags_db.py)
 *
 * The file is memory mapped read-only; the images point into it, so they
 * cost no RAM and no decoding.
 */
#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maps the flags file; returns the number of flags (0 = none). */
int  dsc_flags_open(void);
void dsc_flags_close(void);

/* Flag of an ISO 3166 alpha-2 code ("it"), NULL if there is none. */
const lv_img_dsc_t *dsc_flag(const char *iso);

#ifdef __cplusplus
}
#endif
