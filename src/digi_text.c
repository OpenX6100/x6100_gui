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

#include "cfg/cfg_api.h"

#define BUF_SIZE      4096
#define FLUSH_S       2
#define ROTATE_BYTES  (4L * 1024 * 1024)

static pthread_mutex_t   lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    wake = PTHREAD_COND_INITIALIZER;
static char              buf[BUF_SIZE];
static size_t            len;
static bool              opened;        /* a window has it open */
static bool              active;        /* opened and switched on */
static bool              need_header;
static uint64_t          header_hz;     /* the frequency when it was asked for */
static bool              at_line_start = true;
static char              mode_name[16];
static char              file_path[64];
static digi_text_freq_fn freq_fn;
static pthread_t         writer;
static bool              writer_on;
static bool              writer_quit;

/* lock held */
static void put(char c)
{
    if (len < BUF_SIZE) {
        buf[len++] = c;
    }
}

/* lock held: the header goes in with the first text after it was asked
 * for, with that moment's time and the frequency of when it was asked
 * for (a band change), not of when the buffer is written out. */
static void put_header_if_needed(void)
{
    char      h[96];
    struct tm tm;
    time_t    now;
    int       n;

    if (!need_header) {
        return;
    }
    need_header = false;
    if (!at_line_start) {
        put('\n');
    }
    now = time(NULL);
    gmtime_r(&now, &tm);
    n = snprintf(h, sizeof(h), "\n--- %04d-%02d-%02d %02d:%02d:%02d UTC  %s", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, mode_name);
    if (header_hz > 0 && n > 0 && (size_t)n < sizeof(h)) {
        n += snprintf(h + n, sizeof(h) - (size_t)n, "  %.3f MHz", (double)header_hz / 1e6);
    }
    if (n > 0 && (size_t)n < sizeof(h)) {
        snprintf(h + n, sizeof(h) - (size_t)n, " ---\n");
    }
    for (const char *p = h; *p; p++) {
        put(*p);
    }
    at_line_start = true;
}

/* Writer thread: append the buffer to the file. The SD card can take a
 * while over fsync(), which is why this isn't on the UI thread. */
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
    fwrite(out, 1, n, f);
    /* /mnt is FAT on an SD card that loses power when the battery is
     * pulled: do not leave the text in a page cache. */
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void *writer_fn(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!writer_quit) {
        struct timespec until;

        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += FLUSH_S;
        pthread_cond_timedwait(&wake, &lock, &until);
        pthread_mutex_unlock(&lock);
        flush_out();
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    flush_out();                        /* what came in last */
    return NULL;
}

static uint64_t freq_now(void)
{
    return freq_fn ? freq_fn() : 0;
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
    header_hz = freq_now();
    at_line_start = true;
    active = true;
    writer_quit = false;
    pthread_mutex_unlock(&lock);
    if (!writer_on) {
        writer_on = pthread_create(&writer, NULL, writer_fn, NULL) == 0;
    }
}

static void stop(void)
{
    pthread_mutex_lock(&lock);
    active = false;
    writer_quit = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    if (writer_on) {
        pthread_join(writer, NULL);     /* it writes out the rest first */
        writer_on = false;
    }
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
    uint64_t hz = freq_now();           /* UI thread: read it now */

    pthread_mutex_lock(&lock);
    need_header = true;
    header_hz = hz;
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
