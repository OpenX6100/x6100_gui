/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 - NAVTEX bulletin assembler, see bulletin.h.
 */
#include "bulletin.h"
#include "decoder.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NB_LINE_MAX         200     /* a line longer than this is cut */
#define NB_SIGNAL_GONE_S    20      /* sync lost for this long: bulletin over */
#define NB_SILENCE_S        60      /* no characters for this long: over */
#define NB_JUNK_MIN_ALNUM   40      /* text without header: keep only if */
#define NB_JUNK_MIN_LINES   2       /*   long enough and not mostly bad */
#define NB_JUNK_MAX_BAD     30      /*   (% guessed or lost characters) */

struct nb_assembler {
    nb_done_cb_t done;
    void *user;

    nb_bulletin_t cur;              /* bulletin being received */
    bool has_header;
    bool dtg_first;                 /* no header, first line a date-time group */
    int content_lines;              /* lines with text so far */
    bool blank_after;               /* an empty line after the last one */

    char line[NB_LINE_MAX];
    uint8_t line_q[NB_LINE_MAX];
    int line_len;

    time_t last_char;
    time_t sync_off;                /* 0 = in sync */
};

/* ---- small helpers ------------------------------------------------------------ */

/* digits received as letters after a lost FIGS shift: QWERTYUIOP = 1234567890 */
static int figs_digit(char c)
{
    static const char row[] = "QWERTYUIOP";
    if (c >= '0' && c <= '9') return c - '0';
    const char *p = strchr(row, c);
    if (c && p) return (int)(p - row + 1) % 10;
    return -1;
}

static void cur_reset(nb_assembler_t *a)
{
    nb_bulletin_t *b = &a->cur;
    b->b1 = b->b2 = 0;
    b->num = -1;
    b->lost_head = true;
    b->lost_tail = false;
    b->sitor = false;
    b->first_rx = b->last_rx = 0;
    b->receptions = 1;
    b->len = 0;
    b->text[0] = '\0';
    a->has_header = false;
    a->dtg_first = false;
    a->content_lines = 0;
    a->blank_after = false;
}

static void cur_append(nb_assembler_t *a, const char *s, const uint8_t *q, int n, time_t now)
{
    nb_bulletin_t *b = &a->cur;
    if (n > NB_MAX_TEXT - b->len) n = NB_MAX_TEXT - b->len;
    if (n <= 0) return;
    memcpy(b->text + b->len, s, (size_t)n);
    memcpy(b->q + b->len, q, (size_t)n);
    b->len += n;
    b->text[b->len] = '\0';
    if (!b->first_rx) b->first_rx = now;
    b->last_rx = now;
}

/* ---- header and end lines ----------------------------------------------------- */

/*
 * "ZCZC B1B2B3B4", anywhere in the line: ZCZC with at most one wrong
 * character, then the 4 header characters with at most one unreadable.
 */
static bool parse_header(const char *s, int n, char *b1, char *b2, int *num)
{
    for (int i = 0; i + 8 <= n; ++i) {
        int match = 0;
        for (int k = 0; k < 4; ++k) match += (s[i + k] == "ZCZC"[k]);
        if (match < 3 || (s[i] != 'Z' && s[i + 1] != 'C')) continue;

        int j = i + 4, sp = 0;
        while (j < n && s[j] == ' ' && sp < 3) { j++; sp++; }
        if (j + 4 > n) continue;

        const char *h = s + j;
        int bad = 0;
        char c1 = isupper((unsigned char)h[0]) ? h[0] : 0;
        char c2 = isupper((unsigned char)h[1]) ? h[1] : 0;
        int d3 = figs_digit(h[2]), d4 = figs_digit(h[3]);
        bad += !c1 + !c2 + (d3 < 0) + (d4 < 0);
        if (bad > 1) continue;

        /* nothing else of substance on the line */
        int extra = 0;
        for (int k = j + 4; k < n; ++k) if (s[k] != ' ') extra++;
        if (extra > 4) continue;

        *b1 = c1;
        *b2 = c2;
        *num = (d3 >= 0 && d4 >= 0) ? d3 * 10 + d4 : -1;
        return true;
    }
    return false;
}

