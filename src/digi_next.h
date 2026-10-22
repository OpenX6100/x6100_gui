/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the Next button of the keyboard modes
 *
 *  Which of the window's macros a QSO needs now, found by the names the
 *  starter macros have (CQ, Answer, Report, TNX, 73; RTTY has no Answer
 *  and TNX and skips them):
 *
 *    no DX call ................................... CQ
 *    after our CQ, with a DX call ................. Report
 *    a DX call we have sent nothing to ............ Answer (else Report)
 *    after Answer ................................. TNX (else Report)
 *    after Report or TNX .......................... 73
 *    after 73 ..................................... CQ
 *
 *  A new DX call starts over (except after our own CQ: it answered us).
 *  Macros with other names may be sent at any time; they do not move the
 *  sequence. Names are compared without regard to case.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char last[24];      /* the sequence's last macro sent, "" for none */
    char dx[16];        /* the DX call it went to */
} digi_next_t;

/* Name of macro `idx`, NULL past the end. */
typedef const char *(*digi_next_name_fn)(int idx);

/* The macro Next sends now, -1 if the window has none that fits. */
int  digi_next_pick(const digi_next_t *s, const char *dx, int count, digi_next_name_fn name);

/* After any macro went out, from Next or from the list. */
void digi_next_sent(digi_next_t *s, const char *name, const char *dx);

/* For the button: "CQ", "Report\nSP3WAZ", "Next\n(no macro)". */
void digi_next_label(const digi_next_t *s, const char *dx, int count, digi_next_name_fn name,
                     char *out, size_t size);

bool digi_next_is_cq(const char *name);
