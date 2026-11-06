/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - QSO state for the keyboard digital modes
 */

#include "digi_qso.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg/cfg_api.h"
#include "msg.h"

#define TEXT_LEN 32

static char    qso_dx[16];
static char    qso_name[TEXT_LEN];
static char    qso_qth[TEXT_LEN];
static time_t  qso_start;
static bool    qso_logged;
static time_t  qso_logged_at;
static bool    name_from_op;    /* taken after "OP": a NAME later replaces it */
static char    my_call[16];     /* ours, never taken as the DX call */
static uint8_t rst_s = 9;          /* the strength digit, 1..9 */
static uint8_t rst_r = 9;
static char    rst_s_str[8] = "599";
static char    rst_r_str[8] = "599";
static bool    autograb = true;

const char *digi_qso_dx(void) { return qso_dx; }

/* A new correspondent: nothing of the last QSO carries over. */
/* Contest serial numbers: the one we sent in this QSO (0: none yet) and
 * the one received. The next to send is p_digi_serial. */
static int     serial_sent;
static char    serial_rcvd[8];

static void new_qso(void) {
    qso_name[0] = '\0';
    name_from_op = false;
    qso_qth[0] = '\0';
    serial_sent = 0;
    serial_rcvd[0] = '\0';
    qso_logged = false;
    qso_start = time(NULL);
    rst_s = rst_r = 9;
    snprintf(rst_s_str, sizeof(rst_s_str), "5%u9", rst_s);
    snprintf(rst_r_str, sizeof(rst_r_str), "5%u9", rst_r);
}

/* The same call heard again this long after its QSO was logged is a new
 * QSO: a second contact later in the day. Sooner, it is the station's own
 * "73 DE CALL" at the end of the one just logged. */
#define SAME_CALL_NEW_QSO_S (10 * 60)

/* `typed`: the operator set it (a text window), so the same call again
 * after logging means a new QSO at once; from the text it does only
 * after SAME_CALL_NEW_QSO_S. */
static void set_dx(const char *call, bool typed) {
    char   up[sizeof(qso_dx)];
    size_t o = 0;

    for (const char *p = call ? call : ""; *p && o + 1 < sizeof(up); p++) {
        if (*p != ' ') {
            up[o++] = (char)toupper((unsigned char)*p);
        }
    }
    up[o] = '\0';
    if (strcmp(up, qso_dx) != 0) {
        memcpy(qso_dx, up, sizeof(qso_dx));
        new_qso();
    } else if (qso_logged && up[0] &&
               (typed || time(NULL) - qso_logged_at >= SAME_CALL_NEW_QSO_S)) {
        new_qso();      /* logged already: the name, QTH and reports were that QSO's */
    }
}

void digi_qso_set_dx(const char *call) { set_dx(call, true); }

/* Copy, without line breaks, which would end up in the middle of a
 * macro and in the log. */
