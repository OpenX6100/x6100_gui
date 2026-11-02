/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - tests for the digital modes' drive level
 *  (src/tx_level.c), against a model of the radio
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "tx_level.h"

// ---- The drive level (src/tx_level.c) ---------------------------------------

namespace {
// A model of the radio as tx_player.c sees it, a 2048-sample block (43 ms)
// at a time: the output follows the drive up to the set power; past that
// the base's ALC holds it there and shows how far over it is. The readings
// come back late (the meter's lag) and in 0.1 W steps, a little noisy.
struct ModelRadio {
    float              set_w, full_db; // the drive (dB) that just gives set_w
    size_t             delay  = 8;      // blocks late (8: about 0.35 s)
    float              smooth = 0.25f;  // the meter's own lag
    bool               round_p = false; // the power reading rounded, not cut down
    std::mt19937       rng{7};
    std::vector<float> lag_p, lag_a;    // readings in flight
    float              meter_p = 0, meter_a = 0;
    float true_power(float g) const { return g <= full_db ? set_w * powf(10.f, (g - full_db) / 10.f) : set_w; }
    float true_alc(float g) const { return g <= full_db ? 0.f : std::min(10.f, (g - full_db) * 2.f); }
    // One block at drive g: the reading the player gets after it.
    void block(float g, float *pwr, float *alc) {
        lag_p.push_back(true_power(g));
        lag_a.push_back(true_alc(g));
        if (lag_p.size() > delay) {
            meter_p += (lag_p.front() - meter_p) * smooth;
            meter_a += (lag_a.front() - meter_a) * smooth;
            lag_p.erase(lag_p.begin());
            lag_a.erase(lag_a.begin());
        }
        std::uniform_real_distribution<float> n(-0.03f, 0.03f), m(-0.1f, 0.1f);
        float w = std::max(0.f, meter_p + n(rng)) * 10.f; // 0.1 W steps
        *pwr = (round_p ? roundf(w) : floorf(w)) / 10.f;
        *alc = roundf(std::max(0.f, meter_a + m(rng)) * 10.f) / 10.f;
    }
};

constexpr int FRAME_BLOCKS = 296; // a 12.64 s JS8 Normal frame at 48 kHz

// The loop before (tx_player.c up to beta 4.5): every block from the latest
// reading, after 30 blocks.
float old_correction(float target, float pwr, float alc) {
    if (alc > 0.5f) return log10f(log10f(11.1f - alc)) * 20.0f - 0.38f;
    if (pwr < target * 0.8f && target - pwr > 0.1f) return std::min(3.0f, log10f(target / (pwr + 0.01f)) * 10.0f);
    return 0.0f;
}

struct FrameStats {
    float min_w, max_w, mean_w, mean_alc;
    int   turns; // times the drive changed direction (up to down or back)
};

// Frames one after another, the gain carried over (the learned offset).
// every: the radio sends a reading after one block in `every` (1: after each).
std::vector<FrameStats> run_frames(bool old_loop, float set_w, float full_db, float start_db, int frames,
                                   size_t delay = 8, float smooth = 0.25f, bool round_p = false, int every = 1) {
    ModelRadio r{set_w, full_db};
    r.delay   = delay;
    r.smooth  = smooth;
    r.round_p = round_p;
    float      g = start_db;
    std::vector<FrameStats> out;
    for (int f = 0; f < frames; f++) {
        tx_level_t l;
        tx_level_start(&l, set_w);
        FrameStats st{1e9f, 0, 0, 0, 0};
        int        n = 0, last_dir = 0;
        for (int b = 0; b < FRAME_BLOCKS; b++) {
            float pwr, alc;
            r.block(g, &pwr, &alc);
            if (b >= 60) { // the steady part
                float p = r.true_power(g);
                st.min_w = std::min(st.min_w, p);
                st.max_w = std::max(st.max_w, p);
                st.mean_w += p;
                st.mean_alc += r.true_alc(g);
                n++;
            }
            bool  have = b % every == every - 1;
            float d = old_loop ? (b > 30 && have ? old_correction(set_w, pwr, alc) * 0.4f : 0.0f)
                               : tx_level_block(&l, have, pwr, alc);
            int   dir = d > 0 ? 1 : d < 0 ? -1 : 0;
            if (dir && last_dir && dir != last_dir) st.turns++;
            if (dir) last_dir = dir;
            g = std::clamp(g + d, -30.0f, 0.0f);
        }
        st.mean_w /= n;
        st.mean_alc /= n;
        out.push_back(st);
        r.lag_p.clear(); // the radio unkeys between frames
        r.lag_a.clear();
        r.meter_p = r.meter_a = 0;
    }
    return out;
}

float swing_db(const FrameStats &s) {
    return 10.0f * log10f(s.max_w / std::max(s.min_w, 1e-4f));
}
} // namespace

