/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - Bluetooth window
 *
 *  Adapter power, the device list, and the listening loopback to
 *  Bluetooth headphones, in one place: status on the left, a table on
 *  the right, actions on the button row.
 *
 *  PTT keys the radio from the Bluetooth headset microphone (HFP).
 *  It is a toggle: press to transmit, press again to receive. A short
 *  press of the headset's own button does the same (bt_audio.c). The
 *  worker in bt_audio drops TX after BT_PTT_TIMEOUT_S, when the headset
 *  goes away, when Bluetooth is switched off and when this window
 *  closes.
 *
 *  The UDP switch (WSJT-X datagrams over WiFi, ft8_udp.c) lives here as
 *  well: the FT8 window has no free button and this is the window that
 *  already deals with where the FT8 packets go.
 *
 *  Buttons, three pages:
 *    1: (Page 1:3)  BT  Audio  PTT  UDP      - used while operating
 *    2: (Page 2:3)  Scan  Connect  Forget  Exit  - pairing and devices
 *    3: (Page 3:3)  Mic gain                 - headset mic level for PTT
 *
 *  Mic gain: press +1 dB, hold -1 dB. Set it so the ALC just moves on
 *  voice peaks while transmitting from the headset.
 *
 *  Scan opens pairing for 3 minutes: the radio is visible and a phone
 *  can pair from its side; the rest of the time it is hidden and refuses
 *  pairing. A keyboard: press its pairing key, then Connect, and type
 *  the code shown here if it asks for one.
 *
 *  Pairing is part of Connect (bt_dev pairs first when needed). Forget
 *  removes the pairing; it acts on hold only, a press just says so.
 *
 *  Nothing here blocks. Every D-Bus and pactl call belongs to
 *  bt_dev or bt_audio and runs on their worker threads; this file only
 *  reads cached values and asks for work.
 */

#include "dialog_bt.h"

#include "lvgl/lvgl.h"

#include "buttons.h"
#include "dialog.h"
#include "events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "radio.h"
#include "styles.h"
#include "wifi.h"
#include "cfg/cfg_api.h"

#include "bt_audio.h"
#include "bt_ctl.h"
#include "bt_dev.h"
#include "bt_spp.h"
#include "ft8_udp.h"

#include <stdio.h>
#include <string.h>

/* Window size: DIALOG_WIDTH/HEIGHT from styles.h, as the WiFi window
 * uses. */
#define PARAMS_WIDTH  300

/* A device refresh is a D-Bus round trip plus one pactl, so it does not
 * get to run every tick. pactl is not free either. */
#define TICK_MS       1000
#define DEV_EVERY     3
#define AUDIO_EVERY   2

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void bt_toggle_cb(button_data_t *item);
static void audio_toggle_cb(button_data_t *item);
static void connect_cb(button_data_t *item);
static void scan_cb(button_data_t *item);
static void ptt_cb(button_data_t *item);
static void udp_toggle_cb(button_data_t *item);
static void forget_cb(button_data_t *item);
static void forget_hold_cb(button_data_t *item);
static void exit_cb(button_data_t *item);
static void mic_gain_up_cb(button_data_t *item);
static void mic_gain_down_cb(button_data_t *item);

static const char *bt_label_getter(void);
static const char *audio_label_getter(void);
static const char *connect_label_getter(void);
static const char *scan_label_getter(void);
static const char *ptt_label_getter(void);
static const char *udp_label_getter(void);
static const char *mic_gain_label_getter(void);

static button_data_t btn_bt = {
    .type = BTN_TEXT_FN, .label_fn = bt_label_getter, .press = bt_toggle_cb,
};
static button_data_t btn_audio = {
    .type = BTN_TEXT_FN, .label_fn = audio_label_getter,
    .press = audio_toggle_cb,
};
static button_data_t btn_ptt = {
    .type = BTN_TEXT_FN, .label_fn = ptt_label_getter, .press = ptt_cb,
};
static button_data_t btn_udp = {
    .type = BTN_TEXT_FN, .label_fn = udp_label_getter,
    .press = udp_toggle_cb,
};
/* Exit stays on the hold of Scan too, as in the one-page layout.
 * ESC closes the window as well. */
