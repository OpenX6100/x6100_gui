# js8core (vendored)

Source: https://github.com/JS8Call-improved/Android-port
Commit: `1f8d390a52bbcdda98316060c479eb1a8d5a584c` (2026-09-23)
License: GPLv3 (see `LICENSE`)

Copied from that commit:

| here              | upstream            |
|-------------------|---------------------|
| `include/`, `src/`| `core/include`, `core/src` (minus `placeholder.cpp`) |
| `commons.h`       | `commons.h`         |
| `vendor/Eigen`    | `vendor/Eigen`      |
| `vendor/CRCpp`    | `vendor/CRCpp`      |

`CMakeLists.txt` and this file are ours, and so are the licence files of the
two vendored libraries, which upstream doesn't carry: `vendor/CRCpp/LICENSE`
(the BSD-3-Clause notice from `CRC.h`'s header) and `vendor/Eigen/COPYING.*`
(from Eigen 3.4.0, the version vendored).

## Local patches

In randal007/x6100-js8, where they were made, each one is its own commit
(`git log -- third-party/js8core`). In OpenX6100/x6100_gui they came in
together, in the commit after the unchanged import, so `git diff` between
the two shows all of them.

1. **Missing standard includes.** `varicode.hpp` uses `std::function` and
   `jsc.cpp` uses `std::strncmp` without including `<functional>` /
   `<cstring>`. Android's libc++ pulls those in transitively; GCC's
   libstdc++ does not.
2. **`EngineConfig::spectrum_enabled`.** When false, the engine does not
   start its spectrum thread or run a 4096-point double FFT every 100 ms.
   The X6100 draws its own waterfall from liquid-dsp.
3. **`Js8Engine::request_realign()`.** Re-snaps the 60 s RX ring to the
   wall clock on the next captured buffer. The engine otherwise aligns only
   at construction or when the drift setting changes, so audio gaps or a
   sound-card/system clock mismatch would slowly walk the decode windows.
4. **Lazy per-submode decoders.** `legacy_decode()` used to construct all
   five `DecodeMode` instances on first call. Now each is built on first use,
   which cut peak RSS from ~73 MB to ~44 MB in a Normal-only test and the
   first-decode setup time by ~4x.

5. **Value-initialised decoder arrays.** `DecodeMode` reads some of its
   arrays before writing them. Upstream gets away with this because its
   instances live in static storage, which is always zeroed. Patch 4 moved
   them to the heap, where they started with whatever the memory held
   before. In one reproducible case (a GCC -O2/-O3 build, with the decoder
   built right after large buffers were freed) the sync search returned only
   garbage candidates and nothing decoded. Zeroing the storage before
   construction did not help, because GCC's lifetime dead-store elimination
   removes that `memset()`. The fix gives every array member a `{}`
   initialiser, so construction zeroes them wherever the object lives. The
   failure depends on exact heap history, so there is no reliable
   regression test. The investigation is in the commit message.

6. **Callsign validation matches desktop.** `is_valid_callsign()` was a
   simplified rewrite that accepted almost any short word ("JUST",
   "HELLO", "THE") as a callsign. Desktop JS8Call requires a letter-digit
   pair, or a real compound call or @group. One visible effect:
   `build_message_frames()` took the first word of free text for a
   recipient, so forced identification never happened and free text went
   out without the sender's callsign, where desktop prepends it. This is a
   straight port of desktop's `isValidCallsign()` and
   `isValidCompoundCallsign()`. The upstream varicode round-trip test
   still passes (31/31).

7. **SNR format matches desktop.** `format_snr()` printed "-8" and "+5"
   where desktop's `Varicode::formatSNR()` prints "-08" and "+05", and it
   didn't return empty outside -60..+60 as desktop does. Only affects how
   received reports are displayed.

8. **Ready decode windows are never dropped.** `enqueue_decode()` returned
   without queuing while a decode was running, but `isDecodeReady()` had
   already marked the window done. With several speeds on (Turbo re-checks
   every second), a Normal or Fast slot falling due during a decode was
   silently never decoded; a test that switches Fast on mid-run lost a
   Normal frame every time. Desktop JS8Call queues ready windows. Now at
   most one snapshot waits behind the running decode; a newer snapshot
   replaces it and inherits any speed's window only the older one had
   (the 60 s ring still holds that audio).

9. **Decode range and QSO offset are settable.** The engine searched a fixed
   200-2500 Hz with `nfqso` 1500 Hz. Desktop searches its waterfall filter's
   edges (0-5000 Hz without a filter) and passes its own offset as `nfqso`,
   which the decoder uses to try candidates near it first.
   `set_decode_range()` and `set_qso_offset()` feed the next decode.

10. **`events::Decoded::capture_drift_ms`.** The time drift the ring was
    aligned with when the decode's audio was captured
    (`DecodeState::drift_ms_at_capture`), so an app can work out the drift
    that puts the signal on time (`capture_drift_ms - 1000 * xdt`) even when
    the drift changed while the decode ran. The existing `drift_ms`
    (`compute_drift_estimate`, from desktop's auto-sync) isn't used for
    that: it takes the decode window's start minus the submode's start
    delay plus `xdt`, so it comes out one start delay short (500 ms for
    Normal and Slow, 200 Fast, 100 Turbo): signals 1.5 s late give -1000.

11. **A directed message's number only after SNR, as desktop.** The
    directed pattern took `\s*[+-]?\d{1,3}` after *any* command as its
    number, where desktop's `optional_num_pattern` is
    `(?<=SNR)\s?[-+]?(?:3[01]|[0-2]?[0-9])`. So "N0XYZ RR 73" went out as
    RR with a number (desktop shows "RR 31"), "MSG 73 GOOD DAY" lost its
    73, and a relay or `MSG TO:` / `QUERY CALL` to a call starting with a
    digit ("N0XYZ>2E0ABC ...") named "E0ABC". `pack_directed_message()` now
    takes desktop's number only after a command ending in SNR (std::regex
    has no lookbehind), and consumes only that. Also in
    `build_message_frames()`: desktop's `isCommandBuffered()` counts any
    command with a space (" RR", " YES", " HEARTBEAT SNR" ...) as buffered
    there, so the text after it is left-stripped ("RR 73" sends "73", not
    " 73"); ours only did that for truly buffered commands. Normal-speed
    data frames: `pack_data_message()` only used JSC, where desktop's
    `packDataMessage()` also tries its Huffman table and keeps whichever
    fits more characters (text with callsigns took an extra frame). And
    desktop sends `@APRSIS MSG` / `MSG TO:` without a checksum. With all
    of these, frames match desktop's `buildMessageFrames()` bit for bit at
    all four speeds for 50 typical messages (checked against its code
    built with Qt; `tests/test_js8.cpp` "[desktop]" keeps 24 of them).

12. **`Js8Engine::set_sync_stats()`.** `populate_decode_metadata()` always
    set `params.syncStats = false`, so the decoder never emitted its
    `events::SyncState` (each sync candidate with its strength, and each
    decode), which desktop draws on its waterfall when "Show decode
    attempts" is on (`mainwindow.cpp` sets `dec_data.params.syncStats`
    from that option). Now a switch, off by default.

