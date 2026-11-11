/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdint.h>
#include <time.h>

/**
 * Return monotonic time in ms.
 */
static inline uint64_t get_time(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (uint64_t) now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

/**
 * Return monotonic time in us.
 */
static inline uint64_t get_time_us(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (uint64_t) now.tv_sec * 1000000L + now.tv_nsec / 1000L;
}
