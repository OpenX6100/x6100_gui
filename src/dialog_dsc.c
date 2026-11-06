/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - DSC (Digital Selective Calling) receiver, MF/HF
 *
 * Listens to the DSC distress and safety channels and shows every call as a
 * panel: category colour, origin -> destination, what the call says.  The
 * list has a Big view (4 calls) and a Small view (2 calls, spectrum and
 * waterfall stay visible).  A call opens in a detail card with every field;
 * calls are saved in DSC_SAVE_DIR and can be browsed later.
 *
 * Audio: 12 kHz from the DSP path, tones around 1000 Hz (dial = channel
 * frequency - 1 kHz, USB).  The decoder lives in dsc/.
 */

#include "dialog_dsc.h"
#include "dsc/dsc_decoder.h"
#include "dsc/dsc_format.h"
#include "dsc/dsc_ships.h"
#include "dsc/dsc_qrcodegen.h"
#include "dsc/dsc_flags.h"
#include "dialog.h"
#include "styles.h"
#include "buttons.h"
#include "keyboard.h"
#include "radio.h"
#include "events.h"
#include "dsp.h"
#include "msg.h"
#include "ui_list.h"
#include "app_info.h"
#include "app_radio.h"  /* 1KO125: radio set up while open */
#include "spectrum.h"
#include "cfg/cfg_api.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DSC_SAMPLE_RATE     12000
#define DSC_SAVE_DIR        "/mnt/dsc"
#define DSC_FILTER_FILE     DSC_SAVE_DIR "/view_filter"
#define DSC_SAVE_PREFIX     "dsc-"
#define DSC_SAVE_EXT        ".txt"
#define DSC_NAMES_FILE      "/mnt/dsc/mmsi.txt"     /* optional "MMSI;name" lines */
#define DSC_TEST_WAV        "/mnt/dsc_test.wav"     /* mono 12000 Hz PCM int16 */
#define DSC_MAX_CALLS       200                     /* calls kept in the live list */
#define DSC_BROWSER_MAX     200                     /* newest saved calls shown */
#define DSC_KEEP_FILES      500                     /* saved calls kept on the SD card: */
                                                    /* older ones are deleted, except */
                                                    /* DISTRESS and URGENCY calls */
#define DSC_FIFO_LEN        16
#define DSC_UI_TIMER_MS     100
#define DSC_TEST_CHUNK      480
#define DSC_NAMES_MAX       200000
#define DSC_REPLY_WINDOW    (30 * 60)               /* s: acknowledgement linking */

#define BIG_W               795
#define BIG_H               350
#define PANEL_GAP           4

/* colours */
#define C_PANEL             0x1c2229
#define C_PANEL_SEL         0x222932
#define C_LINE              0x343d48
#define C_TEXT              0xe8edf2
#define C_SUB               0x96a3b0
#define C_SELECT            0x5aaaff
#define C_OK                0x3aaa6e
#define C_BAD               0xe04848
#define C_DISTRESS          0xcd3030
#define C_URGENCY           0xe88014
#define C_SAFETY            0xe2b028
#define C_ROUTINE           0x7cc87a        /* light green: not to be confused with unknown */
#define C_UNKNOWN           0x5a6672
#define C_SELFTEST          0xa98be0        /* lilac: a station testing its own DSC */

typedef struct {
    dsc_message_t msg;
    dsc_info_t    info;
    time_t        utc;
    int32_t       rx_hz;            /* RF frequency of the channel */
    int           num;              /* session number, 0 = from the archive */
    int           reply_to;         /* number of the call this acknowledges */
    int           answered_by;      /* number of the acknowledgement */
    char          file[64];
} dsc_call_t;

typedef enum { V_LIST = 0, V_DETAIL, V_BROWSER, V_FREQ } view_t;
static void explain_call(const dsc_call_t *c, char *dst, size_t sz);
typedef enum { SRC_LIVE = 0, SRC_BROWSER } src_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void show_view(view_t v);

/* ---- state -------------------------------------------------------- */

static dialog_t dialog;
dialog_t *dialog_dsc = &dialog;

static lv_obj_t *parent_obj = NULL;
static lv_obj_t *small_panel = NULL;
static lv_obj_t *list_obj = NULL;
static lv_obj_t *detail_obj = NULL;
static lv_obj_t *browser_obj = NULL;
static lv_obj_t *freq_obj = NULL;
static lv_timer_t *ui_timer = NULL;

static view_t view = V_LIST;
static bool small_size = true;

/* what the list and the browser show (cycled with F3 on page 2, kept in
 * DSC_FILTER_FILE).  Distress and urgency calls pass every filter. */
typedef enum { FLT_ALL = 0, FLT_NO_UNKNOWN, FLT_VERIFIED, FLT_EMERGENCY, FLT_N } filter_t;
static filter_t filter = FLT_ALL;
static char browser_empty[40] = "No saved calls";

/* live calls: kept while the radio is on, oldest first */
static dsc_call_t calls[DSC_MAX_CALLS];
static int ncalls = 0;
static int next_num = 1;
static int list_sel = 0;        /* display index, 0 = newest */
static int list_top = 0;

/* archive */
static dsc_call_t *bcalls = NULL;   /* newest first */
static int nbcalls = 0;

/* detail card */
static src_t detail_src = SRC_LIVE;
static int detail_idx = 0;

/* decoder and the audio -> UI queue */
static dsc_decoder_t *decoder = NULL;
static pthread_mutex_t decoder_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fifo_mutex = PTHREAD_MUTEX_INITIALIZER;
static dsc_message_t fifo[DSC_FIFO_LEN];
static int fifo_head = 0, fifo_count = 0;
static uint32_t dsp_audio_sub_id = AUDIO_SUB_INVALID;

/* WAV test */
static pthread_t test_thread;
static bool test_thread_valid = false;
static volatile bool test_mode = false;
static volatile bool test_stop = false;
static volatile bool test_done = false;

/* optional MMSI -> name file */
typedef struct { uint32_t mmsi; uint32_t off; } name_t;
static name_t *names = NULL;
static char *names_buf = NULL;
static int nnames = 0;
static bool names_loaded = false;

/* ---- buttons -------------------------------------------------------- */

static void page2_cb(button_data_t *d);
static void page1_cb(button_data_t *d);
static void freq_cb(button_data_t *d);
static void size_cb(button_data_t *d);
static void details_cb(button_data_t *d);
static void browser_cb(button_data_t *d);
static void clear_cb(button_data_t *d);
static void test_cb(button_data_t *d);
static void close_cb(button_data_t *d);
static void back_cb(button_data_t *d);
static void prev_cb(button_data_t *d);
static void next_cb(button_data_t *d);
static void delete_cb(button_data_t *d);
static void open_cb(button_data_t *d);
static void tune_cb(button_data_t *d);

static char freq_label[24] = "Freq.";
static char size_label[24] = "View Size\nSmall";
static char test_label[16] = "Test\nWAV";

static button_data_t btn_p1      = { .type = BTN_TEXT, .label = "(DSC 1:2)", .press = page2_cb };
static button_data_t btn_freq    = { .type = BTN_TEXT, .label = freq_label, .press = freq_cb };
static button_data_t btn_size    = { .type = BTN_TEXT, .label = size_label, .press = size_cb };
static button_data_t btn_details = { .type = BTN_TEXT, .label = "Details", .press = details_cb };
static button_data_t btn_browser = { .type = BTN_TEXT, .label = "Calls\nBrowser", .press = browser_cb };

static button_data_t btn_p2      = { .type = BTN_TEXT, .label = "(DSC 2:2)", .press = page1_cb };
static button_data_t btn_clear   = { .type = BTN_TEXT, .label = "Clear\nList", .press = clear_cb };
static button_data_t btn_test    = { .type = BTN_TEXT, .label = test_label, .press = test_cb };
static void filter_cb(button_data_t *d);
static char filter_label[24] = "Show all\ncalls";
static button_data_t btn_filter  = { .type = BTN_TEXT, .label = filter_label, .press = filter_cb };
static button_data_t btn_close   = { .type = BTN_TEXT, .label = "Close\nDSC", .press = close_cb };
/* 1KO125: Info window, see app_info.c */
static void info_cb(button_data_t *d) { (void)d; app_info_open(dialog.obj, APP_INFO_DSC); }
static button_data_t btn_info    = { .type = BTN_TEXT, .label = "Info", .press = info_cb };

static button_data_t btn_pd      = { .type = BTN_TEXT, .label = "(DSC Call)", .press = NULL };
static button_data_t btn_back    = { .type = BTN_TEXT, .label = "Back", .press = back_cb };
static button_data_t btn_prev    = { .type = BTN_TEXT, .label = "Newer\ncall", .press = prev_cb };
static button_data_t btn_next    = { .type = BTN_TEXT, .label = "Older\ncall", .press = next_cb };
static button_data_t btn_delete  = { .type = BTN_TEXT, .label = "Delete", .press = delete_cb };

static button_data_t btn_pb      = { .type = BTN_TEXT, .label = "(DSC\nBrowser)", .press = NULL };
static button_data_t btn_open    = { .type = BTN_TEXT, .label = "Open", .press = open_cb };
static button_data_t btn_bclose  = { .type = BTN_TEXT, .label = "Close\nBrowser", .press = back_cb };

static button_data_t btn_pf      = { .type = BTN_TEXT, .label = "(DSC Freq.)", .press = NULL };
static button_data_t btn_tune    = { .type = BTN_TEXT, .label = "Tune", .press = tune_cb };
static button_data_t btn_fclose  = { .type = BTN_TEXT, .label = "Close\nList", .press = back_cb };

static buttons_page_t page_list_1 = { .items = { &btn_p1, &btn_freq, &btn_size, &btn_details, &btn_browser } };
/* Test WAV (btn_test, test_cb) removed from the page, code kept: put &btn_test in a slot of page 2 to use it */
static buttons_page_t page_list_2 = { .items = { &btn_p2, &btn_clear, &btn_filter, &btn_info, &btn_close } };
static buttons_page_t page_detail = { .items = { &btn_pd, &btn_back, &btn_prev, &btn_next, &btn_delete } };
static buttons_page_t page_browser = { .items = { &btn_pb, &btn_open, &btn_delete, &btn_filter, &btn_bclose } };
static buttons_page_t page_freq = { .items = { &btn_pf, &btn_tune, NULL, NULL, &btn_fclose } };

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .key_cb = key_cb,
    .btn_page = &page_list_1
};

bool dialog_dsc_is_active(void)
{
    return dialog.run;
}

/* ---- frequencies ---------------------------------------------------- */

/*
 * DSC channels (ITU RR Appendix 17, FCC 47 CFR 80.359).  A row with a
 * section title and hz = 0 is a heading.  Worldwide channels are valid
 * everywhere; the regional series only in their area ("Series B": all
 * areas other than the Atlantic, Gulf of Mexico and Caribbean).
 */
typedef enum { SEC_DISTRESS = 0, SEC_WORLD, SEC_REGIONAL } sec_kind_t;
typedef struct { int32_t hz; const char *band; const char *section; sec_kind_t kind; } dsc_freq_t;

#define HEAD(t, k)  { 0, NULL, t, k }
#define CH(h, b)    { h, b, NULL, 0 }

