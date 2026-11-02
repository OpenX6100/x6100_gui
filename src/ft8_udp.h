/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSJT-X compatible UDP broadcaster
 *
 *  Emits WSJT-X UDP messages (schema 3) so that GridTracker, JTAlert,
 *  Log4OM and friends can consume decodes from the radio's built-in
 *  FT8/FT4 application over WiFi - and, through bt_spp.c, to a phone
 *  over Bluetooth SPP.
 *
 *  Config file: /mnt/ft8_udp.conf   (created with defaults on first run,
 *  UDP enabled)
 *      enabled=1
 *      host=239.255.0.0,gateway   ; up to 4 targets: unicast, broadcast,
 *                                 ; multicast, or "gateway" = the default
 *                                 ; gateway (the phone on a hotspot)
 *      port=2237
 *      id=X6100
 *
 *  ft8_udp_is_enabled() / ft8_udp_set_enabled() back the UDP switch in
 *  the Bluetooth window; they work with the FT8 window closed too.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read config, open the socket. Safe to call repeatedly. */
void ft8_udp_init(void);

/* Close the socket and release resources. */
void ft8_udp_deinit(void);

bool ft8_udp_is_enabled(void);

/* Toggle at runtime (persists to the config file). Both work with the
 * FT8 window closed too: they then read/write the config file only. */
void ft8_udp_set_enabled(bool on);

/* Current destination, for display in the UI. */
const char *ft8_udp_host(void);
uint16_t    ft8_udp_port(void);

/* --- Message emitters (no-ops when disabled) ------------------------- */

/* Heartbeat: lets receivers discover us. Call once per ~15 s. */
void ft8_udp_send_heartbeat(void);

/* Status: dial frequency, mode, own call/grid, tx state. */
void ft8_udp_send_status(uint64_t dial_freq_hz, const char *mode,
                         const char *de_call, const char *de_grid,
                         const char *dx_call, uint32_t rx_df, uint32_t tx_df,
                         bool transmitting, bool decoding);

/* Decode: one decoded message.
 *   time_sec  - seconds since the start of the current UTC day is computed
 *               internally; pass the decode's offset within the slot only
 *               if you need it for delta_t (see delta_t_sec).
 *   df_hz     - audio offset of the signal (Hz)
 *   delta_t_sec - time offset of the decode (s), as WSJT-X reports it
 */
void ft8_udp_send_decode(bool is_new, const char *mode, const char *message,
                         int32_t snr, float delta_t_sec, uint32_t df_hz);

/* Clear: tells receivers to wipe their band activity window. */
void ft8_udp_send_clear(void);

/* Log a QSO to every listener: sends both the structured "QSO Logged"
 * (type 5) and "Logged ADIF" (type 12) messages, exactly as WSJT-X does.
 * Builds a valid ADIF record internally (band derived from frequency,
 * empty fields omitted rather than emitted with zero length). */
void ft8_udp_log_qso(time_t when, const char *dx_call, const char *dx_grid,
                     uint64_t freq_hz, const char *mode,
                     int snr_sent, int snr_rcvd, int tx_power_w,
                     const char *my_call, const char *my_grid);

/* Exposed for testing: ADIF band name for a frequency, "" if unknown. */
const char *ft8_udp_band_name(uint64_t freq_hz);

/* Exposed for testing: build the ADIF record. Returns length written. */
size_t ft8_udp_build_adif(char *out, size_t out_sz, time_t when,
                          const char *dx_call, const char *dx_grid,
                          uint64_t freq_hz, const char *mode,
                          int snr_sent, int snr_rcvd, int tx_power_w,
                          const char *my_call, const char *my_grid);

/* QSO logged (structured, WSJT-X type 5). This is what most loggers and
 * GridTracker act on. Times are UTC. */
void ft8_udp_send_qso_logged(time_t when, const char *dx_call, const char *dx_grid,
                             uint64_t tx_freq_hz, const char *mode,
                             const char *report_sent, const char *report_rcvd,
                             const char *tx_power, const char *comments,
                             const char *name, const char *my_call,
                             const char *my_grid);

/* QSO logged (ADIF text, WSJT-X type 12). Sent alongside the above for
 * receivers that prefer the ADIF form. Pass a full ADIF fragment. */
void ft8_udp_send_logged_adif(const char *adif_record);

#ifdef __cplusplus
}
#endif
