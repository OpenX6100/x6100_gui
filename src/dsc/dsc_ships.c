/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - DSC ship names database, see dsc_ships.h.
 */
#include "dsc_ships.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* the user's copy on the SD card first, then the one installed with the firmware */
static const char *const paths[] = {
    "/mnt/dsc/ships.bin",
    "/usr/share/x6100/dsc_ships.bin",
};

#define HDR_LEN     16
#define ENTRY_LEN   8

static const uint8_t *map = NULL;
static size_t map_len = 0;
static uint32_t count = 0;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int dsc_ships_open(void)
{
    if (map) return (int)count;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < HDR_LEN) { close(fd); continue; }
        void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) continue;
        const uint8_t *p = m;
        uint32_t n = rd32(p + 8);
        if (memcmp(p, "DSCS", 4) != 0 || rd32(p + 4) != 2 ||
            (uint64_t)HDR_LEN + (uint64_t)n * ENTRY_LEN > (uint64_t)st.st_size) {
            fprintf(stderr, "DSC: %s is not a valid ships database\n", paths[i]);
            munmap(m, (size_t)st.st_size);
            continue;
        }
        map = p;
        map_len = (size_t)st.st_size;
        count = n;
        fprintf(stderr, "DSC: %u ships in %s\n", (unsigned)n, paths[i]);
        return (int)count;
    }
    fprintf(stderr, "DSC: no ships database (%s or %s)\n", paths[0], paths[1]);
    return 0;
}

void dsc_ships_close(void)
{
    if (map) munmap((void *)map, map_len);
    map = NULL;
    map_len = 0;
    count = 0;
}

/* next string at *off, or "" when it would run past the end */
static const char *str_at(size_t *off)
{
    if (*off >= map_len) return "";
    const char *s = (const char *)map + *off;
    size_t n = strnlen(s, map_len - *off);
    if (*off + n >= map_len) return "";
    *off += n + 1;
    return s;
}

bool dsc_ship_lookup(const char *mmsi, dsc_ship_t *out)
{
    if (!map || !mmsi || strlen(mmsi) != 9) return false;
    uint32_t m = 0;
    for (int i = 0; i < 9; ++i) {
        if (mmsi[i] < '0' || mmsi[i] > '9') return false;
        m = m * 10 + (uint32_t)(mmsi[i] - '0');
    }
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = map + HDR_LEN + (size_t)mid * ENTRY_LEN;
        uint32_t v = rd32(e);
        if (v == m) {
            size_t off = rd32(e + 4);
            out->name = str_at(&off);
            out->call = str_at(&off);
            out->type = str_at(&off);
            return out->name[0] != '\0';
        }
        if (v < m) lo = mid + 1; else hi = mid;
    }
    return false;
}