static button_data_t btn_scan = {
    .type = BTN_TEXT_FN, .label_fn = scan_label_getter, .press = scan_cb,
    .hold = exit_cb,
};
static button_data_t btn_connect = {
    .type = BTN_TEXT_FN, .label_fn = connect_label_getter,
    .press = connect_cb,
};
static button_data_t btn_forget = {
    .type = BTN_TEXT, .label = "Forget\n(hold)", .press = forget_cb,
    .hold = forget_hold_cb,
};
static button_data_t btn_exit = {
    .type = BTN_TEXT, .label = "Exit", .press = exit_cb,
};
static button_data_t btn_mic = {
    .type = BTN_TEXT_FN, .label_fn = mic_gain_label_getter,
    .press = mic_gain_up_cb, .hold = mic_gain_down_cb,
};

static buttons_page_t btn_page_1;
static buttons_page_t btn_page_2;
static buttons_page_t btn_page_3;

static button_data_t btn_next_1 = {
    .type = BTN_TEXT, .label = "(Page: 1:3)", .press = button_next_page_cb,
    .next = &btn_page_2,
};
static button_data_t btn_next_2 = {
    .type = BTN_TEXT, .label = "(Page: 2:3)", .press = button_next_page_cb,
    .next = &btn_page_3,
};
static button_data_t btn_next_3 = {
    .type = BTN_TEXT, .label = "(Page: 3:3)", .press = button_next_page_cb,
    .next = &btn_page_1,
};

static buttons_page_t btn_page_1 = {
    {&btn_next_1, &btn_bt, &btn_audio, &btn_ptt, &btn_udp}
};
static buttons_page_t btn_page_2 = {
    {&btn_next_2, &btn_scan, &btn_connect, &btn_forget, &btn_exit}
};
static buttons_page_t btn_page_3 = {
    {&btn_next_3, &btn_mic}
};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .key_cb = key_cb,
};

dialog_t *dialog_bt = &dialog;

static lv_obj_t   *table = NULL;
static lv_obj_t   *label_adapter = NULL;
static lv_obj_t   *label_phone = NULL;
static lv_obj_t   *label_audio = NULL;
static lv_timer_t *timer_status = NULL;
static uint32_t    tick = 0;
static bool        udp_on = false;   /* cached ft8_udp_is_enabled() */
static char        last_activity[48];

/* ---- Table ------------------------------------------------------------- */

/* Address shown in each row, as last drawn. bt_dev's list changes order
 * while a Scan finds devices, so the highlight is kept on a device, not
 * on a row number: Connect must act on the device the operator picked. */
static char row_addr[BT_DEV_MAX][18];
static int  row_count = 0;

/* The device in the highlighted row, looked up by its address in the
 * current list. */
static bool selected_device(bt_dev_t *out)
{
    uint16_t row, col;
    int      i;

    if (table == NULL) {
        return false;
    }
    lv_table_get_selected_cell(table, &row, &col);
    if (row >= row_count) {
        return false;
    }
    for (i = 0; i < bt_dev_count(); i++) {
        if (bt_dev_get(i, out) && strcmp(out->addr, row_addr[row]) == 0) {
            return true;
        }
    }
    return false;
}

static void table_refresh(void)
{
    char     buf[96];
    char     sel[18] = "";
    int      n = bt_dev_count();
    int      i;
    uint16_t row, col;
    bool     seen = false;
    bt_dev_t dev;

    if (table == NULL) {
        return;
    }

    lv_table_get_selected_cell(table, &row, &col);
    if (row < row_count) {
        snprintf(sel, sizeof(sel), "%s", row_addr[row]);
    }

    if (n == 0) {
        row_count = 0;
        lv_table_set_row_cnt(table, 1);
        lv_table_set_cell_value(table, 0, 0,
                                "No devices - press Scan");
        return;
    }

    lv_table_set_row_cnt(table, (uint16_t)n);

    row_count = 0;
    for (i = 0; i < n; i++) {
        if (!bt_dev_get(i, &dev)) {
            break;
        }
        snprintf(row_addr[i], sizeof(row_addr[i]), "%s", dev.addr);
        row_count = i + 1;
        snprintf(buf, sizeof(buf), "%s%s%s",
                 dev.connected ? "* " : "  ",
                 dev.name,
                 dev.audio ? "  [audio]" : dev.input ? "  [kbd]" : "");
        lv_table_set_cell_value(table, (uint16_t)i, 0, buf);
        /* The highlighted device moved: the highlight goes with it.
         * LVGL 8.3 has no setter for the selected cell. */
        if (sel[0] != '\0' && strcmp(dev.addr, sel) == 0) {
            seen = true;
            if (i != row) {
                ((lv_table_t *)table)->row_act = (uint16_t)i;
                lv_obj_invalidate(table);
            }
        }
    }
    /* It left the list: no highlight rather than whatever moved into
     * its row. The next turn of the knob starts again at the top. */
    if (sel[0] != '\0' && !seen) {
        ((lv_table_t *)table)->row_act = LV_TABLE_CELL_NONE;
        ((lv_table_t *)table)->col_act = LV_TABLE_CELL_NONE;
        lv_obj_invalidate(table);
    }
}

