/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#ifndef SSTV_AUTODETECT_H
#define SSTV_AUTODETECT_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
    SSTV_AUTODETECT_NONE = 0,
    SSTV_AUTODETECT_ROBOT36,
    SSTV_AUTODETECT_MARTIN_M1,
    SSTV_AUTODETECT_MARTIN_M2,
    SSTV_AUTODETECT_SCOTTIE_S1,
    SSTV_AUTODETECT_SCOTTIE_S2,
    SSTV_AUTODETECT_SCOTTIE_DX
} sstv_autodetect_mode_t;

typedef enum {
    SSTV_AUTODETECT_IDLE = 0,
    SSTV_AUTODETECT_LISTENING,
    SSTV_AUTODETECT_DETECTED,
    SSTV_AUTODETECT_TIMEOUT
} sstv_autodetect_state_t;

void sstv_autodetect_start(void);
void sstv_autodetect_stop(void);
void sstv_autodetect_process(const float *samples, size_t count);

sstv_autodetect_state_t sstv_autodetect_state(void);
sstv_autodetect_mode_t sstv_autodetect_result(void);
bool sstv_autodetect_is_listening(void);
const char *sstv_autodetect_mode_name(sstv_autodetect_mode_t mode);

#endif
