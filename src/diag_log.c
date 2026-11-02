/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Xiegu X6100 - diagnostic logs (1KO125, Hany El Imam 2026), see diag_log.h
 */

#include "diag_log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define PTT_LOG         "/mnt/ptt.log"
#define DIAG_MAX_BYTES  (512 * 1024)
#define DIAG_MAX_PER_S  10          /* lines per second and file, the rest only counted */

#define DIAG_QUEUE      32          /* lines waiting for the writer thread */
#define DIAG_LINE       512

static __thread const char *thread_source = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  log_cond  = PTHREAD_COND_INITIALIZER;

/* lines written in the current second, per file */
typedef struct {
    time_t   sec;
    int      n;
    unsigned skipped;
} rate_t;
static rate_t rate_ptt;

/* Lines are queued and written by a thread of their own, so a TX command
 * never waits for the SD card (open, write, fsync, close). */
static char     queue[DIAG_QUEUE][DIAG_LINE];
static unsigned q_head, q_count;
static bool     writer_running;

/* ---- writing ------------------------------------------------------------------- */

static void write_line(const char *path, const char *line, size_t len)
{
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > DIAG_MAX_BYTES) {
        char old[64];
        snprintf(old, sizeof(old), "%s.old", path);
        rename(path, old);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ssize_t w = write(fd, line, len);
        (void)w;
        fsync(fd);              /* the next event may be a crash or a power cut */
        close(fd);
    }
}

static void *writer_thread(void *arg)
{
    (void)arg;
    char line[DIAG_LINE];

    pthread_mutex_lock(&log_mutex);
    for (;;) {
        while (q_count == 0)
            pthread_cond_wait(&log_cond, &log_mutex);
        memcpy(line, queue[q_head], sizeof(line));
        q_head = (q_head + 1) % DIAG_QUEUE;
        q_count--;
        pthread_mutex_unlock(&log_mutex);

        write_line(PTT_LOG, line, strlen(line));

        pthread_mutex_lock(&log_mutex);
    }
    return NULL;
}

static void vlog_to(const char *path, rate_t *rate, int max_per_s, const char *fmt, va_list ap)
{
    char line[DIAG_LINE];
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    int n = (int)strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S", &tm);
    n += snprintf(line + n, sizeof(line) - (size_t)n, ".%03ld UTC  ", ts.tv_nsec / 1000000);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    pthread_mutex_lock(&log_mutex);
    /* a burst must not wear the SD card: DIAG_MAX_PER_S lines a second */
    if (rate->sec != ts.tv_sec) {
        rate->sec = ts.tv_sec;
        rate->n   = 0;
    }
    if (++rate->n > max_per_s || (writer_running && q_count == DIAG_QUEUE)) {
        rate->skipped++;
        pthread_mutex_unlock(&log_mutex);
        return;
    }
    if (rate->skipped) {
        snprintf(line + len, sizeof(line) - len, "  (%u lines skipped before this one)", rate->skipped);
        len = strlen(line);
        rate->skipped = 0;
    }
    if (len > sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    line[len]   = '\0';

    if (writer_running) {
        memcpy(queue[(q_head + q_count) % DIAG_QUEUE], line, len + 1);
        q_count++;
        pthread_cond_signal(&log_cond);
    } else {
        write_line(path, line, len);    /* no thread (yet): write it here */
    }
    pthread_mutex_unlock(&log_mutex);
}

void diag_ptt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_to(PTT_LOG, &rate_ptt, DIAG_MAX_PER_S, fmt, ap);
    va_end(ap);
}

const char *diag_set_source(const char *name)
{
    const char *prev = thread_source;
    thread_source = name;
    return prev;
}

void diag_source_copy(char *dst, size_t size)
{
    if (thread_source)
        snprintf(dst, size, "%s", thread_source);
    else
        snprintf(dst, size, "thread %ld", (long)syscall(SYS_gettid));
}

void diag_init(void)
{
    pthread_t thread;

    diag_set_source("UI");              /* the thread of main(): LVGL, keys */
    diag_ptt_log("---- start (firmware running) ----");
    if (pthread_create(&thread, NULL, writer_thread, NULL) == 0) {
        pthread_detach(thread);
        pthread_mutex_lock(&log_mutex);
        writer_running = true;
        pthread_mutex_unlock(&log_mutex);
    }
}
