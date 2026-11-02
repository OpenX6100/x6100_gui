/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * WEFAX decoder test tool: a recording through src/wefax/decoder.c, the
 * picture as PGM (1809 pixels wide, one row per line, no slant correction).
 *
 *   wefax_wav IN.wav OUT.pgm [--cont] [--denoise]
 *     --cont     Manual RX (default: Auto RX, waits for the start tone)
 *     --denoise  Denoise ON (default: OFF, the phase discriminator)
 *
 * IN.wav: 12000 Hz mono, 16-bit PCM (the recordings of the radio).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"

static uint8_t *img;
static unsigned int rows;

static void row_cb(const uint8_t *row, unsigned int width, unsigned int n, void *user)
{
    (void)user;
    if (n >= WEFAX_MAX_ROWS) return;
    memcpy(img + (size_t)n * WEFAX_WIDTH, row, width < WEFAX_WIDTH ? width : WEFAX_WIDTH);
    if (n + 1 > rows) rows = n + 1;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    bool cont = false, denoise = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cont")) cont = true;
        else if (!strcmp(argv[i], "--denoise")) denoise = true;
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
    }
    if (!in || !out) {
        fprintf(stderr, "usage: wefax_wav IN.wav OUT.pgm [--cont] [--denoise]\n");
        return 2;
    }
    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }

    /* the "data" chunk of the WAV */
    char id[4]; uint32_t sz; uint16_t fmt[8] = {0};
    fseek(f, 12, SEEK_SET);
    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            if (fread(fmt, 1, sz < 16 ? sz : 16, f) != (sz < 16 ? sz : 16)) break;
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            break;
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
    if (fmt[0] != 1 || fmt[1] != 1 || fmt[7] != 16 || (fmt[2] | (uint32_t)fmt[3] << 16) != WEFAX_SAMPLE_RATE) {
        fprintf(stderr, "%s: not 12000 Hz mono 16-bit PCM\n", in);
        return 1;
    }

    img = calloc((size_t)WEFAX_MAX_ROWS * WEFAX_WIDTH, 1);
    wefax_decoder_t *d = wefax_decoder_create();
    wefax_decoder_set_mode(d, cont ? WEFAX_MODE_CONT : WEFAX_MODE_AUTO);
    wefax_decoder_set_row_callback(d, row_cb, NULL);
    wefax_decoder_set_denoise(d, denoise);

    int16_t b[1200];
    float fb[1200];
    size_t n;
    while ((n = fread(b, 2, 1200, f)) > 0) {
        for (size_t i = 0; i < n; i++) fb[i] = b[i] / 32768.0f;
        wefax_decoder_process(d, fb, (unsigned int)n);
    }
    fclose(f);

    FILE *o = fopen(out, "wb");
    if (!o) { perror(out); return 1; }
    fprintf(o, "P5\n%d %u\n255\n", WEFAX_WIDTH, rows);
    fwrite(img, WEFAX_WIDTH, rows, o);
    fclose(o);
    fprintf(stderr, "%s: %u rows%s\n", out, rows, rows ? "" : " (no start tone: try --cont)");
    wefax_decoder_destroy(d);
    free(img);
    return 0;
}
