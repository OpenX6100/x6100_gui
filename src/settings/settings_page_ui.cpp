#include "settings_widgets.h"

#include "../settings_types.h"

extern "C" {
#include "../meter.h"
#include "../styles.h"
}

/***** CLOCK *****/

static void make_clock(SettingsPage &page) {
    uint8_t col = 1;

    page.label("Clock view");

    lv_obj_t *obj = page.dropdown_int(page.grid, *cfg.ui.clock_view(), " Always Time \n Time and Power \n Always Power");

    lv_obj_set_size(obj, SMALL_6, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
    lv_obj_center(obj);

    /* * */

    page.row++;
    page.label("Timeout Clock, Power, TX");

    obj = page.spinbox_int(page.grid, *cfg.ui.clock_time_timeout(), 1, 59);
    lv_spinbox_set_digit_format(obj, 2, 0);
    lv_spinbox_set_digit_step_direction(obj, LV_DIR_LEFT);
    lv_obj_set_size(obj, SMALL_2, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, col, 2, LV_GRID_ALIGN_CENTER, page.row, 1);
    col += 2;

    obj = page.spinbox_int(page.grid, *cfg.ui.clock_power_timeout(), 1, 59);
    lv_spinbox_set_digit_format(obj, 2, 0);
    lv_spinbox_set_digit_step_direction(obj, LV_DIR_LEFT);
    lv_obj_set_size(obj, SMALL_2, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, col, 2, LV_GRID_ALIGN_CENTER, page.row, 1);
    col += 2;

    obj = page.spinbox_int(page.grid, *cfg.ui.clock_tx_timeout(), 0, 10);
    lv_spinbox_set_digit_format(obj, 2, 0);
    lv_spinbox_set_digit_step_direction(obj, LV_DIR_LEFT);
    lv_obj_set_size(obj, SMALL_2, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, col, 2, LV_GRID_ALIGN_CENTER, page.row, 1);
    col += 2;

    page.row++;
}

/***** LONG PRESS ACTIONS *****/

typedef struct {
    const char    *label;
    press_action_t action;
} action_items_t;

static action_items_t long_action_items[] = {
    {.label = " None ",            .action = ACTION_NONE        },
    {.label = " Screenshot ",      .action = ACTION_SCREENSHOT  },
    {.label = " Recorder on/off ", .action = ACTION_RECORDER    },
    {.label = " Mute ",            .action = ACTION_MUTE        },
    {.label = " Voice mode ",      .action = ACTION_VOICE_MODE  },
    {.label = " Battery info ",    .action = ACTION_BAT_INFO    },
    {.label = " APP RTTY ",        .action = ACTION_APP_RTTY    },
    {.label = " APP FT8 ",         .action = ACTION_APP_FT8     },
    {.label = " APP SWR Scan ",    .action = ACTION_APP_SWRSCAN },
    {.label = " APP GPS ",         .action = ACTION_APP_GPS     },
    {.label = " APP Settings",     .action = ACTION_APP_SETTINGS},
    {.label = " APP Recorder",     .action = ACTION_APP_RECORDER},
    {.label = " QTH Grid",         .action = ACTION_APP_QTH     },
    {.label = NULL,                .action = ACTION_NONE        }
};

static ParamInt *long_action_params[6];

static void long_action_update_cb(lv_event_t *e) {
    lv_obj_t  *obj   = lv_event_get_target(e);
    ParamInt **param = (ParamInt **)lv_event_get_user_data(e);
    uint8_t    val   = long_action_items[lv_dropdown_get_selected(obj)].action;

    (*param)->set(val);
}

static void make_long_action(SettingsPage &page) {
    const char *labels[] = {
        "GEN long press", "APP long press", "KEY long press", "MSG long press", "DFN long press", "DFL long press",
    };

    long_action_params[0] = cfg.keys.long_gen();
    long_action_params[1] = cfg.keys.long_app();
    long_action_params[2] = cfg.keys.long_key();
    long_action_params[3] = cfg.keys.long_msg();
    long_action_params[4] = cfg.keys.long_dfn();
    long_action_params[5] = cfg.keys.long_dfl();

    for (uint8_t i = 0; i < 6; i++) {

        page.label(labels[i]);

        lv_obj_t *obj = lv_dropdown_create(page.grid);

        dialog_item(page.dialog, obj);

        lv_obj_set_size(obj, SMALL_6, 56);
        lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
        lv_obj_center(obj);

        lv_obj_t *list = lv_dropdown_get_list(obj);
        lv_obj_add_style(list, &style.dialog.dropdown, 0);

        lv_dropdown_set_symbol(obj, NULL);

        uint8_t x = (uint8_t)long_action_params[i]->get();

        lv_dropdown_clear_options(obj);

        uint8_t n = 0;

        while (long_action_items[n].label) {
            lv_dropdown_add_option(obj, long_action_items[n].label, LV_DROPDOWN_POS_LAST);

            if (long_action_items[n].action == x) {
                lv_dropdown_set_selected(obj, n);
            }

            n++;
        }

        lv_obj_add_event_cb(obj, long_action_update_cb, LV_EVENT_VALUE_CHANGED, &long_action_params[i]);

        page.row++;
    }
}

/***** HMIC F1, F2 ACTIONS *****/

static action_items_t hmic_action_items[] = {
    {.label = " None ",            .action = ACTION_NONE      },
    {.label = " Recorder on/off ", .action = ACTION_RECORDER  },
    {.label = " Mute ",            .action = ACTION_MUTE      },
    {.label = " Step up ",         .action = ACTION_STEP_UP   },
    {.label = " Step down ",       .action = ACTION_STEP_DOWN },
    {.label = " Voice mode ",      .action = ACTION_VOICE_MODE},
    {.label = " Battery info ",    .action = ACTION_BAT_INFO  },
    {.label = " NR toggle ",       .action = ACTION_NR_TOGGLE },
    {.label = " NB toggle ",       .action = ACTION_NB_TOGGLE },
    {.label = NULL,                .action = ACTION_NONE      }
};

static ParamInt *hmic_action_params[4];

static void hmic_action_update_cb(lv_event_t *e) {
    lv_obj_t  *obj   = lv_event_get_target(e);
    ParamInt **param = (ParamInt **)lv_event_get_user_data(e);
    uint8_t    val   = hmic_action_items[lv_dropdown_get_selected(obj)].action;

    (*param)->set(val);
}

static void make_hmic_action(SettingsPage &page) {
    const char  *labels[]  = {"HMic F1 press", "HMic F2 press", "HMic F1 long press", "HMic F2 long press"};
    const size_t items_len = sizeof(labels) / sizeof(labels[0]);

    hmic_action_params[0] = cfg.keys.press_f1();
    hmic_action_params[1] = cfg.keys.press_f2();
    hmic_action_params[2] = cfg.keys.long_f1();
    hmic_action_params[3] = cfg.keys.long_f2();

    for (uint8_t i = 0; i < items_len; i++) {

        page.label(labels[i]);

        lv_obj_t *obj = lv_dropdown_create(page.grid);

        dialog_item(page.dialog, obj);

        lv_obj_set_size(obj, SMALL_6, 56);
        lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
        lv_obj_center(obj);

        lv_obj_t *list = lv_dropdown_get_list(obj);
        lv_obj_add_style(list, &style.dialog.dropdown, 0);

        lv_dropdown_set_symbol(obj, NULL);

        lv_dropdown_clear_options(obj);

        uint8_t x = (uint8_t)hmic_action_params[i]->get();
        uint8_t n = 0;

        while (hmic_action_items[n].label) {
            lv_dropdown_add_option(obj, hmic_action_items[n].label, LV_DROPDOWN_POS_LAST);

            if (hmic_action_items[n].action == x) {
                lv_dropdown_set_selected(obj, n);
            }

            n++;
        }

        lv_obj_add_event_cb(obj, hmic_action_update_cb, LV_EVENT_VALUE_CHANGED, &hmic_action_params[i]);

        page.row++;
    }
}

/***** MAG FREQ, INFO, ALC *****/

static void make_mag(SettingsPage &page) {
    lv_obj_t *obj;
    uint8_t   col = 1;

    page.label("Mag Freq, Info, ALC");

    /* Freq */

    obj = page.cell(col, 3, SMALL_2);
    col += 2;

    obj = page.switch_bool(obj, *cfg.ui.mag_freq(), "Magnification of frequency");
    lv_obj_set_width(obj, SMALL_2 - 30);

    /* Info */

    obj = page.cell(col, 3, SMALL_2);
    col += 2;

    obj = page.switch_bool(obj, *cfg.ui.mag_info(), "Magnification of info");
    lv_obj_set_width(obj, SMALL_2 - 30);

    /* ALC */

    obj = page.cell(col, 3, SMALL_2);
    col += 2;

    obj = page.switch_bool(obj, *cfg.ui.mag_alc(), "Magnification of A L C");
    lv_obj_set_width(obj, SMALL_2 - 30);

    page.row++;
}

/***** FREQ ACCELERATION *****/

static void make_freq_accel(SettingsPage &page) {
    page.label("Freq acceleration");

    lv_obj_t *obj =
        page.dropdown_int(page.grid, *cfg.radio.freq_accel(), " None \n Lite \n Strong", "Frequency acceleration");

    lv_obj_set_size(obj, SMALL_6, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
    lv_obj_center(obj);

    page.row++;
}

/***** SPECTRUM AND WATERFALL AUTO *****/

#define AUTO_LEVEL_STEP 0.2f

static lv_obj_t   *auto_level_sw;
static row_items_t row_level_manual_items;

static void auto_level_on_off_cb(lv_event_t *e) {
    lv_obj_t     *obj  = lv_event_get_target(e);
    SettingsPage *page = (SettingsPage *)lv_event_get_user_data(e);

    if (row_level_manual_items.cnt) {
        page->show_row(row_level_manual_items, !lv_obj_has_state(obj, LV_STATE_CHECKED));
    }
}

static void make_auto_offset(SettingsPage &page) {
    lv_obj_t *obj;

    page.label("Levels auto, offset");

    /* On/off */
    obj           = page.cell(1, 3, SMALL_3);
    auto_level_sw = page.switch_bool(obj, *cfg.ui.auto_level_enabled());
    lv_obj_add_event_cb(auto_level_sw, auto_level_on_off_cb, LV_EVENT_VALUE_CHANGED, &page);
    lv_obj_set_width(auto_level_sw, SMALL_3 - 30);

    /* Offset */
    obj = page.cell(4, 3, SMALL_3);
    obj = page.slider_float(obj, *cfg.ui.auto_level_offset(), -15.0f, 15.0f, AUTO_LEVEL_STEP, SMALL_3 - 120,
                            "%0.1f");
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    page.row++;
}

/***** LEVELS MIN, MAX (WHEN NO AUTO) *****/

static void make_spectrum_min_max(SettingsPage &page) {
    lv_obj_t *obj;
    lv_obj_t *cell;

    static lv_obj_t *items[3];
    row_level_manual_items.cnt   = 3;
    row_level_manual_items.row   = page.row;
    row_level_manual_items.items = items;
    lv_obj_t **items_ptr         = items;

    *items_ptr++ = page.label("Levels min, max");

    cell         = page.cell(1, 3, SMALL_3);
    *items_ptr++ = cell;

    obj = page.slider_int(cell, *cfg.band.grid_min(), S_MIN, S7, 1, SMALL_3 - 120, "%d");

    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    cell         = page.cell(4, 3, SMALL_3);
    *items_ptr++ = cell;

    obj = page.slider_int(cell, *cfg.band.grid_max(), S8, S9_40, 1, SMALL_3 - 120, "%d");

    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    lv_event_send(auto_level_sw, LV_EVENT_VALUE_CHANGED, NULL);

    page.row++;
}

/***** SPECTRUM FILL, PEAKS *****/

static void make_spectrum_fill_peak(SettingsPage &page) {
    lv_obj_t *obj;

    page.label("Spectrum fill, peaks");

    obj = page.cell(1, 3, SMALL_3);

    obj = page.switch_bool(obj, *cfg.ui.spectrum_filled());
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);
    lv_obj_set_width(obj, SMALL_3 - 30);

    obj = page.cell(4, 3, SMALL_3);

    obj = page.switch_bool(obj, *cfg.ui.spectrum_peak());
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);
    lv_obj_set_width(obj, SMALL_3 - 30);

    page.row++;
}

