/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 - WEFAX stations and broadcast schedules, see stations.h.
 */
#include "stations.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FREQS   8       /* per station */
#define MAX_GROUPS  16      /* per station */

typedef struct {
    int     minute;
    int     text;           /* offset in buf */
    uint32_t groups;        /* bit mask of the groups that send it */
} entry_t;

typedef struct {
    int      country, place, call, note;    /* offsets in buf */
    int      nfreq;
    int32_t  hz[MAX_FREQS];
    int      fcall[MAX_FREQS];              /* per-frequency callsign, -1 = station's */
    int      ngroups;
    uint32_t group_freqs[MAX_GROUPS];       /* bit mask of frequencies per group */
    int      first_entry, nentries;
} station_t;

static char *buf = NULL;
static size_t buf_len = 0, buf_cap = 0;
static station_t *st = NULL;
static int nst = 0, cap_st = 0;
static entry_t *ent = NULL;
static int nent = 0, cap_ent = 0;
static wefax_row_t *rows = NULL;
static int nrows = 0;
static wefax_entry_t *sched = NULL;
static int sched_cap = 0;

static int str_add(const char *s)
{
    size_t n = strlen(s) + 1;
    if (buf_len + n > buf_cap) {
        size_t c = buf_cap ? buf_cap * 2 : 65536;
        while (c < buf_len + n) c *= 2;
        char *b = realloc(buf, c);
        if (!b) return 0;
        buf = b;
        buf_cap = c;
    }
    memcpy(buf + buf_len, s, n);
    int off = (int)buf_len;
    buf_len += n;
    return off;
}

/* split line in place at '|'; returns the number of fields */
static int split(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; ++p)
        if (*p == '|') { *p = '\0'; f[n++] = p + 1; }
    return n;
}

static int32_t khz_to_hz(const char *s)
{
    return (int32_t)(strtod(s, NULL) * 1000.0 + 0.5);
}

static void free_all(void)
{
    free(buf); buf = NULL; buf_len = buf_cap = 0;
    free(st); st = NULL; nst = cap_st = 0;
    free(ent); ent = NULL; nent = cap_ent = 0;
    free(rows); rows = NULL; nrows = 0;
    free(sched); sched = NULL; sched_cap = 0;
}

static bool load_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    str_add("");                        /* offset 0 = "" */
    char line[512];
    station_t *s = NULL;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[1] != '|') continue;
        char *f[8];
        int n = split(line, f, 8);
        if (f[0][0] == 'S' && n >= 5) {
            if (nst == cap_st) {
                cap_st = cap_st ? cap_st * 2 : 32;
                station_t *t = realloc(st, sizeof(station_t) * (size_t)cap_st);
                if (!t) break;
                st = t;
            }
            s = &st[nst++];
            memset(s, 0, sizeof(*s));
            s->country = str_add(f[1]);
            s->place = str_add(f[2]);
            s->call = str_add(f[3]);
            s->note = str_add(n >= 6 ? f[5] : "");
            s->first_entry = nent;
            char *save = NULL;
            for (char *tok = strtok_r(f[4], " ", &save); tok && s->nfreq < MAX_FREQS; tok = strtok_r(NULL, " ", &save)) {
                char *c = strchr(tok, ':');
                if (c) *c++ = '\0';
                s->hz[s->nfreq] = khz_to_hz(tok);
                s->fcall[s->nfreq] = c ? str_add(c) : -1;
                s->nfreq++;
            }
        } else if (f[0][0] == 'G' && n >= 2 && s) {
            if (s->ngroups >= MAX_GROUPS) continue;
            uint32_t mask = 0;
            char *save = NULL;
            for (char *tok = strtok_r(f[1], " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
                int32_t hz = khz_to_hz(tok);
                for (int i = 0; i < s->nfreq; ++i) if (s->hz[i] == hz) mask |= 1u << i;
            }
            s->group_freqs[s->ngroups++] = mask;
        } else if (f[0][0] == 'E' && n >= 3 && s && s->ngroups > 0) {
            int t = atoi(f[1]);
            if (t < 0 || t > 2359 || t % 100 > 59) continue;
            if (nent == cap_ent) {
                cap_ent = cap_ent ? cap_ent * 2 : 1024;
                entry_t *e = realloc(ent, sizeof(entry_t) * (size_t)cap_ent);
                if (!e) break;
                ent = e;
            }
            entry_t *e = &ent[nent++];
            e->minute = t / 100 * 60 + t % 100;
            e->text = str_add(f[2]);
            e->groups = 1u << (s->ngroups - 1);
            s->nentries = nent - s->first_entry;
        }
    }
    fclose(fp);

    /* one row per frequency, in file order */
    int total = 0;
    for (int i = 0; i < nst; ++i) total += st[i].nfreq;
    rows = calloc((size_t)(total ? total : 1), sizeof(wefax_row_t));
    if (!rows) return false;
    const char *last_country = NULL;
    for (int i = 0; i < nst; ++i) {
        station_t *x = &st[i];
        for (int k = 0; k < x->nfreq; ++k) {
            wefax_row_t *r = &rows[nrows++];
            r->country = buf + x->country;
            r->place = buf + x->place;
            r->call = buf + (x->fcall[k] >= 0 ? x->fcall[k] : x->call);
            r->note = buf + x->note;
            r->hz = x->hz[k];
            r->station = i;
            r->first = !last_country || strcmp(last_country, r->country) != 0;
            last_country = r->country;
        }
    }
    return nrows > 0;
}