static void copy_line(char *dst, size_t size, const char *src) {
    size_t o = 0;

    for (const char *p = src ? src : ""; *p && o + 1 < size; p++) {
        dst[o++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
    }
    while (o > 0 && dst[o - 1] == ' ') {
        o--;
    }
    dst[o] = '\0';
}

const char *digi_qso_name(void) { return qso_name; }
void digi_qso_set_name(const char *name) {
    copy_line(qso_name, sizeof(qso_name), name);
    name_from_op = false;
}
const char *digi_qso_qth(void) { return qso_qth; }
void digi_qso_set_qth(const char *qth) {
    copy_line(qso_qth, sizeof(qso_qth), qth);
}

time_t digi_qso_start(void) { return qso_start ? qso_start : time(NULL); }
bool digi_qso_logged(void) { return qso_logged; }
void digi_qso_set_logged(bool logged) {
    qso_logged = logged;
    qso_logged_at = logged ? time(NULL) : 0;
}

int digi_qso_rst_num(const char *rst) {
    int v = rst ? atoi(rst) : 0;

    return (v >= 111 && v <= 599) ? v : 599;
}

/* ---- contest serial numbers ------------------------------------------- */

int digi_qso_serial_next(void) { return param_i_get(cfg.digi_serial()); }

void digi_qso_set_serial_next(int n) {
    param_i_set(cfg.digi_serial(), n < 1 ? 1 : (n > 9999 ? 9999 : n));
}

int digi_qso_serial_sent(void) { return serial_sent; }

const char *digi_qso_serial_rcvd(void) { return serial_rcvd; }

void digi_qso_set_serial_rcvd(const char *nr) {
    size_t o = 0;

    for (const char *p = nr ? nr : ""; *p && o + 1 < sizeof(serial_rcvd); p++) {
        if (*p >= '0' && *p <= '9') {
            serial_rcvd[o++] = *p;
        }
    }
    serial_rcvd[o] = '\0';
}

void digi_qso_note_sent(const char *macro_text) {
    if (macro_text && strstr(macro_text, "<CNTR>") && serial_sent == 0) {
        serial_sent = digi_qso_serial_next();   /* the same one if sent again */
    }
}

void digi_qso_serial_used(int sent) {
    if (sent > 0 && digi_qso_serial_next() <= sent) {
        digi_qso_set_serial_next(sent + 1);
    }
}

void digi_qso_clear(void) {
    qso_dx[0] = '\0';
    new_qso();
}

const char *digi_qso_rst_sent(void) { return rst_s_str; }
const char *digi_qso_rst_rcvd(void) { return rst_r_str; }

static uint8_t rst_change(uint8_t *v, char *str, size_t sz, int16_t df) {
    if (df) {
        int n = (int)*v + (df > 0 ? 1 : -1);

        if (n < 1) n = 9;
        if (n > 9) n = 1;
        *v = (uint8_t)n;
        snprintf(str, sz, "5%u9", *v);
    }
    return *v;
}

uint8_t digi_qso_change_rst_sent(int16_t df) {
    return rst_change(&rst_s, rst_s_str, sizeof(rst_s_str), df);
}

uint8_t digi_qso_change_rst_rcvd(int16_t df) {
    return rst_change(&rst_r, rst_r_str, sizeof(rst_r_str), df);
}

void digi_qso_refresh_mycall(void) {
    char   call[PARAM_TEXT_MAX];
    size_t o = 0;

    param_t_get_into(cfg.callsign(), call, sizeof(call));
    for (const char *p = call; *p && o + 1 < sizeof(my_call); p++) {
        my_call[o++] = (char)toupper((unsigned char)*p);
    }
    my_call[o] = '\0';
}

/* SP2KMO, SP2KMO/P, SP/SP2KMO: all ours. */
static bool is_my_call(const char *w) {
    size_t n = strlen(my_call);
    size_t len = strlen(w);

    if (n == 0 || len < n) return false;
    for (size_t i = 0; i + n <= len; i++) {
        if (strncmp(w + i, my_call, n) == 0 && (i == 0 || w[i - 1] == '/') &&
            (w[i + n] == '\0' || w[i + n] == '/')) {
            return true;
        }
    }
    return false;
}

bool digi_qso_autograb(void) { return autograb; }

bool digi_qso_toggle_autograb(void) {
    autograb = !autograb;
    return autograb;
}

/* Does this word look like an amateur callsign? Requires a digit and at
 * least one letter after it - enough to reject ordinary words without
 * rejecting real calls, including the /P and /QRP forms. */
static bool looks_like_call(const char *w) {
    size_t len = strlen(w);
    bool   has_digit = false, letter_after_digit = false;

    if (len < 3 || len > 12) return false;

    for (size_t i = 0; i < len; i++) {
        char c = w[i];

        if (c >= '0' && c <= '9') {
            has_digit = true;
        } else if (c >= 'A' && c <= 'Z') {
            if (has_digit) letter_after_digit = true;
        } else if (c != '/') {
            return false;                 /* anything else disqualifies */
        }
    }
    return has_digit && letter_after_digit;
}

/* ---- name and QTH from the received text ---------------------------- */

/* Words that are never a name or a QTH, only the talk around them. */
static const char *const filler[] = {
    "IS", "HR", "HERE", "MY", "THE", "OP", "NAME", "QTH", "NR", "IN", "AT", "OF", "ES",
    "AND", "UR", "YR", "YOUR", "DE", "K", "KN", "BK", "SK", "TU", "TNX", "FB", "HI",
    "RST", "LOC", "LOCATOR", "GRID", "RIG", "ANT", "PWR", "WX", "HW", "BTU", "OM", "YL",
    "QSL", "CQ", "PSE", "DR", "OK", "R", "SO", "NEAR", "CITY", "TOWN", "OPERATOR",
    /* talk around "OP": "TNX FER QSO GUD OP", "OP HR" */
    "FER", "FOR", "GUD", "GD", "GOOD", "GM", "GA", "GE", "GN", "VY", "NICE", "AGN", "CPY",
    "CU", "CUL", "SRI", "NW", "ALL", "HPE", "WID", "WITH", "FROM", "TO", "BE", "QSO", NULL
};

static bool is_filler(const char *w) {
    for (int i = 0; filler[i]; i++) {
        if (strcmp(w, filler[i]) == 0) return true;
    }
    return false;
}

/* A name or a place: letters only (a hyphen allowed), two or more. */
static bool looks_like_word(const char *w) {
    size_t len = strlen(w);

    if (len < 2) return false;
    for (size_t i = 0; i < len; i++) {
        if (!(w[i] >= 'A' && w[i] <= 'Z') && w[i] != '-') return false;
    }
    return true;
}

/* "JOHN" -> "John", "ZIELONA GORA" -> "Zielona Gora". */
static void title_case(char *s) {
    bool start = true;

    for (; *s; s++) {
        if (*s == ' ' || *s == '-') {
            start = true;
        } else {
            *s = start ? (char)toupper((unsigned char)*s) : (char)tolower((unsigned char)*s);
            start = false;
        }
    }
}

/* The last words received that could be a name or a place, newest
 * first, for the QSO menu to offer. */
#define RECENT_MAX 12

static pthread_mutex_t recent_lock = PTHREAD_MUTEX_INITIALIZER;
static char            recent[RECENT_MAX][16];
static int             recent_n;

static void recent_add(const char *w) {
    int at = recent_n < RECENT_MAX ? recent_n : RECENT_MAX - 1;

    pthread_mutex_lock(&recent_lock);
    for (int i = 0; i < recent_n; i++) {
        if (strcmp(recent[i], w) == 0) {
            at = i;                     /* move it to the front */
            break;
        }
    }
    memmove(recent[1], recent[0], (size_t)at * sizeof(recent[0]));
    snprintf(recent[0], sizeof(recent[0]), "%s", w);
    title_case(recent[0]);
    if (at == recent_n && recent_n < RECENT_MAX) recent_n++;
    pthread_mutex_unlock(&recent_lock);
}

int digi_qso_recent_words(char (*out)[16], int max) {
    int n;

    pthread_mutex_lock(&recent_lock);
    n = recent_n < max ? recent_n : max;
    memcpy(out, recent, (size_t)n * sizeof(recent[0]));
    pthread_mutex_unlock(&recent_lock);
    return n;
}

/* "599", "579", "5NN": a report as sent in RTTY and the keyboard modes */
static bool looks_like_rst(const char *w) {
    return strlen(w) == 3 && w[0] == '5' && (isdigit((unsigned char)w[1]) || w[1] == 'N') &&
           (isdigit((unsigned char)w[2]) || w[2] == 'N');
}

/* "1", "012", "1234" */
static bool looks_like_serial(const char *w) {
    size_t len = strlen(w);

    if (len < 1 || len > 4) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)w[i])) return false;
    }
    return true;
}

