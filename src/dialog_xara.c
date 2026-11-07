/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *
 *  XARA: runs the xara modem (VARA HF compatible, github.com/x0rro/xara)
 *  as its own program and shows what it is doing. A phone app (WoAD for
 *  Winlink, APRSdroid on the KISS port) connects to xara over WiFi.
 *
 *  Nothing here knows the VARA protocol: the app starts /usr/bin/xara,
 *  keys the radio when xara asks (a rigctld-style "T 1" / "T 0" on a
 *  local port), sends at a fixed transmit level the operator sets with the
 *  MFK knob (the codec's play volume: xara plays at a fixed level), reads
 *  xara's status feed (one JSON line per update on its local web port)
 *  and draws JS8's waterfall: receive audio, and while keyed the audio
 *  going out (the output's PulseAudio monitor), marked red at the edge.
 *  xara answers no call unless Settings > Auto answer is on and a phone
 *  app is connected on the VARA port (xara's --answer-gate, our patch:
 *  without it xara answers every call to our callsign from the start).
 *  The VARA and KISS ports take local addresses only (--local-only, our
 *  patch): the VARA protocol has no password, and a client from outside
 *  the home network could otherwise make the radio connect and transmit.
 *  Phones and laptops on the same WiFi connect as before; the screen
 *  shows the address of what is connected.
 *
 *  Monitor runs xara-monitor instead of xara: it decodes VARA traffic
 *  between other stations and never transmits. Record has xara write
 *  what it hears, what it sends and PTT to a FLAC file on the SD card.
 *
 *  VARA is the modem of Jose Alberto Nieto Ros, EA5HVK (rosmodem.wordpress.com).
 *  xara is x0rro's independent, compatible program (MIT) and is not his.
 *  Settings > Full speed uses VARA's registered speed levels: only with an
 *  xara built with them (BR2_PACKAGE_XARA_REGISTERED) and a VARA licence.
 */

#define _GNU_SOURCE /* accept4 */
#include "dialog_xara.h"

#include "audio.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "diag_log.h"
#include "dialog.h"
#include "dsp.h"
#include "events.h"
#include "js8_wf.h"
#include "keyboard.h"
#include "lock_manager.h"
#include "main_screen.h"
#include "msg.h"
#include "radio.h"
#include "spectrum.h"
#include "styles.h"
#include "tx_info.h"
#include "tx_player.h"
#include "waterfall.h"

#include "lvgl/lvgl.h"

#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <liquid/liquid.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define XARA_BIN        "/usr/bin/xara"
#define XARA_LOG        "/tmp/xara.log"
#define XARA_MON_BIN    "/usr/bin/xara-monitor"
#define XARA_MON_LOG    "/tmp/xara-monitor.log"
#define REC_DIR         "/mnt/xara-rec" /* DATA partition */
#define REC_KEEP        10              /* the newest recordings kept, the rest deleted */
#define REC_MIN_FREE_MB 500             /* no recording with less free on the card */
#define REC_STOP_MB     100             /* recording stops at this much free */
#define MON_LINES       3               /* the monitor's newest lines on screen */
#define XARA_CAPTURE    "alsa_input.platform-sound.stereo-fallback"
#define XARA_PLAYBACK   "alsa_output.platform-sound.stereo-fallback"
#define VARA_CMD_PORT   8300 /* the phone connects here (and to 8301 for data) */
#define KISS_PORT       8100
#define PTT_PORT        4532 /* local only: xara keys the radio through us */
#define UI_PORT         8515 /* local only: xara's status feed */
#define BT_KISS_CHANNEL 3    /* Bluetooth RFCOMM: 1 is CAT, 2 the WSJT-X frames (bt_ctl.c) */
#define BT_STORE        "/var/lib/bluetooth" /* BlueZ: <adapter>/<device>/info, [LinkKey] once paired */
#define XARA_DRIVE_DB   -1   /* xara's own level: fixed; the app sets the codec's */

/* 100-3000 Hz: every xara bandwidth fits, centred on 1500 Hz (500: 1250-
 * 1750, 2300: 350-2650, 2750: 125-2875). The TX filter goes down to 50. */
#define FILTER_LOW      100  /* the waterfall's span, and the widest RX filter */
#define FILTER_HIGH     3000
#define RX_MARGIN       150  /* the RX filter: xara's passband and this much each side */
#define TX_FILTER_LOW   100
#define TX_FILTER_HIGH  3000
#define PTT_MAX_S       35 /* xara drops PTT after 30 s itself; this is the backstop */
#define SWR_TRIP        3.0f /* JS8's guard: above this for SWR_TRIP_MS while sending, TX stops */
#define SWR_TRIP_MS     500
#define VOL_BACK_US     3000000 /* the play volume goes back this long after the last unkey */

/* Transmit level: the codec's play volume while keyed (audio_set_play_vol),
 * fixed, set by the operator with the MFK knob (also while sending). JS8's
 * automatic level (tx_level.c) was used before build 9: on the bench
 * (2026-10-28) each 18 s APRS burst faded ~8 dB against the noise while
 * xara's own audio stayed constant, and real VARA decoded none of them
 * over the air but all of xara's clean audio. Suspect: VARA's peaks
 * kept that loop turning the level down. Kept while the radio is on. */
/* -7 dB: worked out from the radio's FT8 calibration (ft8_output_gain_offset
 * -7.7 dB: FT8's tone, 0.8 of full scale less 7.7 dB, gives 5 W with the ALC
 * just showing), xara plays 5.3 dB louder than that tone, so -5.3 dB would be
 * ~5 W at the ALC's edge; -7 leaves room (~3-3.5 W, ALC quiet). Untested. */
#define LEVEL_START_DB  -7.0f
#define LEVEL_FILE      "/mnt/xara.conf" /* DATA: tx_level_db=, lead_ms=, audio_ms= (send), rx_audio_ms= (one a line) */
#define LEVEL_MIN_DB    -40.0f
#define LEVEL_MAX_DB    0.0f
#define LEVEL_STEP_DB   0.5f
#define LEVEL_TICK_US   100000 /* ALC and power readings for the screen */

/* Waterfall, as JS8's (dialog_js8.c): 12 kHz audio, 15 rows a second,
 * drawn on the display's lower plane by js8_wf.c, relative to the noise
 * floor over WF_MIN_DB..WF_MAX_DB. */
#define SAMPLE_RATE     12000
#define WF_ROWS_PER_SEC 15
#define WF_ROW_SAMPLES  (SAMPLE_RATE / WF_ROWS_PER_SEC)
#define WF_QUEUE        16
#define WF_TICK_MS      5
#define WF_MIN_DB       0
#define WF_MAX_DB       30
#define TX_EDGE_PX      6 /* the red edge on rows of our own transmission */
#define WF_CALM_NEW     0.65f /* JS8's "Light" calm: each receive row blends 65 % new into the ones before */

/* Layout, in the dialog (DIALOG_WIDTH x DIALOG_HEIGHT, 796 x 345) */
#define INSET           4
#define WIDTH           (DIALOG_WIDTH - 2 * INSET) /* 788: waterfall columns */
#define TOP_Y           2
#define WF_Y            36
#define WF_H            130
#define X               20

extern char **environ;

/* What the status feed reports, shared with the GUI timer. */
typedef struct {
    bool   seen;      /* a status line came in */
    char   state[24]; /* xara's link state: Listen, Connecting, Receiving, ... */
    char   peer[24];
    char   bw[8];
    int    level;
    int    bps;
    bool   have_bps;
    double sn;
    bool   have_sn;
    double afc;       /* xara's frequency correction (AFC), Hz */
    bool   have_afc;
    int    buffer;
    double rx_db;
    bool   busy;
    int    pass_lo, pass_hi; /* the passband in audio Hz */
    char   last_line[80];    /* the last host line: CONNECTED ..., DISCONNECTED */
} xara_status_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb();
static void key_cb(lv_event_t *e);
static void band_cb(lv_event_t *e);
static void band_entered(void);
static void rec_space_tick(void);
static void stop_tx(void);
static bool linked(void);
static void xara_start(void);
static void xara_stop(void);
static void bw_cb(button_data_t *btn);
static void aprs_cb(button_data_t *btn);
static void monitor_cb(button_data_t *btn);
static void settings_cb(button_data_t *btn);
static void restart_cb(button_data_t *btn);

static const char *bw_label(void);
static const char *aprs_label(void);
static const char *monitor_label(void);

static button_data_t btn_bw = {
    .type     = BTN_TEXT_FN,
    .label_fn = bw_label,
    .press    = bw_cb,
};
static button_data_t btn_aprs = {
    .type     = BTN_TEXT_FN,
    .label_fn = aprs_label,
    .press    = aprs_cb,
};
static button_data_t btn_monitor = {
    .type     = BTN_TEXT_FN,
    .label_fn = monitor_label,
    .press    = monitor_cb,
};
static button_data_t btn_settings = {
    .type  = BTN_TEXT,
    .label = "Settings...",
    .press = settings_cb,
};
static button_data_t btn_restart = {
    .type  = BTN_TEXT,
    .label = "Restart\nmodem",
    .press = restart_cb,
};

static buttons_page_t btn_page = {
    {&btn_bw, &btn_aprs, &btn_monitor, &btn_settings, &btn_restart}
};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &btn_page,
    .key_cb       = NULL, /* added in construct_cb, as the GPS window does */
};

dialog_t *dialog_xara = &dialog;

/* Kept while the radio is on. */
static int   bw_hz       = 2300;
static float level_db    = LEVEL_START_DB;
static bool  level_loaded;          /* LEVEL_FILE read (once per radio start) */
static bool  level_dirty;           /* changed since it was saved */
static lv_obj_t *level_item;      /* Settings' TX level line, while the list is open */
static bool      level_editing;   /* that line pressed: the MFK knob sets the level */
static _Atomic float tx_alc, tx_pwr; /* the last readings while keyed, for the screen */
static atomic_bool   tx_meter;       /* readings have come in this transmission */
static bool  ignore_busy = false; /* xara's busy detector bypassed (SIGUSR1 / SIGUSR2) */
static bool  busy_sent;           /* this xara run has been told */
static bool  record_on   = true;  /* Settings: Record (on while we test xara against VARA) */
static bool  auto_answer;         /* Settings: Auto answer; off every time XARA opens */
static bool  full_speed;          /* Settings: Full speed (registered); off every time XARA opens */
static int   lead_idx    = 4;     /* Settings: PTT lead, lead_ms[] (100 ms: the base keys 60-90 ms after MODEM ON, ptt.log) */
static int   buf_idx     = 2;     /* Settings: send buffer, buf_ms[] (40 ms: clean twice on the bench, 2026-10-28) */
static int   rbuf_idx    = 2;     /* Settings: receive buffer, buf_ms[] (40 ms; smaller answers sooner) */
static int   prio_idx    = 0;     /* Settings: CPU priority, prio_names[] (test, 2026-10-31) */
static _Atomic float tx_max_pwr;  /* the highest power read while keyed, this transmission */
static _Atomic float last_max_pwr;/* ... and of the last one (0: none yet) */
static atomic_int    wf_lo_hz = 100, wf_hi_hz = 3000; /* the RX filter, for the waterfall's floor */
static bool  gate_open;           /* this xara run's answer gate, as last signalled */
static bool  monitoring;          /* xara-monitor runs instead of xara */
static char  rec_path[96];        /* this xara run's recording, "" if none */
static char  log_path[100] = XARA_LOG; /* xara's log: beside the recording when recording (kept
                                        * over a power-off, 2026-11-01), else XARA_LOG */
static long     log_off;          /* how far XARA_LOG has been read */
static unsigned gaps_tx, gaps_rx; /* audio dropouts xara logged this run: playback / capture */
static int   aprs_idx    = -1;    /* the last APRS preset picked */

/* VARA HF APRS: USB, 500 Hz (dial frequencies). */
static const struct {
    const char *label;
    uint32_t    hz;
} aprs_presets[] = {
    {"40m", 7083500},
    {"30m", 10148200},
};
#define APRS_PRESETS (sizeof(aprs_presets) / sizeof(aprs_presets[0]))

static lv_obj_t   *settings_list;
static bool        settings_was_editing; /* the knob's mode before the list opened */
static lv_obj_t   *top_label, *flags_label, *state_label, *link_label, *level_label, *phone_label, *last_label, *tx_label;
static lv_obj_t   *wf_box, *pass_lo_line, *pass_hi_line;
static lv_timer_t *timer, *wf_timer;
static Observer   *wf_watch[4];

static pid_t           xara_pid = -1;
static pid_t           mon_pid  = -1;
static int             mon_fd   = -1; /* the monitor's output (a pipe) */
static pthread_t       mon_thread;
static bool            mon_thread_up;
static atomic_bool     mon_stop;
static pthread_mutex_t mon_lock = PTHREAD_MUTEX_INITIALIZER;
static char            mon_lines[MON_LINES][100]; /* newest first */
static unsigned        mon_count;                 /* lines decoded since Monitor started */
static pthread_t       ptt_thread, stream_thread, level_thread, txmon_thread, btkiss_thread;
static pthread_mutex_t bt_lock = PTHREAD_MUTEX_INITIALIZER;
static char            bt_note[64]; /* the Bluetooth KISS line, for the screen */
static bool            threads_up;
static atomic_bool     stop_threads;
static atomic_bool     keyed;
static atomic_long     keyed_since;
static atomic_bool     level_restore; /* unkeyed: the play volume goes back */
static atomic_bool     vol_is_level;  /* the play volume is level_db: a key needn't open the mixer */
static _Atomic float   swr_tripped;   /* level_main: the SWR that stopped TX, for timer_cb */
static pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER; /* keying, unkeying and the play volume, one at a time */
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static xara_status_t   status;
static unsigned        status_gen; /* status_clear(): a parse under way doesn't write the old one back */
static Observer       *dsp_watch[4];
static bool            rec_full; /* the card filled: Record turned off, the modem restarts once unlinked */
static char            shown_bw[8];

static int32_t saved_filter_low, saved_filter_high;
static bool    filter_saved;
static int32_t saved_mode; /* the band's own mode, put back on close (XARA runs USB-D) */
static int32_t saved_band; /* the band saved_mode belongs to */

static uint32_t        rx_sub = AUDIO_SUB_INVALID;
static pthread_mutex_t wf_lock = PTHREAD_MUTEX_INITIALIZER;
static spgramf         sg;
static uint16_t        nfft;
static float          *psd;
static unsigned        wf_row_fill;
static bool            wf_row_tx; /* the row being filled is our own transmission */
static float           wf_row[WIDTH], wf_sel[WIDTH];
static float           wf_floor_db;
static float           wf_avg[WIDTH];
static bool            wf_avg_set;
static bool            wf_floor_set;
static float           wf_rows[WF_QUEUE][WIDTH];
static bool            wf_rows_tx[WF_QUEUE];
static unsigned        wf_q_head, wf_q_count;
static int64_t         wf_due_us;

/* ---------------------------------------------------------------- helpers */

/* Both RX filter edges; the edge that moves out first, so low never passes
 * high (each edge is clipped against the other). */
static void filter_set(int32_t lo, int32_t hi) {
    if (cparam_i_get(cfg.filter.low()) == lo && cparam_i_get(cfg.filter.high()) == hi) return;
    if (hi > cparam_i_get(cfg.filter.high())) {
        cparam_i_set(cfg.filter.high(), hi);
        cparam_i_set(cfg.filter.low(), lo);
    } else {
        cparam_i_set(cfg.filter.low(), lo);
        cparam_i_set(cfg.filter.high(), hi);
    }
}

/* The radio's RX filter for the bandwidth in use: xara's passband (500:
 * 1250-1750, 2300: 350-2650, 2750: 125-2875 Hz) with RX_MARGIN each side,
 * so the AGC and the noise outside it stay out (VE7NHW, 2026-10-28). */
static void rx_filter_apply(void) {
    int lo = bw_hz == 500 ? 1250 : bw_hz == 2300 ? 350 : 125;
    int hi = bw_hz == 500 ? 1750 : bw_hz == 2300 ? 2650 : 2875;
    lo     = lo - RX_MARGIN < FILTER_LOW ? FILTER_LOW : lo - RX_MARGIN;
    hi     = hi + RX_MARGIN > FILTER_HIGH ? FILTER_HIGH : hi + RX_MARGIN;
    atomic_store(&wf_lo_hz, lo);
    atomic_store(&wf_hi_hz, hi);
    filter_set(lo, hi);
}

/* PTT lead: xara keys, waits this long, then plays (see xara_start). */
static const int lead_ms[] = {0, 25, 50, 75, 100, 125, 150};
#define LEADS (int)(sizeof(lead_ms) / sizeof(lead_ms[0]))

/* Send / receive buffers: xara's PulseAudio buffer sizes (our xara's
 * XARA_PLAYBACK_MS / XARA_CAPTURE_MS; build 10-12 used PULSE_LATENCY_MSEC). xara asks for 10 ms in / 20 ms out,
 * too little for the radio: its playback ran dry ~6 times a second and each
 * APRS burst came out ~4 % long, the data pieces undecodable (bench,
 * 2026-10-28). 0 = xara's own. Each ms here is a ms more before an answer. */
static const int buf_ms[] = {0, 20, 40, 60, 80, 100, 150, 200};
#define BUFS (int)(sizeof(buf_ms) / sizeof(buf_ms[0]))

/* 10-31: after FS + the X6100's sending stutters (frames come out up to 55 %
 * long), suspected: xara at nice -10 starves PulseAudio, which plays its
 * audio. To try on the radio: xara -10 (builds 14-15), xara at normal
 * priority, or PulseAudio's threads above xara while XARA is open. */
static const char *const prio_names[] = {"xara -10", "xara normal", "audio first (pulse -15)"};
#define PRIOS (int)(sizeof(prio_names) / sizeof(prio_names[0]))
#define PULSE_NICE -15

static int index_of(const int *v, int n, int want, int fallback) {
    for (int i = 0; i < n; i++)
        if (v[i] == want) return i;
    return fallback;
}

/* The TX level, the PTT lead and the audio buffer, kept on the DATA
 * partition between radio starts. */
static void level_load(void) {
    if (level_loaded) return;
    level_loaded = true;
    FILE *f      = fopen(LEVEL_FILE, "r");
    if (!f) return;
    char  line[64];
    float v;
    int   ms;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "tx_level_db=%f", &v) == 1 && v >= LEVEL_MIN_DB && v <= LEVEL_MAX_DB) level_db = v;
        else if (sscanf(line, "lead_ms=%d", &ms) == 1) lead_idx = index_of(lead_ms, LEADS, ms, lead_idx);
        else if (sscanf(line, "audio_ms=%d", &ms) == 1) buf_idx = index_of(buf_ms, BUFS, ms, buf_idx);
        else if (sscanf(line, "rx_audio_ms=%d", &ms) == 1) rbuf_idx = index_of(buf_ms, BUFS, ms, rbuf_idx);
        else if (sscanf(line, "prio=%d", &ms) == 1 && ms >= 0 && ms < PRIOS) prio_idx = ms;
    }
    fclose(f);
}

