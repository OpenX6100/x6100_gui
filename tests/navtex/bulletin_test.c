/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * Test of the NAVTEX bulletin assembler (src/navtex/bulletin.c): texts fed
 * as received characters, and how they are cut into bulletins and flagged.
 *
 *   make bulletin_test && ./bulletin_test
 */
#include "bulletin.h"
#include "decoder.h"

#include <stdio.h>
#include <string.h>

#define MAX_OUT 16

static nb_bulletin_t out[MAX_OUT];
static int n_out;

static void done(const nb_bulletin_t *b, void *user)
{
    (void)user;
    if (n_out < MAX_OUT) out[n_out++] = *b;
}

/* the text, one character a second, then the signal goes */
static void feed(const char *text)
{
    n_out = 0;
    nb_assembler_t *a = nb_create(done, NULL);
    time_t t = 1000;
    nb_feed(a, NAVTEX_EV_SYNC_ON, NAVTEX_Q_EVENT, t);
    for (const char *p = text; *p; ++p) nb_feed(a, *p, NAVTEX_Q_CLEAN, t++);
    nb_feed(a, NAVTEX_EV_SYNC_OFF, NAVTEX_Q_EVENT, t);
    nb_tick(a, t + 30);
    nb_destroy(a);
}

static int failed;

/* kind: 'S' SITOR-B message, 'N' NAVTEX bulletin; head / tail: 1 = lost */
static void expect(int i, char kind, int head, int tail, const char *first)
{
    if (i >= n_out) {
        printf("  FAIL  bulletin %d missing\n", i + 1);
        failed++;
        return;
    }
    const nb_bulletin_t *b = &out[i];
    char st[80], dtg[80], t1[80], t2[80];
    nb_summary(b, st, dtg, t1, t2, sizeof(t1));
    bool ok = (kind == 'S') == b->sitor && head == b->lost_head && tail == b->lost_tail &&
              (!first || !strncmp(b->text, first, strlen(first)));
    printf("  %s  %d: %s%s%s  \"%s\"\n", ok ? "ok  " : "FAIL", i + 1, b->sitor ? "SITOR-B" : "NAVTEX",
           b->lost_head ? ", start lost" : "", b->lost_tail ? ", end lost" : "", t1);
    failed += !ok;
}

static void count(int n)
{
    if (n_out != n) {
        printf("  FAIL  %d bulletins, %d expected\n", n_out, n);
        failed++;
    }
}

/* as sent by a Russian coast station on 8416.5 kHz */
static const char sitorb[] =
    "EACH SIDE 1 MILE CENTRE LINE:\n"
    "A. 70-12.0N 162-30.0E\n"
    "B. 70-15.0N 162-45.0E\n"
    "HYDROGRAPHIC ENTERPRISE.\n"
    "\n\n"
    "190730 UTC JUN  26\n"
    "COASTAL WARNING EAST 6/26\n"
    "EAST SIBERIAN SEA.\n"
    "REKA KOLYMA.\n"
    "CHARTS RUS 19420, 19423.\n"
    "LIGHT AMBARCHIK UNLIT.\n"
    "HYDROGRAPHIC ENTERPRISE.\n"
    "\n\n"
    "181215 UTC JUN  26\n"
    "COASTAL WARNING EAST 5/26\n"
    "CHUKCHI SEA.\n"
    "CHARTS RUS 19400.\n"
    "DRIFTING BUOY IN 69-40.0N 172-10.0W.\n"
    "HYDROGRAPHIC ENTERPRISE.\n"
    "\n\n"
    "150900 UTC JUN  26\n"
    "COASTAL WARNING EAST 4/26\n"
    "LAPTEV SEA.\n"
    "CHARTS RUS 19300.\n"
    "DANGEROUS WRECK IN 73-10.0N 128-05.0E\n"
    "A. SHOAL 2.3 M";

/* the same, the signal gone after the blank lines of the last message */
static const char sitorb_end[] =
    "190730 UTC JUN  26\n"
    "COASTAL WARNING EAST 6/26\n"
    "EAST SIBERIAN SEA.\n"
    "REKA KOLYMA.\n"
    "LIGHT AMBARCHIK UNLIT.\n"
    "HYDROGRAPHIC ENTERPRISE.\n"
    "\n\n";

static const char navtex[] =
    "ZCZC MA64\n"
    "102320 UTC OCT 26\n"
    "ISTANBUL TURK RADYO\n"
    "NAVIGATIONAL WARNING NR 1234/26\n"
    "MARMARA SEA. MILITARY EXERCISES IN AREA\n"
    "40-50N 028-10E 40-55N 028-20E.\n"
    "NNNN\n"
    "\n"
    "ZCZC ME65\n"
    "102330 UTC OCT 26\n"
    "WEATHER FORECAST FOR MARMARA SEA\n"
    "WIND NORTHEAST 4 TO 5, SEA SLIGHT TO MODERATE.\n"
    "NNNN\n";

/* a NAVTEX bulletin whose ZCZC was lost: still NAVTEX, start lost */
static const char navtex_no_header[] =
    "102320 UTC OCT 26\n"
    "ISTANBUL TURK RADYO\n"
    "NAVIGATIONAL WARNING NR 1234/26\n"
    "MARMARA SEA. MILITARY EXERCISES IN AREA\n"
    "40-50N 028-10E 40-55N 028-20E.\n"
    "NNNN\n";

/* a NAVTEX bulletin cut by the end of the signal */
static const char navtex_cut[] =
    "ZCZC MA64\n"
    "102320 UTC OCT 26\n"
    "ISTANBUL TURK RADYO\n"
    "NAVIGATIONAL WARNING NR 1234/26\n"
    "MARMARA SEA. MILITARY EXERCISES IN AREA\n";

int main(void)
{
    printf("SITOR-B messages, first caught in the middle, last cut:\n");
    feed(sitorb);
    count(4);
    expect(0, 'N', 1, 0, "EACH SIDE");
    expect(1, 'S', 0, 0, "190730 UTC");
    expect(2, 'S', 0, 0, "181215 UTC");
    expect(3, 'S', 0, 1, "150900 UTC");

    printf("SITOR-B message, signal gone after it:\n");
    feed(sitorb_end);
    count(1);
    expect(0, 'S', 0, 0, "190730 UTC");

    printf("NAVTEX bulletins:\n");
    feed(navtex);
    count(2);
    expect(0, 'N', 0, 0, "102320 UTC");
    expect(1, 'N', 0, 0, "102330 UTC");

    printf("NAVTEX bulletin, ZCZC lost:\n");
    feed(navtex_no_header);
    count(1);
    expect(0, 'N', 1, 0, "102320 UTC");

    printf("NAVTEX bulletin, cut:\n");
    feed(navtex_cut);
    count(1);
    expect(0, 'N', 0, 1, "102320 UTC");

    printf("Saved and read back:\n");
    feed(sitorb);
    const char *path = "bulletin_test.tmp";
    nb_bulletin_t back;
    bool ok = nb_save(&out[1], path) && nb_load(&back, path) && back.sitor &&
              !back.lost_head && !back.lost_tail && back.len == out[1].len &&
              !memcmp(back.text, out[1].text, (size_t)back.len);
    remove(path);
    printf("  %s  SITOR-B message\n", ok ? "ok  " : "FAIL");
    failed += !ok;

    printf(failed ? "\n%d FAILED\n" : "\nall passed\n", failed);
    return failed != 0;
}
