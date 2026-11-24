/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - RTTY tuning: snap and AFC
 *
 *  Finds an RTTY signal near the receiver's centre, so it need not be
 *  set by hand. The audio is kept as an averaged power spectrum (2048
 *  point FFT at the decoder's 16 kHz, 7.8 Hz bins, about 2 s memory);
 *  a signal is a pair of peaks one shift apart, mark and space, both
 *  well above the noise of the search band. Its centre is the middle of
 *  the two peaks, each placed to a fraction of a bin.
 *
 *  Snap (hold Center, or Menu) looks 300 Hz either side of the centre
 *  and moves to the strongest pair. AFC (Menu, off by default) keeps a
 *  decoded signal centred: about once a second it looks 50 Hz either
 *  side and follows a drift of more than 4 Hz, only while characters
 *  are actually being decoded (most frames with a good stop bit) and not
 *  while transmitting. Both move the RTTY centre, which the
 *  transmitter uses too, so the reply goes out where the station is.
 *
 *  Everything here runs on the decoder (DSP) thread, under rtty.c's lock.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTTY_AFC_SNAP_HZ  300.0f
#define RTTY_AFC_TRACK_HZ  50.0f

/* (Re)start with the sample rate; forgets the averaged spectrum. */
bool rtty_afc_init(float sample_rate);
void rtty_afc_free(void);
void rtty_afc_reset(void);

/* Real audio samples, as the decoder gets them. */
void rtty_afc_feed(const float *samples, size_t n);

/* Seconds of audio averaged since the last reset. */
float rtty_afc_seconds(void);

/* The centre of the strongest mark/space pair `shift` apart within
 * `range` of `center` (audio Hz), kept inside lo..hi. Both tones and the
 * noise they are measured against are taken inside the receive filter,
 * pass_lo..pass_hi: outside it the bins are the filter's skirt, quieter
 * than the band, and ordinary noise inside would stand out of them.
 * False when no pair stands out of the noise. */
bool rtty_afc_find(float center, float shift, float range, float lo, float hi,
                   float pass_lo, float pass_hi, float *found);

#ifdef __cplusplus
}
#endif
