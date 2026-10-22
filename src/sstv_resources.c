/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#include "sstv_resources.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SSTV_DIR              "/mnt/sstv"
#define SSTV_PICS_DIR         SSTV_DIR "/tx_pics"
#define SSTV_TEXT_DIR         SSTV_DIR "/tx_text"
#define SSTV_MARKER           SSTV_DIR "/.resources-v1"
#define SSTV_DEFAULT_PICS_DIR "/usr/share/x6100/sstv/tx_pics"
#define SSTV_DEFAULT_TEXT_DIR "/usr/share/x6100/sstv/tx_text"

static const char *const default_pics[] = {
    "defaultpic1.png", "defaultpic2.png", "defaultpic3.png", "defaultpic4.png"
};

static const char *const default_text[] = {
    "73.sstvtxt", "CQ REPLY.sstvtxt", "PSEQSL.sstvtxt",
    "R454QRM.sstvtxt", "R575QSB.sstvtxt", "R595.sstvtxt"
};

static bool ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST)
        return true;
    fprintf(stderr, "SSTV resources: mkdir %s failed: %s\n", path, strerror(errno));
    return false;
}

static bool copy_if_missing(const char *src, const char *dst)
{
    /* Never overwrite a user's existing file, even when installing a new
     * resource set after an upgrade. */
    if (access(dst, F_OK) == 0)
        return true;

    FILE *in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "SSTV resources: cannot open %s: %s\n", src, strerror(errno));
        return false;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "SSTV resources: cannot create %s: %s\n", dst, strerror(errno));
        fclose(in);
        return false;
    }

    unsigned char buf[4096];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) != 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;

    if (!ok) {
        fprintf(stderr, "SSTV resources: copy %s -> %s failed\n", src, dst);
        unlink(dst);
    }
    return ok;
}

static bool install_group(const char *src_dir, const char *dst_dir,
                          const char *const *names, size_t count)
{
    char src[512], dst[512];
    for (size_t i = 0; i < count; ++i) {
        if (snprintf(src, sizeof(src), "%s/%s", src_dir, names[i]) >= (int)sizeof(src) ||
            snprintf(dst, sizeof(dst), "%s/%s", dst_dir, names[i]) >= (int)sizeof(dst))
            return false;
        if (!copy_if_missing(src, dst))
            return false;
    }
    return true;
}

void sstv_resources_init(void)
{
    if (!ensure_dir(SSTV_DIR) || !ensure_dir(SSTV_PICS_DIR) || !ensure_dir(SSTV_TEXT_DIR))
        return;

    if (access(SSTV_MARKER, F_OK) == 0)
        return;

    bool ok = install_group(SSTV_DEFAULT_PICS_DIR, SSTV_PICS_DIR,
                            default_pics, sizeof(default_pics) / sizeof(default_pics[0])) &&
              install_group(SSTV_DEFAULT_TEXT_DIR, SSTV_TEXT_DIR,
                            default_text, sizeof(default_text) / sizeof(default_text[0]));
    if (!ok)
        return; /* retry next boot; existing files will not be overwritten */

    FILE *marker = fopen(SSTV_MARKER, "wb");
    if (!marker) {
        fprintf(stderr, "SSTV resources: cannot create %s: %s\n",
                SSTV_MARKER, strerror(errno));
        return;
    }
    fputs("R1CBU Custom SSTV resources v1\n", marker);
    fclose(marker);
    fprintf(stderr, "SSTV resources: v1 installed\n");
}
