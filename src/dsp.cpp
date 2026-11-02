/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "dsp.h"

#include "cfg/cfg_api.h"

#include "common/resampler.h"

#include "helpers.h"
#include "util.h"
#include "common/vector.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <numeric>
#include <vector>

extern "C" {
    #include "audio.h"
    #include "meter.h"
    #include "radio.h"

    #include <math.h>
    #include <pthread.h>
    #include <stdlib.h>
}

#define DB_OFFSET -34.0f
#define OEM_PSD_DELAY 4
#define R8_PSD_DELAY 0

/* Chunks accumulated by the internal meter accumulator before the S-meter and
 * (see DSP_METER_NOISE_ENABLED) the noise floor / auto-levels are refreshed. */
#define DSP_METER_CHUNKS 2

/* 1 — compute the noise floor and auto min/max together with the S-meter;
 * 0 — compute only the S-meter (noise floor and auto-levels stay at their
 * startup/manual values). S-meter is computed in either case. */
#define DSP_METER_NOISE_ENABLED 1

/* Levels used for the TX spectrum/waterfall/scope, matching the existing
 * S-meter scale (S4 .. S9+20). */
#define DSP_TX_LEVEL_MIN S4
#define DSP_TX_LEVEL_MAX S9_20

#define DSP_ATT_DB -15.0f
#define DSP_PRE_DB 19.0f

// Forward declaration
class ChunkedSpgram;

static iirfilt_cccf dc_block;

static pthread_mutex_t spectrum_mux = PTHREAD_MUTEX_INITIALIZER;

static x6100_base_ver_t base_ver;
static bool fw_dc_blocker = false;  // BASE firmware performs a DC blocker on IQ

/* Firmware decimation exponent reported through flow_info (fft_dec). The BASE
 * firmware always decimates on the supported revisions; only the reported
 * factor is tracked here. */
static uint8_t spectrum_factor = 1;
static float   zoom_level_offset = 0.0f;

static float auto_min = S_MIN;
static float auto_max = S9_40;

/* One shared FFT per direction, always at DSP_MAX_NFFT. */
static ChunkedSpgram *spgram_rx;
static ChunkedSpgram *spgram_tx;
static float          psd_lin[DSP_MAX_NFFT];

/* Internal full-resolution accumulator for the S-meter / noise floor /
 * auto-levels. Independent of the frame subscribers: it is always filled and
 * never decimated. */
static float    meter_accum[DSP_MAX_NFFT];
static uint32_t meter_count = 0;

static cfloat buf_filtered[RADIO_SAMPLES * 2];

static uint32_t cur_freq;
static uint8_t  psd_delay;
static uint8_t  min_max_delay;
static bool     cur_tx;

static iirfilt_rrrf audio_dc_blocker;

static bool ready = false;

static int32_t filter_from = 0;
static int32_t filter_to   = 3000;
static x6100_mode_t cur_mode;
static float noise_level = S_MIN;

/* S-meter raw value ownership: DSP computes, corrects (att/pre) and clamps the
 * value. meter.c consumes it for display (and applies its own clamp for the
 * smoothed value), CAT reads it via dsp_get_s_meter_db(). */
static std::atomic<bool>  s_meter_pre{false};
static std::atomic<bool>  s_meter_att{false};
static std::atomic<float> s_meter_db_raw{S1};
/* 1KO125: user calibration offset (dB), cfg.ui.s_meter_offset */
static std::atomic<int32_t> s_meter_offset_db{0};

/* Audio subscriptions for audio from BASE */

#define MAX_RAW_SUBS        4
#define MAX_RESAMPLED_SUBS  8
#define MAX_AUDIO_SUBS      (MAX_RAW_SUBS + MAX_RESAMPLED_SUBS)

enum AudioSubKind {
    AUDIO_SUB_FREE = 0,
    AUDIO_SUB_RAW,
    AUDIO_SUB_FLOAT,
};

struct AudioSub {
    uint32_t          id        = 0;
    AudioSubKind      kind      = AUDIO_SUB_FREE;
    audio_raw_cb_t    raw_cb    = nullptr;
    audio_float_cb_t  float_cb  = nullptr;
    uint32_t          rate_hz   = 0;
    std::atomic<bool> active{false};
    bool              exclusive = false;
    Resampler        *resampler = nullptr;
};

/*
 * Flat list of subscriptions, guarded by subs_mutex. Free slots have
 * kind == AUDIO_SUB_FREE. IDs are monotonic and never reused, so a stale id
 * matches nothing and dsp_audio_set_active()/dsp_audio_unsubscribe() become
 * harmless no-ops for it.
 *
 * subs_mutex is held for the whole dsp_put_audio_samples() dispatch, so
 * callbacks must never call dsp_audio_subscribe_*(), dsp_audio_unsubscribe()
 * or dsp_audio_set_active() (non-recursive mutex -> deadlock).
 */
