/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */

/*
 * The WEFAX postprocessing of the firmware (src/wefax/wefax_pp.c) on a
 * computer: cleans a fax saved as 8-bit PGM.
 *
 *   wefax_pp_test MODEL.bin IN.pgm OUT.pgm [--threads N] [--tile N] [--generic]
 *
 * --generic: the plain C loop instead of NEON (ARM only, to compare).
 * Prints the multiply-adds and the time taken.
 */
#include "wefax_pp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t *read_pgm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[3] = { 0 };
    int maxv;
    if (fscanf(f, "%2s %d %d %d", magic, w, h, &maxv) != 4 || strcmp(magic, "P5") || maxv != 255) {
        fclose(f);
        return NULL;
    }
    fgetc(f);
    uint8_t *p = malloc((size_t)*w * *h);
    if (p && fread(p, 1, (size_t)*w * *h, f) != (size_t)*w * *h) {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: wefax_pp_test MODEL.bin IN.pgm OUT.pgm [--threads N] [--tile N] [--generic]\n");
        return 2;
    }
    int threads = 4, tile = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tile") && i + 1 < argc) tile = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--generic")) wefax_pp_generic = 1;
    }
    wefax_pp_t *m = wefax_pp_load(argv[1]);
    if (!m) return 1;
    int w, h;
    uint8_t *g = read_pgm(argv[2], &w, &h);
    if (!g) {
        fprintf(stderr, "%s: not an 8-bit PGM\n", argv[2]);
        return 1;
    }
    printf("base %d, %d x %d, %.1f G multiply-adds, NEON %s, %d threads\n", wefax_pp_base(m), w, h,
           wefax_pp_macs(m, w, h, tile) / 1e9, wefax_pp_has_neon() && !wefax_pp_generic ? "yes" : "no",
           threads);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = wefax_pp_run(m, g, g, w, h, threads, tile);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc) {
        fprintf(stderr, "wefax_pp_run: %d\n", rc);
        return 1;
    }
    printf("%.1f s\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    FILE *f = fopen(argv[3], "wb");
    if (!f) return 1;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(g, 1, (size_t)w * h, f);
    fclose(f);
    wefax_pp_free(m);
    free(g);
    return 0;
}
