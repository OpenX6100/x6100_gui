/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - DSC call interpretation, see dsc_format.h.
 */
#include "dsc_format.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char *fmt_name(int v)
{
    switch (v) {
    case 102: return "GEOGRAPHIC AREA";
    case 112: return "DISTRESS ALERT";
    case 114: return "GROUP";
    case 116: return "ALL SHIPS";
    case 120: return "INDIVIDUAL";
    case 123: return "INDIVIDUAL AUTO";
    default:  return NULL;
    }
}

static dsc_cat_t cat_of(int v, char *name)
{
    switch (v) {
    case 100: strcpy(name, "ROUTINE");  return DSC_CAT_ROUTINE;
    case 108: strcpy(name, "SAFETY");   return DSC_CAT_SAFETY;
    case 110: strcpy(name, "URGENCY");  return DSC_CAT_URGENCY;
    case 112: strcpy(name, "DISTRESS"); return DSC_CAT_DISTRESS;
    default:  strcpy(name, "UNKNOWN");  return DSC_CAT_UNKNOWN;
    }
}

static const char *tc_name(int v)
{
    switch (v) {
    case 100: return "F3E/G3E all modes";
    case 101: return "F3E/G3E duplex";
    case 103: return "Polling";
    case 104: return "Unable to comply";
    case 105: return "End of call";
    case 106: return "Data";
    case 109: return "J3E telephony";
    case 110: return "Distress acknowledgement";
    case 112: return "Distress relay";
    case 113: return "F1B/J2B TTY-FEC";
    case 115: return "F1B/J2B TTY-ARQ";
    case 118: return "TEST";
    case 121: return "Position update";
    case 126: return "no information";
    default:  return NULL;
    }
}

/* telecommand 2 (M.493 table 4): reasons for "unable to comply" and a few
 * qualifiers that can go with other first telecommands */
static const char *unable_reason(int v)
{
    switch (v) {
    case 100: return "no reason given";
    case 101: return "congestion";
    case 102: return "busy";
    case 103: return "queue indication";
    case 104: return "station barred";
    case 105: return "no operator";
    case 106: return "operator unavailable";
    case 107: return "equipment disabled";
    case 108: return "cannot use channel";
    case 109: return "cannot use mode";
    case 110: return "not party to conflict";
    case 111: return "medical transports";
    case 112: return "pay-phone/public call";
    case 113: return "facsimile/data";
    case 126: return "no information";
    default:  return NULL;
    }
}

static const char *nature_name(int v)
{
    switch (v) {
    case 100: return "Fire, explosion";
    case 101: return "Flooding";
    case 102: return "Collision";
    case 103: return "Grounding";
    case 104: return "Listing, capsizing";
    case 105: return "Sinking";
    case 106: return "Disabled and adrift";
    case 107: return "Undesignated distress";
    case 108: return "Abandoning ship";
    case 109: return "Piracy/armed robbery";
    case 110: return "Man overboard";
    case 112: return "EPIRB emission";
    default:  return NULL;
    }
}

static const char *eos_name(int v)
{
    switch (v) {
    case 117: return "Ack required (RQ)";
    case 122: return "Acknowledgement (BQ)";
    case 127: return "End of sequence";
    default:  return NULL;
    }
}

static bool is_eos(int v) { return v == 117 || v == 122 || v == 127; }

/* "code - text" or "code" or "?" */
static void code_text(char *dst, size_t sz, int v, const char *txt)
{
    if (v < 0) snprintf(dst, sz, "?");
    else if (txt) snprintf(dst, sz, "%d - %s", v, txt);
    else snprintf(dst, sz, "%d", v);
}

/* Digits of n characters ("??" for unreadable ones) */
static void digits(const int *s, int n, char *dst)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] >= 0 && s[i] <= 99) { dst[k++] = (char)('0' + s[i] / 10); dst[k++] = (char)('0' + s[i] % 10); }
        else { dst[k++] = '?'; dst[k++] = '?'; }
    }
    dst[k] = '\0';
}

dsc_kind_t dsc_mmsi_kind(const char *m, int *mid)
{
    int dummy;
    if (!mid) mid = &dummy;
    *mid = -1;
    if (!m || strlen(m) < 9) return DSC_KIND_UNKNOWN;
    #define DIG(i) (m[i] >= '0' && m[i] <= '9')
    #define MIDAT(i) ((DIG(i) && DIG(i + 1) && DIG(i + 2)) ? (m[i]-'0')*100 + (m[i+1]-'0')*10 + (m[i+2]-'0') : -1)
    if (m[0] == '0' && m[1] == '0') { *mid = MIDAT(2); return DSC_KIND_COAST; }
    if (m[0] == '0') { *mid = MIDAT(1); return DSC_KIND_GROUP; }
    /* 111 MID xxx: SAR aircraft, only with a real MID (111111111 is a dummy) */
    if (m[0] == '1' && m[1] == '1' && m[2] == '1' && MIDAT(3) > 0 && dsc_mid_country(MIDAT(3))) { *mid = MIDAT(3); return DSC_KIND_SAR; }
    if (m[0] == '9' && m[1] == '7') return DSC_KIND_BEACON;
    if (m[0] == '9' && (m[1] == '8' || m[1] == '9')) { *mid = MIDAT(2); return DSC_KIND_SHIP; }
    if (m[0] == '8') { *mid = MIDAT(1); return DSC_KIND_SHIP; }
    *mid = MIDAT(0);
    return DSC_KIND_SHIP;
    #undef DIG
    #undef MIDAT
}