TEST_CASE("drive level at 0.3 W into an amplifier: steady within a frame, not swinging", "[txlevel]") {
    // VE7NHW's case: 0.3 W into an XPA125B, the learned drive a little hot.
    auto old_f = run_frames(true, 0.3f, -14.0f, -12.0f, 10);
    auto new_f = run_frames(false, 0.3f, -14.0f, -12.0f, 10);
    float old_swing = 0, new_swing = 0;
    for (int f = 1; f < 10; f++) {
        old_swing = std::max(old_swing, swing_db(old_f[f]));
        new_swing = std::max(new_swing, swing_db(new_f[f]));
        INFO("frame " << f << ": old " << old_f[f].min_w << "-" << old_f[f].max_w << " W, new " << new_f[f].min_w
                      << "-" << new_f[f].max_w << " W (mean " << new_f[f].mean_w << ", ALC " << new_f[f].mean_alc
                      << ")");
        CHECK(new_f[f].mean_w >= 0.3f * 0.7f); // within 1.5 dB of the setting
        CHECK(new_f[f].mean_alc <= 1.0f);      // not driven into the ALC
        CHECK(new_f[f].turns <= 1);            // no see-saw
    }
    std::printf("[txlevel] 0.3 W: within-frame swing old %.1f dB, new %.2f dB\n", old_swing, new_swing);
    CHECK(old_swing > 2.0f);  // the old loop swings (as the radio did)
    CHECK(new_swing < 0.8f);  // the new one holds from the second frame on
}

TEST_CASE("drive level: starting far too low or too hot, it settles within two transmissions", "[txlevel]") {
    for (float set : {0.3f, 1.0f, 5.0f}) {
        INFO("set " << set << " W");
        // 6 dB too low: ramped up live in the first, steady from the second.
        auto low = run_frames(false, set, -14.0f, -20.0f, 6);
        CHECK(low[0].min_w < set * 0.6f); // it started short (measured from 2.6 s, the ramp already going)
        CHECK(low[0].turns <= 1); // a ramp, not a see-saw
        for (int f = 1; f < 6; f++) {
            CHECK(low[f].mean_w >= set * 0.7f);
            CHECK(low[f].mean_alc <= 1.0f);
            CHECK(swing_db(low[f]) < 0.8f);
        }
        // 10 dB too low: there by the second transmission (8 dB a transmission at most).
        auto far = run_frames(false, set, -14.0f, -24.0f, 6);
        CHECK(far[1].max_w >= set * 0.7f);
        for (int f = 2; f < 6; f++) CHECK(swing_db(far[f]) < 0.8f);
        // 10 dB too hot: down within the first two, steady after.
        auto hot = run_frames(false, set, -14.0f, -4.0f, 10);
        CHECK(hot[0].mean_alc > 1.0f);
        CHECK(hot[2].mean_alc <= 1.0f);
        CHECK(hot[9].mean_w >= set * 0.7f);
        CHECK(swing_db(hot[9]) < 0.8f);
    }
}

TEST_CASE("drive level: right however few readings the radio sends", "[txlevel]") {
    // A reading doesn't come after every block: 6-7 in each half second on
    // VE7NHW's radio, and IU1WXY reports about 2.5 a second on his (one in 9
    // blocks). A window is half a second of blocks, whatever readings came in
    // it, so the drive comes up at the same pace either way.
    for (float set : {0.3f, 1.0f, 5.0f}) {
        INFO("set " << set << " W");
        auto low = run_frames(false, set, -14.0f, -20.0f, 6, 8, 0.25f, false, 9); // 6 dB too low
        for (int f = 1; f < 6; f++) {
            INFO("frame " << f << ": " << low[f].min_w << "-" << low[f].max_w << " W, ALC " << low[f].mean_alc);
            CHECK(low[f].mean_w >= set * 0.7f);
            CHECK(low[f].mean_alc <= 1.0f);
            CHECK(swing_db(low[f]) < 0.8f);
        }
        auto hot = run_frames(false, set, -14.0f, -4.0f, 10, 8, 0.25f, false, 9); // 10 dB too hot
        CHECK(hot[3].mean_alc <= 1.0f);
        CHECK(hot[9].mean_w >= set * 0.7f);
        CHECK(swing_db(hot[9]) < 0.8f);
    }
    // No reading at all in a window: no decision until one comes.
    tx_level_t l;
    tx_level_start(&l, 5.0f);
    float total = 0;
    for (int b = 0; b < TX_LEVEL_SETTLE_BLOCKS + 3 * TX_LEVEL_WINDOW_BLOCKS; b++) total += tx_level_block(&l, false, 0, 0);
    CHECK(total == 0.0f);
    CHECK(tx_level_block(&l, true, 1.0f, 0.0f) == TX_LEVEL_UP_FAST_DB); // the late reading decides at once
}