/* "NNNN", also "NNN", "NN*N", "NNNN" followed by noise on the same line */
static bool is_end(const char *s, int n)
{
    int i = 0;
    while (i < n && s[i] == ' ') i++;
    int nn = 0, other = 0, k = i;
    for (; k < n && k < i + 6 && s[k] != ' '; ++k) {
        if (s[k] == 'N') nn++;
        else if (s[k] == '*') other++;
        else break;
    }
    if (nn >= 3 && nn + other >= 3 && (k - i) <= 6) return true;
    /* "*NN", "N*N" alone on the line */
    if (nn >= 2 && nn + other >= 3 && other <= 1 && (k - i) <= 5 && k >= n - 1) return true;
    /* NNN at the start of a line glued to noise */
    return n - i >= 3 && s[i] == 'N' && s[i + 1] == 'N' && s[i + 2] == 'N';
}

/*
 * Date-time group, the first line of a bulletin: "102320 UTC OCT 26",
 * "261408 LMT MAR 25" (digits possibly received as letters).  Seen in the
 * middle of a bulletin it means the next bulletin started and its header
 * (and our NNNN) were lost.
 */
static bool is_dtg(const char *s, int n)
{
    int i = 0;
    while (i < n && s[i] == ' ') i++;
    int digits = 0, k = 0;
    for (; k < 6 && i + k < n; ++k) digits += figs_digit(s[i + k]) >= 0 || s[i + k] == '*';
    if (k < 6 || digits < 5) return false;
    i += 6;
    if (i >= n || (s[i] != ' ' && s[i] != '*')) return false;
    while (i < n && s[i] == ' ') i++;
    if (i + 3 > n) return false;
    int utc = (s[i] == 'U') + (s[i + 1] == 'T') + (s[i + 2] == 'C');
    int lmt = (s[i] == 'L') + (s[i + 1] == 'M') + (s[i + 2] == 'T');
    int gmt = (s[i] == 'G') + (s[i + 1] == 'M') + (s[i + 2] == 'T');
    return utc >= 2 || lmt >= 2 || gmt >= 2;
}

/* ---- closing a bulletin -------------------------------------------------------- */

static void trim(nb_bulletin_t *b)
{
    int start = 0;
    while (start < b->len && (b->text[start] == '\n' || b->text[start] == ' ')) start++;
    int end = b->len;
    while (end > start && (b->text[end - 1] == '\n' || b->text[end - 1] == ' ')) end--;
    if (start) {
        memmove(b->text, b->text + start, (size_t)(end - start));
        memmove(b->q, b->q + start, (size_t)(end - start));
    }
    b->len = end - start;
    b->text[b->len] = '\0';
}

/* text without a header: worth keeping? */
static bool worth_keeping(const nb_bulletin_t *b)
{
    int alnum = 0, bad = 0, lines = 0, in_line = 0;
    for (int i = 0; i < b->len; ++i) {
        char c = b->text[i];
        if (c == '\n') { if (in_line) lines++; in_line = 0; continue; }
        if (c != ' ') in_line = 1;
        if (isalnum((unsigned char)c)) alnum++;
        if (c != ' ' && b->q[i] >= NAVTEX_Q_GUESS) bad++;
    }
    if (in_line) lines++;
    return alnum >= NB_JUNK_MIN_ALNUM && lines >= NB_JUNK_MIN_LINES &&
           bad * 100 <= NB_JUNK_MAX_BAD * (alnum ? alnum : 1);
}

/* how a bulletin ended */
typedef enum {
    END_NNNN,                       /* its NNNN */
    END_NEXT,                       /* the header of the next one */
    END_DTG,                        /* the date-time group of the next one */
    END_SIGNAL                      /* signal gone, silence, decoder stopped */
} end_t;