/***** SPECTRUM BETA, PEAK HOLD, PEAK SPEED *****/

static void make_spectrum_beta_peak_hold_speed(SettingsPage &page) {
    lv_obj_t *obj;
    uint8_t   col = 1;

    page.label("Spec. beta, hold, speed");

    obj = page.spinbox_int(page.grid, *cfg.ui.spectrum_beta(), 0, 90);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    lv_spinbox_set_digit_format(obj, 2, 0);
    lv_spinbox_set_digit_step_direction(obj, LV_DIR_LEFT);
    lv_obj_set_size(obj, SMALL_2, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, col, 2, LV_GRID_ALIGN_CENTER, page.row, 1);
    col += 2;

    obj = page.spinbox_int(page.grid, *cfg.ui.spectrum_peak_hold(), 1, 10);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    lv_spinbox_set_digit_format(obj, 2, 0);
    lv_spinbox_set_digit_step_direction(obj, LV_DIR_LEFT);
    lv_obj_set_size(obj, SMALL_2, 56);

    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, col, 2, LV_GRID_ALIGN_CENTER, page.row, 1);
    col += 2;

    obj = page.spinbox_int(page.grid, *cfg.ui.spectrum_peak_speed(), 1, 30);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    lv_spinbox_set_digit_format(obj, 2, 0);
    lv_spinbox_set_digit_step_direction(obj, LV_DIR_LEFT);
    lv_obj_set_size(obj, SMALL_2, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, col, 2, LV_GRID_ALIGN_CENTER, page.row, 1);
    col += 2;

    page.row++;
}

