/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#pragma once

#include "assembler.hpp"
#include "render.hpp"
#include "resampler.hpp"
#include "timesync.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace js8core {
class Js8Engine;
}

namespace x6100::js8 {

/// Submode bits for Receiver::Config::submodes (js8core SubmodeId order).
enum SubmodeMask : int {
    SUBMODE_NORMAL = 1 << 0,
    SUBMODE_FAST   = 1 << 1,
    SUBMODE_TURBO  = 1 << 2,
    SUBMODE_SLOW   = 1 << 3,
    SUBMODE_ULTRA  = 1 << 4, ///< Ultra
};

/// JS8 receive pipeline for the X6100.
///
/// feed() may be called from the audio thread; it only appends to a buffer.
/// A worker thread resamples to 12 kHz and hands audio to the js8core engine,
/// whose own thread decodes. Callbacks run on those worker threads, never on
/// the caller's thread; UI code must marshal them to its own thread.
class Receiver {
public:
    struct Config {
        int input_rate = 11025;
        int submodes   = SUBMODE_NORMAL;
        /// Re-snap the decoder's 60 s ring to the wall clock when the sample
        /// count and real time disagree by more than this. 0 disables it
        /// (tests feed audio faster than real time).
        int realign_threshold_ms = 150;
    };

    /// A decode attempt, as desktop's "Show decode attempts" draws them: a
    /// sync candidate the decoder is trying (with its sync strength), or a
    /// decode. Only while set_sync_marks(true).
    struct SyncMark {
        float freq_hz = 0; ///< audio offset of the lowest tone
        float dt      = 0; ///< s
        int   submode = 0; ///< desktop numbering: 0 Normal, 1 Fast, 2 Turbo, 4 Slow, 8 Ultra
        int   sync    = 0; ///< candidate's sync strength (7..); 0 for a decode
        bool  decoded = false;
    };

    struct Callbacks {
        /// Every decoded frame, rendered (band activity).
        std::function<void(const RxFrame &)> on_frame;
        /// Complete messages after multi-frame assembly, and (partial set)
        /// the text so far of a message still arriving, after each frame.
        std::function<void(const RxFrame &)> on_message;
        /// A decode pass finished with this many unique decodes.
        std::function<void(std::size_t)> on_cycle_done;
        /// Input-rate audio as the worker takes it, for a waterfall.
        std::function<void(const float *, std::size_t)> on_audio;
        /// Decode attempts (set_sync_marks); many per decode pass.
        std::function<void(const SyncMark &)> on_sync;
        /// Engine diagnostics. Very chatty; leave empty in production.
        std::function<void(const std::string &)> on_log;
        /// Health, a few lines a minute at most: the decoder's load once a
        /// minute, a decode pass that ran very long, audio missing (gap) or
        /// thrown away (the worker fell behind), clock realigns.
        std::function<void(const std::string &)> on_report;
        /// Automatic time sync (set_auto_sync): after a decode pass with
        /// Normal or Slow frames, the drift desktop would set, and how many
        /// frames came in it. The caller sets it (set_drift_ms) when it may.
        std::function<void(std::int64_t, unsigned)> on_auto_drift;
        /// The search (start_search) ended: found, or time ran out.
        std::function<void(const TimeSearch::Result &)> on_search;
    };

    Receiver(const Config &config, Callbacks callbacks);
    ~Receiver();

    Receiver(const Receiver &)            = delete;
    Receiver &operator=(const Receiver &) = delete;

    /// Queue audio at Config::input_rate, float in [-1, 1]. Thread-safe.
    void feed(const float *samples, std::size_t n);

    /// Drop partially received multi-frame messages (e.g. after a band change).
    void clear_messages();

    /// Change which speeds are decoded (SubmodeMask bits), from any thread.
    /// The engine keeps a slot schedule for every speed; this only switches
    /// them on and off, and a speed's decoder is built the first time it's used.
    void set_submodes(int submodes);

