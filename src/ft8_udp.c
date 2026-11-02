/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSJT-X compatible UDP broadcaster
 */

#include "ft8_udp.h"

#include "bt_ctl.h"
#include "bt_spp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#define CONF_PATH        "/mnt/ft8_udp.conf"

#define WSJTX_MAGIC      0xadbccbdaU
#define WSJTX_SCHEMA     3U          /* schema 3 is what WSJT-X 2.x emits */

/* Message type numbers from the WSJT-X NetworkMessage protocol */
#define MSG_HEARTBEAT    0U
#define MSG_STATUS       1U
#define MSG_DECODE       2U
#define MSG_CLEAR        3U
#define MSG_QSO_LOGGED   5U
#define MSG_LOGGED_ADIF  12U

#define BUF_MAX          1024

typedef struct {
    uint8_t  data[BUF_MAX];
    size_t   len;
    bool     overflow;
} qbuf_t;

static int      sock_fd  = -1;
static bool     enabled  = false;
/* True between ft8_udp_init() and ft8_udp_deinit(), i.e. while the FT8
 * window is open. Outside that the Bluetooth window may still read and
 * flip the UDP switch; it then works on the config file only. */
static bool     running  = false;
static char     host[64] = "239.255.0.0,gateway";
static uint16_t port     = 2237;
static char     wsjt_id[32] = "X6100";
/* Several destinations at once. Multicast is the tidy answer on a normal
 * LAN, but when the phone itself is the access point Android will not
 * hand multicast from its SoftAP interface to local apps - the app joins
 * the group on the default route, which is usually cellular. A plain
 * unicast to the phone always arrives, so the config may list more than
 * one target and every packet goes to all of them. UDP is cheap. */
#define MAX_DEST 4
static struct sockaddr_in dest[MAX_DEST];
static int    dest_n = 0;

/* ==================================================================== *
 *  Qt QDataStream compatible serialisation (big endian)
 * ==================================================================== */

static void qb_reset(qbuf_t *b) {
    b->len = 0;
    b->overflow = false;
}

static void qb_raw(qbuf_t *b, const void *src, size_t n) {
    if (b->len + n > BUF_MAX) { b->overflow = true; return; }
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void qb_u8(qbuf_t *b, uint8_t v) {
    qb_raw(b, &v, 1);
}

static void qb_bool(qbuf_t *b, bool v) {
    qb_u8(b, v ? 1 : 0);
}

static void qb_u32(qbuf_t *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    qb_raw(b, t, 4);
}

static void qb_i32(qbuf_t *b, int32_t v) {
    qb_u32(b, (uint32_t)v);
}

static void qb_u64(qbuf_t *b, uint64_t v) {
    qb_u32(b, (uint32_t)(v >> 32));
    qb_u32(b, (uint32_t)(v & 0xffffffffU));
}

/* Qt serialises double as IEEE-754 big endian */
static void qb_double(qbuf_t *b, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    qb_u64(b, bits);
}

/* QString: quint32 byte length + UTF-8 payload; 0xffffffff means null */
static void qb_str(qbuf_t *b, const char *s) {
    if (!s) { qb_u32(b, 0xffffffffU); return; }
    size_t n = strlen(s);
    qb_u32(b, (uint32_t)n);
    qb_raw(b, s, n);
}

/* QTime: milliseconds since midnight (UTC) */
static void qb_time_now_utc(qbuf_t *b) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm_utc;
    time_t secs = tv.tv_sec;
    gmtime_r(&secs, &tm_utc);

    uint32_t msecs = (uint32_t)((tm_utc.tm_hour * 3600 + tm_utc.tm_min * 60 +
                                tm_utc.tm_sec) * 1000 + tv.tv_usec / 1000);
    qb_u32(b, msecs);
}

/* Common header for every message */
static void qb_header(qbuf_t *b, uint32_t msg_type) {
    qb_reset(b);
    qb_u32(b, WSJTX_MAGIC);
    qb_u32(b, WSJTX_SCHEMA);
    qb_u32(b, msg_type);
    qb_str(b, wsjt_id);
}

/* Every emitter returns at once unless the datagram has somewhere to go:
 * UDP, or a phone on Bluetooth SPP (bt_spp.c), which carries the same
 * datagrams and has to work with UDP off. So the hooks in the FT8 window
 * cost nothing when nobody listens. */
