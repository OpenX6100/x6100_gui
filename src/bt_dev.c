/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_dev.c - known Bluetooth devices: list, scan, pair, connect, forget.
 *
 * Everything goes straight to BlueZ over D-Bus, as bt.cpp on the
 * ver_1.1 branch does, instead of driving bluetoothctl:
 *
 *   list     ObjectManager.GetManagedObjects, Device1 properties
 *   scan     Adapter1.StartDiscovery / StopDiscovery
 *   pair     Device1.Pair, then Trusted = true, then Device1.Connect
 *   connect  Device1.Connect           disconnect  Device1.Disconnect
 *   forget   Adapter1.RemoveDevice
 *
 * The result of each step is BlueZ's own reply (or its error name, e.g.
 * org.bluez.Error.AuthenticationFailed), not text guessed from a
 * terminal. An earlier version typed into an interactive bluetoothctl on
 * a pty and waited for "Pairing successful"; that broke whenever the
 * output came differently (a phone already connected, a headset that
 * bonded without printing it).
 *
 * Pairing needs an agent. A small one is registered here, on its own
 * thread and D-Bus connection, and made the default agent. It answers
 * pairing questions only while the pairing window is open (agent_call()
 * has the details) and registers again whenever bluetoothd (re)appears on
 * the bus. It is the only agent: bt_start.sh and bt_up.sh start none.
 *
 * All D-Bus calls of the worker are synchronous on a private connection
 * of the worker thread; the window only reads the cached list.
 *
 * Every step is logged to /mnt/bt_pair.log - the SD card's FAT
 * partition, readable on a PC without SSH.
 */

#include "bt_dev.h"

#include <gio/gio.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SCAN_LE_S       3       /* Scan: LE turn (keyboards) ...           */
#define SCAN_BREDR_S    5       /* ... then classic (headsets, phones) ... */
#define SCAN_ROUNDS     2       /* ... twice: 16 s, see do_scan()          */
#define PAIR_FIND_S     30      /* wait this long for a new device to show up */
#define PAIR_TIMEOUT_MS 60000
#define CONN_TIMEOUT_MS 30000
#define CALL_TIMEOUT_MS 10000

#define BLUEZ           "org.bluez"
#define ADAPTER_PATH    "/org/bluez/hci0"
#define AGENT_PATH      "/org/x6100/bt_agent"
#define AGENT_CAP       "NoInputNoOutput"
#define AGENT_CAP_KBD   "KeyboardDisplay"   /* only while pairing a keyboard */
#define AGENT_PIN       "0000"

/* Scan (or Connect on a device not paired yet) makes the radio visible
 * and lets devices pair for this long; the rest of the time it is hidden
 * and the agent refuses every pairing request. */
#define PAIR_WINDOW_S   180

#define PAIR_LOG        "/mnt/bt_pair.log"
#define PAIR_LOG_MAX    262144

typedef enum {
    CMD_NONE = 0,
    CMD_REFRESH,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_SCAN,
    CMD_FORGET,
} cmd_t;

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            thread_valid;
    bool            quit;

    cmd_t           cmd;
    char            cmd_addr[18];

    bt_dev_t        list[BT_DEV_MAX];
    int             count;
    bool            scanning;
    char            activity[48];
    time_t          open_until; /* pairing window, see pairing_open() */
    char            agent_cap[20];

    GDBusConnection *conn;      /* worker thread only */
} S = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static void agent_set_cap(const char *cap);
static void agent_make_default(void);

