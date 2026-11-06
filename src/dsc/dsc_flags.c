/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - country flags for the DSC detail card, see dsc_flags.h.
 */
#include "dsc_flags.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const path = "/usr/share/x6100/dsc_flags.bin";

#define HDR_LEN     16
#define ENTRY_LEN   8
#define MAX_FLAGS   300

static const uint8_t *map = NULL;
static size_t map_len = 0;
static int count = 0;
static char codes[MAX_FLAGS][2];
static lv_img_dsc_t imgs[MAX_FLAGS];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int dsc_flags_open(void)
{
    if (map) return count;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "DSC: no flags (%s)\n", path); return 0; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < HDR_LEN) { close(fd); return 0; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return 0;

    const uint8_t *p = m;
    int n = rd16(p + 6), w = rd16(p + 8), h = rd16(p + 10);
    uint32_t img_len = (uint32_t)w * (uint32_t)h * 4;
    if (memcmp(p, "DSCF", 4) != 0 || rd16(p + 4) != 1 || n > MAX_FLAGS || w <= 0 || h <= 0 ||
        (uint64_t)HDR_LEN + (uint64_t)n * ENTRY_LEN > (uint64_t)st.st_size || LV_COLOR_DEPTH != 32) {
        fprintf(stderr, "DSC: %s is not a valid flags file\n", path);
        munmap(m, (size_t)st.st_size);
        return 0;
    }
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const uint8_t *e = p + HDR_LEN + i * ENTRY_LEN;
        uint32_t off = rd32(e + 4);
        if ((uint64_t)off + img_len > (uint64_t)st.st_size) continue;
        codes[k][0] = (char)e[0];
        codes[k][1] = (char)e[1];
        memset(&imgs[k], 0, sizeof(imgs[k]));
        imgs[k].header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        imgs[k].header.w = (uint32_t)w;
        imgs[k].header.h = (uint32_t)h;
        imgs[k].data_size = img_len;
        imgs[k].data = p + off;
        k++;
    }
    map = p;
    map_len = (size_t)st.st_size;
    count = k;
    fprintf(stderr, "DSC: %d flags in %s\n", count, path);
    return count;
}

void dsc_flags_close(void)
{
    if (map) munmap((void *)map, map_len);
    map = NULL;
    map_len = 0;
    count = 0;
}

const lv_img_dsc_t *dsc_flag(const char *iso)
{
    if (!map || !iso || strlen(iso) != 2) return NULL;
    int lo = 0, hi = count - 1;               /* the index is sorted by code */
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        int c = memcmp(codes[m], iso, 2);
        if (c == 0) return &imgs[m];
        if (c < 0) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}
