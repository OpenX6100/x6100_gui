/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 - WEFAX automatic slant (Tilt), included by dialog_wefax.c.
 *
 * 1. Tilt memory per station, always active (also with Auto Slant OFF).
 *    Stored for the station the radio is tuned to every time the user
 *    changes the Tilt, and when a measurement is applied (see 2), in
 *    FAX_SAVE_DIR/slant.txt.  Recalled when the radio is tuned to a station
 *    of the list and when a fax starts in Auto RX.  A station of the list
 *    without memory gets Tilt 0; a frequency not in the list leaves the
 *    Tilt as it is.
 *
 * 2. Auto Slant (button, ON by default), Auto RX only: the slant measured
 *    on the phasing band (wefax/decoder.c) replaces the memory only when the
 *    station has none, or when it clearly disagrees with it (more than three
 *    standard errors, at least SLANT_DISAGREE units).  On HF the phasing is
 *    good to about +/-30 units: the memory, corrected by hand once, is the
 *    exact value, the measurement only catches a wrong or missing one.
 *
 * If the user touches the Tilt during a fax, the user's value wins for the
 * rest of that fax.
 */

#include <unistd.h>

#define SLANT_FILE          FAX_SAVE_DIR "/slant.txt"
#define SLANT_MAX_STATIONS  64
#define SLANT_KEY_LEN       96
#define SLANT_SAVE_DELAY_MS 3000
#define SLANT_MATCH_HZ      1000    /* VFO near a listed frequency */
#define SLANT_DISAGREE      50      /* Tilt units, minimum to replace the memory */

typedef struct {
    char key[SLANT_KEY_LEN];        /* "country|place" */
    int  tilt;
} slant_station_t;

static slant_station_t slant_st[SLANT_MAX_STATIONS];
static int slant_nst = 0;
static bool slant_auto = true;          /* Auto Slant ON/OFF */
static bool slant_user_locked = false;  /* user changed the Tilt during this fax */
static bool slant_dirty = false;
static uint32_t slant_dirty_tick = 0;
static int32_t slant_last_dial = 0;     /* VFO seen by the last station check */
static char slant_last_key[SLANT_KEY_LEN] = "";

/* ---- station of the current tuning ---------------------------------------- */

static bool slant_station_key(char *key, size_t sz)
{
    if (wefax_db_load() == 0) return false;

    int row = -1;
    if (freq_recalled_row >= 0 && cparam_i_get(cfg.cur.fg_freq()) == freq_recalled_dial)
        row = freq_recalled_row;

    if (row < 0) {
        int32_t hz = freq_current_hz();
        int32_t best = SLANT_MATCH_HZ + 1;
        for (int i = 0; i < wefax_db_rows(); ++i) {
            int32_t d = wefax_db_row(i)->hz - hz;
            if (d < 0) d = -d;
            if (d < best) { best = d; row = i; }
        }
    }

    const wefax_row_t *r = wefax_db_row(row);
    if (!r) return false;
    snprintf(key, sz, "%s|%s", r->country, r->place);
    return true;
}

static slant_station_t *slant_find(const char *key)
{
    for (int i = 0; i < slant_nst; ++i)
        if (strcmp(slant_st[i].key, key) == 0) return &slant_st[i];
    return NULL;
}

/* ---- file ------------------------------------------------------------------ */

static void slant_load(void)
{
    slant_nst = 0;
    slant_auto = true;              /* default, the file may say otherwise */
    slant_last_dial = 0;            /* recall the memory of the current station */
    slant_last_key[0] = '\0';
    FILE *fp = fopen(SLANT_FILE, "r");
    if (!fp) {
        fprintf(stderr, "WEFAX SLANT: Auto Slant ON, no memory file yet (%s)\n", SLANT_FILE);
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;
        if (strncmp(line, "auto ", 5) == 0) {
            slant_auto = atoi(line + 5) != 0;
            continue;
        }
        char *bar = strrchr(line, '|');
        if (!bar || bar == line || slant_nst >= SLANT_MAX_STATIONS) continue;
        *bar = '\0';
        int t = atoi(bar + 1);
        if (t < -400 || t > 400) continue;
        snprintf(slant_st[slant_nst].key, SLANT_KEY_LEN, "%s", line);
        slant_st[slant_nst].tilt = t;
        slant_nst++;
    }
    fclose(fp);
    fprintf(stderr, "WEFAX SLANT: Auto Slant %s, %d stations in %s\n",
           slant_auto ? "ON" : "OFF", slant_nst, SLANT_FILE);
}

