/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 automatic time sync (see timesync.hpp)
 */

#include "timesync.hpp"

#include "render.hpp"
#include "cpu_fence.hpp"

#include "js8core/decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <variant>

#if defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace x6100::js8 {

namespace {

constexpr int RATE         = js8core::kJs8RxSampleRate;
constexpr int WINDOW       = TimeSearch::WINDOW_MS * RATE / 1000;
constexpr int STEP         = TimeSearch::STEP_MS * RATE / 1000;
constexpr int NORMAL       = 0; ///< desktop's submode numbers
constexpr int SLOW         = 4;
constexpr int NORMAL_BIT   = 1 << 0; ///< js8core's nsubmodes bit for Normal (A)

std::int64_t steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/// `v` into (-period/2, period/2].
std::int64_t centred(std::int64_t v, std::int64_t period) {
    v %= period;
    if (v > period / 2) v -= period;
    else if (v <= -period / 2) v += period;
    return v;
}

} // namespace

bool auto_sync_submode(int submode) {
    return submode == NORMAL || submode == SLOW;
}

std::int64_t drift_short_way(std::int64_t suggested_ms, std::int64_t current_ms, std::int64_t period_ms) {
    if (period_ms <= 0) period_ms = 15000;
    // processDecodeEvent.cpp: the correction the short way round the slot,
    // added to the drift now, then kept within one period (C++'s % keeps
    // the sign, as desktop's two branches do).
    return (current_ms + centred(suggested_ms - current_ms, period_ms)) % period_ms;
}

void AutoTimeSync::frame(int submode, std::int64_t suggested_ms, std::int64_t current_ms) {
    if (!auto_sync_submode(submode)) return;
    queue_.push_back(drift_short_way(suggested_ms, current_ms, submode == SLOW ? 30000 : 15000));
}

std::optional<std::int64_t> AutoTimeSync::pass_done(std::int64_t current_ms, unsigned *frames) {
    if (frames) *frames = (unsigned)queue_.size();
    if (queue_.empty()) return std::nullopt;
    if (n_ == 0) { // desktop: the average starts at the drift in effect...
        n_   = 1;
        mma_ = current_ms;
    }
    for (std::int64_t d : queue_) { // ...so the first frame replaces it
        mma_ = ((n_ - 1) * mma_ + d) / n_;
        if (n_ < MAX_N) n_++;
    }
    queue_.clear();
    return mma_;
}

void AutoTimeSync::restart(std::int64_t drift_ms, bool keep) {
    queue_.clear();
    mma_ = drift_ms;
    n_   = keep ? 2 : 0;
}

std::int64_t TimeSearch::drift_for(std::int64_t window_start_sys_ms, float xdt, std::int64_t period_ms) {
    // On time means starting on a slot boundary of JS8's time (system clock
    // plus the drift): the drift is minus where it started, modulo the slot.
    std::int64_t start = window_start_sys_ms + (std::int64_t)std::lround(xdt * 1000.0f);
    return centred(-start, period_ms);
}

TimeSearch::TimeSearch(Done done) : done_(std::move(done)), ring_(WINDOW) {}

TimeSearch::~TimeSearch() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        active_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TimeSearch::start(std::int64_t max_ms, int low_hz, int high_hz, int qso_hz) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_++;
        deadline_ms_ = steady_ms() + max_ms;
        low_hz_      = low_hz;
        high_hz_     = high_hz;
        qso_hz_      = qso_hz;
        head_ = filled_ = since_ = 0;
        active_ = true;
        if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void TimeSearch::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_++;
        active_ = false;
    }
    cv_.notify_all();
}

void TimeSearch::feed(const std::int16_t *pcm, std::size_t n, std::int64_t end_sys_ms) {
    if (!active_.load()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.load()) return;
        for (std::size_t i = 0; i < n; i++) {
            ring_[head_] = pcm[i];
            head_        = (head_ + 1) % ring_.size();
        }
        filled_     = std::min(ring_.size(), filled_ + n);
        since_     += n;
        end_sys_ms_ = end_sys_ms;
    }
    cv_.notify_all();
}

TimeSearch::Result TimeSearch::decode_window(const std::int16_t *samples, std::int64_t window_start_sys_ms,
                                             int low_hz, int high_hz, int qso_hz) {
    // js8core's ring: a minute of audio, the window at its start.
    js8core::DecodeState state;
    state.samples.assign((std::size_t)js8core::kJs8NtMax * RATE, 0);
    std::copy(samples, samples + WINDOW, state.samples.begin());
    state.params.nsubmodes = NORMAL_BIT;
    state.params.kposA     = 0;
    state.params.kszA      = WINDOW;
    state.params.kin       = WINDOW;
    state.params.nfa       = low_hz;
    state.params.nfb       = high_hz;
    state.params.nfqso     = qso_hz;
    state.params.newdat    = true;

    Result        best;
    FrameRenderer renderer;
    js8core::legacy_decode(state, [&](js8core::events::Variant const &ev) {
        auto d = std::get_if<js8core::events::Decoded>(&ev);
        if (!d || d->mode != NORMAL || d->quality < LOW_CONFIDENCE_QUALITY) return;
        if (best.found && d->snr <= best.snr) return; // the strongest says it best
        int type      = d->type;
        best.found    = true;
        best.drift_ms = drift_for(window_start_sys_ms, d->xdt);
        best.snr      = d->snr;
        best.freq_hz  = d->frequency;
        best.text     = renderer.render(d->data, &type, d->frequency);
    });
    return best;
}

std::int64_t ClockStepWatch::check(std::int64_t wall_ms, std::int64_t mono_ms) {
    const std::int64_t offset = wall_ms - mono_ms;
    if (!have_) {
        have_   = true;
        offset_ = offset;
        return 0;
    }
    const std::int64_t step = offset - offset_;
    offset_                 = offset; // a slew follows along, a step is reported once
    return std::llabs(step) >= THRESHOLD_MS ? step : 0;
}

void TimeSearch::loop() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "js8-tsearch");
    // Below everything else: the decoder, the screen and the audio come first.
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    fence_decoder_thread(0); // off the screen's core too, as the decode threads
    std::vector<std::int16_t> window(WINDOW);

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
            return quit_ || (active_ && filled_ >= ring_.size() && since_ >= (std::size_t)STEP);
        });
        if (quit_) return;
        if (!active_) continue;
        unsigned gen = generation_;
        if (steady_ms() >= deadline_ms_) {
            active_ = false;
            lock.unlock();
            if (done_) done_(Result{});
            lock.lock();
            continue;
        }
        if (filled_ < ring_.size() || since_ < (std::size_t)STEP) continue;

        // The latest 15 s, oldest first.
        for (std::size_t i = 0; i < ring_.size(); i++) window[i] = ring_[(head_ + i) % ring_.size()];
        since_                  = 0;
        std::int64_t window_sys = end_sys_ms_ - WINDOW_MS;
        int          lo = low_hz_, hi = high_hz_, qso = qso_hz_;
        lock.unlock();

        Result best = decode_window(window.data(), window_sys, lo, hi, qso);

        lock.lock();
        if (!best.found || gen != generation_ || !active_) continue; // stopped or restarted meanwhile
        active_ = false;
        lock.unlock();
        if (done_) done_(best);
        lock.lock();
    }
}

} // namespace x6100::js8