static bool emitters_on(void) {
    return (enabled && sock_fd >= 0) ||
           (bt_spp_state() == BT_SPP_CONNECTED);
}

static void qb_send(qbuf_t *b) {
    if (b->overflow || b->len == 0 || !emitters_on()) return;

    if (enabled && sock_fd >= 0) {
        for (int i = 0; i < dest_n; i++) {
            sendto(sock_fd, b->data, b->len, 0,
                   (struct sockaddr *)&dest[i], sizeof(dest[i]));
        }
    }

    /* Queued for the SPP thread; returns at once when no phone is
     * connected. */
    bt_spp_send(b->data, b->len);
}

/* ==================================================================== *
 *  Config
 * ==================================================================== */

static void conf_write(void) {
    FILE *f = fopen(CONF_PATH, "w");
    if (!f) return;
    fprintf(f, "# X6100 WSJT-X UDP broadcaster\n");
    fprintf(f, "# host: one or more targets separated by commas.\n");
    fprintf(f, "#   239.255.0.0  multicast, works on a normal LAN\n");
    fprintf(f, "#   gateway      resolved at start; on a phone hotspot\n");
    fprintf(f, "#                this is the phone, and Android does not\n");
    fprintf(f, "#                deliver multicast from its own hotspot\n");
    fprintf(f, "#   1.2.3.4      any fixed address\n");
    fprintf(f, "enabled=%d\n", enabled ? 1 : 0);
    fprintf(f, "host=%s\n", host);
    fprintf(f, "port=%u\n", (unsigned)port);
    fprintf(f, "id=%s\n", wsjt_id);
    fclose(f);
}

static void conf_read(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        /* First run: enabled. Multicast plus the default gateway is
         * harmless on any network - with no listener the datagrams are
         * simply dropped - and an image handed to someone else should
         * work without editing files over SSH. Setting enabled=0 here,
         * or the UDP switch in the Bluetooth window, turns it off. */
        enabled = true;
        conf_write();
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "enabled") == 0) {
            enabled = (atoi(val) != 0);
        } else if (strcmp(key, "host") == 0) {
            strncpy(host, val, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        } else if (strcmp(key, "port") == 0) {
            int p = atoi(val);
            if (p > 0 && p < 65536) port = (uint16_t)p;
        } else if (strcmp(key, "id") == 0) {
            strncpy(wsjt_id, val, sizeof(wsjt_id) - 1);
            wsjt_id[sizeof(wsjt_id) - 1] = '\0';
        }
    }
    fclose(f);
}

/* ==================================================================== *
 *  Socket setup
 * ==================================================================== */

/* Default gateway from the routing table. On a phone hotspot this is the
 * phone, which is exactly where the packets need to go. */
