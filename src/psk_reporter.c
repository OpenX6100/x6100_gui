/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - who hears you, live from PSK Reporter
 *
 *  A small MQTT 3.1.1 client: QoS 0, one subscription. The image has no MQTT
 *  library and this needs five packet types.
 */

#include "psk_reporter.h"

#include "scheduler.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef PSKR_HOST
#define PSKR_HOST    "mqtt.pskreporter.info"
#define PSKR_PORT    "1883"
#endif
#define IO_TIMEOUT_S 10
#define KEEPALIVE_S  60
#define PING_S       30
#define SILENCE_S    100 /* nothing at all, not even a ping's answer: gone */
#define RETRY_S      90  /* as GridTracker */
#define RX_MAX       2048
#define TOPIC_LEN    96

#define MQTT_CONNECT     0x10
#define MQTT_PUBLISH     3
#define MQTT_SUBSCRIBE   0x82
#define MQTT_SUBACK      9
#define MQTT_UNSUBSCRIBE 0xA2
#define MQTT_PINGREQ     0xC0
#define MQTT_DISCONNECT  0xE0

typedef struct {
    uint32_t gen;
    char     call[13];
    char     grid[7];
    int      snr;
    int64_t  when;
} spot_t;

typedef struct {
    uint32_t gen;
    bool     up;
} state_t;

static pthread_mutex_t mux  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static bool            thread_up;

static char              want_topic[TOPIC_LEN]; /* "" stopped */
static char              want_me[16], want_band[8], want_mode[8];
static uint32_t          gen; /* what's from an older request is dropped */
static psk_reporter_cb_t cbs;

static const struct {
    int32_t     lo, hi;
    const char *name;
} bands[] = {
    {1800000, 2000000, "160m"},   {3500000, 4000000, "80m"},    {5250000, 5450000, "60m"},
    {7000000, 7300000, "40m"},    {10100000, 10150000, "30m"},  {14000000, 14350000, "20m"},
    {18068000, 18168000, "17m"},  {21000000, 21450000, "15m"},  {24890000, 24990000, "12m"},
    {28000000, 29700000, "10m"},  {50000000, 54000000, "6m"},
};

const char *psk_reporter_band(int32_t hz) {
    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++)
        if (hz >= bands[i].lo && hz <= bands[i].hi) return bands[i].name;
    return NULL;
}

/* ---- To the GUI ---- */

static void spot_cb(void *arg) {
    spot_t *s = arg;
    pthread_mutex_lock(&mux);
    bool                    current = s->gen == gen;
    const psk_reporter_cb_t cb      = cbs;
    pthread_mutex_unlock(&mux);
    if (current && cb.on_spot) cb.on_spot(s->call, s->grid, s->snr, s->when);
}

static void state_cb(void *arg) {
    state_t *s = arg;
    pthread_mutex_lock(&mux);
    bool                    current = s->gen == gen;
    const psk_reporter_cb_t cb      = cbs;
    pthread_mutex_unlock(&mux);
    if (current && cb.on_state) cb.on_state(s->up);
}

static void tell_state(uint32_t g, bool up) {
    state_t s = {.gen = g, .up = up};
    scheduler_put(state_cb, &s, sizeof(s));
}

/* ---- The reports ---- */

/* {"sq":1,"f":14075076,"md":"FT8","rp":1,"t":1794157439,"sc":"KB0KU","sl":"DN40AI","rc":"W8XY","rl":"EN90GR",...,"b":"20m"} */
static bool json_str(const char *j, const char *key, char *out, size_t size) {
    char pat[12];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t n = 0;
    for (; *p && *p != '"' && n + 1 < size; p++) out[n++] = *p;
    out[n] = '\0';
    return *p == '"';
}

static bool json_num(const char *j, const char *key, long long *out) {
    char pat[12];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    char *end;
    *out = strtoll(p, &end, 10);
    return end != p;
}

