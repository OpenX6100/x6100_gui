/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_ctl.c - power and discoverability for the built-in Bluetooth adapter.
 *
 * Bring-up is done by /usr/bin/bt_up.sh (rootfs overlay) rather than in
 * C: the sequence is a pile of small tools with timing between them, and
 * a shell runs it the way it was tested at a prompt. Status is read
 * straight from the kernel with hci_devinfo(), not by parsing hciconfig
 * output.
 */

#include "bt_ctl.h"
#include "bt_spp.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#define BT_CONF     "/mnt/bt.conf"
#define BT_UP       "/usr/bin/bt_up.sh"
#define BT_DOWN     "/usr/bin/bt_down.sh"
/*
 * RFCOMM channel for the WSJT-X frames. x6100_gui listens on
 * channel 1 itself (CAT over Bluetooth, cat/cat.cpp) and bt_start.sh
 * publishes that as the Serial Port record, so ours moved to 2. bt_up.sh
 * publishes it under the Dial-up Networking class (UUID 0x1103) so the
 * phone can find it by UUID without knowing the channel number - two
 * records of the same Serial Port class could not be told apart.
 */
#define BT_CHANNEL  2

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            thread_valid;
    bool            quit;
    bool            want_on;
    bool            pending;
    bool            lost;       /* chip lost power: down without saving */
    bool            come_back;  /* ... and up again when it is back */
    bool            settle;     /* wait for bt_start.sh before bt_up.sh */
    ino_t           hci_ino;    /* sysfs inode of the hci0 we set up */
    bool            discoverable; /* cached by the worker, see below */

    bt_ctl_state_t  state;
    char            addr[18];
    char            name[32];
} S = {
    .lock  = PTHREAD_MUTEX_INITIALIZER,
    .cond  = PTHREAD_COND_INITIALIZER,
    .state = BT_CTL_OFF,
    .name  = "X6100",
};

/* ------------------------------------------------------------------ */

static void ctl_log(const char *fmt, ...);

static void conf_load(void)
{
    FILE *f = fopen(BT_CONF, "r");
    char  line[128];
    bool  enabled = false;

    if (f == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char *nl = strchr(line, '\n');

        if (nl) {
            *nl = '\0';
        }
        if (strncmp(line, "enabled=", 8) == 0) {
            enabled = (atoi(line + 8) != 0);
        } else if (strncmp(line, "name=", 5) == 0 && line[5] != '\0') {
            snprintf(S.name, sizeof(S.name), "%.31s", line + 5);
        }
    }
    fclose(f);

    if (enabled) {
        ctl_log("start: bt.conf enabled=1, Bluetooth on");
        S.want_on = true;
        S.pending = true;
    }
}

static void conf_save(bool enabled)
{
    FILE *f = fopen(BT_CONF, "w");

    if (f == NULL) {
        return;
    }
    fprintf(f, "# Bluetooth SPP bridge for FT8/FT4 packets\n");
    fprintf(f, "enabled=%d\n", enabled ? 1 : 0);
    fprintf(f, "name=%s\n", S.name);
    fclose(f);
}

/* What the adapter's On/Off did, and why it failed. /mnt is the SD
 * card's FAT partition: readable on a PC, survives a restart. */
#define CTL_LOG     "/mnt/bt_ctl.log"
#define CTL_LOG_MAX 131072