static void level_save(void) {
    if (!level_dirty) return;
    FILE *f = fopen(LEVEL_FILE ".tmp", "w");
    if (!f) return;
    fprintf(f, "tx_level_db=%.1f\nlead_ms=%d\naudio_ms=%d\nrx_audio_ms=%d\nprio=%d\n", level_db, lead_ms[lead_idx],
            buf_ms[buf_idx], buf_ms[rbuf_idx], prio_idx);
    if (fclose(f) == 0 && rename(LEVEL_FILE ".tmp", LEVEL_FILE) == 0) level_dirty = false;
}

/* Up or down one step; applied at once while sending. */
static void level_step(bool up) {
    float n  = level_db + (up ? LEVEL_STEP_DB : -LEVEL_STEP_DB);
    level_db = n < LEVEL_MIN_DB ? LEVEL_MIN_DB : n > LEVEL_MAX_DB ? LEVEL_MAX_DB : n;
    level_dirty = true;
    pthread_mutex_lock(&key_lock);
    if (atomic_load(&keyed)) audio_set_play_vol(level_db);
    else atomic_store(&vol_is_level, false); /* the next key sets it */
    pthread_mutex_unlock(&key_lock);
}

static long now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Any thread (the PTT port's, the GUI's): one at a time, so an unkey can't
 * slip in between a key's flag and its MODEM ON and leave the radio keyed
 * with keyed false. The level and the power cap go on before the radio
 * keys: the cap again each time, since an ATU tune or a power change sends
 * the full cfg.pwr to the radio (xara keys without tx_player). The play volume
 * stays at the level between the bursts of a link (level_main puts it
 * back VOL_BACK_US after the last), so a key seldom opens the mixer. */
static void set_keyed(bool on) {
    int64_t t0 = now_us();
    pthread_mutex_lock(&key_lock);
    int64_t t1 = now_us(), t2 = t1;
    bool    change = atomic_load(&keyed) != on;
    if (change) {
        if (on) {
            if (param_f_get(cfg.pwr()) > TX_PLAYER_MAX_PWR_W) radio_set_pwr(TX_PLAYER_MAX_PWR_W);
            if (!atomic_load(&vol_is_level)) {
                audio_set_play_vol(level_db);
                atomic_store(&vol_is_level, true);
            }
            t2 = now_us();
            atomic_store(&keyed_since, now_s());
            atomic_store(&keyed, true);
            radio_set_modem(true);
        } else {
            atomic_store(&keyed, false);
            radio_set_modem(false);
            atomic_store(&level_restore, true);
        }
    }
    pthread_mutex_unlock(&key_lock);
    /* 10-31 test: keys reached the radio seconds late (xara waits for each
     * answer), so where the time goes, per key. */
    if (change)
        diag_ptt_log("XARA %s: lock %lld ms, level %lld ms, modem %lld ms", on ? "key" : "unkey",
                     (long long)(t1 - t0) / 1000, (long long)(t2 - t1) / 1000, (long long)(now_us() - t2) / 1000);
}

/* "key":"text" -> out, false when missing or null */
static bool json_str(const char *line, const char *key, char *out, size_t size) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < size) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

/* "key":number -> out, false when missing or null. The first match: the
 * caller passes a starting point for nested keys. */
static bool json_num(const char *line, const char *key, double *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    if (strncmp(p, "null", 4) == 0) return false;
    char  *end;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool json_bool(const char *line, const char *key) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":true", key);
    return strstr(line, pat) != NULL;
}

static void parse_status(const char *line) {
    xara_status_t s;
    double        v;
    unsigned      gen;

    pthread_mutex_lock(&status_lock);
    s   = status;
    gen = status_gen;
    pthread_mutex_unlock(&status_lock);

    if (strstr(line, "\"t\":\"status\"")) {
        s.seen = true;
        json_str(line, "state", s.state, sizeof(s.state));
        json_str(line, "peer", s.peer, sizeof(s.peer));
        json_str(line, "bw", s.bw, sizeof(s.bw));
        s.busy    = json_bool(line, "busy");
        s.buffer  = json_num(line, "buffer", &v) ? (int)v : 0;
        s.have_sn  = json_num(line, "sn", &s.sn);
        s.have_afc = json_num(line, "afc", &s.afc);
        const char *rx = strstr(line, "\"rx\":{");
        if (rx && json_num(rx, "rms", &v)) s.rx_db = v;
        const char *pb = strstr(line, "\"passband\":[");
        if (pb) sscanf(pb + 12, "%d,%d", &s.pass_lo, &s.pass_hi);
        const char *br = strstr(line, "\"bitrate\":{");
        s.have_bps     = false;
        if (br && json_num(br, "bps", &v)) {
            s.bps      = (int)v;
            s.have_bps = true;
            if (json_num(br, "level", &v)) s.level = (int)v;
        } else if (json_num(line, "level", &v)) {
            s.level = (int)v;
        }
    } else if (strstr(line, "\"t\":\"host\"")) {
        char l[80];
        if (json_str(line, "line", l, sizeof(l))) {
            time_t    t = time(NULL);
            struct tm tm;
            gmtime_r(&t, &tm);
            snprintf(s.last_line, sizeof(s.last_line), "%02d:%02d:%02dZ  %s", tm.tm_hour, tm.tm_min, tm.tm_sec, l);
        }
    } else {
        return;
    }

    pthread_mutex_lock(&status_lock);
    if (status_gen == gen) status = s; /* else cleared meanwhile (a new modem): this line was the old one's */
    pthread_mutex_unlock(&status_lock);
}

