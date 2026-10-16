#include "cfg_api.h"

#include <type_traits>

#include "atu.h"
#include "settings_internal.h"

// Owned by C++ for the whole program; never deleted.
SettingsManager cfg_sm;

// Active instance behind the accessors (test seam via cfg_set_instance).
static SettingsManager *g_cfg = &cfg_sm;

SettingsManager &cfg_instance() { return *g_cfg; }

void cfg_set_instance(SettingsManager *sm) { g_cfg = sm ? sm : &cfg_sm; }

namespace {
// Enforces at compile time that a SettingsManager member's concrete type
// matches the handle type the C/C++ API promises. Replaces the old unchecked
// reinterpret_cast<ParamFloat *> casts, which silently changed set() semantics
// when DbType/Scale differed.
template <typename Expected, typename Actual>
Expected *param_ref(Actual &p) {
    static_assert(std::is_same_v<std::decay_t<Actual>, std::decay_t<Expected>>,
                  "cfg accessor: SettingsManager parameter type does not match the handle type");
    return &p;
}
} // namespace

// --- Parameter handle accessors ---
// One function per C-reachable parameter. Each returns the address of the
// corresponding SettingsManager member; the parameters are static and live for
// the whole program, so the pointer is always valid (no init-order window).
// Grouped by storage scope, matching cfg_api.h.

