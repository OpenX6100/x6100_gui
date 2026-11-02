/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_wefax;
bool dialog_wefax_is_active(void);

/* Microphone UP (dir < 0) / DOWN (dir > 0) while WEFAX is open: previous /
 * next station of the stations list.  false: not handled (WEFAX closed). */
bool dialog_wefax_mic_step(int dir);

/* Microphone V/M while WEFAX is open: open / close the stations list.
 * Returns false (key not used) when WEFAX is not open. */
bool dialog_wefax_vm_toggle(void);

#ifdef __cplusplus
}
#endif
