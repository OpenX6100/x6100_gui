/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - Olivia macros
 *
 *  Olivia's own macro list, kept in /mnt/olivia_macros.txt, one macro
 *  per line as "name<TAB>text". Olivia sends 7-bit ASCII, so case is
 *  free; every block carries a fixed number of characters whatever they
 *  are.
 *
 *  Storage only - no user interface lives here. The editor is in the
 *  Olivia window, which has to manage the keyboard focus itself.
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

#define OLIVIA_MACRO_MAX       12
#define OLIVIA_MACRO_NAME_LEN  24
#define OLIVIA_MACRO_TEXT_LEN  200

typedef struct {
    char name[OLIVIA_MACRO_NAME_LEN];
    char text[OLIVIA_MACRO_TEXT_LEN];
} olivia_macro_t;

/* Load the list; a missing file is created with a starter set. An
 * existing file is taken as it is, even if empty. */
void               olivia_macros_load(void);
bool               olivia_macros_save(void);

int                olivia_macros_count(void);
const olivia_macro_t *olivia_macro_get(int idx);

/* add returns the new index, or -1 when all slots are taken. Tabs and
 * line breaks are turned into spaces, since they would break the file
 * format. None of these save; call olivia_macros_save(). */
int                olivia_macro_add(const char *name, const char *text);
void               olivia_macro_set(int idx, const char *name, const char *text);
void               olivia_macro_delete(int idx);

/*
 * Replace <MYCALL>, <CALL>, <RST> (sent) and <RSTR> (received).
 *
 * Returns false and leaves *missing pointing at the token's name when
 * the text needs a value that is empty - in practice <CALL> before any
 * callsign has been caught or typed. Sending "UR RST 599" to nobody is
 * worse than not sending at all.
 */
bool               olivia_macro_expand(const char *in, char *out, size_t out_sz,
                                    const char **missing);

#ifdef __cplusplus
}
#endif