/* ---- Log ------------------------------------------------------------ */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void plog(const char *fmt, ...)
{
    struct stat st;
    struct tm   tm;
    time_t      now = time(NULL);
    char        ts[24];
    va_list     ap;
    FILE       *f;

    pthread_mutex_lock(&log_lock);
    f = fopen(PAIR_LOG,
              (stat(PAIR_LOG, &st) == 0 && st.st_size > PAIR_LOG_MAX) ? "w" : "a");
    if (f != NULL) {
        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(f, "%s ", ts);
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ---- Helpers ---------------------------------------------------------- */

static void set_activity(const char *what)
{
    pthread_mutex_lock(&S.lock);
    snprintf(S.activity, sizeof(S.activity), "%s", what);
    pthread_mutex_unlock(&S.lock);
}

static bool addr_ok(const char *addr)
{
    int i;

    if (addr == NULL || strlen(addr) != 17) {
        return false;
    }
    for (i = 0; i < 17; i++) {
        char c = addr[i];

        if ((i % 3) == 2) {
            if (c != ':') {
                return false;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* "AA:BB:..." -> "/org/bluez/hci0/dev_AA_BB_..." */
static void dev_path(const char *addr, char *out, size_t out_sz)
{
    char *p;

    snprintf(out, out_sz, ADAPTER_PATH "/dev_%.17s", addr);
    for (p = out; *p != '\0'; p++) {
        if (*p == ':') {
            *p = '_';
        }
    }
}

static void addr_to_underscores(const char *addr, char *out, size_t out_sz)
{
    size_t i;

    snprintf(out, out_sz, "%.17s", addr);
    for (i = 0; i < out_sz && out[i] != '\0'; i++) {
        if (out[i] == ':') {
            out[i] = '_';
        }
    }
}

/* Private system-bus connection of the worker. Reopened when bluetoothd
 * or the bus went away. */
static GDBusConnection *bus(void)
{
    GError *err = NULL;
    gchar  *addr;

    if (S.conn != NULL && !g_dbus_connection_is_closed(S.conn)) {
        return S.conn;
    }
    if (S.conn != NULL) {
        g_object_unref(S.conn);
        S.conn = NULL;
    }
    addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (addr == NULL) {
        plog("D-Bus: no system bus address: %s", err ? err->message : "?");
        g_clear_error(&err);
        return NULL;
    }
    S.conn = g_dbus_connection_new_for_address_sync(
        addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    g_free(addr);
    if (S.conn == NULL) {
        plog("D-Bus: cannot connect: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    return S.conn;
}

/* BlueZ error name of a failed call ("org.bluez.Error.AlreadyExists"). */
static void err_name(GError *err, char *out, size_t out_sz)
{
    gchar *remote;

    out[0] = '\0';
    if (err == NULL) {
        return;
    }
    remote = g_dbus_error_get_remote_error(err);
    if (remote != NULL) {
        snprintf(out, out_sz, "%s", remote);
        g_free(remote);
    }
}

/* Synchronous method call. Returns the reply (unref it) or NULL; on
 * failure the BlueZ error name goes to errname (may be NULL) and the
 * full message to the log. */
static GVariant *call(const char *path, const char *iface, const char *method,
                      GVariant *params, const GVariantType *reply_type,
                      int timeout_ms, char *errname, size_t errname_sz)
{
    GDBusConnection *c = bus();
    GError          *err = NULL;
    GVariant        *r;

    if (errname != NULL && errname_sz > 0) {
        errname[0] = '\0';
    }
    if (c == NULL) {
        if (params != NULL) {
            g_variant_unref(g_variant_ref_sink(params));
        }
        return NULL;
    }
    r = g_dbus_connection_call_sync(c, BLUEZ, path, iface, method, params,
                                    reply_type, G_DBUS_CALL_FLAGS_NONE,
                                    timeout_ms, NULL, &err);
    if (r == NULL) {
        char name[96];

        err_name(err, name, sizeof(name));
        if (errname != NULL && errname_sz > 0) {
            snprintf(errname, errname_sz, "%s", name);
        }
        /* The list and property polls fail quietly while BT is off or a
         * device is simply unknown. */
        if (strcmp(method, "GetManagedObjects") != 0 &&
            strcmp(method, "Get") != 0) {
            plog("%s %s: %s", method, path, err ? err->message : "?");
        }
        g_clear_error(&err);
    }
    return r;
}

static bool set_bool_prop(const char *path, const char *prop, bool val)
{
    GVariant *r = call(path, "org.freedesktop.DBus.Properties", "Set",
                       g_variant_new("(ssv)", "org.bluez.Device1", prop,
                                     g_variant_new_boolean(val)),
                       NULL, CALL_TIMEOUT_MS, NULL, 0);

    if (r == NULL) {
        return false;
    }
    g_variant_unref(r);
    return true;
}

static bool set_adapter_prop(const char *prop, GVariant *val)
{
    GVariant *r = call(ADAPTER_PATH, "org.freedesktop.DBus.Properties", "Set",
                       g_variant_new("(ssv)", "org.bluez.Adapter1", prop, val),
                       NULL, CALL_TIMEOUT_MS, NULL, 0);

    if (r == NULL) {
        return false;
    }
    g_variant_unref(r);
    return true;
}

static bool get_adapter_bool(const char *prop)
{
    GVariant *r;
    GVariant *v = NULL;
    bool      val = false;

    r = call(ADAPTER_PATH, "org.freedesktop.DBus.Properties", "Get",
             g_variant_new("(ss)", "org.bluez.Adapter1", prop),
             G_VARIANT_TYPE("(v)"), CALL_TIMEOUT_MS, NULL, 0);
    if (r == NULL) {
        return false;
    }
    g_variant_get(r, "(v)", &v);
    if (v != NULL && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
        val = g_variant_get_boolean(v);
    }
    if (v != NULL) {
        g_variant_unref(v);
    }
    g_variant_unref(r);
    return val;
}

/* Device1 boolean property; false when the device is unknown, and then
 * *exists (if given) is false too. */
static bool get_bool_prop(const char *addr, const char *prop, bool *exists)
{
    char      path[64];
    GVariant *r;
    GVariant *v = NULL;
    bool      val = false;

    dev_path(addr, path, sizeof(path));
    r = call(path, "org.freedesktop.DBus.Properties", "Get",
             g_variant_new("(ss)", "org.bluez.Device1", prop),
             G_VARIANT_TYPE("(v)"), CALL_TIMEOUT_MS, NULL, 0);
    if (exists != NULL) {
        *exists = (r != NULL);
    }
    if (r == NULL) {
        return false;
    }
    g_variant_get(r, "(v)", &v);
    if (v != NULL && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
        val = g_variant_get_boolean(v);
    }
    if (v != NULL) {
        g_variant_unref(v);
    }
    g_variant_unref(r);
    return val;
}

static bool device_known(const char *addr)
{
    bool exists = false;

    (void)get_bool_prop(addr, "Paired", &exists);
    return exists;
}

/* Start or stop an inquiry. Returns false only if it could not start.
 * `transport` (when starting): "auto" classic and LE together, "le" or
 * "bredr" one of them only.
 *
 * Classic and LE together is what this chip (RTL8723BU) does worst: while
 * a classic inquiry runs it hears hardly any LE advertising - none at all
 * after Bluetooth has been switched off and on with WiFi running. An LE
 * keyboard was then found only in the moment between two inquiries, or
 * not at all. So keyboards are looked for with "le", and Scan takes turns
 * (do_scan()). */
static bool discovery(bool on, const char *transport)
{
    char      en[96];
    GVariant *r;

    if (on) {
        /* Headsets and phones are BR/EDR, most keyboards sold now are LE
         * only. do_refresh() keeps LE beacons out of the list. Errors
         * here are harmless. */
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Transport", g_variant_new_string(transport));
        r = call(ADAPTER_PATH, "org.bluez.Adapter1", "SetDiscoveryFilter",
                 g_variant_new("(a{sv})", &b), NULL, CALL_TIMEOUT_MS, NULL, 0);
        if (r != NULL) {
            g_variant_unref(r);
        }
    }
    r = call(ADAPTER_PATH, "org.bluez.Adapter1",
             on ? "StartDiscovery" : "StopDiscovery", NULL, NULL,
             CALL_TIMEOUT_MS, en, sizeof(en));
    if (r != NULL) {
        g_variant_unref(r);
        return true;
    }
    /* "InProgress": another client's inquiry is running - fine too. */
    return !on || strstr(en, "InProgress") != NULL;
}

/* ---- Device list -------------------------------------------------------- */

/* ---- Phones -------------------------------------------------------------
 *
 * A phone (Class major 0x02 or Icon "phone") is only ever used for the
 * WSJT-X packets over SPP, and it opens that channel itself from its
 * app. So Connect never pages a phone; it only lets it in (see "One
 * device at a time"). Its audio profiles are left alone: refusing them
 * made Samsung phones abort the pairing ("incorrect PIN or passkey").
 */
static bool props_phone(guint32 cls, const char *icon)
{
    return ((cls >> 8) & 0x1F) == 0x02 ||
           (icon != NULL && strcmp(icon, "phone") == 0);
}

/* ---- Keyboards ----------------------------------------------------------
 *
 * A keyboard (or mouse) is told by its Class (major 0x05, peripheral),
 * its Icon ("input-..."), its Appearance (0x03C0-0x03FF, HID) or the HID
 * services it offers (0x1812 HID over GATT, 0x1124 classic HID). It is
 * paired differently (see do_pair_connect()) and is left out of "one
 * device at a time": it carries no audio and no SPP. */
static bool uuids_input(GVariant *uuids)
{
    GVariantIter it;
    const char  *u;

    if (uuids == NULL || !g_variant_is_of_type(uuids, G_VARIANT_TYPE("as"))) {
        return false;
    }
    g_variant_iter_init(&it, uuids);
    while (g_variant_iter_next(&it, "&s", &u)) {
        if (g_ascii_strncasecmp(u, "00001812", 8) == 0 ||
            g_ascii_strncasecmp(u, "00001124", 8) == 0) {
            return true;
        }
    }
    return false;
}

/* `dev`: a Device1 property dictionary (a{sv}). */
static bool props_input(GVariant *dev)
{
    guint32     cls = 0;
    guint16     appearance = 0;
    const char *icon = NULL;
    GVariant   *uuids;
    bool        input;

    g_variant_lookup(dev, "Class", "u", &cls);
    g_variant_lookup(dev, "Appearance", "q", &appearance);
    g_variant_lookup(dev, "Icon", "&s", &icon);
    input = ((cls >> 8) & 0x1F) == 0x05 ||
            (appearance & 0xFFC0) == 0x03C0 ||
            (icon != NULL && strncmp(icon, "input-", 6) == 0);
    if (!input) {
        uuids = g_variant_lookup_value(dev, "UUIDs", G_VARIANT_TYPE("as"));
        input = uuids_input(uuids);
        if (uuids != NULL) {
            g_variant_unref(uuids);
        }
    }
    return input;
}

/* Device1 properties of `path` asked over `c` (unref), or NULL. */
static GVariant *dev_props(GDBusConnection *c, const char *path)
{
    GVariant *r;
    GVariant *props;

    if (c == NULL || path == NULL) {
        return NULL;
    }
    r = g_dbus_connection_call_sync(c, BLUEZ, path,
                                    "org.freedesktop.DBus.Properties", "GetAll",
                                    g_variant_new("(s)", "org.bluez.Device1"),
                                    G_VARIANT_TYPE("(a{sv})"),
                                    G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS,
                                    NULL, NULL);
    if (r == NULL) {
        return NULL;
    }
    props = g_variant_get_child_value(r, 0);
    g_variant_unref(r);
    return props;
}

/* Asks bluez over `c` whether the device at `path` is a phone. */
static bool path_is_phone(GDBusConnection *c, const char *path)
{
    GVariant   *props = dev_props(c, path);
    guint32     cls = 0;
    const char *icon = NULL;
    bool        phone;

    if (props == NULL) {
        return false;
    }
    g_variant_lookup(props, "Class", "u", &cls);
    g_variant_lookup(props, "Icon", "&s", &icon);
    phone = props_phone(cls, icon);
    g_variant_unref(props);
    return phone;
}

/* ... and whether it is a keyboard or another input device. */
static bool path_is_input(GDBusConnection *c, const char *path)
{
    GVariant *props = dev_props(c, path);
    bool      input;

    if (props == NULL) {
        return false;
    }
    input = props_input(props);
    g_variant_unref(props);
    return input;
}

/* Paired with this radio (bluez has its key)? */
static bool path_is_paired(GDBusConnection *c, const char *path)
{
    GVariant *props = dev_props(c, path);
    gboolean  paired = FALSE;

    if (props == NULL) {
        return false;
    }
    g_variant_lookup(props, "Paired", "b", &paired);
    g_variant_unref(props);
    return paired;
}

/* ---- Pairing window ------------------------------------------------------
 *
 * The radio has no screen of its own for pairing questions, and an agent
 * that accepts everything all the time let anyone in range pair with it.
 * So pairing is open only for PAIR_WINDOW_S after Scan (or after Connect
 * on a device that still has to be paired): the adapter is discoverable
 * and pairable for that time, and the agent accepts pairing requests only
 * then. Devices already paired connect as before at any time. */
static bool pairing_is_open(void)
{
    bool open;

    pthread_mutex_lock(&S.lock);
    open = time(NULL) < S.open_until;
    pthread_mutex_unlock(&S.lock);
    return open;
}

/* Worker thread. */
static void pairing_open(void)
{
    pthread_mutex_lock(&S.lock);
    S.open_until = time(NULL) + PAIR_WINDOW_S;
    pthread_mutex_unlock(&S.lock);

    /* bluez restarts its pairable timer only when PairableTimeout is set
     * or Pairable changes, so set the timeout on every Scan: a second
     * Scan while still pairable would otherwise close pairing on the
     * first one's timer (main.conf has 180 s for both). */
    (void)set_adapter_prop("PairableTimeout", g_variant_new_uint32(PAIR_WINDOW_S));
    (void)set_adapter_prop("Pairable", g_variant_new_boolean(TRUE));
    (void)set_adapter_prop("DiscoverableTimeout", g_variant_new_uint32(PAIR_WINDOW_S));
    (void)set_adapter_prop("Discoverable", g_variant_new_boolean(TRUE));
    plog("pairing open: visible and pairable for %d s", PAIR_WINDOW_S);
}

/* Worker thread, on every refresh: once the window is over, hidden and
 * not pairable again, whatever turned them on (bluez also turns both off
 * after main.conf's timeouts). */
static void pairing_enforce(void)
{
    if (pairing_is_open()) {
        return;
    }
    if (get_adapter_bool("Discoverable") || get_adapter_bool("Pairable")) {
        (void)set_adapter_prop("Discoverable", g_variant_new_boolean(FALSE));
        (void)set_adapter_prop("Pairable", g_variant_new_boolean(FALSE));
        plog("pairing closed: hidden, not pairable (Scan opens it again)");
    }
}

static void do_refresh(void)
{
    bt_dev_t      tmp[BT_DEV_MAX];
    bt_dev_t      rest[BT_DEV_MAX];
    int           n = 0;
    int           nrest = 0;
    GVariant     *r;
    GVariantIter *objs = NULL;
    const char   *path;
    GVariant     *ifaces;
    FILE         *p;
    char          line[256];
    int           i;

    memset(tmp, 0, sizeof(tmp));
    memset(rest, 0, sizeof(rest));

    r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
             NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), CALL_TIMEOUT_MS, NULL, 0);
    if (r != NULL) {
        g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
        while (g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            GVariant   *dev;
            const char *addr = NULL;
            const char *name = NULL;
            gboolean    paired = FALSE;
            gboolean    connected = FALSE;
            gboolean    blocked = FALSE;
            gboolean    has_class;
            GVariant   *cls_v;
            bool        input;
            bt_dev_t    d;

            dev = g_variant_lookup_value(ifaces, "org.bluez.Device1",
                                         G_VARIANT_TYPE("a{sv}"));
            if (dev != NULL &&
                strncmp(path, ADAPTER_PATH "/", strlen(ADAPTER_PATH) + 1) == 0) {
                g_variant_lookup(dev, "Address", "&s", &addr);
                /* Name is there only when the device told us its name;
                 * nameless passers-by from an inquiry are left out. */
                g_variant_lookup(dev, "Name", "&s", &name);
                g_variant_lookup(dev, "Paired", "b", &paired);
                g_variant_lookup(dev, "Connected", "b", &connected);
                g_variant_lookup(dev, "Blocked", "b", &blocked);
                /* Class comes only from a classic (BR/EDR) inquiry. A
                 * device seen only over LE is listed only when it is a
                 * keyboard or other input device: the rest are beacons,
                 * watches, TVs. */
                cls_v = g_variant_lookup_value(dev, "Class", NULL);
                has_class = (cls_v != NULL);
                if (cls_v != NULL) {
                    g_variant_unref(cls_v);
                }
                input = props_input(dev);

                if (addr != NULL && addr_ok(addr) &&
                    (paired || connected ||
                     (name != NULL && (has_class || input)))) {
                    memset(&d, 0, sizeof(d));
                    snprintf(d.addr, sizeof(d.addr), "%s", addr);
                    /* "(off)": Blocked, see make_exclusive() - Connect
                     * brings it back. */
                    if (blocked) {
                        snprintf(d.name, sizeof(d.name), "%.25s (off)", name ? name : addr);
                    } else {
                        snprintf(d.name, sizeof(d.name), "%s", name ? name : addr);
                    }
                    d.connected = connected;
                    d.input = input;
                    /* Paired and connected first, then what a scan found. */
                    if ((paired || connected) && n < BT_DEV_MAX) {
                        tmp[n++] = d;
                    } else if (nrest < BT_DEV_MAX) {
                        rest[nrest++] = d;
                    }
                }
            }
            if (dev != NULL) {
                g_variant_unref(dev);
            }
            g_variant_unref(ifaces);
        }
        g_variant_iter_free(objs);
        g_variant_unref(r);
    }
    for (i = 0; i < nrest && n < BT_DEV_MAX; i++) {
        tmp[n++] = rest[i];
    }

    /* "audio": the radio can play to it - PulseAudio has a sink for it in
     * a headset/speaker profile, the same test bt_audio.c uses to pick
     * where listening goes. A phone also gets a PulseAudio card, but
     * only as a source (a2dp_source, its music) or as an audio gateway
     * (handsfree_audio_gateway, calls): never "audio" here. */
    p = popen("pactl list short sinks 2>/dev/null", "r");
    if (p != NULL) {
        while (fgets(line, sizeof(line), p) != NULL) {
            if (strstr(line, "bluez_sink.") == NULL ||
                (strstr(line, ".a2dp_sink") == NULL &&
                 strstr(line, ".handsfree_head_unit") == NULL &&
                 strstr(line, ".headset_head_unit") == NULL)) {
                continue;
            }
            for (i = 0; i < n; i++) {
                char under[24];

                addr_to_underscores(tmp[i].addr, under, sizeof(under));
                if (strstr(line, under) != NULL) {
                    tmp[i].audio = true;
                }
            }
        }
        pclose(p);
    }

    pthread_mutex_lock(&S.lock);
    memcpy(S.list, tmp, sizeof(tmp));
    S.count = n;
    pthread_mutex_unlock(&S.lock);

    pairing_enforce();
    {
        static unsigned n_refresh;

        if ((n_refresh++ % 10) == 0) {
            agent_make_default();
        }
    }
}

/* ---- Pair / connect ------------------------------------------------------ */

typedef enum {
    PAIR_OK = 0,
    PAIR_NOT_FOUND,
    PAIR_FAILED,
    PAIR_CONNECT_FAILED,
} pair_result_t;

/* Device1.Connect, once more after a pause if the first one fails: some
 * headsets refuse the profile connection while still settling after
 * pairing (br-connection-profile-unavailable, InProgress). */
static bool connect_dev(const char *path)
{
    char      en[96];
    GVariant *r;
    int       attempt;

    for (attempt = 1; attempt <= 2; attempt++) {
        r = call(path, "org.bluez.Device1", "Connect", NULL, NULL,
                 CONN_TIMEOUT_MS, en, sizeof(en));
        if (r != NULL) {
            g_variant_unref(r);
            plog("Connect: ok");
            return true;
        }
        if (strstr(en, "AlreadyConnected") != NULL) {
            plog("Connect: already connected");
            return true;
        }
        if (attempt == 1) {
            sleep(2);
        }
    }
    return false;
}

/* Wait for a device to (re)appear in an inquiry; true when it is there. */
static bool wait_known(const char *addr, bool *scanned)
{
    int i;

    *scanned = discovery(true, "auto");
    plog("discovery %s, waiting up to %d s", *scanned ? "on" : "FAILED", PAIR_FIND_S);
    for (i = 0; i < PAIR_FIND_S && !device_known(addr); i++) {
        sleep(1);
    }
    if (!device_known(addr)) {
        return false;
    }
    plog("found after %d s", i);
    return true;
}

/* A keyboard not paired yet, at `addr` or under the same name: many
 * keyboards take a new address each time their pairing key is pressed
 * (seen: C0:53:A4:C5:A3:D7, D8, D9, ...), so the entry Connect was
 * pressed on may be gone while the keyboard advertises under the next
 * one. A keyboard without a name is looked for at `addr` only. The
 * address found goes to `out`. */
static bool find_keyboard(const char *name, const char *addr, char *out, size_t out_sz)
{
    GVariant     *r;
    GVariantIter *objs = NULL;
    const char   *path;
    GVariant     *ifaces;
    bool          found = false;

    r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
             NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), CALL_TIMEOUT_MS, NULL, 0);
    if (r == NULL) {
        return false;
    }
    g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
    while (!found && g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant   *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1",
                                                 G_VARIANT_TYPE("a{sv}"));
        const char *a = NULL;
        const char *n = NULL;
        gboolean    paired = FALSE;

        if (dev != NULL) {
            g_variant_lookup(dev, "Address", "&s", &a);
            g_variant_lookup(dev, "Name", "&s", &n);
            g_variant_lookup(dev, "Paired", "b", &paired);
            if (a != NULL && addr_ok(a) && !paired &&
                (g_ascii_strcasecmp(a, addr) == 0 ||
                 (name[0] != '\0' && n != NULL && strcmp(n, name) == 0 &&
                  props_input(dev)))) {
                snprintf(out, out_sz, "%s", a);
                found = true;
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(objs);
    g_variant_unref(r);
    return found;
}

/* Wait for a keyboard to (re)appear, LE only (see discovery()); polled
 * every half second, as its pairing window may be short. */
static bool wait_keyboard(const char *name, const char *addr, char *out,
                          size_t out_sz, bool *scanned)
{
    int i;

    *scanned = discovery(true, "le");
    plog("discovery (LE) %s, waiting up to %d s", *scanned ? "on" : "FAILED", PAIR_FIND_S);
    for (i = 0; i < PAIR_FIND_S * 2; i++) {
        if (find_keyboard(name, addr, out, out_sz)) {
            plog("found %s after %d.%d s", out, i / 2, (i % 2) * 5);
            return true;
        }
        usleep(500000);
    }
    return false;
}

/* After a keyboard paired at a new address: its old pairings under the
 * same name go. Pressing the pairing key made the keyboard drop them, so
 * they can never connect again, and Connect on one only waited a minute
 * for "Connect failed". One that is connected stays (a second keyboard of
 * the same model). */
static void forget_old_keyboards(const char *name, const char *keep)
{
    GVariant     *r;
    GVariantIter *objs = NULL;
    const char   *path;
    GVariant     *ifaces;

    if (name[0] == '\0') {
        return;
    }
    r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
             NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), CALL_TIMEOUT_MS, NULL, 0);
    if (r == NULL) {
        return;
    }
    g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
    while (g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant   *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1",
                                                 G_VARIANT_TYPE("a{sv}"));
        const char *a = NULL;
        const char *n = NULL;
        gboolean    paired = FALSE;
        gboolean    connected = FALSE;

        if (dev != NULL) {
            g_variant_lookup(dev, "Address", "&s", &a);
            g_variant_lookup(dev, "Name", "&s", &n);
            g_variant_lookup(dev, "Paired", "b", &paired);
            g_variant_lookup(dev, "Connected", "b", &connected);
            if (a != NULL && addr_ok(a) && g_ascii_strcasecmp(a, keep) != 0 &&
                paired && !connected && n != NULL && strcmp(n, name) == 0 &&
                props_input(dev)) {
                GVariant *rr = call(ADAPTER_PATH, "org.bluez.Adapter1", "RemoveDevice",
                                    g_variant_new("(o)", path), NULL,
                                    CALL_TIMEOUT_MS, NULL, 0);

                plog("keyboard: old pairing %s forgotten: %s", a, rr != NULL ? "ok" : "failed");
                if (rr != NULL) {
                    g_variant_unref(rr);
                }
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(objs);
    g_variant_unref(r);
}

static pair_result_t do_pair_connect(const char *addr)
{
    char      path[64];
    char      en[96];
    char      kbd_addr[18];
    char      name[64] = "";
    GVariant *r;
    bool      scanned = false;
    bool      input;

    dev_path(addr, path, sizeof(path));
    plog("=== pair %s", addr);
    pairing_open();

    input = device_known(addr) && path_is_input(bus(), path);
    if (input) {
        GVariant   *props = dev_props(bus(), path);
        const char *n = NULL;

        if (props != NULL) {
            if (g_variant_lookup(props, "Name", "&s", &n) && n != NULL) {
                snprintf(name, sizeof(name), "%s", n);
            }
            g_variant_unref(props);
        }
        /* Many keyboards accept a pairing only for a few seconds after
         * their pairing key is pressed, and an entry left from an earlier
         * Scan says nothing about whether that is now. So the old entry
         * goes, and Pair() is sent the moment the keyboard shows up
         * again - at its old address or a new one, see find_keyboard(). */
        r = call(ADAPTER_PATH, "org.bluez.Adapter1", "RemoveDevice",
                 g_variant_new("(o)", path), NULL, CALL_TIMEOUT_MS, NULL, 0);
        if (r != NULL) {
            g_variant_unref(r);
        }
        set_activity("Press the keyboard's pairing key");
        plog("keyboard: waiting for \"%s\" to advertise again", name);
        if (!wait_keyboard(name, addr, kbd_addr, sizeof(kbd_addr), &scanned)) {
            plog("result: keyboard not found - pairing key pressed?");
            if (scanned) {
                discovery(false, NULL);
            }
            return PAIR_NOT_FOUND;
        }
        addr = kbd_addr;
        dev_path(addr, path, sizeof(path));
    } else if (!device_known(addr)) {
        /* The device must be known to bluez. A Scan a moment ago leaves it
         * there for a while; otherwise look for it now (pairing mode). */
        set_activity("searching...");
        if (!wait_known(addr, &scanned)) {
            plog("result: not found - is the headset in pairing mode?");
            if (scanned) {
                discovery(false, NULL);
            }
            return PAIR_NOT_FOUND;
        }
        input = path_is_input(bus(), path);
    }
    /* An inquiry running alongside slows paging and pairing down. */
    if (scanned) {
        discovery(false, NULL);
    }

    /* A keyboard often pairs with passkey entry: the radio shows a number
     * (DisplayPasskey, see the agent), the operator types it on the
     * keyboard. bluez offers that only to an agent that can display, and
     * only for this pairing: headsets and phones stay with the Just Works
     * pairing that works with them. */
    if (input) {
        agent_set_cap(AGENT_CAP_KBD);
    }
    set_activity("pairing...");
    r = call(path, "org.bluez.Device1", "Pair", NULL, NULL, PAIR_TIMEOUT_MS,
             en, sizeof(en));
    if (input) {
        agent_set_cap(AGENT_CAP);
    }
    if (r != NULL) {
        g_variant_unref(r);
        plog("Pair: ok");
        if (input) {
            forget_old_keyboards(name, addr);
        }
    } else if (strstr(en, "AlreadyExists") != NULL) {
        plog("Pair: already paired");
    } else {
        plog("result: pair failed (%s)", en[0] ? en : "no reply");
        return PAIR_FAILED;
    }

    /* Trusted: it may reconnect on its own later, and bluez accepts its
     * profile connections without asking the agent. */
    plog("Trusted: %s", set_bool_prop(path, "Trusted", true) ? "ok" : "failed");

    if (path_is_phone(bus(), path)) {
        plog("result: phone paired - connect from the phone app");
        return PAIR_OK;
    }

    set_activity("connecting...");
    if (!connect_dev(path)) {
        plog("result: connect failed");
        return PAIR_CONNECT_FAILED;
    }
    plog("result: connected%s", input ? " (keyboard)" : "");
    return PAIR_OK;
}

/* ---- One device at a time -------------------------------------------
 *
 * This radio's Bluetooth chip (RTL8723BU, one antenna shared with WiFi)
 * cannot hold a headset's audio and a phone's SPP at the same time:
 * whichever comes second gets its link but not its channels, and a
 * phone keeps reconnecting by itself. So the window works one device at
 * a time, with bluez's Blocked flag:
 *   Connect X     every other paired device is Blocked (bluez drops it
 *                 and refuses it from then on, also its automatic
 *                 reconnects), X is unblocked and connected;
 *   Disconnect X  X is Blocked, so it stays away until Connect.
 * Blocked is stored by bluez and survives a restart: after power-up only
 * the device used last comes back. Forget clears it with the pairing.
 * Keyboards are left out both ways: they carry no audio and no SPP, and
 * connecting one blocks nothing. */
static void set_blocked(const char *dev_addr, bool blocked)
{
    char path[64];

    dev_path(dev_addr, path, sizeof(path));
    if (set_bool_prop(path, "Blocked", blocked)) {
        plog("%s %s", blocked ? "blocked" : "unblocked", dev_addr);
    }
}

static void make_exclusive(const char *addr, bool input)
{
    GVariant     *r;
    GVariantIter *objs = NULL;
    const char   *path;
    GVariant     *ifaces;
    bool          dropped = false;

    if (input) {
        set_blocked(addr, false);
        return;
    }
    r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
             NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), CALL_TIMEOUT_MS, NULL, 0);
    if (r != NULL) {
        g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
        while (g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            GVariant   *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1",
                                                     G_VARIANT_TYPE("a{sv}"));
            const char *other = NULL;
            gboolean    paired = FALSE;
            gboolean    connected = FALSE;
            gboolean    blocked = FALSE;

            if (dev != NULL) {
                g_variant_lookup(dev, "Address", "&s", &other);
                g_variant_lookup(dev, "Paired", "b", &paired);
                g_variant_lookup(dev, "Connected", "b", &connected);
                g_variant_lookup(dev, "Blocked", "b", &blocked);
                if (other != NULL && addr_ok(other) &&
                    g_ascii_strcasecmp(other, addr) != 0 &&
                    (paired || connected) && !blocked && !props_input(dev)) {
                    set_blocked(other, true);
                    dropped = dropped || connected;
                }
                g_variant_unref(dev);
            }
            g_variant_unref(ifaces);
        }
        g_variant_iter_free(objs);
        g_variant_unref(r);
    }
    set_blocked(addr, false);
    if (dropped) {
        sleep(2);       /* let the old links go before paging the new one */
    }
}

static void do_connect(const char *addr, bool connect)
{
    char path[64];

    if (!addr_ok(addr)) {
        return;
    }
    dev_path(addr, path, sizeof(path));

    if (connect) {
        bool input = path_is_input(bus(), path);

        set_activity("connecting...");
        plog("=== connect %s%s", addr, input ? " (keyboard)" : " (others get blocked)");
        make_exclusive(addr, input);

        /* Pair first when needed. The button says Connect because that
         * is what the operator wants; pairing is plumbing. It needs the
         * headset in pairing mode, and when that is missing the state
         * line says so instead of the device silently vanishing. */
        if (!get_bool_prop(addr, "Paired", NULL)) {
            pair_result_t pr = do_pair_connect(addr);

            sleep(1);
            do_refresh();
            switch (pr) {
                case PAIR_OK:
                    /* PulseAudio makes its card on the connection
                     * event; look again in a moment. */
                    sleep(2);
                    do_refresh();
                    set_activity("");
                    break;
                case PAIR_NOT_FOUND:
                    set_activity("Not found");
                    break;
                case PAIR_CONNECT_FAILED:
                    set_activity("Connect failed");
                    break;
                default:
                    set_activity("Pairing failed");
                    break;
            }
            return;
        }

        (void)set_bool_prop(path, "Trusted", true);
        if (path_is_phone(bus(), path)) {
            /* The phone opens the SPP channel itself from its app; the
             * radio paging the phone's own services only failed
             * (br-connection-unknown). Unblocked is all it needs. */
            plog("phone: unblocked - connect from the phone app");
            set_activity("Phone: connect from app");
            sleep(4);
            do_refresh();
            set_activity("");
            return;
        }
        if (!connect_dev(path)) {
            /* Typical cause: the headset refuses (busy with two other
             * sources, or it lost its key for us -> Forget, pair again). */
            plog("result: connect failed");
            sleep(1);
            do_refresh();
            set_activity("Connect failed");
            return;
        }
    } else {
        GVariant *r;

        set_activity("disconnecting...");
        plog("=== disconnect %s", addr);
        r = call(path, "org.bluez.Device1", "Disconnect", NULL, NULL,
                 CALL_TIMEOUT_MS, NULL, 0);
        if (r != NULL) {
            g_variant_unref(r);
        }
        /* A phone reconnects by itself within seconds: keep it out until
         * Connect is pressed for it again. */
        set_blocked(addr, true);
    }

    /* PulseAudio only notices a device when the connection event
     * arrives, so give it a moment before looking for its card. */
    sleep(2);
    do_refresh();
    set_activity("");
}

/* Drop the pairing on our side. Needed when the headset has lost or
 * replaced its key for this radio (reset, paired elsewhere): bluez
 * keeps offering the old key, the headset refuses, and nothing short of
 * forgetting it and pairing afresh with the headset in pairing mode
 * gets out of that. */
static void do_forget(const char *addr)
{
    char      path[64];
    GVariant *r;

    if (!addr_ok(addr)) {
        return;
    }
    dev_path(addr, path, sizeof(path));

    set_activity("removing...");
    plog("=== forget %s", addr);
    r = call(path, "org.bluez.Device1", "Disconnect", NULL, NULL,
             CALL_TIMEOUT_MS, NULL, 0);
    if (r != NULL) {
        g_variant_unref(r);
    }
    r = call(ADAPTER_PATH, "org.bluez.Adapter1", "RemoveDevice",
             g_variant_new("(o)", path), NULL, CALL_TIMEOUT_MS, NULL, 0);
    plog("RemoveDevice: %s", r != NULL ? "ok" : "failed");
    if (r != NULL) {
        g_variant_unref(r);
    }

    do_refresh();
    set_activity("");
}

/* Worker thread: Connect, Disconnect or Forget is waiting. (The timer's
 * refreshes are not posted while a Scan runs.) */
static bool cmd_waiting(void)
{
    bool w;

    pthread_mutex_lock(&S.lock);
    w = S.cmd != CMD_NONE && S.cmd != CMD_REFRESH;
    pthread_mutex_unlock(&S.lock);
    return w;
}

static void do_scan(void)
{
    int i;

    pthread_mutex_lock(&S.lock);
    S.scanning = true;
    pthread_mutex_unlock(&S.lock);
    set_activity("scanning...");
    /* Scan is also how a phone gets to pair from its side. */
    pairing_open();

    /* LE and classic in turns, not together (see discovery()): an LE turn
     * is never far off, so it doesn't matter whether the keyboard's
     * pairing key was pressed before Scan or during it. A classic turn is
     * long enough for an inquiry to reach a headset. The list is refreshed
     * after every turn so devices appear as they are found. */
    /* Connect pressed during the Scan ends it at once: a keyboard may
     * advertise for a few seconds only, and the worker does one thing at
     * a time. */
    for (i = 0; i < SCAN_ROUNDS * 2 && !cmd_waiting(); i++) {
        bool le = (i % 2) == 0;

        if (discovery(true, le ? "le" : "bredr")) {
            int q;

            for (q = 0; q < (le ? SCAN_LE_S : SCAN_BREDR_S) * 4 && !cmd_waiting(); q++) {
                usleep(250000);
            }
            discovery(false, NULL);
        }
        do_refresh();
    }
    if (cmd_waiting()) {
        plog("scan: ended early for the next command");
    }

    pthread_mutex_lock(&S.lock);
    S.scanning = false;
    pthread_mutex_unlock(&S.lock);
    set_activity("");
}

/* ---- Agent ------------------------------------------------------------ */

static struct {
    pthread_t        thread;
    bool             thread_valid;
    volatile bool    quit;
    GMainContext    *ctx;
    GDBusConnection *conn;
    guint            reg_id;
    guint            watch_id;
} A;

static const char AGENT_XML[] =
    "<node>"
    " <interface name='org.bluez.Agent1'>"
    "  <method name='Release'/>"
    "  <method name='RequestPinCode'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='s' name='pincode' direction='out'/>"
    "  </method>"
    "  <method name='DisplayPinCode'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='s' name='pincode' direction='in'/>"
    "  </method>"
    "  <method name='RequestPasskey'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='u' name='passkey' direction='out'/>"
    "  </method>"
    "  <method name='DisplayPasskey'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='u' name='passkey' direction='in'/>"
    "   <arg type='q' name='entered' direction='in'/>"
    "  </method>"
    "  <method name='RequestConfirmation'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='u' name='passkey' direction='in'/>"
    "  </method>"
    "  <method name='RequestAuthorization'>"
    "   <arg type='o' name='device' direction='in'/>"
    "  </method>"
    "  <method name='AuthorizeService'>"
    "   <arg type='o' name='device' direction='in'/>"
    "   <arg type='s' name='uuid' direction='in'/>"
    "  </method>"
    "  <method name='Cancel'/>"
    " </interface>"
    "</node>";

static void agent_refuse(GDBusMethodInvocation *inv, const char *method)
{
    plog("agent: refused %s - pairing is closed (press Scan to open it)", method);
    g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected",
                                               "Pairing closed - press Scan on the radio");
}

/* A code for a legacy (PIN) keyboard: typed on it, so not "0000". */
static void random_pin(char *out, size_t out_sz)
{
    unsigned v = 0;
    FILE    *f = fopen("/dev/urandom", "rb");

    if (f == NULL || fread(&v, sizeof(v), 1, f) != 1) {
        v = (unsigned)time(NULL) * 2654435761u;
    }
    if (f != NULL) {
        fclose(f);
    }
    snprintf(out, out_sz, "%06u", v % 1000000u);
}

/* Runs on the agent thread. Pairing questions are answered only while the
 * pairing window is open (see pairing_open()); then:
 *   RequestPinCode       "0000" (older headsets), a random code shown in
 *                        the window for a keyboard (typed on it)
 *   DisplayPasskey/PinCode  shown in the window: type it on the keyboard
 *   RequestPasskey       0 (nothing to type it on)
 *   RequestConfirmation, RequestAuthorization  accepted (no keypad)
 * AuthorizeService is accepted for a paired device at any time. */
static void agent_call(GDBusConnection *c, const gchar *sender,
                       const gchar *obj, const gchar *iface,
                       const gchar *method, GVariant *params,
                       GDBusMethodInvocation *inv, gpointer user)
{
    const char *dev = NULL;
    char        text[48];

    (void)c; (void)sender; (void)obj; (void)iface; (void)user;

    if (g_variant_n_children(params) > 0) {
        GVariant *v = g_variant_get_child_value(params, 0);

        if (g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)) {
            dev = g_variant_get_string(v, NULL);
        }
        plog("agent: %s %s", method, dev ? dev : "");
        g_variant_unref(v);     /* dev stays valid: params holds it */
    } else {
        plog("agent: %s", method);
    }

    if (strcmp(method, "AuthorizeService") == 0) {
        if (pairing_is_open() || path_is_paired(A.conn, dev)) {
            g_dbus_method_invocation_return_value(inv, NULL);
        } else {
            agent_refuse(inv, method);
        }
        return;
    }
    if (strcmp(method, "Release") == 0) {
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    if (strcmp(method, "Cancel") == 0) {
        pthread_mutex_lock(&S.lock);
        if (strncmp(S.activity, "Type ", 5) == 0) {
            S.activity[0] = '\0';
        }
        pthread_mutex_unlock(&S.lock);
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    if (!pairing_is_open()) {
        agent_refuse(inv, method);
        return;
    }

    if (strcmp(method, "RequestPinCode") == 0) {
        if (path_is_input(A.conn, dev)) {
            char pin[8];

            random_pin(pin, sizeof(pin));
            snprintf(text, sizeof(text), "Type %s on the keyboard, Enter", pin);
            set_activity(text);
            plog("agent: PIN %s for the keyboard", pin);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", pin));
        } else {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", AGENT_PIN));
        }
    } else if (strcmp(method, "DisplayPinCode") == 0) {
        const char *pin = NULL;

        g_variant_get(params, "(&o&s)", NULL, &pin);
        snprintf(text, sizeof(text), "Type %.8s on the keyboard, Enter", pin ? pin : "?");
        set_activity(text);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "DisplayPasskey") == 0) {
        guint32 passkey = 0;
        guint16 entered = 0;

        g_variant_get(params, "(&ouq)", NULL, &passkey, &entered);
        snprintf(text, sizeof(text), "Type %06u on the keyboard, Enter", (unsigned)passkey);
        set_activity(text);
        plog("agent: passkey %06u (%u typed)", (unsigned)passkey, (unsigned)entered);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "RequestPasskey") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0u));
    } else {
        /* RequestConfirmation, RequestAuthorization: accept. */
        g_dbus_method_invocation_return_value(inv, NULL);
    }
}