static void cur_close(nb_assembler_t *a, end_t end)
{
    nb_bulletin_t *b = &a->cur;
    bool tail_seen = end == END_NNNN;
    trim(b);
    if (!a->has_header && a->dtg_first && !tail_seen) {
        /*
         * A SITOR-B message without the NAVTEX frame (no ZCZC, no NNNN),
         * as some coast stations send on HF: it starts with its date-time
         * group and ends where the next one starts.  Complete, unless the
         * signal went in the middle of a line of text.
         */
        b->sitor = true;
        b->lost_head = false;
        b->lost_tail = end == END_SIGNAL && !a->blank_after;
    } else {
        /* (a date-time group first and an NNNN: a NAVTEX bulletin whose header was lost) */
        b->lost_head = !a->has_header;
        /* text without header up to the date-time group of a SITOR-B
         * message: caught in the middle, but received to its end */
        b->lost_tail = !tail_seen && !(end == END_DTG && !a->has_header);
    }
    /* a header and nothing else, cut by the end of the signal: nothing to show */
    bool empty_cut = a->has_header && b->len == 0 && !tail_seen;
    if (!empty_cut && (a->has_header || (b->len > 0 && worth_keeping(b)))) {
        if (a->done) a->done(b, a->user);
    }
    cur_reset(a);
}

static bool cur_has_content(const nb_assembler_t *a)
{
    return a->has_header || a->cur.len > 0;
}

static void commit_line(nb_assembler_t *a, time_t now)
{
    const char *s = a->line;
    int n = a->line_len;
    a->line_len = 0;

    char b1, b2;
    int num;
    if (parse_header(s, n, &b1, &b2, &num)) {
        if (cur_has_content(a)) cur_close(a, END_NEXT);
        a->has_header = true;
        a->cur.b1 = b1;
        a->cur.b2 = b2;
        a->cur.num = num;
        a->cur.first_rx = a->cur.last_rx = now;
        return;
    }
    if (is_end(s, n)) {
        if (cur_has_content(a)) cur_close(a, END_NNNN);
        return;
    }
    /* a date-time group after some text: a new bulletin whose header was lost */
    if (a->cur.len > 0 && is_dtg(s, n)) {
        int lines = 0;
        for (int i = 0; i < a->cur.len; ++i) lines += a->cur.text[i] == '\n';
        if (lines >= 2) cur_close(a, END_DTG);
    }

    bool blank = true;
    for (int i = 0; i < n && blank; ++i) blank = s[i] == ' ';
    if (!blank) {
        if (a->content_lines == 0 && !a->has_header && is_dtg(s, n)) a->dtg_first = true;
        a->content_lines++;
    }
    a->blank_after = blank && a->content_lines > 0;

    static const uint8_t nl_q = NAVTEX_Q_CLEAN;
    cur_append(a, s, a->line_q, n, now);
    cur_append(a, "\n", &nl_q, 1, now);
}

/* ---- public ------------------------------------------------------------------ */

nb_assembler_t *nb_create(nb_done_cb_t done, void *user)
{
    nb_assembler_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->done = done;
    a->user = user;
    cur_reset(a);
    return a;
}

void nb_destroy(nb_assembler_t *a)
{
    free(a);
}

void nb_feed(nb_assembler_t *a, int c, int quality, time_t now)
{
    if (!a) return;

    if (quality == NAVTEX_Q_EVENT) {
        if (c == NAVTEX_EV_SYNC_OFF) a->sync_off = now ? now : 1;
        else if (c == NAVTEX_EV_SYNC_ON) a->sync_off = 0;
        return;
    }

    a->last_char = now;
    if (c == '\r') return;
    if (c == '\n') { commit_line(a, now); return; }
    if (c < ' ' || c > '~') c = '*';

    if (a->line_len >= NB_LINE_MAX) commit_line(a, now);
    a->line[a->line_len] = (char)c;
    a->line_q[a->line_len] = (uint8_t)(quality < 0 ? NAVTEX_Q_LOST : quality);
    a->line_len++;
}

