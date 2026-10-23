/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 transmit
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "speeds.hpp"

namespace x6100::js8 {

constexpr int TX_SYMBOLS        = 79;
constexpr int TX_MIN_OFFSET_HZ  = 500;   ///< the top is Speed::max_offset_hz()
constexpr int TX_MAX_FRAMES     = 20;    ///< 5 minutes of transmitting
constexpr double TX_GFSK_BT     = 3.0;   ///< see docs/TX_PLAN.md: decodes like CPFSK, less splatter

struct TxFrame {
    std::string                  frame; ///< 12-character JS8 frame
    int                          bits = 0;
    std::array<int, TX_SYMBOLS>  tones{};
};

/// A message ready to send, with what other stations will see.
struct TxPlan {
    std::vector<TxFrame> frames;
    std::string          text;    ///< as sent (upper case, trimmed)
    std::string          preview; ///< our own frames decoded back, as others will see them
    std::string          error;   ///< non-empty if the message can't be sent
    js8_speed_t          speed = JS8_SPEED_NORMAL;

    bool ok() const { return error.empty() && !frames.empty(); }
    /// Seconds of air time, from the first frame's start to the last one's end.
    double seconds() const {
        const Speed &sp = x6100::js8::speed(speed);
        return frames.empty() ? 0.0 : (frames.size() - 1) * (double)sp.period_s + sp.frame_seconds();
    }
};

/// Build frames for `text` exactly as desktop JS8Call would, then decode them
/// back with this app's receive code to produce `preview`. Examples of text:
/// "K2XYZ SNR?", "K2XYZ HELLO THERE", "CQ CQ CQ FN42", "@ALLCALL QRV".
/// Untargeted free text is sent with our callsign attached. Outside Normal,
/// free text goes in desktop's "fast data" frames and the tones use the
/// modified Costas array, as desktop does for that speed.
TxPlan plan_message(const std::string &my_call, const std::string &my_grid, const std::string &text,
                    js8_speed_t speed = JS8_SPEED_NORMAL);

/// Characters JS8 can carry in free text, plus those its message syntax uses.
bool is_sendable_char(char c);

/// GFSK audio for one frame at `rate`: 79 symbols of the speed's length
/// (0.16 s Normal, 0.1 Fast, 0.05 Turbo, 0.32 Slow; tone spacing 1 / symbol)
/// from `offset_hz`, amplitude 1.0 with a short ramp at each end.
std::vector<float> synth_frame(const std::array<int, TX_SYMBOLS> &tones, double offset_hz, int rate,
                               js8_speed_t speed = JS8_SPEED_NORMAL, double bt = TX_GFSK_BT);

/// Wall-clock time (ms since the epoch) at which a message queued at `now_ms`
/// starts: the next slot boundary of the speed plus its start delay (0.5 s
/// Normal and Slow, 0.2 s Fast, 0.1 s Turbo), or this slot's if we are
/// still inside the delay.
std::int64_t next_tx_start_ms(std::int64_t now_ms, js8_speed_t speed = JS8_SPEED_NORMAL);

/// Sends queued messages frame by frame in consecutive slots.
///
/// The host supplies `play`, which keys the radio, plays one frame's audio
/// and returns when done. It runs on the transmitter's thread and may block
/// for the whole frame (~12.6 s). It should return false if it had to stop
/// early. stop() may be called from any thread; the host is expected to make
/// `play` return promptly once stop() has been called (e.g. by polling
/// stopping()).
class Transmitter {
public:
    enum class State { Idle, Waiting, Keying };

    struct Status {
        State        state   = State::Idle;
        int          frame   = 0;  ///< 1-based frame being sent or next up
        int          frames  = 0;
        std::int64_t next_ms = 0;  ///< wall-clock start of the next frame
        std::string  text;
        double       offset_hz = 0;
        js8_speed_t  speed     = JS8_SPEED_NORMAL;
    };

    struct Callbacks {
        std::function<bool(const std::vector<float> &audio, const TxFrame &frame, int index, int count)> play;
        std::function<void(const Status &)> on_status;   ///< state changes, from the TX thread
        std::function<void(const std::string &text, bool completed)> on_done;
    };

    /// Time source, replaceable in tests. wait_until() returns early (false)
    /// when `cancelled` becomes true.
    struct Clock {
        virtual ~Clock() = default;
        virtual std::int64_t now_ms()                                         = 0;
        virtual bool wait_until(std::int64_t ms, const std::atomic<bool> &cancelled) = 0;
    };

    Transmitter(int rate, Callbacks callbacks, Clock *clock = nullptr);
    ~Transmitter();

    Transmitter(const Transmitter &)            = delete;
    Transmitter &operator=(const Transmitter &) = delete;

    /// Queue a planned message at audio offset `offset_hz`, at the plan's
    /// speed. Returns false if something is already queued or sending, if
    /// the plan isn't ok(), or if the offset (500 Hz to the speed's
    /// max_offset_hz()) or length is out of range.
    ///
    /// `synth_hz`, if non-zero, is the tone the audio is generated at
    /// instead of `offset_hz`. The X6100's TX player always takes audio
    /// centred on a fixed tone and shifts the VFO to reach the offset.
    bool send(const TxPlan &plan, double offset_hz, std::string *why = nullptr, double synth_hz = 0);

    /// Abandon the current message. Returns at once; busy() turns false once
    /// the current frame's play() has returned.
    void stop();

    /// Wait until the current message's thread has ended (stop() first to
    /// end it early). Not from the transmitter's own callbacks.
    void join();

    bool   busy() const { return busy_; }
    bool   stopping() const { return stop_; }
    Status status() const;

private:
    void run(TxPlan plan, double offset_hz, double synth_hz);
    void set_status(const Status &s);

    int                 rate_;
    Callbacks           cb_;
    Clock              *clock_;
    std::unique_ptr<Clock> own_clock_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_{false};
    std::thread       thread_;

    mutable std::mutex status_mutex_;
    Status             status_;
};

} // namespace x6100::js8