/* ---- the decoder's text ------------------------------------------------- */

typedef enum { WANT_NONE, WANT_NAME, WANT_NAME_OP, WANT_QTH } want_t;

#define QTH_WORDS 3

/*
 * Assemble words from the decoded stream and pick out of them, when
 * auto pick-up is on:
 *   the call after "DE"            "SP2KMO DE SP3WAZ"
 *   the name after NAME or OP      "NAME IS JAN", "NAME HR JAN", "OP JAN"
 *   the QTH after QTH              "QTH IS ZIELONA GORA, LOC JO71"
 * Talk such as IS, HR, HERE between the keyword and the value is
 * skipped; a QTH is up to three words, ended by punctuation, a line break
 * or the next keyword. "UR NAME?" asks rather than tells and is ignored.
 * Each is taken once per QSO, into an empty field: the first mention is
 * the correspondent's, while later ones may be other stations talking,
 * and what was typed or picked in the menu stays.
 *
 * The call is taken only while the DX field is free: empty (after a CQ or
 * New QSO) or its QSO logged. Mid-QSO, two other stations on frequency
 * ("W1AW DE K2XX") would otherwise replace it, and the next report would
 * go to them. UI thread: the QSO fields are read there.
 */
void digi_qso_feed_rx(char c) {
    static char   word[16];
    static size_t wl = 0;
    static bool   after_de = false;
    static want_t want = WANT_NONE;
    static char   qth[sizeof(qso_qth)];
    static int    qth_words = 0;
    static char   rst[8];             /* the report just heard, "599" */
    bool          is_word;

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

    is_word = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-';
    if (is_word) {
        if (wl + 1 < sizeof(word)) word[wl++] = c;
        return;
    }

    if (wl == 0) {                        /* separator run */
        if (c != ' ' && want == WANT_QTH && qth_words > 0) {
            want = WANT_NONE;             /* ",", "." or a line break ends it */
            goto qth_done;
        }
        return;
    }
    word[wl] = '\0';
    wl = 0;

    if (looks_like_word(word) && !is_filler(word) && !looks_like_call(word)) {
        recent_add(word);
    }

    /* A contest exchange, "599 012 012": the number after the report is
     * the serial received, once per QSO. A repeated report is skipped. */
    if (looks_like_rst(word)) {
        if (rst[0] == '\0') {
            snprintf(rst, sizeof(rst), "%s", word);
        }
        return;
    }
    if (rst[0]) {
        if (autograb && serial_rcvd[0] == '\0' && looks_like_serial(word)) {
            digi_qso_set_serial_rcvd(word);
            msg_update_text_fmt("#FFFFFF Serial received: %s", serial_rcvd);
        }
        rst[0] = '\0';
    }

    if (strcmp(word, "DE") == 0) {
        after_de = true;
        return;
    }
    if (after_de) {
        after_de = false;
        if (autograb && (qso_dx[0] == '\0' || qso_logged) && looks_like_call(word) &&
            !is_my_call(word)) {
            bool was_logged = qso_logged;

            set_dx(word, false);
            if (!qso_logged || !was_logged) {
                msg_update_text_fmt("#FFFFFF DX: %s", qso_dx);
            }
        }
    }
    if (!autograb) {
        want = WANT_NONE;
        return;
    }

    /* a keyword starts a capture; a question mark right after it does not */
    if (strcmp(word, "NAME") == 0 || strcmp(word, "OP") == 0 || strcmp(word, "OPERATOR") == 0) {
        want_t next = (c == '?') ? WANT_NONE : (word[0] == 'N' ? WANT_NAME : WANT_NAME_OP);

        if (want == WANT_QTH && qth_words > 0) {
            want = next;
            goto qth_done;
        }
        want = next;
        return;
    }
    if (strcmp(word, "QTH") == 0) {
        want = (c == '?') ? WANT_NONE : WANT_QTH;
        qth[0] = '\0';
        qth_words = 0;
        return;
    }

    switch (want) {
        case WANT_NAME:
        case WANT_NAME_OP: {
            /* "OP" is often just talk ("GUD OP"): what follows it is taken
             * only into an empty field, and a NAME later replaces it */
            bool by_op = (want == WANT_NAME_OP);

            if (is_filler(word)) {
                if (by_op && strcmp(word, "IS") != 0 && strcmp(word, "HR") != 0 &&
                    strcmp(word, "HERE") != 0) {
                    want = WANT_NONE;         /* "OP FER...": no name */
                }
                return;                       /* "NAME IS", "OP HR" */
            }
            want = WANT_NONE;
            if (looks_like_word(word) && c != '?' &&
                (qso_name[0] == '\0' || (name_from_op && !by_op))) {
                snprintf(qso_name, sizeof(qso_name), "%s", word);
                title_case(qso_name);
                name_from_op = by_op;
                msg_update_text_fmt("#FFFFFF DX name: %s", qso_name);
            }
            return;
        }

        case WANT_QTH:
            if (qth_words == 0 && is_filler(word)) return;  /* "QTH IS" */
            if (!looks_like_word(word) || is_filler(word) || c == '?') {
                want = WANT_NONE;             /* "LOC", "RIG", a number... */
                break;
            }
            if (qth_words > 0) {
                const char *last = strrchr(qth, ' ');

                if (strcmp(last ? last + 1 : qth, word) == 0) {
                    want = WANT_NONE;         /* "QTH POZNAN POZNAN": said twice */
                    break;
                }
                strncat(qth, " ", sizeof(qth) - strlen(qth) - 1);
            }
            strncat(qth, word, sizeof(qth) - strlen(qth) - 1);
            if (++qth_words < QTH_WORDS && c == ' ') return;
            want = WANT_NONE;
            break;

        default:
            return;
    }

qth_done:
    if (qth_words > 0 && qso_qth[0] == '\0') {
        snprintf(qso_qth, sizeof(qso_qth), "%s", qth);
        title_case(qso_qth);
        msg_update_text_fmt("#FFFFFF DX QTH: %s", qso_qth);
    }
    qth_words = 0;
    qth[0] = '\0';
}