void nb_tick(nb_assembler_t *a, time_t now)
{
    if (!a || (!cur_has_content(a) && a->line_len == 0)) return;
    bool gone = a->sync_off && now - a->sync_off >= NB_SIGNAL_GONE_S;
    bool silent = a->last_char && now - a->last_char >= NB_SILENCE_S;
    if (gone || silent) nb_flush(a);
}

bool nb_active(const nb_assembler_t *a)
{
    return a && a->has_header;
}

void nb_flush(nb_assembler_t *a)
{
    if (!a) return;
    if (a->line_len) commit_line(a, a->last_char);
    if (cur_has_content(a)) cur_close(a, END_SIGNAL);
}

/* ---- helpers ------------------------------------------------------------------ */

void nb_counts(const nb_bulletin_t *b, int *total, int *clean, int *guess, int *lost)
{
    int t = 0, c = 0, g = 0, l = 0;
    for (int i = 0; i < b->len; ++i) {
        if (b->text[i] == ' ' || b->text[i] == '\n') continue;
        t++;
        switch (b->q[i]) {
        case NAVTEX_Q_CLEAN:
        case NAVTEX_Q_REP:   c++; break;
        case NAVTEX_Q_GUESS: g++; break;
        default:             l++; break;
        }
    }
    if (total) *total = t;
    if (clean) *clean = c;
    if (guess) *guess = g;
    if (lost) *lost = l;
}

int nb_quality(const nb_bulletin_t *b)
{
    int t, c;
    nb_counts(b, &t, &c, NULL, NULL);
    return t ? (c * 100 + t / 2) / t : 0;
}

bool nb_corrupted(const nb_bulletin_t *b)
{
    int t, l;
    nb_counts(b, &t, NULL, NULL, &l);
    return t == 0 || l * 100 > 4 * t;
}

const char *nb_subject_name(char b2)
{
    switch (b2) {
    case 'A': return "Navigational warning";
    case 'B': return "Meteorological warning";
    case 'C': return "Ice report";
    case 'D': return "Search and rescue / piracy";
    case 'E': return "Weather forecast";
    case 'F': return "Pilot service";
    case 'G': return "AIS";
    case 'H': return "LORAN";
    case 'J': return "GNSS";
    case 'K': return "Other navaids";
    case 'L': return "Navigational warning (add.)";
    case 'T': return "Test";
    case 'V': case 'W': case 'X': case 'Y': return "Special service";
    case 'Z': return "No message on hand";
    default:  return "";
    }
}

/* ---- merging two copies of the same bulletin ----------------------------------- */

#define NB_MAX_LINES 400

static int split_lines(const nb_bulletin_t *b, int *start, int *len)
{
    int n = 0, s = 0;
    for (int i = 0; i <= b->len && n < NB_MAX_LINES; ++i) {
        if (i == b->len || b->text[i] == '\n') {
            start[n] = s;
            len[n] = i - s;
            n++;
            s = i + 1;
        }
    }
    return n;
}

/* share of equal characters of two lines of the same length, 0..100 */
static int line_match(const char *x, const char *y, int n)
{
    if (n == 0) return 100;
    int eq = 0;
    for (int i = 0; i < n; ++i) eq += (x[i] == y[i]) || x[i] == '*' || y[i] == '*';
    return eq * 100 / n;
}

static int bad_chars(const nb_bulletin_t *b)
{
    int g, l;
    nb_counts(b, NULL, NULL, &g, &l);
    return g + 2 * l;
}