/* ---- Status ------------------------------------------------------------ */

/* bluez_sink.50_C2_ED_96_B6_72.a2dp_sink is wider than the panel and
 * the interesting part is at the end. */
static const char *short_sink(const char *sink)
{
    const char *dot;

    if (sink == NULL || sink[0] == '\0') {
        return "none";
    }
    dot = strchr(sink, '.');
    return (dot != NULL) ? dot + 1 : sink;
}

static void status_refresh(void)
{
    uint32_t sent = 0;
    uint32_t dropped = 0;

    if (label_adapter == NULL) {
        return;
    }

    switch (bt_ctl_state()) {
        case BT_CTL_STARTING:
            lv_label_set_text(label_adapter, "Starting...");
            break;
        case BT_CTL_STOPPING:
            lv_label_set_text(label_adapter, "Stopping...");
            break;
        case BT_CTL_FAILED:
            lv_label_set_text(label_adapter, "Failed");
            break;
        case BT_CTL_ON: {
            /* Visible and pairable only for a few minutes after Scan. */
            int left = bt_dev_visible_s();

            if (left > 0) {
                lv_label_set_text_fmt(label_adapter, "%s\nPairing %d:%02d",
                                      bt_ctl_addr(), left / 60, left % 60);
            } else {
                lv_label_set_text_fmt(label_adapter, "%s\nHidden", bt_ctl_addr());
            }
            break;
        }
        default:
            /* WiFi off in the WiFi window cuts power to the shared
             * WiFi/BT chip; BT On powers it up again. */
            lv_label_set_text(label_adapter,
                              param_i_get(cfg.network.wifi_enabled())
                                  ? "Off" : "Off\nWiFi/BT chip off");
            break;
    }

    /* The UDP state comes from a cached flag; ft8_udp_is_enabled()
     * would re-read the config file every second. */
    bt_spp_stats(&sent, &dropped);
    if (bt_spp_state() == BT_SPP_CONNECTED) {
        lv_label_set_text_fmt(label_phone, "%s\nsent %u drop %u\nUDP: %s",
                              bt_spp_peer(), (unsigned)sent,
                              (unsigned)dropped, udp_on ? "On" : "Off");
    } else if (bt_spp_state() == BT_SPP_LISTENING) {
        lv_label_set_text_fmt(label_phone, "BT: waiting\nUDP: %s",
                              udp_on ? "On" : "Off");
    } else {
        lv_label_set_text_fmt(label_phone, "BT: -\nUDP: %s",
                              udp_on ? "On" : "Off");
    }

    /* While bt_dev pairs or connects, or after it failed, say so here
     * whatever the loopback state; before, it only showed with audio
     * Off and "No headphones" hid it. */
    if (bt_dev_activity()[0] != '\0' && bt_audio_state() != BT_AUDIO_ON) {
        lv_label_set_text(label_audio, bt_dev_activity());
        return;
    }

    switch (bt_audio_state()) {
        case BT_AUDIO_STARTING:
            lv_label_set_text(label_audio, "Starting...");
            break;
        case BT_AUDIO_STOPPING:
            lv_label_set_text(label_audio, "Stopping...");
            break;
        case BT_AUDIO_NO_SINK:
            lv_label_set_text(label_audio, "No headphones");
            break;
        case BT_AUDIO_FAILED:
            lv_label_set_text(label_audio, "Failed");
            break;
        case BT_AUDIO_ON:
            lv_label_set_text_fmt(label_audio, "%s\n%u ms",
                                  short_sink(bt_audio_sink()),
                                  (unsigned)bt_audio_measured_ms());
            break;
        default:
            if (bt_dev_activity()[0] != '\0') {
                lv_label_set_text(label_audio, bt_dev_activity());
            } else {
                lv_label_set_text(label_audio, "Off");
            }
            break;
    }
}