static const char *kind_word(dsc_kind_t k)
{
    switch (k) {
    case DSC_KIND_SHIP:   return "ship";
    case DSC_KIND_COAST:  return "coast";
    case DSC_KIND_GROUP:  return "group";
    case DSC_KIND_SAR:    return "SAR aircraft";
    case DSC_KIND_BEACON: return "beacon";
    default:              return "";
    }
}

/* MMSI from 5 characters: 10 digits, the last is always 0 */
static void mmsi_of(const int *s, char *id, char *where, dsc_kind_t *kind)
{
    char d[16];
    digits(s, 5, d);
    d[9] = '\0';
    strcpy(id, d);
    int mid;
    *kind = dsc_mmsi_kind(d, &mid);
    const char *c = mid > 0 ? dsc_mid_country(mid) : NULL;
    const char *w = kind_word(*kind);
    if (c) snprintf(where, 32, "%s - %s", c, w);
    else snprintf(where, 32, "%s", w);
}

static void coords(const int *s, char *dst, size_t sz)
{
    for (int i = 0; i < 5; ++i)
        if (s[i] < 0 || s[i] > 99) { snprintf(dst, sz, s[i] == 126 ? "no information" : "?"); return; }
    char d[16];
    digits(s, 5, d);
    if (strcmp(d, "9999999999") == 0) { snprintf(dst, sz, "no information"); return; }
    int q = d[0] - '0';
    int la = (d[1] - '0') * 10 + (d[2] - '0'), lam = (d[3] - '0') * 10 + (d[4] - '0');
    int lo = (d[5] - '0') * 100 + (d[6] - '0') * 10 + (d[7] - '0'), lom = (d[8] - '0') * 10 + (d[9] - '0');
    bool bad = q > 3 || la > 90 || lam > 59 || lo > 180 || lom > 59;
    snprintf(dst, sz, "%.2s %.2s'%c  %.3s %.2s'%c%s",
             d + 1, d + 3, (q == 0 || q == 1) ? 'N' : 'S',
             d + 5, d + 8, (q == 0 || q == 2) ? 'E' : 'W', bad ? " (invalid)" : "");
}

static void utc_of(const int *s, char *dst, size_t sz)
{
    if (s[0] < 0 || s[1] < 0) snprintf(dst, sz, "?");
    else if (s[0] == 88 || s[0] > 23 || s[1] > 59) snprintf(dst, sz, "no information");
    else snprintf(dst, sz, "%02d:%02d UTC", s[0], s[1]);
}

/* Frequency/channel field at s; returns the characters used. */
static int freq_of(const int *s, int n, char *dst, size_t sz)
{
    if (n < 1) { snprintf(dst, sz, "-"); return 0; }
    /* "no information" is 126 three times; a frequency digit pair is
     * never 126, so any 126 in the field means no information, even
     * when the other characters were lost or damaged */
    int k3 = n < 3 ? n : 3, n126 = 0;
    for (int i = 0; i < k3; ++i) if (s[i] == 126) n126++;
    if (n126 > 0) {
        snprintf(dst, sz, n126 == 3 ? "no information" : "no information (partial)");
        return k3;
    }
    if (n < 3) { snprintf(dst, sz, "unreadable"); return n; }
    char d[12];
    digits(s, 3, d);
    if (d[0] >= '0' && d[0] <= '2' && d[1] != '?') {
        if (strchr(d, '?')) {
            /* partly received: keep the readable digits in place, "829?.? kHz" */
            const char *p = d;
            while (*p == '0' && p < d + 4) p++;
            snprintf(dst, sz, "%.*s.%c kHz (partial)", (int)(d + 5 - p), p, d[5]);
            return 3;
        }
        long v = 0;
        if (sscanf(d, "%6ld", &v) == 1) { snprintf(dst, sz, "%ld.%ld kHz", v / 10, v % 10); return 3; }
    }
    if (d[0] == '3' && !strchr(d, '?')) {
        /* MF/HF working channel (RR Appendix 17), e.g. 300805 = channel 805 */
        const char *p = d + 1;
        while (*p == '0' && p[1]) p++;
        snprintf(dst, sz, "channel %s", p);
        return 3;
    }
    if (d[0] == '4' && n >= 4) {
        char d4[12];
        digits(s, 4, d4);
        long v = 0;
        if (strchr(d4, '?')) { snprintf(dst, sz, "unreadable"); return 4; }
        if (sscanf(d4 + 1, "%7ld", &v) == 1) { snprintf(dst, sz, "%ld.%02ld kHz", v / 100, v % 100); return 4; }
    }
    if (d[0] == '9' && !strchr(d, '?')) {
        /* 9 0 m ccc: VHF channel ccc, m = 1 ship / 2 coast frequency simplex */
        int ch = (d[3] - '0') * 100 + (d[4] - '0') * 10 + (d[5] - '0');
        snprintf(dst, sz, "VHF channel %d%s", ch, d[2] == '1' ? " (ship freq.)" : d[2] == '2' ? " (coast freq.)" : "");
        return 3;
    }
    if (d[0] == '5' && d[1] == '5') { snprintf(dst, sz, "position follows"); return 3; }
    if (strchr(d, '?')) snprintf(dst, sz, "unreadable");
    else snprintf(dst, sz, "code %s", d);
    return 3;
}

static void add(dsc_info_t *o, const char *k, const char *v, dsc_hl_t hl)
{
    if (o->nfields >= DSC_FIELDS_MAX) return;
    dsc_field_t *f = &o->fields[o->nfields++];
    snprintf(f->key, sizeof(f->key), "%s", k);
    snprintf(f->val, sizeof(f->val), "%s", v);
    f->hl = hl;
}

