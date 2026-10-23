/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 automatic time sync
 *
 *  Desktop JS8Call-improved's Automatic Time Drift (44fa092,
 *  JS8_Mainwindow/processDecodeEvent.cpp, JS8_UI/WideGraph.cpp), which the
 *  JS8Call-improved Android app uses for its "Auto time sync" too: every
 *  decoded Normal or Slow frame suggests the drift that would put it on
 *  time, and at the end of each decode pass they're folded into a
 *  cumulative moving average of up to 60 frames, which becomes the drift.
 *  One heartbeat is enough to start.
 *
 *  And a search for when the clock is too far off for anything to decode
 *  (the Normal decoder looks only about 2.5 s either side of the slot):
 *  desktop decodes Normal every second while its automatic drift starts;
 *  here the latest 15 s of audio is decoded every 4 s, which finds a
 *  signal wherever it falls in the 15 s cycle at about a quarter of the
 *  cost. The first decode gives the drift.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace x6100::js8 {

/// Speeds whose frames time the band, as desktop's shouldAutoSyncSubmode():
/// Normal (0) and Slow (4). Fast and Turbo timing is rougher.
bool auto_sync_submode(int submode);

/// `suggested_ms` the short way round a slot of `period_ms` from
/// `current_ms` (a drift is only known to its slot: -14 s and +1 s put a
/// 15 s slot in the same place), kept within one period as desktop keeps it.
std::int64_t drift_short_way(std::int64_t suggested_ms, std::int64_t current_ms, std::int64_t period_ms);

/// Desktop's automatic time drift.
class AutoTimeSync {
public:
    static constexpr int MAX_N = 60; ///< desktop's cap on the average

    /// A decoded frame (duplicates already dropped): its speed, and the drift
    /// that puts it on time (its capture drift less its DT). Other speeds
    /// than Normal and Slow are ignored. `current_ms`: the drift now.
    void frame(int submode, std::int64_t suggested_ms, std::int64_t current_ms);
    /// The end of a decode pass: the drift to set if frames came in it, and
    /// how many (in `frames`). The first frame ever sets it outright.
    std::optional<std::int64_t> pass_done(std::int64_t current_ms, unsigned *frames = nullptr);
    /// The drift was set another way: the average goes on from `drift_ms`,
    /// counted as one frame (`keep`, a search's find), or starts afresh.
    void restart(std::int64_t drift_ms, bool keep);
    /// How many frames the average holds (at most MAX_N).
    int count() const { return n_ > 0 ? n_ - 1 : 0; }

private:
    std::vector<std::int64_t> queue_; ///< this pass's frames
    std::int64_t              mma_ = 0;
    int                       n_   = 0; ///< desktop's m_driftMsMMA_N: the next frame's divisor
};

/// Notices the system clock being stepped (ntpd setting it from a GPS, the
/// radio's Settings, hwclock): the wall clock jumping against the monotonic
/// one. ntpd's slewing (at most 0.5 ms a second) never counts. Fed both
/// clocks often (the receiver, every audio block); no clock of its own.
class ClockStepWatch {
public:
    static constexpr std::int64_t THRESHOLD_MS = 250;
    /// The step (wall clock moved by this, + forward) if one happened since
    /// the last call, else 0.
    std::int64_t check(std::int64_t wall_ms, std::int64_t mono_ms);

private:
    bool         have_   = false;
    std::int64_t offset_ = 0; ///< wall - monotonic, as last seen
};

/// The search: decodes the latest 15 s of audio every 4 s (Normal), until
/// one decodes or time runs out. Its own thread (js8core's decoders are per
/// thread); fed the 12 kHz audio the engine gets.
class TimeSearch {
public:
    struct Result {
        bool         found    = false; ///< false: time ran out, nothing decoded
        std::int64_t drift_ms = 0;     ///< the drift that puts it on time
        int          snr      = 0;
        float        freq_hz  = 0;
        std::string  text;             ///< the frame, rendered
    };
    using Done = std::function<void(const Result &)>;

    static constexpr int WINDOW_MS = 15000; ///< a Normal slot
    /// A window every 4 s. A window decodes a -18 dB signal starting from
    /// 2.5 s before to 2.25 s after it (unit test "no gap"), and every
    /// station starts at the same point of the slot, so the step must be
    /// narrower or that point could fall between windows for good; 4 s
    /// (not a divisor of 15) also moves the windows round the slot.
    static constexpr int STEP_MS   = 4000;

    explicit TimeSearch(Done done);
    ~TimeSearch();

    TimeSearch(const TimeSearch &)            = delete;
    TimeSearch &operator=(const TimeSearch &) = delete;

    /// Start (or start again) for up to `max_ms`, decoding `low_hz`..`high_hz`
    /// with signals near `qso_hz` first. The first window is ready 15 s on.
    void start(std::int64_t max_ms, int low_hz, int high_hz, int qso_hz);
    /// Stop without a result.
    void stop();
    bool active() const { return active_.load(); }

    /// 12 kHz audio; `end_sys_ms`: the system clock (not JS8's) at its last
    /// sample. From the receiver's worker; ignored unless active.
    void feed(const std::int16_t *pcm, std::size_t n, std::int64_t end_sys_ms);

    /// The drift that puts on time a Normal signal decoded at `xdt` in a
    /// window that started at `window_start_sys_ms` (system clock): one
    /// that makes it start on a slot of `period_ms`, the short way.
    static std::int64_t drift_for(std::int64_t window_start_sys_ms, float xdt, std::int64_t period_ms = WINDOW_MS);

    /// One window: WINDOW_MS of 12 kHz audio that started at
    /// `window_start_sys_ms`, decoded (Normal); the strongest decode's
    /// result. What the search does every STEP_MS.
    static Result decode_window(const std::int16_t *samples, std::int64_t window_start_sys_ms, int low_hz,
                                int high_hz, int qso_hz);

private:
    void loop();

    Done done_;

    mutable std::mutex        mutex_;
    std::condition_variable   cv_;
    std::thread               thread_;
    bool                      quit_ = false;
    std::atomic<bool>         active_{false};
    unsigned                  generation_ = 0; ///< a new start() or stop() ends the search under way
    std::int64_t              deadline_ms_ = 0; ///< steady clock
    int                       low_hz_ = 200, high_hz_ = 3000, qso_hz_ = 1500;
    std::vector<std::int16_t> ring_;          ///< the latest WINDOW_MS
    std::size_t               head_ = 0, filled_ = 0, since_ = 0;
    std::int64_t              end_sys_ms_ = 0;
};

} // namespace x6100::js8
