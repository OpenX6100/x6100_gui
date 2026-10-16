#pragma once

// C-compatible API for the SettingsManager's parameters. This single header is
// the umbrella entry point for C (and C++) consumers: it includes the generic
// Subject helpers (subject_api.h) and adds the Parameter<T>/ComputedParameter<T>
// typed access, the ATU tuner-network cache API and the SettingsManager-level
// operations, and exposes the parameter access tree `cfg`.
//
// Access is through cfg.<group>.<name>(), which returns the Parameter<T>*
// handle (opaque in C, concrete in C++). All writes route through
// Parameter<T>::set, so validators, deferred-write enqueue and observer
// notifications all apply.
// Ownership: the parameters (and the SettingsManager singleton) are static and
// owned by C++ (cfg_api.cpp). C code only receives/holds borrowed pointers and
// must never free them.
//
// cfg_api_init() does NOT open the DB or call cfg_db_init(): the caller owns
// the sqlite3 connection and table initialisation (avoids double-Init).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "subject_api.h"

// Upper bound for a text parameter copied into a PARAM_T_GET stack buffer.
#define PARAM_TEXT_MAX 64

// Copy the parameter's text into caller-owned storage (NUL-terminated,
// truncated to n-1 chars) and return dst. Unlike the old shared-buffer
// param_t_get, every call writes to its own destination, so several results can
// coexist. C callers normally use PARAM_T_GET, which supplies a fresh stack
// buffer per expansion.
#ifndef __cplusplus
#define PARAM_T_GET(p) param_t_get_into((p), (char[PARAM_TEXT_MAX]){0}, PARAM_TEXT_MAX)
#endif

#ifdef __cplusplus
#include <string>

#include "computed_parameter.h"
#include "parameter.h"

using ParamInt   = Parameter<int32_t>;
using ParamFloat = Parameter<float>;
using ParamText  = Parameter<std::string>;

using ComputedParamInt   = ComputedParameter<int32_t>;
using ComputedParamFloat = ComputedParameter<float>;
using ComputedParamText  = ComputedParameter<std::string>;
#else
// Opaque C handles (resolved to real C++ types in C++ builds).
typedef struct ParamInt   ParamInt;
typedef struct ParamFloat ParamFloat;
typedef struct ParamText  ParamText;

typedef struct ComputedParamInt   ComputedParamInt;
typedef struct ComputedParamFloat ComputedParamFloat;
typedef struct ComputedParamText  ComputedParamText;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// --- Parameter<T> typed get/set ---
// Typed get/set only: route through Parameter<T>::set / SubjectT<T>::get, so
// validators, deferred-write enqueue and observer notifications all apply.
// Creation stays C++-only (validators and storage wiring are C++ constructs).

CPP_UNWANTED("Use modern C++ 'ParamInt::get' instead.")
int32_t     param_i_get(const ParamInt *p);

CPP_UNWANTED("Use modern C++ 'ParamInt::set' instead.")
void        param_i_set(ParamInt *p, int32_t v);

CPP_UNWANTED("Use modern C++ 'ParamFloat::get' instead.")
float       param_f_get(const ParamFloat *p);

CPP_UNWANTED("Use modern C++ 'ParamFloat::set' instead.")
void        param_f_set(ParamFloat *p, float v);

CPP_UNWANTED("Use modern C++ 'ParamText::get' instead.")
char       *param_t_get_into(const ParamText *p, char *dst, size_t n); // dst owns the result

CPP_UNWANTED("Use modern C++ 'ParamText::set' instead.")
void        param_t_set(ParamText *p, const char *v); // NULL -> ""

// --- ComputedParameter<T> set/get ---
// Only set/get are provided: creation stays C++-only (ComputeFn/
// ReverseFn are C++ callables, constructed by SettingsManager).
//   cparam_i_set(cfg.cur.fg_freq(), 7100000);

CPP_UNWANTED("Use modern C++ 'ComputedParamInt::get' instead.")
int32_t cparam_i_get(const ComputedParamInt *p);

