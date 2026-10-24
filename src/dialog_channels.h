/*
 * Copyright (C) 2026 Hany El Imam
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dialog.h"

extern dialog_t *dialog_channels;

/*
 * Open the CHANNELS window, or close it if it is already open.
 *
 * other_dialog_running: true when another dialog (WEFAX, SSTV, FT8, ...)
 * is open.  CHANNELS then opens as an overlay that leaves that dialog
 * untouched; otherwise it opens as a normal dialog.
 */
void dialog_channels_toggle(lv_obj_t *parent, bool other_dialog_running);

/* True while the CHANNELS window is open (either mode). */
bool dialog_channels_is_open(void);

/*
 * True while the open window has unsaved changes in progress (Drag or
 * Edit): the channel database must not be modified from outside.
 */
bool dialog_channels_busy(void);

/*
 * A channel was appended to the database from outside the window (NW key):
 * refresh the open window and select the new channel.
 */
void dialog_channels_show_added(void);

/*
 * Recall the previous/next channel, with wrap-around.
 * direction < 0: previous; direction > 0: next.
 */
bool dialog_channels_recall_relative(int8_t direction);
/* true when there are no channels (mic UP/DOWN then tunes a step) */
bool dialog_channels_empty(void);

/*
 * Name of the last recalled channel ("CHANNEL n" if it has no name).
 * Returns an empty string if no channel has been recalled.
 */
const char *dialog_channels_last_recalled_name(void);

/*
 * Request that the next opening of CHANNELS immediately adds the current
 * radio settings as a new channel.
 */