bool nb_merge(nb_bulletin_t *dst, const nb_bulletin_t *src)
{
    /* same header; a copy whose header was lost must match on the text alone */
    bool dkey = dst->b1 && dst->b2 && dst->num >= 0;
    bool skey = src->b1 && src->b2 && src->num >= 0;
    if (dkey && skey &&
        (dst->b1 != src->b1 || dst->b2 != src->b2 || dst->num != src->num))
        return false;
    if (!dkey && !skey) return false;
    bool by_text = !(dkey && skey);

    static int ds[NB_MAX_LINES], dl[NB_MAX_LINES], ss[NB_MAX_LINES], sl[NB_MAX_LINES];
    int nd = split_lines(dst, ds, dl), ns = split_lines(src, ss, sl);

    /* the same text?  (serial numbers are reused every 100 bulletins) */
    int matched = 0, lines = 0;
    for (int i = 0; i < nd; ++i) {
        if (dl[i] < 4) continue;
        lines++;
        for (int j = 0; j < ns; ++j)
            if (sl[j] == dl[i] && line_match(dst->text + ds[i], src->text + ss[j], dl[i]) >= 70) {
                matched++;
                break;
            }
    }
    if (lines && matched * 2 < lines) return false;
    if (by_text && (matched < 3 || matched * 10 < lines * 6)) return false;

    /* the more complete / cleaner copy becomes the base */
    static nb_bulletin_t other;
    int dscore = (!dst->lost_head) + (!dst->lost_tail);
    int sscore = (!src->lost_head) + (!src->lost_tail);
    bool swap = sscore > dscore ||
                (sscore == dscore && src->len >= dst->len * 9 / 10 && bad_chars(src) < bad_chars(dst));
    int receptions = dst->receptions + src->receptions;
    time_t first = dst->first_rx < src->first_rx ? dst->first_rx : src->first_rx;
    time_t last = dst->last_rx > src->last_rx ? dst->last_rx : src->last_rx;
    if (swap) {
        other = *dst;
        *dst = *src;
    } else {
        other = *src;
    }
    nd = split_lines(dst, ds, dl);
    ns = split_lines(&other, ss, sl);

    /* character by character on the lines found in both copies */
    int from = 0;
    for (int i = 0; i < nd; ++i) {
        for (int j = from; j < ns; ++j) {
            if (sl[j] != dl[i] || line_match(dst->text + ds[i], other.text + ss[j], dl[i]) < 70)
                continue;
            for (int k = 0; k < dl[i]; ++k) {
                int p = ds[i] + k, r = ss[j] + k;
                if (other.q[r] < dst->q[p]) {
                    dst->text[p] = other.text[r];
                    dst->q[p] = other.q[r];
                }
            }
            from = j + 1;
            break;
        }
    }

    /* the header of the copy that has it */
    if (!dst->b1 && other.b1) { dst->b1 = other.b1; dst->b2 = other.b2; dst->num = other.num; }
    dst->sitor = dst->sitor && !dst->b1 && !dst->b2;
    dst->lost_head = dst->lost_head && other.lost_head;
    dst->lost_tail = dst->lost_tail && other.lost_tail;
    dst->receptions = receptions;
    dst->first_rx = first;
    dst->last_rx = last;
    return true;
}

/* ---- quality level ------------------------------------------------------------ */

nb_level_t nb_level(const nb_bulletin_t *b)
{
    int t, l;
    nb_counts(b, &t, NULL, NULL, &l);
    if (t == 0 || l * 100 > 15 * t) return NB_LEVEL_UNREADABLE;
    if (l * 100 > 4 * t) return NB_LEVEL_DAMAGED;
    return NB_LEVEL_OK;
}

/* ---- summary -------------------------------------------------------------------- */

static void copy_line(char *dst, int sz, const char *s, int n)
{
    while (n > 0 && *s == ' ') { s++; n--; }
    while (n > 0 && s[n - 1] == ' ') n--;
    if (n > sz - 1) n = sz - 1;
    memcpy(dst, s, (size_t)(n > 0 ? n : 0));
    dst[n > 0 ? n : 0] = '\0';
}