/***** SPECTRUM HEIGHT *****/

static void make_spectrum_height(SettingsPage &page) {
    page.label("Spectrum height");

    lv_obj_t *obj = page.cell(1, 6, SMALL_6);
    obj           = page.slider_int(obj, *cfg.ui.spectrum_height(), 160, 260, 10, SMALL_6 - 120, "%d");

    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    page.row++;
}

/***** WATERFALL CENTER LINE *****/

static void make_waterfall_line(SettingsPage &page) {
    lv_obj_t *obj;

    page.label("Waterfall line");

    obj = page.cell(4, 3, SMALL_3);

    obj = page.switch_bool(obj, *cfg.ui.waterfall_center_line(), "Waterfall center line");
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);
    lv_obj_set_width(obj, SMALL_3 - 30);

    page.row++;
}

static void make_knob_info(SettingsPage &page) {
    lv_obj_t *obj;

    page.label("Knob info");

    obj = page.cell(4, 3, SMALL_3);

    obj = page.switch_bool(obj, *cfg.ui.knob_info());

    lv_obj_set_width(obj, SMALL_3 - 30);

    page.row++;
}

static void make_meter_label_info(SettingsPage &page) {
    lv_obj_t *obj;

    page.label("Meter label");

    obj = page.cell(4, 3, SMALL_3);

    obj = page.switch_bool(obj, *cfg.ui.show_meter_value());

    lv_obj_set_width(obj, SMALL_3 - 30);

    page.row++;
}

