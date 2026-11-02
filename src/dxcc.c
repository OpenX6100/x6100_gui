/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - country (DXCC entity) of a callsign
 *
 *  cty.dat format (country-files.com/cty-dat-format):
 *
 *      Fed. Rep. of Germany:  14:  28:  EU:  51.00:  -10.00:  -1.0:  DL:
 *          DA,DB,DC,DD,DE,DF,DG,DH,DI,DJ,DK,DL,DM,DN,DO,DP,DQ,DR,=DL0ABC;
 *
 *  A header of eight ':'-terminated fields (name, CQ zone, ITU zone,
 *  continent, latitude, longitude, UTC offset, primary prefix - a leading
 *  '*' marks a WAE-only entity), then the entity's prefixes, separated by
 *  commas and ended by ';'. "=CALL" is a whole callsign, not a prefix.
 *  Overrides in (), [], <>, {} and ~~ after an entry (zones, position,
 *  continent, UTC offset) are not needed here and are cut off.
 *
 *  The whole file is read into one buffer and parsed in place; two sorted
 *  arrays point into it (prefixes, whole calls). A lookup is a binary
 *  search per prefix length, so a few microseconds per decode.
 */

#include "dxcc.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CTY_SD                     /* tests point these elsewhere */
#define CTY_SD      "/mnt/cty.dat"
#endif
#ifndef CTY_IMAGE
#define CTY_IMAGE   "/usr/share/x6100/cty.dat"
#endif
#define CTY_MAX     (4 * 1024 * 1024)
#define CALL_MAX    24

typedef struct {
    const char *name;
    const char *prefix;
} entity_t;

typedef struct {
    const char *key;
    unsigned    ent;
} alias_t;

typedef struct {
    alias_t  *v;
    unsigned  n;
    unsigned  cap;
} alias_list_t;

static char           *buf;
static entity_t       *ents;
static unsigned        n_ents;
static unsigned        cap_ents;
static alias_list_t    pfx;
static alias_list_t    exact;
static pthread_once_t  once = PTHREAD_ONCE_INIT;

/* ---- Loading ----------------------------------------------------------- */

static char *trim(char *s)
{
    char *e;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = '\0';
    }
    return s;
}

static bool add_alias(alias_list_t *l, const char *key, unsigned ent)
{
    if (l->n == l->cap) {
        unsigned  cap = l->cap ? l->cap * 2 : 4096;
        alias_t  *v = realloc(l->v, cap * sizeof(*v));

        if (v == NULL) {
            return false;
        }
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n].key = key;
    l->v[l->n].ent = ent;
    l->n++;
    return true;
}

/* cty.dat's names are official and some are long for a table row; these
 * are shown shorter. */
static const char *short_name(const char *name)
{
    static const char *const map[][2] = {
        { "Fed. Rep. of Germany",     "Germany" },
        { "United States",            "USA" },
        { "European Russia",          "Russia" },
        { "Asiatic Russia",           "Asiatic Russia" },
        { "Republic of Korea",        "South Korea" },
        { "DPR of Korea",             "North Korea" },
        { "United Arab Emirates",     "UAE" },
        { "Slovak Republic",          "Slovakia" },
        { "Czech Republic",           "Czechia" },
        { "Bosnia-Herzegovina",       "Bosnia" },
        { "Dem. Rep. of the Congo",   "DR Congo" },
        { "Republic of the Congo",    "Congo" },
        { "Republic of South Sudan",  "South Sudan" },
        { "Antigua & Barbuda",        "Antigua" },
        { "Trinidad & Tobago",        "Trinidad" },
        { "Turks & Caicos Islands",   "Turks & Caicos" },
        { "St. Kitts & Nevis",        "St. Kitts" },
        { "St. Vincent",              "St. Vincent" },
        { "Sov Mil Order of Malta",   "SMOM" },
        { "UK Base Areas on Cyprus",  "UK Cyprus" },
        { NULL, NULL }
    };
    int i;

    for (i = 0; map[i][0] != NULL; i++) {
        if (strcmp(name, map[i][0]) == 0) {
            return map[i][1];
        }
    }
    return name;
}

static bool add_entity(const char *name, const char *prefix)
{
    if (n_ents == cap_ents) {
        unsigned  cap = cap_ents ? cap_ents * 2 : 512;
        entity_t *v = realloc(ents, cap * sizeof(*v));

        if (v == NULL) {
            return false;
        }
        ents = v;
        cap_ents = cap;
    }
    ents[n_ents].name = short_name(name);
    ents[n_ents].prefix = prefix;
    n_ents++;
    return true;
}

static int alias_cmp(const void *a, const void *b)
{
    return strcmp(((const alias_t *)a)->key, ((const alias_t *)b)->key);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long  sz;
    char *data;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 || sz > CTY_MAX ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)sz + 1);
    if (data != NULL && fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data != NULL) {
        data[sz] = '\0';
    }
    return data;
}

