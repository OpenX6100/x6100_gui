/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 decoder setup: FFTW wisdom
 *
 *  js8core's decoder plans its FFTs with a measuring planner
 *  (FFTW_ESTIMATE_PATIENT without FFTW_ESTIMATE): building a speed's decoder
 *  times several ways of doing each transform and keeps the fastest. FFTW
 *  remembers what it found ("wisdom") for as long as the program runs, so
 *  each plan is measured once per power-on; desktop JS8Call also keeps it
 *  in a file (js8call_wisdom.dat), so it measures once ever. So do we.
 */

#pragma once

#include <atomic>
#include <string>

namespace x6100::js8 {

/// FFTW's wisdom as text (what the file holds). Empty if FFTW has none.
std::string export_wisdom();
/// Load `text` into FFTW. False (and FFTW's wisdom forgotten, so nothing
/// half-loaded is left) if it isn't wisdom from this FFTW build: damaged,
/// cut short, from another FFTW version.
bool import_wisdom(const std::string &text);
/// `now` (wisdom text) holds something `before` doesn't.
bool wisdom_has_new(const std::string &now, const std::string &before);
/// Forget everything FFTW has measured (tests).
void forget_wisdom();

/// Build one speed's decoder (a SubmodeMask bit) and drop it again: one
/// decode of silence on a thread of its own, so the decoder, which the
/// thread keeps, goes when it ends. Builds its FFT plans, measuring each one
/// FFTW has no wisdom for. Returns the seconds it took.
double build_decoder(int submode_bit);

/// The one-time setup, once per program: load the wisdom file, or, when it
/// is missing or can't be used, measure every speed's plans (all five,
/// whichever are switched on) on a thread of its own and save the file.
/// The thread carries on if the caller goes away (JS8 closed meanwhile) and
/// saves the file when it's done; it never touches the caller's objects.
class DecoderSetup {
public:
    /// The process-wide one.
    static DecoderSetup &get();

    /// Before the decoder starts. True: the decoder can start now (the file
    /// was loaded, now or by an earlier call, or the setup is done). False:
    /// the setup is running (started now, or by an earlier call that hasn't
    /// finished); poll running().
    bool begin(const std::string &path);
    /// The setup is still running; `speeds_done` (if given) gets how many of
    /// the five speeds are measured so far.
    bool running(unsigned *speeds_done = nullptr) const;
    /// After the decoder has stopped: save FFTW's wisdom to `path` if it
    /// has changed since the file was loaded or saved (as desktop at exit).
    /// Nothing while the setup runs: it saves when it ends.
    void save(const std::string &path);

    /// How long the last setup took, in seconds (0: none ran).
    double last_setup_s() const { return setup_s_.load(); }

    /// Back to the state at start-up, FFTW's wisdom forgotten (tests; not
    /// while the setup runs).
    void reset();

private:
    DecoderSetup() = default;
    void run(std::string path);

    std::atomic<bool>     ready_{false};   ///< wisdom loaded or measured
    std::atomic<bool>     running_{false}; ///< the setup thread is at work
    std::atomic<unsigned> done_{0};        ///< speeds measured by it so far
    std::atomic<double>   setup_s_{0};
    std::string           saved_; ///< the file's wisdom as loaded or saved; under setup_mutex()
};

} // namespace x6100::js8
