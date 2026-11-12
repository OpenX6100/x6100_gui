/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * Decodes the DSC calls of a recording with the firmware decoder, on a
 * computer: one line per call (time, error check, characters, meaning).
 *
 *   dsc_wav IN.wav        16-bit PCM or 32-bit float, mono, >= 8000 Hz
 */
#include "dsc_decoder.h"
#include "dsc_format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_call(const dsc_message_t *m, void *user)
{
    (void)user;
    dsc_info_t in;
    dsc_format_parse(m->sym, m->n, &in);
    /* OK: verified, as the DSC window shows it; ECC: the error check
     * agrees but the call is not trusted (see dsc_decoder.h) */
    printf("%.1f %s", m->start_t, m->verified ? "OK " : (m->ecc_ok ? "ECC" : "BAD"));
    for (int i = 0; i < m->n; i++) printf(" %d", m->sym[i]);
    printf(" | %s from %s to %s | %s | valid %d/%d repaired %d unsure %d\n", in.fmt_name, in.from_id,
           in.to_id, in.line2, m->raw_valid, m->raw_total, m->repaired, m->unsure);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: dsc_wav IN.wav\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "not a WAV file\n");
        return 1;
    }
    unsigned fmt = 0, ch = 1, rate = 0, bits = 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) return 1;
        uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[40] = {0};
            if (fread(fm, 1, len < 40 ? len : 40, f) == 0) return 1;
            if (len > 40) fseek(f, len - 40, SEEK_CUR);
            fmt = fm[0] | fm[1] << 8;
            if (fmt == 0xFFFE && len >= 26) fmt = fm[24] | fm[25] << 8;
            ch = fm[2] | fm[3] << 8;
            rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
            bits = fm[14] | fm[15] << 8;
        } else if (!memcmp(c, "data", 4)) {
            break;
        } else {
            fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
    int pcm16 = fmt == 1 && bits == 16, flt = fmt == 3 && bits == 32;
    if (!(pcm16 || flt) || ch < 1) { fprintf(stderr, "only 16-bit PCM or 32-bit float\n"); return 1; }
    dsc_decoder_t *d = dsc_decoder_create((int)rate, on_call, NULL);
    if (!d) { fprintf(stderr, "sample rate %u not supported\n", rate); return 1; }
    unsigned bps = bits / 8 * ch;
    uint8_t buf[4096 * 8];
    float x[4096];
    size_t n;
    while ((n = fread(buf, bps, 4096, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (pcm16) { int16_t v; memcpy(&v, buf + i * bps, 2); x[i] = v / 32768.0f; }
            else memcpy(&x[i], buf + i * bps, 4);
        }
        dsc_decoder_process(d, x, (int)n);
    }
    /* 3 s of silence, then whatever is still pending */
    memset(x, 0, sizeof(x));
    for (unsigned k = 0; k < rate * 3 / 4096 + 1; k++) dsc_decoder_process(d, x, 4096);
    dsc_decoder_flush(d);
    dsc_decoder_destroy(d);
    fclose(f);
    return 0;
}
