/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - Olivia modulator to receiver, every variant the
 *  Mode button offers (src/olivia.c, olivia_mod.c, olivia_demod.c,
 *  olivia_rx.c; plain C, no radio)
 */

extern "C" {
#include "olivia_mod.h"
#include "olivia_rx.h"
}

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

const int RATE = 8000;      /* OLIVIA_RX_RATE and the modulator's rate */
const int MARGIN = 4;       /* olivia_app.c's TUNE_MARGIN */

struct variant {
    int tones;
    int bw;
};

/* olivia_tx.c's mode table */
const variant variants[] = {{8, 250}, {8, 500}, {16, 500}, {32, 1000}};

/* Repeatable Gaussian noise. */
struct noise {
    uint32_t s = 12345;

    double uni() {
        s = s * 1664525u + 1013904223u;
        return (s + 1.0) / 4294967297.0;
    }
    double gauss() { return std::sqrt(-2.0 * std::log(uni())) * std::cos(2.0 * M_PI * uni()); }
};

void on_text(void *user, const uint8_t *chars, int count) {
    auto *got = static_cast<std::string *>(user);

    for (int i = 0; i < count; i++) {
        if (chars[i]) {
            got->push_back((char)chars[i]);
        }
    }
}

/* Send `msg` with the sender `offset` Hz off the receiver's centre, at
 * `snr_db` in 2500 Hz, and return what the receiver printed. */
std::string loop(variant v, double offset, double snr_db, const std::string &msg) {
    olivia_mod_t mod;
    olivia_rx_t  rx;
    std::string  got;
    const double center = 1500.0;

    REQUIRE(olivia_mod_init(&mod, v.tones, v.bw, RATE, center + offset - v.bw / 2.0) == 0);
    olivia_mod_seed(&mod, 1234);
    REQUIRE(olivia_rx_init(&rx, v.tones, v.bw, RATE, center - v.bw / 2.0, MARGIN) == 0);
    olivia_rx_set_callback(&rx, on_text, &got);

    int                bits = mod.cfg.bits;
    int                blk = olivia_mod_block_samples(&mod);
    int                data = ((int)msg.size() + bits - 1) / bits;
    std::vector<float> audio;
    std::vector<float> buf(blk);

    /* a run-in of empty blocks, as olivia_tx.c sends */
    for (int b = 0; b < 4 + data + 1; b++) {
        uint8_t chars[OLIVIA_MAX_BITS] = {0};

        for (int i = 0; b >= 4 && i < bits; i++) {
            size_t k = (size_t)(b - 4) * bits + i;
            chars[i] = k < msg.size() ? (uint8_t)msg[k] : 0;
        }
        int n = olivia_mod_block(&mod, chars, buf.data());
        audio.insert(audio.end(), buf.begin(), buf.begin() + n);
    }
    int n = olivia_mod_flush(&mod, buf.data());
    audio.insert(audio.end(), buf.begin(), buf.begin() + n);

    double p = 0;
    for (float s : audio) {
        p += (double)s * s;
    }
    p /= audio.size();

    /* two more blocks of noise alone, so the last one is printed */
    audio.resize(audio.size() + 2 * blk, 0.0f);

    noise  rnd;
    double sd = std::sqrt(p / std::pow(10.0, snr_db / 10.0) * (RATE / 2.0) / 2500.0);
    for (float &s : audio) {
        s += (float)(sd * rnd.gauss());
    }

    olivia_rx_process(&rx, audio.data(), (int)audio.size());
    olivia_rx_free(&rx);
    olivia_mod_free(&mod);
    return got;
}

} // namespace

TEST_CASE("Olivia: every variant decodes its own signal", "[olivia]") {
    const std::string msg = "CQ CQ DE SP2KMO SP2KMO K ";

    for (variant v : variants) {
        double spacing = (double)v.bw / v.tones;

        INFO(v.tones << "/" << v.bw);
        CHECK(loop(v, 0.0, 30.0, msg).find(msg) != std::string::npos);
        /* tuned off by two and a half tones either way, in noise */
        CHECK(loop(v, 2.5 * spacing, -6.0, msg).find(msg) != std::string::npos);
        CHECK(loop(v, -2.5 * spacing, -6.0, msg).find(msg) != std::string::npos);
    }
}

TEST_CASE("Olivia: the receiver can be built for every variant", "[olivia]") {
    for (variant v : variants) {
        olivia_rx_t rx;

        INFO(v.tones << "/" << v.bw);
        REQUIRE(olivia_rx_init(&rx, v.tones, v.bw, RATE, 1500.0 - v.bw / 2.0, MARGIN) == 0);
        olivia_rx_free(&rx);
    }
}