static void page_refresh(void)
{
    buttons_page_t *page = buttons_get_cur_page();
    int             i;

    if (page == NULL) {
        return;
    }
    for (i = 0; i < BUTTONS; i++) {
        if (page->items[i] != NULL && page->items[i]->disp_btn != NULL) {
            buttons_refresh(page->items[i]);
        }
    }
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;

    tick++;

    if (bt_ctl_state() == BT_CTL_ON) {
        if ((tick % DEV_EVERY) == 0) {
            bt_dev_refresh();
        }
        if ((tick % AUDIO_EVERY) == 0) {
            bt_audio_poll();
        }
    }

    table_refresh();
    status_refresh();

    /* bt_dev works in the background; a failed pair/connect would only
     * show in the small audio line, so say it out loud once. */
    if (strcmp(bt_dev_activity(), last_activity) != 0) {
        snprintf(last_activity, sizeof(last_activity), "%s",
                 bt_dev_activity());
        if (strncmp(last_activity, "Type ", 5) == 0 ||
            strncmp(last_activity, "Press the keyboard", 18) == 0) {
            /* Keyboard pairing: the code to type, or the pairing key. */
            msg_update_text_fmt("%s", last_activity);
        } else if (strcmp(last_activity, "Not found") == 0) {
            msg_update_text_fmt("Not found - is it in pairing mode?");
        } else if (strcmp(last_activity, "Pairing failed") == 0) {
            msg_update_text_fmt("Pairing failed - headset in pairing mode?");
        } else if (strcmp(last_activity, "Connect failed") == 0) {
            msg_update_text_fmt("Connect failed - headset busy? Try Forget");
        }
    }

    /* BTN_TEXT_FN without .subj does not repaint itself. Only the
     * buttons of the page on screen have a widget; buttons_refresh()
     * on the others would log a warning every tick. */
    page_refresh();
}

/* ---- Button labels ----------------------------------------------------- */

static const char *bt_label_getter(void)
{
    static char buf[24];
    const char *st;

    switch (bt_ctl_state()) {
        case BT_CTL_ON:       st = "On";    break;
        case BT_CTL_STARTING: st = "...";   break;
        case BT_CTL_STOPPING: st = "...";   break;
        case BT_CTL_FAILED:   st = "Error"; break;
        default:              st = "Off";   break;
    }
    snprintf(buf, sizeof(buf), "BT:\n%s", st);
    return buf;
}

static const char *audio_label_getter(void)
{
    static char buf[24];
    const char *st;

    switch (bt_audio_state()) {
        case BT_AUDIO_ON:       st = "On";    break;
        case BT_AUDIO_STARTING: st = "...";   break;
        case BT_AUDIO_STOPPING: st = "...";   break;
        case BT_AUDIO_NO_SINK:  st = "No BT"; break;
        case BT_AUDIO_FAILED:   st = "Error"; break;
        default:                st = "Off";   break;
    }
    snprintf(buf, sizeof(buf), "Audio:\n%s", st);
    return buf;
}

static const char *connect_label_getter(void)
{
    bt_dev_t dev;

    if (!selected_device(&dev)) {
        return "Connect";
    }
    return dev.connected ? "Disconnect" : "Connect";
}

static const char *scan_label_getter(void)
{
    return bt_dev_scanning() ? "Scanning" : "Scan";
}

static const char *ptt_label_getter(void)
{
    static char buf[24];

    switch (bt_ptt_state()) {
        case BT_PTT_KEYING:
        case BT_PTT_UNKEYING:
            snprintf(buf, sizeof(buf), "PTT:\n...");
            break;
        case BT_PTT_TX:
            snprintf(buf, sizeof(buf), "PTT:\nTX %us",
                     (unsigned)bt_ptt_seconds());
            break;
        case BT_PTT_NO_MIC:
            snprintf(buf, sizeof(buf), "PTT:\nNo mic");
            break;
        case BT_PTT_FAILED:
            snprintf(buf, sizeof(buf), "PTT:\nError");
            break;
        case BT_PTT_TIMEOUT:
            snprintf(buf, sizeof(buf), "PTT:\nTimeout");
            break;
        default:
            snprintf(buf, sizeof(buf), "PTT:\nRX");
            break;
    }
    return buf;
}

static const char *udp_label_getter(void)
{
    return udp_on ? "UDP:\nOn" : "UDP:\nOff";
}

/* ---- Actions ----------------------------------------------------------- */