static AudioSub           subs[MAX_AUDIO_SUBS];
static std::mutex         subs_mutex;
static uint32_t           next_sub_id = 1;

/* PSD frame subscribers (spectrum / waterfall / scope). */

#define MAX_FRAME_SUBS  4

struct FrameSub {
    uint32_t          id               = 0;
    uint16_t          nfft             = 0;
    uint16_t          chunks_per_frame = 0;
    bool              allow_vary_freq  = false;
    dsp_frame_cb_t    cb               = nullptr;
    void             *ud               = nullptr;
    bool              used             = false;
    std::atomic<bool> active{false};

    /* Accumulator of linear power over DSP_MAX_NFFT in DC-shifted order, and
     * how many chunks were actually summed into it (used for normalisation). */
    float    accum[DSP_MAX_NFFT] = {};
    uint32_t accum_count         = 0;

    /* Cadence counter: +1 per active chunk, saturates at chunks_per_frame and
     * is cleared only when a frame is emitted. It is not reset by
     * reset_accumulators()/frame_sub_zero(), so a run of rejected (dirty)
     * chunks or a frequency change cannot stall delivery. */
    uint16_t chunks_elapsed = 0;

    /* Precomputed max-pool decimation ranges: output bin j takes the maximum of
     * the accumulator bins [bstart[j], bend[j]). */
    uint16_t          bstart[DSP_MAX_NFFT] = {};
    uint16_t          bend[DSP_MAX_NFFT]   = {};
};

/*
 * Flat list of frame subscribers, guarded by frame_subs_mutex. Frames are
 * dispatched in the DSP thread; the same reentrancy contract as the audio
 * subscriptions applies (callbacks must not call dsp_frame_*()).
 */
static FrameSub           frame_subs[MAX_FRAME_SUBS];
static std::mutex         frame_subs_mutex;
static uint32_t           next_frame_sub_id = 1;

static void update_noise_and_levels(float *psd_lin, uint16_t size, bool update_levels);
static void update_s_meter(float *lin);
static void dsp_resolve_levels(bool tx, float *out_min, float *out_max);
static void set_spectrum_factor(uint8_t factor);
static void on_zoom_change(Subject *subj, void *user_data);
static void update_filters(Subject *subj, void *user_data);
static void update_cur_mode(Subject *subj, void *user_data);
static void on_cur_freq_change(Subject *subj, void *user_data);
static void on_pre_att_change(Subject *subj, void *user_data);
static void on_s_meter_offset_change(Subject *subj, void *user_data);


class ChunkedSpgram {

    // Window cache keyed by window size. The vectors own the window samples, so
    // the cache is released cleanly when the static map is destroyed at exit.
    static inline std::map<int32_t, std::vector<cfloat>> w_cache;

    size_t   nfft_;
    size_t   chunk_size_  = 0;
    size_t   window_size_ = 0;
    size_t   buffer_size_ = 0;
    windowcf buffer_      = NULL;
    fftplan  fft_;
    cfloat  *buf_time_;
    cfloat  *buf_freq_;
    cfloat  *w_ = NULL;

    void setup_buffer() {
        if (buffer_) {
            windowcf_destroy(buffer_);
        }
        buffer_ = windowcf_create(buffer_size_);
    };

    void setup_window() { w_ = get_cached_window(window_size_); };

    cfloat *get_cached_window(size_t window_size) {
        auto search = w_cache.find((int32_t)window_size);
        if (search != w_cache.end()) {
            return search->second.data();
        }

        std::vector<cfloat> window(window_size);
        for (size_t i = 0; i < window_size; i++) {
            window[i] = liquid_kaiser(i, window_size, 10.0f);
            // window[i] = liquid_hann(i, window_size);
        }
        // scale by window magnitude
        float g = 0.0f;
        for (size_t i = 0; i < window_size; i++)
            g += std::norm(window[i]);
        g = 1.0f / sqrtf(g * nfft_ / window_size);

        // scale window and copy
        for (size_t i = 0; i < window_size; i++)
            window[i] *= g;

        auto it = w_cache.emplace((int32_t)window_size, std::move(window)).first;
        return it->second.data();
    };

  public:
    ChunkedSpgram(size_t nfft) : nfft_(nfft) {
        buf_time_ = (cfloat *)calloc(sizeof(cfloat), nfft);
        buf_freq_ = (cfloat *)calloc(sizeof(cfloat), nfft);
        fft_      = fft_create_plan(nfft, buf_time_, buf_freq_, LIQUID_FFT_FORWARD, 0);
    };

    ~ChunkedSpgram() {
        free(buf_time_);
        free(buf_freq_);

        if (buffer_) {
            windowcf_destroy(buffer_);
        }
        fft_destroy_plan(fft_);
    };

    void reset() {
        if (buffer_) {
            windowcf_reset(buffer_);
        }
    };

