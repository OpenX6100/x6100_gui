/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - DSC call interpretation (ITU-R M.493 / M.541)
 *
 * dsc_format_parse() turns the merged characters of a call (as produced by
 * dsc_decoder) into texts ready for the UI.  ASCII only (the GUI fonts
 * have no other glyphs).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSC_KIND_UNKNOWN = 0,
    DSC_KIND_SHIP,
    DSC_KIND_COAST,
    DSC_KIND_GROUP,
    DSC_KIND_ALL,       /* all ships */
    DSC_KIND_AREA,      /* geographic area */
    DSC_KIND_SAR,       /* SAR aircraft */
    DSC_KIND_BEACON,    /* AIS-SART / MOB / EPIRB */
} dsc_kind_t;

typedef enum {
    DSC_CAT_UNKNOWN = 0,
    DSC_CAT_ROUTINE,
    DSC_CAT_SAFETY,
    DSC_CAT_URGENCY,
    DSC_CAT_DISTRESS,
    DSC_CAT_SELFTEST,   /* not an M.493 category: call to its own MMSI (loop test) */
} dsc_cat_t;

typedef enum { DSC_HL_NONE = 0, DSC_HL_OK, DSC_HL_BAD } dsc_hl_t;

#define DSC_FIELDS_MAX  16
#define DSC_KEY_LEN     24
#define DSC_VAL_LEN     48

typedef struct {
    char key[DSC_KEY_LEN];
    char val[DSC_VAL_LEN];
    dsc_hl_t hl;
} dsc_field_t;

typedef struct {
    int         fmt;                /* format specifier code, -1 unknown */
    dsc_cat_t   cat;
    char        cat_name[12];       /* "DISTRESS", "SAFETY" ... */
    char        fmt_name[28];       /* "INDIVIDUAL", "ALL SHIPS" ... */

    dsc_kind_t  from_kind, to_kind;
    char        from_id[16], to_id[24];     /* MMSI (with '?') or text */
    char        from_where[32], to_where[32];   /* "Greece - coast" */

    char        line2[128];          /* what the call says, one line */
    char        tc1_name[32];       /* telecommand 1 / nature of distress */
    int         eos;                /* 117, 122, 127 or -1 */
    bool        ecc_ok;
    int         ecc_rx, ecc_calc;   /* -1 unknown */

    int         nfields;
    dsc_field_t fields[DSC_FIELDS_MAX];

    /* structured content, for dsc_explain() (-1 / "" when absent) */
    int         tc1, tc2;
    int         nature;             /* nature of distress */
    int         subseq;             /* subsequent communication (distress) */
    char        rx_freq[40], tx_freq[40];
    char        pos[40], pos_time[24];
    char        dis_id[16], dis_where[32];  /* ship in distress (relay/ack) */
    char        number[24];
    bool        self_test;          /* addressed to its own MMSI (cat = DSC_CAT_SELFTEST) */
    bool        self_cancel;        /* distress ack of its own alert = false alert cancelled */         /* telephone number (message 3), "" if none */
} dsc_info_t;

void dsc_format_parse(const int *sym, int n, dsc_info_t *out);

/* One or two plain sentences that say what the call means.  Names (from
 * the databases) may be NULL: a known name replaces the MMSI.  rx_hz is the
 * DSC channel the call was received on (0 if unknown), used for the
 * associated voice channel. */
void dsc_explain(const dsc_info_t *in, const char *from_name, const char *to_name,
                 const char *dis_name, long rx_hz, char *dst, size_t sz);

/* Country of a MID (201..775), NULL if unknown. */
const char *dsc_mid_country(int mid);

/* ISO 3166 alpha-2 code ("it", "gr") of a MID, for its flag; NULL if unknown. */
const char *dsc_mid_iso(int mid);

/* Name of a coast station from the built-in ITU table, NULL if unknown. */
const char *dsc_coast_name(const char *mmsi);

/* Kind and MID of a 9-digit MMSI string ('?' allowed). */
dsc_kind_t dsc_mmsi_kind(const char *mmsi, int *mid);

#ifdef __cplusplus
}
#endif
