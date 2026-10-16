/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - the Next button of the keyboard modes
 */

#include "digi_next.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const STEPS[] = {"CQ", "Answer", "Report", "TNX", "73"};

static bool is_step(const char *name)
{
    for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
        if (name != NULL && strcasecmp(name, STEPS[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool digi_next_is_cq(const char *name)
{
    return name != NULL && strcasecmp(name, "CQ") == 0;
}

static int find(int count, digi_next_name_fn name, const char *want)
{
    for (int i = 0; i < count; i++) {
        const char *n = name(i);

        if (n != NULL && strcasecmp(n, want) == 0) {
            return i;
        }
    }
    return -1;
}

int digi_next_pick(const digi_next_t *s, const char *dx, int count, digi_next_name_fn name)
{
    const char *last = s->last;
    const char *want[3] = {NULL, NULL, NULL};

    if (dx == NULL || dx[0] == '\0') {
        want[0] = "CQ";
    } else {
        /* a new station starts over, unless it answered our CQ */
        if (strcasecmp(s->dx, dx) != 0 && !digi_next_is_cq(last)) {
            last = "";
        }
        if (last[0] == '\0') {
            want[0] = "Answer";
            want[1] = "Report";
        } else if (strcasecmp(last, "CQ") == 0) {
            want[0] = "Report";
            want[1] = "Answer";
        } else if (strcasecmp(last, "Answer") == 0) {
            want[0] = "TNX";
            want[1] = "Report";
            want[2] = "73";
        } else if (strcasecmp(last, "73") == 0) {
            want[0] = "CQ";
        } else {
            want[0] = "73";                     /* after Report or TNX */
        }
    }
    for (int k = 0; k < 3 && want[k] != NULL; k++) {
        int i = find(count, name, want[k]);

        if (i >= 0) {
            return i;
        }
    }
    return -1;
}

void digi_next_sent(digi_next_t *s, const char *name, const char *dx)
{
    if (!is_step(name)) {
        return;
    }
    snprintf(s->last, sizeof(s->last), "%s", name);
    snprintf(s->dx, sizeof(s->dx), "%s", (dx != NULL && !digi_next_is_cq(name)) ? dx : "");
}

void digi_next_label(const digi_next_t *s, const char *dx, int count, digi_next_name_fn name,
                     char *out, size_t size)
{
    int i = digi_next_pick(s, dx, count, name);

    if (i < 0) {
        snprintf(out, size, "Next\n(no macro)");
    } else if (digi_next_is_cq(name(i)) || dx == NULL || dx[0] == '\0') {
        snprintf(out, size, "Next:\n%s", name(i));
    } else {
        snprintf(out, size, "%s\n%s", name(i), dx);
    }
}