    /* Window the current chunk, run one FFT and write the linear power of the
     * current transform (unshifted FFT order) to mag[DSP_MAX_NFFT]. */
    void execute_block(cfloat *chunk, size_t n_samples, float *mag) {
        if (n_samples != chunk_size_) {
            chunk_size_  = n_samples;
            window_size_ = LV_MIN(nfft_, chunk_size_);
            buffer_size_ = nfft_ - nfft_ % window_size_;
            setup_window();
            setup_buffer();
        }

        for (size_t i = 0; i < window_size_; i++) {
            windowcf_push(buffer_, chunk[i] * w_[i]);
        }

        cfloat *rc;
        if (windowcf_read(buffer_, &rc) != LIQUID_OK) {
            return;
        }
        memcpy(buf_time_, rc, sizeof(cfloat) * buffer_size_);
        fft_execute(fft_);

        // TODO: vectorize this operation
        for (size_t i = 0; i < nfft_; i++) {
            mag[i] = LV_MAX(LIQUID_SPGRAM_PSD_MIN, std::norm(buf_freq_[i]));
        }
    };
};

/* * */

void dsp_init() {
    base_ver = x6100_control_get_base_ver();

    /* Only firmware with the decimating flow is supported. Older firmware is
     * logged and treated as if it decimated (degraded operation). */
    bool supported = (util_compare_version(base_ver, (x6100_base_ver_t){1, 1, 9, 0}) >= 0) || (base_ver.rev >= 8);
    if (!supported) {
        LV_LOG_ERROR("BASE firmware is too old, not fully supported");
    }
    if (base_ver.rev >= 8) {
        fw_dc_blocker = true;
    }

    spgram_rx = new ChunkedSpgram(DSP_MAX_NFFT);
    spgram_tx = new ChunkedSpgram(DSP_MAX_NFFT);

    dc_block = iirfilt_cccf_create_dc_blocker(0.005f);

    if (base_ver.rev < 8) {
        psd_delay = OEM_PSD_DELAY;
    } else {
        psd_delay = R8_PSD_DELAY;
    }

    audio_dc_blocker = iirfilt_rrrf_create_dc_blocker(2.0f * M_PI_2f32 * 50.0f / AUDIO_CAPTURE_RATE);

    cfg.mode.zoom()->subscribe_and_notify(on_zoom_change);

    cfg.band.if_shift()->subscribe(update_filters);
    cfg.filter.low()->subscribe(update_filters);
    cfg.filter.high()->subscribe_and_notify(update_filters);
    cfg.cur.mode()->subscribe_and_notify(update_filters);

    cfg.cur.mode()->subscribe_and_notify(update_cur_mode);

    cfg.cur.pre()->subscribe_and_notify(on_pre_att_change, &s_meter_pre);
    cfg.cur.att()->subscribe_and_notify(on_pre_att_change, &s_meter_att);
    cfg.ui.s_meter_offset()->subscribe_and_notify(on_s_meter_offset_change);

    cfg.cur.fg_freq()->subscribe(on_cur_freq_change);
    ready = true;
}

/* Precompute the max-pool decimation ranges for one subscriber. Output bin j
 * spans the input range [bstart[j], bend[j]) on the DC-shifted axis. */
static void build_decim_ranges(FrameSub &s) {
    const uint32_t n = DSP_MAX_NFFT;
    const uint32_t w = s.nfft;

    for (uint32_t j = 0; j < w; j++) {
        uint32_t start = (j * n) / w;
        uint32_t end   = ((j + 1) * n) / w;

        if (end <= start) {
            end = start + 1;
        }
        s.bstart[j] = (uint16_t)start;
        s.bend[j]   = (uint16_t)end;
    }
}

static void frame_sub_consume(FrameSub &s) {
    if (s.accum_count == 0) {
        return;
    }

    const float inv = 1.0f / (float)s.accum_count;

    for (uint16_t i = 0; i < DSP_MAX_NFFT; i++) {
        s.accum[i] *= inv;
    }
    s.accum_count = 0;
}

/* Drop the accumulated data only. The cadence counter is deliberately left
 * alone: delivery must not restart from zero after a rejected chunk or a
 * frequency / rx-tx / spectrum-factor change. */
static void frame_sub_zero(FrameSub &s) {
    memset(s.accum, 0, sizeof(s.accum));
    s.accum_count = 0;
}

/* Fold one transform into a full-resolution accumulator, reversing the FFT DC
 * shift so the accumulator is in DC-centered order. */
static void accumulate_shifted(float *accum) {
    const uint16_t half = DSP_MAX_NFFT / 2;

    for (uint16_t i = 0; i < half; i++) {
        accum[i] += psd_lin[i + half];
    }
    for (uint16_t i = half; i < DSP_MAX_NFFT; i++) {
        accum[i] += psd_lin[i - half];
    }
}

