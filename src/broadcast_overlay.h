/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 */

#pragma once

/* Show the stations scheduled now on the current frequency (from
 * broadcast.csv); while the pop-up is up, the next match. */
void broadcast_overlay_show(void);

/* Save the current frequency as a channel, named after the station shown
 * (or the first one scheduled now). */
void broadcast_overlay_save_channel(void);
