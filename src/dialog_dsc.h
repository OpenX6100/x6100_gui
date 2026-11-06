/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - DSC (Digital Selective Calling) receiver
 */
#pragma once

#include <stdbool.h>

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_dsc;
bool dialog_dsc_is_active(void);

/* Microphone UP (dir < 0) / DOWN (dir > 0) while DSC is open: previous /
 * next channel of the frequency list.  false: not handled (DSC closed). */
bool dialog_dsc_mic_step(int dir);

/* Microphone V/M while DSC is open: open / close the frequency list.
 * Returns false (key not used) when DSC is not open. */
bool dialog_dsc_vm_toggle(void);

#ifdef __cplusplus
}
#endif