/* 1KO125: S-meter calibration offset, dB (also applied to the CAT S-meter). */
static void make_s_meter_offset(SettingsPage &page) {
    page.label("S-meter offset");

    lv_obj_t *obj = page.cell(1, 6, SMALL_6);
    obj           = page.slider_int(obj, *cfg.ui.s_meter_offset(), -30, 30, 1, SMALL_6 - 120, "%d dB");

    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(obj, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    page.row++;
}

static void make_display_invert(SettingsPage &page) {
    lv_obj_t *obj;

    page.label("Display invert");

    obj = page.cell(4, 3, SMALL_3);

    obj = page.switch_bool(obj, *cfg.display.invert());

    lv_obj_set_width(obj, SMALL_3 - 30);

    page.row++;
}

/***** RGB PICKER *****/

static lv_obj_t   *spectrum_color_sw;
static lv_obj_t   *color_preview_rect;
static lv_obj_t   *color_preview_hex;
static lv_obj_t   *rgb_sliders[3];
static row_items_t row_rgb_picker_items;

static void rgb_color_update_cb(lv_event_t *e) {
    uint8_t r = lv_slider_get_value(rgb_sliders[0]);
    uint8_t g = lv_slider_get_value(rgb_sliders[1]);
    uint8_t b = lv_slider_get_value(rgb_sliders[2]);

    lv_color_t col = lv_color_make(r, g, b);
    lv_obj_set_style_bg_color(color_preview_rect, col, 0);
    lv_obj_set_style_bg_opa(color_preview_rect, LV_OPA_COVER, 0);

    char buf[10];
    lv_snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    lv_label_set_text(color_preview_hex, buf);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *slider = rgb_sliders[i];
        lv_obj_t *label  = (lv_obj_t *)lv_obj_get_user_data(slider);
        if (label) {
            char label_buf[16];
            lv_snprintf(label_buf, sizeof(label_buf), "%d", lv_slider_get_value(slider));
            lv_label_set_text(label, label_buf);
        }
    }
    cfg.ui.spectrum_color()->set(static_cast<int32_t>(col.full));
}

