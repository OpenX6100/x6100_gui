/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */

/*
 * 1KO125 WEFAX postprocessing (included by dialog_wefax.c).
 *
 * With Denoise set to "Post Proc." every saved fax is also cleaned by the
 * WEFAX denoiser network (wefax/wefax_pp.c) and saved next to it as
 * NAME_pp.png; the fax as received is always kept.
 *
 * The work runs in a worker thread of its own, at low priority (nice), so
 * that the reception and the screen go on; a new fax saved meanwhile stops
 * the one in course (the new one is cleaned instead).  The model is read
 * once, when first needed, from WEFAX_PP_MODEL.  Time, multiply-adds and
 * memory go to the log.
 */

#include "wefax/wefax_pp.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define WEFAX_PP_MODEL    "/usr/share/x6100/wefax_pp.bin"
#define WEFAX_PP_THREADS  3     /* of the 4 cores: one left to audio and screen */

typedef struct {
    uint8_t *image;             /* rows x WEFAX_WIDTH, as received (oldest first) */
    unsigned int rows;
    uint32_t row_base;
    int align, tilt;
    char path[128];             /* the fax saved */
} pp_job_t;

static pthread_mutex_t pp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pp_job_t *pp_pending = NULL;   /* next job, taken by the worker */
static bool pp_worker_alive = false;
static bool pp_busy = false;          /* a fax is being cleaned */
static int  pp_result = 0;            /* 1 saved, -1 failed, -2 no model; read by pp_poll */
static char pp_result_name[64];
static wefax_pp_t *pp_model = NULL;
static bool pp_model_tried = false;
static int  pp_shown = -1;            /* progress shown, in steps of 10 % */

static void pp_job_free(pp_job_t *j)
{
    if (j) {
        free(j->image);
        free(j);
    }
}

/* the picture as saved in the PNG: Align and Tilt applied, as in
 * save_fax_worker() */
static void pp_aligned(const pp_job_t *j, uint8_t *dst)
{
    for (unsigned int r = 0; r < j->rows; r++) {
        const uint8_t *src = j->image + (size_t)r * WEFAX_WIDTH;
        const int64_t row_from_first = (int64_t)j->row_base + (int64_t)r;
        const int tilt_shift = (int)((row_from_first * (int64_t)j->tilt * (int64_t)PREVIEW_WIDTH) /
                                     (1000LL * (int64_t)WEFAX_WIDTH));
        uint8_t *out = dst + (size_t)r * WEFAX_WIDTH;
        for (unsigned int x = 0; x < WEFAX_WIDTH; x++) {
            int sx = (int)x - j->align - tilt_shift;
            while (sx < 0) sx += WEFAX_WIDTH;
            while (sx >= WEFAX_WIDTH) sx -= WEFAX_WIDTH;
            out[x] = src[sx];
        }
    }
}

static bool pp_write_png(const char *path, const uint8_t *img, unsigned int rows)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) png_destroy_write_struct(&png, info ? &info : NULL);
        fclose(fp);
        remove(path);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        remove(path);
        return false;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, WEFAX_WIDTH, rows, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (unsigned int r = 0; r < rows; r++)
        png_write_row(png, (png_bytep)(img + (size_t)r * WEFAX_WIDTH));
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    if (fclose(fp) != 0) {
        remove(path);
        return false;
    }
    return true;
}

