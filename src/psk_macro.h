/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - PSK31 macros
 *
 *  PSK31's own macro list, kept in /mnt/psk_macros.txt, one macro per
 *  line as "name<TAB>text". Lower case is used freely: on PSK31 it is
 *  about twice as fast to send as capitals.
 *
 *  Storage only - no user interface lives here. The editor is in the
 *  PSK31 window, which has to manage the keyboard focus itself.
 *
 *  The QSO values the tokens expand to (<CALL>, <RST>, <RSTR>) come from
 *  digi_qso.c, shared with any other keyboard mode window.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSK_MACRO_MAX       12
#define PSK_MACRO_NAME_LEN  24
#define PSK_MACRO_TEXT_LEN  200

typedef struct {
    char name[PSK_MACRO_NAME_LEN];
    char text[PSK_MACRO_TEXT_LEN];
} psk_macro_t;

/* Load the list; a missing file is created with a starter set. An
 * existing file is taken as it is, even if empty. */
void               psk_macros_load(void);
bool               psk_macros_save(void);

int                psk_macros_count(void);
const psk_macro_t *psk_macro_get(int idx);

/* add returns the new index, or -1 when all slots are taken. Tabs and
 * line breaks are turned into spaces, since they would break the file
 * format. None of these save; call psk_macros_save(). */
int                psk_macro_add(const char *name, const char *text);
void               psk_macro_set(int idx, const char *name, const char *text);
void               psk_macro_delete(int idx);

/*
 * Replace <MYCALL>, <CALL>, <RST> (sent) and <RSTR> (received).
 *
 * Returns false and leaves *missing pointing at the token's name when
 * the text needs a value that is empty - in practice <CALL> before any
 * callsign has been caught or typed. Sending "UR RST 599" to nobody is
 * worse than not sending at all.
 */
bool               psk_macro_expand(const char *in, char *out, size_t out_sz,
                                    const char **missing);

#ifdef __cplusplus
}
#endif
