/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4 contest and DXpedition messages
 *
 *  Field layouts, tables and text as WSJT-X's lib/77bit/packjt77.f90
 *  (unpack77), GPL-3.0, by Joe Taylor K1JT and the WSJT team.
 */

#include "contest_msg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ARRL/RAC sections (Field Day), 1-based in the message */
static const char *const sections[] = {
    "AB",  "AK",  "AL",  "AR",  "AZ",  "BC",  "CO",  "CT",  "DE",  "EB",  "EMA", "ENY", "EPA", "EWA", "GA",
    "GH",  "IA",  "ID",  "IL",  "IN",  "KS",  "KY",  "LA",  "LAX", "NS",  "MB",  "MDC", "ME",  "MI",  "MN",
    "MO",  "MS",  "MT",  "NC",  "ND",  "NE",  "NFL", "NH",  "NL",  "NLI", "NM",  "NNJ", "NNY", "TER", "NTX",
    "NV",  "OH",  "OK",  "ONE", "ONN", "ONS", "OR",  "ORG", "PAC", "PR",  "QC",  "RI",  "SB",  "SC",  "SCV",
    "SD",  "SDG", "SF",  "SFL", "SJV", "SK",  "SNJ", "STX", "SV",  "TN",  "UT",  "VA",  "VI",  "VT",  "WCF",
    "WI",  "WMA", "WNY", "WPA", "WTX", "WV",  "WWA", "WY",  "DX",  "PE",  "NB",
};

/* States, provinces and the rest (RTTY Roundup), 1-based; X01-X99 after these */
static const char *const mults[] = {
    "AL", "AK", "AZ", "AR", "CA", "CO",  "CT", "DE", "FL", "GA", "HI", "ID", "IL", "IN",  "IA", "KS", "KY", "LA",
    "ME", "MD", "MA", "MI", "MN", "MS",  "MO", "MT", "NE", "NV", "NH", "NJ", "NM", "NY",  "NC", "ND", "OH", "OK",
    "OR", "PA", "RI", "SC", "SD", "TN",  "TX", "UT", "VT", "VA", "WA", "WV", "WI", "WY",  "NB", "NS", "QC", "ON",
    "MB", "SK", "AB", "BC", "NWT", "NF", "LB", "NU", "YT", "PEI", "DC", "DR", "FR", "GD", "GR", "OV", "ZH", "ZL",
};
#define N_MULTS ((int)(sizeof(mults) / sizeof(mults[0])))
#define N_MULTS_ALL (N_MULTS + 99)

/* Bits [start, start+n) of the 77, the first one the MSB of payload[0]. */
static uint32_t bits(const uint8_t *p, int start, int n) {
    uint32_t v = 0;
    for (int i = start; i < start + n; i++) v = (v << 1) | ((p[i / 8] >> (7 - i % 8)) & 1u);
    return v;
}

static void put_bits(uint8_t *p, int start, int n, uint32_t v) {
    for (int i = start + n - 1; i >= start; i--, v >>= 1) {
        if (v & 1u) p[i / 8] |= (uint8_t)(0x80u >> (i % 8));
        else p[i / 8] &= (uint8_t)~(0x80u >> (i % 8));
    }
}

/* Two 28-bit callsign fields, through ft8_lib: a standard (type 1)
 * message carrying them, with no suffix flags and grid AA00. */
static bool two_calls(uint32_t n28a, uint32_t n28b, ftx_callsign_hash_interface_t *hash_if, char *a, char *b) {
    ftx_message_t std;
    char          extra[16];
    ftx_message_init(&std);
    put_bits(std.payload, 0, 28, n28a);
    put_bits(std.payload, 29, 28, n28b);
    put_bits(std.payload, 74, 3, 1);
    return ftx_message_decode_std(&std, hash_if, a, b, extra) == FTX_MESSAGE_RC_OK && a[0] && b[0];
}

/* A hashed call: "<K1ABC>", or "<...>" not heard yet. */
static void hashed(ftx_callsign_hash_interface_t *hash_if, ftx_callsign_hash_type_t type, uint32_t h, char *out,
                   size_t len) {
    char call[16] = "";
    if (hash_if && hash_if->lookup_hash && hash_if->lookup_hash(type, h, call) && call[0])
        snprintf(out, len, "<%s>", call);
    else snprintf(out, len, "<...>");
}

