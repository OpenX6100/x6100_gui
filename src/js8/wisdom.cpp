/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 decoder setup: FFTW wisdom
 */

#include "wisdom.hpp"

#include "cpu_fence.hpp"
#include "datafile.hpp"
#include "js8_wisdom.h"
#include "receiver.hpp"
#include "js8core/decoder.hpp"
#include "js8core/protocol/submode.hpp"

#include <fftw3.h>

#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#endif

// js8core's lock around FFTW's planner, which isn't thread-safe: plans are
// made and destroyed under it on the decode threads (legacy_decoder.cpp).
// Importing and exporting wisdom use the planner too.
extern std::mutex fftw_mutex;

namespace x6100::js8 {

namespace {

constexpr int RATE = js8core::kJs8RxSampleRate;

// The speeds, in the decoder's order (fastest first, as legacy_decode()).
constexpr int SPEEDS[] = {SUBMODE_ULTRA, SUBMODE_SLOW, SUBMODE_TURBO, SUBMODE_FAST, SUBMODE_NORMAL};
constexpr unsigned SPEED_COUNT = sizeof(SPEEDS) / sizeof(SPEEDS[0]);

// Guards DecoderSetup::saved_ and the file.
std::mutex &setup_mutex() {
    static std::mutex m;
    return m;
}

// The entries of FFTW's wisdom text, "(solver flags... hash)" each, spaces
// evened out. FFTW writes them in its hash table's order, which isn't the
// same after a reload.
std::set<std::string> wisdom_entries(const std::string &text) {
    std::set<std::string> entries;
    std::size_t           open = text.find('(', 1); // past the header's own "("
    while (open != std::string::npos) {
        std::size_t close = text.find(')', open);
        if (close == std::string::npos) break;
        std::string entry;
        for (std::size_t i = open; i <= close; i++) {
            char c     = text[i];
            bool space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
            if (space && entry.back() == ' ') continue;
            entry += space ? ' ' : c;
        }
        entries.insert(entry);
        open = text.find('(', close);
    }
    return entries;
}

} // namespace

bool wisdom_has_new(const std::string &now, const std::string &before) {
    auto had = wisdom_entries(before);
    for (const auto &e : wisdom_entries(now))
        if (!had.count(e)) return true;
    return false;
}

std::string export_wisdom() {
    std::lock_guard<std::mutex> lock(fftw_mutex);
    char *w = fftwf_export_wisdom_to_string();
    if (!w) return {};
    std::string text(w);
    fftwf_free(w);
    return text;
}

bool import_wisdom(const std::string &text) {
    std::lock_guard<std::mutex> lock(fftw_mutex);
    if (!text.empty() && fftwf_import_wisdom_from_string(text.c_str())) return true;
    fftwf_forget_wisdom();
    return false;
}

void forget_wisdom() {
    std::lock_guard<std::mutex> lock(fftw_mutex);
    fftwf_forget_wisdom();
}

double build_decoder(int submode_bit) {
    auto start = std::chrono::steady_clock::now();
    std::thread t([submode_bit] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "js8-setup");
#endif
        fence_decoder_thread(0); // off the screen's core, as the decode threads

        // A minute of silence, the speed's slot at its start. Nothing is
        // found in it, so the decode after the decoder is built is short.
        js8core::DecodeState state;
        state.samples.assign((std::size_t)js8core::kJs8NtMax * RATE, 0);
        state.params.nsubmodes = submode_bit;
        state.params.nfa       = 200;
        state.params.nfb       = 3000;
        state.params.nfqso     = 1500;
        state.params.newdat    = true;
        state.params.kin       = (int)state.samples.size();
        for (const auto &sm : js8core::protocol::submodes()) {
            int size = sm.tx_seconds * RATE;
            switch (sm.id) {
            case js8core::protocol::SubmodeId::A: state.params.kszA = size; break;
            case js8core::protocol::SubmodeId::B: state.params.kszB = size; break;
            case js8core::protocol::SubmodeId::C: state.params.kszC = size; break;
            case js8core::protocol::SubmodeId::E: state.params.kszE = size; break;
            case js8core::protocol::SubmodeId::I: state.params.kszI = size; break;
            }
        }
        js8core::legacy_decode(state, nullptr);
    });
    t.join();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

DecoderSetup &DecoderSetup::get() {
    // Never destroyed: a setup thread may still be running at exit.
    static DecoderSetup *setup = new DecoderSetup;
    return *setup;
}

bool DecoderSetup::begin(const std::string &path) {
    std::lock_guard<std::mutex> lock(setup_mutex());
    if (ready_) return true;
    if (running_) return false;

    // Loaded once per program: FFTW keeps it from then on.
    DataFile file = read_data_file(path);
    if (file.exists && import_wisdom(file.text)) {
        saved_ = file.text;
        ready_ = true;
        return true;
    }

    // Missing, damaged or from another FFTW: measure everything again.
    saved_.clear();
    done_     = 0;
    running_  = true;
    std::thread([this, path] { run(path); }).detach();
    return false;
}

void DecoderSetup::run(std::string path) {
    auto start = std::chrono::steady_clock::now();
    // One speed at a time, each decoder dropped before the next is built:
    // the memory of one, not all five.
    for (int speed : SPEEDS) {
        build_decoder(speed);
        done_++;
    }
    std::string text = export_wisdom();
    {
        std::lock_guard<std::mutex> lock(setup_mutex());
        if (!text.empty() && write_data_file(path, text)) saved_ = text;
        setup_s_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        ready_   = true;
        running_ = false;
    }
}

bool DecoderSetup::running(unsigned *speeds_done) const {
    if (speeds_done) *speeds_done = done_.load();
    return running_.load();
}

void DecoderSetup::save(const std::string &path) {
    std::lock_guard<std::mutex> lock(setup_mutex());
    if (!ready_ || running_) return;
    std::string text = export_wisdom();
    if (text.empty() || !wisdom_has_new(text, saved_)) return;
    if (write_data_file(path, text)) saved_ = text;
}

void DecoderSetup::reset() {
    std::lock_guard<std::mutex> lock(setup_mutex());
    if (running_) return;
    forget_wisdom();
    saved_.clear();
    ready_   = false;
    done_    = 0;
    setup_s_ = 0;
}

} // namespace x6100::js8

// ---- C interface (js8_wisdom.h) ----------------------------------------

using x6100::js8::DecoderSetup;

extern "C" bool js8_decoder_setup_begin(const char *path) {
    return DecoderSetup::get().begin(path ? path : "");
}

extern "C" bool js8_decoder_setup_running(unsigned *speeds_done) {
    return DecoderSetup::get().running(speeds_done);
}

extern "C" unsigned js8_decoder_setup_speeds(void) { return x6100::js8::SPEED_COUNT; }

extern "C" double js8_decoder_setup_seconds(void) { return DecoderSetup::get().last_setup_s(); }

extern "C" void js8_decoder_setup_save(const char *path) {
    if (path) DecoderSetup::get().save(path);
}
