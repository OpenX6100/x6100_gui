/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 - NAVTEX / SITOR-B scan ("Scan"), included by dialog_navtex.c.
 *
 * The frequencies of the list ("Freq.", dialog_navtex_freq.inc) up to the
 * highest, each once and in order of frequency (fewer clicks of the band
 * relays), then again from the lowest: from the lowest (490 kHz) the first
 * time after the app opens and on a long press of Scan; otherwise on from
 * the frequency where it stopped.  On each one:
 *
 *   1. Wait: the time the radio needs after a frequency change (relays,
 *      audio, spectrum); 0.5 to 1.5 s, chosen with the "Wait" button;
 *   2. 0.5 s of audio, its spectrum from 550 to 1450 Hz in 25 Hz steps:
 *      two tones 150-175 Hz apart (SITOR-B: 170 Hz) near 1000 Hz (AFC
 *      range included), both 3 dB above the median of the spectrum; or one
 *      tone 10 dB over it and the other present (a run of one tone).  A
 *      broad signal (radar, noise, pulses) is flat there.  The dip between
 *      the tones is only logged: real signals often have none (a weak
 *      station of 2026-11-04, +6 dB tones, failed on it one time in five).  A broad signal (radar, noise,
 *      pulses) is flat there and does not pass.  If not, on to the next
 *      frequency;
 *   3. if so, up to 8 s for the decoder to lock, then up to 15 s for
 *      SCAN_GOOD characters received correctly (clean or from the repeated
 *      copy), at least twice as many as the lost or guessed ones.  A
 *      station sending only phasing or idle keeps the decoder locked with
 *      nothing to print: if no message starts within 15 s, on to the next
 *      frequency.  A lock on something that only looks like SITOR-B gives
 *      random characters.  A lock lost in a fade may come back within 3 s.
 * Checked with tests/navtex/scan_sim on recordings of 518 kHz and of
 * Istanbul (shifted to 1000 Hz), arriving every 5 s: every arrival during
 * a transmission stops on the station.
 *
 * The scan stops on such a signal, when the frequency is changed by hand,
 * or when Scan is pressed again; after the highest frequency it goes on
 * from the lowest.  Once stopped it never starts again by itself.
 */

#define SCAN_MEASURE_MS     500
#define SCAN_LOCK_MS        8000    /* for the decoder to lock */
#define SCAN_TEXT_MS        15000   /* after the lock, for good characters (the
                                     * decoder prints them 1.5 s late); a station
                                     * only phasing or idle this long: on */
#define SCAN_RELOCK_MS      3000    /* a lock lost: this long to lock again */
#define SCAN_GOOD           12
/* The weaker tone over the median of 550-1450 Hz: SITOR-B at -8 dB SNR
 * (2500 Hz, synthetic) +4.3 dB, a weak real station +5 to +7 dB; plain
 * noise at most +1.9 dB in 0.5 s, coloured noise and broadband pulses at
 * most +1.6 dB. */
#define SCAN_TONE_DB        3.0f
#define SCAN_ONE_DB         10.0f   /* one tone this strong, the other present:
                                     * a run of one tone (phasing); plain noise
                                     * reaches +3 dB at most */
#define SCAN_FILE           NAVTEX_SAVE_DIR "/scan.txt"

/* Goertzel blocks of 20 ms at 12000 Hz: bins 50 Hz wide, every 25 Hz */
#define SCAN_BLOCK          240
#define SCAN_F0             550
#define SCAN_BIN_STEP       25
#define SCAN_BINS           37      /* 550 ... 1450 Hz */
#define SCAN_LOW_MIN        850     /* the lower tone: 1000 - 85 -/+ 65 Hz */
#define SCAN_LOW_MAX        1000

typedef enum { SCAN_OFF, SCAN_SETTLE, SCAN_MEASURE, SCAN_CONFIRM, SCAN_TEXT } scan_state_t;

static const uint16_t scan_wait_ms[] = { 500, 700, 1000, 1200, 1500 };
#define SCAN_WAITS ((int)(sizeof(scan_wait_ms) / sizeof(scan_wait_ms[0])))

static scan_state_t scan_state = SCAN_OFF;
static int scan_wait = 1;                   /* index in scan_wait_ms */
static bool scan_wait_loaded = false;
static uint32_t scan_since;                 /* lv_tick of the current state */
static int32_t scan_dial;                   /* the dial the scan set */
static int32_t scan_hz;                     /* channel being looked at */
static bool scan_resume = false;            /* the next Scan goes on from scan_hz */
static char scan_btn_label[16] = "Scan";
static char scan_wait_label[16] = "Wait\n0.7 s";

