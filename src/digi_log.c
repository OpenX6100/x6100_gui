/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - QSO log of the keyboard modes
 */

#include "digi_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cfg/cfg_api.h"
#include "digi_qso.h"
#include "ft8_udp.h"
#include "msg.h"
#include "qso_log.h"
#include "util.h"

/* The keyboard modes cap the transmitter at 5 W (psk_tx.c, olivia_tx.c,
 * rtty_tx.c): that is the power that went out. */
#define MAX_PWR_W 5.0f

static bool udp_open;

void digi_log_open(void)
{
    digi_qso_refresh_mycall();
    if (!udp_open) {
        ft8_udp_init();         /* safe to call again; no-op when UDP is off */
        udp_open = true;
    }
}

void digi_log_close(void)
{
    if (udp_open) {
        ft8_udp_deinit();
        udp_open = false;
    }
}

static const char *adif_mode(digi_log_mode_t m)
{
    switch (m) {
        case DIGI_LOG_PSK31:  return "PSK";
        case DIGI_LOG_OLIVIA: return "OLIVIA";
        default:              return "RTTY";
    }
}

static const char *adif_submode(const digi_log_qso_t *q)
{
    switch (q->mode) {
        case DIGI_LOG_PSK31:  return "PSK31";
        case DIGI_LOG_OLIVIA: return q->submode;
        default:              return "";
    }
}

/* The mode loggers expect in the WSJT-X "QSO Logged" message. */
static const char *udp_mode(digi_log_mode_t m)
{
    switch (m) {
        case DIGI_LOG_PSK31:  return "PSK31";
        case DIGI_LOG_OLIVIA: return "OLIVIA";
        default:              return "RTTY";
    }
}

static qso_log_mode_t db_mode(digi_log_mode_t m)
{
    switch (m) {
        case DIGI_LOG_PSK31:  return MODE_PSK31;
        case DIGI_LOG_OLIVIA: return MODE_OLIVIA;
        default:              return MODE_RTTY;
    }
}