static const dsc_freq_t dsc_freqs[] = {
    HEAD("DISTRESS & SAFETY - WORLDWIDE", SEC_DISTRESS),
    CH( 2187500, "MF"),     CH( 4207500, "4 MHz"),  CH( 6312000, "6 MHz"),
    CH( 8414500, "8 MHz"),  CH(12577000, "12 MHz"), CH(16804500, "16 MHz"),

    HEAD("SHIP TO SHIP - WORLDWIDE", SEC_WORLD),
    CH( 2177000, "MF"),

    HEAD("SHIP TX - WORLDWIDE", SEC_WORLD),
    CH( 2189500, "MF"),     CH( 4208000, "4 MHz"),  CH( 6312500, "6 MHz"),
    CH( 8415000, "8 MHz"),  CH(12577500, "12 MHz"), CH(16805000, "16 MHz"),
    CH(18898500, "18 MHz"), CH(22374500, "22 MHz"), CH(25208500, "25 MHz"),

    HEAD("COAST TX - WORLDWIDE", SEC_WORLD),
    CH( 2177000, "MF"),     CH( 4219500, "4 MHz"),  CH( 6331000, "6 MHz"),
    CH( 8436500, "8 MHz"),  CH(12657000, "12 MHz"), CH(16903000, "16 MHz"),
    CH(19703500, "19 MHz"), CH(22444000, "22 MHz"), CH(26121000, "26 MHz"),

    HEAD("SHIP TX - REGIONAL: MED & OTHER SEAS", SEC_REGIONAL),
    CH( 4209000, "4 MHz"),  CH( 6313500, "6 MHz"),  CH( 8416000, "8 MHz"),
    CH(12578500, "12 MHz"), CH(16806000, "16 MHz"), CH(18899500, "18 MHz"),
    CH(22375500, "22 MHz"), CH(25209500, "25 MHz"),

    HEAD("COAST TX - REGIONAL: MED & OTHER SEAS", SEC_REGIONAL),
    CH( 4220500, "4 MHz"),  CH( 6332000, "6 MHz"),  CH( 8437500, "8 MHz"),
    CH(12658000, "12 MHz"), CH(16904000, "16 MHz"), CH(19704500, "19 MHz"),
    CH(22445000, "22 MHz"), CH(26122000, "26 MHz"),

    HEAD("SHIP TX - REGIONAL: ATLANTIC & CARIBBEAN", SEC_REGIONAL),
    CH( 4208500, "4 MHz"),  CH( 6313000, "6 MHz"),  CH( 8415500, "8 MHz"),
    CH(12578000, "12 MHz"), CH(16805500, "16 MHz"), CH(18899000, "18 MHz"),
    CH(22375000, "22 MHz"), CH(25209000, "25 MHz"),

    HEAD("COAST TX - REGIONAL: ATLANTIC & CARIBBEAN", SEC_REGIONAL),
    CH( 4220000, "4 MHz"),  CH( 6331500, "6 MHz"),  CH( 8437000, "8 MHz"),
    CH(12657500, "12 MHz"), CH(16903500, "16 MHz"), CH(19704000, "19 MHz"),
    CH(22444500, "22 MHz"), CH(26121500, "26 MHz"),
};

#undef HEAD
#undef CH
#define DSC_NFREQS ((int)(sizeof(dsc_freqs) / sizeof(dsc_freqs[0])))

/* RF frequency of the 1000 Hz audio centre */
static int32_t channel_hz(void)
{
    int32_t f = cparam_i_get(cfg.cur.fg_freq());
    int mode = cparam_i_get(cfg.cur.mode());
    return (mode == x6100_mode_lsb || mode == x6100_mode_lsb_dig) ? f - 1000 : f + 1000;
}

static void fmt_khz(char *dst, size_t sz, int32_t hz)
{
    snprintf(dst, sz, "%ld.%ld", (long)(hz / 1000), (long)((hz % 1000) / 100));
}

/* row recalled from the list (2177.0 is listed twice), -1 = none */
static int freq_recalled = -1;

static bool freq_is_head(int i)
{
    return i >= 0 && i < DSC_NFREQS && dsc_freqs[i].hz == 0;
}

/* row of the current frequency: the recalled one if still tuned, else the
 * first row with that frequency, -1 when not in the list */
static int freq_current_row(void)
{
    int32_t hz = channel_hz();
    if (freq_recalled >= 0 && freq_recalled < DSC_NFREQS && dsc_freqs[freq_recalled].hz == hz)
        return freq_recalled;
    for (int i = 0; i < DSC_NFREQS; ++i) if (dsc_freqs[i].hz == hz) return i;
    return -1;
}

static void update_freq_label(void)
{
    char k[16];
    fmt_khz(k, sizeof(k), channel_hz());
    snprintf(freq_label, sizeof(freq_label), "Freq.\n%s", k);
    if (btn_freq.disp_btn) buttons_refresh(&btn_freq);
}

/* ---- names file ------------------------------------------------------ */

static int name_cmp(const void *a, const void *b)
{
    uint32_t x = ((const name_t *)a)->mmsi, y = ((const name_t *)b)->mmsi;
    return x < y ? -1 : x > y;
}

static void names_load(void)
{
    if (names_loaded) return;
    names_loaded = true;
    FILE *f = fopen(DSC_NAMES_FILE, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32L * 1024 * 1024) { fclose(f); return; }
    names_buf = malloc((size_t)sz + 1);
    names = malloc(sizeof(name_t) * DSC_NAMES_MAX);
    if (!names_buf || !names) { fclose(f); free(names_buf); free(names); names_buf = NULL; names = NULL; return; }
    size_t got = fread(names_buf, 1, (size_t)sz, f);
    fclose(f);
    names_buf[got] = '\0';

    char *p = names_buf;
    while (*p && nnames < DSC_NAMES_MAX) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        uint32_t m = 0;
        int nd = 0;
        while (isdigit((unsigned char)*q) && nd < 9) { m = m * 10 + (uint32_t)(*q - '0'); q++; nd++; }
        if (nd == 9 && (*q == ';' || *q == ',' || *q == '\t' || *q == ' ')) {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '"') q++;
            char *e = q + strlen(q);
            while (e > q && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '"' || e[-1] == ';')) *--e = '\0';
            /* keep the name only up to the next separator */
            char *s = strpbrk(q, ";\t");
            if (s) *s = '\0';
            if (*q) {
                names[nnames].mmsi = m;
                names[nnames].off = (uint32_t)(q - names_buf);
                nnames++;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    qsort(names, (size_t)nnames, sizeof(name_t), name_cmp);
    fprintf(stderr, "DSC: %d station names loaded from %s\n", nnames, DSC_NAMES_FILE);
}

/* built-in lists: coast stations, then the ships database */
static const char *builtin_name(const char *id)
{
    const char *c = dsc_coast_name(id);
    if (c) return c;
    dsc_ship_t sh;
    return dsc_ship_lookup(id, &sh) ? sh.name : NULL;
}

/* name of a station: the user's file first, then the built-in lists */
static const char *name_of(const char *id)
{
    if (!id || strlen(id) != 9) return NULL;
    if (!names || nnames == 0) return builtin_name(id);
    uint32_t m = 0;
    for (int i = 0; i < 9; ++i) {
        if (!isdigit((unsigned char)id[i])) return NULL;
        m = m * 10 + (uint32_t)(id[i] - '0');
    }
    name_t key = { m, 0 };
    name_t *r = bsearch(&key, names, (size_t)nnames, sizeof(name_t), name_cmp);
    return r ? names_buf + r->off : builtin_name(id);
}

/* ---- calls ------------------------------------------------------------ */

static uint32_t cat_color(dsc_cat_t c)
{
    switch (c) {
    case DSC_CAT_DISTRESS: return C_DISTRESS;
    case DSC_CAT_URGENCY:  return C_URGENCY;
    case DSC_CAT_SAFETY:   return C_SAFETY;
    case DSC_CAT_ROUTINE:  return C_ROUTINE;
    case DSC_CAT_SELFTEST: return C_SELFTEST;
    default:               return C_UNKNOWN;
    }
}

static bool cat_dark_text(dsc_cat_t c)
{
    return c == DSC_CAT_SAFETY || c == DSC_CAT_URGENCY || c == DSC_CAT_ROUTINE || c == DSC_CAT_SELFTEST;
}

/* MMSI equality with unreadable digits ('?') */
static bool id_match(const char *a, const char *b)
{
    if (strlen(a) != 9 || strlen(b) != 9) return false;
    int known = 0;
    for (int i = 0; i < 9; ++i) {
        if (a[i] == '?' || b[i] == '?') continue;
        if (a[i] != b[i]) return false;
        known++;
    }
    return known >= 7;
}

/* ---- view filter ------------------------------------------------------- */

static const char *const filter_labels[FLT_N] = {
    "Show all\ncalls", "Ignore\nunknown", "Show only\nverified", "Show only\nemergency",
};
/* a station whose MMSI was only partly received ("2534??000"); broadcasts
 * (all ships, area) have no MMSI to check */
static bool id_incomplete(dsc_kind_t k, const char *id)
{
    if (k == DSC_KIND_ALL || k == DSC_KIND_AREA) return false;
    return !id[0] || strchr(id, '?') != NULL;
}

static bool call_shown(const dsc_call_t *c)
{
    bool emergency = c->info.cat == DSC_CAT_DISTRESS || c->info.cat == DSC_CAT_URGENCY;
    switch (filter) {
    case FLT_NO_UNKNOWN:            /* unknown category, or sender / addressee not known */
        return emergency || (c->info.cat != DSC_CAT_UNKNOWN &&
                             !id_incomplete(c->info.from_kind, c->info.from_id) &&
                             !id_incomplete(c->info.to_kind, c->info.to_id));
    case FLT_VERIFIED:   return emergency || c->info.ecc_ok;
    case FLT_EMERGENCY:  return emergency;
    default:             return true;
    }
}

static void filter_load(void)
{
    FILE *f = fopen(DSC_FILTER_FILE, "r");
    int v = 0;
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = 0;
        fclose(f);
    }
    filter = (v >= 0 && v < FLT_N) ? (filter_t)v : FLT_ALL;
}

static void filter_save(void)
{
    if (mkdir(DSC_SAVE_DIR, 0755) != 0 && errno != EEXIST) return;
    FILE *f = fopen(DSC_FILTER_FILE, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)filter);
    fclose(f);
}

/* live calls shown by the filter */
static int live_count(void)
{
    int n = 0;
    for (int i = 0; i < ncalls; ++i) if (call_shown(&calls[i])) n++;
    return n;
}

/* the disp-th shown live call, newest first */
static dsc_call_t *live_at(int disp)
{
    if (disp < 0) return NULL;
    for (int i = ncalls - 1; i >= 0; --i) {
        if (!call_shown(&calls[i])) continue;
        if (disp-- == 0) return &calls[i];
    }
    return NULL;
}

static dsc_call_t *live_by_num(int num)
{
    for (int i = 0; i < ncalls; ++i)
        if (calls[i].num == num) return &calls[i];
    return NULL;
}

