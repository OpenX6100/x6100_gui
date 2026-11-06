/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSPR: spots to a file, to WSPRnet, and who heard you
 */

#define _GNU_SOURCE

#include "wspr_net.h"

#include "cfg/cfg_api.h"
#include "scheduler.h"
#include "wspr_map.h"

#include <ctype.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SPOTS_FILE      "/mnt/wspr_spots.txt"
#define SPOTS_OLD       "/mnt/wspr_spots.txt.old"
#define SPOTS_MAX_BYTES (2L * 1024 * 1024)

#ifndef WSPRNET_HOST
#define WSPRNET_HOST    "wsprnet.org"
#define WSPRLIVE_HOST   "db1.wspr.live"
#define HTTP_PORT       "80"
#endif
#define VERSION_TAG     "OpenX6100"
#define HTTP_TIMEOUT_S  10
#define POST_GAP_MS     200     /* between spots, as WSJT-X */

#define QUEUE_MAX       64
#define POST_LEN        320
#define HEARD_MAX       100

typedef enum { NET_OK, NET_DOWN, NET_REFUSED } net_err_t;

static pthread_mutex_t mux  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static bool            thread_up;

/* Form bodies waiting for wsprnet.org, oldest at q_head. */
static char     queue[QUEUE_MAX][POST_LEN];
static int      q_head, q_n;
static uint32_t q_pops;         /* heads removed: one in flight that was
                                 * dropped meanwhile isn't removed twice */
static bool     upload_now;     /* a slot ended: send what is queued */
static int      slot_spots;     /* spots queued in the slot now ending */

/* The slot's lines for wspr_spots.txt, written once it ends (worker thread) */
static char     file_buf[4096];
static size_t   file_len;

static char     fetch_sql[512];
static char     fetch_call[16];
static bool     fetch_pending;
static uint32_t gen;            /* answers of an older request are dropped */

static uint32_t  sent_total;
static net_err_t net_err;

typedef struct {
    uint32_t gen;
    char     call[13];
    char     grid[9];
    int      snr;
} heard_t;

/* ---- Small helpers ---- */

static bool call_ok(const char *c) {
    size_t n = strlen(c);
    if (n < 3 || n > 12) return false;
    for (; *c; c++)
        if (!isupper((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '/') return false;
    return true;
}

/* Form/URL encoding: everything but the unreserved characters. */
static void url_enc(char *out, size_t cap, const char *in) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = '\0';
}

/* Start of the slot that just ended: decoding finishes about 116 s into
 * it, so a minute back, rounded down to the even minute, is inside it. */
void wspr_net_slot_time(char date[16], char hhmm[16]) {
    time_t    t = time(NULL) - 60;
    struct tm tm;
    t -= t % 120;
    gmtime_r(&t, &tm);
    strftime(date, 16, "%y%m%d", &tm);
    strftime(hhmm, 16, "%H%M", &tm);
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Enough to report from: a call, and a grid WSPRnet can place us by. */
static bool me_ok(const wspr_net_station_t *me) {
    return call_ok(me->call) && strlen(me->grid) >= 4;
}

/* ---- HTTP/1.0 over a socket ---- */

static char resp[16384];

/* Dechunk in place (a server may still chunk); returns the new length. */
static size_t dechunk(char *b, size_t n) {
    size_t in = 0, out = 0;
    while (in < n) {
        char         *end;
        unsigned long len = strtoul(b + in, &end, 16);
        char         *crlf = strstr(end, "\r\n");
        if (!crlf || len == 0) break;
        in = (size_t)(crlf + 2 - b);
        if (in + len > n) len = n - in;
        memmove(b + out, b + in, len);
        out += len;
        in += len + 2;
    }
    b[out] = '\0';
    return out;
}

/* Send `req` to host:80; the reply body ends up NUL-terminated in resp.
 * Returns the HTTP status, or -1 when the host can't be reached. */
static int http(const char *host, const char *req) {
    struct addrinfo hints = {0}, *res, *ai;
    struct timeval  tv = {HTTP_TIMEOUT_S, 0};
    int             fd = -1, status = 0;
    size_t          n = 0, len = strlen(req);
    char           *body;

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, HTTP_PORT, &hints, &res) != 0) return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    for (size_t s = 0; s < len;) {
        ssize_t k = send(fd, req + s, len - s, MSG_NOSIGNAL);
        if (k <= 0) { close(fd); return -1; }
        s += (size_t)k;
    }
    for (;;) {
        ssize_t k = recv(fd, resp + n, sizeof(resp) - 1 - n, 0);
        if (k <= 0) break;
        n += (size_t)k;
        if (n == sizeof(resp) - 1) break;
    }
    close(fd);
    resp[n] = '\0';

    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1) return -1;
    body = strstr(resp, "\r\n\r\n");
    if (!body) { resp[0] = '\0'; return status; }
    *body = '\0';
    bool chunked = strcasestr(resp, "transfer-encoding: chunked") != NULL;
    body += 4;
    n -= (size_t)(body - resp);
    memmove(resp, body, n + 1);
    if (chunked) dechunk(resp, n);
    return status;
}

