/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Copyright (c) the JS8Call-improved authors (ported from their Android app)
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Tests for src/js8 (JS8 receive).
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "assembler.hpp"
#include "classify.hpp"
#include "receiver.hpp"
#include "js8_ops.h"
#include "js8_tx.h"
#include "qsolog.hpp"
#include "inbox.hpp"
#include "alerts.hpp"
#include "macros.hpp"
#include "timesync.hpp"
#include "testsignal.hpp"
#include "speeds.hpp"
#include "history.hpp"
#include "js8_history.h"
#include "js8_wisdom.h"
#include "wisdom.hpp"
#include "datafile.hpp"

#include <unistd.h>
#include <glob.h>
#include <sys/stat.h>
#include "tx.hpp"
#include "render.hpp"
#include "resampler.hpp"

#include "js8core/decoder.hpp"
#include "js8core/protocol/costas.hpp"
#include "js8core/protocol/varicode.hpp"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <random>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace x6100::js8;
namespace vc = js8core::protocol::varicode;

namespace {

// Power of a single frequency (Goertzel), normalised to a sine's amplitude.
double tone_amplitude(const std::vector<float> &x, double freq, double rate, std::size_t skip = 0) {
    double      w = 2.0 * M_PI * freq / rate, c = 2.0 * std::cos(w);
    double      s1 = 0, s2 = 0;
    std::size_t n = x.size() - skip;
    for (std::size_t i = skip; i < x.size(); ++i) {
        double s = x[i] + c * s1 - s2;
        s2       = s1;
        s1       = s;
    }
    double power = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt(power) / n;
}

std::vector<float> sine(double freq, double rate, std::size_t n, double amp = 0.5) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = (float)(amp * std::sin(2.0 * M_PI * freq * i / rate));
    return v;
}

// Build frames for `text` as JS8Call would send them, then render and
// assemble them as the receiver does. Returns the assembled message.
std::string roundtrip(const std::string &mycall, const std::string &selected, const std::string &text) {
    auto            frames = vc::build_message_frames(mycall, "FN42", selected, text, false, false, 0);
    FrameRenderer   renderer;
    std::string     got;
    MessageAssembler assembler([&](const RxFrame &m) { got = m.text; });
    for (auto &[frame, bits] : frames) {
        RxFrame f;
        f.type    = bits;
        f.freq_hz = 1500;
        f.text    = renderer.render(frame, &f.type, f.freq_hz);
        assembler.add(f);
    }
    return got;
}

RxFrame frame(const std::string &text, int type, float freq = 1000.0f, std::int64_t t_ms = 0, int mode = 0) {
    RxFrame f;
    f.text         = text;
    f.type         = type;
    f.freq_hz      = freq;
    f.timestamp_ms = t_ms;
    f.mode         = mode;
    return f;
}

} // namespace

/* ---- Resampler -------------------------------------------------------- */

TEST_CASE("resampler converts 11025 Hz to 12000 Hz at the right length", "[js8][resampler]") {
    RationalResampler r(160, 147);
    std::vector<float> in(11025 * 2, 0.0f), out;
    r.process(in.data(), in.size(), out);
    REQUIRE(std::abs((long)out.size() - 24000) <= 1);
}

TEST_CASE("resampler preserves in-band tones", "[js8][resampler]") {
    for (double f : {300.0, 1000.0, 1500.0, 2500.0, 3500.0}) {
        RationalResampler  r(160, 147);
        auto               in = sine(f, 11025, 11025 * 2);
        std::vector<float> out;
        r.process(in.data(), in.size(), out);
        INFO("tone " << f << " Hz");
        CHECK(tone_amplitude(out, f, 12000, 200) == Catch::Approx(0.5).epsilon(0.02));
    }
}

TEST_CASE("resampler suppresses images that would fold into the JS8 band", "[js8][resampler]") {
    // An input tone f leaves an image at 11025 - f; at 12 kHz that folds to
    // 12000 - (11025 - f) = f + 975 Hz, inside the JS8 passband.
    for (double f : {500.0, 1500.0, 2000.0}) {
        RationalResampler  r(160, 147);
        auto               in = sine(f, 11025, 11025 * 2);
        std::vector<float> out;
        r.process(in.data(), in.size(), out);
        double wanted = tone_amplitude(out, f, 12000, 200);
        double image  = tone_amplitude(out, f + 975.0, 12000, 200);
        INFO("tone " << f << " Hz, image " << 20 * std::log10(image / wanted) << " dB");
        CHECK(20 * std::log10(image / wanted) < -60.0);
    }
}

/* ---- Render + assemble round trips ------------------------------------ */

TEST_CASE("frames built by JS8Call's encoder render and reassemble", "[js8][render]") {
    CHECK(roundtrip("W1ABC", "", "W1ABC: HEARTBEAT FN42") == "W1ABC: @HB HEARTBEAT FN42");
    CHECK(roundtrip("W1ABC", "", "CQ CQ CQ FN42") == "W1ABC: @ALLCALL CQ CQ CQ FN42");
    CHECK(roundtrip("W1ABC", "K2XYZ", "K2XYZ SNR?") == "W1ABC: K2XYZ SNR?");
    CHECK(roundtrip("W1ABC", "K2XYZ", "K2XYZ SNR -12") == "W1ABC: K2XYZ SNR -12");
    CHECK(roundtrip("W1ABC", "K2XYZ", "K2XYZ GRID?") == "W1ABC: K2XYZ GRID?");
    CHECK(roundtrip("W1ABC", "K2XYZ", "K2XYZ HELLO FROM THE X6100 TEST") ==
          "W1ABC: K2XYZ HELLO FROM THE X6100 TEST");
    CHECK(roundtrip("W1ABC", "", "@ALLCALL HELLO EVERYONE ON THE BAND") ==
          "W1ABC: @ALLCALL HELLO EVERYONE ON THE BAND");
    CHECK(roundtrip("W1ABC", "", "JUST SOME FREE TEXT WITHOUT A CALL") == "JUST SOME FREE TEXT WITHOUT A CALL");
}

TEST_CASE("every printable character survives encoding", "[js8][render]") {
    for (char c = 33; c < 127; c++) {
        if (c >= 'a' && c <= 'z') continue;
        std::string m = std::string("K2XYZ HELLO ") + c + c + " WORLD";
        INFO("char " << c);
        CHECK(roundtrip("W1ABC", "K2XYZ", m).rfind("W1ABC: " + m, 0) == 0);
        CHECK(is_sendable_char(c));
    }
}

TEST_CASE("APRS gateway commands survive encoding", "[js8][render][aprs]") {
    for (const char *m : {"@APRSIS GRID CN89LH", "@APRSIS CMD :SMS      :@6045551234 TEST FROM JS8{01}",
                          "@APRSIS CMD :EMAIL-2  :TEST@EXAMPLE.COM HELLO{01}",
                          "@APRSIS CMD :POTAGW   :VE7NHW CA-1234 7078 JS8 QRV",
                          "@APRSIS CMD :APRS2SOTA:VE7/LM-001 7.078 DATA VE7NHW QRV",
                          "@APRSIS CMD :WLNK-1   :SP TEST@EXAMPLE.COM SUBJECT",
                          "@APRSIS CMD :APSPOT   :! POTA CA-1234 7.078 DATA JS8",
                          "@APRSIS CMD =4916.9 N/12307.2 WG MADE IT TO CAMP"}) {
        INFO(m);
        // CMD carries a 3-character checksum after the text, like MSG.
        CHECK(roundtrip("VE7NHW", "", m).rfind(std::string("VE7NHW: ") + m, 0) == 0);
    }
}

TEST_CASE("a compound sender's helper frame is folded into the header", "[js8][render]") {
    CHECK(roundtrip("EA8/W1ABC", "K2XYZ", "K2XYZ HELLO FROM THE CANARIES") ==
          "EA8/W1ABC: K2XYZ HELLO FROM THE CANARIES");
}

TEST_CASE("renderer passes through frames it cannot unpack", "[js8][render]") {
    FrameRenderer r;
    int           type = 0;
    CHECK(r.render("short", &type, 1000) == "short");
    CHECK(r.render("has a space!", &type, 1000) == "has a space!");
}

/* ---- Multipart text (ported from DecodeViewModelMultipartAssemblyTest.kt) */

TEST_CASE("multipart text joining matches the Android port", "[js8][assembler]") {
    auto directed = [](const std::string &t) { return frame(t, FRAME_FIRST); };
    auto data     = [](const std::string &t) { return frame(t, FRAME_DATA); };

    // Data frames split mid-token and concatenate without a space.
    CHECK(assemble_multipart_text({data("N5EKS GRID EM10"), data("SM87MJ")}) == "N5EKS GRID EM10SM87MJ");
    // A buffered command strips its separator, so the space is put back.
    CHECK(assemble_multipart_text({directed("NT5DF: N5EKS GRID"), data("EM13TE")}) == "NT5DF: N5EKS GRID EM13TE");
    // An unbuffered command's payload brings its own space.
    CHECK(assemble_multipart_text({directed("NT5DF: N5EKS STATUS"), data(" IDLE AND MONITORING")}) ==
          "NT5DF: N5EKS STATUS IDLE AND MONITORING");
    CHECK(assemble_multipart_text({directed("2W0OXE: @RAYNET"), data("TEST")}) == "2W0OXE: @RAYNET TEST");
    CHECK(assemble_multipart_text(
              {directed("NT5DF: N5EKS MSG"), data("HELLO WORL"), data("D HOW ARE Y"), data("OU")}) ==
          "NT5DF: N5EKS MSG HELLO WORLD HOW ARE YOU");
    // A blanked helper frame leaves no leading space.
    CHECK(assemble_multipart_text({directed(""), data("NT5DF: N5EKS GRID EM13")}) == "NT5DF: N5EKS GRID EM13");
    CHECK(assemble_multipart_text({directed("K0OG: KN4CRD SNR +2")}) == "K0OG: KN4CRD SNR +2");
}

/* ---- Assembler buffering ---------------------------------------------- */

TEST_CASE("assembler groups frames by offset and emits on the last frame", "[js8][assembler]") {
    std::vector<std::string> out;
    MessageAssembler         a([&](const RxFrame &m) { out.push_back(m.text); });

    // Two stations interleaved; the second frame of A drifts 5 Hz.
    a.add(frame("A1AA: B1BB", FRAME_FIRST, 1000));
    a.add(frame("C1CC: D1DD", FRAME_FIRST, 1500));
    a.add(frame(" HELLO", FRAME_DATA, 1005));
    a.add(frame(" THERE", FRAME_DATA | FRAME_LAST, 1500));
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "C1CC: D1DD THERE");
    a.add(frame(" WORLD", FRAME_DATA | FRAME_LAST, 998));
    REQUIRE(out.size() == 2);
    CHECK(out[1] == "A1AA: B1BB HELLO WORLD");
}

TEST_CASE("assembler treats first+last frames as complete messages", "[js8][assembler]") {
    std::vector<std::string> out;
    MessageAssembler         a([&](const RxFrame &m) { out.push_back(m.text); });
    a.add(frame("A1AA: B1BB", FRAME_FIRST, 1000));
    // A new single-frame message at the same offset replaces the stale buffer.
    a.add(frame("A1AA: B1BB SNR -5", FRAME_FIRST | FRAME_LAST, 1002));
    REQUIRE(out == std::vector<std::string>{"A1AA: B1BB SNR -5"});
    a.add(frame(" LATE", FRAME_DATA | FRAME_LAST, 1000));
    CHECK(out.back() == " LATE");
}