static bool has_word(const char *s, int n, const char *w)
{
    int L = (int)strlen(w);
    for (int i = 0; i + L <= n; ++i)
        if (!memcmp(s + i, w, (size_t)L) &&
            (i == 0 || s[i - 1] == ' ') && (i + L == n || s[i + L] == ' ' || s[i + L] == '.'))
            return true;
    return false;
}

void nb_summary(const nb_bulletin_t *b, char *station, char *dtg,
                char *title, char *title2, int sz)
{
    station[0] = dtg[0] = title[0] = title2[0] = '\0';
    int s = 0, line = 0;
    for (int i = 0; i <= b->len && line < 12; ++i) {
        if (i < b->len && b->text[i] != '\n') continue;
        const char *p = b->text + s;
        int n = i - s;
        s = i + 1;
        int k = 0;
        while (k < n && (p[k] == ' ' || p[k] == '*')) k++;
        if (k == n) continue;                       /* empty or only lost characters */
        line++;
        if (!dtg[0] && line <= 2 && is_dtg(p, n)) { copy_line(dtg, sz, p, n); continue; }
        if (!station[0] && line <= 3 &&
            (has_word(p, n, "RADIO") || has_word(p, n, "RADYO") || has_word(p, n, "NAVTEX") ||
             has_word(p, n, "RADIOSTATION") || has_word(p, n, "COASTGUARD") || has_word(p, n, "MRCC"))) {
            copy_line(station, sz, p, n);
            continue;
        }
        int alnum = 0;
        for (int j = 0; j < n; ++j) alnum += isalnum((unsigned char)p[j]) != 0;
        if (alnum < 3) continue;                    /* "." or noise: not a title */
        if (!title[0]) copy_line(title, sz, p, n);
        else if (!title2[0]) { copy_line(title2, sz, p, n); break; }
    }
}

/* ---- positions --------------------------------------------------------------------- */

/* letters that stand for digits and . , - when the FIGS shift was lost */
static int figs_char(char c)
{
    int d = figs_digit(c);
    if (d >= 0 && !(c >= '0' && c <= '9')) return '0' + d;
    if (c == 'M') return '.';
    if (c == 'N') return ',';
    if (c == 'A') return '-';
    return 0;
}

