#include "indicators.h"

#include <cstdio>
#include <cstring>

#include "globals.h"
#include "cfg/cfg_api.h"
#include "cfg/atu.h"
#include "pubsub_ids.h"
#include "wifi.h"

extern "C" {
    #include "styles.h"
    #include "bt_audio.h"
    #include "bt_ctl.h"
    #include "bt_spp.h"
    #include "keyboard.h"

    LV_FONT_DECLARE(bt_icons_17);

    #include <aether_radio/x6100_control/control.h>
}

#define SEP_WIDTH 4
#define ATU_STATE_OUT_OF_RANGE LV_STATE_USER_1

static lv_obj_t * create_item(lv_obj_t *parent, const char *label);
static lv_obj_t * create_spacer(lv_obj_t *parent);
static lv_obj_t * create_sep(lv_obj_t *parent);

static void wifi_state_change_cb(void *s, lv_msg_t *m);
static void bt_update_cb(lv_timer_t *t);

static void mark_non_zero(Subject *subj, void *user_data);
static void on_agc_change(Subject *subj, void *user_data);
static void atu_label_update(Subject *subj, void * user_data);


static lv_obj_t     *cont_left;
static lv_obj_t     *cont_right;

static lv_obj_t     *att_label;
static lv_obj_t     *pre_label;

static lv_obj_t     *nr_label;
static lv_obj_t     *nb_label;
static lv_obj_t     *dnf_label;

static lv_obj_t     *rit_label;
static lv_obj_t     *xit_label;
static lv_obj_t     *atu_label;
static lv_obj_t     *spl_label;

static lv_obj_t     *agc_label;
static lv_obj_t     *wifi_label;
static lv_obj_t     *bt_label;

/* bt_icons_17: Bluetooth, and what is connected through it */
#define BT_ICON         "\xEF\x8A\x93"   /* U+F293 Bluetooth */
#define BT_ICON_AUDIO   "\xEF\x80\xA5"   /* U+F025 headphones: the radio's audio there */
#define BT_ICON_PHONE   "\xEF\x8F\x8D"   /* U+F3CD phone: the app connected (SPP) */
#define BT_ICON_KBD     "\xEF\x84\x9C"   /* U+F11C keyboard */
#define BT_POLL_MS      1000

static lv_style_t ind_style;
static lv_style_t ind_style_active;
static lv_style_t ind_style_disabled;
static lv_style_t atu_style_out_of_range;
static lv_style_t ind_sep_style;  // group separator style



