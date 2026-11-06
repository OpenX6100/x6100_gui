/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - text queue of the keyboard-mode transmitters
 */

#include "digi_txq.h"

#include <string.h>
#include <time.h>

static long now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

digi_txq_put_t digi_txq_put(digi_txq_t *q, const char *text)
{
    size_t         len = text ? strlen(text) : 0;
    digi_txq_put_t res;

    pthread_mutex_lock(&q->lock);
    if (len > DIGI_TXQ_SIZE - (q->state == DIGI_TXQ_IDLE ? 0 : q->count)) {
        res = DIGI_TXQ_FULL;
    } else {
        if (q->state == DIGI_TXQ_IDLE) {
            q->head = 0;
            q->count = 0;
            q->state = DIGI_TXQ_OPEN;
            res = DIGI_TXQ_STARTED;
        } else {
            res = DIGI_TXQ_ADDED;
        }
        q->hold_since = now_s();
        for (size_t i = 0; i < len; i++) {
            q->buf[(q->head + q->count) % DIGI_TXQ_SIZE] = text[i];
            q->count++;
        }
    }
    pthread_mutex_unlock(&q->lock);
    return res;
}

int digi_txq_get(digi_txq_t *q)
{
    int c = -1;

    pthread_mutex_lock(&q->lock);
    if (q->count > 0) {
        c = (unsigned char)q->buf[q->head];
        q->head = (q->head + 1) % DIGI_TXQ_SIZE;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return c;
}

bool digi_txq_close_if_empty(digi_txq_t *q)
{
    bool closed;

    pthread_mutex_lock(&q->lock);
    if (q->hold && now_s() - q->hold_since > DIGI_TXQ_HOLD_MAX_S) {
        q->hold = false;
    }
    closed = (q->count == 0 && !q->hold);
    if (closed && q->state == DIGI_TXQ_OPEN) {
        q->state = DIGI_TXQ_CLOSING;
    }
    pthread_mutex_unlock(&q->lock);
    return closed;
}

bool digi_txq_held(digi_txq_t *q)
{
    bool h;

    pthread_mutex_lock(&q->lock);
    if (q->hold && now_s() - q->hold_since > DIGI_TXQ_HOLD_MAX_S) {
        q->hold = false;            /* a window left open: let it end */
    }
    h = q->hold;
    pthread_mutex_unlock(&q->lock);
    return h;
}

bool digi_txq_hold(digi_txq_t *q, bool on)
{
    bool h;

    pthread_mutex_lock(&q->lock);
    q->hold = on && q->state == DIGI_TXQ_OPEN;
    q->hold_since = now_s();
    h = q->hold;
    pthread_mutex_unlock(&q->lock);
    return h;
}

void digi_txq_abort(digi_txq_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->count = 0;
    q->hold = false;
    if (q->state == DIGI_TXQ_OPEN) {
        q->state = DIGI_TXQ_CLOSING;
    }
    pthread_mutex_unlock(&q->lock);
}

bool digi_txq_finish(digi_txq_t *q)
{
    bool again;

    pthread_mutex_lock(&q->lock);
    again = (q->count > 0);
    q->hold = false;
    q->state = again ? DIGI_TXQ_OPEN : DIGI_TXQ_IDLE;
    pthread_mutex_unlock(&q->lock);
    return again;
}

bool digi_txq_running(digi_txq_t *q)
{
    bool r;

    pthread_mutex_lock(&q->lock);
    r = (q->state != DIGI_TXQ_IDLE);
    pthread_mutex_unlock(&q->lock);
    return r;
}

size_t digi_txq_pending(digi_txq_t *q)
{
    size_t n;

    pthread_mutex_lock(&q->lock);
    n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}
