/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#include "receiver.hpp"

#include "classify.hpp"
#include "cpu_fence.hpp"
#include "js8core/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <pthread.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <span>
#include <variant>

namespace x6100::js8 {

namespace {

constexpr int JS8_RATE = 12000;

// Longest backlog the worker will accept before discarding it: the decoder
// needs contiguous audio, so a stalled worker is better restarted cleanly.
constexpr double MAX_PENDING_SEC = 5.0;

// How often the sample count is compared with the wall clock.
constexpr std::int64_t CLOCK_CHECK_MS = 2000;

// A decode pass longer than this is reported at once (others only in the
// minute's summary): with four speeds decoding, the next windows wait.
constexpr double LONG_PASS_S = 10.0;
// Ultra's own decode thread runs below the main one and the GUI.
constexpr int ULTRA_DECODE_NICE = 5;

// Audio this far behind the clock went missing (a stall, or the caller
// stopped feeding it during TX). The gap is filled with silence: a realign
// alone would leave minute-old audio in the ring, which decodes again as new.
constexpr std::int64_t GAP_FILL_MS = 1000;
constexpr std::int64_t RING_MS     = 60000;

int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }

} // namespace

namespace {
std::atomic<std::int64_t> g_drift_ms{0};
} // namespace

// JS8's time: the system clock plus the drift set by Time Sync, as desktop
// JS8Call's DriftingDateTime. Decode windows, transmit slots and everything
// else JS8 times go by it; the system clock itself is never changed.
std::int64_t wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() +
           g_drift_ms.load(std::memory_order_relaxed);
}

void set_drift_ms(std::int64_t ms) { g_drift_ms.store(ms, std::memory_order_relaxed); }

std::int64_t drift_ms() { return g_drift_ms.load(std::memory_order_relaxed); }