static void bt_toggle_cb(button_data_t *item)
{
    bt_ctl_state_t st = bt_ctl_state();

    if (st == BT_CTL_OFF || st == BT_CTL_FAILED) {
        /* WiFi and Bluetooth are one chip on one power pin
         * (x6100_pin_wifi). With WiFi switched off in the WiFi window
         * the chip is unpowered, hci0 never appears and bt_up.sh used to
         * give up after 30 s with "Error". Power it up first, the way
         * the WiFi window does; bt_up.sh waits for hci0. */
        if (!param_i_get(cfg.network.wifi_enabled())) {
            wifi_power_on();
            msg_update_text_fmt("Bluetooth on - powering up the WiFi/BT chip");
        } else {
            msg_update_text_fmt("Bluetooth on");
        }
        bt_ctl_request(true);
    } else {
        /* The loopbacks feed and read a headset that is about to
         * disappear. */
        bt_ptt_request(false);
        bt_audio_request(false);
        bt_ctl_request(false);
        msg_update_text_fmt("Bluetooth off");
    }

    status_refresh();
    buttons_refresh(item);
}

static void audio_toggle_cb(button_data_t *item)
{
    bt_audio_state_t st = bt_audio_state();

    if (bt_ctl_state() != BT_CTL_ON) {
        msg_update_text_fmt("Turn Bluetooth on first");
        return;
    }

    if (st == BT_AUDIO_ON || st == BT_AUDIO_STARTING) {
        bt_audio_request(false);
        msg_update_text_fmt("Bluetooth audio off");
    } else {
        bt_audio_request(true);
        msg_update_text_fmt("Bluetooth audio on");
    }

    buttons_refresh(item);
}

static void connect_cb(button_data_t *item)
{
    bt_dev_t dev;

    if (bt_ctl_state() != BT_CTL_ON) {
        msg_update_text_fmt("Turn Bluetooth on first");
        return;
    }
    if (!selected_device(&dev)) {
        msg_update_text_fmt("No device selected");
        return;
    }

    if (dev.connected) {
        bt_dev_disconnect(dev.addr);
        msg_update_text_fmt("Disconnecting %s", dev.name);
    } else {
        bt_dev_connect(dev.addr);
        msg_update_text_fmt("Connecting %s", dev.name);
    }

    buttons_refresh(item);
}

static void scan_cb(button_data_t *item)
{
    if (bt_ctl_state() != BT_CTL_ON) {
        msg_update_text_fmt("Turn Bluetooth on first");
        return;
    }
    if (bt_dev_scanning()) {
        return;
    }

    bt_dev_scan();
    msg_update_text_fmt("Scanning - pairing open for 3 min (a phone can pair now)");
    buttons_refresh(item);
}

static void ptt_cb(button_data_t *item)
{
    bt_ptt_state_t st = bt_ptt_state();

    if (st == BT_PTT_TX || st == BT_PTT_KEYING) {
        bt_ptt_request(false);
        msg_update_text_fmt("PTT off");
    } else {
        if (bt_ctl_state() != BT_CTL_ON) {
            msg_update_text_fmt("Turn Bluetooth on first");
            return;
        }
        bt_ptt_request(true);
        msg_update_text_fmt("PTT on - BT microphone");
    }

    buttons_refresh(item);
}

static void forget_cb(button_data_t *item)
{
    bt_dev_t dev;

    (void)item;
    if (!selected_device(&dev)) {
        msg_update_text_fmt("No device selected");
        return;
    }
    msg_update_text_fmt("Hold to forget %s", dev.name);
}

static void forget_hold_cb(button_data_t *item)
{
    bt_dev_t dev;

    (void)item;
    if (bt_ctl_state() != BT_CTL_ON) {
        msg_update_text_fmt("Turn Bluetooth on first");
        return;
    }
    if (!selected_device(&dev)) {
        msg_update_text_fmt("No device selected");
        return;
    }
    /* The listening loopback may be playing into this very headset. */
    if (dev.audio) {
        bt_ptt_request(false);
        bt_audio_request(false);
    }
    bt_dev_forget(dev.addr);
    msg_update_text_fmt("Forgetting %s - pair again in pairing mode",
                        dev.name);
}

static void udp_toggle_cb(button_data_t *item)
{
    ft8_udp_set_enabled(!udp_on);
    udp_on = ft8_udp_is_enabled();

    if (udp_on) {
        msg_update_text_fmt("UDP on -> %s:%u", ft8_udp_host(),
                            ft8_udp_port());
    } else {
        msg_update_text_fmt("UDP off");
    }

    status_refresh();
    buttons_refresh(item);
}