/* No modem: nothing seen, no peer (linked() false). */
static void status_clear(void) {
    pthread_mutex_lock(&status_lock);
    memset(&status, 0, sizeof(status));
    status_gen++;
    pthread_mutex_unlock(&status_lock);
}

/* ------------------------------------------------------------ PTT port */

static int listen_local(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* All of n, or false. MSG_NOSIGNAL: a peer that reset must not kill the
 * GUI program with SIGPIPE (bt_spp.c does the same). Never blocks: a phone
 * that stops reading would otherwise hold up closing XARA (the join in
 * destruct_cb). */
static bool send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (atomic_load(&stop_threads)) return false;
            struct pollfd q = {.fd = fd, .events = POLLOUT};
            poll(&q, 1, 200);
        } else {
            return false;
        }
    }
    return true;
}

/* xara keys the radio through here: "T 1" / "T 0", answered "RPRT 0". */
static void *ptt_main(void *arg) {
    (void)arg;
    int lfd = listen_local(PTT_PORT);
    if (lfd < 0) {
        LV_LOG_ERROR("XARA: PTT port %d: %s", PTT_PORT, strerror(errno));
        return NULL;
    }
    while (!atomic_load(&stop_threads)) {
        struct pollfd p = {.fd = lfd, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) continue;
        int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char   buf[128];
        size_t have = 0;
        bool   ok   = true;
        while (ok && !atomic_load(&stop_threads)) {
            struct pollfd c = {.fd = cfd, .events = POLLIN};
            if (poll(&c, 1, 200) <= 0) continue;
            ssize_t n = read(cfd, buf + have, sizeof(buf) - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have] = '\0';
            char *nl;
            while (ok && (nl = strchr(buf, '\n'))) {
                *nl               = '\0';
                const char *reply = "RPRT -1\n";
                if (strcmp(buf, "T 1") == 0) {
                    set_keyed(true);
                    reply = "RPRT 0\n";
                } else if (strcmp(buf, "T 0") == 0) {
                    set_keyed(false);
                    reply = "RPRT 0\n";
                }
                if (!send_all(cfd, reply, strlen(reply))) ok = false;
                size_t rest = have - (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, rest);
                have      = rest;
                buf[have] = '\0';
            }
            if (have == sizeof(buf) - 1) have = 0; /* a line that long is garbage */
        }
        close(cfd);
        set_keyed(false); /* xara gone or the link broke: never left keyed */
    }
    close(lfd);
    return NULL;
}

/* ------------------------------------------------------------ TX level */

/* While keyed: the ALC and power readings, for the screen (the level is
 * the operator's). On unkey the GUI's own play volume goes back. */
static void *level_main(void *arg) {
    (void)arg;
    bool    was_keyed = false;
    uint8_t msg_id    = 0;
    int64_t unkeyed_us = 0, swr_high_us = 0;
    float   swr_high   = 0.0f;

    while (!atomic_load(&stop_threads)) {
        usleep(LEVEL_TICK_US);
        bool k = atomic_load(&keyed);
        if (k && !was_keyed) {
            tx_info_refresh(&msg_id, NULL, NULL, NULL); /* readings from before this one don't count */
            atomic_store(&tx_meter, false);
            atomic_store(&tx_max_pwr, 0.0f);
            swr_high_us = 0;
        }
        if (k) {
            float alc = 0.0f, pwr = 0.0f, swr = 0.0f;
            if (tx_info_refresh(&msg_id, &alc, &pwr, &swr)) {
                atomic_store(&tx_alc, alc);
                atomic_store(&tx_pwr, pwr);
                atomic_store(&tx_meter, true);
                if (pwr > atomic_load(&tx_max_pwr)) atomic_store(&tx_max_pwr, pwr);
                /* High SWR as JS8's guard: SWR_TRIP_MS above SWR_TRIP, then
                 * timer_cb stops TX (an antenna gone, the tuner off). */
                if (swr <= SWR_TRIP) {
                    swr_high_us = 0;
                } else if (!swr_high_us) {
                    swr_high_us = now_us();
                    swr_high    = swr;
                } else {
                    swr_high = LV_MAX(swr_high, swr);
                    if (now_us() - swr_high_us >= SWR_TRIP_MS * 1000) {
                        swr_high_us = 0;
                        atomic_store(&swr_tripped, swr_high);
                    }
                }
            }
        }
        if (!k && was_keyed) {
            unkeyed_us = now_us();
            if (atomic_load(&tx_meter)) atomic_store(&last_max_pwr, atomic_load(&tx_max_pwr));
        }
        if (!k && atomic_load(&level_restore) && now_us() - unkeyed_us >= VOL_BACK_US) {
            /* Under the key lock: a key arriving meanwhile sets its own level after this. */
            pthread_mutex_lock(&key_lock);
            if (!atomic_load(&keyed)) {
                audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
                atomic_store(&vol_is_level, false);
                atomic_store(&level_restore, false);
            }
            pthread_mutex_unlock(&key_lock);
        }
        was_keyed = k;
    }
    return NULL;
}

/* ------------------------------------------------------------ waterfall */

static float select_nth(float *v, int n, int k) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = v[lo + (hi - lo) / 2];
        int   i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) {
                float t = v[i];
                v[i]    = v[j];
                v[j]    = t;
                i++;
                j--;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else return v[k];
    }
    return v[k];
}

/* wf_lock held */
static void wf_emit_row(void) {
    spgramf_get_psd(sg, psd);
    liquid_vectorf_addscalar(psd, nfft, -10.f * log10f(sqrtf(nfft)), psd);
    spgramf_reset(sg);

    /* spgram output is FFT-shifted: bin nfft/2 is 0 Hz. */
    uint32_t low_bin  = nfft / 2u + (uint32_t)nfft * FILTER_LOW / SAMPLE_RATE;
    uint32_t high_bin = nfft / 2u + (uint32_t)nfft * FILTER_HIGH / SAMPLE_RATE;
    if (high_bin > nfft) high_bin = nfft;
    uint32_t bins = high_bin - low_bin;
    for (uint32_t x = 0; x < WIDTH; x++) {
        uint32_t b0 = low_bin + x * bins / WIDTH, b1 = low_bin + (x + 1) * bins / WIDTH;
        float    v  = psd[b0];
        for (uint32_t b = b0 + 1; b < b1; b++) v = psd[b] > v ? psd[b] : v;
        wf_row[x] = v;
    }

    /* Noise floor: the 20th percentile of the columns inside the RX filter,
     * smoothed over ~2 s. With BW 500 most of the row is filtered out, so a
     * floor over the whole row sank far below the passband's noise and made
     * it glare (VE7NHW, 2026-10-28). Our own transmission doesn't move it. */
    if (!wf_row_tx) {
        int c0 = (int)((int64_t)WIDTH * (atomic_load(&wf_lo_hz) - FILTER_LOW) / (FILTER_HIGH - FILTER_LOW));
        int c1 = (int)((int64_t)WIDTH * (atomic_load(&wf_hi_hz) - FILTER_LOW) / (FILTER_HIGH - FILTER_LOW));
        if (c0 < 0) c0 = 0;
        if (c1 > (int)WIDTH) c1 = WIDTH;
        if (c1 - c0 < 16) c0 = 0, c1 = WIDTH;
        int n = c1 - c0;
        memcpy(wf_sel, wf_row + c0, n * sizeof(float));
        float floor_now = select_nth(wf_sel, n, n / 5); /* 20th: a 500 Hz signal fills ~60 % of BW 500's filter */
        wf_floor_db     = wf_floor_set ? wf_floor_db + 0.05f * (floor_now - wf_floor_db) : floor_now;
        wf_floor_set    = true;
    }
    float floor_db = wf_floor_set ? wf_floor_db : -100.0f;
    /* Our own transmission (the clean audio going out) is far above the
     * receiver's noise: draw it relative to its own peak, a few dB under
     * the top of the palette, not solid white. */
    if (wf_row_tx) {
        float top = wf_row[0];
        for (uint32_t i = 1; i < WIDTH; i++) top = wf_row[i] > top ? wf_row[i] : top;
        floor_db = top - (WF_MAX_DB - 4);
    }
    for (uint32_t i = 0; i < WIDTH; i++) wf_row[i] -= floor_db;

    /* Calm, as JS8's "Light": receive rows blend into the ones before.
     * Our own transmission is drawn as it is, and starts the blend again. */
    if (wf_row_tx) {
        wf_avg_set = false;
    } else {
        if (!wf_avg_set) memcpy(wf_avg, wf_row, sizeof(wf_avg));
        else
            for (uint32_t i = 0; i < WIDTH; i++) wf_avg[i] += WF_CALM_NEW * (wf_row[i] - wf_avg[i]);
        wf_avg_set = true;
        memcpy(wf_row, wf_avg, sizeof(wf_row));
    }

    if (wf_q_count == WF_QUEUE) {
        wf_q_head = (wf_q_head + 1) % WF_QUEUE;
        wf_q_count--;
    }
    unsigned at = (wf_q_head + wf_q_count) % WF_QUEUE;
    memcpy(wf_rows[at], wf_row, sizeof(wf_row));
    wf_rows_tx[at] = wf_row_tx;
    wf_q_count++;
}

/* Receive audio (dsp's thread) or the audio going out (txmon's). */
static void wf_feed(const float *samples, unsigned n, bool tx) {
    pthread_mutex_lock(&wf_lock);
    if (!sg || !psd) {
        pthread_mutex_unlock(&wf_lock);
        return;
    }
    if (tx != wf_row_tx) { /* a new row for the other side */
        spgramf_reset(sg);
        wf_row_fill = 0;
        wf_row_tx   = tx;
    }
    while (n) {
        unsigned take = WF_ROW_SAMPLES - wf_row_fill;
        if (take > n) take = n;
        spgramf_write(sg, (float *)samples, take);
        samples += take;
        n -= take;
        wf_row_fill += take;
        if (wf_row_fill >= WF_ROW_SAMPLES) {
            wf_row_fill = 0;
            wf_emit_row();
        }
    }
    pthread_mutex_unlock(&wf_lock);
}

static void rx_audio_cb(size_t n, float *samples) {
    if (atomic_load(&keyed)) return; /* the receiver is off while we send */
    wf_feed(samples, (unsigned)n, false);
}

/* The audio going out: the playback device's monitor, read the whole time
 * the app is open (it's small), drawn only while keyed. */
static void *txmon_main(void *arg) {
    (void)arg;
    pa_sample_spec spec = {.format = PA_SAMPLE_FLOAT32NE, .channels = 1, .rate = SAMPLE_RATE};
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .fragsize  = WF_ROW_SAMPLES * sizeof(float),
    };
    int        err;
    pa_simple *pa = NULL;
    float      buf[WF_ROW_SAMPLES / 2];

    while (!atomic_load(&stop_threads)) {
        if (!pa) {
            pa = pa_simple_new(NULL, "X6100 GUI XARA TX", PA_STREAM_RECORD, XARA_PLAYBACK ".monitor", "XARA TX monitor",
                               &spec, NULL, &attr, &err);
            if (!pa) {
                LV_LOG_WARN("XARA: TX monitor: %s", pa_strerror(err));
                for (int i = 0; i < 10 && !atomic_load(&stop_threads); i++) usleep(200000);
                continue;
            }
        }
        if (pa_simple_read(pa, buf, sizeof(buf), &err) < 0) {
            pa_simple_free(pa);
            pa = NULL;
            continue;
        }
        if (atomic_load(&keyed)) wf_feed(buf, WF_ROW_SAMPLES / 2, true);
    }
    if (pa) pa_simple_free(pa);
    return NULL;
}