static bool default_gateway(char *out, size_t out_sz) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return false;

    char line[256];
    bool found = false;
    /* skip header */
    if (fgets(line, sizeof(line), f)) {
        while (fgets(line, sizeof(line), f)) {
            char iface[32];
            unsigned long dst = 0, gw = 0;
            if (sscanf(line, "%31s %lx %lx", iface, &dst, &gw) != 3) continue;
            if (dst != 0 || gw == 0) continue;          /* want 0.0.0.0 */
            struct in_addr a;
            a.s_addr = (in_addr_t)gw;                   /* already net order */
            snprintf(out, out_sz, "%s", inet_ntoa(a));
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static bool is_multicast(const char *ip) {
    struct in_addr a;
    if (inet_aton(ip, &a) == 0) return false;
    uint32_t h = ntohl(a.s_addr);
    return (h >= 0xE0000000U && h <= 0xEFFFFFFFU);   /* 224.0.0.0/4 */
}

static void socket_open(void) {
    if (sock_fd >= 0) { close(sock_fd); sock_fd = -1; }

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) return;

    int on = 1;
    /* allow broadcast destinations */
    setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    /* Non-blocking is essential: these sends happen on the UI thread (and
     * the decoder thread), and a blocking sendto can stall it if the
     * socket buffer fills or the network hiccups - which freezes the
     * whole radio. Telemetry that cannot be queued is simply dropped,
     * which is the right trade. */
    int fl = fcntl(sock_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(sock_fd, F_SETFL, fl | O_NONBLOCK);

    /* host may be a comma separated list; the word "gateway" resolves to
     * the current default gateway, so one config works both on a router
     * and on a phone hotspot without editing anything. */
    memset(dest, 0, sizeof(dest));
    dest_n = 0;

    char list[sizeof(host)];
    strncpy(list, host, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';

    for (char *tok = strtok(list, ","); tok && dest_n < MAX_DEST;
         tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;

        char resolved[64];
        if (strcmp(tok, "gateway") == 0) {
            if (!default_gateway(resolved, sizeof(resolved))) continue;
        } else {
            snprintf(resolved, sizeof(resolved), "%s", tok);
        }

        struct sockaddr_in *d = &dest[dest_n];
        d->sin_family = AF_INET;
        d->sin_port   = htons(port);
        if (inet_aton(resolved, &d->sin_addr) == 0) continue;

        if (is_multicast(resolved)) {
            unsigned char ttl = 2;
            setsockopt(sock_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
            unsigned char loop = 1;
            setsockopt(sock_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
                       sizeof(loop));
        }
        dest_n++;
    }

    if (dest_n == 0) {                       /* nothing usable: broadcast */
        dest[0].sin_family = AF_INET;
        dest[0].sin_port   = htons(port);
        dest[0].sin_addr.s_addr = htonl(INADDR_BROADCAST);
        dest_n = 1;
    }
}

void ft8_udp_init(void) {
    running = true;
    conf_read();
    socket_open();
    /* The Bluetooth bridge carries the same datagrams, so it lives and
     * dies with this module rather than needing its own hook in
     * main_screen.c. It reads /mnt/bt.conf and brings the adapter and
     * the SPP server up only if the Bluetooth window left them on. */
    bt_ctl_init();
}

void ft8_udp_deinit(void) {
    running = false;
    bt_ctl_deinit();
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
}

bool ft8_udp_is_enabled(void) {
    /* With the FT8 window closed nothing has read the file yet, or it
     * may have been edited by hand since. */
    if (!running) conf_read();
    return enabled;
}

void ft8_udp_set_enabled(bool on) {
    if (!running) conf_read();
    if (enabled == on) return;
    enabled = on;
    conf_write();
    /* The socket belongs to the FT8 window; opening it here with the
     * window closed would leave it dangling until the next init. */
    if (running && on && sock_fd < 0) socket_open();
}

const char *ft8_udp_host(void) { return host; }
uint16_t    ft8_udp_port(void) { return port; }

/* ==================================================================== *
 *  Message emitters
 * ==================================================================== */

void ft8_udp_send_heartbeat(void) {
    if (!emitters_on()) return;
    qbuf_t b;
    qb_header(&b, MSG_HEARTBEAT);
    qb_u32(&b, WSJTX_SCHEMA);      /* max schema we speak */
    qb_str(&b, "2.6.1");           /* version string      */
    qb_str(&b, "x6100");           /* revision            */
    qb_send(&b);
}

void ft8_udp_send_status(uint64_t dial_freq_hz, const char *mode,
                         const char *de_call, const char *de_grid,
                         const char *dx_call, uint32_t rx_df, uint32_t tx_df,
                         bool transmitting, bool decoding) {
    if (!emitters_on()) return;
    qbuf_t b;
    qb_header(&b, MSG_STATUS);
    qb_u64(&b, dial_freq_hz);
    qb_str(&b, mode ? mode : "FT8");
    qb_str(&b, dx_call ? dx_call : "");
    qb_str(&b, "");                    /* report            */
    qb_str(&b, mode ? mode : "FT8");   /* tx mode           */
    qb_bool(&b, false);                /* tx enabled        */
    qb_bool(&b, transmitting);
    qb_bool(&b, decoding);
    qb_u32(&b, rx_df);
    qb_u32(&b, tx_df);
    qb_str(&b, de_call ? de_call : "");
    qb_str(&b, de_grid ? de_grid : "");
    qb_str(&b, "");                    /* dx grid           */
    qb_bool(&b, false);                /* tx watchdog       */
    qb_str(&b, "");                    /* sub mode          */
    qb_bool(&b, false);                /* fast mode         */
    qb_send(&b);
}

void ft8_udp_send_decode(bool is_new, const char *mode, const char *message,
                         int32_t snr, float delta_t_sec, uint32_t df_hz) {
    if (!emitters_on() || !message || !*message) return;

    qbuf_t b;
    qb_header(&b, MSG_DECODE);
    qb_bool(&b, is_new);
    qb_time_now_utc(&b);
    qb_i32(&b, snr);
    qb_double(&b, (double)delta_t_sec);
    qb_u32(&b, df_hz);
    qb_str(&b, mode ? mode : "~");     /* WSJT-X sends "~" for FT8 */
    qb_str(&b, message);
    qb_bool(&b, false);                /* low confidence    */
    qb_bool(&b, false);                /* off air           */
    qb_send(&b);
}

void ft8_udp_send_clear(void) {
    if (!emitters_on()) return;
    qbuf_t b;
    qb_header(&b, MSG_CLEAR);
    qb_send(&b);
}

/* Qt QDate is serialised as a qint64 Julian Day number. */
static int64_t julian_day(int year, int month, int day) {
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    return (int64_t)day + (153 * m + 2) / 5 + 365LL * y + y / 4 - y / 100 +
           y / 400 - 32045;
}

/* QDateTime: QDate (qint64 JDN) + QTime (quint32 ms) + quint8 timespec.
 * timespec 1 == Qt::UTC, which is what WSJT-X uses. */
static void qb_datetime_utc(qbuf_t *b, time_t when) {
    struct tm tm_utc;
    gmtime_r(&when, &tm_utc);

    int64_t jdn = julian_day(tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                             tm_utc.tm_mday);
    qb_u64(b, (uint64_t)jdn);
    qb_u32(b, (uint32_t)((tm_utc.tm_hour * 3600 + tm_utc.tm_min * 60 +
                          tm_utc.tm_sec) * 1000));
    qb_u8(b, 1);   /* Qt::UTC */
}

void ft8_udp_send_qso_logged(time_t when, const char *dx_call, const char *dx_grid,
                             uint64_t tx_freq_hz, const char *mode,
                             const char *report_sent, const char *report_rcvd,
                             const char *tx_power, const char *comments,
                             const char *name, const char *my_call,
                             const char *my_grid) {
    if (!emitters_on()) return;

    qbuf_t b;
    qb_header(&b, MSG_QSO_LOGGED);
    qb_datetime_utc(&b, when);              /* date/time off */
    qb_str(&b, dx_call ? dx_call : "");
    qb_str(&b, dx_grid ? dx_grid : "");
    qb_u64(&b, tx_freq_hz);
    qb_str(&b, mode ? mode : "FT8");
    qb_str(&b, report_sent ? report_sent : "");
    qb_str(&b, report_rcvd ? report_rcvd : "");
    qb_str(&b, tx_power ? tx_power : "");
    qb_str(&b, comments ? comments : "");
    qb_str(&b, name ? name : "");
    qb_datetime_utc(&b, when);              /* date/time on  */
    qb_str(&b, my_call ? my_call : "");     /* operator call */
    qb_str(&b, my_call ? my_call : "");     /* my call       */
    qb_str(&b, my_grid ? my_grid : "");
    qb_str(&b, "");                         /* exchange sent */
    qb_str(&b, "");                         /* exchange rcvd */
    qb_str(&b, "");                         /* ADIF propagation mode */
    qb_send(&b);
}

/* ==================================================================== *
 *  ADIF record construction
 * ==================================================================== */

const char *ft8_udp_band_name(uint64_t f) {
    /* ADIF band names for the HF/6m ranges this radio covers. */
    if (f >=  1800000ULL && f <=  2000000ULL) return "160m";
    if (f >=  3500000ULL && f <=  4000000ULL) return "80m";
    if (f >=  5250000ULL && f <=  5450000ULL) return "60m";
    if (f >=  7000000ULL && f <=  7300000ULL) return "40m";
    if (f >= 10100000ULL && f <= 10150000ULL) return "30m";
    if (f >= 14000000ULL && f <= 14350000ULL) return "20m";
    if (f >= 18068000ULL && f <= 18168000ULL) return "17m";
    if (f >= 21000000ULL && f <= 21450000ULL) return "15m";
    if (f >= 24890000ULL && f <= 24990000ULL) return "12m";
    if (f >= 28000000ULL && f <= 29700000ULL) return "10m";
    if (f >= 50000000ULL && f <= 54000000ULL) return "6m";
    return "";
}

/* Append "<name:len>value" only when the value is non-empty. An empty
 * field written as "<name:0>" is not valid ADIF and can make a parser
 * discard the whole record, which is why this guard matters. */
static void adif_add(char *out, size_t out_sz, size_t *o,
                     const char *name, const char *value) {
    if (!value || !*value) return;
    int n = snprintf(out + *o, out_sz - *o, "<%s:%d>%s",
                     name, (int)strlen(value), value);
    if (n > 0 && (size_t)n < out_sz - *o) *o += (size_t)n;
}

size_t ft8_udp_build_adif(char *out, size_t out_sz, time_t when,
                          const char *dx_call, const char *dx_grid,
                          uint64_t freq_hz, const char *mode,
                          int snr_sent, int snr_rcvd, int tx_power_w,
                          const char *my_call, const char *my_grid) {
    struct tm tm_utc;
    gmtime_r(&when, &tm_utc);

    /* Sized for any int, so the compiler can prove nothing truncates. */
    char date[40], tstr[40], freq_str[32], rs[12], rr[12], pwr[12];
    snprintf(date, sizeof(date), "%04d%02d%02d",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
    snprintf(tstr, sizeof(tstr), "%02d%02d%02d",
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    snprintf(freq_str, sizeof(freq_str), "%.6f", (double)freq_hz / 1000000.0);
    snprintf(rs, sizeof(rs), "%+d", snr_sent);
    snprintf(rr, sizeof(rr), "%+d", snr_rcvd);
    snprintf(pwr, sizeof(pwr), "%d", tx_power_w);

    size_t o = 0;
    int n = snprintf(out, out_sz,
                     "X6100 ADIF export\n<adif_ver:5>3.1.0\n"
                     "<programid:5>X6100\n<EOH>\n");
    if (n < 0 || (size_t)n >= out_sz) { if (out_sz) out[0] = '\0'; return 0; }
    o = (size_t)n;

    adif_add(out, out_sz, &o, "call", dx_call);
    adif_add(out, out_sz, &o, "gridsquare", dx_grid);
    adif_add(out, out_sz, &o, "mode", mode);
    adif_add(out, out_sz, &o, "rst_sent", rs);
    adif_add(out, out_sz, &o, "rst_rcvd", rr);
    adif_add(out, out_sz, &o, "qso_date", date);
    adif_add(out, out_sz, &o, "time_on", tstr);
    adif_add(out, out_sz, &o, "qso_date_off", date);
    adif_add(out, out_sz, &o, "time_off", tstr);
    adif_add(out, out_sz, &o, "band", ft8_udp_band_name(freq_hz));
    adif_add(out, out_sz, &o, "freq", freq_str);
    adif_add(out, out_sz, &o, "station_callsign", my_call);
    adif_add(out, out_sz, &o, "my_gridsquare", my_grid);
    adif_add(out, out_sz, &o, "tx_pwr", pwr);

    n = snprintf(out + o, out_sz - o, "<eor>\n");
    if (n > 0 && (size_t)n < out_sz - o) o += (size_t)n;
    return o;
}

void ft8_udp_log_qso(time_t when, const char *dx_call, const char *dx_grid,
                     uint64_t freq_hz, const char *mode,
                     int snr_sent, int snr_rcvd, int tx_power_w,
                     const char *my_call, const char *my_grid) {
    if (!emitters_on()) return;

    char rs[12], rr[12], pwr[12];
    snprintf(rs, sizeof(rs), "%+d", snr_sent);
    snprintf(rr, sizeof(rr), "%+d", snr_rcvd);
    snprintf(pwr, sizeof(pwr), "%d", tx_power_w);

    ft8_udp_send_qso_logged(when, dx_call, dx_grid, freq_hz, mode,
                            rs, rr, pwr, "", "", my_call, my_grid);

    char adif[768];
    if (ft8_udp_build_adif(adif, sizeof(adif), when, dx_call, dx_grid,
                           freq_hz, mode, snr_sent, snr_rcvd, tx_power_w,
                           my_call, my_grid) > 0) {
        ft8_udp_send_logged_adif(adif);
    }
}

void ft8_udp_send_logged_adif(const char *adif_record) {
    if (!emitters_on() || !adif_record) return;
    qbuf_t b;
    qb_header(&b, MSG_LOGGED_ADIF);
    qb_str(&b, adif_record);
    qb_send(&b);
}
