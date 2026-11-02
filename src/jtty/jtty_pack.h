/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY transmit side of the source grammar:
 *  ordinary text packed into the fewest atoms
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jtty_proto.h"
#include "jtty_source.h"

#define JTTY_MAX_FRAMES 16

/*
 * The literal source interface: uppercase, unsupported characters become
 * '#', leading, trailing and repeated spaces go, at most 80 characters.
 */
void jtty_pack_normalize(const char *in, char *out, size_t size);

/*
 * Packs ordinary text (Unknown exchange profile) into payloads, the last
 * one with EOM: calls and call actions, CONTROL phrases, generic numbers
 * and locations, grids and Field Day class/section where they fit at
 * token boundaries, TEXT5 for the rest, with the fewest frames. `norm`
 * (may be NULL) receives the normalized text the receiver will show.
 * Returns the number of frames, 0 for empty text.
 */
int jtty_pack_text(const char *text, jtty_payload_t out[JTTY_MAX_FRAMES],
                   char *norm, size_t norm_size);

/* ---- Native function keys (WSJT-X's shipped F1-F8) ---------------------- */

#define JTTY_MACROS         8
#define JTTY_SERIAL_MAX     131071

/* The template of F<key>, key 1..8: "CQ %M CQ", "%H %E", ... */
const char *jtty_macro_template(int key);

/*
 * Compiles F<key> to its native atoms: %M my call, %H (and %Q) his call,
 * %E the serial exchange "599 nnn". Returns the number of frames, 0 when
 * a call it needs is missing or not a standard call, or the serial is out
 * of range: a native template is never sent as different text. `norm`
 * (may be NULL) receives the text as the receivers will show it.
 */
int jtty_pack_macro(int key, const char *my_call, const char *his_call, uint32_t serial,
                    jtty_payload_t out[JTTY_MAX_FRAMES], char *norm, size_t norm_size);