void digi_log_from_qso(digi_log_qso_t *q, digi_log_mode_t mode, const char *submode,
                       uint64_t freq_hz)
{
    memset(q, 0, sizeof(*q));
    q->mode = mode;
    snprintf(q->submode, sizeof(q->submode), "%s", submode ? submode : "");
    snprintf(q->call, sizeof(q->call), "%s", digi_qso_dx());
    snprintf(q->name, sizeof(q->name), "%s", digi_qso_name());
    snprintf(q->qth, sizeof(q->qth), "%s", digi_qso_qth());
    snprintf(q->rst_sent, sizeof(q->rst_sent), "%s", digi_qso_rst_sent());
    snprintf(q->rst_rcvd, sizeof(q->rst_rcvd), "%s", digi_qso_rst_rcvd());
    q->end = time(NULL);
    q->start = digi_qso_start();
    if (q->start > q->end) {
        q->start = q->end;
    }
    q->freq_hz = freq_hz;
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

size_t digi_log_adif(char *out, size_t size, const digi_log_qso_t *q, const char *my_call,
                     const char *my_grid, int tx_power_w)
{
    struct tm tm;
    char      date_on[40], time_on[40], date_off[40], time_off[40];
    char      freq[24];
    char      pwr[12];
    size_t    o = 0;
    int       n;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    gmtime_r(&q->start, &tm);
    snprintf(date_on, sizeof(date_on), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday);
    snprintf(time_on, sizeof(time_on), "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    gmtime_r(&q->end, &tm);
    snprintf(date_off, sizeof(date_off), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday);
    snprintf(time_off, sizeof(time_off), "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(freq, sizeof(freq), "%.6f", (double)q->freq_hz / 1e6);
    snprintf(pwr, sizeof(pwr), "%d", tx_power_w);

    adif_add(out, size, &o, "call", q->call);
    adif_add(out, size, &o, "mode", adif_mode(q->mode));
    adif_add(out, size, &o, "submode", adif_submode(q));
    adif_add(out, size, &o, "rst_sent", q->rst_sent);
    adif_add(out, size, &o, "rst_rcvd", q->rst_rcvd);
    adif_add(out, size, &o, "qso_date", date_on);
    adif_add(out, size, &o, "time_on", time_on);
    adif_add(out, size, &o, "qso_date_off", date_off);
    adif_add(out, size, &o, "time_off", time_off);
    adif_add(out, size, &o, "band", ft8_udp_band_name(q->freq_hz));
    adif_add(out, size, &o, "freq", freq);
    adif_add(out, size, &o, "name", q->name);
    adif_add(out, size, &o, "qth", q->qth);
    adif_add(out, size, &o, "station_callsign", my_call);
    adif_add(out, size, &o, "my_gridsquare", my_grid);
    adif_add(out, size, &o, "tx_pwr", tx_power_w > 0 ? pwr : "");

    n = snprintf(out + o, size - o, "<eor>\n");
    if (n > 0 && (size_t)n < size - o) {
        o += (size_t)n;
    }
    return o;
}

static void append_file(const char *record)
{
    struct stat st;
    bool        fresh = stat(DIGI_LOG_PATH, &st) != 0 || st.st_size == 0;
    FILE       *f = fopen(DIGI_LOG_PATH, "a");

    if (f == NULL) {
        return;
    }
    if (fresh) {
        fprintf(f, "X6100 PSK31 / Olivia / RTTY log\n<adif_ver:5>3.1.4 <programid:5>X6100 <eoh>\n");
    }
    fputs(record, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

bool digi_log_save(const digi_log_qso_t *q)
{
    char             my_call[PARAM_TEXT_MAX];
    char             my_grid[PARAM_TEXT_MAX];
    char             adif[1024];
    char            *canon;
    float            p = param_f_get(cfg.pwr());
    int              pwr = (int)((p < MAX_PWR_W ? p : MAX_PWR_W) + 0.5f);
    qso_log_record_t rec;

    if (q->call[0] == '\0') {
        return false;
    }
    param_t_get_into(cfg.callsign(), my_call, sizeof(my_call));
    param_t_get_into(cfg.qth(), my_grid, sizeof(my_grid));

    canon = util_canonize_callsign(q->call, false);
    rec = qso_log_record_create(my_call, canon ? canon : q->call, q->start, db_mode(q->mode),
                                digi_qso_rst_num(q->rst_sent), digi_qso_rst_num(q->rst_rcvd),
                                q->freq_hz, q->name, q->qth, my_grid, NULL);
    free(canon);
    qso_log_record_save(rec);

    if (digi_log_adif(adif, sizeof(adif), q, my_call, my_grid, pwr) > 0) {
        char pwr_s[12];

        append_file(adif);
        snprintf(pwr_s, sizeof(pwr_s), "%d", pwr);
        ft8_udp_send_qso_logged(q->end, q->call, "", q->freq_hz, udp_mode(q->mode), q->rst_sent,
                                q->rst_rcvd, pwr_s, "", q->name, my_call, my_grid);
        /* the QTH has no field there: it goes with the ADIF below */
        ft8_udp_send_logged_adif(adif);
    }
    return true;
}

bool digi_log_current(digi_log_mode_t mode, const char *submode, uint64_t freq_hz)
{
    digi_log_qso_t q;

    digi_log_from_qso(&q, mode, submode, freq_hz);
    if (q.call[0] == '\0') {
        msg_update_text_fmt("#FF0000 No DX call to log - Menu > DX call");
        return false;
    }
    if (!digi_log_save(&q)) {
        msg_update_text_fmt("#FF0000 Could not log %s", q.call);
        return false;
    }
    digi_qso_set_logged(true);
    msg_update_text_fmt("#FFFFFF Logged %s  %s / %s%s%s", q.call, q.rst_sent, q.rst_rcvd,
                        q.name[0] ? "  " : "", q.name);
    return true;
}
