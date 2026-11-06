/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <liquid/liquid.h>

#include "helpers.h"
#include "audio.h"

#define RTTY_CAPTURE_RATE (AUDIO_CAPTURE_RATE / 3)

typedef enum {
    RTTY_OFF = 0,
    RTTY_RX,
    RTTY_TX
} rtty_state_t;

void rtty_init();
void rtty_put_audio_samples(size_t n, float *samples);

void rtty_set_state(rtty_state_t state);
rtty_state_t rtty_get_state();

float rtty_change_rate(int16_t df);
uint16_t rtty_change_shift(int16_t df);
uint16_t rtty_change_center(int16_t df);
bool rtty_change_reverse(int16_t df);

/* Find the signal within 300 Hz of the centre and centre on it, from
 * the next 1.5 s of audio (rtty_afc.h). The result is on the message
 * line. */
void rtty_snap(void);