static void ctl_log(const char *fmt, ...)
{
    struct stat st;
    struct tm   tm;
    time_t      now = time(NULL);
    char        ts[24];
    va_list     ap;
    FILE       *f;

    f = fopen(CTL_LOG,
              (stat(CTL_LOG, &st) == 0 && st.st_size > CTL_LOG_MAX) ? "w" : "a");
    if (f == NULL) {
        return;
    }
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Runs a script the way a shell would and waits for it. Called only from
 * the worker thread, never from the UI thread. The script's own messages
 * (bt_up: ...) go to the same log. */
static bool run_script(const char *path, const char *arg)
{
    char cmd[320];
    int  rc;

    ctl_log("run %s", path);
    snprintf(cmd, sizeof(cmd), "%s '%s' %d >>" CTL_LOG " 2>&1", path, arg, BT_CHANNEL);
    rc = system(cmd);
    if (rc != 0) {
        ctl_log("%s failed (status %d)", path, rc);
    }
    return (rc == 0);
}

/* ------------------------------------------------------------------ */

static bool adapter_info(char *addr, size_t addr_len, bool *discoverable)
{
    struct hci_dev_info di;

    memset(&di, 0, sizeof(di));
    di.dev_id = 0;

    if (hci_devinfo(0, &di) < 0) {
        return false;
    }
    if (!hci_test_bit(HCI_UP, &di.flags)) {
        return false;
    }
    if (addr != NULL && addr_len >= 18) {
        ba2str(&di.bdaddr, addr);
    }
    if (discoverable != NULL) {
        *discoverable = hci_test_bit(HCI_ISCAN, &di.flags) != 0;
    }
    return true;
}

static void *worker(void *arg)
{
    (void)arg;

    for (;;) {
        bool on;

        pthread_mutex_lock(&S.lock);
        while (!S.quit && !S.pending) {
            pthread_cond_wait(&S.cond, &S.lock);
        }
        if (S.quit) {
            pthread_mutex_unlock(&S.lock);
            break;
        }
        S.pending = false;
        on = S.want_on;
        S.state = on ? BT_CTL_STARTING : BT_CTL_STOPPING;
        pthread_mutex_unlock(&S.lock);

        if (on) {
            bool        ok;
            bool        disc = false;
            char        addr[18] = "";
            struct stat st;

            if (S.settle) {
                /* The chip has just come back: udev runs bt_start.sh for
                 * it (power on, hidden, not pairable, the CAT record).
                 * Let that finish before bt_up.sh. */
                S.settle = false;
                sleep(5);
            }
            ok = run_script(BT_UP, S.name);

            if (ok) {
                ok = adapter_info(addr, sizeof(addr), &disc);
                if (!ok) {
                    ctl_log("adapter_info: hci0 not up");
                }
            }
            if (ok) {
                ok = bt_spp_start(BT_CHANNEL);
                if (!ok) {
                    ctl_log("SPP server on channel %d did not start: %s", BT_CHANNEL, strerror(errno));
                }
            }

            pthread_mutex_lock(&S.lock);
            if (S.pending) {
                /* The user changed their mind while we were starting.
                 * Leave the new request to the next pass. */
                pthread_mutex_unlock(&S.lock);
                continue;
            }
            snprintf(S.addr, sizeof(S.addr), "%s", addr);
            S.hci_ino = (stat("/sys/class/bluetooth/hci0", &st) == 0) ? st.st_ino : 0;
            S.discoverable = ok && disc;
            S.state = ok ? BT_CTL_ON : BT_CTL_FAILED;
            pthread_mutex_unlock(&S.lock);
            ctl_log("on: %s", ok ? "ok" : "FAILED");
            /* Only a success is remembered. A failed start used to save
             * enabled=0, so the radio came up with Bluetooth off after a
             * restart that the operator never asked for. */
            if (ok) {
                conf_save(true);
            }
        } else {
            bool lost;

            bt_spp_stop();
            pthread_mutex_lock(&S.lock);
            lost = S.lost;
            pthread_mutex_unlock(&S.lock);
            /* Lost with the chip's power: nothing to switch off, and
             * bt_down.sh (power off, rfkill block) could hit the chip
             * that is already coming back. */
            if (!lost) {
                run_script(BT_DOWN, S.name);
            }

            pthread_mutex_lock(&S.lock);
            S.addr[0] = '\0';
            S.discoverable = false;
            if (!S.pending) {
                S.state = BT_CTL_OFF;
            }
            lost = S.lost;
            S.lost = false;
            pthread_mutex_unlock(&S.lock);
            /* Switched off by the operator: remembered across reboots.
             * Lost with the chip's power: the saved choice stays. */
            if (!lost) {
                conf_save(false);
            }
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */

/* ft8_udp_init() runs only when the FT8 window opens, and the Bluetooth
 * window may well be opened first, so every entry point makes sure the
 * worker thread exists; starting it twice is a no-op. */
static void ensure_thread(void)
{
    bool start = false;

    pthread_mutex_lock(&S.lock);
    if (!S.thread_valid) {
        conf_load();
        S.quit = false;
        start = true;
    }
    pthread_mutex_unlock(&S.lock);

    if (!start) {
        return;
    }

    if (pthread_create(&S.thread, NULL, worker, NULL) == 0) {
        S.thread_valid = true;
        pthread_mutex_lock(&S.lock);
        if (S.pending) {
            pthread_cond_signal(&S.cond);
        }
        pthread_mutex_unlock(&S.lock);
    }
}

void bt_ctl_init(void)
{
    ensure_thread();
}

void bt_ctl_deinit(void)
{
    if (!S.thread_valid) {
        return;
    }

    bt_spp_stop();

    pthread_mutex_lock(&S.lock);
    S.quit = true;
    pthread_cond_signal(&S.cond);
    pthread_mutex_unlock(&S.lock);

    pthread_join(S.thread, NULL);
    S.thread_valid = false;
}

void bt_ctl_request(bool on)
{
    ensure_thread();
    ctl_log("request: Bluetooth %s", on ? "on" : "off");

    pthread_mutex_lock(&S.lock);
    S.come_back = false;        /* the operator decides from here */
    S.want_on = on;
    S.pending = true;
    pthread_cond_signal(&S.cond);
    pthread_mutex_unlock(&S.lock);
}

bt_ctl_state_t bt_ctl_state(void)
{
    bt_ctl_state_t st;

    ensure_thread();

    pthread_mutex_lock(&S.lock);
    st = S.state;
    /* WiFi switched off in the WiFi window cuts power to the shared
     * chip and hci0 disappears under us. Say Off and let the worker stop
     * the SPP server and tidy up, instead of showing On for a dead
     * adapter. The chip is often back (WiFi on again) before anyone
     * looks: then hci0 is there but it is a new one (another sysfs
     * inode), started by bt_start.sh as at boot - hidden, not pairable,
     * our SPP server gone. Either way it is set up again once it is back,
     * without switching Bluetooth off and on: that leaves this chip
     * deaf to LE keyboards. One stat() per call, cheap enough. */
    if (st == BT_CTL_ON && !S.pending) {
        struct stat sb;
        bool        present = (stat("/sys/class/bluetooth/hci0", &sb) == 0);

        if (!present || sb.st_ino != S.hci_ino) {
            ctl_log(present ? "hci0 replaced: chip lost power and came back, setting it up again"
                            : "hci0 gone from sysfs: chip lost power, Bluetooth off until it is back");
            S.want_on = false;
            S.pending = true;
            S.lost = true;
            S.come_back = true;
            S.addr[0] = '\0';
            S.state = st = BT_CTL_OFF;
            pthread_cond_signal(&S.cond);
        }
    } else if (st == BT_CTL_OFF && S.come_back && !S.pending &&
               access("/sys/class/bluetooth/hci0", F_OK) == 0) {
        ctl_log("hci0 back: Bluetooth on again");
        S.come_back = false;
        S.settle = true;
        S.want_on = true;
        S.pending = true;
        S.state = st = BT_CTL_STARTING;
        pthread_cond_signal(&S.cond);
    }
    pthread_mutex_unlock(&S.lock);
    return st;
}

const char *bt_ctl_addr(void)
{
    return S.addr;
}

const char *bt_ctl_name(void)
{
    return S.name;
}

void bt_ctl_set_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }
    pthread_mutex_lock(&S.lock);
    snprintf(S.name, sizeof(S.name), "%s", name);
    pthread_mutex_unlock(&S.lock);
    conf_save(bt_ctl_state() == BT_CTL_ON);
}

/* Cached: never asks the kernel from here. This is called by the
 * Bluetooth window's timer on the GUI thread, and it used to run
 * hci_devinfo() - socket, ioctl, close - right there. With a phone
 * reconnecting next to a headset the kernel's Bluetooth stack stalled
 * and that close() blocked in __lock_sock in uninterruptible sleep:
 * the whole radio froze, only a power cycle helped. The worker thread
 * reads the flag when it brings the adapter up (bt_up.sh makes it
 * discoverable and keeps it so). */
bool bt_ctl_discoverable(void)
{
    bool disc;

    pthread_mutex_lock(&S.lock);
    disc = S.discoverable;
    pthread_mutex_unlock(&S.lock);
    return disc;
}
