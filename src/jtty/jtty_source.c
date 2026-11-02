/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY source grammar: validation and rendering
 *  of one 34-bit atom
 *
 *  Written from the JTTY source-encoding description (WSJT-X 3.2+,
 *  lib/jtty/jtty_source_encoding.txt).
 */

#include "jtty_source.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- 28-bit standard callsign -------------------------------------------
 *
 * The 77-bit message format's encoding: below NTOKENS are special tokens
 * (DE, QRZ, CQ ...), then MAX22 hashed calls, then standard calls of up
 * to six characters with the call area digit in the third position. */
#define NTOKENS     2063592u
#define MAX22       4194304u

static const char A1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char A2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char A3[] = "0123456789";
static const char A4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int idx(const char *alphabet, char c)
{
    const char *p = (c != '\0') ? strchr(alphabet, c) : NULL;

    return (p != NULL) ? (int)(p - alphabet) : -1;
}

bool jtty_call28_pack(const char *call, uint32_t *n28)
{
    char   c6[7] = "      ";
    char   tmp[16];
    size_t len = strlen(call);
    int    i[6];

    if (len < 3 || len > 6 + 1) {           /* 3DA0xxx is 7 long */
        return false;
    }
    snprintf(tmp, sizeof(tmp), "%s", call);

    /* Swaps of the standard encoding: 3DA0 -> 3D0, 3X<letter> -> Q */
    if (strncmp(tmp, "3DA0", 4) == 0 && len > 4) {
        memmove(tmp + 2, tmp + 3, strlen(tmp + 3) + 1);
    } else if (strncmp(tmp, "3X", 2) == 0 && isupper((unsigned char)tmp[2])) {
        memmove(tmp, tmp + 1, strlen(tmp + 1) + 1);
        tmp[0] = 'Q';
    }
    len = strlen(tmp);
    if (len > 6) {
        return false;
    }

    if (len >= 3 && isdigit((unsigned char)tmp[2])) {
        memcpy(c6, tmp, len);
    } else if (len >= 2 && isdigit((unsigned char)tmp[1]) && len <= 5) {
        memcpy(c6 + 1, tmp, len);
    } else {
        return false;
    }

    i[0] = idx(A1, c6[0]);
    i[1] = idx(A2, c6[1]);
    i[2] = idx(A3, c6[2]);
    i[3] = idx(A4, c6[3]);
    i[4] = idx(A4, c6[4]);
    i[5] = idx(A4, c6[5]);
    for (int k = 0; k < 6; k++) {
        if (i[k] < 0) {
            return false;
        }
    }
    *n28 = NTOKENS + MAX22 +
           (uint32_t)(((((i[0] * 36 + i[1]) * 10 + i[2]) * 27 + i[3]) * 27 + i[4]) * 27 + i[5]);
    return true;
}

bool jtty_call_standard(const char *call)
{
    size_t   n = strlen(call);
    size_t   d;
    size_t   letters = 0;
    uint32_t n28;
    char     back[16];

    if (n < 3 || n > 6 || call[0] == 'Q') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isupper((unsigned char)call[i]) && !isdigit((unsigned char)call[i])) {
            return false;
        }
    }
    if (!isupper((unsigned char)call[0]) && !isupper((unsigned char)call[1])) {
        return false;
    }
    if (isdigit((unsigned char)call[2])) {
        d = 2;
    } else if (isdigit((unsigned char)call[1])) {
        d = 1;
    } else {
        return false;
    }
    for (size_t i = d + 1; i < n; i++) {
        if (!isupper((unsigned char)call[i])) {
            return false;
        }
        letters++;
    }
    if (letters < 1 || letters > 3) {
        return false;
    }
    return jtty_call28_pack(call, &n28) && jtty_call28_unpack(n28, back, sizeof(back)) &&
           strcmp(back, call) == 0;
}

bool jtty_call28_unpack(uint32_t n28, char *call, size_t size)
{
    char     c6[7];
    char     out[12];
    uint32_t n;
    uint32_t check;
    char    *s;
    size_t   len;

    if (n28 < NTOKENS + MAX22) {
        return false;                       /* token or hashed call */
    }
    n = n28 - NTOKENS - MAX22;
    c6[5] = A4[n % 27]; n /= 27;
    c6[4] = A4[n % 27]; n /= 27;
    c6[3] = A4[n % 27]; n /= 27;
    c6[2] = A3[n % 10]; n /= 10;
    c6[1] = A2[n % 36]; n /= 36;
    if (n >= 37) {
        return false;
    }
    c6[0] = A1[n];
    c6[6] = '\0';

    s = c6;
    while (*s == ' ') {
        s++;
    }
    len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') {
        s[--len] = '\0';
    }
    if (len < 3 || strchr(s, ' ') != NULL) {
        return false;
    }

    if (strncmp(s, "3D0", 3) == 0 && len > 3) {
        snprintf(out, sizeof(out), "3DA0%s", s + 3);
    } else if (s[0] == 'Q' && isupper((unsigned char)s[1])) {
        snprintf(out, sizeof(out), "3X%s", s + 1);
    } else {
        snprintf(out, sizeof(out), "%s", s);
    }

    /* Only spellings that come back to the same code are valid. */
    if (!jtty_call28_pack(out, &check) || check != n28) {
        return false;
    }
    snprintf(call, size, "%s", out);
    return true;
}

