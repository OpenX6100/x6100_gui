/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - QSO log of the keyboard modes
 *
 *  Log QSO in PSK31, Olivia and RTTY sends a QSO three ways at once, as
 *  JTTY does: the radio's QSO log (qso_log.db, which also answers
 *  "worked before?"), /mnt/digi_log.adi (all three modes, one file) and
 *  the WSJT-X UDP "QSO Logged" messages for HamLogBridge, Log4OM and
 *  other loggers when UDP is on.
 *
 *  What is logged comes from digi_qso.c: the DX call, RST sent and
 *  received, the DX name and QTH, the time the DX call was set as the
 *  start and now as the end. The frequency is the signal's, the dial
 *  plus the audio offset, as fldigi logs it.
 *
 *  ADIF modes: PSK31 is MODE PSK SUBMODE PSK31, Olivia MODE OLIVIA with
 *  SUBMODE "OLIVIA 32/1000" (and so on), RTTY MODE RTTY.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIGI_LOG_PATH "/mnt/digi_log.adi"

typedef enum {
    DIGI_LOG_PSK31,
    DIGI_LOG_OLIVIA,
    DIGI_LOG_RTTY,
} digi_log_mode_t;

typedef struct {
    digi_log_mode_t mode;
    char            submode[24];   /* Olivia: "OLIVIA 32/1000", else "" */
    char            call[16];
    char            name[32];
    char            qth[32];
    char            rst_sent[8];
    char            rst_rcvd[8];
    time_t          start;         /* UTC */
    time_t          end;
    uint64_t        freq_hz;       /* the signal, not the dial */
} digi_log_qso_t;

/* The window opens and closes UDP with these. */
void   digi_log_open(void);
void   digi_log_close(void);

/* Fills a QSO from the shared QSO state (digi_qso.h), ending now. */
void   digi_log_from_qso(digi_log_qso_t *q, digi_log_mode_t mode, const char *submode,
                         uint64_t freq_hz);

/* Saves the QSO everywhere; false when the call is empty. */
bool   digi_log_save(const digi_log_qso_t *q);

/* Logs the current QSO and says so on the message line; marks it logged
 * in digi_qso. False (and a message) when there is no DX call. */
bool   digi_log_current(digi_log_mode_t mode, const char *submode, uint64_t freq_hz);

/* The ADIF record of a QSO (no header), for the file and for UDP. */
size_t digi_log_adif(char *out, size_t size, const digi_log_qso_t *q, const char *my_call,
                     const char *my_grid, int tx_power_w);

#ifdef __cplusplus
}
#endif