/* An acknowledgement (BQ) answers the latest call with swapped addresses. */
static void link_reply(dsc_call_t *c)
{
    if (c->info.eos != 122) return;
    for (int i = ncalls - 2; i >= 0; --i) {
        dsc_call_t *o = &calls[i];
        if (c->utc - o->utc > DSC_REPLY_WINDOW) break;
        if (o->answered_by || o->info.eos == 122) continue;
        if (id_match(o->info.from_id, c->info.to_id) && id_match(o->info.to_id, c->info.from_id)) {
            o->answered_by = c->num;
            c->reply_to = o->num;
            return;
        }
    }
}

static void call_from_message(dsc_call_t *c, const dsc_message_t *m, time_t utc, int32_t rx_hz)
{
    memset(c, 0, sizeof(*c));
    c->msg = *m;
    dsc_format_parse(m->sym, m->n, &c->info);
    /* the error check verifies a call only if the decoder trusts it: two
     * wrong characters can cancel out (see dsc_decoder.h) */
    if (!m->verified && c->info.ecc_ok) {
        c->info.ecc_ok = false;
        c->info.ecc_calc = -1;
    }
    c->utc = utc;
    c->rx_hz = rx_hz;
}

/* ---- saving and the archive -------------------------------------------- */

static bool ensure_dir(void)
{
    if (mkdir(DSC_SAVE_DIR, 0755) == 0 || errno == EEXIST) return true;
    fprintf(stderr, "DSC: cannot create %s: %s\n", DSC_SAVE_DIR, strerror(errno));
    return false;
}

static void save_call(dsc_call_t *c)
{
    if (!ensure_dir()) return;
    struct tm tm;
    gmtime_r(&c->utc, &tm);
    char base[48], path[128];
    strftime(base, sizeof(base), DSC_SAVE_PREFIX "%Y%m%d-%H%M%S", &tm);
    snprintf(c->file, sizeof(c->file), "%s" DSC_SAVE_EXT, base);
    snprintf(path, sizeof(path), DSC_SAVE_DIR "/%s", c->file);
    for (int k = 2; access(path, F_OK) == 0 && k < 100; ++k) {
        snprintf(c->file, sizeof(c->file), "%s-%d" DSC_SAVE_EXT, base, k);
        snprintf(path, sizeof(path), DSC_SAVE_DIR "/%s", c->file);
    }
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "DSC: cannot write %s\n", path); c->file[0] = '\0'; return; }
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "DSC call\nutc=%s\nfreq_hz=%ld\ncenter_hz=%.0f\nvalid=%d/%d\nrepaired=%d\nunsure=%d\nverified=%d\nsymbols=",
            ts, (long)c->rx_hz, c->msg.center_hz, c->msg.raw_valid, c->msg.raw_total, c->msg.repaired,
            c->msg.unsure, c->msg.verified ? 1 : 0);
    for (int i = 0; i < c->msg.n; ++i) fprintf(f, "%s%d", i ? " " : "", c->msg.sym[i]);
    fprintf(f, "\n\n%s - %s\nfrom %s (%s)\nto   %s (%s)\n%s\n\n",
            c->info.cat_name, c->info.fmt_name, c->info.from_id, c->info.from_where,
            c->info.to_id, c->info.to_where, c->info.line2);
    for (int i = 0; i < c->info.nfields; ++i)
        fprintf(f, "%-18s %s\n", c->info.fields[i].key, c->info.fields[i].val);
    char ex[512];
    explain_call(c, ex, sizeof(ex));
    fprintf(f, "\nMeaning: %s\n", ex);
    fclose(f);
}

static bool load_call(const char *name, dsc_call_t *c)
{
    char path[160];
    snprintf(path, sizeof(path), DSC_SAVE_DIR "/%s", name);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    dsc_message_t m;
    memset(&m, 0, sizeof(m));
    char line[512], ts[32] = "";
    long hz = 0;
    float ctr = 0;
    bool have_sym = false, have_verified = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "utc=", 4)) snprintf(ts, sizeof(ts), "%.19s", line + 4);
        else if (!strncmp(line, "freq_hz=", 8)) hz = atol(line + 8);
        else if (!strncmp(line, "center_hz=", 10)) ctr = (float)atof(line + 10);
        else if (!strncmp(line, "valid=", 6)) sscanf(line + 6, "%d/%d", &m.raw_valid, &m.raw_total);
        else if (!strncmp(line, "repaired=", 9)) m.repaired = atoi(line + 9);
        else if (!strncmp(line, "unsure=", 7)) m.unsure = atoi(line + 7);
        else if (!strncmp(line, "verified=", 9)) { m.verified = atoi(line + 9) != 0; have_verified = true; }
        else if (!strncmp(line, "symbols=", 8)) {
            char *p = line + 8, *e;
            while (m.n < DSC_MAX_SYMBOLS) {
                long v = strtol(p, &e, 10);
                if (e == p) break;
                m.sym[m.n++] = (int)v;
                p = e;
            }
            have_sym = true;
        }
    }
    fclose(f);
    if (!have_sym) return false;
    m.center_hz = ctr;
    if (!have_verified) m.verified = true;     /* saved before 1.0.2 beta 6: as it was then */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    time_t utc = 0;
    if (sscanf(ts, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        utc = timegm(&tm);
    }
    call_from_message(c, &m, utc, (int32_t)hz);
    snprintf(c->file, sizeof(c->file), "%s", name);
    return true;
}

static int file_cmp_desc(const void *a, const void *b)
{
    return strcmp(*(char *const *)b, *(char *const *)a);
}

static void browser_free(void)
{
    free(bcalls);
    bcalls = NULL;
    nbcalls = 0;
}

/* a saved call worth keeping forever: DISTRESS or URGENCY */
static bool file_is_important(const char *name)
{
    char path[160], line[160];
    snprintf(path, sizeof(path), DSC_SAVE_DIR "/%s", name);
    FILE *f = fopen(path, "r");
    if (!f) return true;                        /* when in doubt, keep it */
    bool imp = false;
    for (int i = 0; i < 20 && fgets(line, sizeof(line), f); ++i)
        if (!strncmp(line, "DISTRESS", 8) || !strncmp(line, "URGENCY", 7)) { imp = true; break; }
    fclose(f);
    return imp;
}

/* list sorted newest first: delete the calls beyond DSC_KEEP_FILES */
static void prune_files(char **list, int n)
{
    int deleted = 0, kept = 0;
    for (int i = DSC_KEEP_FILES; i < n; ++i) {
        if (file_is_important(list[i])) { kept++; continue; }
        char path[160];
        snprintf(path, sizeof(path), DSC_SAVE_DIR "/%s", list[i]);
        if (unlink(path) == 0) deleted++;
    }
    if (deleted || kept)
        fprintf(stderr, "DSC: %d old saved calls deleted (newest %d kept, %d distress/urgency kept)\n",
                deleted, DSC_KEEP_FILES, kept);
}

static void browser_load(void)
{
    browser_free();
    DIR *d = opendir(DSC_SAVE_DIR);
    if (!d) return;
    char **list = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    size_t pl = strlen(DSC_SAVE_PREFIX), el = strlen(DSC_SAVE_EXT);
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l <= pl + el || strncmp(de->d_name, DSC_SAVE_PREFIX, pl) || strcmp(de->d_name + l - el, DSC_SAVE_EXT))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            char **nl = realloc(list, sizeof(char *) * (size_t)cap);
            if (!nl) break;
            list = nl;
        }
        list[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (n == 0) { free(list); return; }
    qsort(list, (size_t)n, sizeof(char *), file_cmp_desc);
    if (n > DSC_KEEP_FILES) prune_files(list, n);
    int keep = n < DSC_BROWSER_MAX ? n : DSC_BROWSER_MAX;
    bcalls = calloc((size_t)keep, sizeof(dsc_call_t));
    for (int i = 0; i < n; ++i) {
        if (bcalls && nbcalls < keep && load_call(list[i], &bcalls[nbcalls]) && call_shown(&bcalls[nbcalls]))
            nbcalls++;
        free(list[i]);
    }
    free(list);
}

/* ---- drawing helpers ------------------------------------------------------ */

static void d_rect(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2,
                   uint32_t color, lv_coord_t radius, uint32_t border, lv_coord_t bw)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(color);
    r.bg_opa = LV_OPA_COVER;
    r.radius = radius;
    if (bw > 0) {
        r.border_color = lv_color_hex(border);
        r.border_width = bw;
        r.border_opa = LV_OPA_COVER;
    }
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(ctx, &r, &a);
}