// --- GLOBAL params (flat `params` table) ---
static ParamInt *cfg_volume(void) { return param_ref<ParamInt>(cfg_instance().p_volume); }
static ParamInt *cfg_squelch(void) { return param_ref<ParamInt>(cfg_instance().p_squelch); }
static ParamInt *cfg_rfgain(void) { return param_ref<ParamInt>(cfg_instance().p_rfgain); }
static ParamInt *cfg_rit(void) { return param_ref<ParamInt>(cfg_instance().p_rit); }
static ParamInt *cfg_xit(void) { return param_ref<ParamInt>(cfg_instance().p_xit); }
static ParamFloat *cfg_pwr(void) { return param_ref<ParamFloat>(cfg_instance().p_pwr); }
static ParamInt *cfg_band_id(void) { return param_ref<ParamInt>(cfg_instance().p_band_id); }
static ParamInt *cfg_mic(void) { return param_ref<ParamInt>(cfg_instance().p_mic); }
static ParamInt *cfg_hmic(void) { return param_ref<ParamInt>(cfg_instance().p_hmic); }
static ParamInt *cfg_imic(void) { return param_ref<ParamInt>(cfg_instance().p_imic); }
static ParamInt *cfg_moni(void) { return param_ref<ParamInt>(cfg_instance().p_moni); }
static ParamInt *cfg_ant_id(void) { return param_ref<ParamInt>(cfg_instance().p_ant_id); }
static ParamInt *cfg_atu_enabled(void) { return param_ref<ParamInt>(cfg_instance().p_atu_enabled); }
static ParamInt *cfg_cat_baud(void) { return param_ref<ParamInt>(cfg_instance().p_cat_baud); }
static ParamInt *cfg_show_meter_values(void) { return param_ref<ParamInt>(cfg_instance().p_show_meter_values); }
static ParamInt *cfg_display_invert(void) { return param_ref<ParamInt>(cfg_instance().p_display_invert); }
static ParamInt *cfg_auto_level_enabled(void) { return param_ref<ParamInt>(cfg_instance().p_auto_level_enabled); }
static ParamFloat *cfg_auto_level_offset(void) { return param_ref<ParamFloat>(cfg_instance().p_auto_level_offset); }
static ParamInt *cfg_knob_info(void) { return param_ref<ParamInt>(cfg_instance().p_knob_info); }
static ParamInt *cfg_spectrum_use_custom_color(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_use_custom_color); }
static ParamInt *cfg_spectrum_color(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_color); }
static ParamText *cfg_encoder_bind(void) { return param_ref<ParamText>(cfg_instance().p_encoder_bind); }
static ParamInt *cfg_vox_on(void) { return param_ref<ParamInt>(cfg_instance().p_vox_en); }
static ParamInt *cfg_vox_gain(void) { return param_ref<ParamInt>(cfg_instance().p_vox_gain); }
static ParamInt *cfg_vox_ag(void) { return param_ref<ParamInt>(cfg_instance().p_vox_ag); }
static ParamInt *cfg_vox_delay(void) { return param_ref<ParamInt>(cfg_instance().p_vox_delay); }
static ParamInt *cfg_ft8_show_all(void) { return param_ref<ParamInt>(cfg_instance().p_ft8_show_all); }
static ParamInt *cfg_ft8_protocol(void) { return param_ref<ParamInt>(cfg_instance().p_ft8_protocol); }
static ParamInt *cfg_ft8_auto(void) { return param_ref<ParamInt>(cfg_instance().p_ft8_auto); }
static ParamInt *cfg_ft8_hold_freq(void) { return param_ref<ParamInt>(cfg_instance().p_ft8_hold_freq); }
static ParamInt *cfg_ft8_max_repeats(void) { return param_ref<ParamInt>(cfg_instance().p_ft8_max_repeats); }
static ParamInt *cfg_swrscan_linear(void) { return param_ref<ParamInt>(cfg_instance().p_swrscan_linear); }
static ParamInt *cfg_swrscan_span(void) { return param_ref<ParamInt>(cfg_instance().p_swrscan_span); }
static ParamInt *cfg_key_tone(void) { return param_ref<ParamInt>(cfg_instance().p_key_tone); }
static ParamInt *cfg_key_speed(void) { return param_ref<ParamInt>(cfg_instance().p_key_speed); }
static ParamInt *cfg_key_mode(void) { return param_ref<ParamInt>(cfg_instance().p_key_mode); }
static ParamInt *cfg_iambic_mode(void) { return param_ref<ParamInt>(cfg_instance().p_iambic_mode); }
static ParamInt *cfg_key_vol(void) { return param_ref<ParamInt>(cfg_instance().p_key_vol); }
static ParamInt *cfg_key_train(void) { return param_ref<ParamInt>(cfg_instance().p_key_train); }
static ParamInt *cfg_qsk_time(void) { return param_ref<ParamInt>(cfg_instance().p_qsk_time); }
static ParamFloat *cfg_key_ratio(void) { return param_ref<ParamFloat>(cfg_instance().p_key_ratio); }
static ParamInt *cfg_cw_peak_on(void) { return param_ref<ParamInt>(cfg_instance().p_cw_peak_on); }
static ParamInt *cfg_cw_peak_q(void) { return param_ref<ParamInt>(cfg_instance().p_cw_peak_q); }
static ParamInt *cfg_cw_decoder(void) { return param_ref<ParamInt>(cfg_instance().p_cw_decoder); }
static ParamInt *cfg_cw_tune(void) { return param_ref<ParamInt>(cfg_instance().p_cw_tune); }
static ParamFloat *cfg_cw_decoder_snr(void) { return param_ref<ParamFloat>(cfg_instance().p_cw_decoder_snr); }
static ParamFloat *cfg_cw_decoder_snr_gist(void) { return param_ref<ParamFloat>(cfg_instance().p_cw_decoder_snr_gist); }
static ParamInt *cfg_agc_hang(void) { return param_ref<ParamInt>(cfg_instance().p_agc_hang); }
static ParamInt *cfg_agc_knee(void) { return param_ref<ParamInt>(cfg_instance().p_agc_knee); }
static ParamInt *cfg_agc_slope(void) { return param_ref<ParamInt>(cfg_instance().p_agc_slope); }
static ParamInt *cfg_dnf(void) { return param_ref<ParamInt>(cfg_instance().p_dnf); }
static ParamInt *cfg_dnf_center(void) { return param_ref<ParamInt>(cfg_instance().p_dnf_center); }
static ParamInt *cfg_dnf_width(void) { return param_ref<ParamInt>(cfg_instance().p_dnf_width); }
static ParamInt *cfg_dnf_auto(void) { return param_ref<ParamInt>(cfg_instance().p_dnf_auto); }
static ParamInt *cfg_nb(void) { return param_ref<ParamInt>(cfg_instance().p_nb); }
static ParamInt *cfg_nb_level(void) { return param_ref<ParamInt>(cfg_instance().p_nb_level); }
static ParamInt *cfg_nb_width(void) { return param_ref<ParamInt>(cfg_instance().p_nb_width); }
static ParamInt *cfg_nr(void) { return param_ref<ParamInt>(cfg_instance().p_nr); }
static ParamInt *cfg_nr_level(void) { return param_ref<ParamInt>(cfg_instance().p_nr_level); }
static ParamFloat *cfg_output_gain(void) { return param_ref<ParamFloat>(cfg_instance().p_output_gain); }
static ParamInt *cfg_comp(void) { return param_ref<ParamInt>(cfg_instance().p_comp); }
static ParamFloat *cfg_comp_threshold_offset(void) { return param_ref<ParamFloat>(cfg_instance().p_comp_threshold_offset); }
static ParamFloat *cfg_comp_makeup_offset(void) { return param_ref<ParamFloat>(cfg_instance().p_comp_makeup_offset); }
static ParamInt *cfg_fm_emphasis(void) { return param_ref<ParamInt>(cfg_instance().p_fm_emphasis); }
static ParamInt *cfg_tx_filter_low(void) { return param_ref<ParamInt>(cfg_instance().p_tx_filter_low); }
static ParamInt *cfg_tx_filter_high(void) { return param_ref<ParamInt>(cfg_instance().p_tx_filter_high); }
static ParamInt *cfg_cessb_on(void) { return param_ref<ParamInt>(cfg_instance().p_cessb_on); }
static ParamFloat *cfg_cessb_power_up(void) { return param_ref<ParamFloat>(cfg_instance().p_cessb_power_up); }

