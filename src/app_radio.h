/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * The radio set up for a receiving app (WEFAX, NAVTEX, DSC) while it is
 * open:
 *
 *   app_radio_begin()  when the app opens
 *   app_radio_end()    when it closes
 *
 * While the app is open:
 *   - mode USB-D, locked (the mode lock); a band change that brings
 *     another mode is set back to USB-D;
 *   - spectrum zoom 4x; a band, mode or frequency change that brings
 *     another zoom is set back to 4x.  The 4x is never saved: the zoom
 *     stored for each mode group stays the user's own;
 *   - noise reduction, noise blanker and notches off on the radio
 *     (radio_set_rx_dsp_off(), not saved); their buttons still change
 *     and save the setting, but it reaches the radio only when the app
 *     closes.
 * When it closes: the mode and the mode lock of before, the zoom stored
 * for that mode, the DSP settings.  The frequency stays the app's.
 */
#pragma once

void app_radio_begin(void);
void app_radio_end(void);
