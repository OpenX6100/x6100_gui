/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - Olivia macros
 */

#include "olivia_macro.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cfg/cfg_api.h"
#include "digi_qso.h"         /* shared QSO state: DX call and RST */

#ifndef OLIVIA_MACROS_PATH
#define OLIVIA_MACROS_PATH "/mnt/olivia_macros.txt"
#endif

static olivia_macro_t macros[OLIVIA_MACRO_MAX];
static int         macro_count = 0;

/* Copy with truncation, turning tabs and line breaks into spaces. */
static void copy_clean(char *dst, size_t dst_sz, const char *src) {
    size_t o = 0;

    if (!src) src = "";
    for (const char *p = src; *p && o + 1 < dst_sz; p++) {
        char c = *p;
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        dst[o++] = c;
    }
    dst[o] = '\0';
}

/* A starter set; edited from the window (hold a macro button). */
static void seed_default_macros(void) {
    macro_count = 0;
    olivia_macro_add("CQ",     "CQ CQ CQ de <MYCALL> <MYCALL> <MYCALL> pse k");
    olivia_macro_add("Answer", "<CALL> <CALL> de <MYCALL> <MYCALL> <MYCALL> kn");
    olivia_macro_add("Report", "<CALL> de <MYCALL> - tnx for the call, ur rst <RST> <RST> - <CALL> de <MYCALL> kn");
    olivia_macro_add("TNX",    "<CALL> de <MYCALL> - tnx for rst <RSTR> - btu <CALL> de <MYCALL> kn");
    olivia_macro_add("73",     "<CALL> de <MYCALL> - tnx fer qso, 73 es gl - <CALL> de <MYCALL> sk");
}

void olivia_macros_load(void) {
    FILE *f = fopen(OLIVIA_MACROS_PATH, "r");

    macro_count = 0;

    if (!f) {
        seed_default_macros();
        olivia_macros_save();
        return;
    }

    char line[OLIVIA_MACRO_NAME_LEN + OLIVIA_MACRO_TEXT_LEN + 8];

    while (macro_count < OLIVIA_MACRO_MAX && fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;

        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';

        olivia_macro_t *m = &macros[macro_count++];
        copy_clean(m->name, sizeof(m->name), line);
        copy_clean(m->text, sizeof(m->text), tab + 1);
    }
    fclose(f);
}

bool olivia_macros_save(void) {
    FILE *f = fopen(OLIVIA_MACROS_PATH, "w");
    if (!f) return false;

    for (int i = 0; i < macro_count; i++) {
        fprintf(f, "%s\t%s\n", macros[i].name, macros[i].text);
    }

    /* /mnt is a FAT partition on an SD card that loses power when the
     * battery is pulled; do not leave the new list in a page cache. */
    bool ok = (fflush(f) == 0);
    if (ok) fsync(fileno(f));
    if (fclose(f) != 0) ok = false;
    return ok;
}

int olivia_macros_count(void) {
    return macro_count;
}

const olivia_macro_t *olivia_macro_get(int idx) {
    if (idx < 0 || idx >= macro_count) return NULL;
    return &macros[idx];
}

int olivia_macro_add(const char *name, const char *text) {
    if (macro_count >= OLIVIA_MACRO_MAX) return -1;

    olivia_macro_t *m = &macros[macro_count];
    copy_clean(m->name, sizeof(m->name), name);
    copy_clean(m->text, sizeof(m->text), text);
    return macro_count++;
}

void olivia_macro_set(int idx, const char *name, const char *text) {
    if (idx < 0 || idx >= macro_count) return;

    olivia_macro_t *m = &macros[idx];
    if (name) copy_clean(m->name, sizeof(m->name), name);
    if (text) copy_clean(m->text, sizeof(m->text), text);
}

void olivia_macro_delete(int idx) {
    if (idx < 0 || idx >= macro_count) return;

    for (int i = idx; i < macro_count - 1; i++) {
        macros[i] = macros[i + 1];
    }
    macro_count--;
}

static bool put_str(char *out, size_t out_sz, size_t *o, const char *s) {
    for (; s && *s; s++) {
        if (*o + 1 >= out_sz) return false;
        out[(*o)++] = *s;
    }
    return true;
}

bool olivia_macro_expand(const char *in, char *out, size_t out_sz,
                      const char **missing) {
    static const struct {
        const char *tok;
        int         id;
    } toks[] = {
        /* <RSTR> before <RST>: the shorter token is a prefix of it */
        {"<MYCALL>", 0}, {"<CALL>", 1}, {"<RSTR>", 3}, {"<RST>", 2},
    };

    size_t o = 0;
    bool   ok = true;
    char   mycall[PARAM_TEXT_MAX];

    if (!out || out_sz == 0) return false;
    param_t_get_into(cfg.callsign(), mycall, sizeof(mycall));
    out[0] = '\0';
    if (!in) return true;

    for (const char *p = in; *p; ) {
        bool matched = false;

        for (size_t t = 0; t < sizeof(toks) / sizeof(toks[0]); t++) {
            size_t len = strlen(toks[t].tok);
            if (strncmp(p, toks[t].tok, len) != 0) continue;

            const char *val = "";
            switch (toks[t].id) {
                case 0: val = mycall;                break;
                case 1: val = digi_qso_dx();         break;
                case 2: val = digi_qso_rst_sent();   break;
                case 3: val = digi_qso_rst_rcvd();   break;
            }
            if (!val || !*val) {
                if (ok && missing) *missing = toks[t].tok;
                ok = false;
            } else {
                put_str(out, out_sz, &o, val);
            }
            p += len;
            matched = true;
            break;
        }

        if (!matched) {
            if (o + 1 < out_sz) out[o++] = *p;
            p++;
        }
    }
    out[o] = '\0';
    return ok;
}