static void slant_save(void)
{
    slant_dirty = false;
    mkdir(FAX_SAVE_DIR, 0755);
    FILE *fp = fopen(SLANT_FILE ".tmp", "w");
    if (!fp) {
        fprintf(stderr, "WEFAX SLANT: cannot write %s (%s)\n", SLANT_FILE, strerror(errno));
        return;
    }
    fprintf(fp, "# 1KO125 WEFAX: Auto Slant and Tilt per station (country|place|tilt)\n");
    fprintf(fp, "auto %d\n", slant_auto ? 1 : 0);
    for (int i = 0; i < slant_nst; ++i)
        fprintf(fp, "%s|%d\n", slant_st[i].key, slant_st[i].tilt);
    fflush(fp);
    fsync(fileno(fp));      /* the radio may be switched off right after */
    fclose(fp);
    if (rename(SLANT_FILE ".tmp", SLANT_FILE) != 0)
        fprintf(stderr, "WEFAX SLANT: cannot write %s (%s)\n", SLANT_FILE, strerror(errno));
    else
        fprintf(stderr, "WEFAX SLANT: %d stations saved in %s\n", slant_nst, SLANT_FILE);
}

static void slant_mark_dirty(void)
{
    slant_dirty = true;
    slant_dirty_tick = lv_tick_get();
}

/* store the Tilt for the station the radio is tuned to */
static void slant_store(int tilt)
{
    char key[SLANT_KEY_LEN];
    if (!slant_station_key(key, sizeof(key))) {
        fprintf(stderr, "WEFAX SLANT: Tilt %+d not stored - %ld Hz is not a station of the list\n",
               tilt, (long)freq_current_hz());
        return;
    }
    slant_station_t *s = slant_find(key);
    if (!s) {
        if (slant_nst >= SLANT_MAX_STATIONS) return;
        s = &slant_st[slant_nst++];
        snprintf(s->key, SLANT_KEY_LEN, "%s", key);
    } else if (s->tilt == tilt) {
        return;
    }
    s->tilt = tilt;
    slant_mark_dirty();
    fprintf(stderr, "WEFAX SLANT: Tilt %+d stored for %s\n", tilt, key);
}

/* ---- Tilt changes ------------------------------------------------------------ */

static bool slant_auto_fax_running(void)
{
    return decoder && wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO && auto_fax_confirmed_seen;
}

static void slant_set_tilt(int tilt)
{
    wefax_tilt = tilt;
    if (btn_tilt.disp_btn) buttons_refresh(&btn_tilt);
    if (!fax_browser_open && fax_canvas) rerender_preview();
}

/* the user turned the Tilt (MFK) */
static void slant_user_tilt_changed(void)
{
    if (slant_auto_fax_running()) slant_user_locked = true;
    slant_store(wefax_tilt);
}

/*
 * An AUTO fax has started (or restarted).  Returns true when it showed the
 * "Fax started" message itself.
 */
static bool slant_fax_started(void)
{
    slant_user_locked = false;

    char key[SLANT_KEY_LEN];
    if (!slant_station_key(key, sizeof(key))) return false;     /* not in the list: unchanged */
    slant_station_t *s = slant_find(key);
    if (!s) {
        if (wefax_tilt != 0) slant_set_tilt(0);                 /* no memory: Tilt 0 */
        return false;
    }

    slant_set_tilt(s->tilt);
    fprintf(stderr, "WEFAX SLANT: fax started, Tilt %+d from the memory of %s\n", s->tilt, key);
    msg_schedule_text_fmt("Fax started - Tilt %+d (memory)", s->tilt);
    return true;
}