Receiver::Receiver(const Config &config, Callbacks callbacks)
    : config_(config),
      cb_(std::move(callbacks)),
      resampler_(JS8_RATE / gcd(JS8_RATE, config.input_rate), config.input_rate / gcd(JS8_RATE, config.input_rate)),
      assembler_([this](const RxFrame &assembled) {
          if (!cb_.on_message) return;
          RxFrame msg = assembled;
          switch (verify_command_checksum(msg.text)) {
          case Checksum::None: msg.checksum = 0; break;
          case Checksum::Valid: msg.checksum = 1; break;
          case Checksum::Invalid: msg.checksum = -1; break;
          }
          cb_.on_message(msg);
      },
      [this](const RxFrame &so_far) {
          // No checksum yet: it comes with the last frame.
          if (cb_.on_message) cb_.on_message(so_far);
      }) {
    js8core::EngineConfig ec;
    ec.sample_rate_hz   = JS8_RATE;
    // Schedules for every speed; set_submodes() below picks what's decoded.
    ec.submodes         = SUBMODE_NORMAL | SUBMODE_FAST | SUBMODE_TURBO | SUBMODE_SLOW | SUBMODE_ULTRA;
    ec.spectrum_enabled = false;
    // Ultra on its own decode thread (patch 15): its 4 s slots don't wait
    // behind a pass of the slower speeds, nor theirs behind it.
    ec.ultra_own_thread = true;

    js8core::EngineCallbacks ecb;
    // Both decode threads stay off one core, so the GUI (the waterfall)
    // always has one free; Ultra's runs at a lower priority too.
    ecb.on_decode_thread_start = [this](std::string_view lane) {
        bool ultra = lane == "ultra";
        int  free  = fence_decoder_thread(ultra ? ULTRA_DECODE_NICE : 0);
        char line[96];
        if (free >= 0)
            snprintf(line, sizeof(line), "decode thread %.*s: off core %d (left for the screen)%s", (int)lane.size(),
                     lane.data(), free, ultra ? ", nice 5" : "");
        else snprintf(line, sizeof(line), "decode thread %.*s: on any core", (int)lane.size(), lane.data());
        report(line);
    };
    ecb.on_event = [this](js8core::events::Variant const &ev) {
        if (auto d = std::get_if<js8core::events::Decoded>(&ev)) {
            {
                std::lock_guard<std::mutex> lock(assembler_mutex_);
                if (duplicates_.seen(d->mode, d->data, d->frequency, wall_ms())) return;
            }
            RxFrame f;
            f.type           = d->type;
            f.text           = renderer_.render(d->data, &f.type, d->frequency);
            f.utc            = d->utc;
            f.snr            = d->snr;
            f.dt             = d->xdt;
            f.freq_hz        = d->frequency;
            f.quality        = d->quality;
            f.low_confidence = d->quality < LOW_CONFIDENCE_QUALITY;
            f.mode           = d->mode;
            f.capture_drift_ms = d->capture_drift_ms;
            f.timestamp_ms   = wall_ms();

            if (cb_.on_frame) cb_.on_frame(f);
            if (auto_on_ && !f.low_confidence) {
                std::lock_guard<std::mutex> lock(auto_mutex_);
                auto_sync_.frame(f.mode, f.capture_drift_ms - std::lround(f.dt * 1000.0f), drift_ms());
            }
            std::lock_guard<std::mutex> lock(assembler_mutex_);
            assembler_.add(f);
        } else if (auto s = std::get_if<js8core::events::SyncState>(&ev)) {
            if (!cb_.on_sync) return;
            SyncMark m;
            m.freq_hz = s->frequency;
            m.dt      = s->dt;
            m.submode = s->mode;
            m.decoded = s->kind == js8core::events::SyncState::Kind::Decoded;
            m.sync    = m.decoded ? 0 : s->sync.candidate;
            cb_.on_sync(m);
        } else if (auto st = std::get_if<js8core::events::DecodeStarted>(&ev)) {
            if (st->submodes & ~SUBMODE_ULTRA) main_busy_ = true;
        } else if (auto fin = std::get_if<js8core::events::DecodeFinished>(&ev)) {
            // Ultra's own thread (patch 15): its pass ending is a cycle's end
            // only while the main thread isn't in a pass (its end is then);
            // Time: Auto doesn't learn from Ultra, as desktop.
            if (fin->submodes == SUBMODE_ULTRA) {
                if (!main_busy_ && cb_.on_cycle_done) cb_.on_cycle_done(fin->decoded);
                return;
            }
            main_busy_ = false;
            if (auto_on_) { // desktop sets its drift here, at the end of the pass
                std::optional<std::int64_t> drift;
                unsigned                    frames = 0;
                {
                    std::lock_guard<std::mutex> lock(auto_mutex_);
                    drift = auto_sync_.pass_done(drift_ms(), &frames);
                }
                if (drift && cb_.on_auto_drift) cb_.on_auto_drift(*drift, frames);
            }
            if (cb_.on_cycle_done) cb_.on_cycle_done(fin->decoded);
        }
    };
    if (cb_.on_log || cb_.on_report) {
        ecb.on_log   = [this](js8core::LogLevel, std::string_view m) {
            engine_log(m);
            if (cb_.on_log) cb_.on_log(std::string(m));
        };
        ecb.on_error = [this](std::string_view m) { cb_.on_log("error: " + std::string(m)); };
    }

    // Made before the worker starts, which feeds it; its thread starts with
    // the first search, and it's destroyed (joined) after the worker.
    search_ = std::make_unique<TimeSearch>([this](const TimeSearch::Result &r) {
        if (cb_.on_search) cb_.on_search(r);
    });
    engine_ = js8core::make_engine(ec, std::move(ecb), {});
    engine_->set_submodes(config_.submodes);
    applied_drift_ms_ = drift_ms(); // a Time Sync drift from before JS8 reopened
    if (applied_drift_ms_) engine_->set_time_drift_ms(applied_drift_ms_);
    engine_->start();

    stats_start_ = std::chrono::steady_clock::now();
    worker_      = std::thread([this] { worker_loop(); });
}

Receiver::~Receiver() {
    {
        std::lock_guard<std::mutex> lock(in_mutex_);
        stop_ = true;
    }
    in_cv_.notify_one();
    if (worker_.joinable()) worker_.join();

    // Joins the engine's decode thread; no callbacks fire after this.
    engine_->stop();
    engine_.reset();
}

void Receiver::set_submodes(int submodes) {
    engine_->set_submodes(submodes);
}

void Receiver::set_decode_range(int low_hz, int high_hz) {
    low_hz_  = low_hz;
    high_hz_ = high_hz;
    engine_->set_decode_range(low_hz, high_hz);
}

void Receiver::set_qso_offset(int offset_hz) {
    qso_hz_ = offset_hz;
    engine_->set_qso_offset(offset_hz);
}

void Receiver::restart_auto_sync(std::int64_t drift_ms, bool keep) {
    std::lock_guard<std::mutex> lock(auto_mutex_);
    auto_sync_.restart(drift_ms, keep);
}