// --- BAND params ---
static ParamInt *cfg_band_current_vfo(void) { return param_ref<ParamInt>(cfg_instance().p_band_current_vfo); }
static ParamInt *cfg_band_if_shift(void) { return param_ref<ParamInt>(cfg_instance().p_band_if_shift); }
static ParamInt *cfg_band_vfoa_freq(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfoa_freq); }
static ParamInt *cfg_band_vfob_freq(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfob_freq); }
static ParamFloat *cfg_band_dac_offset(void) { return param_ref<ParamFloat>(cfg_instance().p_band_dac_offset); }
static ParamInt *cfg_band_grid_min(void) { return param_ref<ParamInt>(cfg_instance().p_band_grid_min); }
static ParamInt *cfg_band_grid_max(void) { return param_ref<ParamInt>(cfg_instance().p_band_grid_max); }
static ParamInt *cfg_band_split(void) { return param_ref<ParamInt>(cfg_instance().p_band_split); }
static ParamInt *cfg_band_tx_i_offset(void) { return param_ref<ParamInt>(cfg_instance().p_band_tx_i_offset); }
static ParamInt *cfg_band_tx_q_offset(void) { return param_ref<ParamInt>(cfg_instance().p_band_tx_q_offset); }
static ParamInt *cfg_band_vfoa_mode(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfoa_mode); }
static ParamInt *cfg_band_vfob_mode(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfob_mode); }
static ParamInt *cfg_band_vfoa_att(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfoa_att); }
static ParamInt *cfg_band_vfob_att(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfob_att); }
static ParamInt *cfg_band_vfoa_pre(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfoa_pre); }
static ParamInt *cfg_band_vfob_pre(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfob_pre); }
static ParamInt *cfg_band_vfoa_agc(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfoa_agc); }
static ParamInt *cfg_band_vfob_agc(void) { return param_ref<ParamInt>(cfg_instance().p_band_vfob_agc); }

// --- MODE params ---
static ParamInt *cfg_mode_zoom(void) { return param_ref<ParamInt>(cfg_instance().p_mode_zoom); }
static ParamInt *cfg_mode_freq_step(void) { return param_ref<ParamInt>(cfg_instance().p_mode_freq_step); }

// --- Transverter params (OTHER storage, fixed HW conversion) ---
static ParamInt *cfg_transverter_0_from(void) { return param_ref<ParamInt>(cfg_instance().p_transverter_0_from); }
static ParamInt *cfg_transverter_0_to(void) { return param_ref<ParamInt>(cfg_instance().p_transverter_0_to); }
static ParamInt *cfg_transverter_0_shift(void) { return param_ref<ParamInt>(cfg_instance().p_transverter_0_shift); }
static ParamInt *cfg_transverter_1_from(void) { return param_ref<ParamInt>(cfg_instance().p_transverter_1_from); }
static ParamInt *cfg_transverter_1_to(void) { return param_ref<ParamInt>(cfg_instance().p_transverter_1_to); }
static ParamInt *cfg_transverter_1_shift(void) { return param_ref<ParamInt>(cfg_instance().p_transverter_1_shift); }