/* ---- The thread ---- */

static void heard_cb(void *arg) {
    heard_t *h = arg;
    pthread_mutex_lock(&mux);
    bool current = h->gen == gen;
    pthread_mutex_unlock(&mux);
    if (current) wspr_map_heard_you(h->call, h->grid, h->snr);
}

/* One TSV line per station: rx_sign, rx_loc, best SNR. */
static void parse_heard(uint32_t g, const char *me) {
    int   n = 0;
    char *save, *line = strtok_r(resp, "\n", &save);
    for (; line && n < HEARD_MAX; line = strtok_r(NULL, "\n", &save)) {
        heard_t h = {.gen = g};
        if (sscanf(line, "%12[^\t]\t%8[^\t]\t%d", h.call, h.grid, &h.snr) != 3) continue;
        if (!call_ok(h.call) || strcmp(h.call, me) == 0) continue;
        scheduler_put(heard_cb, &h, sizeof(h));
        n++;
    }
}

static void *net_main(void *arg) {
    static char req[2048];
    (void)arg;

    pthread_mutex_lock(&mux);
    for (;;) {
        while (!(upload_now && q_n > 0) && !fetch_pending)
            pthread_cond_wait(&cond, &mux);

        if (upload_now && q_n > 0) {
            char     body[POST_LEN];
            uint32_t pops = q_pops;
            snprintf(body, sizeof(body), "%s", queue[q_head]);
            pthread_mutex_unlock(&mux);

            snprintf(req, sizeof(req),
                     "POST /post/ HTTP/1.0\r\nHost: " WSPRNET_HOST "\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\n"
                     "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
            int  st   = http(WSPRNET_HOST, req);
            bool spot = strncmp(body, "function=wspr&", 14) == 0;

            pthread_mutex_lock(&mux);
            if (st < 0 || st >= 500) {
                /* Not reachable: keep it for the next slot. */
                net_err    = NET_DOWN;
                upload_now = false;
            } else {
                /* Answered: done with it either way. A spot it didn't
                 * take ("N spot(s) added" missing) isn't sent again.
                 * Unless WSPRnet off or a full queue dropped it already. */
                if (q_pops == pops) {
                    q_head = (q_head + 1) % QUEUE_MAX;
                    q_n--;
                    q_pops++;
                }
                if (st == 200 && (!spot || strstr(resp, "added"))) {
                    net_err = NET_OK;
                    if (spot) sent_total++;
                } else {
                    net_err = NET_REFUSED;
                }
                pthread_mutex_unlock(&mux);
                sleep_ms(POST_GAP_MS);
                pthread_mutex_lock(&mux);
            }
            if (q_n == 0) upload_now = false;
            continue;
        }

        if (fetch_pending) {
            char     enc[1024], me[16];
            uint32_t g = gen;
            fetch_pending = false;
            url_enc(enc, sizeof(enc), fetch_sql);
            snprintf(me, sizeof(me), "%s", fetch_call);
            pthread_mutex_unlock(&mux);

            snprintf(req, sizeof(req),
                     "GET /?query=%s HTTP/1.0\r\nHost: " WSPRLIVE_HOST "\r\n\r\n", enc);
            int st = http(WSPRLIVE_HOST, req);
            if (st == 200) parse_heard(g, me);

            pthread_mutex_lock(&mux);
            if (st < 0) net_err = NET_DOWN;
        }
    }
    return NULL;
}

/* Called with mux held. */
static void kick(void) {
    if (!thread_up) {
        pthread_t      t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        thread_up = pthread_create(&t, &a, net_main, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    pthread_cond_signal(&cond);
}

/* Called with mux held. A full queue drops its oldest. */
static void enqueue(const char *body) {
    if (q_n == QUEUE_MAX) {
        q_head = (q_head + 1) % QUEUE_MAX;
        q_n--;
        q_pops++;
    }
    snprintf(queue[(q_head + q_n) % QUEUE_MAX], POST_LEN, "%s", body);
    q_n++;
}

/* ---- The file ---- */

static void flush_spots(void) {
    if (!file_len) return;
    struct stat st;
    if (stat(SPOTS_FILE, &st) == 0 && st.st_size > SPOTS_MAX_BYTES) rename(SPOTS_FILE, SPOTS_OLD);
    FILE *f = fopen(SPOTS_FILE, "a");
    if (f) {
        fwrite(file_buf, 1, file_len, f);
        fclose(f);
    }
    file_len = 0;
}

/* Kept until the slot ends: no card writes during the decode. */
static void save_spot(const char *date, const char *hhmm, const wspr_spot_t *s, double mhz) {
    if (file_len > sizeof(file_buf) - 128) flush_spots();
    /* wsprd's wspr_spots.txt: date time sync snr dt MHz call grid dBm
     * drift cycles jitter (sync, cycles, jitter not measured here: 0) */
    int n = snprintf(file_buf + file_len, sizeof(file_buf) - file_len,
                     "%s %s %2d %4d %5.1f %11.6f  %-6s %-4s %2d %4d %4d %4d\n", date, hhmm, 0,
                     (int)lroundf(s->snr_db), (double)s->dt_s, mhz, s->callsign, s->locator, s->power_dbm, 0,
                     0, 0);
    if (n > 0 && (size_t)n < sizeof(file_buf) - file_len) file_len += (size_t)n;
}

/* ---- API ---- */

bool wspr_net_enabled(void) {
    return param_i_get(cfg.wspr.net()) != 0;
}

void wspr_net_set_enabled(bool on) {
    param_i_set(cfg.wspr.net(), on ? 1 : 0);
    pthread_mutex_lock(&mux);
    if (!on) {
        q_pops    += (uint32_t)q_n;
        q_n        = 0;
        upload_now = false;
        net_err    = NET_OK;
    }
    pthread_mutex_unlock(&mux);
}

/* WSPR's tone spacing: 12000 / 8192 Hz */
#define TONE_SPACING_HZ 1.4648

void wspr_net_spot(const wspr_spot_t *spot, const wspr_net_station_t *me) {
    char   date[16], hhmm[16], body[POST_LEN], rcall[48], tcall[32];
    double rqrg = me->dial_hz / 1e6;
    /* The decoder gives the lowest of the 4 tones; WSJT-X and WSPRnet
     * use their centre, 1.5 tones up. */
    double tqrg = (me->dial_hz + (double)spot->freq_hz + 1.5 * TONE_SPACING_HZ) / 1e6;

    /* A type 1 message's power ends in 0, 3 or 7. Others are type 2/3
     * (compound calls), which the decoder reads as type 1: the grid and
     * power it gives are made up, so they go neither to the file nor to
     * WSPRnet. */
    int d = spot->power_dbm % 10;
    if (d != 0 && d != 3 && d != 7) return;

    wspr_net_slot_time(date, hhmm);
    save_spot(date, hhmm, spot, tqrg);

    if (!wspr_net_enabled() || !me_ok(me)) return;
    url_enc(rcall, sizeof(rcall), me->call);
    url_enc(tcall, sizeof(tcall), spot->callsign);
    snprintf(body, sizeof(body),
             "function=wspr&rcall=%s&rgrid=%s&rqrg=%.6f&date=%s&time=%s&sig=%d&dt=%.1f"
             "&drift=0&tqrg=%.6f&tcall=%s&tgrid=%s&dbm=%d&version=" VERSION_TAG "&mode=2",
             rcall, me->grid, rqrg, date, hhmm, (int)lroundf(spot->snr_db), (double)spot->dt_s, tqrg,
             tcall, spot->locator, spot->power_dbm);

    pthread_mutex_lock(&mux);
    enqueue(body);
    slot_spots++;
    pthread_mutex_unlock(&mux);
}

void wspr_net_slot_end(const wspr_net_station_t *me, bool transmitted) {
    char body[POST_LEN], rcall[48];

    pthread_mutex_lock(&mux);
    bool none = slot_spots == 0;
    slot_spots = 0;
    pthread_mutex_unlock(&mux);

    flush_spots();
    if (!wspr_net_enabled() || !me_ok(me)) return;

    pthread_mutex_lock(&mux);
    /* No spots: tell WSPRnet we're listening, as WSJT-X does. Not for a
     * slot we sent in, nor behind a backlog where it'd be out of date. */
    if (none && !transmitted && q_n == 0) {
        url_enc(rcall, sizeof(rcall), me->call);
        snprintf(body, sizeof(body),
                 "function=wsprstat&rcall=%s&rgrid=%s&rqrg=%.6f&tpct=%d&tqrg=%.6f&dbm=%d"
                 "&version=" VERSION_TAG "&mode=2",
                 rcall, me->grid, me->dial_hz / 1e6, me->tpct, (me->dial_hz + me->tx_hz) / 1e6,
                 me->dbm);
        enqueue(body);
    }
    if (q_n > 0) {
        upload_now = true;
        kick();
    }
    pthread_mutex_unlock(&mux);
}

void wspr_net_fetch_heard(const char *call, uint32_t dial_hz) {
    if (!wspr_net_enabled() || !call_ok(call) || dial_hz < 1000000) return;
    pthread_mutex_lock(&mux);
    gen++;
    snprintf(fetch_call, sizeof(fetch_call), "%s", call);
    /* wspr.live's band is the whole MHz (14 for 20 m); its table is
     * ordered by time and band, so both go in every query. */
    snprintf(fetch_sql, sizeof(fetch_sql),
             "SELECT rx_sign, rx_loc, max(snr) FROM wspr.rx WHERE time > now() - INTERVAL 1 HOUR "
             "AND band = %u AND tx_sign = '%s' GROUP BY rx_sign, rx_loc ORDER BY max(snr) DESC "
             "LIMIT %d FORMAT TSV",
             (unsigned)(dial_hz / 1000000), call, HEARD_MAX);
    fetch_pending = true;
    kick();
    pthread_mutex_unlock(&mux);
}

void wspr_net_forget(void) {
    pthread_mutex_lock(&mux);
    gen++;
    pthread_mutex_unlock(&mux);
}

const char *wspr_net_state(void) {
    static char buf[16];
    if (!wspr_net_enabled()) return "off";
    pthread_mutex_lock(&mux);
    net_err_t e = net_err;
    uint32_t  n = sent_total;
    pthread_mutex_unlock(&mux);
    if (e == NET_DOWN) return "no net";
    if (e == NET_REFUSED) return "refused";
    if (n == 0) return "on";
    snprintf(buf, sizeof(buf), "%u sent", (unsigned)n);
    return buf;
}
