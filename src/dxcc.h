/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - country (DXCC entity) of a callsign
 *
 *  From AD1C's cty.dat (country-files.com), the file WSJT-X, JTDX and the
 *  contest loggers use. /mnt/cty.dat, when present, is used instead of the
 *  one in the image (/usr/share/x6100/cty.dat), so a newer list can be put
 *  on the SD card without a new image.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Country of `call`: the entity name, shortened for display where
 * cty.dat's is long ("Germany" for "Fed. Rep. of Germany"), and its
 * primary prefix ("DL"). Either pointer may be NULL. False when the call
 * is unknown, maritime or aeronautical mobile, or no cty.dat was found.
 * The strings stay valid for the life of the program.
 *
 * Thread safe. The file is read on the first call (about 0.1 s), so call
 * it from a worker thread, not from the LVGL thread. */
bool dxcc_lookup(const char *call, const char **name, const char **prefix);

#ifdef __cplusplus
}
#endif