/* one job: 1 saved, -1 failed, -2 no model, 0 stopped */
static int pp_do(const pp_job_t *j)
{
    if (!pp_model_tried) {
        pp_model_tried = true;
        pp_model = wefax_pp_load(WEFAX_PP_MODEL);
    }
    if (!pp_model) return -2;

    uint8_t *img = malloc((size_t)j->rows * WEFAX_WIDTH);
    if (!img) return -1;
    pp_aligned(j, img);

    const double macs = wefax_pp_macs(pp_model, WEFAX_WIDTH, (int)j->rows, 0);
    fprintf(stderr, "WEFAX PP: %s, %u rows, base %d, %.1f G multiply-adds, %d threads, NEON %s\n", j->path, j->rows,
           wefax_pp_base(pp_model), macs / 1e9, WEFAX_PP_THREADS, wefax_pp_has_neon() ? "yes" : "no");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const int rc = wefax_pp_run(pp_model, img, img, WEFAX_WIDTH, (int)j->rows, WEFAX_PP_THREADS, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    if (rc) {
        fprintf(stderr, "WEFAX PP: %s after %.1f s (rc %d), peak memory of the program %ld MB\n",
               rc == -3 ? "stopped" : "failed", secs, rc, ru.ru_maxrss / 1024);
        free(img);
        return rc == -3 ? 0 : -1;
    }
    fprintf(stderr, "WEFAX PP: done in %.1f s (%.2f G multiply-adds/s), peak memory of the program %ld MB\n", secs,
           macs / 1e9 / secs, ru.ru_maxrss / 1024);

    /* NAME.png -> NAME_pp.png */
    char out[sizeof(j->path) + 4];
    snprintf(out, sizeof(out), "%s", j->path);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    strncat(out, "_pp.png", sizeof(out) - strlen(out) - 1);
    const bool ok = pp_write_png(out, img, j->rows);
    free(img);
    fprintf(stderr, "WEFAX PP: %s %s\n", ok ? "saved" : "save failed", out);
    if (ok) {
        const char *base = strrchr(out, '/');
        pthread_mutex_lock(&pp_mutex);
        snprintf(pp_result_name, sizeof(pp_result_name), "%s", base ? base + 1 : out);
        pthread_mutex_unlock(&pp_mutex);
    }
    return ok ? 1 : -1;
}

static void *pp_worker(void *arg)
{
    (void)arg;
    /* low priority: reception and screen first (the threads of
     * wefax_pp_run inherit it) */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    for (;;) {
        pthread_mutex_lock(&pp_mutex);
        pp_job_t *j = pp_pending;
        pp_pending = NULL;
        if (!j) {
            pp_worker_alive = false;
            pp_busy = false;
            pthread_mutex_unlock(&pp_mutex);
            return NULL;
        }
        wefax_pp_cancel = 0;
        wefax_pp_progress = 0;
        pp_busy = true;
        pthread_mutex_unlock(&pp_mutex);

        const int r = pp_do(j);
        pp_job_free(j);

        pthread_mutex_lock(&pp_mutex);
        pp_busy = false;
        if (r != 0) pp_result = r;
        pthread_mutex_unlock(&pp_mutex);
    }
}

/* From save_fax_worker(): clean this fax (takes image).  A fax being
 * cleaned is stopped: the newest one goes first. */
static void pp_queue(uint8_t *image, unsigned int rows, uint32_t row_base, int align, int tilt,
                     const char *path)
{
    pp_job_t *j = calloc(1, sizeof(*j));
    if (!j) {
        free(image);
        return;
    }
    j->image = image;
    j->rows = rows;
    j->row_base = row_base;
    j->align = align;
    j->tilt = tilt;
    snprintf(j->path, sizeof(j->path), "%s", path);

    pthread_mutex_lock(&pp_mutex);
    pp_job_free(pp_pending);
    pp_pending = j;
    if (pp_busy) wefax_pp_cancel = 1;
    bool start = !pp_worker_alive;
    if (start) pp_worker_alive = true;
    pthread_mutex_unlock(&pp_mutex);

    if (start) {
        pthread_t th;
        if (pthread_create(&th, NULL, pp_worker, NULL) == 0) {
            pthread_detach(th);
        } else {
            pthread_mutex_lock(&pp_mutex);
            pp_worker_alive = false;
            pp_job_free(pp_pending);
            pp_pending = NULL;
            pthread_mutex_unlock(&pp_mutex);
            fprintf(stderr, "WEFAX PP: cannot start the worker\n");
        }
    }
}

/* LVGL thread (update_preview_cb): progress and result on the screen */
static void pp_poll(void)
{
    pthread_mutex_lock(&pp_mutex);
    const bool busy = pp_busy;
    const int result = pp_result;
    char name[sizeof(pp_result_name)];
    snprintf(name, sizeof(name), "%s", pp_result_name);
    pp_result = 0;
    pthread_mutex_unlock(&pp_mutex);

    if (busy) {
        const int step = wefax_pp_progress / 100;
        if (step != pp_shown) {
            pp_shown = step;
            msg_update_text_fmt("Post Proc. %d%%", step * 10);
        }
    } else {
        pp_shown = -1;
    }
    if (result == 1) msg_schedule_text_fmt("Post Proc. saved\n%s", name);
    else if (result == -1) msg_schedule_text_fmt("Post Proc. failed");
    else if (result == -2) msg_schedule_text_fmt("Post Proc.: no model\n" WEFAX_PP_MODEL);
}
