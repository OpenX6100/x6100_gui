/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8: keep the decoder threads off one core.
 */

#pragma once

namespace x6100::js8 {

/// Called on a decoder thread (js8core's decode threads, the time search):
/// keeps it off the last CPU core, so the GUI thread, which draws the
/// waterfall, always has a core no decoder can take (the GUI itself may
/// still run anywhere). The radio has four cores; core 0 takes the
/// interrupts, so the free one is the last. `nice` > 0 also lowers the
/// thread's priority. Lasts as long as the thread (JS8 closing ends it).
/// Returns the core left free, or -1 (fewer than 3 cores, or not Linux).
int fence_decoder_thread(int nice);

} // namespace x6100::js8
