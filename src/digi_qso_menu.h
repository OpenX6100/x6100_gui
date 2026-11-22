/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the QSO menu of the keyboard modes
 *
 *  One list, the same in PSK31, Olivia and RTTY, opened from each
 *  window's Menu ("QSO: log, name, QTH..."):
 *
 *    Log QSO now (DX call, reports, name, QTH -> digi_log.h)
 *    DX name..., DX QTH...           for <NAME>, <QTH> and the log; taken
 *                                    from the text by themselves, else
 *                                    a word received lately, or typed
 *    RST received (press: next)      for <RSTR> and the log
 *    Serial <CNTR>..., Serial received...   contest serial numbers
 *    My name..., My QTH...           for <MYNAME>, <MYQTH>; kept
 *    Call, name, QTH from the text: on/off (digi_qso_feed_rx)
 *    Log when 73 goes out: on/off    kept
 *    Save the text to the card: on/off (digi_text.h); kept
 *
 *  The window describes itself with a digi_qso_menu_t: where the list
 *  opens, how its keyboard focus is handed to a text window and back,
 *  what to repaint, and the mode and frequency for the log.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "digi_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t      *(*parent)(void);     /* where the list opens */
    bool           (*input_begin)(void); /* false when a text window is open */
    void           (*input_end)(void);
    void           (*changed)(void);    /* repaint the window's buttons, may be NULL */
    digi_log_mode_t  mode;
    const char    *(*submode)(void);    /* Olivia's "OLIVIA 32/1000", may be NULL */
    uint64_t       (*freq)(void);       /* the signal's radio frequency, Hz */
    bool           (*tx_on)(void);      /* transmitting; NULL: log a 73 at once */
} digi_qso_menu_t;

/* `m` must stay valid while the window is open (a static). */
void digi_qso_menu_open(const digi_qso_menu_t *m);

/* After a macro was queued, from Next or from the list: a 73 to a DX
 * call logs the QSO, once, when "Log when 73 goes out" is on - when the
 * transmission ends, not when the 73 is queued, so a 73 that never went
 * out is not logged. The QSO is taken as it is now: a CQ queued after the
 * 73 (which clears the DX call) does not change what is logged. */
void digi_qso_menu_sent(const digi_qso_menu_t *m, const char *macro_name);

/* The operator stopped the transmission: a QSO waiting for its 73 to go
 * out is not logged (a message says so). */
void digi_qso_menu_tx_stopped(void);

/* Log QSO now, for a window that wants it on a button of its own. */
void digi_qso_menu_log(const digi_qso_menu_t *m);

/* The window is closing: a text window this menu opened is dropped, and
 * a QSO waiting for its 73 is not logged, as on a stop. */
void digi_qso_menu_close(void);

#ifdef __cplusplus
}
#endif
