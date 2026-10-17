#pragma once

// SettingsManager: orchestrator that owns the global/band/mode parameters and
// the deferred-write queue (PendingWrites), and implements band/mode switching
// semantics.
//
// Scope of this header (project rules):
//   - Only src/cfg/ dependencies: parameter.h, pending_writes.h,
//     computed_parameter.h, db.h.
//   - Parameters live statically (owned by the manager, never deleted) and
//     are wired to the pending-write queue at construction time.
//   - No exceptions, C++17 only.
//
// The manager is not thread-safe itself: the UI / C-API layer serialises
// access (single event loop). Individual SubjectT/Parameter values still
// protect themselves with their own mutexes, and PendingWrites has its own
// mutex, so concurrent readers of parameter values remain safe.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <ft8lib/constants.h>

#include "../common/math.h"
#include "../settings_types.h"
#include "computed_parameter.h"
#include "encoder_bind_types.h"
#include "parameter.h"
#include "pending_writes.h"

extern "C" {
#include <aether_radio/x6100_control/control.h>
}

enum ModeGroup {
    MODE_GROUP_SSB = x6100_mode_lsb,
    MODE_GROUP_DIGI = x6100_mode_lsb_dig,
    MODE_GROUP_CW = x6100_mode_cw,
    MODE_GROUP_AM = x6100_mode_am,
    MODE_GROUP_FM = x6100_mode_nfm,
};

class SettingsManager {
  public:
    // Deferred-write buffer (PendingWrites). Declared BEFORE the parameters
    // (C++ member initialisation order) because every Parameter enqueues its
    // changes into this buffer via a WriteSink& bound at construction time.
    // Public because it is the shared sink of the managed parameters;
    // flushing goes through the manager methods (flush_all / flush_storage).
    PendingWrites pending_writes_;

    // Parameter registries (non-owning). MUST be declared before any Parameter
    // member — self-registration in NSDMI constructors pushes into these
    // vectors, so they must already exist (C++ member initialisation order).
    std::vector<ParamBase *> global_params_;
    std::vector<ParamBase *> band_params_;
    std::vector<ParamBase *> mode_params_;

    // -- Parameters (public for C-API / UI access; live for the whole program) --
    // All regular GLOBAL/BAND/MODE params are declared with NSDMI here (one
    // line each — the single source of truth). The trailing group argument
    // (&global_params_ / &band_params_ / &mode_params_) makes each Parameter
    // self-register into its registry on construction. VFO params and computed
    // params are deliberately NOT registered (loaded in dependency order by
    // load_band_vfo).

