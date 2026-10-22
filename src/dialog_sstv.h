/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_sstv;

/* Closes the target-callsign virtual keyboard, if open (see dialog_sstv_tx.inc). */
void dialog_sstv_keyboard_dismiss(void);

#ifdef __cplusplus
}
#endif