static lv_coord_t text_w(const lv_font_t *font, const char *t)
{
    lv_point_t sz;
    lv_txt_get_size(&sz, t, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* Text at (x, y); w > 0 limits the width (".." when cut); returns the end x */
static lv_coord_t d_text(lv_draw_ctx_t *ctx, const lv_font_t *font, uint32_t color,
                         lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_text_align_t align, const char *t)
{
    char buf[128];
    if (w > 0 && text_w(font, t) > w) {
        size_t n = strlen(t);
        if (n > sizeof(buf) - 3) n = sizeof(buf) - 3;
        memcpy(buf, t, n);
        buf[n] = '\0';
        while (n > 0) {
            buf[n] = '\0';
            strcat(buf, "..");
            if (text_w(font, buf) <= w) break;
            buf[--n] = '\0';
        }
        t = buf;
    }
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = font;
    l.color = lv_color_hex(color);
    l.align = align;
    lv_coord_t tw = text_w(font, t);
    lv_area_t a;
    if (align == LV_TEXT_ALIGN_RIGHT) { a.x1 = x - (w > 0 ? w : tw); a.x2 = x; }
    else if (align == LV_TEXT_ALIGN_CENTER) { a.x1 = x - (w > 0 ? w : tw) / 2; a.x2 = a.x1 + (w > 0 ? w : tw); }
    else { a.x1 = x; a.x2 = x + (w > 0 ? w : tw) + 2; }
    a.y1 = y;
    a.y2 = y + lv_font_get_line_height(font);
    lv_draw_label(ctx, &l, &a, t, NULL);
    return x + tw;
}

static void d_line(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2,
                   uint32_t color, lv_coord_t width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    d.opa = LV_OPA_COVER;
    lv_point_t a = { x1, y1 }, b = { x2, y2 };
    lv_draw_line(ctx, &d, &a, &b);
}

static void d_poly(lv_draw_ctx_t *ctx, const lv_point_t *p, uint16_t n, uint32_t color)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(color);
    r.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(ctx, &r, p, n);
}

/* 30 x 22 icons, top-left at (x, y), scaled by s/10 */
static void icon_ship(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, int s, uint32_t c)
{
    #define S(v) ((lv_coord_t)((v) * s / 10))
    lv_point_t hull[4] = { { x, y + S(12) }, { x + S(30), y + S(12) }, { x + S(25), y + S(20) }, { x + S(5), y + S(20) } };
    d_poly(ctx, hull, 4, c);
    d_rect(ctx, x + S(8), y + S(5), x + S(20), y + S(12), c, 1, 0, 0);
    d_rect(ctx, x + S(12), y, x + S(15), y + S(5), c, 0, 0, 0);
    #undef S
}

static void icon_coast(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, int s, uint32_t c)
{
    #define S(v) ((lv_coord_t)((v) * s / 10))
    lv_coord_t cx = x + S(15), top = y + S(4), bot = y + S(22);
    d_line(ctx, cx, top, x + S(7), bot, c, 2);
    d_line(ctx, cx, top, x + S(23), bot, c, 2);
    d_line(ctx, x + S(10), y + S(15), x + S(20), y + S(15), c, 2);
    d_line(ctx, x + S(9), bot, x + S(21), bot, c, 2);
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.color = lv_color_hex(c);
    a.width = 2;
    lv_point_t ctr = { cx, top };
    lv_draw_arc(ctx, &a, &ctr, (uint16_t)S(6), 200, 340);
    lv_draw_arc(ctx, &a, &ctr, (uint16_t)S(11), 205, 335);
    #undef S
}

static void icon_kind(lv_draw_ctx_t *ctx, dsc_kind_t k, lv_coord_t x, lv_coord_t y, int s, uint32_t c)
{
    if (k == DSC_KIND_COAST) icon_coast(ctx, x, y, s, c);
    else if (k == DSC_KIND_SHIP || k == DSC_KIND_GROUP || k == DSC_KIND_SAR || k == DSC_KIND_BEACON)
        icon_ship(ctx, x, y, s, c);
}

/* flag of a station's country (from the MMSI), NULL for broadcasts or unknown */
static const lv_img_dsc_t *station_flag(dsc_kind_t k, const char *id)
{
    if (k == DSC_KIND_ALL || k == DSC_KIND_AREA || k == DSC_KIND_UNKNOWN) return NULL;
    int mid = -1;
    dsc_mmsi_kind(id, &mid);
    return mid > 0 ? dsc_flag(dsc_mid_iso(mid)) : NULL;
}

/* the flag with its top left corner at (x, y) */
static void d_flag(lv_draw_ctx_t *ctx, const lv_img_dsc_t *flag, lv_coord_t x, lv_coord_t y)
{
    lv_area_t fa = { x, y, x + (lv_coord_t)flag->header.w - 1, y + (lv_coord_t)flag->header.h - 1 };
    lv_draw_img_dsc_t dsc;
    lv_draw_img_dsc_init(&dsc);
    lv_draw_img(ctx, &dsc, &fa, flag);
}

static void d_arrow(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, lv_coord_t len, uint32_t c)
{
    d_line(ctx, x, y, x + len - 8, y, c, 3);
    lv_point_t t[3] = { { x + len - 10, y - 6 }, { x + len, y }, { x + len - 10, y + 6 } };
    d_poly(ctx, t, 3, c);
}

/* "MMSI  where" or a plain address; returns the end x */
static lv_coord_t d_station(lv_draw_ctx_t *ctx, dsc_kind_t k, const char *id, const char *where,
                            lv_coord_t x, lv_coord_t y, lv_coord_t maxw)
{
    lv_coord_t x0 = x;
    /* "Liberia - ship" is shown as "Liberia ship" */
    char wh[64];
    const char *dash = strstr(where, " - ");
    if (dash) snprintf(wh, sizeof(wh), "%.*s %s", (int)(dash - where), where, dash + 3);
    else snprintf(wh, sizeof(wh), "%s", where);
    if (k == DSC_KIND_ALL || k == DSC_KIND_AREA) {
        x = d_text(ctx, &sony_24, C_TEXT, x, y, maxw, LV_TEXT_ALIGN_LEFT, id);
        if (wh[0]) x = d_text(ctx, &sony_18, C_SUB, x + 8, y + 5, maxw - (x - x0) - 8, LV_TEXT_ALIGN_LEFT, wh);
        return x;
    }
    if (k != DSC_KIND_UNKNOWN) { icon_kind(ctx, k, x, y + 1, 9, C_TEXT); x += 34; }
    const char *nm = name_of(id);
    if (nm) x = d_text(ctx, &sony_24, C_TEXT, x, y, maxw - (x - x0) - 40, LV_TEXT_ALIGN_LEFT, nm);
    else x = d_text(ctx, &sony_24, C_TEXT, x, y, 0, LV_TEXT_ALIGN_LEFT, id);
    /* after a name the country, not the MMSI again (it is in the detail card) */
    if (wh[0] && maxw - (x - x0) > 30)
        x = d_text(ctx, &sony_18, C_SUB, x + 8, y + 5, maxw - (x - x0) - 8, LV_TEXT_ALIGN_LEFT, wh);
    return x;
}

/* ---- the call panel ----------------------------------------------------------- */

/* share of the received characters that passed their check, in % */
static int quality_pct(const dsc_call_t *c)
{
    if (c->msg.raw_total <= 0) return 0;
    return (c->msg.raw_valid * 100 + c->msg.raw_total / 2) / c->msg.raw_total;
}

static void draw_panel(lv_draw_ctx_t *ctx, const dsc_call_t *c, lv_coord_t x1, lv_coord_t y1,
                       lv_coord_t x2, lv_coord_t y2, bool selected)
{
    uint32_t cc = cat_color(c->info.cat);
    bool distress = c->info.cat == DSC_CAT_DISTRESS;
    uint32_t border = selected ? C_SELECT : (distress ? C_DISTRESS : C_LINE);
    d_rect(ctx, x1, y1, x2, y2, selected ? C_PANEL_SEL : C_PANEL, 6, border, (selected || distress) ? 3 : 1);

    /* category band */
    lv_coord_t bw = 92;
    d_rect(ctx, x1, y1, x1 + bw, y2, cc, 6, 0, 0);
    d_rect(ctx, x1 + bw - 10, y1, x1 + bw, y2, cc, 0, 0, 0);
    if (selected || distress)        /* keep the border visible over the band */
        d_rect(ctx, x1, y1, x1 + 2, y2, border, 0, 0, 0);
    uint32_t tc = cat_dark_text(c->info.cat) ? 0x141414 : 0xffffff;
    lv_coord_t h = y2 - y1;
    /* bw - 2: "SELF TEST" (88 px) is the widest name */
    d_text(ctx, &sony_20, tc, x1 + bw / 2, y1 + h / 2 - 20, bw - 2, LV_TEXT_ALIGN_CENTER, c->info.cat_name);
    char num[16];
    if (c->num) snprintf(num, sizeof(num), "#%d", c->num); else strcpy(num, "saved");
    d_text(ctx, &sony_20, tc, x1 + bw / 2, y1 + h / 2 + 2, bw - 6, LV_TEXT_ALIGN_CENTER, num);

    /* line 1: origin -> destination (the whole width, the time is on line 3) */
    lv_coord_t x = x1 + bw + 12, ly = y1 + 5;
    lv_coord_t avail = x2 - 12 - x;
    lv_coord_t half = (avail - 52) / 2;
    lv_coord_t e = d_station(ctx, c->info.from_kind, c->info.from_id, c->info.from_where, x, ly, half);
    d_arrow(ctx, e + 12, ly + 14, 28, C_TEXT);
    d_station(ctx, c->info.to_kind, c->info.to_id, c->info.to_where, e + 52, ly, x2 - 12 - (e + 52));

    /* line 3: technical, reply link and time on the right */
    const lv_font_t *f3 = &sony_22;
    lv_coord_t l3y = y2 - lv_font_get_line_height(f3) - 3;
    struct tm tm;
    gmtime_r(&c->utc, &tm);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    d_text(ctx, &sony_20, C_TEXT, x2 - 12, l3y + 1, 0, LV_TEXT_ALIGN_RIGHT, ts);
    lv_coord_t r = x2 - 12 - text_w(&sony_20, ts) - 14;     /* d_text returns x + width */
    char k[16], l3[96], link[32] = "";
    fmt_khz(k, sizeof(k), c->rx_hz);
    snprintf(l3, sizeof(l3), "%s kHz - ECC %s - quality %d%%", k,
             c->info.ecc_ok ? "OK" : (c->info.ecc_calc >= 0 ? "WRONG" : "not verifiable"),
             quality_pct(c));
    if (c->reply_to) snprintf(link, sizeof(link), "reply to #%d", c->reply_to);
    else if (c->answered_by) snprintf(link, sizeof(link), "answered by #%d", c->answered_by);
    if (link[0]) {
        d_text(ctx, &sony_18, C_OK, r, l3y + 2, 0, LV_TEXT_ALIGN_RIGHT, link);
        r -= text_w(&sony_18, link) + 14;
    }
    /* the reply link and the time are never cut: if the technical text does
     * not fit beside them, its compact form is used */
    if (text_w(f3, l3) > r - x)
        snprintf(l3, sizeof(l3), "%s kHz - ECC %s - %d%%", k,
                 c->info.ecc_ok ? "OK" : (c->info.ecc_calc >= 0 ? "WRONG" : "n/a"), quality_pct(c));
    d_text(ctx, f3, C_SUB, x, l3y, r - x, LV_TEXT_ALIGN_LEFT, l3);

    /* line 2: what it says, between line 1 and line 3 */
    const lv_font_t *f2 = &sony_22;
    lv_coord_t top = ly + lv_font_get_line_height(&sony_24);
    lv_coord_t l2y = top + (l3y - top - lv_font_get_line_height(f2)) / 2;
    d_text(ctx, f2, C_TEXT, x, l2y, x2 - 12 - x, LV_TEXT_ALIGN_LEFT, c->info.line2);
}

static int list_rows(void)
{
    return small_size ? 2 : 4;
}

static void list_fix_top(void)
{
    int rows = list_rows();
    int shown = live_count();
    if (list_sel >= shown) list_sel = shown - 1;
    if (list_sel < 0) list_sel = 0;
    if (list_sel < list_top) list_top = list_sel;
    if (list_sel >= list_top + rows) list_top = list_sel - rows + 1;
    if (list_top > shown - rows) list_top = shown - rows;
    if (list_top < 0) list_top = 0;
}

static void list_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);

    if (live_count() == 0) {
        char k[16], t[96];
        fmt_khz(k, sizeof(k), channel_hz());
        if (test_mode) snprintf(t, sizeof(t), "Playing " DSC_TEST_WAV "...");
        else if (ncalls > 0) snprintf(t, sizeof(t), "Listening on %s kHz - no calls pass the filter", k);
        else snprintf(t, sizeof(t), "Listening on %s kHz - no calls yet", k);
        lv_coord_t h = lv_obj_get_height(obj);
        d_text(ctx, &sony_22, C_SUB, (box.x1 + box.x2) / 2, box.y1 + h / 2 - 14, box.x2 - box.x1 - 20,
               LV_TEXT_ALIGN_CENTER, t);
        return;
    }
    int rows = list_rows();
    lv_coord_t h = box.y2 - box.y1 + 1;
    lv_coord_t ph = (h - PANEL_GAP * (rows - 1)) / rows;
    for (int r = 0; r < rows; ++r) {
        dsc_call_t *c = live_at(list_top + r);
        if (!c) break;
        lv_coord_t y = box.y1 + r * (ph + PANEL_GAP);
        draw_panel(ctx, c, box.x1, y, box.x2, y + ph - 1, list_top + r == list_sel);
    }
}

/* ---- the detail card --------------------------------------------------------- */

/* plain-language meaning of a call, with the reply link */
static void explain_call(const dsc_call_t *c, char *dst, size_t sz)
{
    dsc_explain(&c->info, name_of(c->info.from_id), name_of(c->info.to_id),
                c->info.dis_id[0] ? name_of(c->info.dis_id) : NULL, (long)c->rx_hz, dst, sz);
    size_t L = strlen(dst);
    if (c->reply_to) snprintf(dst + L, sz - L, " This answers call #%d.", c->reply_to);
    else if (c->answered_by) snprintf(dst + L, sz - L, " Call #%d answers it.", c->answered_by);
}

