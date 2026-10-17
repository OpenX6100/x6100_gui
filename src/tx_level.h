/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - the drive level of a digital-mode transmission
 *
 *  tx_player.c plays the audio in blocks of 2048 samples (about 43 ms at
 *  48 kHz) and reads the radio's ALC and power after each. The old loop
 *  turned the gain after every block from that one reading: up to +0.6 dB
 *  whenever the power read short, down when the ALC rose. The radio reads
 *  power in 0.1 W steps, so at 0.3 W a true 0.28 W reads 0.2 W "short",
 *  and the drive swung by several dB a second (VE7NHW, 2026-10-04: an
 *  XPA125B behind it went 18-35 W and back every two seconds).
 *  A reading doesn't come after every block: on VE7NHW's radio 6-7 came
 *  in each half second, and IU1WXY reports fewer on his.
 *
 *  Now, with the readings of each half second of blocks averaged: while the ALC reads
 *  zero the drive ramps up, one way only (1 dB a second while the power is
 *  under half the setting, then 0.5 dB a second), and stops for the rest
 *  of the transmission once the ALC shows (it then holds the power at the
 *  setting, whatever the 0.1 W reading says) or the power reads the
 *  setting. Overdrive (ALC over 0.5) brings it down a little. The level
 *  found is kept (tx_player.c), so the next transmission starts there and
 *  stays steady. Pure arithmetic: tests/test_tx_level.cpp runs it
 *  against a model radio.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TX_LEVEL_SETTLE_BLOCKS 30    /* about 1.3 s: the PA and the ALC settle first */
#define TX_LEVEL_WINDOW_BLOCKS 12    /* about 0.5 s of blocks per decision */
#define TX_LEVEL_ALC_HIGH      0.5f  /* the radio's ALC (0-10) above this: overdriven */
#define TX_LEVEL_ALC_SHOWS     0.25f /* from 0.3 (read in 0.1 steps) the ALC is working: hold there.
                                      * Not 0.1: a few stray 0.1 readings in a window held the
                                      * drive short of the setting */
#define TX_LEVEL_DOWN_MAX_DB   0.75f /* the most the drive drops in one window */
#define TX_LEVEL_UP_FAST_DB    0.5f  /* up a window while far short (1 dB a second) */
#define TX_LEVEL_UP_SLOW_DB    0.25f /* up a window when near (0.5 dB a second) */
#define TX_LEVEL_UP_MAX_DB     8.0f  /* the most it ramps up in one transmission */
#define TX_LEVEL_PWR_STEP_W    0.1f  /* the radio's power reading resolution */

typedef struct {
    float target_w; /* the power asked for */
    int   blocks;   /* blocks played so far */
    int   n;        /* readings in the current window */
    int   wb;       /* blocks in the current window */
    float sum_pwr;
    float sum_alc;
    bool  held;     /* the level was found (or had to come down): no more ramping up */
    float up_db;    /* ramped up so far this transmission */
} tx_level_t;

void tx_level_start(tx_level_t *l, float target_w);

/* One block played; `have` a new reading (power in W, ALC 0-10). Returns
 * the gain change to make now, in dB. */
float tx_level_block(tx_level_t *l, bool have, float pwr_w, float alc);

#ifdef __cplusplus
}
#endif
