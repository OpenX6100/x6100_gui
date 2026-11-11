/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - every FT8 and FT4 frequency, for the FT8 window's
 *  list
 *
 *  The band buttons step through one frequency per band (the digital_modes
 *  table). WSJT-X knows a few more: where DXpeditions run Fox and Hound, and
 *  FT8's intercontinental one on 6 m. This is WSJT-X 3.3's list
 *  (models/FrequencyList.cpp) for the bands this radio has, standard and
 *  other alike, so the list can also go back to the standard one.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t    hz;         /* dial, USB */
    const char *what;       /* NULL for the band's standard frequency, else
                               "DXpedition", "intercontinental", ... */
} ftx_freq_t;

/* The frequencies of `protocol` (FTX_PROTOCOL_FT8 or FTX_PROTOCOL_FT4) for
 * ITU `region` (1..3), low to high, at most `max`; returns how many. 0 for
 * FT2: Decodium and WSJT-X Improved have one per band, which the band
 * buttons already step through. */
int ftx_freqs(int protocol, int region, ftx_freq_t *out, int max);

/* The one closest to `hz` among `n` of them (0 when n is 0). */
int ftx_freqs_closest(const ftx_freq_t *list, int n, uint32_t hz);

#ifdef __cplusplus
}
#endif