CPP_UNWANTED("Use modern C++ 'ComputedParamInt::set' instead.")
void    cparam_i_set(ComputedParamInt *p, int32_t value);

CPP_UNWANTED("Use modern C++ 'ComputedParamFloat::get' instead.")
float cparam_f_get(const ComputedParamFloat *p);

CPP_UNWANTED("Use modern C++ 'ComputedParamFloat::set' instead.")
void  cparam_f_set(ComputedParamFloat *p, float value);

CPP_UNWANTED("Use modern C++ 'ComputedParamText::set' instead.")
void cparam_t_set(ComputedParamText *p, const char *value);

// --- ATU tuner-network cache ---
// The cache lives as a C++ static, exposed to C code as subscribe/read helpers;
// saving a freshly-tuned network wakes the cache.

// Save a freshly-tuned network immediately for the current antenna/frequency
// (replacement for the legacy cfg_atu_save_network). Values are the auto-tuner
// network number for the current front-panel frequency.
int cfg_atu_save_network(uint32_t network);

// Direct value reads (for C code that needs the current value without
// subscribing).
bool     cfg_atu_is_loaded(void);
uint32_t cfg_atu_get_network(void);

// ATU subjects: whether a saved network exists for the current freq/ant and
// its value (0 when not loaded). The returned Observer / ObserverDelayed is
// borrowed from the Subject and its reference is released with
// param_unsubscribe.
Observer        *cfg_atu_network_subscribe(observer_cb cb, void *user_data);


// --- SettingsManager-level operations ---
// Initialisation, deferred-write flushing, band/VFO switching, frequency-step
// cycling and hardware-frequency helpers.

// Single process-wide settings entry point: opens CFG_DB_PATH, applies
// migrations, initialises the DB tables, loads the manager params through
// cfg_api_init() and starts the deferred-save flush thread. Unlike
// cfg_api_init/init_load this is NOT idempotent (it opens the DB and re-inits
// the tables); call once from main() before any settings consumer.
void cfg_init(void);

// Initialise the manager (loads global/band/mode params). Parameter handles
// are accessor functions in this header and need no wiring. The caller owns
// the sqlite3 connection/table init (cfg_api_init is not handed a db handle).
// on_db_error is kept for signature compatibility; it is currently inert.
void cfg_api_init(void (*on_db_error)(const char *));

// Transverter shift for a frequency: the shift of the transverter whose
// [from, to] range contains freq, or 0 when none covers it.
int32_t cfg_transverter_shift_for(int32_t freq);

// True when freq is usable by the hardware: HF 0.5-55 MHz or inside any
// transverter range.
bool cfg_is_valid_hw_freq(int32_t freq);

// Persist all pending deferred writes immediately.
void cfg_api_flush_all(void);

// Start/stop the background deferred-save thread; wakes every ~3 s and calls
// flush_all(). Inert/no-op when the thread is already running / already stopped.
void cfg_api_start_flush_thread(void);
void cfg_api_stop_flush_thread(void);

// --- Band switching ---
// Load the next/previous band above/below current frequency.
void cfg_band_load_next(bool up);

// Copy active VFO (freq, mode, agc, att, pre) to inactive VFO.
void cfg_band_vfo_copy(void);

// --- Step cycling ---
// Cycle freq_step through [10, 100, 500, 1000, 5000] Hz. Returns new step.
int32_t cfg_mode_change_freq_step(bool up);

// --- Parameter access tree ---
// cfg.<group>.<name>() returns the handle of the matching SettingsManager
// parameter, grouped for discoverability. Works identically from C and C++; in
// C++ the returned Parameter<T>* additionally supports ->get()/->set()/
// ->subscribe(). The member name in SettingsManager is shown in the comment.