/* Spectrum custom color toggle */
static void spectrum_custom_color_toggle_cb(lv_event_t *e) {
    lv_obj_t     *obj  = lv_event_get_target(e);
    SettingsPage *page = (SettingsPage *)lv_event_get_user_data(e);
    bool          on   = lv_obj_has_state(obj, LV_STATE_CHECKED);
    cfg.ui.spectrum_use_custom_color()->set(on);

    if (row_rgb_picker_items.cnt) {
        page->show_row(row_rgb_picker_items, on);
    }
}

static void make_spectrum_custom_color_toggle(SettingsPage &page) {
    lv_obj_t *obj;

    page.label("Custom spectrum color");

    obj = page.cell(4, 3, SMALL_3);

    spectrum_color_sw = lv_switch_create(obj);
    dialog_item(page.dialog, spectrum_color_sw);
    lv_obj_center(spectrum_color_sw);
    lv_obj_set_width(spectrum_color_sw, SMALL_3 - 30);

    if (cfg.ui.spectrum_use_custom_color()->get()) {
        lv_obj_add_state(spectrum_color_sw, LV_STATE_CHECKED);
    }

    lv_obj_add_event_cb(spectrum_color_sw, spectrum_custom_color_toggle_cb, LV_EVENT_VALUE_CHANGED, &page);

    lv_obj_add_event_cb(spectrum_color_sw, settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
    lv_obj_add_event_cb(spectrum_color_sw, settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);

    page.row++;
}

static void make_rgb_color_picker(SettingsPage &page) {
    static lv_obj_t *items[3];
    row_rgb_picker_items.cnt            = 3;
    row_rgb_picker_items.row            = page.row;
    row_rgb_picker_items.items          = items;
    row_rgb_picker_items.default_height = 120;

    lv_obj_t **items_ptr = items;

    lv_obj_t *obj;
    uint8_t   col = 0;

    // Label
    obj          = page.label("Spectrum Color");
    *items_ptr++ = obj;
    col++;

    // Preview Container
    lv_obj_t *preview_cont = lv_obj_create(page.grid);
    *items_ptr++           = preview_cont;
    lv_obj_remove_style_all(preview_cont);
    lv_obj_set_layout(preview_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(preview_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(preview_cont, SMALL_2, LV_SIZE_CONTENT);
    lv_obj_add_style(preview_cont, &style.rgb.preview_cont, 0);
    lv_obj_set_grid_cell(preview_cont, LV_GRID_ALIGN_START, col, 2, LV_GRID_ALIGN_CENTER, page.row, 1);
    col += 2;

    // Rectangle Color Preview
    color_preview_rect = lv_obj_create(preview_cont);
    lv_obj_set_size(color_preview_rect, 80, 50);
    lv_obj_add_style(color_preview_rect, &style.rgb.preview_rect, 0);

    // Hex-Label
    color_preview_hex = lv_label_create(preview_cont);
    lv_label_set_text(color_preview_hex, "#AAAAAA");
    lv_obj_add_style(color_preview_hex, &style.rgb.preview_hex, 0);

    // Slider Panel
    lv_obj_t *slider_panel = lv_obj_create(page.grid);
    *items_ptr++           = slider_panel;
    lv_obj_remove_style_all(slider_panel);
    lv_obj_set_layout(slider_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(slider_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(slider_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_size(slider_panel, SMALL_3 + 60, LV_SIZE_CONTENT);
    lv_obj_add_style(slider_panel, &style.rgb.slider_panel, 0);
    lv_obj_set_grid_cell(slider_panel, LV_GRID_ALIGN_START, col, 3, LV_GRID_ALIGN_CENTER, page.row, 1);

    const char  *labels[]   = {"R", "G", "B"};
    lv_palette_t palettes[] = {LV_PALETTE_RED, LV_PALETTE_GREEN, LV_PALETTE_BLUE};

    int32_t full          = cfg.ui.spectrum_color()->get();
    uint8_t init_values[] = {(uint8_t)((full >> 16) & 0xFF), (uint8_t)((full >> 8) & 0xFF), (uint8_t)(full & 0xFF)};

    for (int i = 0; i < 3; i++) {
        lv_obj_t *slider_row = lv_obj_create(slider_panel);
        lv_obj_remove_style_all(slider_row);
        lv_obj_set_layout(slider_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(slider_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_size(slider_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_add_style(slider_row, &style.rgb.slider_row, 0);

        // Letter R / G / B
        lv_obj_t *letter_label = lv_label_create(slider_row);
        lv_label_set_text(letter_label, labels[i]);
        lv_obj_set_width(letter_label, 20);
        lv_obj_add_style(letter_label, &style.rgb.letter, 0);

        // Slider
        rgb_sliders[i] = page.slider_with_text<uint8_t>(slider_row, init_values[i], 0, 255, 1, SMALL_4 - 100, "%d",
                                                        rgb_color_update_cb, NULL);

        // Dynamic Color
        lv_obj_set_style_bg_color(rgb_sliders[i], lv_palette_main(palettes[i]), LV_PART_INDICATOR | LV_PART_KNOB);

        // Static Styles
        lv_obj_add_style(rgb_sliders[i], &style.rgb.slider, 0);
        lv_obj_add_style(rgb_sliders[i], &style.rgb.slider_focused, LV_STATE_FOCUSED);

        // Value-Label
        lv_obj_t *val_label = (lv_obj_t *)lv_obj_get_user_data(rgb_sliders[i]);
        lv_obj_add_style(val_label, &style.rgb.val_label, 0);

        lv_obj_add_event_cb(rgb_sliders[i], settings_change_bg_opa_cb, LV_EVENT_FOCUSED, &page);
        lv_obj_add_event_cb(rgb_sliders[i], settings_change_bg_opa_cb, LV_EVENT_DEFOCUSED, &page);
    }

    lv_event_send(spectrum_color_sw, LV_EVENT_VALUE_CHANGED, NULL);
    lv_event_send(rgb_sliders[0], LV_EVENT_VALUE_CHANGED, NULL);

    page.row++;
}

/***** THEME / COLORS *****/

static void apply_theme() {
    styles_set_theme((themes_t)cfg.ui.theme()->get());
}

static void apply_meter_color() {
    styles_update_meter_colors((meter_color_t)cfg.ui.meter_color()->get());
}

static void make_theme(SettingsPage &page) {
    page.label("Theme");

    lv_obj_t *obj =
        page.dropdown_int(page.grid, *cfg.ui.theme(), " Simple \n Black \n Flat", nullptr, apply_theme);

    lv_obj_set_size(obj, SMALL_6, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
    lv_obj_center(obj);

    page.row++;
}

static void make_meter_color(SettingsPage &page) {
    page.label("Meter Color");

    lv_obj_t *obj =
        page.dropdown_int(page.grid, *cfg.ui.meter_color(), " Gray \n Colored", nullptr, apply_meter_color);

    lv_obj_set_size(obj, SMALL_6, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
    lv_obj_center(obj);

    page.row++;
}

/***** FT8/FT4 TABLE INFO *****/

static void make_ft8_info(SettingsPage &page) {
    page.label("FT8/FT4 info");

    lv_obj_t *obj = page.dropdown_int(page.grid, *cfg.ui.ft8_info(),
                                      " Distance km \n Distance mi \n Country \n Country + km \n Country + mi",
                                      "FT8 table info");

    lv_obj_set_size(obj, SMALL_6, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
    lv_obj_center(obj);

    page.row++;
}

static void make_swr_color(SettingsPage &page) {
    page.label("SWR Color");

    lv_obj_t *obj = page.dropdown_int(page.grid, *cfg.ui.swr_color(), " Gray \n Colored");

    lv_obj_set_size(obj, SMALL_6, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
    lv_obj_center(obj);

    page.row++;
}

/***** DECODER TEXT SIZE *****/

static void make_digi_text(SettingsPage &page) {
    page.label("Decoder text");

    lv_obj_t *obj = page.dropdown_int(page.grid, *cfg.ui.digi_compact(), " Large \n Compact",
                                      "Decoder text size");

    lv_obj_set_size(obj, SMALL_6, 56);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, page.row, 1);
    lv_obj_center(obj);

    page.row++;
}

/***** PAGE *****/

void make_ui_page(SettingsPage &page) {
    page.reset();

    // page.delimiter();
    make_clock(page);
    page.delimiter();

    make_long_action(page);
    page.delimiter();

    make_hmic_action(page);
    page.delimiter();

    make_freq_accel(page);
    page.delimiter();

    make_mag(page);

    make_auto_offset(page);
    make_spectrum_min_max(page);
    page.delimiter();

    make_spectrum_fill_peak(page);
    make_spectrum_beta_peak_hold_speed(page);
    make_spectrum_height(page);
    page.delimiter();

    make_waterfall_line(page);
    page.delimiter();

    make_digi_text(page);
    page.delimiter();

    make_knob_info(page);
    page.delimiter();

    make_ft8_info(page);
    page.delimiter();

    make_meter_label_info(page);
    make_s_meter_offset(page);
    page.delimiter();

    make_display_invert(page);
    page.delimiter();

    /* RGB picker Meter SWR Color */
    make_spectrum_custom_color_toggle(page);
    make_rgb_color_picker(page);
    page.delimiter();

    make_theme(page);
    make_meter_color(page);
    make_swr_color(page);

    page.finish();
}