/* Upper case, "." back to "/"; false if it isn't a callsign. */
static bool clean_call(char *c) {
    size_t n = strlen(c);
    if (n < 3 || n > 12) return false;
    for (; *c; c++) {
        if (*c == '.') *c = '/';
        *c = (char)toupper((unsigned char)*c);
        if (!isupper((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '/') return false;
    }
    return true;
}

/* Upper case, 6 at most; false without a usable 4 (as GridTracker). */
static bool clean_grid(char *g) {
    g[strnlen(g, 6)] = '\0';
    size_t n         = strlen(g);
    for (char *c = g; *c; c++) *c = (char)toupper((unsigned char)*c);
    if (n < 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) ||
        !isdigit((unsigned char)g[3]))
        return false;
    if (n != 6 || !isupper((unsigned char)g[4]) || !isupper((unsigned char)g[5])) g[4] = '\0';
    return true;
}

static void on_report(char *json, uint32_t g, const char *me, const char *band, const char *mode) {
    spot_t    s = {.gen = g};
    char      sc[16], grid[12], b[8], md[8];
    long long rp, t;
    if (!json_str(json, "rc", s.call, sizeof(s.call)) || !json_str(json, "sc", sc, sizeof(sc)) ||
        !json_str(json, "rl", grid, sizeof(grid)) || !json_str(json, "b", b, sizeof(b)) ||
        !json_str(json, "md", md, sizeof(md)) || !json_num(json, "rp", &rp) || !json_num(json, "t", &t))
        return;
    /* From a subscription just left, or a station spotting itself: not for us */
    if (!clean_call(s.call) || !clean_call(sc) || strcmp(sc, me) != 0 || strcmp(s.call, me) == 0) return;
    if (strcasecmp(b, band) != 0 || strcasecmp(md, mode) != 0) return;
    if (!clean_grid(grid) || rp < -60 || rp > 60) return;
    snprintf(s.grid, sizeof(s.grid), "%.6s", grid);
    s.snr  = (int)rp;
    s.when = t;
    scheduler_put(spot_cb, &s, sizeof(s));
}

/* ---- MQTT over a socket ---- */

static bool send_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* Every packet we send is under 128 bytes: a one-byte length. */
static bool send_packet(int fd, uint8_t type, const uint8_t *body, size_t n) {
    uint8_t b[130];
    if (n > 127) return false;
    b[0] = type;
    b[1] = (uint8_t)n;
    if (n) memcpy(b + 2, body, n);
    return send_all(fd, b, n + 2);
}

static size_t put_str(uint8_t *p, const char *s) {
    size_t n = strlen(s);
    p[0]     = (uint8_t)(n >> 8);
    p[1]     = (uint8_t)n;
    memcpy(p + 2, s, n);
    return n + 2;
}

static int open_broker(void) {
    struct addrinfo hints = {0}, *res, *ai;
    struct timeval  tv = {IO_TIMEOUT_S, 0};
    int             fd = -1;

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(PSKR_HOST, PSKR_PORT, &hints, &res) != 0) return -1;
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
    return fd;
}

/* CONNECT, and its CONNACK. */
static bool mqtt_hello(int fd, const char *me) {
    uint8_t b[96], ack[4];
    char    id[48];
    size_t  n = put_str(b, "MQTT");
    b[n++]    = 4;              /* 3.1.1 */
    b[n++]    = 2;              /* clean session */
    b[n++]    = 0;
    b[n++]    = KEEPALIVE_S;
    snprintf(id, sizeof(id), "X6100_%s_%06x", me, (unsigned)(rand() & 0xffffff));
    for (char *c = id; *c; c++)
        if (*c == '/') *c = '.';
    n += put_str(b + n, id);
    if (!send_packet(fd, MQTT_CONNECT, b, n)) return false;
    return recv(fd, ack, sizeof(ack), MSG_WAITALL) == sizeof(ack) && ack[0] == 0x20 && ack[3] == 0;
}

static bool mqtt_topic(int fd, uint8_t type, uint16_t id, const char *topic) {
    uint8_t b[TOPIC_LEN + 8];
    size_t  n = 0;
    b[n++]    = (uint8_t)(id >> 8);
    b[n++]    = (uint8_t)id;
    n += put_str(b + n, topic);
    if (type == MQTT_SUBSCRIBE) b[n++] = 0; /* QoS 0 */
    return send_packet(fd, type, b, n);
}

typedef struct {
    uint8_t  rx[RX_MAX + 1];
    size_t   len;
    uint32_t gen;
    char     me[16], band[8], mode[8];
} session_t;

/* What came in, packet by packet; false ends the connection. */
static bool take_packets(session_t *s) {
    for (;;) {
        size_t used = 1, rem = 0, mul = 1;
        bool   whole = false;
        while (used < s->len && used <= 4) {
            uint8_t b = s->rx[used++];
            rem += (size_t)(b & 127) * mul;
            mul *= 128;
            if (!(b & 128)) {
                whole = true;
                break;
            }
        }
        if (!whole) return used <= 4; /* more to come, or a length that can't be */
        if (used + rem > RX_MAX) return false;
        if (s->len < used + rem) return true;

        uint8_t *body = s->rx + used;
        int      type = s->rx[0] >> 4;
        if (type == MQTT_PUBLISH && rem >= 2) {
            size_t tl  = ((size_t)body[0] << 8) | body[1];
            size_t qos = (s->rx[0] >> 1) & 3;
            size_t at  = 2 + tl + (qos ? 2 : 0);
            if (at <= rem) {
                /* The byte after it is the next packet's or spare: put back */
                uint8_t keep    = body[rem];
                body[rem]       = 0;
                on_report((char *)body + at, s->gen, s->me, s->band, s->mode);
                body[rem] = keep;
            }
        } else if (type == MQTT_SUBACK && rem >= 3) {
            if (body[2] & 0x80) return false;
            tell_state(s->gen, true);
        }
        s->len -= used + rem;
        memmove(s->rx, s->rx + used + rem, s->len);
    }
}

