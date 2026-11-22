/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Tests for src/digi_txq.c: the text queue the PSK31, Olivia and RTTY
 *  transmitters stream from, so text typed during a transmission joins it.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <unistd.h>

extern "C" {
#include "digi_txq.h"
}

static void reset(digi_txq_t *q)
{
    digi_txq_abort(q);
    digi_txq_finish(q);
}

/* What a transmit thread does: take everything, then ask to close. */
static std::string drain(digi_txq_t *q)
{
    std::string out;
    int         c;

    while ((c = digi_txq_get(q)) >= 0) {
        out += (char)c;
    }
    return out;
}

TEST_CASE("first text starts a transmission, later text joins it", "[digi_txq]")
{
    static digi_txq_t q = DIGI_TXQ_INITIALIZER;

    reset(&q);
    REQUIRE_FALSE(digi_txq_running(&q));
    REQUIRE(digi_txq_put(&q, "CQ CQ") == DIGI_TXQ_STARTED);
    REQUIRE(digi_txq_running(&q));
    REQUIRE(digi_txq_put(&q, " DE SP2KMO") == DIGI_TXQ_ADDED);
    REQUIRE(digi_txq_pending(&q) == 15);
    REQUIRE(drain(&q) == "CQ CQ DE SP2KMO");
    REQUIRE(digi_txq_close_if_empty(&q));
    REQUIRE(digi_txq_running(&q));          /* the tail is still going out */
    digi_txq_finish(&q);
    REQUIRE_FALSE(digi_txq_running(&q));
}

TEST_CASE("text that arrives during the tail carries the transmission on", "[digi_txq]")
{
    static digi_txq_t q = DIGI_TXQ_INITIALIZER;

    reset(&q);
    REQUIRE(digi_txq_put(&q, "ABC") == DIGI_TXQ_STARTED);
    REQUIRE(drain(&q) == "ABC");
    /* the thread is sending its tail; the operator presses a macro */
    REQUIRE(digi_txq_put(&q, "DEF") == DIGI_TXQ_ADDED);
    REQUIRE_FALSE(digi_txq_close_if_empty(&q));
    REQUIRE(drain(&q) == "DEF");
    REQUIRE(digi_txq_close_if_empty(&q));
}

TEST_CASE("text put while closing makes the thread transmit again", "[digi_txq]")
{
    static digi_txq_t q = DIGI_TXQ_INITIALIZER;

    reset(&q);
    REQUIRE(digi_txq_put(&q, "X") == DIGI_TXQ_STARTED);
    drain(&q);
    REQUIRE(digi_txq_close_if_empty(&q));
    REQUIRE(digi_txq_put(&q, "late") == DIGI_TXQ_ADDED);
    REQUIRE(digi_txq_finish(&q));           /* again: OPEN, same thread */
    REQUIRE(digi_txq_running(&q));
    REQUIRE(drain(&q) == "late");
    REQUIRE(digi_txq_close_if_empty(&q));
    REQUIRE_FALSE(digi_txq_finish(&q));
    REQUIRE_FALSE(digi_txq_running(&q));
    REQUIRE(digi_txq_put(&q, "next") == DIGI_TXQ_STARTED);
}

TEST_CASE("a held queue idles instead of closing", "[digi_txq]")
{
    static digi_txq_t q = DIGI_TXQ_INITIALIZER;

    reset(&q);
    REQUIRE_FALSE(digi_txq_hold(&q, true));     /* nothing running: no hold */
    REQUIRE(digi_txq_put(&q, "A") == DIGI_TXQ_STARTED);
    REQUIRE(digi_txq_hold(&q, true));
    drain(&q);
    REQUIRE(digi_txq_held(&q));
    REQUIRE_FALSE(digi_txq_close_if_empty(&q));
    REQUIRE(digi_txq_put(&q, "B") == DIGI_TXQ_ADDED);
    digi_txq_hold(&q, false);
    REQUIRE(drain(&q) == "B");
    REQUIRE(digi_txq_close_if_empty(&q));
}

TEST_CASE("stop drops what waits and the hold", "[digi_txq]")
{
    static digi_txq_t q = DIGI_TXQ_INITIALIZER;

    reset(&q);
    REQUIRE(digi_txq_put(&q, "a long message") == DIGI_TXQ_STARTED);
    REQUIRE(digi_txq_hold(&q, true));
    REQUIRE(digi_txq_get(&q) == 'a');
    digi_txq_abort(&q);
    REQUIRE(digi_txq_get(&q) == -1);
    REQUIRE_FALSE(digi_txq_held(&q));
    REQUIRE(digi_txq_close_if_empty(&q));
    REQUIRE_FALSE(digi_txq_finish(&q));
}

TEST_CASE("text that does not fit is not added at all", "[digi_txq]")
{
    static digi_txq_t q = DIGI_TXQ_INITIALIZER;
    std::string       big(DIGI_TXQ_SIZE - 2, 'x');

    reset(&q);
    REQUIRE(digi_txq_put(&q, big.c_str()) == DIGI_TXQ_STARTED);
    REQUIRE(digi_txq_put(&q, "abc") == DIGI_TXQ_FULL);
    REQUIRE(digi_txq_pending(&q) == big.size());
    REQUIRE(digi_txq_put(&q, "ab") == DIGI_TXQ_ADDED);
    /* across the end of the ring */
    for (int i = 0; i < 10; i++) {
        REQUIRE(digi_txq_get(&q) == 'x');
    }
    REQUIRE(digi_txq_put(&q, "0123456789") == DIGI_TXQ_ADDED);
    REQUIRE(drain(&q) == std::string(DIGI_TXQ_SIZE - 12, 'x') + "ab0123456789");
}

/* A transmit thread and a typist at once: every character goes out
 * exactly once and in order, whatever the timing. */
TEST_CASE("typing while a thread sends loses nothing", "[digi_txq]")
{
    static digi_txq_t q = DIGI_TXQ_INITIALIZER;

    for (int round = 0; round < 20; round++) {
        std::string       sent, typed;
        std::atomic<bool> done{false};

        reset(&q);
        REQUIRE(digi_txq_put(&q, "start ") == DIGI_TXQ_STARTED);
        typed = "start ";

        std::thread tx([&] {
            do {
                for (;;) {
                    int c = digi_txq_get(&q);

                    if (c >= 0) {
                        sent += (char)c;
                        continue;
                    }
                    usleep(50);         /* the tail */
                    if (digi_txq_close_if_empty(&q)) {
                        break;
                    }
                }
                usleep(30);             /* the transmitter released */
            } while (digi_txq_finish(&q));
            done = true;
        });

        for (int i = 0; i < 200 && !done; i++) {
            std::string word = "w" + std::to_string(i) + " ";

            digi_txq_put_t r = digi_txq_put(&q, word.c_str());

            if (r == DIGI_TXQ_ADDED) {
                typed += word;
            } else if (r == DIGI_TXQ_STARTED) {
                digi_txq_abort(&q);     /* a new transmission: not this test's */
                digi_txq_finish(&q);
                break;
            }
            usleep((i * 7919 + round * 104729) % 120);
        }
        tx.join();
        REQUIRE(sent == typed);
        REQUIRE_FALSE(digi_txq_running(&q));
    }
}
