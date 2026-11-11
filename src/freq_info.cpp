#include "freq_info.h"

#include <cstdio>
#include <string>
#include <string_view>

#include "cfg/cfg_api.h"
#include "radio.h"
#include "styles.h"
#include "pubsub_ids.h"
#include "util.h"
#include "lock_manager.h"

static lv_obj_t *obj;

static lv_obj_t *fg_freq_row;
static lv_obj_t *fg_mhz_label;
static lv_obj_t *fg_khz_label;
static lv_obj_t *fg_hz_label;

static lv_obj_t *bg_freq_label;

static lv_obj_t *vfo_label;
static lv_obj_t *mode_label;

static int32_t freq_step;

// static lv_style_t hz_label_style;

static lv_style_t fg_freq_big_style;
static lv_style_t fg_freq_small_style;
static lv_style_t bg_freq_style;
static lv_style_t locked_freq_style;

static void update_fg_freq(void);
static void update_bg_freq(void);
static void on_vfo_change(Subject *subj, void *user_data);
static void on_step_change(Subject *subj, void *user_data);
static void on_mode_change(Subject *subj, void *user_data);
static void lock_freq_change_cb(void *s, lv_msg_t *msg);
static void tx_cb(void *s, lv_msg_t *msg);
static void rx_cb(void *s, lv_msg_t *msg);

static void fg_freq_post_draw_event_cb(lv_event_t *e);