/* ---- macro tokens ---------------------------------------------------- */

enum { T_MYCALL, T_CALL, T_RSTR, T_RST, T_NAME, T_QTH, T_MYNAME, T_MYQTH, T_MYLOC, T_CNTR };

static const struct {
    const char *tok;
    int         id;
    const char *hint;
} toks[] = {
    {"<MYCALL>", T_MYCALL, "No callsign - set it in the Callsign app"},
    {"<CALL>",   T_CALL,   "No DX call - Menu > DX call"},
    {"<RSTR>",   T_RSTR,   "No RST received - Menu > QSO"},
    {"<RST>",    T_RST,    "No RST - Menu"},
    {"<NAME>",   T_NAME,   "No DX name - Menu > QSO > DX name"},
    {"<QTH>",    T_QTH,    "No DX QTH - Menu > QSO > DX QTH"},
    {"<MYNAME>", T_MYNAME, "Your name is not set - Menu > QSO > My name"},
    {"<MYQTH>",  T_MYQTH,  "Your QTH is not set - Menu > QSO > My QTH"},
    {"<MYLOC>",  T_MYLOC,  "No locator - set it in the QTH Grid app"},
    {"<CNTR>",   T_CNTR,   "No serial number"},
};

#define N_TOKS (sizeof(toks) / sizeof(toks[0]))