    /// The audio range searched for signals, and our TX offset (signals near
    /// it are decoded first), as desktop JS8Call passes its filter and freq().
    void set_decode_range(int low_hz, int high_hz);
    void set_qso_offset(int offset_hz);

    /// Report decode attempts through Callbacks::on_sync (js8core patch 12).
    /// From the next decode pass; off by default.
    void set_sync_marks(bool on);

    /// How often the ring has been re-snapped to the clock since start.
    unsigned realign_count() const { return realigns_.load(); }

    /// Automatic time sync, as desktop's Automatic Time Drift: off to start.
    void set_auto_sync(bool on) { auto_on_ = on; }
    /// The drift was set another way (a search, a reset): see
    /// AutoTimeSync::restart().
    void restart_auto_sync(std::int64_t drift_ms, bool keep);
    /// Search for the band's timing for up to `max_ms` (TimeSearch); the
    /// result comes through Callbacks::on_search. Starting again restarts.
    void start_search(std::int64_t max_ms);
    void stop_search();
    bool searching() const { return search_->active(); }

private:
    void worker_loop();
    void submit(const std::vector<float> &audio_12k);
    void check_clock(std::size_t new_samples);
    void push_pcm(const std::int16_t *pcm, std::size_t count);
    void engine_log(std::string_view m);
    void report(const std::string &line);

    Config    config_;
    Callbacks cb_;

    std::unique_ptr<js8core::Js8Engine> engine_;
    RationalResampler                   resampler_;

    // feed() -> worker
    std::mutex              in_mutex_;
    std::condition_variable in_cv_;
    std::vector<float>      pending_;
    bool                    stop_ = false;
    std::thread             worker_;

    // Clock tracking (worker thread only)
    bool                  aligned_ = false;
    std::int64_t          applied_drift_ms_ = 0; ///< drift the engine was last given
    std::int64_t          align_wall_ms_ = 0;
    std::uint64_t         samples_since_align_ = 0;
    std::atomic<unsigned> realigns_{0};
    ClockStepWatch        clock_watch_; ///< the system clock stepped (worker thread)

    // Decode load for on_report, from the engine's log (decode threads; the
    // merge note comes from the worker). [0] the main decode thread, [1]
    // Ultra's own (js8core patch 15).
    struct PassStats {
        std::chrono::steady_clock::time_point pass_start;
        unsigned                              passes = 0, decodes = 0, merged = 0;
        double                                busy_s = 0, longest_s = 0;
    };
    std::mutex                            stats_mutex_;
    std::chrono::steady_clock::time_point stats_start_;
    PassStats                             stats_[2];
    /// The main decode thread is in a pass: an Ultra pass ending meanwhile
    /// isn't the end of a cycle (the main pass's end is).
    std::atomic<bool> main_busy_{false};

    // Automatic time sync: frames and passes on the decode thread, restarts
    // from the caller's.
    std::atomic<bool> auto_on_{false};
    std::mutex        auto_mutex_;
    AutoTimeSync      auto_sync_; ///< under auto_mutex_

    // The search, fed by the worker; what it decodes, as the engine has it.
    // Declared after the worker's state: destroyed after it's joined.
    std::unique_ptr<TimeSearch> search_;
    std::atomic<int>            low_hz_{200}, high_hz_{3000}, qso_hz_{1500};

    // Decode-thread state; assembler also touched by the worker's flush.
    FrameRenderer    renderer_;
    std::mutex       assembler_mutex_;
    DuplicateFilter  duplicates_; ///< under assembler_mutex_
    MessageAssembler assembler_;
};

/// JS8's time in ms since the epoch: the system clock plus the drift.
std::int64_t wall_ms();
/// Drift added to the system clock (Time Sync); 0 until set. Process-wide.
void         set_drift_ms(std::int64_t ms);
std::int64_t drift_ms();

} // namespace x6100::js8
