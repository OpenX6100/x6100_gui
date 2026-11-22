/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "keyboard.h"

#include "pubsub_ids.h"
#include "usb_devices.h"

#include "kbd_rollover.h"
#include "lv_drivers/indev/evdev.h"
#include "lv_drivers/indev/xkb.h"

#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glob.h>
#include <stdio.h>
#include <string.h>
#ifndef KBD_BT_GLOB
#include <libudev.h>
#endif

#ifndef KBD_GLOB
#define KBD_GLOB "/dev/input/by-path/*-kbd" /* tests point it elsewhere */
#endif

/* After a USB or input device comes or goes, look for the keyboard this
 * often, this many times: udev's "add" for the USB device comes before the
 * keyboard's /dev/input node and its by-path link exist, so looking once at once
 * finds nothing (1.0's USB thread passes the events on without the old
 * half-second pause between them). */
#define KBD_RESCAN_MS    500
#define KBD_RESCAN_TIMES 10


lv_group_t *keyboard_group;

static lv_indev_drv_t       indev_drv_2;
static bool                 ready = false;
static kbd_rollover_t       rollover;
static dev_t                kbd_rdev;  /* the keyboard opened: a new one (unplugged and back) opens again */
static ino_t                kbd_ino;
static bool                 bt_kbd_present; /* a Bluetooth keyboard connected, open or not */
static lv_timer_t          *rescan_timer;
static int                  rescans_left;

extern int evdev_fd; /* lv_drivers/indev/evdev.c, set by evdev_set_file() */

/* The next key event from the keyboard (USB or Bluetooth), through xkb (layout, Shift). */
static bool kbd_next(void *ctx, uint16_t *scancode, uint32_t *key, int *value) {
    (void)ctx;
    struct input_event in;
    while (read(evdev_fd, &in, sizeof(in)) > 0) {
        if (in.type != EV_KEY) continue;
        *scancode = in.code;
        *value    = in.value;
        *key      = xkb_process_key(in.code, in.value); /* also keeps Shift state */
        return true;
    }
    return false;
}

static void kbd_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    kbd_rollover_read(&rollover, kbd_next, NULL, data);
}

static bool glob_first(const char *pattern, char *path, size_t n) {
    glob_t globbuf;
    bool   found = glob(pattern, 0, NULL, &globbuf) == 0;
    if (found) snprintf(path, n, "%s", globbuf.gl_pathv[0]);
    globfree(&globbuf);
    return found;
}

/* A Bluetooth keyboard (HID over GATT through uhid, or classic HID) gets
 * no by-path link: udev makes those only for devices with a physical path
 * (USB, platform). Its event node, from udev's database: ID_INPUT_KEYBOARD
 * is what makes the by-path "-kbd" links too. */
#ifdef KBD_BT_GLOB /* tests: a folder of their own in place of udev's database */
static bool search_bt_kbd(char *path, size_t n) {
    return glob_first(KBD_BT_GLOB, path, n);
}
#else
static bool search_bt_kbd(char *path, size_t n) {
    struct udev *udev = udev_new();
    if (!udev) return false;
    struct udev_enumerate *en = udev_enumerate_new(udev);
    bool                   found = false;
    if (en) {
        udev_enumerate_add_match_subsystem(en, "input");
        udev_enumerate_add_match_sysname(en, "event*");
        udev_enumerate_add_match_property(en, "ID_INPUT_KEYBOARD", "1");
        udev_enumerate_scan_devices(en);
        struct udev_list_entry *entry;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
            struct udev_device *dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
            if (!dev) continue;
            const char *bus  = udev_device_get_property_value(dev, "ID_BUS");
            const char *node = udev_device_get_devnode(dev);
            if (bus && node && strcmp(bus, "bluetooth") == 0) {
                snprintf(path, n, "%s", node);
                found = true;
            }
            udev_device_unref(dev);
            if (found) break;
        }
        udev_enumerate_unref(en);
    }
    udev_unref(udev);
    return found;
}
#endif

/* A USB keyboard first (as before), else a Bluetooth one. */
static char* search_kbd_device() {
    static char path[256];
    if (glob_first(KBD_GLOB, path, sizeof(path))) return path;
    if (search_bt_kbd(path, sizeof(path))) return path;
    return NULL;
}

static void setup_kbd(char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !evdev_set_file(path)) {
        return;
    }
    kbd_rdev = st.st_rdev;
    kbd_ino  = st.st_ino;
    rollover = (kbd_rollover_t){0};

    if (indev_drv_2.type == LV_INDEV_TYPE_NONE) {
        lv_indev_drv_init(&indev_drv_2);
        indev_drv_2.type = LV_INDEV_TYPE_KEYPAD;
        indev_drv_2.read_cb = kbd_read;

        lv_indev_t *keyboard_indev = lv_indev_drv_register(&indev_drv_2);

        lv_indev_set_group(keyboard_indev, keyboard_group);
    }

    ready = true;
}

/* A keyboard there and not open (or a different one: unplugged and back):
 * open it; none there: not ready. */
static void check_kbd(void) {
    char        bt_path[256];
    char       *dev_path = search_kbd_device();
    struct stat st;

    /* For the status bar's icon: a Bluetooth keyboard is there even when
     * a USB one is the one open. */
    bt_kbd_present = search_bt_kbd(bt_path, sizeof(bt_path));
    if (dev_path && stat(dev_path, &st) == 0) {
        if (!ready || st.st_rdev != kbd_rdev || st.st_ino != kbd_ino) setup_kbd(dev_path);
        return;
    }
    ready = false;
}

/* All the checks, also once one finds it: an unplugged keyboard's link can
 * outlive the "remove" for a moment. */
static void rescan_cb(lv_timer_t *t) {
    check_kbd();
    if (--rescans_left <= 0) {
        lv_timer_del(t);
        rescan_timer = NULL;
    }
}

/* A USB device, or an input device (a Bluetooth keyboard connecting, or
 * coming back from sleep as a new node), came or went. */
static void on_device_change(void * s, lv_msg_t * msg) {
    (void)s;
    (void)msg;
    check_kbd();
    /* Look again for a few seconds: the keyboard's node comes later. */
    rescans_left = KBD_RESCAN_TIMES;
    if (!rescan_timer) rescan_timer = lv_timer_create(rescan_cb, KBD_RESCAN_MS, NULL);
}

void keyboard_init() {
    keyboard_group = lv_group_create();

    lv_msg_subscribe(MSG_USB_DEVICE_CHANGED, on_device_change, NULL);
    lv_msg_subscribe(MSG_INPUT_DEVICE_CHANGED, on_device_change, NULL);

    char bt_path[256];
    bt_kbd_present = search_bt_kbd(bt_path, sizeof(bt_path));

    char *path = search_kbd_device();
    if (!path)
        return;

    setup_kbd(path);
}

bool keyboard_ready() {
    return ready;
}

bool keyboard_bt_connected() {
    return bt_kbd_present;
}