const char *digi_qso_missing_hint(const char *token) {
    for (size_t t = 0; token && t < N_TOKS; t++) {
        if (strcmp(token, toks[t].tok) == 0) {
            return toks[t].hint;
        }
    }
    return "A macro field is empty";
}

bool digi_qso_expand(const char *in, char *out, size_t out_sz, const char **missing) {
    char   mycall[PARAM_TEXT_MAX];
    char   myname[PARAM_TEXT_MAX];
    char   myqth[PARAM_TEXT_MAX];
    char   myloc[PARAM_TEXT_MAX];
    char   cntr[8];
    size_t o = 0;
    bool   ok = true;

    if (out == NULL || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (in == NULL) {
        return true;
    }
    param_t_get_into(cfg.callsign(), mycall, sizeof(mycall));
    digi_qso_refresh_mycall();
    param_t_get_into(cfg.op_name(), myname, sizeof(myname));
    param_t_get_into(cfg.op_qth(), myqth, sizeof(myqth));
    param_t_get_into(cfg.qth(), myloc, sizeof(myloc));
    /* the number this QSO was given, or the next one: "001" as contests send it */
    snprintf(cntr, sizeof(cntr), "%03d", serial_sent > 0 ? serial_sent : digi_qso_serial_next());

    for (const char *p = in; *p;) {
        bool matched = false;

        for (size_t t = 0; t < N_TOKS; t++) {
            size_t      len = strlen(toks[t].tok);
            const char *val = "";

            if (strncmp(p, toks[t].tok, len) != 0) {
                continue;
            }
            switch (toks[t].id) {
                case T_MYCALL: val = mycall; break;
                case T_CALL:   val = qso_dx; break;
                case T_RSTR:   val = rst_r_str; break;
                case T_RST:    val = rst_s_str; break;
                case T_NAME:   val = qso_name; break;
                case T_QTH:    val = qso_qth; break;
                case T_MYNAME: val = myname; break;
                case T_MYQTH:  val = myqth; break;
                case T_MYLOC:  val = myloc; break;
                case T_CNTR:   val = cntr; break;
            }
            if (val[0] == '\0') {
                if (ok && missing) {
                    *missing = toks[t].tok;
                }
                ok = false;
            }
            for (const char *v = val; *v && o + 1 < out_sz; v++) {
                out[o++] = *v;
            }
            p += len;
            matched = true;
            break;
        }
        if (!matched) {
            if (o + 1 < out_sz) {
                out[o++] = *p;
            }
            p++;
        }
    }
    out[o] = '\0';
    return ok;
}
