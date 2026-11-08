/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - ADIF records for the logs and for UDP
 */

#include "adif_rec.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* ---- modes ------------------------------------------------------------ */

typedef struct {
    qso_log_mode_t mode;
    const char    *name;        /* as the apps say it */
    const char    *adif_mode;
    const char    *adif_submode;
} mode_row_t;

static const mode_row_t modes[] = {
    {MODE_SSB,    "SSB",    "SSB",    NULL},
    {MODE_AM,     "AM",     "AM",     NULL},
    {MODE_FM,     "FM",     "FM",     NULL},
    {MODE_CW,     "CW",     "CW",     NULL},
    {MODE_FT8,    "FT8",    "FT8",    NULL},
    {MODE_FT4,    "FT4",    "MFSK",   "FT4"},
    {MODE_FT2,    "FT2",    "MFSK",   "FT2"},
    {MODE_JS8,    "JS8",    "MFSK",   "JS8"},
    {MODE_RTTY,   "RTTY",   "RTTY",   NULL},
    {MODE_JTTY,   "JTTY",   "JTTY",   NULL},   /* as WSJT-X writes it */
    {MODE_PSK31,  "PSK31",  "PSK",    "PSK31"},
    {MODE_OLIVIA, "OLIVIA", "OLIVIA", NULL},   /* SUBMODE "OLIVIA 32/1000" */
};

void adif_mode_of(qso_log_mode_t mode, const char **adif_mode, const char **adif_submode)
{
    *adif_mode = NULL;
    *adif_submode = NULL;
    for (size_t i = 0; i < N(modes); i++) {
        if (modes[i].mode == mode) {
            *adif_mode = modes[i].adif_mode;
            *adif_submode = modes[i].adif_submode;
            return;
        }
    }
}

qso_log_mode_t adif_mode_parse(const char *adif_mode, const char *adif_submode)
{
    if (adif_mode == NULL || adif_mode[0] == '\0') {
        return MODE_OTHER;
    }
    /* the submode first: MFSK alone says little */
    for (size_t i = 0; i < N(modes); i++) {
        if (modes[i].adif_submode && adif_submode &&
            strcasecmp(modes[i].adif_mode, adif_mode) == 0 &&
            strcasecmp(modes[i].adif_submode, adif_submode) == 0) {
            return modes[i].mode;
        }
    }
    for (size_t i = 0; i < N(modes); i++) {
        if (modes[i].adif_submode == NULL && strcasecmp(modes[i].adif_mode, adif_mode) == 0) {
            return modes[i].mode;
        }
    }
    /* ADIF before 3.0: "<mode:5>PSK31", "<mode:3>FT4" */
    for (size_t i = 0; i < N(modes); i++) {
        if (modes[i].adif_submode && strcasecmp(modes[i].adif_submode, adif_mode) == 0) {
            return modes[i].mode;
        }
    }
    return MODE_OTHER;
}

qso_log_mode_t adif_mode_by_name(const char *name)
{
    for (size_t i = 0; name && i < N(modes); i++) {
        if (strcasecmp(modes[i].name, name) == 0) {
            return modes[i].mode;
        }
    }
    return MODE_OTHER;
}

/* ---- records ---------------------------------------------------------- */

void adif_rec_begin(adif_rec_t *r, char *out, size_t size)
{
    r->out = out;
    r->size = size;
    r->len = 0;
    r->overflow = size == 0;
    if (size > 0) {
        out[0] = '\0';
    }
}

void adif_rec_str(adif_rec_t *r, const char *name, const char *value)
{
    int n;

    if (value == NULL || value[0] == '\0' || r->overflow) {
        return;
    }
    n = snprintf(r->out + r->len, r->size - r->len, "<%s:%d>%s ", name, (int)strlen(value), value);
    if (n < 0 || (size_t)n >= r->size - r->len) {
        r->overflow = true;
        r->out[r->len] = '\0';          /* no half field */
        return;
    }
    r->len += (size_t)n;
}

void adif_rec_int(adif_rec_t *r, const char *name, int value)
{
    char s[16];

    snprintf(s, sizeof(s), "%d", value);
    adif_rec_str(r, name, s);
}

void adif_rec_times(adif_rec_t *r, time_t on, time_t off)
{
    struct tm tm;
    char      date[40], hms[40];      /* room for any int: no truncation warning */

    gmtime_r(&on, &tm);
    snprintf(date, sizeof(date), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday);
    snprintf(hms, sizeof(hms), "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    adif_rec_str(r, "qso_date", date);
    adif_rec_str(r, "time_on", hms);

    gmtime_r(&off, &tm);
    snprintf(date, sizeof(date), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday);
    snprintf(hms, sizeof(hms), "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    adif_rec_str(r, "qso_date_off", date);
    adif_rec_str(r, "time_off", hms);
}

void adif_rec_freq(adif_rec_t *r, uint64_t hz)
{
    char mhz[24];

    snprintf(mhz, sizeof(mhz), "%.6f", (double)hz / 1e6);
    adif_rec_str(r, "band", adif_band_name(hz));
    adif_rec_str(r, "freq", mhz);
}

void adif_rec_mode(adif_rec_t *r, qso_log_mode_t mode, const char *submode)
{
    const char *m, *s;

    adif_mode_of(mode, &m, &s);
    adif_rec_str(r, "mode", m);
    adif_rec_str(r, "submode", submode ? submode : s);
}

size_t adif_rec_end(adif_rec_t *r)
{
    if (!r->overflow && r->size - r->len > strlen("<eor>\n")) {
        memcpy(r->out + r->len, "<eor>\n", sizeof("<eor>\n"));
        r->len += strlen("<eor>\n");
    } else {
        r->overflow = true;
    }
    if (r->overflow) {
        if (r->size > 0) {
            r->out[0] = '\0';
        }
        return 0;
    }
    return r->len;
}

bool adif_rec_append(const char *path, const char *title, const char *record)
{
    struct stat st;
    bool        fresh = stat(path, &st) != 0 || st.st_size == 0;
    FILE       *f = fopen(path, "a");
    bool        ok;

    if (f == NULL) {
        return false;
    }
    if (fresh) {
        fprintf(f, "%s\n%s", title ? title : "X6100 log", ADIF_HEADER);
    }
    ok = fputs(record, f) >= 0;
    ok = fflush(f) == 0 && ok;
    fsync(fileno(f));
    return fclose(f) == 0 && ok;
}

/* ---- bands ------------------------------------------------------------ */

const char *adif_band_name(uint64_t f)
{
    /* ADIF band names for the HF/6m ranges this radio covers. */
    if (f >=  1800000ULL && f <=  2000000ULL) return "160m";
    if (f >=  3500000ULL && f <=  4000000ULL) return "80m";
    if (f >=  5250000ULL && f <=  5450000ULL) return "60m";
    if (f >=  7000000ULL && f <=  7300000ULL) return "40m";
    if (f >= 10100000ULL && f <= 10150000ULL) return "30m";
    if (f >= 14000000ULL && f <= 14350000ULL) return "20m";
    if (f >= 18068000ULL && f <= 18168000ULL) return "17m";
    if (f >= 21000000ULL && f <= 21450000ULL) return "15m";
    if (f >= 24890000ULL && f <= 24990000ULL) return "12m";
    if (f >= 28000000ULL && f <= 29700000ULL) return "10m";
    if (f >= 50000000ULL && f <= 54000000ULL) return "6m";
    return "";
}