TEST_CASE("assembler flushes an incomplete message 60 s after its latest frame", "[js8][assembler]") {
    std::vector<std::string> out;
    MessageAssembler         a([&](const RxFrame &m) { out.push_back(m.text); });
    a.add(frame("A1AA: B1BB", FRAME_FIRST, 1000, 0));
    a.add(frame(" PARTIAL", FRAME_DATA, 1000, 15'000));
    a.flush_stale(75'000); // 60 s after the latest frame: not yet
    CHECK(out.empty());
    a.flush_stale(75'001);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "A1AA: B1BB PARTIAL");
}

TEST_CASE("a long message isn't cut: the timeout runs from the latest frame", "[js8][assembler]") {
    // 9 Normal frames take 2 minutes; desktop keeps the buffer open while
    // frames keep coming (the old 90 s-from-the-first-frame rule split it).
    std::vector<std::string> out;
    MessageAssembler         a([&](const RxFrame &m) { out.push_back(m.text); });
    a.add(frame("A1AA: B1BB", FRAME_FIRST, 1000, 0));
    for (int i = 1; i < 8; i++) a.add(frame(" W" + std::to_string(i), FRAME_DATA, 1000, i * 15'000));
    CHECK(out.empty());
    a.add(frame(" END", FRAME_DATA | FRAME_LAST, 1000, 8 * 15'000));
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "A1AA: B1BB W1 W2 W3 W4 W5 W6 W7 END");
}

TEST_CASE("a frame decoded twice in one slot is dropped", "[js8][assembler][speed]") {
    DuplicateFilter f;
    CHECK_FALSE(f.seen(2, "ABCDEFGHIJKL", 1500, 0));
    CHECK(f.seen(2, "ABCDEFGHIJKL", 1510, 1'000));      // Turbo retry a second later
    CHECK_FALSE(f.seen(2, "ABCDEFGHIJKL", 1500, 6'000)); // next Turbo slot: a real repeat
    CHECK_FALSE(f.seen(0, "ABCDEFGHIJKL", 1500, 6'500)); // another speed
    CHECK_FALSE(f.seen(2, "ABCDEFGHIJKL", 1600, 6'500)); // another station
    CHECK_FALSE(f.seen(2, "ZZZZZZZZZZZZ", 1500, 6'500)); // another frame
    CHECK(f.seen(0, "ABCDEFGHIJKL", 1505, 20'000));      // Normal, same slot window (15 s)
    CHECK_FALSE(f.seen(0, "ABCDEFGHIJKL", 1500, 21'500));
}

TEST_CASE("assembler keeps speeds apart and uses each speed's window", "[js8][assembler]") {
    std::vector<std::string> out;
    MessageAssembler         a([&](const RxFrame &m) { out.push_back(m.text); });
    // A Normal and a Turbo station 5 Hz apart: never mixed.
    a.add(frame("A1AA: B1BB", FRAME_FIRST, 1000, 0, 0));
    a.add(frame("C1CC: D1DD", FRAME_FIRST, 1005, 0, 2));
    a.add(frame(" TURBO", FRAME_DATA | FRAME_LAST, 1025, 6'000, 2)); // Turbo drifts 20 Hz (window 32)
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "C1CC: D1DD TURBO");
    a.add(frame(" NORMAL", FRAME_DATA | FRAME_LAST, 1008, 15'000, 0));
    REQUIRE(out.size() == 2);
    CHECK(out[1] == "A1AA: B1BB NORMAL");

    // Fast's window is 16 Hz: 14 Hz away joins, 20 Hz away doesn't.
    a.add(frame("E1EE: F1FF", FRAME_FIRST, 1500, 30'000, 1));
    a.add(frame(" NEAR", FRAME_DATA, 1514, 40'000, 1));
    a.add(frame(" FAR", FRAME_DATA | FRAME_LAST, 1534, 50'000, 1));
    CHECK(out.back() == " FAR"); // a stray last frame on its own
    a.add(frame(" DONE", FRAME_DATA | FRAME_LAST, 1512, 50'000, 1));
    CHECK(out.back() == "E1EE: F1FF NEAR DONE");

    // Slow frames 30 s apart stay together.
    a.add(frame("G1GG: H1HH", FRAME_FIRST, 700, 100'000, 4));
    a.add(frame(" SLOW", FRAME_DATA, 700, 130'000, 4));
    a.add(frame(" ONE", FRAME_DATA | FRAME_LAST, 702, 160'000, 4));
    CHECK(out.back() == "G1GG: H1HH SLOW ONE");
}

/* ---- Classification --------------------------------------------------- */

TEST_CASE("classify recognises heartbeats, CQs and messages to me", "[js8][classify]") {
    auto hb = classify("W1ABC: @HB HEARTBEAT FN42", "K2XYZ");
    CHECK(hb.heartbeat);
    CHECK(hb.from == "W1ABC");
    CHECK_FALSE(hb.to_me);

    auto cq = classify("W1ABC: @ALLCALL CQ CQ CQ FN42", "K2XYZ");
    CHECK(cq.cq);
    CHECK(cq.to_group);
    CHECK(cq.to == "@ALLCALL");

    auto dm = classify("W1ABC: K2XYZ SNR?", "k2xyz");
    CHECK(dm.to_me);
    CHECK(dm.to == "K2XYZ");
    CHECK_FALSE(dm.snr_report); // a question, not a report

    // SNR reports (mostly answers to heartbeats), hidden with the heartbeats.
    CHECK(classify("W1ABC: N0XYZ SNR -12", "K2XYZ").snr_report);
    CHECK(classify("W1ABC: K2XYZ SNR +03", "K2XYZ").snr_report);
    CHECK_FALSE(classify("W1ABC: @ALLCALL SNR -12", "K2XYZ").snr_report);
    CHECK_FALSE(classify("W1ABC: N0XYZ HELLO SNR -12", "K2XYZ").snr_report);

    // As desktop: my call or my base call exactly. K2XYZ/P is another
    // station (a portable, a club's /P) when I'm K2XYZ (bug hunt 4).
    CHECK_FALSE(classify("W1ABC: K2XYZ/P HELLO", "K2XYZ").to_me);
    CHECK_FALSE(classify("W1ABC: VE3/K2XYZ HELLO", "K2XYZ").to_me);
    CHECK(classify("W1ABC: K2XYZ/P HELLO", "K2XYZ/P").to_me); // me, portable
    CHECK(classify("W1ABC: K2XYZ HELLO", "K2XYZ/P").to_me);   // my base call
    CHECK_FALSE(classify("W1ABC: K2XYA HELLO", "K2XYZ").to_me);
    CHECK_FALSE(classify("free text", "K2XYZ").to_me);

    // CQ is the word CQ, not a call starting with it (Portugal's CQ7ABC).
    CHECK_FALSE(classify("CT1ABC: CQ7ABC HELLO", "K2XYZ").cq);
    CHECK(classify("CT1ABC: CQ7ABC HELLO", "K2XYZ").to == "CQ7ABC");
    CHECK(classify("W1ABC: CQ CQ DE W1ABC", "K2XYZ").cq);
}

TEST_CASE("base_callsign strips prefixes and suffixes", "[js8][classify]") {
    CHECK(base_callsign("EA8/G4ABC/P") == "G4ABC");
    CHECK(base_callsign("k1abc") == "K1ABC");
    CHECK(base_callsign("VE3/K1ABC") == "K1ABC");
}

/* ---- Buffered-command checksums --------------------------------------- */

TEST_CASE("valid MSG, MSG TO: and relay checksums are verified and stripped", "[js8][checksum]") {
    for (auto [sent, shown] : std::vector<std::pair<std::string, std::string>>{
             {"K2XYZ MSG THIS IS A STORED MESSAGE FOR YOU", "W1ABC: K2XYZ MSG THIS IS A STORED MESSAGE FOR YOU"},
             {"K2XYZ MSG TO: N0CALL SEE YOU TOMORROW", "W1ABC: K2XYZ MSG TO: N0CALL SEE YOU TOMORROW"},
             {"K2XYZ>N0CALL HELLO VIA RELAY", "W1ABC: K2XYZ > N0CALL HELLO VIA RELAY"},
         }) {
        auto text = roundtrip("W1ABC", "K2XYZ", sent);
        INFO(sent << " -> " << text);
        CHECK(verify_command_checksum(text) == Checksum::Valid);
        CHECK(text == shown);
    }
}

TEST_CASE("a corrupted buffered message fails its checksum and is left as-is", "[js8][checksum]") {
    auto text = roundtrip("W1ABC", "K2XYZ", "K2XYZ MSG THIS IS A STORED MESSAGE FOR YOU");
    auto pos  = text.find("STORED");
    REQUIRE(pos != std::string::npos);
    text[pos] = 'X';
    auto before = text;
    CHECK(verify_command_checksum(text) == Checksum::Invalid);
    CHECK(text == before);
}

TEST_CASE("commands without a buffered payload have no checksum", "[js8][checksum]") {
    for (auto sent : {"K2XYZ QUERY MSGS", "K2XYZ SNR?", "K2XYZ HELLO THERE", "K2XYZ GRID FN42AB"}) {
        auto text = roundtrip("W1ABC", "K2XYZ", sent);
        INFO(text);
        CHECK(verify_command_checksum(text) == Checksum::None);
    }
}

/* ---- End to end: radio-rate audio through the receiver ---------------- */

TEST_CASE("receiver decodes a multi-frame message from the radio's audio", "[js8][receiver][slow]") {
    // R1CBU 0.34 gave apps 11025 Hz (resampled here), 1.0 gives 12000 Hz.
    const int    RATE = GENERATE(11025, 12000);
    const double NSPS = 0.160 * RATE; // samples per symbol
    INFO("input rate " << RATE);

    auto frames = vc::build_message_frames("W1ABC", "FN42", "K2XYZ", "K2XYZ HELLO FROM THE X6100 TEST", false,
                                           false, 0);
    REQUIRE(frames.size() == 3);

    std::mutex               mu;
    std::condition_variable  cv;
    std::vector<std::string> messages;
    std::vector<std::string> decoded_frames;
    std::size_t              cycles = 0;

    Receiver::Config cfg;
    cfg.input_rate           = RATE;
    cfg.realign_threshold_ms = 0; // audio is fed much faster than real time

    Receiver::Callbacks cb;
    cb.on_frame = [&](const RxFrame &f) {
        std::lock_guard<std::mutex> l(mu);
        decoded_frames.push_back(f.text);
    };
    cb.on_message = [&](const RxFrame &m) {
        if (m.partial) return; // complete messages only
        std::lock_guard<std::mutex> l(mu);
        messages.push_back(m.text);
        cv.notify_all();
    };
    cb.on_cycle_done = [&](std::size_t) {
        std::lock_guard<std::mutex> l(mu);
        cycles++;
        cv.notify_all();
    };
    Receiver rx(cfg, cb);

    // The ring aligns to the wall clock on the first buffer, so start the
    // audio with silence up to the next 15 s slot boundary.
    const std::int64_t now      = wall_ms();
    const std::int64_t to_slot  = 15'000 - (now % 15'000);
    std::vector<float> audio((std::size_t)(to_slot * RATE / 1000), 0.0f);

    // Plus one whole silent slot. Its decode builds the decoder's FFT plans
    // (seconds on a slow machine), so we wait for it before the real signal
    // and never have a decode skipped because the previous one is running.
    audio.resize(audio.size() + 15 * RATE, 0.0f);
    const std::size_t warmup_end = audio.size();

    const auto &costas = js8core::protocol::costas(js8core::protocol::CostasType::Original);
    for (auto &[text, bits] : frames) {
        int tones[js8core::kJs8NumSymbols];
        js8core::legacy_encode(bits, costas, text.c_str(), tones);

        std::size_t slot_start = audio.size();
        audio.resize(slot_start + 15 * RATE, 0.0f);
        double phi   = 0;
        auto   start = slot_start + RATE / 2; // JS8 Normal's 0.5 s start delay
        for (int s = 0; s < js8core::kJs8NumSymbols; ++s) {
            double dphi = 2 * M_PI * (1200.0 + tones[s] * 6.25) / RATE;
            for (int i = 0; i < (int)NSPS; ++i) {
                audio[start + (std::size_t)(s * NSPS) + i] += 0.1f * (float)std::sin(phi);
                phi += dphi;
            }
        }
    }
    audio.resize(audio.size() + 15 * RATE, 0.0f); // one more slot to trigger the last decode

    std::mt19937                    rng(7);
    std::normal_distribution<float> noise(0.0f, 0.01f);
    for (auto &s : audio) s += noise(rng);

    // Feed at 10x real time: fast, but in steady pieces like a sound card,
    // and slow enough that each slot's decode finishes before the next one
    // is due (the engine skips a decode while one is still running).
    const std::size_t PIECE = RATE / 10; // 100 ms of audio per 10 ms
    auto feed_range = [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; i += PIECE) {
            rx.feed(&audio[i], std::min<std::size_t>(PIECE, to - i));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    feed_range(0, warmup_end);
    {
        std::unique_lock<std::mutex> l(mu);
        REQUIRE(cv.wait_for(l, std::chrono::seconds(60), [&] { return cycles > 0; }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    feed_range(warmup_end, audio.size());

    std::unique_lock<std::mutex> l(mu);
    bool got = cv.wait_for(l, std::chrono::seconds(120), [&] { return !messages.empty(); });
    INFO("frames decoded: " << decoded_frames.size());
    REQUIRE(got);
    CHECK(messages[0] == "W1ABC: K2XYZ HELLO FROM THE X6100 TEST");
    CHECK(decoded_frames.size() == 3);
}

TEST_CASE("receiver re-snaps to the clock when audio and wall time disagree", "[js8][receiver]") {
    Receiver::Config cfg;
    cfg.input_rate = 11025;
    Receiver rx(cfg, {});

    // 5 s of audio in well under 2 s of wall time: the sample count runs ahead.
    std::vector<float> piece(11025 / 10, 0.0f);
    for (int i = 0; i < 50; ++i) rx.feed(piece.data(), piece.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rx.realign_count() == 0); // not checked until 2 s have passed

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    rx.feed(piece.data(), piece.size());
    for (int i = 0; i < 40 && rx.realign_count() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(rx.realign_count() == 1);
}

TEST_CASE("receiver fills an audio gap with silence instead of realigning", "[js8][receiver]") {
    // The dialog stops feeding audio while it transmits. A plain realign would
    // leave minute-old audio in the ring, and it would decode again as new.
    Receiver::Config cfg;
    cfg.input_rate = 11025;
    std::mutex               mu;
    std::vector<std::string> logs;
    Receiver::Callbacks      cb;
    cb.on_log = [&](const std::string &s) {
        std::lock_guard<std::mutex> lk(mu);
        logs.push_back(s);
    };
    Receiver rx(cfg, cb);

    std::vector<float> piece(11025 / 10, 0.0f);
    rx.feed(piece.data(), piece.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(2600)); // ~2.5 s missing
    rx.feed(piece.data(), piece.size());

    bool filled = false;
    for (int i = 0; i < 40 && !filled; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk(mu);
        for (auto &l : logs) filled |= l.rfind("gap:", 0) == 0;
    }
    CHECK(filled);
    CHECK(rx.realign_count() == 0);
}

TEST_CASE("lat/lon to Maidenhead grid", "[js8][ops][aprs]") {
    char g[12];
    REQUIRE(js8_latlon_to_grid(41.714775, -72.727260, 6, g, sizeof(g))); // W1AW
    CHECK(std::string(g) == "FN31PR");
    REQUIRE(js8_latlon_to_grid(0.0, 0.0, 10, g, sizeof(g)));
    CHECK(std::string(g) == "JJ00AA00AA");
    REQUIRE(js8_latlon_to_grid(-33.8688, 151.2093, 6, g, sizeof(g))); // Sydney
    CHECK(std::string(g) == "QF56OD");
    REQUIRE(js8_latlon_to_grid(90.0, 180.0, 4, g, sizeof(g))); // the far corner stays in range
    CHECK(std::string(g) == "RR99");
    // Longer grids extend the shorter ones.
    char g6[8], g10[12];
    REQUIRE(js8_latlon_to_grid(49.2827, -123.1207, 6, g6, sizeof(g6)));
    REQUIRE(js8_latlon_to_grid(49.2827, -123.1207, 10, g10, sizeof(g10)));
    CHECK(std::string(g10).rfind(g6, 0) == 0);
    CHECK(std::string(g6) == "CN89KG"); // downtown Vancouver; L starts at -123.0833
    CHECK_FALSE(js8_latlon_to_grid(95.0, 0.0, 6, g, sizeof(g)));
    CHECK_FALSE(js8_latlon_to_grid(0.0, 0.0, 7, g, sizeof(g)));
    CHECK_FALSE(js8_latlon_to_grid(0.0, 0.0, 10, g, 8));
}


TEST_CASE("a stepped system clock is noticed once; ntpd's slewing never", "[js8][timesync]") {
    ClockStepWatch w;
    std::int64_t   wall = 1'790'000'000'000, mono = 5'000;
    CHECK(w.check(wall, mono) == 0); // the first reading only sets the base
    // ntpd slewing at its fastest, 0.5 ms a second, for an hour of 100 ms blocks.
    for (int i = 0; i < 36'000; i++) {
        wall += 100;
        mono += 100;
        if (i % 20 == 0) wall += 1; // 0.5 ms/s, rounded to whole ms
        REQUIRE(w.check(wall, mono) == 0);
    }
    // Set from a GPS: the clock was 2.9 s fast.
    wall += 100 - 2'900;
    mono += 100;
    CHECK(w.check(wall, mono) == -2'900);
    wall += 100;
    mono += 100;
    CHECK(w.check(wall, mono) == 0); // reported once
    // Set by hand in the radio's Settings, 61 s ahead; a small 0.2 s nudge doesn't count.
    wall += 61'000;
    CHECK(w.check(wall, mono) == 61'000);
    wall += 200;
    CHECK(w.check(wall, mono) == 0);
}

TEST_CASE("auto time sync: desktop's average, one heartbeat to start", "[js8][timesync]") {
    AutoTimeSync a;
    CHECK_FALSE(a.pass_done(0));
    a.frame(0, -1200, 0); // Normal: counts
    a.frame(2, 5000, 0);  // Turbo: desktop leaves it out
    unsigned frames = 0;
    auto     d      = a.pass_done(0, &frames);
    REQUIRE(d);
    CHECK(*d == -1200); // the first frame sets it outright
    CHECK(frames == 1);
    a.frame(4, -1000, -1200); // Slow counts too
    d = a.pass_done(-1200);
    REQUIRE(d);
    CHECK(*d == -1100); // (1 x -1200 + -1000) / 2
    CHECK_FALSE(a.pass_done(-1100)); // a pass without frames sets nothing
    // A station 1.5 s off among many on time moves it a 60th, no more.
    for (int i = 0; i < 300; i++) {
        a.frame(0, 0, *d);
        d = a.pass_done(*d);
    }
    CHECK(std::llabs(*d) < 25);
    CHECK(a.count() == AutoTimeSync::MAX_N - 1);
    std::int64_t before = *d;
    a.frame(0, 1500, before);
    d = a.pass_done(before);
    CHECK(*d - before == (1500 - before) / AutoTimeSync::MAX_N);
    // A search's find counts as one frame; a reset starts afresh.
    a.restart(3000, true);
    a.frame(0, 2000, 3000);
    CHECK(*a.pass_done(3000) == 2500);
    a.restart(0, false);
    a.frame(0, -700, 0);
    CHECK(*a.pass_done(0) == -700);
}

TEST_CASE("auto time sync: the short way round each speed's slot", "[js8][timesync]") {
    // A frame 14 s late in a 15 s slot is 1 s early.
    CHECK(drift_short_way(14000, -500, 15000) == -1000);
    CHECK(drift_short_way(-14500, 0, 15000) == 500);
    CHECK(drift_short_way(29000, 0, 30000) == -1000); // Slow: 30 s
    CHECK(drift_short_way(800, 600, 15000) == 800);
    AutoTimeSync a;
    a.frame(0, 14000, -500);
    CHECK(*a.pass_done(-500) == -1000);
}

TEST_CASE("time search: the drift from where a signal starts", "[js8][timesync]") {
    const std::int64_t T = 1'791'000'000'000LL - 1'791'000'000'000LL % 15000; // a slot boundary
    // Our clock 5 s fast: a slot that really starts at T starts at T+5 s
    // by it, so JS8 time must be 5 s behind it.
    CHECK(TimeSearch::drift_for(T + 5000, 0.0f) == -5000);
    CHECK(TimeSearch::drift_for(T + 3000, 2.0f) == -5000); // window 2 s earlier, signal 2 s into it
    CHECK(TimeSearch::drift_for(T - 1200, 0.0f) == 1200);   // clock slow
    CHECK(TimeSearch::drift_for(T + 8000, 0.0f) == 7000);   // the short way: 8 s fast = 7 s slow
}

namespace {

// A CQ whose slot starts `lead_s` into the audio, as int16 (noise before it).
std::vector<std::int16_t> search_test_audio(double lead_s) {
    std::vector<TestStation> band = {{"W1ABC", "FN42", "CQ CQ CQ FN42", 1500, -18}};
    auto                     sig  = make_test_band(band, 12000, 0.02f, 7);
    auto                     pre  = make_test_band({}, 12000, 0.02f, 8);
    std::vector<float>       f(pre.begin(), pre.begin() + (std::ptrdiff_t)(lead_s * 12000));
    f.insert(f.end(), sig.begin(), sig.end());
    std::vector<std::int16_t> pcm(f.size());
    for (std::size_t i = 0; i < f.size(); i++) pcm[i] = (std::int16_t)std::lrintf(std::clamp(f[i], -1.0f, 1.0f) * 32767);
    return pcm;
}

} // namespace

TEST_CASE("time search: finds a signal 5 s out and gives up when nothing comes", "[js8][timesync]") {
    const std::int64_t T    = 1'791'000'000'000LL - 1'791'000'000'000LL % 15000;
    const double       lead = 20.0; // the slot starts 20 s into the audio
    auto               pcm  = search_test_audio(lead);
    // Our clock 5 s fast: audio sample i is at T + 5 s + (i - lead) by it.
    auto sys_at = [&](std::size_t i) { return T + 5000 + (std::int64_t)((double)i / 12 - lead * 1000); };

    std::mutex              m;
    std::condition_variable cv;
    std::optional<TimeSearch::Result> got;
    TimeSearch s([&](const TimeSearch::Result &r) {
        std::lock_guard<std::mutex> lock(m);
        got = r;
        cv.notify_all();
    });
    s.start(60000, 200, 3000, 1500);
    CHECK(s.active());
    // Exactly the window that starts just before the signal: one decode of
    // it. (Fed from the start, faster than real time, the worker could take
    // an earlier window part-way and then never see this one: Kris SP2KMO,
    // on OpenX6100 #314.)
    std::size_t end = (std::size_t)((lead - 0.5) * 12000) + 15 * 12000;
    for (std::size_t i = end - 15 * 12000; i < end; i += 1200) s.feed(&pcm[i], std::min<std::size_t>(1200, end - i), sys_at(std::min(end, i + 1200)));
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(60), [&] { return got.has_value(); });
    }
    REQUIRE(got);
    CHECK(got->found);
    CHECK(std::llabs(got->drift_ms + 5000) < 150);
    CHECK(got->text.find("W1ABC") != std::string::npos);
    CHECK_FALSE(s.active()); // stops at the first find

    got.reset();
    s.start(1500, 200, 3000, 1500); // nothing fed: time runs out
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return got.has_value(); });
    }
    REQUIRE(got);
    CHECK_FALSE(got->found);
    CHECK_FALSE(s.active());
}

// Which window starts (relative to the signal's slot) still decode it: the
// search's step must be no wider, or a signal could fall between windows.
TEST_CASE("time search: windows a step apart leave no gap", "[js8][timesync][slow]") {
    const std::int64_t T    = 1'791'000'000'000LL - 1'791'000'000'000LL % 15000;
    const double       lead = 20.0;
    auto               pcm  = search_test_audio(lead);
    int                lo = 99999, hi = -99999;
    std::string        seen;
    for (int x = -4000; x <= 4000; x += 250) { // window start - slot start, ms
        std::size_t at = (std::size_t)((lead * 1000 + x) * 12);
        auto        r  = TimeSearch::decode_window(&pcm[at], T + 5000 + x, 200, 3000, 1500);
        seen += r.found ? "#" : ".";
        if (!r.found) continue;
        CHECK(std::llabs(r.drift_ms + 5000) < 150);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    INFO("window start - slot start, -4..+4 s by 0.25 s: " << seen);
    WARN("decodes from window starts " << lo << " to " << hi << " ms: " << seen);
    CHECK(hi - lo + 250 >= TimeSearch::STEP_MS);
}

TEST_CASE("JS8 time is the system clock plus the Time Sync drift", "[js8][drift]") {
    using namespace std::chrono;
    auto sys = [] { return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(); };
    REQUIRE(js8_drift_ms() == 0);
    CHECK(std::llabs(js8_wall_ms() - sys()) < 50);
    js8_set_drift_ms(-1500);
    CHECK(js8_drift_ms() == -1500);
    CHECK(std::llabs(js8_wall_ms() - (sys() - 1500)) < 50);
    CHECK(std::llabs(x6100::js8::wall_ms() - (sys() - 1500)) < 50);
    js8_set_drift_ms(0);
    CHECK(std::llabs(js8_wall_ms() - sys()) < 50);
}

TEST_CASE("DT is 0 on time and positive when a signal starts late", "[js8][receiver][slow]") {
    // The sign the Time Sync button relies on: our clock 1 s fast means
    // signals appear 1 s late, DT = +1, correction -1 s.
    constexpr int    RATE = 11025;
    constexpr double NSPS = 0.160 * RATE;

    auto frames = vc::build_message_frames("W1ABC", "FN42", "", "W1ABC: @HB HEARTBEAT FN42", false, false, 0);
    REQUIRE(frames.size() == 1);

    for (double late : {0.0, 1.0}) {
        std::mutex         mu;
        std::condition_variable cv;
        std::vector<float> dts;
        std::size_t        cycles = 0;

        Receiver::Config cfg;
        cfg.input_rate           = RATE;
        cfg.realign_threshold_ms = 0;
        Receiver::Callbacks cb;
        cb.on_frame = [&](const RxFrame &f) {
            std::lock_guard<std::mutex> l(mu);
            dts.push_back(f.dt);
            cv.notify_all();
        };
        cb.on_cycle_done = [&](std::size_t) {
            std::lock_guard<std::mutex> l(mu);
            cycles++;
            cv.notify_all();
        };
        Receiver rx(cfg, cb);

        const std::int64_t now     = wall_ms();
        const std::int64_t to_slot = 15'000 - (now % 15'000);
        std::vector<float> audio((std::size_t)(to_slot * RATE / 1000), 0.0f);
        audio.resize(audio.size() + 15 * RATE, 0.0f); // warm-up slot
        const std::size_t warmup_end = audio.size();

        const auto &costas = js8core::protocol::costas(js8core::protocol::CostasType::Original);
        int         tones[js8core::kJs8NumSymbols];
        js8core::legacy_encode(frames[0].second, costas, frames[0].first.c_str(), tones);
        std::size_t slot_start = audio.size();
        audio.resize(slot_start + 30 * RATE, 0.0f);
        double phi   = 0;
        auto   start = slot_start + (std::size_t)((0.5 + late) * RATE);
        for (int s = 0; s < js8core::kJs8NumSymbols; ++s) {
            double dphi = 2 * M_PI * (1200.0 + tones[s] * 6.25) / RATE;
            for (int i = 0; i < (int)NSPS; ++i) {
                audio[start + (std::size_t)(s * NSPS) + i] += 0.1f * (float)std::sin(phi);
                phi += dphi;
            }
        }
        std::mt19937                    rng(7);
        std::normal_distribution<float> noise(0.0f, 0.01f);
        for (auto &x : audio) x += noise(rng);

        constexpr std::size_t PIECE = RATE / 10;
        auto feed_range = [&](std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; i += PIECE) {
                rx.feed(&audio[i], std::min<std::size_t>(PIECE, to - i));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        feed_range(0, warmup_end);
        {
            std::unique_lock<std::mutex> l(mu);
            REQUIRE(cv.wait_for(l, std::chrono::seconds(60), [&] { return cycles > 0; }));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        feed_range(warmup_end, audio.size());

        std::unique_lock<std::mutex> l(mu);
        REQUIRE(cv.wait_for(l, std::chrono::seconds(120), [&] { return !dts.empty(); }));
        INFO("late " << late << " s, DT " << dts[0]);
        CHECK(dts[0] == Catch::Approx(late).margin(0.2));
    }
}

/* ---- WAV files and test mode ------------------------------------------ */

#include "js8_rx.h"
#include "testsignal.hpp"
#include "wav.hpp"

#include <cstdio>
#include <filesystem>

TEST_CASE("WAV files round-trip", "[js8][wav]") {
    auto path = (std::filesystem::temp_directory_path() / "js8_wav_roundtrip.wav").string();
    std::vector<float> in = {0.0f, 0.5f, -0.5f, 0.99f, -1.0f, 0.25f};
    REQUIRE(write_wav(path, 12000, in));

    WavAudio    wav;
    std::string err;
    REQUIRE(read_wav(path, wav, err));
    CHECK(wav.rate == 12000);
    REQUIRE(wav.samples.size() == in.size());
    for (std::size_t i = 0; i < in.size(); i++) CHECK(wav.samples[i] == Catch::Approx(in[i]).margin(1.0 / 32767));
    std::remove(path.c_str());

    CHECK_FALSE(read_wav("/nonexistent/x.wav", wav, err));
    CHECK_FALSE(err.empty());
}

namespace {
struct Collected {
    std::mutex               mu;
    std::condition_variable  cv;
    std::vector<std::string> messages;
};
void collect_message(const js8_rx_msg_t *m, void *ctx) {
    if (m->partial) return; // complete messages only
    auto                       *c = static_cast<Collected *>(ctx);
    std::lock_guard<std::mutex> l(c->mu);
    c->messages.push_back(m->text);
    c->cv.notify_all();
}
} // namespace

TEST_CASE("decode marks follow desktop's Show decode attempts colours", "[js8][marks]") {
    // A decode is always marked, whatever its DT.
    CHECK(js8_mark_level(true, 0, 0.0f) == JS8_MARK_DECODED);
    CHECK(js8_mark_level(true, 0, 3.5f) == JS8_MARK_DECODED);
    // Candidates by sync strength: dark cyan, cyan, white.
    CHECK(js8_mark_level(false, 7, 0.1f) == JS8_MARK_WEAK);
    CHECK(js8_mark_level(false, 9, -1.0f) == JS8_MARK_WEAK);
    CHECK(js8_mark_level(false, 10, 0.0f) == JS8_MARK_MEDIUM);
    CHECK(js8_mark_level(false, 15, 0.0f) == JS8_MARK_MEDIUM);
    CHECK(js8_mark_level(false, 16, 0.0f) == JS8_MARK_STRONG);
    CHECK(js8_mark_level(false, 21, 2.0f) == JS8_MARK_STRONG);
    // Stronger candidates decode (and are marked then); far off the slot: none.
    CHECK(js8_mark_level(false, 22, 0.0f) == -1);
    CHECK(js8_mark_level(false, 12, 2.1f) == -1);
    CHECK(js8_mark_level(false, 12, -2.5f) == -1);
}

// Real time: up to 15 s to the slot boundary plus 45 s of audio.
TEST_CASE("test mode plays a 12 kHz WAV through the decoder", "[js8][wav][.slow]") {
    std::vector<TestStation> band = {
        {"W1ABC", "FN42", "K2XYZ HELLO FROM A WAV FILE", 1200, -8},
        {"VE3KP", "FN03", "CQ CQ CQ FN03", 800, -5},
    };
    auto path = (std::filesystem::temp_directory_path() / "js8_testmode.wav").string();
    REQUIRE(write_wav(path, 12000, make_test_band(band, 12000)));

    Collected   c;
    js8_rx_cb_t cb{};
    cb.on_message = collect_message;
    cb.ctx        = &c;
    js8_rx_t *rx  = js8_rx_create(11025, JS8_SUBMODE_NORMAL, "K2XYZ", &cb);
    REQUIRE(rx);

    char  err[128] = {};
    float starts   = js8_rx_play_wav(rx, path.c_str(), err, sizeof(err));
    INFO(err);
    REQUIRE(starts >= 0.5f);
    CHECK(js8_rx_wav_active(rx));

    {
        std::unique_lock<std::mutex> l(c.mu);
        c.cv.wait_for(l, std::chrono::seconds(90), [&] { return c.messages.size() >= 2; });
        std::sort(c.messages.begin(), c.messages.end());
        REQUIRE(c.messages.size() == 2);
        CHECK(c.messages[0] == "VE3KP: @ALLCALL CQ CQ CQ FN03");
        CHECK(c.messages[1] == "W1ABC: K2XYZ HELLO FROM A WAV FILE");
    }
    js8_rx_destroy(rx);
    std::remove(path.c_str());
}

/* ---- Transmit (phase T1) ---------------------------------------------- */

#include "tx.hpp"

TEST_CASE("plan_message builds JS8Call's frames and previews what others see", "[js8][tx]") {
    struct Case {
        const char *typed, *preview;
    };
    for (auto [typed, preview] : std::vector<Case>{
             {"k2xyz snr?", "W1ABC: K2XYZ SNR?"},
             {"K2XYZ   HELLO FROM THE X6100", "W1ABC: K2XYZ HELLO FROM THE X6100"},
             {"CQ CQ CQ FN42", "W1ABC: @ALLCALL CQ CQ CQ FN42"},
             {"@ALLCALL QRV ON 20M", "W1ABC: @ALLCALL QRV ON 20M"},
             {"K2XYZ MSG SEE YOU TOMORROW", "W1ABC: K2XYZ MSG SEE YOU TOMORROW"},
         }) {
        auto plan = plan_message("w1abc", "fn42", typed);
        INFO(typed << " -> " << plan.preview << " / " << plan.error);
        REQUIRE(plan.ok());
        CHECK(plan.preview == preview);
    }
}

TEST_CASE("untargeted free text is sent with our callsign", "[js8][tx]") {
    auto plan = plan_message("W1ABC", "FN42", "JUST TESTING THE NEW RADIO");
    REQUIRE(plan.ok());
    CHECK(plan.preview.find("W1ABC") != std::string::npos);
    CHECK(plan.preview.find("JUST TESTING THE NEW RADIO") != std::string::npos);
}

TEST_CASE("plan_message keeps the spaces inside an APRS command", "[js8][tx][aprs]") {
    auto p = plan_message("VE7NHW", "CN89", "  @aprsis cmd :SMS      :@6045551234 hi{01}\n");
    REQUIRE(p.ok());
    CHECK(p.text == "@APRSIS CMD :SMS      :@6045551234 HI{01}");
}

TEST_CASE("plan_message refuses what it can't send", "[js8][tx]") {
    CHECK_FALSE(plan_message("", "FN42", "K2XYZ SNR?").ok());
    CHECK_FALSE(plan_message("W1ABC", "FN42", "   ").ok());
    auto bad = plan_message("W1ABC", "FN42", "K2XYZ CTRL\x01HERE"); // printable ASCII only
    CHECK_FALSE(bad.ok());
    CHECK(bad.error.find("can't send") != std::string::npos);
    CHECK(plan_message("W1ABC", "FN42", "K2XYZ 50% OFF").ok());
    std::string longtext(400, 'A');
    CHECK_FALSE(plan_message("W1ABC", "FN42", longtext).ok());
}

TEST_CASE("next_tx_start_ms follows JS8 slot timing", "[js8][tx]") {
    const std::int64_t slot = 1'700'000'010'000; // a 15 s boundary
    REQUIRE(slot % 15000 == 0);
    CHECK(next_tx_start_ms(slot) == slot + 500);            // still in time for this slot
    CHECK(next_tx_start_ms(slot + 499) == slot + 500);
    CHECK(next_tx_start_ms(slot + 500) == slot + 15500);    // too late: next slot
    CHECK(next_tx_start_ms(slot + 14999) == slot + 15500);
}

TEST_CASE("TX waveform stays in its channel and at full scale", "[js8][tx]") {
    auto plan = plan_message("W1ABC", "FN42", "K2XYZ SNR?");
    REQUIRE(plan.ok());
    auto audio = synth_frame(plan.frames[0].tones, 1500, 11025);
    CHECK(audio.size() == (std::size_t)TX_SYMBOLS * 1764);
    float peak = 0;
    for (float x : audio) peak = std::max(peak, std::abs(x));
    CHECK(peak == Catch::Approx(1.0f).margin(0.01));
    // Energy well away from the 1500-1550 Hz channel is small.
    double in_band  = tone_amplitude(audio, 1525, 11025);
    double off_band = tone_amplitude(audio, 1700, 11025);
    CHECK(20 * std::log10(off_band / in_band) < -30.0);
}

TEST_CASE("TX audio at the radio's rate decodes in our receiver (loopback)", "[js8][tx][.slow]") {
    constexpr int RATE = 11025;
    auto          plan = plan_message("W1ABC", "FN42", "K2XYZ HELLO FROM THE TRANSMITTER");
    REQUIRE(plan.ok());
    REQUIRE(plan.frames.size() > 1);

    std::mutex               mu;
    std::condition_variable  cv;
    std::vector<std::string> messages;
    std::size_t              cycles = 0;
    Receiver::Config         cfg;
    cfg.input_rate           = RATE;
    cfg.realign_threshold_ms = 0;
    Receiver::Callbacks cb;
    cb.on_message = [&](const RxFrame &m) {
        if (m.partial) return; // complete messages only
        std::lock_guard<std::mutex> l(mu);
        messages.push_back(m.text);
        cv.notify_all();
    };
    cb.on_cycle_done = [&](std::size_t) {
        std::lock_guard<std::mutex> l(mu);
        cycles++;
        cv.notify_all();
    };
    Receiver rx(cfg, cb);

    // Lay the frames out as the transmitter would: one per slot, 0.5 s in,
    // at 30 dB below full scale plus noise.
    const std::int64_t now  = wall_ms();
    std::size_t        lead = (std::size_t)((15'000 - now % 15'000) * RATE / 1000) + 15 * RATE;
    std::vector<float> audio(lead + (plan.frames.size() + 1) * 15 * RATE, 0.0f);
    for (std::size_t i = 0; i < plan.frames.size(); i++) {
        auto        wave  = synth_frame(plan.frames[i].tones, 1200, RATE);
        std::size_t start = lead + i * 15 * RATE + RATE / 2;
        for (std::size_t k = 0; k < wave.size(); k++) audio[start + k] += 0.03f * wave[k];
    }
    std::mt19937                    rng(9);
    std::normal_distribution<float> noise(0.0f, 0.01f);
    for (auto &x : audio) x += noise(rng);

    const std::size_t piece = RATE / 10;
    for (std::size_t i = 0; i < lead; i += piece) {
        rx.feed(&audio[i], std::min(piece, lead - i));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        std::unique_lock<std::mutex> l(mu);
        REQUIRE(cv.wait_for(l, std::chrono::seconds(60), [&] { return cycles > 0; }));
    }
    for (std::size_t i = lead; i < audio.size(); i += piece) {
        rx.feed(&audio[i], std::min(piece, audio.size() - i));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::unique_lock<std::mutex> l(mu);
    REQUIRE(cv.wait_for(l, std::chrono::seconds(60), [&] { return !messages.empty(); }));
    CHECK(messages[0] == plan.preview);
    CHECK(messages[0] == "W1ABC: K2XYZ HELLO FROM THE TRANSMITTER");
}

namespace {
// Instant clock for Transmitter tests: waiting just advances time.
struct FakeClock : Transmitter::Clock {
    std::atomic<std::int64_t> t{1'700'000'003'000}; // partway into a slot
    std::int64_t now_ms() override { return t; }
    bool wait_until(std::int64_t ms, const std::atomic<bool> &cancelled) override {
        if (cancelled) return false;
        if (ms > t) t = ms;
        return true;
    }
};
} // namespace

TEST_CASE("a 20-frame message isn't cut anywhere", "[js8][tx]") {
    // Up to ~425 characters fit 20 frames (a repeated phrase packs best);
    // the C structs cut them at 255 before (VE7NHW, 2026-10-13).
    std::string text;
    while (text.size() < 425) text += "THANKS FOR THE QSO ";
    text.resize(425);
    js8_tx_preview_t pv;
    js8_tx_preview("VE7NHW", "CN89", text.c_str(), JS8_SPEED_FAST, &pv);
    REQUIRE(pv.ok);
    CHECK(pv.frames == 20);
    CHECK(std::strlen(pv.preview) >= 400);
    CHECK(std::string(pv.preview).find("THANKS FOR THE QSO THANKS") != std::string::npos);
    // Received, with the sender in front, it still fits.
    CHECK(text.size() + std::strlen("VE7NHW: ") < JS8_RX_TEXT_LEN);
    // One frame more is refused, not cut.
    text += " AND MORE TEXT HERE";
    js8_tx_preview("VE7NHW", "CN89", text.c_str(), JS8_SPEED_FAST, &pv);
    CHECK_FALSE(pv.ok);
}

TEST_CASE("Transmitter sends frames in consecutive slots", "[js8][tx]") {
    FakeClock clock;
    struct Keyed {
        std::int64_t at;
        int          index, count;
        std::size_t  samples;
    };
    std::vector<Keyed> keyed;
    bool               completed = false;
    std::mutex         mu;
    std::condition_variable cv;
    bool               done = false;

    Transmitter::Callbacks cb;
    cb.play = [&](const std::vector<float> &a, const TxFrame &, int i, int n) {
        keyed.push_back({clock.now_ms(), i, n, a.size()});
        clock.t += 12'640; // the frame's air time
        return true;
    };
    cb.on_done = [&](const std::string &, bool c) {
        std::lock_guard<std::mutex> l(mu);
        completed = c;
        done      = true;
        cv.notify_all();
    };
    Transmitter tx(11025, cb, &clock);

    auto plan = plan_message("W1ABC", "FN42", "K2XYZ HELLO FROM THE TRANSMITTER");
    REQUIRE(plan.frames.size() == 3);
    std::string why;
    REQUIRE(tx.send(plan, 1200, &why));
    CHECK_FALSE(tx.send(plan, 1200, &why)); // one message at a time
    CHECK(why == "already sending");

    std::unique_lock<std::mutex> l(mu);
    REQUIRE(cv.wait_for(l, std::chrono::seconds(10), [&] { return done; }));
    CHECK(completed);
    REQUIRE(keyed.size() == 3);
    const std::int64_t t0   = 1'700'000'003'000;
    const std::int64_t slot = t0 - t0 % 15'000 + 15'000; // next boundary after the fake clock's start
    for (int i = 0; i < 3; i++) {
        CHECK(keyed[i].at == slot + i * 15'000 + 500);
        CHECK(keyed[i].index == i);
        CHECK(keyed[i].count == 3);
        CHECK(keyed[i].samples == (std::size_t)TX_SYMBOLS * 1764);
    }
    CHECK_FALSE(tx.busy());
}

TEST_CASE("Transmitter stop abandons the rest of the message", "[js8][tx]") {
    FakeClock   clock;
    int         played = 0;
    bool        completed = true, done = false;
    std::mutex  mu;
    std::condition_variable cv;
    Transmitter *txp = nullptr;

    Transmitter::Callbacks cb;
    cb.play = [&](const std::vector<float> &, const TxFrame &, int, int) {
        played++;
        clock.t += 12'640;
        if (played == 1) {
            // The user presses Stop while the first frame is on the air.
            std::thread([&] { txp->stop(); }).detach();
            while (!txp->stopping()) std::this_thread::yield();
            return false;
        }
        return true;
    };
    cb.on_done = [&](const std::string &, bool c) {
        std::lock_guard<std::mutex> l(mu);
        completed = c;
        done      = true;
        cv.notify_all();
    };
    Transmitter tx(11025, cb, &clock);
    txp = &tx;

    REQUIRE(tx.send(plan_message("W1ABC", "FN42", "K2XYZ HELLO FROM THE TRANSMITTER"), 1200));
    std::unique_lock<std::mutex> l(mu);
    REQUIRE(cv.wait_for(l, std::chrono::seconds(10), [&] { return done; }));
    CHECK(played == 1);
    CHECK_FALSE(completed);
}

namespace {
js8_tx_t        *g_close_tx; // the dialog's global, as its tx_abort_check reads it
std::atomic<int> g_close_keyed{0};
std::atomic<int> g_close_saw_stop{0};

bool close_play(int16_t *, unsigned, int, int, void *) {
    g_close_keyed = 1;
    for (int k = 0; k < 300; k++) { // tx_player: an abort check between parts
        if (js8_tx_stopping(g_close_tx)) {
            g_close_saw_stop = 1;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}
} // namespace

TEST_CASE("closing the app while a frame is on the air stops it cleanly", "[js8][tx]") {
    // The dialog's tx_abort_check polls js8_tx_stopping(tx) on the TX thread
    // while destruct_cb runs js8_tx_destroy(tx) (GEN, APP or another app
    // mid-frame). It used to clear the Transmitter pointer before joining the
    // thread, and the poll crashed the app with the radio keyed.
    js8_tx_cb_t cb{};
    cb.play    = close_play;
    g_close_tx = js8_tx_create(48000, 1325, &cb);
    REQUIRE(g_close_tx);
    char err[64] = "";
    REQUIRE(js8_tx_send(g_close_tx, "VE7NHW", "CN89", "CQ CQ CQ", 1500, JS8_SPEED_TURBO, err, sizeof err));
    for (int i = 0; i < 800 && !g_close_keyed; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(g_close_keyed); // a Turbo slot comes within 6 s
    js8_tx_destroy(g_close_tx); // what tx_stop_all() does
    g_close_tx = nullptr;
    CHECK(g_close_saw_stop);
}

TEST_CASE("Transmitter rejects out-of-range offsets", "[js8][tx]") {
    FakeClock   clock;
    Transmitter tx(11025, {}, &clock);
    auto        plan = plan_message("W1ABC", "FN42", "K2XYZ SNR?");
    std::string why;
    CHECK_FALSE(tx.send(plan, 300, &why));
    CHECK_FALSE(tx.send(plan, 2980, &why));
    CHECK(why.find("offset") != std::string::npos);
}

/* ---- T3: queries, heartbeats, station list ---------------------------- */

#include "commands.hpp"
#include "stations.hpp"

TEST_CASE("one-press messages have desktop JS8Call's wording and survive the air", "[js8][t3]") {
    struct Case {
        Query       q;
        const char *text, *seen;
    };
    for (auto [q, text, seen] : std::vector<Case>{
             {Query::SnrQ, "N0XYZ SNR?", "W1ABC: N0XYZ SNR?"},
             {Query::SendSnr, "N0XYZ SNR -12", "W1ABC: N0XYZ SNR -12"},
             {Query::GridQ, "N0XYZ GRID?", "W1ABC: N0XYZ GRID?"},
             {Query::MyGrid, "N0XYZ GRID FN42AB", "W1ABC: N0XYZ GRID FN42AB"},
             {Query::InfoQ, "N0XYZ INFO?", "W1ABC: N0XYZ INFO?"},
             {Query::StatusQ, "N0XYZ STATUS?", "W1ABC: N0XYZ STATUS?"},
             {Query::HearingQ, "N0XYZ HEARING?", "W1ABC: N0XYZ HEARING?"},
             {Query::AgnQ, "N0XYZ AGN?", "W1ABC: N0XYZ AGN?"},
             {Query::RR, "N0XYZ RR", "W1ABC: N0XYZ RR"},
             {Query::SeventyThree, "N0XYZ 73", "W1ABC: N0XYZ 73"},
         }) {
        auto t = query_text(q, "n0xyz", -12, "fn42ab");
        CHECK(t == text);
        auto plan = plan_message("W1ABC", "FN42AB", t);
        INFO(t << " -> " << plan.preview << plan.error);
        REQUIRE(plan.ok());
        CHECK(plan.preview == seen);
    }
    CHECK(query_text(Query::SendSnr, "N0XYZ", 5, "") == "N0XYZ SNR +05");
    CHECK(query_text(Query::SendSnr, "N0XYZ", -99, "").empty()); // desktop sends nothing out of range
    CHECK(query_text(Query::MyGrid, "N0XYZ", 0, "").empty());
    CHECK(query_text(Query::SnrQ, "", 0, "").empty());
}

TEST_CASE("heartbeat is sent the way desktop JS8Call sends it", "[js8][t3]") {
    CHECK(heartbeat_text("w1abc", "fn42ab") == "W1ABC: HEARTBEAT FN42");
    auto plan = plan_message("W1ABC", "FN42AB", heartbeat_text("W1ABC", "FN42AB"));
    REQUIRE(plan.ok());
    CHECK(plan.frames.size() == 1);
    CHECK(plan.preview == "W1ABC: @HB HEARTBEAT FN42");
}

TEST_CASE("free heartbeat offsets follow desktop's rule", "[js8][t3]") {
    std::mt19937       rng(1);
    const std::int64_t now = 1'000'000;

    for (int i = 0; i < 50; i++) {
        int f = find_free_offset({}, now, rng);
        CHECK(f >= 500);
        CHECK(f < 1000);
    }

    // Everything but 700 Hz heard in the last 30 s. Desktop's search only
    // tries random slots, so it finds the one clear slot some of the time
    // and otherwise falls back to 500 Hz; never anything else.
    std::vector<OffsetActivity> busy;
    for (int f = 500; f < 1000; f += 50)
        if (f != 700) busy.push_back({(float)f, now - 5'000});
    int found = 0;
    for (int i = 0; i < 200; i++) {
        int f = find_free_offset(busy, now, rng);
        INFO(f);
        bool clear = true;
        for (auto &a : busy) clear &= std::fabs(a.offset_hz - f) >= 50;
        CHECK((clear || f == 500));
        found += clear;
    }
    CHECK(found > 50);

    // Activity older than 30 s doesn't count.
    std::vector<OffsetActivity> old;
    for (int f = 500; f < 1000; f += 10) old.push_back({(float)f, now - 31'000});
    int f = find_free_offset(old, now, rng);
    CHECK(f >= 500);
    CHECK(f < 1000);

    // A solid band falls back to 500 Hz, as desktop does.
    std::vector<OffsetActivity> full;
    for (int f2 = 450; f2 < 1050; f2 += 10) full.push_back({(float)f2, now});
    CHECK(find_free_offset(full, now, rng) == 500);
}

namespace {
// Run `text` from `call` through JS8Call's encoder and our decoder, then
// classify it as the dialog does.
StationEvent heard(const std::string &call, const std::string &grid, const std::string &text, int snr,
                   std::int64_t when, const std::string &my_call = "K2XYZ") {
    auto plan = plan_message(call, grid, text);
    REQUIRE(plan.ok());
    auto         mc = classify(plan.preview, my_call);
    StationEvent ev;
    ev.from    = mc.from;
    ev.to      = mc.to;
    ev.text    = plan.preview;
    ev.to_me   = mc.to_me;
    ev.snr     = snr;
    ev.freq_hz = 1000;
    ev.when_ms = when;
    return ev;
}
} // namespace

TEST_CASE("station list marks who heard us, with the SNR they reported", "[js8][t3]") {
    StationList list;
    const std::int64_t t0 = 10'000'000;

    list.add(heard("VE3KP", "FN03", "CQ CQ CQ FN03", -7, t0), "K2XYZ");
    list.add(heard("DL1XX", "JO62", "DL1XX: HEARTBEAT JO62", -24, t0 + 1000), "K2XYZ");
    // K9ABC acknowledges our heartbeat, as desktop JS8Call's auto-reply does.
    list.add(heard("K9ABC", "EN52", "K2XYZ HEARTBEAT SNR -08", -15, t0 + 2000), "K2XYZ");
    // G4ABC sends us a message (heard us, but no SNR report).
    list.add(heard("G4ABC", "IO91", "K2XYZ HELLO FROM LONDON", -19, t0 + 3000), "K2XYZ");
    // Our own heartbeat echoing back isn't a station.
    list.add(heard("K2XYZ", "FN42", "K2XYZ: HEARTBEAT FN42", 0, t0 + 4000), "K2XYZ");

    auto s = list.sorted(t0 + 5000);
    REQUIRE(s.size() == 4);
    CHECK(s[0].call == "G4ABC"); // heard us, most recently
    CHECK(s[0].heard_me);
    CHECK_FALSE(s[0].reported_snr.has_value());
    CHECK(s[1].call == "K9ABC");
    CHECK(s[1].heard_me);
    REQUIRE(s[1].reported_snr.has_value());
    CHECK(*s[1].reported_snr == -8);
    CHECK(s[1].snr == -15);
    CHECK(s[2].call == "DL1XX"); // then the rest, most recent first
    CHECK(s[2].grid == "JO62");
    CHECK_FALSE(s[2].heard_me);
    CHECK(s[3].call == "VE3KP");
    CHECK(s[3].grid == "FN03");

    // An hour later they've all expired.
    CHECK(list.sorted(t0 + StationList::EXPIRE_MS + 10'000).empty());
}

// Anyone can send this as free text; std::stoi threw on it (ultrareview R1).
TEST_CASE("a long number after SNR is no report, and no crash", "[js8][t3]") {
    // "K2XYZ SNR +9999999999" goes out as "SNR +09 999999999" (a command
    // carries a small number); with a word before SNR it's free text, the
    // digits as typed.
    const std::string huge = "+9999999999";
    StationList list;
    list.add(heard("K9ABC", "EN52", "K2XYZ HI SNR " + huge, -15, 10'000'000), "K2XYZ");
    list.add(heard("W1ABC", "FN42", "K2XYZ HI SNR -9999999999", -15, 10'001'000), "K2XYZ");
    auto s = list.sorted(10'002'000);
    REQUIRE(s.size() == 2);
    CHECK_FALSE(s[0].reported_snr.has_value());
    CHECK_FALSE(s[1].reported_snr.has_value());

    QsoTracker t;
    CHECK_FALSE(t.received("N7EAL", "N7EAL: VE7NHW HI SNR " + huge, true, -11, "VE7NHW", 1000));
    CHECK_FALSE(t.sent("VE7NHW: N7EAL HI SNR " + huge, "VE7NHW", 16000));
    auto q = t.get("N7EAL", 16000);
    REQUIRE(q);
    CHECK_FALSE(q->rcvd_snr.has_value());
    CHECK_FALSE(q->sent_snr.has_value());

    auto n = x6100::js8::history_note_rx("W1ABC", "W1ABC: K2XYZ SNR " + huge, true, "K2XYZ");
    REQUIRE(n);
    CHECK_FALSE(n->reported_snr.has_value());
    // Three digits still read.
    auto ok = x6100::js8::history_note_rx("W1ABC", "W1ABC: K2XYZ SNR -100", true, "K2XYZ");
    REQUIRE(ok);
    CHECK(ok->reported_snr == -100);
}

/* ---- T4: auto-reply, heartbeat acks, heartbeat timing ------------------ */

#include "autoreply.hpp"

namespace {
// A message from `call` built by JS8Call's encoder and decoded by ours.
Incoming incoming(const std::string &call, const std::string &text, int snr, const std::string &my_call = "K2XYZ") {
    auto plan = plan_message(call, "EN52", text);
    REQUIRE(plan.ok());
    auto     mc = classify(plan.preview, my_call);
    Incoming in;
    in.from      = mc.from;
    in.to        = mc.to;
    in.text      = plan.preview;
    in.to_me     = mc.to_me;
    in.to_group  = mc.to_group;
    in.heartbeat = mc.heartbeat;
    in.snr       = snr;
    return in;
}
AutoSettings settings() {
    AutoSettings s;
    s.my_call = "K2XYZ";
    s.my_grid = "FN42AB";
    s.info    = "X6100 5W EFHW";
    s.status  = "IDLE";
    return s;
}
} // namespace

TEST_CASE("auto-reply builds desktop JS8Call's answers to queries", "[js8][t4]") {
    auto                     s     = settings();
    std::vector<Heard>       heard = {"N0XYZ", "VE3KP", "K2XYZ", "G4ABC", "DL1XX", "W1ABC"};

    struct Case {
        const char *asked, *answer;
    };
    for (auto [asked, answer] : std::vector<Case>{
             {"K2XYZ SNR?", "N0XYZ SNR -12"},
             {"K2XYZ GRID?", "N0XYZ GRID FN42AB"},
             {"K2XYZ INFO?", "N0XYZ INFO X6100 5W EFHW"},
             {"K2XYZ STATUS?", "N0XYZ STATUS IDLE"},
             // up to 4, not the asker, not us
             {"K2XYZ HEARING?", "N0XYZ HEARING VE3KP G4ABC DL1XX W1ABC"},
             {"K2XYZ AGN?", "N0XYZ HELLO AGAIN"},
         }) {
        auto r = build_reply(incoming("N0XYZ", asked, -12), s, heard, "N0XYZ HELLO AGAIN");
        INFO(asked);
        REQUIRE(r.has_value());
        CHECK(r->text == answer);
        CHECK(r->kind == ReplyKind::Query);
        // What we'd send decodes back as a normal message from us.
        CHECK(plan_message("K2XYZ", "FN42AB", r->text).ok());
    }
}

TEST_CASE("auto-reply ignores what desktop JS8Call ignores", "[js8][t4]") {
    auto s = settings();
    // Queries to someone else, or to a group.
    CHECK_FALSE(build_reply(incoming("N0XYZ", "W1ABC SNR?", -5), s, {}, "").has_value());
    CHECK_FALSE(build_reply(incoming("N0XYZ", "@ALLCALL SNR?", -5), s, {}, "").has_value());
    // Low-confidence decodes.
    auto low           = incoming("N0XYZ", "K2XYZ SNR?", -5);
    low.low_confidence = true;
    CHECK_FALSE(build_reply(low, s, {}, "").has_value());
    // Nothing to say: no INFO text, no grid, nothing heard, nothing sent yet.
    auto empty = s;
    empty.info = empty.status = empty.my_grid = "";
    CHECK_FALSE(build_reply(incoming("N0XYZ", "K2XYZ INFO?", -5), empty, {}, "").has_value());
    CHECK_FALSE(build_reply(incoming("N0XYZ", "K2XYZ STATUS?", -5), empty, {}, "").has_value());
    CHECK_FALSE(build_reply(incoming("N0XYZ", "K2XYZ GRID?", -5), empty, {}, "").has_value());
    CHECK_FALSE(build_reply(incoming("N0XYZ", "K2XYZ HEARING?", -5), empty, {"N0XYZ"}, "").has_value());
    CHECK_FALSE(build_reply(incoming("N0XYZ", "K2XYZ AGN?", -5), empty, {}, "").has_value());
    // Ordinary chat isn't a query.
    CHECK_FALSE(build_reply(incoming("N0XYZ", "K2XYZ HELLO THERE", -5), s, {}, "").has_value());
}

TEST_CASE("heartbeats get desktop's ack; acks and our own traffic don't", "[js8][t4]") {
    auto s = settings();
    auto r = build_reply(incoming("K9ABC", "K9ABC: HEARTBEAT EN52", -8), s, {}, "");
    REQUIRE(r.has_value());
    CHECK(r->kind == ReplyKind::HeartbeatAck);
    CHECK(r->text == "K9ABC HEARTBEAT SNR -08");
    auto plan = plan_message("K2XYZ", "FN42AB", r->text);
    REQUIRE(plan.ok());
    CHECK(plan.preview == "K2XYZ: K9ABC HEARTBEAT SNR -08");

    // Someone's ack to us, or to another station: not acked again.
    CHECK_FALSE(build_reply(incoming("K9ABC", "K2XYZ HEARTBEAT SNR -08", -8), s, {}, "").has_value());
    CHECK_FALSE(build_reply(incoming("K9ABC", "W1ABC HEARTBEAT SNR -08", -8), s, {}, "").has_value());
    // Our own heartbeat heard back.
    CHECK_FALSE(build_reply(incoming("K2XYZ", "K2XYZ: HEARTBEAT FN42", -8), s, {}, "").has_value());
    // A CQ isn't a heartbeat.
    CHECK_FALSE(build_reply(incoming("VE3KP", "CQ CQ CQ FN03", -8), s, {}, "").has_value());
}

TEST_CASE("the switches: AUTO sends, off offers; HB ACK needs AUTO and HB", "[js8][t4]") {
    AutoPolicy         p;
    const std::int64_t t = 1'000'000'000;
    p.user_activity(t);
    auto s = settings();

    auto query = *build_reply(incoming("N0XYZ", "K2XYZ SNR?", -12), s, {}, "");
    auto ack   = *build_reply(incoming("K9ABC", "K9ABC: HEARTBEAT EN52", -8), s, {}, "");

    // All off (the default): offer the query reply, ignore heartbeats.
    CHECK(p.decide(query, s, t) == AutoPolicy::Action::Offer);
    CHECK(p.decide(ack, s, t) == AutoPolicy::Action::Ignore);

    s.autoreply = true;
    CHECK(p.decide(query, s, t) == AutoPolicy::Action::Send);
    s.hb_ack = true; // without HB networking: still no acks
    CHECK(p.decide(ack, s, t) == AutoPolicy::Action::Ignore);
    s.heartbeat = true;
    CHECK(p.decide(ack, s, t) == AutoPolicy::Action::Send);
    s.autoreply = false; // HB + HB ACK without AUTO: no acks either
    CHECK(p.decide(ack, s, t) == AutoPolicy::Action::Ignore);
    s.autoreply = true;

    // A question asked again gets its answer again, as on desktop (no
    // repeat guard); heartbeat acks: one per station per 55 min (desktop's
    // @ALLCALL cooldown).
    p.sent(query, t);
    CHECK(p.decide(query, s, t + 60'000) == AutoPolicy::Action::Send);
    p.sent(ack, t);
    CHECK(p.decide(ack, s, t + 50 * 60'000) == AutoPolicy::Action::Ignore);
    CHECK(p.decide(ack, s, t + AutoPolicy::HB_ACK_REPEAT_MS + 1) == AutoPolicy::Action::Send);

    // Idle watchdog: an hour without a key press stops automatic TX.
    AutoPolicy idle;
    idle.user_activity(t);
    CHECK(idle.decide(query, s, t + AutoPolicy::IDLE_MS + 1) == AutoPolicy::Action::Offer);
    CHECK(idle.decide(ack, s, t + AutoPolicy::IDLE_MS + 1) == AutoPolicy::Action::Ignore);
    idle.user_activity(t + AutoPolicy::IDLE_MS + 2);
    CHECK(idle.decide(query, s, t + AutoPolicy::IDLE_MS + 3) == AutoPolicy::Action::Send);
}

TEST_CASE("a QSO starts with any message to us except a heartbeat ack", "[js8][t4]") {
    CHECK(starts_qso(incoming("N0XYZ", "K2XYZ HELLO", -5)));
    CHECK(starts_qso(incoming("N0XYZ", "K2XYZ SNR?", -5)));
    CHECK_FALSE(starts_qso(incoming("K9ABC", "K2XYZ HEARTBEAT SNR -08", -5)));
    CHECK_FALSE(starts_qso(incoming("VE3KP", "CQ CQ CQ FN03", -5)));
    CHECK_FALSE(starts_qso(incoming("N0XYZ", "W1ABC HELLO", -5)));
    // The usual answers to a CQ, all with our call (they pause heartbeats).
    CHECK(starts_qso(incoming("N0XYZ", "K2XYZ HW CPY?", -5)));
    CHECK(starts_qso(incoming("N0XYZ", "K2XYZ GRID?", -5)));
    CHECK(starts_qso(incoming("N0XYZ", "K2XYZ SNR -12 GOOD MORNING", -5)));
    CHECK(starts_qso(incoming("N0XYZ", "K2XYZ MSG HELLO FROM THE PARK", -5)));
    CHECK_FALSE(starts_qso(incoming("N0XYZ", "K2XYZ/P HELLO", -5))); // /P is another call, as desktop
    // Not to us: someone's answer to someone else, a group, @ALLCALL.
    CHECK_FALSE(starts_qso(incoming("N0XYZ", "W1ABC HW CPY?", -5)));
    CHECK_FALSE(starts_qso(incoming("N0XYZ", "@ALLCALL SNR?", -5)));
}

TEST_CASE("heartbeat timing follows desktop's TxLoop", "[js8][t4]") {
    const std::int64_t now = 1'700'000'007'400; // 7.4 s past a 15 s boundary
    // The first: 10 min from now, up to the next slot of the speed.
    auto first = next_heartbeat_ms(now, 10, 15'000);
    CHECK(first % 15'000 == 0);
    CHECK(first >= now + 10 * 60'000);
    CHECK(first < now + 10 * 60'000 + 15'000);
    CHECK(next_heartbeat_ms(now, 10, 30'000) % 30'000 == 0); // Slow's 30 s slots
    // Then every 10 min after the one scheduled: on the grid, no jitter.
    CHECK(following_heartbeat_ms(first, first + 2'000, 10) == first + 10 * 60'000);
    // After a pause or a long message: whole intervals on, never in the past.
    CHECK(following_heartbeat_ms(first, first + 25 * 60'000, 10) == first + 30 * 60'000);
    CHECK(following_heartbeat_ms(first, first + 10 * 60'000, 10) == first + 20 * 60'000);
    // Interval clamped to 5-30 min.
    CHECK(next_heartbeat_ms(now, 1, 15'000) - now < 5 * 60'000 + 15'000);
    CHECK(next_heartbeat_ms(now, 99, 15'000) - now >= 30 * 60'000);
    CHECK(following_heartbeat_ms(first, first, 1) == first + 5 * 60'000);
}

TEST_CASE("auto-reply answers as desktop's processCommandActivity does", "[js8][t4][parity]") {
    // What arrives (from, text as desktop sends it, checksum good) and the
    // reply desktop JS8Call-improved (d9c50510,
    // JS8_Mainwindow/processCommandActivity.cpp) queues for it, or nothing.
    // Heard: W1ABC 5 min ago at -05, VE7ABC only through a relay (-64).
    struct Row {
        const char *from, *text, *reply;
    };
    const Row rows[] = {
        {"N0XYZ", "K2XYZ SNR?", "N0XYZ SNR -12"},
        {"N0XYZ", "K2XYZ GRID?", "N0XYZ GRID FN42AB"},
        {"N0XYZ", "K2XYZ INFO?", "N0XYZ INFO X6100 5W EFHW"},
        {"N0XYZ", "K2XYZ STATUS?", "N0XYZ STATUS IDLE"},
        {"N0XYZ", "K2XYZ AGN?", "N0XYZ HELLO FROM K2XYZ"},        // our last message again
        {"N0XYZ", "K2XYZ HEARING?", "N0XYZ HEARING W1ABC VE7ABC"}, // up to 4, not the asker
        {"N0XYZ", "@ALLCALL SNR?", nullptr},                       // questions never to everyone
        {"N0XYZ", "K2XYZ MSG HELLO THERE", "N0XYZ ACK"},
        {"N0XYZ", "@APRSIS MSG HELLO", nullptr}, // not a gateway's MSG TO: form: for the gateway
        {"N0XYZ", "K2XYZ QUERY MSGS", "N0XYZ NO"},
        {"N0XYZ", "@ALLCALL QUERY MSGS", nullptr}, // never NO to everyone
        {"N0XYZ", "K2XYZ QUERY CALL W1ABC?", "N0XYZ YES -05 (5m)"},
        {"N0XYZ", "K2XYZ QUERY CALL VE7ABC?", "N0XYZ YES (5m)"}, // relay-only: no SNR
        {"N0XYZ", "K2XYZ QUERY CALL ZZ9ZZ?", nullptr},             // never heard
        {"N0XYZ", "K2XYZ>W1ABC HELLO THERE", "W1ABC>HELLO THERE *DE* N0XYZ"},
        {"K9ABC", "K9ABC: HEARTBEAT EN52", "K9ABC HEARTBEAT SNR -12"},
    };
    const std::int64_t       now   = 1'000'000'000;
    const std::vector<Heard> heard = {Heard("W1ABC", -5, now - 300'000), Heard("VE7ABC", -64, now - 300'000)};
    for (auto &row : rows) {
        INFO(row.from << ": " << row.text);
        auto in        = incoming(row.from, row.text, -12);
        in.checksum_ok = true;
        in.when_ms     = now;
        auto r         = build_reply(in, settings(), heard, "N0XYZ HELLO FROM K2XYZ");
        if (!row.reply) {
            CHECK_FALSE(r);
            continue;
        }
        REQUIRE(r);
        CHECK(r->text == row.reply);
    }

    // And when desktop sends them: every time (a question asked again is
    // answered again), @ALLCALL and heartbeat answers once per station per
    // 55 min, QUERY MSGS / QUERY CALL only with AUTO on.
    AutoPolicy p;
    auto       s = settings();
    s.autoreply = s.heartbeat = s.hb_ack = true;
    p.user_activity(now);
    auto snr = *build_reply(incoming("N0XYZ", "K2XYZ SNR?", -12), s, {}, "");
    p.sent(snr, now);
    CHECK(p.decide(snr, s, now + 1000) == AutoPolicy::Action::Send);
    auto hb = *build_reply(incoming("K9ABC", "K9ABC: HEARTBEAT EN52", -12), s, {}, "");
    p.sent(hb, now);
    CHECK(p.decide(hb, s, now + 54 * 60'000) == AutoPolicy::Action::Ignore);
    CHECK(p.decide(hb, s, now + 56 * 60'000) == AutoPolicy::Action::Send);
    auto msgs = *build_reply(incoming("N0XYZ", "K2XYZ QUERY MSGS", -12), s, {}, "");
    s.autoreply = false;
    CHECK(p.decide(msgs, s, now) == AutoPolicy::Action::Ignore);
    CHECK(p.decide(snr, s, now) == AutoPolicy::Action::Offer);
}

// ---- QSO log ---------------------------------------------------------------

TEST_CASE("a two-way QSO ending in 73 is offered for logging once", "[js8][log]") {
    QsoTracker t;
    const std::string me = "VE7NHW";
    CHECK_FALSE(t.sent("VE7NHW: N7EAL SNR -12", me, 1000));
    CHECK_FALSE(t.received("N7EAL", "N7EAL: VE7NHW SNR -08 TNX", true, -11, me, 16000));
    CHECK_FALSE(t.received("N7EAL", "N7EAL: VE7NHW GRID DN17", true, -9, me, 31000));
    auto ended = t.sent("VE7NHW: N7EAL TU 73!", me, 46000);
    REQUIRE(ended);
    CHECK(*ended == "N7EAL");
    CHECK_FALSE(t.received("N7EAL", "N7EAL: VE7NHW 73 SK", true, -9, me, 61000)); // once

    auto q = t.get("N7EAL", 62000);
    REQUIRE(q);
    CHECK(q->start_ms == 1000);
    CHECK(q->sent_snr == -12);
    CHECK(q->rcvd_snr == -8);
    CHECK(q->heard_snr == -9);
    CHECK(q->grid == "DN17");

    t.logged("N7EAL");
    CHECK_FALSE(t.get("N7EAL", 62000));
}

TEST_CASE("heartbeat acks and one-way traffic aren't QSOs", "[js8][log]") {
    QsoTracker t;
    const std::string me = "VE7NHW";
    // They ack our heartbeat, we ack theirs: reports, but no QSO.
    CHECK_FALSE(t.received("KK6WVY", "KK6WVY: VE7NHW HEARTBEAT SNR -23", true, -20, me, 0));
    CHECK_FALSE(t.sent("VE7NHW: KK6WVY HEARTBEAT SNR -20", me, 15000));
    CHECK_FALSE(t.received("KK6WVY", "KK6WVY: VE7NHW 73", true, -20, me, 30000)); // we never sent
    auto q = t.get("KK6WVY", 30000);
    REQUIRE(q);
    CHECK(q->rcvd_snr == -23);
    CHECK(q->sent_snr == -20);
    CHECK_FALSE(q->we_sent);

    // Groups, our own call and other people's QSOs are ignored.
    CHECK_FALSE(t.sent("VE7NHW: @ALLCALL CQ CQ CN89 73", me, 0));
    CHECK_FALSE(t.received("W1ABC", "W1ABC: K2XYZ 73", false, -5, me, 0));
    CHECK_FALSE(t.get("W1ABC", 0));
    CHECK_FALSE(t.get("@ALLCALL", 0));
}

TEST_CASE("QSOs match base calls and expire", "[js8][log]") {
    QsoTracker t;
    const std::string me = "VE7NHW";
    t.sent("VE7NHW: VA7XYZ/P HELLO", me, 0);
    CHECK(t.received("VA7XYZ", "VA7XYZ: VE7NHW RR SK", true, -3, me, 15000));
    auto q = t.get("VA7XYZ/P", 15000);
    REQUIRE(q);
    CHECK(q->call == "VA7XYZ");

    // Half an hour later it's a new QSO.
    std::int64_t later = 15000 + QsoTracker::EXPIRE_MS + 1;
    CHECK_FALSE(t.get("VA7XYZ", later));
    t.sent("VE7NHW: VA7XYZ HI AGAIN", me, later);
    q = t.get("VA7XYZ", later);
    REQUIRE(q);
    CHECK(q->start_ms == later);
    CHECK_FALSE(q->they_sent);
    CHECK_FALSE(q->offered);
}

TEST_CASE("ADIF records are desktop JS8Call's plus power and POTA/SOTA", "[js8][log]") {
    LogEntry e;
    e.call     = "N7EAL";
    e.grid     = "DN17";
    e.rst_sent = format_snr(-12);
    e.rst_rcvd = format_snr(5);
    e.on_ms    = 1790000000000LL;          // 2026-09-21 14:13:20 UTC
    e.off_ms   = 1790000000000LL + 125000; // 14:15:25
    e.freq_hz  = 7078000 + 1500;
    e.my_call  = e.op_call = "VE7NHW";
    e.my_grid  = "CN89KG";
    e.comment  = "FIRST X6100 JS8";
    e.tx_pwr_w = 5;
    e.pota_ref = "CA-1234";
    CHECK(adif_record(e) ==
          "<call:5>N7EAL <gridsquare:4>DN17 <mode:4>MFSK <submode:3>JS8 <rst_sent:3>-12 <rst_rcvd:3>+05 "
          "<qso_date:8>20260921 <time_on:6>141320 <qso_date_off:8>20260921 <time_off:6>141525 <band:3>40m "
          "<freq:8>7.079500 <station_callsign:6>VE7NHW <my_gridsquare:6>CN89KG <comment:15>FIRST X6100 JS8 "
          "<operator:6>VE7NHW <tx_pwr:1>5 <my_sig:4>POTA <my_sig_info:7>CA-1234 <eor>\n");

    LogEntry s = e;
    s.pota_ref.clear();
    s.sota_ref = "VE7/LM-001";
    s.grid.clear();
    s.comment  = "two\nlines";
    auto r     = adif_record(s);
    CHECK(r.find("<my_sota_ref:10>VE7/LM-001 <eor>") != std::string::npos);
    CHECK(r.find("my_sig") == std::string::npos);
    CHECK(r.find("gridsquare:") == r.find("my_gridsquare:") + 3); // only ours
    CHECK(r.find("<comment:9>two lines") != std::string::npos);
}

TEST_CASE("the ADIF file gets one header", "[js8][log]") {
    char path[] = "/tmp/js8_log_test_XXXXXX";
    int  fd     = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    LogEntry e;
    e.call = "N7EAL";
    e.on_ms = e.off_ms = 1790000000000LL;
    e.freq_hz          = 14078000;
    std::string err;
    REQUIRE(adif_append(path, e, err));
    e.call = "KN6OEH";
    REQUIRE(adif_append(path, e, err));

    FILE *f = fopen(path, "r");
    REQUIRE(f);
    std::string all;
    char        buf[512];
    while (fgets(buf, sizeof(buf), f)) all += buf;
    fclose(f);
    unlink(path);
    CHECK(all.rfind("X6100 JS8 log", 0) == 0);
    CHECK(all.find("<eoh>") == all.rfind("<eoh>"));
    CHECK(all.find("<call:5>N7EAL") < all.find("<call:6>KN6OEH"));
    CHECK(all.find("<band:3>20m") != std::string::npos);

    CHECK_FALSE(adif_append("/nonexistent/dir/log.adi", e, err));
    CHECK(err.find("can't open") == 0);
}

TEST_CASE("ADIF bands", "[js8][log]") {
    CHECK(adif_band(1842000) == "160m");
    CHECK(adif_band(3578000) == "80m");
    CHECK(adif_band(5357000) == "60m");
    CHECK(adif_band(7078000) == "40m");
    CHECK(adif_band(10130000) == "30m");
    CHECK(adif_band(14078000) == "20m");
    CHECK(adif_band(18104000) == "17m");
    CHECK(adif_band(21078000) == "15m");
    CHECK(adif_band(24922000) == "12m");
    CHECK(adif_band(28078000) == "10m");
    CHECK(adif_band(50318000) == "6m");
    CHECK(adif_band(9000000).empty());
    CHECK(format_snr(0) == "+00");
    CHECK(format_snr(-3) == "-03");
}

TEST_CASE("the QSO log C API", "[js8][log]") {
    js8_qsos_t *q = js8_qsos_create();
    REQUIRE(q);
    char ended[JS8_RX_CALL_LEN] = "";
    CHECK_FALSE(js8_qsos_sent(q, "K2XYZ: N0XYZ HELLO", "K2XYZ", 0, ended, sizeof(ended)));
    js8_rx_msg_t m{};
    snprintf(m.from, sizeof(m.from), "N0XYZ");
    snprintf(m.text, sizeof(m.text), "N0XYZ: K2XYZ SNR +03 73");
    m.to_me = true;
    m.snr   = -7;
    CHECK(js8_qsos_received(q, &m, "K2XYZ", 15000, ended, sizeof(ended)));
    CHECK(std::string(ended) == "N0XYZ");
    js8_qso_t out;
    REQUIRE(js8_qsos_get(q, "N0XYZ", 15000, &out));
    CHECK(out.two_way);
    CHECK(out.has_rcvd_snr);
    CHECK(out.rcvd_snr == 3);
    CHECK_FALSE(out.has_sent_snr);
    CHECK(out.heard_snr == -7);
    js8_qsos_logged(q, "N0XYZ");
    CHECK_FALSE(js8_qsos_get(q, "N0XYZ", 15000, &out));
    CHECK(std::string(js8_log_band(7078000)) == "40m");
    js8_qsos_destroy(q);
}

TEST_CASE("grids: 4 to 10 characters read, 6 kept, not RR73, most precise kept", "[js8][log]") {
    CHECK(is_grid("DN17"));
    CHECK(is_grid("DN17AB"));
    CHECK(is_grid("CN89KG12"));
    CHECK(is_grid("CN89KG12AB"));
    CHECK_FALSE(is_grid("RR73"));
    CHECK_FALSE(is_grid("DN1"));
    CHECK_FALSE(is_grid("DN17A"));
    CHECK_FALSE(is_grid("SN17")); // fields stop at R
    CHECK_FALSE(is_grid("DN17AB1"));

    CHECK(find_grid("VE7NHW GRID DN17 TU 73") == "DN17");
    CHECK(find_grid("VE7NHW MY QTH DN17AB NAME BOB") == "DN17AB");
    CHECK(find_grid("VE7NHW RR73") == "");
    CHECK(find_grid("@HB HEARTBEAT CN89") == "CN89");
    CHECK(find_grid("VE7NHW GRID CN89KG12AB") == "CN89KG"); // logged as 6
    CHECK(find_grid("VE7NHW QTH CN89KG12") == "CN89KG");

    // The sender's own grid, as desktop takes it: heartbeats, CQs and GRID
    // commands only (bug hunt 6).
    CHECK(announced_grid("@HB HEARTBEAT CN89") == "CN89");
    CHECK(announced_grid("@ALLCALL CQ CQ CQ FN03") == "FN03");
    CHECK(announced_grid("VE7NHW GRID DN17AB TNX") == "DN17AB");
    CHECK(announced_grid("@APRSIS GRID CN89KG12") == "CN89KG");
    CHECK(announced_grid("K2XYZ RR73 GRID EN34KS") == "EN34KS");
    CHECK(announced_grid("K2XYZ MY DAUGHTER LIVES IN EM12") == "");
    CHECK(announced_grid("K2XYZ W1ABC IS FN42") == "");
    CHECK(announced_grid("K2XYZ HEARTBEAT SNR -08") == "");
    CHECK(announced_grid("VE7NHW RR73") == "");

    CHECK(better_grid("", "DN17") == "DN17");
    CHECK(better_grid("DN17AB", "DN17") == "DN17AB");  // less precise: keep
    CHECK(better_grid("DN17", "DN17AB") == "DN17AB");
    CHECK(better_grid("DN17AB", "CN89") == "CN89");    // they moved
    CHECK(better_grid("DN17AB", "") == "DN17AB");
}

TEST_CASE("a station passing APRS messages back over JS8 gets the @ badge", "[js8][stations]") {
    StationList list;
    const std::int64_t t0 = 10'000'000;

    // NR4U relays an Echo test's answer to us: a gateway both ways.
    list.add(heard("NR4U", "EM95", "@APRSIS MSG TO:K2XYZ TEST DE ECHO", -12, t0), "K2XYZ");
    // W1GW relays an SMS to someone else: a gateway all the same.
    list.add(heard("W1GW", "FN42", "@APRSIS MSG TO:VE7ABC HELLO DE SMS", -15, t0 + 1000), "K2XYZ");
    // Sending to a gateway, or a grid spot, isn't one.
    list.add(heard("VE3KP", "FN03", "@APRSIS CMD :SMS      :@6045551234 HI", -7, t0 + 2000), "K2XYZ");
    list.add(heard("DL1XX", "JO62", "@APRSIS GRID JO62AB", -20, t0 + 3000), "K2XYZ");

    for (auto &st : list.sorted(t0 + 4000)) {
        INFO(st.call);
        CHECK(st.aprs_gate == (st.call == "NR4U" || st.call == "W1GW"));
    }
    // It stays when they're heard again with something else.
    list.add(heard("NR4U", "EM95", "NR4U: HEARTBEAT EM95", -10, t0 + 5000), "K2XYZ");
    for (auto &st : list.sorted(t0 + 6000))
        if (st.call == "NR4U") CHECK(st.aprs_gate);
}

TEST_CASE("the QSO and the station keep the grid they announced, not any grid-shaped word", "[js8][log]") {
    QsoTracker        t;
    const std::string me = "VE7NHW";
    t.sent("VE7NHW: N7EAL GRID?", me, 0);
    t.received("N7EAL", "N7EAL: VE7NHW GRID DN17AB TNX", true, -9, me, 15000);
    t.received("N7EAL", "N7EAL: VE7NHW RR73", true, -9, me, 30000);
    t.received("N7EAL", "N7EAL: VE7NHW HB DN17", true, -9, me, 45000);
    auto q = t.get("N7EAL", 45000);
    REQUIRE(q);
    CHECK(q->grid == "DN17AB");

    StationList st;
    StationEvent ev{"N7EAL", "VE7NHW", "N7EAL: VE7NHW GRID DN17AB", true, -9, 1500, 0, 1000};
    st.add(ev, me);
    ev.text = "N7EAL: VE7NHW RR73";
    st.add(ev, me);
    ev.text = "N7EAL: @HB HEARTBEAT DN17";
    ev.to_me = false;
    st.add(ev, me);
    auto list = st.sorted(1000);
    REQUIRE(list.size() == 1);
    CHECK(list[0].grid == "DN17AB");

    // Someone else's grid, or a grid in passing, isn't theirs (bug hunt 6).
    StationList st2;
    StationEvent e2{"N0XYZ", "VE7NHW", "N0XYZ: VE7NHW MY DAUGHTER LIVES IN EM12", true, -9, 1500, 0, 1000};
    st2.add(e2, me);
    CHECK(st2.sorted(1000)[0].grid.empty());
    QsoTracker t2;
    t2.sent("VE7NHW: N0XYZ HELLO", me, 0);
    t2.received("N0XYZ", "N0XYZ: VE7NHW W1ABC IS IN FN42", true, -9, me, 15000);
    REQUIRE(t2.get("N0XYZ", 15000));
    CHECK(t2.get("N0XYZ", 15000)->grid.empty());
}

TEST_CASE("a QSO ends on 73 near the end of a message, not anywhere", "[js8][log]") {
    const std::string me = "VE7NHW";
    auto ends = [&](const char *last) {
        QsoTracker t;
        t.sent("VE7NHW: N7EAL SNR -12", me, 1000);
        t.received("N7EAL", "N7EAL: VE7NHW SNR -08", true, -11, me, 16000);
        return (bool)t.received("N7EAL", last, true, -11, me, 31000);
    };
    CHECK(ends("N7EAL: VE7NHW TNX QSO 73"));
    CHECK(ends("N7EAL: VE7NHW 73 GL"));
    CHECK(ends("N7EAL: VE7NHW FB OM 73 ES GL"));
    CHECK(ends("N7EAL: VE7NHW RR73"));
    CHECK(ends("N7EAL: VE7NHW TU SK"));
    CHECK_FALSE(ends("N7EAL: VE7NHW 73 DEGREES HERE TODAY")); // bug hunt 13
    CHECK_FALSE(ends("N7EAL: VE7NHW RIG IS 73 WATTS INTO A DIPOLE"));
}

// ---- Inbox -------------------------------------------------------------------

TEST_CASE("MSG to us: the text for the inbox", "[js8][inbox]") {
    CHECK(msg_body("N0XYZ: K2XYZ MSG HELLO THERE", "K2XYZ") == "HELLO THERE");
    CHECK(msg_body("N0XYZ: K2XYZ MSG   SPACED  OUT ", "K2XYZ/P") == "SPACED  OUT");
    CHECK_FALSE(msg_body("N0XYZ: W1ABC MSG HELLO", "K2XYZ"));          // not ours
    CHECK_FALSE(msg_body("N0XYZ: K2XYZ MSG TO:W1ABC HELLO", "K2XYZ")); // store for W1ABC
    CHECK_FALSE(msg_body("N0XYZ: K2XYZ MSG", "K2XYZ"));
    CHECK_FALSE(msg_body("N0XYZ: K2XYZ HELLO MSG X", "K2XYZ"));
    CHECK_FALSE(msg_body("N0XYZ: @ALLCALL MSG HI", "K2XYZ"));

    CHECK(msg_id_offered("N0XYZ: K2XYZ YES MSG ID 3") == 3);
    CHECK(msg_id_offered("N0XYZ: K2XYZ HEARTBEAT SNR -08 MSG ID 42") == 42);
    CHECK_FALSE(msg_id_offered("N0XYZ: K2XYZ MSG ID X"));
    CHECK_FALSE(msg_id_offered("N0XYZ: K2XYZ NO"));
}

TEST_CASE("the inbox keeps messages, drops resends, survives a reload", "[js8][inbox]") {
    char path[] = "/tmp/js8_inbox_test_XXXXXX";
    int  fd     = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    unlink(path); // a missing file is an empty inbox

    Inbox b;
    REQUIRE(b.load(path));
    CHECK(b.size() == 0);
    int a = b.add("N0XYZ", "HELLO", 1000);
    int c = b.add("W1ABC", "TEST\tWITH TAB", 2000);
    CHECK(b.add("N0XYZ", "HELLO", 60000) == a); // resend: same message
    CHECK(b.add("N0XYZ", "HELLO", 1000 + Inbox::REPEAT_MS + 1) != a); // much later: new
    CHECK(b.size() == 3);
    CHECK(b.unread() == 3);
    CHECK(b.list().front().id == b.list().back().id + 2); // newest first
    CHECK(b.mark_read(a));
    CHECK_FALSE(b.mark_read(a));
    CHECK(b.unread() == 2);
    REQUIRE(b.save(path));

    Inbox r;
    REQUIRE(r.load(path));
    CHECK(r.size() == 3);
    CHECK(r.unread() == 2);
    REQUIRE(r.get(c));
    CHECK(r.get(c)->text == "TEST WITH TAB");
    CHECK(r.get(c)->from == "W1ABC");
    CHECK(r.get(a)->read);
    CHECK(r.remove(c));
    CHECK_FALSE(r.remove(c));
    CHECK(r.add("K9DEF", "NEXT", 5000) > c); // ids never reused
    unlink(path);

    // Full: the oldest read message goes first.
    Inbox full;
    for (std::size_t i = 0; i < Inbox::MAX_MESSAGES; i++) full.add("N0XYZ", "M" + std::to_string(i), (std::int64_t)i);
    full.mark_read(5);
    full.add("N0XYZ", "ONE MORE", 999);
    CHECK(full.size() == Inbox::MAX_MESSAGES);
    CHECK_FALSE(full.get(5));
    CHECK(full.get(1));
}

TEST_CASE("MSG gets desktop's ACK; QUERY MSGS a NO; MSG ID is only offered", "[js8][inbox]") {
    auto s  = settings();
    auto in = incoming("N0XYZ", "K2XYZ MSG HELLO FROM THE PARK", -5);
    in.checksum_ok = true;
    auto r         = build_reply(in, s, {}, "");
    REQUIRE(r);
    CHECK(r->text == "N0XYZ ACK");
    CHECK(r->kind == ReplyKind::MsgAck);

    in.checksum_ok = false; // not until the checksum is good
    CHECK_FALSE(build_reply(in, s, {}, ""));

    auto q = build_reply(incoming("N0XYZ", "K2XYZ QUERY MSGS", -5), s, {}, "");
    REQUIRE(q);
    CHECK(q->text == "N0XYZ NO");

    auto y = build_reply(incoming("N0XYZ", "K2XYZ YES MSG ID 3", -5), s, {}, "");
    REQUIRE(y);
    CHECK(y->text == "N0XYZ QUERY MSG 3");
    CHECK(y->kind == ReplyKind::Suggest);

    // AUTO on: ACK every time (a resend means they missed it); Suggest never sends.
    AutoPolicy p;
    s.autoreply = true;
    p.user_activity(0);
    CHECK(p.decide(*r, s, 0) == AutoPolicy::Action::Send);
    p.sent(*r, 0);
    CHECK(p.decide(*r, s, 1000) == AutoPolicy::Action::Send);
    CHECK(p.decide(*y, s, 0) == AutoPolicy::Action::Offer);
    s.autoreply = false;
    CHECK(p.decide(*r, s, 0) == AutoPolicy::Action::Offer);
}

TEST_CASE("the inbox C API", "[js8][inbox]") {
    char path[] = "/tmp/js8_inbox_c_XXXXXX";
    int  fd     = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    js8_inbox_t *b = js8_inbox_open(path);
    REQUIRE(b);
    js8_rx_msg_t m{};
    snprintf(m.from, sizeof(m.from), "N0XYZ");
    snprintf(m.text, sizeof(m.text), "N0XYZ: K2XYZ MSG MEET AT 1800Z");
    m.to_me    = true;
    m.checksum = 1;
    char text[JS8_RX_TEXT_LEN];
    REQUIRE(js8_msg_for_me(&m, "K2XYZ", text, sizeof(text)));
    CHECK(std::string(text) == "MEET AT 1800Z");
    m.checksum = -1;
    CHECK_FALSE(js8_msg_for_me(&m, "K2XYZ", text, sizeof(text)));

    int id = js8_inbox_add(b, "N0XYZ", "MEET AT 1800Z", 1000);
    CHECK(id > 0);
    CHECK(js8_inbox_unread(b) == 1);
    js8_inbox_msg_t list[4];
    REQUIRE(js8_inbox_list(b, list, 4) == 1);
    CHECK(std::string(list[0].text) == "MEET AT 1800Z");
    js8_inbox_mark_read(b, id);
    CHECK(js8_inbox_unread(b) == 0);
    js8_inbox_close(b);

    b = js8_inbox_open(path); // saved on every change
    CHECK(js8_inbox_count(b) == 1);
    js8_inbox_delete(b, id);
    CHECK(js8_inbox_count(b) == 0);
    js8_inbox_close(b);
    unlink(path);
}

// ---- Data files: full, unreadable, cut short ------------------------------------

namespace {
// A fresh directory for one test's data files, removed with them at the end.
struct TempDir {
    std::string path;
    TempDir() {
        char t[] = "/tmp/js8_files_XXXXXX";
        path     = mkdtemp(t);
    }
    ~TempDir() {
        chmod(path.c_str(), 0755);
        std::string cmd = "chmod -R u+rwX '" + path + "' && rm -rf '" + path + "'";
        (void)std::system(cmd.c_str());
    }
    std::string file(const char *name) const { return path + "/" + name; }
};

std::string slurp(const std::string &path) {
    std::ifstream     f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

bool exists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

// Files matching a glob pattern.
std::vector<std::string> matching(const std::string &pattern) {
    glob_t g{};
    std::vector<std::string> out;
    if (glob(pattern.c_str(), 0, nullptr, &g) == 0)
        for (std::size_t i = 0; i < g.gl_pathc; i++) out.push_back(g.gl_pathv[i]);
    globfree(&g);
    return out;
}

// "FROM: K2XYZ MSG text" as the receiver hands it over, checksum good.
js8_rx_msg_t msg_to_k2xyz(const char *from, const char *text) {
    js8_rx_msg_t m{};
    auto         plan = plan_message(from, "EN52", text);
    REQUIRE(plan.ok());
    auto mc = classify(plan.preview, "K2XYZ");
    snprintf(m.from, sizeof(m.from), "%s", from);
    snprintf(m.to, sizeof(m.to), "%s", mc.to.c_str());
    snprintf(m.text, sizeof(m.text), "%s", plan.preview.c_str());
    m.to_me    = mc.to_me;
    m.checksum = 1;
    m.snr      = -6;
    return m;
}

js8_auto_settings_t auto_on(js8_held_t *held) {
    js8_auto_settings_t st{};
    st.autoreply = true;
    st.relay     = true;
    st.my_call   = "K2XYZ";
    st.my_grid   = "FN42";
    st.held      = held;
    return st;
}

const char *const OLD_INBOX = "# X6100 JS8 inbox v2: id, UTC ms, U(nread)/R(ead), from, to, path, message\n"
                              "1\t1000\tU\tN0XYZ\tK2XYZ\tN0XYZ\tTHE OLD MESSAGE\n";
} // namespace

TEST_CASE("a full inbox still saves, announces and ACKs a new message", "[js8][inbox][files]") {
    // Bug hunt 8: once the inbox held 200 messages, a new one left its size
    // unchanged and was taken for a resend: not saved, not announced, but ACKed.
    TempDir      d;
    auto         ipath = d.file("js8_inbox.txt");
    js8_inbox_t *inbox = js8_inbox_open(ipath.c_str());
    js8_held_t  *held  = js8_held_open(d.file("js8_held.txt").c_str());
    for (int i = 0; i < 200; i++) js8_inbox_add(inbox, "N0XYZ", ("OLD " + std::to_string(i)).c_str(), i);
    REQUIRE(js8_inbox_count(inbox) == 200);

    js8_auto_t *a  = js8_auto_create();
    auto        st = auto_on(held);
    js8_auto_user_activity(a, 1000);
    auto              m = msg_to_k2xyz("W1ABC", "K2XYZ MSG ONE MORE MESSAGE");
    js8_stored_t      kept;
    js8_auto_result_t r;
    js8_process(a, &m, &st, nullptr, 0, "", 5000, inbox, &kept, &r);
    CHECK(kept.kind == JS8_STORED_INBOX);
    CHECK_FALSE(kept.resend); // announced as new
    CHECK(kept.id > 0);
    CHECK(std::string(r.text) == "W1ABC ACK");
    CHECK(js8_inbox_count(inbox) == 200); // the oldest made room
    CHECK(slurp(ipath).find("ONE MORE MESSAGE") != std::string::npos); // and it's on the card

    js8_auto_destroy(a);
    js8_inbox_close(inbox);
    js8_held_close(held);
}

TEST_CASE("an inbox file that can't be read is moved aside, not written over", "[js8][inbox][files]") {
    if (geteuid() == 0) SKIP("root can read any file");
    TempDir d;
    auto    ipath = d.file("js8_inbox.txt");
    { std::ofstream(ipath) << OLD_INBOX; }
    chmod(ipath.c_str(), 0); // exists, can't be read (an SD card read error)

    js8_inbox_t *b = js8_inbox_open(ipath.c_str());
    CHECK(std::string(js8_inbox_notice(b)).find("couldn't be read") != std::string::npos);
    CHECK(js8_inbox_count(b) == 0);
    auto aside = matching(ipath + ".unreadable-*");
    REQUIRE(aside.size() == 1);
    chmod(aside[0].c_str(), 0600);
    CHECK(slurp(aside[0]).find("THE OLD MESSAGE") != std::string::npos); // kept for the user

    CHECK(js8_inbox_add(b, "W1ABC", "A NEW ONE", 2000) > 0); // a new inbox from here on
    CHECK(slurp(ipath).find("A NEW ONE") != std::string::npos);
    CHECK(slurp(aside[0]).find("THE OLD MESSAGE") != std::string::npos);
    js8_inbox_close(b);

    // Held messages the same way.
    auto hpath = d.file("js8_held.txt");
    { std::ofstream(hpath) << "1\t1000\tH\tN0XYZ\tW1ABC\tMEET AT THE PARK\n"; }
    chmod(hpath.c_str(), 0);
    js8_held_t *h = js8_held_open(hpath.c_str());
    CHECK(std::string(js8_held_notice(h)).find("couldn't be read") != std::string::npos);
    CHECK(matching(hpath + ".unreadable-*").size() == 1);
    js8_held_close(h);
}

TEST_CASE("an inbox that can't be read or moved is never written over, and gets no ACK", "[js8][inbox][files]") {
    if (geteuid() == 0) SKIP("root can read any file");
    TempDir d;
    auto    ipath = d.file("js8_inbox.txt");
    { std::ofstream(ipath) << OLD_INBOX; }
    chmod(ipath.c_str(), 0);
    chmod(d.path.c_str(), 0555); // nor can it be renamed

    js8_inbox_t *inbox = js8_inbox_open(ipath.c_str());
    CHECK(std::string(js8_inbox_notice(inbox)).find("can't be read or moved") != std::string::npos);
    js8_auto_t *a  = js8_auto_create();
    auto        st = auto_on(nullptr);
    js8_auto_user_activity(a, 1000);
    auto              m = msg_to_k2xyz("W1ABC", "K2XYZ MSG CAN YOU HEAR ME");
    js8_stored_t      kept;
    js8_auto_result_t r;
    js8_process(a, &m, &st, nullptr, 0, "", 5000, inbox, &kept, &r);
    CHECK(kept.kind == JS8_STORED_INBOX);
    CHECK(kept.id == -1);                // not on the card...
    CHECK(r.action == JS8_AUTO_IGNORE); // ...so no ACK: their station can send it again
    js8_inbox_msg_t list[2];
    CHECK(js8_inbox_list(inbox, list, 2) == 1); // still shown until power-off

    chmod(d.path.c_str(), 0755);
    chmod(ipath.c_str(), 0600);
    CHECK(slurp(ipath) == OLD_INBOX); // untouched
    js8_auto_destroy(a);
    js8_inbox_close(inbox);
}

TEST_CASE("a save cut short by a power cut: the .tmp is put back", "[js8][inbox][files]") {
    TempDir d;
    auto    ipath = d.file("js8_inbox.txt");
    js8_inbox_t *b = js8_inbox_open(ipath.c_str());
    REQUIRE(js8_inbox_add(b, "N0XYZ", "KEEP ME", 1000) > 0);
    js8_inbox_close(b);
    CHECK_FALSE(exists(ipath + ".tmp")); // renamed into place after each save
    // Power cut on FAT32 after the old file went but before the new one
    // took its name: only the .tmp is left.
    REQUIRE(std::rename(ipath.c_str(), (ipath + ".tmp").c_str()) == 0);

    b = js8_inbox_open(ipath.c_str());
    CHECK(js8_inbox_count(b) == 1);
    CHECK(std::string(js8_inbox_notice(b)).empty());
    CHECK(exists(ipath));
    CHECK_FALSE(exists(ipath + ".tmp"));
    js8_inbox_close(b);
}

TEST_CASE("js8_texts.txt is written safely and read back", "[js8][files]") {
    TempDir     d;
    auto        p = d.file("js8_texts.txt");
    char        buf[256], notice[160];
    const char *text = "INFO=X6100 5W EFHW\nGROUPS=@NET @PNW\n";

    CHECK(js8_file_read(p.c_str(), buf, sizeof(buf), notice, sizeof(notice))); // none yet: empty
    CHECK(std::string(buf).empty());
    REQUIRE(js8_file_write(p.c_str(), text));
    CHECK_FALSE(exists(p + ".tmp"));
    CHECK(js8_file_read(p.c_str(), buf, sizeof(buf), notice, sizeof(notice)));
    CHECK(std::string(buf) == text);
    CHECK(std::string(notice).empty());

    REQUIRE(std::rename(p.c_str(), (p + ".tmp").c_str()) == 0); // a save cut short
    CHECK(js8_file_read(p.c_str(), buf, sizeof(buf), notice, sizeof(notice)));
    CHECK(std::string(buf) == text);
    CHECK(exists(p));
}

TEST_CASE("groups: only whole groups are kept when they don't all fit", "[js8][files]") {
    char out[40];
    js8_groups_normalise("GROUPONE GROUPTWO GROUPTHREE GROUPFOUR", out, sizeof(out));
    CHECK(std::string(out) == "@GROUPONE @GROUPTWO @GROUPTHREE"); // not "... @GROUPF"
    char big[160];
    js8_groups_normalise("A1 B2 C3 D4 E5 F6 G7 H8 I9 J10 K11", big, sizeof(big));
    CHECK(std::string(big) == "@A1 @B2 @C3 @D4 @E5 @F6 @G7 @H8 @I9 @J10"); // ten at most
}

// ---- Alerts ------------------------------------------------------------------

TEST_CASE("alert words: typed any way, saved one way", "[js8][alerts]") {
    auto w = parse_alert_words(" ve7abc, @pota  sota,,VE7ABC\tn7eal ");
    REQUIRE(w.size() == 4);
    CHECK(format_alert_words(w) == "VE7ABC @POTA SOTA N7EAL");
    CHECK(parse_alert_words("").empty());
    std::string many;
    for (int i = 0; i < 30; i++) many += "W" + std::to_string(i) + " ";
    CHECK(parse_alert_words(many).size() == 20);
}

TEST_CASE("alert words match whole words and the sender's call", "[js8][alerts]") {
    auto w = parse_alert_words("VE7ABC @POTA SOTA");
    CHECK(alert_word_hit("N7EAL: @POTA VE7/LM-001 SPOT", "N7EAL", w) == "@POTA");
    CHECK(alert_word_hit("N7EAL: VE7ABC HELLO", "N7EAL", w) == "VE7ABC");
    CHECK(alert_word_hit("VE7ABC/P: @HB HEARTBEAT CN89", "VE7ABC/P", w) == "VE7ABC"); // their portable call
    CHECK(alert_word_hit("N7EAL: K2XYZ>VE7ABC HI", "N7EAL", w) == "VE7ABC");          // relay path
    CHECK(alert_word_hit("N7EAL: W1ABC GOING SOTA TODAY", "N7EAL", w) == "SOTA");
    CHECK(alert_word_hit("N7EAL: W1ABC SOTAS AND POTA", "N7EAL", w) == "");           // whole words only
    CHECK(alert_word_hit("N7EAL: W1ABC HI", "N7EAL", {}) == "");
    // Punctuation around a word isn't part of it (B-26); @ and / are.
    CHECK(alert_word_hit("N7EAL: W1ABC GOING SOTA, 73", "N7EAL", w) == "SOTA");
    CHECK(alert_word_hit("N7EAL: W1ABC (SOTA) TODAY", "N7EAL", w) == "SOTA");
    CHECK(alert_word_hit("N7EAL: W1ABC HEARD VE7ABC?", "N7EAL", w) == "VE7ABC");
    CHECK(alert_word_hit("N7EAL: W1ABC POTA TODAY", "N7EAL", w) == ""); // @POTA wants the @

    char hit[16];
    CHECK(js8_alert_hit("N7EAL: @POTA HI", "N7EAL", "ve7abc @pota", hit, sizeof(hit)));
    CHECK(std::string(hit) == "@POTA");
    CHECK_FALSE(js8_alert_hit("N7EAL: HI", "N7EAL", "", hit, sizeof(hit)));
    char norm[64];
    js8_alert_words_normalise("sota,  pota", norm, sizeof(norm));
    CHECK(std::string(norm) == "SOTA POTA");
}

// ---- T6: speeds ----------------------------------------------------------------

TEST_CASE("the speed table matches desktop JS8Call's JS8Submode.cpp", "[js8][speed]") {
    struct Want {
        js8_speed_t id;
        int         varicode, bandwidth, period, delay, threshold, max_offset;
        double      spacing;
        bool        hb, original;
    };
    const Want want[] = {
        {JS8_SPEED_NORMAL, 0, 50, 15, 500, 10, 2950, 6.25, true, true},
        {JS8_SPEED_FAST, 1, 80, 10, 200, 16, 2920, 10.0, true, false},
        {JS8_SPEED_TURBO, 2, 160, 6, 100, 32, 2840, 20.0, false, false},
        {JS8_SPEED_SLOW, 4, 25, 30, 500, 10, 2975, 3.125, true, false},
        {JS8_SPEED_ULTRA, 8, 250, 4, 100, 50, 2750, 31.25, false, false}, // desktop's "JS8 60"
    };
    for (auto &w : want) {
        const Speed &sp = speed(w.id);
        INFO(sp.name);
        CHECK(sp.varicode == w.varicode);
        CHECK(sp.bandwidth_hz() == w.bandwidth);
        CHECK(sp.period_s == w.period);
        CHECK(sp.start_delay_ms == w.delay);
        CHECK(sp.rx_threshold_hz == w.threshold);
        CHECK(sp.max_offset_hz() == w.max_offset);
        CHECK(sp.tone_spacing_hz() == Catch::Approx(w.spacing));
        CHECK(sp.heartbeats == w.hb);
        CHECK(sp.original_costas == w.original);
        CHECK(&speed_from_varicode(w.varicode) == &sp);
        CHECK(js8_speed_from_submode(w.varicode) == w.id);
        CHECK(60 % sp.period_s == 0); // a minute is a slot start for every speed
        CHECK(sp.frame_seconds() < sp.period_s - sp.start_delay_ms / 1000.0);
    }
    CHECK(speed_from_varicode(99).id == JS8_SPEED_NORMAL);
    CHECK(js8_speed_letter(JS8_SPEED_TURBO) == 'T');
    CHECK(js8_speed_letter(JS8_SPEED_ULTRA) == 'U');
    CHECK(std::string(js8_speed_name(JS8_SPEED_SLOW)) == "Slow");
    CHECK(std::string(js8_speed_name(JS8_SPEED_TURBO)) == "Turbo");
    CHECK(std::string(js8_speed_name(JS8_SPEED_ULTRA)) == "Ultra");
    CHECK(js8_speed_rx_mask(JS8_SPEED_SLOW) == JS8_SUBMODE_SLOW);
    CHECK(js8_speed_rx_mask(JS8_SPEED_ULTRA) == JS8_SUBMODE_ULTRA);
}

TEST_CASE("each speed starts on its own slot grid", "[js8][speed][tx]") {
    const std::int64_t minute = 1'700'000'040'000; // a minute boundary
    REQUIRE(minute % 60000 == 0);
    CHECK(next_tx_start_ms(minute, JS8_SPEED_FAST) == minute + 200);
    CHECK(next_tx_start_ms(minute + 200, JS8_SPEED_FAST) == minute + 10'200);
    CHECK(next_tx_start_ms(minute + 3'000, JS8_SPEED_TURBO) == minute + 6'100);
    CHECK(next_tx_start_ms(minute + 6'099, JS8_SPEED_TURBO) == minute + 6'100);
    CHECK(next_tx_start_ms(minute + 1'000, JS8_SPEED_SLOW) == minute + 30'500);
    CHECK(next_tx_start_ms(minute + 29'999, JS8_SPEED_SLOW) == minute + 30'500);
    CHECK(next_tx_start_ms(minute + 1'000, JS8_SPEED_NORMAL) == minute + 15'500);
    CHECK(next_tx_start_ms(minute + 50, JS8_SPEED_ULTRA) == minute + 100);
    CHECK(next_tx_start_ms(minute + 1'000, JS8_SPEED_ULTRA) == minute + 4'100);
    CHECK(next_tx_start_ms(minute + 4'099, JS8_SPEED_ULTRA) == minute + 4'100);
}

TEST_CASE("messages plan and preview the same at every speed", "[js8][speed][tx]") {
    // Directed, free text (fast data outside Normal) and a checksummed MSG.
    const char *texts[] = {"K2XYZ SNR?", "JUST TESTING THE NEW RADIO", "K2XYZ MSG MEET AT THE PARK 1800Z"};
    const char *want[]  = {"W1ABC: K2XYZ SNR?", "W1ABC: JUST TESTING THE NEW RADIO",
                           "W1ABC: K2XYZ MSG MEET AT THE PARK 1800Z"};
    for (int s = 0; s < JS8_SPEED_COUNT; s++) {
        auto id = (js8_speed_t)s;
        INFO(speed(id).name);
        for (int i = 0; i < 3; i++) {
            auto plan = plan_message("W1ABC", "FN42", texts[i], id);
            REQUIRE(plan.ok());
            CHECK(plan.speed == id);
            CHECK(plan.preview == want[i]);
            const Speed &sp = speed(id);
            CHECK(plan.seconds() ==
                  Catch::Approx((plan.frames.size() - 1) * sp.period_s + 79 * sp.symbol_seconds()));
        }
    }
    // Free text really is packed differently outside Normal (desktop's
    // packFastDataMessage), and the tones use a different Costas array.
    auto n = plan_message("W1ABC", "FN42", "JUST TESTING THE NEW RADIO", JS8_SPEED_NORMAL);
    auto f = plan_message("W1ABC", "FN42", "JUST TESTING THE NEW RADIO", JS8_SPEED_FAST);
    CHECK(n.frames.back().frame != f.frames.back().frame);
    auto n1 = plan_message("W1ABC", "FN42", "K2XYZ SNR?", JS8_SPEED_NORMAL);
    auto t1 = plan_message("W1ABC", "FN42", "K2XYZ SNR?", JS8_SPEED_TURBO);
    CHECK(n1.frames[0].frame == t1.frames[0].frame); // same payload...
    CHECK(n1.frames[0].tones != t1.frames[0].tones); // ...different sync tones
}

TEST_CASE("TX audio has each speed's symbol length and bandwidth", "[js8][speed][tx]") {
    for (int s = 0; s < JS8_SPEED_COUNT; s++) {
        auto         id = (js8_speed_t)s;
        const Speed &sp = speed(id);
        INFO(sp.name);
        auto plan  = plan_message("W1ABC", "FN42", "K2XYZ SNR?", id);
        auto audio = synth_frame(plan.frames[0].tones, 1000, 11025, id);
        CHECK(audio.size() == (std::size_t)TX_SYMBOLS * (std::size_t)std::lround(sp.symbol_seconds() * 11025));
        double in_band  = tone_amplitude(audio, 1000 + sp.bandwidth_hz() / 2.0, 11025);
        double off_band = tone_amplitude(audio, 1000 + sp.bandwidth_hz() + 150, 11025);
        CHECK(20 * std::log10(off_band / in_band) < -30.0);
    }
}

TEST_CASE("Transmitter uses the plan's speed for slots and offsets", "[js8][speed][tx]") {
    FakeClock                 clock;
    std::vector<std::int64_t> starts;
    std::atomic<bool>         done{false};
    Transmitter::Callbacks    cb;
    cb.play = [&](const std::vector<float> &, const TxFrame &, int, int) {
        starts.push_back(clock.now_ms());
        clock.t += 7'900; // Fast frame air time
        return true;
    };
    cb.on_done = [&](const std::string &, bool) { done = true; };
    Transmitter tx(11025, cb, &clock);

    auto plan = plan_message("W1ABC", "FN42", "K2XYZ HELLO FROM THE FAST TRANSMITTER", JS8_SPEED_FAST);
    REQUIRE(plan.frames.size() >= 3);
    std::string why;
    CHECK_FALSE(tx.send(plan, 2930, &why)); // above Fast's 2920 Hz
    CHECK(why.find("Fast") != std::string::npos);
    REQUIRE(tx.send(plan, 2920, &why));
    for (int i = 0; i < 200 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(done);
    REQUIRE(starts.size() == plan.frames.size());
    CHECK(starts[0] % 10'000 == 200);
    for (std::size_t i = 1; i < starts.size(); i++) CHECK(starts[i] - starts[i - 1] == 10'000);

    // Turbo's top is 2840 Hz; Slow's 2975 Hz.
    CHECK_FALSE(tx.send(plan_message("W1ABC", "FN42", "K2XYZ SNR?", JS8_SPEED_TURBO), 2841, &why));
    while (tx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

namespace {

// Feed audio to a Receiver decoding `submodes`, starting at the next 30 s
// wall-clock boundary (a slot start for every speed) after one silent 30 s
// block that builds each speed's decoder. `audio` starts at a slot start.
// Fed at `speedup` x real time. Returns the assembled messages and every frame.
struct Decoded {
    std::vector<RxFrame> messages, frames, partials;
};

Decoded decode_all_speeds(const std::vector<float> &band, int rate, int submodes, double speedup,
                          int switch_to = 0) {
    std::mutex              mu;
    std::condition_variable cv;
    Decoded                 out;
    std::size_t             cycles = 0;
    Receiver::Config        cfg;
    cfg.input_rate           = rate;
    cfg.submodes             = submodes;
    cfg.realign_threshold_ms = 0;
    Receiver::Callbacks cb;
    cb.on_frame = [&](const RxFrame &f) {
        std::lock_guard<std::mutex> l(mu);
        out.frames.push_back(f);
    };
    cb.on_message = [&](const RxFrame &m) {
        std::lock_guard<std::mutex> l(mu);
        if (m.partial) {
            out.partials.push_back(m);
            return;
        }
        out.messages.push_back(m);
        cv.notify_all();
    };
    cb.on_cycle_done = [&](std::size_t) {
        std::lock_guard<std::mutex> l(mu);
        cycles++;
        cv.notify_all();
    };
    Receiver rx(cfg, cb);

    const std::int64_t now  = wall_ms();
    // The band starts on a minute: every speed's slots line up there (Ultra's
    // 4 s slots don't at :30, so a band starting then had Ultra frames
    // between slots and decoded them only half the time).
    std::int64_t       to_minute = 60'000 - now % 60'000;
    if (to_minute < 30'000) to_minute += 60'000; // at least 30 s for the receiver to settle
    std::size_t        lead = (std::size_t)(to_minute * rate / 1000);
    std::vector<float> audio(lead, 0.0f);
    audio.insert(audio.end(), band.begin(), band.end());
    audio.resize(audio.size() + (std::size_t)31 * rate, 0.0f); // time for the last decodes
    std::mt19937                    rng(11);
    std::normal_distribution<float> noise(0.0f, 0.005f);
    for (std::size_t i = 0; i < lead; i++) audio[i] += noise(rng);

    const std::size_t piece    = (std::size_t)rate / 10;
    const auto        piece_us = std::chrono::microseconds((long long)(100'000 / speedup));
    auto feed = [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; i += piece) {
            rx.feed(&audio[i], std::min(piece, to - i));
            std::this_thread::sleep_for(piece_us);
        }
    };
    feed(0, lead);
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(60), [&] { return cycles > 0; });
    }
    if (switch_to) rx.set_submodes(switch_to); // as the Decode button does, mid-run
    feed(lead, audio.size());
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::lock_guard<std::mutex> l(mu);
    return out;
}

bool has_message(const Decoded &d, const std::string &text, int mode) {
    for (auto &m : d.messages)
        if (m.text == text && m.mode == mode) return true;
    return false;
}

} // namespace

TEST_CASE("all five speeds decode together from one band", "[js8][speed][receiver][.slow]") {
    constexpr int RATE = 11025;
    // Multi-frame messages at every speed, spread over the band.
    std::vector<TestStation> band = {
        {"W1ABC", "FN42", "K2XYZ HELLO AT NORMAL SPEED", 700, -5, JS8_SPEED_NORMAL},
        {"K9DEF", "EN52", "K2XYZ HELLO AT FAST SPEED", 1100, -5, JS8_SPEED_FAST},
        {"N0XYZ", "EN34", "K2XYZ HELLO AT TURBO SPEED", 1500, -5, JS8_SPEED_TURBO},
        {"VE7ABC", "CN89", "K2XYZ HELLO AT SLOW", 2000, -5, JS8_SPEED_SLOW},
        {"KL7QXZ", "BP51", "K2XYZ HELLO AT ULTRA", 2400, -5, JS8_SPEED_ULTRA},
    };
    auto audio = make_test_band(band, RATE, 0.02f, 3);
    int  all   = JS8_SUBMODE_NORMAL | JS8_SUBMODE_FAST | JS8_SUBMODE_TURBO | JS8_SUBMODE_SLOW | JS8_SUBMODE_ULTRA;
    auto d     = decode_all_speeds(audio, RATE, all, 1.5);
    for (auto &m : d.messages) UNSCOPED_INFO("mode " << m.mode << " dt " << m.dt << ": " << m.text);
    CHECK(has_message(d, "W1ABC: K2XYZ HELLO AT NORMAL SPEED", 0));
    CHECK(has_message(d, "K9DEF: K2XYZ HELLO AT FAST SPEED", 1));
    CHECK(has_message(d, "N0XYZ: K2XYZ HELLO AT TURBO SPEED", 2));
    CHECK(has_message(d, "VE7ABC: K2XYZ HELLO AT SLOW", 4));
    CHECK(has_message(d, "KL7QXZ: K2XYZ HELLO AT ULTRA", 8));
    CHECK(d.messages.size() == 5); // no Turbo / Ultra retry decoded twice into a stray message
    // Each multi-frame message showed as it grew (desktop updates the line
    // every decode cycle), under the same id as the final message.
    for (auto &m : d.messages) {
        int grew = 0;
        for (auto &p : d.partials)
            if (p.msg_id == m.msg_id) {
                CHECK(m.text.rfind(p.text, 0) == 0); // the final text starts with what was shown
                grew++;
            }
        INFO(m.text);
        CHECK(grew >= 1);
    }
    // Time Sync: on-time signals at every speed have DT near 0.
    for (auto &f : d.frames) {
        INFO("mode " << f.mode << ": " << f.text);
        CHECK(std::fabs(f.dt) < 0.3f);
    }
}

TEST_CASE("our TX audio decodes at every speed (loopback)", "[js8][speed][tx][.slow]") {
    constexpr int RATE = 11025;
    for (int s = 0; s < JS8_SPEED_COUNT; s++) {
        auto         id = (js8_speed_t)s;
        const Speed &sp = speed(id);
        INFO(sp.name);
        auto plan = plan_message("W1ABC", "FN42", "K2XYZ LOOPBACK AT EVERY SPEED", id);
        REQUIRE(plan.ok());
        std::vector<float> band((std::size_t)((plan.frames.size() * sp.period_s + 30) / 30 * 30) * RATE, 0.0f);
        for (std::size_t i = 0; i < plan.frames.size(); i++) {
            auto        wave  = synth_frame(plan.frames[i].tones, 1200, RATE, id);
            std::size_t start = (i * sp.period_ms() + sp.start_delay_ms) * (std::size_t)RATE / 1000;
            for (std::size_t k = 0; k < wave.size(); k++) band[start + k] += 0.03f * wave[k];
        }
        std::mt19937                    rng(9);
        std::normal_distribution<float> noise(0.0f, 0.01f);
        for (auto &x : band) x += noise(rng);
        // Only this speed, as "Decode: mine only" would.
        auto d = decode_all_speeds(band, RATE, sp.rx_mask, 3.0);
        CHECK(has_message(d, plan.preview, sp.varicode));
        CHECK(plan.preview == "W1ABC: K2XYZ LOOPBACK AT EVERY SPEED");
    }
}

TEST_CASE("Ultra decodes on its own thread; decoders stay off the last core", "[js8][speed][receiver]") {
    std::mutex               mu;
    std::vector<std::string> lines;
    Receiver::Config         cfg;
    cfg.input_rate = 11025;
    cfg.submodes   = JS8_SUBMODE_NORMAL | JS8_SUBMODE_ULTRA;
    Receiver::Callbacks cb;
    cb.on_report = [&](const std::string &l) {
        std::lock_guard<std::mutex> lock(mu);
        lines.push_back(l);
    };
    Receiver rx(cfg, cb);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the threads start
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    {
        std::lock_guard<std::mutex> lock(mu);
        auto has = [&](const std::string &want) {
            return std::any_of(lines.begin(), lines.end(), [&](auto &l) { return l.find(want) != std::string::npos; });
        };
        for (auto &l : lines) UNSCOPED_INFO(l);
        if (n >= 3) {
            CHECK(has("decode thread main: off core " + std::to_string(n - 1)));
            CHECK(has("decode thread ultra: off core " + std::to_string(n - 1) + " (left for the screen), nice 5"));
        }
    }
    // The kernel's view of each thread: name, allowed cores, nice.
    int found = 0;
    for (auto &t : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream c(t.path() / "comm");
        std::string   name;
        std::getline(c, name);
        if (name != "js8-decode" && name != "js8-decode-u") continue;
        found++;
        std::ifstream st(t.path() / "status");
        std::string   line, allowed;
        while (std::getline(st, line))
            if (line.rfind("Cpus_allowed_list:", 0) == 0) allowed = line.substr(line.find_first_not_of(" \t", 18));
        INFO(name << " allowed " << allowed);
        if (n >= 3) CHECK(allowed == (n - 2 == 0 ? std::string("0") : "0-" + std::to_string(n - 2)));
        std::ifstream sf(t.path() / "stat");
        std::string   stat((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
        std::istringstream rest(stat.substr(stat.rfind(')') + 2));
        std::string        field;
        for (int k = 0; k < 17 && rest >> field; k++) {} // field 19: nice
        CHECK(std::stoi(field) == (name == "js8-decode-u" ? 5 : 0));
    }
    CHECK(found == 2);
}

TEST_CASE("speeds switched on while running decode from their next slot", "[js8][speed][receiver][.slow]") {
    constexpr int            RATE = 11025;
    std::vector<TestStation> band = {
        {"W1ABC", "FN42", "K2XYZ NORMAL AS BEFORE", 700, -5, JS8_SPEED_NORMAL},
        {"K9DEF", "EN52", "K2XYZ FAST AFTER THE SWITCH", 1500, -5, JS8_SPEED_FAST},
    };
    auto audio = make_test_band(band, RATE, 0.02f, 5);
    // Normal only, then Normal + Fast; and Normal only throughout.
    auto on  = decode_all_speeds(audio, RATE, JS8_SUBMODE_NORMAL, 1.0, JS8_SUBMODE_NORMAL | JS8_SUBMODE_FAST);
    auto off = decode_all_speeds(audio, RATE, JS8_SUBMODE_NORMAL, 1.5);
    for (auto &f : on.frames) UNSCOPED_INFO("on: mode " << f.mode << " " << f.text);
    CHECK(has_message(on, "W1ABC: K2XYZ NORMAL AS BEFORE", 0));
    CHECK(has_message(on, "K9DEF: K2XYZ FAST AFTER THE SWITCH", 1));
    CHECK(has_message(off, "W1ABC: K2XYZ NORMAL AS BEFORE", 0));
    for (auto &m : off.frames) CHECK(m.mode == 0); // nothing Fast when it's off
}

TEST_CASE("a long message shows as it grows, then completes under the same id", "[js8][assembler]") {
    std::vector<RxFrame> done, so_far;
    MessageAssembler     a([&](const RxFrame &m) { done.push_back(m); }, [&](const RxFrame &m) { so_far.push_back(m); });
    a.add(frame("A1AA: B1BB", FRAME_FIRST, 1000, 0));
    REQUIRE(so_far.size() == 1);
    CHECK(so_far[0].text == "A1AA: B1BB");
    CHECK(so_far[0].partial);
    a.add(frame(" HELLO", FRAME_DATA, 1000, 15'000));
    REQUIRE(so_far.size() == 2);
    CHECK(so_far[1].text == "A1AA: B1BB HELLO");
    CHECK(so_far[1].msg_id == so_far[0].msg_id);
    a.add(frame(" WORLD", FRAME_DATA | FRAME_LAST, 1000, 30'000));
    CHECK(so_far.size() == 2); // the last frame completes it instead
    REQUIRE(done.size() == 1);
    CHECK(done[0].text == "A1AA: B1BB HELLO WORLD");
    CHECK_FALSE(done[0].partial);
    CHECK(done[0].msg_id == so_far[0].msg_id);

    // Single-frame messages don't show partials, and get their own ids.
    a.add(frame("C1CC: D1DD SNR -05", FRAME_FIRST | FRAME_LAST, 1500, 40'000));
    CHECK(so_far.size() == 2);
    REQUIRE(done.size() == 2);
    CHECK(done[1].msg_id != done[0].msg_id);

    // One that never finishes completes after 60 s quiet, same id.
    a.add(frame("E1EE: F1FF", FRAME_FIRST, 700, 50'000));
    auto id = so_far.back().msg_id;
    a.flush_stale(110'001);
    REQUIRE(done.size() == 3);
    CHECK(done[2].msg_id == id);
    CHECK(done[2].text == "E1EE: F1FF");
}

// ---- Held messages (store and forward) ------------------------------------------

TEST_CASE("MSG TO: and QUERY MSG parse as desktop sends them", "[js8][held]") {
    auto a = msg_to_body("N0XYZ: K2XYZ MSG TO:W1ABC SEE YOU AT 1800Z", "K2XYZ");
    REQUIRE(a);
    CHECK(a->first == "W1ABC");
    CHECK(a->second == "SEE YOU AT 1800Z");
    auto b = msg_to_body("N0XYZ: K2XYZ MSG TO: W1ABC/P HELLO", "K2XYZ"); // desktop accepts the space too
    REQUIRE(b);
    CHECK(b->first == "W1ABC/P");
    CHECK(b->second == "HELLO");
    CHECK_FALSE(msg_to_body("N0XYZ: K2XYZ MSG HELLO", "K2XYZ"));          // for our inbox
    CHECK_FALSE(msg_to_body("N0XYZ: W9ZZZ MSG TO:W1ABC HI", "K2XYZ"));    // held by someone else
    CHECK_FALSE(msg_to_body("N0XYZ: K2XYZ MSG TO:W1ABC", "K2XYZ"));       // no text
    CHECK(query_msg_id("W1ABC: K2XYZ QUERY MSG 12") == 12);
    CHECK_FALSE(query_msg_id("W1ABC: K2XYZ QUERY MSGS"));
    CHECK_FALSE(query_msg_id("W1ABC: K2XYZ QUERY MSG X"));
    // Desktop's "QUERY MSG [ID]" template with the brackets left in, as
    // VE7NHW sent it on the air (2026-10-09): we take it all the same.
    CHECK(query_msg_id("VA7NHW: VE7NHW QUERY MSG [1]") == 1);
    CHECK(query_msg_id("VA7NHW: VE7NHW QUERY MSG [ID1]") == 1);
    CHECK(query_msg_id("VA7NHW: VE7NHW QUERY MSG [ID 1]") == 1);
    CHECK(query_msg_id("VA7NHW: VE7NHW QUERY MSG ID 12") == 12);
    CHECK_FALSE(query_msg_id("VA7NHW: VE7NHW QUERY MSG [ID]"));
    CHECK_FALSE(query_msg_id("VA7NHW: VE7NHW QUERY MSG []"));
}

TEST_CASE("held messages: stored by base call, next for a station, delivered", "[js8][held]") {
    char path[] = "/tmp/js8_held_test_XXXXXX";
    int  fd     = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    HeldMessages h;
    REQUIRE(h.load(path));
    int a = h.add("N0XYZ", "W1ABC/P", "FIRST", 1000);
    int b = h.add("K9DEF", "W1ABC", "SECOND", 2000);
    CHECK(h.add("N0XYZ", "W1ABC/P", "FIRST", 5000) == a); // a resend
    CHECK(h.get(a)->to == "W1ABC");
    CHECK(h.next_for("W1ABC") == a);         // oldest first
    CHECK(h.next_for("VE7/W1ABC") == a);     // any form of their call
    CHECK_FALSE(h.next_for("W9ZZZ"));
    CHECK(h.is_for(b, "W1ABC/M"));
    CHECK_FALSE(h.is_for(b, "W9ZZZ"));
    CHECK(h.mark_delivered(a));
    CHECK(h.next_for("W1ABC") == b);
    CHECK(h.waiting() == 1);
    REQUIRE(h.save(path));
    HeldMessages r;
    REQUIRE(r.load(path));
    CHECK(r.size() == 2);
    CHECK(r.get(a)->delivered);
    CHECK(r.get(b)->text == "SECOND");
    CHECK(r.get(b)->from == "K9DEF");
    CHECK(r.remove(b));
    CHECK_FALSE(r.next_for("W1ABC"));
    unlink(path);
}

TEST_CASE("store and forward replies follow desktop", "[js8][held]") {
    HeldMessages held;
    int          id = held.add("N0XYZ", "W1ABC", "MEET AT THE PARK", 1000);
    auto         s  = settings();
    s.held          = &held;

    // Someone leaves a message here for W1ABC: ACKed once the checksum is good.
    auto store = incoming("N0XYZ", "K2XYZ MSG TO:W1ABC MEET AT THE PARK", -5);
    store.checksum_ok = true;
    auto r            = build_reply(store, s, {}, "");
    REQUIRE(r);
    CHECK(r->text == "N0XYZ ACK");

    // W1ABC asks what we hold: YES MSG ID n; anyone else: NO.
    auto yes = build_reply(incoming("W1ABC", "K2XYZ QUERY MSGS", -5), s, {}, "");
    REQUIRE(yes);
    CHECK(yes->text == "W1ABC YES MSG ID " + std::to_string(id));
    auto no = build_reply(incoming("W9ZZZ", "K2XYZ QUERY MSGS", -5), s, {}, "");
    REQUIRE(no);
    CHECK(no->text == "W9ZZZ NO");

    // W1ABC fetches it: "W1ABC MSG <text> FROM N0XYZ", marked for delivery.
    auto q        = incoming("W1ABC", "K2XYZ QUERY MSG " + std::to_string(id), -5);
    q.checksum_ok = true;
    auto d        = build_reply(q, s, {}, "");
    REQUIRE(d);
    CHECK(d->text == "W1ABC MSG MEET AT THE PARK FROM N0XYZ");
    CHECK(d->deliver_id == id);
    // Not someone else's, not an unknown id, not a bad checksum.
    auto other        = incoming("W9ZZZ", "K2XYZ QUERY MSG " + std::to_string(id), -5);
    other.checksum_ok = true;
    CHECK_FALSE(build_reply(other, s, {}, ""));
    auto bad = incoming("W1ABC", "K2XYZ QUERY MSG " + std::to_string(id), -5);
    CHECK_FALSE(build_reply(bad, s, {}, ""));
    // The id typed into desktop's "QUERY MSG [ID]" with the brackets kept.
    for (std::string arg : {"[" + std::to_string(id) + "]", "[ID" + std::to_string(id) + "]",
                            "[ID " + std::to_string(id) + "]"}) {
        auto b        = incoming("W1ABC", "K2XYZ QUERY MSG " + arg, -5);
        b.checksum_ok = true;
        auto got      = build_reply(b, s, {}, "");
        INFO(arg);
        REQUIRE(got);
        CHECK(got->text == "W1ABC MSG MEET AT THE PARK FROM N0XYZ");
        CHECK(got->deliver_id == id);
    }
    // Asked again: it didn't get there, so AUTO sends it again, as desktop
    // does (it answers every time; QUERY MSGS only with AUTO on).
    {
        AutoPolicy p;
        auto       s2 = s;
        s2.autoreply  = true;
        p.user_activity(0);
        CHECK(p.decide(*d, s2, 0) == AutoPolicy::Action::Send);
        p.sent(*d, 0);
        CHECK(p.decide(*d, s2, 60 * 1000) == AutoPolicy::Action::Send);
        p.sent(*yes, 0);
        CHECK(p.decide(*yes, s2, 60 * 1000) == AutoPolicy::Action::Send);
        s2.autoreply = false;
        CHECK(p.decide(*yes, s2, 60 * 1000) == AutoPolicy::Action::Ignore);
        CHECK(p.decide(*d, s2, 60 * 1000) == AutoPolicy::Action::Offer);
    }

    // "@ALLCALL QUERY MSGS" (or @HB): YES MSG ID n from whoever holds one for
    // the asker, as on desktop; nobody answers NO to a group.
    auto all = build_reply(incoming("W1ABC", "@ALLCALL QUERY MSGS", -5), s, {}, "");
    REQUIRE(all);
    CHECK(all->text == "W1ABC YES MSG ID " + std::to_string(id));
    CHECK(all->kind == ReplyKind::Stored);
    CHECK(all->allcall);
    auto hbq = build_reply(incoming("W1ABC", "@HB QUERY MSGS", -5), s, {}, "");
    REQUIRE(hbq);
    CHECK(hbq->text == all->text);
    CHECK_FALSE(build_reply(incoming("W9ZZZ", "@ALLCALL QUERY MSGS", -5), s, {}, ""));
    CHECK_FALSE(build_reply(incoming("W1ABC", "@ALLCALL SNR?", -5), s, {}, ""));
    CHECK_FALSE(build_reply(incoming("W1ABC", "@ALLCALL QUERY MSGS", -5), settings(), {}, ""));

    // W1ABC's heartbeat gets "MSG ID n" in our ack.
    auto hb = build_reply(incoming("W1ABC", "@HB HEARTBEAT FN42", -8), s, {}, "");
    REQUIRE(hb);
    CHECK(hb->text == "W1ABC HEARTBEAT SNR -08 MSG ID " + std::to_string(id));
    auto hb2 = build_reply(incoming("W9ZZZ", "@HB HEARTBEAT FN42", -8), s, {}, "");
    REQUIRE(hb2);
    CHECK(hb2->text == "W9ZZZ HEARTBEAT SNR -08");

    // HW CPY? is only offered: how we hear them.
    auto hw = build_reply(incoming("W1ABC", "K2XYZ HW CPY?", -11), s, {}, "");
    REQUIRE(hw);
    CHECK(hw->text == "W1ABC SNR -11");
    CHECK(hw->kind == ReplyKind::Suggest);

    // Delivered: nothing more for W1ABC.
    held.mark_delivered(id);
    auto after = build_reply(incoming("W1ABC", "K2XYZ QUERY MSGS", -5), s, {}, "");
    REQUIRE(after);
    CHECK(after->text == "W1ABC NO");
    CHECK_FALSE(build_reply(incoming("W1ABC", "@ALLCALL QUERY MSGS", -5), s, {}, ""));
}

TEST_CASE("the held-message C API", "[js8][held]") {
    char path[] = "/tmp/js8_held_c_XXXXXX";
    int  fd     = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    js8_held_t  *h = js8_held_open(path);
    js8_rx_msg_t m{};
    snprintf(m.from, sizeof(m.from), "N0XYZ");
    snprintf(m.text, sizeof(m.text), "N0XYZ: K2XYZ MSG TO:W1ABC HELLO THERE");
    m.checksum = 1;
    char to[16], text[256];
    REQUIRE(js8_msg_to_for_me(&m, "K2XYZ", to, sizeof(to), text, sizeof(text)));
    CHECK(std::string(to) == "W1ABC");
    CHECK(std::string(text) == "HELLO THERE");
    int id = js8_held_add(h, "N0XYZ", to, text, 1000);
    CHECK(id > 0);
    CHECK(js8_held_waiting(h) == 1);
    js8_held_delivered(h, id);
    CHECK(js8_held_waiting(h) == 0);
    js8_held_msg_t list[4];
    REQUIRE(js8_held_list(h, list, 4) == 1);
    CHECK(list[0].delivered);
    js8_held_close(h);
    h = js8_held_open(path);
    CHECK(js8_held_count(h) == 1);
    js8_held_delete(h, id);
    CHECK(js8_held_count(h) == 0);
    js8_held_close(h);
    unlink(path);
}

// ---- Relays and store-and-forward, as desktop's processCommandActivity() -----

namespace {
// A buffered command (MSG, relay, QUERY ...) whose checksum checked out.
Incoming buffered(const std::string &call, const std::string &text, int snr = -5,
                  const std::string &my_call = "K2XYZ") {
    auto in        = incoming(call, text, snr, my_call);
    in.checksum_ok = true;
    return in;
}
AutoSettings settings_for(const std::string &call) {
    auto s    = settings();
    s.my_call = call;
    return s;
}
} // namespace

TEST_CASE("directed commands split the way desktop's CommandDetail holds them", "[js8][relay]") {
    struct Case {
        const char *text, *to, *cmd, *rest;
    };
    for (auto c : std::vector<Case>{
             {"N0XYZ: K2XYZ MSG TO: W1ABC HI", "K2XYZ", " MSG TO:", "W1ABC HI"},
             {"N0XYZ: K2XYZ MSG TO:W1ABC HI", "K2XYZ", " MSG TO:", "W1ABC HI"},
             {"N0XYZ: K2XYZ MSG HELLO", "K2XYZ", " MSG", "HELLO"},
             {"N0XYZ: K2XYZ MSGS ARE FUN", "K2XYZ", " ", "MSGS ARE FUN"},
             {"N0XYZ: K2XYZ > W1ABC HI", "K2XYZ", ">", "W1ABC HI"},
             {"N0XYZ: K2XYZ>W1ABC HI", "K2XYZ", ">", "W1ABC HI"},
             {"N0XYZ: K2XYZ QUERY MSG 3", "K2XYZ", " QUERY", "MSG 3"},
             {"N0XYZ: K2XYZ QUERY MSGS", "K2XYZ", " QUERY MSGS", ""},
             {"N0XYZ: K2XYZ QUERY MSGS?", "K2XYZ", " QUERY MSGS", ""},
             {"N0XYZ: K2XYZ QUERY CALL W1ABC?", "K2XYZ", " QUERY CALL", "W1ABC?"},
             {"N0XYZ: K2XYZ SNR?", "K2XYZ", " SNR?", ""},
             {"N0XYZ: K2XYZ SNR -12", "K2XYZ", " SNR", "-12"},
             {"N0XYZ: K2XYZ HEARTBEAT SNR -12 MSG ID 3", "K2XYZ", " HEARTBEAT SNR", "-12 MSG ID 3"},
             {"N0XYZ: K2XYZ ACK", "K2XYZ", " ACK", ""},
             {"N0XYZ: K2XYZ HELLO THERE", "K2XYZ", " ", "HELLO THERE"},
             {"N0XYZ: @ALLCALL QUERY MSGS", "@ALLCALL", " QUERY MSGS", ""},
         }) {
        INFO(c.text);
        auto d = parse_directed(c.text);
        REQUIRE(d);
        CHECK(d->from == "N0XYZ");
        CHECK(d->to == c.to);
        CHECK(d->cmd == c.cmd);
        CHECK(d->text == c.rest);
    }
    CHECK_FALSE(parse_directed("HELLO THERE"));
    // What our decoder makes of a relay JS8Call's encoder sent.
    auto relay = parse_directed(plan_message("N0XYZ", "EN52", "K2XYZ>W1ABC HELLO").preview);
    REQUIRE(relay);
    CHECK(relay->cmd == ">");
    CHECK(relay->text == "W1ABC HELLO");
}

TEST_CASE("relay hops and paths follow desktop's patterns", "[js8][relay]") {
    CHECK(relay_next_hop("W1ABC HELLO") == "W1ABC>HELLO");
    CHECK(relay_next_hop("W1ABC>N0CALL HELLO") == "W1ABC>N0CALL HELLO");
    CHECK(relay_next_hop("VE7/W1ABC/P HI") == "VE7/W1ABC/P>HI");
    CHECK_FALSE(relay_next_hop("HELLO *DE* N0XYZ"));
    CHECK_FALSE(relay_next_hop("SNR? *DE* N0XYZ"));
    CHECK_FALSE(relay_next_hop("MSG HELLO *DE* N0XYZ"));
    CHECK_FALSE(relay_next_hop("73 *DE* N0XYZ")); // desktop's \b: no hop before "*DE*"

    CHECK(relay_path_calls("K2XYZ", "HELLO *DE* N0XYZ") == std::vector<std::string>{"K2XYZ", "N0XYZ"});
    CHECK(relay_path_calls("W1ABC", "HI *DE* N0XYZ *DE* K2XYZ") ==
          std::vector<std::string>{"W1ABC", "K2XYZ", "N0XYZ"});
    CHECK(relay_path_calls("K2XYZ", "HELLO VIA N0XYZ") == std::vector<std::string>{"K2XYZ", "N0XYZ"});
    CHECK(relay_path_calls("K2XYZ", "HELLO") == std::vector<std::string>{"K2XYZ"});
    CHECK(path_display("K2XYZ>N0XYZ") == "N0XYZ via K2XYZ");
    CHECK(path_display("N0XYZ") == "N0XYZ");

    using P = std::pair<std::string, std::string>;
    CHECK(relayed_command("MSG HELLO *DE* N0XYZ") == P{" MSG", "HELLO *DE* N0XYZ"});
    CHECK(relayed_command("MSG TO:W1ABC HI *DE* N0XYZ") == P{" MSG TO:", "W1ABC HI *DE* N0XYZ"});
    CHECK(relayed_command("MSG TO: W1ABC HI") == P{" MSG TO:", "W1ABC HI"});
    CHECK(relayed_command("QUERY MSGS *DE* N0XYZ") == P{" QUERY MSGS", "*DE* N0XYZ"});
    CHECK(relayed_command("QUERY MSG 3 *DE* N0XYZ") == P{" QUERY", "MSG 3 *DE* N0XYZ"});
    CHECK(relayed_command("QUERY CALL W1ABC? *DE* N0XYZ") == P{" QUERY CALL", "W1ABC? *DE* N0XYZ"});
    CHECK(relayed_command("SNR? *DE* N0XYZ") == P{" SNR?", "*DE* N0XYZ"});
    CHECK_FALSE(relayed_command("HELLO *DE* N0XYZ"));
    CHECK_FALSE(relayed_command("73 *DE* N0XYZ")); // not one desktop answers

    CHECK(parse_callsigns("W1ABC? FN42") == std::vector<std::string>{"W1ABC"});
}

TEST_CASE("a relay goes on, ends with an ACK back along the path, and stops at the ACK", "[js8][relay]") {
    // N0XYZ asks us (K2XYZ) to pass a message on to W1ABC.
    auto fwd = build_reply(buffered("N0XYZ", "K2XYZ>W1ABC HELLO THERE"), settings(), {}, "");
    REQUIRE(fwd);
    CHECK(fwd->kind == ReplyKind::Relay);
    CHECK(fwd->text == "W1ABC>HELLO THERE *DE* N0XYZ");
    CHECK(fwd->to == "N0XYZ");
    auto sent = plan_message("K2XYZ", "FN42AB", fwd->text);
    REQUIRE(sent.ok());

    // W1ABC hears it from us: it ends there, ACKed back through us.
    auto at_w1   = buffered("K2XYZ", fwd->text, -9, "W1ABC");
    auto ack     = process(at_w1, settings_for("W1ABC"), {}, "");
    REQUIRE(ack.reply);
    CHECK(ack.reply->kind == ReplyKind::MsgAck);
    CHECK(ack.reply->text == "K2XYZ>N0XYZ ACK");
    CHECK(ack.store.kind == StoreAction::Kind::None); // plain relayed text isn't kept (desktop)

    // We pass the ACK on to N0XYZ; N0XYZ doesn't answer an ACK.
    auto back = build_reply(buffered("W1ABC", ack.reply->text), settings(), {}, "");
    REQUIRE(back);
    CHECK(back->text == "N0XYZ>ACK *DE* W1ABC");
    CHECK_FALSE(build_reply(buffered("K2XYZ", back->text, -5, "N0XYZ"), settings_for("N0XYZ"), {}, ""));

    // Two hops: each station adds itself.
    auto hop1 = build_reply(buffered("N0XYZ", "K2XYZ>W1ABC>VE7ABC HI"), settings(), {}, "");
    REQUIRE(hop1);
    CHECK(hop1->text == "W1ABC>VE7ABC HI *DE* N0XYZ");
    auto hop2 = build_reply(buffered("K2XYZ", hop1->text, -5, "W1ABC"), settings_for("W1ABC"), {}, "");
    REQUIRE(hop2);
    CHECK(hop2->text == "VE7ABC>HI *DE* N0XYZ *DE* K2XYZ");
    auto end = build_reply(buffered("W1ABC", hop2->text, -5, "VE7ABC"), settings_for("VE7ABC"), {}, "");
    REQUIRE(end);
    CHECK(end->text == "W1ABC>K2XYZ>N0XYZ ACK");
    REQUIRE(plan_message("VE7ABC", "CN89", end->text).ok());

    // Relay switched off: nothing passed on, nothing ACKed.
    auto off  = settings();
    off.relay = false;
    CHECK_FALSE(build_reply(buffered("N0XYZ", "K2XYZ>W1ABC HELLO THERE"), off, {}, ""));
    CHECK_FALSE(build_reply(buffered("K2XYZ", fwd->text, -9, "W1ABC"), [] {
                    auto s  = settings_for("W1ABC");
                    s.relay = false;
                    return s;
                }(),
                            {}, ""));
    // A bad checksum, or a relay for someone else: nothing.
    CHECK_FALSE(build_reply(incoming("N0XYZ", "K2XYZ>W1ABC HELLO THERE", -5), settings(), {}, ""));
    CHECK_FALSE(build_reply(buffered("N0XYZ", "W9ZZZ>W1ABC HELLO THERE"), settings(), {}, ""));

    // AUTO sends a relay every time it's asked; off, it's offered.
    AutoPolicy p;
    auto       s = settings();
    p.user_activity(0);
    CHECK(p.decide(*fwd, s, 0) == AutoPolicy::Action::Offer);
    s.autoreply = true;
    CHECK(p.decide(*fwd, s, 0) == AutoPolicy::Action::Send);
    p.sent(*fwd, 0);
    CHECK(p.decide(*fwd, s, 60'000) == AutoPolicy::Action::Send);
    CHECK(p.decide(*end, s, 0) == AutoPolicy::Action::Send);
}

TEST_CASE("a relay carrying a command gets that command's answer, back along the path", "[js8][relay]") {
    HeldMessages held;
    int          id = held.add("VE7ABC", "N0XYZ", "SEE YOU AT 1800Z", 1000);
    auto         s  = settings_for("W1ABC");
    s.held          = &held;
    // We're W1ABC; K2XYZ passes on what N0XYZ sent.
    auto relayed = [&](const std::string &text, int snr = -7) {
        return process(buffered("K2XYZ", "W1ABC>" + text + " *DE* N0XYZ", snr, "W1ABC"), s, {}, "");
    };

    auto snr = relayed("SNR?");
    REQUIRE(snr.reply);
    CHECK(snr.reply->text == "K2XYZ>N0XYZ SNR -07");
    auto info = relayed("INFO?");
    REQUIRE(info.reply);
    CHECK(info.reply->text == "K2XYZ>N0XYZ INFO X6100 5W EFHW");

    auto msg = relayed("MSG MEET AT THE PARK");
    CHECK(msg.store.kind == StoreAction::Kind::Inbox);
    CHECK(msg.store.from == "K2XYZ");
    CHECK(msg.store.path == "K2XYZ>N0XYZ");
    CHECK(msg.store.text == "MEET AT THE PARK *DE* N0XYZ");
    REQUIRE(msg.reply);
    CHECK(msg.reply->text == "K2XYZ>N0XYZ ACK");

    auto store = relayed("MSG TO:VE7ABC CALL ME");
    CHECK(store.store.kind == StoreAction::Kind::Held);
    CHECK(store.store.to == "VE7ABC");
    CHECK(store.store.path == "K2XYZ>N0XYZ");
    CHECK(store.store.text == "CALL ME *DE* N0XYZ");
    REQUIRE(store.reply);
    CHECK(store.reply->text == "K2XYZ>N0XYZ ACK");

    auto yes = relayed("QUERY MSGS");
    REQUIRE(yes.reply);
    CHECK(yes.reply->text == "K2XYZ>N0XYZ YES MSG ID " + std::to_string(id));
    auto get = relayed("QUERY MSG " + std::to_string(id));
    REQUIRE(get.reply);
    CHECK(get.reply->text == "K2XYZ>N0XYZ MSG SEE YOU AT 1800Z FROM VE7ABC");
    CHECK(get.reply->deliver_id == id);

    // A command desktop doesn't answer (NACK) gets nothing, not even the ACK.
    CHECK_FALSE(relayed("NACK").reply);
    // Everything we'd send encodes.
    for (auto *r : {&snr, &info, &msg, &store, &yes, &get}) CHECK(plan_message("W1ABC", "FN42", r->reply->text).ok());
}

TEST_CASE("held messages: +N waiting, NEXT MSG ID, relay switch", "[js8][held]") {
    HeldMessages held;
    int          a = held.add("N0XYZ", "W1ABC", "ONE", 1000);
    int          b = held.add("VE7ABC", "W1ABC", "TWO", 2000);
    int          c = held.add("K9DEF", "W1ABC", "THREE", 3000);
    auto         s = settings();
    s.held         = &held;
    auto ask       = [&](const std::string &text) { return build_reply(buffered("W1ABC", text), s, {}, ""); };

    CHECK(ask("K2XYZ QUERY MSGS")->text == "W1ABC YES MSG ID " + std::to_string(a) + " +2");
    CHECK(build_reply(incoming("W1ABC", "@HB HEARTBEAT FN42", -8), s, {}, "")->text ==
          "W1ABC HEARTBEAT SNR -08 MSG ID " + std::to_string(a) + " +2");
    CHECK(ask("K2XYZ QUERY MSG " + std::to_string(a))->text ==
          "W1ABC MSG ONE FROM N0XYZ NEXT MSG ID " + std::to_string(b) + " +1");
    held.mark_delivered(a);
    CHECK(ask("K2XYZ QUERY MSG " + std::to_string(b))->text ==
          "W1ABC MSG TWO FROM VE7ABC NEXT MSG ID " + std::to_string(c));
    held.mark_delivered(b);
    CHECK(ask("K2XYZ QUERY MSG " + std::to_string(c))->text == "W1ABC MSG THREE FROM K9DEF");
    CHECK(ask("K2XYZ QUERY MSGS")->text == "W1ABC YES MSG ID " + std::to_string(c));
    // Asked again for one already delivered: desktop sends it again.
    CHECK(ask("K2XYZ QUERY MSG " + std::to_string(a))->text ==
          "W1ABC MSG ONE FROM N0XYZ NEXT MSG ID " + std::to_string(c));

    // Relay switched off: desktop holds nothing for others either (MSG to
    // us still goes to the inbox).
    s.relay = false;
    auto to = process(buffered("N0XYZ", "K2XYZ MSG TO:W1ABC HI"), s, {}, "");
    CHECK(to.store.kind == StoreAction::Kind::None);
    CHECK_FALSE(to.reply);
    auto mine = process(buffered("N0XYZ", "K2XYZ MSG HI"), s, {}, "");
    CHECK(mine.store.kind == StoreAction::Kind::Inbox);
    CHECK(mine.store.path == "N0XYZ");
    REQUIRE(mine.reply);
    CHECK(mine.reply->text == "N0XYZ ACK");
}

TEST_CASE("group messages: kept for members of our groups, fetched by any member", "[js8][held]") {
    HeldMessages held;
    auto         s = settings();
    s.held         = &held;
    s.groups       = {"@NET"};
    const std::int64_t t0 = 1'000'000;

    // A MSG to our group goes to our inbox and is ACKed; other groups: nothing.
    auto in = process(buffered("N0XYZ", "@NET MSG NET AT 0100Z"), s, {}, "");
    CHECK(in.store.kind == StoreAction::Kind::Inbox);
    CHECK(in.store.to == "@NET");
    REQUIRE(in.reply);
    CHECK(in.reply->text == "N0XYZ ACK");
    auto other = process(buffered("N0XYZ", "@OTHER MSG HI"), s, {}, "");
    CHECK(other.store.kind == StoreAction::Kind::None);
    CHECK_FALSE(other.reply);

    // "MSG TO:@NET" is held for the group.
    auto keep = process(buffered("N0XYZ", "K2XYZ MSG TO:@NET CHECK IN TONIGHT"), s, {}, "");
    CHECK(keep.store.kind == StoreAction::Kind::Held);
    CHECK(keep.store.to == "@NET");
    int g = held.add(keep.store.from, keep.store.to, keep.store.text, t0, keep.store.path);

    auto query = [&](const std::string &call, const std::string &text, std::int64_t when) {
        auto q    = buffered(call, text);
        q.when_ms = when;
        return build_reply(q, s, {}, "");
    };
    // Any member asking the group hears of it; a delivery is per member.
    auto yes = query("W1ABC", "@NET QUERY MSGS", t0 + 60'000);
    REQUIRE(yes);
    CHECK(yes->text == "W1ABC YES MSG ID " + std::to_string(g));
    auto get = query("W1ABC", "K2XYZ QUERY MSG " + std::to_string(g), t0 + 60'000);
    REQUIRE(get);
    CHECK(get->text == "W1ABC MSG CHECK IN TONIGHT FROM N0XYZ");
    CHECK(get->deliver_id == g);
    CHECK(get->deliver_group_call == "W1ABC");
    CHECK(held.mark_group_delivered(g, "W1ABC"));
    CHECK(query("W1ABC", "@NET QUERY MSGS", t0 + 60'000)->text == "W1ABC NO"); // to a group: desktop says NO
    CHECK(query("VE7ABC", "@NET QUERY MSGS", t0 + 60'000)->text == "VE7ABC YES MSG ID " + std::to_string(g));
    // Two days on, it's gone for the group.
    CHECK(query("VE7ABC", "@NET QUERY MSGS", t0 + HeldMessages::GROUP_WINDOW_MS + 1)->text == "VE7ABC NO");
    // Never delivered as a whole; RETRIEVE MSG isn't sent for it.
    CHECK_FALSE(held.get(g)->delivered);
    CHECK_FALSE(held.push_due({Heard("W1ABC", -5, t0)}, t0 + 1000));
}

TEST_CASE("a QUERY CALL gets desktop's '?' when it's left off", "[js8][relay]") {
    CHECK(query_call_question("K2XYZ QUERY CALL W1ABC") == "K2XYZ QUERY CALL W1ABC?");
    CHECK(query_call_question("K2XYZ QUERY CALL W1ABC  ") == "K2XYZ QUERY CALL W1ABC?");
    CHECK(query_call_question("@ALLCALL QUERY CALL VE7ABC/P") == "@ALLCALL QUERY CALL VE7ABC/P?");
    CHECK(query_call_question("K2XYZ QUERY CALL W1ABC?") == "K2XYZ QUERY CALL W1ABC?");
    CHECK(query_call_question("K2XYZ QUERY CALL") == "K2XYZ QUERY CALL");                 // nothing to ask about
    CHECK(query_call_question("K2XYZ QUERY CALL W1ABC PSE") == "K2XYZ QUERY CALL W1ABC PSE"); // more than a call
    CHECK(query_call_question("K2XYZ QUERY MSGS") == "K2XYZ QUERY MSGS");
    CHECK(query_call_question("K2XYZ MSG QUERY CALL W1ABC") == "K2XYZ MSG QUERY CALL W1ABC");
    CHECK(query_call_question("N0XYZ>K2XYZ QUERY CALL W1ABC") == "N0XYZ>K2XYZ QUERY CALL W1ABC"); // a relay: as typed

    // What goes on the air is desktop's menu text, '?' included.
    for (auto speed : {JS8_SPEED_NORMAL, JS8_SPEED_FAST}) {
        auto typed = plan_message("VE7NHW", "CN89", "n0xyz query call w1abc", speed);
        auto desk  = plan_message("VE7NHW", "CN89", "N0XYZ QUERY CALL W1ABC?", speed);
        REQUIRE(typed.ok());
        CHECK(typed.text == "N0XYZ QUERY CALL W1ABC?");
        CHECK(typed.preview == desk.preview);
        REQUIRE(typed.frames.size() == desk.frames.size());
        for (std::size_t i = 0; i < typed.frames.size(); i++) CHECK(typed.frames[i].frame == desk.frames[i].frame);
    }
}

TEST_CASE("QUERY CALL, RETRIEVE MSG and APRS gateway messages follow desktop", "[js8][held]") {
    auto               s   = settings();
    const std::int64_t now = 10'000'000;
    std::vector<Heard> heard{Heard("W1ABC/P", -12, now - 5 * 60'000), Heard("VE7ABC", 3, now - 20'000)};

    auto qc    = buffered("N0XYZ", "K2XYZ QUERY CALL W1ABC?");
    qc.when_ms = now;
    auto r     = build_reply(qc, s, heard, "");
    REQUIRE(r);
    CHECK(r->text == "N0XYZ YES -12 (5m)");
    CHECK(r->auto_only);
    REQUIRE(plan_message("K2XYZ", "FN42AB", r->text).ok());
    auto qv    = buffered("N0XYZ", "K2XYZ QUERY CALL VE7ABC?");
    qv.when_ms = now;
    CHECK(build_reply(qv, s, heard, "")->text == "N0XYZ YES +03 (15s)");
    auto qn    = buffered("N0XYZ", "K2XYZ QUERY CALL G4ABC?");
    qn.when_ms = now;
    CHECK_FALSE(build_reply(qn, s, heard, "")); // not heard: desktop stays quiet
    {
        AutoPolicy p;
        p.user_activity(now);
        CHECK(p.decide(*r, s, now) == AutoPolicy::Action::Ignore); // AUTO off: not even offered
    }

    // "RETRIEVE MSG 7" to us: suggest fetching it.
    auto rt = build_reply(incoming("W1ABC", "K2XYZ RETRIEVE MSG 7", -5), s, {}, "");
    REQUIRE(rt);
    CHECK(rt->text == "W1ABC QUERY MSG 7");
    CHECK(rt->kind == ReplyKind::Suggest);

    // An APRS gateway's "@APRSIS MSG TO:K2XYZ ..." lands in our inbox, unACKed.
    auto aprs = process(buffered("W1GW", "@APRSIS MSG TO:K2XYZ HELLO FROM APRS DE N0CALL"), s, {}, "");
    CHECK(aprs.store.kind == StoreAction::Kind::Inbox);
    CHECK(aprs.store.from == "APRS");
    CHECK(aprs.store.text == "HELLO FROM APRS DE N0CALL");
    CHECK_FALSE(aprs.reply);
    auto not_mine = process(buffered("W1GW", "@APRSIS MSG TO:W9ZZZ HELLO"), s, {}, "");
    CHECK(not_mine.store.kind == StoreAction::Kind::None);
    CHECK_FALSE(not_mine.reply);

    // An SMS gateway's receipt for our "{04}" message: not an Inbox message
    // (bug hunt S7, as seen on the air 2026-09-28), and no ACK sent.
    auto ack = process(buffered("NR4U", "@APRSIS MSG TO: K2XYZ ACK04} DE SMS"), s, {}, "");
    CHECK(ack.store.kind == StoreAction::Kind::AprsReceipt);
    CHECK(ack.store.path == "ACK");
    CHECK(ack.store.text == "04");
    CHECK(ack.store.from == "SMS");
    CHECK_FALSE(ack.reply);
    auto rej = process(buffered("NR4U", "@APRSIS MSG TO:K2XYZ REJ7} DE EMAIL-2"), s, {}, "");
    CHECK(rej.store.kind == StoreAction::Kind::AprsReceipt);
    CHECK(rej.store.path == "REJ");
    CHECK(rej.store.text == "7");
    CHECK(rej.store.from == "EMAIL-2");
    auto reply_ack = process(buffered("NR4U", "@APRSIS MSG TO:K2XYZ ACK12}AB"), s, {}, ""); // APRS 1.1 reply-ack
    CHECK(reply_ack.store.kind == StoreAction::Kind::AprsReceipt);
    CHECK(reply_ack.store.text == "12");
    // A message that merely starts with ACK is still a message.
    auto words = process(buffered("NR4U", "@APRSIS MSG TO:K2XYZ ACK THAT, SEE YOU AT 5 DE SMS"), s, {}, "");
    CHECK(words.store.kind == StoreAction::Kind::Inbox);
}

TEST_CASE("a long relay path comes back whole from the Inbox", "[js8][aprs]") {
    // Seven hops of long calls (desktop has no limit; ours was cut at 48,
    // so Reply sent to a chopped call: bug hunt S2).
    const std::string path = "VE7ABC/P>EA8/G4XYZ>W1ABC/M>VA7XYZ/P>KH6/N0XYZ>DL1XX/P>VE3KP/M";
    char ipath[] = "/tmp/js8_path_inbox_XXXXXX";
    int  fd      = mkstemp(ipath);
    REQUIRE(fd >= 0);
    std::string line = "1\t1000\tU\tVE3KP/M\tK2XYZ\t" + path + "\tHELLO VIA MANY\n";
    REQUIRE(write(fd, line.data(), line.size()) == (ssize_t)line.size());
    close(fd);
    js8_inbox_t    *inbox = js8_inbox_open(ipath);
    js8_inbox_msg_t m;
    REQUIRE(js8_inbox_get(inbox, 1, &m));
    CHECK(std::string(m.path) == path);
    CHECK(path.size() > 48);
    js8_inbox_close(inbox);
    unlink(ipath);
}

TEST_CASE("an APRS receipt through js8_process: reported, not kept", "[js8][aprs]") {
    char ipath[] = "/tmp/js8_receipt_inbox_XXXXXX";
    int  fd      = mkstemp(ipath);
    REQUIRE(fd >= 0);
    close(fd);
    unlink(ipath);
    js8_inbox_t        *inbox = js8_inbox_open(ipath);
    js8_auto_t         *a     = js8_auto_create();
    js8_auto_settings_t st{};
    st.autoreply = true;
    st.my_call   = "K2XYZ";
    st.my_grid   = "FN42";
    js8_rx_msg_t m{};
    std::strcpy(m.from, "NR4U");
    std::strcpy(m.to, "@APRSIS");
    std::strcpy(m.text, "NR4U: @APRSIS MSG TO: K2XYZ ACK04} DE SMS");
    m.checksum = 0; // gateways relay without a checksum
    js8_stored_t      kept;
    js8_auto_result_t r;
    js8_process(a, &m, &st, nullptr, 0, "", 1000, inbox, &kept, &r);
    CHECK(kept.kind == JS8_STORED_APRS_RECEIPT);
    CHECK(std::string(kept.text) == "04");
    CHECK(std::string(kept.from) == "SMS");
    CHECK_FALSE(kept.rejected);
    CHECK(r.action == JS8_AUTO_IGNORE);
    CHECK(js8_inbox_count(inbox) == 0);
    js8_auto_destroy(a);
    js8_inbox_close(inbox);
    unlink(ipath);
}

TEST_CASE("RETRIEVE MSG: when the station is heard, once per 8 hours", "[js8][held]") {
    HeldMessages       held;
    int                id  = held.add("N0XYZ", "W1ABC", "HELLO", 1000);
    const std::int64_t now = 50'000'000;
    CHECK_FALSE(held.push_due({Heard("W1ABC", -5, now - HeldMessages::PUSH_SEEN_MS - 1)}, now)); // heard too long ago
    auto due = held.push_due({Heard("W1ABC/P", -5, now - 60'000)}, now);
    REQUIRE(due);
    CHECK(due->first == id);
    CHECK(due->second == "W1ABC RETRIEVE MSG " + std::to_string(id));
    // Not told until it has gone out (bug hunt S4): a notice that couldn't
    // be queued is due again at the next look, not in 8 hours.
    CHECK(held.push_due({Heard("W1ABC", -5, now)}, now + 60'000));
    CHECK(held.notified(id, now));
    CHECK_FALSE(held.push_due({Heard("W1ABC", -5, now)}, now + 60'000)); // told already
    CHECK(held.push_due({Heard("W1ABC", -5, now + HeldMessages::PUSH_REPEAT_MS)}, now + HeldMessages::PUSH_REPEAT_MS));
    held.mark_delivered(id);
    CHECK_FALSE(held.push_due({Heard("W1ABC", -5, now + 2 * HeldMessages::PUSH_REPEAT_MS)},
                              now + 2 * HeldMessages::PUSH_REPEAT_MS));
}

TEST_CASE("inbox and held files from the last release still load", "[js8][held]") {
    char ipath[] = "/tmp/js8_inbox_v1_XXXXXX";
    int  fd      = mkstemp(ipath);
    REQUIRE(fd >= 0);
    const char *v1 = "# X6100 JS8 inbox: id, UTC ms, U(nread)/R(ead), from, message\n"
                     "3\t1000\tU\tN0XYZ\tMEET AT 1800Z\n";
    REQUIRE(write(fd, v1, strlen(v1)) == (ssize_t)strlen(v1));
    close(fd);
    Inbox box;
    REQUIRE(box.load(ipath));
    REQUIRE(box.get(3));
    CHECK(box.get(3)->path == "N0XYZ");
    CHECK(box.get(3)->text == "MEET AT 1800Z");
    int id = box.add("K2XYZ", "HI *DE* N0XYZ", 2000, "W1ABC", "K2XYZ>N0XYZ");
    CHECK(id == 4);
    REQUIRE(box.save(ipath));
    Inbox again;
    REQUIRE(again.load(ipath));
    CHECK(again.get(4)->path == "K2XYZ>N0XYZ");
    CHECK(again.get(4)->to == "W1ABC");
    CHECK(again.get(3)->text == "MEET AT 1800Z");
    unlink(ipath);

    char hpath[] = "/tmp/js8_held_v1_XXXXXX";
    fd           = mkstemp(hpath);
    REQUIRE(fd >= 0);
    const char *h1 = "# X6100 JS8 held messages: id, UTC ms, H(eld)/D(elivered), from, for, message\n"
                     "2\t1000\tH\tN0XYZ\tW1ABC\tSEE YOU\n";
    REQUIRE(write(fd, h1, strlen(h1)) == (ssize_t)strlen(h1));
    close(fd);
    HeldMessages held;
    REQUIRE(held.load(hpath));
    REQUIRE(held.get(2));
    CHECK(held.get(2)->path == "N0XYZ");
    CHECK(held.get(2)->text == "SEE YOU");
    int g = held.add("N0XYZ", "@NET", "NET TONIGHT", 2000, "K2XYZ>N0XYZ");
    held.mark_group_delivered(g, "W1ABC");
    held.mark_group_delivered(g, "VE7ABC");
    held.notified(2, 5000);
    REQUIRE(held.save(hpath));
    HeldMessages r;
    REQUIRE(r.load(hpath));
    CHECK(r.get(g)->got == std::vector<std::string>{"W1ABC", "VE7ABC"});
    CHECK(r.get(g)->path == "K2XYZ>N0XYZ");
    CHECK(r.get(2)->notified_ms == 5000);
    unlink(hpath);
}

TEST_CASE("a delivered message's signature, as desktop's inbox reads it", "[js8][held]") {
    auto a = delivered_signature("MEET AT 1800Z FROM N0XYZ NEXT MSG ID 4 +2");
    REQUIRE(a);
    CHECK(a->from == "N0XYZ");
    CHECK(a->next_id == 4);
    auto b = delivered_signature("I AM FROM TEXAS FROM N0XYZ");
    REQUIRE(b);
    CHECK(b->from == "N0XYZ");
    CHECK(b->next_id == 0);
    CHECK_FALSE(delivered_signature("HELLO THERE"));
    // Not signed: "HOME" isn't a callsign (bug hunt 3; desktop checks too).
    CHECK_FALSE(delivered_signature("I'M AWAY FROM HOME"));
    auto c = delivered_signature("SEE YOU FROM VE7/N0XYZ");
    REQUIRE(c);
    CHECK(c->from == "VE7/N0XYZ");
    // A long id is no next id (std::stoi threw on it; ultrareview R1).
    auto d = delivered_signature("HI FROM N0XYZ NEXT MSG ID 99999999999");
    REQUIRE(d);
    CHECK(d->from == "N0XYZ");
    CHECK(d->next_id == 0);
}

TEST_CASE("js8_process keeps messages and answers them", "[js8][held]") {
    char ipath[] = "/tmp/js8_proc_inbox_XXXXXX";
    char hpath[] = "/tmp/js8_proc_held_XXXXXX";
    close(mkstemp(ipath));
    close(mkstemp(hpath));
    js8_inbox_t *inbox = js8_inbox_open(ipath);
    js8_held_t  *held  = js8_held_open(hpath);
    js8_auto_t  *a     = js8_auto_create();

    js8_auto_settings_t st{};
    st.autoreply = true;
    st.relay     = true;
    st.my_call   = "K2XYZ";
    st.my_grid   = "FN42";
    st.groups    = "@net";
    st.held      = held;
    js8_auto_user_activity(a, 1000);

    auto msg = [](const char *from, const char *text) {
        js8_rx_msg_t m{};
        auto         plan = plan_message(from, "EN52", text);
        REQUIRE(plan.ok());
        auto mc = classify(plan.preview, "K2XYZ");
        snprintf(m.from, sizeof(m.from), "%s", from);
        snprintf(m.to, sizeof(m.to), "%s", mc.to.c_str());
        snprintf(m.text, sizeof(m.text), "%s", plan.preview.c_str());
        m.to_me    = mc.to_me;
        m.to_group = mc.to_group;
        m.checksum = 1;
        m.snr      = -6;
        return m;
    };

    js8_stored_t      kept;
    js8_auto_result_t r;
    auto              m1 = msg("K2XYZ", "K2XYZ MSG HI"); // our own: nothing
    js8_process(a, &m1, &st, nullptr, 0, "", 2000, inbox, &kept, &r);
    CHECK(kept.kind == JS8_STORED_NONE);

    auto m2 = msg("W1ABC", "K2XYZ>N0XYZ MSG HELLO");
    js8_process(a, &m2, &st, nullptr, 0, "", 2000, inbox, &kept, &r);
    CHECK(kept.kind == JS8_STORED_NONE);
    CHECK(r.action == JS8_AUTO_SEND);
    CHECK(r.kind == JS8_REPLY_RELAY);
    CHECK(std::string(r.text) == "N0XYZ>MSG HELLO *DE* W1ABC");

    auto m3 = msg("W1ABC", "K2XYZ MSG TO:@NET NET TONIGHT");
    js8_process(a, &m3, &st, nullptr, 0, "", 3000, inbox, &kept, &r);
    CHECK(kept.kind == JS8_STORED_HELD);
    CHECK(kept.id > 0);
    CHECK(std::string(kept.to) == "@NET");
    CHECK(std::string(r.text) == "W1ABC ACK");
    js8_held_msg_t hm;
    REQUIRE(js8_held_get(held, kept.id, &hm));
    CHECK(hm.group);

    auto m4 = msg("N0XYZ", "K2XYZ MSG MEET AT 1800Z");
    js8_process(a, &m4, &st, nullptr, 0, "", 4000, inbox, &kept, &r);
    CHECK(kept.kind == JS8_STORED_INBOX);
    CHECK_FALSE(kept.resend);
    CHECK(std::string(r.text) == "N0XYZ ACK");
    js8_process(a, &m4, &st, nullptr, 0, "", 5000, inbox, &kept, &r); // again: a resend, ACKed again
    CHECK(kept.resend);
    CHECK(r.action == JS8_AUTO_SEND);
    js8_inbox_msg_t im;
    REQUIRE(js8_inbox_get(inbox, kept.id, &im));
    CHECK(std::string(im.path) == "N0XYZ");

    js8_heard_t heard[] = {{"W1ABC", -3, 10'000}};
    char        text[64];
    CHECK_FALSE(js8_held_push_due(held, heard, 1, 20'000, text, sizeof(text), nullptr)); // only a group message
    char groups[64];
    js8_groups_normalise("net, @Net  aa", groups, sizeof(groups));
    CHECK(std::string(groups) == "@NET @AA");
    char from[16];
    int  next = -1;
    CHECK(js8_delivered_signature("HI FROM N0XYZ NEXT MSG ID 9", from, sizeof(from), &next));
    CHECK(next == 9);
    char shown[64];
    js8_path_display("K2XYZ>N0XYZ", shown, sizeof(shown));
    CHECK(std::string(shown) == "N0XYZ via K2XYZ");

    js8_auto_destroy(a);
    js8_inbox_close(inbox);
    js8_held_close(held);
    unlink(ipath);
    unlink(hpath);
}

TEST_CASE("stations heard through a relay are listed via it; aging is adjustable", "[js8][stations]") {
    StationList list;
    StationEvent direct;
    direct.from    = "W1ABC";
    direct.text    = "W1ABC: @HB HEARTBEAT FN42";
    direct.snr     = -9;
    direct.when_ms = 1000;
    list.add(direct, "K2XYZ");
    list.add_via("W1ABC", "N0XYZ", 1500, 0, 2000); // heard directly: kept
    list.add_via("VE7ABC", "N0XYZ", 1500, 0, 3000);
    auto l = list.sorted(4000);
    REQUIRE(l.size() == 2);
    CHECK(l[0].call == "VE7ABC");
    CHECK(l[0].via == "N0XYZ");
    CHECK(l[0].snr == -64);
    CHECK(l[1].via.empty());
    CHECK(l[1].snr == -9);
    direct.from = "VE7ABC"; // then heard directly: no longer "via"
    direct.when_ms = 5000;
    list.add(direct, "K2XYZ");
    CHECK(list.sorted(6000)[0].via.empty());
    // Aging: 0 keeps everything; the default drops after an hour.
    CHECK(list.sorted(1000 + StationList::EXPIRE_MS + 10).size() == 1);
    CHECK(list.sorted(1000 + StationList::EXPIRE_MS + 10, 0).size() == 2);
    CHECK(list.sorted(10'000, 5500).size() == 1);
}

TEST_CASE("stations: expired ones forgotten, one found by call, the most recent, heard before", "[js8][stations]") {
    StationList  list;
    StationEvent ev;
    auto         hear = [&](const char *call, std::int64_t when) {
        ev.from    = call;
        ev.text    = std::string(call) + ": @HB HEARTBEAT FN42";
        ev.when_ms = when;
        list.add(ev, "K2XYZ");
    };
    hear("W1ABC", 1000);
    hear("N0XYZ", 2000);
    hear("VE7ABC", 3000);
    hear("K2XYZ", 3500); // our own echo: not a station
    CHECK(list.size() == 3);

    // The most recent first, only as many as asked for.
    auto r = list.recent(4000, 2);
    REQUIRE(r.size() == 2);
    CHECK(r[0].call == "VE7ABC");
    CHECK(r[1].call == "N0XYZ");
    CHECK(list.recent(4000, 10, 2500).size() == 2); // W1ABC is past 2.5 s

    // Found by call while listed.
    REQUIRE(list.find("N0XYZ", 4000) != nullptr);
    CHECK(list.find("N0XYZ", 4000)->heard_ms == 2000);
    CHECK(list.find("W9XX", 4000) == nullptr);
    CHECK(list.find("W1ABC", 4000, 2500) == nullptr); // expired

    // Expired stations are erased (they were only hidden); 0 keeps all.
    list.expire(4000, 0);
    CHECK(list.size() == 3);
    list.expire(4000, 2500);
    CHECK(list.size() == 2);
    CHECK(list.find("W1ABC", 4000, 0) == nullptr);

    // Heard before stays true after expiry and the Clear button, not after
    // a reset (the list reused for another frequency).
    CHECK(list.heard_before("W1ABC"));
    CHECK_FALSE(list.heard_before("K2XYZ"));
    list.clear();
    CHECK(list.size() == 0);
    CHECK(list.heard_before("VE7ABC"));
    list.reset();
    CHECK_FALSE(list.heard_before("VE7ABC"));

    // Only through a relay: listed, but not heard directly.
    list.add_via("KK6ABC", "N0XYZ", 1500, 0, 5000);
    CHECK(list.find("KK6ABC", 5000) != nullptr);
    CHECK_FALSE(list.heard_before("KK6ABC"));
}

TEST_CASE("stations C API: the list forgets expired stations as it goes", "[js8][stations]") {
    js8_stations_t *s = js8_stations_create();
    js8_rx_msg_t    m{};
    std::strcpy(m.from, "W1ABC");
    std::strcpy(m.text, "W1ABC: @HB HEARTBEAT FN42");
    js8_stations_add(s, &m, "K2XYZ", 1000);
    std::strcpy(m.from, "N0XYZ");
    std::strcpy(m.text, "N0XYZ: @HB HEARTBEAT EN34");
    const std::int64_t later = 1000 + StationList::EXPIRE_MS + 70'000; // W1ABC expired, a minute on
    js8_stations_add(s, &m, "K2XYZ", later);
    js8_station_t st;
    CHECK_FALSE(js8_stations_find(s, "W1ABC", later, &st));
    REQUIRE(js8_stations_find(s, "N0XYZ", later, &st));
    CHECK(std::string(st.call) == "N0XYZ");
    js8_station_t list[4];
    CHECK(js8_stations_recent(s, later, list, 4) == 1);
    CHECK(js8_stations_heard_before(s, "W1ABC")); // not "new" if heard again
    js8_stations_clear(s);
    CHECK(js8_stations_heard_before(s, "N0XYZ"));
    js8_stations_reset(s);
    CHECK_FALSE(js8_stations_heard_before(s, "N0XYZ"));
    js8_stations_destroy(s);
}

TEST_CASE("stations: the Sort button's orders", "[js8][stations]") {
    auto st = [](const char *call, const char *grid, int snr, std::int64_t heard_ms, bool heard_me) {
        js8_station_t s{};
        std::strcpy(s.call, call);
        std::strcpy(s.grid, grid);
        s.snr      = (int16_t)snr;
        s.heard_ms = heard_ms;
        s.heard_me = heard_me;
        return s;
    };
    // As js8_stations_list() gives them: who heard us first, then newest.
    const std::vector<js8_station_t> list = {
        st("K1AAA", "FN42", -15, 5000, true),  // Boston, ~4000 km from CN89
        st("VE7BBB", "CN89", 2, 9000, false),  // next door
        st("JA1CCC", "PM95", -20, 7000, false), // Tokyo, ~7600 km
        st("W7DDD", "", 2, 8000, false),        // no grid
        st("VK2EEE", "QF56", -8, 6000, false),  // Sydney, ~12500 km
    };
    auto order = [&](js8_st_sort_t o, const char *my_grid) {
        auto v = list;
        js8_stations_sort(v.data(), (int)v.size(), o, my_grid);
        std::string calls;
        for (auto &s : v) calls += std::string(calls.empty() ? "" : " ") + s.call;
        return calls;
    };
    CHECK(order(JS8_ST_SORT_HEARD_ME, "CN89") == "K1AAA VE7BBB JA1CCC W7DDD VK2EEE");
    // Strongest first; VE7BBB and W7DDD tie at +02 and keep their places.
    CHECK(order(JS8_ST_SORT_SNR, "CN89") == "VE7BBB W7DDD VK2EEE K1AAA JA1CCC");
    CHECK(order(JS8_ST_SORT_TIME, "CN89") == "VE7BBB W7DDD JA1CCC VK2EEE K1AAA");
    // Farthest first, no grid last.
    CHECK(order(JS8_ST_SORT_DISTANCE, "CN89") == "VK2EEE JA1CCC K1AAA VE7BBB W7DDD");
    // Without our own grid there's no distance: the list as it came.
    CHECK(order(JS8_ST_SORT_DISTANCE, "") == "K1AAA VE7BBB JA1CCC W7DDD VK2EEE");
    js8_station_t one = list[0];
    js8_stations_sort(&one, 1, JS8_ST_SORT_SNR, "CN89");
    js8_stations_sort(nullptr, 0, JS8_ST_SORT_SNR, "CN89");
    CHECK(std::string(one.call) == "K1AAA");
}

TEST_CASE("macros: desktop JS8Call's, filled in as desktop does", "[js8][macros]") {
    MacroInput in;
    in.my_call   = "VE7NHW";
    in.my_grid   = "cn89kg";
    in.my_info   = "X6100 5W <MYGRID4>";
    in.my_status = "IDLE <MYIDLE> VERSION <MYVERSION>"; // desktop's default STATUS
    in.version   = "X6100 JS8 beta 5";
    in.idle_ms   = 5 * 60000 + 59000;
    auto v       = macro_values(in);
    CHECK(replace_macros("<MYCALL> <MYGRID4> <MYGRID12>", v, false) == "VE7NHW CN89 CN89KG");
    CHECK(replace_macros("RIG <MYINFO>", v, false) == "RIG X6100 5W CN89");
    CHECK(replace_macros("<MYSTATUS>", v, false) == "IDLE 5M VERSION X6100 JS8 BETA 5");
    CHECK(replace_macros("<MYCQ> / <MYHB> / <MYREPLY>", v, false) == "CQ CQ CQ CN89 / HB CN89 / HW CPY?");
    // Nothing selected: <CALL> and <SNR> stay as typed, or go (prune).
    CHECK(replace_macros("<CALL> UR <SNR> TNX", v, false) == "<CALL> UR <SNR> TNX");
    CHECK(replace_macros("<CALL> UR <SNR> TNX", v, true) == " UR  TNX");
    CHECK(replace_macros("<FOO> <> A<B", v, true) == " <> A<B"); // desktop's regex: <, one or more not >, >
    CHECK(macros_need_station("<CALL> TNX"));
    CHECK(macros_need_station("UR <SNR>"));
    CHECK(macros_need_station("DT <TDELTA>"));
    CHECK_FALSE(macros_need_station("TNX 73 GL <MYCALL>"));

    in.call      = "w1abc";
    in.snr       = -5;
    in.tdelta_ms = 150;
    v            = macro_values(in);
    CHECK(replace_macros("<CALL> UR <SNR> DT <TDELTA>", v, true) == "W1ABC UR -05 DT 150 MS");
    in.snr = 7;
    CHECK(replace_macros("<SNR>", macro_values(in), true) == "+07");
    in.snr = -31; // desktop leaves <SNR> out from -31 down
    CHECK(replace_macros("UR <SNR>", macro_values(in), true) == "UR ");

    CHECK(idle_text(0) == "0M");
    CHECK(idle_text(59999) == "0M");
    CHECK(idle_text(60000) == "1M");
    CHECK(idle_text(2 * 3600000 + 1) == "2H");
    CHECK(idle_text(3LL * 86400000) == "3D");

    // The C API, as the dialog uses it.
    js8_macro_values_t c{};
    c.my_call = "VE7NHW";
    c.my_grid = "CN89";
    c.call    = "K2XYZ";
    c.has_snr = true;
    c.snr     = -3;
    char out[64];
    js8_macros_expand("<CALL> UR <SNR> QTH <MYGRID4>", &c, true, out, sizeof(out));
    CHECK(std::string(out) == "K2XYZ UR -03 QTH CN89");
    js8_macros_expand("<MYCALL> <MYCALL> <MYCALL> <MYCALL>", &c, true, out, 12);
    CHECK(std::string(out) == "VE7NHW VE7N"); // cut to fit
    CHECK(js8_macros_need_station("<CALL> 73"));
    CHECK_FALSE(js8_macros_need_station(nullptr));
}

TEST_CASE("relay stations and command spans for the message list", "[js8][relay]") {
    auto msg = [](const char *from, const char *text, const char *my_call) {
        js8_rx_msg_t m{};
        auto         plan = plan_message(from, "EN52", text);
        REQUIRE(plan.ok());
        auto mc = classify(plan.preview, my_call);
        snprintf(m.from, sizeof(m.from), "%s", from);
        snprintf(m.text, sizeof(m.text), "%s", plan.preview.c_str());
        m.to_me    = mc.to_me;
        m.checksum = 1;
        return m;
    };
    char calls[4][JS8_RX_CALL_LEN], via[16];
    auto end = msg("K2XYZ", "W1ABC>HELLO *DE* N0XYZ *DE* VE7ABC", "W1ABC");
    REQUIRE(js8_relay_stations(&end, "W1ABC", calls, 4, via, sizeof(via)) == 2);
    CHECK(std::string(via) == "K2XYZ");
    CHECK(std::string(calls[0]) == "VE7ABC");
    CHECK(std::string(calls[1]) == "N0XYZ");
    auto on  = msg("N0XYZ", "K2XYZ>W1ABC HELLO", "K2XYZ"); // passed on, not ending here
    CHECK(js8_relay_stations(&on, "K2XYZ", calls, 4, via, sizeof(via)) == 0);
    auto ack = msg("K2XYZ", "W1ABC>ACK *DE* N0XYZ", "W1ABC");
    CHECK(js8_relay_stations(&ack, "W1ABC", calls, 4, via, sizeof(via)) == 0);

    struct Case {
        const char *text, *cmd;
    };
    for (auto c : std::vector<Case>{
             {"N0XYZ: K2XYZ SNR?", "SNR?"},
             {"N0XYZ: K2XYZ MSG HELLO THERE", "MSG"},
             {"N0XYZ: K2XYZ MSG TO: W1ABC HI", "MSG TO:"},
             {"N0XYZ: K2XYZ QUERY MSGS", "QUERY MSGS"},
             {"N0XYZ: K2XYZ ACK", "ACK"},
             {"N0XYZ: K2XYZ > W1ABC HI", ">"},
             {"N0XYZ: @ALLCALL CQ CQ CQ FN42", "CQ CQ CQ"},
             {"N0XYZ: K2XYZ HEARTBEAT SNR -08", "HEARTBEAT SNR"},
             {"N0XYZ: K2XYZ HELLO THERE", nullptr},
             {"HELLO THERE", nullptr},
         }) {
        INFO(c.text);
        unsigned s = 0, n = 0;
        bool     ok = js8_command_span(c.text, &s, &n);
        CHECK(ok == (c.cmd != nullptr));
        if (ok && c.cmd) CHECK(std::string(c.text).substr(s, n) == c.cmd);
    }
}

TEST_CASE("the operator callsign goes to the ADIF OPERATOR field", "[js8][log]") {
    char path[] = "/tmp/js8_op_adif_XXXXXX";
    int  fd     = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    unlink(path);
    js8_log_entry_t e{};
    snprintf(e.call, sizeof(e.call), "W1ABC");
    snprintf(e.my_call, sizeof(e.my_call), "VE7NHW");
    e.on_ms = e.off_ms = 1'790'000'000'000;
    e.freq_hz          = 14'079'500;
    char err[64];
    REQUIRE(js8_log_append(path, &e, err, sizeof(err))); // none set: the station call, as desktop
    snprintf(e.op_call, sizeof(e.op_call), "VA7XYZ");
    REQUIRE(js8_log_append(path, &e, err, sizeof(err)));
    std::ifstream     f(path);
    std::stringstream all;
    all << f.rdbuf();
    auto text = all.str();
    CHECK(text.find("<operator:6>VE7NHW") != std::string::npos);
    CHECK(text.find("<operator:6>VA7XYZ") != std::string::npos);
    CHECK(text.find("<station_callsign:6>VE7NHW") != std::string::npos);
    unlink(path);

    char out[16];
    CHECK(js8_operator_call_valid("va7xyz/p", out, sizeof(out)));
    CHECK(std::string(out) == "VA7XYZ/P");
    CHECK_FALSE(js8_operator_call_valid("HELLO", out, sizeof(out)));
    CHECK_FALSE(js8_operator_call_valid("VA7-XYZ", out, sizeof(out)));
    CHECK_FALSE(js8_operator_call_valid("", out, sizeof(out)));
}

// Frames from desktop JS8Call-improved's own Varicode::buildMessageFrames() (e3d7a3b),
// built with Qt, for VE7NHW / CN89: ours must match them bit for bit.
TEST_CASE("our frames are desktop JS8Call's, bit for bit", "[js8][desktop]") {
    struct Case {
        int         submode;
        const char *text, *frames;
    };
    for (auto c : std::vector<Case>{
             {0, "N0XYZ RR 73", "UeWdy-ytVbK0 1 xiJ+++++++++ 2"},
             {0, "N0XYZ MSG 73 GOOD DAY", "UeWdy-ytVaa0 1 xiUw5wWE4W07 2"},
             {0, "N0XYZ>2E0ABC HELLO", "UeWdy-ytVaK0 1 XCAdvnHpFFxh 0 xOf+++++++++ 2"},
             {0, "N0XYZ>W1ABC HELLO THERE", "UeWdy-ytVaK0 1 Yo9-SKSpp-wD 0 mE8C4T5cV+++ 2"},
             {0, "N0XYZ MSG TO:W1ABC SEE YOU AT 1800Z", "UeWdy-ytVae0 1 Yo9-SMaY7-rD 0 jIFYLAY7s6GV 2"},
             {0, "N0XYZ QUERY CALL W1ABC?", "UeWdy-ytVaq0 1 Yo9-SRB+7s++ 2"},
             {0, "N0XYZ YES MSG ID 3 +2", "UeWdy-ytVbi0 1 vTA7BRPHPd++ 2"},
             {0, "N0XYZ HEARTBEAT SNR -08 MSG ID 3", "UeWdy-ytVbqN 1 vTA7BR7+++++ 2"},
             {0, "N0XYZ SNR -12", "UeWdy-ytVbaJ 3"},
             {0, "N0XYZ MSG MEET AT THE PARK 1800Z", "UeWdy-ytVaa0 1 -X91YS0SZps1 0 xarAjU0ZaF++ 2"},
             {0, "@ALLCALL CQ CQ CQ CN89", "3u7hI9zNPrMu 3"},
             {0, "@APRSIS CMD :SMS      :@6045551234 TEST{01}", "UeWdy+GGPpW0 1 xGnsDz7ez7eV 0 +HQDLSWmLqn+ 0 y1OBDRC7Eav3 0 wLOENOdHY7++ 2"},
             {1, "N0XYZ RR 73", "UeWdy-ytVbK0 1 knF+++++++++ 6"},
             {1, "N0XYZ MSG 73 GOOD DAY", "UeWdy-ytVaa0 1 knxeNg0uI00V 6"},
             {1, "N0XYZ>2E0ABC HELLO", "UeWdy-ytVaK0 1 imAKySwtYuM9 4 jYd+++++++++ 6"},
             {1, "N0XYZ>W1ABC HELLO THERE", "UeWdy-ytVaK0 1 W5WUETRnSBV+ 4 -ZmHqMP+++++ 6"},
             {1, "N0XYZ MSG TO:W1ABC SEE YOU AT 1800Z", "UeWdy-ytVae0 1 W5WUETR01+nl 4 8EmN9gLQt+++ 4 UlwIF+++++++ 6"},
             {1, "N0XYZ QUERY CALL W1ABC?", "UeWdy-ytVaq0 1 W5WUETKjc4GX 6"},
             {1, "N0XYZ YES MSG ID 3 +2", "UeWdy-ytVbi0 1 bqeSjjb5cV++ 6"},
             {1, "N0XYZ HEARTBEAT SNR -08 MSG ID 3", "UeWdy-ytVbqN 1 bqeSjiV+++++ 6"},
             {1, "N0XYZ SNR -12", "UeWdy-ytVbaJ 3"},
             {1, "N0XYZ MSG MEET AT THE PARK 1800Z", "UeWdy-ytVaa0 1 w4a69m1oFFO7 4 kJKgru2EG+++ 6"},
             {1, "@ALLCALL CQ CQ CQ CN89", "3u7hI9zNPrMu 3"},
             {1, "@APRSIS CMD :SMS      :@6045551234 TEST{01}", "UeWdy+GGPpW0 1 j37OtqUZqUX+ 4 z5erLo31NJC0 4 i5cjc3dISYbF 4 i7BiJen3++++ 6"},
         }) {
        INFO(c.text << " at submode " << c.submode);
        std::string got;
        for (auto &[frame, bits] : vc::build_message_frames("VE7NHW", "CN89", "", c.text, false, false, c.submode))
            got += (got.empty() ? "" : " ") + frame + " " + std::to_string(bits);
        CHECK(got == c.frames);
    }
}

// Compound callsigns (a "/" and a number, as KG4UHM/6) go in their own frame,
// whose command and SNR packing desktop does as [1][type][6-bit number]
// (Varicode::packCmd); js8core kept only 2 bits of the number, so an SNR -03
// to KG4UHM/6 went out as -31 (seen on the air 2026-10-11). Frames from
// desktop's Varicode::buildMessageFrames() built with Qt (js8-android 1f8d390a).
// From a compound call the command rides in the same kind of frame: our old
// packing showed KG4UHM/6's heartbeat ACKs ("VE7NHW HEARTBEAT SNR -17") as
// "HW CPY?", "FB" and "RR", and the auto-reply answered a question never asked.
TEST_CASE("frames to and from compound and portable calls are desktop's, bit for bit", "[js8][desktop]") {
    struct Case {
        const char *mycall;
        int         submode;
        const char *text, *frames;
    };
    for (auto c : std::vector<Case>{
             {"VE7NHW", 0, "KG4UHM/6 SNR -03", "Bu7hI9zNOrMu 1 IXXfezaRO+cm 2"},
             {"VE7NHW", 0, "KG4UHM/6 SNR +05", "Bu7hI9zNOrMu 1 IXXfezaRO+dm 2"},
             {"VE7NHW", 0, "KG4UHM/6 SNR -24", "Bu7hI9zNOrMu 1 IXXfezaRO+a8 2"},
             {"VE7NHW", 0, "KG4UHM/6 HW CPY?", "Bu7hI9zNOrMu 1 IXXfezaRO+Le 2"},
             {"VE7NHW", 0, "KG4UHM/6 RR", "Bu7hI9zNOrMu 1 IXXfezaRO+Lu 2"},
             {"VE7NHW", 0, "KG4UHM/6 73", "Bu7hI9zNOrMu 1 IXXfezaRO+Mm 2"},
             {"VE7NHW", 0, "KG4UHM/6 ACK", "Bu7hI9zNOrMu 1 IXXfezaRO+L0 2"},
             {"VE7NHW", 0, "KG4UHM/6 HEARTBEAT SNR -08", "Bu7hI9zNOrMu 1 IXXfezaRO+k8 2"},
             {"VE7NHW", 0, "KG4UHM/6 SNR?", "Bu7hI9zNOrMu 1 IXXfezaRO+JG 2"},
             {"VE7NHW", 0, "KG4UHM/6 GRID?", "Bu7hI9zNOrMu 1 IXXfezaRO+Jm 2"},
             {"VE7NHW", 0, "KG4UHM/6 FB", "Bu7hI9zNOrMu 1 IXXfezaRO+LW 2"},
             {"VE7NHW", 0, "KG4UHM/6 QSL?", "Bu7hI9zNOrMu 1 IXXfezaRO+M0 2"},
             {"VE7NHW", 0, "KG4UHM/6 GOOD AFTERNOON JUST DOING SOME TESTING", "Bu7hI9zNOrMu 1 IXXfezaRO+N8 0 zqEFWNc6oxjV 0 +aBrSYV+++++ 2"},
             {"VE7NHW", 0, "W1ABC/P SNR -12", "UeWdy+APb1bJ 3"},
             {"KG4UHM/6", 0, "KJ7PRS RR", "AXXfezaROrMu 1 IYAexYQ7O+Lu 2"},
             {"KG4UHM/6", 0, "KJ7PRS HEARTBEAT SNR -08", "AXXfezaROrMu 1 IYAexYQ7O+k8 2"},
             {"KG4UHM/6", 0, "KJ7PRS SNR -03", "AXXfezaROrMu 1 IYAexYQ7O+cm 2"},
             {"KG4UHM/6", 0, "VE7NHW HW CPY?", "AXXfezaROrMu 1 Ju7hI9zNO+Le 2"},
             {"KG4UHM/6", 0, "VE7NHW FB", "AXXfezaROrMu 1 Ju7hI9zNO+LW 2"},
             {"KG4UHM/6", 0, "@ALLCALL CQ CQ CQ", "2XXfezaRP++u 3"},
             {"KG4UHM/6", 0, "KJ7PRS 73", "AXXfezaROrMu 1 IYAexYQ7O+Mm 2"},
             {"KG4UHM/6", 1, "KJ7PRS RR", "AXXfezaROrMu 1 IYAexYQ7O+Lu 2"},
             {"KG4UHM/6", 1, "KJ7PRS HEARTBEAT SNR -08", "AXXfezaROrMu 1 IYAexYQ7O+k8 2"},
             {"KG4UHM/6", 1, "KJ7PRS SNR -03", "AXXfezaROrMu 1 IYAexYQ7O+cm 2"},
             {"KG4UHM/6", 1, "VE7NHW HW CPY?", "AXXfezaROrMu 1 Ju7hI9zNO+Le 2"},
             {"KG4UHM/6", 1, "VE7NHW FB", "AXXfezaROrMu 1 Ju7hI9zNO+LW 2"},
             {"KG4UHM/6", 1, "@ALLCALL CQ CQ CQ", "2XXfezaRP++u 3"},
             {"KG4UHM/6", 1, "KJ7PRS 73", "AXXfezaROrMu 1 IYAexYQ7O+Mm 2"},
             {"VE7NHW/P", 0, "N0XYZ SNR -03", "UeWdy-ytVbcS 3"},
             {"VE7NHW/P", 0, "N0XYZ HW CPY?", "UeWdy-ytVbE0 3"},
             {"VE7NHW/P", 0, "N0XYZ RR", "UeWdy-ytVbM0 3"},
             {"VE7NHW/P", 0, "N0XYZ HEARTBEAT SNR -08", "UeWdy-ytVbsN 3"},
             {"VE7NHW/P", 0, "@ALLCALL CQ CQ CQ CN89", "3u7hIAGPnrMu 3"},
             {"VE7NHW", 1, "KG4UHM/6 SNR -03", "Bu7hI9zNOrMu 1 IXXfezaRO+cm 2"},
             {"VE7NHW", 1, "KG4UHM/6 SNR +05", "Bu7hI9zNOrMu 1 IXXfezaRO+dm 2"},
             {"VE7NHW", 1, "KG4UHM/6 SNR -24", "Bu7hI9zNOrMu 1 IXXfezaRO+a8 2"},
             {"VE7NHW", 1, "KG4UHM/6 HW CPY?", "Bu7hI9zNOrMu 1 IXXfezaRO+Le 2"},
             {"VE7NHW", 1, "KG4UHM/6 RR", "Bu7hI9zNOrMu 1 IXXfezaRO+Lu 2"},
             {"VE7NHW", 1, "KG4UHM/6 73", "Bu7hI9zNOrMu 1 IXXfezaRO+Mm 2"},
             {"VE7NHW", 1, "KG4UHM/6 ACK", "Bu7hI9zNOrMu 1 IXXfezaRO+L0 2"},
             {"VE7NHW", 1, "KG4UHM/6 HEARTBEAT SNR -08", "Bu7hI9zNOrMu 1 IXXfezaRO+k8 2"},
             {"VE7NHW", 1, "KG4UHM/6 SNR?", "Bu7hI9zNOrMu 1 IXXfezaRO+JG 2"},
             {"VE7NHW", 1, "KG4UHM/6 GRID?", "Bu7hI9zNOrMu 1 IXXfezaRO+Jm 2"},
             {"VE7NHW", 1, "KG4UHM/6 FB", "Bu7hI9zNOrMu 1 IXXfezaRO+LW 2"},
             {"VE7NHW", 1, "KG4UHM/6 QSL?", "Bu7hI9zNOrMu 1 IXXfezaRO+M0 2"},
             {"VE7NHW", 1, "KG4UHM/6 GOOD AFTERNOON JUST DOING SOME TESTING", "Bu7hI9zNOrMu 1 IXXfezaRO+N8 0 tGu-1UORBkr+ 4 -GlLo9++++++ 6"},
             {"VE7NHW", 1, "W1ABC/P SNR -12", "UeWdy+APb1bJ 3"},
             {"VE7NHW/P", 1, "N0XYZ SNR -03", "UeWdy-ytVbcS 3"},
             {"VE7NHW/P", 1, "N0XYZ HW CPY?", "UeWdy-ytVbE0 3"},
             {"VE7NHW/P", 1, "N0XYZ RR", "UeWdy-ytVbM0 3"},
             {"VE7NHW/P", 1, "N0XYZ HEARTBEAT SNR -08", "UeWdy-ytVbsN 3"},
             {"VE7NHW/P", 1, "@ALLCALL CQ CQ CQ CN89", "3u7hIAGPnrMu 3"},
         }) {
        INFO(c.text << " from " << c.mycall << " at submode " << c.submode);
        std::string got;
        for (auto &[frame, bits] : vc::build_message_frames(c.mycall, "CN89", "", c.text, false, false, c.submode))
            got += (got.empty() ? "" : " ") + frame + " " + std::to_string(bits);
        CHECK(got == c.frames);
    }
}

// The other way: desktop's compound frames (above) read back as desktop
// reads them (Varicode::unpackCompoundMessage).
TEST_CASE("desktop's frames to a compound call read back with their command and SNR", "[js8][desktop]") {
    struct Case {
        const char *frame, *cmd, *num, *call = "KG4UHM/6";
    };
    for (auto c : std::vector<Case>{
             // KG4UHM/6 acking our heartbeat: the old code showed "HW CPY?"
             {"Ju7hI9zNO+j0", " HEARTBEAT SNR", "-17", "VE7NHW"},
             {"IXXfezaRO+cm", " SNR", "-03"},
             {"IXXfezaRO+dm", " SNR", "+05"},
             {"IXXfezaRO+a8", " SNR", "-24"},
             {"IXXfezaRO+k8", " HEARTBEAT SNR", "-08"},
             {"IXXfezaRO+Le", " HW CPY?", ""},
             {"IXXfezaRO+Lu", " RR", ""},
             {"IXXfezaRO+Mm", " 73", ""},
             {"IXXfezaRO+L0", " ACK", ""},
             {"IXXfezaRO+JG", " SNR?", ""},
             {"IXXfezaRO+Jm", " GRID?", ""},
         }) {
        INFO(c.frame);
        std::uint8_t type = 0;
        auto parts = vc::unpack_compound_message(c.frame, &type, nullptr, nullptr);
        // As desktop's: callsign, "" (no grid), command, number.
        REQUIRE(parts.size() >= 3);
        CHECK(parts[0] == c.call);
        CHECK(parts[2] == c.cmd);
        if (*c.num) {
            REQUIRE(parts.size() >= 4);
            CHECK(parts[3] == c.num);
        }
    }
}

TEST_CASE("an APRS gateway's reply (no checksum, as desktop sends it) reaches the inbox", "[js8][held]") {
    // Desktop's AprsInboundRelay: "@APRSIS MSG to:<DEST> <MESSAGE> DE <SENDER>",
    // sent without a checksum; e.g. an SMS reply from the SMS gateway.
    auto plan = plan_message("VA7GW", "CN89", "@APRSIS MSG TO:K2XYZ @6045551234 2 WAY DE SMS");
    REQUIRE(plan.ok());
    std::string text = plan.preview;
    CHECK(verify_command_checksum(text) == Checksum::None); // not "bad"
    CHECK(text == "VA7GW: @APRSIS MSG TO: K2XYZ @6045551234 2 WAY DE SMS");
    auto in = incoming("VA7GW", "@APRSIS MSG TO:K2XYZ @6045551234 2 WAY DE SMS", -9);
    in.checksum_ok = false;
    auto p = process(in, settings(), {}, "");
    CHECK(p.store.kind == StoreAction::Kind::Inbox);
    CHECK(p.store.from == "APRS");
    CHECK(p.store.text == "@6045551234 2 WAY DE SMS");
    CHECK_FALSE(p.reply); // never ACKed on JS8
    // Other checksummed messages still need theirs.
    auto msg        = incoming("N0XYZ", "K2XYZ MSG HELLO", -5);
    msg.checksum_ok = false;
    CHECK(process(msg, settings(), {}, "").store.kind == StoreAction::Kind::None);
}

/* ---- Show Map: positions, views, callsign places (docs/MAP_PLAN.md) ------ */

#include "callsign_place.hpp"
#include "geo.hpp"
#include "js8_map.h"

#ifndef CTY_DAT_PATH
#define CTY_DAT_PATH "third-party/cty/cty.dat"
#endif

namespace geo = x6100::js8::geo;
#include <algorithm>
#include <cstdio>

TEST_CASE("map: locators to areas and centres, as the radio's qth.c", "[map]") {
    auto b = geo::grid_box("CN89");
    REQUIRE(b);
    CHECK(b->sw.lon == Catch::Approx(-124));
    CHECK(b->sw.lat == Catch::Approx(49));
    CHECK(b->ne.lon == Catch::Approx(-122));
    CHECK(b->ne.lat == Catch::Approx(50));
    auto c = geo::grid_center("cn89kg"); // any case; qth_str_to_pos: -123.125, 49.2708
    REQUIRE(c);
    CHECK(c->lon == Catch::Approx(-123.125));
    CHECK(c->lat == Catch::Approx(49.0 + 6 * 2.5 / 60 + 2.5 / 120));
    CHECK(geo::grid_box("CN89KG12AB"));
    for (const char *bad : {"", "HOME", "CN8", "SS00", "CN89ZZ", "CN89K", "C189"}) {
        INFO(bad);
        CHECK_FALSE(geo::grid_box(bad));
    }
    // Round trip through js8_latlon_to_grid: the centre is in the square.
    for (double lat = -80; lat <= 80; lat += 17.3)
        for (double lon = -179; lon <= 179; lon += 23.7) {
            char g[11];
            REQUIRE(js8_latlon_to_grid(lat, lon, 6, g, sizeof(g)));
            auto box = geo::grid_box(g);
            REQUIRE(box);
            CHECK(lat >= box->sw.lat - 1e-9); // points on an edge: rounding either way
            CHECK(lat < box->ne.lat + 1e-9);
            CHECK(lon >= box->sw.lon - 1e-9);
            CHECK(lon < box->ne.lon + 1e-9);
        }
}

TEST_CASE("map: distance, bearing, Mercator", "[map]") {
    CHECK(geo::distance_km({0, 0}, {0, 1}) == Catch::Approx(111.195).epsilon(0.001));
    CHECK(geo::bearing_deg({0, 0}, {1, 0}) == Catch::Approx(0).margin(1e-6));
    CHECK(geo::bearing_deg({0, 0}, {0, 1}) == Catch::Approx(90));
    CHECK(geo::mercator_y(0) == Catch::Approx(0).margin(1e-9));
    CHECK(geo::mercator_y(45) == Catch::Approx(50.4987).epsilon(1e-4));
    CHECK(geo::mercator_y(90) == Catch::Approx(180).epsilon(1e-6)); // clamped: the square world
    CHECK(geo::mercator_lat(geo::mercator_y(60)) == Catch::Approx(60));
    CHECK(geo::unwrap(170, -170) == Catch::Approx(-190));
}

TEST_CASE("map: great-circle paths curve and stay in one piece across the Pacific", "[map]") {
    auto home = *geo::grid_center("CN89");
    auto ja   = *geo::grid_center("PM95");
    auto path = geo::great_circle(home, ja, 64);
    REQUIRE(path.size() == 65);
    CHECK(path.front().lat == Catch::Approx(home.lat));
    CHECK(path.back().lat == Catch::Approx(ja.lat));
    CHECK(path.back().lon == Catch::Approx(ja.lon - 360)); // went west, over the Pacific
    double top = -90;
    for (size_t i = 1; i < path.size(); i++) {
        CHECK(std::fabs(path[i].lon - path[i - 1].lon) < 10); // no jump at the date line
        top = std::max(top, path[i].lat);
    }
    CHECK(top > home.lat + 5); // it bows north, as GridTracker's paths do
}

TEST_CASE("map: the fitted view shows every station, never too close in", "[map]") {
    auto home = *geo::grid_center("CN89");
    std::vector<geo::LatLon> na = {*geo::grid_center("FN42"), *geo::grid_center("EM12"), *geo::grid_center("CN85")};
    auto v = geo::fit(home, na, 771, 268);
    for (auto p : na) {
        double x, y;
        v.project(p, x, y);
        CHECK(x > 0.1 * 771);
        CHECK(x < 0.9 * 771);
        CHECK(y > 0.1 * 268);
        CHECK(y < 0.9 * 268);
    }
    // One station next door: at least ~40 degrees across.
    auto near = geo::fit(home, {*geo::grid_center("CN88")}, 771, 268);
    CHECK(771 / near.px_deg >= 40);
    // Home in North America and a station in Japan: centred over the
    // Pacific, not over Europe.
    auto pac = geo::fit(home, {*geo::grid_center("PM95")}, 771, 268);
    CHECK(std::fabs(geo::unwrap(pac.lon_c, -170) + 170) < 30);
}

TEST_CASE("map: portable calls as desktop JS8Call reads them", "[map]") {
    using x6100::js8::call_area;
    using x6100::js8::effective_prefix;
    CHECK(effective_prefix("VE7NHW") == "VE7NHW");
    CHECK(effective_prefix("w6/ve7nhw") == "W6");
    CHECK(effective_prefix("VE7NHW/W6") == "W6");
    CHECK(effective_prefix("VE7NHW/P") == "VE7NHW");
    CHECK(effective_prefix("VE7NHW/MM") == "VE7NHW");
    CHECK(effective_prefix("KG4UHM/6") == "KG4UHM");
    CHECK(call_area("VE7NHW") == 7);
    CHECK(call_area("KG4UHM/6") == 6);
    CHECK(call_area("W6/VE7NHW") == 6);
    CHECK(call_area("7K1ABC") == 1);
    CHECK(call_area("2E0ABC") == 0);
    CHECK(call_area("ABC") == -1);
}

TEST_CASE("map: callsigns placed by AD1C's country file and our regions", "[map]") {
    x6100::js8::CountryFile cty;
    REQUIRE(cty.load(CTY_DAT_PATH));
    CHECK(cty.entity_count() > 300);
    struct Case {
        const char *call, *country, *continent, *region;
    };
    for (auto [call, country, continent, region] : std::vector<Case>{
             {"VE7NHW", "Canada", "NA", "British Columbia"},
             {"va3xyz", "Canada", "NA", "Ontario"},
             {"VO1ABC", "Canada", "NA", "Newfoundland"},
             {"VO2ABC", "Canada", "NA", "Labrador"},
             {"VY1ABC", "Canada", "NA", "Yukon"},
             {"VE7NHW/P", "Canada", "NA", "British Columbia"},
             {"W6ABC", "United States", "NA", "US call area 6"},
             {"KG4UHM/6", "United States", "NA", "US call area 6"}, // desktop's KG4 fixup, then /6
             {"W6/VE7NHW", "United States", "NA", "US call area 6"},
             {"VE7NHW/W6", "United States", "NA", "US call area 6"},
             {"KG4AB", "Guantanamo Bay", "NA", ""},
             {"KL7QXZ", "Alaska", "NA", ""},
             {"KL7AB", "United States", "NA", "US call area 7"}, // a real call cty.dat lists (lives in the lower 48)
             {"KH6XX", "Hawaii", "OC", ""},
             {"JA1ABC", "Japan", "AS", "Kanto"},
             {"7K1ABC", "Japan", "AS", "Kanto"},
             {"VK2ABC", "Australia", "OC", "New South Wales"},
             {"G4XYZ", "England", "EU", ""},
             {"ZS6ABC", "South Africa", "AF", ""},
             {"LU1ABC", "Argentina", "SA", ""},
             {"4X1ABC", "Israel", "AS", ""},
         }) {
        INFO(call);
        auto pl = cty.find(call);
        REQUIRE(pl);
        CHECK(pl->country == country);
        CHECK(pl->continent == continent);
        CHECK(pl->region == region);
    }
    // Regions sit inside their region: VE7 in British Columbia.
    auto bc = cty.find("VE7NHW");
    CHECK(bc->pos.lat > 48);
    CHECK(bc->pos.lat < 60);
    CHECK(bc->pos.lon > -139);
    CHECK(bc->pos.lon < -114);
    CHECK_FALSE(cty.find(""));
    CHECK(cty.continent_near(*geo::grid_center("JO62")).value_or("") == "EU");
}

TEST_CASE("map: C API places stations and picks the view (Auto / Close-in / World)", "[map]") {
    REQUIRE(js8_map_load_countries(CTY_DAT_PATH));
    js8_map_place_t pl;
    REQUIRE(js8_map_place("VE6ABC", "", &pl));
    CHECK(pl.approx);
    CHECK(std::string(pl.where) == "Alberta");
    CHECK(std::string(pl.continent) == "NA");
    REQUIRE(js8_map_place("KK7RFI", "CN85", &pl)); // a grid wins
    CHECK_FALSE(pl.approx);
    CHECK(std::string(pl.where) == "CN85");
    CHECK(pl.lat == Catch::Approx(45.5));

    char mine[3];
    REQUIRE(js8_map_my_continent("VE7NHW", "CN89", mine));
    CHECK(std::string(mine) == "NA");
    REQUIRE(js8_map_my_continent("", "JO62", mine)); // no call: the grid's country
    CHECK(std::string(mine) == "EU");

    auto home = *geo::grid_center("CN89");
    auto pt   = [](const char *grid, const char *cont) {
        js8_map_point_t p{};
        auto c = *geo::grid_center(grid);
        p.lat  = c.lat;
        p.lon  = c.lon;
        std::snprintf(p.continent, sizeof(p.continent), "%s", cont);
        return p;
    };
    std::vector<js8_map_point_t> na = {pt("CN85", "NA"), pt("FN42", "NA"), pt("EM12", "NA")};
    js8_map_view_t               v;
    CHECK_FALSE(js8_map_choose_view(JS8_MAP_AUTO, home.lat, home.lon, "NA", na.data(), na.size(), 771, 268, &v));
    auto dx = na;
    dx.push_back(pt("PM95", "AS"));
    CHECK(js8_map_choose_view(JS8_MAP_AUTO, home.lat, home.lon, "NA", dx.data(), dx.size(), 771, 268, &v));
    float x, y;
    js8_map_project(&v, 35.5, 139, &x, &y); // Japan is on the world view
    CHECK(x > 0);
    CHECK(x < 771);
    // Close-in holds on North America; Japan is off the edge.
    CHECK_FALSE(js8_map_choose_view(JS8_MAP_CLOSE, home.lat, home.lon, "NA", dx.data(), dx.size(), 771, 268, &v));
    js8_map_project(&v, 35.5, 139, &x, &y);
    CHECK((x < 0 || x > 771));
    // World: the whole inhabited world, centred on our longitude.
    CHECK(js8_map_choose_view(JS8_MAP_WORLD, home.lat, home.lon, "NA", na.data(), na.size(), 771, 268, &v));
    CHECK(v.lon_c == Catch::Approx(home.lon));
    for (auto [lat, lon] : std::vector<std::pair<double, double>>{{70, 25}, {-54, -68}, {-45, 170}, {64, -150}}) {
        js8_map_project(&v, lat, lon, &x, &y); // North Cape, Tierra del Fuego, New Zealand, Alaska
        CHECK(y >= 0);
        CHECK(y <= 268);
    }
    // A station whose continent is unknown doesn't switch to the world.
    std::vector<js8_map_point_t> unk = {pt("FN42", "NA"), pt("EM12", "")};
    CHECK_FALSE(js8_map_choose_view(JS8_MAP_AUTO, home.lat, home.lon, "NA", unk.data(), unk.size(), 771, 268, &v));

    // Squares and paths on the screen.
    js8_map_choose_view(JS8_MAP_AUTO, home.lat, home.lon, "NA", na.data(), na.size(), 771, 268, &v);
    float x0, y0, x1, y1;
    REQUIRE(js8_map_grid_rect(&v, "CN89", &x0, &y0, &x1, &y1));
    CHECK(x0 < x1);
    CHECK(y0 < y1);
    float xs[33], ys[33], hx, hy, tx, ty;
    js8_map_path(&v, home.lat, home.lon, 42.5, -71, 32, xs, ys);
    js8_map_project(&v, home.lat, home.lon, &hx, &hy);
    js8_map_project(&v, 42.5, -71, &tx, &ty);
    CHECK(xs[0] == Catch::Approx(hx).margin(0.01));
    CHECK(ys[32] == Catch::Approx(ty).margin(0.01));
    CHECK(xs[32] == Catch::Approx(tx).margin(0.01));
}

#include "map_render.hpp"

#ifndef MAP_BIN_PATH
#define MAP_BIN_PATH "tools/map_data/js8_map.bin"
#endif

TEST_CASE("map: the base map is drawn right for any view", "[map]") {
    namespace mr = x6100::js8::map;
    mr::MapData data;
    REQUIRE(data.load(MAP_BIN_PATH));
    CHECK(data.layers().size() == 8);
    const mr::Style style;
    const int       W = 771, H = 268;
    std::vector<uint32_t> px((size_t)W * H);
    auto at = [&](const geo::View &v, double lat, double lon) {
        double x, y;
        v.project({lat, lon}, x, y);
        REQUIRE(x >= 0);
        REQUIRE(x < W);
        REQUIRE(y >= 0);
        REQUIRE(y < H);
        return px[(size_t)y * W + (size_t)x];
    };
    auto home = *geo::grid_center("CN89");

    // North America close-in: 50m detail; Kansas is land, the Pacific
    // and Lake Superior are water.
    auto na = geo::fit(home, {*geo::grid_center("FN42"), *geo::grid_center("EM12")}, W, H);
    mr::RenderStats st;
    mr::render(data, na, px.data(), W, style, &st);
    CHECK(st.detail == 1);
    CHECK(at(na, 38.5, -98.5) == style.land);
    CHECK(at(na, 42.5, -135.0) == style.ocean); // (points off the 10/20-degree field lines)
    CHECK(at(na, 47.7, -87.5) == style.ocean); // Lake Superior
    CHECK(at(na, 51.5, -106.0) == style.land); // Saskatchewan

    // The whole world: 110m; Australia is land; wider than 360 degrees, so
    // the world repeats at the sides: Germany shows twice, a turn apart.
    auto world = geo::whole_world(home.lon, W, H);
    CHECK(W / world.px_deg > 360);
    mr::render(data, world, px.data(), W, style, &st);
    CHECK(st.detail == 0);
    CHECK(at(world, -25.0, 134.0) == style.land); // Australia
    CHECK(at(world, 51.5, 11.0) == style.land);   // Germany
    {
        double y  = (world.merc_c - geo::mercator_y(51.5)) * world.px_deg + H / 2.0;
        double x1 = (11.0 - world.lon_c) * world.px_deg + W / 2.0, x2 = x1 - 360 * world.px_deg;
        REQUIRE(x2 >= 0);
        CHECK(px[(size_t)y * W + (size_t)x1] == style.land);
        CHECK(px[(size_t)y * W + (size_t)x2] == style.land); // the repeat on the left
    }

    // Across the date line: a view centred on 180 draws both sides of it
    // (Chukotka and Kamchatka are land either side).
    geo::View pac = geo::fit(*geo::grid_center("RP81"), {*geo::grid_center("AP65"), *geo::grid_center("QO93")}, W, H);
    mr::render(data, pac, px.data(), W, style, &st);
    CHECK(at(pac, 66.0, -172.0) == style.land); // Chukotka, west of the date line in longitude terms
    CHECK(at(pac, 56.0, 159.0) == style.land);  // Kamchatka

    // A bad file isn't taken.
    mr::MapData bad;
    CHECK_FALSE(bad.parse(std::vector<uint8_t>{'J', 'S', '8', 'M', 'A', 'P', '1', 0, 99, 0, 0, 0}));

    // The C API.
    js8_map_data_t *d = js8_map_data_load(MAP_BIN_PATH);
    REQUIRE(d);
    js8_map_view_t v{};
    js8_map_choose_view(JS8_MAP_WORLD, home.lat, home.lon, "NA", nullptr, 0, W, H, &v);
    CHECK(js8_map_render_base(d, &v, px.data(), W));
    js8_map_data_free(d);
    CHECK(js8_map_data_load("/nonexistent/js8_map.bin") == nullptr);
}

#include <sqlite3.h>

TEST_CASE("map: new grid and new DXCC from the QSO log", "[map]") {
    REQUIRE(js8_map_load_countries(CTY_DAT_PATH));
    char what[40];
    CHECK_FALSE(js8_map_load_worked("/nonexistent/qso_log.db")); // no log: nothing is new
    CHECK(js8_map_new_kind("G4XYZ", "IO91", what, sizeof(what)) == JS8_MAP_NEW_NONE);

    // A log as the radio writes it (qso_log.c's table, the columns we read).
    auto path = (std::filesystem::temp_directory_path() / "js8_map_worked_test.db").string();
    std::remove(path.c_str());
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
                         "CREATE TABLE qso_log(remote_callsign TEXT NOT NULL, remote_grid TEXT);"
                         "INSERT INTO qso_log VALUES('NR5D','EM26'),('WB8PLB',NULL),('JA1XYZ','PM95ab');",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    REQUIRE(js8_map_load_worked(path.c_str()));
    CHECK(js8_map_worked_grids() == 2);     // EM26, PM95
    CHECK(js8_map_worked_countries() == 2); // United States, Japan
    CHECK(js8_map_new_kind("NR5D", "EM26", what, sizeof(what)) == JS8_MAP_NEW_NONE);
    CHECK(js8_map_new_kind("K1ABC", "FN31", what, sizeof(what)) == JS8_MAP_NEW_GRID);
    CHECK(std::string(what) == "FN31");
    CHECK(js8_map_new_kind("JA2ABC", "pm85", what, sizeof(what)) == JS8_MAP_NEW_GRID); // Japan worked, PM85 not
    CHECK(std::string(what) == "PM85");
    CHECK(js8_map_new_kind("G4XYZ", "IO91", what, sizeof(what)) == JS8_MAP_NEW_DXCC); // country beats grid
    CHECK(std::string(what) == "England");
    CHECK(js8_map_new_kind("VE7ABC", "", what, sizeof(what)) == JS8_MAP_NEW_DXCC); // no grid: country only
    js8_map_worked_add("G4XYZ", "IO91");
    CHECK(js8_map_new_kind("G4XYZ", "IO91", what, sizeof(what)) == JS8_MAP_NEW_NONE);
    CHECK(js8_map_bearing_deg(0, 0, 10, 0) == Catch::Approx(0).margin(1e-6));
    std::remove(path.c_str());
}

// ---- Station history ------------------------------------------------------

TEST_CASE("history: what a message means", "[js8][history]") {
    using x6100::js8::history_note_rx;
    using x6100::js8::history_note_tx;
    auto a = history_note_rx("W1ABC", "W1ABC: K2XYZ HW CPY?", true, "K2XYZ");
    REQUIRE(a);
    CHECK(a->call == "W1ABC");
    CHECK(a->exchange);
    CHECK_FALSE(a->heartbeat);

    auto hb = history_note_rx("W1ABC", "W1ABC: K2XYZ HEARTBEAT SNR -12", true, "K2XYZ");
    REQUIRE(hb);
    CHECK(hb->exchange);
    CHECK(hb->heartbeat); // kept, but not a QSO
    REQUIRE(hb->reported_snr);
    CHECK(*hb->reported_snr == -12);

    auto snr = history_note_rx("W1ABC", "W1ABC: K2XYZ SNR -08", true, "K2XYZ");
    REQUIRE(snr);
    CHECK_FALSE(snr->heartbeat); // an answer to SNR?
    CHECK(snr->reported_snr == -8);

    // Their heartbeat to everyone: not an exchange, but the grid's theirs.
    auto theirs = history_note_rx("W1ABC", "W1ABC: @HB HEARTBEAT FN42", false, "K2XYZ");
    REQUIRE(theirs);
    CHECK_FALSE(theirs->exchange);
    CHECK(theirs->grid == "FN42");

    // Their INFO and STATUS, to anyone.
    auto info = history_note_rx("W1ABC", "W1ABC: N0XYZ INFO IC-705 5W EFHW", false, "K2XYZ");
    REQUIRE(info);
    CHECK_FALSE(info->exchange);
    CHECK(info->info_kind == 0);
    CHECK(info->info_text == "IC-705 5W EFHW");
    CHECK(info->info_to == "N0XYZ");
    auto status = history_note_rx("W1ABC/P", "W1ABC/P: K2XYZ STATUS AT CAMP", true, "K2XYZ");
    REQUIRE(status);
    CHECK(status->call == "W1ABC"); // one station, portable or not
    CHECK(status->as_sent == "W1ABC/P");
    CHECK(status->exchange);
    CHECK(status->info_kind == 1);
    CHECK(status->info_text == "AT CAMP");
    // A question isn't an answer.
    auto ask = history_note_rx("W1ABC", "W1ABC: N0XYZ INFO?", false, "K2XYZ");
    REQUIRE(ask);
    CHECK(ask->info_kind == -1);

    CHECK_FALSE(history_note_rx("K2XYZ", "K2XYZ: W1ABC HW CPY?", false, "K2XYZ")); // our own echo

    auto tx = history_note_tx("K2XYZ: W1ABC HEARTBEAT SNR -10", "K2XYZ");
    REQUIRE(tx);
    CHECK(tx->call == "W1ABC");
    CHECK(tx->exchange);
    CHECK(tx->heartbeat);
    CHECK(history_note_tx("K2XYZ: N0XYZ HW CPY?", "K2XYZ"));
    CHECK_FALSE(history_note_tx("K2XYZ: @HB HEARTBEAT FN42", "K2XYZ"));
    CHECK_FALSE(history_note_tx("K2XYZ: @ALLCALL CQ CQ FN42", "K2XYZ"));
    CHECK_FALSE(history_note_tx("K2XYZ: @APRSIS GRID FN42AB", "K2XYZ"));
}

TEST_CASE("history: QSOs, INFO and STATUS kept in the file, per band", "[js8][history]") {
    using namespace x6100::js8;
    TempDir            dir;
    const std::string  path = dir.path + "/js8_history.db";
    const std::int64_t t0   = 1'790'000'000'000;
    const std::string  me   = "K2XYZ";
    auto rx = [&](History &h, const char *from, const char *text, bool to_me, const char *band, std::int64_t ms,
                  int snr = -10) {
        auto n = history_note_rx(from, text, to_me, me);
        REQUIRE(n);
        h.received(*n, text, band, 14078000 + 1200, snr, 0, ms);
    };
    auto tx = [&](History &h, const char *text, const char *band, std::int64_t ms, bool automatic = false) {
        auto n = history_note_tx(text, me);
        REQUIRE(n);
        h.sent(*n, text, band, 14078000 + 1500, 0, automatic, ms);
    };
    {
        History h;
        REQUIRE(h.open(path));
        // A QSO on 20 m.
        rx(h, "W1ABC", "W1ABC: K2XYZ HW CPY?", true, "20m", t0);
        tx(h, "K2XYZ: W1ABC FB COPY -10 HERE", "20m", t0 + 15'000);
        rx(h, "W1ABC", "W1ABC: K2XYZ TNX 73", true, "20m", t0 + 30'000, -8);
        h.logged("W1ABC", "20m", t0 + 31'000);
        // Their INFO to someone else, later their newer one.
        rx(h, "W1ABC", "W1ABC: N0XYZ INFO IC-705 5W", false, "20m", t0 + 60'000);
        rx(h, "W1ABC", "W1ABC: N0XYZ INFO IC-7300 100W", false, "20m", t0 + 90'000);
        // 40 minutes on: a new QSO. Then on 40 m: another, on its own band.
        rx(h, "W1ABC", "W1ABC: K2XYZ ARE YOU STILL THERE?", true, "20m", t0 + 40 * 60'000);
        tx(h, "K2XYZ: W1ABC SNR -12", "40m", t0 + 41 * 60'000, true);
        // Two hours on: only heartbeat ACKs, both ways.
        tx(h, "K2XYZ: W1ABC HEARTBEAT SNR -14", "20m", t0 + 120 * 60'000, true);
        rx(h, "W1ABC", "W1ABC: K2XYZ HEARTBEAT SNR -15", true, "20m", t0 + 125 * 60'000);
        // A stranger's heartbeat and CQ: not kept.
        rx(h, "VE3KP", "VE3KP: @HB HEARTBEAT FN03", false, "20m", t0 + 130 * 60'000);
        h.heard("VE3KP", "20m", -5, "FN03", t0 + 130 * 60'000);
        // W1ABC's heartbeat later: when we heard them, their grid.
        rx(h, "W1ABC", "W1ABC: @HB HEARTBEAT FN42", false, "20m", t0 + 140 * 60'000, -3);

        auto c20 = h.contacts("20m");
        REQUIRE(c20.size() == 1);
        CHECK(c20[0].call == "W1ABC");
        CHECK(c20[0].grid == "FN42");
        CHECK(c20[0].first_ms == t0);
        CHECK(c20[0].last_ms == t0 + 125 * 60'000);
        CHECK(c20[0].heard_ms == t0 + 140 * 60'000);
        CHECK(c20[0].snr == -3);
        REQUIRE(c20[0].reported_snr);
        CHECK(*c20[0].reported_snr == -15);
        CHECK(h.contacts("40m").size() == 1);
        CHECK(h.contacts("80m").empty());
        CHECK_FALSE(h.known("VE3KP", "20m"));
        CHECK(h.known("W1ABC", "40m"));
        CHECK_FALSE(h.known("W1ABC", "80m"));

        auto qsos = h.qsos("W1ABC");
        REQUIRE(qsos.size() == 3); // the heartbeat-only one left out
        CHECK(qsos[0].band == "40m");
        CHECK(qsos[1].start_ms == t0 + 40 * 60'000);
        CHECK(qsos[2].start_ms == t0);
        CHECK(qsos[2].lines == 3);
        CHECK(qsos[2].logged);
        CHECK_FALSE(qsos[1].logged);
        CHECK(h.qsos("W1ABC", true).size() == 4);
        CHECK(h.had_qso("W1ABC"));
        tx(h, "K2XYZ: N0XYZ HEARTBEAT SNR -10", "20m", t0 + 150 * 60'000, true); // a heartbeat ACK only
        CHECK(h.known("N0XYZ", "20m"));
        CHECK_FALSE(h.had_qso("N0XYZ"));

        auto lines = h.lines(qsos[2].id);
        REQUIRE(lines.size() == 3);
        CHECK_FALSE(lines[0].tx);
        CHECK(lines[0].text == "W1ABC: K2XYZ HW CPY?");
        CHECK(lines[1].tx);
        CHECK(lines[1].text == "K2XYZ: W1ABC FB COPY -10 HERE");
        CHECK(lines[2].snr == -8);

        auto info = h.latest_info("W1ABC", 0);
        REQUIRE(info);
        CHECK(info->text == "IC-7300 100W");
        CHECK(info->to == "N0XYZ");
        CHECK_FALSE(h.latest_info("W1ABC", 1));

        auto st = h.take_stats();
        CHECK(st.rows > 0);
        CHECK(st.failed == 0);
    }
    // Kept for good: the next start reads it back, and a message within
    // 30 min of the last one still joins its QSO.
    History h;
    REQUIRE(h.open(path));
    CHECK(h.known("W1ABC", "20m"));
    CHECK(h.had_qso("W1ABC")); // read back from the file
    CHECK_FALSE(h.had_qso("N0XYZ"));
    rx(h, "W1ABC", "W1ABC: K2XYZ ONE MORE THING", true, "20m", t0 + 140 * 60'000);
    auto all = h.qsos("W1ABC", true);
    REQUIRE(all.size() == 4);
    CHECK(all[0].lines == 3);      // the heartbeat exchange, now with a real message
    CHECK(all[0].real_lines == 1);
    CHECK(h.qsos("W1ABC").size() == 4);
}

TEST_CASE("history C API, and a damaged file kept aside", "[js8][history]") {
    TempDir     dir;
    std::string path = dir.path + "/js8_history.db";
    {
        std::ofstream bad(path);
        bad << "this is not a database, an SD card error";
    }
    js8_history_t *h = js8_history_open(path.c_str());
    REQUIRE(h);
    bool aside = false;
    for (auto &e : std::filesystem::directory_iterator(dir.path))
        aside |= e.path().filename().string().rfind("js8_history.db.unreadable-", 0) == 0;
    CHECK(aside);

    js8_rx_msg_t m{};
    std::snprintf(m.from, sizeof(m.from), "W1ABC");
    std::snprintf(m.to, sizeof(m.to), "K2XYZ");
    std::snprintf(m.text, sizeof(m.text), "W1ABC: K2XYZ STATUS QRV ALL WEEK");
    m.to_me   = true;
    m.snr     = -11;
    m.freq_hz = 1200;
    js8_history_rx(h, &m, "K2XYZ", 7078000, nullptr, 1'790'000'000'000);
    m.partial = true; // the text so far of one still arriving: not kept
    std::snprintf(m.text, sizeof(m.text), "W1ABC: K2XYZ PARTIAL");
    js8_history_rx(h, &m, "K2XYZ", 7078000, nullptr, 1'790'000'010'000);
    js8_history_tx(h, "K2XYZ: W1ABC RR TNX", "K2XYZ", 7078000, 1500, 0, false, nullptr, nullptr, 1'790'000'020'000);

    // N0XYZ's heartbeat (grid EN34) was heard before we answered it.
    js8_stations_t *st = js8_stations_create();
    js8_rx_msg_t    hb{};
    std::snprintf(hb.from, sizeof(hb.from), "N0XYZ");
    std::snprintf(hb.to, sizeof(hb.to), "@HB");
    std::snprintf(hb.text, sizeof(hb.text), "N0XYZ: @HB HEARTBEAT EN34");
    hb.heartbeat = true;
    js8_stations_add(st, &hb, "K2XYZ", 1'790'000'030'000);
    js8_history_tx(h, "K2XYZ: N0XYZ HEARTBEAT SNR -09", "K2XYZ", 7078000, 800, 0, true, nullptr, st, 1'790'000'031'000);
    js8_stations_destroy(st);

    js8_hist_contact_t c[4];
    REQUIRE(js8_history_contacts(h, "40m", c, 4) == 2);
    CHECK(std::string(c[0].call) == "N0XYZ");
    CHECK(std::string(c[0].grid) == "EN34");
    c[0] = c[1];
    CHECK(std::string(c[0].call) == "W1ABC");
    CHECK(c[0].snr == -11);
    js8_hist_info_t info;
    REQUIRE(js8_history_info(h, "W1ABC", 1, &info));
    CHECK(std::string(info.text) == "QRV ALL WEEK");
    js8_hist_qso_t q[4];
    REQUIRE(js8_history_qsos(h, "W1ABC", q, 4) == 1);
    CHECK(q[0].lines == 2);
    js8_hist_line_t l[4];
    REQUIRE(js8_history_lines(h, q[0].id, l, 4) == 2);
    CHECK(l[1].tx);
    unsigned rows = 0, commits = 0, failed = 0;
    int64_t  busy = 0;
    CHECK(js8_history_stats(h, &rows, &commits, &failed, &busy));
    CHECK(rows == 3);
    CHECK(failed == 0);
    js8_history_close(h);
}

TEST_CASE("history: text without the calls joins an open QSO, never starts one", "[js8][history]") {
    using namespace x6100::js8;
    // Free text goes out as "K2XYZ: GOOD COPY": to nobody, not to "GOOD".
    auto free = history_note_tx("K2XYZ: GOOD COPY NAME IS BOB", "K2XYZ");
    REQUIRE(free);
    CHECK(free->loose);
    CHECK(free->call.empty());
    CHECK_FALSE(free->exchange);
    auto theirs = history_note_rx("W1ABC", "W1ABC: NAME IS ALICE", false, "K2XYZ");
    REQUIRE(theirs);
    CHECK(theirs->loose);
    CHECK_FALSE(history_note_rx("W1ABC", "W1ABC: @HB HEARTBEAT FN42", false, "K2XYZ")->loose);
    CHECK_FALSE(history_note_rx("W1ABC", "W1ABC: N0XYZ HW CPY?", false, "K2XYZ")->loose); // to someone else

    TempDir            dir;
    const std::int64_t t0 = 1'790'000'000'000;
    const double       f  = 14078000 + 1200; // W1ABC's offset
    History            h;
    REQUIRE(h.open(dir.path + "/js8_history.db"));
    auto rx = [&](const char *from, const char *text, std::int64_t ms) {
        auto n = history_note_rx(from, text, std::string(text).find("K2XYZ") != std::string::npos, "K2XYZ");
        REQUIRE(n);
        h.received(*n, text, "20m", f, -10, 0, ms);
    };
    auto tx_free = [&](const char *text, const char *partner, std::int64_t ms) {
        auto n = history_note_tx(text, "K2XYZ");
        REQUIRE(n);
        n->call    = partner;
        n->as_sent = partner;
        h.sent(*n, text, "20m", 14078000 + 1500, 0, false, ms);
    };
    // Nothing open: free text and call-less text go nowhere.
    tx_free("K2XYZ: GOOD COPY", "W1ABC", t0 - 60'000);
    h.received_callless("QTH OMAHA", "20m", f, 10, -12, 0, t0 - 60'000);

    rx("W1ABC", "W1ABC: K2XYZ HW CPY?", t0);
    rx("W1ABC", "W1ABC: NAME IS ALICE", t0 + 15'000);                  // their text, our call dropped
    h.received_callless("QTH OMAHA", "20m", f + 3, 10, -12, 0, t0 + 30'000); // sender missed, their offset
    h.received_callless("CQ CONTEST", "20m", f + 200, 10, -12, 0, t0 + 31'000); // someone else's offset
    tx_free("K2XYZ: GOOD COPY ALICE", "W1ABC", t0 + 45'000);            // ours, W1ABC selected
    tx_free("K2XYZ: HELLO", "N0XYZ", t0 + 50'000);                      // N0XYZ: no QSO open
    rx("W1ABC", "W1ABC: LATER", t0 + 50 * 60'000);                       // too late: the QSO ended

    CHECK_FALSE(h.known("GOOD", "20m"));
    CHECK(h.had_qso("W1ABC"));
    CHECK_FALSE(h.known("N0XYZ", "20m"));
    CHECK(h.contacts("20m").size() == 1);
    auto q = h.qsos("W1ABC");
    REQUIRE(q.size() == 1);
    auto l = h.lines(q[0].id);
    REQUIRE(l.size() == 4);
    CHECK(l[0].text == "W1ABC: K2XYZ HW CPY?");
    CHECK(l[1].text == "W1ABC: NAME IS ALICE");
    CHECK(l[2].text == "QTH OMAHA");
    CHECK_FALSE(l[2].tx);
    CHECK(l[3].text == "K2XYZ: GOOD COPY ALICE");
    CHECK(l[3].tx);
    CHECK(q[0].end_ms == t0 + 45'000);

    // Settings > Clear: gone, every band; the next exchange starts afresh.
    CHECK(h.station_count() == 1);
    h.clear();
    CHECK(h.station_count() == 0);
    CHECK(h.contacts("20m").empty());
    CHECK(h.qsos("W1ABC", true).empty());
    CHECK_FALSE(h.known("W1ABC", "20m"));
    CHECK_FALSE(h.had_qso("W1ABC"));
    rx("W1ABC", "W1ABC: K2XYZ ARE YOU THERE", t0 + 51 * 60'000);
    CHECK(h.had_qso("W1ABC"));
    REQUIRE(h.qsos("W1ABC").size() == 1);
    CHECK(h.lines(h.qsos("W1ABC")[0].id).size() == 1);
}

// ---- Decoder setup: FFTW wisdom (js8_wisdom.h) ----------------------------

namespace {

std::string wisdom_test_dir() {
    char t[] = "/tmp/js8_wisdom_XXXXXX";
    REQUIRE(mkdtemp(t));
    return t;
}

std::string read_whole(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Waits for the setup thread; false if it didn't end in time.
bool wait_setup(unsigned max_s = 300) {
    for (unsigned i = 0; i < max_s * 10; i++) {
        if (!js8_decoder_setup_running(nullptr)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

} // namespace

TEST_CASE("decoder setup: a second build with the wisdom loaded doesn't measure again", "[js8][wisdom]") {
    using namespace x6100::js8;
    forget_wisdom();
    double measured = build_decoder(SUBMODE_NORMAL);
    std::string text = export_wisdom();
    REQUIRE(text.rfind("(fftw-", 0) == 0);

    forget_wisdom();
    REQUIRE(import_wisdom(text));
    double loaded = build_decoder(SUBMODE_NORMAL);
    INFO("Normal's decoder: " << measured << " s measuring, " << loaded << " s with the wisdom");
    CHECK(loaded < measured / 2);
    forget_wisdom();
}

TEST_CASE("decoder setup: damaged or cut-short wisdom isn't loaded", "[js8][wisdom]") {
    using namespace x6100::js8;
    forget_wisdom();
    build_decoder(SUBMODE_ULTRA);
    std::string text = export_wisdom();
    REQUIRE(!text.empty());
    forget_wisdom();
    const std::string none = export_wisdom(); // FFTW's header only

    CHECK_FALSE(import_wisdom(""));
    CHECK_FALSE(import_wisdom("not wisdom at all\n"));
    CHECK_FALSE(import_wisdom(text.substr(0, text.size() / 2)));
    // Nothing half-loaded is left behind.
    CHECK(export_wisdom() == none);
}

TEST_CASE("decoder setup: no file, every speed measured and the file written; read back next time", "[js8][wisdom]") {
    using namespace x6100::js8;
    const std::string dir  = wisdom_test_dir();
    const std::string path = dir + "/js8_wisdom.dat";
    DecoderSetup::get().reset();

    unsigned done = 99;
    CHECK_FALSE(js8_decoder_setup_begin(path.c_str())); // missing: the setup starts
    CHECK(js8_decoder_setup_running(&done));
    CHECK(done < js8_decoder_setup_speeds());
    CHECK_FALSE(js8_decoder_setup_begin(path.c_str())); // a second open meanwhile: still running
    REQUIRE(wait_setup());
    js8_decoder_setup_running(&done);
    CHECK(done == js8_decoder_setup_speeds());
    CHECK(js8_decoder_setup_seconds() > 0);
    CHECK(js8_decoder_setup_begin(path.c_str())); // done: the decoder can start

    const std::string text = read_whole(path);
    CHECK(text == export_wisdom());
    CHECK_FALSE(std::filesystem::exists(path + ".tmp"));

    // Every speed is in it: none of them measures again.
    for (int speed : {SUBMODE_NORMAL, SUBMODE_FAST, SUBMODE_TURBO, SUBMODE_SLOW, SUBMODE_ULTRA}) {
        build_decoder(speed);
        INFO("speed bit " << speed);
        CHECK_FALSE(wisdom_has_new(export_wisdom(), text));
    }

    // Next power-on: loaded from the file, no setup.
    DecoderSetup::get().reset();
    CHECK(js8_decoder_setup_begin(path.c_str()));
    CHECK_FALSE(js8_decoder_setup_running(nullptr));
    CHECK_FALSE(wisdom_has_new(export_wisdom(), text));
    CHECK_FALSE(wisdom_has_new(text, export_wisdom()));
    // Closing with nothing new learnt leaves the file alone.
    std::filesystem::remove(path);
    js8_decoder_setup_save(path.c_str());
    CHECK_FALSE(std::filesystem::exists(path));

    DecoderSetup::get().reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("decoder setup: a damaged file is measured again and written over", "[js8][wisdom]") {
    using namespace x6100::js8;
    const std::string dir  = wisdom_test_dir();
    const std::string path = dir + "/js8_wisdom.dat";
    {
        std::ofstream out(path);
        out << "(fftw-3.3.10 fftwf_wisdom #x12345678\n  (garbage";
    }
    DecoderSetup::get().reset();
    CHECK_FALSE(js8_decoder_setup_begin(path.c_str()));
    REQUIRE(wait_setup());
    CHECK(js8_decoder_setup_begin(path.c_str()));
    const std::string text = read_whole(path);
    CHECK(text == export_wisdom());

    DecoderSetup::get().reset();
    CHECK(js8_decoder_setup_begin(path.c_str())); // good now
    DecoderSetup::get().reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("decoder setup: closing saves wisdom learnt since the file was read", "[js8][wisdom]") {
    using namespace x6100::js8;
    const std::string dir  = wisdom_test_dir();
    const std::string path = dir + "/js8_wisdom.dat";
    // A file with Normal's plans only (as an older build might leave).
    forget_wisdom();
    build_decoder(SUBMODE_NORMAL);
    const std::string normal_only = export_wisdom();
    REQUIRE(write_data_file(path, normal_only));

    DecoderSetup::get().reset();
    REQUIRE(js8_decoder_setup_begin(path.c_str()));
    js8_decoder_setup_save(path.c_str());
    CHECK(read_whole(path) == normal_only); // nothing new yet

    build_decoder(SUBMODE_TURBO); // measured: not in the file
    js8_decoder_setup_save(path.c_str());
    const std::string text = read_whole(path);
    CHECK(wisdom_has_new(text, normal_only));
    CHECK_FALSE(wisdom_has_new(export_wisdom(), text));

    DecoderSetup::get().reset();
    std::filesystem::remove_all(dir);
}
