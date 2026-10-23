/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 decoder setup (FFTW wisdom), C interface
 *
 *  The decoder's FFT plans are measured once ever and kept in a file on the
 *  DATA partition, as desktop JS8Call keeps js8call_wisdom.dat. Without the
 *  file (first open of a new build, or deleted) every speed's plans are
 *  measured up front by a thread of its own, once: about 17 s on the radio
 *  (estimated from PC timings: the four main speeds took 12-13 s there).
 *  See wisdom.hpp.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On each open, before js8_rx_create(). True: start the decoder now.
 * False: the one-time setup is running (started now, or still going from
 * an earlier open); start the decoder once js8_decoder_setup_running()
 * says it's done. A missing or damaged file only means measuring again. */
bool js8_decoder_setup_begin(const char *path);

/* The setup is still running; `speeds_done` (may be NULL) gets how many of
 * js8_decoder_setup_speeds() are measured so far. Any thread. */
bool     js8_decoder_setup_running(unsigned *speeds_done);
unsigned js8_decoder_setup_speeds(void);

/* How long the last setup took, s (0: none ran this power-on). */
double js8_decoder_setup_seconds(void);

/* After js8_rx_destroy(): save the plans to `path` (.tmp + rename) if they
 * changed since the file was loaded or saved. Nothing while the setup runs
 * (it saves the file itself when done, JS8 open or not). */
void js8_decoder_setup_save(const char *path);

#ifdef __cplusplus
}
#endif