typedef struct {
    ParamInt *(*auto_level_enabled)(void); /* p_auto_level_enabled */
    ParamFloat *(*auto_level_offset)(void); /* p_auto_level_offset */
    ParamInt *(*knob_info)(void); /* p_knob_info */
    ParamInt *(*spectrum_use_custom_color)(void); /* p_spectrum_use_custom_color */
    ParamInt *(*spectrum_color)(void); /* p_spectrum_color */
    ParamInt *(*spectrum_beta)(void); /* p_spectrum_beta */
    ParamInt *(*spectrum_peak)(void); /* p_spectrum_peak */
    ParamInt *(*spectrum_peak_hold)(void); /* p_spectrum_peak_hold */
    ParamInt *(*spectrum_peak_speed)(void); /* p_spectrum_peak_speed */
    ParamInt *(*spectrum_filled)(void); /* p_spectrum_filled */
    ParamInt *(*spectrum_height)(void); /* p_spectrum_height */

    ParamInt *(*waterfall_center_line)(void); /* p_waterfall_center_line */

    ParamInt *(*show_meter_value)(void); /* p_show_meter_values */

    ParamInt *(*mag_freq)(void); /* p_mag_freq */
    ParamInt *(*mag_info)(void); /* p_mag_info */
    ParamInt *(*mag_alc)(void); /* p_mag_alc */

    ParamInt *(*clock_view)(void); /* p_clock_view */
    ParamInt *(*clock_time_timeout)(void); /* p_clock_time_timeout */
    ParamInt *(*clock_power_timeout)(void); /* p_clock_power_timeout */
    ParamInt *(*clock_tx_timeout)(void); /* p_clock_tx_timeout */

    ParamInt *(*theme)(void); /* p_theme */
    ParamInt *(*meter_color)(void); /* p_meter_color */
    ParamInt *(*ft8_info)(void); /* p_ft8_info */
    ParamInt *(*swr_color)(void); /* p_swr_color */
    ParamInt *(*digi_compact)(void); /* p_digi_text_compact */
} cfg_ui_refs_t;

typedef struct {
    ParamInt *(*invert)(void); /* p_display_invert */
    ParamInt *(*brightness_normal)(void); /* p_brightness_normal */
    ParamInt *(*brightness_idle)(void); /* p_brightness_idle */
    ParamInt *(*brightness_timeout)(void); /* p_brightness_timeout */
    ParamInt *(*brightness_buttons)(void); /* p_brightness_buttons */
} cfg_display_refs_t;

typedef struct {
    ParamInt *(*mode)(void); /* p_voice_mode */
    ParamInt *(*lang)(void); /* p_voice_lang */
    ParamInt *(*rate)(void); /* p_voice_rate */
    ParamInt *(*pitch)(void); /* p_voice_pitch */
    ParamInt *(*volume)(void); /* p_voice_volume */
    ParamInt *(*msg_period)(void); /* p_voice_msg_period */
} cfg_voice_refs_t;

typedef struct {
    ParamFloat *(*play_gain_db)(void); /* p_play_gain_db */
    ParamFloat *(*rec_gain_db)(void); /* p_rec_gain_db */
} cfg_audio_refs_t;

typedef struct {
    ParamInt *(*center)(void); /* p_rtty_center */
    ParamInt *(*shift)(void); /* p_rtty_shift */
    ParamInt *(*rate)(void); /* p_rtty_rate */
    ParamInt *(*reverse)(void); /* p_rtty_reverse */
    ParamInt *(*squelch)(void); /* p_rtty_squelch */
} cfg_rtty_refs_t;

typedef struct {
    ParamInt *(*wifi_enabled)(void); /* p_wifi_enabled */
} cfg_network_refs_t;

typedef struct {
    ParamInt *(*long_gen)(void); /* p_long_gen */
    ParamInt *(*long_app)(void); /* p_long_app */
    ParamInt *(*long_key)(void); /* p_long_key */
    ParamInt *(*long_msg)(void); /* p_long_msg */
    ParamInt *(*long_dfn)(void); /* p_long_dfn */
    ParamInt *(*long_dfl)(void); /* p_long_dfl */
    ParamInt *(*press_f1)(void); /* p_press_f1 */
    ParamInt *(*press_f2)(void); /* p_press_f2 */
    ParamInt *(*long_f1)(void); /* p_long_f1 */
    ParamInt *(*long_f2)(void); /* p_long_f2 */
} cfg_keys_refs_t;