/* ---- Tables ---------------------------------------------------------- */

static const char TEXT5_ALPHABET[64] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;";

static const char *const CONTROL_PHRASES[] = {
    "AGN?", "CALL?", "AGN CALL", "NR?", "AGN NR", "EXCH?", "STATE?",
    "SECTION?", "ZONE?", "GRID?", "RPRT?", "QSL TU", "TU", "QRZ?",
    "QSO B4", "WAIT", "NIL?", "OK?",
};
#define N_CONTROL (sizeof(CONTROL_PHRASES) / sizeof(CONTROL_PHRASES[0]))

/* ARRL/RAC sections, 1-based on the wire (the 77-bit format's table). */
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
#define N_SECTIONS (sizeof(SECTIONS) / sizeof(SECTIONS[0]))

/* ---- STRUCT30 -------------------------------------------------------- */

/* Takes `n` bits off the top of a 27-bit body read from bit 1 on. */
typedef struct {
    uint32_t body;
    int      left;
} fields_t;

static uint32_t take(fields_t *f, int n)
{
    f->left -= n;
    return (f->body >> f->left) & ((1u << n) - 1u);
}

/* Base-36 token: exactly two characters (length 0) or three without a
 * leading zero (length 1). */
static bool token36(uint32_t length, uint32_t token, char *out)
{
    if (length == 0) {
        if (token >= 36u * 36u) {
            return false;
        }
        out[0] = A2[token / 36];
        out[1] = A2[token % 36];
        out[2] = '\0';
    } else {
        if (token < 36u * 36u || token >= 36u * 36u * 36u) {
            return false;
        }
        out[0] = A2[token / 1296];
        out[1] = A2[(token / 36) % 36];
        out[2] = A2[token % 36];
        out[3] = '\0';
    }
    return true;
}

static bool decode_struct(uint32_t body, unsigned family, char *out, size_t size)
{
    fields_t f = {body, 27};
    char     tok[4];

    switch (family) {
        case 0: {                                   /* EXCH_NUM */
            uint32_t role = take(&f, 1);
            uint32_t kind = take(&f, 4);
            uint32_t v = take(&f, 17);
            const char *pre = role ? "599 " : "";

            if (take(&f, 5) != 0) {
                return false;
            }
            switch (kind) {
                case 0: snprintf(out, size, "%s%03u", pre, v); break;      /* SERIAL */
                case 1:                                                    /* CQ_ZONE */
                    if (v < 1 || v > 40) return false;
                    snprintf(out, size, "%s%02u", pre, v);
                    break;
                case 2:                                                    /* ITU_ZONE */
                    if (v < 1 || v > 90) return false;
                    snprintf(out, size, "%s%02u", pre, v);
                    break;
                case 3:                                                    /* AGE */
                case 4:                                                    /* POWER */
                case 7:                                                    /* GENERIC */
                    snprintf(out, size, "%s%u", pre, v);
                    break;
                case 5: snprintf(out, size, "%s%02u", pre, v); break;      /* CHECK */
                case 6:                                                    /* LICENSE YEAR */
                    if (v > 9999) return false;
                    snprintf(out, size, "%s%04u", pre, v);
                    break;
                default:
                    return false;
            }
            return true;
        }

        case 1: {                                   /* EXCH_LOC */
            uint32_t role = take(&f, 1);
            uint32_t kind = take(&f, 4);
            uint32_t length = take(&f, 1);
            uint32_t token = take(&f, 16);

            if (take(&f, 5) != 0 || kind > 4 || !token36(length, token, tok)) {
                return false;
            }
            snprintf(out, size, "%s%s", role ? "599 " : "", tok);
            return true;
        }

        case 2: {                                   /* EXCH_PAIR */
            uint32_t schema = take(&f, 3);

            if (schema == 0) {                      /* ZONE_LOC3 */
                uint32_t zone = take(&f, 6);
                uint32_t length = take(&f, 1);
                uint32_t token = take(&f, 16);

                if (take(&f, 1) != 0 || zone < 1 || zone > 40 || !token36(length, token, tok)) {
                    return false;
                }
                snprintf(out, size, "599 %02u %s", zone, tok);
                return true;
            }
            if (schema == 1) {                      /* CLASS_SECTION */
                uint32_t count = take(&f, 6);
                uint32_t cls = take(&f, 3);
                uint32_t sec = take(&f, 7);

                if (take(&f, 7) != 0 || take(&f, 1) != 0 ||
                    count < 1 || count > 32 || cls > 5 || sec < 1 || sec > N_SECTIONS) {
                    return false;
                }
                snprintf(out, size, "%u%c %s", count, 'A' + cls, SECTIONS[sec - 1]);
                return true;
            }
            return false;
        }

        case 3: {                                   /* EXCH_NUM_TIME */
            uint32_t role = take(&f, 1);
            uint32_t serial = take(&f, 14);
            uint32_t minute = take(&f, 11);

            if (take(&f, 1) != 0 || minute > 1439) {
                return false;
            }
            snprintf(out, size, "%s%03u %02u%02u", role ? "599 " : "",
                     serial, minute / 60, minute % 60);
            return true;
        }

        case 4: {                                   /* MISC */
            uint32_t subtype = take(&f, 4);

            if (subtype == 0) {                     /* CONTROL */
                uint32_t phrase = take(&f, 7);

                if (take(&f, 16) != 0 || phrase >= N_CONTROL) {
                    return false;
                }
                snprintf(out, size, "%s", CONTROL_PHRASES[phrase]);
                return true;
            }
            if (subtype == 1) {                     /* GRID4 */
                uint32_t role = take(&f, 1);
                uint32_t code = take(&f, 15);

                if (take(&f, 7) != 0 || code > 32399) {
                    return false;
                }
                snprintf(out, size, "%s%c%c%c%c", role ? "599 " : "",
                         'A' + (int)(code / 1800), 'A' + (int)((code / 100) % 18),
                         '0' + (int)((code / 10) % 10), '0' + (int)(code % 10));
                return true;
            }
            return false;
        }

        default:                                    /* 101, 110, 111 */
            return false;
    }
}

