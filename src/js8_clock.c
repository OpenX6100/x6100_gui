/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8: the system clock's sync, and the battery clock
 */

#include "js8_clock.h"

#include <stdlib.h>
#include <sys/timex.h>
#include <unistd.h>

bool js8_clock_synced(void) {
    struct timex tx = {0}; /* modes 0: only read */
    int          state = adjtimex(&tx);
    return state != -1 && state != TIME_ERROR && !(tx.status & STA_UNSYNC);
}

bool js8_clock_save_rtc(void) {
    if (access("/dev/rtc1", W_OK) != 0) return false;
    return system("hwclock -w -u -f /dev/rtc1") == 0;
}