/*
 * The meaning for the detail card: upper case, with the stations (name, or
 * MMSI when the name is not known) in the colour of the category, using the
 * LVGL recolour syntax "#rrggbb text#" ('#' in the text becomes "##").
 */
static void explain_markup(const dsc_call_t *c, uint32_t color, char *dst, size_t sz)
{
    char ex[512];
    explain_call(c, ex, sizeof(ex));
    for (char *p = ex; *p; ++p) *p = (char)toupper((unsigned char)*p);

    /* what to colour: each station as it appears in the text */
    char keys[3][40];
    int nk = 0;
    const char *ids[3] = { c->info.from_id, c->info.to_id, c->info.dis_id };
    for (int i = 0; i < 3; ++i) {
        if (!ids[i][0] || !strcmp(ids[i], "unknown")) continue;
        const char *nm = name_of(ids[i]);
        snprintf(keys[nk], sizeof(keys[nk]), "%s", nm ? nm : ids[i]);
        for (char *p = keys[nk]; *p; ++p) *p = (char)toupper((unsigned char)*p);
        if (strlen(keys[nk]) >= 2) nk++;
    }

    size_t o = 0;
    #define PUT(ch) do { if (o + 1 < sz) dst[o++] = (ch); } while (0)
    for (const char *p = ex; *p; ) {
        int hit = -1;
        bool word_start = p == ex || !isalnum((unsigned char)p[-1]);
        for (int k = 0; k < nk && word_start; ++k) {
            size_t L = strlen(keys[k]);
            if (!strncmp(p, keys[k], L) && !isalnum((unsigned char)p[L])) { hit = k; break; }
        }
        if (hit >= 0 && o + 16 + strlen(keys[hit]) < sz) {
            o += (size_t)snprintf(dst + o, sz - o, "#%06X ", (unsigned)(color & 0xffffff));
            for (const char *q = keys[hit]; *q; ++q) {
                if (*q == '#') PUT('#');
                PUT(*q);
            }
            PUT('#');
            p += strlen(keys[hit]);
            continue;
        }
        if (*p == '#') PUT('#');            /* literal '#': "call ##25" */
        PUT(*p);
        p++;
    }
    #undef PUT
    dst[o < sz ? o : sz - 1] = '\0';
}

/* wrapped text in [x1, x2] starting at y; returns its height.  The text may
 * use the recolour syntax "#rrggbb text#" (a literal '#' is "##"). */
static lv_coord_t d_text_wrap(lv_draw_ctx_t *ctx, const lv_font_t *font, uint32_t color,
                              lv_coord_t x1, lv_coord_t y, lv_coord_t x2, lv_coord_t line_space,
                              const char *t, bool draw)
{
    lv_point_t sz;
    lv_txt_get_size(&sz, t, font, 0, line_space, x2 - x1, LV_TEXT_FLAG_RECOLOR);
    if (draw) {
        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.font = font;
        l.color = lv_color_hex(color);
        l.line_space = line_space;
        l.flag = LV_TEXT_FLAG_RECOLOR;
        lv_area_t a = { x1, y, x2, y + sz.y };
        lv_draw_label(ctx, &l, &a, t, NULL);
    }
    return sz.y;
}

/* ---- QR code of the ship (VesselFinder page) ---------------------------------- */

#define QR_URL      "https://vesselfinder.com/vessels/details/%s"
#define QR_MOD      3                   /* pixels per module (2 when space is short) */
#define QR_QUIET    2                   /* white modules around the code */

static uint8_t qr_code[dscqr_BUFFER_LEN_FOR_VERSION(6)];
static char qr_for[16] = "";            /* MMSI of qr_code, "" = none */

static bool id_full(const char *id)
{
    if (strlen(id) != 9) return false;
    for (int i = 0; i < 9; ++i) if (!isdigit((unsigned char)id[i])) return false;
    return true;
}

/* the ship of a call: the calling one, else the called one; NULL if none */
static const char *qr_ship(const dsc_info_t *in)
{
    if (in->from_kind == DSC_KIND_SHIP && id_full(in->from_id)) return in->from_id;
    if (in->to_kind == DSC_KIND_SHIP && id_full(in->to_id)) return in->to_id;
    return NULL;
}

/* QR code of the VesselFinder page of mmsi (kept until another MMSI is
 * asked); returns its size in modules, 0 on error */
static int qr_make(const char *mmsi)
{
    if (strcmp(qr_for, mmsi)) {
        char url[80];
        uint8_t tmp[dscqr_BUFFER_LEN_FOR_VERSION(6)];
        snprintf(url, sizeof(url), QR_URL, mmsi);
        qr_for[0] = '\0';
        if (!dscqr_encodeText(url, tmp, qr_code, dscqr_Ecc_LOW, 1, 6, dscqr_Mask_AUTO, true)) return 0;
        snprintf(qr_for, sizeof(qr_for), "%s", mmsi);
    }
    return dscqr_getSize(qr_code);
}

/* the code at (x, y), quiet zone included: black runs on white */
static void d_qr(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, int n, int m)
{
    lv_coord_t side = (n + 2 * QR_QUIET) * m;
    d_rect(ctx, x, y, x + side - 1, y + side - 1, 0xffffff, 0, 0, 0);
    x += QR_QUIET * m;
    y += QR_QUIET * m;
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ) {
            if (!dscqr_getModule(qr_code, c, r)) { c++; continue; }
            int e = c;
            while (e < n && dscqr_getModule(qr_code, e, r)) e++;
            d_rect(ctx, x + c * m, y + r * m, x + e * m - 1, y + (r + 1) * m - 1, 0x000000, 0, 0, 0);
            c = e;
        }
    }
}

static dsc_call_t *detail_call(void)
{
    if (detail_src == SRC_LIVE) return live_at(detail_idx);
    return (detail_idx >= 0 && detail_idx < nbcalls) ? &bcalls[detail_idx] : NULL;
}

static int detail_count(void)
{
    return detail_src == SRC_LIVE ? live_count() : nbcalls;
}

static void detail_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t b;
    lv_obj_get_coords(obj, &b);
    dsc_call_t *c = detail_call();
    if (!c) {
        d_text(ctx, &sony_22, C_SUB, (b.x1 + b.x2) / 2, b.y1 + 150, 0, LV_TEXT_ALIGN_CENTER, "No call");
        return;
    }
    const dsc_info_t *in = &c->info;
    lv_coord_t x1 = b.x1 + 6, x2 = b.x2 - 6, y = b.y1 + 6;

    /* header */
    uint32_t cc = cat_color(in->cat), tc = cat_dark_text(in->cat) ? 0x141414 : 0xffffff;
    d_rect(ctx, x1, y, x2, y + 32, cc, 6, 0, 0);
    char t[96];
    snprintf(t, sizeof(t), "%s - %s", in->cat_name, in->fmt_name);
    d_text(ctx, &sony_24, tc, x1 + 12, y + 3, 470, LV_TEXT_ALIGN_LEFT, t);
    struct tm tm;
    gmtime_r(&c->utc, &tm);
    char ts[40], num[16];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", &tm);
    if (c->num) snprintf(num, sizeof(num), "#%d  ", c->num); else num[0] = '\0';
    snprintf(t, sizeof(t), "%s%s", num, ts);
    d_text(ctx, &sony_18, tc, x2 - 12, y + 7, 0, LV_TEXT_ALIGN_RIGHT, t);
    y += 37;

    /* from -> to */
    lv_coord_t bw = 330;
    for (int i = 0; i < 2; ++i) {
        lv_coord_t bx = i == 0 ? x1 : x2 - bw;
        d_rect(ctx, bx, y, bx + bw, y + 50, C_PANEL, 6, C_LINE, 1);
        dsc_kind_t k = i == 0 ? in->from_kind : in->to_kind;
        const char *id = i == 0 ? in->from_id : in->to_id;
        const char *wh = i == 0 ? in->from_where : in->to_where;
        lv_coord_t tx = bx + 10;
        if (k != DSC_KIND_ALL && k != DSC_KIND_AREA && k != DSC_KIND_UNKNOWN) {
            icon_kind(ctx, k, bx + 10, y + 12, 12, C_TEXT);
            tx = bx + 54;
        }
        /* flag of the station's country, at the top right of its box */
        lv_coord_t nx2 = bx + bw - 8;
        const lv_img_dsc_t *flag = station_flag(k, id);
        if (flag) {
            lv_coord_t fx = bx + bw - 8 - (lv_coord_t)flag->header.w;
            d_flag(ctx, flag, fx, y + 6);
            nx2 = fx - 8;                       /* the name stops before the flag */
        }
        const char *nm = name_of(id);
        d_text(ctx, &sony_26, C_TEXT, tx, y + 5, nx2 - tx, LV_TEXT_ALIGN_LEFT, nm ? nm : id);
        char w2[80];
        if (nm) snprintf(w2, sizeof(w2), "%s - %s", id, wh); else snprintf(w2, sizeof(w2), "%s", wh);
        d_text(ctx, &sony_14, C_SUB, tx, y + 31, bx + bw - 8 - tx, LV_TEXT_ALIGN_LEFT, w2);
    }
    d_arrow(ctx, x1 + bw + 14, y + 25, (x2 - bw) - (x1 + bw) - 28, C_TEXT);
    y += 56;

    /* fields: decoded ones, then reception data */
    dsc_field_t f[DSC_FIELDS_MAX + 8];
    int nf = 0;
    for (int i = 0; i < in->nfields; ++i) f[nf++] = in->fields[i];
    /* ship particulars from the database: "Yacht, 3FOV" */
    for (int i = 0; i < 2; ++i) {
        dsc_ship_t sh;
        if (!dsc_ship_lookup(i == 0 ? in->from_id : in->to_id, &sh)) continue;
        char v[DSC_VAL_LEN] = "";
        size_t L = 0;
        #define ADDV(...) do { if (L < sizeof(v)) L += (size_t)snprintf(v + L, sizeof(v) - L, __VA_ARGS__); } while (0)
        if (sh.type[0]) ADDV("%s", sh.type);
        if (sh.call[0]) ADDV("%s%s", L ? ", " : "", sh.call);
        #undef ADDV
        if (!v[0]) continue;
        snprintf(f[nf].key, DSC_KEY_LEN, "%s ship", i == 0 ? "From" : "To");
        snprintf(f[nf].val, DSC_VAL_LEN, "%s", v);
        f[nf++].hl = DSC_HL_NONE;
    }
    char k[16];
    fmt_khz(k, sizeof(k), c->rx_hz);
    if (c->reply_to) { snprintf(f[nf].key, DSC_KEY_LEN, "Reply to"); snprintf(f[nf].val, DSC_VAL_LEN, "#%d", c->reply_to); f[nf++].hl = DSC_HL_OK; }
    if (c->answered_by) { snprintf(f[nf].key, DSC_KEY_LEN, "Answered by"); snprintf(f[nf].val, DSC_VAL_LEN, "#%d", c->answered_by); f[nf++].hl = DSC_HL_OK; }
    snprintf(f[nf].key, DSC_KEY_LEN, "Received on"); snprintf(f[nf].val, DSC_VAL_LEN, "%s kHz", k); f[nf++].hl = DSC_HL_NONE;
    snprintf(f[nf].key, DSC_KEY_LEN, "Quality");
    snprintf(f[nf].val, DSC_VAL_LEN, "%d%% (%d/%d) - %d repaired", quality_pct(c),
             c->msg.raw_valid, c->msg.raw_total, c->msg.repaired);
    f[nf++].hl = DSC_HL_NONE;
    snprintf(f[nf].key, DSC_KEY_LEN, "Audio centre"); snprintf(f[nf].val, DSC_VAL_LEN, "%.0f Hz", c->msg.center_hz); f[nf++].hl = DSC_HL_NONE;

    /* fields: names in grey (sony_18), values in white (sony_20) right after
     * the longest name of their column */
    lv_coord_t colw = (x2 - x1) / 2, rh = 21;
    int rows = (nf + 1) / 2;

    /* meaning of the call, at the bottom, upper case with the stations in
     * the colour of the category: the largest font that fits below the
     * fields; if even the smallest does not, fewer field rows are shown */
    char ex[768];
    explain_markup(c, in->cat == DSC_CAT_UNKNOWN ? C_TEXT : cc, ex, sizeof(ex));
    lv_coord_t ex_bottom = b.y2 - 4;

    /* QR code of the ship's VesselFinder page, at the bottom left */
    const char *qs = qr_ship(in);
    int qn = qs ? qr_make(qs) : 0;
    /* the largest font that fits beside the code; with 3 pixel modules
     * first, then with 2, so that no field row is lost */
    static const lv_font_t *const efonts[] = { &sony_24, &sony_22, &sony_20, &sony_18, &sony_16 };
    const lv_font_t *ef = NULL;
    lv_coord_t els = 3, eh = 0, bh = 0, qside = 0, tx1 = x1 + 10;
    int qm = QR_MOD;
    for (bool fits = false; !fits; ) {
        qside = qn ? (qn + 2 * QR_QUIET) * qm : 0;
        tx1 = x1 + 10 + (qn ? qside + 12 : 0);
        for (size_t i = 0; i < sizeof(efonts) / sizeof(efonts[0]); ++i) {
            ef = efonts[i];
            eh = d_text_wrap(ctx, ef, C_TEXT, tx1, 0, x2 - 10, els, ex, false);
            bh = eh > qside ? eh : qside;
            if (ex_bottom - bh - 8 >= y + rows * rh) { fits = true; break; }
        }
        if (!fits && qn && qm > 2) qm--;
        else break;
    }
    lv_coord_t ey = ex_bottom - bh;

    int max_rows = (ey - 6 - y) / rh;
    if (max_rows < 1) max_rows = 1;
    if (rows > max_rows) rows = max_rows;
    for (int col = 0; col < 2; ++col) {
        lv_coord_t cx = x1 + col * colw, kw = 0;
        for (int i = col * rows; i < nf && i < (col + 1) * rows; ++i) {
            lv_coord_t w = text_w(&sony_18, f[i].key);
            if (w > kw) kw = w;
        }
        lv_coord_t vx = cx + 4 + kw + 12;
        for (int i = col * rows; i < nf && i < (col + 1) * rows; ++i) {
            lv_coord_t cy = y + (i - col * rows) * rh;
            d_text(ctx, &sony_18, C_SUB, cx + 4, cy + 3, kw, LV_TEXT_ALIGN_LEFT, f[i].key);
            uint32_t vc = f[i].hl == DSC_HL_OK ? C_OK : (f[i].hl == DSC_HL_BAD ? C_BAD : C_TEXT);
            d_text(ctx, &sony_20, vc, vx, cy + 1, cx + colw - 8 - vx, LV_TEXT_ALIGN_LEFT, f[i].val);
        }
    }

    d_line(ctx, x1 + 4, ey - 4, x2 - 4, ey - 4, C_LINE, 1);
    if (qn) d_qr(ctx, x1 + 6, ey + (bh - qside) / 2, qn, qm);
    d_text_wrap(ctx, ef, C_TEXT, tx1, ey + (bh - eh) / 2, x2 - 10, els, ex, true);
}

