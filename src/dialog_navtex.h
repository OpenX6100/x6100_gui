/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
#pragma once

#include <stdbool.h>

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_navtex;
bool dialog_navtex_is_active(void);

/* Microphone while NAVTEX is open: V/M opens / closes the frequency list,
 * UP (dir < 0) / DOWN (dir > 0) step through it.  false when not open. */
bool dialog_navtex_vm_toggle(void);
bool dialog_navtex_mic_step(int dir);

#ifdef __cplusplus
}
#endif