/* At most one row per tick, each when it's due by the clock (JS8's pacing). */
static void wf_timer_cb(lv_timer_t *t) {
    (void)t;
    js8_wf_tick();
    pthread_mutex_lock(&wf_lock);
    unsigned waiting = wf_q_count;
    pthread_mutex_unlock(&wf_lock);
    if (!waiting) return;
    int64_t now    = now_us();
    int64_t period = 1000000 / WF_ROWS_PER_SEC;
    if (waiting > 6) period = period * 3 / 4;
    else if (waiting > 3) period = period * 19 / 20;
    if (now - wf_due_us > period) wf_due_us = now;
    if (now < wf_due_us) return;

    static float row[WIDTH];
    bool         tx;
    pthread_mutex_lock(&wf_lock);
    memcpy(row, wf_rows[wf_q_head], sizeof(row));
    tx        = wf_rows_tx[wf_q_head];
    wf_q_head = (wf_q_head + 1) % WF_QUEUE;
    wf_q_count--;
    pthread_mutex_unlock(&wf_lock);
    js8_wf_add_row(row, WIDTH);
    if (tx) {
        js8_wf_fill_rect(0, 0, TX_EDGE_PX - 1, 0, lv_color_hex(0xe63c3c));
        js8_wf_fill_rect(WIDTH - TX_EDGE_PX, 0, WIDTH - 1, 0, lv_color_hex(0xe63c3c));
    }
    wf_due_us += period;
    js8_wf_tick();
}

static void wf_watch_cb(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    js8_wf_repaint_soon();
}

static void wf_start(void) {
    /* At least a bin per pixel, a power of two (as JS8). */
    unsigned want = WIDTH * SAMPLE_RATE / (FILTER_HIGH - FILTER_LOW);
    nfft          = 256;
    while (nfft < want && nfft < 8192) nfft *= 2;
    unsigned step = nfft / 2 < WF_ROW_SAMPLES / 2 ? nfft / 2 : WF_ROW_SAMPLES / 2;
    pthread_mutex_lock(&wf_lock);
    sg           = spgramf_create(nfft, LIQUID_WINDOW_HANN, nfft, step);
    psd          = malloc(nfft * sizeof(float));
    wf_row_fill  = 0;
    wf_row_tx    = false;
    wf_floor_set = false;
    wf_avg_set   = false;
    wf_q_head = wf_q_count = 0;
    pthread_mutex_unlock(&wf_lock);
    wf_due_us = 0;

    waterfall_set_enabled(false);
    spectrum_set_enabled(false);
    if (!js8_wf_create(wf_box, (const lv_color_t *)style.wf_palette, WF_MIN_DB, WF_MAX_DB))
        msg_update_text_fmt("XARA: no memory for the waterfall");
    wf_watch[0] = subject_subscribe((Subject *)cfg.cur.fg_freq(), wf_watch_cb, NULL);
    wf_watch[1] = subject_subscribe((Subject *)cfg.band.if_shift(), wf_watch_cb, NULL);
    wf_watch[2] = subject_subscribe((Subject *)cfg.mode.zoom(), wf_watch_cb, NULL);
    wf_watch[3] = subject_subscribe((Subject *)cfg.cur.mode_lo_offset(), wf_watch_cb, NULL);
    wf_timer    = lv_timer_create(wf_timer_cb, WF_TICK_MS, NULL);

    rx_sub = dsp_audio_subscribe_float(rx_audio_cb, SAMPLE_RATE);
    dsp_audio_set_active(rx_sub, true);
}

static void wf_stop(void) {
    /* No receive callback after this returns (it takes dsp's lock). */
    dsp_audio_unsubscribe(rx_sub);
    rx_sub = AUDIO_SUB_INVALID;
    if (wf_timer) lv_timer_del(wf_timer);
    wf_timer = NULL;
    for (unsigned i = 0; i < sizeof(wf_watch) / sizeof(wf_watch[0]); i++) {
        if (wf_watch[i]) param_unsubscribe(wf_watch[i]);
        wf_watch[i] = NULL;
    }
    js8_wf_destroy();
    waterfall_set_enabled(true);
    spectrum_set_enabled(true);

    pthread_mutex_lock(&wf_lock); /* the TX monitor thread is joined already */
    if (sg) spgramf_destroy(sg);
    sg = NULL;
    free(psd);
    psd = NULL;
    pthread_mutex_unlock(&wf_lock);
}

/* The passband marks over the waterfall: two thin lines, from xara's
 * status (350-2650 Hz at 2300). */
static lv_obj_t *pass_line(void) {
    lv_obj_t *l = lv_obj_create(wf_box);
    lv_obj_remove_style_all(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(l, lv_color_hex(0xffa600), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_70, 0);
    lv_obj_set_size(l, 2, WF_H);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static void show(lv_obj_t *o, bool on);

static void pass_place(lv_obj_t *l, int hz) {
    if (hz <= FILTER_LOW || hz >= FILTER_HIGH) {
        show(l, false);
        return;
    }
    lv_obj_set_x(l, (lv_coord_t)((int64_t)WIDTH * (hz - FILTER_LOW) / (FILTER_HIGH - FILTER_LOW)) - 1);
    show(l, true);
}

/* ------------------------------------------------------------ Bluetooth KISS */

static void bt_set_note(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&bt_lock);
    va_start(ap, fmt);
    vsnprintf(bt_note, sizeof(bt_note), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&bt_lock);
}

/* Paired with this radio: BlueZ keeps a link key for it. */
static bool bt_paired(const char *addr) {
    DIR *d = opendir(BT_STORE);
    if (!d) return false;
    bool           found = false;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[160];
        snprintf(path, sizeof(path), BT_STORE "/%s/%s/info", e->d_name, addr);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "[LinkKey]", 9) == 0 || strncmp(line, "[LongTermKey]", 13) == 0) {
                found = true;
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* Closing an RFCOMM socket can block in the kernel (bt_spp.c): off the
 * thread that has to keep going. */
static void *bt_closer(void *arg) {
    close((int)(intptr_t)arg);
    return NULL;
}

static void bt_close_async(int fd) {
    pthread_t      t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, bt_closer, (void *)(intptr_t)fd) != 0) close(fd);
    pthread_attr_destroy(&at);
}

/* Bytes both ways between a paired phone's RFCOMM channel and xara's KISS
 * port, until either side closes. xara takes several KISS clients, so the
 * phone can be on WiFi and Bluetooth at once. */
static void bt_bridge(int bt, const char *addr) {
    int                kfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a   = {.sin_family = AF_INET, .sin_port = htons(KISS_PORT)};
    a.sin_addr.s_addr      = htonl(INADDR_LOOPBACK);
    if (kfd < 0 || connect(kfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (kfd >= 0) close(kfd);
        bt_set_note("BT KISS: modem not running, %s dropped", addr);
        return;
    }
    bt_set_note("BT KISS: connected  %s", addr);
    char buf[1024];
    while (!atomic_load(&stop_threads)) {
        struct pollfd p[2] = {{.fd = bt, .events = POLLIN}, {.fd = kfd, .events = POLLIN}};
        if (poll(p, 2, 200) <= 0) continue;
        if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL) && !(p[0].revents & POLLIN)) break;
        if (p[1].revents & (POLLERR | POLLHUP | POLLNVAL) && !(p[1].revents & POLLIN)) break;
        for (int i = 0; i < 2; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            ssize_t n = read(p[i].fd, buf, sizeof(buf));
            if (n <= 0) goto done;
            if (!send_all(i == 0 ? kfd : bt, buf, (size_t)n)) goto done;
        }
    }
done:
    close(kfd);
}

/* RFCOMM channel BT_KISS_CHANNEL while the app is open: APRSdroid's
 * "Bluetooth SPP TNC" with its channel set to 3. Only phones paired with
 * the radio (the Bluetooth window) get through. Retries while Bluetooth is
 * off. */
static void *btkiss_main(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_threads)) {
        int srv = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
        if (srv < 0) {
            bt_set_note("BT KISS: no Bluetooth");
            for (int i = 0; i < 10 && !atomic_load(&stop_threads); i++) usleep(200000);
            continue;
        }
        /* Medium: the link encrypted with the pairing's key, so a phone that
         * only claims a paired phone's address doesn't get through (KISS
         * frames go out on the air). A phone that isn't paired waits for
         * an agent and is never accepted (bt_spp.c's LOW is for pairing
         * from that window); bt_paired() below is the second check. */
        struct bt_security sec = {.level = BT_SECURITY_MEDIUM};
        setsockopt(srv, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec));
        struct sockaddr_rc loc = {.rc_family = AF_BLUETOOTH, .rc_channel = BT_KISS_CHANNEL};
        bdaddr_t           any = {{0, 0, 0, 0, 0, 0}};
        loc.rc_bdaddr          = any;
        if (bind(srv, (struct sockaddr *)&loc, sizeof(loc)) < 0 || listen(srv, 1) < 0) {
            close(srv);
            bt_set_note("BT KISS: Bluetooth off (channel %d)", BT_KISS_CHANNEL);
            for (int i = 0; i < 10 && !atomic_load(&stop_threads); i++) usleep(200000);
            continue;
        }
        bt_set_note("BT KISS: channel %d, waiting", BT_KISS_CHANNEL);
        while (!atomic_load(&stop_threads)) {
            struct pollfd p = {.fd = srv, .events = POLLIN};
            int           r = poll(&p, 1, 200);
            if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break; /* adapter gone */
            if (r == 0) continue;
            struct sockaddr_rc rem;
            socklen_t          len = sizeof(rem);
            int                c   = accept4(srv, (struct sockaddr *)&rem, &len, SOCK_CLOEXEC);
            if (c < 0) continue;
            char addr[18];
            ba2str(&rem.rc_bdaddr, addr);
            if (!bt_paired(addr)) {
                bt_set_note("BT KISS: refused %s (not paired)", addr);
                bt_close_async(c);
                continue;
            }
            bt_bridge(c, addr);
            bt_close_async(c);
            bt_set_note("BT KISS: channel %d, waiting", BT_KISS_CHANNEL);
        }
        bt_close_async(srv);
    }
    return NULL;
}

/* ------------------------------------------------------------ status feed */

static void *stream_main(void *arg) {
    (void)arg;
    static char buf[8192];
    while (!atomic_load(&stop_threads)) {
        int                fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a  = {.sin_family = AF_INET, .sin_port = htons(UI_PORT)};
        a.sin_addr.s_addr     = htonl(INADDR_LOOPBACK);
        if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            if (fd >= 0) close(fd);
            for (int i = 0; i < 5 && !atomic_load(&stop_threads); i++) usleep(200000);
            continue;
        }
        const char *req = "GET /stream HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        if (!send_all(fd, req, strlen(req))) {
            close(fd);
            continue;
        }
        size_t have = 0;
        while (!atomic_load(&stop_threads)) {
            struct pollfd p = {.fd = fd, .events = POLLIN};
            if (poll(&p, 1, 200) <= 0) continue;
            ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have]   = '\0';
            char *start = buf, *nl;
            while ((nl = strchr(start, '\n'))) {
                *nl = '\0';
                if (*start == '{') parse_status(start);
                start = nl + 1;
            }
            have = strlen(start);
            memmove(buf, start, have + 1);
            if (have == sizeof(buf) - 1) have = 0;
        }
        close(fd);
    }
    return NULL;
}

/* About the PTT lead (lead_ms[] above): xara keys, waits that long, then
 * plays, and counts the wait in its VARA timing, so the radio and the amp's
 * relays are on the air before a burst starts. xara's own default is 0.
 * Kept small: xara waits 83 ms after a frame before answering and keys that
 * much earlier, but once its decoding takes longer than that, every ms here
 * adds to the answer time, and VARA allows only so much. */

/* ------------------------------------------------------------ recording */

static int rec_filter(const struct dirent *d) {
    size_t n = strlen(d->d_name);
    return strncmp(d->d_name, "xara-", 5) == 0 && n > 5 && strcmp(d->d_name + n - 5, ".flac") == 0;
}