/* ---- archive and frequency lists (ui_list) ----------------------------------- */

/* browser columns: time | flag icon from | flag icon to | telecommand */
#define BR_FROM_X   200
#define BR_NAME_W   165
#define BR_TO_X     (BR_FROM_X + BR_NAME_W + 78)

static void browser_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    if (index < 0 || index >= nbcalls) return;
    dsc_call_t *c = &bcalls[index];
    struct tm tm;
    gmtime_r(&c->utc, &tm);
    strftime(row->cell[0], UI_LIST_CELL_LEN, "%m-%d %H:%M", &tm);
    /* the name when it is known, else the MMSI (ui_list shortens what does
     * not fit the column with "..") */
    const char *nf = name_of(c->info.from_id), *nt = name_of(c->info.to_id);
    snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", nf ? nf : c->info.from_id);
    snprintf(row->cell[2], UI_LIST_CELL_LEN, "%s", nt ? nt : c->info.to_id);
    snprintf(row->cell[3], UI_LIST_CELL_LEN, "%s", c->info.tc1_name);
}

static void browser_draw_cb(void *user, int index, lv_draw_ctx_t *ctx, const lv_area_t *a, bool sel)
{
    (void)user; (void)sel;
    if (index < 0 || index >= nbcalls) return;
    dsc_call_t *c = &bcalls[index];
    lv_coord_t cy = (a->y1 + a->y2) / 2;
    /* category: a bar on the left edge; then for each station its flag and its icon */
    d_rect(ctx, a->x1 + 2, a->y1 + 3, a->x1 + 6, a->y2 - 3, cat_color(c->info.cat), 2, 0, 0);
    for (int i = 0; i < 2; ++i) {
        dsc_kind_t k = i == 0 ? c->info.from_kind : c->info.to_kind;
        lv_coord_t x = a->x1 + (i == 0 ? BR_FROM_X : BR_TO_X) - 70;    /* flag 36 + 6 + icon 21 + 7 */
        const lv_img_dsc_t *flag = station_flag(k, i == 0 ? c->info.from_id : c->info.to_id);
        if (flag) d_flag(ctx, flag, x, cy - (lv_coord_t)flag->header.h / 2);
        icon_kind(ctx, k, x + 42, cy - 9, 7, C_TEXT);
    }
}

static void browser_activate_cb(void *user, int index)
{
    (void)user;
    if (index < 0 || index >= nbcalls) return;
    detail_src = SRC_BROWSER;
    detail_idx = index;
    show_view(V_DETAIL);
}

static void freq_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;
    if (index < 0 || index >= DSC_NFREQS) return;
    const dsc_freq_t *f = &dsc_freqs[index];
    if (f->hz == 0) {                           /* section heading */
        snprintf(row->cell[0], UI_LIST_CELL_LEN, "%s", f->section);
        return;
    }
    char k[16];
    snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", f->band);
    fmt_khz(k, sizeof(k), f->hz);
    snprintf(row->cell[2], UI_LIST_CELL_LEN, "%s kHz", k);
    fmt_khz(k, sizeof(k), f->hz - 1000);
    snprintf(row->cell[3], UI_LIST_CELL_LEN, "dial %s USB", k);
    row->mark = (index == freq_current_row());
}

/* coloured bar under each section heading */
static void freq_draw_cb(void *user, int index, lv_draw_ctx_t *ctx, const lv_area_t *a, bool sel)
{
    (void)user; (void)sel;
    if (!freq_is_head(index)) return;
    uint32_t c = dsc_freqs[index].kind == SEC_DISTRESS ? C_DISTRESS
               : dsc_freqs[index].kind == SEC_WORLD ? C_SELECT : C_OK;
    d_rect(ctx, a->x1 + 4, a->y1 + 4, a->x1 + 9, a->y2 - 4, c, 2, 0, 0);
    d_rect(ctx, a->x1 + 14, a->y2 - 3, a->x2 - 14, a->y2 - 2, c, 0, 0, 0);
}

static void freq_tune_row(int index)
{
    if (index < 0 || index >= DSC_NFREQS || freq_is_head(index)) return;
    cparam_i_set(cfg.cur.mode(), x6100_mode_usb_dig);
    cparam_i_set(cfg.cur.fg_freq(), dsc_freqs[index].hz - 1000);
    freq_recalled = index;
    update_freq_label();
    char k[16];
    fmt_khz(k, sizeof(k), dsc_freqs[index].hz);
    /* the section a channel belongs to is the nearest heading above it */
    int h = index;
    while (h > 0 && !freq_is_head(h)) h--;
    /* update, not schedule: stepping fast replaces the text instead of queueing */
    msg_update_text_fmt("%s %s kHz", dsc_freqs[h].kind == SEC_DISTRESS ? "DISTRESS" : "DSC", k);
}

static void freq_activate_cb(void *user, int index)
{
    (void)user;
    if (freq_is_head(index)) return;            /* a heading: nothing to tune */
    freq_tune_row(index);
    show_view(V_LIST);
}

/*
 * Microphone UP (dir < 0) / DOWN (dir > 0) while DSC is open: previous /
 * next channel of the list, wrapping at both ends and skipping the
 * headings.  On a frequency that is not in the list (moved with the VFO):
 * the first channel of the list, 2187.5 kHz distress.
 */
bool dialog_dsc_mic_step(int dir)
{
    if (!dialog.run || dir == 0) return false;
    int row = freq_current_row();
    if (row < 0) {
        row = 0;
        while (freq_is_head(row)) row++;
    } else {
        int step = dir > 0 ? 1 : -1;
        do {
            row = (row + step + DSC_NFREQS) % DSC_NFREQS;
        } while (freq_is_head(row));
    }
    freq_tune_row(row);
    if (view == V_FREQ && freq_obj) {
        ui_list_select(freq_obj, row);
        lv_obj_invalidate(freq_obj);
    }
    return true;
}

/* ---- views ------------------------------------------------------------------------- */

static void focus_obj(lv_obj_t *obj)
{
    if (!obj) return;
    if (lv_obj_get_group(obj) != keyboard_group) lv_group_add_obj(keyboard_group, obj);
    lv_group_focus_obj(obj);
    lv_group_set_editing(keyboard_group, true);
}

static void unfocus_obj(lv_obj_t *obj)
{
    if (obj && lv_obj_get_group(obj)) lv_group_remove_obj(obj);
}

static lv_obj_t *make_list(const ui_list_cfg_t *cfg)
{
    lv_obj_t *l = ui_list_create(dialog.obj, cfg);
    lv_obj_center(l);
    lv_obj_add_event_cb(l, key_cb, LV_EVENT_KEY, NULL);       /* VOL */
    return l;
}

static void destroy_lists(void)
{
    if (browser_obj) { unfocus_obj(browser_obj); lv_obj_del(browser_obj); browser_obj = NULL; }
    if (freq_obj) { unfocus_obj(freq_obj); lv_obj_del(freq_obj); freq_obj = NULL; }
}

