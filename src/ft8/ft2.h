/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT2
 *
 *  FT2 (IU8LMC, 2026) is FT4 at twice the speed: the same 77-bit payload,
 *  LDPC(174,91), Costas sync, scrambling and 4 tones, but 41.67 Bd (288
 *  samples at 12 kHz), 103 symbols in 2.47 s, 167 Hz wide and a 3.75 s
 *  T/R period (16 slots a minute). ft8_lib decodes it as FT4 on a
 *  waterfall built with the shorter symbol: the number of symbols per
 *  slot is the same as FT4 (156), so its candidate search fits as is.
 *
 *  ft8_lib knows FT4 (0) and FT8 (1) only; FT2 is a value of the GUI's
 *  own (cfg.ft8.protocol) and is mapped to FT4 for the library.
 */

#pragma once

#include <ft8lib/constants.h>

#define FTX_PROTOCOL_FT2   ((ftx_protocol_t)2)
#define FT2_SYMBOL_PERIOD  (0.024f)
#define FT2_SLOT_TIME      (3.75f)

static inline ftx_protocol_t ftx_lib_protocol(int proto) {
    return (proto == (int)FTX_PROTOCOL_FT2) ? FTX_PROTOCOL_FT4 : (ftx_protocol_t)proto;
}

static inline float ftx_slot_time(int proto) {
    if (proto == (int)FTX_PROTOCOL_FT2) return FT2_SLOT_TIME;
    if (proto == (int)FTX_PROTOCOL_FT4) return FT4_SLOT_TIME;
    return FT8_SLOT_TIME;
}