typedef struct {
    ParamInt *(*charger)(void); /* p_charger */
    ParamInt *(*line_in)(void); /* p_line_in */
    ParamInt *(*line_out)(void); /* p_line_out */
    ParamInt *(*spmode)(void); /* p_spmode */
    ParamInt *(*freq_accel)(void); /* p_freq_accel */
} cfg_radio_refs_t;

typedef struct {
    ParamInt *(*tx_freq)(void); /* p_js8_tx_freq */
    ParamInt *(*hold_offset)(void); /* p_js8_hold_offset */
    ParamInt *(*auto_mode)(void); /* p_js8_auto */
    ParamInt *(*hb)(void); /* p_js8_hb */
    ParamInt *(*hb_ack)(void); /* p_js8_hb_ack */
    ParamInt *(*relay)(void); /* p_js8_relay */
    ParamInt *(*st_keep)(void); /* p_js8_st_keep */
    ParamInt *(*msg_keep)(void); /* p_js8_msg_keep */
    ParamInt *(*miles)(void); /* p_js8_miles */
    ParamInt *(*decode_marks)(void); /* p_js8_decode_marks */
    ParamInt *(*wf_avg)(void); /* p_js8_wf_avg */
    ParamInt *(*map_mode)(void); /* p_js8_map_mode */
    ParamInt *(*st_sort)(void); /* p_js8_st_sort */
    ParamInt *(*tsync_auto)(void); /* p_js8_tsync_auto */
    ParamInt *(*hb_interval)(void); /* p_js8_hb_interval */
    ParamInt *(*cq_interval)(void); /* p_js8_cq_interval */
    ParamInt *(*log_prompt)(void); /* p_js8_log_prompt */
    ParamInt *(*log_activation)(void); /* p_js8_log_activation */
    ParamInt *(*alerts)(void); /* p_js8_alerts */
    ParamInt *(*speed)(void); /* p_js8_speed */
    ParamInt *(*rx_all)(void); /* p_js8_rx_all */
    ParamInt *(*ultra)(void); /* p_js8_ultra */
    ParamInt *(*ghostnet)(void); /* p_js8_ghostnet */
    ParamInt *(*custom_on)(void); /* p_js8_custom_on */
    ParamInt *(*custom_hz)(void); /* p_js8_custom_hz */
} cfg_js8_refs_t;

typedef struct {
    ParamText *(*bind)(void); /* p_encoder_bind */
} cfg_encoder_refs_t;

typedef struct {
    ParamInt *(*on)(void); /* p_vox_en */
    ParamInt *(*gain)(void); /* p_vox_gain */
    ParamInt *(*ag)(void); /* p_vox_ag */
    ParamInt *(*delay)(void); /* p_vox_delay */
} cfg_vox_refs_t;

typedef struct {
    ParamInt *(*show_all)(void); /* p_ft8_show_all */
    ParamInt *(*protocol)(void); /* p_ft8_protocol */
    ParamInt *(*auto_mode)(void); /* p_ft8_auto */
    ParamInt *(*hold_freq)(void); /* p_ft8_hold_freq */
    ParamInt *(*max_repeats)(void); /* p_ft8_max_repeats */
    ParamInt *(*tx_freq)(void); /* p_ft8_tx_freq */
    ParamFloat *(*output_gain_offset)(void); /* p_ft8_output_gain_offset */
    ParamText *(*cq_modifier)(void); /* p_ft8_cq_modifier */
} cfg_ft8_refs_t;

typedef struct {
    ParamInt *(*linear)(void); /* p_swrscan_linear */
    ParamInt *(*span)(void); /* p_swrscan_span */
} cfg_swrscan_refs_t;