static bool load_tried = false;

int wefax_db_load(void)
{
    if (nrows || load_tried) return nrows;
    load_tried = true;
    const char *paths[] = { WEFAX_DB_FILE, WEFAX_DB_DEFAULT_FILE };
    for (int i = 0; i < 2; ++i) {
        if (load_file(paths[i])) {
            fprintf(stderr, "WEFAX: %d stations, %d frequencies, %d broadcasts from %s\n",
                    nst, nrows, nent, paths[i]);
            return nrows;
        }
        free_all();
    }
    fprintf(stderr, "WEFAX: no stations file (%s or %s)\n", paths[0], paths[1]);
    return 0;
}

void wefax_db_free(void)
{
    free_all();
    load_tried = false;
}

int wefax_db_rows(void)
{
    return nrows;
}

const wefax_row_t *wefax_db_row(int i)
{
    return (i >= 0 && i < nrows) ? &rows[i] : NULL;
}

int wefax_db_find(int32_t hz)
{
    for (int i = 0; i < nrows; ++i) if (rows[i].hz == hz) return i;
    return -1;
}

static int cmp_entry(const void *a, const void *b)
{
    const wefax_entry_t *x = a, *y = b;
    if (x->minute != y->minute) return x->minute - y->minute;
    return strcmp(x->text, y->text);
}

int wefax_db_schedule(int row, const wefax_entry_t **out)
{
    *out = NULL;
    if (row < 0 || row >= nrows) return 0;
    station_t *s = &st[rows[row].station];
    int fi = -1;
    for (int k = 0; k < s->nfreq; ++k) if (s->hz[k] == rows[row].hz) { fi = k; break; }
    if (fi < 0 || s->nentries == 0) return 0;
    uint32_t groups = 0;
    for (int g = 0; g < s->ngroups; ++g) if (s->group_freqs[g] & (1u << fi)) groups |= 1u << g;
    if (s->nentries > sched_cap) {
        wefax_entry_t *t = realloc(sched, sizeof(wefax_entry_t) * (size_t)s->nentries);
        if (!t) return 0;
        sched = t;
        sched_cap = s->nentries;
    }
    int n = 0;
    for (int i = 0; i < s->nentries; ++i) {
        entry_t *e = &ent[s->first_entry + i];
        if (!(e->groups & groups)) continue;
        sched[n].minute = e->minute;
        sched[n].text = buf + e->text;
        n++;
    }
    qsort(sched, (size_t)n, sizeof(wefax_entry_t), cmp_entry);
    /* the same broadcast listed by two groups of the same frequency */
    int m = 0;
    for (int i = 0; i < n; ++i)
        if (m == 0 || sched[i].minute != sched[m - 1].minute || strcmp(sched[i].text, sched[m - 1].text))
            sched[m++] = sched[i];
    *out = sched;
    return m;
}

int wefax_db_next(const wefax_entry_t *e, int n, int now)
{
    if (n <= 0) return -1;
    for (int i = 0; i < n; ++i) if (e[i].minute >= now) return i;
    return 0;
}

int wefax_db_wait(const wefax_entry_t *e, int now)
{
    int d = e->minute - now;
    return d < 0 ? d + 1440 : d;
}