static void exit_cb(button_data_t *item)
{
    (void)item;
    dialog_destruct(&dialog);
}

/* ---- Dialog ------------------------------------------------------------ */

static void key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct(&dialog);
            break;

        case LV_KEY_ENTER:
            connect_cb(&btn_connect);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static void construct_cb(lv_obj_t *parent)
{
    lv_obj_t *cont, *param_cont, *label;
    static lv_style_t style_val;

    dialog.obj = dialog_init(parent);

    udp_on = ft8_udp_is_enabled();
    buttons_load_page(&btn_page_1);

    cont = lv_obj_create(dialog.obj);
    lv_obj_remove_style(cont, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(cont, DIALOG_WIDTH, DIALOG_HEIGHT);
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);

    param_cont = lv_obj_create(cont);
    lv_obj_remove_style(param_cont, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(param_cont, PARAMS_WIDTH, DIALOG_HEIGHT);
    lv_obj_set_flex_flow(param_cont, LV_FLEX_FLOW_COLUMN);

    lv_style_init(&style_val);
    lv_style_set_pad_bottom(&style_val, 14);
    lv_style_set_pad_left(&style_val, 10);

    label = lv_label_create(param_cont);
    lv_label_set_text(label, "Adapter:");
    label_adapter = lv_label_create(param_cont);
    lv_label_set_text(label_adapter, "Off");
    lv_obj_add_style(label_adapter, &style_val, 0);

    label = lv_label_create(param_cont);
    lv_label_set_text(label, "FT8 packets:");
    label_phone = lv_label_create(param_cont);
    lv_label_set_text(label_phone, "-");
    lv_obj_add_style(label_phone, &style_val, 0);

    label = lv_label_create(param_cont);
    lv_label_set_text(label, "Listening audio:");
    label_audio = lv_label_create(param_cont);
    lv_label_set_text(label_audio, "Off");
    lv_obj_add_style(label_audio, &style_val, 0);

    table = lv_table_create(cont);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_row_cnt(table, 1);
    lv_table_set_col_width(table, 0, DIALOG_WIDTH - PARAMS_WIDTH - 2);
    lv_obj_set_height(table, DIALOG_HEIGHT);
    lv_obj_set_flex_grow(table, 1);

    lv_obj_remove_style(table, NULL, LV_PART_MAIN | LV_PART_ITEMS |
                        LV_STATE_ANY);
    lv_obj_set_style_bg_opa(table, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(table, 128, LV_PART_MAIN);

    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(table, lv_color_white(),
                              LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, LV_OPA_30,
                            LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_pad_top(table, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 5, LV_PART_ITEMS);

    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    lv_table_set_cell_value(table, 0, 0, " ");
    row_count = 0;

    tick = 0;
    last_activity[0] = '\0';
    bt_dev_init();
    bt_audio_init();

    if (bt_ctl_state() == BT_CTL_ON) {
        bt_dev_refresh();
        bt_audio_poll();
    }

    table_refresh();
    status_refresh();

    timer_status = lv_timer_create(status_timer_cb, TICK_MS, NULL);
}

static void destruct_cb(void)
{
    if (timer_status) {
        lv_timer_del(timer_status);
        timer_status = NULL;
    }

    /* Never leave the transmitter keyed with nothing on screen to
     * unkey it. */
    bt_ptt_request(false);

    /*
     * The adapter, the SPP server and the listening loopback keep running
     * after the window closes - that is the point of having On and Off.
     * Only the widgets go.
     */
    table = NULL;
    label_adapter = NULL;
    label_phone = NULL;
    label_audio = NULL;
}

/* ---- Mic gain ----------------------------------------------------------- */

static const char *mic_gain_label_getter(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), "Mic gain\n%+d dB", bt_audio_mic_gain());
    return buf;
}

static void mic_gain_step(button_data_t *item, int step)
{
    int db = bt_audio_set_mic_gain(bt_audio_mic_gain() + step);

    msg_update_text_fmt("Mic gain %+d dB (press +1, hold -1)", db);
    buttons_refresh(item);
}

static void mic_gain_up_cb(button_data_t *item)
{
    mic_gain_step(item, 1);
}

static void mic_gain_down_cb(button_data_t *item)
{
    mic_gain_step(item, -1);
}