bool ftx_contest_decode(const ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, char *text, size_t len) {
    const uint8_t *p  = msg->payload;
    uint32_t       i3 = bits(p, 74, 3), n3 = bits(p, 71, 3);
    char           c1[16], c2[16], c3[20];

    if (i3 == 0 && n3 == 1) { /* DXpedition: n28 n28 h10 r5 */
        uint32_t n28a = bits(p, 0, 28), n28b = bits(p, 28, 28);
        if (n28a <= 2 || n28b <= 2 || !two_calls(n28a, n28b, hash_if, c1, c2)) return false;
        hashed(hash_if, FTX_CALLSIGN_HASH_10_BITS, bits(p, 56, 10), c3, sizeof(c3));
        snprintf(text, len, "%s RR73; %s %s %+03d", c1, c2, c3, 2 * (int)bits(p, 66, 5) - 30);
        return true;
    }
    if (i3 == 0 && (n3 == 3 || n3 == 4)) { /* Field Day: n28 n28 R1 n4 k3 s7 */
        uint32_t n28a = bits(p, 0, 28), n28b = bits(p, 28, 28), sec = bits(p, 64, 7);
        if (n28a <= 2 || n28b <= 2 || sec < 1 || sec > sizeof(sections) / sizeof(sections[0])) return false;
        if (!two_calls(n28a, n28b, hash_if, c1, c2)) return false;
        int ntx = (int)bits(p, 57, 4) + 1 + (n3 == 4 ? 16 : 0);
        snprintf(text, len, "%s %s %s%d%c %s", c1, c2, bits(p, 56, 1) ? "R " : "", ntx, 'A' + (int)bits(p, 61, 3),
                 sections[sec - 1]);
        return true;
    }
    if (i3 == 3) { /* RTTY Roundup: TU1 n28 n28 R1 r3 s13 */
        uint32_t nexch = bits(p, 61, 13);
        if (!two_calls(bits(p, 1, 28), bits(p, 29, 28), hash_if, c1, c2)) return false;
        char exch[8];
        if (nexch > 8000) {
            int m = (int)nexch - 8000;
            if (m < 1 || m > N_MULTS_ALL) return false;
            if (m <= N_MULTS) snprintf(exch, sizeof(exch), "%s", mults[m - 1]);
            else snprintf(exch, sizeof(exch), "X%02d", m - N_MULTS);
        } else if (nexch >= 1 && nexch <= 7999) {
            snprintf(exch, sizeof(exch), "%04u", (unsigned)nexch);
        } else {
            return false;
        }
        snprintf(text, len, "%s%s %s %s5%u9 %s", bits(p, 0, 1) ? "TU; " : "", c1, c2, bits(p, 57, 1) ? "R " : "",
                 (unsigned)bits(p, 58, 3) + 2, exch);
        return true;
    }
    if (i3 == 5) { /* EU VHF: h12 h22 R1 r3 s11 g25 */
        uint32_t g = bits(p, 49, 25);
        if (g > 18662399) return false;
        char grid[7];
        grid[0] = (char)('A' + g / (18 * 10 * 10 * 24 * 24));
        g %= 18 * 10 * 10 * 24 * 24;
        grid[1] = (char)('A' + g / (10 * 10 * 24 * 24));
        g %= 10 * 10 * 24 * 24;
        grid[2] = (char)('0' + g / (10 * 24 * 24));
        g %= 10 * 24 * 24;
        grid[3] = (char)('0' + g / (24 * 24));
        g %= 24 * 24;
        grid[4] = (char)('A' + g / 24);
        grid[5] = (char)('A' + g % 24);
        grid[6] = '\0';
        if (grid[1] > 'R') return false;
        hashed(hash_if, FTX_CALLSIGN_HASH_12_BITS, bits(p, 0, 12), c1, sizeof(c1));
        hashed(hash_if, FTX_CALLSIGN_HASH_22_BITS, bits(p, 12, 22), c3, sizeof(c3));
        snprintf(text, len, "%s %s %s%u%04u %s", c1, c3, bits(p, 34, 1) ? "R " : "", 52 + (unsigned)bits(p, 35, 3),
                 (unsigned)bits(p, 38, 11), grid);
        return true;
    }
    return false;
}
