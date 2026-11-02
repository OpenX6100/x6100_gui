/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY transmit side of the source grammar
 *
 *  Written from the JTTY source-encoding description ("Automatic text
 *  packing"): at every token boundary the structured atoms whose
 *  canonical rendering matches the text there are candidates; TEXT5 takes
 *  exactly five characters (the last one may be shorter, padded). A
 *  dynamic program over character offsets picks the fewest frames,
 *  preferring a structured atom, then the longest span, on a tie.
 *
 *  Every result is decoded back with jtty_source.c and must render
 *  exactly as the normalized text; if not, the text goes as TEXT5 only,
 *  which is always valid.
 */

#include "jtty_pack.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char TEXT5_ALPHABET[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;";

static const char *const CONTROL[] = {
    "AGN?", "CALL?", "AGN CALL", "NR?", "AGN NR", "EXCH?", "STATE?",
    "SECTION?", "ZONE?", "GRID?", "RPRT?", "QSL TU", "TU", "QRZ?",
    "QSO B4", "WAIT", "NIL?", "OK?",
};
#define N_CONTROL ((int)(sizeof(CONTROL) / sizeof(CONTROL[0])))

static const char *const SECTIONS[] = {
    "AB",  "AK",  "AL",  "AR",  "AZ",  "BC",  "CO",  "CT",  "DE",  "EB",
    "EMA", "ENY", "EPA", "EWA", "GA",  "GH",  "IA",  "ID",  "IL",  "IN",
    "KS",  "KY",  "LA",  "LAX", "NS",  "MB",  "MDC", "ME",  "MI",  "MN",
    "MO",  "MS",  "MT",  "NC",  "ND",  "NE",  "NFL", "NH",  "NL",  "NLI",
    "NM",  "NNJ", "NNY", "TER", "NTX", "NV",  "OH",  "OK",  "ONE", "ONN",
    "ONS", "OR",  "ORG", "PAC", "PR",  "QC",  "RI",  "SB",  "SC",  "SCV",
    "SD",  "SDG", "SF",  "SFL", "SJV", "SK",  "SNJ", "STX", "SV",  "TN",
    "UT",  "VA",  "VI",  "VT",  "WCF", "WI",  "WMA", "WNY", "WPA", "WTX",
    "WV",  "WWA", "WY",  "DX",  "PE",  "NB",
};
#define N_SECTIONS ((int)(sizeof(SECTIONS) / sizeof(SECTIONS[0])))

/* ---- Normalization ------------------------------------------------------ */

void jtty_pack_normalize(const char *in, char *out, size_t size)
{
    size_t o = 0;
    size_t n = 0;
    bool   space = false;

    for (; *in != '\0' && n < JTTY_MSG_MAX; in++, n++) {
        char c = (char)toupper((unsigned char)*in);

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = (o > 0);
            continue;
        }
        if (c == '\0' || strchr(TEXT5_ALPHABET, c) == NULL) {
            c = '#';
        }
        if (space && o + 1 < size) {
            out[o++] = ' ';
        }
        space = false;
        if (o + 1 < size) {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* ---- Atoms ---------------------------------------------------------------- */

static jtty_payload_t grammar(uint32_t g)
{
    return (jtty_payload_t)g << 2;
}

static jtty_payload_t struct30(uint32_t body27, unsigned family)
{
    return grammar((body27 << 5) | (family << 2) | 2u);
}

static jtty_payload_t call_atom(uint32_t n28, unsigned i2, unsigned n2)
{
    return grammar((n28 << 4) | (n2 << 2) | i2);
}

/* A standard call that comes back the same through the 28-bit codec. */
static bool std_call(const char *tok, uint32_t *n28)
{
    char back[16];

    (void)back;
    return jtty_call_standard(tok) && jtty_call28_pack(tok, n28);
}

/* Canonical unsigned decimal 0..max: no sign, no leading zero but "0". */
static bool decimal(const char *tok, uint32_t max, uint32_t *v)
{
    uint32_t x = 0;
    size_t   n = strlen(tok);

    if (n == 0 || n > 6 || (n > 1 && tok[0] == '0')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)tok[i])) {
            return false;
        }
        x = x * 10 + (uint32_t)(tok[i] - '0');
    }
    if (x > max) {
        return false;
    }
    *v = x;
    return true;
}