/* Room for one more: only the newest REC_KEEP - 1 stay (names sort by time). */
static void rec_prune(void) {
    struct dirent **list;
    int             n = scandir(REC_DIR, &list, rec_filter, alphasort);
    if (n < 0) return;
    for (int i = 0; i < n; i++) {
        if (i < n - (REC_KEEP - 1)) {
            char path[300];
            snprintf(path, sizeof(path), REC_DIR "/%s", list[i]->d_name);
            unlink(path);
            size_t n = strlen(path);
            if (n > 5) {
                strcpy(path + n - 5, ".log"); /* its xara log, if any */
                unlink(path);
            }
        }
        free(list[i]);
    }
    free(list);
}

/* The next recording's name, or false (and why on screen) if none. */
static bool rec_prepare(void) {
    rec_path[0] = '\0';
    if (mkdir(REC_DIR, 0755) != 0 && errno != EEXIST) {
        msg_update_text_fmt("Record: can't make " REC_DIR ": %s", strerror(errno));
        return false;
    }
    struct statvfs v;
    if (statvfs(REC_DIR, &v) == 0 && (unsigned long long)v.f_bavail * v.f_frsize < REC_MIN_FREE_MB * 1024ULL * 1024ULL) {
        msg_update_text_fmt("Record: less than %d MB free on the SD card, not recording", REC_MIN_FREE_MB);
        return false;
    }
    rec_prune();
    time_t    t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(rec_path, sizeof(rec_path), REC_DIR "/xara-%04d%02d%02d-%02d%02d%02dZ-%d.flac", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, bw_hz);
    return true;
}

/* GUI thread, each tick: Record turns itself off when the card fills, and
 * the modem restarts without it once no link is up (a restart drops one). */
static void rec_space_tick(void) {
    static unsigned tick;
    if (!rec_full) {
        if (tick++ % 40 != 0 || monitoring || xara_pid <= 0 || !rec_path[0]) return; /* every 10 s */
        struct statvfs v;
        if (statvfs(REC_DIR, &v) != 0 || (unsigned long long)v.f_bavail * v.f_frsize >= REC_STOP_MB * 1024ULL * 1024ULL)
            return;
        rec_full  = true;
        record_on = false;
        msg_update_text_fmt("Record off: less than %d MB free on the SD card", REC_STOP_MB);
    }
    if (atomic_load(&keyed) || linked()) return;
    rec_full = false;
    if (!monitoring && xara_pid > 0 && rec_path[0]) {
        xara_stop();
        xara_start();
    }
}

/* ------------------------------------------------------------ the modem */

/* A xara or xara-monitor left from a GUI program that crashed or was
 * restarted while XARA was open: it would keep the ports, keep answering
 * calls and key through our PTT port. Gone before ours starts. */
static void stale_kill(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        long  pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 1 || pid == getpid()) continue;
        char path[32], comm[32] = "";
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = '\0';
        fclose(f);
        if (strcmp(comm, "xara") == 0 || strcmp(comm, "xara-monitor") == 0) {
            LV_LOG_WARN("XARA: killing a leftover %s (pid %ld)", comm, pid);
            kill((pid_t)pid, SIGKILL);
        }
    }
    closedir(d);
}

/* PulseAudio's threads: logged to ptt.log at each xara start (nice and
 * scheduling policy, so we see what xara competes with), and raised to
 * PULSE_NICE for "audio first", put back on xara_stop. */
static struct {
    pid_t tid;
    int   nice;
} pulse_saved[32];
static int pulse_nsaved;

static void pulse_prio(bool raise) {
    if (!raise) {
        for (int i = 0; i < pulse_nsaved; i++) setpriority(PRIO_PROCESS, pulse_saved[i].tid, pulse_saved[i].nice);
        pulse_nsaved = 0;
        return;
    }
    DIR *d = opendir("/proc");
    if (!d) return;
    char   line[400] = "";
    size_t used      = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        long  pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 1) continue;
        char path[64], comm[32] = "";
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = '\0';
        fclose(f);
        if (strcmp(comm, "pulseaudio") != 0) continue;
        snprintf(path, sizeof(path), "/proc/%ld/task", pid);
        DIR *t = opendir(path);
        if (!t) continue;
        struct dirent *te;
        while ((te = readdir(t))) {
            long tid = strtol(te->d_name, &end, 10);
            if (*end || tid <= 0) continue;
            char stat[512] = "", tname[32] = "";
            snprintf(path, sizeof(path), "/proc/%ld/task/%ld/stat", pid, tid);
            FILE *sf = fopen(path, "r");
            if (!sf) continue;
            size_t      n  = fread(stat, 1, sizeof(stat) - 1, sf);
            fclose(sf);
            stat[n]        = '\0';
            const char *l  = strchr(stat, '(');
            const char *r  = strrchr(stat, ')');
            if (!l || !r) continue;
            snprintf(tname, sizeof(tname), "%.*s", (int)(r - l - 1), l + 1);
            /* After ")": field 3 on; nice is field 19, policy field 41. */
            int   nice = 0, policy = -1, field = 3;
            for (const char *q = r + 2; *q; q++) {
                if (field == 19) nice = atoi(q);
                if (field == 41) { policy = atoi(q); break; }
                q = strchr(q, ' ');
                if (!q) break;
                field++;
            }
            if (used < sizeof(line))
                used += (size_t)snprintf(line + used, sizeof(line) - used, " %ld(%s) nice %d pol %d%s;", tid, tname, nice,
                                         policy, prio_idx == 2 && policy == 0 ? "->raised" : "");
            if (prio_idx == 2 && policy == 0 && pulse_nsaved < (int)(sizeof(pulse_saved) / sizeof(pulse_saved[0])) &&
                setpriority(PRIO_PROCESS, (id_t)tid, PULSE_NICE) == 0) {
                pulse_saved[pulse_nsaved].tid  = (pid_t)tid;
                pulse_saved[pulse_nsaved].nice = nice;
                pulse_nsaved++;
            }
        }
        closedir(t);
    }
    closedir(d);
    diag_ptt_log("XARA prio %s: pulseaudio%s", prio_names[prio_idx], used ? line : " not found");
}

static void xara_start(void) {
    char call[PARAM_TEXT_MAX];
    param_t_get_into(cfg.callsign(), call, sizeof(call));
    if (!call[0]) {
        lv_label_set_text(state_label, "Set your callsign first");
        lv_label_set_text(last_label, "APP > Callsign, then open XARA again");
        return;
    }
    if (access(XARA_BIN, X_OK) != 0) {
        lv_label_set_text(state_label, "xara is not installed");
        lv_label_set_text(last_label, XARA_BIN " is missing from this image");
        return;
    }

    char bw[8], drive[8], ptt[32], port[8], kiss[8], ui[8], lead[8];
    snprintf(bw, sizeof(bw), "%d", bw_hz);
    snprintf(lead, sizeof(lead), "%d", lead_ms[lead_idx]);
    /* No --ptt-tail-ms: our xara (f98e0cd) counts the send buffer as TX
     * latency and stays keyed until it has played out (builds 11-13 passed
     * buffer + 20 here instead). */
    snprintf(drive, sizeof(drive), "%d", XARA_DRIVE_DB);
    snprintf(ptt, sizeof(ptt), "rigctld:127.0.0.1:%d", PTT_PORT);
    snprintf(port, sizeof(port), "%d", VARA_CMD_PORT);
    snprintf(kiss, sizeof(kiss), "%d", KISS_PORT);
    snprintf(ui, sizeof(ui), "%d", UI_PORT);

    /* --answer-gate: no call answered until Auto answer lets it (SIGRTMIN). */
    /* Room for every option below plus --record PATH and the NULL: build 11
     * added --ptt-tail-ms, the list grew to 31 and --record wrote past the
     * end (xara then saw an empty --hf: "bad bandwidth"). */
    char *argv[40] = {
        "xara", "--call", call, "--hf", bw, "--audio", "pulse", "--capture", XARA_CAPTURE, "--playback", XARA_PLAYBACK,
        "--radio", "--ptt", ptt, "--drive-db", drive, "--addr", "0.0.0.0", "--port", port, "--kiss-port", kiss,
        "--ui-port", ui, "--ui-addr", "127.0.0.1", "--answer-gate", "--local-only", "--ptt-lead-ms", lead,
    };
    unsigned argc = 0;
    while (argv[argc]) argc++;
    /* VARA's registered speeds: needs an xara built with them (it refuses
     * the option otherwise) and the operator's VARA licence. Off unless chosen. */
    if (full_speed && argc + 1 < sizeof(argv) / sizeof(argv[0])) argv[argc++] = "--registered";
    rec_path[0] = '\0'; /* this run's: none unless it records (the log goes beside it) */
    if (record_on && argc + 3 <= sizeof(argv) / sizeof(argv[0]) && rec_prepare()) {
        argv[argc++] = "--record";
        argv[argc++] = rec_path;
    }
    argv[argc] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    /* The log beside the recording (same name, .log): /tmp is lost at power-off. */
    snprintf(log_path, sizeof(log_path), "%s", XARA_LOG);
    size_t rl = strlen(rec_path);
    if (rl > 5 && rl + 1 < sizeof(log_path)) {
        snprintf(log_path, sizeof(log_path), "%.*s.log", (int)(rl - 5), rec_path);
    }
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    /* The GUI's environment, plus the audio buffer (Settings). */
    size_t nenv = 0;
    while (environ[nenv]) nenv++;
    /* xara's own buffer settings (our xara): send and receive apart.
     * PULSE_LATENCY_MSEC is kept out: it would set both and override them. */
    char **envp = calloc(nenv + 4, sizeof(char *));
    char   play_env[40], cap_env[40];
    int    err  = ENOMEM;
    if (envp) {
        size_t k = 0;
        for (size_t i = 0; i < nenv; i++)
            if (strncmp(environ[i], "PULSE_LATENCY_MSEC=", 19) != 0 && strncmp(environ[i], "XARA_CAPTURE_MS=", 16) != 0 &&
                strncmp(environ[i], "XARA_PLAYBACK_MS=", 17) != 0 && strncmp(environ[i], "XARA_NICE=", 10) != 0)
                envp[k++] = environ[i];
        /* Higher CPU priority for xara (the GUI shares the CPU; full-speed
         * sending ran dry, 2026-10-29). */
        envp[k++] = prio_idx == 1 ? "XARA_NICE=0" : "XARA_NICE=-10";
        if (buf_ms[buf_idx] > 0) {
            snprintf(play_env, sizeof(play_env), "XARA_PLAYBACK_MS=%d", buf_ms[buf_idx]);
            envp[k++] = play_env;
        }
        if (buf_ms[rbuf_idx] > 0) {
            snprintf(cap_env, sizeof(cap_env), "XARA_CAPTURE_MS=%d", buf_ms[rbuf_idx]);
            envp[k++] = cap_env;
        }
        envp[k] = NULL;
        err     = posix_spawn(&xara_pid, XARA_BIN, &fa, NULL, argv, envp);
        free(envp);
    }
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) {
        xara_pid = -1;
        lv_label_set_text(state_label, "The modem did not start");
        lv_label_set_text_fmt(last_label, "%s", strerror(err));
        return;
    }
    pulse_prio(true);
    status_clear();
    busy_sent = false; /* told once its status shows (its handlers are in by then) */
    gate_open = false; /* --answer-gate starts closed */
    log_off   = 0;     /* the log starts again (O_TRUNC) */
    gaps_tx = gaps_rx = 0;
    lv_label_set_text(state_label, "Starting the modem...");
    lv_label_set_text(last_label, "");
}

static void xara_stop(void) {
    if (xara_pid > 0) {
        kill(xara_pid, SIGTERM);
        for (int i = 0; i < 30; i++) { /* up to 1.5 s, then it's killed */
            if (waitpid(xara_pid, NULL, WNOHANG) != 0) {
                xara_pid = -1;
                break;
            }
            usleep(50000);
        }
        if (xara_pid > 0) {
            kill(xara_pid, SIGKILL);
            waitpid(xara_pid, NULL, 0);
        }
    }
    xara_pid = -1;
    set_keyed(false);
    status_clear();
    pulse_prio(false);
}