lv_obj_t * freq_info_init(lv_obj_t * parent) {

    lv_font_t *fg_freq_big_font = &mono_48;
    lv_font_t *fg_freq_small_font = &mono_28;
    lv_coord_t small_offset = fg_freq_big_font->base_line - fg_freq_small_font->base_line;

    lv_style_init(&fg_freq_big_style);
    lv_style_set_text_font(&fg_freq_big_style, fg_freq_big_font);
    lv_style_set_text_letter_space(&fg_freq_big_style, -1);

    lv_style_init(&fg_freq_small_style);
    lv_style_set_text_font(&fg_freq_small_style, fg_freq_small_font);
    lv_style_set_text_letter_space(&fg_freq_small_style, -1);
    lv_style_set_translate_y(&fg_freq_small_style, -small_offset);

    lv_style_init(&bg_freq_style);
    lv_style_set_text_font(&bg_freq_style, &mono_22);
    lv_style_set_text_color(&bg_freq_style, lv_color_hex(0xF9A851));

    lv_style_init(&locked_freq_style);
    lv_style_set_text_color(&locked_freq_style, lv_color_hex(0x969696));

    obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &style.freq_info, LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(obj, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(obj, 2, LV_PART_MAIN);

    /* Bg freq */

    bg_freq_label = lv_label_create(obj);
    lv_obj_set_align(bg_freq_label, LV_ALIGN_TOP_RIGHT);
    lv_obj_add_style(bg_freq_label, &bg_freq_style, LV_PART_MAIN);
    lv_obj_add_style(bg_freq_label, &locked_freq_style, LV_STATE_DISABLED);

    /* Fg freq */

    fg_freq_row = lv_obj_create(obj);
    lv_obj_remove_style_all(fg_freq_row);
    lv_obj_set_flex_flow(fg_freq_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fg_freq_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_align(fg_freq_row, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_width(fg_freq_row, lv_pct(95));
    lv_obj_set_style_pad_column(fg_freq_row, 5, LV_PART_MAIN);

    lv_obj_add_event_cb(fg_freq_row, fg_freq_post_draw_event_cb, LV_EVENT_DRAW_POST, NULL);


    fg_mhz_label = lv_label_create(fg_freq_row);
    lv_obj_add_style(fg_mhz_label, &fg_freq_big_style, LV_PART_MAIN);
    lv_obj_add_style(fg_mhz_label, &style.text_base_color, LV_PART_MAIN);
    lv_obj_add_style(fg_mhz_label, &style.text_muted_color, LV_STATE_DISABLED);

    fg_khz_label = lv_label_create(fg_freq_row);
    lv_obj_add_style(fg_khz_label, &fg_freq_big_style, LV_PART_MAIN);
    lv_obj_add_style(fg_khz_label, &style.text_base_color, LV_PART_MAIN);
    lv_obj_add_style(fg_khz_label, &style.text_muted_color, LV_STATE_DISABLED);

    fg_hz_label = lv_label_create(fg_freq_row);
    lv_obj_add_style(fg_hz_label, &fg_freq_small_style, LV_PART_MAIN);
    lv_obj_add_style(fg_hz_label, &style.text_base_color, LV_PART_MAIN);
    lv_obj_add_style(fg_hz_label, &style.text_muted_color, LV_STATE_DISABLED);

    /* VFO */

    vfo_label = lv_label_create(obj);
    lv_obj_add_style(vfo_label, &style.text_base_color, LV_PART_MAIN);
    lv_obj_align(vfo_label, LV_ALIGN_BOTTOM_LEFT, 5, -4);
    lv_obj_set_style_bg_color(vfo_label, lv_color_hex(0xD32F2F), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vfo_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(vfo_label, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(vfo_label, 5, LV_PART_MAIN);
    lv_obj_set_style_text_font(vfo_label, &sony_32, LV_PART_MAIN);

    /* Mode */
    mode_label = lv_label_create(obj);
    lv_obj_add_style(mode_label, &style.text_base_color, LV_PART_MAIN);
    lv_obj_align(mode_label, LV_ALIGN_TOP_LEFT, 5, -2);
    lv_obj_set_style_bg_color(mode_label, lv_color_hex(0x4CAF50), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mode_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(mode_label, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(mode_label, 3, LV_PART_MAIN);
    lv_obj_set_style_text_font(mode_label, &sony_28, LV_PART_MAIN);

    /* Subscriptions */
    constexpr auto observer_fn = [](Subject *, void *) {
        update_fg_freq();
        update_bg_freq();
    };

    radio_fg_freq_subj->subscribe_delayed(observer_fn);
    radio_bg_freq_subj->subscribe_delayed_and_notify(observer_fn);

    cfg.band.current_vfo()->subscribe_delayed_and_notify(on_vfo_change, NULL);
    cfg.mode.freq_step()->subscribe_delayed_and_notify(on_step_change, NULL);
    cfg.cur.mode()->subscribe_delayed_and_notify(on_mode_change, NULL);

    lv_msg_subscribe(MSG_LOCK_FREQ, lock_freq_change_cb, NULL);

    /* During TX the TX meter takes this box's place too (tx_info.c) */
    lv_msg_subscribe(MSG_RADIO_TX, tx_cb, NULL);
    lv_msg_subscribe(MSG_RADIO_RX, rx_cb, NULL);

    return NULL;
}

const char *mode_to_str(x6100_mode_t mode) {
    switch (mode) {
        case x6100_mode_lsb:     return "LSB";
        case x6100_mode_lsb_dig: return "LSB-D";
        case x6100_mode_usb:     return "USB";
        case x6100_mode_usb_dig: return "USB-D";
        case x6100_mode_cw:      return "CW";
        case x6100_mode_cwr:     return "CW-R";
        case x6100_mode_am:      return "AM";
        case x6100_mode_nfm:     return "NFM";
    }
    return "?";
}

static void update_fg_freq(void) {
    uint16_t mhz, khz, hz;
    int32_t freq = cfg.cur.fg_freq()->get();

    split_freq(freq, &mhz, &khz, &hz);
    if (mhz) {
        lv_label_set_text_fmt(fg_mhz_label, "%i", mhz);
        lv_label_set_text_fmt(fg_khz_label, "%03i", khz);
    } else {
        lv_label_set_text(fg_mhz_label, "");
        lv_label_set_text_fmt(fg_khz_label, "%i", khz);
    }
    lv_label_set_text_fmt(fg_hz_label, "%03i", hz);
}

static void update_bg_freq(void) {
    uint16_t mhz, khz, hz;
    int32_t freq = cfg.cur.bg_freq()->get();
    split_freq(freq, &mhz, &khz, &hz);
    char buffer[16];
    if (mhz) {
        std::snprintf(buffer, sizeof(buffer), "%i %03i %03i", mhz, khz, hz);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%i %03i", khz, hz);
    }
    lv_label_set_text(bg_freq_label, buffer);
}


static void on_vfo_change(Subject *subj, void *user_data) {
    x6100_vfo_t vfo = (x6100_vfo_t)cfg.band.current_vfo()->get();
    if (vfo == X6100_VFO_A) {
        lv_label_set_text(vfo_label, "A");
    } else {
        lv_label_set_text(vfo_label, "B");
    }
}


static void on_step_change(Subject *subj, void *user_data) {
    freq_step = cfg.mode.freq_step()->get();
    lv_obj_invalidate(fg_freq_row);
}

static void on_mode_change(Subject *subj, void *user_data) {
    x6100_mode_t mode = (x6100_mode_t)cfg.cur.mode()->get();
    lv_label_set_text(mode_label, mode_to_str(mode));
}

static void tx_cb(void *s, lv_msg_t *msg) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void rx_cb(void *s, lv_msg_t *msg) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void lock_freq_change_cb(void *s, lv_msg_t *msg) {
    bool *lock = (bool*)lv_msg_get_payload(msg);
    if (*lock) {
        lv_obj_add_state(bg_freq_label, LV_STATE_DISABLED);
        lv_obj_add_state(fg_hz_label, LV_STATE_DISABLED);
        lv_obj_add_state(fg_khz_label, LV_STATE_DISABLED);
        lv_obj_add_state(fg_mhz_label, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(bg_freq_label, LV_STATE_DISABLED);
        lv_obj_clear_state(fg_hz_label, LV_STATE_DISABLED);
        lv_obj_clear_state(fg_khz_label, LV_STATE_DISABLED);
        lv_obj_clear_state(fg_mhz_label, LV_STATE_DISABLED);
    }

}

/* Custom draw for fg freq */

static void fg_freq_post_draw_event_cb(lv_event_t *e) {

    if (lm_get_freq()) return;

    lv_obj_t *label;
    uint8_t ch_pos = 0;

    int32_t tmp = freq_step;
    while (tmp >= 10) {
        tmp /= 10;
        ch_pos++;
    }
    bool big_mark = tmp > 1;

    if (ch_pos < 3) {
        // modify Hz
        label = fg_hz_label;
        ch_pos = 2 - ch_pos;
    } else if (ch_pos < 6) {
        // modify kHz
        label = fg_khz_label;
        ch_pos = 5 - ch_pos;
    } else {
        return;
    }

    const char * text = lv_label_get_text(label);
    if(text == NULL || text[0] == '\0') return;

    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);

    lv_draw_ctx_t  *draw_ctx = lv_event_get_draw_ctx(e);

    lv_area_t label_coords;
    lv_obj_get_coords(label, &label_coords);

    lv_point_t p1, p2;
    lv_label_get_letter_pos(label, ch_pos, &p1);
    lv_label_get_letter_pos(label, ch_pos + 1, &p2);

    p1.x += label_coords.x1;
    p2.x += label_coords.x1;
    p1.y += label_coords.y1 + font->line_height - font->base_line + 4;
    p2.y += label_coords.y1 + font->line_height - font->base_line + 4;

    if (!big_mark) {
        lv_coord_t cx = (p1.x + p2.x) / 2;
        lv_coord_t w = (p2.x - p1.x) / 2;
        p1.x = cx - w / 2;
        p2.x = p1.x + w;
    }

    lv_draw_line_dsc_t draw_dsc;
    lv_draw_line_dsc_init(&draw_dsc);
    draw_dsc.color = lv_color_hex(0x4CAF50);
    draw_dsc.width = 3;
    draw_dsc.round_start = 0;
    draw_dsc.round_end = 0;

    lv_draw_line(draw_ctx, &draw_dsc, &p1, &p2);
}