/* plain English words written only with those letters: never digits */
static bool is_common_word(const char *w, int len)
{
    static const char *const words[] = {
        "AM", "AN", "AT", "IN", "IT", "ON", "OR", "TO", "UP", "WE", "PM", "MAN", "MAY", "NOT",
        "NOW", "ONE", "OUR", "OUT", "PUT", "TOW", "TWO", "WAY", "WET", "WIN", "WON", "YOU",
        "ANY", "ARE", "EAT", "TEN", "TOP", "WAR", "AREA", "ITEM", "INTO", "IRON", "MANY",
        "MEAN", "MEET", "MINE", "MORE", "NAME", "NEAR", "NINE", "NONE", "NOON", "NOTE", "OPEN",
        "OUTER", "PART", "PIER", "PIPE", "PORT", "POWER", "QUIT", "RATE", "REPORT", "RETURN",
        "RIOT", "ROUTE", "TIME", "TOWER", "TOWN", "TYPE", "UNIT", "UPON", "WAIT", "WANT",
        "WATER", "WAYPOINT", "WEAPON", "WERE", "WIRE", "WORE", "WORN", "WRITE", "YEAR",
        "YOUR", "MOTOR", "NORM", "POINT", "PRIOR", "OPERATE", "OPERATION", "OPERATOR",
        "IMPORTANT", "ENTER", "ENTRY", "EMPTY", "PROPER", "TERM", "TIMER",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if ((int)strlen(words[i]) == len && memcmp(words[i], w, (size_t)len) == 0) return true;
    return false;
}

static bool is_hemisphere(char c)
{
    /* N S E W, Turkish K(uzey) G(uney) D(ogu) B(ati) */
    return c == 'N' || c == 'S' || c == 'E' || c == 'W' ||
           c == 'K' || c == 'G' || c == 'D' || c == 'B';
}

void nb_mark_positions(const nb_bulletin_t *b, char *fixed, uint8_t *mark)
{
    memcpy(fixed, b->text, (size_t)b->len);
    memset(mark, NB_MARK_NONE, (size_t)b->len);

    int s = 0;
    for (int i = 0; i <= b->len; ++i) {
        if (i < b->len && b->text[i] != '\n') continue;
        int ls = s, le = i;
        s = i + 1;

        /* words of the line */
        int ws[64], we[64], nw = 0;
        for (int k = ls; k < le && nw < 64; ) {
            while (k < le && b->text[k] == ' ') k++;
            if (k >= le) break;
            ws[nw] = k;
            while (k < le && b->text[k] != ' ') k++;
            we[nw++] = k;
        }

        /* which words are numbers, as received or received as letters */
        bool num[64] = { 0 }, conv[64] = { 0 }, hemi[64] = { 0 };
        int digits_real = 0, nconv = 0, nhemi = 0;
        for (int w = 0; w < nw; ++w) {
            int d = 0, letters = 0, figs = 0, other = 0;
            for (int k = ws[w]; k < we[w]; ++k) {
                char c = b->text[k];
                if (c >= '0' && c <= '9') d++;
                else if (c == '.' || c == ',' || c == '-' || c == '*' || c == '\'') ;
                else if (isupper((unsigned char)c)) { letters++; if (figs_char(c)) figs++; }
                else other++;
            }
            int len = we[w] - ws[w];
            /* "38", "17.00D", "12.133N" */
            if (d >= 2 && letters <= 1) {
                num[w] = true;
                digits_real += d;
                char last = b->text[we[w] - 1];
                if (letters == 1 && is_hemisphere(last)) hemi[w] = true;
            } else if (len == 1 && is_hemisphere(b->text[ws[w]])) {
                hemi[w] = true;
            } else if (len >= 2 && letters == figs && figs >= 2 && other == 0 &&
                       !is_common_word(b->text + ws[w], len)) {
                conv[w] = true;
            } else if (len >= 2 && is_hemisphere(b->text[ws[w]]) && b->text[ws[w] + 1] == '-') {
                hemi[w] = true;                     /* "K-" */
            }
            nhemi += hemi[w];
            nconv += conv[w];
        }
        int nnum = 0;
        for (int w = 0; w < nw; ++w) nnum += num[w];

        /* a position: numbers and a hemisphere letter, mostly numeric */
        if (!nhemi || nnum + nconv < 2 || (digits_real < 3 && nconv < 2)) continue;
        if ((nnum + nconv + nhemi) * 10 < nw * 6 && nnum < 4) continue;

        /* only the words next to the position (a number, a hemisphere or a word
         * already put back): not every word of a long line ("WE INFORM YOU") */
        bool ok[64] = { 0 };
        for (bool more = true; more; ) {
            more = false;
            for (int w = 0; w < nw; ++w) {
                if (!conv[w] || ok[w]) continue;
                bool l = w > 0 && (num[w - 1] || hemi[w - 1] || ok[w - 1]);
                bool r = w + 1 < nw && (num[w + 1] || hemi[w + 1] || ok[w + 1]);
                if (l || r) ok[w] = more = true;
            }
        }

        for (int w = 0; w < nw; ++w) {
            if (ok[w]) {
                for (int k = ws[w]; k < we[w]; ++k) {
                    int f = figs_char(b->text[k]);
                    if (f) { fixed[k] = (char)f; mark[k] = NB_MARK_FIXED; }
                    else mark[k] = NB_MARK_COORD;
                }
            } else if (num[w] || hemi[w]) {
                for (int k = ws[w]; k < we[w]; ++k) mark[k] = NB_MARK_COORD;
            }
        }
    }
}

/* ---- files ------------------------------------------------------------------------------ */

#include <stdio.h>

/* Written as PATH.tmp and renamed when complete: a copy received again
 * rewrites the bulletin, and a power-off during the save must not leave it
 * broken.  dialog_navtex_bul.inc deletes the .tmp left behind. */
bool nb_save(const nb_bulletin_t *b, const char *final_path)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s.tmp", final_path) >= (int)sizeof(path)) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "NAVTEX-BULLETIN 1\n");
    fprintf(f, "HEADER %c%c%02d\n", b->b1 ? b->b1 : '-', b->b2 ? b->b2 : '-', b->num >= 0 ? b->num : 99);
    fprintf(f, "KNOWN %d %d %d\n", b->b1 != 0, b->b2 != 0, b->num >= 0);
    fprintf(f, "LOST %d %d\n", b->lost_head, b->lost_tail);
    if (b->sitor) fprintf(f, "KIND SITORB\n");
    fprintf(f, "RX %lld %lld %d\n", (long long)b->first_rx, (long long)b->last_rx, b->receptions);
    fprintf(f, "QUALITY ");
    for (int i = 0; i < b->len; ++i) fputc('0' + (b->q[i] & 3), f);
    fprintf(f, "\nLEN %d\n", b->len);
    fwrite(b->text, 1, (size_t)b->len, f);
    fputc('\n', f);
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok && rename(path, final_path) == 0;
    if (!ok) remove(path);
    return ok;
}