/*
 * Same MMSI with unreadable digits ('?'): no digit differs and either 7
 * digits agree, or 6 agree and the two copies together give all 9 digits.
 * (Fewer is not enough: trailing zeros and the MID are shared by many ships.)
 */
static bool same_id(const char *a, const char *b)
{
    if (strlen(a) != 9 || strlen(b) != 9) return false;
    int known = 0, covered = 0;
    for (int i = 0; i < 9; ++i) {
        if (a[i] != '?' || b[i] != '?') covered++;
        if (a[i] == '?' || b[i] == '?') continue;
        if (a[i] != b[i]) return false;
        known++;
    }
    return known >= 7 || (known >= 6 && covered == 9);
}

/* fill the unreadable digits of a and b from each other */
static void merge_id(char *a, char *b)
{
    for (int i = 0; i < 9; ++i) {
        if (a[i] == '?') a[i] = b[i];
        else if (b[i] == '?') b[i] = a[i];
    }
}

static int at(const int *s, int n, int i) { return i >= 0 && i < n ? s[i] : -1; }

static bool is_cat(int v) { return v == 100 || v == 108 || v == 110 || v == 112; }
static bool is_dig(int v) { return v >= 0 && v <= 99; }

/* error check from message start st: format once + st..EOS; true if it matches */
static bool ecc_from(const int *sym, int n, int fmt, int st)
{
    if (fmt < 0) return false;
    int x = fmt;
    for (int i = st; i < n; ++i) {
        if (sym[i] < 0) return false;
        x ^= sym[i];
        if (is_eos(sym[i])) return i + 1 < n && sym[i + 1] == x;
    }
    return false;
}

/* how well the fixed fields fit a message starting at st */
static int fit_score(const int *sym, int n, int fmt, int st)
{
    int sc = 0;
    #define A(i) at(sym, n, st + (i))
    if (fmt == 112) {
        for (int i = 0; i < 5; ++i) sc += is_dig(A(i));
        sc += nature_name(A(5)) ? 3 : 0;
        for (int i = 6; i < 13; ++i) sc += is_dig(A(i));
        sc += tc_name(A(13)) ? 2 : 0;
        sc += is_eos(A(14)) ? 3 : 0;
    } else {
        int c = fmt == 116 ? 0 : 5;
        for (int i = 0; i < c; ++i) sc += is_dig(A(i));
        sc += is_cat(A(c)) ? 3 : 0;
        for (int i = c + 1; i < c + 6; ++i) sc += is_dig(A(i));
        sc += tc_name(A(c + 6)) ? 2 : 0;
        sc += (A(c + 7) == 126 || unable_reason(A(c + 7))) ? 1 : 0;
    }
    #undef A
    return sc;
}

/*
 * The format specifier is sent twice, but the decoder may deliver only the
 * second copy (the message then starts at index 1).  Take the alignment
 * whose error check is right, otherwise the one whose fields fit better.
 */
static int msg_start(const int *sym, int n, int fmt)
{
    if (n > 1 && sym[1] == fmt) return 2;
    if (ecc_from(sym, n, fmt, 2)) return 2;
    if (ecc_from(sym, n, fmt, 1)) return 1;
    return fit_score(sym, n, fmt, 1) > fit_score(sym, n, fmt, 2) ? 1 : 2;
}