// --- Stage 8.2: legacy params migrated to cfg ---
static ParamInt *cfg_brightness_normal(void) { return param_ref<ParamInt>(cfg_instance().p_brightness_normal); }
static ParamInt *cfg_brightness_idle(void) { return param_ref<ParamInt>(cfg_instance().p_brightness_idle); }
static ParamInt *cfg_brightness_timeout(void) { return param_ref<ParamInt>(cfg_instance().p_brightness_timeout); }
static ParamInt *cfg_brightness_buttons(void) { return param_ref<ParamInt>(cfg_instance().p_brightness_buttons); }
static ParamInt *cfg_clock_view(void) { return param_ref<ParamInt>(cfg_instance().p_clock_view); }
static ParamInt *cfg_clock_time_timeout(void) { return param_ref<ParamInt>(cfg_instance().p_clock_time_timeout); }
static ParamInt *cfg_clock_power_timeout(void) { return param_ref<ParamInt>(cfg_instance().p_clock_power_timeout); }
static ParamInt *cfg_clock_tx_timeout(void) { return param_ref<ParamInt>(cfg_instance().p_clock_tx_timeout); }
static ParamInt *cfg_spectrum_beta(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_beta); }
static ParamInt *cfg_spectrum_peak(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_peak); }
static ParamInt *cfg_spectrum_peak_hold(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_peak_hold); }
static ParamInt *cfg_spectrum_peak_speed(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_peak_speed); }
static ParamInt *cfg_spectrum_filled(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_filled); }
static ParamInt *cfg_spectrum_height(void) { return param_ref<ParamInt>(cfg_instance().p_spectrum_height); }
static ParamInt *cfg_waterfall_center_line(void) { return param_ref<ParamInt>(cfg_instance().p_waterfall_center_line); }
static ParamInt *cfg_mag_freq(void) { return param_ref<ParamInt>(cfg_instance().p_mag_freq); }
static ParamInt *cfg_mag_info(void) { return param_ref<ParamInt>(cfg_instance().p_mag_info); }
static ParamInt *cfg_mag_alc(void) { return param_ref<ParamInt>(cfg_instance().p_mag_alc); }
static ParamInt *cfg_voice_mode(void) { return param_ref<ParamInt>(cfg_instance().p_voice_mode); }
static ParamInt *cfg_voice_lang(void) { return param_ref<ParamInt>(cfg_instance().p_voice_lang); }
static ParamInt *cfg_voice_rate(void) { return param_ref<ParamInt>(cfg_instance().p_voice_rate); }
static ParamInt *cfg_voice_pitch(void) { return param_ref<ParamInt>(cfg_instance().p_voice_pitch); }
static ParamInt *cfg_voice_volume(void) { return param_ref<ParamInt>(cfg_instance().p_voice_volume); }
static ParamInt *cfg_voice_msg_period(void) { return param_ref<ParamInt>(cfg_instance().p_voice_msg_period); }
static ParamFloat *cfg_play_gain_db(void) { return param_ref<ParamFloat>(cfg_instance().p_play_gain_db); }
static ParamFloat *cfg_rec_gain_db(void) { return param_ref<ParamFloat>(cfg_instance().p_rec_gain_db); }
static ParamInt *cfg_rtty_center(void) { return param_ref<ParamInt>(cfg_instance().p_rtty_center); }
static ParamInt *cfg_rtty_shift(void) { return param_ref<ParamInt>(cfg_instance().p_rtty_shift); }
static ParamInt *cfg_rtty_rate(void) { return param_ref<ParamInt>(cfg_instance().p_rtty_rate); }
static ParamInt *cfg_rtty_reverse(void) { return param_ref<ParamInt>(cfg_instance().p_rtty_reverse); }
static ParamInt *cfg_rtty_squelch(void) { return param_ref<ParamInt>(cfg_instance().p_rtty_squelch); }
static ParamInt *cfg_cw_encoder_period(void) { return param_ref<ParamInt>(cfg_instance().p_cw_encoder_period); }
static ParamInt *cfg_ft8_tx_freq(void) { return param_ref<ParamInt>(cfg_instance().p_ft8_tx_freq); }
static ParamFloat *cfg_ft8_output_gain_offset(void) { return param_ref<ParamFloat>(cfg_instance().p_ft8_output_gain_offset); }
static ParamText *cfg_ft8_cq_modifier(void) { return param_ref<ParamText>(cfg_instance().p_ft8_cq_modifier); }
static ParamText *cfg_qth(void) { return param_ref<ParamText>(cfg_instance().p_qth); }
static ParamText *cfg_callsign(void) { return param_ref<ParamText>(cfg_instance().p_callsign); }
static ParamInt *cfg_wifi_enabled(void) { return param_ref<ParamInt>(cfg_instance().p_wifi_enabled); }
static ParamInt *cfg_long_gen(void) { return param_ref<ParamInt>(cfg_instance().p_long_gen); }
static ParamInt *cfg_long_app(void) { return param_ref<ParamInt>(cfg_instance().p_long_app); }
static ParamInt *cfg_long_key(void) { return param_ref<ParamInt>(cfg_instance().p_long_key); }
static ParamInt *cfg_long_msg(void) { return param_ref<ParamInt>(cfg_instance().p_long_msg); }
static ParamInt *cfg_long_dfn(void) { return param_ref<ParamInt>(cfg_instance().p_long_dfn); }
static ParamInt *cfg_long_dfl(void) { return param_ref<ParamInt>(cfg_instance().p_long_dfl); }
static ParamInt *cfg_press_f1(void) { return param_ref<ParamInt>(cfg_instance().p_press_f1); }
static ParamInt *cfg_press_f2(void) { return param_ref<ParamInt>(cfg_instance().p_press_f2); }
static ParamInt *cfg_long_f1(void) { return param_ref<ParamInt>(cfg_instance().p_long_f1); }
static ParamInt *cfg_long_f2(void) { return param_ref<ParamInt>(cfg_instance().p_long_f2); }
static ParamInt *cfg_charger(void) { return param_ref<ParamInt>(cfg_instance().p_charger); }
static ParamInt *cfg_line_in(void) { return param_ref<ParamInt>(cfg_instance().p_line_in); }
static ParamInt *cfg_line_out(void) { return param_ref<ParamInt>(cfg_instance().p_line_out); }
static ParamInt *cfg_spmode(void) { return param_ref<ParamInt>(cfg_instance().p_spmode); }
static ParamInt *cfg_freq_accel(void) { return param_ref<ParamInt>(cfg_instance().p_freq_accel); }