static void accumulate(FrameSub &s) {
    accumulate_shifted(s.accum);
    s.accum_count++;
}

static void meter_zero() {
    memset(meter_accum, 0, sizeof(meter_accum));
    meter_count = 0;
}

/* Zero the accumulated data owned by the DSP pipeline, which requires stable
 * freq. Only the data is dropped; the per-subscriber cadence counters are left
 * alone so a frequency / rx-tx / spectrum-factor change does not restart
 * delivery from the beginning of the interval. Acquires frame_subs_mutex, so
 * callers must not hold it; do not call it from deliver_frames()/meter_tick()
 * or any other path that already holds the mutex. */
static void reset_accumulators() {
    std::lock_guard<std::mutex> lock(frame_subs_mutex);

    for (size_t i = 0; i < MAX_FRAME_SUBS; i++) {
        frame_sub_zero(frame_subs[i]);
    }
}

/* S-meter / noise / auto-levels tick. Runs on every chunk from the internal
 * full-resolution accumulator, independent of the frame subscribers, so the
 * S-meter keeps moving even when every consumer is paused (e.g. FT8).
 * update_levels is false when no external frame subscriber needs auto levels. */
static void meter_tick(bool tx, bool update_levels) {
    accumulate_shifted(meter_accum);
    meter_count++;

    if (meter_count < DSP_METER_CHUNKS) {
        return;
    }

    const float inv = 1.0f / (float)meter_count;

    for (uint16_t i = 0; i < DSP_MAX_NFFT; i++) {
        meter_accum[i] *= inv;
    }
    meter_count = 0;

#if DSP_METER_NOISE_ENABLED
    if (tx) {
        min_max_delay = 2;
    } else {
        update_noise_and_levels(meter_accum, DSP_MAX_NFFT, update_levels);
    }
#else
    (void)tx;
    (void)update_levels;
#endif

    update_s_meter(meter_accum);

    /* Clear the mean so the next window starts fresh. */
    meter_zero();
}

/* Max-pool the accumulated mean linear power down to the subscriber nfft,
 * convert to dB and hand the frame to the callback. */
static void decimate_emit(FrameSub &s, bool tx, uint32_t base_freq, uint32_t width_hz, uint8_t fft_dec, float min,
                          float max) {
    /* Per-call frame buffer: valid only for the duration of the callback, since
     * delivery is serialized on the DSP thread and subscribers copy the bins
     * they need. Sized to this subscriber's nfft instead of the shared max. */
    float frame_out[s.nfft];

    for (uint16_t j = 0; j < s.nfft; j++) {
        uint16_t start = s.bstart[j];
        uint16_t end   = s.bend[j];
        float    v     = s.accum[start];

        for (uint16_t i = start + 1; i < end; i++) {
            float u = s.accum[i];
            if (u > v) {
                v = u;
            }
        }
        frame_out[j] = 10.0f * log10f(LV_MAX(v, 1e-20f)) + DB_OFFSET + zoom_level_offset;
    }

    const dsp_frame_t frame = {
        .psd_db    = frame_out,
        .size      = s.nfft,
        .tx        = tx,
        .base_freq = base_freq,
        .width_hz  = width_hz,
        .fft_dec   = fft_dec,
        .min       = min,
        .max       = max,
    };
    s.cb(&frame, s.ud);
}

void dsp_reset() {
    if (base_ver.rev < 8) {
        psd_delay = OEM_PSD_DELAY;
    } else {
        psd_delay = R8_PSD_DELAY;
    }

    iirfilt_cccf_reset(dc_block);
    spgram_rx->reset();
    spgram_tx->reset();

    reset_accumulators();
}

static void process_samples(cfloat *buf_samples, uint16_t size, bool tx) {
    // Swap I and Q
    for (size_t i = 0; i < size; i++) {
        buf_filtered[i] = {buf_samples[i].imag(), buf_samples[i].real()};
    }

    if (!fw_dc_blocker) {
        iirfilt_cccf_execute_block(dc_block, buf_filtered, size, buf_filtered);
    }

    ChunkedSpgram *sg = tx ? spgram_tx : spgram_rx;
    sg->execute_block(buf_filtered, size, psd_lin);
}

static void update_s_meter(float *lin) {
    int32_t from, to, center;
    int32_t bw = FULL_BW_HZ / spectrum_factor;

    center = DSP_MAX_NFFT / 2;
    from = center + filter_from * DSP_MAX_NFFT / bw;
    to = center + filter_to * DSP_MAX_NFFT / bw;
    from = LV_MAX(from, 0);
    to = LV_MIN(to, DSP_MAX_NFFT - 1);

    float sum = 0.0f;

    for (int32_t i = from; i <= to; i++) {
        sum += lin[i];
    }

    float sum_db = 10.0f * log10f(sum) + DB_OFFSET;

    if (s_meter_att.load()) {
        sum_db -= DSP_ATT_DB;
    }
    if (s_meter_pre.load()) {
        sum_db -= DSP_PRE_DB;
    }
    sum_db += (float)s_meter_offset_db.load();
    s_meter_db_raw.store(sum_db);

    // TODO: use subscription
    meter_update(sum_db, cfg.ui.spectrum_beta()->get() * 0.01f);
}