void dsc_format_parse(const int *sym, int n, dsc_info_t *o)
{
    char buf[64], b2[48];
    memset(o, 0, sizeof(*o));
    o->fmt = -1; o->eos = -1; o->ecc_rx = o->ecc_calc = -1;
    o->tc1 = o->tc2 = o->nature = o->subseq = -1;
    strcpy(o->cat_name, "UNKNOWN");
    strcpy(o->from_id, "?"); strcpy(o->to_id, "?");
    if (n < 2) return;

    /* a wrong start of the decoder can leave phasing characters (125,
     * 104..111) in front of the format specifiers: restart after them */
    for (int i = 13; i >= 1; --i) {
        if (i + 1 < n && fmt_name(sym[i]) && sym[i + 1] == sym[i] &&
            (sym[i - 1] == 125 || (sym[i - 1] >= 104 && sym[i - 1] <= 111))) {
            sym += i; n -= i;
            break;
        }
    }

    /* the format specifier is always sent twice (M.493): take a valid one;
     * when the two copies are different valid formats, the error check decides */
    int fmt = fmt_name(sym[0]) ? sym[0] : fmt_name(sym[1]) ? sym[1] : (sym[0] >= 0 ? sym[0] : sym[1]);
    if (fmt_name(sym[0]) && fmt_name(sym[1]) && sym[0] != sym[1] &&
        !ecc_from(sym, n, sym[0], 2) && ecc_from(sym, n, sym[1], 2))
        fmt = sym[1];
    int j = msg_start(sym, n, fmt);
    int st = j;
    o->fmt = fmt;
    snprintf(o->fmt_name, sizeof(o->fmt_name), "%s", fmt_name(fmt) ? fmt_name(fmt) : "UNKNOWN FORMAT");
    code_text(buf, sizeof(buf), fmt, fmt_name(fmt));
    add(o, "Format", buf, DSC_HL_NONE);

    int tc1 = -1;
    if (fmt == 112) {
        o->cat = DSC_CAT_DISTRESS; strcpy(o->cat_name, "DISTRESS");
        o->to_kind = DSC_KIND_ALL; strcpy(o->to_id, "ALL STATIONS");
        if (j + 5 <= n) mmsi_of(sym + j, o->from_id, o->from_where, &o->from_kind);
        j += 5;
        int nat = at(sym, n, j++);
        o->nature = nat;
        snprintf(o->dis_id, sizeof(o->dis_id), "%s", o->from_id);
        snprintf(o->dis_where, sizeof(o->dis_where), "%s", o->from_where);
        code_text(buf, sizeof(buf), nat, nature_name(nat));
        add(o, "Nature", buf, DSC_HL_BAD);
        snprintf(o->tc1_name, sizeof(o->tc1_name), "%s", nature_name(nat) ? nature_name(nat) : "distress");
        if (j + 5 <= n) coords(sym + j, b2, sizeof(b2)); else strcpy(b2, "?");
        add(o, "Position", b2, DSC_HL_NONE); j += 5;
        snprintf(o->pos, sizeof(o->pos), "%.39s", b2);
        if (j + 2 <= n) utc_of(sym + j, buf, sizeof(buf)); else strcpy(buf, "?");
        add(o, "Position time", buf, DSC_HL_NONE); j += 2;
        snprintf(o->pos_time, sizeof(o->pos_time), "%.23s", buf);
        int sc = at(sym, n, j++);
        o->subseq = sc;
        code_text(buf, sizeof(buf), sc, tc_name(sc));
        add(o, "Subsequent comm.", buf, DSC_HL_NONE);
        snprintf(o->line2, sizeof(o->line2), "%s - %s - next: %s",
                 o->tc1_name, b2, tc_name(sc) ? tc_name(sc) : "?");
    } else {
        if (fmt == 116) {
            o->to_kind = DSC_KIND_ALL; strcpy(o->to_id, "ALL SHIPS");
        } else if (fmt == 102) {
            o->to_kind = DSC_KIND_AREA;
            char d[16];
            if (j + 5 <= n) digits(sym + j, 5, d); else strcpy(d, "??????????");
            int q = d[0] - '0';
            snprintf(o->to_id, sizeof(o->to_id), "AREA %.2s%c %.3s%c", d + 1,
                     (q == 0 || q == 1) ? 'N' : 'S', d + 3, (q == 0 || q == 2) ? 'E' : 'W');
            snprintf(o->to_where, sizeof(o->to_where), "%.2s x %.2s deg", d + 6, d + 8);
            j += 5;
        } else {
            if (j + 5 <= n) mmsi_of(sym + j, o->to_id, o->to_where, &o->to_kind);
            /* format 114: the address is a group call, whatever its digits look like */
            if (fmt == 114 && o->to_kind != DSC_KIND_UNKNOWN) {
                o->to_kind = DSC_KIND_GROUP;
                snprintf(o->to_where, sizeof(o->to_where), "group");
            }
            j += 5;
        }
        int cat = at(sym, n, j++);
        o->cat = cat_of(cat, o->cat_name);
        code_text(buf, sizeof(buf), cat, cat >= 0 ? o->cat_name : NULL);
        add(o, "Category", buf, o->cat == DSC_CAT_DISTRESS ? DSC_HL_BAD : DSC_HL_NONE);
        if (j + 5 <= n) mmsi_of(sym + j, o->from_id, o->from_where, &o->from_kind);
        j += 5;
        tc1 = at(sym, n, j++);
        o->tc1 = tc1;
        code_text(buf, sizeof(buf), tc1, tc_name(tc1));
        add(o, "Telecommand 1", buf, DSC_HL_NONE);
        snprintf(o->tc1_name, sizeof(o->tc1_name), "%s", tc_name(tc1) ? tc_name(tc1) : "?");

        if (o->cat == DSC_CAT_DISTRESS && (tc1 == 110 || tc1 == 112)) {
            char id[16], wh[32]; dsc_kind_t k;
            if (j + 5 <= n && sym[j] == 126 && sym[j + 1] == 126 && sym[j + 2] == 126 &&
                sym[j + 3] == 126 && sym[j + 4] == 126) {
                /* 126 x 5: the ship in distress is not identified */
                snprintf(buf, sizeof(buf), "not identified");
                snprintf(o->dis_id, sizeof(o->dis_id), "unknown");
                o->dis_where[0] = '\0';
            } else if (j + 5 <= n) {
                mmsi_of(sym + j, id, wh, &k); snprintf(buf, sizeof(buf), "%s (%s)", id, wh);
                snprintf(o->dis_id, sizeof(o->dis_id), "%s", id);
                snprintf(o->dis_where, sizeof(o->dis_where), "%s", wh);
            } else strcpy(buf, "?");
            add(o, "Ship in distress", buf, DSC_HL_BAD); j += 5;
            int nat = at(sym, n, j++);
            o->nature = nat;
            code_text(buf, sizeof(buf), nat, nature_name(nat));
            add(o, "Nature", buf, DSC_HL_BAD);
            if (j + 5 <= n) coords(sym + j, b2, sizeof(b2)); else strcpy(b2, "?");
            add(o, "Position", b2, DSC_HL_NONE); j += 5;
            snprintf(o->pos, sizeof(o->pos), "%.39s", b2);
            if (j + 2 <= n) utc_of(sym + j, buf, sizeof(buf)); else strcpy(buf, "?");
            add(o, "Position time", buf, DSC_HL_NONE); j += 2;
            snprintf(o->pos_time, sizeof(o->pos_time), "%.23s", buf);
            int sc = at(sym, n, j++);
            o->subseq = sc;
            code_text(buf, sizeof(buf), sc, tc_name(sc));
            add(o, "Subsequent comm.", buf, DSC_HL_NONE);
            snprintf(o->line2, sizeof(o->line2), "%s - %s - %s",
                     tc_name(tc1), nature_name(at(sym, n, j - 9)) ? nature_name(at(sym, n, j - 9)) : "?", b2);
            /* own MMSI as the ship in distress: cancellation of a false alert */
            if (tc1 == 110 && strchr(o->dis_id, '?') == NULL && !strcmp(o->dis_id, o->from_id)) {
                o->self_cancel = true;
                snprintf(o->line2, sizeof(o->line2), "Distress alert CANCELLED (false alert)");
                add(o, "Meaning", "self-cancel of a false alert", DSC_HL_OK);
            }
        } else {
            int tc2 = at(sym, n, j++);
            o->tc2 = tc2;
            /* telecommand 2 has its own table, whatever telecommand 1 is */
            const char *t2 = unable_reason(tc2);
            code_text(buf, sizeof(buf), tc2, t2);
            add(o, tc1 == 104 ? "Reason" : "Telecommand 2", buf, DSC_HL_NONE);
            /* frequency (or position) fields up to the end of sequence */
            int k = j;
            while (k < n && !is_eos(sym[k])) k++;
            char rxf[40] = "-", txf[40] = "-";
            if (k > j && sym[j] == 55) {
                /* ITU-R M.493: symbol 55 in place of the frequencies = the
                 * message is the ship position (quadrant + 10 digits in 5
                 * symbols), optionally followed by its UTC time */
                if (k - j >= 6) coords(sym + j + 1, b2, sizeof(b2));
                else strcpy(b2, "?");
                add(o, "Position", b2, DSC_HL_NONE);
                snprintf(o->pos, sizeof(o->pos), "%.39s", b2);
                if (k - j >= 8) {
                    utc_of(sym + j + 6, buf, sizeof(buf));
                    add(o, "Position time", buf, DSC_HL_NONE);
                    snprintf(o->pos_time, sizeof(o->pos_time), "%.23s", buf);
                }
                j = k;
                snprintf(o->rx_freq, sizeof(o->rx_freq), "-");
                snprintf(o->tx_freq, sizeof(o->tx_freq), "-");
                snprintf(o->line2, sizeof(o->line2), "%s - %s%s%s", o->fmt_name, o->tc1_name,
                         tc1 == 104 && t2 ? ": " : "", tc1 == 104 && t2 ? t2 : "");
                size_t L = strlen(o->line2);
                snprintf(o->line2 + L, sizeof(o->line2) - L, " - position %s", o->pos);
                goto eos;
            }
            int used = freq_of(sym + j, k - j, rxf, sizeof(rxf));
            used += freq_of(sym + j + used, k - j - used, txf, sizeof(txf));
            /* message 3: telephone number, 105 (odd digits) / 106 (even) */
            int t = j + used;
            if (t < k && (sym[t] == 105 || sym[t] == 106) && k - t - 1 <= 8) {
                char d[24];
                digits(sym + t + 1, k - t - 1, d);
                snprintf(o->number, sizeof(o->number), "%s", sym[t] == 105 ? d + 1 : d);
            }
            if (k > j) {
                add(o, "RX frequency", rxf, DSC_HL_NONE);
                add(o, "TX frequency", txf, DSC_HL_NONE);
                if (o->number[0]) add(o, "Telephone number", o->number, DSC_HL_NONE);
            }
            j = k;
            snprintf(o->rx_freq, sizeof(o->rx_freq), "%s", rxf);
            snprintf(o->tx_freq, sizeof(o->tx_freq), "%s", txf);
            char fq[56] = "no frequency";
            bool rx_bad = !strcmp(rxf, "unreadable"), tx_bad = !strcmp(txf, "unreadable");
            bool rx_none = rx_bad || !strncmp(rxf, "no information", 14) || !strcmp(rxf, "-");
            bool tx_none = tx_bad || !strncmp(txf, "no information", 14) || !strcmp(txf, "-");
            if (rx_none && tx_none) snprintf(fq, sizeof(fq), (rx_bad || tx_bad) ? "frequency unreadable" : "no frequency");
            else if (rx_none) snprintf(fq, sizeof(fq), "TX %.40s", txf);
            else if (tx_none) snprintf(fq, sizeof(fq), "RX %.40s", rxf);
            else {
                if (!strcmp(rxf, txf)) snprintf(fq, sizeof(fq), "RX/TX %.40s", rxf);
                else snprintf(fq, sizeof(fq), "RX %.24s TX %.24s", rxf, txf);
            }
            snprintf(o->line2, sizeof(o->line2), "%s - %s%s%s", o->fmt_name, o->tc1_name,
                     tc1 == 104 && t2 ? ": " : "", tc1 == 104 && t2 ? t2 : "");
            size_t L = strlen(o->line2);
            snprintf(o->line2 + L, sizeof(o->line2) - L, " - %s", fq);
        }
    }

eos:
    /* addressed to its own MMSI: a loop test of the station's own DSC
     * equipment.  Shown as a category of its own ("SELF TEST"), also when
     * the category character was lost; distress and urgency stay as they are. */
    if ((fmt == 120 || fmt == 123) && o->cat != DSC_CAT_DISTRESS && o->cat != DSC_CAT_URGENCY &&
        same_id(o->from_id, o->to_id)) {
        o->self_test = true;
        merge_id(o->from_id, o->to_id);     /* one copy completes the other */
        o->cat = DSC_CAT_SELFTEST;
        strcpy(o->cat_name, "SELF TEST");
    }

    /* end of sequence and error check character */
    while (j < n && !is_eos(sym[j])) j++;
    o->eos = j < n ? sym[j] : -1;
    code_text(buf, sizeof(buf), o->eos, eos_name(o->eos));
    add(o, "End of sequence", buf, DSC_HL_NONE);
    if (o->eos >= 0) {
        size_t L = strlen(o->line2);
        snprintf(o->line2 + L, sizeof(o->line2) - L, " - %s", eos_name(o->eos));
    }
    o->ecc_rx = at(sym, n, j + 1);
    bool all = j < n && fmt >= 0;
    int x = fmt;
    for (int i = st; i <= j && i < n; ++i) {
        if (sym[i] < 0) { all = false; break; }
        x ^= sym[i];
    }
    o->ecc_calc = all ? x : -1;
    o->ecc_ok = all && o->ecc_rx >= 0 && o->ecc_rx == o->ecc_calc;
    if (o->ecc_ok) snprintf(buf, sizeof(buf), "%d - valid", o->ecc_rx);
    else if (o->ecc_rx >= 0 && o->ecc_calc >= 0) snprintf(buf, sizeof(buf), "%d - WRONG (calc %d)", o->ecc_rx, o->ecc_calc);
    else snprintf(buf, sizeof(buf), "not verifiable");
    add(o, "ECC", buf, o->ecc_ok ? DSC_HL_OK : DSC_HL_BAD);
}

