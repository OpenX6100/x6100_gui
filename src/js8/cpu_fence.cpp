/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8: keep the decoder threads off one core.
 */

#include "cpu_fence.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace x6100::js8 {

int fence_decoder_thread(int nice) {
#if defined(__linux__)
    // Per-thread on Linux: setpriority() on a thread id sets that thread's nice.
    if (nice > 0) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 3 || n > CPU_SETSIZE) return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (long c = 0; c < n - 1; c++) CPU_SET(c, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) return -1;
    return (int)(n - 1);
#else
    (void)nice;
    return -1;
#endif
}

} // namespace x6100::js8