static ParamInt *cfg_js8_tx_freq(void) { return param_ref<ParamInt>(cfg_instance().p_js8_tx_freq); }
static ParamInt *cfg_js8_hold_offset(void) { return param_ref<ParamInt>(cfg_instance().p_js8_hold_offset); }
static ParamInt *cfg_js8_auto_mode(void) { return param_ref<ParamInt>(cfg_instance().p_js8_auto); }
static ParamInt *cfg_js8_hb(void) { return param_ref<ParamInt>(cfg_instance().p_js8_hb); }
static ParamInt *cfg_js8_hb_ack(void) { return param_ref<ParamInt>(cfg_instance().p_js8_hb_ack); }
static ParamInt *cfg_js8_relay(void) { return param_ref<ParamInt>(cfg_instance().p_js8_relay); }
static ParamInt *cfg_js8_st_keep(void) { return param_ref<ParamInt>(cfg_instance().p_js8_st_keep); }
static ParamInt *cfg_js8_msg_keep(void) { return param_ref<ParamInt>(cfg_instance().p_js8_msg_keep); }
static ParamInt *cfg_js8_miles(void) { return param_ref<ParamInt>(cfg_instance().p_js8_miles); }
static ParamInt *cfg_js8_decode_marks(void) { return param_ref<ParamInt>(cfg_instance().p_js8_decode_marks); }
static ParamInt *cfg_js8_wf_avg(void) { return param_ref<ParamInt>(cfg_instance().p_js8_wf_avg); }
static ParamInt *cfg_js8_map_mode(void) { return param_ref<ParamInt>(cfg_instance().p_js8_map_mode); }
static ParamInt *cfg_js8_st_sort(void) { return param_ref<ParamInt>(cfg_instance().p_js8_st_sort); }
static ParamInt *cfg_js8_tsync_auto(void) { return param_ref<ParamInt>(cfg_instance().p_js8_tsync_auto); }
static ParamInt *cfg_js8_hb_interval(void) { return param_ref<ParamInt>(cfg_instance().p_js8_hb_interval); }
static ParamInt *cfg_js8_cq_interval(void) { return param_ref<ParamInt>(cfg_instance().p_js8_cq_interval); }
static ParamInt *cfg_js8_log_prompt(void) { return param_ref<ParamInt>(cfg_instance().p_js8_log_prompt); }
static ParamInt *cfg_js8_log_activation(void) { return param_ref<ParamInt>(cfg_instance().p_js8_log_activation); }
static ParamInt *cfg_js8_alerts(void) { return param_ref<ParamInt>(cfg_instance().p_js8_alerts); }
static ParamInt *cfg_js8_speed(void) { return param_ref<ParamInt>(cfg_instance().p_js8_speed); }
static ParamInt *cfg_js8_rx_all(void) { return param_ref<ParamInt>(cfg_instance().p_js8_rx_all); }
static ParamInt *cfg_js8_ultra(void) { return param_ref<ParamInt>(cfg_instance().p_js8_ultra); }
static ParamInt *cfg_js8_ghostnet(void) { return param_ref<ParamInt>(cfg_instance().p_js8_ghostnet); }
static ParamInt *cfg_js8_custom_on(void) { return param_ref<ParamInt>(cfg_instance().p_js8_custom_on); }
static ParamInt *cfg_js8_custom_hz(void) { return param_ref<ParamInt>(cfg_instance().p_js8_custom_hz); }
static ParamInt *cfg_theme(void) { return param_ref<ParamInt>(cfg_instance().p_theme); }
static ParamInt *cfg_meter_color(void) { return param_ref<ParamInt>(cfg_instance().p_meter_color); }
static ParamInt *cfg_ft8_info(void) { return param_ref<ParamInt>(cfg_instance().p_ft8_info); }
static ParamInt *cfg_swr_color(void) { return param_ref<ParamInt>(cfg_instance().p_swr_color); }
static ParamInt *cfg_digi_compact(void) { return param_ref<ParamInt>(cfg_instance().p_digi_text_compact); }