/* Noise floor and display levels from the full-resolution linear spectrum.
 * Called from the meter tick; auto_min/auto_max are only refreshed when a frame
 * subscriber needs them (update_levels), while the noise floor always updates. */
static void update_noise_and_levels(float *lin, uint16_t size, bool update_levels) {
    if (min_max_delay) {
        min_max_delay--;
        return;
    }

    int32_t bw_hz = FULL_BW_HZ / spectrum_factor;

    // 2.5 kHz
    uint32_t win_size_hz = 2500;

    int window_size = (size * win_size_hz) / bw_hz;

    // Skip borders
    size_t start = size * 0.04f;
    size_t stop = size * (1 - 0.04f);

    float running = 0.0f;
    for (size_t j = 0; j < window_size; j++)
        running += lin[start + j];
    float min = running;

    for (size_t i = 1; i < stop - start - window_size; i++) {
        running += lin[start + i + window_size - 1] - lin[start + i - 1];
        if (running < min) min = running;
    }
    min = LV_MAX(1e-12f, min);

    // Get Minimum Statistics offset for the noise level
    float offset;
    switch (bw_hz)
    {
    case FULL_BW_HZ:
        offset = 3.98f;
        break;
    case FULL_BW_HZ / 2:
        offset = 2.45f;
        break;
    case FULL_BW_HZ / 4:
        offset = 1.46f;
        break;
    default:
        offset = 0.82f;
        break;
    }

    // Convert to db
    min = 10.0f * log10f(min) + DB_OFFSET + offset;

    lpf(&noise_level, min, 0.8f, S_MIN);

    min = noise_level;
    // Use win size for min/max and bandwidth for noise level on S-meter
    float noise_bw_offset = 10.0f * log10f(((float)filter_to - filter_from) / win_size_hz);
    float noise_db = min + noise_bw_offset;
    if (s_meter_att.load()) {
        noise_db -= DSP_ATT_DB;
    }
    if (s_meter_pre.load()) {
        noise_db -= DSP_PRE_DB;
    }
    noise_db += (float)s_meter_offset_db.load();
    meter_set_noise(noise_db);

    if (!update_levels) {
        return;
    }

    min -= 10.0f*log10f(window_size) + 5.0f;
    /* 1KO125: the displayed bins carry zoom_level_offset (narrower bins have
     * less noise each, compensated in decimate_emit): the auto levels must
     * carry it too, or the noise floor climbs by 3 dB per zoom step. */
    min += zoom_level_offset;

    if (min < S_MIN) {
        min = S_MIN;
    } else if (min > S8) {
        min = S8;
    }
    auto_min = min;
    auto_max = auto_min + 48.0f;
}

/* Whether any external frame subscriber is active right now, i.e. whether the
 * auto levels must be refreshed. Must be called with frame_subs_mutex held. */
