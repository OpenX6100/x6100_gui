/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY QSO log
 */

#include "jtty_log.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adif_rec.h"
#include "cfg/cfg_api.h"
#include "ft8_udp.h"
#include "jtty_tx.h"
#include "qso_log.h"
#include "util.h"

#define WORKED_CACHE 64

static struct {
    char call[16];
    bool worked;
} cache[WORKED_CACHE];
static int             ncache;
static qso_log_band_t  cache_band = BAND_OTHER;

void jtty_log_open(void)
{
    ft8_udp_init();             /* safe to call again; no-op when UDP is off */
    jtty_log_forget();
}

void jtty_log_close(void)
{
    ft8_udp_close();            /* not the Bluetooth bridge: that is FT8's */
}

void jtty_log_forget(void)
{
    ncache = 0;
}

/* "599 012" -> "12": a contest serial, for ADIF's numeric STX/SRX. */
static bool serial_of(const char *exch, char *out, size_t size)
{
    const char *p = exch;
    long        v;
    char       *end;

    if (strncmp(p, "599 ", 4) == 0) {
        p += 4;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    v = strtol(p, &end, 10);
    if (*end != '\0' || v < 0) {
        return false;
    }
    snprintf(out, size, "%ld", v);
    return true;
}

size_t jtty_log_adif(char *out, size_t size, const jtty_qso_t *q, const char *my_call,
                     const char *my_grid, int tx_power_w)
{
    adif_rec_t r;
    char       num[16];

    adif_rec_begin(&r, out, size);
    adif_rec_str(&r, "call", q->call);
    adif_rec_mode(&r, MODE_JTTY, NULL);
    adif_rec_str(&r, "rst_sent", "599");
    adif_rec_str(&r, "rst_rcvd", "599");
    adif_rec_times(&r, q->when, q->when);
    adif_rec_freq(&r, q->dial_hz);
    adif_rec_str(&r, "station_callsign", my_call);
    adif_rec_str(&r, "my_gridsquare", my_grid);
    if (tx_power_w > 0) {
        adif_rec_int(&r, "tx_pwr", tx_power_w);
    }
    if (serial_of(q->sent, num, sizeof(num))) {
        adif_rec_str(&r, "stx", num);
    } else if (strncmp(q->sent, "599 ", 4) == 0) {
        adif_rec_str(&r, "stx_string", q->sent + 4);
    }
    if (serial_of(q->rcvd, num, sizeof(num))) {
        adif_rec_str(&r, "srx", num);
    } else if (strncmp(q->rcvd, "599 ", 4) == 0) {
        adif_rec_str(&r, "srx_string", q->rcvd + 4);
    }
    return adif_rec_end(&r);
}

bool jtty_log_save(const jtty_qso_t *q)
{
    char             my_call[PARAM_TEXT_MAX];
    char             my_grid[PARAM_TEXT_MAX];
    char             adif[768];
    char            *canon;
    /* what was sent: JTTY caps the power (jtty_tx.c) */
    int              pwr = (int)(fminf(param_f_get(cfg.pwr()), JTTY_TX_MAX_PWR_W) + 0.5f);
    qso_log_record_t rec;

    if (q->call[0] == '\0') {
        return false;
    }
    param_t_get_into(cfg.callsign(), my_call, sizeof(my_call));
    param_t_get_into(cfg.qth(), my_grid, sizeof(my_grid));

    canon = util_canonize_callsign(q->call, false);
    rec = qso_log_record_create(my_call, canon ? canon : q->call, q->when, MODE_JTTY, 599, 599,
                                q->dial_hz, NULL, NULL, my_grid, NULL);
    free(canon);
    if (qso_log_record_save(rec) != 0) {
        return false;                   /* e.g. no callsign set: nothing was logged */
    }

    if (jtty_log_adif(adif, sizeof(adif), q, my_call, my_grid, pwr) > 0) {
        adif_rec_append(JTTY_LOG_PATH, "X6100 JTTY log", adif);
        ft8_udp_send_qso_logged(q->when, q->call, "", q->dial_hz, "JTTY", "599", "599", "",
                                q->rcvd, "", my_call, my_grid);
        ft8_udp_send_logged_adif(adif);
    }

    /* worked now: the cache must say so */
    for (int i = 0; i < ncache; i++) {
        if (strcmp(cache[i].call, q->call) == 0) {
            cache[i].worked = true;
            return true;
        }
    }
    if (ncache < WORKED_CACHE && qso_log_freq_to_band(q->dial_hz) == cache_band) {
        snprintf(cache[ncache].call, sizeof(cache[ncache].call), "%s", q->call);
        cache[ncache++].worked = true;
    }
    return true;
}

bool jtty_log_worked(const char *call, uint64_t dial_hz)
{
    qso_log_band_t band = qso_log_freq_to_band(dial_hz);
    bool           worked;

    if (call == NULL || call[0] == '\0') {
        return false;
    }
    if (band != cache_band) {
        ncache = 0;
        cache_band = band;
    }
    for (int i = 0; i < ncache; i++) {
        if (strcmp(cache[i].call, call) == 0) {
            return cache[i].worked;
        }
    }
    worked = qso_log_search_worked(call, MODE_JTTY, band) == SEARCH_WORKED_SAME_MODE;
    if (ncache == WORKED_CACHE) {
        memmove(cache, cache + 1, sizeof(cache[0]) * (WORKED_CACHE - 1));
        ncache--;
    }
    snprintf(cache[ncache].call, sizeof(cache[ncache].call), "%s", call);
    cache[ncache++].worked = worked;
    return worked;
}
