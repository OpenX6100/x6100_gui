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

#include "adif_rec.h"
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
        ft8_udp_close();        /* not the Bluetooth bridge: that isn't ours */
        udp_open = false;
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
    q->stx = digi_qso_serial_sent();
    snprintf(q->srx, sizeof(q->srx), "%s", digi_qso_serial_rcvd());
    q->end = time(NULL);
    q->start = digi_qso_start();
    if (q->start > q->end) {
        q->start = q->end;
    }
    q->freq_hz = freq_hz;
}

size_t digi_log_adif(char *out, size_t size, const digi_log_qso_t *q, const char *my_call,
                     const char *my_grid, int tx_power_w)
{
    adif_rec_t r;

    adif_rec_begin(&r, out, size);
    adif_rec_str(&r, "call", q->call);
    adif_rec_mode(&r, db_mode(q->mode), q->mode == DIGI_LOG_OLIVIA ? q->submode : NULL);
    adif_rec_str(&r, "rst_sent", q->rst_sent);
    adif_rec_str(&r, "rst_rcvd", q->rst_rcvd);
    if (q->stx > 0) {
        adif_rec_int(&r, "stx", q->stx);
    }
    adif_rec_str(&r, "srx", q->srx);
    adif_rec_times(&r, q->start, q->end);
    adif_rec_freq(&r, q->freq_hz);
    adif_rec_str(&r, "name", q->name);
    adif_rec_str(&r, "qth", q->qth);
    adif_rec_str(&r, "station_callsign", my_call);
    adif_rec_str(&r, "my_gridsquare", my_grid);
    if (tx_power_w > 0) {
        adif_rec_int(&r, "tx_pwr", tx_power_w);
    }
    return adif_rec_end(&r);
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

        adif_rec_append(DIGI_LOG_PATH, "X6100 PSK31 / Olivia / RTTY log", adif);
        snprintf(pwr_s, sizeof(pwr_s), "%d", pwr);
        ft8_udp_send_qso_logged(q->end, q->call, "", q->freq_hz, udp_mode(q->mode), q->rst_sent,
                                q->rst_rcvd, pwr_s, "", q->name, my_call, my_grid);
        /* the QTH has no field there: it goes with the ADIF below */
        ft8_udp_send_logged_adif(adif);
    }
    return true;
}

bool digi_log_qso(const digi_log_qso_t *q)
{
    if (!digi_log_save(q)) {
        msg_update_text_fmt("#FF0000 Could not log %s", q->call);
        return false;
    }
    if (strcmp(digi_qso_dx(), q->call) == 0) {
        digi_qso_set_logged(true);      /* still the QSO on screen */
    }
    digi_qso_serial_used(q->stx);
    {
        char stx[8] = "";

        if (q->stx > 0) {
            snprintf(stx, sizeof(stx), " %03d", q->stx);
        }
        msg_update_text_fmt("#FFFFFF Logged %s  %s%s / %s%s%s%s%s", q->call, q->rst_sent, stx,
                            q->rst_rcvd, q->srx[0] ? " " : "", q->srx, q->name[0] ? "  " : "",
                            q->name);
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
    return digi_log_qso(&q);
}