static void parse(char *p)
{
    while (*p != '\0') {
        char    *field[8];
        char    *end;
        char    *tok;
        char    *save = NULL;
        unsigned ent;
        int      i;

        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        for (i = 0; i < 8; i++) {
            char *colon = strchr(p, ':');

            if (colon == NULL) {
                return;             /* truncated file: keep what we have */
            }
            *colon = '\0';
            field[i] = p;
            p = colon + 1;
        }
        {
            char *prefix = trim(field[7]);

            if (*prefix == '*') {
                prefix++;
            }
            if (!add_entity(trim(field[0]), prefix)) {
                return;
            }
        }
        ent = n_ents - 1;

        end = strchr(p, ';');
        if (end != NULL) {
            *end = '\0';
        }
        for (tok = strtok_r(p, ", \t\r\n", &save); tok != NULL;
             tok = strtok_r(NULL, ", \t\r\n", &save)) {
            bool whole = (*tok == '=');

            if (whole) {
                tok++;
            }
            tok[strcspn(tok, "([<{~")] = '\0';
            if (*tok == '\0') {
                continue;
            }
            if (!add_alias(whole ? &exact : &pfx, tok, ent)) {
                return;
            }
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
}

static void load(void)
{
    buf = read_file(CTY_SD);
    if (buf == NULL) {
        buf = read_file(CTY_IMAGE);
    }
    if (buf == NULL) {
        return;
    }
    parse(buf);
    qsort(pfx.v, pfx.n, sizeof(alias_t), alias_cmp);
    qsort(exact.v, exact.n, sizeof(alias_t), alias_cmp);
}

/* ---- Lookup ------------------------------------------------------------ */

static const alias_t *find(const alias_list_t *l, const char *key)
{
    alias_t k = { .key = key, .ent = 0 };

    if (l->n == 0) {
        return NULL;
    }
    return bsearch(&k, l->v, l->n, sizeof(alias_t), alias_cmp);
}

/* Longest prefix of `s` in the list, or -1. */
static int longest_prefix(const char *s)
{
    char   tmp[CALL_MAX];
    size_t len = strlen(s);

    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }
    for (; len > 0; len--) {
        const alias_t *a;

        memcpy(tmp, s, len);
        tmp[len] = '\0';
        a = find(&pfx, tmp);
        if (a != NULL) {
            return (int)a->ent;
        }
    }
    return -1;
}

/* A whole call first ("=..." entries), then its longest prefix. */
static int lookup_call(const char *s)
{
    const alias_t *a = find(&exact, s);

    return a != NULL ? (int)a->ent : longest_prefix(s);
}

/* Suffixes that say how or where the station operates, not which
 * country it is in: /P, /M, /QRP, a call area digit and the like. */
static bool is_modifier(const char *s)
{
    static const char *const mods[] = {
        "P", "M", "A", "B", "R", "T", "J", "QRP", "QRPP", "LH", "LGT",
        "AE", "AG", "KT", NULL
    };
    int i;

    if (s[0] != '\0' && s[1] == '\0' && isdigit((unsigned char)s[0])) {
        return true;
    }
    for (i = 0; mods[i] != NULL; i++) {
        if (strcmp(s, mods[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool dxcc_lookup(const char *call, const char **name, const char **prefix)
{
    char  s[CALL_MAX];
    char *part[3];
    int   n_parts = 0;
    int   ent = -1;
    int   i;
    int   o = 0;

    pthread_once(&once, load);
    if (call == NULL || n_ents == 0) {
        return false;
    }

    /* Upper case, without the <> of a hashed call. */
    for (i = 0; call[i] != '\0' && o < (int)sizeof(s) - 1; i++) {
        char c = call[i];

        if (c == '<' || c == '>' || isspace((unsigned char)c)) {
            continue;
        }
        s[o++] = (char)toupper((unsigned char)c);
    }
    s[o] = '\0';
    if (o < 2 || strstr(s, "...") != NULL) {
        return false;               /* "<...>": hash not resolved */
    }

    /* Whole calls in cty.dat may contain a '/' themselves. */
    {
        const alias_t *a = find(&exact, s);

        if (a != NULL) {
            ent = (int)a->ent;
        }
    }

    if (ent < 0) {
        char *save = NULL;
        char *t;

        for (t = strtok_r(s, "/", &save); t != NULL && n_parts < 3;
             t = strtok_r(NULL, "/", &save)) {
            if (strcmp(t, "MM") == 0 || strcmp(t, "AM") == 0) {
                return false;       /* maritime / aeronautical mobile */
            }
            if (n_parts > 0 && is_modifier(t)) {
                continue;
            }
            part[n_parts++] = t;
        }
        if (n_parts == 1) {
            ent = lookup_call(part[0]);
        } else if (n_parts >= 2) {
            /* "DL/SP2KMO" or "SP2KMO/DL": the shorter part says where
             * the station is. */
            int short_i = strlen(part[1]) < strlen(part[0]) ? 1 : 0;

            ent = longest_prefix(part[short_i]);
            if (ent < 0) {
                ent = lookup_call(part[1 - short_i]);
            }
        }
    }

    if (ent < 0) {
        return false;
    }
    if (name != NULL) {
        *name = ents[ent].name;
    }
    if (prefix != NULL) {
        *prefix = ents[ent].prefix;
    }
    return true;
}