bool nb_load(nb_bulletin_t *b, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    memset(b, 0, sizeof(*b));
    b->num = -1;
    b->receptions = 1;

    char line[128];
    bool ok = false;
    int kb1 = 1, kb2 = 1, knum = 1, qn = 0;
    char h[8] = "";
    static uint8_t qbuf[NB_MAX_TEXT];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "NAVTEX-BULLETIN", 15)) goto out;

    for (;;) {
        int c = fgetc(f);
        if (c == EOF) goto out;
        if (c == 'Q') {                 /* QUALITY: long line, read by hand */
            char tag[8];
            /* no space after %7s: with no text the line is "QUALITY \n", and
             * a space there would skip the newline and eat the LEN line */
            if (fscanf(f, "%7s", tag) != 1) goto out;
            c = fgetc(f);                   /* the one space after the tag */
            while (c != EOF && c != '\n') {
                c = fgetc(f);
                if (c != EOF && c != '\n' && qn < NB_MAX_TEXT) qbuf[qn++] = (uint8_t)(c - '0');
            }
            continue;
        }
        ungetc(c, f);
        if (!fgets(line, sizeof(line), f)) goto out;
        long long a1, a2;
        int r, l1, l2, n;
        if (sscanf(line, "HEADER %7s", h) == 1) continue;
        if (sscanf(line, "KNOWN %d %d %d", &kb1, &kb2, &knum) == 3) continue;
        if (sscanf(line, "LOST %d %d", &l1, &l2) == 2) { b->lost_head = l1; b->lost_tail = l2; continue; }
        if (!strncmp(line, "KIND SITORB", 11)) { b->sitor = true; continue; }
        if (sscanf(line, "RX %lld %lld %d", &a1, &a2, &r) == 3) {
            b->first_rx = (time_t)a1; b->last_rx = (time_t)a2; b->receptions = r; continue;
        }
        if (sscanf(line, "LEN %d", &n) == 1) {
            if (n < 0 || n > NB_MAX_TEXT) goto out;
            b->len = (int)fread(b->text, 1, (size_t)n, f);
            b->text[b->len] = '\0';
            ok = b->len == n;
            break;
        }
    }
    if (strlen(h) == 4) {
        if (kb1) b->b1 = h[0];
        if (kb2) b->b2 = h[1];
        if (knum && isdigit((unsigned char)h[2]) && isdigit((unsigned char)h[3]))
            b->num = (h[2] - '0') * 10 + (h[3] - '0');
    }
    for (int i = 0; i < b->len; ++i) b->q[i] = i < qn ? qbuf[i] : NAVTEX_Q_CLEAN;
out:
    fclose(f);
    return ok;
}
