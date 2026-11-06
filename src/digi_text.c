/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - sent and received text of the keyboard modes
 *  saved to the SD card
 */

#include "digi_text.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "cfg/cfg_api.h"

#define BUF_SIZE      4096
#define FLUSH_MS      2000
#define ROTATE_BYTES  (4L * 1024 * 1024)

/* Marks the place of a header in the buffer; the header itself is made
 * when the buffer is written out, on the UI thread, where the frequency
 * can be read. Never sent or received: the decoders drop control
 * characters other than line breaks, and so does digi_text_rx(). */
#define HEADER_MARK   '\x01'

static pthread_mutex_t   lock = PTHREAD_MUTEX_INITIALIZER;
static char              buf[BUF_SIZE];
static size_t            len;
static bool              opened;        /* a window has it open */
static bool              active;        /* opened and switched on */
static bool              need_header;
static bool              at_line_start = true;
static char              mode_name[16];
static char              file_path[64];
static digi_text_freq_fn freq_fn;
static lv_timer_t       *timer;

/* lock held */
static void put(char c)
{
    if (len < BUF_SIZE) {
        buf[len++] = c;
    }
}

/* lock held */
static void put_header_if_needed(void)
{
    if (need_header) {
        need_header = false;
        if (!at_line_start) {
            put('\n');
        }
        put(HEADER_MARK);
        at_line_start = true;
    }
}

static void write_header(FILE *f)
{
    struct tm tm;
    time_t    now = time(NULL);
    uint64_t  hz = freq_fn ? freq_fn() : 0;

    gmtime_r(&now, &tm);
    fprintf(f, "\n--- %04d-%02d-%02d %02d:%02d:%02d UTC  %s", tm.tm_year + 1900, tm.tm_mon + 1,
            tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, mode_name);
    if (hz > 0) {
        fprintf(f, "  %.3f MHz", (double)hz / 1e6);
    }
    fputs(" ---\n", f);
}

/* UI thread: append the buffer to the file. */
static void flush_out(void)
{
    static char out[BUF_SIZE];
    size_t      n;
    FILE       *f;

    pthread_mutex_lock(&lock);
    n = len;
    memcpy(out, buf, n);
    len = 0;
    pthread_mutex_unlock(&lock);

    if (n == 0 || file_path[0] == '\0') {
        return;
    }
    f = fopen(file_path, "a");
    if (f == NULL) {
        return;
    }
    for (size_t i = 0; i < n;) {
        size_t j = i;

        while (j < n && out[j] != HEADER_MARK) {
            j++;
        }
        if (j > i) {
            fwrite(out + i, 1, j - i, f);
        }
        if (j < n) {
            write_header(f);
            j++;
        }
        i = j;
    }
    /* /mnt is FAT on an SD card that loses power when the battery is
     * pulled: do not leave the text in a page cache. */
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    flush_out();
}

static void start(void)
{
    struct stat st;

    if (stat(file_path, &st) == 0 && st.st_size > ROTATE_BYTES) {
        char old[sizeof(file_path) + 4];

        snprintf(old, sizeof(old), "%s.1", file_path);
        rename(file_path, old);
    }
    pthread_mutex_lock(&lock);
    len = 0;
    need_header = true;
    at_line_start = true;
    active = true;
    pthread_mutex_unlock(&lock);
    if (timer == NULL) {
        timer = lv_timer_create(timer_cb, FLUSH_MS, NULL);
    }
}

static void stop(void)
{
    pthread_mutex_lock(&lock);
    active = false;
    pthread_mutex_unlock(&lock);
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    flush_out();
}

void digi_text_open(const char *mode, const char *path, digi_text_freq_fn freq)
{
    if (opened) {
        digi_text_close();
    }
    snprintf(mode_name, sizeof(mode_name), "%s", mode ? mode : "");
    snprintf(file_path, sizeof(file_path), "%s", path ? path : "");
    freq_fn = freq;
    opened = true;
    if (digi_text_enabled() && file_path[0]) {
        start();
    }
}

void digi_text_close(void)
{
    if (!opened) {
        return;
    }
    stop();
    opened = false;
    freq_fn = NULL;
}

void digi_text_rx(const char *text)
{
    pthread_mutex_lock(&lock);
    if (active) {
        for (const char *p = text ? text : ""; *p; p++) {
            unsigned char c = (unsigned char)*p;

            /* decoding noise, as on the screen */
            if ((c < 32 && c != '\n') || c == 127) {
                continue;
            }
            put_header_if_needed();
            put((char)c);
            at_line_start = (c == '\n');
        }
    }
    pthread_mutex_unlock(&lock);
}

void digi_text_rx_char(char c)
{
    char s[2] = {c, '\0'};

    digi_text_rx(s);
}

void digi_text_tx(const char *text)
{
    const char *p = text ? text : "";
    size_t      n;

    /* The line breaks sent around a transmission are for the other
     * station's screen; here each transmission gets lines of its own. */
    while (*p == '\n' || *p == '\r') {
        p++;
    }
    n = strlen(p);
    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) {
        n--;
    }
    if (n == 0) {
        return;
    }

    pthread_mutex_lock(&lock);
    if (active) {
        put_header_if_needed();
        if (!at_line_start) {
            put('\n');
        }
        put('T');
        put('X');
        put(':');
        put(' ');
        for (size_t i = 0; i < n; i++) {
            if (p[i] == '\r') {
                continue;
            }
            put(p[i]);
            if (p[i] == '\n') {
                put('T');
                put('X');
                put(':');
                put(' ');
            }
        }
        put('\n');
        at_line_start = true;
    }
    pthread_mutex_unlock(&lock);
}

void digi_text_new_header(void)
{
    pthread_mutex_lock(&lock);
    need_header = true;
    pthread_mutex_unlock(&lock);
}

bool digi_text_enabled(void)
{
    return param_i_get(cfg.digi_text_save()) != 0;
}

void digi_text_set_enabled(bool on)
{
    param_i_set(cfg.digi_text_save(), on ? 1 : 0);
    if (!opened || file_path[0] == '\0') {
        return;
    }
    if (on && !active) {
        start();
    } else if (!on && active) {
        stop();
    }
}

const char *digi_text_path(void)
{
    return active ? file_path : "";
}