/* the radio was tuned to another station of the list: recall its Tilt */
static void slant_check_station(void)
{
    int32_t dial = cparam_i_get(cfg.cur.fg_freq());
    if (dial == slant_last_dial) return;
    slant_last_dial = dial;

    char key[SLANT_KEY_LEN];
    if (!slant_station_key(key, sizeof(key))) {
        slant_last_key[0] = '\0';
        return;
    }
    if (strcmp(key, slant_last_key) == 0) return;
    snprintf(slant_last_key, sizeof(slant_last_key), "%s", key);

    /* a station of the list without memory starts from Tilt 0 */
    slant_station_t *s = slant_find(key);
    int tilt = s ? s->tilt : 0;
    if (tilt != wefax_tilt) {
        fprintf(stderr, "WEFAX SLANT: tuned to %s, Tilt %+d %s\n", key, tilt,
                s ? "from the memory" : "(no memory)");
        slant_set_tilt(tilt);
    }
}

/* a slant measured on the phasing of an Auto RX fax */
static void slant_measured(float px, float se)
{
    /* decoder: px per decoder row; Tilt: 1/1000 px per preview row */
    const float k = -1000.0f * (float)WEFAX_WIDTH / (float)PREVIEW_WIDTH;
    float t = px * k;
    int tilt = (int)(t >= 0 ? t / 10.0f + 0.5f : t / 10.0f - 0.5f) * 10;
    if (tilt > 400) tilt = 400;
    if (tilt < -400) tilt = -400;
    float limit = 3.0f * se * -k;
    if (limit < SLANT_DISAGREE) limit = SLANT_DISAGREE;

    if (!slant_auto) {
        fprintf(stderr, "WEFAX SLANT: measured Tilt %+d, Auto Slant OFF - not applied\n", tilt);
        return;
    }
    if (wefax_decoder_get_mode(decoder) != WEFAX_MODE_AUTO) {
        fprintf(stderr, "WEFAX SLANT: measured Tilt %+d, not in Auto RX - not applied\n", tilt);
        return;
    }
    if (slant_user_locked) {
        fprintf(stderr, "WEFAX SLANT: measured Tilt %+d, Tilt set by the user - not applied\n", tilt);
        return;
    }

    char key[SLANT_KEY_LEN];
    bool known = slant_station_key(key, sizeof(key));
    slant_station_t *s = known ? slant_find(key) : NULL;

    if (s) {
        int d = tilt - s->tilt;
        if (d < 0) d = -d;
        if ((float)d <= limit) {
            fprintf(stderr, "WEFAX SLANT: measured Tilt %+d agrees with the memory %+d (limit %.0f) - "
                   "memory kept\n", tilt, s->tilt, limit);
            return;
        }
        fprintf(stderr, "WEFAX SLANT: measured Tilt %+d disagrees with the memory %+d (limit %.0f) - "
               "applied\n", tilt, s->tilt, limit);
    } else {
        fprintf(stderr, "WEFAX SLANT: measured Tilt %+d, %s - applied\n", tilt,
               known ? "no memory for this station" : "station not in the list");
    }

    slant_set_tilt(tilt);
    if (known) slant_store(tilt);
    msg_update_text_fmt("Auto Slant: Tilt %+d", tilt);
}

/* every preview tick: station memory, measured slant, delayed save */
static void slant_poll(void)
{
    slant_check_station();

    float px, se;
    if (decoder && wefax_decoder_slant_take(decoder, &px, &se))
        slant_measured(px, se);

    if (slant_dirty && lv_tick_elaps(slant_dirty_tick) >= SLANT_SAVE_DELAY_MS)
        slant_save();
}

static void slant_flush(void)
{
    if (slant_dirty) slant_save();
}

/* ---- button ------------------------------------------------------------------ */

static const char *slant_label_getter(void)
{
    return slant_auto ? "Auto Slant\nON" : "Auto Slant\nOFF";
}

static void slant_auto_cb(button_data_t *data)
{
    (void)data;
    slant_auto = !slant_auto;
    if (btn_auto_slant.disp_btn) buttons_refresh(&btn_auto_slant);
    msg_update_text_fmt("Auto Slant %s", slant_auto ? "ON" : "OFF");
    slant_mark_dirty();
}