static const GDBusInterfaceVTable agent_vtable = { .method_call = agent_call };

static void agent_register(void)
{
    GError   *err = NULL;
    GVariant *r;
    char      cap[20];

    if (A.conn == NULL) {
        return;
    }
    pthread_mutex_lock(&S.lock);
    snprintf(cap, sizeof(cap), "%s", S.agent_cap[0] ? S.agent_cap : AGENT_CAP);
    pthread_mutex_unlock(&S.lock);

    r = g_dbus_connection_call_sync(A.conn, BLUEZ, "/org/bluez",
                                    "org.bluez.AgentManager1", "RegisterAgent",
                                    g_variant_new("(os)", AGENT_PATH, cap),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS,
                                    NULL, &err);
    if (r == NULL) {
        plog("agent: RegisterAgent: %s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    g_variant_unref(r);

    r = g_dbus_connection_call_sync(A.conn, BLUEZ, "/org/bluez",
                                    "org.bluez.AgentManager1", "RequestDefaultAgent",
                                    g_variant_new("(o)", AGENT_PATH),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS,
                                    NULL, &err);
    if (r == NULL) {
        plog("agent: RequestDefaultAgent: %s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    g_variant_unref(r);
    plog("agent: registered (%s, PIN %s)", cap, AGENT_PIN);
}

/* Default agent again, e.g. when the adapter comes back. */
static void agent_make_default(void)
{
    GVariant *r;

    if (A.conn == NULL) {
        return;
    }
    r = g_dbus_connection_call_sync(A.conn, BLUEZ, "/org/bluez",
                                    "org.bluez.AgentManager1", "RequestDefaultAgent",
                                    g_variant_new("(o)", AGENT_PATH),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS,
                                    NULL, NULL);
    if (r != NULL) {
        g_variant_unref(r);
    }
}

/* Worker thread: register again with another IO capability. bluez reads
 * it when a pairing starts. */
static void agent_set_cap(const char *cap)
{
    GVariant *r;

    pthread_mutex_lock(&S.lock);
    if (strcmp(S.agent_cap[0] ? S.agent_cap : AGENT_CAP, cap) == 0) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    snprintf(S.agent_cap, sizeof(S.agent_cap), "%s", cap);
    pthread_mutex_unlock(&S.lock);

    if (A.conn == NULL) {
        return;
    }
    r = g_dbus_connection_call_sync(A.conn, BLUEZ, "/org/bluez",
                                    "org.bluez.AgentManager1", "UnregisterAgent",
                                    g_variant_new("(o)", AGENT_PATH),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS,
                                    NULL, NULL);
    if (r != NULL) {
        g_variant_unref(r);
    }
    agent_register();
}

static void bluez_appeared(GDBusConnection *c, const gchar *name,
                           const gchar *owner, gpointer user)
{
    (void)c; (void)name; (void)owner; (void)user;
    agent_register();
}

static void bluez_vanished(GDBusConnection *c, const gchar *name, gpointer user)
{
    (void)c; (void)name; (void)user;
}

static void *agent_thread(void *arg)
{
    GError        *err = NULL;
    GDBusNodeInfo *info;
    gchar         *addr;

    (void)arg;

    A.ctx = g_main_context_new();
    /* Method calls to the agent and the name watch are dispatched in
     * this thread's own context, iterated below. */
    g_main_context_push_thread_default(A.ctx);

    addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (addr != NULL) {
        A.conn = g_dbus_connection_new_for_address_sync(
            addr,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, &err);
        g_free(addr);
    }
    if (A.conn == NULL) {
        plog("agent: no D-Bus: %s", err ? err->message : "?");
        g_clear_error(&err);
        goto out;
    }

    info = g_dbus_node_info_new_for_xml(AGENT_XML, &err);
    if (info == NULL) {
        plog("agent: bad XML: %s", err ? err->message : "?");
        g_clear_error(&err);
        goto out;
    }
    A.reg_id = g_dbus_connection_register_object(A.conn, AGENT_PATH,
                                                 info->interfaces[0],
                                                 &agent_vtable, NULL, NULL, &err);
    g_dbus_node_info_unref(info);
    if (A.reg_id == 0) {
        plog("agent: register_object: %s", err ? err->message : "?");
        g_clear_error(&err);
        goto out;
    }

    /* (Re)register with bluez every time bluetoothd appears. */
    A.watch_id = g_bus_watch_name_on_connection(A.conn, BLUEZ,
                                                G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                bluez_appeared, bluez_vanished,
                                                NULL, NULL);

    while (!A.quit) {
        g_main_context_iteration(A.ctx, TRUE);
    }

    g_bus_unwatch_name(A.watch_id);
    g_dbus_connection_unregister_object(A.conn, A.reg_id);

out:
    if (A.conn != NULL) {
        g_object_unref(A.conn);
        A.conn = NULL;
    }
    g_main_context_pop_thread_default(A.ctx);
    g_main_context_unref(A.ctx);
    A.ctx = NULL;
    return NULL;
}

static void agent_start(void)
{
    if (A.thread_valid) {
        return;
    }
    A.quit = false;
    if (pthread_create(&A.thread, NULL, agent_thread, NULL) == 0) {
        A.thread_valid = true;
    }
}

static void agent_stop(void)
{
    if (!A.thread_valid) {
        return;
    }
    A.quit = true;
    if (A.ctx != NULL) {
        g_main_context_wakeup(A.ctx);
    }
    pthread_join(A.thread, NULL);
    A.thread_valid = false;
}

/* ---- Worker ------------------------------------------------------------ */

static void *worker(void *arg)
{
    (void)arg;

    for (;;) {
        cmd_t cmd;
        char  addr[18];

        pthread_mutex_lock(&S.lock);
        while (!S.quit && S.cmd == CMD_NONE) {
            pthread_cond_wait(&S.cond, &S.lock);
        }
        if (S.quit) {
            pthread_mutex_unlock(&S.lock);
            break;
        }
        cmd = S.cmd;
        S.cmd = CMD_NONE;
        snprintf(addr, sizeof(addr), "%s", S.cmd_addr);
        pthread_mutex_unlock(&S.lock);

        switch (cmd) {
            case CMD_REFRESH:
                do_refresh();
                break;
            case CMD_CONNECT:
                do_connect(addr, true);
                break;
            case CMD_DISCONNECT:
                do_connect(addr, false);
                break;
            case CMD_SCAN:
                do_scan();
                break;
            case CMD_FORGET:
                do_forget(addr);
                break;
            default:
                break;
        }
    }

    if (S.conn != NULL) {
        g_object_unref(S.conn);
        S.conn = NULL;
    }
    return NULL;
}

static void ensure_thread(void)
{
    bool start = false;

    pthread_mutex_lock(&S.lock);
    if (!S.thread_valid) {
        S.quit = false;
        start = true;
    }
    pthread_mutex_unlock(&S.lock);

    if (start && pthread_create(&S.thread, NULL, worker, NULL) == 0) {
        S.thread_valid = true;
    }
    agent_start();
}

static void post(cmd_t cmd, const char *addr)
{
    ensure_thread();

    pthread_mutex_lock(&S.lock);
    /* A scan holds the worker for a dozen seconds; do not pile requests
     * behind it, the timer would queue one every second. */
    if (S.scanning && cmd == CMD_REFRESH) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    /* There is one slot. The timer's refresh must not overwrite a
     * connect, disconnect or forget the worker has not picked up yet. */
    if (cmd == CMD_REFRESH && S.cmd != CMD_NONE) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    S.cmd = cmd;
    if (addr != NULL) {
        snprintf(S.cmd_addr, sizeof(S.cmd_addr), "%s", addr);
    }
    pthread_cond_signal(&S.cond);
    pthread_mutex_unlock(&S.lock);
}

/* ------------------------------------------------------------------ */

void bt_dev_init(void)
{
    ensure_thread();
    post(CMD_REFRESH, NULL);
}

void bt_dev_deinit(void)
{
    if (S.thread_valid) {
        pthread_mutex_lock(&S.lock);
        S.quit = true;
        pthread_cond_signal(&S.cond);
        pthread_mutex_unlock(&S.lock);

        pthread_join(S.thread, NULL);
        S.thread_valid = false;
    }
    agent_stop();
}

void bt_dev_refresh(void)
{
    post(CMD_REFRESH, NULL);
}

int bt_dev_count(void)
{
    int n;

    pthread_mutex_lock(&S.lock);
    n = S.count;
    pthread_mutex_unlock(&S.lock);
    return n;
}

bool bt_dev_get(int idx, bt_dev_t *out)
{
    bool ok = false;

    if (out == NULL) {
        return false;
    }

    pthread_mutex_lock(&S.lock);
    if (idx >= 0 && idx < S.count) {
        *out = S.list[idx];
        ok = true;
    }
    pthread_mutex_unlock(&S.lock);
    return ok;
}

void bt_dev_connect(const char *addr)
{
    post(CMD_CONNECT, addr);
}

void bt_dev_disconnect(const char *addr)
{
    post(CMD_DISCONNECT, addr);
}

void bt_dev_forget(const char *addr)
{
    post(CMD_FORGET, addr);
}

void bt_dev_scan(void)
{
    post(CMD_SCAN, NULL);
}

bool bt_dev_scanning(void)
{
    bool s;

    pthread_mutex_lock(&S.lock);
    s = S.scanning;
    pthread_mutex_unlock(&S.lock);
    return s;
}

const char *bt_dev_activity(void)
{
    return S.activity;
}

int bt_dev_visible_s(void)
{
    time_t left;

    pthread_mutex_lock(&S.lock);
    left = S.open_until - time(NULL);
    pthread_mutex_unlock(&S.lock);
    return left > 0 ? (int)left : 0;
}