static bool grid4(const char *tok, uint32_t *code)
{
    if (strlen(tok) != 4 || tok[0] < 'A' || tok[0] > 'R' || tok[1] < 'A' || tok[1] > 'R' ||
        !isdigit((unsigned char)tok[2]) || !isdigit((unsigned char)tok[3])) {
        return false;
    }
    *code = (uint32_t)((((tok[0] - 'A') * 18 + (tok[1] - 'A')) * 10 + (tok[2] - '0')) * 10 +
                       (tok[3] - '0'));
    return true;
}

/* Base-36 token, two characters, or three without a leading zero, with
 * at least one letter. */
static bool qth_token(const char *tok, uint32_t *length, uint32_t *token)
{
    size_t   n = strlen(tok);
    uint32_t v = 0;
    bool     letter = false;

    if (n < 2 || n > 3 || (n == 3 && tok[0] == '0')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = tok[i];

        if (isdigit((unsigned char)c)) {
            v = v * 36 + (uint32_t)(c - '0');
        } else if (c >= 'A' && c <= 'Z') {
            v = v * 36 + (uint32_t)(c - 'A' + 10);
            letter = true;
        } else {
            return false;
        }
    }
    *length = (n == 3);
    *token = v;
    return letter;
}

/* ---- Candidates at one position ------------------------------------------- */

typedef struct {
    int            len;         /* characters of rendering consumed */
    jtty_payload_t payload;
} cand_t;

#define MAX_CAND 16

/* Copies the k-th space-separated token from s (k = 0 first), "" past
 * the end. */
static void token_at(const char *s, int k, char *tok, size_t size)
{
    while (k-- > 0) {
        const char *sp = strchr(s, ' ');

        if (sp == NULL) {
            tok[0] = '\0';
            return;
        }
        s = sp + 1;
    }
    {
        const char *sp = strchr(s, ' ');
        size_t      n = sp ? (size_t)(sp - s) : strlen(s);

        if (n >= size) {
            n = size - 1;
        }
        memcpy(tok, s, n);
        tok[n] = '\0';
    }
}

/* Length of the first `k` tokens, with the spaces between them. */
static int span(const char *s, int k)
{
    int len = 0;

    for (int i = 0; i < k; i++) {
        char tok[32];

        token_at(s, i, tok, sizeof(tok));
        len += (int)strlen(tok) + (i ? 1 : 0);
    }
    return len;
}

static int candidates(const char *s, cand_t *c)
{
    char     t[4][32];
    int      n = 0;
    uint32_t n28;
    uint32_t v;

    for (int k = 0; k < 4; k++) {
        token_at(s, k, t[k], sizeof(t[k]));
    }

    /* call actions */
    if (!strcmp(t[0], "CQ") && !strcmp(t[2], "CQ") && std_call(t[1], &n28)) {
        c[n++] = (cand_t){span(s, 3), call_atom(n28, 0, 0)};
    }
    if (!strcmp(t[0], "TU") && !strcmp(t[2], "CQ") && std_call(t[1], &n28)) {
        c[n++] = (cand_t){span(s, 3), call_atom(n28, 0, 2)};
    }
    if (!strcmp(t[0], "TU") && !strcmp(t[1], "NOW") && std_call(t[2], &n28)) {
        c[n++] = (cand_t){span(s, 3), call_atom(n28, 1, 1)};
    }
    if (std_call(t[0], &n28)) {
        c[n++] = (cand_t){span(s, 1), call_atom(n28, 0, 1)};
        if (!strcmp(t[1], "TU")) {
            c[n++] = (cand_t){span(s, 2), call_atom(n28, 0, 3)};
        }
        if (!strcmp(t[1], "AGN?")) {
            c[n++] = (cand_t){span(s, 2), call_atom(n28, 1, 0)};
        }
    }

    /* CONTROL phrases, one or two words */
    for (int p = 0; p < N_CONTROL; p++) {
        size_t len = strlen(CONTROL[p]);

        if (strncmp(s, CONTROL[p], len) == 0 && (s[len] == '\0' || s[len] == ' ')) {
            c[n++] = (cand_t){(int)len, struct30((uint32_t)p << 16, 4)};
        }
    }

    /* "599 <number>": GENERIC_NUMERIC, full role; a bare number field-only */
    if (!strcmp(t[0], "599") && decimal(t[1], 131071, &v)) {
        c[n++] = (cand_t){span(s, 2), struct30((1u << 26) | (7u << 22) | (v << 5), 0)};
    }
    if (decimal(t[0], 131071, &v)) {
        c[n++] = (cand_t){span(s, 1), struct30((7u << 22) | (v << 5), 0)};
    }

    /* "599 <token>": GENERIC_QTH, full role only */
    {
        uint32_t length;
        uint32_t token;

        if (!strcmp(t[0], "599") && qth_token(t[1], &length, &token)) {
            c[n++] = (cand_t){span(s, 2),
                              struct30((1u << 26) | (3u << 22) | (length << 21) | (token << 5), 1)};
        }
    }

    /* GRID4, full with "599 " or field-only */
    if (!strcmp(t[0], "599") && grid4(t[1], &v)) {
        c[n++] = (cand_t){span(s, 2), struct30((1u << 23) | (1u << 22) | (v << 7), 4)};
    }
    if (grid4(t[0], &v)) {
        c[n++] = (cand_t){span(s, 1), struct30((1u << 23) | (v << 7), 4)};
    }

    /* CLASS_SECTION "<count><class> <section>" */
    {
        size_t   len = strlen(t[0]);
        uint32_t count;
        char     num[4];

        if (len >= 2 && len <= 3 && t[0][len - 1] >= 'A' && t[0][len - 1] <= 'F') {
            memcpy(num, t[0], len - 1);
            num[len - 1] = '\0';
            if (decimal(num, 32, &count) && count >= 1) {
                for (int sec = 0; sec < N_SECTIONS; sec++) {
                    if (!strcmp(t[1], SECTIONS[sec])) {
                        uint32_t cls = (uint32_t)(t[0][len - 1] - 'A');

                        c[n++] = (cand_t){span(s, 2),
                                          struct30((1u << 24) | (count << 18) | (cls << 15) |
                                                       ((uint32_t)(sec + 1) << 8), 2)};
                        break;
                    }
                }
            }
        }
    }
    return n;
}

