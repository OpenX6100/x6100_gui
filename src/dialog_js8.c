/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 (receive)
 *
 *  Layout follows the FT8 app: a waterfall strip across the top and a
 *  message list over the rest of it. Decoding lives in src/js8 (no LVGL);
 *  its callbacks arrive on worker threads and reach the GUI thread through
 *  JS8's own queues (ev_push, the waterfall ring), not scheduler_put().
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_setname_np (the radio build defines it for every file) */
#endif

#include "dialog_js8.h"

#include "js8/js8_rx.h"
#include "js8/js8_tx.h"
#include "js8/js8_ops.h"
#include "js8/js8_speed.h"
#include "js8/js8_map.h"
#include "js8/js8_history.h"
#include "js8/js8_wisdom.h"
#include "qth/qth.h"
#include "qso_log.h"

#include "audio.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cfg/digital_modes.h"
#include "dialog.h"
#include "dsp.h"
#include "events.h"
#include "gps.h"
#include "keyboard.h"
#include "lock_manager.h"
#include "main.h"
#include "main_screen.h"
#include "msg.h"
#include "radio.h"
#include "keypad.h"
#include "scheduler.h"
#include "spectrum.h"
#include "styles.h"
#include "textarea_window.h"
#include "tx_info.h"
#include "tx_player.h"
#include "util.h"
#include "waterfall.h"
#include "widgets/lv_finder.h"
#include "js8_wf.h"
#include "js8_clock.h"

#include <liquid/liquid.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Asked of R1CBU 1.0's audio subscription, which divides its 48 kHz by 4
 * with the Kaiser decimator 0.34 used to give us 11025 Hz: JS8's own rate,
 * nothing left to resample. */
#define SAMPLE_RATE      12000
/* R1CBU 1.0's dialog (DIALOG_WIDTH x DIALOG_HEIGHT, 796 x 345) has a 1 px
 * border with 8 px corners (none in Black): everything sits INSET inside
 * it, the closest a square corner gets without poking through the curve.
 * 0.34's dialog image needed 13 px (771 x 325). */
#define INSET            4
#define WIDTH            (DIALOG_WIDTH - 2 * INSET)   /* 788 */
#define WF_HEIGHT        (DIALOG_HEIGHT - 2 * INSET)  /* 337 */
#define WF_VISIBLE       55     /* waterfall rows left uncovered by the list */
#define TX_BAR_H         30     /* TX status line between waterfall and list */
#define TX_BAR_IDLE      0x000000 /* its background when idle, half see-through */
#define TX_TEXT_MAX      500    /* longest message the compose window takes: past 20 frames
                                   (~345-425 characters), so the frame limit is what stops you,
                                   as on desktop, whose box has no limit */
#define OFFER_MS         (5 * 60 * 1000) /* how long an offered reply stays on Reply */
#define TEXT_MAX         64     /* INFO / STATUS */
#ifndef JS8_TEXTS_PATH
#define JS8_TEXTS_PATH   "/mnt/js8_texts.txt" /* DATA partition; editable on a PC */
#endif
#ifndef JS8_INBOX_PATH
#define JS8_INBOX_PATH   "/mnt/js8_inbox.txt"   /* one message per line */
#endif
#ifndef JS8_HELD_PATH
#define JS8_HELD_PATH    "/mnt/js8_held.txt"    /* MSG TO: messages held for others */
#endif
#ifndef JS8_SAVED_PATH
#define JS8_SAVED_PATH   "/mnt/js8_saved.txt"   /* saved messages, one a line (empty: none) */
#endif
#define SAVED_N          10     /* saved messages (Query > Saved messages >) */
/* <MYVERSION> and the default STATUS send this, so other stations see which
 * firmware answered: OpenX6100's own version (main.h). */
#define JS8_APP_VERSION  "OpenX6100 JS8 " VERSION /* desktop's is its version number */
#ifndef JS8_LOG_PATH
#define JS8_LOG_PATH     "/mnt/js8call_log.adi" /* desktop JS8Call's name */
#endif
#ifndef JS8_MAP_DATA_PATH
#define JS8_MAP_DATA_PATH "/usr/share/x6100/js8/js8_map.bin" /* tools/map_data */
#endif
#ifndef JS8_CTY_PATH
#define JS8_CTY_PATH     "/usr/share/x6100/cty.dat" /* third-party/cty, the same file FT8's countries use (dxcc.c) */
#endif
#define JS8_CTY_SD_PATH  "/mnt/cty.dat" /* a newer list put on the card wins, as for FT8 */
#ifndef JS8_HISTORY_PATH
#define JS8_HISTORY_PATH "/mnt/js8_history.db" /* every station exchanged with: INFO, STATUS, QSO texts */
#endif
#ifndef JS8_WISDOM_PATH
#define JS8_WISDOM_PATH  "/mnt/js8_wisdom.dat" /* the decoder's measured FFT plans (desktop: js8call_wisdom.dat) */
#endif
#ifndef JS8_QSO_DB_PATH
#define JS8_QSO_DB_PATH  "/mnt/qso_log.db" /* the radio's QSO log, read-only: new grids and countries */
#endif
#define WF_ROWS_PER_SEC  15     /* waterfall rows per second of audio, as the main screen's */
#define WF_ROW_SAMPLES   (SAMPLE_RATE / WF_ROWS_PER_SEC)
#define WF_QUEUE         16     /* rows waiting to be drawn (jitter buffer) */
#define WF_TICK_MS       5      /* how often the drawing timer looks at the clock */
#define HB_ADJUST_MS     8000   /* setting the HB interval ends after this idle */
#define HB_PAUSE_MS      (10 * 60 * 1000) /* heartbeats wait this long after you send */
#define ALERT_COLOR      0x6a2ca0 /* rows matching an alert word */
/* params.js8_alerts bits: what beeps (alert words always highlight). */
#define JS8_ALERT_BEEP     0x01 /* beeping at all */
#define JS8_ALERT_TO_ME    0x02 /* a message to your call (not a heartbeat ack) */
#define JS8_ALERT_INBOX    0x04 /* a MSG saved to the inbox */
#define JS8_ALERT_CQ       0x08 /* someone calling CQ */
#define JS8_ALERT_NEW      0x10 /* a station heard for the first time, not in the log */
/* The waterfall is drawn relative to the noise floor, so it works at any
 * audio level: WF_MIN_DB..WF_MAX_DB above the floor spans the palette. */
#define WF_MIN_DB        0
#define WF_MAX_DB        30

/* The receive filter while the app is open, and the range the decoder
 * searches (desktop searches its waterfall filter's edges). Yours comes
 * back when the app closes. */
#define JS8_FILTER_LOW   200
#define JS8_FILTER_HIGH  3000
/* The TX filter while the app is open: the radio's own default, so it
 * only changes anything if yours was set narrower (for voice). JS8 never
 * sends below 500 Hz; the top must pass a signal up to 3000 Hz. */
#define JS8_TX_FILTER_LOW  160
#define JS8_TX_FILTER_HIGH 3000

#define HISTORY          300    /* messages kept for re-filtering */
#define MAX_ROWS         200    /* rows shown before trimming to KEEP_ROWS */
#define KEEP_ROWS        150
#define CQ_MIN_INTERVAL  1      /* auto CQ: minutes after our last TX ends (desktop's shortest: 1) */
#define CQ_MAX_INTERVAL  30
#define EOT_MARK         "\xE2\x99\xA2" /* desktop JS8Call's end-of-transmission mark, U+2662 */
#define READ_PAUSE_MS    30000 /* list follows new rows again this long after the last MFK move */
#define HOLD_MS          500      /* a button held this long is a hold (1 s elsewhere) */
#define CUSTOM_MIN_HZ    1800000  /* custom dial frequency: 160m ... */
#define CUSTOM_MAX_HZ    54000000 /* ... to the top of 6m */

typedef enum {
    SHOW_ALL,       /* everything, heartbeats included */
    SHOW_NO_HB,     /* everything except heartbeats and SNR reports */
    SHOW_DIRECTED,  /* to me, or to a group (not HB/CQ) */
    SHOW_COUNT,
} show_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void audio_cb(size_t n, float *samples);
static void gps_msg_cb(void *s, lv_msg_t *m);
static void finder_invalidate_band(int32_t hz);
static void finder_sync(void);
static void cursor_place(void);
static bool selected_speed(js8_speed_t *out);
static int32_t   finder_shown = -1; /* the red band's offset on screen, -1 not drawn yet */
static lv_obj_t *cursor_box;        /* the green band (cursor_place) */

/* R1CBU 1.0 hands text settings out as copies: the station's call and grid
 * for the GUI thread, valid until the next call. */
static const char *my_call(void) {
    static char buf[PARAM_TEXT_MAX];
    return param_t_get_into(cfg.callsign(), buf, sizeof(buf));
}

static void    *gps_sub;            /* MSG_GPS while JS8 is open */
static double   gps_lat, gps_lon;   /* the latest 2D/3D fix */
static time_t   gps_when;           /* when it came; 0: none since JS8 opened */
static time_t   gps_seen;           /* the latest GPS report, fix or not */
#define GPS_FRESH_S 120             /* a fix this recent is where we are */

static bool gps_fix_current(void) {
    return gps_when && time(NULL) - gps_when <= GPS_FRESH_S;
}

/* Our grid: a current GPS fix's, else the saved QTH (APP > QTH). The GPS
 * grid isn't saved (the radio's GPS screen does that); `chars` 6 for what
 * goes on the air, 10 for <MYGRID12>. The radio's style: CN89sb. */
static const char *grid_of(int chars) {
    static char buf[PARAM_TEXT_MAX];
    char        g[12];
    if (gps_fix_current() && js8_latlon_to_grid(gps_lat, gps_lon, chars, g, sizeof(g))) {
        for (int i = 4; i < 6 && g[i]; i++) g[i] = (char)tolower((unsigned char)g[i]);
        snprintf(buf, sizeof(buf), "%s", g);
        return buf;
    }
    return param_t_get_into(cfg.qth(), buf, sizeof(buf));
}

static const char *my_grid(void) {
    return grid_of(6);
}

static uint32_t audio_sub = AUDIO_SUB_INVALID; /* receive audio, 12 kHz (dsp_audio_subscribe_float) */
static void key_cb(lv_event_t *e);

static const char *show_label_getter(void);
static void        show_cb(button_data_t *btn);
static void        clear_cb(button_data_t *btn);
static void        time_sync_reset(void);
static const char *time_label_getter(void);
static void        time_cb(button_data_t *btn);
static void        time_hold_cb(button_data_t *btn);
static void        time_apply_found(void);
static const char *map_view_label_getter(void);
static void        map_view_cb(button_data_t *btn);
static void        map_follow_cb(button_data_t *btn);
static void        map_update(bool force);
static void        map_popup_add(const char *call);
static void        map_qso_add(const char *call, bool ring);
static void        map_qrz_add(const char *call);
static void        map_status_changed(void);
static bool        map_top_place(void);
static void        map_talk_note(const js8_rx_msg_t *m);
static void        map_pulse_add(const char *call);
static void        map_qrz_clear(const char *call);
static void        map_qrz_show(void);
static void        map_show(bool on);
static void        map_free(void);
static void        rotary_cb(int32_t diff, uint16_t dt);
static void        reply_cb(button_data_t *btn);
static void        send_cb(button_data_t *btn);
static void        stop_tx_cb(button_data_t *btn);
static void        hw_cpy_cb(button_data_t *btn);
static void        cq_cb(button_data_t *btn);
static const char *cq_label_getter(void);
static int64_t     cq_interval_ms(void);
static void        cq_adjust_end(void);
static void        cq_adjust_turn(int32_t diff);
static void        cq_hold_cb(button_data_t *btn);
static void        auto_cq_stop(const char *why);
static void        auto_cq_tick(void);
static void        heartbeat_cb(button_data_t *btn);
static void        query_cb(button_data_t *btn);
static const char *hold_label_getter(void);
static void        hold_cb(button_data_t *btn);
static const char *ever_label_getter(void);
static void        ever_cb(button_data_t *btn);
static void        hpage_close(void);
static bool        starts_with_call(const char *text, char *call, size_t len);
static void        ever_merge(const char *call);
static void        ever_load(void);
static void        hclear_tick(void);
static void        hpage_open(const char *call, int64_t qso_id);
static const char *stations_label_getter(void);
static void        stations_cb(button_data_t *btn);
static void        query_close(void);
static const char *auto_label_getter(void);
static void        auto_cb(button_data_t *btn);
static const char *hb_label_getter(void);
static void        heartbeat_hold_cb(button_data_t *btn);
static void        hb_auto_stop(const char *why);
static void        hb_adjust_start(button_data_t *btn);
static void        hb_resume(void);
static const char *hb_ack_label_getter(void);
static void        hb_ack_cb(button_data_t *btn);
static void        texts_cb(button_data_t *btn);
static bool        tx_queue_at(const char *text, int offset_hz, bool automatic);
static void        user_touch(void);
static void        auto_queue(const js8_auto_result_t *r);
static void        auto_try_send(void);
static void        auto_clear(void);
static int         offer_find(const char *call);
static void        hb_tick(void);
static int64_t     hb_first_ms(void);
static void        load_texts(void);
static void        load_saved(void);
static void        save_saved(void);
static void        macros_fill(const char *text, bool prune, char *out, size_t out_len);
static void        saved_open(int focus);
static void        data_file_notice(const char *notice);
static void        save_texts(void);
static void        compose_open(const char *prefill);
static void        update_tx_bar(void);
static void        tx_start(void);
static void        tx_stop_all(void);
static void        tx_timer_cb(lv_timer_t *t);
static void        compose_close(void);
static void        hb_adjust_end(void);
static void        aprs_cb(button_data_t *btn);
static void        aprs_open(bool more);
static void        aprs_item_done(void);
static void        aprs_close(void);
static bool        popup_guard(void);
static void        js8_next_page_cb(button_data_t *btn);
static void        js8_prev_page_cb(button_data_t *btn);
static void        texts_close(void);
static bool        aprs_prepare(const char *in, char *out, size_t size);
static void        aprs_sent_commit(void);
static bool        any_popup(void);
static void        log_cb(button_data_t *btn);
static void        log_close(void);
static void        log_offer(const char *call);

static void        log_refresh(void);
static const char *inbox_label_getter(void);
static void        inbox_cb(button_data_t *btn);
static void        inbox_close(void);
static void        stored_received(const js8_stored_t *k);
static void        deliver_start(int id, const char *group_call, const char *text);
static void        deliver_end(const char *text, bool completed);
static void        inbox_refresh_button(void);
static void        msg_compose(const char *call, const char *kind);
static void        alerts_cb(button_data_t *btn);
static void        alerts_close(void);
static const char *freq_name(void);
static void        freq_close(void);
static void        freq_show(void);
static void        tune_custom(const char *khz);
static bool        parse_custom(const char *text, int32_t *out);
typedef enum { SP_SEND, SP_REF, SP_FREQ, SP_MODE, SP_NOTE, SP_CLOSE, SP_COUNT } spot_item_t;
static bool        spot_sota; /* the spot form is for SOTA, else POTA */
static bool        spot_parse_freq(const char *text, int32_t *out);
static bool        spot_freq_empty(const char *text);
static void        spot_show(bool sota, spot_item_t focus);
static void        spot_close(void);
static void        aprs_beacon(bool gps, const char *message);
static void        beacon_changed_cb(lv_event_t *e);
#define APRS_COMMENT_MAX 43 /* an APRS position report's comment */
static const char *where_label(void);
static void        alert_check(js8_rx_msg_t *m, bool new_station);
static void        alert_beep(int count);
static bool        beep_start(int count);
static void        swr_guard_tick(void);
static void        swr_guard_reset(void);
static void        beep_log_level(void);
static const char *speed_label_getter(void);
static void        speed_cb(button_data_t *btn);
static void        speed_hold_cb(button_data_t *btn);
static void        decode_toggle(void);
static void        ultra_toggle(void);
static void        speed_warn(void);
static const char *act_label_getter(void);
static void        act_cb(button_data_t *btn);
static void        act_hold_cb(button_data_t *btn);
static const char *prompt_label_getter(void);
static void        prompt_cb(button_data_t *btn);

/* ---- State (UI thread unless noted) ------------------------------------ */

static js8_rx_t *rx;
static js8_tx_t *tx;

static lv_obj_t   *tx_bar;
static lv_timer_t *tx_timer;           /* refreshes the TX bar countdown */
static js8_tx_status_t tx_status;      /* UI-thread copy of the last status */
static char        tx_preview[JS8_RX_TEXT_LEN]; /* what we're sending, as others see it */
static float       base_gain_offset;
static float       qso_freq = -1;     /* the selected station's offset (the green line), -1 none */
/* The selected station: set by MFK or a tap on its row, never by new rows
 * arriving, so Reply still answers it after the list moves on. */
static char        sel_call[JS8_RX_CALL_LEN];
static int         sel_snr;
/* Hold MFK on a station to lock it: turning then only scrolls, and the
 * selection stays until you press another station (or hold it again). */
static bool        sel_locked;
/* The cursor is on a row without a callsign (a QSO's later lines often
 * have none): its offset, whose rows get the green bar; -1 otherwise. It
 * selects nobody: Reply still goes to the selected station. */
static float       cursor_freq = -1;
static bool        press_on_locked; /* this press began on the locked station */
static bool        press_held;      /* this press has already been a hold */
static int64_t     cursor_user_ms;    /* last MFK move on the list */
static atomic_bool keyed;              /* a frame is on the air (TX thread) */
/* Held while an alert beep plays; tx_play takes it after setting keyed, so
 * a beep stops within one part and never goes out with our TX audio. */
static pthread_mutex_t speaker_lock = PTHREAD_MUTEX_INITIALIZER;
/* While the speaker plays a beep the base doesn't send us the receiver:
 * the decoder gets silence instead (same length, so its clock stays in
 * step) and the waterfall pauses. On from speaker-on until beep_guard_end
 * (monotonic ms), a little after speaker-off, for the capture latency. */
static atomic_bool     beep_guard;
static _Atomic int64_t beep_guard_end;
static _Atomic int64_t beep_guard_sq, beep_guard_n; /* capture level during it (log) */
static atomic_int  tx_offset_active;   /* offset of the message being sent */
static bool        composing;          /* compose window open */

static js8_stations_t *stations;       /* who we've heard, who heard us: this band's */

/* One Stations list per dial frequency (7.078 and GhostNet's 7.107 are
 * different nets): going back brings its list back. They last while the
 * radio is on, JS8 closed and reopened included; each station drops off an
 * hour after it was last heard. */
#define BAND_LISTS 16
static struct {
    int32_t         dial_khz;
    js8_stations_t *list;
} band_lists[BAND_LISTS];
static int band_lists_next; /* slot reused when all are taken */
static bool           view_stations;   /* list shows stations, not messages */
/* Show Map: the map covers the waterfall and the list; the Stations view
 * stays underneath (view_stations too), so the MFK selects as there. */
static bool           view_map;
static bool           map_heard_me_only; /* Show in the map: All heard / Heard me */
static lv_obj_t      *wf_box;           /* the waterfall: a hole down to the lower plane (hidden under the map) */
static lv_obj_t      *map_box, *map_canvas, *map_status; /* made when the map first opens */
static lv_obj_t      *map_qrz_label;    /* "QRZ 2  W7XYZ K9DEF" under the status line */
static lv_obj_t      *map_stats_label;  /* "14 heard  5 hear you  DX ..." under the status line */
/* Stations that called CQ lately: green in the Stations view, a "CQ" tag
 * on the map, for 5 min (VE7NHW). */
#define CQ_HEARD_N  16
#define CQ_HEARD_MS 300000
static struct {
    char    call[JS8_RX_CALL_LEN];
    int64_t until_ms; /* monotonic */
} cq_heard[CQ_HEARD_N];
static int64_t my_cq_until_ms; /* monotonic: our own CQ, the tag on our square too (VE7NHW) */
static void cq_heard_add(const char *call);
static bool cq_heard_on(const char *call, int64_t now);
static int64_t now_mono_ms(void);
/* QRZ: stations that sent something to your call (not heartbeat replies)
 * while the message list wasn't showing, newest last. On the map and over
 * the Stations view until you select them, send to them, or go back to the
 * message list. */
#define MAP_QRZ 8
#define MAP_QRZ_COLOR  0xFFFF00 /* GridTracker's QRZ ("calling me") yellow */
static char map_qrz[MAP_QRZ][JS8_RX_CALL_LEN];
static int  map_qrz_n;
static js8_station_t  st_rows[MAX_ROWS];
static bool           st_all_time;          /* Stations view and map: the history's stations of this band (Heard: All time) */
static js8_station_t  ever_rows[MAX_ROWS];  /* those, from the station history (ever_load), newest first */
static int            ever_count;
static int            st_count;
static lv_obj_t      *query_list;      /* Query popup, when open */
static lv_obj_t      *aprs_list;       /* APRS popup, when open */
static lv_obj_t      *log_list;        /* Log QSO popup, when open */
static lv_obj_t      *inbox_list;      /* Inbox popup (list or one message), when open */
static lv_obj_t      *alerts_list;     /* Alerts popup, when open */
static lv_obj_t      *freq_list;       /* Freq popup, when open */
static lv_obj_t      *spot_list;       /* POTA / SOTA spot form, when open */
static char           alert_words[128]; /* "VE7ABC @POTA SOTA", ALERTS= in JS8_TEXTS_PATH */
static char           groups_text[160]; /* "@NET @GROUP2": groups we're in (10 at most), GROUPS= in JS8_TEXTS_PATH */
static char           operator_call[JS8_RX_CALL_LEN]; /* logged as OPERATOR ("": the station call), OPERATOR= */
static bool           st_alert[MAX_ROWS]; /* st_rows matching an alert word */
static js8_inbox_t   *inbox;           /* MSGs to us, JS8_INBOX_PATH */
static js8_history_t *history_db;      /* the station history, JS8_HISTORY_PATH (NULL: not recorded) */
static js8_held_t    *held;            /* MSG TO: messages held for others, JS8_HELD_PATH */
static struct {
    int  id;                           /* offered on Reply: queued as offered, it's on its way */
    char group_call[JS8_RX_CALL_LEN];  /* a group message: who it's for */
    char text[JS8_RX_TEXT_LEN];
} deliver_pending;
/* Held messages on their way: each is delivered once it has gone out in
 * full. Desktop marks it as soon as it starts sending; stopped halfway
 * here, it stays held and is offered again when they next ask. */
#define DELIVERIES 4
static struct {
    int  id; /* 0: a free entry */
    char group_call[JS8_RX_CALL_LEN];
    char text[JS8_RX_TEXT_LEN];
} deliveries[DELIVERIES];
/* How a message ended, from the transmitter's thread to ours. */
typedef struct {
    bool completed;
    char text[JS8_RX_TEXT_LEN]; /* as sent: upper case, trimmed */
} tx_done_t;
static bool           st_worked[MAX_ROWS]; /* in the log already (st_rows) */
static js8_qsos_t    *qsos;            /* QSOs, for the log */
static js8_log_entry_t log_entry;      /* the entry the Log popup shows */
static char           log_pending[JS8_RX_CALL_LEN]; /* a QSO that ended, not logged yet */
static bool           log_grid_typed;  /* the grid was typed: don't replace it */
static lv_obj_t      *log_reports;     /* the popup's Sent / Rcvd line */
static lv_obj_t      *log_grid_btn;    /* the popup's Grid item */

/* T4: auto-reply and heartbeats. The switches live in params (js8_auto,
 * js8_hb, js8_hb_ack, js8_hb_interval), all off by default. */
static js8_auto_t *autop;
static int64_t     hb_next_ms;       /* 0: send the first one at the next chance */
static int64_t     hb_paused_until;  /* HB and HB ACK wait until then; 0: not paused */
static bool        auto_cq;          /* hold CQ: a CQ every js8_cq_interval min until answered */
static int64_t     auto_cq_next_ms;
static int64_t     auto_cq_from_ms;  /* the interval counts from here: our last TX's end */
static bool        hb_adjusting;     /* main knob sets the HB interval */
static int64_t     hb_adjust_ms;     /* last knob turn while adjusting */
static bool        cq_adjusting;     /* main knob sets the auto CQ interval */
static int64_t     cq_adjust_ms;
static char        info_text[TEXT_MAX + 1], status_text[TEXT_MAX + 1];
/* Saved messages as typed, macros and all (desktop JS8Call's Saved
 * Messages), JS8_SAVED_PATH. */
static char        saved_msgs[SAVED_N][TX_TEXT_MAX + 1];
static bool        saved_writable = true; /* the file couldn't be read or moved: never write over it */
static int         saved_edit;            /* the one in the keyboard (EDIT_SAVED) */
static char        last_pota[16], last_sota[24]; /* last park / summit spotted via APRS */
/* The spot form (APRS > POTA/SOTA spot), remembered in JS8_TEXTS_PATH. */
static char        spot_mode[8] = "DATA";
static int32_t     spot_typed_hz;  /* last frequency typed in the form, 0 = none */
static bool        spot_use_typed; /* spot that one, not the JS8 dial */
static char        spot_note[32];
static char        last_tx_text[JS8_RX_TEXT_LEN]; /* for AGN?: set when its first frame keys */
/* Our message on the air, from queueing it until ui_tx_done hears it
 * ended (the transmitter says "not busy" a moment before that). */
static bool        tx_active;
static bool        tx_auto;        /* it's automatic: not our side of a QSO */
static bool        tx_cq;          /* it's a CQ: auto CQ counts from its end */
static int64_t     tx_quiet_ms;    /* tx_active but the transmitter idle since then */
#define TX_DONE_LOST_MS  5000      /* then the "done" was lost: not active */
/* AUTO off: answers waiting for Reply, one per station (desktop has one
 * outgoing box). */
#define OFFERS 8
static struct {
    char    call[JS8_RX_CALL_LEN]; /* "": a free entry */
    char    text[JS8_RX_TEXT_LEN];
    int64_t ms;
    int     deliver_id; /* the held message it delivers, or 0 */
    char    deliver_group_call[JS8_RX_CALL_LEN];
} offers[OFFERS];
/* Automatic replies waiting for their turn (the end of their decode
 * cycle, our TX, the keyboard, a message to us still arriving): oldest
 * first, the same text once. */
#define REPLIES          8
#define REPLY_WAIT_MS    (2 * 60 * 1000) /* then it's too late to answer */
#define CYCLE_WAIT_MS    5000            /* no end of cycle heard by then: go on */
static struct {
    js8_auto_result_t r;
    int64_t           ms;
    unsigned          cycle;   /* `cycles` when it was decided */
    bool              checked; /* its cycle ended and it wasn't dropped */
} replies[REPLIES];
static int     n_replies;
static int64_t push_next_ms; /* next look for RETRIEVE MSG notices, 0: 15 min from now */
/* Multi-frame messages still arriving, as desktop's open message buffers:
 * while one to us is open nothing automatic answers anyone, and while any
 * is open no HB ACK goes out, so we don't key over its next frames. */
#define OPEN_MSGS        8
#define OPEN_MSG_MS      60000 /* no frame for this long: it ended */
static struct {
    uint32_t id; /* 0: a free entry */
    bool     to_me;
    int64_t  ms; /* its last frame */
} open_msgs[OPEN_MSGS];
/* Band activity for heartbeat offsets (desktop's isFreqOffsetFree): every
 * decode's offset and time; the last 30 s counts. */
#define ACTIVITY         128
static struct {
    float   hz;
    int64_t ms;
} activity[ACTIVITY];
static int activity_head;
static int               edit_target;        /* 0 compose, else an edit_t */
static lv_obj_t         *texts_list;
static lv_obj_t         *hpage_list; /* a station's History page */
static lv_obj_t         *hclear_btn; /* Settings' Clear station history line, while open */

/* Every list popup: whether it's open and how it closes. any_popup(),
 * close_popups() and destruct_cb() all go through this table, so a popup
 * can't be missing from one of them (GEN with the Query list open once
 * crashed the app that way). */
static const struct {
    lv_obj_t **list;
    void (*close)(void);
} popups[] = {
    {&query_list, query_close}, {&texts_list, texts_close},   {&aprs_list, aprs_close}, {&log_list, log_close},
    {&inbox_list, inbox_close}, {&alerts_list, alerts_close}, {&freq_list, freq_close}, {&spot_list, spot_close},
    {&hpage_list, hpage_close},
};
#define POPUPS (sizeof(popups) / sizeof(popups[0]))

static lv_obj_t *finder;
static lv_obj_t *table;
static lv_obj_t *status;
static lv_obj_t *st_qrz_label; /* the Stations view's QRZ line, under the status line (the map's own is map_qrz_label) */

static int32_t filter_low, filter_high;
static int32_t saved_filter_low, saved_filter_high; /* the user's, restored on close */
static bool    filter_saved;
static show_t  show = SHOW_NO_HB;

/* What the compose window is editing (edit_target). */
typedef enum {
    EDIT_INFO = 1,
    EDIT_STATUS,
    EDIT_GROUPS,   /* Settings: the groups we're in */
    EDIT_OPERATOR, /* Settings: the operator, if not the station call */
    EDIT_SAVED,    /* a saved message (saved_edit), then back to their list */
    EDIT_LOG_GRID, /* the Log popup's fields, then back to it */
    EDIT_LOG_NAME,
    EDIT_LOG_NOTE,
    EDIT_POTA_REF, /* your park / summit for the log */
    EDIT_SOTA_REF,
    EDIT_ALERT_WORDS, /* then back to the Alerts popup */
    EDIT_FREQ,        /* custom dial frequency, kHz */
    EDIT_SPOT_REF,    /* the spot form's fields, then back to it */
    EDIT_SPOT_FREQ,
    EDIT_SPOT_NOTE,
    EDIT_BEACON_GRID, /* optional message, then Spot my grid */
    EDIT_BEACON_GPS,  /* optional message, then Spot GPS position */
    EDIT_COUNT,
} edit_t;

/* The Log popup's items. */
typedef enum { LOG_SAVE, LOG_GRID, LOG_NAME, LOG_NOTE, LOG_CANCEL } log_item_t;
static void log_list_open(log_item_t focus);

static void log_edit_done(const char *value);
static void alerts_show(void);

/* Message history, a ring, so the list can be rebuilt when the filter
 * changes. row_hist[] maps a table row to its history slot (-1 = info row). */
LV_FONT_DECLARE(js8_marks_24);
static lv_font_t    table_font; /* the list's font: sony_24 with js8_marks_24 as fallback */
static js8_rx_msg_t history[HISTORY];
static int64_t      hist_ms[HISTORY]; /* when each arrived (JS8 time), for Messages kept */
static uint32_t     hist_seq[HISTORY]; /* arrival order, shared with info rows */
/* Your own messages' rows (VE7NHW): " ..." while it goes out, desktop's
 * end mark once the last frame has, "(stopped 2/5)" if it was stopped. */
enum { TX_ROW_NONE, TX_ROW_SENDING, TX_ROW_SENT, TX_ROW_STOPPED };
static struct {
    uint8_t  state;
    uint16_t frame, frames; /* the frame on the air or next, as the TX bar counts */
} hist_tx[HISTORY];
static int          tx_row_slot = -1; /* the row of the message going out */
static uint32_t     tx_row_seq;
static uint16_t     hist_head;  /* next slot to write */
static uint16_t     hist_count;
/* Info rows ("Auto: N0XYZ SNR -05", "Held message 3 delivered"), kept
 * apart from the messages but in arrival order with them (seq), so a
 * rebuild (the Show filter, leaving Stations, a full list, Messages kept)
 * shows them again instead of dropping them (bug hunt 18). */
#define INFO_ROWS 64
static struct {
    char     text[128];
    int64_t  ms;
    uint32_t seq;
} info_rows[INFO_ROWS];
static unsigned info_head, info_count;
static uint32_t row_seq; /* the last message or info row's */
static int16_t      row_hist[MAX_ROWS + 1];
static uint16_t     rows;

static unsigned cycles, cycle_decodes;

/* Waterfall PSD, touched only on the receiver's worker thread. */
static spgramf  sg;
static float   *psd;
static uint16_t nfft;
static unsigned wf_row_fill;   /* samples in the row being built (receiver thread) */

/* Rows are made per fixed amount of audio, but audio arrives in bursts; a
 * timer draws them at an even pace so the waterfall scrolls smoothly. The
 * pace comes from the system clock: an LVGL timer's period counts from when
 * it last ran (and LVGL's tick runs slow), so a 100 ms timer drew ~9.7 rows
 * a second, fell behind and caught up with a two-row jump every 2 s. */
static pthread_mutex_t wf_lock = PTHREAD_MUTEX_INITIALIZER;
static float      wf_rows[WF_QUEUE][WIDTH]; /* rows made, not drawn yet (wf_lock) */
static unsigned   wf_q_head, wf_q_count;    /* (wf_lock) */
static float      wf_row[WIDTH], wf_sel[WIDTH]; /* the row being made (receiver thread) */
static lv_timer_t *wf_timer;
static int64_t    wf_due_us;   /* when the next row should be drawn (monotonic) */
static float    wf_floor_db;   /* smoothed noise floor (receiver thread) */
static bool     wf_floor_set;
static unsigned wf_dropped;    /* rows the GUI fell too far behind to draw (wf_lock) */
/* Waterfall: Sharp / Light / Medium / Calm (Settings): each row averaged
 * with the ones before (exponentially, wf_avg_new[level] of the new row), as
 * desktop waterfalls' averaging. Every new row moves the whole picture down
 * a pixel, and the screen dims for a moment while that many pixels change
 * (they darken faster than they brighten): a flicker at the row rate. A
 * calmer picture changes less at each step. */
#define WF_AVG_LEVELS 4
static const float       wf_avg_new[WF_AVG_LEVELS]  = {1.0f, 0.65f, 0.5f, 0.35f};
static const char *const wf_avg_name[WF_AVG_LEVELS] = {"Sharp", "Light", "Medium", "Calm"};
static atomic_int        wf_avg_level;  /* GUI thread sets, receiver thread reads */
static atomic_bool wf_avg_restart;  /* retune / clear: start the average over */
static float       wf_avg[WIDTH];   /* receiver thread */
static bool        wf_avg_set;
/* What makes the main screen's spectrum and waterfall redraw their part of
 * the lower plane, over ours (js8_wf_repaint_soon). */
static Observer *wf_watch[4];
/* NR, NB and the notches: off while JS8 is open (dsp_held_cb). */
static Observer *dsp_watch[4];

/* Health in the app log, once a minute and only if it happened: the GUI
 * thread stalling (the waterfall then lags), rows dropped. */
#define GUI_STALL_US  200000
#define HEALTH_US     60000000
static int64_t  gui_last_us, health_start_us, gui_stall_max_us;
static unsigned gui_stalls;

/* ---- Buttons ---------------------------------------------------------- */

static buttons_page_t page_1;
static buttons_page_t page_2;
static buttons_page_t page_3;
static buttons_page_t page_4;
static buttons_page_t page_5;
static buttons_page_t page_6;

static button_data_t btn_p1      = {.type = BTN_TEXT, .label = "(JS8 1:6)", .press = js8_next_page_cb, .hold = js8_prev_page_cb, .next = &page_2, .prev = &page_6};
static button_data_t btn_show    = {.type = BTN_TEXT_FN, .label_fn = show_label_getter, .press = show_cb};
static button_data_t btn_reply   = {.type = BTN_TEXT, .label = "Reply", .press = reply_cb};
static button_data_t btn_send    = {.type = BTN_TEXT, .label = "Send...", .press = send_cb};
static button_data_t btn_hw_cpy  = {.type = BTN_TEXT, .label = "HW CPY?", .press = hw_cpy_cb};

static button_data_t btn_p2    = {.type = BTN_TEXT, .label = "(JS8 2:6)", .press = js8_next_page_cb, .hold = js8_prev_page_cb, .next = &page_3, .prev = &page_1};
static button_data_t btn_cq    = {.type = BTN_TEXT_FN, .label_fn = cq_label_getter, .press = cq_cb, .hold = cq_hold_cb};
static button_data_t btn_hb    = {.type = BTN_TEXT_FN, .label_fn = hb_label_getter, .press = heartbeat_cb, .hold = heartbeat_hold_cb};
static button_data_t btn_query = {.type = BTN_TEXT, .label = "Query >", .press = query_cb};
static button_data_t btn_clear = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};

static button_data_t btn_p3        = {.type = BTN_TEXT, .label = "(JS8 3:6)", .press = js8_next_page_cb, .hold = js8_prev_page_cb, .next = &page_4, .prev = &page_2};
/* Sort in the Stations view, the map's view on the map. */
static button_data_t btn_map_view = {.type = BTN_TEXT_FN, .label_fn = map_view_label_getter, .press = map_view_cb, .hold = map_follow_cb};
static button_data_t btn_stations  = {.type = BTN_TEXT_FN, .label_fn = stations_label_getter, .press = stations_cb};
static button_data_t btn_inbox     = {.type = BTN_TEXT_FN, .label_fn = inbox_label_getter, .press = inbox_cb};

static buttons_page_t page_1 = {{&btn_p1, &btn_cq, &btn_hb, &btn_query, &btn_hw_cpy}};
static buttons_page_t page_2 = {{&btn_p2, &btn_show, &btn_reply, &btn_send, &btn_clear}};
/* Heard: Recent / All time in the Stations view and on the map; over the
 * messages, Show History (the Stations view in All time). Hold took page
 * 6's Decode slot to make room for it. */
static button_data_t btn_ever = {.type = BTN_TEXT_FN, .label_fn = ever_label_getter, .press = ever_cb};
static buttons_page_t page_3 = {{&btn_p3, &btn_map_view, &btn_ever, &btn_stations, &btn_inbox}};

static button_data_t btn_p4     = {.type = BTN_TEXT, .label = "(JS8 4:6)", .press = js8_next_page_cb, .hold = js8_prev_page_cb, .next = &page_5, .prev = &page_3};
static button_data_t btn_auto   = {.type = BTN_TEXT_FN, .label_fn = auto_label_getter, .press = auto_cb};
static button_data_t btn_hbackk = {.type = BTN_TEXT_FN, .label_fn = hb_ack_label_getter, .press = hb_ack_cb};
static button_data_t btn_texts  = {.type = BTN_TEXT, .label = "Settings...", .press = texts_cb};
/* Time: press = Auto on/off, hold = search (where "HB: 10 min" was, before
 * heartbeats moved to page 1's Heartbeat). */
static button_data_t btn_time   = {.type = BTN_TEXT_FN, .label_fn = time_label_getter, .press = time_cb, .hold = time_hold_cb};
static buttons_page_t page_4 = {{&btn_p4, &btn_auto, &btn_time, &btn_hbackk, &btn_texts}};

static button_data_t  btn_p5        = {.type = BTN_TEXT, .label = "(JS8 5:6)", .press = js8_next_page_cb, .hold = js8_prev_page_cb, .next = &page_6, .prev = &page_4};
static button_data_t  btn_aprs      = {.type = BTN_TEXT, .label = "APRS >", .press = aprs_cb};
static button_data_t  btn_log       = {.type = BTN_TEXT, .label = "Log QSO", .press = log_cb};
static button_data_t  btn_act       = {.type = BTN_TEXT_FN, .label_fn = act_label_getter, .press = act_cb, .hold = act_hold_cb};
static button_data_t  btn_prompt    = {.type = BTN_TEXT_FN, .label_fn = prompt_label_getter, .press = prompt_cb};
static buttons_page_t page_5        = {{&btn_p5, &btn_aprs, &btn_log, &btn_act, &btn_prompt}};

static button_data_t  btn_p6        = {.type = BTN_TEXT, .label = "(JS8 6:6)", .press = js8_next_page_cb, .hold = js8_prev_page_cb, .next = &page_1, .prev = &page_5};
static button_data_t  btn_alerts    = {.type = BTN_TEXT, .label = "Alerts >", .press = alerts_cb};
static button_data_t  btn_speed     = {.type = BTN_TEXT_FN, .label_fn = speed_label_getter, .press = speed_cb, .hold = speed_hold_cb};
static button_data_t  btn_hold      = {.type = BTN_TEXT_FN, .label_fn = hold_label_getter, .press = hold_cb};
/* Slot 4 is free: Freq is a Settings line now (Frequencies: ...). */
static buttons_page_t page_6        = {{&btn_p6, &btn_alerts, &btn_speed, &btn_hold, NULL}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .key_cb       = key_cb,
    .rotary_cb    = rotary_cb,
    .btn_page     = &page_1,
};

dialog_t *dialog_js8 = &dialog;

/* ---- Message list ----------------------------------------------------- */

static bool passes_filter(const js8_rx_msg_t *m) {
    if (m->tx) return true;
    switch (show) {
    case SHOW_ALL:      return true;
    /* SNR reports go with heartbeats: mostly answers to them. */
    case SHOW_NO_HB:    return !(m->heartbeat || m->snr_report) || m->to_me; /* e.g. HB acks to us */
    case SHOW_DIRECTED:
        /* Also everything on the selected station's frequency: in a long
         * QSO they often stop putting your call in. */
        /* 'On their frequency': within the decode's speed's rxThreshold, as desktop. */
        if (qso_freq >= 0 &&
            fabsf(m->freq_hz - qso_freq) <= js8_speed_rx_threshold_hz(js8_speed_from_submode(m->submode)))
            return true;
        return m->to_me || (m->to_group && !m->heartbeat && !m->cq);
    default:            return true;
    }
}

/* " ...", the end mark, "  (stopped 2/5)" or nothing, for a row of yours;
 * the map's strip has no end mark (its font hasn't the glyph). */
static void tx_row_mark(int slot, bool eot, char *buf, size_t size) {
    buf[0] = '\0';
    switch (hist_tx[slot].state) {
    case TX_ROW_SENDING: snprintf(buf, size, " ..."); break;
    case TX_ROW_SENT:    if (eot) snprintf(buf, size, " " EOT_MARK); break;
    case TX_ROW_STOPPED: snprintf(buf, size, "  (stopped %u/%u)", hist_tx[slot].frame, hist_tx[slot].frames); break;
    }
}

static void format_row(int slot, char *buf, size_t size) {
    const js8_rx_msg_t *m = &history[slot];
    int hh = m->utc / 10000, mm = (m->utc / 100) % 100, ss = m->utc % 100;

    /* Speed letter outside Normal: " F", " T", " S" (desktop's speed column). */
    js8_speed_t sp       = js8_speed_from_submode(m->submode);
    char        speed[4] = "";
    if (sp != JS8_SPEED_NORMAL) snprintf(speed, sizeof(speed), " %c", js8_speed_letter(sp));

    if (m->tx) {
        char mark[32];
        tx_row_mark(slot, true, mark, sizeof(mark));
        snprintf(buf, size, "%02d:%02d:%02d  TX %4.0f%s  %s%s", hh, mm, ss, m->freq_hz, speed, m->text, mark);
        return;
    }

    snprintf(buf, size, "%02d:%02d:%02d %+3d %4.0f%s  %s%s%s%s%s",
             hh, mm, ss, m->snr, m->freq_hz, speed,
             m->low_confidence ? "[" : "",
             m->text,
             m->low_confidence ? "]" : "",
             m->partial ? " ..." : m->checksum < 0 ? "  (bad checksum)" : "",
             /* Desktop's end-of-transmission mark: the message's last frame arrived. */
             !m->partial && (m->type & JS8_FRAME_LAST) ? " " EOT_MARK : "");
}

static bool auto_selecting; /* follow() is moving the selection, not the user */

static int64_t now_wall_ms(void);
static bool    hb_paused(void);
static void    show_selection(void);
static void    table_invalidate_row(uint16_t row);

/* Keep following new rows if the cursor is on the last one, or once the
 * knob has been left alone for a while after scrolling up to read. The
 * cursor only scrolls: the selected station (sel_call) stays put. */
static bool at_bottom(void) {
    uint16_t sel_row = 0, sel_col = 0;
    lv_table_get_selected_cell(table, &sel_row, &sel_col);
    return rows == 0 || sel_row == LV_TABLE_CELL_NONE || sel_row + 1 >= rows ||
           now_wall_ms() - cursor_user_ms > READ_PAUSE_MS;
}

/* Select the last row and scroll it into view. lv_table 8.3 has no setter
 * for the selection, so put it one row above and let the table's own key
 * handler step down: that also runs its scroll-to-selected logic. */
static void select_row(uint16_t r) {
    if (rows == 0) return;
    if (r >= rows) r = rows - 1;
    lv_table_t *t  = (lv_table_t *)table;
    auto_selecting = true;
    if (r == 0) {
        t->row_act = 0;
        t->col_act = 0;
        lv_obj_scroll_to_y(table, 0, LV_ANIM_OFF);
        lv_obj_invalidate(table);
    } else {
        static uint32_t key = LV_KEY_DOWN;
        uint16_t        was = t->row_act;
        t->row_act          = r - 1;
        t->col_act          = 0;
        lv_event_send(table, LV_EVENT_KEY, &key);
        if (was != t->row_act) table_invalidate_row(was); /* the step redraws r-1 and r only */
    }
    auto_selecting = false;
}

/* Scroll to the very end: a long last row can be taller than its step.
 * After the list got shorter (Show, a rebuild) the old scroll can sit past
 * the new end, which showed blank rows until the MFK moved: back up to the
 * end then, or to the top if it all fits. */
static void list_scroll_end(void) {
    lv_obj_update_layout(table);
    lv_coord_t below = lv_obj_get_scroll_bottom(table);
    if (below > 0) {
        lv_obj_scroll_by(table, 0, -below, LV_ANIM_OFF);
    } else if (below < 0) {
        lv_coord_t y = lv_obj_get_scroll_y(table) + below;
        lv_obj_scroll_to_y(table, y > 0 ? y : 0, LV_ANIM_OFF);
    }
}

static void follow(void) {
    if (rows == 0) return;
    select_row(rows - 1);
    list_scroll_end();
}

static void append_row(const char *text, int16_t hist) {
    lv_table_set_cell_value(table, rows, 0, text);
    row_hist[rows] = hist;
    rows++;
}

static void rebuild_rows(void);

static void add_info_row(const char *fmt, ...) {
    char   *buf = info_rows[info_head].text;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(info_rows[0].text), fmt, args);
    va_end(args);
    info_rows[info_head].ms  = now_wall_ms();
    info_rows[info_head].seq = ++row_seq;
    info_head                = (info_head + 1) % INFO_ROWS;
    if (info_count < INFO_ROWS) info_count++;

    if (view_stations || !table) return;
    if (rows >= MAX_ROWS) { /* full: trimmed, this one included (it was dropped) */
        rebuild_rows();
        return;
    }
    bool scroll = at_bottom();
    append_row(buf, -1);
    if (scroll) follow();
}

/* Rebuild the list from history, newest KEEP_ROWS matching messages. */
static void rebuild_station_rows(void);

/* Settings: how long stations and messages stay listed (desktop's callsign
 * and activity aging), 0 minutes = always. */
static const struct {
    const char *label;
    int         min;
} st_keep_opts[] = {{"15 min", 15}, {"30 min", 30}, {"1 hour", 60}, {"2 hours", 120}, {"6 hours", 360}, {"always", 0}},
  msg_keep_opts[] = {{"all", 0}, {"15 min", 15}, {"30 min", 30}, {"1 hour", 60}, {"2 hours", 120}};
#define ST_KEEP_N  (int)(sizeof(st_keep_opts) / sizeof(st_keep_opts[0]))
#define MSG_KEEP_N (int)(sizeof(msg_keep_opts) / sizeof(msg_keep_opts[0]))

static int64_t msg_keep_ms(void) {
    return (int64_t)msg_keep_opts[param_i_get(cfg.js8.msg_keep()) < MSG_KEEP_N ? param_i_get(cfg.js8.msg_keep()) : 0].min * 60000;
}

static void apply_station_keep(void) {
    js8_stations_set_expire_ms((int64_t)st_keep_opts[param_i_get(cfg.js8.st_keep()) < ST_KEEP_N ? param_i_get(cfg.js8.st_keep()) : 2].min *
                               60000);
}

static void rebuild_rows(void) {
    if (view_stations) {
        rebuild_station_rows();
        return;
    }
    int16_t idx[KEEP_ROWS]; /* newest first */
    int     n = 0;

    int64_t keep = msg_keep_ms(), now = now_wall_ms();
    for (int age = 0; age < hist_count && n < KEEP_ROWS; age++) {
        int slot = (hist_head - 1 - age + HISTORY) % HISTORY;
        if (keep && now - hist_ms[slot] > keep) break; /* older ones too */
        if (passes_filter(&history[slot])) idx[n++] = (int16_t)slot;
    }
    /* Info rows in between, by arrival, newest first; they age out with
     * Messages kept as well. */
    int info[INFO_ROWS], ni = 0;
    for (unsigned a = 0; a < info_count; a++) {
        unsigned i = (info_head - 1 - a + INFO_ROWS) % INFO_ROWS;
        if (keep && now - info_rows[i].ms > keep) break;
        info[ni++] = (int)i;
    }
    /* The newest KEEP_ROWS of both: pick[k] >= 0 a history slot, else
     * -1 - an info row. */
    int pick[KEEP_ROWS], np = 0, a = 0, b = 0;
    while (np < KEEP_ROWS && (a < n || b < ni)) {
        if (b < ni && (a >= n || info_rows[info[b]].seq > hist_seq[idx[a]])) pick[np++] = -1 - info[b++];
        else pick[np++] = idx[a++];
    }

    lv_table_set_row_cnt(table, 1);
    lv_table_set_cell_value(table, 0, 0, "");
    rows = 0;

    char buf[JS8_RX_TEXT_LEN + 48];
    if (n == 0) append_row("Listening for JS8...", -1); /* no messages (yet, or through the filter) */
    for (int k = np - 1; k >= 0; k--) {
        if (pick[k] < 0) {
            append_row(info_rows[-1 - pick[k]].text, -1);
            continue;
        }
        format_row(pick[k], buf, sizeof(buf));
        append_row(buf, (int16_t)pick[k]);
    }
    follow();
}

/* The Speed button's order, fastest-but-one up to Slow: Ultra after Turbo
 * (the user's choice). js8_speed_t (and the saved js8_speed) keeps its
 * numbers; only the stepping order is this. */
static const js8_speed_t speed_order[JS8_SPEED_COUNT] = {
    JS8_SPEED_NORMAL, JS8_SPEED_FAST, JS8_SPEED_TURBO, JS8_SPEED_ULTRA, JS8_SPEED_SLOW,
};

/* Ultra is always decoded, as desktop does (measured on the radio
 * 2026-10-10: about a tenth of a core); its Setting only puts it on the
 * Speed button, so nobody lands on it by accident. Hold Speed on an Ultra
 * station switches to it either way. */
static bool speed_on_button(js8_speed_t s) {
    return s != JS8_SPEED_ULTRA || param_i_get(cfg.js8.ultra());
}

/* The speed we transmit at (page 6). */
static js8_speed_t cur_speed(void) {
    int s = param_i_get(cfg.js8.speed());
    return s < JS8_SPEED_COUNT ? (js8_speed_t)s : JS8_SPEED_NORMAL;
}

/* What the receiver decodes: every speed (desktop's multi-decoder, the
 * default) or only the one we transmit at. */
static int rx_speed_mask(void) {
    if (!param_i_get(cfg.js8.rx_all())) return js8_speed_rx_mask(cur_speed());
    int mask = 0;
    for (int s = 0; s < JS8_SPEED_COUNT; s++) mask |= js8_speed_rx_mask((js8_speed_t)s);
    return mask;
}

/* The one-time decoder setup (js8_wisdom.h): no speed is decoded until the
 * FFT plans are measured; the notice banner shows meanwhile (rx_start()). */
static bool        decoder_setup;
static lv_obj_t   *setup_box;   /* the banner */
static lv_obj_t   *setup_title; /* its text */
static lv_timer_t *setup_timer;

static int rx_decode_mask(void) { return decoder_setup ? 0 : rx_speed_mask(); }

/* JS8's time: the radio's clock plus the Time Sync drift (js8_rx.h). */
static int64_t now_wall_ms(void) {
    return js8_wall_ms();
}

static void handle_incoming(const js8_rx_msg_t *m);

static bool find_station(const char *call, js8_station_t *out);

/* The history slot of a message still arriving (desktop grows the line
 * each decode cycle), or -1. Only recent slots can hold one. */
static int find_partial(uint32_t msg_id) {
    if (!msg_id) return -1;
    for (int age = 0; age < hist_count && age < 100; age++) {
        int slot = (hist_head - 1 - age + HISTORY) % HISTORY;
        if (!history[slot].tx && history[slot].partial && history[slot].msg_id == msg_id) return slot;
    }
    return -1;
}

/* Messages still arriving when the receiver stops listening (a retune, JS8
 * closing) never will: their rows stop showing " ...". A new message can't
 * take their rows over either: ids are unique for the whole run now
 * (B-12). True if there were any. */
static bool partials_end(void) {
    bool any = false;
    for (int age = 0; age < hist_count; age++) {
        int slot = (hist_head - 1 - age + HISTORY) % HISTORY;
        if (history[slot].partial && !history[slot].tx) {
            history[slot].partial = false;
            any                   = true;
        }
    }
    return any;
}

/* Everything that acts on a message, once it's complete: never on the text
 * so far of one still arriving. */
static void process_message(js8_rx_msg_t *m) {
    /* New: not heard on this frequency since the radio was switched on
     * (your choice), not just "not listed now": a regular who dropped off
     * the list an hour after they were last heard beeped again (B-20). */
    bool new_station = !m->tx && m->from[0] && !js8_stations_heard_before(stations, m->from);
    if (new_station) map_popup_add(m->from); /* pops up on the map for a few seconds */
    js8_stations_add(stations, m, my_call(), now_wall_ms());
    if (m->tx) return;
    js8_history_rx(history_db, m, my_call(), (uint64_t)cparam_i_get(cfg.cur.fg_freq()), stations, now_wall_ms());
    if (st_all_time) {
        ever_merge(m->from);
        if (view_stations) rebuild_station_rows();
    }
    if (param_i_get(cfg.js8.relay())) { /* stations a relay to us came through, as desktop lists them */
        char via_calls[4][JS8_RX_CALL_LEN], via[JS8_RX_CALL_LEN];
        int  n = js8_relay_stations(m, my_call(), via_calls, 4, via, sizeof(via));
        for (int i = 0; i < n; i++)
            js8_stations_add_via(stations, via_calls[i], via, m->freq_hz, m->submode, now_wall_ms());
    }
    if (sel_call[0] && strcasecmp(m->from, sel_call) == 0) { /* they moved: the green line follows */
        qso_freq = m->freq_hz;
        sel_snr  = m->snr;
        show_selection();
    }
    alert_check(m, new_station);
    handle_incoming(m);
    char ended[JS8_RX_CALL_LEN];
    if (js8_qsos_received(qsos, m, my_call(), now_wall_ms(), ended, sizeof(ended))) log_offer(ended);
    if (m->to_me && log_list) log_refresh();
}

/* A message that was showing as it arrived: new text in its slot, and in
 * its row if it has one (else a row now, if it passes the filter). */
static void update_slot(int slot, const js8_rx_msg_t *m) {
    history[slot] = *m;
    if (view_stations) {
        if (!m->partial) rebuild_station_rows();
        return;
    }
    char buf[JS8_RX_TEXT_LEN + 48];
    format_row(slot, buf, sizeof(buf));
    for (uint16_t r = 0; r < rows; r++) {
        if (row_hist[r] == slot) {
            /* A message growing as it arrives wraps onto new lines: keep
             * its newest words in view. */
            bool scroll = at_bottom();
            lv_table_set_cell_value(table, r, 0, buf);
            if (scroll) follow();
            return;
        }
    }
    if (!passes_filter(m) || rows >= MAX_ROWS) return;
    bool scroll = at_bottom();
    append_row(buf, (int16_t)slot);
    if (scroll) follow();
}

/* After every frame heard (a message, or the text so far of one still
 * arriving): the band activity, and which messages are still open. Before
 * the message is acted on, so a message to us that has just ended no
 * longer holds up its own answer. */
static void track_incoming(const js8_rx_msg_t *m) {
    int64_t now = now_wall_ms();
    activity[activity_head].hz = m->freq_hz;
    activity[activity_head].ms = now;
    activity_head              = (activity_head + 1) % ACTIVITY;

    if (!m->msg_id) return;
    int slot = -1, spare = 0; /* spare: a free entry, else the oldest */
    for (int i = 0; i < OPEN_MSGS; i++) {
        if (open_msgs[i].id == m->msg_id) slot = i;
        if (!open_msgs[spare].id) continue;
        if (!open_msgs[i].id || open_msgs[i].ms < open_msgs[spare].ms) spare = i;
    }
    if (!m->partial) { /* it ended */
        if (slot >= 0) open_msgs[slot].id = 0;
        return;
    }
    if (!m->to[0]) return; /* desktop opens a buffer only for a directed command */
    if (slot < 0) slot = spare;
    open_msgs[slot].id    = m->msg_id;
    open_msgs[slot].to_me = m->to_me;
    open_msgs[slot].ms    = now;
}

/* Is a message still arriving (to us only, or any)? */
static bool message_open(bool to_me) {
    int64_t now = now_wall_ms();
    for (int i = 0; i < OPEN_MSGS; i++) {
        if (!open_msgs[i].id) continue;
        if (now - open_msgs[i].ms > OPEN_MSG_MS) open_msgs[i].id = 0;
        else if (open_msgs[i].to_me || !to_me) return true;
    }
    return false;
}

static void add_message(const js8_rx_msg_t *msg) {
    js8_rx_msg_t  copy = *msg;
    js8_rx_msg_t *m    = &copy;

    if (!m->tx) track_incoming(m);
    if (!m->tx && m->to_me && m->from[0]) { /* their path turns red on the map; a real message also rings */
        map_qso_add(m->from, !m->heartbeat && !m->snr_report);
        map_pulse_add(m->from); /* and a dot runs from them to you, each frame */
    }
    map_talk_note(m); /* other stations' QSOs: grey lines */
    if (!m->tx && !m->partial && m->cq && m->from[0]) cq_heard_add(m->from); /* green in Stations, "CQ" on the map */
    if (!m->tx && !m->partial && m->to_me && m->from[0] && !m->heartbeat && !m->snr_report && view_stations)
        map_qrz_add(m->from); /* QRZ on the map (the message list shows it anyway) */
    int existing = m->tx ? -1 : find_partial(m->msg_id);
    if (!m->partial) process_message(m);
    if (existing >= 0) {
        update_slot(existing, m);
        return;
    }

    int slot = hist_head;
    history[slot]  = *m;
    hist_tx[slot].state  = m->tx ? TX_ROW_SENDING : TX_ROW_NONE; /* ours: added as its first frame keys */
    hist_tx[slot].frame  = hist_tx[slot].frames = 0;
    hist_ms[slot]  = now_wall_ms();
    hist_seq[slot] = ++row_seq;
    hist_head      = (hist_head + 1) % HISTORY;
    if (hist_count < HISTORY) hist_count++;

    /* Rows pointing at the slot we just overwrote would now show the wrong
     * message; the ring is larger than MAX_ROWS so this only happens after a
     * long session, and rebuilding fixes it. */
    if (view_stations) {
        if (!m->partial) rebuild_station_rows();
        return;
    }
    for (uint16_t r = 0; r < rows; r++) {
        if (row_hist[r] == slot) {
            rebuild_rows();
            return;
        }
    }

    if (!passes_filter(m)) return;

    if (rows >= MAX_ROWS) {
        rebuild_rows();
        return;
    }

    bool scroll = at_bottom();
    char buf[JS8_RX_TEXT_LEN + 48];
    format_row(slot, buf, sizeof(buf));
    append_row(buf, (int16_t)slot);
    if (scroll) follow();
}

/* "4m" style age; "now" under a minute. */
static void format_age(int64_t ms, char *buf, size_t size) {
    int64_t min = ms / 60000;
    if (min <= 0) snprintf(buf, size, "now");
    else if (min < 60) snprintf(buf, size, "%dm", (int)min);
    else if (min < 48 * 60) snprintf(buf, size, "%dh", (int)(min / 60));
    else if (min < 14 * 24 * 60) snprintf(buf, size, "%dd", (int)(min / (24 * 60)));
    else snprintf(buf, size, "%dw", (int)(min / (7 * 24 * 60)));
}

/* Station-view fields, drawn in fixed columns by table_draw_end_cb() since
 * the radio's font is proportional and spaces can't line text up. */
typedef struct {
    char star[2], call[JS8_RX_CALL_LEN], speed[2], age[8], snr[8], heard[40], grid[8], dist[16], az[8];
} station_fields_t;

/* Initial great-circle bearing from 1 to 2, degrees 0-359. */
static double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180, dl = (lon2 - lon1) * M_PI / 180;
    double b  = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) * 180 / M_PI;
    return fmod(b + 360, 360);
}

static void station_fields(const js8_station_t *st, int64_t now, station_fields_t *f) {
    memset(f, 0, sizeof(*f));
    f->star[0] = st->aprs_gate ? '@' : st->heard_me ? '*' : ' '; /* @: an APRS gateway both ways */
    snprintf(f->call, sizeof(f->call), "%s", st->call);
    js8_speed_t sp = js8_speed_from_submode(st->submode);
    if (sp != JS8_SPEED_NORMAL) f->speed[0] = js8_speed_letter(sp); /* F, T, S */
    if (!st->via[0]) snprintf(f->snr, sizeof(f->snr), "%+d", st->snr);
    snprintf(f->grid, sizeof(f->grid), "%s", st->grid);
    char *age = f->age, *heard = f->heard, *dist = f->dist;
    format_age(now - st->heard_ms, age, sizeof(f->age));
    if (st->via[0]) {
        snprintf(heard, sizeof(f->heard), "via %s", st->via); /* only heard through a relay */
    } else if (st->heard_me) {
        char hage[8];
        format_age(now - st->heard_me_ms, hage, sizeof(hage));
        if (st->has_reported_snr) {
            snprintf(heard, sizeof(f->heard), "heard you %+03d (%s)", st->reported_snr, hage);
        } else {
            snprintf(heard, sizeof(f->heard), "heard you (%s)", hage);
        }
    }
    if (st->grid[0] && my_grid()[0]) {
        double lat, lon, my_lat, my_lon;
        qth_str_to_pos(st->grid, &lat, &lon);
        qth_str_to_pos(my_grid(), &my_lat, &my_lon);
        double km = qth_pos_dist(lat, lon, my_lat, my_lon);
        if (param_i_get(cfg.js8.miles())) snprintf(dist, sizeof(f->dist), "%.0f mi", km * 0.621371);
        else snprintf(dist, sizeof(f->dist), "%.0f km", km);
        snprintf(f->az, sizeof(f->az), "%.0f\xC2\xB0", bearing_deg(my_lat, my_lon, lat, lon)); /* ° */
    }
}

/* The station on the list's cursor row, in either view (not our own rows
 * or info rows). */
static bool row_station(char *call, size_t call_len, float *freq, int *snr) {
    uint16_t row, col;
    lv_table_get_selected_cell(table, &row, &col);
    if (row >= rows || row_hist[row] < 0) return false;
    if (view_stations) {
        const js8_station_t *st = &st_rows[row_hist[row]];
        snprintf(call, call_len, "%s", st->call);
        *freq = st->freq_hz;
        *snr  = st->snr;
    } else {
        const js8_rx_msg_t *m = &history[row_hist[row]];
        if (m->tx || !m->from[0]) return false;
        snprintf(call, call_len, "%s", m->from);
        *freq = m->freq_hz;
        *snr  = m->snr;
    }
    return true;
}

/* The selected station, with its latest offset and SNR. */
static bool selected_station(char *call, size_t call_len, float *freq, int *snr) {
    if (!sel_call[0]) return false;
    snprintf(call, call_len, "%s", sel_call);
    *freq = qso_freq;
    *snr  = sel_snr;
    return true;
}

/* The time offset (DT) of the latest message decoded from `call`. */
static bool station_dt(const char *call, float *dt) {
    for (int age = 0; age < hist_count; age++) {
        int slot = (hist_head - 1 - age + HISTORY) % HISTORY;
        if (!history[slot].tx && strcasecmp(history[slot].from, call) == 0) {
            *dt = history[slot].dt;
            return true;
        }
    }
    return false;
}

/* Desktop JS8Call's macros (<CALL>, <SNR>, <MYGRID4> ...; js8_ops.h)
 * filled in: our station, and the selected one. prune: drop the ones that
 * don't apply (a saved message as it's sent), else leave them as typed. */
static void macros_fill(const char *text, bool prune, char *out, size_t out_len) {
    js8_macro_values_t v = {
        .my_call   = my_call(),
        .my_grid   = grid_of(10), /* <MYGRID12>: a GPS fix to 10 characters */
        .my_info   = info_text,
        .my_status = status_text,
        .version   = JS8_APP_VERSION,
    };
    int64_t last = js8_auto_last_activity(autop);
    v.idle_ms    = last ? now_wall_ms() - last : 0;
    if (sel_call[0]) {
        float dt;
        v.call    = sel_call;
        v.has_snr = true;
        v.snr     = sel_snr;
        if (station_dt(sel_call, &dt)) {
            v.has_tdelta = true;
            v.tdelta_ms  = (int)(dt * 1000);
        }
    }
    js8_macros_expand(text, &v, prune, out, (unsigned)out_len);
}

/* Worked before (the radio's QSO database), per call and band. The
 * Stations view asked the database for every listed station every 5 s and
 * after each decode (bug hunt 17). Forgotten when a QSO is logged here and
 * each time JS8 opens (the FT8 app may have logged meanwhile). */
#define WORKED_CACHE 512
static struct {
    char            call[JS8_RX_CALL_LEN]; /* "": empty */
    qso_log_band_t  band;
    bool            worked;
} worked_cache[WORKED_CACHE];

static bool worked_before(const char *call) {
    qso_log_band_t band = qso_log_freq_to_band(cparam_i_get(cfg.cur.fg_freq()));
    unsigned       h    = 5381;
    for (const char *p = call; *p; p++) h = h * 33 + (unsigned char)*p;
    h = (h * 33 + (unsigned)band) % WORKED_CACHE;
    if (worked_cache[h].call[0] && worked_cache[h].band == band && strcmp(worked_cache[h].call, call) == 0)
        return worked_cache[h].worked;
    bool worked = qso_log_search_worked(call, MODE_JS8, band) > 0;
    snprintf(worked_cache[h].call, sizeof(worked_cache[h].call), "%s", call);
    worked_cache[h].band   = band;
    worked_cache[h].worked = worked;
    return worked;
}

static void cq_heard_add(const char *call) {
    int slot = 0;
    for (int i = 0; i < CQ_HEARD_N; i++) {
        if (strcasecmp(cq_heard[i].call, call) == 0) {
            slot = i;
            break;
        }
        if (cq_heard[i].until_ms < cq_heard[slot].until_ms) slot = i;
    }
    snprintf(cq_heard[slot].call, sizeof(cq_heard[slot].call), "%s", call);
    cq_heard[slot].until_ms = now_mono_ms() + CQ_HEARD_MS;
    if (view_stations) lv_obj_invalidate(table); /* its row turns green */
}

static bool cq_heard_on(const char *call, int64_t now) {
    for (int i = 0; i < CQ_HEARD_N; i++)
        if (cq_heard[i].until_ms > now && strcasecmp(cq_heard[i].call, call) == 0) return true;
    return false;
}

static void worked_forget(void) {
    memset(worked_cache, 0, sizeof(worked_cache));
}

/* ---- Heard: All time -------------------------------------------------
 *
 * The Stations view (and the map) can list every station of this band you
 * have ever exchanged messages with, from the station history, instead of
 * those heard in the last hour. Loaded once (button, band change); new
 * messages update the copy here, so the file isn't read again. A station
 * not heard now has no offset (freq_hz -1). */

static const char *dial_band(void) {
    return js8_log_band((uint64_t)cparam_i_get(cfg.cur.fg_freq()));
}

/* The band as the screens name it: "40m", or "Custom" off the amateur
 * bands (a CB frequency has none). */
static const char *band_name(const char *band) {
    return band && band[0] ? band : "Custom";
}

static void ever_from_live(js8_station_t *row, const js8_station_t *live) {
    row->heard_ms = live->heard_ms;
    row->snr      = live->snr;
    row->freq_hz  = live->freq_hz;
    row->submode  = live->submode;
    if (live->grid[0]) snprintf(row->grid, sizeof(row->grid), "%s", live->grid);
    if (live->heard_me && live->heard_me_ms >= row->heard_me_ms) {
        row->heard_me    = true;
        row->heard_me_ms = live->heard_me_ms;
        if (live->has_reported_snr) {
            row->has_reported_snr = true;
            row->reported_snr     = live->reported_snr;
        }
    }
    row->aprs_gate |= live->aprs_gate;
}

static void ever_load(void) {
    static js8_hist_contact_t c[MAX_ROWS];
    int     n   = js8_history_contacts(history_db, dial_band(), c, MAX_ROWS);
    int64_t now = now_wall_ms();
    ever_count  = 0;
    for (int i = 0; i < n; i++) {
        js8_station_t *r = &ever_rows[ever_count++];
        memset(r, 0, sizeof(*r));
        snprintf(r->call, sizeof(r->call), "%s", c[i].call);
        snprintf(r->grid, sizeof(r->grid), "%s", c[i].grid);
        r->heard_ms         = c[i].heard_ms > c[i].last_ms ? c[i].heard_ms : c[i].last_ms;
        r->snr              = c[i].snr;
        r->freq_hz          = -1;
        r->heard_me         = c[i].heard_us_ms > 0;
        r->heard_me_ms      = c[i].heard_us_ms;
        r->has_reported_snr = c[i].has_reported_snr;
        r->reported_snr     = c[i].reported_snr;
        js8_station_t live;
        if (js8_stations_find(stations, r->call, now, &live)) ever_from_live(r, &live); /* on the air now */
    }
}

/* A message from or to `call`: its row brought up to date, or added if
 * this was your first exchange with them on this band. */
static void ever_merge(const char *call) {
    if (!st_all_time || !call || !call[0]) return;
    if (!js8_history_known(history_db, call, (uint64_t)cparam_i_get(cfg.cur.fg_freq()))) return;
    js8_station_t live;
    if (!js8_stations_find(stations, call, now_wall_ms(), &live)) return;
    for (int i = 0; i < ever_count; i++)
        if (strcasecmp(ever_rows[i].call, call) == 0) {
            ever_from_live(&ever_rows[i], &live);
            return;
        }
    if (ever_count >= MAX_ROWS) return;
    ever_rows[ever_count++] = live;
}

static void rebuild_station_rows(void) {
    /* Keep the cursor on the same station while the list re-sorts. */
    char  keep[JS8_RX_CALL_LEN] = "";
    float f;
    int   n;
    if (!selected_station(keep, sizeof(keep), &f, &n)) keep[0] = '\0';

    int64_t now = now_wall_ms();
    if (st_all_time) {
        st_count = ever_count;
        memcpy(st_rows, ever_rows, sizeof(st_rows[0]) * (size_t)ever_count);
    } else {
        st_count = js8_stations_list(stations, now, st_rows, MAX_ROWS);
    }
    js8_st_sort_t sort = (js8_st_sort_t)(param_i_get(cfg.js8.st_sort()) % JS8_ST_SORT_COUNT);
    if (sort == JS8_ST_SORT_QSO) { /* only those you've had a QSO with: in the History, or logged (green) */
        int n = 0;
        for (int i = 0; i < st_count; i++)
            if (js8_history_had_qso(history_db, st_rows[i].call) || worked_before(st_rows[i].call))
                st_rows[n++] = st_rows[i];
        st_count = n;
    }
    js8_stations_sort(st_rows, st_count, sort, my_grid());

    lv_table_set_row_cnt(table, 1);
    lv_table_set_cell_value(table, 0, 0, "");
    rows = 0;
    if (st_count == 0)
        append_row(sort == JS8_ST_SORT_QSO ? "No stations here you've had a QSO with (Sort: QSO)"
                   : st_all_time           ? "No stations in the history on this band yet"
                                           : "No stations heard yet",
                   -1);

    int keep_row = 0;
    for (int i = 0; i < st_count; i++) {
        char hit[16];
        st_alert[i]  = js8_alert_hit("", st_rows[i].call, alert_words, hit, sizeof(hit));
        st_worked[i] = worked_before(st_rows[i].call);
        if (keep[0] && strcmp(st_rows[i].call, keep) == 0) keep_row = rows;
        append_row(" ", (int16_t)i); /* drawn by table_draw_end_cb() */
    }
    select_row(keep_row);
    if (view_map) map_update(false);
}

/* Commands in colour (desktop draws them as "pills"): the table draws such
 * a row's text transparent, and table_draw_end_cb() draws it again with
 * the command recoloured. Recolour codes take no width, so the lines break
 * exactly as the table measured them. */
#define CMD_COLOR "ffd24a"
static struct {
    bool                on;
    uint32_t            id;          /* the cell */
    unsigned            start, len;  /* the command in the cell's text */
    lv_draw_label_dsc_t label;       /* how the table would have drawn it */
} cmd_draw;

/* The command in message row `row`'s text: false for rows without one. */
static bool row_command(uint32_t row, const js8_rx_msg_t *m, unsigned *start, unsigned *len) {
    const char *cell = lv_table_get_cell_value(table, row, 0);
    const char *at   = cell ? strstr(cell, m->text) : NULL;
    if (!at || !js8_command_span(m->text, start, len)) return false;
    *start += (unsigned)(at - cell);
    return true;
}

static void draw_recoloured(lv_obj_t *obj, lv_obj_draw_part_dsc_t *dsc, uint32_t row) {
    const char *cell = lv_table_get_cell_value(obj, row, 0);
    if (!cell) return;
    char   buf[2 * (JS8_RX_TEXT_LEN + 48) + 16];
    size_t o = 0, n = strlen(cell);
    for (size_t i = 0; i < n && o + 12 < sizeof(buf); i++) {
        if (i == cmd_draw.start) o += (size_t)snprintf(buf + o, sizeof(buf) - o, "#" CMD_COLOR " ");
        if (cell[i] == '#' && o + 2 < sizeof(buf)) buf[o++] = '#'; /* "##" is a plain '#' */
        buf[o++] = cell[i];
        if (i + 1 == cmd_draw.start + cmd_draw.len) buf[o++] = '#';
    }
    buf[o] = '\0';

    /* Where lv_table puts a cell's text: inside the padding, centred. */
    lv_draw_label_dsc_t l    = cmd_draw.label;
    lv_area_t           cell_a = *dsc->draw_area, a;
    a.x1 = cell_a.x1 + lv_obj_get_style_pad_left(obj, LV_PART_ITEMS);
    a.x2 = cell_a.x2 - lv_obj_get_style_pad_right(obj, LV_PART_ITEMS);
    lv_point_t size;
    lv_txt_get_size(&size, cell, l.font, l.letter_space, l.line_space, lv_area_get_width(&a), LV_TEXT_FLAG_NONE);
    lv_coord_t h = lv_area_get_height(&cell_a);
    a.y1         = cell_a.y1 + h / 2 - size.y / 2;
    a.y2         = cell_a.y1 + h / 2 + size.y / 2;
    l.flag |= LV_TEXT_FLAG_RECOLOR;

    const lv_area_t *clip_ori = dsc->draw_ctx->clip_area;
    lv_area_t        clip;
    if (!_lv_area_intersect(&clip, clip_ori, &cell_a)) return;
    dsc->draw_ctx->clip_area = &clip;
    lv_draw_label(dsc->draw_ctx, &l, &a, buf, NULL);
    dsc->draw_ctx->clip_area = clip_ori;
}

static void table_draw_cb(lv_event_t *e) {
    lv_obj_t               *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
    int16_t  h   = row < rows ? row_hist[row] : -1;

    dsc->rect_dsc->bg_opa = LV_OPA_50;

    if (h < 0) {
        dsc->label_dsc->align   = LV_TEXT_ALIGN_CENTER;
        dsc->rect_dsc->bg_color = lv_color_hex(0x303030);
    } else if (view_stations) {
        /* Stations that heard us stand out, like desktop's star; those who
         * called CQ in the last 5 min are green, as CQ rows in the list. */
        dsc->rect_dsc->bg_color = st_alert[h]                                  ? lv_color_hex(ALERT_COLOR)
                                  : cq_heard_on(st_rows[h].call, now_mono_ms()) ? lv_color_hex(0x006000)
                                  : st_rows[h].heard_me                        ? lv_color_hex(0x5a4400)
                                                                               : lv_color_black();
    } else {
        const js8_rx_msg_t *m = &history[h];
        if (m->tx) { /* red: transmitting, as the TX bar */
            dsc->rect_dsc->bg_color = lv_color_hex(0xB00000);
        } else if (m->alert) {
            dsc->rect_dsc->bg_color = lv_color_hex(ALERT_COLOR);
        } else if (m->to_me) {
            dsc->rect_dsc->bg_color = lv_color_hex(0x1830a0);
        } else if (m->cq) {
            dsc->rect_dsc->bg_color = lv_color_hex(0x006000);
        } else if (m->heartbeat) {
            dsc->rect_dsc->bg_color = lv_color_black();
            dsc->label_dsc->color   = lv_color_hex(0x808080);
        } else if (m->to_group) {
            dsc->rect_dsc->bg_color = lv_color_hex(0x102a60);
        } else {
            dsc->rect_dsc->bg_color = lv_color_black();
        }
        if (m->low_confidence) dsc->label_dsc->opa = LV_OPA_60;
    }

    uint16_t sel_row, sel_col;
    lv_table_get_selected_cell(obj, &sel_row, &sel_col);
    if (sel_row == row) dsc->rect_dsc->bg_color = lv_color_lighten(dsc->rect_dsc->bg_color, 30);

    /* A command to colour (not in the dimmed heartbeat rows): drawn later. */
    cmd_draw.on = false;
    if (h >= 0 && !view_stations && !history[h].heartbeat &&
        row_command(row, &history[h], &cmd_draw.start, &cmd_draw.len)) {
        cmd_draw.on          = true;
        cmd_draw.id          = dsc->id;
        cmd_draw.label       = *dsc->label_dsc;
        dsc->label_dsc->opa = LV_OPA_TRANSP;
    }
}

/* On offset `f`: within desktop's "same station" window for the row's speed. */
static bool row_on_freq(const js8_rx_msg_t *m, float f) {
    return f >= 0 && fabsf(m->freq_hz - f) <= js8_speed_rx_threshold_hz(js8_speed_from_submode(m->submode));
}

/* Rows with the green bar: with the cursor on a row without a callsign,
 * every row on its frequency; else the selected station's rows, ours to
 * them, and the rows without a callsign on their frequency. */
static bool row_is_selected_station(int16_t h) {
    if (h < 0) return false;
    if (view_stations) return sel_call[0] && strcasecmp(st_rows[h].call, sel_call) == 0;
    const js8_rx_msg_t *m = &history[h];
    if (cursor_freq >= 0) return row_on_freq(m, cursor_freq);
    if (!sel_call[0]) return false;
    if (!m->tx) return strcasecmp(m->from, sel_call) == 0 || (!m->from[0] && row_on_freq(m, qso_freq));
    size_t n = strlen(sel_call);
    return strncasecmp(m->text, sel_call, n) == 0 && (m->text[n] == ' ' || m->text[n] == '\0');
}

/* Which rows have the green bar on the screen now (as last drawn), so a new
 * selection redraws only the rows whose mark changes: on R1CBU 1.0 every
 * redrawn pixel is rotated too, and the MFK steps through stations
 * quickly (the user saw the waterfall stutter while scrolling). */
static bool row_mark_drawn[MAX_ROWS + 1];

/* A row's place on the screen, as lv_table's get_cell_area() has it. */
static void table_row_area(uint16_t row, lv_area_t *a) {
    lv_table_t *t = (lv_table_t *)table;
    lv_coord_t  y = 0;
    for (uint16_t r = 0; r < row && r < t->row_cnt; r++) y += t->row_h[r];
    y += lv_obj_get_style_pad_top(table, 0) - lv_obj_get_scroll_y(table);
    a->x1 = table->coords.x1;
    a->x2 = table->coords.x2;
    a->y1 = table->coords.y1 + y;
    a->y2 = a->y1 + (row < t->row_cnt ? t->row_h[row] : 0) - 1;
}

static void table_invalidate_row(uint16_t row) {
    if (!table || row >= ((lv_table_t *)table)->row_cnt || !lv_obj_is_visible(table)) return;
    lv_area_t a;
    table_row_area(row, &a);
    if (_lv_area_intersect(&a, &a, &table->coords)) _lv_inv_area(lv_obj_get_disp(table), &a);
}

/* The rows whose green bar comes or goes with the selection now. */
static void marks_invalidate(void) {
    if (!table) return;
    for (unsigned r = 0; r < rows && r <= MAX_ROWS; r++)
        if (row_is_selected_station(row_hist[r]) != row_mark_drawn[r]) table_invalidate_row((uint16_t)r);
}

/* The MFK (and arrow keys) on the list, before lv_table's own handler: the
 * same step, but only the old and new rows redrawn, and a scroll at once
 * when the row leaves the view. lv_table redraws the whole list on every
 * step and scrolls with an animation, which redraws all of it for each
 * frame of the scroll. */
static void table_key_pre_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT && key != LV_KEY_UP && key != LV_KEY_DOWN) return;
    lv_table_t *t = (lv_table_t *)table;
    if (!t->row_cnt || t->row_act == LV_TABLE_CELL_NONE) return; /* lv_table's first step: as it is */
    lv_event_stop_processing(e);
    user_touch(); /* key_cb's part for these keys */
    int32_t row = t->row_act >= t->row_cnt ? 0 : t->row_act;
    int32_t old = t->row_act;
    if (key == LV_KEY_LEFT || key == LV_KEY_UP) row--;
    else row++;
    if (row < 0) row = 0;
    if (row >= (int32_t)t->row_cnt) row = t->row_cnt - 1;
    if (row == old) return;
    t->col_act = 0;
    t->row_act = (uint16_t)row;
    table_invalidate_row((uint16_t)old);
    table_invalidate_row((uint16_t)row);
    lv_area_t a;
    table_row_area((uint16_t)row, &a);
    if (a.y1 < table->coords.y1) lv_obj_scroll_by_bounded(table, 0, table->coords.y1 - a.y1, LV_ANIM_OFF);
    else if (a.y2 > table->coords.y2) lv_obj_scroll_by_bounded(table, 0, table->coords.y2 - a.y2, LV_ANIM_OFF);
    lv_event_send(table, LV_EVENT_VALUE_CHANGED, NULL);
}

/* A green bar marks the selected station's rows; station view: draw the
 * fields at fixed x positions within the cell. */
static void table_draw_end_cb(lv_event_t *e) {
    lv_obj_t               *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
    int16_t  h   = row < rows ? row_hist[row] : -1;
    if (cmd_draw.on && cmd_draw.id == dsc->id) {
        cmd_draw.on = false;
        draw_recoloured(obj, dsc, row);
    }
    if (row <= MAX_ROWS) row_mark_drawn[row] = h >= 0 && row_is_selected_station(h);
    if (h < 0) return;

    if (row_is_selected_station(h)) {
        lv_draw_rect_dsc_t bar;
        lv_draw_rect_dsc_init(&bar);
        bar.bg_color   = lv_color_hex(0x40ff40);
        lv_area_t area = *dsc->draw_area;
        area.x2        = area.x1 + 3;
        lv_draw_rect(dsc->draw_ctx, &bar, &area);
    }
    if (!view_stations) return;

    station_fields_t f;
    station_fields(&st_rows[h], now_wall_ms(), &f);

    static const struct {
        lv_coord_t x;
        size_t     field;
    } cols[] = {
        {0, offsetof(station_fields_t, star)},    {22, offsetof(station_fields_t, call)},
        {140, offsetof(station_fields_t, speed)}, {162, offsetof(station_fields_t, age)},
        {218, offsetof(station_fields_t, snr)},   {276, offsetof(station_fields_t, heard)},
        {505, offsetof(station_fields_t, grid)},  {588, offsetof(station_fields_t, dist)},
        {712, offsetof(station_fields_t, az)}, /* "10833 km" needs the room */
    };

    lv_area_t area = *dsc->draw_area;
    area.y1 += lv_obj_get_style_pad_top(obj, LV_PART_ITEMS);
    area.y2 -= lv_obj_get_style_pad_bottom(obj, LV_PART_ITEMS);
    lv_coord_t x0 = dsc->draw_area->x1 + lv_obj_get_style_pad_left(obj, LV_PART_ITEMS);

    /* Calls already in the log in green, like desktop's worked-before tick. */
    lv_draw_label_dsc_t worked = *dsc->label_dsc;
    worked.color               = lv_color_hex(0x80ff80);

    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        area.x1 = x0 + cols[i].x;
        area.x2 = (i + 1 < sizeof(cols) / sizeof(cols[0])) ? x0 + cols[i + 1].x - 6 : dsc->draw_area->x2;
        bool green = cols[i].field == offsetof(station_fields_t, call) && st_worked[h];
        lv_draw_label(dsc->draw_ctx, green ? &worked : dsc->label_dsc, &area, (const char *)&f + cols[i].field, NULL);
    }
}

/* The green band: the selected station's offset, as wide as the speed it
 * was last heard at (selected_speed(): its Stations entry, or the row's
 * own), so a Turbo station's band covers its 160 Hz. JS8 draws it itself
 * (lv_finder's cursor is as wide as the TX band, and lv_finder is FT8's
 * too); moving an object redraws only its old and new place. Hidden
 * only where the red band covers it exactly (same offset and width). */
static void cursor_place(void) {
    if (!cursor_box || !finder) return;
    int32_t span = filter_high - filter_low;
    int32_t hz   = qso_freq >= 0 ? (int32_t)(qso_freq + 0.5f) : -1;
    js8_speed_t sp;
    int32_t     bw = js8_speed_bandwidth_hz(table && selected_speed(&sp) ? sp : cur_speed());
    if (hz < 0 || span <= 0 || (hz == finder_shown && bw == js8_speed_bandwidth_hz(cur_speed()))) {
        lv_obj_add_flag(cursor_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_coord_t  w  = lv_obj_get_width(finder);
    lv_coord_t  x1 = (lv_coord_t)((int64_t)w * (hz - filter_low) / span);
    lv_coord_t  x2 = (lv_coord_t)((int64_t)w * (hz + bw - filter_low) / span);
    lv_obj_set_pos(cursor_box, x1, 1);
    lv_obj_set_size(cursor_box, LV_MAX(x2 - x1 + 1, 3), lv_obj_get_height(finder) - 2);
    lv_obj_clear_flag(cursor_box, LV_OBJ_FLAG_HIDDEN);
}

static void show_selection(void) {
    if (sel_call[0]) map_qrz_clear(sel_call); /* you've seen who called */
    cursor_place();
    marks_invalidate();
    update_tx_bar();
    if (view_map) map_update(false);
}

/* The station on the cursor's row becomes the selection (the user moved
 * or tapped); rows without one (ours, info) leave it as it was. `announce`
 * also pops up who it is (on a tap, not on every MFK step). */
static void select_at_cursor(bool announce) {
    char  call[JS8_RX_CALL_LEN];
    float freq;
    int   snr;
    if (!row_station(call, sizeof(call), &freq, &snr)) return;
    snprintf(sel_call, sizeof(sel_call), "%s", call);
    qso_freq = freq;
    sel_snr  = snr;
    show_selection();
    if (announce) {
        if (freq > 0) msg_update_text_fmt("%s at %.0f Hz, %+d dB", call, freq, snr);
        else msg_update_text_fmt("%s: not heard on this frequency lately", call);
    }
}

static void clear_selection(void) {
    sel_call[0] = '\0';
    sel_locked  = false;
    qso_freq    = -1;
    cursor_freq = -1;
    show_selection();
}

/* The station on the cursor's row, if any. */
static bool cursor_call(char *call, size_t len) {
    float freq;
    int   snr;
    return row_station(call, len, &freq, &snr);
}

/* A press selects the station on its row; on another station than the
 * locked one it also ends the lock. */
static void table_press_cb(lv_event_t *e) {
    (void)e;
    char call[JS8_RX_CALL_LEN];
    press_held      = false;
    press_on_locked = sel_locked && cursor_call(call, sizeof(call)) && strcasecmp(call, sel_call) == 0;
    if (press_on_locked) return;
    if (sel_locked && cursor_call(call, sizeof(call))) {
        sel_locked = false;
        msg_update_text_fmt("%s unlocked", sel_call);
    }
    select_at_cursor(true);
}

/* A short press in the Stations view or on the map: the station's
 * history. (After a hold, LVGL sends this too: the table is the group's
 * only object.) */
static void table_short_click_cb(lv_event_t *e) {
    (void)e;
    char call[JS8_RX_CALL_LEN];
    if (press_held || !view_stations || !cursor_call(call, sizeof(call))) return;
    hpage_open(call, 0);
}

/* Held (about half a second, LVGL's long press plus one repeat): lock the
 * station, or unlock it if it was the locked one. */
static void table_hold_cb(lv_event_t *e) {
    (void)e;
    if (press_held) return; /* one per press */
    press_held = true;
    user_touch();
    if (press_on_locked) {
        sel_locked = false;
        msg_update_text_fmt("%s unlocked: turning the knob selects again", sel_call);
    } else if (sel_call[0]) {
        char call[JS8_RX_CALL_LEN];
        if (!cursor_call(call, sizeof(call)) || strcasecmp(call, sel_call) != 0) return;
        sel_locked = true;
        msg_update_text_fmt("%s locked: the knob only scrolls; press another station to change, hold to unlock",
                            sel_call);
    }
    update_tx_bar();
}

/* A received row without a callsign under the cursor: its offset, else -1. */
static float callless_cursor_freq(void) {
    if (view_stations) return -1;
    uint16_t row = 0, col = 0;
    lv_table_get_selected_cell(table, &row, &col);
    if (row == LV_TABLE_CELL_NONE || row >= rows || row_hist[row] < 0) return -1;
    const js8_rx_msg_t *m = &history[row_hist[row]];
    return m->tx || m->from[0] ? -1 : m->freq_hz;
}

static void table_select_cb(lv_event_t *e) {
    (void)e;
    float f = auto_selecting ? -1 : callless_cursor_freq(); /* following new rows: back to the station */
    if (f != cursor_freq) {
        cursor_freq = f;
        marks_invalidate();
    }
    if (auto_selecting) return;
    cursor_user_ms = now_wall_ms();
    if (sel_locked) return; /* scrolling to read: the locked station stays */
    select_at_cursor(false);
}

/* ---- Log ----------------------------------------------------------------
 *
 * Health lines to the app's log (/tmp/x6100_log.txt on the radio), stamped
 * with UTC so they line up with x6100-cpulog and the console. stderr: not
 * buffered, so a line is there as it happens. Any thread. */
static void js8_log(const char *fmt, ...) {
    char            text[200];
    va_list         ap;
    struct timespec ts;
    struct tm       tm;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "JS8 %02d:%02d:%02d.%03ldZ %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, text);
}

/* The receiver's and decoder's health lines (receiver.cpp on_report). */
static void on_report(const char *line, void *ctx) {
    (void)ctx;
    js8_log("%s", line);
}

static void wf_watch_cb(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    js8_wf_repaint_soon();
}

/* NR, NB, DNF or DNF auto changed while JS8 is open: on whichever thread
 * changed it (CAT's own), so radio calls only, nothing on the screen. */
static void dsp_held_cb(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    radio_set_rx_dsp_off(true);
}

/* GUI thread, every tick: a long gap since the last one is a stall. */
static void health_tick(void) {
    int64_t now = now_mono_ms() * 1000;
    if (gui_last_us && now - gui_last_us > GUI_STALL_US) {
        gui_stalls++;
        if (now - gui_last_us > gui_stall_max_us) gui_stall_max_us = now - gui_last_us;
    }
    gui_last_us = now;
    if (!health_start_us) health_start_us = now;
    if (now - health_start_us < HEALTH_US) return;
    pthread_mutex_lock(&wf_lock);
    unsigned dropped = wf_dropped;
    wf_dropped       = 0;
    pthread_mutex_unlock(&wf_lock);
    if (gui_stalls || dropped)
        js8_log("GUI: %u stalls over %d ms (longest %.1f s), %u waterfall rows dropped", gui_stalls,
                GUI_STALL_US / 1000, gui_stall_max_us / 1e6, dropped);
    unsigned h_rows, h_commits, h_failed;
    int64_t  h_busy_us;
    if (js8_history_stats(history_db, &h_rows, &h_commits, &h_failed, &h_busy_us))
        js8_log("history: %u rows in %u writes, %.1f ms in the database%s", h_rows, h_commits, h_busy_us / 1000.0,
                h_failed ? ", some NOT written" : "");
    gui_stalls       = 0;
    gui_stall_max_us = 0;
    health_start_us  = now;
}

/* ---- From the worker threads to the GUI -------------------------------
 *
 * Messages, the end of each decode cycle and the transmitter's progress
 * wait in JS8's own queue, drained on the GUI thread every WF_TICK_MS
 * (ev_tick in wf_timer_cb), and waterfall rows in their own ring. The
 * shared scheduler queue (64 items, src/scheduler.cpp) drops what doesn't
 * fit, and waterfall rows alone were 15 a second: a GUI stall of a few
 * seconds lost decoded messages (B-25). The text so far of a message still
 * arriving replaces its older text still waiting, so only the newest
 * counts. */

typedef enum { EV_MESSAGE, EV_CYCLE_DONE, EV_TX_STATUS, EV_TX_DONE, EV_AUTO_DRIFT, EV_SEARCH } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    union {
        js8_rx_msg_t    msg;
        unsigned        decodes;
        js8_tx_status_t tx_status;
        tx_done_t       tx_done;
        struct {
            int64_t  drift_ms;
            unsigned frames;
        } auto_drift;
        js8_rx_search_t search;
    };
} ui_event_t;

#define EVENTS   128 /* a busy slot's decodes are a few dozen */
#define EV_BATCH 16  /* handled per tick */

static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static ui_event_t      ev_queue[EVENTS];
static unsigned        ev_head, ev_count, ev_lost;

static void ui_add_message(void *arg);
static void ui_cycle_done(void *arg);
static void ui_tx_status(void *arg);
static void ui_tx_done(void *arg);
static void ui_auto_drift(int64_t drift_ms, unsigned frames);
static void ui_search_done(const js8_rx_search_t *r);

static void ev_push(const ui_event_t *e) {
    pthread_mutex_lock(&ev_lock);
    /* Newer text of a message still arriving (or its end): replaces the
     * text waiting, back to the last end of cycle (which must follow its
     * cycle's messages). */
    if (e->kind == EV_MESSAGE && e->msg.msg_id && !e->msg.tx) {
        for (unsigned k = ev_count; k-- > 0;) {
            ui_event_t *q = &ev_queue[(ev_head + k) % EVENTS];
            if (q->kind == EV_CYCLE_DONE) break;
            if (q->kind == EV_MESSAGE && q->msg.partial && !q->msg.tx && q->msg.msg_id == e->msg.msg_id) {
                *q = *e;
                pthread_mutex_unlock(&ev_lock);
                return;
            }
        }
    }
    if (ev_count == EVENTS) {
        ev_lost++;
    } else {
        ev_queue[(ev_head + ev_count) % EVENTS] = *e;
        ev_count++;
    }
    pthread_mutex_unlock(&ev_lock);
}

static void ev_clear(void) {
    pthread_mutex_lock(&ev_lock);
    ev_count = ev_lost = 0;
    pthread_mutex_unlock(&ev_lock);
}

/* The GUI thread, every WF_TICK_MS. */
static void ev_tick(void) {
    static ui_event_t batch[EV_BATCH];
    pthread_mutex_lock(&ev_lock);
    unsigned n = ev_count < EV_BATCH ? ev_count : EV_BATCH, lost = ev_lost;
    for (unsigned i = 0; i < n; i++) batch[i] = ev_queue[(ev_head + i) % EVENTS];
    ev_head  = (ev_head + n) % EVENTS;
    ev_count -= n;
    ev_lost  = 0;
    pthread_mutex_unlock(&ev_lock);

    if (lost) {
        LV_LOG_WARN("JS8: %u decoder/TX events lost, the GUI fell behind", lost);
        add_info_row("%u decodes lost: the screen fell behind", lost);
    }
    for (unsigned i = 0; i < n; i++) {
        ui_event_t *e = &batch[i];
        switch (e->kind) {
        case EV_MESSAGE: ui_add_message(&e->msg); break;
        case EV_CYCLE_DONE: ui_cycle_done(&e->decodes); break;
        case EV_TX_STATUS: ui_tx_status(&e->tx_status); break;
        case EV_TX_DONE: ui_tx_done(&e->tx_done); break;
        case EV_AUTO_DRIFT: ui_auto_drift(e->auto_drift.drift_ms, e->auto_drift.frames); break;
        case EV_SEARCH: ui_search_done(&e->search); break;
        }
    }
}

/* ---- Receiver callbacks (worker threads) ------------------------------- */

static void ui_add_message(void *arg) {
    if (!dialog.run || !table) return;
    add_message((const js8_rx_msg_t *)arg);
}

static void on_message(const js8_rx_msg_t *m, void *ctx) {
    (void)ctx;
    ui_event_t e = {.kind = EV_MESSAGE, .msg = *m};
    ev_push(&e);
}

static void update_status(void) {
    time_t    now = (time_t)(now_wall_ms() / 1000);
    struct tm tm;
    gmtime_r(&now, &tm);
    bool testing = js8_rx_wav_active(rx);

    /* Always show what may transmit by itself. */
    char    flags[80] = "";
    int64_t now_ms    = now_wall_ms();
    bool    any_auto  = param_i_get(cfg.js8.auto_mode()) || param_i_get(cfg.js8.hb());
    if (any_auto && js8_auto_idle(autop, now_ms)) {
        snprintf(flags, sizeof(flags), "AUTO/HB PAUSED (idle)  ");
    } else {
        if (param_i_get(cfg.js8.auto_mode())) strcat(flags, "AUTO  ");
        if (param_i_get(cfg.js8.hb())) {
            char hb[40];
            if (!js8_speed_heartbeats(cur_speed())) {
                snprintf(hb, sizeof(hb), "HB paused (%s)  ", js8_speed_name(cur_speed()));
            } else if (hb_paused()) {
                time_t    t = (time_t)(hb_paused_until / 1000);
                struct tm nt;
                gmtime_r(&t, &nt);
                snprintf(hb, sizeof(hb), "HB paused to %02d:%02d  ", nt.tm_hour, nt.tm_min);
            } else if (hb_next_ms > now_ms) {
                time_t    t = (time_t)(hb_next_ms / 1000);
                struct tm nt;
                gmtime_r(&t, &nt);
                snprintf(hb, sizeof(hb), "HB %um next %02d:%02d  ", param_i_get(cfg.js8.hb_interval()), nt.tm_hour, nt.tm_min);
            } else {
                snprintf(hb, sizeof(hb), "HB %um  ", param_i_get(cfg.js8.hb_interval()));
            }
            strcat(flags, hb);
        }
        if (param_i_get(cfg.js8.hb_ack()) && param_i_get(cfg.js8.auto_mode()) && param_i_get(cfg.js8.hb())) strcat(flags, "ACK  ");
    }
    int unread = js8_inbox_unread(inbox);
    if (unread) {
        char m[24];
        snprintf(m, sizeof(m), "MSG %d NEW  ", unread);
        strcat(flags, m);
    }
    /* JS8 runs off the radio's clock by this much (Time Sync). */
    char    drift[24] = "";
    int64_t d         = js8_drift_ms();
    if (d) snprintf(drift, sizeof(drift), " drift %+.1fs", d / 1000.0);
    /* GPS: our grid comes from it while it has a fix. */
    const char *gps = gps_fix_current()                          ? "  GPS"
                      : gps_seen && time(NULL) - gps_seen <= 10 ? "  GPS no fix"
                                                                 : "";
    lv_label_set_text_fmt(status, "%s%s%s%s  %02d:%02d:%02dZ%s  total %u", flags, testing ? "TEST WAV  " : "",
                          where_label(), gps, tm.tm_hour, tm.tm_min, tm.tm_sec, drift, hist_count);
    if (map_status && view_map) { /* the same, over the map */
        lv_label_set_text(map_status, lv_label_get_text(status));
        map_status_changed();
    }
}

static void ui_cycle_done(void *arg) {
    if (!dialog.run || !status) return;
    cycles++;
    cycle_decodes = *(unsigned *)arg;
    update_status();
    auto_try_send(); /* the cycle's replies, now that all of it is in */
}

/* Automatic time sync's drift after a decode pass (decode thread). */
static void on_auto_drift(int64_t drift_ms, unsigned frames, void *ctx) {
    (void)ctx;
    ui_event_t e = {.kind = EV_AUTO_DRIFT, .auto_drift = {drift_ms, frames}};
    ev_push(&e);
}

/* The time search ended (its own thread). */
static void on_search(const js8_rx_search_t *r, void *ctx) {
    (void)ctx;
    ui_event_t e = {.kind = EV_SEARCH, .search = *r};
    ev_push(&e);
}

static void on_cycle_done(unsigned decodes, void *ctx) {
    (void)ctx;
    ui_event_t e = {.kind = EV_CYCLE_DONE, .decodes = decodes};
    ev_push(&e);
}

static void wf_queue_clear(void) {
    pthread_mutex_lock(&wf_lock);
    wf_q_count = 0;
    pthread_mutex_unlock(&wf_lock);
    atomic_store(&wf_avg_restart, true); /* a new band or a cleared waterfall: no old rows in the average */
}

/* ---- Decode marks (Settings: Decode marks) -------------------------------
 *
 * Desktop JS8Call's "Show decode attempts": a bracket as wide as the signal
 * over the newest waterfall rows wherever the decoder found a JS8 sync and
 * is trying to decode, coloured by the sync's strength, and yellow for what
 * it decoded (desktop's red is our TX band here). They scroll down with the
 * waterfall, so weak signals the waterfall barely shows are marked. The
 * decoder reports them from its thread, many per pass: they wait in their
 * own queue, not the scheduler's, whose 64 items are for messages. */

#define MARK_QUEUE     128                    /* per 5 ms tick; more are dropped (cosmetic) */
#define MARK_MEMORY    48                     /* brackets remembered, to recolour in place */
#define MARK_ROWS      15                     /* bracket height */
#define MARK_LINE      3                      /* line thickness */
#define MARK_SAME_ROWS (8 * WF_ROWS_PER_SEC)  /* another pass over the same signal comes sooner */

typedef struct {
    float    freq_hz;
    uint32_t row; /* js8_wf_rows() when drawn */
    uint8_t  submode;
    uint8_t  level;
    bool     used;
} mark_drawn_t;

static pthread_mutex_t mark_lock = PTHREAD_MUTEX_INITIALIZER;
static js8_rx_mark_t   mark_queue[MARK_QUEUE];
static unsigned        mark_count;
static mark_drawn_t    marks_drawn[MARK_MEMORY];
static unsigned        marks_next;

static const uint32_t mark_colors[] = {
    [JS8_MARK_WEAK]    = 0x008ca0, /* dark cyan */
    [JS8_MARK_MEDIUM]  = 0x00e6ff, /* cyan */
    [JS8_MARK_STRONG]  = 0xffffff,
    [JS8_MARK_DECODED] = 0xffd600, /* yellow */
};

/* The decoder's thread. */
static void on_mark(const js8_rx_mark_t *m, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&mark_lock);
    if (mark_count < MARK_QUEUE) mark_queue[mark_count++] = *m;
    pthread_mutex_unlock(&mark_lock);
}

static void marks_reset(void) {
    pthread_mutex_lock(&mark_lock);
    mark_count = 0;
    pthread_mutex_unlock(&mark_lock);
    memset(marks_drawn, 0, sizeof(marks_drawn));
}

/* |-----| from the lowest tone across the signal's bandwidth, its top `y`
 * rows below the newest row. */
static void mark_draw(float freq_hz, int bw_hz, int y, uint8_t level) {
    int span = filter_high - filter_low;
    if (span <= 0) return;
    lv_coord_t x1   = (lv_coord_t)((freq_hz - filter_low) * WIDTH / span);
    lv_coord_t x2   = (lv_coord_t)((freq_hz + bw_hz - filter_low) * WIDTH / span);
    lv_coord_t half = MARK_LINE / 2, mid = y + MARK_ROWS / 2;
    lv_color_t c    = lv_color_hex(mark_colors[level]);
    js8_wf_fill_rect(x1 - half, y, x1 + half, y + MARK_ROWS - 1, c);
    js8_wf_fill_rect(x2 - half, y, x2 + half, y + MARK_ROWS - 1, c);
    js8_wf_fill_rect(x1, mid - half, x2, mid + half, c);
}

/* A new bracket, unless it's the same signal as one drawn moments ago
 * (every decode pass reports its candidates again): that one is recoloured
 * where it has scrolled to, if this is stronger or decoded. */
static void mark_show(const js8_rx_mark_t *m) {
    int      bw  = js8_speed_bandwidth_hz(js8_speed_from_submode(m->submode));
    uint32_t now = js8_wf_rows();
    for (unsigned i = 0; i < MARK_MEMORY; i++) {
        mark_drawn_t *d = &marks_drawn[i];
        if (!d->used || d->submode != m->submode) continue;
        uint32_t age = now - d->row;
        if (age > MARK_SAME_ROWS || fabsf(d->freq_hz - m->freq_hz) > bw / 2.0f) continue;
        if (m->level > d->level) {
            mark_draw(d->freq_hz, bw, (int)age, m->level);
            d->level = m->level;
        }
        return;
    }
    mark_draw(m->freq_hz, bw, 0, m->level);
    marks_drawn[marks_next] = (mark_drawn_t){.freq_hz = m->freq_hz, .row = now, .submode = m->submode,
                                             .level = m->level, .used = true};
    marks_next = (marks_next + 1) % MARK_MEMORY;
}

static void marks_tick(void) {
    js8_rx_mark_t batch[MARK_QUEUE];
    pthread_mutex_lock(&mark_lock);
    unsigned n = mark_count;
    memcpy(batch, mark_queue, n * sizeof(batch[0]));
    mark_count = 0;
    pthread_mutex_unlock(&mark_lock);
    if (!param_i_get(cfg.js8.decode_marks()) || !wf_box) return;
    for (unsigned i = 0; i < n; i++) mark_show(&batch[i]);
}

static int64_t now_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* At most one row per tick, each when it's due by the clock. Rows are made
 * per WF_ROW_SAMPLES (800 at 12 kHz) and the audio clock isn't the CPU's, so a
 * queue that builds up is drained by drawing slightly faster, never by a
 * jump. A row goes straight onto the display's lower plane (js8_wf.c), and
 * reaches the screen at the main loop's page flip right after this timer. */
static void wf_timer_cb(lv_timer_t *t) {
    (void)t;
    health_tick();
    marks_tick();
    ev_tick();
    js8_wf_tick(); /* marks drawn, or a repaint due */
    pthread_mutex_lock(&wf_lock);
    unsigned waiting = wf_q_count;
    pthread_mutex_unlock(&wf_lock);
    if (!waiting) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now    = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    int64_t period = 1000000 / WF_ROWS_PER_SEC;
    if (waiting > 6) period = period * 3 / 4;
    else if (waiting > 3) period = period * 19 / 20;
    /* After a pause (no audio while transmitting) start again now. */
    if (now - wf_due_us > period) wf_due_us = now;
    if (now < wf_due_us) return;

    static float row[WIDTH];
    pthread_mutex_lock(&wf_lock);
    memcpy(row, wf_rows[wf_q_head], sizeof(row));
    wf_q_head = (wf_q_head + 1) % WF_QUEUE;
    wf_q_count--;
    pthread_mutex_unlock(&wf_lock);
    js8_wf_add_row(row, WIDTH);
    wf_due_us += period;
    js8_wf_tick();
}

/* The k-th smallest of v[0..n-1], reordering v: quickselect (Hoare's
 * partition), O(n), where sorting the row was O(n log n) per row. */
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

static void wf_emit_row(void) {
    spgramf_get_psd(sg, psd);
    liquid_vectorf_addscalar(psd, nfft, -10.f * log10f(sqrtf(nfft)), psd);
    spgramf_reset(sg);

    /* spgram output is FFT-shifted: bin nfft/2 is 0 Hz. */
    uint32_t low_bin  = nfft / 2u + (uint32_t)nfft * filter_low / SAMPLE_RATE;
    uint32_t high_bin = nfft / 2u + (uint32_t)nfft * filter_high / SAMPLE_RATE;
    if (high_bin > nfft) high_bin = nfft;
    if (low_bin >= high_bin) return;

    /* One value per pixel: the strongest of the bins under it, so a narrow
     * signal between two pixels' centres doesn't flicker. */
    uint32_t bins = high_bin - low_bin;
    for (uint32_t x = 0; x < WIDTH; x++) {
        uint32_t b0 = low_bin + x * bins / WIDTH, b1 = low_bin + (x + 1) * bins / WIDTH;
        float    v  = psd[b0];
        for (uint32_t b = b0 + 1; b < b1; b++) v = psd[b] > v ? psd[b] : v;
        wf_row[x] = v;
    }

    /* Noise floor: the 30th percentile of the row, smoothed over ~2 s. */
    memcpy(wf_sel, wf_row, sizeof(wf_sel));
    float floor_now = select_nth(wf_sel, WIDTH, WIDTH * 3 / 10);
    wf_floor_db     = wf_floor_set ? wf_floor_db + 0.05f * (floor_now - wf_floor_db) : floor_now;
    wf_floor_set    = true;
    for (uint32_t i = 0; i < WIDTH; i++) wf_row[i] -= wf_floor_db;

    int level = atomic_load(&wf_avg_level);
    if (atomic_exchange(&wf_avg_restart, false) || level <= 0) wf_avg_set = false;
    if (level > 0 && level < WF_AVG_LEVELS) {
        float a = wf_avg_new[level];
        if (!wf_avg_set) memcpy(wf_avg, wf_row, sizeof(wf_avg));
        else
            for (uint32_t i = 0; i < WIDTH; i++) wf_avg[i] += a * (wf_row[i] - wf_avg[i]);
        wf_avg_set = true;
        memcpy(wf_row, wf_avg, sizeof(wf_row));
    }

    /* Into the ring for wf_timer_cb; far behind, the oldest goes. */
    pthread_mutex_lock(&wf_lock);
    if (wf_q_count == WF_QUEUE) {
        wf_q_head = (wf_q_head + 1) % WF_QUEUE;
        wf_q_count--;
        wf_dropped++;
    }
    memcpy(wf_rows[(wf_q_head + wf_q_count) % WF_QUEUE], wf_row, sizeof(wf_row));
    wf_q_count++;
    pthread_mutex_unlock(&wf_lock);
}

static void on_audio(const float *samples, unsigned n, void *ctx) {
    (void)ctx;
    if (!sg || !psd) return; /* rx_start couldn't get them: no waterfall rows */

    /* One row per WF_ROW_SAMPLES of audio, however the audio is chunked.
     * Exact zeros are the beep guard's silence (real audio never is): the
     * waterfall pauses over them instead of drawing a dark band. */
    while (n) {
        unsigned take = WF_ROW_SAMPLES - wf_row_fill;
        if (take > n) take = n;
        for (unsigned i = 0; i < take; i++) {
            if (samples[i] == 0.0f) {
                take = i;
                break;
            }
        }
        if (!take) { /* a run of guard silence: skip it */
            while (n && *samples == 0.0f) samples++, n--;
            continue;
        }
        spgramf_write(sg, (float *)samples, take);
        samples += take;
        n -= take;
        wf_row_fill += take;
        if (wf_row_fill >= WF_ROW_SAMPLES) {
            wf_row_fill = 0;
            wf_emit_row();
        }
    }
}

/* ---- Receiver lifecycle ----------------------------------------------- */

/* The decoder setup's notice: a yellow banner over the waterfall strip
 * (mock-up C in docs/mockups/fftw-wisdom/, VE7NHW's pick), counting the
 * speeds measured. Over the waterfall and the status line, under the
 * popups made later. */
static void setup_notice_update(void) {
    unsigned done = 0;
    js8_decoder_setup_running(&done);
    lv_label_set_text_fmt(setup_title, "Setting up the decoder (one time only)  %u/%u", done,
                          js8_decoder_setup_speeds());
}

static void setup_end(void) {
    if (setup_timer) lv_timer_del(setup_timer);
    setup_timer = NULL;
    if (setup_box) lv_obj_del(setup_box);
    setup_box = setup_title = NULL;
    decoder_setup = false;
}

static void setup_timer_cb(lv_timer_t *t) {
    (void)t;
    if (js8_decoder_setup_running(NULL)) {
        setup_notice_update();
        return;
    }
    setup_end();
    js8_rx_set_submodes(rx, rx_speed_mask());
    if (js8_decoder_setup_seconds() > 0)
        js8_log("decoder setup: done in %.1f s, saved to %s; decoding", js8_decoder_setup_seconds(), JS8_WISDOM_PATH);
}

static void setup_notice_show(void) {
    setup_box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(setup_box);
    lv_obj_clear_flag(setup_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(setup_box, WIDTH, WF_VISIBLE);
    lv_obj_set_pos(setup_box, INSET, INSET);
    lv_obj_set_style_bg_color(setup_box, lv_color_hex(0xFFD000), 0);
    lv_obj_set_style_bg_opa(setup_box, LV_OPA_COVER, 0);

    setup_title = lv_label_create(setup_box);
    lv_obj_set_style_text_font(setup_title, &sony_28, 0);
    lv_obj_set_style_text_color(setup_title, lv_color_black(), 0);
    setup_notice_update();
    lv_obj_center(setup_title);
    setup_timer = lv_timer_create(setup_timer_cb, 250, NULL);
}

static void rx_start(void) {
    /* At least a bin per pixel, rounded up to a power of two: FFTW is
     * several times faster at 4096 than at 3035 (= 5 x 607). */
    int      span = filter_high - filter_low;
    unsigned want = span > 0 ? WIDTH * SAMPLE_RATE / span : 4096;
    nfft          = 256;
    while (nfft < want && nfft < 8192) nfft *= 2;
    /* Step at most half a row, so every row gets at least one transform. */
    unsigned step = nfft / 2 < WF_ROW_SAMPLES / 2 ? nfft / 2 : WF_ROW_SAMPLES / 2;
    sg           = spgramf_create(nfft, LIQUID_WINDOW_HANN, nfft, step);
    wf_floor_set = false;
    wf_avg_set   = false;
    wf_row_fill  = 0;
    psd      = malloc(nfft * sizeof(float));

    js8_rx_cb_t cb = {
        .on_message    = on_message,
        .on_cycle_done = on_cycle_done,
        .on_audio      = on_audio,
        .on_mark       = on_mark,
        .on_report     = on_report,
        .on_auto_drift = on_auto_drift,
        .on_search     = on_search,
    };
    marks_reset();
    /* No js8_wisdom.dat yet (a new build's first open): every speed's FFT
     * plans are measured first, on a thread of their own, and nothing is
     * decoded meanwhile; the waterfall runs. Closing doesn't wait for it. */
    bool still    = js8_decoder_setup_running(NULL); /* from an open closed during it */
    decoder_setup = !js8_decoder_setup_begin(JS8_WISDOM_PATH);
    if (decoder_setup && still) js8_log("decoder setup: still measuring; decoding after");
    else if (decoder_setup)
        js8_log("decoder setup: measuring the FFT plans (%s missing or unusable); decoding after", JS8_WISDOM_PATH);
    rx = js8_rx_create(SAMPLE_RATE, rx_decode_mask(), my_call(), &cb);
    if (!rx) msg_schedule_text_fmt("JS8: cannot start decoder");
    js8_rx_set_decode_range(rx, filter_low, filter_high);
    js8_rx_set_qso_offset(rx, param_i_get(cfg.js8.tx_freq()));
    js8_rx_set_sync_marks(rx, param_i_get(cfg.js8.decode_marks()));
    js8_rx_set_auto_sync(rx, param_i_get(cfg.js8.tsync_auto()));
    if (decoder_setup) setup_notice_show();
}

static void rx_stop(void) {
    /* Closed during the decoder setup: no waiting for it; it carries on
     * behind and saves its file. */
    if (decoder_setup) js8_log("decoder setup: JS8 closed, the setup finishes on its own");
    setup_end();
    /* Joins the receiver's threads: no callback runs after this, so the
     * spgram below is no longer in use. */
    js8_rx_destroy(rx);
    rx = NULL;
    js8_decoder_setup_save(JS8_WISDOM_PATH); /* anything new measured (as desktop at exit) */

    if (sg) {
        spgramf_destroy(sg);
        sg = NULL;
    }
    free(psd);
    psd = NULL;
}

static float beep_level_before_db = -200.0f; /* receiver level, smoothed (beep log) */

static bool beep_guard_active(void) {
    return atomic_load(&beep_guard) || now_mono_ms() < atomic_load(&beep_guard_end);
}

static void audio_cb(size_t n, float *samples) {
    if (atomic_load(&keyed)) return; /* our own TX, or nothing useful */
    if (beep_guard_active()) {
        static const float silence[512];
        double             sq = 0;
        for (unsigned i = 0; i < n; i++) sq += (double)samples[i] * samples[i];
        atomic_fetch_add(&beep_guard_sq, (int64_t)(sq * 1e9));
        atomic_fetch_add(&beep_guard_n, n);
        for (unsigned at = 0; at < n; at += 512) js8_rx_feed(rx, silence, n - at < 512 ? n - at : 512);
        return;
    }
    if (n) {
        double sq = 0;
        for (unsigned i = 0; i < n; i++) sq += (double)samples[i] * samples[i];
        float db             = (float)(10.0 * log10(sq / n + 1e-24));
        beep_level_before_db = beep_level_before_db < -150.0f ? db : 0.9f * beep_level_before_db + 0.1f * db;
    }
    js8_rx_feed(rx, samples, (unsigned)n);
}

/* ---- Transmit ---------------------------------------------------------- */

/* TX thread. Reading `tx` here is safe: tx_stop_all() clears it only after
 * js8_tx_destroy() has stopped and joined this thread. */
static bool tx_abort_check(void *ctx) {
    (void)ctx;
    return js8_tx_stopping(tx);
}

/* TX thread: key the radio for one frame. tx_player shifts the VFO so the
 * 1325 Hz synthesis tone lands at our offset, drops PTT and restores the VFO
 * afterwards. */
static bool tx_play(int16_t *samples, unsigned n, int index, int count, void *ctx) {
    (void)index;
    (void)count;
    (void)ctx;
    atomic_store(&keyed, true);
    struct timespec until; /* a beep stops within one part; never hang TX on it */
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    if (pthread_mutex_timedlock(&speaker_lock, &until) == 0) pthread_mutex_unlock(&speaker_lock);
    else radio_speaker_play(false); /* never key with the speaker path switched */
    /* Per frame, so a power change made while the app is open counts. */
    base_gain_offset = tx_player_base_gain_offset();
    bool done = tx_player_play(samples, n, atomic_load(&tx_offset_active), base_gain_offset, tx_abort_check, NULL);
    atomic_store(&keyed, false);
    return done;
}

static int current_utc_hhmmss(void) {
    time_t    now = (time_t)(now_wall_ms() / 1000);
    struct tm tm;
    gmtime_r(&now, &tm);
    return tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec;
}

/* The row of the message going out, if it's still in the history. */
static bool tx_row_ok(void) {
    return tx_row_slot >= 0 && hist_count && history[tx_row_slot].tx && hist_seq[tx_row_slot] == tx_row_seq;
}

/* A row of yours changed its mark: in the list now, if it shows there (the
 * Stations view rebuilds the list when you go back). */
static void tx_row_refresh(int slot) {
    if (view_stations || !table) return;
    char buf[JS8_RX_TEXT_LEN + 48];
    format_row(slot, buf, sizeof(buf));
    for (uint16_t r = 0; r < rows; r++) {
        if (row_hist[r] == slot) {
            bool scroll = at_bottom();
            lv_table_set_cell_value(table, r, 0, buf);
            if (scroll) follow();
            return;
        }
    }
}

/* The message going out ended: the end mark, or how far it got. */
static void tx_row_end(bool completed) {
    if (tx_row_ok()) {
        hist_tx[tx_row_slot].state = completed ? TX_ROW_SENT : TX_ROW_STOPPED;
        tx_row_refresh(tx_row_slot);
    }
    tx_row_slot = -1;
}

/* JS8 closed while one went out (it stops then): no " ..." left behind. */
static void tx_rows_stopped(void) {
    for (int i = 0; i < HISTORY; i++)
        if (hist_tx[i].state == TX_ROW_SENDING) hist_tx[i].state = TX_ROW_STOPPED;
    tx_row_slot = -1;
}

static void ui_tx_status(void *arg) {
    if (!dialog.run || !tx_bar) return;
    const js8_tx_status_t *st = (const js8_tx_status_t *)arg;

    /* When the first frame goes out, add the message to the list. */
    if (st->state == JS8_TX_KEYING && st->frame == 1 && tx_status.state != JS8_TX_KEYING) {
        js8_rx_msg_t m = {0};
        m.tx           = true;
        m.utc          = current_utc_hhmmss();
        m.freq_hz      = st->offset_hz;
        m.submode      = (uint8_t)js8_speed_submode(st->speed);
        snprintf(m.text, sizeof(m.text), "%s", tx_preview[0] ? tx_preview : st->text);
        add_message(&m);
        tx_row_slot = (hist_head - 1 + HISTORY) % HISTORY;
        tx_row_seq  = hist_seq[tx_row_slot];
        /* AGN? repeats what went out, as desktop: not a message stopped
         * before it keyed. */
        snprintf(last_tx_text, sizeof(last_tx_text), "%s", st->text);
        /* The history keeps automatic ones too: an HB ACK is an exchange. */
        js8_history_tx(history_db, m.text, my_call(), (uint64_t)cparam_i_get(cfg.cur.fg_freq()), st->offset_hz,
                       m.submode, tx_auto, sel_call, stations, now_wall_ms());
        char        to[JS8_RX_CALL_LEN];
        const char *body = strstr(m.text, ": "); /* "MYCALL: W1ABC ..." */
        if (st_all_time && starts_with_call(body ? body + 2 : m.text, to, sizeof(to))) {
            ever_merge(to);
            if (view_stations) rebuild_station_rows();
        }
        /* Only what you send is your side of a QSO: an unattended station
         * answering SNR? and hearing "TNX 73" hasn't had one. */
        char ended[JS8_RX_CALL_LEN];
        if (!tx_auto && js8_qsos_sent(qsos, m.text, my_call(), now_wall_ms(), ended, sizeof(ended)))
            log_offer(ended);
    }
    if (st->frame > 0 && tx_row_ok()) { /* how far it got, for "(stopped 2/5)" */
        hist_tx[tx_row_slot].frame  = (uint16_t)st->frame;
        hist_tx[tx_row_slot].frames = (uint16_t)st->frames;
    }
    tx_status = *st;
    update_tx_bar();
}

static void on_tx_status(const js8_tx_status_t *st, void *ctx) {
    (void)ctx;
    ui_event_t e = {.kind = EV_TX_STATUS, .tx_status = *st};
    ev_push(&e);
}

static void ui_tx_done(void *arg) {
    if (!dialog.run || !tx_bar) return;
    const tx_done_t *done      = arg;
    bool             completed = done->completed;
    if (!completed) add_info_row("TX stopped");
    tx_row_end(completed);
    deliver_end(done->text, completed);
    memset(&tx_status, 0, sizeof(tx_status));
    tx_active = false;
    finder_sync();
    update_tx_bar();
    /* Auto CQ counts its minutes from the end of our CQ (your choice);
     * replies and heartbeats in between don't move it. */
    if (auto_cq && tx_cq) {
        auto_cq_from_ms = now_wall_ms();
        auto_cq_next_ms = auto_cq_from_ms + cq_interval_ms();
        if (btn_cq.disp_btn) buttons_refresh(&btn_cq);
    }
    if (tx_cq && completed) my_cq_until_ms = now_mono_ms() + CQ_HEARD_MS; /* as others' CQs on the map */
    tx_cq = false;
    time_apply_found(); /* a time search's find, held while we sent */

    /* Replies that arrived while we were sending. */
    auto_try_send();
}

static void on_tx_done(const char *text, bool completed, void *ctx) {
    (void)ctx;
    ui_event_t e = {.kind = EV_TX_DONE, .tx_done = {.completed = completed}};
    snprintf(e.tx_done.text, sizeof(e.tx_done.text), "%s", text ? text : "");
    ev_push(&e);
}

static void tx_start(void) {
    js8_tx_cb_t cb = {
        .play      = tx_play,
        .on_status = on_tx_status,
        .on_done   = on_tx_done,
    };
    memset(&tx_status, 0, sizeof(tx_status));
    atomic_store(&keyed, false);
    tx = js8_tx_create(AUDIO_PLAY_RATE, TX_PLAYER_AUDIO_HZ, &cb);
    if (!tx) msg_schedule_text_fmt("JS8: cannot start transmitter");
}

static void tx_stop_all(void) {
    /* Stops the current frame (tx_player drops PTT within one part) and
     * waits for the TX thread; only then is `tx` cleared. */
    js8_tx_destroy(tx);
    tx = NULL;
    atomic_store(&keyed, false);
}

static void tx_timer_cb(lv_timer_t *t) {
    (void)t;
    static unsigned ticks;
    swr_guard_tick();
    hclear_tick();
    update_tx_bar();
    /* Keep the status clock moving; decode cycles, which also refresh it,
     * pause while we transmit. */
    if (++ticks % 4 == 0) {
        hb_tick();
        auto_cq_tick();
        update_status();
    }
    if (view_stations && ticks % 20 == 0) rebuild_station_rows(); /* ages */
    if (view_map) map_update(false); /* pop-ups flash and end; redrawn only if something changed */
}

/* The TX bar and the waterfall frame as last set. Every LVGL style or
 * text call redraws, changed or not, and the frame's redraw spills onto the
 * dialog background behind the waterfall: set only what changed (I-14). */
static struct {
    bool     fresh; /* just created: set everything */
    uint32_t bg;
    bool     recolor;
    char     text[JS8_RX_TEXT_LEN + 64];
    bool     frame;
} tx_bar_shown;

static void tx_bar_set(uint32_t bg, bool recolor, const char *text) {
    bool fresh = tx_bar_shown.fresh;
    if (fresh || tx_bar_shown.bg != bg) {
        lv_obj_set_style_bg_color(tx_bar, lv_color_hex(bg), 0);
        /* Waiting and sending stay bold enough to catch the eye; idle is
         * as see-through as the list's rows. */
        lv_obj_set_style_bg_opa(tx_bar, bg == TX_BAR_IDLE ? LV_OPA_50 : LV_OPA_70, 0);
    }
    if (fresh || tx_bar_shown.recolor != recolor) lv_label_set_recolor(tx_bar, recolor);
    if (fresh || strcmp(tx_bar_shown.text, text) != 0) lv_label_set_text(tx_bar, text);
    tx_bar_shown.bg      = bg;
    tx_bar_shown.recolor = recolor;
    snprintf(tx_bar_shown.text, sizeof(tx_bar_shown.text), "%s", text);
    if (fresh) { /* the frame too: its colour never changes */
        lv_obj_set_style_border_color(wf_box, lv_color_hex(0xff2020), 0);
        lv_obj_set_style_border_width(wf_box, 0, 0);
        tx_bar_shown.frame = false;
        tx_bar_shown.fresh = false;
    }
}

/* A red frame round the waterfall while keyed. */
static void wf_frame_set(bool on) {
    if (tx_bar_shown.frame == on) return;
    lv_obj_set_style_border_width(wf_box, on ? 3 : 0, 0);
    tx_bar_shown.frame = on;
}

/* "TX 1500 Hz  ready" / "... K2XYZ SNR?  starts in 9 s" / "... sending 2/3" */
static void update_tx_bar(void) {
    if (!tx_bar) return;
    char     line[JS8_RX_TEXT_LEN + 64];
    uint16_t offset = param_i_get(cfg.js8.tx_freq());

    if (cq_adjusting && tx_status.state == JS8_TX_IDLE) {
        snprintf(line, sizeof(line), "Auto CQ %u min after each CQ: turn the knob (%d-%d), press CQ when done",
                 param_i_get(cfg.js8.cq_interval()), CQ_MIN_INTERVAL, CQ_MAX_INTERVAL);
        tx_bar_set(0x5a4a00, false, line);
        return;
    }
    if (hb_adjusting && tx_status.state == JS8_TX_IDLE) {
        snprintf(line, sizeof(line), "Auto HB every %u min: turn the knob (5-30), press Heartbeat when done",
                 param_i_get(cfg.js8.hb_interval()));
        tx_bar_set(0x5a4a00, false, line);
        return;
    }

    uint32_t bg;

    switch (tx_status.state) {
    case JS8_TX_WAITING: {
        int64_t now_ms = now_wall_ms(); /* the transmitter's slots are in JS8 time */
        int     secs   = (int)((tx_status.next_ms - now_ms + 999) / 1000);
        if (secs < 0) secs = 0;
        /* Progress before the text: a long message only loses its end. */
        snprintf(line, sizeof(line), "TX %4.0f Hz %s  %d/%d %s %d s   %s", tx_status.offset_hz,
                 js8_speed_name(tx_status.speed), tx_status.frame, tx_status.frames,
                 tx_status.frame == 1 ? "starts in" : "next in", secs, tx_status.text);
        bg = 0x5a4a00;
        break;
    }
    case JS8_TX_KEYING:
        snprintf(line, sizeof(line), "TX %4.0f Hz %s  sending %d/%d   %s", tx_status.offset_hz,
                 js8_speed_name(tx_status.speed), tx_status.frame, tx_status.frames, tx_status.text);
        bg = 0xa00000;
        break;
    default:
        /* A locked station in red (recolor only here: sent text may hold '#'). */
        snprintf(line, sizeof(line), "TX %4u Hz %s   ready%s%s", offset, js8_speed_name(cur_speed()),
                 my_call()[0] ? "" : "  (set your callsign: APP > Callsign)",
                 !sel_call[0] ? "" : sel_locked ? "      #ff5050 locked: " : "      selected: ");
        if (sel_call[0]) strncat(line, sel_call, sizeof(line) - strlen(line) - 1);
        if (sel_call[0] && sel_locked) strncat(line, "#", sizeof(line) - strlen(line) - 1);
        if (auto_cq) strncat(line, "      auto CQ", sizeof(line) - strlen(line) - 1);
        bg = TX_BAR_IDLE;
        break;
    }
    tx_bar_set(bg, tx_status.state != JS8_TX_WAITING && tx_status.state != JS8_TX_KEYING && sel_locked, line);
    wf_frame_set(tx_status.state == JS8_TX_KEYING);
}

/* Queue `text` at our offset. Returns false (with a message shown) if it
 * can't be sent, e.g. a bad character or something already sending. */
static void hb_pause(const char *why);

/* "N0XYZ ..." with a real-looking call first: a directed message. */
static bool starts_with_call(const char *text, char *call, size_t len) {
    size_t n = strcspn(text, " ");
    if (n < 3 || n >= len || text[0] == '@' || strncmp(text, "CQ", 2) == 0) return false;
    bool letter = false, digit = false;
    for (size_t i = 0; i < n; i++) {
        if (text[i] >= 'A' && text[i] <= 'Z') letter = true;
        else if (text[i] >= '0' && text[i] <= '9') digit = true;
        else if (text[i] != '/') return false;
    }
    if (!letter || !digit) return false;
    memcpy(call, text, n);
    call[n] = '\0';
    return true;
}

static bool tx_queue_at(const char *text, int offset_hz, bool automatic) {
    if (!tx) { /* js8_tx_create() failed when the app opened */
        msg_update_text_fmt("JS8: transmitter not running - close JS8 and open it again");
        return false;
    }
    if (tx_active || js8_tx_busy(tx)) {
        msg_update_text_fmt("Already sending - Stop TX first");
        return false;
    }

    js8_tx_preview_t pv;
    js8_tx_preview(my_call(), my_grid(), text, cur_speed(), &pv);
    if (!pv.ok) {
        msg_update_text_fmt("JS8: %s", pv.error);
        return false;
    }

    char err[JS8_TX_ERR_LEN] = "";
    atomic_store(&tx_offset_active, offset_hz);
    snprintf(tx_preview, sizeof(tx_preview), "%s", pv.preview);
    if (!js8_tx_send(tx, my_call(), my_grid(), text, (float)offset_hz, cur_speed(), err, sizeof(err))) {
        msg_update_text_fmt("JS8: %s", err);
        return false;
    }
    tx_active   = true;
    tx_auto     = automatic;
    tx_cq       = false;
    tx_quiet_ms = 0;
    finder_sync();
    msg_update_text_fmt("%sQueued: %d frame%s, %.0f s", automatic ? "Auto: " : "", pv.frames,
                        pv.frames == 1 ? "" : "s", pv.seconds);

    /* A directed message you sent yourself starts a QSO. */
    char call[JS8_RX_CALL_LEN];
    if (!automatic && starts_with_call(text, call, sizeof(call))) {
        auto_cq_stop("replying");
        map_qrz_clear(call);
    }
    /* Anything you send by hand pauses heartbeats, except a heartbeat or a
     * CQ: they carry on while you call CQ, auto CQ too (the user's choice). */
    if (!automatic && strncmp(text, "CQ ", 3) != 0) {
        char hb[48];
        js8_heartbeat_text(my_call(), my_grid(), hb, sizeof(hb));
        if (strcmp(text, hb) != 0) hb_pause("you sent");
    }
    return true;
}

/* At our TX offset (the red band), sent by you. */
static bool tx_queue(const char *text) {
    return tx_queue_at(text, param_i_get(cfg.js8.tx_freq()), false);
}

/* Hold off: answer on the other station's offset. Hold on (default): stay
 * on ours, which is JS8 etiquette. */
static void apply_hold(float their_freq) {
    if (param_i_get(cfg.js8.hold_offset())) return;
    if (their_freq <= 0) return; /* from the history, not heard now: no offset to go to */
    int f = (int)(their_freq + 0.5f);
    if (f < JS8_TX_MIN_OFFSET) f = JS8_TX_MIN_OFFSET;
    if (f > js8_speed_max_offset_hz(cur_speed())) f = js8_speed_max_offset_hz(cur_speed());
    param_i_set(cfg.js8.tx_freq(), (uint16_t)f);
    js8_rx_set_qso_offset(rx, f);
    finder_sync();
    update_tx_bar();
}

/* Main tuning knob: move the TX offset, as in the FT8 app. The dial
 * frequency stays locked. */
/* Just the strip the finder's band covers at offset `hz` (as lv_finder
 * draws it, its border included), not the whole finder: that is the
 * waterfall's size, and on R1CBU 1.0's overlay every redrawn pixel is
 * rotated too, so the knob made the waterfall lag. */
static void finder_invalidate_band(int32_t hz) {
    if (!finder || !lv_obj_is_visible(finder)) return;
    lv_coord_t w    = lv_obj_get_width(finder);
    int32_t    span = filter_high - filter_low;
    if (span <= 0) return;
    lv_area_t a = finder->coords;
    a.x1 = finder->coords.x1 + (lv_coord_t)((int64_t)w * (hz - filter_low) / span) - 2;
    a.x2 = finder->coords.x1 + (lv_coord_t)((int64_t)w * (hz + js8_speed_bandwidth_hz(cur_speed()) - filter_low) / span) + 2;
    if (!_lv_area_intersect(&a, &a, &finder->coords)) return;
    _lv_inv_area(lv_obj_get_disp(finder), &a);
}

/* The red band: your TX offset, or while a message on another offset is
 * queued or going out (heartbeats and HB ACKs pick a free spot at 500-999
 * Hz, as desktop does), that one, back when it ends or is stopped. */
static void finder_sync(void) {
    if (!finder) return;
    int32_t hz = tx_active ? atomic_load(&tx_offset_active) : (int32_t)param_i_get(cfg.js8.tx_freq());
    if (hz == finder_shown) return;
    lv_finder_set_value(finder, (int16_t)hz);
    if (finder_shown >= 0) finder_invalidate_band(finder_shown);
    finder_invalidate_band(hz);
    finder_shown = hz;
    cursor_place(); /* hidden on the red band's offset */
}

static void rotary_cb(int32_t diff, uint16_t dt) {
    user_touch();
    if (cq_adjusting) {
        cq_adjust_turn(diff);
        return;
    }
    if (hb_adjusting) {
        int v = (int)param_i_get(cfg.js8.hb_interval()) + (diff > 0 ? 1 : -1);
        if (v < JS8_HB_MIN_INTERVAL) v = JS8_HB_MIN_INTERVAL;
        if (v > JS8_HB_MAX_INTERVAL) v = JS8_HB_MAX_INTERVAL;
        param_i_set(cfg.js8.hb_interval(), (uint16_t)v);
        hb_adjust_ms = now_wall_ms();
        if (btn_hb.disp_btn) buttons_refresh(&btn_hb);
        if (param_i_get(cfg.js8.hb()) && hb_next_ms) hb_next_ms = hb_first_ms();
        update_tx_bar();
        update_status();
        return;
    }
    /* Faster turns move further: 4-5 clicks within 30 ms move 5 Hz each,
     * 6 or more 10 Hz. R1CBU 0.34 read the knob every 30 ms and handed us
     * the sum; 1.0 hands over every click on its own (its own dial speeds
     * up by the time between clicks, which apps don't get), so count them
     * per 30 ms here. */
    static int64_t window_ms;
    static int32_t window_clicks;
    int64_t        now_ms = now_mono_ms();
    if (now_ms - window_ms >= 30) {
        window_ms     = now_ms;
        window_clicks = 0;
    }
    window_clicks += abs(diff);
    if (window_clicks > 3) diff *= (window_clicks < 6) ? 5 : 10;

    int32_t old = (int32_t)param_i_get(cfg.js8.tx_freq());
    int32_t f   = old + diff;
    if (f < JS8_TX_MIN_OFFSET) f = JS8_TX_MIN_OFFSET;
    if (f > js8_speed_max_offset_hz(cur_speed())) f = js8_speed_max_offset_hz(cur_speed());
    if (f == old) return;
    param_i_set(cfg.js8.tx_freq(), (uint16_t)f);
    js8_rx_set_qso_offset(rx, f);

    finder_sync(); /* stays on a heartbeat's offset while that is on */
    update_tx_bar();
}

/* ---- Compose window --------------------------------------------------- */

/* The frame count plans the whole message, which on the radio takes a
 * few tenths of a second once it is past ~8 frames: done on every key,
 * fast typing piled the keys up and froze the screen for 20 s. So it is
 * counted once typing pauses, as desktop does (m_txTextDirtyDebounce,
 * 100 ms there; longer here for the slower processor), and on a thread
 * of its own: near 20 frames a plan takes a second or two on the radio,
 * and on the GUI thread the waterfall stopped for it (VE7NHW,
 * 2026-10-13). The newest request wins; the thread stays for the
 * program's life, idle between counts. */
#define COMPOSE_COUNT_MS      350
#define COMPOSE_COUNT_POLL_MS 50
static lv_timer_t *compose_count_timer; /* the pause */
static lv_timer_t *compose_poll_timer;  /* waits for the count */

static struct {
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    bool             started;
    unsigned         want; /* the newest request */
    unsigned         done; /* the request `pv` answers */
    char             call[32], grid[16], text[TX_TEXT_MAX * 2];
    js8_speed_t      speed;
    bool             blank; /* nothing typed: no "too long" either */
    js8_tx_preview_t pv;
} counter = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *count_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "js8-count");
    /* Behind the decoders (nice 0) on the cores they share, and off the
     * last core, the GUI's, as they are (cpu_fence.cpp): a long count
     * waits rather than delaying a decode or the waterfall. */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu >= 3 && ncpu <= CPU_SETSIZE) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (long c = 0; c < ncpu - 1; c++) CPU_SET(c, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    static char      call[sizeof(counter.call)], grid[sizeof(counter.grid)], text[sizeof(counter.text)];
    js8_tx_preview_t pv;
    pthread_mutex_lock(&counter.mu);
    for (;;) {
        while (counter.done == counter.want) pthread_cond_wait(&counter.cv, &counter.mu);
        unsigned    n     = counter.want;
        js8_speed_t speed = counter.speed;
        memcpy(call, counter.call, sizeof(call));
        memcpy(grid, counter.grid, sizeof(grid));
        memcpy(text, counter.text, sizeof(text));
        pthread_mutex_unlock(&counter.mu);
        js8_tx_preview(call, grid, text, speed, &pv);
        pthread_mutex_lock(&counter.mu);
        if (n == counter.want) counter.pv = pv; /* else typed on meanwhile: count again */
        counter.done = n;
    }
    return NULL;
}

static void compose_count_cancel(void) {
    if (compose_count_timer) {
        lv_timer_del(compose_count_timer);
        compose_count_timer = NULL;
    }
    if (compose_poll_timer) { /* a count still running is just never shown */
        lv_timer_del(compose_poll_timer);
        compose_poll_timer = NULL;
    }
}

static void compose_poll_cb(lv_timer_t *t) {
    (void)t;
    js8_tx_preview_t pv;
    bool             blank;
    pthread_mutex_lock(&counter.mu);
    bool ready = counter.done == counter.want;
    if (ready) pv = counter.pv, blank = counter.blank;
    pthread_mutex_unlock(&counter.mu);
    if (!ready) return;
    lv_timer_del(compose_poll_timer);
    compose_poll_timer = NULL;
    if (!composing) return;
    if (pv.ok) msg_update_text_fmt("%d frame%s, %.0f s", pv.frames, pv.frames == 1 ? "" : "s", pv.seconds);
    /* Why it can't go ("too long: 21 frames (max 20)"), as you type, not
     * the last good count frozen until Enter (B-23). */
    else if (!blank) msg_update_text_fmt("JS8: %s", pv.error);
}

static void compose_count_cb(lv_timer_t *t) {
    (void)t;
    compose_count_timer = NULL; /* a one-shot: LVGL deletes it after this */
    if (!composing) return;
    char typed[TX_TEXT_MAX * 2];
    macros_fill(textarea_window_get(), false, typed, sizeof(typed)); /* counted as it goes out */
    pthread_mutex_lock(&counter.mu);
    snprintf(counter.call, sizeof(counter.call), "%s", my_call());
    snprintf(counter.grid, sizeof(counter.grid), "%s", my_grid());
    snprintf(counter.text, sizeof(counter.text), "%s", typed);
    counter.speed = cur_speed();
    counter.blank = !typed[strspn(typed, " ")];
    counter.want++;
    if (!counter.started) {
        pthread_t th;
        if (pthread_create(&th, NULL, count_thread, NULL) == 0) {
            pthread_detach(th);
            counter.started = true;
        }
    }
    pthread_cond_signal(&counter.cv);
    bool started = counter.started;
    pthread_mutex_unlock(&counter.mu);
    if (!started) return; /* no thread: no count, typing still works */
    if (!compose_poll_timer) compose_poll_timer = lv_timer_create(compose_poll_cb, COMPOSE_COUNT_POLL_MS, NULL);
}

static void compose_changed_cb(lv_event_t *e) {
    (void)e;
    if (compose_count_timer) {
        lv_timer_reset(compose_count_timer);
        return;
    }
    compose_count_timer = lv_timer_create(compose_count_cb, COMPOSE_COUNT_MS, NULL);
    lv_timer_set_repeat_count(compose_count_timer, 1);
}

/* JS8 is capitals only: take lowercase (a USB keyboard without Caps Lock,
 * the on-screen "abc" page) as capitals instead of dropping it. */
static void compose_insert_cb(lv_event_t *e) {
    const char *in = lv_event_get_param(e);
    static char up[8];
    size_t      n = strlen(in);
    if (n >= sizeof(up)) return;
    bool lower = false;
    for (size_t i = 0; i <= n; i++) {
        up[i] = in[i] >= 'a' && in[i] <= 'z' ? (char)(in[i] - 'a' + 'A') : in[i];
        lower |= up[i] != in[i];
    }
    if (lower) lv_textarea_set_insert_replace(lv_event_get_target(e), up);
}

/* While typing, the text box goes to the top of the screen and the list
 * takes the space the on-screen keyboard leaves (over the waterfall), so
 * the other station's words stay readable as they arrive. */
static void compose_layout(bool typing) {
    if (!table) return;
    bool       kb_shown = typing && !keyboard_ready();
    lv_area_t  d;
    lv_obj_get_coords(dialog.obj, &d);
    lv_coord_t kb_top = lv_obj_get_height(lv_scr_act()) / 2; /* lv_keyboard: the bottom half */
    if (kb_shown) {
        lv_obj_set_pos(table, INSET, INSET);
        lv_obj_set_size(table, WIDTH, kb_top - d.y1 - INSET - 2);
        lv_obj_add_flag(tx_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(table, INSET, INSET + WF_VISIBLE + TX_BAR_H);
        lv_obj_set_size(table, WIDTH, WF_HEIGHT - WF_VISIBLE - TX_BAR_H);
        lv_obj_clear_flag(tx_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (at_bottom()) follow();
}

static void compose_close(void) {
    if (!composing) return;
    compose_count_cancel();
    textarea_window_close();
    composing = false;
    compose_layout(false);
    if (table) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

/* Same pattern as the FT8 app's keyboard: close here and return true;
 * textarea_window's own close is then a no-op. */
static bool compose_ok_cb(void) {
    if (edit_target == EDIT_FREQ && !parse_custom(textarea_window_get(), NULL)) return false;
    if (edit_target == EDIT_SPOT_FREQ && !spot_freq_empty(textarea_window_get()) &&
        !spot_parse_freq(textarea_window_get(), NULL))
        return false;
    /* A typed log grid must be one (or nothing), not "HOME". */
    if (edit_target == EDIT_LOG_GRID && textarea_window_get()[strspn(textarea_window_get(), " ")] &&
        !js8_is_grid(textarea_window_get())) {
        msg_update_text_fmt("Not a grid: 4 or 6 characters, e.g. CN89 or CN89KG");
        return false;
    }
    if (edit_target >= EDIT_LOG_GRID) {
        log_edit_done(textarea_window_get());
        return true;
    }
    if (edit_target == EDIT_OPERATOR) {
        const char *typed = textarea_window_get();
        while (*typed == ' ') typed++;
        if (!*typed) {
            operator_call[0] = '\0';
            msg_update_text_fmt("Operator: the station call (%s)", my_call());
        } else if (js8_operator_call_valid(typed, operator_call, sizeof(operator_call))) {
            msg_update_text_fmt("Operator %s: logged as OPERATOR; %s is still sent on the air", operator_call,
                                my_call());
        } else {
            msg_update_text_fmt("Not a callsign: letters, digits and /, e.g. VA7XYZ");
            return false; /* keep the keyboard open */
        }
        save_texts();
        edit_target = 0;
        compose_close();
        return true;
    }
    if (edit_target == EDIT_SAVED) {
        const char *typed = textarea_window_get();
        typed += strspn(typed, " ");
        snprintf(saved_msgs[saved_edit], sizeof(saved_msgs[saved_edit]), "%s", typed);
        for (char *end = saved_msgs[saved_edit] + strlen(saved_msgs[saved_edit]);
             end > saved_msgs[saved_edit] && end[-1] == ' ';)
            *--end = '\0';
        save_saved();
        edit_target = 0;
        compose_close();
        saved_open(saved_edit); /* back to the list, on that one */
        msg_update_text_fmt(saved_msgs[saved_edit][0] ? "Saved message %d saved" : "Saved message %d cleared",
                            saved_edit + 1);
        return true;
    }
    if (edit_target == EDIT_GROUPS) {
        js8_groups_normalise(textarea_window_get(), groups_text, sizeof(groups_text));
        save_texts();
        if (groups_text[0]) msg_update_text_fmt("Groups: %s", groups_text);
        else msg_update_text_fmt("No groups");
        edit_target = 0;
        compose_close();
        return true;
    }
    if (edit_target) {
        char *dst = edit_target == 1 ? info_text : status_text;
        snprintf(dst, TEXT_MAX + 1, "%s", textarea_window_get());
        save_texts();
        msg_update_text_fmt("%s saved", edit_target == 1 ? "INFO" : "STATUS");
        edit_target = 0;
        compose_close();
        return true;
    }
    /* Macros typed in a message are filled in as it goes, as desktop
     * does; one that doesn't apply (<CALL> with nothing selected) stays. */
    char typed[TX_TEXT_MAX * 2], text[TX_TEXT_MAX + 8];
    macros_fill(textarea_window_get(), false, typed, sizeof(typed));
    if (!aprs_prepare(typed, text, sizeof(text))) return false;
    if (!tx_queue(text)) return false; /* keep the window open */
    aprs_sent_commit(); /* an APRS message with an id: its receipt is expected */
    /* The held message offered on Reply went as offered: it's on its way. */
    if (deliver_pending.id && strcasecmp(text, deliver_pending.text) == 0)
        deliver_start(deliver_pending.id, deliver_pending.group_call, text);
    deliver_pending.id = 0;
    compose_close();
    return true;
}

static bool compose_cancel_cb(void) {
    deliver_pending.id = 0;
    if (edit_target >= EDIT_LOG_GRID) {
        log_edit_done(NULL);
        return true;
    }
    if (edit_target == EDIT_SAVED) { /* unchanged, back to the list */
        edit_target = 0;
        compose_close();
        saved_open(saved_edit);
        return true;
    }
    edit_target = 0;
    compose_close();
    return true;
}

/* A list popup gives way to the keyboard or another view: its buttons out
 * of the group now (the list itself goes later: this runs in one of its
 * callbacks), and the focus not handed back to the table, or the keyboard
 * opens without it and can't be used. */
static void popup_leave(lv_obj_t **list) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(*list); i++) lv_group_remove_obj(lv_obj_get_child(*list, i));
    lv_obj_del_async(*list);
    *list = NULL;
}

/* From a list (or none) into the keyboard, editing `target` (0: a message
 * to send) with `prefill`. False if it didn't open (no callsign to send). */
static bool popup_to_keyboard(lv_obj_t **list, int target, const char *prefill) {
    if (list && *list) popup_leave(list);
    edit_target = target;
    compose_open(prefill);
    if (!composing) return false;
    lv_group_set_editing(keyboard_group, true); /* as after Reply / Send... */
    return true;
}

static void compose_open(const char *prefill) {
    if (composing) return;
    /* The callsign only for what sends (a message, an APRS beacon): a
     * frequency, alert words, INFO or a log field can be typed without one
     * (B-16). Refused, the edit mode the caller set is undone, or the next
     * Send... would open in it. */
    if (!my_call()[0] && (edit_target == 0 || edit_target >= EDIT_BEACON_GRID)) {
        msg_update_text_fmt("Set your callsign first: APP > Callsign");
        edit_target        = 0;
        deliver_pending.id = 0;
        return;
    }
    composing = true;
    lv_group_remove_obj(table);
    lv_obj_set_y(textarea_window_open(compose_ok_cb, compose_cancel_cb), 0);
    compose_layout(true);

    /* Every printable ASCII character: JS8 sends them all (as desktop,
     * whose text boxes take any), $ % < > [ ] ^ | ~ \ ` included (B-22). */
    static char printable[96];
    if (!printable[0])
        for (int c = ' '; c <= '~'; c++) printable[c - ' '] = (char)c;
    lv_obj_t *text = textarea_window_text();
    lv_textarea_set_accepted_chars(text, printable);
    lv_obj_add_event_cb(text, compose_insert_cb, LV_EVENT_INSERT, NULL);
    lv_textarea_set_max_length(text, TX_TEXT_MAX);
    lv_obj_add_event_cb(text, compose_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (edit_target) {
        lv_textarea_set_max_length(text, edit_target == EDIT_LOG_GRID   ? 6
                                         : edit_target == EDIT_POTA_REF ? sizeof(last_pota) - 1
                                         : edit_target == EDIT_SOTA_REF ? sizeof(last_sota) - 1
                                         : edit_target == EDIT_ALERT_WORDS ? sizeof(alert_words) - 1
                                         : edit_target == EDIT_GROUPS ? sizeof(groups_text) - 1
                                         : edit_target == EDIT_OPERATOR ? sizeof(operator_call) - 1
                                         : edit_target == EDIT_SAVED ? TX_TEXT_MAX
                                         : edit_target == EDIT_FREQ ? 9
                                         : edit_target == EDIT_SPOT_REF ? (spot_sota ? sizeof(last_sota) : sizeof(last_pota)) - 1
                                         : edit_target == EDIT_SPOT_FREQ ? 10
                                         : edit_target == EDIT_SPOT_NOTE ? sizeof(spot_note) - 1
                                         : edit_target >= EDIT_BEACON_GRID ? APRS_COMMENT_MAX
                                                                        : TEXT_MAX);
        if (edit_target == EDIT_FREQ || edit_target == EDIT_SPOT_FREQ)
            lv_textarea_set_accepted_chars(text, "0123456789.");
        if (edit_target != EDIT_SAVED) lv_obj_remove_event_cb(text, compose_changed_cb); /* frames: a message's only */
        if (edit_target >= EDIT_BEACON_GRID) lv_obj_add_event_cb(text, beacon_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (prefill && prefill[0]) {
        textarea_window_set(prefill);
    } else {
        static const char *const placeholders[EDIT_COUNT] = {
            [0]             = " CALL MESSAGE / @ALLCALL ...",
            [EDIT_INFO]     = " INFO, e.g. X6100 5W EFHW",
            [EDIT_STATUS]   = " STATUS, e.g. PORTABLE QRV",
            [EDIT_GROUPS]   = " Your groups, e.g. @NET @CANADA",
            [EDIT_OPERATOR] = " Operator's call - empty: the station's",
            [EDIT_SAVED]    = " Message, e.g. <CALL> TNX FER QSO 73 GL",
            [EDIT_LOG_GRID] = " Their grid, e.g. DN17",
            [EDIT_LOG_NAME] = " Their name",
            [EDIT_LOG_NOTE] = " Comment for the log",
            [EDIT_POTA_REF] = " Your park, e.g. CA-1234",
            [EDIT_SOTA_REF] = " Your summit, e.g. VE7/LM-001",
            [EDIT_ALERT_WORDS] = " Calls or words, e.g. VE7ABC @POTA SOTA",
            [EDIT_FREQ]     = " Dial frequency in kHz, e.g. 7107",
            [EDIT_SPOT_REF] = " Park CA-1234 / summit VE7/LM-001",
            [EDIT_SPOT_FREQ] = " kHz, e.g. 7185 - empty: the JS8 dial",
            [EDIT_SPOT_NOTE] = " Comment, e.g. QRT or CQ",
            [EDIT_BEACON_GRID] = " Message (optional) - Enter sends",
            [EDIT_BEACON_GPS] = " Message (optional) - Enter sends",
        };
        lv_textarea_set_placeholder_text(text, placeholders[edit_target]);
    }
}

/* ---- Band and screen -------------------------------------------------- */

/* "7107.5" from 7107500 Hz: kHz, decimals only when needed. */
static void format_khz(int32_t hz, char *buf, size_t size) {
    snprintf(buf, size, "%d.%03d", (int)(hz / 1000), (int)(hz % 1000));
    char *end = buf + strlen(buf) - 1;
    while (*end == '0') *end-- = '\0';
    if (*end == '.') *end = '\0';
}

/* What the top bar, the message line and the info rows call where we are:
 * the preset ("JS8 20m"), or just "JS8 Custom" on your own frequency (a CB
 * frequency said "JS8 10m", the nearest preset's band: VE7NHW). */
static const char *where_label(void) {
    if (!param_i_get(cfg.js8.custom_on())) return cfg_digital_label_get();
    return "JS8 Custom";
}

/* JS8 always runs in USB-D (the mode keys are locked while it's open).
 * Tuning into another band loads that band's saved mode (USB if it was
 * last used for SSB), so this follows every retune. */
static void js8_usb_dig(void) {
    if (cparam_i_get(cfg.cur.mode()) != x6100_mode_usb_dig) cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);
}

/* The presets the band keys step through: JS8Call's or GhostNet's. False
 * past either end of the list (nothing changes). */
static bool load_band(int8_t dir) {
    cfg_digital_type_t set = param_i_get(cfg.js8.ghostnet()) ? CFG_DIG_TYPE_JS8_GHOSTNET : CFG_DIG_TYPE_JS8;
    bool               ok  = cfg_digital_load(dir, set);
    js8_usb_dig(); /* even with no preset found */
    if (!ok) return false;
    msg_update_text_fmt("%s", cfg_digital_label_get());
    return true;
}

static js8_stations_t *stations_for_band(void) {
    int32_t khz = cparam_i_get(cfg.cur.fg_freq()) / 1000;
    for (int i = 0; i < BAND_LISTS; i++) {
        if (band_lists[i].list && band_lists[i].dial_khz == khz) return band_lists[i].list;
    }
    for (int i = 0; i < BAND_LISTS; i++) {
        if (!band_lists[i].list) {
            band_lists[i].dial_khz = khz;
            band_lists[i].list     = js8_stations_create();
            return band_lists[i].list;
        }
    }
    /* Many custom frequencies: reuse the slots in turn. */
    int i = band_lists_next;
    band_lists_next = (band_lists_next + 1) % BAND_LISTS;
    js8_stations_reset(band_lists[i].list); /* another frequency: nobody heard yet */
    band_lists[i].dial_khz = khz;
    return band_lists[i].list;
}

/* After any change of dial frequency. */
static void retuned(void) {
    auto_cq_stop("band changed");
    /* Replies still waiting (behind our TX or the keyboard) or offered on
     * Reply answered stations on the old frequency: never send them here.
     * AGN? there isn't asking for what we sent here. */
    auto_clear();
    last_tx_text[0] = '\0';

    js8_rx_clear(rx);
    bool ended = partials_end();
    stations   = stations_for_band(); /* that band's list, as we left it */
    if (st_all_time) ever_load();      /* and that band's history */
    js8_wf_clear();
    wf_queue_clear();
    marks_reset();
    clear_selection();
    if (view_stations || ended) rebuild_rows();
    add_info_row("%s", where_label());
    update_status();
}

/* Band keys leave a custom frequency for the preset list. Not while
 * sending, as the Freq list: the rest of the message would go out on the
 * new band, and tx_player puts the old dial back after each frame. */
static void band_cb(lv_event_t *e) {
    if (js8_tx_busy(tx)) {
        msg_update_text_fmt("Not while sending - Stop TX first");
        return;
    }
    if (!load_band(lv_event_get_code(e) == EVENT_BAND_UP ? 1 : -1)) {
        msg_update_text_fmt("End of the %s list", param_i_get(cfg.js8.ghostnet()) ? "GhostNet" : "JS8");
        return;
    }
    if (param_i_get(cfg.js8.custom_on())) param_i_set(cfg.js8.custom_on(), false);
    retuned();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    user_touch();

    switch (key) {
    case LV_KEY_ESC:
        LV_LOG_USER("JS8 ESC on the list: tick %u composing %d popup %d tx %d", (unsigned)lv_tick_get(), composing,
                    any_popup(), js8_tx_busy(tx));
        if (cq_adjusting) {
            cq_adjust_end();
        } else if (hb_adjusting) {
            hb_adjusting = false;
            update_tx_bar();
        } else if (js8_tx_busy(tx)) {
            stop_tx_cb(NULL); /* first ESC stops TX; the next one closes */
        } else {
            dialog_destruct();
        }
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

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    /* Nothing transmits on its own when the app opens: AUTO, HB, HB ACK
     * and auto CQ start off every time (the HB interval is remembered). */
    param_i_set(cfg.js8.auto_mode(), false);
    param_i_set(cfg.js8.hb(), false);
    param_i_set(cfg.js8.hb_ack(), false);
    auto_cq = false;
    swr_guard_reset();
    tx_rows_stopped(); /* a message JS8 was closed on stopped then */

    /* Full-screen app with its own waterfall: skip main-screen DSP. */
    waterfall_set_enabled(false);
    spectrum_set_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    mem_save(MEM_BACKUP_ID);
    load_band(0); /* also sets the mode */
    if (param_i_get(cfg.js8.custom_on())) {
        if (param_i_get(cfg.js8.custom_hz()) >= CUSTOM_MIN_HZ && param_i_get(cfg.js8.custom_hz()) <= CUSTOM_MAX_HZ) {
            cparam_i_set(cfg.cur.fg_freq(), param_i_get(cfg.js8.custom_hz()));
            msg_update_text_fmt("%s", where_label()); /* not the preset load_band() just named */
        } else {
            param_i_set(cfg.js8.custom_on(), false);
        }
    }
    js8_usb_dig(); /* a custom frequency on another band loaded that band's mode */

    /* 200-3000 Hz while JS8 is open. High first: each edge is validated
     * against the other. */
    saved_filter_low  = cparam_i_get(cfg.filter.low());
    saved_filter_high = cparam_i_get(cfg.filter.high());
    filter_saved      = true;
    cparam_i_set(cfg.filter.high(), JS8_FILTER_HIGH);
    cparam_i_set(cfg.filter.low(), JS8_FILTER_LOW);
    /* The TX filter too, so a signal up to 3000 Hz goes out whole. Only the
     * radio is told: the setting itself is untouched, so a power loss here
     * can't leave it changed. */
    radio_set_tx_filter(JS8_TX_FILTER_LOW, JS8_TX_FILTER_HIGH);
    /* The base applies noise reduction, the noise blanker and the notches in
     * DIGI modes too, and they damage JS8's tones (the auto-notch goes for
     * exactly such steady tones). Off while the app is open, radio only. */
    radio_set_rx_dsp_off(true);
    /* Turned on while JS8 is open (CAT, a front-panel control): off again
     * at once, the setting itself kept. radio.c's own subscriber sends the
     * new value first (it subscribed at start-up), this one runs after. */
    dsp_watch[0] = subject_subscribe((Subject *)cfg.dsp.nr(), dsp_held_cb, NULL);
    dsp_watch[1] = subject_subscribe((Subject *)cfg.dsp.nb(), dsp_held_cb, NULL);
    dsp_watch[2] = subject_subscribe((Subject *)cfg.dsp.dnf(), dsp_held_cb, NULL);
    dsp_watch[3] = subject_subscribe((Subject *)cfg.dsp.dnf_auto(), dsp_held_cb, NULL);

    filter_low  = cparam_i_get(cfg.filter.low());
    filter_high = cparam_i_get(cfg.filter.high());

    /* Waterfall: drawn on the display's lower plane (js8_wf.c), seen
     * through a hole in this one, where LVGL draws the app. The list, TX
     * bar and finder are over it as before, but a new row no longer makes
     * LVGL redraw them: with the see-through list on top, each row used to
     * redraw all of its text, two thirds of a row's cost. */
    wf_box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(wf_box);
    lv_obj_clear_flag(wf_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(wf_box, WIDTH, WF_HEIGHT);
    lv_obj_set_pos(wf_box, INSET, INSET);
    atomic_store(&wf_avg_level, param_i_get(cfg.js8.wf_avg()) % WF_AVG_LEVELS);
    if (!js8_wf_create(wf_box, (const lv_color_t *)style.wf_palette, WF_MIN_DB, WF_MAX_DB))
        msg_schedule_text_fmt("JS8: no memory for the waterfall");
    wf_watch[0] = subject_subscribe((Subject *)cfg.cur.fg_freq(), wf_watch_cb, NULL);
    wf_watch[1] = subject_subscribe((Subject *)cfg.band.if_shift(), wf_watch_cb, NULL);
    wf_watch[2] = subject_subscribe((Subject *)cfg.mode.zoom(), wf_watch_cb, NULL);
    wf_watch[3] = subject_subscribe((Subject *)cfg.cur.mode_lo_offset(), wf_watch_cb, NULL);
    wf_due_us   = 0;
    gui_last_us = health_start_us = 0; /* no stall counted across a close */
    gui_stalls  = 0;
    wf_timer  = lv_timer_create(wf_timer_cb, WF_TICK_MS, NULL);

    /* Finder marks the offset of the selected message. */

    finder = lv_finder_create(wf_box);
    lv_finder_set_range(finder, filter_low, filter_high);
    /* A stored offset outside the usable range (an old or damaged setting)
     * would make every send fail; start from 1500 Hz instead. */
    if (param_i_get(cfg.js8.tx_freq()) < JS8_TX_MIN_OFFSET || param_i_get(cfg.js8.tx_freq()) > js8_speed_max_offset_hz(cur_speed())) {
        param_i_set(cfg.js8.tx_freq(), 1500);
    }

    /* The finder's band is our TX offset; its cursor line marks the
     * selected message. */
    lv_finder_set_width(finder, js8_speed_bandwidth_hz(cur_speed()));
    lv_finder_set_value(finder, param_i_get(cfg.js8.tx_freq()));
    finder_shown = param_i_get(cfg.js8.tx_freq());
    lv_finder_clear_cursor(finder); /* the green band is cursor_box */
    qso_freq       = -1;
    sel_call[0]    = '\0';
    sel_locked     = false;
    cursor_freq    = -1;
    cursor_user_ms = 0;
    lv_obj_set_size(finder, WIDTH, WF_HEIGHT);
    lv_obj_set_pos(finder, 0, 0);
    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(finder, style.colors.mark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    /* The green band, as lv_finder drew its cursor: green at 30 % with a
     * 1 px green border at 70 %, over the red band. */
    cursor_box = lv_obj_create(finder);
    lv_obj_remove_style_all(cursor_box);
    lv_obj_clear_flag(cursor_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(cursor_box, lv_color_hex(0x50FF50), 0);
    lv_obj_set_style_bg_opa(cursor_box, LV_OPA_30, 0);
    lv_obj_set_style_border_color(cursor_box, lv_color_hex(0x50FF50), 0);
    lv_obj_set_style_border_opa(cursor_box, LV_OPA_70, 0);
    lv_obj_set_style_border_width(cursor_box, 1, 0);
    lv_obj_add_flag(cursor_box, LV_OBJ_FLAG_HIDDEN);

    status = lv_label_create(wf_box);
    lv_obj_set_style_text_font(status, &sony_18, 0);
    lv_obj_set_style_text_color(status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_50, 0);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -4, 4);

    /* The map's QRZ line, the same over the Stations view (VE7NHW): who
     * called while the messages weren't showing. */
    st_qrz_label = lv_label_create(wf_box);
    lv_obj_set_style_text_font(st_qrz_label, &sony_18, 0);
    lv_obj_set_style_text_color(st_qrz_label, lv_color_hex(MAP_QRZ_COLOR), 0);
    lv_obj_set_style_bg_color(st_qrz_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(st_qrz_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(st_qrz_label, 4, 0);
    lv_obj_add_flag(st_qrz_label, LV_OBJ_FLAG_HIDDEN);
    map_qrz_show(); /* reopened in the Stations view with callers still listed */

    /* TX status line */

    tx_bar = lv_label_create(dialog.obj);
    lv_obj_set_size(tx_bar, WIDTH, TX_BAR_H);
    lv_obj_set_pos(tx_bar, INSET, INSET + WF_VISIBLE);
    lv_obj_set_style_text_font(tx_bar, &sony_22, 0);
    lv_obj_set_style_pad_left(tx_bar, 6, 0);
    lv_obj_set_style_pad_top(tx_bar, 3, 0);
    lv_obj_set_style_bg_opa(tx_bar, LV_OPA_50, 0); /* see-through, as the list: the waterfall or map shows (VE7NHW) */
    lv_label_set_long_mode(tx_bar, LV_LABEL_LONG_DOT);
    tx_bar_shown.fresh = true;

    /* Message list */

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_add_event_cb(table, table_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(table, table_select_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(table, table_hold_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(table, table_short_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(table, table_key_pre_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_draw_end_cb, LV_EVENT_DRAW_PART_END, NULL);
    lv_obj_set_size(table, WIDTH, WF_HEIGHT - WF_VISIBLE - TX_BAR_H);
    lv_obj_set_pos(table, INSET, INSET + WF_VISIBLE + TX_BAR_H);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, WIDTH - 2);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, 150, LV_PART_MAIN); /* waterfall shows through */
    lv_obj_set_style_bg_color(table, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(table, 128, LV_PART_MAIN);
    lv_obj_set_style_text_color(table, lv_color_hex(0xC0C0C0), LV_PART_ITEMS);
    /* JS8 messages are long; the dialog's 36 px font fits only two rows. */
    /* sony_24, with js8_marks_24 for the end mark and the degree sign. */
    table_font          = sony_24;
    table_font.fallback = &js8_marks_24;
    lv_obj_set_style_text_font(table, &table_font, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 0, LV_PART_ITEMS);

    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    rebuild_rows();
    add_info_row("%s", where_label());

    lm_set_ab(true);
    keypad_set_long_time(HOLD_MS); /* page back, auto CQ etc. without the long wait */
    lm_set_mode(true);
    lm_set_freq(true);
    lm_set_band(true);

    cycles = cycle_decodes = 0;
    ev_clear(); /* anything left from the last time JS8 was open */
    edit_target = 0; /* a keyboard mode left from last time (B-16) */
    worked_forget();
    wf_queue_clear();
    rx_start();
    /* Receive audio from here on (dsp's thread, into the receiver). */
    if (audio_sub == AUDIO_SUB_INVALID) audio_sub = dsp_audio_subscribe_float(audio_cb, SAMPLE_RATE);
    dsp_audio_set_active(audio_sub, true);
    gps_when = gps_seen = 0;
    gps_sub  = lv_msg_subscribe(MSG_GPS, gps_msg_cb, NULL);
    update_status();

    /* Transmit: same 5 W cap and gain start as the FT8 app. */
    if (param_f_get(cfg.pwr()) > TX_PLAYER_MAX_PWR_W) {
        radio_set_pwr(TX_PLAYER_MAX_PWR_W);
        msg_schedule_text_fmt("Power was limited to %0.0fW", TX_PLAYER_MAX_PWR_W);
    }
    base_gain_offset = tx_player_base_gain_offset();
    tx_start();
    /* The lists stay while the radio is on, like the messages: closing and
     * reopening JS8 used to empty them, so the next decodes came back all at
     * one time and without the ★ of those that had heard us. Stations still
     * drop off an hour after they were last heard (StationList::EXPIRE_MS). */
    stations = stations_for_band();
    if (!qsos) qsos = js8_qsos_create();
    /* Opened once per power-on; a file that couldn't be read was moved
     * aside (or is left alone): say so. */
    if (!inbox) {
        inbox = js8_inbox_open(JS8_INBOX_PATH);
        data_file_notice(js8_inbox_notice(inbox));
    }
    if (!held) {
        held = js8_held_open(JS8_HELD_PATH);
        data_file_notice(js8_held_notice(held));
    }
    /* The station history: open while the radio is on, as the Inbox; its
     * own thread writes it a few seconds' worth at a time. */
    static bool history_tried;
    if (!history_db && !history_tried) {
        history_tried = true;
        history_db    = js8_history_open(JS8_HISTORY_PATH);
        if (!history_db) js8_log("history: %s can't be opened; not recorded", JS8_HISTORY_PATH);
    }
    if (!autop) autop = js8_auto_create();
    memset(deliveries, 0, sizeof(deliveries));
    tx_active = tx_auto = tx_cq = false;
    inbox_refresh_button();
    user_touch();
    load_texts();
    load_saved();
    hb_next_ms         = 0;
    hb_paused_until    = 0;
    push_next_ms       = 0;
    apply_station_keep();
    hb_adjusting       = false;
    auto_clear();
    last_tx_text[0] = '\0';
    tx_timer = lv_timer_create(tx_timer_cb, 250, NULL);
    update_tx_bar();

    /* dialog_construct() drew this page's buttons before we got here: an
     * auto heartbeat left on when the radio was switched off with JS8 open
     * showed "HB auto: soon" until the page changed, though the lines above
     * turn it off. Draw them again with JS8's state as it is now. */
    buttons_page_t *page = buttons_get_cur_page();
    for (size_t i = 0; page && i < sizeof(page->items) / sizeof(page->items[0]); i++)
        if (page->items[i]) buttons_refresh(page->items[i]);
}

static void destruct_cb(void) {
    /* Unkey and join the TX thread first: tx_player restores the VFO and
     * drops PTT before we restore the memory slot below. */
    tx_stop_all();
    if (tx_timer) {
        lv_timer_del(tx_timer);
        tx_timer = NULL;
    }
    if (wf_timer) {
        lv_timer_del(wf_timer);
        wf_timer = NULL;
    }
    compose_close();
    /* Delete popups now, not with query_close()/aprs_close()'s delayed
     * delete: dialog_destruct() frees dialog.obj (their parent) right after
     * this, and the delayed delete would then touch freed memory - GEN or
     * APP with the Query list open crashed the app. */
    for (size_t i = 0; i < POPUPS; i++) {
        if (!*popups[i].list) continue;
        lv_obj_del(*popups[i].list);
        *popups[i].list = NULL;
    }
    hb_adjusting = false;
    radio_set_pwr(param_f_get(cfg.pwr()));

    /* No audio callback after this returns (it takes dsp's lock), so the
     * receiver can go. */
    dsp_audio_set_active(audio_sub, false);
    if (gps_sub) lv_msg_unsubscribe(gps_sub);
    gps_sub = NULL;
    rx_stop();
    wf_queue_clear();
    for (unsigned i = 0; i < sizeof(wf_watch) / sizeof(wf_watch[0]); i++) {
        if (wf_watch[i]) param_unsubscribe(wf_watch[i]);
        wf_watch[i] = NULL;
    }
    js8_wf_destroy(); /* its part of the lower plane black for the main screen */
    js8_history_flush(history_db); /* what's waiting into the file, before the radio might be switched off */
    partials_end(); /* shown as ended when JS8 opens again */

    waterfall_set_enabled(true);
    spectrum_set_enabled(true);

    for (unsigned i = 0; i < sizeof(dsp_watch) / sizeof(dsp_watch[0]); i++) {
        if (dsp_watch[i]) param_unsubscribe(dsp_watch[i]);
        dsp_watch[i] = NULL;
    }
    /* Your filter back, before the saved band and mode return. */
    if (filter_saved) {
        cparam_i_set(cfg.filter.high(), saved_filter_high);
        cparam_i_set(cfg.filter.low(), saved_filter_low);
        radio_set_tx_filter(param_i_get(cfg.dsp.tx_filter_low()), param_i_get(cfg.dsp.tx_filter_high()));
        radio_set_rx_dsp_off(false);
        filter_saved = false;
    }
    mem_load(MEM_BACKUP_ID);

    lm_set_mode(false);
    lm_set_ab(false);
    keypad_set_long_time(0);
    lm_set_freq(false);
    lm_set_band(false);

    /* LVGL objects are children of dialog.obj, deleted by dialog_destruct()
     * right after this returns. */
    finder = table = status = st_qrz_label = tx_bar = cursor_box = NULL;
    wf_box    = NULL;
    map_free();
}


/* ---- Show Map (docs/MAP_PLAN.md) ----------------------------------------- */

/* A map in place of the waterfall and the list, GridTracker's look: the
 * base map (src/js8/map_render, Natural Earth) drawn for the view, the
 * stations of this frequency in their grid squares (or placed by callsign,
 * hollow), curved paths to those who heard you, you in orange, the
 * selected station in red, and new stations popping up for 8 s. The
 * Stations view stays underneath: the MFK selects there. */

/* The map fills the whole inside of the dialog's border, the frame's
 * blue-grey band included, not just the waterfall's place (VE7NHW). Where
 * that border is depends on the theme's dialog image (map_geometry()); its
 * rounded or cut corners are put back over the map (map_corners_apply()). */
static int map_x = INSET, map_y = INSET, map_w = WIDTH, map_h = WF_HEIGHT; /* map_geometry() */
static int map_corner_r = 9; /* the TX bar's corners on the map, inside the border's */
static int map_edge     = 4; /* the status line this far in, clear of the corners */
#define MAP_X          map_x
#define MAP_Y          map_y
#define MAP_W          map_w
#define MAP_H          map_h
#define MAP_CORNER     11 /* the border's corners reach this far in (x + y) */
#define MAP_LEGEND_H   26
#define MAP_STRIP_H    40 /* the last two messages, above the legend */
#define MAP_FIT_H      (MAP_H - TX_BAR_H - MAP_LEGEND_H - MAP_STRIP_H) /* stations fit above those and the TX bar */
#define MAP_LABEL_BOTTOM MAP_FIT_H /* labels stay above them too */
#define MAP_POPUPS     8
#define MAP_POPUP_MS   8000
#define MAP_PATH_N     48
#define MAP_OCEAN      0x232227
#define MAP_HEARD      0x00FF00 /* GridTracker's CQ green */
#define MAP_HOME       0xFFA600 /* GridTracker's QTH orange */
#define MAP_PATH       0x42A5F5 /* paths to who heard you: a lighter blue (VE7NHW's pick) */
#define MAP_QSO        0xFF3030 /* ... red while a message to or from you is on the air */
#define MAP_QSO_MS     30000    /* after their last frame to you */
#define MAP_RING_MS    16000    /* the flashing ring after each frame to you: a slot and a bit, so it
                                   keeps flashing through a long message */
#define MAP_SELECT     0xFF3030
#define MAP_TX         0xFF1010 /* your outline while transmitting */
#define MAP_NEW_BORDER 0xFFFFFF /* a grid or country you've never worked */
#define MAP_CQ_BG      0x00E000 /* the "CQ" tag: black on the heard green */
#define MAP_STACK_PX   2        /* marks this close are on the same spot: one count tag for them */
#define MAP_STACK_BG   0xFFFFFF /* the count tag: black on white, the CQ tag's shape (VE7NHW's pick) */
#define MAP_FADE_MIN   45       /* squares fade to 30 % over this many minutes unheard */
#define MAP_SQUARE_OPA 136      /* GridTracker's grid alpha */
#define MAP_TAKEN      32       /* label boxes placed per redraw */
#define MAP_TALK       16       /* the band's other conversations remembered */
#define MAP_TALK_MS    600000   /* their grey lines fade out over 10 min */
#define MAP_TALK_COLOR 0xC8C8C8
#define MAP_PULSE_MS   2000     /* a frame to you: a dot runs from them to you */
#define MAP_DOTS       4        /* incoming dots at once; one more for yours */
#define MAP_PATHS      8        /* red paths remembered for the dots */

static uint32_t       *map_base, *map_px; /* the base map, and base + stations as shown */
static js8_map_data_t *map_data;
static js8_map_view_t  map_view;
static bool            map_view_ok, map_world;
static char            map_my_cont[3];
static uint32_t        map_sig;
static struct {
    char    call[JS8_RX_CALL_LEN];
    int64_t until_ms; /* monotonic */
} map_popups[MAP_POPUPS];
static bool      map_tx_drawn; /* the red "transmitting" outline is on the map (harness) */
static int       map_qso_drawn; /* red paths drawn (harness) */
static lv_area_t map_taken[MAP_TAKEN];
static int       map_ntaken;
static struct {
    char    a[JS8_RX_CALL_LEN], b[JS8_RX_CALL_LEN];
    int64_t ms; /* monotonic, last heard; 0: free */
} map_talk[MAP_TALK];
static int map_talk_drawn; /* grey lines drawn (harness) */
static struct {
    char       call[JS8_RX_CALL_LEN];
    lv_point_t p[MAP_PATH_N + 1]; /* from you to them, as drawn */
    float      len[MAP_PATH_N + 1]; /* along the path to each point */
} map_paths[MAP_PATHS];
static int map_npaths;
static struct {
    char    call[JS8_RX_CALL_LEN];
    int64_t start_ms; /* monotonic */
    bool    seen;     /* its dot has shown (harness count) */
} map_pulses[MAP_DOTS];
static int map_pulses_seen;
static lv_obj_t   *map_dots[MAP_DOTS + 1]; /* [0]: yours, along the path while you send */
static lv_timer_t *map_anim;               /* moves them, while the map shows */
static lv_obj_t   *map_strip_box, *map_strip[2];
static char        map_strip_src[2][JS8_RX_TEXT_LEN + 24]; /* each line before it was cut to fit */
static bool        map_follow; /* the view frames you and the selected station (hold Map:) */

static const char *const map_mode_names[3] = {"Auto", "Close-in", "World"};

/* The QRZ line under the status line, on the map and over the Stations
 * view: how many called, newest first. */
static void map_qrz_show(void) {
    char buf[96] = "";
    int  len = map_qrz_n > 1 ? snprintf(buf, sizeof(buf), "QRZ %d ", map_qrz_n) : snprintf(buf, sizeof(buf), "QRZ ");
    for (int i = map_qrz_n - 1, shown = 0; i >= 0 && len < (int)sizeof(buf); i--, shown++) {
        if (shown == 3) {
            snprintf(buf + len, sizeof(buf) - len, " +%d", i + 1);
            break;
        }
        len += snprintf(buf + len, sizeof(buf) - len, " %s", map_qrz[i]);
    }
    if (st_qrz_label && status) {
        if (map_qrz_n && view_stations && !view_map) {
            if (strcmp(lv_label_get_text(st_qrz_label), buf) != 0) lv_label_set_text(st_qrz_label, buf);
            lv_obj_update_layout(status); /* its size, before lining up under it */
            lv_obj_align_to(st_qrz_label, status, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 3);
            lv_obj_clear_flag(st_qrz_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st_qrz_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!map_qrz_label || !map_status) return;
    if (!map_qrz_n) {
        lv_obj_add_flag(map_qrz_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(map_qrz_label, buf);
    lv_obj_clear_flag(map_qrz_label, LV_OBJ_FLAG_HIDDEN);
    if (map_top_place()) map_sig = 0; /* under the status line and the stats */
}

static void map_qrz_add(const char *call) {
    int at = -1;
    for (int i = 0; i < map_qrz_n; i++)
        if (strcasecmp(map_qrz[i], call) == 0) at = i;
    if (at < 0 && map_qrz_n == MAP_QRZ) at = 0; /* full: the oldest goes */
    if (at >= 0) { /* to the end: newest */
        memmove(map_qrz[at], map_qrz[at + 1], (size_t)(map_qrz_n - at - 1) * sizeof(map_qrz[0]));
        map_qrz_n--;
    }
    snprintf(map_qrz[map_qrz_n++], sizeof(map_qrz[0]), "%s", call);
    map_qrz_show();
}

static void map_qrz_clear(const char *call) {
    for (int i = 0; i < map_qrz_n; i++)
        if (strcasecmp(map_qrz[i], call) == 0) {
            memmove(map_qrz[i], map_qrz[i + 1], (size_t)(map_qrz_n - i - 1) * sizeof(map_qrz[0]));
            map_qrz_n--;
            map_qrz_show();
            return;
        }
}

static void map_popup_add(const char *call) {
    int slot = 0;
    for (int i = 0; i < MAP_POPUPS; i++) {
        if (strcasecmp(map_popups[i].call, call) == 0) {
            slot = i;
            break;
        }
        if (map_popups[i].until_ms < map_popups[slot].until_ms) slot = i; /* the oldest goes */
    }
    snprintf(map_popups[slot].call, sizeof(map_popups[slot].call), "%s", call);
    map_popups[slot].until_ms = now_mono_ms() + MAP_POPUP_MS;
}

/* Stations messaging you: their path is red until MAP_QSO_MS after their
 * last frame to you (the frames of a long message keep it red). */
static struct {
    char    call[JS8_RX_CALL_LEN];
    int64_t until_ms; /* monotonic */
    int64_t ring_ms;  /* the new-station ring flashes until then: they're calling you */
} map_qso[MAP_POPUPS];

/* `ring`: flash the ring too (a real message or command, not a heartbeat
 * reply: those come automatically, often). */
static void map_qso_add(const char *call, bool ring) {
    int slot = 0;
    for (int i = 0; i < MAP_POPUPS; i++) {
        if (strcasecmp(map_qso[i].call, call) == 0) {
            slot = i;
            break;
        }
        if (map_qso[i].until_ms < map_qso[slot].until_ms) slot = i;
    }
    snprintf(map_qso[slot].call, sizeof(map_qso[slot].call), "%s", call);
    map_qso[slot].until_ms = now_mono_ms() + MAP_QSO_MS;
    if (ring) map_qso[slot].ring_ms = now_mono_ms() + MAP_RING_MS;
}


static bool map_ring_on(const char *call, int64_t now) {
    for (int i = 0; i < MAP_POPUPS; i++)
        if (map_qso[i].ring_ms > now && strcasecmp(map_qso[i].call, call) == 0) return true;
    return false;
}

static bool map_qso_on(const char *call, int64_t now) {
    for (int i = 0; i < MAP_POPUPS; i++)
        if (map_qso[i].until_ms > now && strcasecmp(map_qso[i].call, call) == 0) return true;
    return false;
}

/* Who we're sending to: the call the text starts with ("W7XYZ SNR -12",
 * "W1ABC>VE7ABC MSG ..." -> W1ABC); "" for CQ, heartbeats, @groups. */
static void map_tx_target(const char *text, char *call, size_t size) {
    size_t n = 0;
    bool   digit = false;
    call[0]      = '\0';
    if (!text || text[0] == '@') return;
    while (text[n] && (isalnum((unsigned char)text[n]) || text[n] == '/') && n + 1 < size) {
        digit |= isdigit((unsigned char)text[n]) != 0;
        n++;
    }
    if (n < 3 || !digit || (text[n] && text[n] != ' ' && text[n] != '>' && text[n] != ':')) return;
    memcpy(call, text, n);
    call[n] = '\0';
    if (strcmp(call, "CQ") == 0) call[0] = '\0';
}

static bool map_popup_on(const char *call, int64_t now) {
    for (int i = 0; i < MAP_POPUPS; i++)
        if (map_popups[i].until_ms > now && strcasecmp(map_popups[i].call, call) == 0) return true;
    return false;
}

/* Objects and buffers, made the first time the map opens. */
/* ---- Moving dots, other QSOs, the message strip ---- */

static void map_pulse_add(const char *call) {
    int slot = 0;
    for (int i = 1; i < MAP_DOTS; i++)
        if (map_pulses[i].start_ms < map_pulses[slot].start_ms) slot = i;
    snprintf(map_pulses[slot].call, sizeof(map_pulses[slot].call), "%s", call);
    map_pulses[slot].start_ms = now_mono_ms();
    map_pulses[slot].seen     = false;
}

static void map_talk_add(const char *a, const char *b) {
    if (!a[0] || !b[0] || strcasecmp(a, b) == 0) return;
    int slot = 0;
    for (int i = 0; i < MAP_TALK; i++) {
        bool same = (strcasecmp(map_talk[i].a, a) == 0 && strcasecmp(map_talk[i].b, b) == 0) ||
                    (strcasecmp(map_talk[i].a, b) == 0 && strcasecmp(map_talk[i].b, a) == 0);
        if (same) {
            slot = i;
            break;
        }
        if (map_talk[i].ms < map_talk[slot].ms) slot = i;
    }
    snprintf(map_talk[slot].a, sizeof(map_talk[slot].a), "%s", a);
    snprintf(map_talk[slot].b, sizeof(map_talk[slot].b), "%s", b);
    map_talk[slot].ms = now_mono_ms();
}

/* Other stations talking to each other ("W7XYZ: K9DEF HW CPY?"), and a
 * relay's hops beyond ("VE3KP: N0XYZ > K5LOW HELLO", as decoded:
 * VE3KP-N0XYZ, N0XYZ-K5LOW). Not groups, not you (that's the red path). */
static void map_talk_note(const js8_rx_msg_t *m) {
    if (m->tx || m->to_me || m->to_group || !m->from[0] || !m->to[0] || m->to[0] == '@') return;
    map_talk_add(m->from, m->to);
    const char *p = strstr(m->text, ": ");
    size_t      tl = strlen(m->to);
    if (!p || strncasecmp(p + 2, m->to, tl) != 0) return;
    char prev[JS8_RX_CALL_LEN];
    snprintf(prev, sizeof(prev), "%s", m->to);
    p += 2 + tl;
    for (int hop = 0; hop < 3; hop++) {
        while (*p == ' ') p++;
        if (*p != '>') return;
        p++;
        while (*p == ' ') p++;
        char   next[JS8_RX_CALL_LEN];
        size_t n = 0;
        while (*p && *p != ' ' && *p != '>' && n + 1 < sizeof(next)) next[n++] = *p++;
        next[n] = '\0';
        if (!n || !strpbrk(next, "0123456789") || strcasecmp(next, my_call()) == 0) return;
        map_talk_add(prev, next);
        snprintf(prev, sizeof(prev), "%s", next);
    }
}

/* A red path as drawn, for the dots: from you (0) to them (1). */
static void map_path_keep(const char *call, const lv_point_t *p) {
    if (map_npaths == MAP_PATHS) return;
    snprintf(map_paths[map_npaths].call, sizeof(map_paths[0].call), "%s", call);
    memcpy(map_paths[map_npaths].p, p, sizeof(map_paths[0].p));
    float len = 0;
    for (int k = 0; k <= MAP_PATH_N; k++) {
        if (k) len += sqrtf((float)((p[k].x - p[k - 1].x) * (p[k].x - p[k - 1].x) +
                                    (p[k].y - p[k - 1].y) * (p[k].y - p[k - 1].y)));
        map_paths[map_npaths].len[k] = len;
    }
    map_npaths++;
}

/* Put dot `i` at fraction `f` along the red path to `call`, or hide it;
 * true if shown. */
static bool map_dot_at(int i, const char *call, float f) {
    lv_obj_t *d = map_dots[i];
    for (int k = 0; call && call[0] && k < map_npaths; k++) {
        if (strcasecmp(map_paths[k].call, call) != 0) continue;
        const float *len  = map_paths[k].len;
        float        want = LV_CLAMP(0.0f, f, 1.0f) * len[MAP_PATH_N];
        int          j    = 1;
        while (j < MAP_PATH_N && len[j] < want) j++;
        float seg = len[j] - len[j - 1], t = seg > 0 ? (want - len[j - 1]) / seg : 0;
        int   x = (int)(map_paths[k].p[j - 1].x + t * (map_paths[k].p[j].x - map_paths[k].p[j - 1].x) + 0.5f);
        int   y = (int)(map_paths[k].p[j - 1].y + t * (map_paths[k].p[j].y - map_paths[k].p[j - 1].y) + 0.5f);
        lv_obj_set_pos(d, x - lv_obj_get_width(d) / 2, y - lv_obj_get_height(d) / 2);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    if (!lv_obj_has_flag(d, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    return false;
}

/* 25 times a second while the map shows: your dot runs from you to the
 * station you're sending to as the message goes out (frame by frame, like
 * a progress bar); a frame to you sends a yellow one from them to you. */
static void map_anim_cb(lv_timer_t *t) {
    (void)t;
    if (!view_map || !map_dots[0]) return;
    bool sending = tx_status.state == JS8_TX_KEYING || (tx_status.state == JS8_TX_WAITING && tx_status.frame > 1);
    char to[JS8_RX_CALL_LEN] = "";
    if (sending && tx_status.frames > 0) map_tx_target(tx_status.text, to, sizeof(to));
    float f = 0;
    if (to[0]) {
        int64_t period = (int64_t)js8_speed_period_s(tx_status.speed) * 1000;
        float   frac   = 0;
        if (tx_status.state == JS8_TX_KEYING && period > 0)
            frac = (float)LV_CLAMP(0, now_wall_ms() - tx_status.next_ms, period) / (float)period;
        f = (tx_status.frame - 1 + frac) / (float)tx_status.frames;
    }
    map_dot_at(0, to, f);
    int64_t now = now_mono_ms();
    for (int i = 0; i < MAP_DOTS; i++) {
        bool on = map_pulses[i].start_ms && now - map_pulses[i].start_ms < MAP_PULSE_MS;
        if (map_dot_at(1 + i, on ? map_pulses[i].call : NULL,
                       1.0f - (float)(now - map_pulses[i].start_ms) / MAP_PULSE_MS) &&
            !map_pulses[i].seen) {
            map_pulses[i].seen = true;
            map_pulses_seen++;
        }
    }
}

/* The strip: the last two messages the list would show (its Show filter),
 * older on top; yours light red, to you yellow. */
static void map_strip_update(void) {
    if (!map_strip_box) return;
    const js8_rx_msg_t *last[2] = {NULL, NULL};
    int                 found   = 0;
    for (int k = 0; k < hist_count && found < 2; k++) {
        const js8_rx_msg_t *m = &history[(hist_head - 1 - k + HISTORY) % HISTORY];
        if (passes_filter(m)) last[found++] = m;
    }
    for (int line = 0; line < 2; line++) {
        const js8_rx_msg_t *m        = last[1 - line];
        char                buf[JS8_RX_TEXT_LEN + 24] = "";
        uint32_t            color    = 0xE0E0E0;
        if (m) {
            int hh = m->utc / 10000, mm = (m->utc / 100) % 100;
            char mark[32] = "";
            if (m->tx) tx_row_mark((int)(m - history), false, mark, sizeof(mark));
            if (m->tx) snprintf(buf, sizeof(buf), "%02d:%02d  TX   %s%s", hh, mm, m->text, mark);
            else snprintf(buf, sizeof(buf), "%02d:%02d %+3d  %s%s", hh, mm, m->snr, m->text, m->partial ? " ..." : "");
            color = m->tx ? 0xFF9090 : m->to_me ? MAP_QRZ_COLOR : 0xE0E0E0;
        }
        if (strcmp(map_strip_src[line], buf) == 0) continue; /* as shown (checked 4 times a second) */
        snprintf(map_strip_src[line], sizeof(map_strip_src[line]), "%s", buf);
        if (buf[0]) {
            /* Too long for the screen: cut, with "..." (its box only as
             * wide as the text, so LVGL's own dots can't be used). */
            lv_point_t sz;
            size_t     len = strlen(buf);
            lv_txt_get_size(&sz, buf, &sony_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            while (sz.x > MAP_W - 20 && len > 4) {
                len -= 2;
                memcpy(buf + len - 3, "...", 4);
                lv_txt_get_size(&sz, buf, &sony_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            }
        }
        if (strcmp(lv_label_get_text(map_strip[line]), buf) != 0) lv_label_set_text(map_strip[line], buf);
        if (!buf[0] != lv_obj_has_flag(map_strip[line], LV_OBJ_FLAG_HIDDEN)) { /* no empty box */
            if (buf[0]) lv_obj_clear_flag(map_strip[line], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(map_strip[line], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_text_color(map_strip[line], lv_color_hex(color), 0);
    }
    if (found && lv_obj_has_flag(map_strip_box, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(map_strip_box, LV_OBJ_FLAG_HIDDEN);
    if (!found && !lv_obj_has_flag(map_strip_box, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(map_strip_box, LV_OBJ_FLAG_HIDDEN);
}

/* The inside of the dialog's border. R1CBU 1.0 draws the dialog itself
 * (styles.c, DIALOG_WIDTH x DIALOG_HEIGHT in every theme): Simple and Flat
 * with a 1 px border and 8 px corners, Black with no border and 7 px. */
static void map_geometry(void) {
    int border   = param_i_get(cfg.ui.theme()) == THEME_BLACK ? 0 : 1;
    map_x        = border;
    map_y        = border;
    map_w        = DIALOG_WIDTH - 2 * border;
    map_h        = DIALOG_HEIGHT - 2 * border;
    map_corner_r = 8;
    map_edge     = 4;
}

/* The border's corners as the dialog image draws them (over the screen
 * behind it): the map's square corners would stick out past them. Read
 * from the image when the map is made. */
static struct {
    int16_t  x, y; /* on the map */
    uint32_t c;
} map_corner_px[4 * MAP_CORNER * (MAP_CORNER + 1) / 2];
static int map_corner_n;

static bool map_img_read(lv_img_decoder_dsc_t *dsc, int x, int y, int len, lv_color32_t *out) {
    if (dsc->img_data) { /* an image in memory */
        memcpy(out, dsc->img_data + ((size_t)y * dsc->header.w + x) * sizeof(lv_color32_t), len * sizeof(lv_color32_t));
        return true;
    }
    return lv_img_decoder_read_line(dsc, x, y, len, (uint8_t *)out) == LV_RES_OK; /* a file */
}

static void map_corners_load(void) {
    map_corner_n = 0;
    const void          *src = lv_obj_get_style_bg_img_src(dialog.obj, LV_PART_MAIN);
    lv_img_decoder_dsc_t dsc;
    if (!src || lv_img_decoder_open(&dsc, src, lv_color_white(), 0) != LV_RES_OK) return;
    if (dsc.header.cf == LV_IMG_CF_TRUE_COLOR_ALPHA && sizeof(lv_color_t) == sizeof(lv_color32_t) &&
        MAP_X + MAP_W < (int)dsc.header.w && MAP_Y + MAP_H < (int)dsc.header.h) {
        /* Behind the dialog: R1CBU 1.0's other display plane, whose spectrum
         * and waterfall JS8 turns off (black). Blending the few corner
         * pixels here keeps the map an opaque canvas (cheap to draw). */
        lv_color32_t bg = {.full = lv_color_to32(lv_color_black())};
        for (int corner = 0; corner < 4; corner++) {
            bool right = corner & 1, bottom = corner & 2;
            int  x0    = right ? MAP_W - MAP_CORNER : 0;
            for (int dy = 0; dy < MAP_CORNER; dy++) {
                int          y = bottom ? MAP_H - 1 - dy : dy;
                lv_color32_t row[MAP_CORNER], mid;
                if (!map_img_read(&dsc, MAP_X + x0, MAP_Y + y, MAP_CORNER, row) ||
                    !map_img_read(&dsc, MAP_X + MAP_W / 2, MAP_Y + y, 1, &mid))
                    continue;
                for (int dx = 0; dx + dy < MAP_CORNER; dx++) {
                    int          x = right ? MAP_W - 1 - dx : dx;
                    lv_color32_t p = row[x - x0];
                    /* As the band in the middle of this row: the map covers it. */
                    if (abs(p.ch.alpha - mid.ch.alpha) <= 8 && abs(p.ch.red - mid.ch.red) <= 40 &&
                        abs(p.ch.green - mid.ch.green) <= 40 && abs(p.ch.blue - mid.ch.blue) <= 40)
                        continue;
                    int a = p.ch.alpha;
                    map_corner_px[map_corner_n].x = x;
                    map_corner_px[map_corner_n].y = y;
                    map_corner_px[map_corner_n].c = 0xFF000000u |
                                                    (uint32_t)((p.ch.red * a + bg.ch.red * (255 - a)) / 255) << 16 |
                                                    (uint32_t)((p.ch.green * a + bg.ch.green * (255 - a)) / 255) << 8 |
                                                    (uint32_t)((p.ch.blue * a + bg.ch.blue * (255 - a)) / 255);
                    map_corner_n++;
                }
            }
        }
    }
    lv_img_decoder_close(&dsc);
}

static void map_corners_apply(void) {
    for (int i = 0; i < map_corner_n; i++) map_px[map_corner_px[i].y * MAP_W + map_corner_px[i].x] = map_corner_px[i].c;
}

static bool map_ensure(void) {
    if (map_box) return true;
    map_geometry();
    map_base = malloc((size_t)MAP_W * MAP_H * sizeof(uint32_t));
    map_px   = malloc((size_t)MAP_W * MAP_H * sizeof(uint32_t));
    if (!map_base || !map_px) {
        free(map_base);
        free(map_px);
        map_base = map_px = NULL;
        return false;
    }
    if (!map_data) map_data = js8_map_data_load(JS8_MAP_DATA_PATH);
    if (!js8_map_countries_loaded() && !js8_map_load_countries(JS8_CTY_SD_PATH))
        js8_map_load_countries(JS8_CTY_PATH);
    if (!map_data) LV_LOG_USER("JS8 map: no map data at %s", JS8_MAP_DATA_PATH);

    /* An opaque box, as the waterfall's: LVGL starts drawing there and
     * never redraws the list and the dialog background behind it. */
    map_box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(map_box);
    lv_obj_set_style_bg_color(map_box, lv_color_hex(MAP_OCEAN), 0);
    lv_obj_set_style_bg_opa(map_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(map_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(map_box, MAP_W, MAP_H);
    lv_obj_set_pos(map_box, MAP_X, MAP_Y);
    lv_obj_add_flag(map_box, LV_OBJ_FLAG_HIDDEN);

    map_canvas = lv_canvas_create(map_box);
    lv_canvas_set_buffer(map_canvas, map_px, MAP_W, MAP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(map_canvas, 0, 0);
    map_corners_load();

    map_status = lv_label_create(map_box);
    lv_obj_set_style_text_font(map_status, &sony_18, 0);
    lv_obj_set_style_text_color(map_status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(map_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(map_status, LV_OPA_50, 0);
    lv_obj_align(map_status, LV_ALIGN_TOP_RIGHT, -map_edge, map_edge);
    lv_label_set_text(map_status, "");

    map_stats_label = lv_label_create(map_box);
    lv_obj_set_style_text_font(map_stats_label, &sony_16, 0);
    lv_obj_set_style_text_color(map_stats_label, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_bg_color(map_stats_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(map_stats_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(map_stats_label, 4, 0);
    lv_obj_align(map_stats_label, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_text(map_stats_label, "");

    map_qrz_label = lv_label_create(map_box);
    lv_obj_set_style_text_font(map_qrz_label, &sony_18, 0);
    lv_obj_set_style_text_color(map_qrz_label, lv_color_hex(MAP_QRZ_COLOR), 0);
    lv_obj_set_style_bg_color(map_qrz_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(map_qrz_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(map_qrz_label, 4, 0);
    lv_obj_add_flag(map_qrz_label, LV_OBJ_FLAG_HIDDEN);

    /* The last two messages, as the list shows them, above the legend:
     * see-through like the status line, a dark box only behind the text. */
    map_strip_box = lv_obj_create(map_box);
    lv_obj_remove_style_all(map_strip_box);
    lv_obj_clear_flag(map_strip_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(map_strip_box, MAP_W, MAP_STRIP_H);
    memset(map_strip_src, 0, sizeof(map_strip_src));
    lv_obj_set_pos(map_strip_box, 0, MAP_FIT_H);
    lv_obj_add_flag(map_strip_box, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) {
        map_strip[i] = lv_label_create(map_strip_box);
        lv_obj_set_style_text_font(map_strip[i], &sony_16, 0);
        lv_obj_set_style_bg_color(map_strip[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(map_strip[i], LV_OPA_50, 0);
        lv_obj_set_style_pad_hor(map_strip[i], 4, 0);
        lv_obj_set_pos(map_strip[i], 4, 2 + i * (MAP_STRIP_H - 4) / 2);
        lv_label_set_text(map_strip[i], "");
        lv_obj_add_flag(map_strip[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* The dots running along red paths: yours white, theirs yellow. */
    for (int i = 0; i <= MAP_DOTS; i++) {
        lv_obj_t *d = map_dots[i] = lv_obj_create(map_box);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, i ? 11 : 13, i ? 11 : 13);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(i ? MAP_QRZ_COLOR : 0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(d, lv_color_hex(i ? 0x000000 : MAP_TX), 0);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    map_anim = lv_timer_create(map_anim_cb, 40, NULL);
    lv_timer_pause(map_anim);
    map_view_ok = false;
    return true;
}

static void map_free(void) {
    /* The objects go with dialog.obj. */
    map_box = map_canvas = map_status = map_qrz_label = map_stats_label = map_strip_box = NULL;
    map_strip[0] = map_strip[1] = NULL;
    memset(map_dots, 0, sizeof(map_dots));
    if (map_anim) lv_timer_del(map_anim);
    map_anim   = NULL;
    map_npaths = 0;
    map_follow = false;
    free(map_base);
    free(map_px);
    map_base = map_px = NULL;
    js8_map_data_free(map_data);
    map_data    = NULL;
    map_view_ok = false;
    view_map    = false;
}

static void map_show(bool on) {
    if (on && !map_ensure()) {
        msg_update_text_fmt("Map: not enough memory");
        return;
    }
    view_map = on;
    if (on) {
        js8_map_my_continent(my_call(), my_grid(), map_my_cont);
        if (!js8_map_load_worked(JS8_QSO_DB_PATH)) LV_LOG_USER("JS8 map: no QSO log at %s", JS8_QSO_DB_PATH);
        lv_obj_add_flag(wf_box, LV_OBJ_FLAG_HIDDEN); /* decoding goes on; rows just aren't drawn */
        lv_obj_clear_flag(map_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(map_box);
        lv_obj_set_pos(tx_bar, MAP_X, MAP_Y + MAP_H - TX_BAR_H);
        lv_obj_set_width(tx_bar, MAP_W);
        lv_obj_set_style_radius(tx_bar, map_corner_r, 0); /* its see-through black stays off the border */
        lv_obj_move_foreground(tx_bar);
        map_view_ok = false;
        update_status();
        map_qrz_show();
        map_update(true);
        lv_timer_resume(map_anim);
    } else {
        if (map_anim) lv_timer_pause(map_anim);
        map_follow = false;
        if (map_box) lv_obj_add_flag(map_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wf_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(tx_bar, INSET, INSET + WF_VISIBLE);
        lv_obj_set_width(tx_bar, WIDTH);
        lv_obj_set_style_radius(tx_bar, 0, 0);
    }
    if (btn_map_view.disp_btn) buttons_refresh(&btn_map_view);
    if (btn_show.disp_btn) buttons_refresh(&btn_show);
}

static void map_rect_ex(int x, int y, int w, int h, uint32_t fill, lv_opa_t fill_opa, uint32_t border,
                        lv_opa_t border_opa, int bw, lv_coord_t radius) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color     = lv_color_hex(fill);
    d.bg_opa       = fill_opa;
    d.border_color = lv_color_hex(border);
    d.border_width = bw;
    d.border_opa   = bw ? border_opa : LV_OPA_TRANSP;
    d.radius       = radius;
    lv_canvas_draw_rect(map_canvas, x, y, w, h, &d);
}

static void map_rect(int x, int y, int w, int h, uint32_t fill, lv_opa_t fill_opa, uint32_t border, int bw,
                     lv_coord_t radius) {
    map_rect_ex(x, y, w, h, fill, fill_opa, border, LV_OPA_COVER, bw, radius);
}

static bool map_overlaps(const lv_area_t *a) {
    for (int i = 0; i < map_ntaken; i++)
        if (a->x1 < map_taken[i].x2 && map_taken[i].x1 < a->x2 && a->y1 < map_taken[i].y2 && map_taken[i].y1 < a->y2)
            return true;
    return false;
}

static void map_take(lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2) {
    if (map_ntaken < MAP_TAKEN) map_taken[map_ntaken++] = (lv_area_t){x1, y1, x2, y2};
}

/* A label on a dark box beside (x, y): right of it, else left, above or
 * below, wherever it covers no other label (or the legend, the status
 * line). A forced one takes the first place that fits on the map; any
 * other is left out if none is free. */
static void map_label(int x, int y, const char *text, uint32_t fg, uint32_t bg, lv_opa_t bg_opa, int dx,
                      bool force) {
    lv_point_t sz;
    lv_txt_get_size(&sz, text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int places[4][2] = {{x + dx, y - sz.y / 2},
                              {x - dx - sz.x, y - sz.y / 2},
                              {x - sz.x / 2, y - dx - sz.y},
                              {x - sz.x / 2, y + dx}};
    lv_area_t box;
    int       bx = 0, by = 0;
    bool      found = false;
    /* Never over its own station (moved back onto the map, a label can
     * land on it). */
    const lv_area_t own = {x - 8, y - 8, x + 8, y + 8};
#define MAP_LABEL_FREE(b)                                                                                              \
    (!map_overlaps(&(b)) && !((b).x1 < own.x2 && own.x1 < (b).x2 && (b).y1 < own.y2 && own.y1 < (b).y2))
    for (int k = 0; k < 4 && !found; k++) {
        bx = LV_CLAMP(4, places[k][0], MAP_W - sz.x - 4);
        by = LV_CLAMP(2, places[k][1], MAP_LABEL_BOTTOM - sz.y - 2);
        box   = (lv_area_t){bx - 4, by - 2, bx + sz.x + 4, by + sz.y + 2};
        found = MAP_LABEL_FREE(box);
        if (force && !found && k == 0) { /* forced: keep the first place unless another is free */
            lv_area_t first = box;
            int       fx = bx, fy = by;
            for (int j = 1; j < 4 && !found; j++) {
                bx    = LV_CLAMP(4, places[j][0], MAP_W - sz.x - 4);
                by    = LV_CLAMP(2, places[j][1], MAP_LABEL_BOTTOM - sz.y - 2);
                box   = (lv_area_t){bx - 4, by - 2, bx + sz.x + 4, by + sz.y + 2};
                found = MAP_LABEL_FREE(box);
            }
            if (!found) {
                box   = first;
                bx    = fx;
                by    = fy;
                found = true;
            }
        }
    }
#undef MAP_LABEL_FREE
    if (!found) return;
    map_take(box.x1, box.y1, box.x2, box.y2);
    map_rect(box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1, bg, bg_opa, 0, 0, 3);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(fg);
    ld.font  = &sony_18;
    lv_canvas_draw_text(map_canvas, bx, by, sz.x + 4, &ld, text);
}

/* A station's square, centred in its 4-character grid square (which is
 * 2 x 1 degrees, a rectangle on the map: the mark is a true square, 3/4 of
 * the grid square's mean side, 6-20 px, `grow` px more or less), or,
 * placed by callsign, a hollow box; `border`/`line_opa` its outline. Its
 * centre in *cx, *cy; returns its side. */
static int map_station_mark(const char *grid, const js8_map_place_t *pl, uint32_t color, lv_opa_t opa,
                            uint32_t border, lv_opa_t line_opa, int grow, int *cx, int *cy) {
    float x0, y0, x1, y1;
    char  g4[5] = "";
    if (!pl->approx && strlen(grid) >= 4) snprintf(g4, sizeof(g4), "%.4s", grid);
    if (g4[0] && js8_map_grid_rect(&map_view, g4, &x0, &y0, &x1, &y1)) {
        float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        int   side = (int)((x1 - x0 + y1 - y0) * 0.375f + 0.5f); /* a little smaller than the square (VE7NHW) */
        side       = LV_CLAMP(6, side, 20) + grow;
        side       = LV_CLAMP(5, side, 22);
        *cx        = (int)(mx + 0.5f);
        *cy        = (int)(my + 0.5f);
        map_rect_ex(*cx - side / 2, *cy - side / 2, side, side, color, opa, border, line_opa, 1, 0);
        return side;
    }
    float x, y;
    int   side = LV_CLAMP(7, 9 + grow, 12);
    js8_map_project(&map_view, pl->lat, pl->lon, &x, &y);
    map_rect_ex((int)x - side / 2, (int)y - side / 2, side, side, color, LV_OPA_TRANSP, border == color ? color : border,
                line_opa, 2, 0);
    *cx = (int)x;
    *cy = (int)y;
    return side;
}

static uint32_t map_hash(uint32_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void map_legend(bool have_home) {
    /* See-through, as the status line: a dark box only behind the text,
     * the map all around (VE7NHW: "makes the screen look bigger"). */
    int y0 = MAP_H - TX_BAR_H - MAP_LEGEND_H, cy = y0 + MAP_LEGEND_H / 2;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(0xDDDDDD);
    ld.font  = &sony_18;
    static const struct {
        uint32_t    color;
        int         kind; /* 0 square, 1 line, 2 hollow */
        const char *text;
    } items[] = {{MAP_HEARD, 0, "heard"},
                 {MAP_PATH, 1, "heard you"},
                 {MAP_QSO, 1, "QSO"},
                 {MAP_HEARD, 2, "by call prefix"},
                 {MAP_SELECT, 0, "selected"},
                 {MAP_HOME, 0, "you"}};
    int x = 8, w = 0;
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_point_t sz;
        lv_txt_get_size(&sz, items[i].text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        w += 16 + sz.x + (i + 1 < sizeof(items) / sizeof(items[0]) ? 16 : 0);
    }
    map_rect(x - 4, y0 + 2, w + 8, MAP_LEGEND_H - 3, 0x000000, LV_OPA_50, 0, 0, 0);
    map_take(x - 4, y0 + 2, x + w + 4, y0 + MAP_LEGEND_H);
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (items[i].kind == 0) map_rect(x, cy - 5, 10, 10, items[i].color, MAP_SQUARE_OPA + 60, items[i].color, 1, 0);
        else if (items[i].kind == 2) map_rect(x, cy - 5, 10, 10, items[i].color, LV_OPA_TRANSP, items[i].color, 2, 0);
        else map_rect(x, cy - 1, 12, 3, items[i].color, LV_OPA_COVER, 0, 0, 0);
        lv_point_t sz;
        lv_txt_get_size(&sz, items[i].text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_canvas_draw_text(map_canvas, x + 16, cy - sz.y / 2, sz.x + 4, &ld, items[i].text);
        x += 16 + sz.x + 16;
    }
    char        right[64];
    const char *cont = strcmp(map_my_cont, "NA") == 0   ? "North America"
                       : strcmp(map_my_cont, "SA") == 0 ? "South America"
                       : strcmp(map_my_cont, "EU") == 0 ? "Europe"
                       : strcmp(map_my_cont, "AF") == 0 ? "Africa"
                       : strcmp(map_my_cont, "AS") == 0 ? "Asia"
                       : strcmp(map_my_cont, "OC") == 0 ? "Oceania"
                                                        : "Your continent";
    if (!have_home) snprintf(right, sizeof(right), "Set your grid: APP > QTH");
    else if (map_follow) snprintf(right, sizeof(right), sel_call[0] ? "Following %s" : "Following: select one", sel_call);
    else if (param_i_get(cfg.js8.map_mode()) == JS8_MAP_WORLD) snprintf(right, sizeof(right), "World");
    else if (map_world) snprintf(right, sizeof(right), "World (DX heard)");
    else snprintf(right, sizeof(right), "%s%s", cont, param_i_get(cfg.js8.map_mode()) == JS8_MAP_CLOSE ? " (close-in)" : "");
    if (map_heard_me_only) strncat(right, ", heard me", sizeof(right) - strlen(right) - 1);
    lv_point_t sz;
    lv_txt_get_size(&sz, right, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    ld.color = lv_color_white();
    map_rect(MAP_W - 12 - sz.x, y0 + 2, sz.x + 8, MAP_LEGEND_H - 3, 0x000000, LV_OPA_50, 0, 0, 0);
    map_take(MAP_W - 12 - sz.x, y0 + 2, MAP_W, y0 + MAP_LEGEND_H);
    lv_canvas_draw_text(map_canvas, MAP_W - 8 - sz.x, cy - sz.y / 2, sz.x + 4, &ld, right);
}

/* A small tag ("CQ") pinned on the top right corner of a station's mark
 * (x, y its centre, `side` its size): map_badge_box() reserves the place
 * before the labels are placed, map_badge() draws it last, over rings and
 * outlines. False where the legend, the status line or the stats are. */
static bool map_badge_box_at(int x, int y, int side, const char *text, bool below, lv_area_t *box) {
    lv_point_t sz;
    lv_txt_get_size(&sz, text, &sony_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int h  = LV_MAX(side / 2, 6);
    int bx = LV_CLAMP(2, x + h - 3, MAP_W - sz.x - 8);
    int by = LV_CLAMP(2, below ? y + h - 3 : y - h + 3 - sz.y - 2, MAP_LABEL_BOTTOM - sz.y - 4);
    *box   = (lv_area_t){bx, by, bx + sz.x + 6, by + sz.y + 2};
    if (map_overlaps(box)) return false;
    map_take(box->x1, box->y1, box->x2, box->y2);
    return true;
}

static bool map_badge_box(int x, int y, int side, const char *text, lv_area_t *box) {
    return map_badge_box_at(x, y, side, text, false, box);
}

static void map_badge_ex(const lv_area_t *box, const char *text, uint32_t bg, uint32_t fg) {
    map_rect(box->x1, box->y1, box->x2 - box->x1, box->y2 - box->y1, bg, 235, 0x000000, 1, 3);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(fg);
    ld.font  = &sony_14;
    lv_canvas_draw_text(map_canvas, box->x1 + 3, box->y1 + 1, box->x2 - box->x1, &ld, text);
}

static void map_badge(const lv_area_t *box, const char *text) {
    map_badge_ex(box, text, MAP_CQ_BG, 0x000000);
}

static int  map_stacks_drawn; /* for the harness: count tags on the map */
static char map_stacks_text[96];

/* Top right, lined up under the status line: the stats, then the QRZ line
 * (VE7NHW: the stats on the status line's side). True if either moved. */
static bool map_top_place(void) {
    if (!map_stats_label || !map_status || !map_qrz_label) return false;
    lv_obj_t *above = map_status;
    bool      moved = false;
    lv_obj_t *below[2] = {map_stats_label, map_qrz_label};
    for (int i = 0; i < 2; i++) {
        if (lv_obj_has_flag(below[i], LV_OBJ_FLAG_HIDDEN)) continue;
        lv_obj_update_layout(above); /* its size, before lining up under it */
        lv_coord_t x = lv_obj_get_x(below[i]), y = lv_obj_get_y(below[i]);
        lv_obj_align_to(below[i], above, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 3);
        lv_obj_update_layout(below[i]);
        moved |= x != lv_obj_get_x(below[i]) || y != lv_obj_get_y(below[i]);
        above = below[i];
    }
    return moved;
}

static void map_status_changed(void) {
    if (map_top_place()) map_sig = 0; /* the stats or QRZ moved: redraw the labels around them */
}

/* Top right: "14 heard  5 hear you  DX JA1ABC 7880 km" (the whole Stations
 * list, whatever Show picks). */
static void map_stats(const js8_map_place_t *home) {
    if (!map_stats_label) return;
    int         heard = 0, heard_me = 0;
    double      best  = -1;
    const char *best_call = NULL;
    for (int i = 0; i < st_count; i++) {
        js8_map_place_t p;
        heard++;
        heard_me += st_rows[i].heard_me;
        if (!home || !js8_map_place(st_rows[i].call, st_rows[i].grid, &p)) continue;
        double km = js8_map_distance_km(home->lat, home->lon, p.lat, p.lon);
        if (km > best) {
            best      = km;
            best_call = st_rows[i].call;
        }
    }
    char text[96] = "";
    if (heard) {
        int len = snprintf(text, sizeof(text), "%d heard  %d hear you", heard, heard_me);
        if (best_call && len < (int)sizeof(text))
            snprintf(text + len, sizeof(text) - len, "  DX %s %.0f %s", best_call,
                     param_i_get(cfg.js8.miles()) ? best * 0.621371 : best, param_i_get(cfg.js8.miles()) ? "mi" : "km");
    }
    if (strcmp(lv_label_get_text(map_stats_label), text) != 0) lv_label_set_text(map_stats_label, text);
    if (!text[0]) {
        lv_obj_add_flag(map_stats_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(map_stats_label, LV_OBJ_FLAG_HIDDEN);
    map_top_place();
    map_take(lv_obj_get_x(map_stats_label) - 4, lv_obj_get_y(map_stats_label) - 2, MAP_W,
             lv_obj_get_y(map_stats_label) + lv_obj_get_height(map_stats_label) + 2);
}

/* "  1234 km" (or mi) from home to a place, for labels. */
static void map_dist(char *buf, size_t size, const js8_map_place_t *home, const js8_map_place_t *p) {
    double km = js8_map_distance_km(home->lat, home->lon, p->lat, p->lon);
    snprintf(buf, size, "  %.0f %s", param_i_get(cfg.js8.miles()) ? km * 0.621371 : km, param_i_get(cfg.js8.miles()) ? "mi" : "km");
}

/* Place, fit, draw. The base map is redrawn only when the view moves; the
 * stations only when something shown changed (the pop-ups' flashing
 * included), checked every 250 ms. */
static void map_update(bool force) {
    if (!view_map || !map_canvas || !map_base) return;
    int64_t                now = now_mono_ms();
    js8_map_place_t        home;
    bool                   have_home = js8_map_place(my_call(), my_grid(), &home);
    static js8_map_place_t pl[MAX_ROWS];
    static bool            shown[MAX_ROWS];
    static js8_map_point_t pts[MAX_ROWS];
    static bool            qso[MAX_ROWS]; /* a message to or from them is on the air: red path */
    static uint8_t         age[MAX_ROWS]; /* 0 (just heard) .. 8 (MAP_FADE_MIN or more): how faded */
    static int8_t          grow[MAX_ROWS]; /* by SNR, a few px at most */
    static uint8_t         new_kind[MAX_ROWS]; /* js8_map_new_t: never worked that grid / country */
    static bool            cq[MAX_ROWS];
    unsigned               n = 0;
    int64_t                wall = now_wall_ms();
    bool transmitting = tx_status.state == JS8_TX_KEYING || (tx_status.state == JS8_TX_WAITING && tx_status.frame > 1);
    char tx_to[JS8_RX_CALL_LEN] = "";
    if (transmitting) map_tx_target(tx_status.text, tx_to, sizeof(tx_to));
    for (int i = 0; i < st_count; i++) {
        const js8_station_t *st  = &st_rows[i];
        bool                 sel = sel_call[0] && strcasecmp(st->call, sel_call) == 0;
        qso[i] = map_qso_on(st->call, now) || (tx_to[0] && strcasecmp(st->call, tx_to) == 0);
        shown[i] = js8_map_place(st->call, st->grid, &pl[i]) && (!map_heard_me_only || st->heard_me || sel || qso[i]);
        if (!shown[i]) continue;
        int64_t mins = (wall - st->heard_ms) / 60000;
        age[i]       = st_all_time ? 0 : (uint8_t)LV_CLAMP(0, mins * 8 / MAP_FADE_MIN, 8); /* All time: no fading */
        grow[i]      = (int8_t)LV_CLAMP(-2, (st->snr + 10 + 60) / 6 - 10, 3); /* -24 dB -2 px ... +8 dB +3 px */
        new_kind[i]  = (uint8_t)js8_map_new_kind(st->call, st->grid, NULL, 0);
        cq[i]        = cq_heard_on(st->call, now);
        pts[n].lat = pl[i].lat;
        pts[n].lon = pl[i].lon;
        memcpy(pts[n].continent, pl[i].continent, sizeof(pts[n].continent));
        n++;
    }
    /* Following: you and the selected station only, however far. */
    int follow_i = -1;
    for (int i = 0; map_follow && sel_call[0] && i < st_count; i++)
        if (shown[i] && strcasecmp(st_rows[i].call, sel_call) == 0) follow_i = i;
    js8_map_point_t fp;
    if (follow_i >= 0) {
        fp.lat = pl[follow_i].lat;
        fp.lon = pl[follow_i].lon;
        memcpy(fp.continent, pl[follow_i].continent, sizeof(fp.continent));
    }
    js8_map_view_t v;
    bool world = follow_i >= 0 ? js8_map_choose_view(JS8_MAP_AUTO, have_home ? home.lat : 0, have_home ? home.lon : 0,
                                                     map_my_cont, &fp, 1, MAP_W, MAP_FIT_H, &v)
                               : js8_map_choose_view((js8_map_mode_t)param_i_get(cfg.js8.map_mode()), have_home ? home.lat : 0,
                                                     have_home ? home.lon : 0, map_my_cont, pts, n, MAP_W, MAP_FIT_H, &v);
    /* Drawn over the whole area; the fitted part sits above the legend. */
    v.merc_c -= (MAP_H / 2.0 - MAP_FIT_H / 2.0) / v.px_deg;
    v.height = MAP_H;
    bool base_new = !map_view_ok || v.lon_c != map_view.lon_c || v.merc_c != map_view.merc_c ||
                    v.px_deg != map_view.px_deg;
    if (base_new) {
        if (!map_data || !js8_map_render_base(map_data, &v, map_base, MAP_W))
            for (size_t i = 0; i < (size_t)MAP_W * MAP_H; i++) map_base[i] = 0xFF000000u | MAP_OCEAN;
        map_view    = v;
        map_view_ok = true;
        map_world   = world;
        LV_LOG_USER("JS8 map: view lon %.1f merc %.1f, %.2f px/deg, %u stations%s", v.lon_c, v.merc_c, v.px_deg, n,
                    world ? ", world" : "");
    }

    map_strip_update();

    /* Everything drawn below, hashed: unchanged means nothing to redraw. */
    bool     blink = (now / 500) % 2 == 0;
    uint32_t sig   = 2166136261u;
    sig            = map_hash(sig, &map_follow, sizeof(map_follow));
    static int8_t talk_ia[MAP_TALK], talk_ib[MAP_TALK];
    static uint8_t talk_age[MAP_TALK];
    for (int t = 0; t < MAP_TALK; t++) { /* both ends on the map, else not drawn */
        talk_ia[t] = talk_ib[t] = -1;
        if (!map_talk[t].ms || now - map_talk[t].ms >= MAP_TALK_MS) continue;
        for (int i = 0; i < st_count; i++) {
            if (!shown[i]) continue;
            if (strcasecmp(st_rows[i].call, map_talk[t].a) == 0) talk_ia[t] = (int8_t)i;
            if (strcasecmp(st_rows[i].call, map_talk[t].b) == 0) talk_ib[t] = (int8_t)i;
        }
        talk_age[t] = (uint8_t)((now - map_talk[t].ms) * 8 / MAP_TALK_MS);
        if (talk_ia[t] < 0 || talk_ib[t] < 0) continue;
        sig = map_hash(sig, map_talk[t].a, strlen(map_talk[t].a));
        sig = map_hash(sig, map_talk[t].b, strlen(map_talk[t].b));
        sig = map_hash(sig, &talk_age[t], sizeof(talk_age[t]));
    }
    sig            = map_hash(sig, &transmitting, sizeof(transmitting));
    bool my_cq     = (tx_cq && transmitting) || my_cq_until_ms > now; /* sending one, or sent in the last 5 min */
    sig            = map_hash(sig, &my_cq, sizeof(my_cq));
    sig            = map_hash(sig, sel_call, strlen(sel_call));
    sig            = map_hash(sig, &map_heard_me_only, sizeof(map_heard_me_only));
    sig            = map_hash(sig, &have_home, sizeof(have_home));
    sig            = map_hash(sig, &map_qrz_n, sizeof(map_qrz_n)); /* labels keep off the QRZ line */
    for (int i = 0; i < st_count; i++) {
        if (!shown[i]) continue;
        const js8_station_t *st  = &st_rows[i];
        bool                 pop = map_popup_on(st->call, now) || map_ring_on(st->call, now);
        sig                      = map_hash(sig, st->call, strlen(st->call));
        sig                      = map_hash(sig, st->grid, strlen(st->grid));
        sig                      = map_hash(sig, &st->snr, sizeof(st->snr));
        sig                      = map_hash(sig, &st->heard_me, sizeof(st->heard_me));
        sig                      = map_hash(sig, &st->reported_snr, sizeof(st->reported_snr));
        sig                      = map_hash(sig, &pop, sizeof(pop));
        sig                      = map_hash(sig, &qso[i], sizeof(qso[i]));
        sig                      = map_hash(sig, &age[i], sizeof(age[i]));
        sig                      = map_hash(sig, &new_kind[i], sizeof(new_kind[i]));
        sig                      = map_hash(sig, &cq[i], sizeof(cq[i]));
        if (pop) sig = map_hash(sig, &blink, sizeof(blink));
    }
    if (!force && !base_new && sig == map_sig) return;
    map_sig = sig;

    memcpy(map_px, map_base, (size_t)MAP_W * MAP_H * sizeof(uint32_t));
    map_ntaken = 0;
    map_legend(have_home);
    map_stats(have_home ? &home : NULL);
    if (map_status) map_take(MAP_W - map_edge - 4 - lv_obj_get_width(map_status), 0, MAP_W, map_edge + lv_obj_get_height(map_status));
    if (map_qrz_n && map_qrz_label) { /* the QRZ line under it */
        lv_obj_update_layout(map_qrz_label);
        map_take(lv_obj_get_x(map_qrz_label) - 4, lv_obj_get_y(map_qrz_label) - 2,
                 lv_obj_get_x(map_qrz_label) + lv_obj_get_width(map_qrz_label) + 4,
                 lv_obj_get_y(map_qrz_label) + lv_obj_get_height(map_qrz_label) + 2);
    }

    /* Paths first, under the squares: GridTracker's curves to the stations
     * that heard you in blue, then red on top for a message to or from you
     * on the air right now. */
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.round_start = 1;
    line.round_end   = 1;

    /* The band's other conversations: thin grey great circles between the
     * two stations, fading over 10 min. */
    line.width     = 2;
    line.color     = lv_color_hex(MAP_TALK_COLOR);
    map_talk_drawn = 0;
    for (int t = 0; t < MAP_TALK; t++) {
        if (talk_ia[t] < 0 || talk_ib[t] < 0) continue;
        float      xs[MAP_PATH_N + 1], ys[MAP_PATH_N + 1];
        lv_point_t p[MAP_PATH_N + 1];
        const js8_map_place_t *a = &pl[talk_ia[t]], *b = &pl[talk_ib[t]];
        js8_map_path(&map_view, a->lat, a->lon, b->lat, b->lon, MAP_PATH_N, xs, ys);
        for (int k = 0; k <= MAP_PATH_N; k++) {
            p[k].x = (lv_coord_t)LV_CLAMP(-4000, (int)xs[k], 4000);
            p[k].y = (lv_coord_t)LV_CLAMP(-4000, (int)ys[k], 4000);
        }
        line.opa = (lv_opa_t)(190 - talk_age[t] * 140 / 8);
        lv_canvas_draw_line(map_canvas, p, MAP_PATH_N + 1, &line);
        map_talk_drawn++;
    }
    line.opa = LV_OPA_COVER;

    line.width    = 3;
    map_qso_drawn = 0;
    map_npaths    = 0;
    for (int pass = 0; have_home && pass < 2; pass++) for (int i = 0; i < st_count; i++) {
        if (!shown[i] || (pass == 0 ? !st_rows[i].heard_me || qso[i] : !qso[i])) continue;
        map_qso_drawn += pass;
        line.color = lv_color_hex(pass == 0 ? MAP_PATH : MAP_QSO);
        float      xs[MAP_PATH_N + 1], ys[MAP_PATH_N + 1];
        lv_point_t p[MAP_PATH_N + 1];
        js8_map_path(&map_view, home.lat, home.lon, pl[i].lat, pl[i].lon, MAP_PATH_N, xs, ys);
        for (int k = 0; k <= MAP_PATH_N; k++) {
            p[k].x = (lv_coord_t)LV_CLAMP(-4000, (int)xs[k], 4000);
            p[k].y = (lv_coord_t)LV_CLAMP(-4000, (int)ys[k], 4000);
        }
        lv_canvas_draw_line(map_canvas, p, MAP_PATH_N + 1, &line);
        if (pass) map_path_keep(st_rows[i].call, p); /* for the running dots */
    }

    /* The stations: squares, hollow boxes, the far ends' dots. */
    static int cx[MAX_ROWS], cy[MAX_ROWS], side[MAX_ROWS];
    int        sel_i = -1;
    for (int i = 0; i < st_count; i++) {
        if (!shown[i]) continue;
        bool sel = sel_call[0] && strcasecmp(st_rows[i].call, sel_call) == 0;
        if (sel) sel_i = i;
        /* Fading to 30 % over MAP_FADE_MIN unheard; never while selected,
         * popping up or in a QSO. */
        bool  lively = sel || qso[i] || map_popup_on(st_rows[i].call, now) || map_ring_on(st_rows[i].call, now);
        int   f      = lively ? 100 : 100 - age[i] * 70 / 8;
        bool  fresh  = new_kind[i] != JS8_MAP_NEW_NONE;
        side[i] = map_station_mark(st_rows[i].grid, &pl[i], sel ? MAP_SELECT : MAP_HEARD,
                                   (lv_opa_t)((sel ? 200 : MAP_SQUARE_OPA) * f / 100),
                                   fresh && !sel ? MAP_NEW_BORDER : sel ? MAP_SELECT : MAP_HEARD,
                                   (lv_opa_t)(255 * f / 100), sel ? 0 : grow[i], &cx[i], &cy[i]);
        if (have_home && (st_rows[i].heard_me || qso[i]))
            map_rect(cx[i] - 2, cy[i] - 2, 5, 5, qso[i] ? MAP_QSO : MAP_PATH, LV_OPA_COVER, 0, 0, LV_RADIUS_CIRCLE);
    }

    /* You; outlined red while transmitting (as the TX bar turns red), also
     * between the frames of a longer message; the "CQ" tag while you call
     * CQ and for 5 min after, as other stations get it. */
    bool      my_badge = false;
    lv_area_t my_badge_box;
    if (have_home) {
        int             hx, hy;
        js8_map_place_t me = home;
        me.approx          = strlen(my_grid()) < 4;
        int hside = map_station_mark(my_grid(), &me, MAP_HOME, 220, MAP_HOME, LV_OPA_COVER, 0, &hx, &hy);
        if (transmitting) map_rect(hx - 10, hy - 10, 21, 21, 0, LV_OPA_TRANSP, MAP_TX, 3, 0);
        map_tx_drawn = transmitting;
        my_badge     = my_cq && map_badge_box(hx, hy, transmitting ? 21 : hside, "CQ", &my_badge_box);
        map_take(hx - 6, hy - 6, hx + 6, hy + 6); /* no label over you */
        map_label(hx, hy, my_call(), MAP_HOME, 0x000000, 170, 8, true);
    }

    /* "CQ" on the corner of stations that called CQ in the last 5 min:
     * reserved now, before the labels, drawn last. */
    static bool      badge[MAX_ROWS];
    static lv_area_t badge_box[MAX_ROWS];
    for (int i = 0; i < st_count; i++)
        badge[i] = shown[i] && cq[i] && map_badge_box(cx[i], cy[i], i == sel_i ? 19 : side[i], "CQ", &badge_box[i]);

    /* Stations on the same spot (the same grid square, or the same call
     * area for those placed by callsign) hide each other: a count tag on
     * the mark's bottom right corner, as the CQ tag on the top right,
     * says how many are there (VE7NHW). On the first of them; reserved
     * now, before the labels, drawn last. */
    static bool      stack_tag[MAX_ROWS];
    static lv_area_t stack_box[MAX_ROWS];
    static char      stack_text[MAX_ROWS][8];
    map_stacks_drawn   = 0;
    map_stacks_text[0] = '\0';
    for (int i = 0; i < st_count; i++) {
        stack_tag[i] = false;
        if (!shown[i]) continue;
        bool first = true;
        int  n = 0, gside = 0;
        for (int j = 0; j < st_count && first; j++) {
            if (!shown[j] || abs(cx[j] - cx[i]) > MAP_STACK_PX || abs(cy[j] - cy[i]) > MAP_STACK_PX) continue;
            if (j < i) first = false;
            n++;
            gside = LV_MAX(gside, j == sel_i ? 19 : side[j]);
        }
        if (!first || n < 2) continue;
        snprintf(stack_text[i], sizeof(stack_text[i]), "%d", n);
        stack_tag[i] = map_badge_box_at(cx[i], cy[i], gside, stack_text[i], true, &stack_box[i]);
        size_t len = strlen(map_stacks_text);
        snprintf(map_stacks_text + len, sizeof(map_stacks_text) - len, "%s%s:%d", len ? " " : "",
                 st_rows[i].grid[0] && !pl[i].approx ? st_rows[i].grid : pl[i].where, n);
    }

    /* The selected station, red, with its label. */
    char dist[24] = "";
    if (sel_i >= 0) {
        const js8_station_t *st = &st_rows[sel_i];
        map_rect(cx[sel_i] - 9, cy[sel_i] - 9, 19, 19, 0, LV_OPA_TRANSP, MAP_SELECT, 2, 0);
        map_take(cx[sel_i] - 10, cy[sel_i] - 10, cx[sel_i] + 10, cy[sel_i] + 10);
        char text[112];
        int  len = snprintf(text, sizeof(text), "%s  %s  %+d dB", st->call, pl[sel_i].where, st->snr);
        if (st->heard_me && st->has_reported_snr)
            len += snprintf(text + len, sizeof(text) - len, "  hears you %+d", st->reported_snr);
        if (have_home) {
            map_dist(dist, sizeof(dist), &home, &pl[sel_i]);
            len += snprintf(text + len, sizeof(text) - len, "%s", dist);
            double az = js8_map_bearing_deg(home.lat, home.lon, pl[sel_i].lat, pl[sel_i].lon);
            if (len < (int)sizeof(text)) /* beam heading, short path; following, the long path's too */
                snprintf(text + len, sizeof(text) - len, map_follow ? "  az %.0f  LP %.0f" : "  az %.0f", az,
                         fmod(az + 180.0, 360.0));
        }
        map_label(cx[sel_i], cy[sel_i], text, 0xFFFFFF, 0x6E0000, 225, 13, true);
    }

    /* The flashing ring: a station calling you (while its message comes
     * in, selected or not), and new stations, which also get their details
     * for 8 s. */
    for (int i = 0; i < st_count; i++) {
        bool ring = map_ring_on(st_rows[i].call, now), fresh = map_popup_on(st_rows[i].call, now);
        if (!shown[i] || (!ring && !fresh)) continue;
        if (blink) {
            map_rect(cx[i] - 9, cy[i] - 9, 19, 19, 0, LV_OPA_TRANSP, 0xFFFFFF, 2, LV_RADIUS_CIRCLE);
            map_rect(cx[i] - 13, cy[i] - 13, 27, 27, 0, LV_OPA_TRANSP, 0x9A9A9A, 2, LV_RADIUS_CIRCLE);
        }
        if (!fresh || i == sel_i) continue;
        char text[112];
        dist[0] = '\0';
        if (have_home) map_dist(dist, sizeof(dist), &home, &pl[i]);
        /* "NEW DXCC Japan  JA1ABC  PM95 ...", "NEW GRID  K9DEF  EN52 ...":
         * never worked (the QSO log), or just new since power-on. */
        char what[48] = "", head[64] = "NEW";
        js8_map_new_t kind = js8_map_new_kind(st_rows[i].call, st_rows[i].grid, what, sizeof(what));
        if (kind == JS8_MAP_NEW_DXCC)
            snprintf(head, sizeof(head), strcmp(what, pl[i].where) ? "NEW DXCC %s" : "NEW DXCC", what);
        if (kind == JS8_MAP_NEW_GRID) snprintf(head, sizeof(head), "NEW GRID");
        snprintf(text, sizeof(text), "%s  %s  %s  %+d dB%s", head, st_rows[i].call, pl[i].where, st_rows[i].snr,
                 dist);
        map_label(cx[i], cy[i], text, 0xFFFFFF, 0x005A00, 225, 15, false); /* only where it fits: the ring shows anyway */
    }

    for (int i = 0; i < st_count; i++)
        if (badge[i]) map_badge(&badge_box[i], "CQ");
    for (int i = 0; i < st_count; i++)
        if (stack_tag[i]) {
            map_badge_ex(&stack_box[i], stack_text[i], MAP_STACK_BG, 0x000000);
            map_stacks_drawn++;
        }
    if (my_badge) map_badge(&my_badge_box, "CQ");
    map_corners_apply();
    lv_obj_invalidate(map_canvas);
}

/* For tools/js8_ui_harness: the count tags on the map ("CN89:3 DM43:2",
 * a spot by its first station's grid or place), how many drawn. */
int dialog_js8_map_stacks(const char **text) {
    if (text) *text = map_stacks_text;
    return map_stacks_drawn;
}

/* For tools/js8_ui_harness: is the map showing, is it a world view, how
 * many new-station pop-ups are up, is the transmitting outline drawn. */
bool dialog_js8_map_state(bool *world, int *popups, bool *tx_outline, int *qso_paths, int *qrz) {
    int64_t now = now_mono_ms();
    int     n   = 0;
    for (int i = 0; i < MAP_POPUPS; i++) n += (map_popups[i].until_ms > now) + (map_qso[i].ring_ms > now);
    if (world) *world = map_world || param_i_get(cfg.js8.map_mode()) == JS8_MAP_WORLD;
    if (popups) *popups = n;
    if (tx_outline) *tx_outline = map_tx_drawn;
    if (qso_paths) *qso_paths = map_qso_drawn;
    if (qrz) *qrz = map_qrz_n;
    return view_map;
}

/* For tools/js8_ui_harness: grey conversation lines drawn, your dot (-1
 * hidden, else its x), incoming dots shown so far, the strip's lines,
 * following. */
int dialog_js8_map_extra(int *talks, int *my_dot_x, int *in_dots, const char **strip0, const char **strip1,
                         bool *follow) {
    int n = map_pulses_seen;
    if (talks) *talks = map_talk_drawn;
    if (my_dot_x)
        *my_dot_x = !map_dots[0] || lv_obj_has_flag(map_dots[0], LV_OBJ_FLAG_HIDDEN) ? -1 : lv_obj_get_x(map_dots[0]);
    if (in_dots) *in_dots = n;
    if (strip0) *strip0 = map_strip[0] ? lv_label_get_text(map_strip[0]) : "";
    if (strip1) *strip1 = map_strip[1] ? lv_label_get_text(map_strip[1]) : "";
    if (follow) *follow = map_follow;
    return view_map;
}

/* For tools/js8_ui_harness: the stats line. */
const char *dialog_js8_map_stats(void) {
    return map_stats_label ? lv_label_get_text(map_stats_label) : "";
}

/* Page 3's second button: Sort in the Stations view, the map's view on
 * the map, nothing over the messages. */
static const char *const st_sort_names[JS8_ST_SORT_COUNT] = {"Heard you", "SNR", "Time", "Distance", "QSO"};

static js8_st_sort_t st_sort(void) {
    return (js8_st_sort_t)(param_i_get(cfg.js8.st_sort()) % JS8_ST_SORT_COUNT);
}

static const char *map_view_label_getter(void) {
    static char buf[24];
    if (!view_map && view_stations) {
        snprintf(buf, sizeof(buf), "Sort:\n%s", st_sort_names[st_sort()]);
        return buf;
    }
    if (!view_map) return ""; /* only while the map shows */
    snprintf(buf, sizeof(buf), "Map:\n%s",
             map_follow ? "Follow" : map_mode_names[param_i_get(cfg.js8.map_mode()) < 3 ? param_i_get(cfg.js8.map_mode()) : 0]);
    return buf;
}

/* The Stations view: Heard you -> SNR -> Time -> Distance -> QSO. The
 * station selected stays selected (rebuild_station_rows keeps the cursor
 * on it). */
static void st_sort_next(button_data_t *btn) {
    js8_st_sort_t next = (js8_st_sort_t)((st_sort() + 1) % JS8_ST_SORT_COUNT);
    param_i_set(cfg.js8.st_sort(), next);
    buttons_refresh(btn);
    rebuild_rows();
    static const char *const what[JS8_ST_SORT_COUNT] = {
        "Stations: who heard you first, then the newest", "Stations: strongest first",
        "Stations: newest first", "Stations: farthest first (no grid: last)",
        "Stations: only those you've had a QSO with (History or your log), newest first"};
    if (next == JS8_ST_SORT_DISTANCE && !my_grid()[0]) msg_update_text_fmt("Distance needs your grid: APP > QTH");
    else msg_update_text_fmt("%s", what[next]);
}

/* The map: Auto -> Close-in -> World. */
static void map_view_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    if (!view_map && view_stations) {
        st_sort_next(btn);
        return;
    }
    if (!view_map) return;
    if (map_follow) { /* a press leaves Follow, back to the view it was on */
        map_follow = false;
        buttons_refresh(btn);
        map_update(true);
        msg_update_text_fmt("Map: %s", map_mode_names[param_i_get(cfg.js8.map_mode()) < 3 ? param_i_get(cfg.js8.map_mode()) : 0]);
        return;
    }
    param_i_set(cfg.js8.map_mode(), (param_i_get(cfg.js8.map_mode()) + 1) % 3);
    buttons_refresh(btn);
    map_update(true);
    static const char *const what[3] = {"Map: your continent, the world when DX is heard",
                                        "Map: your continent only", "Map: the whole world"};
    msg_update_text_fmt("%s", what[param_i_get(cfg.js8.map_mode()) < 3 ? param_i_get(cfg.js8.map_mode()) : 0]);
}

/* Hold: follow the selected station (the view frames you and them, the
 * label adds the long path); hold or press again to stop. */
static void map_follow_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    if (!view_map) return;
    map_follow = !map_follow;
    buttons_refresh(btn);
    map_update(true);
    if (!map_follow) msg_update_text_fmt("Map: %s", map_mode_names[param_i_get(cfg.js8.map_mode()) < 3 ? param_i_get(cfg.js8.map_mode()) : 0]);
    else if (sel_call[0]) msg_update_text_fmt("Map: following %s (press Map to stop)", sel_call);
    else msg_update_text_fmt("Map: following the selected station: turn the MFK to pick one");
}

/* ---- Buttons ---------------------------------------------------------- */

static const char *show_label_getter(void) {
    static const char *const labels[SHOW_COUNT] = {"Show:\nAll", "Show:\nNo HB", "Show:\nDirected"};
    if (view_map) return map_heard_me_only ? "Show:\nHeard me" : "Show:\nAll heard";
    return labels[show];
}

/* Press: next filter. In the Stations view, where the filter means
 * nothing, the first press goes back to the messages as they were. */
static void show_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    if (view_map) { /* the map: everyone heard, or only who heard us */
        map_heard_me_only = !map_heard_me_only;
        buttons_refresh(btn);
        map_update(true);
        msg_update_text_fmt(map_heard_me_only ? "Map: stations that heard you" : "Map: every station heard");
        return;
    }
    if (view_stations) {
        view_stations = false;
        cursor_freq   = -1;
        map_qrz_n     = 0; /* the list shows what they sent, as from the map */
        map_qrz_show();
        if (btn_stations.disp_btn) buttons_refresh(&btn_stations);
        if (btn_map_view.disp_btn) buttons_refresh(&btn_map_view);
        rebuild_rows();
        msg_update_text_fmt("Messages (%s)", show == SHOW_ALL ? "all" : show == SHOW_DIRECTED ? "directed" : "no HB");
        return;
    }
    show = (show + 1) % SHOW_COUNT;
    buttons_refresh(btn);
    rebuild_rows();
}

static void clear_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    (void)btn;
    hist_head = hist_count = 0;
    tx_row_slot           = -1;
    info_head = info_count = 0;
    js8_stations_clear(stations);
    js8_rx_clear(rx);
    js8_wf_clear();
    wf_queue_clear();
    marks_reset();
    clear_selection();
    rebuild_rows();
    update_status();
}

/* ---- Time: automatic time sync and the search ------------------------- */

/* JS8's time is the radio's clock plus a drift (js8_set_drift_ms): receive
 * windows, transmit slots and everything the app times go by it. The
 * radio's clock is never changed; the drift lasts until power-off.
 *
 * Auto (press Time, page 4; on to start): desktop JS8Call's Automatic Time
 * Drift, which the JS8Call-improved Android app has as Auto time sync:
 * every decoded Normal or Slow frame, heartbeats and CQs included, feeds a
 * 60-frame average (src/js8/timesync.hpp), the drift after each decode
 * pass. One heartbeat is enough to start. Set only when it moved
 * TIME_AUTO_MIN_MS or more (each change re-snaps the decoder's windows),
 * and never while sending (the frames still to go would move).
 *
 * Hold Time: the search, for a clock too far off for anything to decode
 * (more than about 2.5 s): the latest 15 s decoded every 4 s for up to
 * TIME_SEARCH_S, wherever the slots fall; the first decode sets the drift.
 * Desktop decodes every second for this while its automatic drift starts. */
#define TIME_AUTO_MIN_MS 50
#define TIME_SEARCH_S    300

static bool             time_found_pending; /* a search's find, waiting for TX to end */
static js8_rx_search_t  time_found;

static bool time_searching(void) {
    return rx && js8_rx_searching(rx);
}

static const char *time_label_getter(void) {
    static char buf[32];
    if (time_searching()) return "Time:\nSearching";
    const char *mode = param_i_get(cfg.js8.tsync_auto()) ? "Auto" : "Off";
    int64_t     d    = js8_drift_ms();
    if (d) snprintf(buf, sizeof(buf), "Time: %s\n%+.1fs", mode, d / 1000.0);
    else snprintf(buf, sizeof(buf), "Time:\n%s", mode);
    return buf;
}

/* The Time button (marked while searching) and the status line's drift. */
static void time_refresh(void) {
    buttons_mark(&btn_time, time_searching());
    update_status();
}

/* After a decode pass: Auto's new drift. */
static void ui_auto_drift(int64_t drift_ms, unsigned frames) {
    if (!dialog.run || !param_i_get(cfg.js8.tsync_auto())) return;
    if (tx_active || js8_tx_busy(tx)) return; /* the next pass tries again */
    if (time_searching()) { /* decoding again: no need to search */
        js8_rx_search_stop(rx);
        msg_update_text_fmt("Time: stations decode again, search stopped");
    }
    int64_t change = drift_ms - js8_drift_ms();
    if (llabs(change) < TIME_AUTO_MIN_MS) {
        time_refresh();
        return;
    }
    js8_set_drift_ms(drift_ms);
    js8_log("auto time: drift %+lld ms (moved %+lld ms, %u frames)", (long long)drift_ms, (long long)change, frames);
    if (llabs(change) >= 500)
        add_info_row("Time: JS8 time %+.1f s to match the decodes (drift %+.1f s)", change / 1000.0, drift_ms / 1000.0);
    time_refresh();
}

/* A search's find goes in when nothing is being sent. */
static void time_apply_found(void) {
    if (!time_found_pending || js8_tx_busy(tx)) return;
    time_found_pending = false;
    int64_t change     = time_found.drift_ms - js8_drift_ms();
    js8_set_drift_ms(time_found.drift_ms);
    js8_rx_auto_sync_restart(rx, time_found.drift_ms, true); /* Auto goes on from it */
    msg_update_text_fmt("Time: found %s - JS8 time %+.1f s (drift %+.1f s)", time_found.text, change / 1000.0,
                        time_found.drift_ms / 1000.0);
    add_info_row("Time search: %s (%+d dB, %.0f Hz): JS8 time %+.1f s, drift %+.1f s", time_found.text,
                 time_found.snr, time_found.freq_hz, change / 1000.0, time_found.drift_ms / 1000.0);
    js8_log("time search: drift %+d ms (moved %+lld ms) from \"%s\"", (int)time_found.drift_ms, (long long)change,
            time_found.text);
    time_refresh();
}

static void ui_search_done(const js8_rx_search_t *r) {
    if (!dialog.run) return;
    if (!r->found) {
        msg_update_text_fmt("Time: nothing decoded in %d min - is the band open? Far off: set the clock in SETTINGS",
                            TIME_SEARCH_S / 60);
        add_info_row("Time search: nothing decoded in %d minutes", TIME_SEARCH_S / 60);
        time_refresh();
        return;
    }
    time_found         = *r;
    time_found_pending = true;
    if (js8_tx_busy(tx)) msg_update_text_fmt("Time: found %s - set when sending ends", r->text);
    time_apply_found();
}

/* Press: Auto on or off. */
static void time_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (popup_guard()) return;
    bool on = !param_i_get(cfg.js8.tsync_auto());
    param_i_set(cfg.js8.tsync_auto(), on);
    js8_rx_set_auto_sync(rx, on);
    if (on) msg_update_text_fmt("Time: Auto - JS8's timing follows the stations decoded (hold: search)");
    else msg_update_text_fmt("Time: Auto off - JS8's timing stays at drift %+.1f s", js8_drift_ms() / 1000.0);
    buttons_refresh(&btn_time);
}

/* Hold: search for the band's timing (again: stop it). */
static void time_hold_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (popup_guard() || !rx) return;
    if (time_searching()) {
        js8_rx_search_stop(rx);
        msg_update_text_fmt("Time: search stopped");
        time_refresh();
        return;
    }
    if (js8_tx_busy(tx)) {
        msg_update_text_fmt("Not while sending - Stop TX first");
        return;
    }
    if (decoder_setup) { /* its decodes would wait behind the setup's measuring */
        msg_update_text_fmt("Time: after the decoder setup (one time only)");
        return;
    }
    js8_rx_search_start(rx, TIME_SEARCH_S);
    msg_update_text_fmt("Time: searching for the band's timing (up to %d min): the first station heard sets it",
                        TIME_SEARCH_S / 60);
    time_refresh();
}

/* Settings > Reset time drift: JS8 back on the radio's clock (desktop's
 * Reset). With Auto on, the next decode sets the drift again. */
static void time_sync_reset(void) {
    user_touch();
    if (js8_tx_busy(tx)) {
        msg_update_text_fmt("Not while sending - Stop TX first");
        return;
    }
    if (time_searching()) js8_rx_search_stop(rx);
    time_found_pending = false;
    js8_rx_auto_sync_restart(rx, 0, false);
    if (!js8_drift_ms()) {
        msg_update_text_fmt("No drift: JS8 is on the radio's clock");
        time_refresh();
        return;
    }
    js8_set_drift_ms(0);
    msg_update_text_fmt(param_i_get(cfg.js8.tsync_auto()) ? "Drift reset: Auto starts again from the next decode"
                                                          : "Drift reset: JS8 back on the radio's clock");
    add_info_row("Time Sync: drift reset");
    time_refresh();
}

static void reply_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    (void)btn;
    char  call[JS8_RX_CALL_LEN];
    float freq;
    int   snr;
    if (!selected_station(call, sizeof(call), &freq, &snr)) {
        msg_update_text_fmt("Select a station first (MFK)");
        return;
    }
    /* AUTO off and this station asked us something: offer the answer. */
    int o = offer_find(call);
    if (o >= 0) {
        char text[JS8_RX_TEXT_LEN];
        snprintf(text, sizeof(text), "%s", offers[o].text);
        deliver_pending.id = offers[o].deliver_id;
        snprintf(deliver_pending.group_call, sizeof(deliver_pending.group_call), "%s", offers[o].deliver_group_call);
        snprintf(deliver_pending.text, sizeof(deliver_pending.text), "%s", offers[o].text);
        offers[o].call[0] = '\0';
        apply_hold(freq);
        compose_open(text);
        speed_warn();
        return;
    }
    char prefill[JS8_RX_CALL_LEN + 2];
    snprintf(prefill, sizeof(prefill), "%s ", call);
    apply_hold(freq);
    compose_open(prefill);
    speed_warn();
}

static void send_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    (void)btn;
    compose_open(NULL);
}

static void stop_tx_cb(button_data_t *btn) {
    user_touch();
    popup_guard(); /* close any list, but always stop */
    (void)btn;
    if (!js8_tx_busy(tx)) {
        msg_update_text_fmt("Not sending");
        return;
    }
    js8_tx_stop(tx);
    msg_update_text_fmt("Stopping TX");
    auto_cq_stop("TX stopped");
}

/* "CALL HW CPY?" to the selected station: desktop's usual first answer to a
 * CQ ("how do you copy?"). Stopping TX is ESC or the top knob's press. */
static void hw_cpy_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (popup_guard()) return;
    char  call[JS8_RX_CALL_LEN];
    float freq;
    int   snr;
    if (!selected_station(call, sizeof(call), &freq, &snr)) {
        msg_update_text_fmt("Select a station first (MFK)");
        return;
    }
    char text[JS8_RX_CALL_LEN + 12];
    snprintf(text, sizeof(text), "%s HW CPY?", call);
    apply_hold(freq);
    if (tx_queue(text)) speed_warn();
}

/* CQ with the 4-character grid, as desktop JS8Call sends it. */
static bool send_cq(bool automatic) {
    char text[32];
    snprintf(text, sizeof(text), "CQ CQ CQ %.4s", my_grid());
    if (!tx_queue_at(text, param_i_get(cfg.js8.tx_freq()), automatic)) return false;
    tx_cq = true;
    return true;
}

static int64_t cq_interval_ms(void) {
    unsigned m = param_i_get(cfg.js8.cq_interval());
    if (m < CQ_MIN_INTERVAL || m > CQ_MAX_INTERVAL) m = CQ_MIN_INTERVAL;
    return (int64_t)m * 60000;
}

static const char *cq_label_getter(void) {
    static char buf[24];
    if (cq_adjusting) {
        snprintf(buf, sizeof(buf), "CQ: knob\n< %u min >", param_i_get(cfg.js8.cq_interval()));
        return buf;
    }
    if (!auto_cq) return "CQ";
    if (js8_tx_busy(tx)) return "CQ auto:\nsending";
    int secs = (int)((auto_cq_next_ms - now_wall_ms() + 999) / 1000);
    if (secs < 1) return "CQ auto:\nnow";
    if (secs < 60) snprintf(buf, sizeof(buf), "CQ auto:\n%d s", secs);
    else snprintf(buf, sizeof(buf), "CQ auto:\n%d:%02d", secs / 60, secs % 60);
    return buf;
}

/* Setting the auto CQ interval with the main knob, as for HB: press CQ
 * (or wait) to finish. */
static void cq_adjust_start(void) {
    hb_adjusting = false;
    cq_adjusting = true;
    cq_adjust_ms = now_wall_ms();
    if (btn_hb.disp_btn) buttons_refresh(&btn_hb);
    if (btn_cq.disp_btn) buttons_refresh(&btn_cq);
    update_tx_bar();
}

static void cq_adjust_end(void) {
    if (!cq_adjusting) return;
    cq_adjusting = false;
    if (btn_cq.disp_btn) buttons_refresh(&btn_cq);
    update_tx_bar();
    if (auto_cq) msg_update_text_fmt("Auto CQ %u min after each CQ (press CQ to stop)", param_i_get(cfg.js8.cq_interval()));
}

/* Knob turn while setting it: the next CQ moves with it. */
static void cq_adjust_turn(int32_t diff) {
    int v = (int)param_i_get(cfg.js8.cq_interval()) + (diff > 0 ? 1 : -1);
    if (v < CQ_MIN_INTERVAL) v = CQ_MIN_INTERVAL;
    if (v > CQ_MAX_INTERVAL) v = CQ_MAX_INTERVAL;
    param_i_set(cfg.js8.cq_interval(), (uint16_t)v);
    cq_adjust_ms = now_wall_ms();
    if (auto_cq && !js8_tx_busy(tx)) auto_cq_next_ms = auto_cq_from_ms + cq_interval_ms();
    if (btn_cq.disp_btn) buttons_refresh(&btn_cq);
    update_tx_bar();
}

/* Press: one CQ; while setting the interval, done; with auto CQ on, back
 * to manual (auto off). */
static void cq_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    (void)btn;
    if (cq_adjusting) {
        cq_adjust_end();
        return;
    }
    if (auto_cq) {
        auto_cq_stop("manual");
        return;
    }
    send_cq(false);
}

/* Hold: auto CQ - one now, then one js8_cq_interval minutes after each
 * ends, until someone answers, you press CQ, reply to someone, stop TX or
 * change band. The knob sets the interval straight away; holding CQ while
 * auto CQ runs sets it again. */
static void cq_hold_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    if (auto_cq) {
        cq_adjust_start();
        return;
    }
    /* Busy sending: the first CQ comes an interval from now (not right
     * after; left so, your call). Otherwise one now, and the interval
     * starts when it ends (ui_tx_done). */
    int64_t now  = now_wall_ms();
    bool    busy = tx_active;
    if (!busy && !send_cq(false)) return;
    auto_cq         = true;
    auto_cq_from_ms = now;
    auto_cq_next_ms = busy ? now + cq_interval_ms() : now;
    buttons_refresh(btn);
    add_info_row("Auto CQ on");
    cq_adjust_start();
    msg_update_text_fmt("Auto CQ on: turn the knob to set the minutes, press CQ when done");
}

static void auto_cq_stop(const char *why) {
    if (!auto_cq) return;
    auto_cq      = false;
    cq_adjusting = false;
    if (btn_cq.disp_btn) buttons_refresh(&btn_cq);
    msg_update_text_fmt("Auto CQ off: %s", why);
    add_info_row("Auto CQ off: %s", why);
    update_tx_bar();
}

/* Once a second: the next CQ when it's due (and the button's countdown). */
static void auto_cq_tick(void) {
    if (cq_adjusting && now_wall_ms() - cq_adjust_ms > HB_ADJUST_MS) cq_adjust_end();
    if (!auto_cq) return;
    int64_t now = now_wall_ms();
    if (js8_auto_idle(autop, now)) {
        auto_cq_stop("idle for an hour");
        return;
    }
    if (btn_cq.disp_btn) buttons_refresh(&btn_cq);
    /* Lists don't hold it up, only the keyboard. Waiting replies went
     * first (hb_tick runs before this). */
    if (now < auto_cq_next_ms || tx_active || composing) return;
    if (send_cq(true)) {
        auto_cq_from_ms = now;
        auto_cq_next_ms = now + cq_interval_ms(); /* restarted when it ends */
    } else {
        auto_cq_stop("could not send");
    }
}

/* Where a heartbeat or HB ACK goes, as desktop: a free spot in the
 * 500-1000 Hz heartbeat sub-band, clear of anything decoded in the last
 * 30 s (call or not). A heartbeat stays on our own offset when that is
 * 1000 Hz or below. Our chat offset (the red band) doesn't move. */
static int free_hb_offset(bool heartbeat) {
    if (heartbeat && param_i_get(cfg.js8.tx_freq()) <= 1000) return param_i_get(cfg.js8.tx_freq());
    float   offsets[ACTIVITY];
    int64_t times[ACTIVITY];
    for (int i = 0; i < ACTIVITY; i++) {
        offsets[i] = activity[i].hz;
        times[i]   = activity[i].ms; /* unused entries: 0, long ago */
    }
    return js8_heartbeat_offset(offsets, times, ACTIVITY, now_wall_ms());
}

static bool send_heartbeat(bool automatic) {
    if (!js8_speed_heartbeats(cur_speed())) {
        if (!automatic) msg_update_text_fmt("No heartbeats in %s, as in desktop JS8Call", js8_speed_name(cur_speed()));
        return false;
    }
    char text[48];
    js8_heartbeat_text(my_call(), my_grid(), text, sizeof(text));
    return tx_queue_at(text, free_hb_offset(true), automatic);
}

/* The first automatic heartbeat: an interval from now, on the slot grid
 * (desktop's TxLoop). */
static int64_t hb_first_ms(void) {
    return js8_next_heartbeat_ms(now_wall_ms(), param_i_get(cfg.js8.hb_interval()), js8_speed_period_s(cur_speed()));
}

/* Page 1's Heartbeat works as CQ does (the user's design): press = one
 * now; hold = auto heartbeats, one now and then every js8_hb_interval
 * minutes on desktop's slot schedule, the knob setting the minutes; press
 * while auto = back to manual (auto off); hold while auto = set the
 * minutes again (and carry on if they were paused). */
static void heartbeat_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    (void)btn;
    if (hb_adjusting) {
        hb_adjust_end();
        return;
    }
    if (param_i_get(cfg.js8.hb())) {
        hb_auto_stop("manual");
        return;
    }
    send_heartbeat(false);
}

static void heartbeat_hold_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    if (param_i_get(cfg.js8.hb())) {
        if (hb_paused()) hb_resume();
        hb_adjust_start(btn);
        return;
    }
    if (!js8_speed_heartbeats(cur_speed())) {
        msg_update_text_fmt("No heartbeats in %s, as in desktop JS8Call", js8_speed_name(cur_speed()));
        return;
    }
    /* Busy sending: the first one comes an interval from now (as auto CQ).
     * Otherwise one now; the next ones count from it. */
    bool busy = tx_active;
    if (!busy && !send_heartbeat(false)) return;
    param_i_set(cfg.js8.hb(), true);
    hb_paused_until = 0;
    hb_next_ms      = hb_first_ms();
    if (btn_hbackk.disp_btn) buttons_refresh(&btn_hbackk);
    add_info_row("Auto HB on");
    hb_adjust_start(btn);
    msg_update_text_fmt("Auto HB on: turn the knob to set the minutes, press Heartbeat when done");
    update_status();
}

static const char *hold_label_getter(void) {
    return param_i_get(cfg.js8.hold_offset()) ? "Hold:\nOn" : "Hold:\nOff";
}

static void hold_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    param_i_set(cfg.js8.hold_offset(), !param_i_get(cfg.js8.hold_offset()));
    buttons_refresh(btn);
    msg_update_text_fmt(param_i_get(cfg.js8.hold_offset()) ? "Replies stay on your offset" : "Replies move to their offset");
}

static const char *stations_label_getter(void) {
    return view_map ? "Show\nMessages" : view_stations ? "Show\nMap" : "Show\nStations";
}

static void stations_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    /* Messages -> Stations -> Map -> Messages. The map keeps the Stations
     * view underneath it: the MFK selects there, the map shows it. */
    if (view_map) {
        map_show(false);
        map_qrz_n = 0; /* the list shows what they sent */
        view_stations = false;
        st_all_time   = false; /* Show Stations is Recent; Show History is All time */
    } else if (view_stations) {
        map_show(true);
    } else {
        view_stations = true;
    }
    cursor_freq = -1;
    buttons_refresh(btn);
    if (btn_map_view.disp_btn) buttons_refresh(&btn_map_view); /* Sort, Map: or nothing */
    if (btn_ever.disp_btn) buttons_refresh(&btn_ever);
    map_qrz_show(); /* the QRZ line: over the Stations view or the map, not the messages */
    /* The Stations view ends a lock: the station stays selected, and the
     * knob selects again there. */
    if (view_stations && sel_locked) {
        sel_locked = false;
        msg_update_text_fmt("%s unlocked", sel_call);
        update_tx_bar();
    }
    rebuild_rows();
}

static const char *ever_label_getter(void) {
    if (!view_stations) return "Show\nHistory";
    return st_all_time ? "Heard:\nAll time" : "Heard:\nRecent";
}

/* Over the messages: the Stations view in All time. In the Stations view
 * or on the map: Recent <-> All time. */
static void ever_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    if (!history_db) {
        msg_update_text_fmt("No station history: js8_history.db on the SD card couldn't be opened");
        return;
    }
    bool from_messages = !view_stations;
    if (from_messages) {
        view_stations = true;
        st_all_time   = true;
        cursor_freq   = -1;
        if (sel_locked) {
            sel_locked = false;
            update_tx_bar();
        }
    } else {
        st_all_time = !st_all_time;
    }
    if (st_all_time) ever_load();
    buttons_refresh(btn);
    if (btn_stations.disp_btn) buttons_refresh(&btn_stations);
    if (btn_map_view.disp_btn) buttons_refresh(&btn_map_view);
    map_qrz_show();
    if (st_all_time)
        msg_update_text_fmt("All time on %s: %d station%s you've exchanged messages with; press the MFK on one for its history",
                            band_name(dial_band()), ever_count, ever_count == 1 ? "" : "s");
    else msg_update_text_fmt("Recent: stations heard lately");
    rebuild_rows();
}

/* ---- Query popup ------------------------------------------------------ */

static void query_close(void) {
    if (!query_list) return;
    /* Often called from one of the list's own buttons: delete later. */
    lv_obj_del_async(query_list);
    query_list = NULL;
    if (table) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void query_item_cb(lv_event_t *e) {
    js8_query_t q = (js8_query_t)(intptr_t)lv_event_get_user_data(e);
    char        call[JS8_RX_CALL_LEN];
    float       freq;
    int         snr;
    bool        have = selected_station(call, sizeof(call), &freq, &snr);
    query_close();
    if (!have) return;

    char text[64];
    if (!js8_query_text(q, call, snr, my_grid(), text, sizeof(text))) {
        msg_update_text_fmt(q == JS8_Q_MY_GRID ? "Set your grid first: APP > QTH" : "Nothing to send");
        return;
    }
    apply_hold(freq);
    tx_queue(text);
}

/* Keys in any list popup: ESC does `esc` (closes it), MFK moves, and the
 * VOL knob (a keypad: its turns arrive as keys at the focus) sets the
 * volume, which only the Query list and the message list did (B-19). */
static void popup_key(lv_event_t *e, void (*esc)(void)) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    switch (key) {
    case LV_KEY_ESC:
        esc();
        break;
    case LV_KEY_LEFT:
    case LV_KEY_UP:
        lv_group_focus_prev(keyboard_group);
        break;
    case LV_KEY_RIGHT:
    case LV_KEY_DOWN:
        lv_group_focus_next(keyboard_group);
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

static void query_key_cb(lv_event_t *e) {
    popup_key(e, query_close);
}

/* Every list popup that's open. */
static void close_popups(void) {
    for (size_t i = 0; i < POPUPS; i++)
        if (*popups[i].list) popups[i].close();
}

/* One-press messages for the selected station: MFK to move, press or tap
 * to send, ESC to close. */
/* A list popup (Query, Texts..., APRS) must close before anything else
 * happens, or it's left behind with its buttons still holding the knob.
 * Any other bottom button just closes it; press again to do the thing. */
static bool popup_guard(void) {
    if (!any_popup()) return false;
    close_popups();
    msg_update_text_fmt("List closed");
    return true;
}

/* Changing page closes a list too (then changes page). */
static void js8_next_page_cb(button_data_t *btn) {
    close_popups();
    button_next_page_cb(btn);
}

/* Holding the page button goes back one, as on the main screen. */
static void js8_prev_page_cb(button_data_t *btn) {
    close_popups();
    button_prev_page_cb(btn);
}

/* Scroll a list to the focused item in one step: the default animated
 * scroll redraws the list for every animation frame, which tears on the
 * radio's display. */
static void list_item_focused_cb(lv_event_t *e) {
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_OFF);
}

static lv_obj_t *list_add_item(lv_obj_t *list, const char *label) {
    lv_obj_t *b = lv_list_add_btn(list, NULL, label);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1830a0), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(b, list_item_focused_cb, LV_EVENT_FOCUSED, NULL);
    return b;
}

/* The Query list's message items, as desktop's call menu has them: the
 * text started in the keyboard (NULL: sent at once, `send` the text) and
 * what to type. The @ALLCALL ones go to everyone, so they're there without
 * a station selected too. */
static const struct {
    const char *label, *prefill, *hint, *send;
    bool        allcall;
} query_msg_items[] = {
    {"Message...", "%s MSG ", "Message for %s's inbox: type it and press Enter", NULL, false},
    {"Message via them...", "%s MSG TO:", "Left at %s for someone: type their call, a space, the message", NULL, false},
    {"Any messages?", NULL, NULL, "%s QUERY MSGS", false},
    /* VE7NHW (2026-10-09): ask everyone too, without unselecting first. */
    {"Anyone have messages?", NULL, NULL, "@ALLCALL QUERY MSGS", true},
    {"Fetch message #...", "%s QUERY MSG ", "Type the number of the message %s holds for you", NULL, false},
    {"Relay via them...", "%s>", "Passed on by %s: type the call it's for, a space, the message", NULL, false},
    {"Can they reach...?", "%s QUERY CALL ", "Ask %s if they hear a station: type its call (the ? goes on by itself)", NULL, false},
    {"Can anyone reach...?", "@ALLCALL QUERY CALL ", "Ask everyone if they hear a station: type its call (the ? goes on by itself)", NULL, true},
};
#define QUERY_MSG_ITEMS (int)(sizeof(query_msg_items) / sizeof(query_msg_items[0]))

static void query_msg_cb(lv_event_t *e) {
    int   which = (int)(intptr_t)lv_event_get_user_data(e);
    char  call[JS8_RX_CALL_LEN];
    float freq;
    int   snr;
    bool  have = selected_station(call, sizeof(call), &freq, &snr);
    if (query_msg_items[which].allcall) { /* to everyone: at our own offset */
        if (!query_msg_items[which].prefill) {
            query_close();
            tx_queue(query_msg_items[which].send);
            return;
        }
        popup_leave(&query_list); /* into the keyboard */
        if (popup_to_keyboard(NULL, 0, query_msg_items[which].prefill)) msg_update_text_fmt("%s", query_msg_items[which].hint);
        return;
    }
    if (!query_msg_items[which].prefill || !have) {
        query_close();
        if (!have) return;
        apply_hold(freq);
        char text[JS8_RX_CALL_LEN + 24];
        snprintf(text, sizeof(text), query_msg_items[which].send, call);
        tx_queue(text);
        return;
    }
    popup_leave(&query_list); /* into the keyboard */
    apply_hold(freq);
    char prefill[JS8_RX_CALL_LEN + 16];
    snprintf(prefill, sizeof(prefill), query_msg_items[which].prefill, call);
    if (popup_to_keyboard(NULL, 0, prefill)) msg_update_text_fmt(query_msg_items[which].hint, call);
}

static void query_close_cb(lv_event_t *e) {
    (void)e;
    query_close();
}

/* The Query list's and the saved messages' frame, top right over the list. */
static lv_obj_t *query_list_create(lv_coord_t width, const char *title) {
    lv_group_remove_obj(table);
    lv_obj_t *list = lv_list_create(dialog.obj);
    lv_obj_set_size(list, width, WF_HEIGHT - 10);
    lv_obj_align(list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(list, &sony_24, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(list, lv_color_white(), 0);
    lv_obj_t *t = lv_list_add_text(list, title);
    lv_obj_set_style_text_font(t, &sony_22, 0);
    return list;
}

/* A gold item at the end: Close, or < Back. */
static void query_list_end_item(const char *label, lv_event_cb_t cb) {
    lv_obj_t *b = list_add_item(query_list, label);
    lv_obj_set_style_text_color(b, lv_color_hex(0xffc040), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b, query_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
}

static void query_saved_cb(lv_event_t *e) {
    (void)e;
    popup_leave(&query_list);
    saved_open(0);
}

static void query_open(void) {
    char  call[JS8_RX_CALL_LEN];
    float freq;
    int   snr;
    /* No station selected: only what goes to everyone (@ALLCALL). */
    bool have = selected_station(call, sizeof(call), &freq, &snr);

    char title[40];
    if (have) snprintf(title, sizeof(title), "To %s (%+d dB)", call, snr);
    else snprintf(title, sizeof(title), "To @ALLCALL"); /* no station selected */
    query_list = query_list_create(330, title); /* "Anyone have messages?" fits */

    lv_obj_t *first = NULL;
    for (int q = 0; have && q < JS8_Q_COUNT; q++) {
        lv_obj_t *b = list_add_item(query_list, js8_query_label((js8_query_t)q));
        lv_obj_add_event_cb(b, query_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)q);
        lv_obj_add_event_cb(b, query_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, b);
        if (!first) first = b;
    }
    for (int i = 0; i < QUERY_MSG_ITEMS; i++) {
        if (!have && !query_msg_items[i].allcall) continue;
        lv_obj_t *b = list_add_item(query_list, query_msg_items[i].label);
        lv_obj_set_style_text_color(b, lv_color_hex(0x80ff80), 0);
        lv_obj_add_event_cb(b, query_msg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, query_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, b);
        if (!first) first = b;
    }
    /* Just before Close, with or without a station selected. */
    lv_obj_t *saved = list_add_item(query_list, "Saved messages >");
    lv_obj_add_event_cb(saved, query_saved_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(saved, query_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, saved);
    /* Last, so one step back from the first item (the group wraps). */
    query_list_end_item("Close", query_close_cb);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(first);
    speed_warn();
}

static void query_cb(button_data_t *btn) {
    user_touch();
    (void)btn;
    if (query_list) { /* Query > again closes it */
        query_close();
        return;
    }
    if (popup_guard()) return;
    if (composing || aprs_list || texts_list) return;
    query_open();
}

/* ---- Saved messages --------------------------------------------------- */

/* Desktop JS8Call's Saved Messages: texts you send often, with macros
 * (<CALL>, <SNR>, <MYGRID4> ...) filled in as they go. Query > Saved
 * messages >: press one to send it at once (desktop's default), hold the
 * MFK on it to edit it. Desktop starts with "TNX 73 GL". */

static void load_saved(void) {
    memset(saved_msgs, 0, sizeof(saved_msgs));
    char buf[SAVED_N * (TX_TEXT_MAX + 2) + 64], notice[160];
    saved_writable = js8_file_read(JS8_SAVED_PATH, buf, sizeof(buf), notice, sizeof(notice));
    data_file_notice(notice);
    if (saved_writable && access(JS8_SAVED_PATH, F_OK) != 0) { /* none saved yet */
        snprintf(saved_msgs[0], sizeof(saved_msgs[0]), "TNX 73 GL");
        return;
    }
    /* One a line, empty lines for empty slots; capitals, as typed here. */
    char *p = buf;
    for (int i = 0; i < SAVED_N && *p; i++) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        p[strcspn(p, "\r")] = '\0';
        snprintf(saved_msgs[i], sizeof(saved_msgs[i]), "%s", p);
        for (char *c = saved_msgs[i]; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        if (!nl) break;
        p = nl + 1;
    }
}

static void save_saved(void) {
    if (!saved_writable) {
        msg_update_text_fmt("Not saved: %s can't be read or moved (check the SD card)", JS8_SAVED_PATH);
        return;
    }
    char   buf[SAVED_N * (TX_TEXT_MAX + 2) + 64];
    size_t n = 0;
    buf[0]   = '\0';
    for (int i = 0; i < SAVED_N; i++) n += snprintf(buf + n, sizeof(buf) - n, "%s\n", saved_msgs[i]);
    if (!js8_file_write(JS8_SAVED_PATH, buf)) msg_update_text_fmt("Can't write %s", JS8_SAVED_PATH);
}

/* Into the keyboard with saved message i; Enter saves it, ESC leaves it. */
static void saved_edit_open(int i) {
    saved_edit = i;
    if (popup_to_keyboard(&query_list, EDIT_SAVED, saved_msgs[i]))
        msg_update_text_fmt("Message %d: Enter saves, empty clears. Macros: <CALL> <SNR> <TDELTA> <MYCALL> "
                            "<MYGRID4> <MYGRID12> <MYINFO> <MYSTATUS> <MYIDLE> <MYVERSION>",
                            i + 1);
}

/* Press: send it now, macros filled in. An empty one: write it. */
static void saved_item_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    user_touch();
    if (!saved_msgs[i][0]) {
        saved_edit_open(i);
        return;
    }
    char  call[JS8_RX_CALL_LEN];
    float freq;
    int   snr;
    bool  have = selected_station(call, sizeof(call), &freq, &snr);
    /* Desktop would send it with <CALL> left out: not to nobody here. */
    if (!have && js8_macros_need_station(saved_msgs[i])) {
        msg_update_text_fmt("Select a station first: message %d has <CALL>, <SNR> or <TDELTA>", i + 1);
        return;
    }
    char text[TX_TEXT_MAX * 2];
    macros_fill(saved_msgs[i], true, text, sizeof(text));
    char *t = text + strspn(text, " ");
    for (char *end = t + strlen(t); end > t && end[-1] == ' ';) *--end = '\0';
    if (!*t) {
        msg_update_text_fmt("Nothing to send: message %d has only macros that are empty now", i + 1);
        return;
    }
    query_close();
    if (have) apply_hold(freq);
    tx_queue(t);
}

/* Hold the MFK: edit it. */
static void saved_hold_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    user_touch();
    /* The MFK's release would otherwise land in the keyboard as Enter. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_wait_release(indev);
    saved_edit_open(i);
}

/* The message line says what a press would send. */
static void saved_focus_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!saved_msgs[i][0]) {
        msg_update_text_fmt("Message %d is empty: press or hold to write one", i + 1);
        return;
    }
    if (!sel_call[0] && js8_macros_need_station(saved_msgs[i])) {
        msg_update_text_fmt("%d: %s (select a station first)", i + 1, saved_msgs[i]);
        return;
    }
    char text[TX_TEXT_MAX * 2];
    macros_fill(saved_msgs[i], true, text, sizeof(text));
    msg_update_text_fmt("%d: %s", i + 1, text);
}

static void saved_back_cb(lv_event_t *e) {
    (void)e;
    popup_leave(&query_list);
    query_open();
}

/* The ten in the Query list's place (wider: messages are long), focus on
 * `focus`; < Back to the Query list. */
static void saved_open(int focus) {
    query_list = query_list_create(560, "Saved messages: press sends, hold edits");
    lv_obj_t *items[SAVED_N];
    for (int i = 0; i < SAVED_N; i++) {
        char label[TX_TEXT_MAX * 2 + 8], text[TX_TEXT_MAX * 2];
        if (saved_msgs[i][0]) {
            macros_fill(saved_msgs[i], false, text, sizeof(text)); /* as desktop's menu shows them */
            snprintf(label, sizeof(label), "%d  %s", i + 1, text);
        } else {
            snprintf(label, sizeof(label), "%d  (empty)", i + 1);
        }
        lv_obj_t *b = items[i] = list_add_item(query_list, label);
        lv_label_set_long_mode(lv_obj_get_child(b, -1), LV_LABEL_LONG_DOT); /* one line, never scrolling */
        if (!saved_msgs[i][0]) lv_obj_set_style_text_color(b, lv_color_hex(0x909090), 0);
        lv_obj_add_event_cb(b, saved_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, saved_hold_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, saved_focus_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, query_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, b);
    }
    query_list_end_item("< Back", saved_back_cb);
    query_list_end_item("Close", query_close_cb);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(items[focus >= 0 && focus < SAVED_N ? focus : 0]);
}

/* For tools/js8_ui_harness: Waterfall: Sharp (0) .. Calm (3), as Settings. */
void dialog_js8_wf_avg(int level) {
    param_i_set(cfg.js8.wf_avg(), level);
    atomic_store(&wf_avg_level, level % WF_AVG_LEVELS);
}

/* For tools/js8_ui_harness: the waterfall's place on the screen, and its
 * palette (WF_MIN_DB..WF_MAX_DB over 256 colours). */
bool dialog_js8_wf_area(lv_area_t *a, const lv_color_t **palette) {
    if (!wf_box) return false;
    lv_obj_get_coords(wf_box, a);
    *palette = (const lv_color_t *)style.wf_palette;
    return true;
}

/* For tools/js8_ui_harness: the red band's offset, and the green band's
 * place and width in pixels (false when it isn't shown). */
int dialog_js8_finder_hz(void) {
    return finder_shown;
}
bool dialog_js8_cursor_band(int *x, int *w) {
    if (!cursor_box || lv_obj_has_flag(cursor_box, LV_OBJ_FLAG_HIDDEN)) return false;
    *x = lv_obj_get_x(cursor_box);
    *w = lv_obj_get_width(cursor_box);
    return true;
}

/* For tools/js8_ui_harness: the station the selection points at. */
bool dialog_js8_selected_call(char *call, unsigned len) {
    float freq;
    int   snr;
    return table && selected_station(call, len, &freq, &snr);
}

/* For tools/js8_ui_harness: the decode marks drawn lately (frequency and
 * JS8_MARK_* level of each bracket), up to `max`. */
unsigned dialog_js8_marks(float *freq_hz, uint8_t *level, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < MARK_MEMORY && n < max; i++) {
        if (!marks_drawn[i].used) continue;
        freq_hz[n] = marks_drawn[i].freq_hz;
        level[n]   = marks_drawn[i].level;
        n++;
    }
    return n;
}

/* For tools/js8_ui_harness: the decoder setup's notice if it shows, else "". */
const char *dialog_js8_setup_notice(void) { return setup_title ? lv_label_get_text(setup_title) : ""; }

/* For tools/js8_ui_harness: the Stations view's QRZ line if it shows, else "". */
const char *dialog_js8_st_qrz(void) {
    return st_qrz_label && !lv_obj_has_flag(st_qrz_label, LV_OBJ_FLAG_HIDDEN) ? lv_label_get_text(st_qrz_label) : "";
}

/* For tools/js8_ui_harness: Auto time sync on or off. Scenarios that move
 * JS8's time on by minutes with a drift switch it off: Auto would put the
 * drift back within a slot, as desktop's does. */
void dialog_js8_time_auto(bool on) {
    param_i_set(cfg.js8.tsync_auto(), on);
    js8_rx_set_auto_sync(rx, on);
}

/* For tools/js8_ui_harness: the Stations view's calls in row order, the
 * cursor's with a '>'; how many stations are listed. */
int dialog_js8_station_rows(char *out, unsigned len) {
    out[0] = '\0';
    if (!table || !view_stations) return 0;
    uint16_t cur = 0, col = 0;
    lv_table_get_selected_cell(table, &cur, &col);
    size_t n = 0;
    for (uint16_t r = 0; r < rows && n < len; r++)
        if (row_hist[r] >= 0)
            n += snprintf(out + n, len - n, "%s%s%s", n ? " " : "", r == cur ? ">" : "", st_rows[row_hist[r]].call);
    return st_count;
}

/* For tools/js8_ui_harness: the station history (NULL if not open). */
js8_history_t *dialog_js8_history(void) {
    return history_db;
}

/* For tools/js8_ui_harness: the Stations view's star column for `call`
 * ('@', '*' or ' '), or 0 if it isn't listed. */
char dialog_js8_station_star(const char *call) {
    for (int i = 0; i < st_count; i++)
        if (strcasecmp(st_rows[i].call, call) == 0) {
            station_fields_t f;
            station_fields(&st_rows[i], now_wall_ms(), &f);
            return f.star[0];
        }
    return 0;
}

/* For tools/js8_ui_harness: the status line (top right), and our grid now. */
const char *dialog_js8_status_text(void) {
    update_status();
    return status ? lv_label_get_text(status) : "";
}
const char *dialog_js8_my_grid(void) {
    return my_grid();
}

/* For tools/js8_ui_harness: a decoded message, handed over as the
 * receiver's thread does. */
void dialog_js8_test_message(const js8_rx_msg_t *m) {
    on_message(m, NULL);
}

/* For tools/js8_ui_harness: how far the list is scrolled past its end, in
 * pixels (blank space under the last row); 0 when it isn't. */
int dialog_js8_list_overscroll(void) {
    if (!table) return -1;
    lv_obj_update_layout(table);
    lv_coord_t below = lv_obj_get_scroll_bottom(table), y = lv_obj_get_scroll_y(table);
    if (below >= 0 || y <= 0) return 0;
    return -below < y ? -below : y;
}

/* For tools/js8_ui_harness: does message-list row `row` have the green bar? */
bool dialog_js8_row_marked(unsigned row) {
    return table && row < rows && row_is_selected_station(row_hist[row]);
}

/* ---- T4: auto-reply, heartbeats ---------------------------------------- */

/* Any key, button or knob: resets the idle watchdog. */
static void user_touch(void) {
    js8_auto_user_activity(autop, now_wall_ms());
}

/* Recently heard stations, most recent first: HEARING?, QUERY CALL and
 * RETRIEVE MSG look here. */
static unsigned heard_stations(js8_heard_t *out, unsigned max) {
    static js8_station_t list[MAX_ROWS];
    if (max > MAX_ROWS) max = MAX_ROWS;
    int n = js8_stations_recent(stations, now_wall_ms(), list, (int)max);
    for (int i = 0; i < n; i++) {
        out[i].call     = list[i].call;
        out[i].snr      = list[i].snr;
        out[i].heard_ms = list[i].heard_ms;
    }
    return (unsigned)n;
}

/* The switches as they are now, for deciding again at send time. */
static js8_auto_settings_t auto_settings(void) {
    /* Their macros filled in, as desktop answers INFO? and STATUS?. */
    static char info_now[TX_TEXT_MAX + 1], status_now[TX_TEXT_MAX + 1];
    macros_fill(info_text, true, info_now, sizeof(info_now));
    macros_fill(status_text, true, status_now, sizeof(status_now));
    js8_auto_settings_t st = {
        .autoreply = param_i_get(cfg.js8.auto_mode()),
        .heartbeat = param_i_get(cfg.js8.hb()) && !hb_paused(),
        .hb_ack    = param_i_get(cfg.js8.hb_ack()) && !hb_paused(),
        .relay     = param_i_get(cfg.js8.relay()),
        .my_call   = my_call(),
        .my_grid   = my_grid(),
        .info      = info_now,
        .status    = status_now,
        .groups    = groups_text,
        .held      = held,
    };
    return st;
}

static void reply_drop(int i) {
    memmove(&replies[i], &replies[i + 1], (size_t)(n_replies - i - 1) * sizeof(replies[0]));
    n_replies--;
}

/* An automatic reply: in the queue, then out at the first chance. */
static void auto_queue(const js8_auto_result_t *r) {
    for (int i = 0; i < n_replies; i++)
        if (strcmp(replies[i].r.text, r->text) == 0) return; /* already waiting */
    if (n_replies == REPLIES) {
        add_info_row("Auto: too many replies waiting, not sent: %s", r->text);
        return;
    }
    replies[n_replies].r       = *r;
    replies[n_replies].ms      = now_wall_ms();
    replies[n_replies].cycle   = cycles;
    replies[n_replies].checked = false;
    n_replies++;
}

/* The oldest reply that may go now, decided again as things are now: the
 * switches, the idle watchdog and the @ALLCALL cooldown. Waits for our own
 * TX and the keyboard (not a list, your choice); while a message to us is
 * still arriving nothing goes, and while any is arriving no HB ACK. */
static void auto_try_send(void) {
    int64_t now = now_wall_ms();
    for (int i = 0; i < n_replies;) {
        if (now - replies[i].ms > REPLY_WAIT_MS) {
            reply_drop(i);
            continue;
        }
        /* Desktop answers once all of a cycle's decodes are in, and drops
         * the answer if a message to us is still arriving then (an HB ACK
         * if any message is): we can't hear its next frames while we send
         * (D3: as desktop). */
        bool ended = replies[i].cycle != cycles || now - replies[i].ms > CYCLE_WAIT_MS;
        if (!replies[i].checked && ended) {
            replies[i].checked = true;
            if (message_open(true) || (replies[i].r.hb_ack && message_open(false))) {
                add_info_row("Auto: %s not sent, a message is still arriving", replies[i].r.text);
                reply_drop(i);
                continue;
            }
        }
        i++;
    }
    if (!n_replies || tx_active || composing || message_open(true)) return;

    js8_auto_settings_t st = auto_settings();
    for (int i = 0; i < n_replies;) {
        js8_auto_result_t r = replies[i].r;
        if (!replies[i].checked || (r.hb_ack && message_open(false))) {
            i++; /* it waits; the others may go */
            continue;
        }
        reply_drop(i);
        if (js8_auto_decide(autop, &r, &st, now) != JS8_AUTO_SEND) continue; /* switched off meanwhile */
        if (r.hb_ack && !js8_speed_heartbeats(cur_speed())) continue;        /* desktop: no HB ACKs in Turbo or Ultra */
        int offset = r.hb_ack ? free_hb_offset(false) : param_i_get(cfg.js8.tx_freq());
        LV_LOG_USER("JS8 auto: '%s' at %d Hz", r.text, offset);
        if (!tx_queue_at(r.text, offset, true)) continue;
        js8_auto_sent(autop, &r, now);
        if (r.deliver_id) deliver_start(r.deliver_id, r.deliver_group_call, r.text);
        if (r.kind == JS8_REPLY_RELAY) msg_update_text_fmt("Relaying %s's message", r.to);
        add_info_row("Auto: %s", r.text);
        return; /* one at a time: the next when this one ends */
    }
}

/* AUTO off: the answer offered to a station, if it's still fresh. */
static int offer_find(const char *call) {
    int64_t now = now_wall_ms();
    for (int i = 0; i < OFFERS; i++)
        if (offers[i].call[0] && strcmp(offers[i].call, call) == 0 && now - offers[i].ms < OFFER_MS) return i;
    return -1;
}

/* One per station: a newer answer to the same station replaces its old
 * one; with all taken, the oldest goes. */
static void offer_add(const js8_auto_result_t *r) {
    int slot = 0;
    for (int i = 0; i < OFFERS; i++) {
        if (offers[i].call[0] && strcmp(offers[i].call, r->to) == 0) {
            slot = i;
            break;
        }
        if (!offers[slot].call[0]) continue;
        if (!offers[i].call[0] || offers[i].ms < offers[slot].ms) slot = i;
    }
    snprintf(offers[slot].call, sizeof(offers[slot].call), "%s", r->to);
    snprintf(offers[slot].text, sizeof(offers[slot].text), "%s", r->text);
    offers[slot].ms         = now_wall_ms();
    offers[slot].deliver_id = r->deliver_id;
    snprintf(offers[slot].deliver_group_call, sizeof(offers[slot].deliver_group_call), "%s", r->deliver_group_call);
}

/* Everything that answered stations on this frequency: waiting replies,
 * offers, the offer taken into the keyboard, messages still arriving and
 * the band activity. */
static void auto_clear(void) {
    n_replies = 0;
    memset(offers, 0, sizeof(offers));
    deliver_pending.id = 0;
    memset(open_msgs, 0, sizeof(open_msgs));
    memset(activity, 0, sizeof(activity));
}

/* Heartbeats and HB ACKs pause while you're busy: anything you send by
 * hand except a heartbeat (Reply, Send..., Query, CQ ...) pauses them until
 * HB_PAUSE_MS after the last one; they resume by themselves (the user's
 * choice: nothing heard or sent automatically pauses them, and selecting
 * or unlocking a station doesn't resume them, unlike desktop). The
 * switches stay on. */
static bool hb_paused(void) {
    return hb_paused_until && now_wall_ms() < hb_paused_until;
}

static void hb_pause(const char *why) {
    if (!param_i_get(cfg.js8.hb()) && !param_i_get(cfg.js8.hb_ack())) return;
    bool was = hb_paused();
    hb_paused_until = now_wall_ms() + HB_PAUSE_MS;
    if (hb_adjusting) hb_adjust_end();
    if (btn_hb.disp_btn) buttons_refresh(&btn_hb);
    if (btn_hbackk.disp_btn) buttons_refresh(&btn_hbackk);
    if (!was) add_info_row("HB and HB ACK paused %d min: %s", HB_PAUSE_MS / 60000, why);
    update_status();
}

/* The pause ran out (or HB was pressed): heartbeats carry on. One that fell
 * due meanwhile goes out at the next chance. */
static void hb_resume(void) {
    hb_paused_until = 0;
    if (btn_hb.disp_btn) buttons_refresh(&btn_hb);
    if (btn_hbackk.disp_btn) buttons_refresh(&btn_hbackk);
    update_status();
}

static void handle_incoming(const js8_rx_msg_t *m) {
    if (js8_starts_qso(m)) {
        char why[JS8_RX_CALL_LEN + 12];
        snprintf(why, sizeof(why), "%s answered", m->from);
        auto_cq_stop(why);
        /* Someone calling you (an answer to your CQ or not; not a heartbeat
         * ACK, nor a doubtful decode) pauses heartbeats too. */
        if (!m->low_confidence) {
            snprintf(why, sizeof(why), "%s called", m->from);
            hb_pause(why);
        }
    }

    js8_heard_t heard[32];
    unsigned    n = heard_stations(heard, 32);

    js8_auto_settings_t st = auto_settings();
    js8_stored_t        kept;
    js8_auto_result_t   r;
    js8_process(autop, m, &st, heard, n, last_tx_text, now_wall_ms(), inbox, &kept, &r);
    stored_received(&kept);

    switch (r.action) {
    case JS8_AUTO_SEND:
        auto_queue(&r); /* goes when this decode cycle ends (auto_try_send) */
        break;
    case JS8_AUTO_OFFER:
        offer_add(&r);
        if (r.kind == JS8_REPLY_RELAY) {
            char dest[JS8_RX_CALL_LEN];
            snprintf(dest, sizeof(dest), "%.*s", (int)strcspn(r.text, ">"), r.text);
            msg_update_text_fmt("%s asks you to relay to %s - select it and press Reply to pass it on", r.to, dest);
            add_info_row("%s: Reply passes on \"%s\" (AUTO does it by itself)", r.to, r.text);
        } else if (strcmp(r.command, ">") == 0) {
            msg_update_text_fmt("Relayed message via %s - select it and press Reply to send ACK", r.to);
            add_info_row("%s: Reply sends \"%s\" (AUTO sends it by itself)", r.to, r.text);
        } else if (strcmp(r.command, "MSG") == 0 || strcmp(r.command, "MSG TO:") == 0) {
            msg_update_text_fmt("Message from %s - select it and press Reply to send ACK", r.to);
            add_info_row("%s: Reply sends \"%s\" (AUTO sends it by itself)", r.to, r.text);
        } else if (r.deliver_id) {
            msg_update_text_fmt("%s asks for the message held for them - select it and press Reply to send it", r.to);
            add_info_row("%s asks for held message %d: Reply sends it", r.to, r.deliver_id);
        } else if (strcmp(r.command, "HW CPY?") == 0) {
            msg_update_text_fmt("%s asks how you copy - select it and press Reply", r.to);
            add_info_row("%s asked HW CPY?: Reply has \"%s\"", r.to, r.text);
        } else if (strcmp(r.command, "MSG ID") == 0) {
            msg_update_text_fmt("%s holds a message for you - select it and press Reply to fetch it", r.to);
            add_info_row("%s holds a message for you: Reply sends \"%s\"", r.to, r.text);
        } else {
            msg_update_text_fmt("%s asked %s - select it and press Reply to answer", r.to, r.command);
            add_info_row("%s asked %s: Reply sends \"%s\"", r.to, r.command, r.text);
        }
        break;
    default:
        break;
    }
}

/* Desktop's stored-message notices (pushNotificationHandler): every 15
 * minutes, with AUTO on and nothing else going out, "W1ABC RETRIEVE MSG 3"
 * to a station we hold a message for, heard in the last 15 minutes and not
 * told in 8 hours. One per look. */
#define PUSH_INTERVAL_MS (15 * 60 * 1000)

static void push_tick(void) {
    int64_t now = now_wall_ms();
    if (!push_next_ms) push_next_ms = now + PUSH_INTERVAL_MS;
    if (now < push_next_ms) return;
    push_next_ms = now + PUSH_INTERVAL_MS;
    if (!param_i_get(cfg.js8.auto_mode()) || !held || js8_auto_idle(autop, now) || !my_call()[0]) return;
    if (tx_active || n_replies || composing) return;
    js8_heard_t heard[32];
    unsigned    n = heard_stations(heard, 32);
    char        text[64];
    int         id;
    if (!js8_held_push_due(held, heard, n, now, text, sizeof(text), &id)) return;
    LV_LOG_USER("JS8 auto: '%s'", text);
    /* Told only once it's queued: one that couldn't go out is tried at the
     * next look, not in 8 hours (bug hunt S4). */
    if (!tx_queue_at(text, param_i_get(cfg.js8.tx_freq()), true)) return;
    js8_held_push_sent(held, id, now);
    add_info_row("Auto: %s", text);
}

/* Messages kept (Settings): every 30 s, drop rows past their time, unless
 * you're scrolled up reading. */
static void msg_age_tick(void) {
    static int64_t next;
    int64_t        now = now_wall_ms(), keep = msg_keep_ms();
    if (now < next) return;
    next = now + 30000;
    if (!keep || view_stations || !table || !at_bottom()) return;
    for (uint16_t r = 0; r < rows; r++) {
        if (row_hist[r] < 0) continue;
        if (now - hist_ms[row_hist[r]] > keep) rebuild_rows();
        return; /* the oldest message row decides */
    }
}

/* Once a second: send a heartbeat when one is due. */
/* GPS time into the battery clock (rtc1), so the radio keeps it after the
 * GPS is unplugged: once a power-on, when the system clock is synced
 * (ntpd, from the GPS: the radio has no network) and there's a current
 * fix, never while sending. rtc1 shares i2c-0 with the amplifier's band
 * data: one write, on its own thread (hwclock takes ~0.1 s). */
#ifndef JS8_RTC_FLAG_PATH
#define JS8_RTC_FLAG_PATH "/tmp/js8_rtc_saved" /* /tmp is cleared at boot */
#endif
static bool        rtc_done;
static atomic_bool rtc_busy;
static atomic_int  rtc_result; /* from the save thread for the list: 1 set, -1 failed, 0 nothing new */

static void *rtc_save_thread(void *arg) {
    (void)arg;
    bool ok = js8_clock_save_rtc();
    LV_LOG_USER("JS8: battery clock %s from GPS time", ok ? "set" : "not set");
    if (ok) {
        FILE *f = fopen(JS8_RTC_FLAG_PATH, "w");
        if (f) fclose(f);
    }
    atomic_store(&rtc_result, ok ? 1 : -1);
    atomic_store(&rtc_busy, false);
    return NULL;
}

static void rtc_tick(void) {
    /* The list says what happened once the write is done, not when it
     * starts. A failed write isn't tried again this power-on: rtc1 shares
     * i2c-0 with the amplifier's band data. */
    int result = atomic_exchange(&rtc_result, 0);
    if (result > 0) add_info_row("Battery clock set from GPS time (kept when the GPS is unplugged)");
    if (result < 0) add_info_row("Battery clock not set: writing it failed (the time is right until power-off)");

    static int64_t next;
    int64_t        now = now_mono_ms();
    if (rtc_done || atomic_load(&rtc_busy) || now < next) return;
    next = now + 30000;
    if (access(JS8_RTC_FLAG_PATH, F_OK) == 0) { /* done since power-on */
        rtc_done = true;
        return;
    }
    if (!gps_fix_current() || tx_active || !js8_clock_synced()) return;
    atomic_store(&rtc_busy, true);
    pthread_t t;
    if (pthread_create(&t, NULL, rtc_save_thread, NULL) != 0) {
        atomic_store(&rtc_busy, false); /* nothing written: tried again in 30 s */
        return;
    }
    rtc_done = true;
    pthread_detach(t);
}

static void hb_tick(void) {
    beep_log_level();
    rtc_tick();
    if (hb_adjusting && now_wall_ms() - hb_adjust_ms > HB_ADJUST_MS) hb_adjust_end();
    /* The transmitter is idle but ui_tx_done never came (its message was
     * lost): don't wait for it for ever. */
    if (tx_active && !js8_tx_busy(tx)) {
        if (!tx_quiet_ms) tx_quiet_ms = now_wall_ms();
        else if (now_wall_ms() - tx_quiet_ms > TX_DONE_LOST_MS) {
            tx_active = false;
            finder_sync();
        }
    } else {
        tx_quiet_ms = 0;
    }
    /* Replies that waited for the keyboard or a message still arriving
     * (after a transmission, ui_tx_done does the same). */
    auto_try_send();
    push_tick();
    msg_age_tick();
    if (hb_paused_until && !hb_paused()) hb_resume();
    if (!param_i_get(cfg.js8.hb())) {
        hb_next_ms = 0;
        return;
    }
    if (btn_hb.disp_btn && !hb_adjusting) buttons_refresh(&btn_hb); /* its countdown */
    int64_t now = now_wall_ms();
    if (hb_paused()) return;
    if (js8_auto_idle(autop, now) || !js8_speed_heartbeats(cur_speed())) return;
    /* As on desktop, the first one comes an interval after switching on;
     * page 1's Heartbeat sends one now. */
    if (hb_next_ms == 0) {
        hb_next_ms = hb_first_ms();
        update_status();
    }
    if (now < hb_next_ms - 5000) return;   /* desktop prepares it 5 s early */
    /* Lists don't hold it up, only the keyboard. Waiting replies went
     * first (auto_try_send above). */
    if (tx_active || composing || !my_call()[0]) return;
    LV_LOG_USER("JS8 auto: heartbeat (due %lld)", (long long)hb_next_ms);
    if (send_heartbeat(true)) {
        /* On desktop's fixed schedule: an interval after the one due, not
         * after this one went (later if it waited). */
        hb_next_ms = js8_following_heartbeat_ms(hb_next_ms, now, param_i_get(cfg.js8.hb_interval()));
        update_status();
    }
}

static const char *auto_label_getter(void) {
    return param_i_get(cfg.js8.auto_mode()) ? "AUTO:\nOn" : "AUTO:\nOff";
}

static void auto_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    param_i_set(cfg.js8.auto_mode(), !param_i_get(cfg.js8.auto_mode()));
    buttons_refresh(btn);
    if (btn_hbackk.disp_btn) buttons_refresh(&btn_hbackk);
    msg_update_text_fmt(param_i_get(cfg.js8.auto_mode()) ? "AUTO on: answers SNR? GRID? INFO? STATUS? HEARING? AGN?"
                                          : "AUTO off: answers are offered on Reply");
    update_status();
}

static const char *hb_label_getter(void) {
    static char buf[24];
    if (hb_adjusting) {
        snprintf(buf, sizeof(buf), "HB: knob\n< %u min >", param_i_get(cfg.js8.hb_interval()));
        return buf;
    }
    if (!param_i_get(cfg.js8.hb())) return "Heart-\nbeat";
    if (hb_paused()) return "HB auto:\npaused";
    if (!js8_speed_heartbeats(cur_speed())) {
        snprintf(buf, sizeof(buf), "HB auto:\nnot %s", js8_speed_name(cur_speed()));
        return buf;
    }
    if (!hb_next_ms) return "HB auto:\nsoon";
    int secs = (int)((hb_next_ms - now_wall_ms() + 999) / 1000);
    if (secs < 1) return "HB auto:\nnow";
    if (secs < 60) snprintf(buf, sizeof(buf), "HB auto:\n%d s", secs);
    else snprintf(buf, sizeof(buf), "HB auto:\n%d:%02d", secs / 60, secs % 60);
    return buf;
}

/* Setting the interval with the main knob, as for auto CQ: press Heartbeat
 * (or wait) to finish. */
static void hb_adjust_start(button_data_t *btn) {
    if (cq_adjusting) cq_adjust_end();
    hb_adjusting    = true;
    hb_adjust_ms    = now_wall_ms();
    buttons_refresh(btn);
    update_tx_bar();
}

static void hb_adjust_end(void) {
    if (!hb_adjusting) return;
    hb_adjusting = false;
    if (btn_hb.disp_btn) buttons_refresh(&btn_hb);
    update_tx_bar();
    if (param_i_get(cfg.js8.hb()))
        msg_update_text_fmt("Auto HB every %u min (press Heartbeat to stop)", param_i_get(cfg.js8.hb_interval()));
}

static void hb_auto_stop(const char *why) {
    if (!param_i_get(cfg.js8.hb())) return;
    param_i_set(cfg.js8.hb(), false);
    hb_adjusting    = false;
    hb_next_ms      = 0;
    hb_paused_until = 0;
    if (btn_hb.disp_btn) buttons_refresh(&btn_hb);
    if (btn_hbackk.disp_btn) buttons_refresh(&btn_hbackk);
    msg_update_text_fmt("Auto HB off: %s", why);
    add_info_row("Auto HB off: %s", why);
    update_status();
}

static const char *hb_ack_label_getter(void) {
    if (!param_i_get(cfg.js8.hb_ack())) return "HB ACK:\nOff";
    if (hb_paused() && param_i_get(cfg.js8.hb())) return "HB ACK:\npaused";
    return (param_i_get(cfg.js8.auto_mode()) && param_i_get(cfg.js8.hb())) ? "HB ACK:\nOn" : "HB ACK:\nOn (idle)";
}

static void hb_ack_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    param_i_set(cfg.js8.hb_ack(), !param_i_get(cfg.js8.hb_ack()));
    buttons_refresh(btn);
    if (param_i_get(cfg.js8.hb_ack()) && !(param_i_get(cfg.js8.auto_mode()) && param_i_get(cfg.js8.hb()))) {
        msg_update_text_fmt("HB ACK acts only while AUTO and auto HB (hold Heartbeat) are on");
    }
    update_status();
}

/* ---- INFO / STATUS texts -------------------------------------------- */

/* js8_texts.txt couldn't be read or moved aside: never write over it. */
static bool texts_writable = true;

/* A data file was moved aside (or can't be written): say so on the screen
 * and in the log. */
static void data_file_notice(const char *notice) {
    if (!notice || !notice[0]) return;
    LV_LOG_USER("JS8: %s", notice);
    msg_update_text_fmt("%s", notice);
    add_info_row("%s", notice);
}

/* Desktop's default STATUS (Configuration.cpp "MyStatus"), the user's
 * choice (2026-10-09): what STATUS? is answered with until you change it. */
#define STATUS_DEFAULT "IDLE <MYIDLE> VERSION <MYVERSION>"

static void load_texts(void) {
    info_text[0] = status_text[0] = last_pota[0] = last_sota[0] = alert_words[0] = spot_note[0] = '\0';
    groups_text[0] = operator_call[0] = '\0';
    bool status_def = false; /* STATUSDEF=1: the default was given once */
    char buf[2048], notice[160];
    texts_writable = js8_file_read(JS8_TEXTS_PATH, buf, sizeof(buf), notice, sizeof(notice));
    data_file_notice(notice);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line[strcspn(line, "\r")] = '\0';
        if (strncmp(line, "INFO=", 5) == 0) snprintf(info_text, sizeof(info_text), "%s", line + 5);
        if (strncmp(line, "STATUS=", 7) == 0) snprintf(status_text, sizeof(status_text), "%s", line + 7);
        if (strcmp(line, "STATUSDEF=1") == 0) status_def = true;
        if (strncmp(line, "POTA=", 5) == 0) snprintf(last_pota, sizeof(last_pota), "%s", line + 5);
        if (strncmp(line, "SOTA=", 5) == 0) snprintf(last_sota, sizeof(last_sota), "%s", line + 5);
        if (strncmp(line, "ALERTS=", 7) == 0) js8_alert_words_normalise(line + 7, alert_words, sizeof(alert_words));
        if (strncmp(line, "GROUPS=", 7) == 0) js8_groups_normalise(line + 7, groups_text, sizeof(groups_text));
        if (strncmp(line, "OPERATOR=", 9) == 0 && !js8_operator_call_valid(line + 9, operator_call, sizeof(operator_call)))
            operator_call[0] = '\0';
        if (strncmp(line, "SPOTMODE=", 9) == 0 && line[9]) snprintf(spot_mode, sizeof(spot_mode), "%s", line + 9);
        if (strncmp(line, "SPOTHZ=", 7) == 0) spot_typed_hz = atoi(line + 7);
        if (strncmp(line, "SPOTTYPED=", 10) == 0) spot_use_typed = atoi(line + 10) != 0;
        if (strncmp(line, "SPOTNOTE=", 9) == 0) snprintf(spot_note, sizeof(spot_note), "%s", line + 9);
    }
    if (!spot_typed_hz) spot_use_typed = false;
    /* Every card so far saved an empty STATUS= without anyone choosing it:
     * the default goes in once (STATUSDEF=1 marks that); a STATUS cleared
     * after that stays empty, as on desktop. */
    if (!status_def) {
        if (!status_text[0]) snprintf(status_text, sizeof(status_text), "%s", STATUS_DEFAULT);
        if (texts_writable) save_texts();
    }
}

/* Written safely (js8_file_write): a power cut during a save used to leave
 * the file empty, losing every setting in it. */
static void save_texts(void) {
    if (!texts_writable) {
        msg_update_text_fmt("Not saved: %s can't be read or moved (check the SD card)", JS8_TEXTS_PATH);
        return;
    }
    char buf[2048];
    snprintf(buf, sizeof(buf),
             "INFO=%s\nSTATUS=%s\nPOTA=%s\nSOTA=%s\nALERTS=%s\n"
             "SPOTMODE=%s\nSPOTHZ=%d\nSPOTTYPED=%d\nSPOTNOTE=%s\n"
             "GROUPS=%s\nOPERATOR=%s\nSTATUSDEF=1\n",
             info_text, status_text, last_pota, last_sota, alert_words, spot_mode, (int)spot_typed_hz,
             spot_use_typed ? 1 : 0, spot_note, groups_text, operator_call);
    if (!js8_file_write(JS8_TEXTS_PATH, buf)) msg_update_text_fmt("Can't write %s", JS8_TEXTS_PATH);
}

static void texts_close(void) {
    hclear_btn = NULL;
    if (!texts_list) return;
    lv_obj_del_async(texts_list);
    texts_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void texts_close_cb(lv_event_t *e) {
    (void)e;
    texts_close();
}

/* The Settings list's items (user data): the texts are edit_t values. */
#define SETTINGS_RELAY    100
#define SETTINGS_ST_KEEP  101
#define SETTINGS_MSG_KEEP 102
#define SETTINGS_MILES    103
#define SETTINGS_MARKS    104
#define SETTINGS_TRESET   106 /* 105 was Time Sync now: the Time button (page 4) now */
#define SETTINGS_WF       107
#define SETTINGS_DECODE   108
#define SETTINGS_HCLEAR   109 /* clear the station history: press, then again within HCLEAR_MS */
#define SETTINGS_FREQ     110 /* Frequencies: JS8Call's / GhostNet / custom kHz (page 6's Freq before) */
#define SETTINGS_ULTRA    111 /* Ultra on the Speed button (experimental) */
#define HCLEAR_MS         5000

static int64_t   hclear_armed_ms; /* the first press (monotonic), 0: not armed */

/* Five seconds on without the second press: the line as it was. */
static void hclear_tick(void) {
    if (!hclear_armed_ms || now_mono_ms() - hclear_armed_ms < HCLEAR_MS) return;
    hclear_armed_ms = 0;
    if (texts_list && hclear_btn) lv_label_set_text(lv_obj_get_child(hclear_btn, 0), "Clear station history...");
}

static const char *relay_label(void) {
    return param_i_get(cfg.js8.relay()) ? "Relay: On" : "Relay: Off";
}

/* The label of a line that changes in place. */
static const char *settings_label(int which) {
    static char buf[40];
    switch (which) {
    case SETTINGS_RELAY: return relay_label();
    case SETTINGS_ST_KEEP:
        snprintf(buf, sizeof(buf), "Stations kept: %s",
                 st_keep_opts[param_i_get(cfg.js8.st_keep()) < ST_KEEP_N ? param_i_get(cfg.js8.st_keep()) : 2].label);
        return buf;
    case SETTINGS_MSG_KEEP:
        snprintf(buf, sizeof(buf), "Messages kept: %s",
                 msg_keep_opts[param_i_get(cfg.js8.msg_keep()) < MSG_KEEP_N ? param_i_get(cfg.js8.msg_keep()) : 0].label);
        return buf;
    case SETTINGS_MILES: return param_i_get(cfg.js8.miles()) ? "Distance: miles" : "Distance: km";
    case SETTINGS_MARKS: return param_i_get(cfg.js8.decode_marks()) ? "Decode marks: On" : "Decode marks: Off";
    case SETTINGS_DECODE: return param_i_get(cfg.js8.rx_all()) ? "Decode: All speeds" : "Decode: My speed";
    case SETTINGS_ULTRA: return param_i_get(cfg.js8.ultra()) ? "Ultra on Speed button: On" : "Ultra on Speed button: Off";
    case SETTINGS_FREQ: snprintf(buf, sizeof(buf), "Frequencies: %s", freq_name()); return buf;
    case SETTINGS_HCLEAR:
        if (hclear_armed_ms && now_mono_ms() - hclear_armed_ms < HCLEAR_MS) {
            int n = js8_history_station_count(history_db);
            snprintf(buf, sizeof(buf), "Press again: clear %d station%s", n, n == 1 ? "" : "s");
            return buf;
        }
        return "Clear station history...";
    case SETTINGS_WF:
        snprintf(buf, sizeof(buf), "Waterfall: %s", wf_avg_name[param_i_get(cfg.js8.wf_avg()) % WF_AVG_LEVELS]);
        return buf;
    case SETTINGS_TRESET:
        if (js8_drift_ms()) snprintf(buf, sizeof(buf), "Reset time drift (%+.1f s)", js8_drift_ms() / 1000.0);
        else snprintf(buf, sizeof(buf), "Reset time drift");
        return buf;
    }
    return "";
}

static void texts_item_cb(lv_event_t *e) {
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which >= SETTINGS_RELAY) { /* switched in place; the list stays */
        switch (which) {
        case SETTINGS_RELAY:
            /* Desktop's "Disable message relay (>)": it stops holding MSG
             * TO: messages for others too. */
            param_i_set(cfg.js8.relay(), !param_i_get(cfg.js8.relay()));
            msg_update_text_fmt(param_i_get(cfg.js8.relay())
                                    ? "Relay on: relays (>) are passed on and MSG TO: messages held, as desktop JS8Call"
                                    : "Relay off: relays (>) and MSG TO: messages for others are ignored");
            break;
        case SETTINGS_ST_KEEP:
            param_i_set(cfg.js8.st_keep(), (param_i_get(cfg.js8.st_keep()) + 1) % ST_KEEP_N);
            apply_station_keep();
            if (view_stations) rebuild_rows();
            msg_update_text_fmt("Stations stay listed %s after they were last heard",
                                st_keep_opts[param_i_get(cfg.js8.st_keep())].min ? st_keep_opts[param_i_get(cfg.js8.st_keep())].label
                                                                       : "until the radio is switched off, however long");
            break;
        case SETTINGS_MSG_KEEP:
            param_i_set(cfg.js8.msg_keep(), (param_i_get(cfg.js8.msg_keep()) + 1) % MSG_KEEP_N);
            if (!view_stations) rebuild_rows();
            if (param_i_get(cfg.js8.msg_keep()))
                msg_update_text_fmt("Messages leave the list %s after they arrived",
                                    msg_keep_opts[param_i_get(cfg.js8.msg_keep())].label);
            else msg_update_text_fmt("Messages stay in the list (the newest %d)", KEEP_ROWS);
            break;
        case SETTINGS_MILES:
            param_i_set(cfg.js8.miles(), !param_i_get(cfg.js8.miles()));
            if (view_stations) rebuild_rows();
            break;
        case SETTINGS_MARKS:
            param_i_set(cfg.js8.decode_marks(), !param_i_get(cfg.js8.decode_marks()));
            js8_rx_set_sync_marks(rx, param_i_get(cfg.js8.decode_marks()));
            if (!param_i_get(cfg.js8.decode_marks())) marks_reset();
            msg_update_text_fmt(param_i_get(cfg.js8.decode_marks())
                                    ? "Decode marks on: where the decoder is trying (cyan, white) and what it decoded (yellow)"
                                    : "Decode marks off");
            break;
        case SETTINGS_WF:
        {
            static const char *const what[WF_AVG_LEVELS] = {
                "every row as heard, as before",
                "rows lightly averaged",
                "rows averaged",
                "rows averaged most: least speckle and flicker",
            };
            int level = (param_i_get(cfg.js8.wf_avg()) + 1) % WF_AVG_LEVELS; /* Sharp > Light > Medium > Calm > Sharp */
            param_i_set(cfg.js8.wf_avg(), level);
            atomic_store(&wf_avg_level, level);
            msg_update_text_fmt("Waterfall: %s (%s)", wf_avg_name[level], what[level]);
        }
            break;
        case SETTINGS_TRESET:
            time_sync_reset();
            break;
        case SETTINGS_DECODE:
            decode_toggle();
            break;
        case SETTINGS_ULTRA:
            ultra_toggle();
            break;
        case SETTINGS_FREQ:
            /* Its own list, as page 6's Freq button opened it. */
            texts_close();
            freq_show();
            return;
        case SETTINGS_HCLEAR:
            /* Everything, every band, for good: a second press within 5 s. */
            if (!history_db) {
                msg_update_text_fmt("No station history: js8_history.db on the SD card couldn't be opened");
            } else if (hclear_armed_ms && now_mono_ms() - hclear_armed_ms < HCLEAR_MS) {
                hclear_armed_ms = 0;
                js8_history_clear(history_db);
                if (st_all_time) {
                    ever_load();
                    if (view_stations) rebuild_station_rows();
                }
                msg_update_text_fmt("Station history cleared: every station, INFO, STATUS and QSO, every band");
                add_info_row("Station history cleared");
            } else {
                hclear_armed_ms = now_mono_ms();
                msg_update_text_fmt("Clear the whole station history (every band)? Press again within 5 s; it can't be undone");
            }
            break;
        }
        lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), settings_label(which));
        return;
    }
    /* Straight into the keyboard. */
    popup_to_keyboard(&texts_list, which,
                      which == EDIT_INFO     ? info_text
                      : which == EDIT_STATUS ? status_text
                      : which == EDIT_GROUPS ? groups_text
                                             : operator_call);
}

static void texts_key_cb(lv_event_t *e) {
    popup_key(e, texts_close);
}

static lv_obj_t *settings_add(const char *label, int which) {
    lv_obj_t *b = list_add_item(texts_list, label);
    lv_obj_add_event_cb(b, texts_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    lv_obj_add_event_cb(b, texts_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_DOT);
    return b;
}

/* Settings: INFO and STATUS (what AUTO sends for INFO? and STATUS?), Relay
 * on/off, and the groups we're in, like desktop's settings. */
static void texts_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (texts_list) { /* Settings... again closes it */
        texts_close();
        return;
    }
    if (popup_guard()) return;
    if (query_list || aprs_list || composing) return;
    lv_group_remove_obj(table);
    texts_list = lv_list_create(dialog.obj);
    lv_obj_set_size(texts_list, 560, 290);
    lv_obj_align(texts_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(texts_list, &sony_24, 0);
    lv_obj_set_style_bg_color(texts_list, lv_color_hex(0x202020), 0);
    lv_obj_t *title = lv_list_add_text(texts_list, "Settings");
    lv_obj_set_style_text_font(title, &sony_22, 0);

    /* Time Sync is the Time button now (page 4); its reset stays here. */
    lv_obj_t *first = settings_add(settings_label(SETTINGS_TRESET), SETTINGS_TRESET);
    settings_add(settings_label(SETTINGS_FREQ), SETTINGS_FREQ);

    char label[TEXT_MAX + 16];
    snprintf(label, sizeof(label), "INFO: %s", info_text[0] ? info_text : "(not set)");
    settings_add(label, EDIT_INFO);
    snprintf(label, sizeof(label), "STATUS: %s", status_text[0] ? status_text : "(not set)");
    settings_add(label, EDIT_STATUS);
    settings_add(relay_label(), SETTINGS_RELAY);
    snprintf(label, sizeof(label), "Groups: %s", groups_text[0] ? groups_text : "(none)");
    settings_add(label, EDIT_GROUPS);
    settings_add(settings_label(SETTINGS_ST_KEEP), SETTINGS_ST_KEEP);
    settings_add(settings_label(SETTINGS_MSG_KEEP), SETTINGS_MSG_KEEP);
    settings_add(settings_label(SETTINGS_MILES), SETTINGS_MILES);
    settings_add(settings_label(SETTINGS_DECODE), SETTINGS_DECODE);
    settings_add(settings_label(SETTINGS_ULTRA), SETTINGS_ULTRA);
    settings_add(settings_label(SETTINGS_MARKS), SETTINGS_MARKS);
    settings_add(settings_label(SETTINGS_WF), SETTINGS_WF);
    hclear_armed_ms = 0;
    hclear_btn      = settings_add(settings_label(SETTINGS_HCLEAR), SETTINGS_HCLEAR);
    snprintf(label, sizeof(label), "Operator: %s", operator_call[0] ? operator_call : "(the station call)");
    settings_add(label, EDIT_OPERATOR);

    lv_obj_t *close = list_add_item(texts_list, "Close");
    lv_obj_set_style_text_color(close, lv_color_hex(0xffc040), 0);
    lv_obj_add_event_cb(close, texts_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(close, texts_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, close);
    lv_group_focus_obj(first);
    lv_group_set_editing(keyboard_group, false);
}

/* ---- APRS via @APRSIS -------------------------------------------------- */

/* JS8Call stations with "spot to APRS" on forward these to APRS-IS; formats
 * as desktop JS8Call and KF7MIX's JS8Spotter send them. Raw packets are
 * "@APRSIS CMD :<addressee padded to 9>:<text>"; APRS allows 67 characters
 * of text. The gateway sends you as your plain callsign (no SSID). */
#define APRS_CMD      "@APRSIS CMD :"
#define APRS_CMD_RAW  "@APRSIS CMD " /* any APRS packet body, e.g. a position */
#define APRS_TEXT_MAX 67

/* The Echo test is gone (VE7NHW, 2026-10-09): the ECHO service answers
 * (findu.com shows it), but relay stations don't bring its answer back
 * over JS8. The @ badge comes from any relayed answer (SMS, MPAD...). */
typedef enum {
    APRS_GRID,
    APRS_GPS,
    APRS_POTA,
    APRS_SOTA,
    APRS_SMS,
    APRS_EMAIL,
    APRS_WL_START,
    APRS_WL_TEXT,
    APRS_WL_SEND,
    APRS_MORE,
    APRS_COUNT
} aprs_item_t;

static const char *const aprs_labels[APRS_COUNT] = {
    "Spot my grid",   "Spot GPS position", "POTA spot",       "SOTA spot", "SMS text", "Email",
    "Winlink: start", "Winlink: text",     "Winlink: send",   "More services >",
};

/* More services >: APRS information services, each message filled in
 * (docs/feature-ideas.md: the ones that answered on 2026-09-28). A "%s"
 * in the text is your 6-character grid. MPAD's commands other than
 * weather use your last APRS position (MPAD docs), so Spot my grid
 * first. Answers come back over JS8 from an inbound relay station and
 * land in the Inbox from the service. */
#define APRS_POS_HINT "uses your last APRS position (Spot my grid first)"
static const struct {
    const char *label, *to, *text, *hint;
} aprs_svcs[] = {
    {"Weather today", "MPAD", "grid %s today", "MPAD weather for your grid, worldwide"},
    {"Weather tomorrow", "MPAD", "grid %s tomorrow", "MPAD weather for your grid, worldwide"},
    {"US forecast", "WXBOT", "grid %s brief", "WXBOT: US National Weather Service only"},
    {"Nearest wx station", "WXNOW", "N 1", "WXNOW: the nearest weather station; " APRS_POS_HINT},
    {"Sunrise / sunset", "MPAD", "riseset", "MPAD " APRS_POS_HINT},
    {"Nearest repeater", "MPAD", "repeater 2m", "MPAD " APRS_POS_HINT "; 2m, 70cm, c4fm, dmr..."},
    {"Next ISS pass", "MPAD", "satpass iss", "MPAD " APRS_POS_HINT},
    {"Nearest hospital", "MPAD", "osm hospital", "MPAD " APRS_POS_HINT},
    {"Nearest fuel", "MPAD", "osm fuel", "MPAD " APRS_POS_HINT},
    {"Drinking water", "MPAD", "osm drinking_water", "MPAD " APRS_POS_HINT},
    {"Where am I", "MPAD", "whereami", "MPAD " APRS_POS_HINT},
    {"Email my position", "MPAD", "posmsg ", "MPAD emails a map link: type the address; " APRS_POS_HINT},
    {"Airport weather", "MPAD", "metar", "MPAD: METAR of the nearest airport; " APRS_POS_HINT},
    {"Callsign lookup", "WHO-IS", "", "WHO-IS: type a callsign"},
    {"Magic 8-ball", "MPAD", "magic8ball", "MPAD's magic 8-ball"},
    {"Joke", "JOKE", "joke", "A joke from JOKE"},
};
#define APRS_SVCS (int)(sizeof(aprs_svcs) / sizeof(aprs_svcs[0]))

static unsigned aprs_msg_id;

/* APRS messages we sent with an id ("{04}"), so the gateway's receipt
 * ("ACK04}" / "REJ04}", relayed back over JS8) marks the right one instead
 * of landing in the Inbox as a message (bug hunt S7). Kept while the radio
 * is on (your choice): receipts come within minutes, and the ids restart
 * at power-on. */
#define APRS_SENT 16
typedef struct {
    char    id[6];     /* "04" */
    char    to[10];    /* the addressee: SMS, EMAIL-2, WLNK-1 ... */
    char    dest[48];  /* the message's first word: "@6045551234", an address */
    int64_t ms;
    bool    answered;  /* its receipt came */
} aprs_sent_t;
static aprs_sent_t aprs_sent[APRS_SENT];
static int         aprs_sent_next;
static aprs_sent_t aprs_pending; /* prepared, not yet queued; id "" = none */

/* "{04}" or "{AB12" at the end of an APRS message: its id (1-5 letters or
 * digits, "}" optional), or NULL. */
static const char *aprs_id_at_end(const char *body, char *id, size_t len) {
    const char *brace = strrchr(body, '{');
    if (!brace) return NULL;
    size_t n = 0;
    while (n < 5 && isalnum((unsigned char)brace[1 + n])) n++;
    const char *end = brace + 1 + n;
    if (!n || (*end == '}' ? end[1] : *end)) return NULL;
    snprintf(id, len, "%.*s", (int)n, brace + 1);
    return brace;
}

static void aprs_sent_commit(void) {
    if (!aprs_pending.id[0]) return;
    aprs_pending.ms          = now_wall_ms();
    aprs_sent[aprs_sent_next] = aprs_pending;
    aprs_sent_next            = (aprs_sent_next + 1) % APRS_SENT;
    aprs_pending.id[0]        = '\0';
}

static const char *aprs_kind(const char *to) {
    return !strcmp(to, "SMS") ? "SMS" : !strcmp(to, "EMAIL-2") ? "Email" : !strcmp(to, "WLNK-1") ? "Winlink" : to;
}

/* A gateway's receipt: the message it answers, marked. */
static void aprs_receipt(const js8_stored_t *k) {
    aprs_sent_t *s = NULL;
    for (int i = 1; i <= APRS_SENT && !s; i++) { /* newest first */
        aprs_sent_t *c = &aprs_sent[(aprs_sent_next - i + APRS_SENT) % APRS_SENT];
        if (c->id[0] && !strcasecmp(c->id, k->text)) s = c;
    }
    const char *what = k->rejected ? "rejected by the gateway" : "delivered";
    if (!s) { /* sent before a restart, or by another radio */
        msg_update_text_fmt("APRS: message {%s} %s (receipt from %s)", k->text, what, k->from);
        add_info_row("APRS receipt from %s: message {%s} %s", k->from, k->text, what);
        return;
    }
    if (s->answered) return; /* the same receipt again */
    s->answered = true;
    msg_update_text_fmt("%s {%s} to %s %s", aprs_kind(s->to), s->id, s->dest, what);
    add_info_row("%s {%s} to %s %s (receipt from %s)", aprs_kind(s->to), s->id, s->dest, what, k->from);
}

/* Before sending anything typed: APRS CMDs get checked, SMS/email/Winlink a
 * message ID like JS8Spotter's "{01}", and POTA/SOTA refs are remembered.
 * Anything else passes through unchanged. */
static bool aprs_prepare(const char *in, char *out, size_t size) {
    snprintf(out, size, "%s", in);
    aprs_pending.id[0] = '\0';
    size_t pre = strlen(APRS_CMD);
    if (strncmp(in, APRS_CMD, pre) != 0) return true;
    if (strlen(in) < pre + 10 || in[pre + 9] != ':') {
        msg_update_text_fmt("APRS: the addressee is 9 characters, then ':'");
        return false;
    }
    char to[10];
    memcpy(to, in + pre, 9);
    to[9] = '\0';
    for (int i = 8; i >= 0 && to[i] == ' '; i--) to[i] = '\0';
    const char *body = in + pre + 10;
    while (*body == ' ') body++;
    if (!*body) {
        msg_update_text_fmt("APRS: nothing to send to %s", to);
        return false;
    }

    /* Not Winlink: WLNK-1 answers each message with its own reply, so an
     * id (which asks for an ACK too) only brought a second acknowledgement
     * (VE7NHW on the air, beta 4). Plain APRS messages may go without. */
    bool want_id = !strcmp(to, "SMS") || !strcmp(to, "EMAIL-2");
    if (want_id && !strchr(body, '{')) {
        if (!aprs_msg_id) aprs_msg_id = (unsigned)(time(NULL) % 90);
        aprs_msg_id = aprs_msg_id % 99 + 1;
        snprintf(out, size, "%s{%02u}", in, aprs_msg_id);
    }
    /* 67 characters of text; the id "{04}" comes on top (APRS spec, bug
     * hunt S1: counting it cut SMS/email/Winlink to 63). */
    const char *text_out = out + pre + 10;
    size_t      text_len = strlen(text_out);
    char        id[6];
    const char *id_at    = aprs_id_at_end(text_out, id, sizeof(id));
    if (id_at) text_len = (size_t)(id_at - text_out);
    if (id_at && want_id) { /* its receipt will come back: remember it */
        snprintf(aprs_pending.id, sizeof(aprs_pending.id), "%s", id);
        snprintf(aprs_pending.to, sizeof(aprs_pending.to), "%s", to);
        snprintf(aprs_pending.dest, sizeof(aprs_pending.dest), "%.*s", (int)strcspn(body, " "), body);
        aprs_pending.answered = false;
    }
    if (text_len > APRS_TEXT_MAX) {
        msg_update_text_fmt("APRS allows %d characters of text (the {id} is extra), this is %u: shorten it",
                            APRS_TEXT_MAX, (unsigned)text_len);
        return false;
    }

    /* "! POTA PARK MHZ MODE ..." / "SUMMIT FREQ MODE ..." / old POTAGW "CALL PARK KHZ MODE ..." */
    char w1[24] = "", w2[24] = "", w3[24] = "";
    sscanf(body, "%23s %23s %23s", w1, w2, w3);
    if (!strcmp(to, "APSPOT") && !strcmp(w1, "!") && w3[0] && (!strcmp(w2, "POTA") || !strcmp(w2, "SOTA"))) {
        if (w2[0] == 'P') snprintf(last_pota, sizeof(last_pota), "%s", w3);
        else snprintf(last_sota, sizeof(last_sota), "%s", w3);
        save_texts();
    } else if (!strcmp(to, "POTAGW") && w2[0]) {
        snprintf(last_pota, sizeof(last_pota), "%s", w2);
        save_texts();
    } else if ((!strcmp(to, "APRS2SOTA") || !strcmp(to, "SOTA")) && w1[0]) {
        snprintf(last_sota, sizeof(last_sota), "%s", w1);
        save_texts();
    }
    return true;
}

/* The keyboard with `head` + `tail`, the cursor between them. */
static void aprs_compose(const char *head, const char *tail) {
    char text[TX_TEXT_MAX + 1];
    snprintf(text, sizeof(text), "%s%s", head, tail);
    compose_open(text);
    if (composing) lv_textarea_set_cursor_pos(textarea_window_text(), (int32_t)strlen(head));
}

/* Your QTH grid (6 characters) and its centre. False, with the reason
 * shown, if it isn't set. Any out-pointer may be NULL. */
static bool aprs_grid(char grid[8], double *lat, double *lon) {
    char g[8];
    snprintf(g, sizeof(g), "%.6s", my_grid());
    if (strlen(g) < 4) {
        msg_update_text_fmt("Set your grid first: APP > QTH");
        return false;
    }
    if (grid) memcpy(grid, g, sizeof(g));
    if (lat && lon) qth_str_to_pos(g, lat, lon);
    return true;
}

/* GPS: R1CBU 1.0 announces each gpsd report (MSG_GPS, on the GUI thread);
 * a 2D/3D fix and when it came are kept while JS8 is open. */
static void gps_msg_cb(void *s, lv_msg_t *m) {
    (void)s;
    (void)m;
    struct gps_data_t d;
    gps_get_snapshot(&d);
    gps_seen = time(NULL);
    if (d.fix.mode < MODE_2D || !isfinite(d.fix.latitude) || !isfinite(d.fix.longitude)) return;
    gps_lat  = d.fix.latitude;
    gps_lon  = d.fix.longitude;
    gps_when = time(NULL);
}

/* The latest fix and its age in seconds; false if none since JS8 opened. */
static bool gps_last_fix(double *lat, double *lon, int *age_s) {
    if (!gps_when) return false;
    *lat   = gps_lat;
    *lon   = gps_lon;
    *age_s = (int)(time(NULL) - gps_when);
    return true;
}

/* A current GPS fix, and its 10-character grid (about 20 x 35 m). False,
 * with the reason shown, if there is none. */
static bool aprs_gps(char grid[12], double *lat_out, double *lon_out) {
    double lat, lon;
    int    age;
    if (!gps_last_fix(&lat, &lon, &age)) {
        msg_update_text_fmt("No GPS fix: plug in a GPS and wait for a fix (APP > GPS shows it)");
        return false;
    }
    if (age > 120) {
        msg_update_text_fmt("No current GPS fix (last one %d min ago)", age / 60);
        return false;
    }
    char g[12];
    if (!js8_latlon_to_grid(lat, lon, 10, g, sizeof(g))) {
        msg_update_text_fmt("GPS position out of range");
        return false;
    }
    if (grid) memcpy(grid, g, sizeof(g));
    if (lat_out) *lat_out = lat;
    if (lon_out) *lon_out = lon;
    return true;
}

/* An APRS position report: "=4916.9 N/12307.2 WG MESSAGE". To about
 * 185 m: minutes to one decimal, the last digit a space (APRS position
 * ambiguity), which JS8 sends in far fewer bits than digits; the space
 * before the message lets JS8's word compression take it whole (6 frames
 * down to 5 for "MADE IT TO CAMP"). "/G" is the grid-square symbol, the
 * one JS8Call's own grid spots use. */
static void aprs_position(double lat, double lon, const char *comment, char *out, size_t size) {
    double alat = fabs(lat), alon = fabs(lon);
    int    lat_d = (int)alat, lon_d = (int)alon;
    double lat_m = (alat - lat_d) * 60.0, lon_m = (alon - lon_d) * 60.0;
    if (lat_m >= 59.95) lat_d++, lat_m = 0; /* don't print 60.0 */
    if (lon_m >= 59.95) lon_d++, lon_m = 0;
    snprintf(out, size, "=%02d%04.1f %c/%03d%04.1f %cG %s", lat_d, lat_m, lat < 0 ? 'S' : 'N', lon_d, lon_m,
             lon < 0 ? 'W' : 'E', comment);
}

/* What Spot my grid / GPS position sends. No message: "@APRSIS GRID
 * <grid>" as desktop JS8Call sends it (the gateway adds frequency and
 * SNR). With one: the GRID command can't carry it, so a position report
 * with the message as its comment goes through CMD (the gateway passes it
 * on as it is). False, with the reason shown, without a position. */
static bool beacon_text(bool gps, const char *message, char *out, size_t size, char grid[12]) {
    double lat, lon;
    if (gps ? !aprs_gps(grid, &lat, &lon) : !aprs_grid(grid, &lat, &lon)) return false;
    while (*message == ' ') message++;
    if (!*message) {
        snprintf(out, size, "@APRSIS GRID %s", grid);
        return true;
    }
    char pos[80];
    aprs_position(lat, lon, message, pos, sizeof(pos));
    snprintf(out, size, APRS_CMD_RAW "%s", pos);
    return true;
}

static void aprs_beacon(bool gps, const char *message) {
    char text[112], grid[12];
    if (!beacon_text(gps, message, text, sizeof(text), grid)) return;
    while (*message == ' ') message++;
    if (!tx_queue(text)) return;
    if (!*message)
        add_info_row("APRS: spotting %s at %s%s", my_call(), grid, gps ? " (GPS)" : "");
    else
        add_info_row("APRS: position %s (%s%s) with \"%s\"", my_call(), grid, gps ? ", GPS" : "", message);
}

/* While typing a beacon message: how long it will be on the air. */
static void beacon_changed_cb(lv_event_t *e) {
    (void)e;
    char text[112], grid[12];
    if (!beacon_text(edit_target == EDIT_BEACON_GPS, textarea_window_get(), text, sizeof(text), grid)) return;
    js8_tx_preview_t pv;
    js8_tx_preview(my_call(), my_grid(), text, cur_speed(), &pv);
    if (!pv.ok) return;
    const char *msg = textarea_window_get();
    while (*msg == ' ') msg++;
    if (*msg) msg_update_text_fmt("Position + message: %d frames, %.0f s", pv.frames, pv.seconds);
    else msg_update_text_fmt("Plain position: %d frames, %.0f s - type a message, or Enter", pv.frames, pv.seconds);
}

static void aprs_close(void) {
    if (!aprs_list) return;
    lv_obj_del_async(aprs_list); /* often called from one of its buttons */
    aprs_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void aprs_item_cb(lv_event_t *e) {
    aprs_item_t item = (aprs_item_t)(intptr_t)lv_event_get_user_data(e);
    /* Straight into the keyboard for most items: take the buttons out of
     * the group first, as Texts... does, or the keyboard opens unfocused. */
    popup_leave(&aprs_list);

    switch (item) {
    case APRS_MORE:
        aprs_open(true);
        return;
    case APRS_GRID:
    case APRS_GPS:
        /* A message to go with it, or just Enter. GPS: a fix first, so
         * nothing is typed for nothing. */
        if (item == APRS_GRID ? !aprs_grid(NULL, NULL, NULL) : !aprs_gps(NULL, NULL, NULL)) break;
        if (popup_to_keyboard(NULL, item == APRS_GRID ? EDIT_BEACON_GRID : EDIT_BEACON_GPS, NULL))
            beacon_changed_cb(NULL); /* the plain beacon's length, until you type */
        break;
    case APRS_POTA:
    case APRS_SOTA:
        spot_show(item == APRS_SOTA, (item == APRS_SOTA ? last_sota : last_pota)[0] ? SP_SEND : SP_REF);
        return;
    case APRS_SMS:
        aprs_compose(APRS_CMD "SMS      :@", "");
        msg_update_text_fmt("SMS (NA7Q gateway): 10-digit number, space, message");
        break;
    case APRS_EMAIL:
        aprs_compose(APRS_CMD "EMAIL-2  :", "");
        msg_update_text_fmt("Email: address, space, message");
        break;
    case APRS_WL_START:
        aprs_compose(APRS_CMD "WLNK-1   :SP ", "");
        msg_update_text_fmt("Winlink 1/3: address (or call), space, subject. Spot your grid first");
        break;
    case APRS_WL_TEXT:
        aprs_compose(APRS_CMD "WLNK-1   :", "");
        msg_update_text_fmt("Winlink 2/3: one line of the message; repeat for more lines");
        break;
    case APRS_WL_SEND:
        if (tx_queue(APRS_CMD "WLNK-1   :/EX")) msg_update_text_fmt("Winlink 3/3: sending /EX");
        break;
    default:
        break;
    }
    aprs_item_done();
}

/* After an item: the keyboard if it opened, else the knob back to the table. */
static void aprs_item_done(void) {
    if (composing) lv_group_set_editing(keyboard_group, true);
    else if (table) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

/* A More services item: its message in the keyboard, Enter sends it. */
static void aprs_svc_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    popup_leave(&aprs_list);
    char grid[8] = "", text[96], head[TX_TEXT_MAX + 1];
    if (strstr(aprs_svcs[i].text, "%s") && !aprs_grid(grid, NULL, NULL)) {
        aprs_item_done();
        return;
    }
    snprintf(text, sizeof(text), aprs_svcs[i].text, grid);
    snprintf(head, sizeof(head), APRS_CMD "%-9s:%s", aprs_svcs[i].to, text);
    aprs_compose(head, "");
    msg_update_text_fmt("%s; the answer comes to the Inbox", aprs_svcs[i].hint);
    aprs_item_done();
}

static void aprs_back_cb(lv_event_t *e) {
    (void)e;
    popup_leave(&aprs_list);
    aprs_open(false);
}

static void aprs_close_cb(lv_event_t *e) {
    (void)e;
    aprs_close();
}

static void aprs_key_cb(lv_event_t *e) {
    popup_key(e, aprs_close);
}

static void aprs_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (aprs_list) { /* APRS > again closes it */
        aprs_close();
        return;
    }
    if (popup_guard()) return;
    if (query_list || texts_list || composing) return;
    if (!my_call()[0]) {
        msg_update_text_fmt("Set your callsign first: APP > Callsign");
        return;
    }
    aprs_open(false);
}

/* The APRS list, or (`more`) the More services list in its place. */
static void aprs_open(bool more) {
    lv_group_remove_obj(table);
    aprs_list = lv_list_create(dialog.obj);
    lv_obj_set_size(aprs_list, 300, WF_HEIGHT - 10);
    lv_obj_align(aprs_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(aprs_list, &sony_24, 0);
    lv_obj_set_style_bg_color(aprs_list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(aprs_list, lv_color_white(), 0);
    lv_obj_t *t = lv_list_add_text(aprs_list, more ? "More APRS services" : "APRS via @APRSIS");
    lv_obj_set_style_text_font(t, &sony_22, 0);

    lv_obj_t *first = NULL;
    for (int i = 0; i < (more ? APRS_SVCS : APRS_COUNT); i++) {
        lv_obj_t *b = list_add_item(aprs_list, more ? aprs_svcs[i].label : aprs_labels[i]);
        lv_obj_add_event_cb(b, more ? aprs_svc_cb : aprs_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, aprs_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, b);
        if (!first) first = b;
    }
    if (more) {
        lv_obj_t *back = list_add_item(aprs_list, "< Back");
        lv_obj_set_style_text_color(back, lv_color_hex(0xffc040), 0);
        lv_obj_add_event_cb(back, aprs_back_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(back, aprs_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, back);
    }
    lv_obj_t *close = list_add_item(aprs_list, "Close");
    lv_obj_set_style_text_color(close, lv_color_hex(0xffc040), 0);
    lv_obj_add_event_cb(close, aprs_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(close, aprs_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, close);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(first);
}

/* ---- A station's History page ------------------------------------------
 *
 * An MFK press on a station in the Stations view or on the map: what the
 * station history knows of them. Their grid and distance, when you first
 * and last exchanged messages on this band and how they heard you, their
 * latest INFO and STATUS with how old they are, then the QSOs, newest
 * first (heartbeat-only exchanges left out); a QSO opens to its messages,
 * time-stamped. */

#define HPAGE_QSOS  64
#define HPAGE_LINES 120

static char    hpage_call[JS8_RX_CALL_LEN];
static int64_t hpage_ids[HPAGE_QSOS];
static int64_t hpage_back_qso; /* focused again when back from reading it */

/* "Oct 4 19:57Z"; another year: "Oct 4 2025". */
static void format_when(int64_t ms, char *buf, size_t size) {
    static const char *const mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t    t = (time_t)(ms / 1000), now = (time_t)(now_wall_ms() / 1000);
    struct tm tm, tn;
    gmtime_r(&t, &tm);
    gmtime_r(&now, &tn);
    if (tm.tm_year == tn.tm_year) snprintf(buf, size, "%s %d %02d:%02dZ", mon[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min);
    else snprintf(buf, size, "%s %d %d", mon[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
}

static void hpage_close(void) {
    if (!hpage_list) return;
    lv_obj_del_async(hpage_list);
    hpage_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void hpage_close_cb(lv_event_t *e) {
    (void)e;
    hpage_close();
}

static void hpage_key_cb(lv_event_t *e) {
    popup_key(e, hpage_close);
}

static void hpage_qso_cb(lv_event_t *e) {
    int i          = (int)(intptr_t)lv_event_get_user_data(e);
    hpage_back_qso = hpage_ids[i];
    char call[JS8_RX_CALL_LEN];
    snprintf(call, sizeof(call), "%s", hpage_call);
    hpage_close();
    hpage_open(call, hpage_back_qso);
}

static void hpage_back_cb(lv_event_t *e) {
    (void)e;
    char call[JS8_RX_CALL_LEN];
    snprintf(call, sizeof(call), "%s", hpage_call);
    hpage_close();
    hpage_open(call, 0);
}

/* A line of text, wrapped, not a button. */
static lv_obj_t *hpage_text(const char *text, uint32_t color) {
    lv_obj_t *t = lv_list_add_text(hpage_list, text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(t, &sony_22, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_ver(t, 3, 0);
    return t;
}

static lv_obj_t *hpage_item(const char *text, lv_event_cb_t cb, void *data, bool wrap) {
    lv_obj_t *b = list_add_item(hpage_list, text);
    lv_obj_set_style_text_font(b, &sony_22, 0);
    if (wrap) lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_WRAP);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_obj_add_event_cb(b, hpage_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    return b;
}

/* `qso_id` 0: the page; else that QSO's messages. */
static void hpage_open(const char *call, int64_t qso_id) {
    if (!history_db) {
        msg_update_text_fmt("No station history: js8_history.db on the SD card couldn't be opened");
        return;
    }
    snprintf(hpage_call, sizeof(hpage_call), "%s", call);
    lv_group_remove_obj(table);
    hpage_list = lv_list_create(dialog.obj);
    lv_obj_set_size(hpage_list, 740, WF_HEIGHT - 10);
    lv_obj_align(hpage_list, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_text_font(hpage_list, &sony_22, 0);
    lv_obj_set_style_bg_color(hpage_list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(hpage_list, lv_color_white(), 0);

    char      line[JS8_RX_TEXT_LEN + 64], when[24], age[8];
    lv_obj_t *first = NULL, *focus = NULL;
    int64_t   now   = now_wall_ms();
    if (qso_id) {
        /* Reading one QSO, oldest message first. */
        static js8_hist_qso_t  q[HPAGE_QSOS];
        static js8_hist_line_t l[HPAGE_LINES];
        int                    nq = js8_history_qsos(history_db, call, q, HPAGE_QSOS);
        const char            *band = "";
        for (int i = 0; i < nq; i++)
            if (q[i].id == qso_id) {
                format_when(q[i].start_ms, when, sizeof(when));
                band = q[i].band;
            }
        snprintf(line, sizeof(line), "%s  %s  %s", call, when, band_name(band));
        lv_obj_t *t = lv_list_add_text(hpage_list, line);
        lv_obj_set_style_text_font(t, &sony_22, 0);
        int n = js8_history_lines(history_db, qso_id, l, HPAGE_LINES);
        for (int i = 0; i < n; i++) {
            time_t    ts = (time_t)(l[i].ms / 1000);
            struct tm tm;
            gmtime_r(&ts, &tm);
            snprintf(line, sizeof(line), "%02d:%02d  %s", tm.tm_hour, tm.tm_min, l[i].text);
            lv_obj_t *b = hpage_item(line, NULL, NULL, true);
            /* Yours in the red of your own rows, heartbeat ACKs grey. */
            if (l[i].tx) lv_obj_set_style_text_color(b, lv_color_hex(0xff9a9a), 0);
            if (l[i].heartbeat) lv_obj_set_style_text_color(b, lv_color_hex(0x9a9a9a), 0);
            if (!first) first = b;
        }
        lv_obj_t *back = hpage_item("< Back", hpage_back_cb, NULL, false);
        lv_obj_set_style_text_color(back, lv_color_hex(0xffc040), 0);
        if (!first) first = back;
    } else {
        lv_obj_t *t = lv_list_add_text(hpage_list, call);
        lv_obj_set_style_text_font(t, &sony_22, 0);
        const char        *band = dial_band();
        js8_hist_contact_t c;
        bool               have = js8_history_contact(history_db, call, band, &c);
        js8_station_t      st;
        memset(&st, 0, sizeof(st));
        for (int i = 0; i < st_count; i++)
            if (strcasecmp(st_rows[i].call, call) == 0) st = st_rows[i];
        if (!st.grid[0] && have) snprintf(st.grid, sizeof(st.grid), "%s", c.grid);
        station_fields_t f;
        station_fields(&st, now, &f);
        if (st.grid[0]) {
            /* "az 58" as the map's label: this font has no degree sign. */
            snprintf(line, sizeof(line), "%s   %s   az %.*s", st.grid, f.dist, (int)strcspn(f.az, "\xC2"), f.az);
            hpage_text(line, 0xffffff);
        }
        if (have) {
            char first_s[24], last_s[24];
            format_when(c.first_ms, first_s, sizeof(first_s));
            format_when(c.last_ms, last_s, sizeof(last_s));
            int len = snprintf(line, sizeof(line), "%s: first %s, last %s", band_name(band), first_s, last_s);
            if (c.heard_us_ms && c.has_reported_snr)
                snprintf(line + len, sizeof(line) - len, "; heard you %+03d", c.reported_snr);
            hpage_text(line, 0xc8c8c8);
        } else {
            snprintf(line, sizeof(line), "No messages exchanged on %s yet", band_name(band));
            hpage_text(line, 0xc8c8c8);
        }
        for (int kind = 0; kind < 2; kind++) {
            js8_hist_info_t info;
            const char     *what = kind ? "STATUS" : "INFO";
            if (js8_history_info(history_db, call, kind, &info)) {
                format_age(now - info.ms, age, sizeof(age));
                snprintf(line, sizeof(line), strcmp(age, "now") ? "%s (%s ago): %s" : "%s (%s): %s", what,
                         strcmp(age, "now") ? age : "just now", info.text);
                hpage_text(line, 0xffd24a);
            } else {
                snprintf(line, sizeof(line), "%s: none heard yet", what);
                hpage_text(line, 0x9a9a9a);
            }
        }
        static js8_hist_qso_t q[HPAGE_QSOS];
        int                   nq = js8_history_qsos(history_db, call, q, HPAGE_QSOS);
        snprintf(line, sizeof(line), nq ? "QSOs, newest first:" : "No QSOs yet (heartbeats aren't listed)");
        hpage_text(line, 0xc8c8c8);
        for (int i = 0; i < nq; i++) {
            hpage_ids[i] = q[i].id;
            format_when(q[i].start_ms, when, sizeof(when));
            snprintf(line, sizeof(line), "%s   %s   %d message%s%s", when, band_name(q[i].band), q[i].lines,
                     q[i].lines == 1 ? "" : "s", q[i].logged ? "   logged" : "");
            lv_obj_t *b = hpage_item(line, hpage_qso_cb, (void *)(intptr_t)i, false);
            if (!first) first = b;
            if (q[i].id == hpage_back_qso) focus = b;
        }
    }
    lv_obj_t *close = hpage_item("Close", hpage_close_cb, NULL, false);
    lv_obj_set_style_text_color(close, lv_color_hex(0xffc040), 0);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(focus ? focus : first ? first : close);
    if (!qso_id) lv_obj_scroll_to_y(hpage_list, 0, LV_ANIM_OFF); /* the top lines in view */
}

/* ---- POTA / SOTA spot form (docs/SPOTS_PLAN.md) ------------------------ */

/* POTA goes to APSPOT, which posts to pota.app (needs a pota.app account);
 * SOTA to APRS2SOTA (needs registering with sotaspots.co.uk). Both take
 * MHz. The frequency is the JS8 dial, or one you typed (your SSB run),
 * each remembered with the mode and comment. Gateway replies come back
 * over APRS only: the radio never sees them. */

static const char *const spot_modes_pota[] = {"DATA", "SSB", "CW", "FM", "AM", "FT8"}; /* APSPOT's */
static const char *const spot_modes_sota[] = {"DATA", "SSB", "CW", "FM", "AM", "DV"};  /* APRS2SOTA's */
#define SPOT_MODES 6

static lv_obj_t *spot_items[SP_COUNT];
static lv_obj_t *spot_preview;

static const char *const *spot_modes(void) {
    return spot_sota ? spot_modes_sota : spot_modes_pota;
}

/* The mode to send: the remembered one if this gateway takes it. */
static const char *spot_mode_now(void) {
    for (int i = 0; i < SPOT_MODES; i++)
        if (!strcmp(spot_modes()[i], spot_mode)) return spot_modes()[i];
    return "DATA";
}

static int32_t spot_hz(void) {
    return spot_use_typed && spot_typed_hz ? spot_typed_hz : cparam_i_get(cfg.cur.fg_freq());
}

/* MHz with at least 3 decimals, more only when needed: "7.078", "7.1855". */
static void format_mhz(int32_t hz, char *buf, size_t size) {
    snprintf(buf, size, "%d.%06d", (int)(hz / 1000000), (int)(hz % 1000000));
    char *end = buf + strlen(buf) - 1;
    while (*end == '0' && end[-3] != '.') *end-- = '\0';
}

/* No comment typed: "JS8" while spotting the JS8 dial. */
static const char *spot_comment(void) {
    if (spot_note[0]) return spot_note;
    return spot_use_typed && spot_typed_hz ? "" : "JS8";
}

/* The APRS text after "@APRSIS CMD :<addressee>:". */
static void spot_body(char *out, size_t size) {
    char mhz[16];
    format_mhz(spot_hz(), mhz, sizeof(mhz));
    const char *note = spot_comment();
    if (spot_sota)
        snprintf(out, size, "%s %s %s %s%s%s", last_sota[0] ? last_sota : "?", mhz, spot_mode_now(), my_call(),
                 note[0] ? " " : "", note);
    else
        snprintf(out, size, "! POTA %s %s %s%s%s", last_pota[0] ? last_pota : "?", mhz, spot_mode_now(), note[0] ? " " : "",
                 note);
}

static bool spot_freq_empty(const char *text) {
    while (*text == ' ') text++;
    return !*text;
}

/* kHz ("7185.5"), or MHz below 1000 ("144.2"); 1.8 MHz to 1.3 GHz. False,
 * with the reason shown, keeps the keyboard open to fix it. */
static bool spot_parse_freq(const char *text, int32_t *out) {
    char  *end;
    double v = strtod(text, &end);
    if (end == text || v <= 0) {
        msg_update_text_fmt("Type the frequency in kHz, e.g. 7185");
        return false;
    }
    double hz = v < 1000 ? v * 1e6 : v * 1e3;
    if (hz < 1800000 || hz > 1300000000) {
        msg_update_text_fmt("Out of range: 1800 kHz - 1300 MHz");
        return false;
    }
    if (out) *out = (int32_t)(hz + 0.5);
    return true;
}

static void spot_close(void) {
    if (!spot_list) return;
    lv_obj_del_async(spot_list); /* often called from one of its buttons */
    spot_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void spot_label(spot_item_t item, char *line, size_t size) {
    char mhz[16];
    switch (item) {
    case SP_SEND:
        snprintf(line, size, "Send spot");
        break;
    case SP_REF:
        if (spot_sota) snprintf(line, size, "Summit: %s", last_sota[0] ? last_sota : "(type it)");
        else snprintf(line, size, "Park: %s", last_pota[0] ? last_pota : "(type it)");
        break;
    case SP_FREQ:
        format_mhz(spot_hz(), mhz, sizeof(mhz));
        snprintf(line, size, "Frequency: %s MHz (%s)", mhz, spot_use_typed && spot_typed_hz ? "typed" : "JS8 dial");
        break;
    case SP_MODE:
        snprintf(line, size, "Mode: %s", spot_mode_now());
        break;
    case SP_NOTE:
        if (spot_note[0]) snprintf(line, size, "Comment: %s", spot_note);
        else if (spot_comment()[0]) snprintf(line, size, "Comment: %s (auto)", spot_comment());
        else snprintf(line, size, "Comment: (none)");
        break;
    case SP_CLOSE:
        snprintf(line, size, "Close");
        break;
    default:
        line[0] = '\0';
    }
}

/* Labels and the message preview, in place. */
static void spot_refresh(void) {
    char line[96];
    for (int i = 0; i < SP_COUNT; i++) {
        if (!spot_items[i]) continue;
        spot_label((spot_item_t)i, line, sizeof(line));
        lv_label_set_text(lv_obj_get_child(spot_items[i], 0), line);
    }
    spot_body(line, sizeof(line));
    lv_label_set_text_fmt(spot_preview, "%s: %s", spot_sota ? "APRS2SOTA" : "APSPOT", line);
}

static void spot_send(void) {
    if (!(spot_sota ? last_sota : last_pota)[0]) {
        msg_update_text_fmt("Type the %s first", spot_sota ? "summit" : "park");
        return;
    }
    char body[96], text[TX_TEXT_MAX + 8], out[TX_TEXT_MAX + 8];
    spot_body(body, sizeof(body));
    snprintf(text, sizeof(text), APRS_CMD "%-9s:%s", spot_sota ? "APRS2SOTA" : "APSPOT", body);
    if (!aprs_prepare(text, out, sizeof(out))) return; /* too long: says so */
    if (!tx_queue(out)) return;
    add_info_row("%s spot: %s. A JS8Call APRS gateway must hear it; the reply goes to APRS, not here",
                 spot_sota ? "SOTA" : "POTA", body);
    spot_close();
}

static void spot_item_cb(lv_event_t *e) {
    spot_item_t item = (spot_item_t)(intptr_t)lv_event_get_user_data(e);
    int         edit = 0;
    switch (item) {
    case SP_SEND:
        spot_send();
        return;
    case SP_CLOSE:
        spot_close();
        return;
    case SP_MODE: { /* next mode, in place */
        const char *cur = spot_mode_now();
        int         i   = 0;
        while (i < SPOT_MODES && strcmp(spot_modes()[i], cur) != 0) i++;
        snprintf(spot_mode, sizeof(spot_mode), "%s", spot_modes()[(i + 1) % SPOT_MODES]);
        save_texts();
        spot_refresh();
        return;
    }
    case SP_FREQ: /* the keyboard, your last one filled in; empty = the JS8 dial */
        edit = EDIT_SPOT_FREQ;
        break;
    case SP_REF:
        edit = EDIT_SPOT_REF;
        break;
    case SP_NOTE:
        edit = EDIT_SPOT_NOTE;
        break;
    default:
        return;
    }
    /* A field: into the keyboard, then back here (see log_item_cb). */
    char khz[16] = ""; /* the last one typed, even while spotting the dial */
    if (spot_typed_hz) format_khz(spot_typed_hz, khz, sizeof(khz));
    popup_to_keyboard(&spot_list, edit,
                      edit == EDIT_SPOT_REF ? (spot_sota ? last_sota : last_pota) : edit == EDIT_SPOT_FREQ ? khz : spot_note);
    if (edit == EDIT_SPOT_REF)
        msg_update_text_fmt(spot_sota ? "Summit, e.g. VE7/LM-001" : "Park, e.g. CA-1234 (POTA uses US- and CA- now, not K- or VE-)");
    else if (edit == EDIT_SPOT_FREQ)
        msg_update_text_fmt("Frequency in kHz (7185) or MHz for VHF (144.2); clear it and Enter for the JS8 dial");
    else
        msg_update_text_fmt("Comment (optional; with none, a JS8 dial spot says JS8)");
}

static void spot_key_cb(lv_event_t *e) {
    popup_key(e, spot_close);
}

/* `focus`: the item to start on - Send, or the field just edited. */
static void spot_show(bool sota, spot_item_t focus) {
    spot_sota = sota;
    lv_group_remove_obj(table);
    spot_list = lv_list_create(dialog.obj);
    lv_obj_set_size(spot_list, 560, WF_HEIGHT - 10);
    lv_obj_align(spot_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(spot_list, &sony_24, 0);
    lv_obj_set_style_bg_color(spot_list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(spot_list, lv_color_white(), 0);
    lv_obj_t *t = lv_list_add_text(spot_list, sota ? "SOTA spot via APRS2SOTA (registration needed)"
                                                   : "POTA spot via APSPOT (your pota.app account)");
    lv_obj_set_style_text_font(t, &sony_22, 0);
    spot_preview = lv_list_add_text(spot_list, "");
    lv_obj_set_style_text_font(spot_preview, &sony_22, 0);
    lv_obj_set_style_text_color(spot_preview, lv_color_hex(0x1030a0), 0); /* on the list's light title bar */

    char line[96];
    for (int i = 0; i < SP_COUNT; i++) {
        spot_label((spot_item_t)i, line, sizeof(line));
        lv_obj_t *b = list_add_item(spot_list, line);
        lv_obj_add_event_cb(b, spot_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, spot_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, b);
        lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_DOT);
        spot_items[i] = b;
    }
    lv_obj_set_style_text_color(spot_items[SP_SEND], lv_color_hex(0x80ff80), 0);
    lv_obj_set_style_text_color(spot_items[SP_CLOSE], lv_color_hex(0xffc040), 0);
    spot_refresh();
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(spot_items[focus]);
    const char *ref = sota ? last_sota : last_pota;
    if (!sota && (!strncmp(ref, "K-", 2) || !strncmp(ref, "VE-", 3)))
        msg_update_text_fmt("POTA park numbers are US-/CA- now (was K-/VE-): check yours");
}

/* ---- Log QSO ----------------------------------------------------------- */

/* Like desktop JS8Call's Log QSO: the entry is filled in from the QSO and
 * goes to JS8_LOG_PATH (ADIF, on the SD card's DATA partition) and to the
 * radio's QSO database, which marks worked stations. A two-way QSO that
 * ends with 73 or SK opens it by itself (Log prompt), and nothing is
 * logged without Save. */

static bool any_popup(void) {
    for (size_t i = 0; i < POPUPS; i++)
        if (*popups[i].list) return true;
    return false;
}

static bool find_station(const char *call, js8_station_t *out) {
    return js8_stations_find(stations, call, now_wall_ms(), out);
}

/* 6-character grid from a current GPS fix (portable), else the QTH setting. */
static void my_log_grid(char *out, size_t size) {
    double lat, lon;
    int    age;
    if (gps_last_fix(&lat, &lon, &age) && age <= 120 && js8_latlon_to_grid(lat, lon, 6, out, size)) return;
    snprintf(out, size, "%s", my_grid());
}

/* Sent: the report we gave them, else how we heard them. Rcvd: the report
 * they gave us (a message or a heartbeat ack). */
static void log_reports_fill(const js8_qso_t *q, const js8_station_t *st) {
    char *sent = log_entry.rst_sent, *rcvd = log_entry.rst_rcvd;
    if (q && q->has_sent_snr) snprintf(sent, sizeof(log_entry.rst_sent), "%+03d", q->sent_snr);
    else if (q && q->has_heard_snr) snprintf(sent, sizeof(log_entry.rst_sent), "%+03d", q->heard_snr);
    else if (st) snprintf(sent, sizeof(log_entry.rst_sent), "%+03d", st->snr);
    if (q && q->has_rcvd_snr) snprintf(rcvd, sizeof(log_entry.rst_rcvd), "%+03d", q->rcvd_snr);
    else if (st && st->has_reported_snr) snprintf(rcvd, sizeof(log_entry.rst_rcvd), "%+03d", st->reported_snr);
}

/* What you typed in the Log popup during a QSO (Name, Note, a Grid you
 * typed), held for that station: Cancel now, and the Log at the end opens
 * with it, so the QSO is logged once with everything in. Forgotten on Save,
 * or with the QSO (30 min without it). Memory only. */
#define LOG_HELD_N       4
#define LOG_HELD_KEEP_MS (30 * 60 * 1000)
static struct {
    char    call[JS8_RX_CALL_LEN];
    char    grid[sizeof(log_entry.grid)]; /* "": not typed */
    char    name[sizeof(log_entry.name)];
    char    comment[sizeof(log_entry.comment)];
    int64_t ms; /* last typed */
} log_held[LOG_HELD_N];

static int log_held_find(const char *call) {
    for (int i = 0; i < LOG_HELD_N; i++)
        if (log_held[i].call[0] && strcasecmp(log_held[i].call, call) == 0) return i;
    return -1;
}

static void log_unhold(const char *call) {
    int i = log_held_find(call);
    if (i >= 0) memset(&log_held[i], 0, sizeof(log_held[i]));
}

/* After a field was typed: keep the popup's typed fields for its station;
 * false when none is filled in (nothing held). */
static bool log_hold(void) {
    if (!log_entry.call[0]) return false;
    if (!log_entry.name[0] && !log_entry.comment[0] && !log_grid_typed) {
        log_unhold(log_entry.call);
        return false;
    }
    int i = log_held_find(log_entry.call);
    if (i < 0) { /* a free slot, else the oldest */
        i = 0;
        for (int k = 0; k < LOG_HELD_N; k++) {
            if (!log_held[k].call[0]) {
                i = k;
                break;
            }
            if (log_held[k].ms < log_held[i].ms) i = k;
        }
    }
    snprintf(log_held[i].call, sizeof(log_held[i].call), "%s", log_entry.call);
    snprintf(log_held[i].grid, sizeof(log_held[i].grid), "%s", log_grid_typed ? log_entry.grid : "");
    snprintf(log_held[i].name, sizeof(log_held[i].name), "%s", log_entry.name);
    snprintf(log_held[i].comment, sizeof(log_held[i].comment), "%s", log_entry.comment);
    log_held[i].ms = now_wall_ms();
    return true;
}

/* Into a freshly prepared entry: what was held for its station, unless both
 * the QSO and the typing are older than 30 min. */
static void log_restore(bool have_q, int64_t now) {
    int i = log_held_find(log_entry.call);
    if (i < 0) return;
    if (!have_q && now - log_held[i].ms > LOG_HELD_KEEP_MS) {
        memset(&log_held[i], 0, sizeof(log_held[i]));
        return;
    }
    if (log_held[i].grid[0]) {
        snprintf(log_entry.grid, sizeof(log_entry.grid), "%s", log_held[i].grid);
        log_grid_typed = true;
    }
    snprintf(log_entry.name, sizeof(log_entry.name), "%s", log_held[i].name);
    snprintf(log_entry.comment, sizeof(log_entry.comment), "%s", log_held[i].comment);
}

static void log_prepare(const char *call) {
    int64_t       now = now_wall_ms();
    js8_qso_t     q;
    js8_station_t st;
    bool          have_q  = js8_qsos_get(qsos, call, now, &q);
    bool          have_st = find_station(call, &st);

    memset(&log_entry, 0, sizeof(log_entry));
    log_grid_typed = false;
    snprintf(log_entry.call, sizeof(log_entry.call), "%s", have_q ? q.call : call);
    snprintf(log_entry.grid, sizeof(log_entry.grid), "%s", have_q && q.grid[0] ? q.grid : have_st ? st.grid : "");

    log_reports_fill(have_q ? &q : NULL, have_st ? &st : NULL);

    log_entry.on_ms   = have_q ? q.start_ms : now;
    log_entry.off_ms  = now;
    log_entry.freq_hz = (uint64_t)cparam_i_get(cfg.cur.fg_freq()) + param_i_get(cfg.js8.tx_freq());
    snprintf(log_entry.my_call, sizeof(log_entry.my_call), "%s", my_call());
    snprintf(log_entry.op_call, sizeof(log_entry.op_call), "%s", operator_call);
    my_log_grid(log_entry.my_grid, sizeof(log_entry.my_grid));
    float pwr          = param_f_get(cfg.pwr());
    log_entry.tx_pwr_w = pwr > TX_PLAYER_MAX_PWR_W ? TX_PLAYER_MAX_PWR_W : pwr;
    if (param_i_get(cfg.js8.log_activation()) == 1) snprintf(log_entry.pota_ref, sizeof(log_entry.pota_ref), "%s", last_pota);
    if (param_i_get(cfg.js8.log_activation()) == 2) snprintf(log_entry.sota_ref, sizeof(log_entry.sota_ref), "%s", last_sota);
    log_restore(have_q, now);
}

static void log_close(void) {
    if (!log_list) return;
    lv_obj_del_async(log_list); /* often called from one of its buttons */
    log_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void log_save(void) {
    if (!log_entry.call[0] || !log_entry.my_call[0]) {
        msg_update_text_fmt("Nothing to log");
        return;
    }
    log_entry.off_ms = now_wall_ms();
    char err[96];
    if (!js8_log_append(JS8_LOG_PATH, &log_entry, err, sizeof(err))) {
        msg_update_text_fmt("Not logged: %s", err);
        return;
    }
    /* The radio's QSO database too: worked-before marks here and in FT8. */
    char            *canon = util_canonize_callsign(log_entry.call, false);
    qso_log_record_t rec   = qso_log_record_create(
        log_entry.my_call, canon ? canon : log_entry.call, (time_t)(log_entry.on_ms / 1000), MODE_JS8,
        atoi(log_entry.rst_sent), atoi(log_entry.rst_rcvd), log_entry.freq_hz, log_entry.name[0] ? log_entry.name : NULL,
        NULL, log_entry.my_grid, log_entry.grid);
    free(canon);
    qso_log_record_save(rec);
    worked_forget();
    js8_map_worked_add(log_entry.call, log_entry.grid); /* no more NEW outline on the map */

    log_unhold(log_entry.call);
    js8_qsos_logged(qsos, log_entry.call);
    js8_history_logged(history_db, log_entry.call, log_entry.freq_hz, now_wall_ms());
    if (strcmp(log_pending, log_entry.call) == 0) log_pending[0] = '\0';
    const char *band = js8_log_band(log_entry.freq_hz);
    msg_update_text_fmt("Logged %s%s%s", log_entry.call, band[0] ? " on " : "", band);
    add_info_row("Logged %s %s %s/%s%s%s", log_entry.call, band, log_entry.rst_sent[0] ? log_entry.rst_sent : "-",
                 log_entry.rst_rcvd[0] ? log_entry.rst_rcvd : "-", log_entry.pota_ref[0] ? " POTA " : "",
                 log_entry.pota_ref[0] ? log_entry.pota_ref : log_entry.sota_ref);
    if (view_stations) rebuild_station_rows();
}

static void log_item_cb(lv_event_t *e) {
    log_item_t item = (log_item_t)(intptr_t)lv_event_get_user_data(e);
    if (item == LOG_SAVE || item == LOG_CANCEL) {
        if (item == LOG_SAVE) log_save();
        else if (strcmp(log_pending, log_entry.call) == 0) log_pending[0] = '\0'; /* not another station's */
        log_close();
        return;
    }
    /* A field: into the keyboard, then back here (see texts_item_cb). */
    popup_to_keyboard(&log_list, item == LOG_GRID ? EDIT_LOG_GRID : item == LOG_NAME ? EDIT_LOG_NAME : EDIT_LOG_NOTE,
                      item == LOG_GRID ? log_entry.grid : item == LOG_NAME ? log_entry.name : log_entry.comment);
}

/* ESC on the prompt for a QSO that ended means "not now": Log QSO then
 * takes the selected station again (B-24, your choice). */
static void log_esc(void) {
    if (log_pending[0] && strcmp(log_entry.call, log_pending) == 0) log_pending[0] = '\0';
    log_close();
}

static void log_key_cb(lv_event_t *e) {
    popup_key(e, log_esc);
}

static lv_obj_t *log_add(log_item_t item, const char *label) {
    lv_obj_t *b = list_add_item(log_list, label);
    lv_obj_add_event_cb(b, log_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item);
    lv_obj_add_event_cb(b, log_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    return b;
}

static void log_reports_line(char *line, size_t size) {
    snprintf(line, size, "Sent %s  Rcvd %s  %.3f MHz  %.0f W", log_entry.rst_sent[0] ? log_entry.rst_sent : "-",
             log_entry.rst_rcvd[0] ? log_entry.rst_rcvd : "-", log_entry.freq_hz / 1e6, log_entry.tx_pwr_w);
}

/* They sent something while the popup is open (often their 73 after ours
 * opened it): take in a new report or grid, in place, focus unchanged. */
static void log_refresh(void) {
    js8_qso_t q;
    if (!log_list || !js8_qsos_get(qsos, log_entry.call, now_wall_ms(), &q)) return;
    log_reports_fill(&q, NULL);
    char line[160];
    log_reports_line(line, sizeof(line));
    lv_label_set_text(log_reports, line);
    if (!log_grid_typed && q.grid[0] && strcmp(q.grid, log_entry.grid) != 0) {
        snprintf(log_entry.grid, sizeof(log_entry.grid), "%s", q.grid);
        snprintf(line, sizeof(line), "Grid: %s", log_entry.grid);
        lv_label_set_text(lv_obj_get_child(log_grid_btn, 0), line);
    }
}

/* `focus`: the item to start on - Save, or the field just edited. */
static void log_list_open(log_item_t focus) {
    lv_group_remove_obj(table);
    log_list = lv_list_create(dialog.obj);
    lv_obj_set_size(log_list, 560, WF_HEIGHT - 10);
    lv_obj_align(log_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(log_list, &sony_24, 0);
    lv_obj_set_style_bg_color(log_list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(log_list, lv_color_white(), 0);

    char      line[160], on[8], off[8];
    time_t    t_on = (time_t)(log_entry.on_ms / 1000), t_off = (time_t)(log_entry.off_ms / 1000);
    struct tm tm;
    strftime(on, sizeof(on), "%H:%M", gmtime_r(&t_on, &tm));
    strftime(off, sizeof(off), "%H:%M", gmtime_r(&t_off, &tm));
    snprintf(line, sizeof(line), "Log %s  %s  %s-%s UTC", log_entry.call, js8_log_band(log_entry.freq_hz), on, off);
    lv_obj_t *t = lv_list_add_text(log_list, line);
    lv_obj_set_style_text_font(t, &sony_22, 0);
    log_reports_line(line, sizeof(line));
    log_reports = lv_list_add_text(log_list, line);
    lv_obj_set_style_text_font(log_reports, &sony_22, 0);

    lv_obj_t *items[LOG_CANCEL + 1];
    items[LOG_SAVE] = log_add(LOG_SAVE, "Save to log");
    lv_obj_set_style_text_color(items[LOG_SAVE], lv_color_hex(0x80ff80), 0);
    snprintf(line, sizeof(line), "Grid: %s", log_entry.grid[0] ? log_entry.grid : "(none)");
    log_grid_btn = items[LOG_GRID] = log_add(LOG_GRID, line);
    snprintf(line, sizeof(line), "Name: %s", log_entry.name[0] ? log_entry.name : "(none)");
    items[LOG_NAME] = log_add(LOG_NAME, line);
    snprintf(line, sizeof(line), "Comment: %s", log_entry.comment[0] ? log_entry.comment : "(none)");
    items[LOG_NOTE] = log_add(LOG_NOTE, line);
    if (log_entry.pota_ref[0] || log_entry.sota_ref[0]) {
        snprintf(line, sizeof(line), "Activating %s %s", log_entry.pota_ref[0] ? "POTA" : "SOTA",
                 log_entry.pota_ref[0] ? log_entry.pota_ref : log_entry.sota_ref);
        t = lv_list_add_text(log_list, line);
        lv_obj_set_style_text_font(t, &sony_22, 0);
    }
    if (log_entry.op_call[0]) { /* Settings: operator, not the station call */
        snprintf(line, sizeof(line), "Operator %s (station %s)", log_entry.op_call, log_entry.my_call);
        t = lv_list_add_text(log_list, line);
        lv_obj_set_style_text_font(t, &sony_22, 0);
    }
    items[LOG_CANCEL] = log_add(LOG_CANCEL, "Cancel");
    lv_obj_set_style_text_color(items[LOG_CANCEL], lv_color_hex(0xffc040), 0);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(items[focus]);
}

/* Back from the keyboard: keep the value (NULL: cancelled), reopen. */
static void log_edit_done(const char *text) {
    /* Copy first: closing the window frees the text. */
    char  buf[sizeof(alert_words)];
    char *value = NULL;
    if (text) value = strncpy(buf, text, sizeof(buf) - 1), buf[sizeof(buf) - 1] = '\0', buf;
    int target  = edit_target;
    edit_target = 0;
    compose_close();
    if (value) {
        switch (target) {
        case EDIT_LOG_GRID: { /* checked in compose_ok_cb; in capitals, as logs have them */
            char *g = value + strspn(value, " ");
            size_t n = strcspn(g, " ");
            g[n]     = '\0';
            for (char *c = g; *c; c++) *c = (char)toupper((unsigned char)*c);
            snprintf(log_entry.grid, sizeof(log_entry.grid), "%s", g);
            log_grid_typed = true;
            break;
        }
        case EDIT_LOG_NAME:
            snprintf(log_entry.name, sizeof(log_entry.name), "%s", value);
            break;
        case EDIT_LOG_NOTE:
            snprintf(log_entry.comment, sizeof(log_entry.comment), "%s", value);
            break;
        case EDIT_POTA_REF:
            snprintf(last_pota, sizeof(last_pota), "%s", value);
            save_texts();
            break;
        case EDIT_SOTA_REF:
            snprintf(last_sota, sizeof(last_sota), "%s", value);
            save_texts();
            break;
        case EDIT_ALERT_WORDS:
            js8_alert_words_normalise(value, alert_words, sizeof(alert_words));
            save_texts();
            if (view_stations) rebuild_station_rows();
            break;
        case EDIT_FREQ:
            tune_custom(value);
            return;
        case EDIT_SPOT_REF:
            if (spot_sota) snprintf(last_sota, sizeof(last_sota), "%s", value);
            else snprintf(last_pota, sizeof(last_pota), "%s", value);
            save_texts();
            if (btn_act.disp_btn) buttons_refresh(&btn_act);
            break;
        case EDIT_SPOT_FREQ: /* a frequency: spot it (remembered); empty: the JS8 dial */
            if (spot_freq_empty(value)) spot_use_typed = false;
            else spot_use_typed = spot_parse_freq(value, &spot_typed_hz);
            save_texts();
            break;
        case EDIT_SPOT_NOTE:
            snprintf(spot_note, sizeof(spot_note), "%s", value);
            save_texts();
            break;
        case EDIT_BEACON_GRID:
        case EDIT_BEACON_GPS:
            aprs_beacon(target == EDIT_BEACON_GPS, value);
            return;
        }
    }
    if (target >= EDIT_BEACON_GRID) { /* cancelled: nothing sent */
        msg_update_text_fmt("Position not sent");
        return;
    }
    if (target == EDIT_FREQ) return; /* cancelled */
    if (target >= EDIT_SPOT_REF) { /* back on the field just edited */
        spot_show(spot_sota, target == EDIT_SPOT_REF ? SP_REF : target == EDIT_SPOT_FREQ ? SP_FREQ : SP_NOTE);
        return;
    }
    if (target == EDIT_ALERT_WORDS) {
        alerts_show();
        return;
    }
    if (target == EDIT_POTA_REF || target == EDIT_SOTA_REF) {
        if (btn_act.disp_btn) buttons_refresh(&btn_act);
        return;
    }
    /* Back on the field just edited: Save is a separate, deliberate step. */
    bool held = log_hold();
    log_list_open(target == EDIT_LOG_GRID ? LOG_GRID : target == EDIT_LOG_NAME ? LOG_NAME : LOG_NOTE);
    /* Not once the QSO has ended (their 73 while typing): Save now. */
    if (held && value && strcmp(log_pending, log_entry.call) != 0)
        msg_update_text_fmt("Kept for %s: Cancel for now, Save at the end of the QSO", log_entry.call);
}

static void log_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (log_list) { /* Log QSO again closes it */
        log_close();
        return;
    }
    if (popup_guard()) return;
    if (composing) return;
    if (!my_call()[0]) {
        msg_update_text_fmt("Set your callsign first: APP > Callsign");
        return;
    }
    /* A QSO that just ended, unless you've selected another station since
     * (your choice): the ended one is still there when you select it. It's
     * forgotten with the QSO (30 min), or once logged. */
    char      call[JS8_RX_CALL_LEN];
    float     freq;
    int       snr;
    js8_qso_t q;
    bool      selected = selected_station(call, sizeof(call), &freq, &snr);
    if (log_pending[0] && !js8_qsos_get(qsos, log_pending, now_wall_ms(), &q)) log_pending[0] = '\0';
    if (log_pending[0] && (!selected || strcmp(call, log_pending) == 0)) {
        snprintf(call, sizeof(call), "%s", log_pending);
    } else if (!selected) {
        msg_update_text_fmt("Select a station first (MFK)");
        return;
    }
    log_prepare(call);
    log_list_open(LOG_SAVE);
}

/* A two-way QSO ended with 73 / SK. */
static void log_offer(const char *call) {
    snprintf(log_pending, sizeof(log_pending), "%s", call);
    if (!param_i_get(cfg.js8.log_prompt())) {
        add_info_row("QSO with %s ended: Log QSO (page 5) to log it", call);
        return;
    }
    if (composing || any_popup()) {
        msg_update_text_fmt("QSO with %s ended: Log QSO (page 5) to log it", call);
        add_info_row("QSO with %s ended: Log QSO (page 5) to log it", call);
        return;
    }
    log_prepare(call);
    log_list_open(LOG_SAVE);
    msg_update_text_fmt("QSO with %s ended - Save to log?", call);
}

static const char *prompt_label_getter(void) {
    return param_i_get(cfg.js8.log_prompt()) ? "Log\nprompt: On" : "Log\nprompt: Off";
}

static void prompt_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    param_i_set(cfg.js8.log_prompt(), !param_i_get(cfg.js8.log_prompt()));
    buttons_refresh(btn);
    msg_update_text_fmt(param_i_get(cfg.js8.log_prompt()) ? "Offer to log when a QSO ends with 73 or SK"
                                                : "No log prompt: use Log QSO");
}

/* Activating a park or summit: MY_SIG / MY_SOTA_REF in the log. The
 * reference is the one last spotted via APRS, or set by holding this. */
static const char *act_label_getter(void) {
    static char label[40];
    switch (param_i_get(cfg.js8.log_activation())) {
    case 1:
        snprintf(label, sizeof(label), "POTA\n%s", last_pota[0] ? last_pota : "(hold: set)");
        break;
    case 2:
        snprintf(label, sizeof(label), "SOTA\n%s", last_sota[0] ? last_sota : "(hold: set)");
        break;
    default:
        snprintf(label, sizeof(label), "Activ.:\nOff");
        break;
    }
    return label;
}

static void act_cb(button_data_t *btn) {
    user_touch();
    if (popup_guard()) return;
    uint8_t v = (param_i_get(cfg.js8.log_activation()) + 1) % 3;
    param_i_set(cfg.js8.log_activation(), v);
    buttons_refresh(btn);
    const char *ref = v == 1 ? last_pota : last_sota;
    if (v == 0) msg_update_text_fmt("Not activating: no park or summit in the log");
    else if (!ref[0]) msg_update_text_fmt("Hold this button to set your %s", v == 1 ? "park" : "summit");
    else msg_update_text_fmt("Logging as %s %s (hold to change)", v == 1 ? "POTA" : "SOTA", ref);
}

static void act_hold_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (popup_guard() || composing) return;
    uint8_t v = param_i_get(cfg.js8.log_activation());
    if (v == 0) {
        msg_update_text_fmt("Press to choose POTA or SOTA first");
        return;
    }
    popup_to_keyboard(NULL, v == 1 ? EDIT_POTA_REF : EDIT_SOTA_REF, v == 1 ? last_pota : last_sota);
}

/* ---- Inbox and messages -------------------------------------------------- */

/* Like desktop JS8Call: "CALL MSG text" to us (checksum good) goes to the
 * inbox, answered with "CALL ACK" (sent by AUTO, else offered on Reply).
 * The Query list sends messages: MSG for their inbox, MSG TO: to leave one
 * at their station for someone else, QUERY MSGS to ask what they hold. */

#define INBOX_ROWS 200 /* all of them (Inbox::MAX_MESSAGES): an older unread one couldn't be opened (bug hunt 9) */
#define HELD_ROWS  100 /* HeldMessages::MAX_MESSAGES */

static int inbox_view_id; /* the message shown, 0: the list */

/* Green, like a selected Settings tab, while there are unread messages. */
static void inbox_refresh_button(void) {
    buttons_mark(&btn_inbox, inbox && js8_inbox_unread(inbox) > 0);
}

static const char *inbox_label_getter(void) {
    static char label[24];
    int         unread = js8_inbox_unread(inbox);
    if (unread) snprintf(label, sizeof(label), "Inbox\n%d new", unread);
    else snprintf(label, sizeof(label), "Inbox");
    return label;
}

/* What js8_process() kept: a MSG for us (or our group) in the inbox, or a
 * "MSG TO:W1ABC ..." held here until W1ABC asks (QUERY MSGS, QUERY MSG n,
 * our HB ack's "MSG ID n", or our RETRIEVE MSG), as desktop does. */
static void stored_received(const js8_stored_t *k) {
    if (k->kind == JS8_STORED_APRS_RECEIPT) {
        aprs_receipt(k);
        return;
    }
    char from[JS8_PATH_LEN + 16];
    js8_path_display(k->path[0] ? k->path : k->from, from, sizeof(from));
    if (k->kind == JS8_STORED_INBOX) {
        bool group = k->to[0] == '@';
        if (k->id < 0) msg_update_text_fmt("Message from %s: can't save %s, so no ACK sent", from, JS8_INBOX_PATH);
        else if (!k->resend && group) msg_update_text_fmt("New message to %s from %s - Inbox on page 3", k->to, from);
        else if (!k->resend) msg_update_text_fmt("New message from %s - Inbox on page 3", from);
        if (!k->resend) add_info_row("Message from %s in the Inbox: %s", from, k->text);
        if (!k->resend && (param_i_get(cfg.js8.alerts()) & JS8_ALERT_INBOX)) alert_beep(2);
        inbox_refresh_button();
        update_status();
    } else if (k->kind == JS8_STORED_HELD && !k->resend) {
        if (k->id < 0) {
            msg_update_text_fmt("Message from %s for %s: can't save %s, so no ACK sent", from, k->to, JS8_HELD_PATH);
            return;
        }
        msg_update_text_fmt("Holding a message from %s for %s (Inbox)", from, k->to);
        add_info_row("Holding message %d from %s for %s: %s", k->id, from, k->to, k->text);
    }
}

/* Same message as the transmitter reports it: it sends upper case, trimmed. */
static bool same_tx_text(const char *a, const char *b) {
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == ' ') la--;
    while (lb && b[lb - 1] == ' ') lb--;
    return la == lb && strncasecmp(a, b, la) == 0;
}

/* A held message's delivery was queued: it counts once it has all gone.
 * A few can be on their way; with all taken, the oldest is forgotten. */
static void deliver_start(int id, const char *group_call, const char *text) {
    int slot = 0;
    while (slot < DELIVERIES - 1 && deliveries[slot].id) slot++;
    if (deliveries[slot].id) { /* all taken */
        memmove(&deliveries[0], &deliveries[1], (DELIVERIES - 1) * sizeof(deliveries[0]));
        slot = DELIVERIES - 1;
    }
    deliveries[slot].id = id;
    snprintf(deliveries[slot].group_call, sizeof(deliveries[slot].group_call), "%s", group_call ? group_call : "");
    snprintf(deliveries[slot].text, sizeof(deliveries[slot].text), "%s", text);
}

/* A message ended. If it was a delivery, sent in full it's delivered;
 * stopped halfway, it stays held, and their next QUERY MSGS or heartbeat
 * is offered it again. */
static void deliver_end(const char *text, bool completed) {
    for (int i = 0; i < DELIVERIES; i++) {
        if (!deliveries[i].id || !same_tx_text(text, deliveries[i].text)) continue;
        int id = deliveries[i].id;
        if (completed) {
            if (deliveries[i].group_call[0]) js8_held_group_delivered(held, id, deliveries[i].group_call);
            else js8_held_delivered(held, id);
            add_info_row("Held message %d delivered", id);
        } else {
            add_info_row("Held message %d stopped before the end: still held", id);
        }
        deliveries[i].id = 0;
        return;
    }
}

static void inbox_close(void) {
    if (!inbox_list) return;
    lv_obj_del_async(inbox_list); /* often called from one of its buttons */
    inbox_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

/* Leave the popup from one of its own buttons, going somewhere else (the
 * keyboard, or another view): buttons out of the group now, list later. */
static void inbox_leave(void) {
    popup_leave(&inbox_list);
}

/* "CALL MSG " / "CALL MSG TO:" in the keyboard; kind is "MSG " or "MSG TO:".
 * `call` may be a relay path ("K2XYZ>N0XYZ"). */
static void msg_compose(const char *call, const char *kind) {
    char prefill[JS8_PATH_LEN + 12];
    snprintf(prefill, sizeof(prefill), "%s %s", call, kind);
    if (!popup_to_keyboard(NULL, 0, prefill)) return;
    if (strcmp(kind, "MSG TO:") == 0)
        msg_update_text_fmt("Left at %s for someone: type their call, a space, the message", call);
    else msg_update_text_fmt("Message for %s's inbox: type it and press Enter", call);
}

static void inbox_show(int id);

/* ESC in a message: back to the list; in the list: close. */
static void inbox_esc(void) {
    if (inbox_view_id) {
        inbox_leave();
        inbox_show(0);
    } else {
        inbox_close();
    }
}

static void inbox_key_cb(lv_event_t *e) {
    popup_key(e, inbox_esc);
}

typedef enum {
    INBOX_NEW        = -1,
    INBOX_CLOSE      = -2,
    INBOX_BACK       = -3,
    INBOX_REPLY      = -4, /* desktop's "Reply to <path>": PATH MSG ... */
    INBOX_DELETE     = -5,
    INBOX_REPLY_VIA  = -6, /* "Reply to <sender> via <path>": PATH MSG TO:SENDER ... */
    INBOX_REPLY_ORIG = -7, /* "Reply to <sender>": SENDER MSG ... */
    INBOX_FETCH_NEXT = -8, /* PATH QUERY MSG n, for its NEXT MSG ID n */
    INBOX_REPLY_APRS = -9, /* from an APRS gateway: an APRS message to its DE sender */
} inbox_action_t;

/* The sender of a message an APRS gateway passed on: "... DE N0CALL". */
static bool aprs_sender(const char *text, char *call, size_t size) {
    const char *de = NULL;
    for (const char *p = strstr(text, " DE "); p; p = strstr(p + 1, " DE ")) de = p;
    if (!de) return false;
    de += 4;
    while (*de == ' ') de++;
    size_t n = strcspn(de, " ");
    if (!n || de[n + strspn(de + n, " ")] != '\0' || n >= size) return false;
    snprintf(call, size, "%.*s", (int)n, de);
    return true;
}

/* A text from a phone, passed on by an SMS gateway (sender SMS, SMSGTE):
 * "@6045551234 TEXT DE SMS" starts with the number (or the alias) it came
 * from, which the gateway needs at the front of the reply. */
static bool aprs_sms_from(const char *text, const char *sender, char *out, size_t size) {
    if (strncmp(sender, "SMS", 3) != 0) return false;
    while (*text == ' ') text++;
    size_t n = strcspn(text, " ");
    if (text[0] != '@' || n < 2 || n >= size) return false;
    for (size_t i = 1; i < n; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    snprintf(out, size, "%.*s", (int)n, text);
    return true;
}
#define INBOX_HELD 100000 /* list/view ids from here on are held messages (id - INBOX_HELD) */

static void inbox_item_cb(lv_event_t *e) {
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    int viewed = inbox_view_id;
    switch (action) {
    case INBOX_CLOSE:
        inbox_close();
        return;
    case INBOX_BACK:
        inbox_leave();
        inbox_show(0);
        return;
    case INBOX_DELETE:
        if (viewed >= INBOX_HELD) js8_held_delete(held, viewed - INBOX_HELD);
        else js8_inbox_delete(inbox, viewed);
        msg_update_text_fmt("Message deleted");
        inbox_leave();
        inbox_show(0);
        inbox_refresh_button();
        return;
    case INBOX_REPLY:
    case INBOX_REPLY_VIA:
    case INBOX_REPLY_ORIG:
    case INBOX_FETCH_NEXT:
    case INBOX_REPLY_APRS: {
        js8_inbox_msg_t m;
        if (!js8_inbox_get(inbox, viewed, &m)) return;
        const char *path = m.path[0] ? m.path : m.from;
        char        orig[JS8_RX_CALL_LEN] = "";
        int         next                  = 0;
        js8_delivered_signature(m.text, orig, sizeof(orig), &next);
        char aprs_to[JS8_RX_CALL_LEN];
        if (action == INBOX_REPLY_APRS && !aprs_sender(m.text, aprs_to, sizeof(aprs_to))) return;
        if (action == INBOX_FETCH_NEXT) { /* sent at once: back to the messages */
            char text[JS8_PATH_LEN + 24];
            snprintf(text, sizeof(text), "%s QUERY MSG %d", path, next);
            inbox_close();
            tx_queue(text);
            return;
        }
        inbox_leave(); /* into the keyboard */
        if (action == INBOX_REPLY) {
            msg_compose(path, "MSG ");
        } else if (action == INBOX_REPLY_ORIG) {
            msg_compose(orig, "MSG ");
        } else if (action == INBOX_REPLY_VIA) {
            char prefill[JS8_PATH_LEN + JS8_RX_CALL_LEN + 12];
            snprintf(prefill, sizeof(prefill), "%s MSG TO:%s ", path, orig);
            if (popup_to_keyboard(NULL, 0, prefill))
                msg_update_text_fmt("Left at %s for %s: type the message and press Enter", path, orig);
        } else {
            /* An SMS: the number filled in, the cursor after it. */
            char head[64], sms[24];
            bool to_sms = aprs_sms_from(m.text, aprs_to, sms, sizeof(sms));
            snprintf(head, sizeof(head), "%s%-9.9s:%s%s", APRS_CMD, aprs_to, to_sms ? sms : "", to_sms ? " " : "");
            aprs_compose(head, "");
            lv_group_set_editing(keyboard_group, true);
            if (to_sms) msg_update_text_fmt("SMS to %s: type your message and press Enter", sms + 1);
            else msg_update_text_fmt("APRS message to %s: type it and press Enter", aprs_to);
        }
        return;
    }
    case INBOX_NEW: {
        char  call[JS8_RX_CALL_LEN];
        float freq;
        int   snr;
        inbox_leave();
        if (selected_station(call, sizeof(call), &freq, &snr)) {
            apply_hold(freq);
            msg_compose(call, "MSG ");
        } else if (popup_to_keyboard(NULL, 0, NULL)) {
            msg_update_text_fmt("Type their call, then MSG and the message");
        }
        return;
    }
    default: /* a message */
        inbox_leave();
        inbox_show(action);
        return;
    }
}

static lv_obj_t *inbox_add(const char *label, int action) {
    lv_obj_t *b = list_add_item(inbox_list, label);
    lv_obj_add_event_cb(b, inbox_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    lv_obj_add_event_cb(b, inbox_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    /* Long lines end in "..." rather than scrolling, which redraws the list. */
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_DOT);
    return b;
}

static void utc_label(int64_t ms, char *buf, size_t size, const char *fmt) {
    time_t    t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, size, fmt, &tm);
}

/* The list (id 0) or one message. */
static void inbox_show(int id) {
    inbox_view_id = id;
    lv_group_remove_obj(table);
    inbox_list = lv_list_create(dialog.obj);
    lv_obj_set_size(inbox_list, 600, WF_HEIGHT - 10);
    lv_obj_align(inbox_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(inbox_list, &sony_24, 0);
    lv_obj_set_style_bg_color(inbox_list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(inbox_list, lv_color_white(), 0);

    char      line[JS8_RX_TEXT_LEN + 48];
    lv_obj_t *first = NULL;
    if (id >= INBOX_HELD) {
        /* A message held here for another station. */
        js8_held_msg_t m;
        if (!js8_held_get(held, id - INBOX_HELD, &m)) {
            inbox_view_id = 0;
            lv_obj_del(inbox_list);
            inbox_show(0);
            return;
        }
        char when[32], from[JS8_PATH_LEN + 16];
        utc_label(m.utc_ms, when, sizeof(when), "%d %b %H:%M UTC");
        js8_path_display(m.path, from, sizeof(from));
        snprintf(line, sizeof(line), "For %s from %s  %s", m.to, from, when);
        lv_obj_t *t = lv_list_add_text(inbox_list, line);
        lv_obj_set_style_text_font(t, &sony_22, 0);
        lv_obj_t *body = lv_list_add_text(inbox_list, m.text);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_bg_color(body, lv_color_hex(0x202020), 0);
        lv_obj_set_style_text_color(body, lv_color_white(), 0);
        lv_obj_set_style_pad_ver(body, 8, 0);
        if (m.group)
            snprintf(line, sizeof(line), "For any %s member to fetch for 2 days; fetched by %d", m.to, m.got);
        else snprintf(line, sizeof(line), m.delivered ? "Delivered to %s" : "Waiting for %s to ask (QUERY MSGS)", m.to);
        t = lv_list_add_text(inbox_list, line);
        lv_obj_set_style_text_font(t, &sony_22, 0);
        first = inbox_add("Delete", INBOX_DELETE);
        lv_obj_t *back = inbox_add("Back", INBOX_BACK);
        lv_obj_set_style_text_color(back, lv_color_hex(0xffc040), 0);
    } else if (id) {
        js8_inbox_msg_t m;
        if (!js8_inbox_get(inbox, id, &m)) {
            inbox_view_id = 0;
            lv_obj_del(inbox_list);
            inbox_show(0);
            return;
        }
        js8_inbox_mark_read(inbox, id);
        inbox_refresh_button();
        update_status();
        char when[32], from[JS8_PATH_LEN + 16];
        utc_label(m.utc_ms, when, sizeof(when), "%d %b %H:%M UTC");
        js8_path_display(m.path[0] ? m.path : m.from, from, sizeof(from));
        if (m.to[0] == '@') snprintf(line, sizeof(line), "From %s to %s  %s", from, m.to, when);
        else snprintf(line, sizeof(line), "From %s  %s", from, when);
        lv_obj_t *t = lv_list_add_text(inbox_list, line);
        lv_obj_set_style_text_font(t, &sony_22, 0);
        lv_obj_t *body = lv_list_add_text(inbox_list, m.text);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_bg_color(body, lv_color_hex(0x202020), 0);
        lv_obj_set_style_text_color(body, lv_color_white(), 0);
        lv_obj_set_style_pad_ver(body, 8, 0);
        /* Desktop's Reply choices: to the path it came by; and for a
         * delivered message ("... FROM N0XYZ"), to its sender through the
         * station that held it (MSG TO:) or straight. */
        const char *path = m.path[0] ? m.path : m.from;
        char        orig[JS8_RX_CALL_LEN] = "", aprs_from[JS8_RX_CALL_LEN];
        int         next                  = 0;
        bool        signed_ = js8_delivered_signature(m.text, orig, sizeof(orig), &next) && orig[0] != '@';
        if (strcmp(path, "APRS") == 0) {
            if (aprs_sender(m.text, aprs_from, sizeof(aprs_from))) {
                char sms[24];
                if (aprs_sms_from(m.text, aprs_from, sms, sizeof(sms)))
                    snprintf(line, sizeof(line), "Reply by SMS to %s", sms);
                else snprintf(line, sizeof(line), "Reply by APRS to %s", aprs_from);
                first = inbox_add(line, INBOX_REPLY_APRS);
            }
        } else {
            snprintf(line, sizeof(line), "Reply: MSG to %s", from);
            first = inbox_add(line, INBOX_REPLY);
            if (signed_ && strcmp(orig, path) != 0) {
                snprintf(line, sizeof(line), "Reply to %s via %s (MSG TO:)", orig, from);
                inbox_add(line, INBOX_REPLY_VIA);
                snprintf(line, sizeof(line), "Reply to %s", orig);
                inbox_add(line, INBOX_REPLY_ORIG);
            }
            if (next > 0) {
                snprintf(line, sizeof(line), "Fetch the next: QUERY MSG %d", next);
                inbox_add(line, INBOX_FETCH_NEXT);
            }
        }
        lv_obj_t *del = inbox_add("Delete", INBOX_DELETE);
        if (!first) first = del;
        lv_obj_t *back = inbox_add("Back", INBOX_BACK);
        lv_obj_set_style_text_color(back, lv_color_hex(0xffc040), 0);
    } else {
        static js8_inbox_msg_t rows[INBOX_ROWS];
        int                    n = js8_inbox_list(inbox, rows, INBOX_ROWS);
        snprintf(line, sizeof(line), "Inbox: %d message%s, %d new", js8_inbox_count(inbox),
                 js8_inbox_count(inbox) == 1 ? "" : "s", js8_inbox_unread(inbox));
        lv_obj_t *t = lv_list_add_text(inbox_list, line);
        lv_obj_set_style_text_font(t, &sony_22, 0);

        char  call[JS8_RX_CALL_LEN];
        float freq;
        int   snr;
        if (selected_station(call, sizeof(call), &freq, &snr)) snprintf(line, sizeof(line), "New message to %s...", call);
        else snprintf(line, sizeof(line), "New message...");
        first = inbox_add(line, INBOX_NEW);
        lv_obj_set_style_text_color(first, lv_color_hex(0x80ff80), 0);

        lv_obj_t *newest_new = NULL;
        for (int i = 0; i < n; i++) {
            char when[16];
            utc_label(rows[i].utc_ms, when, sizeof(when), "%d %b %H:%M");
            char from[JS8_PATH_LEN + 16];
            js8_path_display(rows[i].path[0] ? rows[i].path : rows[i].from, from, sizeof(from));
            snprintf(line, sizeof(line), "%s%s  %s  %s", rows[i].read ? "" : "* ", when, from, rows[i].text);
            lv_obj_t *b = inbox_add(line, rows[i].id);
            if (!rows[i].read) {
                lv_obj_set_style_text_color(b, lv_color_hex(0xffe080), 0);
                if (!newest_new) newest_new = b; /* newest first */
            }
        }
        if (n == 0) {
            t = lv_list_add_text(inbox_list, "No messages yet");
            lv_obj_set_style_text_font(t, &sony_22, 0);
        }
        /* Messages held here for others (MSG TO:), until they ask. */
        static js8_held_msg_t held_rows[HELD_ROWS];
        int                   nh = js8_held_list(held, held_rows, HELD_ROWS);
        if (nh > 0) {
            snprintf(line, sizeof(line), "Held for others: %d waiting", js8_held_waiting(held));
            t = lv_list_add_text(inbox_list, line);
            lv_obj_set_style_text_font(t, &sony_22, 0);
        }
        for (int i = 0; i < nh; i++) {
            char from[JS8_PATH_LEN + 16], got[16] = "";
            js8_path_display(held_rows[i].path, from, sizeof(from));
            if (held_rows[i].group) snprintf(got, sizeof(got), " (%d got it)", held_rows[i].got);
            snprintf(line, sizeof(line), "%sfor %s%s from %s  %s", held_rows[i].delivered ? "(sent) " : "",
                     held_rows[i].to, got, from, held_rows[i].text);
            lv_obj_t *b = inbox_add(line, INBOX_HELD + held_rows[i].id);
            if (held_rows[i].delivered) lv_obj_set_style_text_color(b, lv_color_hex(0x909090), 0);
        }
        lv_obj_t *close = inbox_add("Close", INBOX_CLOSE);
        lv_obj_set_style_text_color(close, lv_color_hex(0xffc040), 0);
        if (newest_new) first = newest_new; /* straight to the newest unread (your choice) */
    }
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(first);
}

static void inbox_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (inbox_list) { /* Inbox again closes it */
        inbox_close();
        return;
    }
    if (popup_guard()) return;
    if (composing || !inbox) return;
    inbox_show(0);
}

/* ---- Alerts ---------------------------------------------------------------- */

/* Like desktop JS8Call's notifications and highlight words: a beep through
 * the speaker for what's switched on in the Alerts list, and rows matching
 * an alert word (calls or words you type) in purple. Never while
 * transmitting: the speaker path is the TX audio path then.
 *
 * The base unit plays this app's audio on the speaker only in its play
 * mode (radio_speaker_play, as voice prompts and the recorder use it);
 * without it the beep went nowhere. In play mode the audio the base sends
 * us isn't the receiver's, so the beep is kept short and beep_guard feeds
 * the decoder silence meanwhile. */

#define BEEP_HZ       1000
#define BEEP_MS       120
#define BEEP_GAP_MS   100
#define BEEP_LEAD_MS  50   /* silence first: the base switches to play mode */
#define BEEP_TAIL_MS  300  /* guard after speaker-off: capture latency */
#define BEEP_AMPL     13000 /* about -8 dBFS */
#define BEEP_EVERY_MS 3000 /* at most one alert sound per 3 s */

static int64_t beep_last_ms;

enum {
    BEEP_TONE = AUDIO_PLAY_RATE * BEEP_MS / 1000,
    BEEP_GAP  = AUDIO_PLAY_RATE * BEEP_GAP_MS / 1000,
    BEEP_LEAD = AUDIO_PLAY_RATE * BEEP_LEAD_MS / 1000,
};
static int16_t         beep_buf[BEEP_TONE + BEEP_GAP];
static int16_t         beep_lead[BEEP_LEAD];
static int             beep_count;
static atomic_bool     beeping;

/* Beep thread: audio_play() waits for room in the stream, so it can't run
 * on the LVGL thread. */
/* audio_play() in parts of at most 2048 samples, as tx_player does: R1CBU
 * 0.34's hung on a bigger write (1.0's waits properly), and small parts let
 * the beep stop as soon as TX keys. */
static void beep_play(const int16_t *buf, size_t n) {
    for (size_t at = 0; at < n && !atomic_load(&keyed);) {
        size_t part = LV_MIN(2048, n - at);
        audio_play((int16_t *)buf + at, part);
        at += part;
    }
}

static void *beep_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "js8-beep");
    pthread_mutex_lock(&speaker_lock);
    if (atomic_load(&keyed)) goto out;
    /* The receiver's level just before, for the log. */
    atomic_store(&beep_guard_sq, 0);
    atomic_store(&beep_guard_n, 0);
    atomic_store(&beep_guard, true);
    radio_speaker_play(true);
    beep_play(beep_lead, BEEP_LEAD);
    for (int i = 0; i < beep_count; i++) beep_play(beep_buf, BEEP_TONE + BEEP_GAP);
    audio_play_wait();
    radio_speaker_play(false);
    atomic_store(&beep_guard_end, now_mono_ms() + BEEP_TAIL_MS);
    atomic_store(&beep_guard, false);
out:
    pthread_mutex_unlock(&speaker_lock);
    atomic_store(&beeping, false);
    return NULL;
}

/* Once the guard is over: what the capture carried while the speaker
 * played, against the receiver just before (tells whether play mode
 * really cuts the receiver on this base). */
static void beep_log_level(void) {
    int64_t n = atomic_load(&beep_guard_n);
    if (!n || beep_guard_active()) return;
    double rms = sqrt((double)atomic_load(&beep_guard_sq) / 1e9 / (double)n);
    LV_LOG_USER("JS8 beep: capture %.1f dBFS during the beep (%lld samples), %.1f dBFS before",
                20.0 * log10(rms + 1e-12), (long long)n, beep_level_before_db);
    atomic_store(&beep_guard_n, 0);
}

static void alert_beep(int count) {
    if (!(param_i_get(cfg.js8.alerts()) & JS8_ALERT_BEEP)) return;
    if (now_wall_ms() - beep_last_ms < BEEP_EVERY_MS) return;
    beep_start(count);
}

static int beeps_started; /* for tools/js8_ui_harness */

/* `count` beeps now, unless we're sending or a beep is playing (false). */
static bool beep_start(int count) {
    if (atomic_load(&keyed) || js8_tx_busy(tx)) return false;
    if (atomic_exchange(&beeping, true)) return false;
    beep_last_ms = now_wall_ms();

    static bool made;
    if (!made) {
        int ramp = AUDIO_PLAY_RATE / 200; /* 5 ms fades: no clicks */
        for (int i = 0; i < BEEP_TONE; i++) {
            float env = 1.0f;
            if (i < ramp) env = (float)i / ramp;
            if (i > BEEP_TONE - ramp) env = (float)(BEEP_TONE - i) / ramp;
            beep_buf[i] = (int16_t)(BEEP_AMPL * env * sinf(2.0f * (float)M_PI * BEEP_HZ * i / AUDIO_PLAY_RATE));
        }
        made = true;
    }
    beep_count = count;
    beeps_started++;
    pthread_t th;
    if (pthread_create(&th, NULL, beep_thread, NULL) == 0) pthread_detach(th);
    else atomic_store(&beeping, false);
    return true;
}

/* ---- High-SWR guard ---------------------------------------------------- */

/* For unattended stations (VE7NHW, beta 5): the SWR above 3:1 for half a
 * second while we transmit turns every automatic sender off (AUTO, auto
 * HB, HB ACK, auto CQ). The message on the air carries on. Three beeps
 * once the transmitter is done (the speaker is the TX audio path while
 * keyed), whatever the Alerts list says. The radio reports the SWR a few
 * times a second while keyed (tx_info, unfiltered in USB-D, capped at 5). */
#define SWR_TRIP    3.0f
#define SWR_TRIP_MS 500

static uint8_t swr_msg_id;
static int64_t swr_high_since; /* 0: the last reading was at or under the limit */
static float   swr_high_max;
static bool    swr_beep_due;

static void swr_guard_trip(float swr) {
    bool any = param_i_get(cfg.js8.auto_mode()) || param_i_get(cfg.js8.hb()) || param_i_get(cfg.js8.hb_ack()) || auto_cq;
    if (!any) return;
    js8_log("high SWR: %.1f:1 for %d ms while sending, AUTO / HB / HB ACK / auto CQ off", swr, SWR_TRIP_MS);
    param_i_set(cfg.js8.auto_mode(), false);
    param_i_set(cfg.js8.hb_ack(), false);
    hb_auto_stop("high SWR");
    auto_cq_stop("high SWR");
    if (btn_auto.disp_btn) buttons_refresh(&btn_auto);
    if (btn_hbackk.disp_btn) buttons_refresh(&btn_hbackk);
    add_info_row("High SWR %.1f:1: AUTO, HB, HB ACK and auto CQ off", swr);
    msg_update_text_fmt("High SWR %.1f:1: AUTO, HB, HB ACK and auto CQ off. Check the antenna", swr);
    update_status();
    swr_beep_due = true;
}

static void swr_guard_reset(void) {
    swr_high_since = 0;
    swr_beep_due   = false;
}

/* Every 250 ms (tx_timer). Three readings over the limit in a row, the
 * first and last 500 ms apart, trip it: a spike as the radio keys doesn't. */
static void swr_guard_tick(void) {
    if (!atomic_load(&keyed)) {
        swr_high_since = 0;
        tx_info_refresh(&swr_msg_id, NULL, NULL, NULL); /* the next TX starts fresh */
        if (swr_beep_due && beep_start(3)) swr_beep_due = false;
        return;
    }
    float swr;
    if (!tx_info_refresh(&swr_msg_id, NULL, NULL, &swr)) return;
    if (swr <= SWR_TRIP) {
        swr_high_since = 0;
        return;
    }
    int64_t now = now_mono_ms();
    if (!swr_high_since) {
        swr_high_since = now;
        swr_high_max   = swr;
        return;
    }
    swr_high_max = LV_MAX(swr_high_max, swr);
    if (now - swr_high_since < SWR_TRIP_MS) return;
    swr_high_since = 0;
    swr_guard_trip(swr_high_max);
}

/* For tools/js8_ui_harness: the automatic switches, and how many beep
 * sounds started (the last one's beep count). */
void dialog_js8_autos(bool *auto_mode, bool *hb, bool *hb_ack, bool *cq) {
    *auto_mode = param_i_get(cfg.js8.auto_mode());
    *hb        = param_i_get(cfg.js8.hb());
    *hb_ack    = param_i_get(cfg.js8.hb_ack());
    *cq        = auto_cq;
}

int dialog_js8_beeps(int *last_count) {
    *last_count = beep_count;
    return beeps_started;
}

/* Every decode but our own: alert words, then what beeps. */
static void alert_check(js8_rx_msg_t *m, bool new_station) {
    if (m->low_confidence) return;
    char hit[16];
    if (js8_alert_hit(m->text, m->from, alert_words, hit, sizeof(hit))) {
        m->alert = true;
        msg_update_text_fmt("Alert %s: %s", hit, m->text);
        alert_beep(2);
        return;
    }
    uint8_t a      = param_i_get(cfg.js8.alerts());
    bool    hb_ack = strstr(m->text, " HEARTBEAT SNR") != NULL;
    if ((a & JS8_ALERT_TO_ME) && m->to_me && !hb_ack) alert_beep(1);
    else if ((a & JS8_ALERT_CQ) && m->cq) alert_beep(1);
    else if ((a & JS8_ALERT_NEW) && new_station && m->from[0] && !worked_before(m->from)) {
        msg_update_text_fmt("New station: %s", m->from);
        alert_beep(1);
    }
}

typedef enum {
    AL_BEEP,
    AL_TO_ME,
    AL_INBOX,
    AL_CQ,
    AL_NEW,
    AL_WORDS,
    AL_TEST,
    AL_CLOSE,
} alerts_item_t;

static const struct {
    uint8_t     bit;
    const char *label;
} alert_switches[] = {
    [AL_BEEP]  = {JS8_ALERT_BEEP, "Beep"},
    [AL_TO_ME] = {JS8_ALERT_TO_ME, "Message to me"},
    [AL_INBOX] = {JS8_ALERT_INBOX, "Inbox message"},
    [AL_CQ]    = {JS8_ALERT_CQ, "Someone calls CQ"},
    [AL_NEW]   = {JS8_ALERT_NEW, "New station (not in log)"},
};

static void alerts_switch_label(alerts_item_t item, char *buf, size_t size) {
    snprintf(buf, size, "%s: %s", alert_switches[item].label,
             (param_i_get(cfg.js8.alerts()) & alert_switches[item].bit) ? "On" : "Off");
}

static void alerts_close(void) {
    if (!alerts_list) return;
    lv_obj_del_async(alerts_list); /* often called from one of its buttons */
    alerts_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

static void alerts_item_cb(lv_event_t *e) {
    alerts_item_t item = (alerts_item_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t     *btn  = lv_event_get_target(e);
    char          line[sizeof(alert_words) + 24];
    switch (item) {
    case AL_CLOSE:
        alerts_close();
        return;
    case AL_TEST: {
        int64_t last = beep_last_ms;
        beep_last_ms = 0;
        if (!(param_i_get(cfg.js8.alerts()) & JS8_ALERT_BEEP)) msg_update_text_fmt("Beep is off");
        else if (atomic_load(&keyed) || js8_tx_busy(tx)) msg_update_text_fmt("Not while transmitting");
        alert_beep(2);
        if (!beep_last_ms) beep_last_ms = last;
        return;
    }
    case AL_WORDS:
        /* Into the keyboard, then back here (see texts_item_cb). */
        if (popup_to_keyboard(&alerts_list, EDIT_ALERT_WORDS, alert_words[0] ? alert_words : NULL))
            msg_update_text_fmt("Calls or words to watch for, separated by spaces");
        return;
    default: /* a switch: flip it, relabel in place */
        param_i_set(cfg.js8.alerts(), param_i_get(cfg.js8.alerts()) ^ alert_switches[item].bit);
        alerts_switch_label(item, line, sizeof(line));
        lv_label_set_text(lv_obj_get_child(btn, 0), line);
        return;
    }
}

static void alerts_key_cb(lv_event_t *e) {
    popup_key(e, alerts_close);
}

static lv_obj_t *alerts_add(alerts_item_t item, const char *label) {
    lv_obj_t *b = list_add_item(alerts_list, label);
    lv_obj_add_event_cb(b, alerts_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item);
    lv_obj_add_event_cb(b, alerts_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_DOT);
    return b;
}

static void alerts_show(void) {
    lv_group_remove_obj(table);
    alerts_list = lv_list_create(dialog.obj);
    lv_obj_set_size(alerts_list, 560, WF_HEIGHT - 10);
    lv_obj_align(alerts_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(alerts_list, &sony_24, 0);
    lv_obj_set_style_bg_color(alerts_list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(alerts_list, lv_color_white(), 0);
    lv_obj_t *t = lv_list_add_text(alerts_list, "Alerts: beep, and alert words in purple");
    lv_obj_set_style_text_font(t, &sony_22, 0);

    char      line[sizeof(alert_words) + 24];
    lv_obj_t *first = NULL;
    for (int i = AL_BEEP; i <= AL_NEW; i++) {
        alerts_switch_label((alerts_item_t)i, line, sizeof(line));
        lv_obj_t *b = alerts_add((alerts_item_t)i, line);
        if (!first) first = b;
    }
    snprintf(line, sizeof(line), "Alert words: %s", alert_words[0] ? alert_words : "(none)");
    lv_obj_t *words = alerts_add(AL_WORDS, line);
    lv_obj_set_style_text_color(words, lv_color_hex(0xd0a0ff), 0);
    alerts_add(AL_TEST, "Test beep");
    lv_obj_t *close = alerts_add(AL_CLOSE, "Close");
    lv_obj_set_style_text_color(close, lv_color_hex(0xffc040), 0);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(first);
}

static void alerts_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (alerts_list) { /* Alerts > again closes it */
        alerts_close();
        return;
    }
    if (popup_guard()) return;
    if (composing) return;
    alerts_show();
}

/* ---- Speeds (T6) ----------------------------------------------------------- */

/* As desktop JS8Call: everything we send goes at one speed (Normal, Fast,
 * Turbo, Slow, and Ultra with its Setting on), and the receiver decodes
 * every speed at once unless Decode is set to My speed. Turbo and Ultra send
 * no heartbeats or HB acks. */

/* The selected station's speed (the one Reply and the green bar are for,
 * not the row under the cursor: with a station locked they differ, bug
 * hunt 15); else the cursor row's (a message without a call). */
static bool selected_speed(js8_speed_t *out) {
    js8_station_t st;
    if (sel_call[0] && find_station(sel_call, &st)) {
        *out = js8_speed_from_submode(st.submode);
        return true;
    }
    uint16_t row, col;
    lv_table_get_selected_cell(table, &row, &col);
    if (row >= rows || row_hist[row] < 0) return false;
    uint8_t submode = view_stations ? st_rows[row_hist[row]].submode : history[row_hist[row]].submode;
    if (!view_stations && history[row_hist[row]].tx) return false;
    *out = js8_speed_from_submode(submode);
    return true;
}

/* Replying to someone heard on another speed: say so (desktop offers "Jump
 * to Fast speed"; here, hold Speed). */
static void speed_warn(void) {
    js8_speed_t their;
    char        call[JS8_RX_CALL_LEN];
    float       freq;
    int         snr;
    if (!selected_speed(&their) || their == cur_speed() || !selected_station(call, sizeof(call), &freq, &snr)) return;
    msg_update_text_fmt("%s was heard on %s, you send %s - hold Speed (page 6) to match", call, js8_speed_name(their),
                        js8_speed_name(cur_speed()));
}

static void set_speed(js8_speed_t s) {
    if (js8_tx_busy(tx)) {
        msg_update_text_fmt("Not while sending - Stop TX first");
        return;
    }
    param_i_set(cfg.js8.speed(), (uint8_t)s);
    /* The offset must leave room for the wider signal below 3000 Hz. */
    int max = js8_speed_max_offset_hz(s);
    if (param_i_get(cfg.js8.tx_freq()) > max) param_i_set(cfg.js8.tx_freq(), (uint16_t)max);
    js8_rx_set_qso_offset(rx, param_i_get(cfg.js8.tx_freq()));
    lv_finder_set_width(finder, js8_speed_bandwidth_hz(s));
    finder_shown = -1; /* another width: all of it again */
    finder_sync();
    lv_obj_invalidate(finder);
    if (!param_i_get(cfg.js8.rx_all())) js8_rx_set_submodes(rx, rx_decode_mask());
    /* A heartbeat that fell due while in Turbo or Ultra mustn't go out the moment we
     * leave it: start the interval again. */
    hb_next_ms = 0;
    if (btn_speed.disp_btn) buttons_refresh(&btn_speed);
    update_tx_bar();
    msg_update_text_fmt("Sending %s: %d s slots, %d Hz wide%s", js8_speed_name(s), js8_speed_period_s(s),
                        js8_speed_bandwidth_hz(s), js8_speed_heartbeats(s) ? "" : ", no heartbeats (as desktop)");
    add_info_row("Sending at %s speed", js8_speed_name(s));
}

static const char *speed_label_getter(void) {
    static char label[24];
    snprintf(label, sizeof(label), "Speed:\n%s", js8_speed_name(cur_speed()));
    return label;
}

static void speed_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (popup_guard()) return;
    int at = 0;
    for (int i = 0; i < JS8_SPEED_COUNT; i++)
        if (speed_order[i] == cur_speed()) at = i;
    js8_speed_t next;
    do next = speed_order[at = (at + 1) % JS8_SPEED_COUNT];
    while (!speed_on_button(next));
    set_speed(next);
}

/* Hold: the selected station's speed. */
static void speed_hold_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (popup_guard()) return;
    js8_speed_t their;
    if (!selected_speed(&their)) {
        msg_update_text_fmt("Select a station first (MFK)");
        return;
    }
    if (their == cur_speed()) {
        msg_update_text_fmt("Already sending %s", js8_speed_name(their));
        return;
    }
    set_speed(their); /* Ultra too, even if it isn't on the button */
}

/* Decode: all speeds or only the one we send at (a Settings line; it was
 * page 6's button before Hold took its place). */
static void decode_toggle(void) {
    param_i_set(cfg.js8.rx_all(), !param_i_get(cfg.js8.rx_all()));
    js8_rx_set_submodes(rx, rx_decode_mask());
    if (param_i_get(cfg.js8.rx_all())) msg_update_text_fmt("Decoding every speed (Normal, Fast, Turbo, Ultra, Slow)");
    else msg_update_text_fmt("Decoding %s only", js8_speed_name(cur_speed()));
}

/* Ultra on the Speed button or not (Settings, experimental). It's decoded
 * either way; switched off while you send Ultra, you stay on it until the
 * next press of Speed. */
static void ultra_toggle(void) {
    bool on = !param_i_get(cfg.js8.ultra());
    param_i_set(cfg.js8.ultra(), on);
    msg_update_text_fmt(on ? "Ultra on the Speed button (page 6): after Turbo"
                           : cur_speed() == JS8_SPEED_ULTRA
                               ? "Ultra off the Speed button: you stay on Ultra until you press Speed"
                               : "Ultra off the Speed button (still decoded; hold Speed on an Ultra station to match)");
}

/* ---- Frequency: JS8Call's, GhostNet's, or your own ---------------------- */

/* The band keys step through JS8Call's usual dial frequencies, or with
 * GhostNet on through GhostNet's (3.575, 7.107, 14.107 MHz). A custom
 * frequency is remembered; the band keys leave it for the preset list. */

typedef enum {
    FQ_JS8,
    FQ_GHOSTNET,
    FQ_CUSTOM,
    FQ_CLOSE,
} freq_item_t;

/* Settings' Frequencies line: which list the band keys step through. */
static const char *freq_name(void) {
    static char buf[32];
    if (param_i_get(cfg.js8.custom_on())) {
        format_khz(param_i_get(cfg.js8.custom_hz()), buf, sizeof(buf));
        return buf;
    }
    return param_i_get(cfg.js8.ghostnet()) ? "GhostNet" : "JS8Call's";
}

static void freq_close(void) {
    if (!freq_list) return;
    lv_obj_del_async(freq_list); /* often called from one of its buttons */
    freq_list = NULL;
    if (table && !composing) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_focus_obj(table);
        lv_group_set_editing(keyboard_group, true);
    }
}

/* To the closest preset of the chosen list. */
static void use_presets(bool ghostnet) {
    if (js8_tx_busy(tx)) {
        msg_update_text_fmt("Not while sending - Stop TX first");
        return;
    }
    param_i_set(cfg.js8.ghostnet(), ghostnet);
    param_i_set(cfg.js8.custom_on(), false);
    load_band(0);
    retuned();
}

/* From the keyboard: kHz ("7107.5"), or MHz when under 100 ("7.1075").
 * False, with the reason shown, keeps the keyboard open to fix it. */
static bool parse_custom(const char *text, int32_t *out) {
    char  *end;
    double v = strtod(text, &end);
    if (end == text || v <= 0) {
        msg_update_text_fmt("Type the dial frequency in kHz, e.g. 7107");
        return false;
    }
    int32_t hz = (int32_t)(v < 100 ? v * 1e6 + 0.5 : v * 1e3 + 0.5);
    if (hz < CUSTOM_MIN_HZ || hz > CUSTOM_MAX_HZ) {
        msg_update_text_fmt("Out of range: 1800 - 54000 kHz");
        return false;
    }
    if (js8_tx_busy(tx)) {
        msg_update_text_fmt("Not while sending - Stop TX first");
        return false;
    }
    if (out) *out = hz;
    return true;
}

static void tune_custom(const char *text) {
    int32_t hz;
    if (!parse_custom(text, &hz)) return;
    param_i_set(cfg.js8.custom_hz(), hz);
    param_i_set(cfg.js8.custom_on(), true);
    cparam_i_set(cfg.cur.fg_freq(), hz);
    js8_usb_dig();
    retuned();
    msg_update_text_fmt("%s", where_label());
}

static void freq_item_cb(lv_event_t *e) {
    freq_item_t item = (freq_item_t)(intptr_t)lv_event_get_user_data(e);
    switch (item) {
    case FQ_JS8:
    case FQ_GHOSTNET:
        freq_close();
        use_presets(item == FQ_GHOSTNET);
        return;
    case FQ_CUSTOM: {
        /* Into the keyboard with the last one filled in. */
        char khz[16] = "";
        if (param_i_get(cfg.js8.custom_hz())) format_khz(param_i_get(cfg.js8.custom_hz()), khz, sizeof(khz));
        if (popup_to_keyboard(&freq_list, EDIT_FREQ, khz)) msg_update_text_fmt("Dial frequency in kHz, then Enter");
        return;
    }
    case FQ_CLOSE:
        freq_close();
        return;
    }
}

static void freq_key_cb(lv_event_t *e) {
    popup_key(e, freq_close);
}

static lv_obj_t *freq_add(freq_item_t item, const char *label) {
    lv_obj_t *b = list_add_item(freq_list, label);
    lv_obj_add_event_cb(b, freq_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item);
    lv_obj_add_event_cb(b, freq_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, b);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_DOT);
    return b;
}

static void freq_show(void) {
    lv_group_remove_obj(table);
    freq_list = lv_list_create(dialog.obj);
    lv_obj_set_size(freq_list, 560, WF_HEIGHT - 10);
    lv_obj_align(freq_list, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(freq_list, &sony_24, 0);
    lv_obj_set_style_bg_color(freq_list, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(freq_list, lv_color_white(), 0);
    char line[64];
    if (param_i_get(cfg.js8.custom_on())) {
        char khz[16];
        format_khz(param_i_get(cfg.js8.custom_hz()), khz, sizeof(khz));
        snprintf(line, sizeof(line), "Now: JS8 Custom, %s kHz", khz);
    } else {
        snprintf(line, sizeof(line), "Now: %s", where_label());
    }
    lv_obj_t *t = lv_list_add_text(freq_list, line);
    lv_obj_set_style_text_font(t, &sony_22, 0);

    bool      custom = param_i_get(cfg.js8.custom_on()), ghost = param_i_get(cfg.js8.ghostnet());
    lv_obj_t *js8    = freq_add(FQ_JS8, "JS8Call frequencies (7.078, 14.078...)");
    lv_obj_t *gn     = freq_add(FQ_GHOSTNET, "GhostNet (3.575, 7.107, 14.107)");
    char      khz[16];
    if (param_i_get(cfg.js8.custom_hz())) {
        format_khz(param_i_get(cfg.js8.custom_hz()), khz, sizeof(khz));
        snprintf(line, sizeof(line), "Custom kHz... (last %s)", khz);
    } else {
        snprintf(line, sizeof(line), "Custom kHz...");
    }
    lv_obj_t *cu    = freq_add(FQ_CUSTOM, line);
    lv_obj_t *close = freq_add(FQ_CLOSE, "Close");
    lv_obj_set_style_text_color(close, lv_color_hex(0xffc040), 0);
    /* The one in use stands out and has the knob. */
    lv_obj_t *cur = custom ? cu : ghost ? gn : js8;
    lv_obj_set_style_text_color(cur, lv_color_hex(0x60ff60), 0);
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(cur);
}

/* For tools/js8_ui_harness: the Frequencies setting as Settings shows it. */
const char *dialog_js8_freq_name(void) {
    return freq_name();
}