/* ---- plain-language explanation ------------------------------------------ */

typedef struct { char *p; size_t left; } sb_t;

static void sb_add(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_add(sb_t *b, const char *fmt, ...)
{
    if (b->left <= 1) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b->p, b->left, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k >= b->left) k = (int)b->left - 1;
    b->p += k;
    b->left -= (size_t)k;
}

/* "Spain" from "Spain - coast"; "" when there is no country */
static void country_of(const char *where, char *dst, size_t sz)
{
    const char *d = strstr(where, " - ");
    if (!d) { dst[0] = '\0'; return; }
    size_t n = (size_t)(d - where);
    if (n >= sz) n = sz - 1;
    memcpy(dst, where, n);
    dst[n] = '\0';
}

static bool id_known(const char *id)
{
    return id[0] && strchr(id, '?') == NULL;
}

/* "coast station 002240994 (Spain)", "ship CORUNA (Bahamas)" */
static void station(char *dst, size_t sz, dsc_kind_t k, const char *id, const char *where, const char *name)
{
    char c[32];
    country_of(where, c, sizeof(c));
    const char *w;
    switch (k) {
    case DSC_KIND_SHIP:   w = "ship"; break;
    case DSC_KIND_COAST:  w = "coast station"; break;
    case DSC_KIND_GROUP:  w = "ship group"; break;
    case DSC_KIND_SAR:    w = "SAR aircraft"; break;
    case DSC_KIND_BEACON: w = "beacon"; break;
    default:              w = "station"; break;
    }
    if (!id_known(id) && !name) {
        snprintf(dst, sz, "a %s (MMSI partly unreadable%s%s)", k == DSC_KIND_UNKNOWN ? "station" : w,
                 c[0] ? ", " : "", c);
        return;
    }
    /* a known name replaces the MMSI (the MMSI is in the details) */
    if (name && c[0]) snprintf(dst, sz, "%s %s (%s)", w, name, c);
    else if (name) snprintf(dst, sz, "%s %s", w, name);
    else if (c[0]) snprintf(dst, sz, "%s %s (%s)", w, id, c);
    else snprintf(dst, sz, "%s %s", w, id);
}