static void show_view(view_t v)
{
    if (!dialog.obj || !small_panel || !list_obj || !detail_obj) return;
    view_t old = view;
    view = v;

    unfocus_obj(list_obj);
    unfocus_obj(detail_obj);
    /* the browser stays alive while one of its calls is open */
    if (v == V_LIST) { destroy_lists(); browser_free(); }
    if (v != V_FREQ && old == V_FREQ && freq_obj) { unfocus_obj(freq_obj); lv_obj_del(freq_obj); freq_obj = NULL; }

    lv_obj_add_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(list_obj, LV_OBJ_FLAG_HIDDEN);
    if (browser_obj) lv_obj_add_flag(browser_obj, LV_OBJ_FLAG_HIDDEN);
    if (freq_obj) lv_obj_add_flag(freq_obj, LV_OBJ_FLAG_HIDDEN);

    bool small = (v == V_LIST) && small_size;
    if (small) {
        lv_obj_add_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(small_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(small_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
    }

    switch (v) {
    case V_LIST:
        if (small) {
            lv_obj_set_parent(list_obj, small_panel);
            lv_obj_update_layout(small_panel);
            lv_obj_set_size(list_obj, lv_obj_get_content_width(small_panel), lv_obj_get_content_height(small_panel));
        } else {
            lv_obj_set_parent(list_obj, dialog.obj);
            lv_obj_set_size(list_obj, BIG_W - 12, BIG_H - 12);
        }
        lv_obj_center(list_obj);
        lv_obj_clear_flag(list_obj, LV_OBJ_FLAG_HIDDEN);
        list_fix_top();
        lv_obj_invalidate(list_obj);
        focus_obj(list_obj);
        buttons_load_page(&page_list_1);
        break;

    case V_DETAIL:
        lv_obj_clear_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(detail_obj);
        focus_obj(detail_obj);
        buttons_load_page(&page_detail);
        break;

    case V_BROWSER:
        if (!browser_obj) {
            browser_load();
            ui_list_cfg_t cfg = {
                .width = BIG_W - 20, .height = BIG_H - 20, .font = &sony_22,
                .col_count = 4,
                .cols = {
                    { .x = 12,  .w = 110, .align = UI_LIST_ALIGN_LEFT, .muted = true },
                    { .x = BR_FROM_X, .w = BR_NAME_W, .align = UI_LIST_ALIGN_LEFT },
                    { .x = BR_TO_X,   .w = BR_NAME_W, .align = UI_LIST_ALIGN_LEFT },
                    { .x = 616, .w = BIG_W - 20 - 616 - 14, .align = UI_LIST_ALIGN_LEFT, .muted = true },
                },
                .row_cb = browser_row_cb,
                .activate_cb = browser_activate_cb,
                .draw_cb = browser_draw_cb,
                .empty_text = browser_empty,
            };
            browser_obj = make_list(&cfg);
            ui_list_set_count(browser_obj, nbcalls);
            ui_list_select(browser_obj, 0);
        } else {
            ui_list_set_count(browser_obj, nbcalls);
            if (old == V_DETAIL && detail_src == SRC_BROWSER && nbcalls > 0)
                ui_list_select(browser_obj, detail_idx < nbcalls ? detail_idx : nbcalls - 1);
        }
        lv_obj_clear_flag(browser_obj, LV_OBJ_FLAG_HIDDEN);
        focus_obj(browser_obj);
        buttons_load_page(&page_browser);
        break;

    case V_FREQ: {
        ui_list_cfg_t cfg = {
            .width = BIG_W - 20, .height = BIG_H - 24, .font = &sony_24,
            .col_count = 4,
            .cols = {
                { .x = 18,  .w = 730, .align = UI_LIST_ALIGN_LEFT },               /* heading */
                { .x = 44,  .w = 120, .align = UI_LIST_ALIGN_LEFT, .muted = true }, /* band */
                { .x = 180, .w = 220, .align = UI_LIST_ALIGN_LEFT },               /* kHz */
                { .x = 420, .w = 300, .align = UI_LIST_ALIGN_LEFT, .muted = true }, /* dial */
            },
            .row_cb = freq_row_cb,
            .activate_cb = freq_activate_cb,
            .draw_cb = freq_draw_cb,
            .empty_text = "",
            .vol_coarse = true,                 /* VOL: 10 rows at a time */
        };
        freq_obj = make_list(&cfg);
        ui_list_set_count(freq_obj, DSC_NFREQS);
        int cur = freq_current_row();
        ui_list_select(freq_obj, cur >= 0 ? cur : 1);
        focus_obj(freq_obj);
        buttons_load_page(&page_freq);
        break;
    }
    }
}

/* ---- incoming calls ------------------------------------------------------------- */

static void message_cb(const dsc_message_t *m, void *user)
{
    (void)user;
    pthread_mutex_lock(&fifo_mutex);
    if (fifo_count < DSC_FIFO_LEN) {
        fifo[(fifo_head + fifo_count) % DSC_FIFO_LEN] = *m;
        fifo_count++;
    }
    pthread_mutex_unlock(&fifo_mutex);
}

static void add_call(const dsc_message_t *m)
{
    if (ncalls == DSC_MAX_CALLS) {
        memmove(&calls[0], &calls[1], sizeof(dsc_call_t) * (DSC_MAX_CALLS - 1));
        ncalls--;
    }
    dsc_call_t *c = &calls[ncalls++];
    call_from_message(c, m, time(NULL), channel_hz());
    c->num = next_num++;
    link_reply(c);
    save_call(c);

    fprintf(stderr, "DSC: #%d %s %s %s -> %s, %s, ECC %s\n", c->num, c->info.cat_name, c->info.fmt_name,
            c->info.from_id, c->info.to_id, c->info.line2, c->info.ecc_ok ? "ok" : "not ok");

    /* keep the selected call where it is unless the newest one is selected
     * (a call hidden by the filter does not move anything) */
    if (call_shown(c)) {
        if (list_sel > 0) list_sel++;
        if (list_top > 0 || list_sel > 0) list_top += (list_sel > 0) ? 1 : 0;
        if (view == V_DETAIL && detail_src == SRC_LIVE) detail_idx++;
    }
    list_fix_top();

    /* a damaged call (error check not right) is only "possible" */
    const char *unv = c->info.ecc_ok ? "" : " (not verified)";
    if (c->info.self_cancel)
        msg_schedule_text_fmt("DSC distress CANCELLED by %s%s", c->info.from_id, unv);
    else if (c->info.cat == DSC_CAT_DISTRESS)
        msg_schedule_text_fmt("DSC DISTRESS from %s%s", c->info.from_id, unv);
    else if (c->info.cat == DSC_CAT_URGENCY)
        msg_schedule_text_fmt("DSC URGENCY from %s%s", c->info.from_id, unv);
}

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    dsc_message_t m;
    bool any = false;
    for (;;) {
        pthread_mutex_lock(&fifo_mutex);
        bool have = fifo_count > 0;
        if (have) { m = fifo[fifo_head]; fifo_head = (fifo_head + 1) % DSC_FIFO_LEN; fifo_count--; }
        pthread_mutex_unlock(&fifo_mutex);
        if (!have) break;
        add_call(&m);
        any = true;
    }
    if (test_done) {
        test_done = false;
        if (test_thread_valid) { pthread_join(test_thread, NULL); test_thread_valid = false; }
        test_mode = false;
        snprintf(test_label, sizeof(test_label), "Test\nWAV");
        if (btn_test.disp_btn) buttons_refresh(&btn_test);
        msg_schedule_text_fmt("DSC test finished");
        any = true;
    }
    static int32_t last_hz = 0;
    int32_t hz = channel_hz();
    if (hz != last_hz) { last_hz = hz; update_freq_label(); any = true; }
    if (any) {
        if (list_obj) lv_obj_invalidate(list_obj);
        if (view == V_DETAIL && detail_obj) lv_obj_invalidate(detail_obj);
    }
}

static void audio_cb(size_t n, float *samples)
{
    if (test_mode) return;
    pthread_mutex_lock(&decoder_mutex);
    if (decoder) dsc_decoder_process(decoder, samples, (int)n);
    pthread_mutex_unlock(&decoder_mutex);
}

/* ---- WAV test ------------------------------------------------------------------ */

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void *test_thread_cb(void *arg)
{
    (void)arg;
    FILE *f = fopen(DSC_TEST_WAV, "rb");
    unsigned char hdr[12], ch[8], fmt[16];
    bool ok = false;
    uint32_t left = 0;
    if (f && fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        bool fmt_ok = false;
        while (fread(ch, 1, 8, f) == 8) {
            uint32_t sz = rd32(ch + 4);
            if (!memcmp(ch, "fmt ", 4) && sz >= 16 && fread(fmt, 1, 16, f) == 16) {
                fmt_ok = rd16(fmt) == 1 && rd16(fmt + 2) == 1 && rd32(fmt + 4) == DSC_SAMPLE_RATE && rd16(fmt + 14) == 16;
                if (sz > 16) fseek(f, (long)(sz - 16), SEEK_CUR);
            } else if (!memcmp(ch, "data", 4)) {
                ok = fmt_ok;
                left = sz;
                break;
            } else {
                fseek(f, (long)sz, SEEK_CUR);
            }
            if (sz & 1) fseek(f, 1, SEEK_CUR);
        }
    }
    if (!ok) {
        fprintf(stderr, "DSC TEST: %s must be a mono 12000 Hz 16-bit PCM WAV\n", DSC_TEST_WAV);
    } else {
        int16_t pcm[DSC_TEST_CHUNK];
        float x[DSC_TEST_CHUNK];
        while (!test_stop && left >= 2) {
            size_t want = DSC_TEST_CHUNK;
            if (want > left / 2) want = left / 2;
            size_t n = fread(pcm, 2, want, f);
            if (n == 0) break;
            left -= (uint32_t)(n * 2);
            for (size_t i = 0; i < n; ++i) x[i] = (float)pcm[i] / 32768.0f;
            pthread_mutex_lock(&decoder_mutex);
            if (decoder) dsc_decoder_process(decoder, x, (int)n);
            pthread_mutex_unlock(&decoder_mutex);
            usleep((useconds_t)(1000000ULL * n / DSC_SAMPLE_RATE / 4));   /* 4x real time */
        }
        pthread_mutex_lock(&decoder_mutex);
        if (decoder) dsc_decoder_flush(decoder);
        pthread_mutex_unlock(&decoder_mutex);
    }
    if (f) fclose(f);
    test_done = true;
    return NULL;
}

static void stop_test(void)
{
    if (!test_thread_valid) { test_mode = false; return; }
    test_stop = true;
    pthread_join(test_thread, NULL);
    test_thread_valid = false;
    test_mode = false;
    test_done = false;
}

/* ---- buttons ------------------------------------------------------------------- */

static void page2_cb(button_data_t *d) { (void)d; buttons_load_page(&page_list_2); }
static void page1_cb(button_data_t *d) { (void)d; buttons_load_page(&page_list_1); }
static void freq_cb(button_data_t *d) { (void)d; show_view(V_FREQ); }

/* Microphone V/M while DSC is open: open / close the frequency list. */
bool dialog_dsc_vm_toggle(void)
{
    if (!dialog.run) return false;
    app_info_close();
    show_view(view == V_FREQ ? V_LIST : V_FREQ);
    return true;
}

static void size_cb(button_data_t *d)
{
    (void)d;
    small_size = !small_size;
    snprintf(size_label, sizeof(size_label), "View Size\n%s", small_size ? "Small" : "Big");
    show_view(V_LIST);
    if (btn_size.disp_btn) buttons_refresh(&btn_size);
}

/* the browser's text when it is empty, for the filter in use */
static void browser_empty_update(void)
{
    snprintf(browser_empty, sizeof(browser_empty), "%s",
             filter == FLT_ALL ? "No saved calls" : "No saved calls pass the filter");
}

static void filter_cb(button_data_t *d)
{
    (void)d;
    filter = (filter_t)((filter + 1) % FLT_N);
    filter_save();
    snprintf(filter_label, sizeof(filter_label), "%s", filter_labels[filter]);
    if (btn_filter.disp_btn) buttons_refresh(&btn_filter);
    list_sel = list_top = 0;
    list_fix_top();
    if (list_obj) lv_obj_invalidate(list_obj);
    browser_empty_update();
    /* the browser open: reload it with the new filter */
    if (view == V_BROWSER && browser_obj) {
        browser_load();
        ui_list_set_count(browser_obj, nbcalls);
        ui_list_select(browser_obj, 0);
        lv_obj_invalidate(browser_obj);
    }
}

static void details_cb(button_data_t *d)
{
    (void)d;
    if (live_count() == 0) { msg_schedule_text_fmt(ncalls ? "No calls pass the filter" : "No calls yet"); return; }
    detail_src = SRC_LIVE;
    detail_idx = list_sel;
    show_view(V_DETAIL);
}

static void browser_cb(button_data_t *d) { (void)d; show_view(V_BROWSER); }

static void clear_cb(button_data_t *d)
{
    (void)d;
    ncalls = 0;
    list_sel = list_top = 0;
    if (list_obj) lv_obj_invalidate(list_obj);
    msg_schedule_text_fmt("List cleared (saved calls are kept)");
}

static void test_cb(button_data_t *d)
{
    (void)d;
    if (test_mode) {
        stop_test();
        snprintf(test_label, sizeof(test_label), "Test\nWAV");
    } else {
        test_stop = false;
        test_done = false;
        test_mode = true;
        if (pthread_create(&test_thread, NULL, test_thread_cb, NULL) != 0) {
            test_mode = false;
            return;
        }
        test_thread_valid = true;
        snprintf(test_label, sizeof(test_label), "Stop\nTest");
    }
    if (btn_test.disp_btn) buttons_refresh(&btn_test);
    if (list_obj) lv_obj_invalidate(list_obj);
}

static void close_cb(button_data_t *d) { (void)d; dialog_destruct(); }

static void back_cb(button_data_t *d)
{
    (void)d;
    if (view == V_DETAIL && detail_src == SRC_BROWSER) { show_view(V_BROWSER); return; }
    if (view == V_BROWSER) browser_free();
    show_view(V_LIST);
}

static void detail_step(int dir)
{
    int n = detail_count(), i = detail_idx + dir;
    if (i < 0 || i >= n) return;
    detail_idx = i;
    if (detail_src == SRC_LIVE) { list_sel = i; list_fix_top(); }
    else if (browser_obj) ui_list_select(browser_obj, i);
    lv_obj_invalidate(detail_obj);
}

static void prev_cb(button_data_t *d) { (void)d; detail_step(-1); }
static void next_cb(button_data_t *d) { (void)d; detail_step(1); }

static void delete_at(src_t src, int idx)
{
    dsc_call_t *c = src == SRC_LIVE ? live_at(idx) : ((idx >= 0 && idx < nbcalls) ? &bcalls[idx] : NULL);
    if (!c) return;
    char file[64];
    snprintf(file, sizeof(file), "%s", c->file);
    if (file[0]) {
        char path[160];
        snprintf(path, sizeof(path), DSC_SAVE_DIR "/%s", file);
        if (unlink(path) != 0) fprintf(stderr, "DSC: cannot delete %s\n", path);
    }
    /* drop it from both lists */
    for (int i = 0; i < ncalls; ++i)
        if (file[0] ? !strcmp(calls[i].file, file) : (&calls[i] == c)) {
            memmove(&calls[i], &calls[i + 1], sizeof(dsc_call_t) * (size_t)(ncalls - i - 1));
            ncalls--;
            break;
        }
    for (int i = 0; i < nbcalls; ++i)
        if (file[0] ? !strcmp(bcalls[i].file, file) : (&bcalls[i] == c)) {
            memmove(&bcalls[i], &bcalls[i + 1], sizeof(dsc_call_t) * (size_t)(nbcalls - i - 1));
            nbcalls--;
            break;
        }
    msg_schedule_text_fmt("Call deleted");
}

static void delete_cb(button_data_t *d)
{
    (void)d;
    if (view == V_DETAIL) {
        delete_at(detail_src, detail_idx);
        int n = detail_count();
        if (n == 0) { back_cb(NULL); return; }
        if (detail_idx >= n) detail_idx = n - 1;
        if (detail_src == SRC_LIVE) { list_sel = detail_idx; list_fix_top(); }
        else if (browser_obj) { ui_list_set_count(browser_obj, nbcalls); ui_list_select(browser_obj, detail_idx); }
        lv_obj_invalidate(detail_obj);
    } else if (view == V_BROWSER && browser_obj) {
        int i = ui_list_get_selected(browser_obj);
        if (i < 0) return;
        delete_at(SRC_BROWSER, i);
        ui_list_set_count(browser_obj, nbcalls);
        lv_obj_invalidate(browser_obj);
    }
    if (list_obj) lv_obj_invalidate(list_obj);
}

static void open_cb(button_data_t *d)
{
    (void)d;
    if (!browser_obj) return;
    browser_activate_cb(NULL, ui_list_get_selected(browser_obj));
}

static void tune_cb(button_data_t *d)
{
    (void)d;
    if (!freq_obj) return;
    freq_activate_cb(NULL, ui_list_get_selected(freq_obj));
}

/* ---- keys ------------------------------------------------------------------------- */

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    /* in the frequency list the VOL knob scrolls 10 rows (ui_list) */
    if (view == V_FREQ && (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT ||
                           key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT))
        return;

    switch (key) {
    case KEY_VOL_LEFT_EDIT:
    case KEY_VOL_LEFT_SELECT:
        radio_change_vol(-1);
        return;
    case KEY_VOL_RIGHT_EDIT:
    case KEY_VOL_RIGHT_SELECT:
        radio_change_vol(1);
        return;
    default:
        break;
    }

    if (view == V_LIST) {
        if (key == LV_KEY_LEFT && list_sel > 0) list_sel--;
        else if (key == LV_KEY_RIGHT && list_sel < live_count() - 1) list_sel++;
        else return;
        list_fix_top();
        lv_obj_invalidate(list_obj);
    } else if (view == V_DETAIL) {
        if (key == LV_KEY_LEFT) detail_step(-1);
        else if (key == LV_KEY_RIGHT) detail_step(1);
    }
}