// --- Computed params (current operating state, not persisted) ---
static ComputedParamInt *cfg_fg_freq(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_fg_freq); }
static ComputedParamInt *cfg_cur_mode(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_cur_mode); }
static ComputedParamInt *cfg_cur_agc(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_cur_agc); }
static ComputedParamInt *cfg_cur_att(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_cur_att); }
static ComputedParamInt *cfg_cur_pre(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_cur_pre); }
static ComputedParamInt *cfg_bg_freq(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_bg_freq); }
static ComputedParamInt *cfg_cur_filter_low(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_cur_filter_low); }
static ComputedParamInt *cfg_cur_filter_high(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_cur_filter_high); }
static ComputedParamInt *cfg_cur_filter_bw(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_cur_filter_bw); }
static ComputedParamInt *cfg_mode_lo_offset(void) { return param_ref<ComputedParamInt>(cfg_instance().cp_mode_lo_offset); }

extern "C" const cfg_refs_t cfg = {
    .volume = &cfg_volume,
    .squelch = &cfg_squelch,
    .rfgain = &cfg_rfgain,
    .rit = &cfg_rit,
    .xit = &cfg_xit,
    .pwr = &cfg_pwr,
    .band_id = &cfg_band_id,
    .mic = &cfg_mic,
    .hmic = &cfg_hmic,
    .imic = &cfg_imic,
    .moni = &cfg_moni,
    .ant_id = &cfg_ant_id,
    .atu_enabled = &cfg_atu_enabled,
    .cat_baud = &cfg_cat_baud,

    .qth = &cfg_qth,
    .callsign = &cfg_callsign,

    .ui = {
        .auto_level_enabled = &cfg_auto_level_enabled,
        .auto_level_offset = &cfg_auto_level_offset,
        .knob_info = &cfg_knob_info,
        .spectrum_use_custom_color = &cfg_spectrum_use_custom_color,
        .spectrum_color = &cfg_spectrum_color,
        .spectrum_beta = &cfg_spectrum_beta,
        .spectrum_peak = &cfg_spectrum_peak,
        .spectrum_peak_hold = &cfg_spectrum_peak_hold,
        .spectrum_peak_speed = &cfg_spectrum_peak_speed,
        .spectrum_filled = &cfg_spectrum_filled,
        .spectrum_height = &cfg_spectrum_height,

        .waterfall_center_line = &cfg_waterfall_center_line,

        .show_meter_value = &cfg_show_meter_values,

        .mag_freq = &cfg_mag_freq,
        .mag_info = &cfg_mag_info,
        .mag_alc = &cfg_mag_alc,

        .clock_view = &cfg_clock_view,
        .clock_time_timeout = &cfg_clock_time_timeout,
        .clock_power_timeout = &cfg_clock_power_timeout,
        .clock_tx_timeout = &cfg_clock_tx_timeout,

        .theme = &cfg_theme,
        .meter_color = &cfg_meter_color,
        .ft8_info = &cfg_ft8_info,
        .swr_color = &cfg_swr_color,
        .digi_compact = &cfg_digi_compact,
    },
    .encoder = {
        .bind = &cfg_encoder_bind,
    },
    .vox = {
        .on = &cfg_vox_on,
        .gain = &cfg_vox_gain,
        .ag = &cfg_vox_ag,
        .delay = &cfg_vox_delay,
    },
    .ft8 = {
        .show_all = &cfg_ft8_show_all,
        .protocol = &cfg_ft8_protocol,
        .auto_mode = &cfg_ft8_auto,
        .hold_freq = &cfg_ft8_hold_freq,
        .max_repeats = &cfg_ft8_max_repeats,
        .tx_freq = &cfg_ft8_tx_freq,
        .output_gain_offset = &cfg_ft8_output_gain_offset,
        .cq_modifier = &cfg_ft8_cq_modifier,
    },
    .swrscan = {
        .linear = &cfg_swrscan_linear,
        .span = &cfg_swrscan_span,
    },
    .cw = {
        .key_tone = &cfg_key_tone,
        .key_speed = &cfg_key_speed,
        .key_mode = &cfg_key_mode,
        .iambic_mode = &cfg_iambic_mode,
        .key_vol = &cfg_key_vol,
        .key_train = &cfg_key_train,
        .qsk_time = &cfg_qsk_time,
        .key_ratio = &cfg_key_ratio,
        .peak_on = &cfg_cw_peak_on,
        .peak_q = &cfg_cw_peak_q,
        .decoder = &cfg_cw_decoder,
        .tune = &cfg_cw_tune,
        .decoder_snr = &cfg_cw_decoder_snr,
        .decoder_snr_gist = &cfg_cw_decoder_snr_gist,
        .encoder_period = &cfg_cw_encoder_period,
    },
    .agc = {
        .hang = &cfg_agc_hang,
        .knee = &cfg_agc_knee,
        .slope = &cfg_agc_slope,
    },
    .dsp = {
        .dnf = &cfg_dnf,
        .dnf_center = &cfg_dnf_center,
        .dnf_width = &cfg_dnf_width,
        .dnf_auto = &cfg_dnf_auto,
        .nb = &cfg_nb,
        .nb_level = &cfg_nb_level,
        .nb_width = &cfg_nb_width,
        .nr = &cfg_nr,
        .nr_level = &cfg_nr_level,
        .output_gain = &cfg_output_gain,
        .comp = &cfg_comp,
        .comp_threshold_offset = &cfg_comp_threshold_offset,
        .comp_makeup_offset = &cfg_comp_makeup_offset,
        .fm_emphasis = &cfg_fm_emphasis,
        .tx_filter_low = &cfg_tx_filter_low,
        .tx_filter_high = &cfg_tx_filter_high,
        .cessb_on = &cfg_cessb_on,
        .cessb_power_up = &cfg_cessb_power_up,
    },
    .band = {
        .current_vfo = &cfg_band_current_vfo,
        .if_shift = &cfg_band_if_shift,
        .vfoa_freq = &cfg_band_vfoa_freq,
        .vfob_freq = &cfg_band_vfob_freq,
        .dac_offset = &cfg_band_dac_offset,
        .grid_min = &cfg_band_grid_min,
        .grid_max = &cfg_band_grid_max,
        .split = &cfg_band_split,
        .tx_i_offset = &cfg_band_tx_i_offset,
        .tx_q_offset = &cfg_band_tx_q_offset,
        .vfoa_mode = &cfg_band_vfoa_mode,
        .vfob_mode = &cfg_band_vfob_mode,
        .vfoa_att = &cfg_band_vfoa_att,
        .vfob_att = &cfg_band_vfob_att,
        .vfoa_pre = &cfg_band_vfoa_pre,
        .vfob_pre = &cfg_band_vfob_pre,
        .vfoa_agc = &cfg_band_vfoa_agc,
        .vfob_agc = &cfg_band_vfob_agc,
    },
    .mode = {
        .zoom = &cfg_mode_zoom,
        .freq_step = &cfg_mode_freq_step,
    },
    .filter = {
        .low = &cfg_cur_filter_low,
        .high = &cfg_cur_filter_high,
        .bw = &cfg_cur_filter_bw,
    },
    .cur = {
        .fg_freq = &cfg_fg_freq,
        .bg_freq = &cfg_bg_freq,
        .mode = &cfg_cur_mode,
        .agc = &cfg_cur_agc,
        .att = &cfg_cur_att,
        .pre = &cfg_cur_pre,
        .mode_lo_offset = &cfg_mode_lo_offset,
    },
    .transverter = {
        .t0_from = &cfg_transverter_0_from,
        .t0_to = &cfg_transverter_0_to,
        .t0_shift = &cfg_transverter_0_shift,
        .t1_from = &cfg_transverter_1_from,
        .t1_to = &cfg_transverter_1_to,
        .t1_shift = &cfg_transverter_1_shift,
    },
    .display = {
        .invert = &cfg_display_invert,
        .brightness_normal = &cfg_brightness_normal,
        .brightness_idle = &cfg_brightness_idle,
        .brightness_timeout = &cfg_brightness_timeout,
        .brightness_buttons = &cfg_brightness_buttons,
    },
    .voice = {
        .mode = &cfg_voice_mode,
        .lang = &cfg_voice_lang,
        .rate = &cfg_voice_rate,
        .pitch = &cfg_voice_pitch,
        .volume = &cfg_voice_volume,
        .msg_period = &cfg_voice_msg_period,
    },
    .audio = {
        .play_gain_db = &cfg_play_gain_db,
        .rec_gain_db = &cfg_rec_gain_db,
    },
    .rtty = {
        .center = &cfg_rtty_center,
        .shift = &cfg_rtty_shift,
        .rate = &cfg_rtty_rate,
        .reverse = &cfg_rtty_reverse,
        .squelch = &cfg_rtty_squelch,
    },
    .network = {
        .wifi_enabled = &cfg_wifi_enabled,
    },
    .keys = {
        .long_gen = &cfg_long_gen,
        .long_app = &cfg_long_app,
        .long_key = &cfg_long_key,
        .long_msg = &cfg_long_msg,
        .long_dfn = &cfg_long_dfn,
        .long_dfl = &cfg_long_dfl,
        .press_f1 = &cfg_press_f1,
        .press_f2 = &cfg_press_f2,
        .long_f1 = &cfg_long_f1,
        .long_f2 = &cfg_long_f2,
    },
    .radio = {
        .charger = &cfg_charger,
        .line_in = &cfg_line_in,
        .line_out = &cfg_line_out,
        .spmode = &cfg_spmode,
        .freq_accel = &cfg_freq_accel,
    },
    .js8 = {
        .tx_freq = &cfg_js8_tx_freq,
        .hold_offset = &cfg_js8_hold_offset,
        .auto_mode = &cfg_js8_auto_mode,
        .hb = &cfg_js8_hb,
        .hb_ack = &cfg_js8_hb_ack,
        .relay = &cfg_js8_relay,
        .st_keep = &cfg_js8_st_keep,
        .msg_keep = &cfg_js8_msg_keep,
        .miles = &cfg_js8_miles,
        .decode_marks = &cfg_js8_decode_marks,
        .wf_avg = &cfg_js8_wf_avg,
        .map_mode = &cfg_js8_map_mode,
        .st_sort = &cfg_js8_st_sort,
        .tsync_auto = &cfg_js8_tsync_auto,
        .hb_interval = &cfg_js8_hb_interval,
        .cq_interval = &cfg_js8_cq_interval,
        .log_prompt = &cfg_js8_log_prompt,
        .log_activation = &cfg_js8_log_activation,
        .alerts = &cfg_js8_alerts,
        .speed = &cfg_js8_speed,
        .rx_all = &cfg_js8_rx_all,
        .ultra = &cfg_js8_ultra,
        .ghostnet = &cfg_js8_ghostnet,
        .custom_on = &cfg_js8_custom_on,
        .custom_hz = &cfg_js8_custom_hz,
    },
};