    // --- GLOBAL params (flat `params` table) ---
    // General
    // clang-format off
    Parameter<int32_t> p_volume{"vol", 30, 0, 55,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float> p_pwr{"pwr", 5.0f, 0.1f, 10.0f,
        StorageType::GLOBAL, pending_writes_, &global_params_, 0, 10};
    Parameter<int32_t> p_squelch{"sql", 0, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_rfgain{"rfgain", 63, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    Parameter<int32_t> p_rit{"rit", 0, -1500, 1500,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_xit{"xit", 0, -1500, 1500,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // Current band id, persisted in the global `params` table (mirrors the
    // legacy cfg.band_id). Restored at init_load to pick the starting band;
    // updated on every band switch so a restart returns to the last band.
    // Default 7 = 20m SSB, matching the VFOA default frequency of 14.1 MHz.
    Parameter<int32_t> p_band_id{"band_id", 7, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_mic{"mic", x6100_mic_auto, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_hmic{"hmic", 20, 0, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_imic{"imic", 30, 0, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_moni{"moni", 30, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ant_id{"ant", 1, 0, 5,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_atu_enabled{"atu", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_cat_baud{"cat_baud", 19200, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // UI
    Parameter<int32_t> p_auto_level_enabled{"auto_level_enabled", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float> p_auto_level_offset{"auto_level_offset", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_, 0, 5};
    Parameter<int32_t> p_display_invert{"invert", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_knob_info{"knob_info", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_use_custom_color{"spectrum_use_custom_color", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_color{"spectrum_color", (int32_t)0xFFEAC345, spectrum_color_validate,
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    // String mapping encoder (cfg_ctrl_t) positions to bind values
    // (ENCODER_BIND_VOL/ENCODER_BIND_MFK/ENCODER_BIND_NONE). GLOBAL: persisted in the flat `params` table under key
    // "encoder_bind", matching the legacy src/cfg key for DB compatibility.
    Parameter<std::string> p_encoder_bind{"encoder_bind", make_default_encoder_bind(), encoder_bind_validate,
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_show_meter_values{"show_meter_values", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    // 1KO125: S-meter calibration offset in dB (display and CAT)
    Parameter<int32_t> p_s_meter_offset{"s_meter_offset", 0, -30, 30,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // VOX
    Parameter<int32_t> p_vox_en{"vox_en", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vox_gain{"vox_gain", 0, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vox_ag{"vox_ag", 0, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_vox_delay{"vox_delay", 500, 100, 2000,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // FT8
    Parameter<int32_t> p_ft8_show_all{"ft8_show_all", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_protocol{"ft8_protocol", FTX_PROTOCOL_FT8, 0, (int32_t)FTX_PROTOCOL_FT8,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_auto{"ft8_auto", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_hold_freq{"ft8_hold_freq", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_ft8_max_repeats{"ft8_max_repeats", 6, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // SWR scan
    Parameter<int32_t> p_swrscan_linear{"swrscan_linear", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_swrscan_span{"swrscan_span", 200'000, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // CW
    Parameter<int32_t> p_key_tone{"key_tone", 700, 400, 1200,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_speed{"key_speed", 15, 5, 50,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_mode{"key_mode", x6100_key_manual, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_iambic_mode{"iambic_mode", x6100_iambic_a, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_vol{"key_vol", 10, 0, 32,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_key_train{"key_train", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_qsk_time{"qsk_time", 100, 0, 1000,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float> p_key_ratio{"key_ratio", 3.0f, 2.5f, 4.5f,
        StorageType::GLOBAL, pending_writes_, &global_params_, 0, 10};
    Parameter<int32_t>            p_cw_peak_on{"cw_peak_on", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t>            p_cw_peak_q{"cw_peak_q", 1, 1, 16,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // CW decoder
    Parameter<int32_t> p_cw_decoder{"cw_decoder", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_cw_tune{"cw_tune", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float> p_cw_decoder_snr{"cw_decoder_snr_2", 8.0f, 5.0f, 30.0f,
        StorageType::GLOBAL, pending_writes_, &global_params_, 0, 10};
    Parameter<float> p_cw_decoder_snr_gist{"cw_decoder_snr_gist", 1.0f, 0.0f, 30.0f,
        StorageType::GLOBAL, pending_writes_, &global_params_, 0, 10};

    // AGC
    Parameter<int32_t> p_agc_hang{"agc_hang", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_agc_knee{"agc_knee", -60, -100, 0,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_agc_slope{"agc_slope", 6, 0, 10,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // DSP
    Parameter<int32_t> p_dnf{"dnf", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_dnf_center{"dnf_center", 1000, 100, 3000,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_dnf_width{"dnf_width", 50, 10, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_dnf_auto{"dnf_auto", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nb{"nb", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nb_level{"nb_level", 10, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nb_width{"nb_width", 10, 0, 100,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nr{"nr", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_nr_level{"nr_level", 0, 0, 60,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // DSP custom
    Parameter<float> p_output_gain{"output_gain", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_, 0, 5};
    Parameter<int32_t>           p_comp{"comp", 4, 1, 8,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float> p_comp_threshold_offset{"comp_threshold_offset", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_, 0, 2};
    Parameter<float> p_comp_makeup_offset{"comp_makeup_offset", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_, 0, 2};
    Parameter<int32_t>            p_fm_emphasis{"fm_emphasis", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t>            p_tx_filter_low{"tx_filter_low", 160, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t>            p_tx_filter_high{"tx_filter_high", 3000, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t>            p_cessb_on{"cessb_on", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<float> p_cessb_power_up{"cessb_power_up", 3.7f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_, 0, 10};

    // --- GLOBAL params migrated from the legacy params module (stage 8.2) ---
    // DB keys are 1:1 with the legacy field names.

    // Display / backlight
    Parameter<int32_t> p_brightness_normal{"brightness_normal", 9, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_brightness_idle{"brightness_idle", 1, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_brightness_timeout{"brightness_timeout", 10, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_brightness_buttons{"brightness_buttons", BUTTONS_TEMPORARILY, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // Clock
    Parameter<int32_t> p_clock_view{"clock_view", CLOCK_TIME_POWER, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_clock_time_timeout{"clock_time_timeout", 5, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_clock_power_timeout{"clock_power_timeout", 3, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_clock_tx_timeout{"clock_tx_timeout", 1, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // Spectrum (migrated legacy fields)
    Parameter<int32_t> p_spectrum_beta{"spectrum_beta", 70, 0, 90,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_peak{"spectrum_peak", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_peak_hold{"spectrum_peak_hold", 5, 1, 10,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_peak_speed{"spectrum_peak_speed", 5, 1, 30,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_filled{"spectrum_filled", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_spectrum_height{"spectrum_height", 160, 160, 260,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // Waterfall
    Parameter<int32_t> p_waterfall_center_line{"waterfall_center_line", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // Main screen magnification
    Parameter<int32_t> p_mag_freq{"mag_freq", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_mag_info{"mag_info", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_mag_alc{"mag_alc", true, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // Voice
    Parameter<int32_t> p_voice_mode{"voice_mode", VOICE_LCD, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_voice_lang{"voice_lang", 0, 0, VOICES_NUM - 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_voice_rate{"voice_rate", 100, 50, 150,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_voice_pitch{"voice_pitch", 100, 50, 150,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_voice_volume{"voice_volume", 100, 50, 150,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_voice_msg_period{"voice_msg_period", 10, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // Audio play/record gain (REAL in the DB, no scaling: matches legacy)
    Parameter<float> p_play_gain_db{"play_gain_db_f", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<float> p_rec_gain_db{"rec_gain_db_f", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // RTTY
    Parameter<int32_t> p_rtty_center{"rtty_center", 800, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_rtty_shift{"rtty_shift", 170, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_rtty_rate{"rtty_rate", 4545, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_rtty_reverse{"rtty_reverse", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // CW beacon period (legacy cw field; CW group lives below)
    Parameter<int32_t> p_cw_encoder_period{"cw_encoder_period", 10, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // FT8 (legacy fields)
    Parameter<int32_t>    p_ft8_tx_freq{"ft8_tx_freq", 1325, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<float>      p_ft8_output_gain_offset{"ft8_output_gain_offset", 0.0f, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<std::string> p_ft8_cq_modifier{"ft8_cq_modifier", "", {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // JS8 (the names the JS8 test builds stored in params.db: values carry over)
    Parameter<int32_t> p_js8_tx_freq{"js8_tx_freq", 1500, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_}; // TX offset in the passband, Hz
    Parameter<int32_t> p_js8_hold_offset{"js8_hold_offset", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_js8_auto{"js8_auto", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // AUTO: automatic replies
    Parameter<int32_t> p_js8_hb{"js8_hb", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // heartbeats
    Parameter<int32_t> p_js8_hb_ack{"js8_hb_ack", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_js8_relay{"js8_relay", 1, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // pass relays (>) on and hold MSG TO: for others, as desktop
    Parameter<int32_t> p_js8_st_keep{"js8_st_keep", 2, 0, 5,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Settings: how long stations stay listed (index; 2 = 1 hour)
    Parameter<int32_t> p_js8_msg_keep{"js8_msg_keep", 0, 0, 4,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Settings: how long messages stay listed (index; 0 = all)
    Parameter<int32_t> p_js8_miles{"js8_miles", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Settings: distances in miles, not km
    Parameter<int32_t> p_js8_decode_marks{"js8_decode_marks", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Settings: decode attempts marked on the waterfall
    Parameter<int32_t> p_js8_wf_avg{"js8_wf_avg", 1, 0, 3,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Settings: waterfall rows averaged over time: 0 Sharp, 1 Light (default), 2 Medium, 3 Calm (less flicker as it steps)
    Parameter<int32_t> p_js8_map_mode{"js8_map_mode", 0, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Show Map: 0 Auto, 1 Close-in, 2 World
    Parameter<int32_t> p_js8_st_sort{"js8_st_sort", 0, 0, 4,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Stations view's Sort: 0 Heard you, 1 SNR, 2 Time, 3 Distance
    Parameter<int32_t> p_js8_tsync_auto{"js8_tsync_auto", 1, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Time: Auto (JS8's timing follows the decodes, as desktop's Automatic Time Drift)
    Parameter<int32_t> p_js8_hb_interval{"js8_hb_interval", 30, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_}; // minutes
    Parameter<int32_t> p_js8_cq_interval{"js8_cq_interval", 1, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_}; // auto CQ: minutes after each CQ ends
    Parameter<int32_t> p_js8_log_prompt{"js8_log_prompt", 1, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // offer to log when a QSO ends
    Parameter<int32_t> p_js8_log_activation{"js8_log_activation", 0, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // 0 off, 1 POTA, 2 SOTA
    Parameter<int32_t> p_js8_alerts{"js8_alerts", 7, 0, 31,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // JS8_ALERT_* bits in dialog_js8.c (7: beep, to me, inbox)
    Parameter<int32_t> p_js8_speed{"js8_speed", 0, 0, 4,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // js8_speed_t we transmit at (0: Normal, 4: Ultra)
    Parameter<int32_t> p_js8_rx_all{"js8_rx_all", 1, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // decode every speed (desktop's multi-decoder)
    Parameter<int32_t> p_js8_ultra{"js8_ultra", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // Ultra on the Speed button (it is always decoded)
    Parameter<int32_t> p_js8_ghostnet{"js8_ghostnet", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // band keys step through GhostNet's frequencies
    Parameter<int32_t> p_js8_custom_on{"js8_custom_on", 0, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_}; // tuned to custom_hz, not a preset
    Parameter<int32_t> p_js8_custom_hz{"js8_custom_hz", 0, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_}; // last custom dial frequency, 0 = none

    // Station identity
    Parameter<std::string> p_qth{"qth", "", {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<std::string> p_callsign{"callsign", "", {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // Network
    Parameter<int32_t> p_wifi_enabled{"wifi_enabled", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // Long-press / HMic action bindings
    Parameter<int32_t> p_long_gen{"long_gen", ACTION_SCREENSHOT, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_long_app{"long_app", ACTION_APP_RECORDER, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_long_key{"long_key", ACTION_NONE, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_long_msg{"long_msg", ACTION_RECORDER, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_long_dfn{"long_dfn", ACTION_VOICE_MODE, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_long_dfl{"long_dfl", ACTION_BAT_INFO, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_press_f1{"press_f1", ACTION_STEP_UP, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_press_f2{"press_f2", ACTION_NONE, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_long_f1{"long_f1", ACTION_STEP_DOWN, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_long_f2{"long_f2", ACTION_NONE, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // Radio hardware settings
    Parameter<int32_t> p_charger{"charger", 1, 0, 2,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_line_in{"line_in", 10, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_line_out{"line_out", 10, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_spmode{"spmode", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_freq_accel{"freq_accel", FREQ_ACCEL_LITE, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};

    // Appearance
    Parameter<int32_t> p_theme{"theme", THEME_SIMPLE, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_meter_color{"meter_color", METER_GRAY, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    /* FT8/FT4 table, right side: 0 km, 1 mi, 2 country, 3 country + km,
     * 4 country + mi (table_view_info_t). */
    Parameter<int32_t> p_ft8_info{"ft8_info", 3, 0, 4,
        StorageType::GLOBAL, pending_writes_, &global_params_};
    Parameter<int32_t> p_swr_color{"swr_color", SWR_GRAY, {},
        StorageType::GLOBAL, pending_writes_, {}, &global_params_};
    Parameter<int32_t> p_digi_text_compact{"digi_text_compact", false, 0, 1,
        StorageType::GLOBAL, pending_writes_, &global_params_};

    // --- BAND params (`band_params` table) ---
    Parameter<int32_t> p_band_current_vfo{"vfo", (int32_t)X6100_VFO_A, 0, 1,
        StorageType::BAND, pending_writes_, &band_params_};
    Parameter<int32_t> p_band_if_shift{"if_shift", 0, -40'000, 40'000,
        StorageType::BAND, pending_writes_, &band_params_};
    Parameter<int32_t> p_band_grid_min{"grid_min", -121, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    Parameter<int32_t> p_band_grid_max{"grid_max", -73, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    Parameter<int32_t> p_band_split{"split", false, 0, 1,
        StorageType::BAND, pending_writes_, &band_params_};
    Parameter<int32_t> p_band_tx_i_offset{"tx_i_offset", 0, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    Parameter<int32_t> p_band_tx_q_offset{"tx_q_offset", 0, {},
        StorageType::BAND, pending_writes_, {}, &band_params_};
    // dB offset of the front panel audio chain (float, stored as REAL,
    // quantized to 0.2 dB (quantize=5)).
    Parameter<float> p_band_dac_offset{"dac_offset", 0.0f, {},
        StorageType::BAND, pending_writes_, {}, &band_params_, 0, 5};
    // BAND VFO params (context_id = bands_id). These are NOT in the band
    // registry: load_band_vfo() loads them in explicit dependency order so the
    // DB restore/clamp rules can reference already-loaded siblings.
    Parameter<int32_t> p_band_vfoa_freq{"vfoa_freq", 14'100'000, 100'000, 500'000'000,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_freq{"vfob_freq", 14'150'000, 100'000, 500'000'000,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_mode{"vfoa_mode", x6100_mode_usb, (int32_t)x6100_mode_lsb, (int32_t)x6100_mode_nfm,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_mode{"vfob_mode", x6100_mode_usb, (int32_t)x6100_mode_lsb, (int32_t)x6100_mode_nfm,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_att{"vfoa_att", x6100_att_off, 0, (int32_t)x6100_att_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_pre{"vfoa_pre", x6100_pre_off, 0, (int32_t)x6100_pre_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfoa_agc{"vfoa_agc", x6100_agc_auto, 0, (int32_t)x6100_agc_auto,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_att{"vfob_att", x6100_att_off, 0, (int32_t)x6100_att_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_pre{"vfob_pre", x6100_pre_off, 0, (int32_t)x6100_pre_on,
        StorageType::BAND, pending_writes_};
    Parameter<int32_t> p_band_vfob_agc{"vfob_agc", x6100_agc_auto, 0, (int32_t)x6100_agc_auto,
        StorageType::BAND, pending_writes_};

    // --- Transverter params (OTHER storage, fixed HW conversion) ---
    // context_id is the fixed transverter number (0 or 1) — never switched. NOT registered
    // in any load-all registry (group = nullptr): they are loaded individually
    // in init_load() with explicit load(0)/load(1). Values are frequencies in Hz.
    // Transverter 0 (2m: 144-150 MHz, IF at 28 MHz)
    Parameter<int32_t> p_transverter_0_from{"from", 144'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 0};
    Parameter<int32_t> p_transverter_0_to{"to", 150'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 0};
    Parameter<int32_t> p_transverter_0_shift{"shift", 116'000'000, 0, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 0};
    // Transverter 1 (70cm: 432-438 MHz, IF at 28 MHz)
    Parameter<int32_t> p_transverter_1_from{"from", 432'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 1};
    Parameter<int32_t> p_transverter_1_to{"to", 438'000'000, 70'000'000, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 1};
    Parameter<int32_t> p_transverter_1_shift{"shift", 404'000'000, 0, 500'000'000,
        StorageType::TRANSVERTER, pending_writes_, nullptr, 1};

    // --- MODE params (`mode_params` table) ---
    // Parameter<int32_t> p_mode_squelch{
    //     "squelch",    0, StorageType::MODE, pending_writes_, [](int32_t v) { return clip(v, 0, 100); }, {},
    //     &mode_params_};
    Parameter<int32_t> p_mode_freq_step{"freq_step", 500, [](int32_t v) { return clip(v, 1, 10000); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_freq_step_not_found(); }, &mode_params_};
    Parameter<int32_t> p_mode_zoom{"spectrum_factor", 1, [this](int32_t v) { return zoom_validate(v); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_zoom_not_found(); }, &mode_params_};

  private:
    // MODE-scoped filter band edges. PRIVATE: never read/written directly
    // outside this class; external C++ reads the effective values via the
    // public computed params cp_cur_filter_low/high/bw and mutates them
    // through their set() (reverse path). Still self-register into
    // mode_params_ so load_mode_all/switch_mode load them like any mode param.
    // Cross-validators reference the sibling member; they only run on set /
    // set_quiet (never during construction), by which time both exist.
    Parameter<int32_t> p_mode_filter_low{"filter_low", 50, [this](int32_t v) { return filter_low_validate(v); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_filter_low_not_found(); }, &mode_params_};
    Parameter<int32_t> p_mode_filter_high{"filter_high", 2900, [this](int32_t v) { return filter_high_validate(v); },
        StorageType::MODE, pending_writes_, [this]() { on_mode_filter_high_not_found(); }, &mode_params_};
    // clang-format on

  public:
    // Computed parameter: front-panel ("FG") frequency = the frequency of the
    // active VFO of the current band. Constructed with compute/reverse closures
    // in the SettingsManager ctor (no default ctor exists).
    ComputedParameter<int32_t> cp_fg_freq;

    // Computed parameter: the mode of the active VFO of the current band.
    // Analogous to cp_fg_freq: p_band_current_vfo selects between the VFO A/B
    // mode params. Constructed with compute/reverse closures in the ctor.
    ComputedParameter<int32_t> cp_cur_mode;

    // Computed parameters for the current VFO's att/pre/agc and the background
    // VFO frequency. Att/pre/agc track the ACTIVE VFO's value; bg_freq returns
    // the INACTIVE VFO's frequency. Constructed with closures in the ctor,
    // sources bound in init_load().
    ComputedParameter<int32_t> cp_cur_att;
    ComputedParameter<int32_t> cp_cur_pre;
    ComputedParameter<int32_t> cp_cur_agc;
    ComputedParameter<int32_t> cp_bg_freq;

    // Computed mode LO offset: CW → -key_tone, CWR → +key_tone, else 0.
    // Driven by cp_cur_mode and p_key_tone.
    ComputedParameter<int32_t> cp_mode_lo_offset;

    // Computed current filter params for the active MODE category. The compute
    // fns map the MODE-scoped filter_low/filter_high (and key_tone for CW)
    // through the mode category (SSB/AM/FM/CW); the reverse fns write back into
    // them. Modelled on cp_fg_freq/cp_cur_mode: constructed with closures in the
    // ctor (no default ctor), sources bound in init_load().
    ComputedParameter<int32_t> cp_cur_filter_low;
    ComputedParameter<int32_t> cp_cur_filter_high;
    ComputedParameter<int32_t> cp_cur_filter_bw;

    // -- Construction / lifetime --
    explicit SettingsManager();
    ~SettingsManager();

    SettingsManager(const SettingsManager &)            = delete;
    SettingsManager &operator=(const SettingsManager &) = delete;
    SettingsManager(SettingsManager &&)                 = delete;
    SettingsManager &operator=(SettingsManager &&)      = delete;

    // -- Initialisation --
    // Loads global params (including the persisted band_id), then band params
    // for the restored band and mode params for the mode derived from the
    // active VFO (cp_cur_mode). on_db_error (optional) is invoked on DB
    // failures; parameters keep their current values on NOT_FOUND.
    void init_load(void (*on_db_error)(const char *msg) = nullptr);

    // -- PendingWrites --
    void flush_all();
    void flush_storage(StorageType type, int context_id);

    // Background deferred-save thread: wakes every 3 seconds (or on notify)
    // and calls flush_all().
    //
    // The flush thread must satisfy PendingWrites invariants I1/I2 (see
    // pending_writes.h): it only reads the queue's own snapshots + the
    // stateless policies and never touches SettingsManager member state
    // (band_id_, mode_id_, the parameters), so it cannot race manager context
    // or param mutation.
    void start_flush_thread();
    void stop_flush_thread();

    // -- Accessors --
    int current_band_id() const { return band_id_; }
    int current_mode_group_id() const { return mode_group_id_; }

    // Hardware-usable frequency range limits (in Hz). HF is the native
    // 0.1-55 MHz band; anything above must be covered by a transverter.
    static constexpr int32_t HF_MIN_FREQ = 100'000;
    static constexpr int32_t HF_MAX_FREQ = 55'000'000;

    // -- VFO operations --
    // Copies VFO-related band parameters (freq, mode, agc, att, pre) from the
    // active VFO to the inactive VFO, based on the current p_band_current_vfo.
    void cfg_band_vfo_copy();

    // Transverter shift for a frequency: the shift of the transverter whose
    // [from, to] range contains `freq`, or 0 when no transverter covers it.
    int32_t transverter_shift_for(int32_t freq) const {
        if (freq >= p_transverter_0_from.get() && freq <= p_transverter_0_to.get())
            return p_transverter_0_shift.get();
        if (freq >= p_transverter_1_from.get() && freq <= p_transverter_1_to.get())
            return p_transverter_1_shift.get();
        return 0;
    }

    // True when `freq` is usable by the hardware: HF 0.5-55 MHz or inside any
    // transverter [from, to] range. Bands-table ranges are informational only.
    bool is_valid_hw_freq(int32_t freq) const {
        if (freq >= HF_MIN_FREQ && freq <= HF_MAX_FREQ) {
            return true;
        }
        return transverter_shift_for(freq) != 0;
    }

    // If `freq` is hardware-usable, return it unchanged. Otherwise clamp to the
    // nearest boundary of any hardware-usable frequency range (HF 0.5-55 MHz or
    // the configured transverter [from, to] ranges).
    int32_t clamp_to_valid_hw_freq(int32_t freq) const;

  private:
    // active VFO frequency of the current band (compute fn for cp_fg_freq).
    int32_t fg_freq_get();

    // reverse fn for cp_fg_freq: writes freq into the active VFO param.
    void fg_freq_set(int32_t freq);

    // compute/reverse fns for cp_bg_freq: the background (inactive) VFO's
    // frequency, i.e. the complement of the active VFO.
    int32_t bg_freq_get();
    void    bg_freq_set(int32_t freq);

    // active VFO mode of the current band (compute fn for cp_cur_mode).
    int32_t cur_mode_get();

    // reverse fn for cp_cur_mode: writes mode into the active VFO's mode param.
    void cur_mode_set(int32_t mode);

    // compute/reverse fns for cp_cur_att / cp_cur_pre / cp_cur_agc: read/write
    // the active VFO's att/pre/agc param based on p_band_current_vfo.
    int32_t cur_att_get();
    void    cur_att_set(int32_t att);
    int32_t cur_pre_get();
    void    cur_pre_set(int32_t pre);
    int32_t cur_agc_get();
    void    cur_agc_set(int32_t agc);

    // Trampoline for the cp_cur_mode observer: calls switch_mode() with the new
    // mode whenever cp_cur_mode's value changes.
    static void switch_mode_observer_cb(Subject *subj, void *user_data);

    // Trampoline for the p_band_vfoa_freq / p_band_vfob_freq observers: when a
    // VFO frequency changes into a different band, triggers an implicit band
    // switch (the frequency that caused the switch is preserved).
    static void vfo_freq_change_cb(Subject *subj, void *user_data);

    // Trampoline for the p_band_id observer: an explicit band switch is triggered
    // by setting p_band_id; the observer runs switch_band with implicit=false.
    static void switch_band_observer_cb(Subject *subj, void *user_data);

    // Mode group functions
    static ModeGroup mode_group(x6100_mode_t mode);
    static int32_t mode_group_filter_low_default(ModeGroup group);
    static int32_t mode_group_filter_high_default(ModeGroup group);
    static int32_t mode_group_freq_step_default(ModeGroup group);
    static int32_t mode_group_zoom_default(ModeGroup group);

    void on_mode_filter_low_not_found();
    void on_mode_filter_high_not_found();
    void on_mode_freq_step_not_found();
    void on_mode_zoom_not_found();

    static int32_t spectrum_color_validate(int32_t full);

    // Shared band-switch core: flush, rebind context, switch-time loads and
    // recomputes. `implicit` selects whether the active VFO's freq+mode is kept.
    void switch_band(int new_band_id, bool implicit);

    // -- Context switching --
    // Switches mode context: saves pending mode writes and loads mode params
    // for new_group_id.
    void switch_mode(ModeGroup new_group_id);

    // Apply the current band/mode context to every parameter in the group.
    void set_band_context(int band_id);
    void set_mode_context(int mode_id);

    // Full group loads (used by init_load).
    void load_band_all(int band_id);
    void load_mode_all();

    // Full reset to a clean state: drop internal observers, unbind every
    // computed parameter, restore all parameters to their construction-time
    // defaults and zero the band/mode context. Emits no notifications itself:
    // init_load holds one NotifySuppressGuard over reset+reload and is
    // idempotent (no accumulated observers, no stale context/values).
    void reset_state();

    // Switch-time band loads: never loads current_vfo; when `implicit` is true
    // also keeps the active VFO's freq+mode (loads only the inactive VFO).
    void load_band_switch(int new_band_id, bool implicit);

    // Loads the four VFO params (vfoa_freq, vfoa_mode, vfob_freq, vfob_mode) in
    // explicit dependency order, applying the DB restore/clamp rules:
    //   - vfoa_freq: NOT_FOUND -> BandInfo.start_freq (or 12000000 if the band
    //     is undefined/missing); a loaded value outside [start,stop] is clamped
    //     to start_freq.
    //   - vfoa_mode: NOT_FOUND/error -> resolve_default_mode(vfoa_freq).
    //   - vfob_freq: NOT_FOUND -> current vfoa_freq; else clamped as vfoa_freq.
    //   - vfob_mode: NOT_FOUND/error -> current vfoa_mode.
    // Restored/clamped values are applied with set_quiet (no deferred write).
    // When `implicit` the active VFO's freq+mode is skipped (kept, not loaded).
    void load_band_vfo(int band_id, bool implicit);

    // Default mode for a frequency: LSB below 10 MHz, USB at or above.
    static int32_t resolve_default_mode(int32_t freq);

    // Default 49-char encoder-bind string (all NONE except the documented
    // VOL/MFK positions). Static: callable from the ctor init list.
    static std::string make_default_encoder_bind();

    // Validate/pad the encoder-bind string to CTRL_FAST_ACCESS_LAST chars
    // (filling any shorter input with ENCODER_BIND_NONE). Pure function.
    static std::string encoder_bind_validate(const std::string &val);

    // Filter category of a mode (SSB/AM/FM/CW; unknown/default -> SSB).
    enum class FilterMode {
        SSB,
        AM,
        FM,
        CW
    };
    FilterMode filter_mode(int32_t mode) const;

    // Validate filters (cross-validate against the sibling filter edge; read
    // member params via `this`).
    int32_t filter_low_validate(int32_t v);
    int32_t filter_high_validate(int32_t v);

    // Validate zoom
    int32_t zoom_validate(int32_t v);

    // compute fns for cp_cur_filter_{low,high,bw}.
    int32_t cur_filter_low_compute();
    int32_t cur_filter_high_compute();
    int32_t cur_filter_bw_compute();

    int32_t lo_offset_compute();

    // reverse fns for cp_cur_filter_{low,high,bw}: map back into
    // p_mode_filter_low/high per the current mode category.
    void cur_filter_low_reverse(int32_t v);
    void cur_filter_high_reverse(int32_t v);
    void cur_filter_bw_reverse(int32_t v);

    // -- Member data --
    int band_id_ = 0;
    int mode_group_id_ = 0;

    // Re-entrancy guard for switch_band: prevents the p_band_id observer (which
    // fires from inside switch_band's p_band_id.set) from running a nested
    // switch_band with a different `implicit` flag.
    bool band_switch_active_ = false;

    // Holds the cp_cur_mode observer that triggers switch_mode() on mode change.
    // RAII: unsubscribes on destruction.
    Subscription switch_mode_obs_;

    // Holds the p_band_id observer that triggers an explicit band switch when
    // p_band_id is set. RAII: unsubscribe on destruction.
    Subscription band_id_obs_;

    // Holds the VFO frequency observers that trigger an implicit band switch
    // when the active VFO is tuned into a different band. RAII: unsubscribe on
    // destruction.
    Subscription vfoa_freq_obs_;
    Subscription vfob_freq_obs_;

    // Background flush thread control.
    bool                    flush_thread_running_ = false;
    std::thread             flush_thread_;
    std::condition_variable flush_cv_;
    std::mutex              flush_mutex_;
};