static bool freq_none(const char *f)
{
    return !f[0] || !strcmp(f, "-") || !strcmp(f, "?") || !strcmp(f, "unreadable") || !strncmp(f, "no information", 14);
}

/* " on 8291.0 kHz", " (RX 8291.0 kHz, TX 8195.0 kHz)" or "" */
static void freq_phrase(const dsc_info_t *in, char *dst, size_t sz)
{
    bool rn = freq_none(in->rx_freq), tn = freq_none(in->tx_freq);
    /* a partly received frequency adds nothing next to a complete one */
    if (!rn && !tn && strstr(in->tx_freq, "partial") && !strstr(in->rx_freq, "partial")) tn = true;
    else if (!rn && !tn && strstr(in->rx_freq, "partial") && !strstr(in->tx_freq, "partial")) rn = true;
    if (rn && tn) dst[0] = '\0';
    else if (rn) snprintf(dst, sz, " on %s", in->tx_freq);
    else if (tn || !strcmp(in->rx_freq, in->tx_freq)) snprintf(dst, sz, " on %s", in->rx_freq);
    else snprintf(dst, sz, " (the sender receives on %s and transmits on %s)", in->rx_freq, in->tx_freq);
}

/* what the follow-up is, for telecommand 1 */
static const char *mode_phrase(int tc1)
{
    switch (tc1) {
    case 100: return "in VHF voice (F3E/G3E)";
    case 101: return "in VHF duplex voice";
    case 106: return "with data";
    case 109: return "in SSB voice (J3E)";
    case 113: return "with teleprinter (NBDP, FEC)";
    case 115: return "with teleprinter (NBDP, ARQ)";
    default:  return NULL;
    }
}

/* voice channel associated with the MF/HF DSC distress channel */
static const char *voice_of(long hz)
{
    switch (hz) {
    case 2187500:  return "2182 kHz";
    case 4207500:  return "4125 kHz";
    case 6312000:  return "6215 kHz";
    case 8414500:  return "8291 kHz";
    case 12577000: return "12290 kHz";
    case 16804500: return "16420 kHz";
    default:       return NULL;
    }
}

