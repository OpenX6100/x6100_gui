/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - text queue of the keyboard-mode transmitters
 *
 *  Lets the operator type, or send a macro, while a transmission is
 *  still going out: the new text joins the end of it instead of being
 *  refused. PSK31, Olivia and RTTY each own one queue; their transmit
 *  threads take characters from it as they go.
 *
 *  A queue is IDLE (no transmission), OPEN (transmitting, more text
 *  joins) or CLOSING (the last character has gone, the tail is being
 *  sent and the transmitter released). A thread that runs out of text
 *  sends its tail and then asks the queue whether anything arrived
 *  meanwhile, under the lock; only when nothing did does the queue close.
 *  Text put while it is CLOSING is kept too: when the thread finishes it
 *  finds it and transmits again at once, so nothing is ever refused or
 *  lost between the two.
 *
 *  While the operator has a text window open during a transmission the
 *  queue is held: a thread out of text then sends its mode's idle (PSK31
 *  reversals, RTTY LTRS, Olivia empty blocks) instead of ending, as
 *  fldigi does, so what is being typed joins the same transmission. A
 *  window left open does not keep the transmitter on for ever: the hold
 *  lapses DIGI_TXQ_HOLD_MAX_S after it was set or text was last added.
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIGI_TXQ_SIZE        2048
#define DIGI_TXQ_HOLD_MAX_S  120

typedef enum {
    DIGI_TXQ_IDLE,
    DIGI_TXQ_OPEN,
    DIGI_TXQ_CLOSING,
} digi_txq_state_t;

typedef struct {
    pthread_mutex_t  lock;
    char             buf[DIGI_TXQ_SIZE];
    size_t           head;      /* next character out */
    size_t           count;     /* characters waiting */
    digi_txq_state_t state;
    bool             hold;      /* idle rather than end when out of text */
    long             hold_since; /* monotonic seconds: hold set, or last text */
} digi_txq_t;

#define DIGI_TXQ_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, {0}, 0, 0, DIGI_TXQ_IDLE, false, 0}

typedef enum {
    DIGI_TXQ_STARTED,   /* nothing was running: start the transmit thread */
    DIGI_TXQ_ADDED,     /* the running thread will send it */
    DIGI_TXQ_FULL,      /* no room for all of it: nothing was added */
} digi_txq_put_t;

/* UI thread: queue `text`, whole or not at all. */
digi_txq_put_t digi_txq_put(digi_txq_t *q, const char *text);

/* Transmit thread: the next character, -1 when none is waiting. */
int            digi_txq_get(digi_txq_t *q);

/* Transmit thread, out of text: keep the transmitter on, idling? False
 * once the hold has lapsed (DIGI_TXQ_HOLD_MAX_S). */
bool           digi_txq_held(digi_txq_t *q);

/* UI thread: hold on (a text window opened while transmitting) or off
 * (it closed). Only takes while a transmission is OPEN; returns whether
 * the queue is held now. */
bool           digi_txq_hold(digi_txq_t *q, bool on);

/* Transmit thread, after its tail: true when nothing arrived meanwhile
 * and nothing is being typed (the queue is now CLOSING and the thread
 * ends), false when more text came or the queue is held (the thread
 * carries on). */
bool           digi_txq_close_if_empty(digi_txq_t *q);

/* Stop: forget what is waiting and drop the hold. The thread finishes
 * the character it is on and sends its tail. */
void           digi_txq_abort(digi_txq_t *q);

/* Transmit thread, with the transmitter released (or the caller, if the
 * thread could not be started): back to IDLE and false - or, when text
 * came in while the transmission was closing, OPEN again and true: the
 * thread transmits once more. */
bool           digi_txq_finish(digi_txq_t *q);

/* A transmission is running (OPEN or CLOSING). */
bool           digi_txq_running(digi_txq_t *q);

/* Characters not sent yet. */
size_t         digi_txq_pending(digi_txq_t *q);

#ifdef __cplusplus
}
#endif
