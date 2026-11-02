/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 waterfall on the display's lower plane
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The JS8 waterfall drawn straight into R1CBU 1.0's primary plane, as the
 * main screen's waterfall is, under the see-through plane LVGL draws apps on.
 * `box` becomes a hole in that plane (its pixels fully transparent, its
 * children drawn over them), and the display hardware puts what's above it
 * (the message list, TX bar, finder) on top. A new row then costs one copy
 * into the plane: LVGL redraws nothing, where before every row redrew the
 * whole see-through list over it.
 *
 * GUI thread only. Rows are as lv_waterfall's: values min..max map onto the
 * palette's 256 colours, the newest row on top. */
bool     js8_wf_create(lv_obj_t *box, const lv_color_t *palette, int min, int max);
/* Blacks out its area on the plane; the main screen's spectrum and waterfall
 * paint theirs again when they come back on. */
void     js8_wf_destroy(void);
void     js8_wf_add_row(const float *data, uint16_t cnt);
/* x across, y in rows below the newest; both copies of each ring line. */
void     js8_wf_fill_rect(int x1, int y1, int x2, int y2, lv_color_t color);
void     js8_wf_clear(void);
/* Rows added so far (decode marks recolour in place by it). */
uint32_t js8_wf_rows(void);
/* Puts what changed on the plane: call often (each WF_TICK_MS) and right
 * after a row. Also repaints now and then while nothing changes, as the main
 * screen's spectrum redraws its part of the plane when the frequency moves. */
void     js8_wf_tick(void);
/* Put it on the plane again over the next few frames: the main screen's
 * spectrum and waterfall (off while JS8 is open) still redraw their part of
 * the plane, over this, in the frame their frequency, mode or zoom changes.
 * Any thread. */
void     js8_wf_repaint_soon(void);

#ifdef __cplusplus
}
#endif