/* from the decoder (DSP thread) */
static volatile bool scan_synced = false;
static volatile bool scan_unsynced = false;
static uint32_t scan_unsync_since;          /* lv_tick when the lock was lost */
static bool scan_unsync_seen = false;
static volatile int scan_good = 0, scan_bad = 0;   /* characters since the lock */

/* tone measurement, filled by scan_audio() in the DSP thread */
static pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool scan_measuring = false;
static float scan_coef[SCAN_BINS];
static bool scan_coef_ok = false;
static float scan_s1[SCAN_BINS], scan_s2[SCAN_BINS];
static int scan_block_n = 0;
static double scan_pow[SCAN_BINS];
static int scan_blocks = 0;

static void scan_cb(button_data_t *d);
static void scan_hold_cb(button_data_t *d);
static void scan_wait_cb(button_data_t *d);

static button_data_t btn_scan = { .type = BTN_TEXT, .label = scan_btn_label, .press = scan_cb,
                               .hold = scan_hold_cb };
static button_data_t btn_scan_wait = { .type = BTN_TEXT, .label = scan_wait_label, .press = scan_wait_cb };

/* ---- settings ------------------------------------------------------------------- */

static void scan_wait_update(void)
{
    unsigned ms = scan_wait_ms[scan_wait];
    snprintf(scan_wait_label, sizeof(scan_wait_label), "Wait\n%u.%u s", ms / 1000, (ms % 1000) / 100);
    if (btn_scan_wait.disp_btn) buttons_refresh(&btn_scan_wait);
}

static void scan_wait_load(void)
{
    if (scan_wait_loaded) return;
    scan_wait_loaded = true;
    FILE *f = fopen(SCAN_FILE, "r");
    if (f) {
        unsigned ms;
        if (fscanf(f, "wait_ms %u", &ms) == 1)
            for (int i = 0; i < SCAN_WAITS; ++i)
                if (scan_wait_ms[i] == ms) scan_wait = i;
        fclose(f);
    }
    scan_wait_update();
}

