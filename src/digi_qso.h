/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - QSO state for the keyboard digital modes
 *
 *  The correspondent's callsign, name and QTH and the RST reports, which
 *  the macro tokens expand to and Log QSO writes. Kept outside any one
 *  mode, so a callsign picked up in one window is still there in another.
 *
 *  The callsign can be typed in or picked up automatically from the
 *  received text ("... DE <call>").
 *
 *  Macro tokens, the same in every keyboard mode:
 *
 *    <CALL>   the DX call              <MYCALL>  our call (Callsign app)
 *    <RST>    RST we send              <RSTR>    RST we received
 *    <NAME>   the DX operator's name   <MYNAME>  our name
 *    <QTH>    the DX station's QTH     <MYQTH>   our town
 *                                      <MYLOC>   our locator (QTH Grid app)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *digi_qso_dx(void);
/* Typed by the operator. A different call starts a new QSO: name, QTH,
 * reports (599) and the start time are reset. So does the same call
 * again once its QSO is logged; before that it changes nothing. (Taken
 * from the text, the same call starts a new QSO only 10 minutes after
 * the log, not on the station's own "73 DE CALL".) */
void        digi_qso_set_dx(const char *call);

/* Forget the callsign, name and QTH and put both reports back to 599. */
void        digi_qso_clear(void);

/* The DX operator's name and QTH, typed in from the QSO menu. */
const char *digi_qso_name(void);
void        digi_qso_set_name(const char *name);
const char *digi_qso_qth(void);
void        digi_qso_set_qth(const char *qth);

/* When the current DX call was set: the QSO's start for the log. */
time_t      digi_qso_start(void);

/* Whether this QSO has been logged, so that sending 73 again does not
 * log it twice. Cleared with a new DX call. */
bool        digi_qso_logged(void);
void        digi_qso_set_logged(bool logged);

/* RST as a number for the log: "579" -> 579, 599 when unreadable. */
int         digi_qso_rst_num(const char *rst);

/*
 * Expands the tokens listed above. Returns false if a token used in `in`
 * has no value yet (`missing` then points to the first such token) -
 * a macro is not sent half filled in. `out` holds the expansion either
 * way, empty tokens left out.
 */
bool        digi_qso_expand(const char *in, char *out, size_t out_sz, const char **missing);

/* What the menu says to do about a missing token, e.g. "No DX call". */
const char *digi_qso_missing_hint(const char *token);

/* RST: readability and tone are fixed at 5 and 9, as usual on the
 * keyboard modes; only the strength digit changes. df > 0 steps up,
 * df < 0 down (wrapping 9 <-> 1), df == 0 only reads. Returns the
 * strength digit. */
uint8_t     digi_qso_change_rst_sent(int16_t df);
uint8_t     digi_qso_change_rst_rcvd(int16_t df);
const char *digi_qso_rst_sent(void);
const char *digi_qso_rst_rcvd(void);

/* Fed with the decoded text, one character at a time, on the UI thread.
 * When auto pick-up is on it takes the callsign after "DE" (only while
 * the DX field is empty or its QSO logged), the name after "NAME (IS/HR)"
 * or "OP" and the QTH after "QTH (IS/HR)", so they need not be typed.
 * Name and QTH are only filled in when empty: once per QSO, and never
 * over what was typed. */
void        digi_qso_feed_rx(char c);

/* UI thread: reread our own call (the Callsign app), which the pick-up
 * never takes as the DX call, e.g. when our CQ is heard back. The
 * windows call it when they open; macro expansion does too. */
void        digi_qso_refresh_mycall(void);

/* Words received lately that could be a name or a place, newest first,
 * in "John" form: the QSO menu offers them to pick when the pick-up
 * missed. Returns how many were copied (at most max). */
int         digi_qso_recent_words(char (*out)[16], int max);
bool        digi_qso_autograb(void);
bool        digi_qso_toggle_autograb(void);

#ifdef __cplusplus
}
#endif