typedef struct {
    ParamInt *(*key_tone)(void); /* p_key_tone */
    ParamInt *(*key_speed)(void); /* p_key_speed */
    ParamInt *(*key_mode)(void); /* p_key_mode */
    ParamInt *(*iambic_mode)(void); /* p_iambic_mode */
    ParamInt *(*key_vol)(void); /* p_key_vol */
    ParamInt *(*key_train)(void); /* p_key_train */
    ParamInt *(*qsk_time)(void); /* p_qsk_time */
    ParamFloat *(*key_ratio)(void); /* p_key_ratio */
    ParamInt *(*peak_on)(void); /* p_cw_peak_on */
    ParamInt *(*peak_q)(void); /* p_cw_peak_q */
    ParamInt *(*decoder)(void); /* p_cw_decoder */
    ParamInt *(*tune)(void); /* p_cw_tune */
    ParamFloat *(*decoder_snr)(void); /* p_cw_decoder_snr */
    ParamFloat *(*decoder_snr_gist)(void); /* p_cw_decoder_snr_gist */
    ParamInt *(*encoder_period)(void); /* p_cw_encoder_period */
} cfg_cw_refs_t;

typedef struct {
    ParamInt *(*hang)(void); /* p_agc_hang */
    ParamInt *(*knee)(void); /* p_agc_knee */
    ParamInt *(*slope)(void); /* p_agc_slope */
} cfg_agc_refs_t;

typedef struct {
    ParamInt *(*dnf)(void); /* p_dnf */
    ParamInt *(*dnf_center)(void); /* p_dnf_center */
    ParamInt *(*dnf_width)(void); /* p_dnf_width */
    ParamInt *(*dnf_auto)(void); /* p_dnf_auto */
    ParamInt *(*nb)(void); /* p_nb */
    ParamInt *(*nb_level)(void); /* p_nb_level */
    ParamInt *(*nb_width)(void); /* p_nb_width */
    ParamInt *(*nr)(void); /* p_nr */
    ParamInt *(*nr_level)(void); /* p_nr_level */
    ParamFloat *(*output_gain)(void); /* p_output_gain */
    ParamInt *(*comp)(void); /* p_comp */
    ParamFloat *(*comp_threshold_offset)(void); /* p_comp_threshold_offset */
    ParamFloat *(*comp_makeup_offset)(void); /* p_comp_makeup_offset */
    ParamInt *(*fm_emphasis)(void); /* p_fm_emphasis */
    ParamInt *(*tx_filter_low)(void); /* p_tx_filter_low */
    ParamInt *(*tx_filter_high)(void); /* p_tx_filter_high */
    ParamInt *(*cessb_on)(void); /* p_cessb_on */
    ParamFloat *(*cessb_power_up)(void); /* p_cessb_power_up */
} cfg_dsp_refs_t;

typedef struct {
    ParamInt *(*current_vfo)(void); /* p_band_current_vfo */
    ParamInt *(*if_shift)(void); /* p_band_if_shift */
    ParamInt *(*vfoa_freq)(void); /* p_band_vfoa_freq */
    ParamInt *(*vfob_freq)(void); /* p_band_vfob_freq */
    ParamFloat *(*dac_offset)(void); /* p_band_dac_offset */
    ParamInt *(*grid_min)(void); /* p_band_grid_min */
    ParamInt *(*grid_max)(void); /* p_band_grid_max */
    ParamInt *(*split)(void); /* p_band_split */
    ParamInt *(*tx_i_offset)(void); /* p_band_tx_i_offset */
    ParamInt *(*tx_q_offset)(void); /* p_band_tx_q_offset */
    ParamInt *(*vfoa_mode)(void); /* p_band_vfoa_mode */
    ParamInt *(*vfob_mode)(void); /* p_band_vfob_mode */
    ParamInt *(*vfoa_att)(void); /* p_band_vfoa_att */
    ParamInt *(*vfob_att)(void); /* p_band_vfob_att */
    ParamInt *(*vfoa_pre)(void); /* p_band_vfoa_pre */
    ParamInt *(*vfob_pre)(void); /* p_band_vfob_pre */
    ParamInt *(*vfoa_agc)(void); /* p_band_vfoa_agc */
    ParamInt *(*vfob_agc)(void); /* p_band_vfob_agc */
} cfg_band_refs_t;