void cfg_api_init(void (*on_db_error)(const char *)) {
    // The manager loads global/band/mode params; the accessors above need no
    // wiring.
    cfg_sm.init_load(on_db_error);

    // Wire the ATU cache to the parameter sources and do the initial load.
    atu_wire_subscriptions();
}

void atu_wire_subscriptions(void) {
    // Recompute the published loaded/network subjects whenever the antenna,
    // front-panel frequency, or ATU-enabled changes.
    auto on_atu_param_change = [](Subject * /*subj*/, void * /*user_data*/) -> void {
        atu_network.on_params_changed(cfg_sm.p_ant_id.get(), cfg_sm.cp_fg_freq.get(), cfg_sm.p_atu_enabled.get() != 0);
    };
    // Keep the subscriptions alive for the whole program (never unsubscribed).
    static Subscription atu_ant_obs(cfg_sm.p_ant_id.subscribe(on_atu_param_change, nullptr));
    static Subscription atu_freq_obs(cfg_sm.cp_fg_freq.subscribe(on_atu_param_change, nullptr));
    static Subscription atu_enabled_obs(cfg_sm.p_atu_enabled.subscribe(on_atu_param_change, nullptr));

    // Initial load: populate the cache for the current antenna/frequency.
    atu_network.on_params_changed(cfg_sm.p_ant_id.get(), cfg_sm.cp_fg_freq.get(), cfg_sm.p_atu_enabled.get() != 0);
}