static void scan_wait_save(void)
{
    if (!nav_ensure_dir()) return;
    FILE *f = fopen(SCAN_FILE, "w");
    if (!f) return;
    fprintf(f, "wait_ms %u\n", (unsigned)scan_wait_ms[scan_wait]);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void scan_wait_cb(button_data_t *d)
{
    (void)d;
    scan_wait = (scan_wait + 1) % SCAN_WAITS;
    scan_wait_update();
    scan_wait_save();
    msg_update_text_fmt("Wait %u ms", (unsigned)scan_wait_ms[scan_wait]);
}

/* ---- the tones (DSP thread) ------------------------------------------------------ */

static void scan_bins_init(void)
{
    if (scan_coef_ok) return;
    for (int b = 0; b < SCAN_BINS; ++b)
        scan_coef[b] = 2.0f * cosf(2.0f * (float)M_PI * (SCAN_F0 + b * SCAN_BIN_STEP) / NAVTEX_SAMPLE_RATE);
    scan_coef_ok = true;
}

static void scan_measure_start(void)
{
    pthread_mutex_lock(&scan_mutex);
    memset(scan_s1, 0, sizeof(scan_s1));
    memset(scan_s2, 0, sizeof(scan_s2));
    memset(scan_pow, 0, sizeof(scan_pow));
    scan_block_n = 0;
    scan_blocks = 0;
    scan_measuring = true;
    pthread_mutex_unlock(&scan_mutex);
}

static int scan_cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The best pair of tones: the weaker of the two over the median (*tone_db)
 * and over the middle between them (*dip_db).  false: too little audio. */
static bool scan_measure_end(float *tone_db, float *dip_db, int *low_hz, float *one_db)
{
    double p[SCAN_BINS], sorted[SCAN_BINS];
    int blocks;

    pthread_mutex_lock(&scan_mutex);
    scan_measuring = false;
    blocks = scan_blocks;
    memcpy(p, scan_pow, sizeof(p));
    pthread_mutex_unlock(&scan_mutex);
    if (blocks < 5) return false;

    memcpy(sorted, p, sizeof(sorted));
    qsort(sorted, SCAN_BINS, sizeof(double), scan_cmp_d);
    double med = sorted[SCAN_BINS / 2] + 1e-20;

    *tone_db = -99.0f;
    *one_db = -99.0f;
    *dip_db = 0.0f;
    *low_hz = 0;
    for (int f = SCAN_LOW_MIN; f <= SCAN_LOW_MAX; f += SCAN_BIN_STEP) {
        int l = (f - SCAN_F0) / SCAN_BIN_STEP;
        int h = l + 6;                      /* +150 Hz; +175 Hz is h + 1 */
        double hi = p[h] > p[h + 1] ? p[h] : p[h + 1];
        double weak = p[l] < hi ? p[l] : hi;
        double mid = (p[l + 3] + p[l + 4]) / 2.0 + 1e-20;   /* +75, +100 Hz */
        float t = 10.0f * log10f((float)(weak / med));
        float o = 10.0f * log10f((float)((p[l] > hi ? p[l] : hi) / med));
        if (o > *one_db) *one_db = o;
        if (t > *tone_db) {
            *tone_db = t;
            *dip_db = 10.0f * log10f((float)(weak / mid));
            *low_hz = f;
        }
    }
    return true;
}

static void scan_audio(const float *samples, size_t n)
{
    if (!scan_measuring) return;            /* read without the lock: a hint */
    pthread_mutex_lock(&scan_mutex);
    if (scan_measuring) {
        for (size_t i = 0; i < n; ++i) {
            float x = samples[i];
            for (int b = 0; b < SCAN_BINS; ++b) {
                float s = x + scan_coef[b] * scan_s1[b] - scan_s2[b];
                scan_s2[b] = scan_s1[b];
                scan_s1[b] = s;
            }
            if (++scan_block_n == SCAN_BLOCK) {
                for (int b = 0; b < SCAN_BINS; ++b) {
                    scan_pow[b] += scan_s1[b] * scan_s1[b] + scan_s2[b] * scan_s2[b] -
                                   scan_coef[b] * scan_s1[b] * scan_s2[b];
                    scan_s1[b] = scan_s2[b] = 0.0f;
                }
                scan_block_n = 0;
                scan_blocks++;
            }
        }
    }
    pthread_mutex_unlock(&scan_mutex);
}

/* the decoder's characters and events: a lock is what the scan looks for */
static void scan_q_cb(int c, int quality, void *user)
{
    if (quality == NAVTEX_Q_EVENT) {
        if (c == NAVTEX_EV_SYNC_ON) {
            scan_synced = true;
            scan_unsynced = false;
        } else if (c == NAVTEX_EV_SYNC_OFF) {
            scan_unsynced = true;
        }
    } else if (quality == NAVTEX_Q_CLEAN || quality == NAVTEX_Q_REP) {
        scan_good++;
    } else {
        scan_bad++;
    }
    nav_char_q_cb(c, quality, user);
}

/* ---- the scan (LVGL thread) ------------------------------------------------------ */

static void scan_set_button(void)
{
    snprintf(scan_btn_label, sizeof(scan_btn_label), "%s", scan_state == SCAN_OFF ? "Scan" : "Stop\nScan");
    if (btn_scan.disp_btn) buttons_refresh(&btn_scan);
}

static bool scan_running(void)
{
    return scan_state != SCAN_OFF;
}

static void scan_stop(const char *why)
{
    if (scan_state == SCAN_OFF) return;
    pthread_mutex_lock(&scan_mutex);
    scan_measuring = false;
    pthread_mutex_unlock(&scan_mutex);
    scan_state = SCAN_OFF;
    scan_set_button();
    if (why) msg_update_text_fmt("%s", why);
}

/* the next frequency of the list above hz, 0 if none; its first row */
static int32_t scan_next_hz(int32_t hz, int *row)
{
    int32_t best = 0;
    for (int i = 0; i < NF_N; ++i) {
        int32_t f = nf_freqs[i].hz;
        if (f > hz && (!best || f < best)) {
            best = f;
            *row = i;
        }
    }
    return best;
}

/* on to the next frequency, after the highest the lowest; false only
 * with an empty list */
static bool scan_step(void)
{
    int row = -1;
    int32_t hz = scan_next_hz(scan_hz, &row);
    if (!hz) {
        hz = scan_next_hz(0, &row);         /* end of the list: from the start */
        if (!hz) return false;
        fprintf(stderr, "NAVTEX SCAN: end of the list, again from the lowest frequency\n");
    }
    scan_hz = hz;
    /* a new decoder for each frequency: its lock (SYNC_ON) is then surely
     * a lock on this one, also when it comes during the wait (a strong
     * station locks in a fraction of a second) */
    nav_flush();                            /* a bulletin being received is closed */
    reset_decoder();
    scan_synced = false;
    scan_unsynced = false;
    nf_tune_row(row);                       /* USB-D, dial, its name on the screen */
    scan_dial = cparam_i_get(cfg.cur.fg_freq());
    scan_state = SCAN_SETTLE;
    scan_since = lv_tick_get();
    return true;
}

static void scan_start(void)
{
    scan_wait_load();
    scan_bins_init();
    if (!scan_resume)
        scan_hz = 0;                        /* from the lowest frequency */
    if (!scan_step())
        return;
    scan_set_button();
}

static void scan_cb(button_data_t *d)
{
    (void)d;
    if (scan_state != SCAN_OFF) {
        scan_stop("Scan stopped");
        scan_resume = true;
        return;
    }
    if (nf_frame) nf_close();               /* started from the list: show the text */
    scan_start();
}

/* long press: a new scan from the lowest frequency, also during a scan */
static void scan_hold_cb(button_data_t *d)
{
    (void)d;
    scan_stop(NULL);
    scan_resume = false;
    if (nf_frame) nf_close();
    scan_start();
}

/* every UI tick */
static void scan_poll(void)
{
    if (scan_state == SCAN_OFF) return;

    /* a frequency change not made by the scan ends it */
    if (cparam_i_get(cfg.cur.fg_freq()) != scan_dial) {
        scan_stop("Scan stopped: frequency changed");
        scan_resume = true;
        return;
    }

    uint32_t t = lv_tick_elaps(scan_since);
    char k[16];
    nf_fmt_khz(k, sizeof(k), scan_hz);

    /* a lock on this frequency, also during the wait: the characters decide */
    if (scan_state != SCAN_TEXT && scan_synced) {
        scan_unsynced = false;
        scan_unsync_seen = false;
        scan_good = scan_bad = 0;
        scan_state = SCAN_TEXT;
        scan_since = lv_tick_get();
        t = 0;
    }

    switch (scan_state) {
        case SCAN_SETTLE:
            if (t >= scan_wait_ms[scan_wait]) {
                scan_measure_start();
                scan_state = SCAN_MEASURE;
                scan_since = lv_tick_get();
            }
            break;

        case SCAN_MEASURE:
            if (t >= SCAN_MEASURE_MS) {
                float tone = -99.0f, dip = 0.0f, one = -99.0f;
                int low = 0;
                bool ok = scan_measure_end(&tone, &dip, &low, &one);
                fprintf(stderr, "NAVTEX SCAN: %s kHz tones %d/%d Hz %+.1f dB (stronger %+.1f), dip %+.1f dB%s\n",
                        k, low, low + 170, tone, one, dip, ok ? "" : " (no audio)");
                if (ok && (tone >= SCAN_TONE_DB || (one >= SCAN_ONE_DB && tone >= 0.0f))) {
                    scan_state = SCAN_CONFIRM;
                    scan_since = lv_tick_get();
                    msg_update_text_fmt("Awaiting lock at %s kHz", k);
                } else if (!scan_step()) {
                    scan_stop("No frequency in the list");
                    scan_resume = false;
                }
            }
            break;

        case SCAN_CONFIRM:
            if (t >= SCAN_LOCK_MS && !scan_step()) {
                scan_stop("No frequency in the list");
                scan_resume = false;
            }
            break;

        case SCAN_TEXT: {
            int good = scan_good, bad = scan_bad;
            if (good >= SCAN_GOOD && bad * 2 <= good) {
                fprintf(stderr, "NAVTEX SCAN: %s kHz locked, %d good / %d lost characters\n", k, good, bad);
                scan_stop(NULL);
                scan_resume = true;         /* the next Scan goes on from here */
                msg_update_text_fmt("SITOR-B signal on %s kHz", k);
                break;
            }
            /* a lost lock: on only if it does not come back */
            bool gone = false;
            if (scan_unsynced) {
                if (!scan_unsync_seen) {
                    scan_unsync_seen = true;
                    scan_unsync_since = lv_tick_get();
                }
                gone = lv_tick_elaps(scan_unsync_since) >= SCAN_RELOCK_MS;
            } else {
                scan_unsync_seen = false;
            }
            if (gone || t >= SCAN_TEXT_MS || (bad >= 2 * SCAN_GOOD && bad > good)) {
                fprintf(stderr, "NAVTEX SCAN: %s kHz lock without text (%d good / %d lost), on\n",
                        k, good, bad);
                if (!scan_step()) {
                    scan_stop("No frequency in the list");
                    scan_resume = false;
                }
            }
            break;
        }

        default:
            break;
    }
}