void Receiver::start_search(std::int64_t max_ms) {
    search_->start(max_ms, low_hz_, high_hz_, qso_hz_);
}

void Receiver::stop_search() {
    search_->stop();
}

void Receiver::set_sync_marks(bool on) {
    engine_->set_sync_stats(on);
}

void Receiver::feed(const float *samples, std::size_t n) {
    {
        std::lock_guard<std::mutex> lock(in_mutex_);
        pending_.insert(pending_.end(), samples, samples + n);
    }
    in_cv_.notify_one();
}

void Receiver::clear_messages() {
    std::lock_guard<std::mutex> lock(assembler_mutex_);
    assembler_.clear();
}

void Receiver::report(const std::string &line) {
    if (cb_.on_report) cb_.on_report(line);
    if (cb_.on_log) cb_.on_log(line);
}

// The engine logs each decode pass's start and end on its decode thread, and
// a window merged because a pass was still running (patch 13).
void Receiver::engine_log(std::string_view m) {
    using clock = std::chrono::steady_clock;
    auto now    = clock::now();
    // Ultra's own decode thread (patch 15) prefixes its lines.
    constexpr std::string_view ULTRA = "[ultra] ";
    int lane = 0;
    if (m.starts_with(ULTRA)) {
        lane = 1;
        m.remove_prefix(ULTRA.size());
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    PassStats &ps = stats_[lane];
    if (m.starts_with("Calling legacy_decode")) {
        ps.pass_start = now;
        return;
    }
    if (m.starts_with("decode window merged")) {
        ps.merged++;
        return;
    }
    if (!m.starts_with("legacy_decode returned") || ps.pass_start == clock::time_point{}) return;
    double s      = std::chrono::duration<double>(now - ps.pass_start).count();
    ps.pass_start = {};
    ps.passes++;
    ps.busy_s += s;
    ps.longest_s = std::max(ps.longest_s, s);
    auto colon   = m.find(':');
    if (colon != std::string_view::npos) ps.decodes += (unsigned)std::atoi(std::string(m.substr(colon + 1)).c_str());
    char line[176];
    if (s > LONG_PASS_S) {
        snprintf(line, sizeof(line), "decode pass%s took %.1f s", lane ? " (Ultra)" : "", s);
        report(line);
    }
    double span = std::chrono::duration<double>(now - stats_start_).count();
    if (span < 60) return;
    for (int l = 0; l < 2; l++) {
        PassStats &p = stats_[l];
        if (l == 1 && !p.passes && !p.merged) continue; // Ultra off: no line
        snprintf(line, sizeof(line),
                 "decode%s: %u passes in %.0f s, busy %.1f s (%.0f %%), longest %.1f s, %u decodes, %u windows waited",
                 l ? " Ultra" : "", p.passes, span, p.busy_s, 100 * p.busy_s / span, p.longest_s, p.decodes, p.merged);
        report(line);
        auto start   = p.pass_start; // a pass still running carries on
        p            = PassStats{};
        p.pass_start = start;
    }
    stats_start_ = now;
}

void Receiver::worker_loop() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "js8-rx"); // per-thread CPU tools tell it apart
#endif
    std::vector<float> in;
    std::vector<float> out;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(in_mutex_);
            in_cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop_ || !pending_.empty(); });
            if (stop_) return;
            in.swap(pending_);
            pending_.clear();
        }

        if (in.size() > MAX_PENDING_SEC * config_.input_rate) {
            // We fell far behind; this audio is stale. Start over in sync.
            char line[96];
            snprintf(line, sizeof(line), "dropped %.1f s of audio: the receiver fell behind",
                     (double)in.size() / config_.input_rate);
            report(line);
            in.clear();
            resampler_.reset();
            aligned_ = false;
        }

        if (!in.empty()) {
            if (cb_.on_audio) cb_.on_audio(in.data(), in.size());
            out.clear();
            if (config_.input_rate == JS8_RATE) // R1CBU 1.0 delivers 12 kHz: nothing to resample
                out.assign(in.begin(), in.end());
            else
                resampler_.process(in.data(), in.size(), out);
            in.clear();
            submit(out);
        }

        std::lock_guard<std::mutex> lock(assembler_mutex_);
        assembler_.flush_stale(wall_ms());
    }
}