TEST_CASE("drive level: ramps up while the ALC reads zero, holds once it shows", "[txlevel]") {
    tx_level_t l;
    tx_level_start(&l, 0.3f);
    for (int b = 0; b < TX_LEVEL_SETTLE_BLOCKS; b++) CHECK(tx_level_block(&l, true, 0.0f, 0.0f) == 0.0f); // settling
    float total = 0;
    for (int b = 0; b < 400; b++) total += tx_level_block(&l, true, 0.0f, 0.0f); // reading nothing at all
    CHECK(total == Catch::Approx(TX_LEVEL_UP_MAX_DB)); // up, but no further than the limit
    tx_level_start(&l, 0.3f);
    for (int b = 0; b < TX_LEVEL_SETTLE_BLOCKS; b++) tx_level_block(&l, true, 0.3f, 9.0f);
    float d = 0;
    for (int b = 0; b < TX_LEVEL_WINDOW_BLOCKS; b++) d += tx_level_block(&l, true, 0.3f, 9.0f);
    CHECK(d == -TX_LEVEL_DOWN_MAX_DB); // overdriven: down, limited
    total = 0;
    for (int b = 0; b < 200; b++) total += tx_level_block(&l, true, 0.0f, 0.0f);
    CHECK(total == 0.0f); // it had to come down: no ramping up after, this transmission
    tx_level_start(&l, 0.3f);
    total = 0;
    for (int b = 0; b < 200; b++) total += tx_level_block(&l, true, 0.2f, 0.3f); // the ALC shows a little
    CHECK(total == 0.0f); // there: held
    tx_level_start(&l, 0.3f);
    total = 0;
    for (int b = 0; b < 30 + 2 * TX_LEVEL_WINDOW_BLOCKS; b++) total += tx_level_block(&l, true, 0.2f, 0.1f); // a stray 0.1
    CHECK(total == Catch::Approx(2 * TX_LEVEL_UP_SLOW_DB)); // not the ALC yet: still up
    tx_level_start(&l, 0.3f);
    total = 0;
    for (int b = 0; b < 200; b++) total += tx_level_block(&l, true, 0.3f, 0.0f); // reads the setting
    CHECK(total == 0.0f);
    tx_level_start(&l, 0.3f);
    total = 0;
    for (int b = 0; b < 30 + 2 * TX_LEVEL_WINDOW_BLOCKS; b++) total += tx_level_block(&l, true, 0.2f, 0.0f); // near
    CHECK(total == Catch::Approx(2 * TX_LEVEL_UP_SLOW_DB)); // gently
    tx_level_start(&l, 0.3f);
    total = 0;
    for (int b = 0; b < 30 + 2 * TX_LEVEL_WINDOW_BLOCKS; b++) total += tx_level_block(&l, true, 0.1f, 0.0f); // far
    CHECK(total == Catch::Approx(2 * TX_LEVEL_UP_FAST_DB)); // faster
}

TEST_CASE("drive level: steady whatever the meter's lag and rounding", "[txlevel]") {
    for (size_t delay : {0u, 8u, 16u})
        for (float smooth : {1.0f, 0.25f, 0.1f})
            for (bool round_p : {false, true})
                for (float start : {-24.0f, -16.0f, -12.0f, -4.0f})
                for (int every : {1, 2, 9}) { // a reading after every block, every other one, or one in 9
                    auto f = run_frames(false, 0.3f, -14.0f, start, 8, delay, smooth, round_p, every);
                    INFO("delay " << delay << " smooth " << smooth << " round " << round_p << " start " << start
                                  << " every " << every
                                  << ": frames 3-7 e.g. " << f[3].min_w << "-" << f[3].max_w << " W, ALC "
                                  << f[3].mean_alc);
                    for (int i = 0; i < 8; i++) CHECK(f[i].turns <= 2); // never a see-saw
                    for (int i = 3; i < 8; i++) {
                        CHECK(swing_db(f[i]) < 0.8f);
                        CHECK(f[i].mean_w >= 0.3f * 0.7f);
                        CHECK(f[i].mean_alc <= 1.0f);
                    }
                }
}

TEST_CASE("drive level: just short with the ALC at zero, there in the first transmission", "[txlevel]") {
    // VE7NHW on 4.6: the ALC never left 0.0 and the amp crept up a watt or
    // two a transmission. 1.5 dB short reads 0.2 W of 0.3.
    auto f = run_frames(false, 0.3f, -14.0f, -15.5f, 6);
    std::printf("[txlevel] 1.5 dB short: first transmission %.2f-%.2f W (ALC %.2f), second %.2f-%.2f W\n",
                f[0].min_w, f[0].max_w, f[0].mean_alc, f[1].min_w, f[1].max_w);
    CHECK(f[0].max_w >= 0.29f);   // at the setting within the first
    CHECK(f[0].turns <= 1);       // a smooth ramp up (at most a small step back)
    for (int i = 1; i < 6; i++) { // steady from the second
        CHECK(swing_db(f[i]) < 0.8f);
        CHECK(f[i].mean_w >= 0.28f);
        CHECK(f[i].mean_alc <= 1.0f);
    }
}
