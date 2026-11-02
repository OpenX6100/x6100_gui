/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY QSO log
 */

#include "jtty_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cfg/cfg_api.h"
#include "ft8_udp.h"
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
    ft8_udp_deinit();
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

static void adif_add(char *out, size_t size, size_t *o, const char *name, const char *value)
{
    int n;

    if (value == NULL || value[0] == '\0' || *o >= size) {
        return;
    }
    n = snprintf(out + *o, size - *o, "<%s:%d>%s ", name, (int)strlen(value), value);
    if (n > 0 && (size_t)n < size - *o) {
        *o += (size_t)n;
    }
}

size_t jtty_log_adif(char *out, size_t size, const jtty_qso_t *q, const char *my_call,
                     const char *my_grid, int tx_power_w)
{
    struct tm tm;
    char      date[16];
    char      tim[16];
    char      freq[24];
    char      pwr[12];
    char      num[16];
    size_t    o = 0;
    int       n;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    gmtime_r(&q->when, &tm);
    snprintf(date, sizeof(date), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(tim, sizeof(tim), "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(freq, sizeof(freq), "%.6f", (double)q->dial_hz / 1e6);
    snprintf(pwr, sizeof(pwr), "%d", tx_power_w);

    adif_add(out, size, &o, "call", q->call);
    adif_add(out, size, &o, "mode", "JTTY");      /* as WSJT-X logs it */
    adif_add(out, size, &o, "rst_sent", "599");
    adif_add(out, size, &o, "rst_rcvd", "599");
    adif_add(out, size, &o, "qso_date", date);
    adif_add(out, size, &o, "time_on", tim);
    adif_add(out, size, &o, "qso_date_off", date);
    adif_add(out, size, &o, "time_off", tim);
    adif_add(out, size, &o, "band", ft8_udp_band_name(q->dial_hz));
    adif_add(out, size, &o, "freq", freq);
    adif_add(out, size, &o, "station_callsign", my_call);
    adif_add(out, size, &o, "my_gridsquare", my_grid);
    adif_add(out, size, &o, "tx_pwr", tx_power_w > 0 ? pwr : "");
    if (serial_of(q->sent, num, sizeof(num))) {
        adif_add(out, size, &o, "stx", num);
    } else if (strncmp(q->sent, "599 ", 4) == 0) {
        adif_add(out, size, &o, "stx_string", q->sent + 4);
    }
    if (serial_of(q->rcvd, num, sizeof(num))) {
        adif_add(out, size, &o, "srx", num);
    } else if (strncmp(q->rcvd, "599 ", 4) == 0) {
        adif_add(out, size, &o, "srx_string", q->rcvd + 4);
    }

    n = snprintf(out + o, size - o, "<eor>\n");
    if (n > 0 && (size_t)n < size - o) {
        o += (size_t)n;
    }
    return o;
}

static void append_file(const char *record)
{
    struct stat st;
    bool        fresh = stat(JTTY_LOG_PATH, &st) != 0 || st.st_size == 0;
    FILE       *f = fopen(JTTY_LOG_PATH, "a");

    if (f == NULL) {
        return;
    }
    if (fresh) {
        fprintf(f, "X6100 JTTY log\n<adif_ver:5>3.1.0 <programid:5>X6100 <eoh>\n");
    }
    fputs(record, f);
    fclose(f);
}

bool jtty_log_save(const jtty_qso_t *q)
{
    char             my_call[PARAM_TEXT_MAX];
    char             my_grid[PARAM_TEXT_MAX];
    char             adif[768];
    char            *canon;
    int              pwr = (int)(param_f_get(cfg.pwr()) + 0.5f);
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
    qso_log_record_save(rec);

    if (jtty_log_adif(adif, sizeof(adif), q, my_call, my_grid, pwr) > 0) {
        append_file(adif);
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
