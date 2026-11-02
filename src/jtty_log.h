/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY QSO log
 *
 *  A QSO goes three ways at once: the radio's QSO log (qso_log.db, MODE
 *  JTTY, which also answers "worked before?"), /mnt/jtty_log.adi with the
 *  contest exchanges sent and received, and the WSJT-X UDP "QSO Logged"
 *  messages for HamLogBridge and other loggers when UDP is on.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define JTTY_LOG_PATH "/mnt/jtty_log.adi"

typedef struct {
    char     call[16];
    time_t   when;          /* UTC, the end of the QSO */
    uint64_t dial_hz;
    char     sent[24];      /* exchange sent, "599 007", may be empty */
    char     rcvd[24];      /* exchange received, "599 012", may be empty */
} jtty_qso_t;

void jtty_log_open(void);
void jtty_log_close(void);

/* Saves the QSO everywhere; false when the call is empty. */
bool jtty_log_save(const jtty_qso_t *q);

/* Worked before in JTTY on the band of dial_hz. Cached: the database is
 * asked once per call until jtty_log_forget() (a band change). */
bool jtty_log_worked(const char *call, uint64_t dial_hz);
void jtty_log_forget(void);

/* The ADIF record of a QSO (no header), for the file and for UDP. */
size_t jtty_log_adif(char *out, size_t size, const jtty_qso_t *q, const char *my_call,
                     const char *my_grid, int tx_power_w);