static bool linked(void) {
    pthread_mutex_lock(&status_lock);
    bool l = status.peer[0] != '\0';
    pthread_mutex_unlock(&status_lock);
    return l;
}

/* Stop TX, as JS8's first ESC: unkeyed at once, and the modem started
 * again, so the link is dropped and nothing goes out until the phone
 * connects again. */
static void stop_tx(void) {
    set_keyed(false);
    xara_stop();
    xara_start();
    msg_update_text_fmt("TX stopped, link dropped");
}

/* ------------------------------------------------------------ the monitor */

/* The monitor's output, a line per decoded frame ("HH:MM:SS.dZ text"). */
static void *mon_main(void *arg) {
    (void)arg;
    char   buf[512];
    size_t have = 0;
    while (!atomic_load(&mon_stop)) {
        struct pollfd p = {.fd = mon_fd, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) continue;
        ssize_t n = read(mon_fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) break; /* the monitor is gone */
        have += (size_t)n;
        buf[have]   = '\0';
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = '\0';
            if (*start) {
                pthread_mutex_lock(&mon_lock);
                memmove(mon_lines[1], mon_lines[0], sizeof(mon_lines[0]) * (MON_LINES - 1));
                snprintf(mon_lines[0], sizeof(mon_lines[0]), "%s", start);
                mon_count++;
                pthread_mutex_unlock(&mon_lock);
            }
            start = nl + 1;
        }
        have = strlen(start);
        memmove(buf, start, have + 1);
        if (have == sizeof(buf) - 1) have = 0;
    }
    return NULL;
}

static void monitor_start(void) {
    char call[PARAM_TEXT_MAX], bw[8];
    param_t_get_into(cfg.callsign(), call, sizeof(call));
    snprintf(bw, sizeof(bw), "%d", bw_hz);
    pthread_mutex_lock(&mon_lock);
    memset(mon_lines, 0, sizeof(mon_lines));
    mon_count = 0;
    pthread_mutex_unlock(&mon_lock);
    if (access(XARA_MON_BIN, X_OK) != 0) {
        lv_label_set_text(state_label, "xara-monitor is not installed");
        return;
    }
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) != 0) {
        lv_label_set_text(state_label, "The monitor did not start");
        return;
    }
    /* --call: our own call is known, so frames to and from us are named. */
    char *argv[] = {"xara-monitor", "--audio", "pulse", "--capture", XARA_CAPTURE, "--hf", bw,
                    call[0] ? "--call" : NULL, call, NULL};

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, XARA_MON_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    int err = posix_spawn(&mon_pid, XARA_MON_BIN, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (err != 0) {
        mon_pid = -1;
        close(fd[0]);
        lv_label_set_text(state_label, "The monitor did not start");
        lv_label_set_text_fmt(last_label, "%s", strerror(err));
        return;
    }
    mon_fd = fd[0];
    atomic_store(&mon_stop, false);
    mon_thread_up = pthread_create(&mon_thread, NULL, mon_main, NULL) == 0;
}

static void monitor_stop(void) {
    if (mon_pid > 0) {
        kill(mon_pid, SIGTERM);
        for (int i = 0; i < 30; i++) { /* up to 1.5 s, then it's killed */
            if (waitpid(mon_pid, NULL, WNOHANG) != 0) {
                mon_pid = -1;
                break;
            }
            usleep(50000);
        }
        if (mon_pid > 0) {
            kill(mon_pid, SIGKILL);
            waitpid(mon_pid, NULL, 0);
        }
    }
    mon_pid = -1;
    atomic_store(&mon_stop, true);
    if (mon_thread_up) pthread_join(mon_thread, NULL);
    mon_thread_up = false;
    if (mon_fd >= 0) close(mon_fd);
    mon_fd = -1;
}

/* BW, APRS and Restart: whichever program runs starts again. */
static void xara_restart(void) {
    if (monitoring) {
        monitor_stop();
        monitor_start();
        return;
    }
    if (linked()) {
        msg_update_text_fmt("Disconnect first (in WoAD)");
        return;
    }
    xara_stop();
    xara_start();
}

/* ------------------------------------------------------------ the screen */

/* The screen is updated 4 times a second: labels, colours and flags are
 * only touched when they change, or LVGL redraws them each time (the CPU
 * is shared with xara). */
static void label_set(lv_obj_t *l, const char *fmt, ...) {
    char    t[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void color_set(lv_obj_t *l, uint32_t rgb) {
    if (lv_color_to32(lv_obj_get_style_text_color(l, 0)) != lv_color_to32(lv_color_hex(rgb)))
        lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
}

static void show(lv_obj_t *o, bool on) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == !on) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static bool wifi_ip(char *out, size_t size) {
    struct ifaddrs *list, *i;
    bool            found = false;
    if (getifaddrs(&list) != 0) return false;
    for (i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(i->ifa_name, "lo") == 0) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, size);
        found = true;
        break;
    }
    freeifaddrs(list);
    return found;
}

/* The addresses of whatever is connected to the VARA and KISS ports, from
 * one pass over /proc/net/tcp; "" where nothing is. */
static void peer_ips(char *vara, char *kiss, size_t size) {
    vara[0] = kiss[0] = '\0';
    FILE *f         = fopen("/proc/net/tcp", "r");
    if (!f) return;
    char line[256];
    while ((!vara[0] || !kiss[0]) && fgets(line, sizeof(line), f)) {
        unsigned lip, lport, rip, rport, st;
        if (sscanf(line, " %*d: %8X:%4X %8X:%4X %2X", &lip, &lport, &rip, &rport, &st) != 5) continue;
        if (st != 1 /* ESTABLISHED */) continue;
        char *out = lport == VARA_CMD_PORT ? vara : lport == KISS_PORT ? kiss : NULL;
        if (!out || out[0]) continue;
        struct in_addr a = {.s_addr = rip};
        inet_ntop(AF_INET, &a, out, size);
    }
    fclose(f);
}

/* xara's "audio dropout:" lines (our xara: PulseAudio underflow / overflow
 * notices): a playback underflow is a gap in what goes on the air, a
 * capture overflow or hole a gap in what xara hears. Reads only what's new. */