/* ---- Atom ------------------------------------------------------------ */

bool jtty_source_decode(jtty_payload_t payload, jtty_atom_t *atom)
{
    uint32_t g = JTTY_GRAMMAR(payload);
    unsigned i2 = g & 3u;

    memset(atom, 0, sizeof(*atom));

    if (payload >> JTTY_PAYLOAD_BITS) {
        return false;
    }
    if ((payload & JTTY_RESERVED_BIT) != 0 || g == 0) {
        return false;
    }
    atom->eom = (payload & JTTY_EOM_BIT) != 0;

    if (i2 == 0 || i2 == 1) {
        unsigned n2 = (g >> 2) & 3u;
        uint32_t n28 = g >> 4;
        const char *fmt;

        if (i2 == 1 && n2 >= 2) {
            return false;                           /* reserved actions */
        }
        if (!jtty_call28_unpack(n28, atom->call, sizeof(atom->call)) ||
            !jtty_call_standard(atom->call)) {
            return false;               /* WSJT-X discards these too */
        }
        if (i2 == 0) {
            static const char *const f0[4] = {"CQ %s CQ", "%s", "TU %s CQ", "%s TU"};
            fmt = f0[n2];
        } else {
            static const char *const f1[2] = {"%s AGN?", "TU NOW %s"};
            fmt = f1[n2];
        }
        snprintf(atom->text, sizeof(atom->text), fmt, atom->call);
        atom->kind = JTTY_ATOM_CALL;
        return true;
    }

    if (i2 == 2) {
        atom->kind = JTTY_ATOM_STRUCT;
        return decode_struct(g >> 5, (g >> 2) & 7u, atom->text, sizeof(atom->text));
    }

    /* i2 == 3: TEXT5, kept with its spaces; padding goes at EOM */
    for (int k = 0; k < 5; k++) {
        atom->text[k] = TEXT5_ALPHABET[(g >> (26 - 6 * k)) & 63u];
    }
    atom->text[5] = '\0';
    atom->kind = JTTY_ATOM_TEXT5;
    return true;
}

void jtty_source_append(char *msg, const jtty_atom_t *atom)
{
    size_t len = strlen(msg);

    if (atom->kind == JTTY_ATOM_TEXT5) {
        snprintf(msg + len, JTTY_MSG_MAX + 1 - len, "%s", atom->text);
    } else {
        snprintf(msg + len, JTTY_MSG_MAX + 1 - len, "%s ", atom->text);
    }
    if (atom->eom) {
        len = strlen(msg);
        while (len > 0 && msg[len - 1] == ' ') {
            msg[--len] = '\0';
        }
    }
}