void Receiver::submit(const std::vector<float> &audio_12k) {
    if (audio_12k.empty()) return;

    check_clock(audio_12k.size());

    std::vector<std::int16_t> pcm(audio_12k.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        float v = std::clamp(audio_12k[i], -1.0f, 1.0f);
        pcm[i]  = (std::int16_t)std::lrintf(v * 32767.0f);
    }
    if (search_->active()) // stamped with the system clock: the search works out the drift itself
        search_->feed(pcm.data(), pcm.size(), wall_ms() - drift_ms());
    push_pcm(pcm.data(), pcm.size());
}

void Receiver::push_pcm(const std::int16_t *pcm, std::size_t count) {
    // The engine's slot scheduler steps through its ring one capture buffer
    // at a time; a multi-second burst (after a stall) would jump over decode
    // windows. Hand it audio in sound-card-sized pieces.
    constexpr std::size_t CHUNK = 4096;
    for (std::size_t off = 0; off < count; off += CHUNK) {
        std::size_t n = std::min(CHUNK, count - off);

        js8core::AudioInputBuffer buf;
        buf.data        = std::as_bytes(std::span<const std::int16_t>(pcm + off, n));
        buf.format      = {JS8_RATE, 1, js8core::SampleType::Int16};
        buf.captured_at = std::chrono::steady_clock::now();
        engine_->submit_capture(buf);
    }
}

void Receiver::check_clock(std::size_t new_samples) {
    // The system clock stepped (ntpd setting it from a GPS, the radio's
    // Settings): JS8's time stays where it was, the drift moving the other
    // way, so decode windows, TX slots and Time: Auto carry on (a clock
    // that was 2.9 s fast with Auto at -2.9 s, set right, leaves Auto at
    // 0). Auto's average starts again: its frames were timed against the
    // old clock. Without this, a step forward read as missing audio and a
    // step back as a clock error, and JS8 ended up off by the step.
    {
        using namespace std::chrono;
        const std::int64_t real = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        const std::int64_t mono = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        if (const std::int64_t step = clock_watch_.check(real, mono)) {
            const std::int64_t kept = drift_ms() - step;
            set_drift_ms(kept);
            {
                std::lock_guard<std::mutex> lock(auto_mutex_);
                auto_sync_.restart(kept, false);
            }
            char line[112];
            snprintf(line, sizeof(line), "clock stepped %+.1f s (GPS / settings): JS8 time kept, drift now %+.1f s",
                     step / 1000.0, kept / 1000.0);
            report(line);
        }
    }
    const std::int64_t drift = drift_ms();
    if (drift != applied_drift_ms_) {
        // Time Sync moved JS8's time. The engine realigns its ring to it
        // (set_time_drift_ms, on the next buffer); our check starts over, so
        // the jump isn't taken for missing audio or a clock error.
        applied_drift_ms_ = drift;
        engine_->set_time_drift_ms(drift);
        aligned_             = true;
        align_wall_ms_       = wall_ms();
        samples_since_align_ = new_samples;
        return;
    }

    const std::int64_t now = wall_ms();

    if (!aligned_) {
        // First audio (or recovery): align on the buffer we're about to submit.
        engine_->request_realign();
        aligned_             = true;
        align_wall_ms_       = now;
        samples_since_align_ = new_samples;
        return;
    }

    samples_since_align_ += new_samples;

    if (config_.realign_threshold_ms <= 0) return;

    const std::int64_t elapsed_wall = now - align_wall_ms_;
    if (elapsed_wall < CLOCK_CHECK_MS) return;

    // Audio arrives in bursts, so compare against the end of this buffer.
    const std::int64_t elapsed_audio = (std::int64_t)(samples_since_align_ * 1000 / JS8_RATE);
    const std::int64_t error_ms      = elapsed_wall - elapsed_audio;

    if (error_ms >= GAP_FILL_MS) {
        // Silence for the missing audio (at most one ring's worth), placed
        // before the buffer about to be submitted.
        const std::int64_t fill_ms = std::min(error_ms, RING_MS);
        std::vector<std::int16_t> silence((std::size_t)(fill_ms * JS8_RATE / 1000), 0);
        push_pcm(silence.data(), silence.size());
        report("gap: " + std::to_string(error_ms) + " ms of audio missing, filled with silence");
        if (error_ms <= RING_MS) {
            samples_since_align_ += silence.size();
            return;
        }
        // Longer than the ring: the silence cleared it; now realign.
    }

    if (std::llabs(error_ms) > config_.realign_threshold_ms) {
        engine_->request_realign();
        realigns_++;
        report("realign: audio/clock error " + std::to_string(error_ms) + " ms");
        align_wall_ms_       = now;
        samples_since_align_ = new_samples;
    }
}

} // namespace x6100::js8