static void gaps_poll(void) {
    FILE *f = fopen(log_path, "r");
    if (!f) return;
    if (fseek(f, log_off, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    char line[200];
    while (fgets(line, sizeof(line), f)) {
        /* Half a line at the end: read it next time. A line longer than
         * line[] comes in pieces: each counts as read. */
        if (!strchr(line, '\n') && strlen(line) < sizeof(line) - 1) break;
        log_off = ftell(f);
        if (!strstr(line, "audio dropout:")) continue;
        if (strstr(line, "playback")) gaps_tx++;
        else gaps_rx++;
    }
    fclose(f);
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    xara_status_t s;
    pthread_mutex_lock(&status_lock);
    s = status;
    pthread_mutex_unlock(&status_lock);

    /* Backstop: never keyed for longer than PTT_MAX_S. */
    if (atomic_load(&keyed) && now_s() - atomic_load(&keyed_since) > PTT_MAX_S) {
        set_keyed(false);
        msg_update_text_fmt("XARA: TX stopped after %d s", PTT_MAX_S);
    }

    if (xara_pid > 0 && waitpid(xara_pid, NULL, WNOHANG) == xara_pid) {
        xara_pid = -1;
        set_keyed(false);
        status_clear(); /* its last peer would keep linked() true */
        label_set(state_label, "%s", "The modem stopped");
        label_set(last_label, "See %s - Restart modem to try again", log_path);
    }

    float swr = atomic_exchange(&swr_tripped, 0.0f);
    if (swr > 0.0f && !monitoring) {
        auto_answer = false;
        stop_tx();
        msg_update_text_fmt("XARA: high SWR %.1f:1 - TX stopped, link dropped, Auto answer off", swr);
    }

    /* The band changed by a typed frequency or an APRS preset: XARA's mode
     * and filters on it (band_cb does the band keys). */
    if (param_i_get(cfg.band_id()) != saved_band) band_entered();

    rec_space_tick();

    /* The radio's address every 2 s (it seldom changes). */
    static unsigned ip_tick;
    if (ip_tick++ % 8 == 0 || !lv_label_get_text(top_label)[0]) {
        char ip[INET_ADDRSTRLEN];
        if (wifi_ip(ip, sizeof(ip))) {
            label_set(top_label, "%s  :  %d VARA   %d KISS", ip, VARA_CMD_PORT, KISS_PORT);
        } else {
            label_set(top_label, "No WiFi: connect the radio to WiFi for the phone");
        }
    }

    char pip[INET_ADDRSTRLEN], kip[INET_ADDRSTRLEN];
    peer_ips(pip, kip, sizeof(pip));
    bool vara = pip[0], kiss = kip[0];

    /* Top right: REC while xara records, ANSWER while it may answer calls. */
    bool rec = !monitoring && xara_pid > 0 && rec_path[0];
    label_set(flags_label, "%s%s%s", rec ? "#e63c3c REC#" : "", rec && auto_answer ? "  " : "",
                          !monitoring && auto_answer ? (vara ? "#ffa600 ANSWER#" : "#9a9aa0 ANSWER#") : "");

    if (monitoring) {
        if (mon_pid > 0 && waitpid(mon_pid, NULL, WNOHANG) == mon_pid) {
            mon_pid = -1;
            label_set(last_label, "%s", "See " XARA_MON_LOG " - Restart modem to try again");
        }
        label_set(state_label, "%s", mon_pid > 0 ? "MONITOR  listening only" : "The monitor stopped");
        color_set(state_label, 0x40c0ff);
        char lines[MON_LINES][100];
        unsigned count;
        pthread_mutex_lock(&mon_lock);
        memcpy(lines, mon_lines, sizeof(lines));
        count = mon_count;
        pthread_mutex_unlock(&mon_lock);
        label_set(link_label, "BW %d   never transmits   %u decoded", bw_hz, count);
        label_set(level_label, "%s", lines[0][0] ? lines[0] : "Waiting for VARA traffic...");
        label_set(phone_label, "%s", lines[1]);
        if (mon_pid > 0) label_set(last_label, "%s", lines[2]);
        show(tx_label, false);
        return;
    }

    /* xara's handlers for SIGUSR1/2 are in before its status feed starts:
     * a signal sent earlier would end it. */
    if (xara_pid > 0 && s.seen && !busy_sent) {
        if (ignore_busy) kill(xara_pid, SIGUSR1);
        busy_sent = true;
    }
    /* Auto answer: the gate opens only while a phone app is on the VARA
     * port (nothing on the radio could take the call otherwise). */
    bool want_open = auto_answer && vara;
    if (xara_pid > 0 && s.seen && want_open != gate_open) {
        kill(xara_pid, want_open ? SIGRTMIN : SIGRTMIN + 1);
        gate_open = want_open;
    }

    if (xara_pid > 0 && s.seen) {
        if (s.peer[0]) {
            label_set(state_label, "%s  %s", s.state, s.peer);
            color_set(state_label, 0x3cd25a);
        } else {
            label_set(state_label, "%s", "READY  not connected");
            color_set(state_label, 0xffffff);
        }
        char sn[48] = "S/N --";
        if (s.have_sn) snprintf(sn, sizeof(sn), "S/N %+.1f dB", s.sn);
        /* AFC: how far off the other station is, as xara corrects it. */
        if (s.have_afc) snprintf(sn + strlen(sn), sizeof(sn) - strlen(sn), "   AFC %+.0f Hz", s.afc);
        if (s.have_bps) {
            label_set(link_label, "Level %d   %d bps   %s   BW %s", s.level, s.bps, sn, s.bw);
        } else {
            label_set(link_label, "%s   BW %s%s", sn, s.bw,
                                  s.busy ? (ignore_busy ? "   busy (ignored)" : "   channel busy") : "");
        }
        if (s.last_line[0]) label_set(last_label, "%s", s.last_line);
        pass_place(pass_lo_line, s.pass_lo);
        pass_place(pass_hi_line, s.pass_hi);
        /* The bandwidth in use, also when the phone app changed it. */
        if (s.bw[0] && strcmp(s.bw, shown_bw) != 0) {
            snprintf(shown_bw, sizeof(shown_bw), "%s", s.bw);
            int b = atoi(s.bw);
            if (b == 500 || b == 2300 || b == 2750) bw_hz = b;
            rx_filter_apply();
            buttons_refresh(&btn_bw);
        }
    }
    gaps_poll();
    if (atomic_load(&keyed) && atomic_load(&tx_meter))
        label_set(level_label, "TX %.1f dB   ALC %.1f   PWR %.1f W   max %.1f W   gaps TX %u RX %u", level_db,
                              atomic_load(&tx_alc), atomic_load(&tx_pwr), atomic_load(&tx_max_pwr), gaps_tx, gaps_rx);
    else if (atomic_load(&last_max_pwr) > 0.0f)
        label_set(level_label, "TX level %.1f dB (MFK)   last TX max %.1f W   gaps TX %u RX %u", level_db,
                              atomic_load(&last_max_pwr), gaps_tx, gaps_rx);
    else
        label_set(level_label, "TX level %.1f dB (MFK knob)   gaps TX %u RX %u", level_db, gaps_tx, gaps_rx);

    if (vara && kiss) label_set(phone_label, "Phone: WoAD %s   KISS %s", pip, kip);
    else if (vara) label_set(phone_label, "Phone: WoAD connected  %s", pip);
    else if (kiss) label_set(phone_label, "Phone: KISS (APRS) connected  %s", kip);
    else label_set(phone_label, "%s", "Phone: waiting for WoAD or APRSdroid");

    char note[64];
    pthread_mutex_lock(&bt_lock);
    snprintf(note, sizeof(note), "%s", bt_note);
    pthread_mutex_unlock(&bt_lock);
    if (strstr(note, "connected")) label_set(phone_label, "%s", note);
    else if (note[0] && !vara && !kiss) label_set(phone_label, "Phone: waiting   %s", note);

    show(tx_label, atomic_load(&keyed));
}

/* NR, NB, DNF or DNF auto turned on while open (CAT, a front-panel
 * control): off again at once, the setting kept, as in JS8. On whichever
 * thread changed it, so radio calls only. */
static void dsp_held_cb(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    radio_set_rx_dsp_off(true);
}

static lv_obj_t *row(lv_coord_t y, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(dialog.obj);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_width(l, DIALOG_WIDTH - 2 * X);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    lv_obj_set_pos(l, X, y);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    top_label = row(TOP_Y, &sony_26);
    lv_obj_set_style_text_color(top_label, lv_color_hex(0xffa600), 0);
    lv_obj_set_width(top_label, DIALOG_WIDTH - 2 * X - 190);
    flags_label = lv_label_create(dialog.obj);
    lv_obj_set_style_text_font(flags_label, &sony_26, 0);
    lv_label_set_recolor(flags_label, true);
    lv_label_set_text(flags_label, "");
    lv_obj_align(flags_label, LV_ALIGN_TOP_RIGHT, -X, TOP_Y);
    level_load();
    auto_answer = false; /* never on when XARA opens: answering transmits */
    full_speed  = false; /* the operator chooses it each time */
    ignore_busy = false; /* sending over a busy channel: chosen each time too */
    monitoring  = false;
    rec_path[0] = '\0';

    wf_box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(wf_box);
    lv_obj_clear_flag(wf_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(wf_box, WIDTH, WF_H);
    lv_obj_set_pos(wf_box, INSET, WF_Y);
    pass_lo_line = pass_line();
    pass_hi_line = pass_line();

    tx_label = lv_label_create(wf_box);
    lv_obj_set_style_text_font(tx_label, &sony_28, 0);
    lv_obj_set_style_text_color(tx_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(tx_label, lv_color_hex(0xe63c3c), 0);
    lv_obj_set_style_bg_opa(tx_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(tx_label, 14, 0);
    lv_label_set_text(tx_label, "TX");
    lv_obj_align(tx_label, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_obj_add_flag(tx_label, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t y = WF_Y + WF_H + 6;
    state_label  = row(y, &sony_36);
    link_label   = row(y += 44, &sony_26);
    level_label  = row(y += 32, &sony_22);
    phone_label  = row(y += 28, &sony_22);
    last_label   = row(y += 28, &sony_20);
    lv_obj_set_style_text_color(level_label, lv_color_hex(0x9a9aa0), 0);
    lv_obj_set_style_text_color(last_label, lv_color_hex(0x9a9aa0), 0);
    shown_bw[0] = '\0';

    /* The radio as JS8 sets it, but 100-3000 Hz (2750 starts at 125): USB-D, the base's noise
     * reduction and notches off, 5 W at most. All put back on close. */
    mem_save(MEM_BACKUP_ID); /* where XARA was opened: back there on close, as JS8 */
    saved_band = param_i_get(cfg.band_id());
    saved_mode = cparam_i_get(cfg.cur.mode());
    if (saved_mode != x6100_mode_usb_dig) cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);
    /* The mode keys and VFO A/B stay off while open (a VARA signal in LSB or
     * CW would go out wrong). The band keys come to band_cb, which changes
     * the band itself (not while sending or linked); the VFO knob works. */
    lm_set_mode(true);
    lm_set_ab(true);
    lm_set_band(true);
    saved_filter_low  = cparam_i_get(cfg.filter.low());
    saved_filter_high = cparam_i_get(cfg.filter.high());
    filter_saved      = true;
    rx_filter_apply();
    radio_set_tx_filter(TX_FILTER_LOW, TX_FILTER_HIGH);
    radio_set_rx_dsp_off(true);
    dsp_watch[0] = subject_subscribe((Subject *)cfg.dsp.nr(), dsp_held_cb, NULL);
    dsp_watch[1] = subject_subscribe((Subject *)cfg.dsp.nb(), dsp_held_cb, NULL);
    dsp_watch[2] = subject_subscribe((Subject *)cfg.dsp.dnf(), dsp_held_cb, NULL);
    dsp_watch[3] = subject_subscribe((Subject *)cfg.dsp.dnf_auto(), dsp_held_cb, NULL);
    if (param_f_get(cfg.pwr()) > TX_PLAYER_MAX_PWR_W) {
        radio_set_pwr(TX_PLAYER_MAX_PWR_W);
        msg_update_text_fmt("Power limited to %0.0fW", TX_PLAYER_MAX_PWR_W);
    }

    wf_start();

    stale_kill();
    atomic_store(&stop_threads, false);
    atomic_store(&keyed, false);
    atomic_store(&level_restore, false);
    atomic_store(&vol_is_level, false);
    atomic_store(&swr_tripped, 0.0f);
    rec_full = false;
    pthread_t *th[]             = {&ptt_thread, &stream_thread, &level_thread, &txmon_thread, &btkiss_thread};
    void *(*fn[])(void *)       = {ptt_main, stream_main, level_main, txmon_main, btkiss_main};
    unsigned started            = 0;
    bt_note[0]                  = '\0';
    for (; started < 5; started++)
        if (pthread_create(th[started], NULL, fn[started], NULL) != 0) break;
    threads_up = started == 5;
    if (!threads_up) {
        atomic_store(&stop_threads, true);
        for (unsigned i = 0; i < started; i++) pthread_join(*th[i], NULL);
        lv_label_set_text(state_label, "XARA could not start");
    } else {
        xara_start();
    }

    timer = lv_timer_create(timer_cb, 250, NULL);
    timer_cb(timer);
}

static void destruct_cb() {
    if (timer) lv_timer_del(timer);
    timer = NULL;

    if (settings_list) lv_group_set_editing(keyboard_group, settings_was_editing);
    settings_list = NULL; /* deleted with the dialog, right after this */
    level_item    = NULL;
    level_editing = false;
    level_save();
    if (monitoring) monitor_stop();
    monitoring = false;
    xara_stop();
    if (threads_up) {
        atomic_store(&stop_threads, true);
        pthread_join(ptt_thread, NULL);
        pthread_join(stream_thread, NULL);
        pthread_join(level_thread, NULL);
        pthread_join(txmon_thread, NULL);
        pthread_join(btkiss_thread, NULL);
        threads_up = false;
    }
    set_keyed(false);
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    atomic_store(&vol_is_level, false);
    for (unsigned i = 0; i < sizeof(dsp_watch) / sizeof(dsp_watch[0]); i++) {
        if (dsp_watch[i]) param_unsubscribe(dsp_watch[i]);
        dsp_watch[i] = NULL;
    }
    wf_stop();
    wf_box = NULL;

    radio_set_pwr(param_f_get(cfg.pwr()));
    if (filter_saved) {
        filter_set(saved_filter_low, saved_filter_high); /* USB-D's own, still in USB-D */
        radio_set_tx_filter(param_i_get(cfg.dsp.tx_filter_low()), param_i_get(cfg.dsp.tx_filter_high()));
        radio_set_rx_dsp_off(false);
        filter_saved = false;
    }
    /* This band's own mode back in its memory, then where XARA was opened. */
    if (saved_mode != x6100_mode_usb_dig && param_i_get(cfg.band_id()) == saved_band)
        cparam_i_set(cfg.cur.mode(), saved_mode);
    mem_load(MEM_BACKUP_ID);
    lm_set_mode(false);
    lm_set_ab(false);
    lm_set_band(false);
}

/* ------------------------------------------------------------ buttons */

/* A band key (the main screen leaves band changes to us, lm_set_band):
 * never while sending or linked, the radio and an amplifier would change
 * band mid-burst. The band left gets its own mode back first, so its
 * memory doesn't keep USB-D. */
static void band_cb(lv_event_t *e) {
    if (atomic_load(&keyed) || linked()) {
        msg_update_text_fmt("Not while sending or linked");
        return;
    }
    if (saved_mode != x6100_mode_usb_dig && param_i_get(cfg.band_id()) == saved_band)
        cparam_i_set(cfg.cur.mode(), saved_mode);
    cfg_band_load_next(lv_event_get_code(e) == EVENT_BAND_UP);
    band_entered();
    aprs_idx = -1;
    buttons_refresh(&btn_aprs);
}

/* On a new band, with its own mode: that mode kept for when it's left, and
 * the radio back to XARA's USB-D and filters. */
static void band_entered(void) {
    saved_band = param_i_get(cfg.band_id());
    saved_mode = cparam_i_get(cfg.cur.mode());
    if (saved_mode != x6100_mode_usb_dig) cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);
    radio_set_tx_filter(TX_FILTER_LOW, TX_FILTER_HIGH);
    rx_filter_apply();
}

static const char *bw_label(void) {
    static char s[24];
    snprintf(s, sizeof(s), "BW:\n%d Hz", bw_hz);
    return s;
}

static bool settings_guard(void);

static void bw_cb(button_data_t *btn) {
    (void)btn;
    if (settings_guard()) return;
    if (linked()) {
        msg_update_text_fmt("Disconnect first (in WoAD)");
        return;
    }
    bw_hz = bw_hz == 500 ? 2300 : bw_hz == 2300 ? 2750 : 500;
    rx_filter_apply();
    buttons_refresh(&btn_bw);
    xara_restart();
}

static const char *aprs_label(void) {
    static char s[24];
    if (aprs_idx < 0) return "APRS\nfreq";
    snprintf(s, sizeof(s), "APRS\n%s", aprs_presets[aprs_idx].label);
    return s;
}

/* VARA HF APRS presets, 40 m and 30 m in turn: the frequency (as typed in,
 * so the band data follows), USB-D, our filter again, and BW 500. */
static void aprs_cb(button_data_t *btn) {
    (void)btn;
    if (settings_guard()) return;
    if (atomic_load(&keyed) || linked()) {
        msg_update_text_fmt("Not while sending or linked");
        return;
    }
    aprs_idx = (aprs_idx + 1) % (int)APRS_PRESETS;
    main_screen_set_freq(aprs_presets[aprs_idx].hz);
    if (param_i_get(cfg.band_id()) != saved_band) band_entered(); /* 40 m to 30 m: that band's mode kept for close */
    if (cparam_i_get(cfg.cur.mode()) != x6100_mode_usb_dig) cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);
    radio_set_tx_filter(TX_FILTER_LOW, TX_FILTER_HIGH);
    buttons_refresh(&btn_aprs);
    msg_update_text_fmt("VARA APRS %s  %u.%03u.%01u", aprs_presets[aprs_idx].label, aprs_presets[aprs_idx].hz / 1000000u,
                        (aprs_presets[aprs_idx].hz / 1000u) % 1000u, (aprs_presets[aprs_idx].hz / 100u) % 10u);
    if (bw_hz != 500) {
        bw_hz = 500;
        buttons_refresh(&btn_bw);
        xara_restart();
    }
    rx_filter_apply(); /* the new band's filter settings may differ */
}

static const char *monitor_label(void) {
    return monitoring ? "Monitor:\nON" : "Monitor:\nOFF";
}

/* Monitor: xara-monitor in place of xara, listening to VARA traffic between
 * other stations; it can't transmit. Press again for the modem. */
static void monitor_cb(button_data_t *btn) {
    (void)btn;
    if (settings_guard()) return;
    if (!monitoring) {
        if (atomic_load(&keyed) || linked()) {
            msg_update_text_fmt("Not while sending or linked");
            return;
        }
        xara_stop(); /* clears status */
        monitoring = true;
        monitor_start();
        msg_update_text_fmt("Monitor: listening to VARA traffic, the modem is off");
    } else {
        monitor_stop();
        monitoring = false;
        lv_obj_set_style_text_color(state_label, lv_color_white(), 0);
        lv_label_set_text(level_label, "");
        lv_label_set_text(phone_label, "");
        lv_label_set_text(last_label, "");
        xara_start();
        msg_update_text_fmt("Monitor off: the modem is back");
    }
    buttons_refresh(&btn_monitor);
}

/* ------------------------------------------------------------ Settings */

/* As JS8's Settings list: MFK moves, press switches the line in place,
 * ESC or Close closes. */
#define SET_BUSY   1
#define SET_RECORD 2
#define SET_ANSWER 3
#define SET_LEAD   4
#define SET_LEVEL  5
#define SET_BUF    6
#define SET_RBUF   7
#define SET_FULL   8
#define SET_PRIO   9


static const char *settings_text(int which) {
    switch (which) {
    case SET_BUSY: return ignore_busy ? "Busy detector: IGNORE" : "Busy detector: ON";
    case SET_RECORD: return record_on ? "Record (FLAC to the SD card): On" : "Record (FLAC to the SD card): Off";
    case SET_ANSWER: return auto_answer ? "Auto answer: ON (answers calls, transmits)" : "Auto answer: Off";
    case SET_FULL: return full_speed ? "Full speed (registered): ON" : "Full speed (registered): Off";
    case SET_PRIO: {
        static char pbuf[48];
        snprintf(pbuf, sizeof(pbuf), "CPU priority (test): %s", prio_names[prio_idx]);
        return pbuf;
    }
    case SET_LEVEL: {
        static char lbuf[48];
        snprintf(lbuf, sizeof(lbuf), level_editing ? "TX level: %.1f dB  < turn MFK, press >" : "TX level: %.1f dB",
                 level_db);
        return lbuf;
    }
    case SET_BUF: {
        static char bbuf[48];
        if (buf_ms[buf_idx] == 0) return "Send buffer: xara's own (20 ms)";
        snprintf(bbuf, sizeof(bbuf), "Send buffer: %d ms", buf_ms[buf_idx]);
        return bbuf;
    }
    case SET_RBUF: {
        static char rbbuf[48];
        if (buf_ms[rbuf_idx] == 0) return "Receive buffer: xara's own (10 ms)";
        snprintf(rbbuf, sizeof(rbbuf), "Receive buffer: %d ms", buf_ms[rbuf_idx]);
        return rbbuf;
    }
    case SET_LEAD: {
        static char buf[48];
        snprintf(buf, sizeof(buf), "PTT lead (radio, amp): %d ms", lead_ms[lead_idx]);
        return buf;
    }
    }
    return "";
}

static void settings_close(void) {
    if (!settings_list) return;
    if (level_editing) level_save();
    level_editing = false;
    level_item    = NULL;
    /* Deleted from its own key handler: wait for the key's release, or LVGL
     * turns the held key into a long press on the dialog (digi_menu.c). */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_wait_release(indev);
    lv_obj_del_async(settings_list);
    settings_list = NULL;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, settings_was_editing);
}

/* Any other button while the list is open just closes it. */
static bool settings_guard(void) {
    if (!settings_list) return false;
    settings_close();
    msg_update_text_fmt("Settings closed");
    return true;
}

static void settings_key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    /* The TX level line while pressed: the knob's turns set the level. */
    if (level_editing && lv_event_get_target(e) == level_item &&
        (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        level_step(key == LV_KEY_RIGHT || key == LV_KEY_UP);
        lv_label_set_text(lv_obj_get_child(level_item, 0), settings_text(SET_LEVEL));
        return;
    }
    switch (key) {
    case LV_KEY_ESC: settings_close(); break;
    case LV_KEY_LEFT:
    case LV_KEY_UP: lv_group_focus_prev(keyboard_group); break;
    case LV_KEY_RIGHT:
    case LV_KEY_DOWN: lv_group_focus_next(keyboard_group); break;
    case KEY_VOL_LEFT_EDIT:
    case KEY_VOL_LEFT_SELECT: radio_change_vol(-1); break;
    case KEY_VOL_RIGHT_EDIT:
    case KEY_VOL_RIGHT_SELECT: radio_change_vol(1); break;
    }
}

static void settings_line_color(lv_obj_t *b, int which) {
    lv_obj_set_style_text_color(b, which == SET_ANSWER && auto_answer ? lv_color_hex(0xffa600) : lv_color_white(), 0);
}

/* Settings that take a modem restart: not while sending or linked (it
 * drops the link). True when refused. */
static bool restart_refused(void) {
    if (!monitoring && (atomic_load(&keyed) || linked())) {
        msg_update_text_fmt("Disconnect first (in WoAD)");
        return true;
    }
    return false;
}

static void modem_restart(void) {
    if (!monitoring) {
        xara_stop();
        xara_start();
    }
}

static void settings_item_cb(lv_event_t *e) {
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    switch (which) {
    case SET_BUSY:
        /* xara holds APRS/KISS broadcasts while its busy detector hears the
         * channel in use; on a noisy band that can be all the time. */
        ignore_busy = !ignore_busy;
        if (xara_pid > 0 && busy_sent) kill(xara_pid, ignore_busy ? SIGUSR1 : SIGUSR2);
        msg_update_text_fmt(ignore_busy ? "Busy detector ignored: sends over a busy channel" : "Busy detector on");
        break;
    case SET_RECORD:
        if (restart_refused()) return;
        record_on = !record_on;
        modem_restart();
        if (record_on && rec_path[0]) msg_update_text_fmt("Recording to %s", rec_path);
        else if (record_on) msg_update_text_fmt("Record on: from the next modem start");
        else msg_update_text_fmt("Record off");
        break;
    case SET_LEAD:
        if (restart_refused()) return;
        lead_idx    = (lead_idx + 1) % LEADS;
        level_dirty = true;
        level_save();
        modem_restart();
        msg_update_text_fmt("PTT lead %d ms: keyed that long before each burst's audio", lead_ms[lead_idx]);
        break;
    case SET_BUF:
    case SET_RBUF:
        if (restart_refused()) return;
        if (which == SET_RBUF) rbuf_idx = (rbuf_idx + 1) % BUFS;
        else buf_idx = (buf_idx + 1) % BUFS;
        level_dirty = true;
        level_save();
        modem_restart();
        msg_update_text_fmt(which == SET_RBUF ? "Receive buffer %d ms (0 = xara's 10): smaller answers sooner"
                                              : "Send buffer %d ms (0 = xara's 20): too small and sending runs dry",
                            buf_ms[which == SET_RBUF ? rbuf_idx : buf_idx]);
        break;
    case SET_LEVEL:
        /* Press to set it with the knob, press again when done. */
        level_editing = !level_editing;
        lv_group_set_editing(keyboard_group, level_editing);
        if (!level_editing) level_save();
        msg_update_text_fmt(level_editing ? "TX level: turn the MFK knob, press when done (ALC should stay at zero)"
                                          : "TX level %.1f dB saved", level_db);
        break;
    case SET_PRIO:
        if (restart_refused()) return;
        prio_idx    = (prio_idx + 1) % PRIOS;
        level_dirty = true;
        level_save();
        modem_restart();
        msg_update_text_fmt("CPU priority: %s (see ptt.log for PulseAudio's threads)", prio_names[prio_idx]);
        break;
    case SET_FULL:
        if (restart_refused()) return;
        full_speed = !full_speed;
        modem_restart();
        msg_update_text_fmt(full_speed ? "Full speed ON: VARA's registered speed levels (your licence)"
                                       : "Full speed off: free speed levels only");
        break;
    case SET_ANSWER:
        auto_answer = !auto_answer;
        msg_update_text_fmt(auto_answer ? "Auto answer ON: calls to you are answered while WoAD is connected"
                                        : "Auto answer off: no calls answered");
        break;
    }
    lv_obj_t *b = lv_event_get_target(e);
    lv_label_set_text(lv_obj_get_child(b, 0), settings_text(which));
    settings_line_color(b, which);
}

static void settings_list_focused_cb(lv_event_t *e) {
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_OFF);
}

static lv_obj_t *settings_item(const char *label) {
    lv_obj_t *b = lv_list_add_btn(settings_list, NULL, label);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1830a0), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(b, settings_list_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(b, settings_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_DOT);
    return b;
}

static void settings_close_cb(lv_event_t *e) {
    (void)e;
    settings_close();
}

static void settings_cb(button_data_t *btn) {
    (void)btn;
    if (settings_list) { /* Settings... again closes it */
        settings_close();
        return;
    }
    settings_was_editing = lv_group_get_editing(keyboard_group);
    lv_group_remove_obj(dialog.obj);
    settings_list = lv_list_create(dialog.obj);
    lv_obj_set_size(settings_list, 560, 340);
    lv_obj_align(settings_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(settings_list, &sony_24, 0);
    lv_obj_set_style_bg_color(settings_list, lv_color_hex(0x202020), 0);
    lv_obj_t *title = lv_list_add_text(settings_list, "Settings");
    lv_obj_set_style_text_font(title, &sony_22, 0);

    lv_obj_t *first = NULL;
    level_editing = false;
    for (int which = SET_BUSY; which <= SET_PRIO; which++) {
        lv_obj_t *b = settings_item(settings_text(which));
        if (which == SET_LEVEL) level_item = b;
        lv_obj_add_event_cb(b, settings_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
        settings_line_color(b, which);
        if (!first) first = b;
    }
    lv_obj_t *about = lv_list_add_text(settings_list, "VARA: EA5HVK's modem. xara: x0rro's compatible modem");
    lv_obj_set_style_text_font(about, &sony_20, 0);
    lv_label_set_long_mode(about, LV_LABEL_LONG_WRAP);
    lv_obj_t *close = settings_item("Close");
    lv_obj_set_style_text_color(close, lv_color_hex(0xffc040), 0);
    lv_obj_add_event_cb(close, settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(first);
    lv_group_set_editing(keyboard_group, false);
}

static void restart_cb(button_data_t *btn) {
    (void)btn;
    if (settings_guard()) return;
    xara_restart();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            /* As JS8: the first ESC stops TX (sending or linked), the next closes. */
            if (atomic_load(&keyed) || linked()) {
                stop_tx();
            } else {
                dialog_destruct();
            }
            break;

        /* MFK: the TX level, live (also while sending). */
        case LV_KEY_LEFT:
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
        case LV_KEY_UP:
            level_step(key == LV_KEY_RIGHT || key == LV_KEY_UP);
            msg_update_text_fmt("TX level %.1f dB", level_db);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}