/* ---- Packing ---------------------------------------------------------------- */

static jtty_payload_t text5(const char *s, int len)
{
    uint32_t g = 0;

    for (int k = 0; k < 5; k++) {
        char        ch = (k < len) ? s[k] : ' ';
        const char *p = strchr(TEXT5_ALPHABET, ch);

        g = (g << 6) | (uint32_t)(p ? p - TEXT5_ALPHABET : 35);
    }
    return grammar((g << 2) | 3u);
}

static bool renders_as(const jtty_payload_t *p, int n, const char *want)
{
    char msg[JTTY_MSG_MAX + 1] = "";

    for (int i = 0; i < n; i++) {
        jtty_atom_t atom;

        if (!jtty_source_decode(p[i], &atom)) {
            return false;
        }
        jtty_source_append(msg, &atom);
    }
    return strcmp(msg, want) == 0;
}

static int pack_text5_only(const char *s, jtty_payload_t *out)
{
    int len = (int)strlen(s);
    int n = 0;

    for (int i = 0; i < len && n < JTTY_MAX_FRAMES; i += 5) {
        out[n++] = text5(s + i, len - i < 5 ? len - i : 5);
    }
    if (n > 0) {
        out[n - 1] |= JTTY_EOM_BIT;
    }
    return n;
}

int jtty_pack_text(const char *text, jtty_payload_t out[JTTY_MAX_FRAMES],
                   char *norm, size_t norm_size)
{
    char           s[JTTY_MSG_MAX + 1];
    int            len;
    int            cost[JTTY_MSG_MAX + 2];
    int            next[JTTY_MSG_MAX + 2];
    int            used[JTTY_MSG_MAX + 2];          /* span consumed */
    jtty_payload_t pay[JTTY_MSG_MAX + 2];
    int            n = 0;

    jtty_pack_normalize(text, s, sizeof(s));
    if (norm != NULL) {
        snprintf(norm, norm_size, "%s", s);
    }
    len = (int)strlen(s);
    if (len == 0) {
        return 0;
    }

    /* fewest frames from each offset to the end */
    cost[len] = 0;
    for (int i = len - 1; i >= 0; i--) {
        cand_t c[MAX_CAND];
        int    nc = 0;
        int    best;
        bool   best_struct;

        /* TEXT5: five characters, or the padded last few */
        {
            int take = (len - i < 5) ? len - i : 5;

            best = 1 + cost[i + take];
            next[i] = i + take;
            used[i] = take;
            pay[i] = text5(s + i, take);
            best_struct = false;
        }

        if (s[i] != ' ' && (i == 0 || s[i - 1] == ' ')) {
            nc = candidates(s + i, c);
        }
        for (int k = 0; k < nc; k++) {
            int end = i + c[k].len;
            int nx = (end < len) ? end + 1 : end;   /* the implicit space */
            int cst = 1 + cost[nx];

            if (cst < best || (cst == best && (!best_struct || c[k].len > used[i]))) {
                best = cst;
                next[i] = nx;
                used[i] = c[k].len;
                pay[i] = c[k].payload;
                best_struct = true;
            }
        }
        cost[i] = best;
    }

    for (int i = 0; i < len && n < JTTY_MAX_FRAMES; i = next[i]) {
        out[n++] = pay[i];
    }
    out[n - 1] |= JTTY_EOM_BIT;

    if (!renders_as(out, n, s)) {
        n = pack_text5_only(s, out);
    }
    return n;
}