typedef struct {
    ParamInt *(*zoom)(void); /* p_mode_zoom */
    ParamInt *(*freq_step)(void); /* p_mode_freq_step */
} cfg_mode_refs_t;

typedef struct {
    ComputedParamInt *(*low)(void); /* cp_cur_filter_low */
    ComputedParamInt *(*high)(void); /* cp_cur_filter_high */
    ComputedParamInt *(*bw)(void); /* cp_cur_filter_bw */
} cfg_filter_refs_t;

typedef struct {
    ComputedParamInt *(*fg_freq)(void); /* cp_fg_freq */
    ComputedParamInt *(*bg_freq)(void); /* cp_bg_freq */
    ComputedParamInt *(*mode)(void); /* cp_cur_mode */
    ComputedParamInt *(*agc)(void); /* cp_cur_agc */
    ComputedParamInt *(*att)(void); /* cp_cur_att */
    ComputedParamInt *(*pre)(void); /* cp_cur_pre */
    ComputedParamInt *(*mode_lo_offset)(void); /* cp_mode_lo_offset */
} cfg_computed_refs_t;

typedef struct {
    ParamInt *(*t0_from)(void); /* p_transverter_0_from */
    ParamInt *(*t0_to)(void); /* p_transverter_0_to */
    ParamInt *(*t0_shift)(void); /* p_transverter_0_shift */
    ParamInt *(*t1_from)(void); /* p_transverter_1_from */
    ParamInt *(*t1_to)(void); /* p_transverter_1_to */
    ParamInt *(*t1_shift)(void); /* p_transverter_1_shift */
} cfg_transverter_refs_t;

typedef struct {
    ParamInt *(*volume)(void); /* p_volume */
    ParamInt *(*squelch)(void); /* p_squelch */
    ParamInt *(*rfgain)(void); /* p_rfgain */
    ParamInt *(*rit)(void); /* p_rit */
    ParamInt *(*xit)(void); /* p_xit */
    ParamFloat *(*pwr)(void); /* p_pwr */
    ParamInt *(*band_id)(void); /* p_band_id */
    ParamInt *(*mic)(void); /* p_mic */
    ParamInt *(*hmic)(void); /* p_hmic */
    ParamInt *(*imic)(void); /* p_imic */
    ParamInt *(*moni)(void); /* p_moni */
    ParamInt *(*ant_id)(void); /* p_ant_id */
    ParamInt *(*atu_enabled)(void); /* p_atu_enabled */
    ParamInt *(*cat_baud)(void); /* p_cat_baud */

    ParamText *(*qth)(void); /* p_qth */
    ParamText *(*callsign)(void); /* p_callsign */


    cfg_ui_refs_t ui;
    cfg_encoder_refs_t encoder;
    cfg_vox_refs_t vox;
    cfg_ft8_refs_t ft8;
    cfg_swrscan_refs_t swrscan;
    cfg_cw_refs_t cw;
    cfg_agc_refs_t agc;
    cfg_dsp_refs_t dsp;
    cfg_band_refs_t band;
    cfg_mode_refs_t mode;
    cfg_filter_refs_t filter;
    cfg_computed_refs_t cur;
    cfg_transverter_refs_t transverter;
    cfg_display_refs_t display;
    cfg_voice_refs_t voice;
    cfg_audio_refs_t audio;
    cfg_rtty_refs_t rtty;
    cfg_network_refs_t network;
    cfg_keys_refs_t keys;
    cfg_radio_refs_t radio;
    cfg_js8_refs_t js8;
} cfg_refs_t;

extern const cfg_refs_t cfg;

#ifdef __cplusplus
} // extern "C"
#endif