13. **Decode thread name and merged-window log.** The decode thread is
    named `js8-decode` (`pthread_setname_np`, Linux and Android), so
    per-thread CPU tools tell it from the app's other threads. And patch
    8's merge (a ready window waiting behind a running decode) logs
    "decode window merged: the decoder was busy" at Info level: the app
    counts these to show when decoding falls behind.

14. **Withdrawn 2026-10-14.** It gave Ultra (I, desktop's "JS8 60")
    Turbo's early window (decode once the 79 symbols are in, retry every
    second), on a misreading of desktop. Desktop JS8Call-improved
    (`mainwindow.cpp`, `turboOrUltra`) decodes Turbo and Ultra once a slot
    at `samplesNeeded()`: the 79 symbols plus the start delay plus 0.5 s,
    which is what js8core does for I without the patch (3.128 s into
    Ultra's 4 s slot). punk-kaos spotted it on Android-port#108: the first
    try came at 2.528 s, before the signal (2.628 s) can be complete.
    Measured with generated signals: the early try decodes on-time
    signals, misses weak or late ones, which the retry then finds 0.4 s
    after desktop would. Back to upstream's code; the number is kept so
    the later patches keep theirs.

15. **Ultra on its own decode thread, and a thread-start hook.**
    `EngineConfig::ultra_own_thread`: the engine's decode queue and
    thread become two "lanes"; a ready window's Ultra (I) part is copied
    into its own snapshot for the second thread (`js8-decode-u`), the rest
    goes to the first (`js8-decode`) as before, each lane merging as patch
    8. Safe because each pass has its own copy of the audio, decoders are
    `thread_local` and FFTW plan creation is already under `fftw_mutex`.
    `DecodeFinished` carries the pass's `submodes`, so a host can tell the
    lanes' passes apart; the second lane's log lines start `[ultra] `.
    `EngineCallbacks::on_decode_thread_start(lane)` runs on each decode
    thread as it starts, for the host's CPU affinity and priority. Off by
    default: one thread, as before.

16. **Compound frames' commands and SNR, as desktop.** `pack_cmd()` /
    `unpack_cmd()` packed a compound-directed frame's command as
    `(cmd & 0x1F) << 2 | (num & 0x03)`; desktop's `Varicode::packCmd()`
    packs an SNR command as `[1][type][6-bit number]` (type 1 = HEARTBEAT
    SNR) and any other command as its 7-bit value. So every command to or
    from a compound call (KG4UHM/6, a prefix/call) differed from desktop's,
    and an SNR kept only 2 bits: VE7NHW's reply "KG4UHM/6 SNR -03" went out
    as -31 (seen on the air 2026-10-11). Now desktop's layout;
    `tests/test_js8.cpp` checks 24 messages to and from compound and
    portable calls against desktop's own `buildMessageFrames()` (built with
    Qt, js8-android 1f8d390a) bit for bit, and desktop's compound frames
    reading back with their command and SNR.

All sixteen are candidates to send upstream (10 with a fix to `compute_drift_estimate` instead). Patch 5 matters to upstream only
if they ever move decoders off static storage; patch 6 affects them
today.

A related cost of upstream's approach, for the record: five `static
thread_local` decoders reserve about 31 MB of address space in every thread
(A 7.3, B 4.8, C 2.9, E 14.5, I 2.0 MB). That is significant in a 32-bit
process and is part of why patch 4 exists.