static void list_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (view == V_LIST) details_cb(NULL);
}

static void detail_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (view == V_DETAIL) back_cb(NULL);
}

/* ---- construct / destruct ---------------------------------------------------- */

static void construct_cb(lv_obj_t *parent)
{
    app_radio_begin();      /* 1KO125: USB-D locked, zoom 4x, DSP off while open */
    spectrum_set_markers(915, 1085);   /* tuning markers: 1000 Hz -/+ 85 Hz */
    parent_obj = parent;
    names_load();
    dsc_ships_open();
    dsc_flags_open();

    pthread_mutex_lock(&decoder_mutex);
    if (!decoder) decoder = dsc_decoder_create(DSC_SAMPLE_RATE, message_cb, NULL);
    pthread_mutex_unlock(&decoder_mutex);
    if (!decoder) fprintf(stderr, "DSC: unable to create the decoder\n");

    if (dsp_audio_sub_id == AUDIO_SUB_INVALID)
        dsp_audio_sub_id = dsp_audio_subscribe_float(audio_cb, DSC_SAMPLE_RATE);
    if (dsp_audio_sub_id != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_audio_sub_id, true);

    /* Big: the WEFAX/SSTV dialog frame */
    dialog.obj = dialog_init(parent);
    lv_obj_set_size(dialog.obj, BIG_W, BIG_H);
    lv_obj_center(dialog.obj);
    lv_obj_set_style_translate_y(dialog.obj, 4, 0);
    lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_SCROLLABLE);

    /* Small: the RTTY/NAVTEX panel above the buttons */
    small_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(small_panel);
    lv_obj_add_style(small_panel, &style.panels.base, 0);
    lv_obj_set_style_pad_all(small_panel, 6, 0);
    lv_obj_clear_flag(small_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(small_panel, LV_OBJ_FLAG_HIDDEN);
    filter_load();
    browser_empty_update();
    snprintf(filter_label, sizeof(filter_label), "%s", filter_labels[filter]);

    list_obj = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(list_obj);
    lv_obj_clear_flag(list_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(list_obj, list_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(list_obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(list_obj, list_pressed_cb, LV_EVENT_PRESSED, NULL);

    detail_obj = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(detail_obj);
    lv_obj_set_size(detail_obj, BIG_W - 8, BIG_H - 8);
    lv_obj_center(detail_obj);
    lv_obj_clear_flag(detail_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(detail_obj, detail_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(detail_obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(detail_obj, detail_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);

    snprintf(size_label, sizeof(size_label), "View Size\n%s", small_size ? "Small" : "Big");
    snprintf(test_label, sizeof(test_label), "Test\nWAV");
    update_freq_label();
    list_sel = 0;
    list_top = 0;
    view = V_LIST;
    show_view(V_LIST);

    ui_timer = lv_timer_create(ui_timer_cb, DSC_UI_TIMER_MS, NULL);
    fprintf(stderr, "DSC: started\n");
}

static void destruct_cb(void)
{
    app_radio_end();        /* 1KO125: mode, zoom and DSP of before */
    spectrum_clear_markers();
    app_info_abort();
    if (dsp_audio_sub_id != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_audio_sub_id, false);
    stop_test();
    if (ui_timer) { lv_timer_del(ui_timer); ui_timer = NULL; }

    destroy_lists();
    browser_free();
    unfocus_obj(list_obj);
    unfocus_obj(detail_obj);
    if (small_panel) {
        /* the list may be inside the small panel: give it back to the dialog */
        if (list_obj && dialog.obj) lv_obj_set_parent(list_obj, dialog.obj);
        lv_obj_del(small_panel);
        small_panel = NULL;
    }
    list_obj = NULL;
    detail_obj = NULL;

    pthread_mutex_lock(&decoder_mutex);
    if (decoder) { dsc_decoder_destroy(decoder); decoder = NULL; }
    pthread_mutex_unlock(&decoder_mutex);

    pthread_mutex_lock(&fifo_mutex);
    fifo_head = fifo_count = 0;
    pthread_mutex_unlock(&fifo_mutex);

    /* names are read again at the next opening (the file may have changed) */
    free(names); names = NULL;
    free(names_buf); names_buf = NULL;
    nnames = 0;
    names_loaded = false;
    dsc_ships_close();
    dsc_flags_close();

    parent_obj = NULL;
    fprintf(stderr, "DSC: closed\n");
}