void indicators_init(lv_obj_t * parent, lv_coord_t h, lv_coord_t meter_w) {

    // Styles
    lv_style_init(&ind_style);
    lv_style_set_text_font(&ind_style, &sony_22);
    lv_style_set_text_color(&ind_style, lv_color_hex(0x8A8A8A));
    lv_style_set_pad_hor(&ind_style, 5);
    lv_style_set_align(&ind_style, LV_ALIGN_CENTER);

    lv_style_init(&ind_style_active);
    lv_style_set_text_color(&ind_style_active, lv_color_hex(0xFFFFFF));
    lv_style_set_bg_color(&ind_style_active, lv_color_hex(0x12526F));
    lv_style_set_bg_opa(&ind_style_active, LV_OPA_COVER);
    lv_style_set_radius(&ind_style_active, 5);
    lv_style_set_pad_ver(&ind_style_active, 3);

    lv_style_init(&ind_style_disabled);
    lv_style_set_text_color(&ind_style_disabled, lv_color_hex(0x606060));

    lv_style_init(&atu_style_out_of_range);
    lv_style_set_text_color(&atu_style_out_of_range, lv_color_hex(0xFF9F43));

    lv_style_init(&ind_sep_style);
    lv_style_set_bg_color(&ind_sep_style, lv_color_hex(0x282828));
    lv_style_set_bg_opa(&ind_sep_style, LV_OPA_COVER);
    lv_style_set_width(&ind_sep_style, SEP_WIDTH);
    lv_style_set_height(&ind_sep_style, 26);

    // End styles

    cont_left = lv_obj_create(parent);
    lv_obj_remove_style_all(cont_left);

    lv_obj_set_size(cont_left, meter_w + SEP_WIDTH / 2, h);
    lv_obj_set_flex_flow(cont_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont_left, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Spacing between items
    // lv_obj_set_style_pad_column(cont_left, 0, LV_PART_MAIN);

    create_spacer(cont_left);
    att_label = create_item(cont_left, "ATT");
    create_spacer(cont_left);
    pre_label = create_item(cont_left, "PRE");
    create_spacer(cont_left);
    create_sep(cont_left);
    create_spacer(cont_left);
    nr_label = create_item(cont_left, "NR");
    create_spacer(cont_left);
    nb_label = create_item(cont_left, "NB");
    create_spacer(cont_left);
    dnf_label = create_item(cont_left, "DNF");
    create_spacer(cont_left);
    create_sep(cont_left);
    create_spacer(cont_left);
    agc_label = create_item(cont_left, "AGC-A");
    create_spacer(cont_left);
    create_sep(cont_left);

    cont_right = lv_obj_create(parent);
    lv_obj_remove_style_all(cont_right);
    lv_obj_set_size(cont_right, SCREEN_WIDTH - meter_w - 14, h);
    lv_obj_set_x(cont_right, meter_w + SEP_WIDTH / 2 + 10);

    lv_obj_set_flex_flow(cont_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Spacing between items
    lv_obj_set_style_pad_column(cont_right, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    spl_label = create_item(cont_right, "SPL");
    atu_label = create_item(cont_right, "ATU1");
    lv_obj_add_style(atu_label, &ind_style_disabled, LV_STATE_DISABLED);
    lv_obj_add_style(atu_label, &atu_style_out_of_range, ATU_STATE_OUT_OF_RANGE);
    create_sep(cont_right);
    rit_label = create_item(cont_right, "RIT");
    xit_label = create_item(cont_right, "XIT");

    create_spacer(cont_right);

    bt_label = create_item(cont_right, BT_ICON);
    lv_obj_set_style_text_font(bt_label, &bt_icons_17, 0);
    lv_obj_set_style_text_letter_space(bt_label, 4, 0);
    lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);

    wifi_label = create_item(cont_right, LV_SYMBOL_WIFI "");


    // Subscriptions
    cfg.cur.att()->subscribe_delayed_and_notify(mark_non_zero, (void*)att_label);
    cfg.cur.pre()->subscribe_delayed_and_notify(mark_non_zero, (void*)pre_label);

    cfg.cur.agc()->subscribe_delayed_and_notify(mark_non_zero, (void*)agc_label);
    cfg.cur.agc()->subscribe_delayed_and_notify(on_agc_change);

    cfg.dsp.nr()->subscribe_delayed_and_notify(mark_non_zero, (void*)nr_label);
    cfg.dsp.nb()->subscribe_delayed_and_notify(mark_non_zero, (void*)nb_label);
    cfg.dsp.dnf()->subscribe_delayed_and_notify(mark_non_zero, (void*)dnf_label);

    cfg.band.split()->subscribe_delayed_and_notify(mark_non_zero, (void*)spl_label);

    cfg.atu_enabled()->subscribe_delayed(atu_label_update);
    cfg.ant_id()->subscribe_delayed(atu_label_update);
    atu_network.loaded.subscribe_delayed_and_notify(atu_label_update);

    cfg.rit()->subscribe_delayed_and_notify(mark_non_zero, (void*)rit_label);
    cfg.xit()->subscribe_delayed_and_notify(mark_non_zero, (void*)xit_label);

    lv_msg_subscribe(MSG_WIFI_STATE_CHANGED, wifi_state_change_cb, NULL);

    /* Bluetooth has no message of its own: its state is read every
     * second (a few mutexes and one stat(), no D-Bus). */
    lv_timer_create(bt_update_cb, BT_POLL_MS, NULL);
    bt_update_cb(NULL);
}

void indicators_left_show(bool v) {
    if (!v) {
        lv_obj_add_flag(cont_left, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(cont_left, LV_OBJ_FLAG_HIDDEN);
    }
}

void indicators_show(bool v) {
    if (!v) {
        lv_obj_add_flag(cont_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cont_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(cont_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cont_right, LV_OBJ_FLAG_HIDDEN);
    }
}


static lv_obj_t * create_item(lv_obj_t *parent, const char *label) {
    lv_obj_t *item = lv_label_create(parent);
    lv_obj_add_style(item, &ind_style, LV_STATE_DEFAULT);
    lv_obj_add_style(item, &ind_style_active, LV_STATE_CHECKED);
    lv_label_set_text(item, label);
    return item;
}

static lv_obj_t * create_spacer(lv_obj_t *parent) {
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    return spacer;
}

static lv_obj_t * create_sep(lv_obj_t *parent) {
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_add_style(item, &ind_sep_style, LV_STATE_DEFAULT);
    return item;
}


static const char * agc_to_label(x6100_agc_t agc) {
    switch (agc) {
        case x6100_agc_off:      return "AGC-0";
        case x6100_agc_fast:     return "AGC-F";
        case x6100_agc_slow:     return "AGC-S";
        case x6100_agc_auto:     return "AGC-A";
    }
    return "AGC-?";
}

static void wifi_state_change_cb(void *s, lv_msg_t *m) {
    wifi_status_t status = wifi_get_status();
    if (status == WIFI_OFF) {
        lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
        if (status == WIFI_CONNECTED) {
            lv_obj_add_state(wifi_label, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(wifi_label, LV_STATE_CHECKED);
        }
    }
    lv_obj_update_layout(lv_obj_get_parent(wifi_label));
}


/*
 * As WiFi's: hidden while Bluetooth is off, grey while it is on with
 * nothing connected, highlighted while something is, followed by what:
 * headphones (the radio's audio goes to a headset), a phone (the app,
 * over SPP), a keyboard.
 */
static void bt_update_cb(lv_timer_t *t) {
    static char last[24] = "";
    static int  last_state = -1;
    char        text[24];
    int         state;      /* 0 hidden, 1 on, 2 connected */

    (void)t;
    bt_ctl_state_t ctl = bt_ctl_state_if_started();
    bool           on = ctl == BT_CTL_ON || ctl == BT_CTL_STARTING;
    bool           audio = bt_audio_state_if_started() == BT_AUDIO_ON;
    bool           phone = bt_spp_state() == BT_SPP_CONNECTED;
    bool           kbd = keyboard_bt_connected();

    snprintf(text, sizeof(text), "%s%s%s%s", BT_ICON, audio ? BT_ICON_AUDIO : "",
             phone ? BT_ICON_PHONE : "", kbd ? BT_ICON_KBD : "");
    if (audio || phone || kbd) {
        state = 2;
    } else {
        state = on ? 1 : 0;
    }
    if (state == last_state && strcmp(text, last) == 0) {
        return;
    }
    last_state = state;
    snprintf(last, sizeof(last), "%s", text);

    lv_label_set_text(bt_label, text);
    if (state == 0) {
        lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (state == 2) {
        lv_obj_add_state(bt_label, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(bt_label, LV_STATE_CHECKED);
    }
    lv_obj_update_layout(lv_obj_get_parent(bt_label));
}

static void mark_non_zero(Subject *subj, void *user_data) {
    lv_obj_t *item = (lv_obj_t *)user_data;
    if (static_cast<SubjectT<int32_t>*>(subj)->get()) {
        lv_obj_add_state(item, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(item, LV_STATE_CHECKED);
    }
}

static void on_agc_change(Subject *subj, void *user_data) {
    x6100_agc_t agc_val = (x6100_agc_t)cfg.cur.agc()->get();
    lv_label_set_text(agc_label, agc_to_label(agc_val));
}

static void atu_label_update(Subject *subj, void * user_data) {
    int32_t ant = cfg.ant_id()->get();
    int32_t freq = cfg.cur.fg_freq()->get();
    lv_label_set_text_fmt(atu_label, "ATU%i", ant);

    lv_obj_clear_state(atu_label, ATU_STATE_OUT_OF_RANGE);
    lv_obj_clear_state(atu_label, LV_STATE_DISABLED);
    lv_obj_clear_state(atu_label, LV_STATE_CHECKED);

    if (cfg_transverter_shift_for(freq)) {
        lv_obj_add_state(atu_label, LV_STATE_DISABLED);
    } else if (cfg.atu_enabled()->get()) {
        lv_obj_add_state(atu_label, LV_STATE_CHECKED);
        if (!atu_network.loaded.get()) {
            lv_obj_add_state(atu_label, ATU_STATE_OUT_OF_RANGE);
        }
    }
}