/* ---- Native function keys ---------------------------------------------------- */

static const char *const MACROS[JTTY_MACROS] = {
    "CQ %M CQ",         /* F1  CQ(my) */
    "%H %E",            /* F2  CALL(his), exchange */
    "%H TU CQ %M CQ",   /* F3  CALL_TU(his), CQ(my) */
    "%M",               /* F4  CALL(my) */
    "%H",               /* F5  CALL(his) */
    "TU NOW %Q %E",     /* F6  TU_NOW_CALL(next), exchange */
    "%H AGN?",          /* F7  CALL_AGN(his) */
    "%E",               /* F8  exchange */
};

const char *jtty_macro_template(int key)
{
    return (key >= 1 && key <= JTTY_MACROS) ? MACROS[key - 1] : "";
}

/* EXCH_NUM, full role ("599 "), kind SERIAL */
static jtty_payload_t serial_atom(uint32_t serial)
{
    return struct30((1u << 26) | (0u << 22) | (serial << 5), 0);
}

int jtty_pack_macro(int key, const char *my_call, const char *his_call, uint32_t serial,
                    jtty_payload_t out[JTTY_MAX_FRAMES], char *norm, size_t norm_size)
{
    char     my[16];
    char     his[16];
    uint32_t m28 = 0;
    uint32_t h28 = 0;
    bool     my_ok;
    bool     his_ok;
    int      n = 0;
    char     msg[JTTY_MSG_MAX + 1] = "";

    if (norm != NULL && norm_size > 0) {
        norm[0] = '\0';
    }
    jtty_pack_normalize(my_call ? my_call : "", my, sizeof(my));
    jtty_pack_normalize(his_call ? his_call : "", his, sizeof(his));
    my_ok = my[0] && strchr(my, ' ') == NULL && std_call(my, &m28);
    his_ok = his[0] && strchr(his, ' ') == NULL && std_call(his, &h28);

    switch (key) {
        case 1:
            if (!my_ok) return 0;
            out[n++] = call_atom(m28, 0, 0);
            break;
        case 2:
            if (!his_ok || serial > JTTY_SERIAL_MAX) return 0;
            out[n++] = call_atom(h28, 0, 1);
            out[n++] = serial_atom(serial);
            break;
        case 3:
            if (!his_ok || !my_ok) return 0;
            out[n++] = call_atom(h28, 0, 3);
            out[n++] = call_atom(m28, 0, 0);
            break;
        case 4:
            if (!my_ok) return 0;
            out[n++] = call_atom(m28, 0, 1);
            break;
        case 5:
            if (!his_ok) return 0;
            out[n++] = call_atom(h28, 0, 1);
            break;
        case 6:
            if (!his_ok || serial > JTTY_SERIAL_MAX) return 0;
            out[n++] = call_atom(h28, 1, 1);
            out[n++] = serial_atom(serial);
            break;
        case 7:
            if (!his_ok) return 0;
            out[n++] = call_atom(h28, 1, 0);
            break;
        case 8:
            if (serial > JTTY_SERIAL_MAX) return 0;
            out[n++] = serial_atom(serial);
            break;
        default:
            return 0;
    }
    out[n - 1] |= JTTY_EOM_BIT;

    for (int i = 0; i < n; i++) {
        jtty_atom_t atom;

        if (!jtty_source_decode(out[i], &atom)) {
            return 0;
        }
        jtty_source_append(msg, &atom);
    }
    if (norm != NULL && norm_size > 0) {
        snprintf(norm, norm_size, "%s", msg);
    }
    return n;
}