/* Seconds that a clock change (Time Sync, GPS) doesn't move. */
static time_t mono_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

/* Connected: follows what's wanted until it's stopped (true) or the
 * connection is lost (false). */
static bool session(int fd) {
    static session_t s;
    char             topic[TOPIC_LEN] = "";
    uint16_t         id               = 0;
    time_t           last_rx          = mono_s(), last_ping = last_rx;
    bool             started          = false;

    s.len = 0;
    for (;;) {
        pthread_mutex_lock(&mux);
        if (!want_topic[0]) {
            pthread_mutex_unlock(&mux);
            send_packet(fd, MQTT_DISCONNECT, NULL, 0);
            return true;
        }
        if (!started || s.gen != gen) {
            char next[TOPIC_LEN];
            snprintf(next, sizeof(next), "%s", want_topic);
            s.gen = gen;
            snprintf(s.me, sizeof(s.me), "%s", want_me);
            snprintf(s.band, sizeof(s.band), "%s", want_band);
            snprintf(s.mode, sizeof(s.mode), "%s", want_mode);
            pthread_mutex_unlock(&mux);
            if (topic[0] && !mqtt_topic(fd, MQTT_UNSUBSCRIBE, ++id, topic)) return false;
            if (!mqtt_topic(fd, MQTT_SUBSCRIBE, ++id, next)) return false;
            snprintf(topic, sizeof(topic), "%s", next);
            started = true;
        } else {
            pthread_mutex_unlock(&mux);
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int           r   = poll(&pfd, 1, 1000);
        time_t        now = mono_s();
        if (r < 0 && errno != EINTR) return false;
        if (r > 0) {
            ssize_t k = recv(fd, s.rx + s.len, RX_MAX - s.len, 0);
            if (k <= 0) return false;
            s.len += (size_t)k;
            last_rx = now;
            if (!take_packets(&s)) return false;
        }
        if (now - last_rx > SILENCE_S) return false;
        if (now - last_ping >= PING_S) {
            if (!send_packet(fd, MQTT_PINGREQ, NULL, 0)) return false;
            last_ping = now;
        }
    }
}

static bool wanted(uint32_t *g, char *me, size_t size) {
    pthread_mutex_lock(&mux);
    bool on = want_topic[0] != '\0';
    if (g) *g = gen;
    if (me) snprintf(me, size, "%s", want_me);
    pthread_mutex_unlock(&mux);
    return on;
}

static void *net_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mux);
        while (!want_topic[0]) pthread_cond_wait(&cond, &mux);
        pthread_mutex_unlock(&mux);

        char     me[16];
        uint32_t g;
        wanted(&g, me, sizeof(me));
        int  fd      = open_broker();
        bool stopped = false;
        if (fd >= 0) {
            if (mqtt_hello(fd, me)) stopped = session(fd);
            close(fd);
        }
        if (stopped || !wanted(&g, NULL, 0)) continue;
        tell_state(g, false);
        for (int i = 0; i < RETRY_S && wanted(NULL, NULL, 0); i++) sleep(1);
    }
    return NULL;
}

void psk_reporter_watch(const char *call, const char *band, const char *mode, const psk_reporter_cb_t *cb) {
    char me[16] = "", topic[TOPIC_LEN] = "";
    if (call && band && mode) {
        snprintf(me, sizeof(me), "%s", call);
        if (clean_call(me)) {
            char dotted[16];
            snprintf(dotted, sizeof(dotted), "%s", me);
            for (char *c = dotted; *c; c++)
                if (*c == '/') *c = '.';
            snprintf(topic, sizeof(topic), "pskr/filter/v2/%s/%s/%s/#", band, mode, dotted);
        }
    }
    pthread_mutex_lock(&mux);
    if (strcmp(topic, want_topic) != 0) {
        gen++;
        snprintf(want_topic, sizeof(want_topic), "%s", topic);
        snprintf(want_me, sizeof(want_me), "%s", me);
        snprintf(want_band, sizeof(want_band), "%s", band ? band : "");
        snprintf(want_mode, sizeof(want_mode), "%s", mode ? mode : "");
    }
    if (cb) cbs = *cb;
    if (topic[0]) {
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
    pthread_mutex_unlock(&mux);
}
