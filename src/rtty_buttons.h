/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - RTTY button page 2
 *
 *  Page 1 (Rate, Shift, Center, Reverse) stays in buttons.cpp. Page 2
 *  is chained after it through buttons_group_rtty:
 *
 *    (RTTY 2:2) | Next | SQL | Macros | Menu
 *
 *  Next sends the QSO's next step (CQ, Answer, Report, TNX, 73, picked
 *  by macro name) and stops a running transmission. SQL shows the
 *  threshold and the live level; press hands it the MFK knob. Macros
 *  and Menu open a list over the screen: MFK to choose, MFK press to
 *  pick, VOL press to close. Free text, new and edited macros are in
 *  Macros; the DX call, both reports and Auto DX in Menu.
 */

#pragma once

#include "buttons.h"

#ifdef __cplusplus
extern "C" {
#endif

extern buttons_page_t buttons_page_rtty_2;

/* Repaint Next after the macros or the QSO changed. */
void rtty_refresh_macro_labels(void);

/* Next follows the transmitter and the DX call, and SQL the level, so
 * they are repainted on a timer while the RTTY application is open; stop also closes a list
 * left open. */
void rtty_sql_timer_start(void);
void rtty_sql_timer_stop(void);

#ifdef __cplusplus
}
#endif