static bool any_external_sub_active() {
    for (size_t i = 0; i < MAX_FRAME_SUBS; i++) {
        if (frame_subs[i].used && frame_subs[i].active.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

static void deliver_frames(bool tx, uint32_t base_freq, uint8_t fft_dec, bool vary_freq) {
    std::lock_guard<std::mutex> lock(frame_subs_mutex);

    const uint32_t width_hz = FULL_BW_HZ / spectrum_factor;

    /* The meter tick runs first: it never depends on a subscriber, and its
     * fresh auto levels are what the frame levels below are resolved from. */
    meter_tick(tx, any_external_sub_active());

    for (size_t i = 0; i < MAX_FRAME_SUBS; i++) {
        FrameSub &s = frame_subs[i];

        if (!s.used || !s.active.load(std::memory_order_acquire)) {
            continue;
        }

        /* Frames collected while the base frequency was changing are dropped
         * unless the subscriber opts in (spectrum keeps them). */
        const bool dirty = (vary_freq || psd_delay) && !s.allow_vary_freq;

        if (dirty) {
            /* The dirty chunk is not used, but it still advances the cadence
             * below so delivery cannot stall while retuning. */
            frame_sub_zero(s);
        } else {
            accumulate(s);
        }

        /* Cadence advances on every active chunk and saturates at the
         * interval, so a saturated subscriber is "due" and emits on the first
         * chunk it can actually use. The increment happens before the early
         * continues to keep the due state. */
        if (s.chunks_elapsed < s.chunks_per_frame) {
            s.chunks_elapsed++;
        }

        if (s.chunks_elapsed < s.chunks_per_frame) {
            continue;
        }
        if (s.accum_count == 0) {
            /* Due, but no usable data yet: stay due and emit on the first
             * usable chunk. */
            continue;
        }

        /* accum now holds the full-resolution DC-shifted mean. */
        frame_sub_consume(s);

        if (s.cb) {
            float level_min, level_max;
            dsp_resolve_levels(tx, &level_min, &level_max);
            decimate_emit(s, tx, base_freq, width_hz, fft_dec, level_min, level_max);
        }

        frame_sub_zero(s);
        s.chunks_elapsed = 0;
    }
}

void dsp_samples(cfloat *buf_samples, uint16_t size, bool tx, uint32_t base_freq, bool vary_freq, uint8_t fft_dec) {
    if (!ready) {
        return;
    }

    if (base_freq != 0) {
        if (cur_freq != base_freq) {
            cur_freq = base_freq;

            pthread_mutex_lock(&spectrum_mux);
            spgram_rx->reset();
            spgram_tx->reset();
            pthread_mutex_unlock(&spectrum_mux);

            reset_accumulators();
        }
    }

    if (fft_dec && (fft_dec != spectrum_factor)) {
        set_spectrum_factor(fft_dec);
    }

    if (psd_delay) {
        psd_delay--;
    }

    /* Subscriber accumulators do not survive a direction change; the meter keeps
     * its own window and the cadence counters are preserved. */
    if (cur_tx != tx) {
        cur_tx = tx;

        reset_accumulators();
    }

    pthread_mutex_lock(&spectrum_mux);
    process_samples(buf_samples, size, tx);
    pthread_mutex_unlock(&spectrum_mux);

    deliver_frames(tx, base_freq, fft_dec, vary_freq);
}

static void set_spectrum_factor(uint8_t factor) {
    if (factor == spectrum_factor) {
        return;
    }

    pthread_mutex_lock(&spectrum_mux);
    spectrum_factor = factor;
    spgram_rx->reset();
    spgram_tx->reset();
    pthread_mutex_unlock(&spectrum_mux);

    reset_accumulators();
}

static void on_zoom_change(Subject *subj, void *user_data) {
    int32_t new_zoom = cfg.mode.zoom()->get();

    zoom_level_offset = log2f(new_zoom) * 3.0f;

    if (base_ver.rev < 8) {
        // OEM BASE >= 1.1.9 decimates in firmware but does not report fft_dec
        // back via flow_info, so the feedback path in dsp_samples() never fires
        // and spectrum_factor stays at 1. Sync it locally instead.
        set_spectrum_factor(new_zoom);
    }
}

static void update_filters(Subject *subj, void *user_data) {
    auto low = cfg.filter.low()->get();
    auto high = cfg.filter.high()->get();
    auto if_shift = cfg.band.if_shift()->get();
    auto mode = cfg.cur.mode()->get();
    switch (mode) {
        case x6100_mode_lsb:
        case x6100_mode_lsb_dig:
        case x6100_mode_cwr:
            filter_from = -high + if_shift;
            filter_to = -low + if_shift;
            break;

        case x6100_mode_am:
        case x6100_mode_nfm:
            filter_from = -high + if_shift;
            filter_to = high + if_shift;
            break;
        default:
            filter_from = low + if_shift;
            filter_to = high + if_shift;
            break;
    }
}

static void update_cur_mode(Subject *subj, void *user_data) {
    cur_mode = (x6100_mode_t)cfg.cur.mode()->get();
}

static void on_pre_att_change(Subject *subj, void *user_data) {
    std::atomic<bool> *flag = static_cast<std::atomic<bool> *>(user_data);
    flag->store(static_cast<SubjectT<int32_t> *>(subj)->get() != 0);
}

static void on_s_meter_offset_change(Subject *subj, void *user_data) {
    (void)user_data;
    s_meter_offset_db.store(static_cast<SubjectT<int32_t> *>(subj)->get());
}

static void on_cur_freq_change(Subject *subj, void *user_data) {
    int32_t new_freq = static_cast<SubjectT<int32_t> *>(subj)->get();
    if (base_ver.rev < 8) {
        psd_delay = OEM_PSD_DELAY;
        cur_freq = new_freq;
    } else {
        psd_delay = R8_PSD_DELAY;
    }
}

float dsp_get_s_meter_db() {
    return s_meter_db_raw.load();
}

static uint32_t alloc_sub_id() {
    if (next_sub_id == AUDIO_SUB_INVALID) {
        next_sub_id = 1;
    }
    return next_sub_id++;
}

uint32_t dsp_audio_subscribe_raw(audio_raw_cb_t cb, bool exclusive) {
    if (!cb) {
        return AUDIO_SUB_INVALID;
    }

    std::lock_guard<std::mutex> lock(subs_mutex);

    for (size_t i = 0; i < MAX_AUDIO_SUBS; i++) {
        if (subs[i].kind != AUDIO_SUB_FREE) {
            continue;
        }
        subs[i].id        = alloc_sub_id();
        subs[i].kind      = AUDIO_SUB_RAW;
        subs[i].raw_cb    = cb;
        subs[i].exclusive = exclusive;
        subs[i].active.store(false, std::memory_order_relaxed);
        return subs[i].id;
    }

    return AUDIO_SUB_INVALID;
}

uint32_t dsp_audio_subscribe_float(audio_float_cb_t cb, uint32_t target_rate_hz) {
    if (!cb) {
        return AUDIO_SUB_INVALID;
    }
    Resampler *resampler = NULL;
    if (target_rate_hz < AUDIO_CAPTURE_RATE) {
        uint8_t N = AUDIO_CAPTURE_RATE / target_rate_hz;
        if (target_rate_hz * N != AUDIO_CAPTURE_RATE) {
            LV_LOG_WARN("Requested sample rate is not supported: %u\n", target_rate_hz);
            return AUDIO_SUB_INVALID;
        }
        resampler = new Resampler(N);
    }

    std::lock_guard<std::mutex> lock(subs_mutex);

    for (size_t i = 0; i < MAX_AUDIO_SUBS; i++) {
        if (subs[i].kind != AUDIO_SUB_FREE) {
            continue;
        }
        subs[i].id        = alloc_sub_id();
        subs[i].kind      = AUDIO_SUB_FLOAT;
        subs[i].float_cb  = cb;
        subs[i].rate_hz   = target_rate_hz;
        subs[i].resampler = resampler;
        subs[i].active.store(false, std::memory_order_relaxed);
        return subs[i].id;
    }
    if (resampler) {
        delete resampler;
    }
    return AUDIO_SUB_INVALID;
}

void dsp_audio_set_active(uint32_t id, bool active) {
    if (id == AUDIO_SUB_INVALID) {
        return;
    }

    std::lock_guard<std::mutex> lock(subs_mutex);

    for (size_t i = 0; i < MAX_AUDIO_SUBS; i++) {
        if (subs[i].kind != AUDIO_SUB_FREE && subs[i].id == id) {
            subs[i].active.store(active, std::memory_order_release);
            return;
        }
    }
}

void dsp_audio_unsubscribe(uint32_t id) {
    if (id == AUDIO_SUB_INVALID) {
        return;
    }

    std::lock_guard<std::mutex> lock(subs_mutex);

    for (size_t i = 0; i < MAX_AUDIO_SUBS; i++) {
        if (subs[i].kind == AUDIO_SUB_FREE || subs[i].id != id) {
            continue;
        }

        subs[i].active.store(false, std::memory_order_relaxed);

        if (subs[i].kind == AUDIO_SUB_FLOAT) {
            if (subs[i].resampler) {
                delete subs[i].resampler;
                subs[i].resampler = nullptr;
            }
            subs[i].float_cb  = nullptr;
        } else {
            subs[i].raw_cb = nullptr;
        }

        subs[i].kind = AUDIO_SUB_FREE;
        subs[i].id   = 0;
        return;
    }
}

uint32_t dsp_frame_subscribe(const dsp_frame_cfg_t *cfg, dsp_frame_cb_t cb, void *user_data) {
    if (!cfg || !cb) {
        LV_LOG_WARN("Empty cfg or cb");
        return DSP_FRAME_SUB_INVALID;
    }
    if (cfg->nfft == 0 || cfg->chunks_per_frame == 0) {
        LV_LOG_WARN("Wrong nfft or chunks_per_frame");
        return DSP_FRAME_SUB_INVALID;
    }
    if (cfg->nfft > DSP_MAX_NFFT) {
        LV_LOG_WARN("dsp_frame_subscribe: nfft %u > %d", cfg->nfft, DSP_MAX_NFFT);
        return DSP_FRAME_SUB_INVALID;
    }

    std::lock_guard<std::mutex> lock(frame_subs_mutex);

    for (size_t i = 0; i < MAX_FRAME_SUBS; i++) {
        if (frame_subs[i].used) {
            continue;
        }
        FrameSub &s = frame_subs[i];

        if (next_frame_sub_id == DSP_FRAME_SUB_INVALID) {
            next_frame_sub_id = 1;
        }
        s.id               = next_frame_sub_id++;
        s.nfft             = cfg->nfft;
        s.chunks_per_frame = cfg->chunks_per_frame;
        s.allow_vary_freq  = cfg->allow_vary_freq;
        s.cb               = cb;
        s.ud               = user_data;
        build_decim_ranges(s);
        frame_sub_zero(s);
        s.chunks_elapsed = 0;
        s.used           = true;
        s.active.store(true, std::memory_order_relaxed);
        return s.id;
    }

    return DSP_FRAME_SUB_INVALID;
}

void dsp_frame_set_active(uint32_t id, bool active) {
    if (id == DSP_FRAME_SUB_INVALID) {
        return;
    }

    std::lock_guard<std::mutex> lock(frame_subs_mutex);

    for (size_t i = 0; i < MAX_FRAME_SUBS; i++) {
        if (frame_subs[i].used && frame_subs[i].id == id) {
            if (active) {
                /* Start the accumulation window fresh so a resumed subscriber
                 * does not blend in samples from before the pause. The cadence
                 * counter is kept, so the first usable chunk after resume is
                 * delivered immediately. */
                frame_sub_zero(frame_subs[i]);
            }
            frame_subs[i].active.store(active, std::memory_order_release);
            return;
        }
    }
}

void dsp_frame_set_chunks_per_frame(uint32_t id, uint16_t chunks_per_frame) {
    if (id == DSP_FRAME_SUB_INVALID || chunks_per_frame == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(frame_subs_mutex);

    for (size_t i = 0; i < MAX_FRAME_SUBS; i++) {
        if (frame_subs[i].used && frame_subs[i].id == id) {
            /* The accumulation window and the cadence counter are intentionally
             * left as is: already accumulated samples are valid, only the
             * averaging interval changes. Lowering the interval below the
             * current chunks_elapsed makes the subscriber immediately due, so
             * the current window is emitted on the next usable chunk. */
            frame_subs[i].chunks_per_frame = chunks_per_frame;
            return;
        }
    }
}

void dsp_frame_unsubscribe(uint32_t id) {
    if (id == DSP_FRAME_SUB_INVALID) {
        return;
    }

    std::lock_guard<std::mutex> lock(frame_subs_mutex);

    for (size_t i = 0; i < MAX_FRAME_SUBS; i++) {
        if (frame_subs[i].used && frame_subs[i].id == id) {
            frame_subs[i].active.store(false, std::memory_order_relaxed);
            frame_subs[i].cb   = nullptr;
            frame_subs[i].ud   = nullptr;
            frame_subs[i].used = false;
            frame_subs[i].id   = 0;
            return;
        }
    }
}

void dsp_put_audio_samples(size_t nsamples, int16_t *samples) {
    if (!ready) {
        return;
    }

    std::lock_guard<std::mutex> lock(subs_mutex);

    for (size_t i = 0; i < MAX_AUDIO_SUBS; i++) {
        if (subs[i].kind == AUDIO_SUB_RAW && subs[i].raw_cb != nullptr && subs[i].exclusive &&
            subs[i].active.load(std::memory_order_acquire)) {
            subs[i].raw_cb(nsamples, samples);
            return;
        }
    }

    for (size_t i = 0; i < MAX_AUDIO_SUBS; i++) {
        if (subs[i].kind == AUDIO_SUB_RAW && subs[i].raw_cb != nullptr && !subs[i].exclusive &&
            subs[i].active.load(std::memory_order_acquire)) {
            subs[i].raw_cb(nsamples, samples);
        }
    }

    float float_samples[nsamples];
    vector_s16_to_f(samples, float_samples, nsamples);
    for (size_t i = 0; i < nsamples; i++) {
        iirfilt_rrrf_execute(audio_dc_blocker, float_samples[i], &float_samples[i]);
    }

    for (size_t si = 0; si < MAX_AUDIO_SUBS; si++) {
        if (subs[si].kind != AUDIO_SUB_FLOAT) {
            continue;
        }
        if (!subs[si].active.load(std::memory_order_acquire)) {
            continue;
        }

        Resampler *s = subs[si].resampler;
        if (!s) {
            // No resample
            subs[si].float_cb(nsamples, float_samples);
        } else {
            size_t decim = s->decim_factor();
            size_t out_n = 0;
            float scratch[nsamples / decim + 1];

            for (size_t i = 0; i < nsamples; i++) {
                if (s->feed(float_samples[i])) {
                    scratch[out_n++] = s->execute();
                }
            }
            if (out_n > 0) {
                subs[si].float_cb(out_n, scratch);
            }
        }
    }
}

/* Resolve the min/max pair passed to the spectrum/waterfall/scope consumers.
 * Single point of level policy: TX defaults, auto levels + offset, or the
 * manual grid levels. Called once per frame from deliver_frames(). */
static void dsp_resolve_levels(bool tx, float *out_min, float *out_max) {
    if (tx) {
        *out_min = DSP_TX_LEVEL_MIN;
        *out_max = DSP_TX_LEVEL_MAX;
    } else if (cfg.ui.auto_level_enabled()->get()) {
        float offset = cfg.ui.auto_level_offset()->get();
        *out_min = auto_min - offset;
        *out_max = auto_max - offset;
    } else {
        *out_min = cfg.band.grid_min()->get();
        *out_max = cfg.band.grid_max()->get();
    }
}