/* NBDP (teleprinter) channel associated with the MF/HF DSC distress channel */
static const char *nbdp_of(long hz)
{
    switch (hz) {
    case 2187500:  return "2174.5 kHz";
    case 4207500:  return "4177.5 kHz";
    case 6312000:  return "6268 kHz";
    case 8414500:  return "8376.5 kHz";
    case 12577000: return "12520 kHz";
    case 16804500: return "16695 kHz";
    default:       return NULL;
    }
}

static void lower_first(char *s)
{
    if (s[0] >= 'A' && s[0] <= 'Z') s[0] = (char)(s[0] - 'A' + 'a');
}

static void upper_first(char *s)
{
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
}

void dsc_explain(const dsc_info_t *in, const char *from_name, const char *to_name,
                 const char *dis_name, long rx_hz, char *dst, size_t sz)
{
    sb_t b = { dst, sz };
    dst[0] = '\0';
    if (sz == 0) return;

    char from[96], to[96], dis[96], nat[48] = "", fq[96];
    station(from, sizeof(from), in->from_kind, in->from_id, in->from_where, from_name);
    station(to, sizeof(to), in->to_kind, in->to_id, in->to_where, to_name);
    if (!strcmp(in->dis_id, "unknown")) snprintf(dis, sizeof(dis), "an unidentified ship");
    else station(dis, sizeof(dis), DSC_KIND_SHIP, in->dis_id, in->dis_where, dis_name);
    if (nature_name(in->nature)) { snprintf(nat, sizeof(nat), "%s", nature_name(in->nature)); lower_first(nat); }
    freq_phrase(in, fq, sizeof(fq));
    const char *voice = voice_of(rx_hz);
    bool pos_ok = in->pos[0] && strcmp(in->pos, "?") && strcmp(in->pos, "no information");
    bool ack = in->eos == 122, rq = in->eos == 117;

    /* recipient as an object of "calls" */
    char whom[110];
    if (in->fmt == 116) snprintf(whom, sizeof(whom), "all ships");
    else if (in->fmt == 102) snprintf(whom, sizeof(whom), "all ships in the area of %s south-east of %s", in->to_where, in->to_id + 5);
    else snprintf(whom, sizeof(whom), "%s", to);

    const char *cat = NULL;
    switch (in->cat) {
    case DSC_CAT_SAFETY:   cat = "SAFETY"; break;
    case DSC_CAT_URGENCY:  cat = "URGENCY"; break;
    case DSC_CAT_DISTRESS: cat = "DISTRESS"; break;
    default: break;
    }

    if (in->fmt == 112) {
        /* distress alert */
        sb_add(&b, "DISTRESS ALERT: %s", from);
        if (nat[0] && in->nature != 107) sb_add(&b, " reports %s", nat);
        else sb_add(&b, " is in distress");
        if (pos_ok) sb_add(&b, " at %s", in->pos);
        if (pos_ok && strcmp(in->pos_time, "?") && strcmp(in->pos_time, "no information"))
            sb_add(&b, " (position at %s)", in->pos_time);
        sb_add(&b, ". ");
        const char *nbdp = nbdp_of(rx_hz);
        if (in->subseq == 113 || in->subseq == 115) {
            sb_add(&b, "Follow-up by teleprinter (NBDP)");
            if (nbdp) sb_add(&b, " on %s", nbdp);
        } else {
            sb_add(&b, "Follow-up in voice");
            if (voice) sb_add(&b, " on %s", voice);
        }
        sb_add(&b, ". Coast stations normally acknowledge it.");
    } else if (in->tc1 == 110 && id_known(in->dis_id) && !strcmp(in->dis_id, in->from_id)) {
        /* M.493: a distress acknowledgement with its own MMSI as the ship
         * in distress cancels a (false) distress alert */
        sb_add(&b, "%s CANCELS its own distress alert: it was a false alert, no assistance is needed.", from);
        if (voice) sb_add(&b, " A cancellation message normally follows in voice on %s.", voice);
    } else if (in->tc1 == 110 && in->dis_id[0]) {
        /* distress acknowledgement */
        sb_add(&b, "%s acknowledges the distress alert", from);
        if (nat[0]) sb_add(&b, " (%s)", nat);
        sb_add(&b, " of %s", dis);
        if (pos_ok) sb_add(&b, " at %s", in->pos);
        sb_add(&b, ". The rescue is being coordinated");
        if (voice) sb_add(&b, ": traffic follows on %s", voice);
        sb_add(&b, ".");
    } else if (in->tc1 == 112 && in->dis_id[0] && ack) {
        /* acknowledgement of a distress relay */
        if (in->fmt == 116 || in->fmt == 102) sb_add(&b, "%s acknowledges a distress relay about %s", from, dis);
        else sb_add(&b, "%s acknowledges the distress relay of %s about %s", from, to, dis);
        if (nat[0]) sb_add(&b, " (%s)", nat);
        if (pos_ok) sb_add(&b, " at %s", in->pos);
        sb_add(&b, ".");
        if (voice) sb_add(&b, " Traffic follows on %s.", voice);
    } else if (in->tc1 == 112 && in->dis_id[0]) {
        /* distress relay */
        sb_add(&b, "%s relays the distress alert", from);
        if (nat[0]) sb_add(&b, " (%s)", nat);
        sb_add(&b, " of %s to %s", dis, whom);
        if (pos_ok) sb_add(&b, " at %s", in->pos);
        sb_add(&b, ".");
        if (voice) sb_add(&b, " Traffic follows on %s.", voice);
    } else if (in->self_test && in->tc1 != 118) {
        /* polling or other call to itself: the same loop test */
        if (ack) sb_add(&b, "%s answers its own call: its DSC equipment received the loop test.", from);
        else sb_add(&b, "%s calls its own MMSI%s%s: a loop test of its DSC equipment, received by its own "
                    "DSC watch receiver. No other station is expected to answer.", from,
                    in->tc1_name[0] && strcmp(in->tc1_name, "?") ? " - " : "",
                    in->tc1_name[0] && strcmp(in->tc1_name, "?") ? in->tc1_name : "");
    } else if (in->tc1 == 118) {
        /* test */
        if (in->self_test) {
            /* a loop test: the station's own DSC watch receiver (or a second
             * DSC unit on board) receives it and checks the transmitter */
            if (ack) sb_add(&b, "%s acknowledges its own test call: its DSC equipment received the loop test.", from);
            else sb_add(&b, "%s sends a test call to its own MMSI: a loop test of its DSC equipment, "
                        "received by its own DSC watch receiver. No other station is expected to answer.", from);
        } else if (ack) {
            sb_add(&b, "%s answers the test call of %s: the DSC equipment of the calling station works.", from, to);
        } else {
            sb_add(&b, "%s sends a test call to %s, only to check its DSC equipment; no voice contact follows.", from, whom);
            if (rq) sb_add(&b, " It expects an automatic acknowledgement.");
        }
    } else if (in->tc1 == 104) {
        const char *r = unable_reason(in->tc2);
        char rr[48] = "";
        if (r && in->tc2 != 100 && in->tc2 != 126) { snprintf(rr, sizeof(rr), " (reason: %s)", r); }
        sb_add(&b, "%s cannot accept the call of %s%s.", from, to, rr);
        bool rn = freq_none(in->rx_freq), tn = freq_none(in->tx_freq);
        if (!rn && !tn && strcmp(in->rx_freq, in->tx_freq))
            sb_add(&b, " Frequencies in the reply: RX %s, TX %s.", in->rx_freq, in->tx_freq);
        else if (!rn || !tn)
            sb_add(&b, " Frequency in the reply: %s.", rn ? in->tx_freq : in->rx_freq);
    } else if (in->tc1 == 105) {
        sb_add(&b, "%s closes the communication with %s.", from, to);
    } else if (in->tc1 == 103) {
        if (ack) sb_add(&b, "%s answers the polling of %s: it is in range and listening.", from, to);
        else sb_add(&b, "%s checks whether %s is in range (polling).", from, whom);
    } else if (in->tc1 == 121) {
        if (ack) sb_add(&b, "%s sends its position to %s.", from, to);
        else sb_add(&b, "%s asks %s for its position.", from, whom);
    } else if (mode_phrase(in->tc1)) {
        const char *m = mode_phrase(in->tc1);
        if (ack) {
            sb_add(&b, "%s accepts the call of %s: they continue %s%s.", from, to, m, fq[0] ? fq : "");
        } else {
            sb_add(&b, "%s calls %s", from, whom);
            if (cat && in->cat != DSC_CAT_DISTRESS) sb_add(&b, " with %s %s message", in->cat == DSC_CAT_URGENCY ? "an" : "a", cat);
            if (in->fmt == 116 || in->fmt == 102 || in->fmt == 114) {
                sb_add(&b, ", which follows %s", m);
                if (!freq_none(in->tx_freq) && !freq_none(in->rx_freq) && strcmp(in->rx_freq, in->tx_freq))
                    sb_add(&b, " on %s (the sender listens on %s)", in->tx_freq, in->rx_freq);
                else if (fq[0]) sb_add(&b, "%s", fq);
                else if (voice && in->tc1 == 109) sb_add(&b, " on %s", voice);
                sb_add(&b, ".");
            } else {
                sb_add(&b, " and proposes to continue %s", m);
                if (fq[0]) sb_add(&b, "%s", fq);
                sb_add(&b, ".");
                if (rq) sb_add(&b, " An acknowledgement is requested.");
            }
        }
    } else {
        sb_add(&b, "%s calls %s", from, whom);
        if (cat) sb_add(&b, " (%s)", cat);
        if (in->tc1_name[0] && strcmp(in->tc1_name, "?")) sb_add(&b, ": %s", in->tc1_name);
        sb_add(&b, ".");
        if (ack) sb_add(&b, " This is an acknowledgement.");
    }

    /* position sent instead of the frequencies (symbol 55) */
    if (pos_ok && in->fmt != 112 && in->tc1 != 110 && in->tc1 != 112) {
        sb_add(&b, " Position of %s: %s", from, in->pos);
        if (strcmp(in->pos_time, "") && strcmp(in->pos_time, "?") && strcmp(in->pos_time, "no information"))
            sb_add(&b, " at %s", in->pos_time);
        sb_add(&b, ".");
    }

    /* telecommand 2 qualifiers (with telecommands other than "unable to comply") */
    if (in->tc1 != 104) {
        if (in->tc2 == 110) sb_add(&b, " The calling station is not party to an armed conflict.");
        else if (in->tc2 == 111) sb_add(&b, " It concerns medical transports.");
        else if (in->tc2 == 113) sb_add(&b, " Facsimile/data (ITU-T V.21).");
    }
    if (in->number[0]) sb_add(&b, " Telephone number: %s.", in->number);

    if (!in->ecc_ok)
        sb_add(&b, " Some characters were not received cleanly: the content is not verified.");
    upper_first(dst);
}
