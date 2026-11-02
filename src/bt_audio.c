/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_audio.c - Bluetooth headset audio: listening and PTT.
 *
 * Everything here shells out to pactl, and every one of those calls can
 * block for a good fraction of a second, so all of it happens on a
 * worker thread. The UI only reads cached values.
 *
 * Listening: the source is the same capture device the DSP reads.
 * PulseAudio lets several clients read one source, so the loopback sits
 * alongside "X6100 GUI Capture" and "X6100 GUI Monitor" rather than
 * displacing them - FT8 decoding and the other DSP clients carry on
 * untouched.
 *
 * PTT: the HFP source of the headset goes through a second loopback
 * into the codec output and the BASE is keyed with radio_set_modem(),
 * exactly like send_thread() in dialog_msg_voice.c.
 * radio_set_ptt() keys the transmitter from the radio's own microphone
 * input, so the codec audio - our headset loopback - never reached the
 * modulator: PTT worked, ALC and power did not move. modem_set takes
 * the TX audio from the codec, as the voice messages and the FT8
 * transmitter do. Record mode (audio_set_play_mode(AUDIO_PLAY_ON), the
 * x6100_voice_rec bit) must NOT be set: dialog_msg_voice.c uses it only
 * for local playback, and a version that set it never went to TX.
 *
 * Both loopbacks are loaded with *_dont_move=true. Without it PulseAudio
 * moves an orphaned loopback to the default device when the headset
 * disappears, and the listening loopback would then feed receiver audio
 * straight into the transmitter's input.
 *
 * Speaker and volume: while the listening loopback plays into
 * connected headphones (state BT_AUDIO_ON) the radio's own speaker is
 * kept silent and the volume - VOL knob, the Volume button, CAT and mute
 * - is handed to the Bluetooth sink through radio_set_volume_sink().
 * When listening stops or the headphones go away, the speaker gets its
 * volume back. Listening that lost its headphones (BT_AUDIO_NO_SINK)
 * starts again by itself when they come back, so the worker keeps an eye
 * on the sink list every BG_POLL_S seconds even with the window closed.
 *
 * Local playback - TTS, the voice message preview, recordings - plays
 * on the radio's speaker in AUDIO_PLAY_ON mode, where the BASE replaces
 * the receiver with the GUI's audio. With the speaker silent that would
 * go nowhere, so while listening is on those players open a stream on
 * the Bluetooth sink instead (bt_audio_playback_begin()), and the
 * listening loopback is muted for the time, as the BASE does locally.
 * begin/end is a flag, not a count, like AUDIO_PLAY_ON/OFF: a cancelled
 * TTS thread that never called end is put right by the next one.
 *
 * Headset button: see "Headset button as PTT" at the end of this file.
 */

#include "bt_audio.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "msg.h"
#include "radio.h"
#include "diag_log.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONF_PATH   "/mnt/bt_audio.conf"
#define SRC_DEVICE  "alsa_input.platform-sound.stereo-fallback"
#define TX_DEVICE   "alsa_output.platform-sound.stereo-fallback"

#define LAT_MIN     50u
#define LAT_MAX     400u
#define LAT_DEFAULT 100u

/* The microphone path is kept short: the operator hears himself in the
 * receiver only after unkeying, so delay here just clips the first
 * syllable less. */
#define TX_LATENCY_MS   60u

/* Level of the headset microphone into the modulator. The headset
 * delivers speech well below full scale and the HFP source volume is
 * the headset's own mic gain (no effect above 100 %), so the gain is
 * set where the voice messages and FT8 set it: the codec playback
 * volume, for the time of the transmission. The Play gain from Settings
 * is put back afterwards. The right value still has to be settled: on
 * one radio the ALC did not follow the Play gain consistently, so the
 * default stays at 0 dB until that is understood. */
#define MIC_GAIN_MIN     (-10)
#define MIC_GAIN_MAX     10
#define MIC_GAIN_DEFAULT 0

/* How long to wait for the HFP source after a profile switch. */
#define HFP_WAIT_STEPS  10
#define HFP_WAIT_US     400000

/* Background check for the headphones while listening is on or waiting
 * for them, window open or not. One "pactl list short sinks". */
#define BG_POLL_S       3

/* cfg.volume runs 0..55 (radio_change_vol). It is mapped linearly onto
 * the PulseAudio volume scale, which is already perceptual (cubic), as
 * pactl's percent is. */
#define RADIO_VOL_MAX   55

typedef enum {
    CMD_NONE = 0,
    CMD_ON,
    CMD_OFF,
    CMD_POLL,
    CMD_RELOAD,
} cmd_t;

static struct {
    pthread_t        thread;
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    bool             thread_valid;
    bool             quit;
    cmd_t            cmd;

    bt_audio_state_t state;
    char             sink[96];
    int              module_idx;    /* listening loopback, -1 if none */
    uint32_t         measured_ms;
    uint32_t         latency_ms;
    bool             headset_ptt;   /* headset button toggles PTT */
    int              mic_gain_db;   /* codec play volume while TX */

    /* Volume handed over by radio.c while the speaker is silent. */
    int              vol_want;      /* 0..RADIO_VOL_MAX, -1 unknown */
    bool             vol_pending;   /* vol_want not yet sent to the sink */
    bool             speaker_handed;/* worker only: radio_set_volume_sink on */

    /* Local playback on the headphones: listening loopback muted. */
    bool             pause_want;
    bool             pause_applied; /* written by the worker only */

    /* PTT. ptt_want is what the UI asked for, ptt_on is what the
     * worker has actually done to the radio. */
    bool             ptt_want;
    bool             ptt_on;
    bt_ptt_state_t   ptt_state;
    int              tx_module_idx; /* TX loopback, -1 if none */
    time_t           tx_since;
} S = {
    .lock          = PTHREAD_MUTEX_INITIALIZER,
    .cond          = PTHREAD_COND_INITIALIZER,
    .state         = BT_AUDIO_OFF,
    .module_idx    = -1,
    .latency_ms    = LAT_DEFAULT,
    .headset_ptt   = true,
    .mic_gain_db   = MIC_GAIN_DEFAULT,
    .vol_want      = -1,
    .ptt_state     = BT_PTT_OFF,
    .tx_module_idx = -1,
};

/* ------------------------------------------------------------------ */

static time_t now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* ---- Log -----------------------------------------------------------
 *
 * /mnt is the FAT partition of the SD card, so the log can be read on a
 * PC without SSH: switch the radio off, put the card in, open
 * bt_audio.log. Kept below BT_LOG_MAX by moving it to bt_audio.log.old.
 */
#define BT_LOG      "/mnt/bt_audio.log"
#define BT_LOG_OLD  "/mnt/bt_audio.log.old"
#define BT_LOG_MAX  262144

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void bt_log(const char *fmt, ...)
{
    struct stat st;
    struct tm   tm;
    time_t      now = time(NULL);
    char        ts[24];
    va_list     ap;
    FILE       *f;

    pthread_mutex_lock(&log_lock);
    if (stat(BT_LOG, &st) == 0 && st.st_size > BT_LOG_MAX) {
        (void)rename(BT_LOG, BT_LOG_OLD);
    }
    f = fopen(BT_LOG, "a");
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

static void conf_load(void)
{
    FILE *f = fopen(CONF_PATH, "r");
    char  line[128];

    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "latency_msec=", 13) == 0) {
            long n = strtol(line + 13, NULL, 10);

            if (n >= (long)LAT_MIN && n <= (long)LAT_MAX) {
                S.latency_ms = (uint32_t)n;
            }
        } else if (strncmp(line, "headset_ptt=", 12) == 0) {
            S.headset_ptt = strtol(line + 12, NULL, 10) != 0;
        } else if (strncmp(line, "mic_gain_db=", 12) == 0) {
            long g = strtol(line + 12, NULL, 10);

            if (g >= MIC_GAIN_MIN && g <= MIC_GAIN_MAX) {
                S.mic_gain_db = (int)g;
            }
        }
    }
    fclose(f);
}

static void conf_save(void)
{
    FILE *f = fopen(CONF_PATH, "w");

    if (f == NULL) {
        return;
    }
    fprintf(f, "# Bluetooth listening loopback\n");
    fprintf(f, "latency_msec=%u\n", (unsigned)S.latency_ms);
    fprintf(f, "# 1: a short press of the headset button toggles PTT\n");
    fprintf(f, "headset_ptt=%d\n", S.headset_ptt ? 1 : 0);
    fprintf(f, "# headset microphone level while transmitting, dB (-10..10)\n");
    fprintf(f, "mic_gain_db=%d\n", S.mic_gain_db);
    fclose(f);
}

/* Runs a command and returns its first line with the newline stripped.
 * Worker thread only. */
static bool run_capture(const char *cmd, char *out, size_t out_sz)
{
    FILE *p = popen(cmd, "r");
    char *nl;

    if (out != NULL && out_sz > 0) {
        out[0] = '\0';
    }
    if (p == NULL) {
        return false;
    }
    if (out != NULL && out_sz > 0) {
        if (fgets(out, (int)out_sz, p) != NULL) {
            nl = strchr(out, '\n');
            if (nl != NULL) {
                *nl = '\0';
            }
        }
    }
    return (pclose(p) == 0);
}

/*
 * Which Bluetooth device is ours.
 *
 * PulseAudio makes sinks, sources and a card for EVERY connected audio
 * device, phones included. A phone connected for media appears as
 * bluez_source.<phone>.a2dp_source (its music coming in), connected for
 * calls as ...handsfree_audio_gateway / headset_audio_gateway. Taking
 * the first "bluez" entry, as an earlier version did, made PTT transmit
 * the phone's media stream instead of the speaker's microphone whenever
 * a phone (one running a WSJT-X client app) was connected as well.
 *
 * So only the profiles in which the remote device is a headset or a
 * speaker count, and the microphone and the card are taken from the
 * same device (MAC) the listening loopback plays to.
 */
static const char *const SINK_PROFILES[] = {
    ".a2dp_sink", ".handsfree_head_unit", ".headset_head_unit", NULL
};
static const char *const MIC_PROFILES[] = {
    ".handsfree_head_unit", ".headset_head_unit", NULL
};

static bool ends_with_any(const char *name, const char *const *sfx)
{
    size_t n = strlen(name);

    for (; *sfx != NULL; sfx++) {
        size_t k = strlen(*sfx);

        if (n >= k && strcmp(name + n - k, *sfx) == 0) {
            return true;
        }
    }
    return false;
}

/* "bluez_sink.AA_BB_CC_DD_EE_FF.a2dp_sink" -> "AA_BB_CC_DD_EE_FF",
 * "bluez_card.AA_BB_CC_DD_EE_FF" -> "AA_BB_CC_DD_EE_FF". */
static void name_mac(const char *name, char *mac, size_t mac_sz)
{
    const char *a = strchr(name, '.');
    const char *b;
    size_t      n;

    mac[0] = '\0';
    if (a == NULL) {
        return;
    }
    a++;
    b = strchr(a, '.');
    n = (b != NULL) ? (size_t)(b - a) : strlen(a);
    if (n == 0 || n >= mac_sz) {
        return;
    }
    memcpy(mac, a, n);
    mac[n] = '\0';
}

/* Reads "pactl list short <what>" and returns the name column of the
 * first entry that starts with `prefix`, ends with one of `sfx` (NULL:
 * any) and belongs to `mac` (NULL or "": any device). */
static void find_dev(const char *what, const char *prefix,
                     const char *const *sfx, const char *mac,
                     char *out, size_t out_sz)
{
    FILE *p;
    char  cmd[64];
    char  line[256];

    out[0] = '\0';

    snprintf(cmd, sizeof(cmd), "pactl list short %s 2>/dev/null", what);
    p = popen(cmd, "r");
    if (p == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), p) != NULL) {
        char *name;
        char *tab;
        char  m[32];

        /* index <tab> name <tab> module ... */
        name = strchr(line, '\t');
        if (name == NULL) {
            continue;
        }
        name++;
        tab = strchr(name, '\t');
        if (tab != NULL) {
            *tab = '\0';
        }
        tab = strchr(name, '\n');
        if (tab != NULL) {
            *tab = '\0';
        }
        if (strncmp(name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        if (sfx != NULL && !ends_with_any(name, sfx)) {
            continue;
        }
        if (mac != NULL && mac[0] != '\0') {
            name_mac(name, m, sizeof(m));
            if (strcmp(m, mac) != 0) {
                continue;
            }
        }
        snprintf(out, out_sz, "%s", name);
        break;
    }
    pclose(p);
}

/* Headphones or speaker to play to: bluez_sink.<MAC>.a2dp_sink, or
 * .handsfree_head_unit once PTT has switched the card to HFP. The
 * device already in use is kept if it is still there. */
static void find_sink(char *out, size_t out_sz)
{
    char cur[96];
    char mac[32];

    pthread_mutex_lock(&S.lock);
    snprintf(cur, sizeof(cur), "%s", S.sink);
    pthread_mutex_unlock(&S.lock);

    name_mac(cur, mac, sizeof(mac));
    if (mac[0] != '\0') {
        find_dev("sinks", "bluez_sink.", SINK_PROFILES, mac, out, out_sz);
        if (out[0] != '\0') {
            return;
        }
    }
    find_dev("sinks", "bluez_sink.", SINK_PROFILES, NULL, out, out_sz);
}

/* Microphone of the device `mac`: bluez_source.<MAC>.handsfree_head_unit
 * (HFP/HSP only). Never an a2dp_source - that is a phone's media. */
static void find_mic(const char *mac, char *out, size_t out_sz)
{
    find_dev("sources", "bluez_source.", MIC_PROFILES, mac, out, out_sz);
}

static void find_card(const char *mac, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (mac == NULL || mac[0] == '\0') {
        return;
    }
    find_dev("cards", "bluez_card.", NULL, mac, out, out_sz);
}

/* The device PTT uses: the one listening plays to, else the first
 * headset or speaker connected. "" when there is none. */
static void ptt_device(char *mac, size_t mac_sz)
{
    char sink[96];

    find_sink(sink, sizeof(sink));
    name_mac(sink, mac, mac_sz);
}

/* Adds up the buffer and sink latency of the listening loopback, which
 * is what the operator actually hears as delay. */
static uint32_t measure_latency(void)
{
    FILE    *p;
    char     line[256];
    uint32_t buf_us = 0;
    uint32_t sink_us = 0;
    uint32_t pending_buf = 0;
    uint32_t pending_sink = 0;
    bool     found = false;

    p = popen("pactl list sink-inputs 2>/dev/null", "r");
    if (p == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), p) != NULL) {
        char *q;

        if (strstr(line, "Sink Input #") != NULL) {
            pending_buf = 0;
            pending_sink = 0;
            continue;
        }
        q = strstr(line, "Buffer Latency:");
        if (q != NULL) {
            pending_buf = (uint32_t)strtoul(q + 15, NULL, 10);
            continue;
        }
        q = strstr(line, "Sink Latency:");
        if (q != NULL) {
            pending_sink = (uint32_t)strtoul(q + 13, NULL, 10);
            continue;
        }
        if (strstr(line, "media.name") != NULL &&
            strstr(line, "BTListen") != NULL) {
            buf_us = pending_buf;
            sink_us = pending_sink;
            found = true;
        }
    }
    pclose(p);

    if (!found) {
        return 0;
    }
    return (buf_us + sink_us) / 1000u;
}

static void unload_module(int *idx)
{
    char cmd[64];

    if (*idx < 0) {
        return;
    }
    snprintf(cmd, sizeof(cmd), "pactl unload-module %d >/dev/null 2>&1",
             *idx);
    (void)run_capture(cmd, NULL, 0);
    *idx = -1;
}

/*
 * Latency creep: every underrun on the Bluetooth side (frequent with
 * WiFi on the same antenna) makes PulseAudio raise the loopback's
 * minimum latency ("Sink minimum latency increased to ..."), and it is
 * never lowered again - after a long listen the delay had grown from
 * 100 ms to 2 s. max_latency_msec caps how far it may grow and
 * fast_adjust_threshold_msec makes the loopback drop the excess at once
 * (a short gap) instead of letting it pile up.
 */
#define LOOP_FAST_ADJUST_MS 200u
#define LOOP_MAX_EXTRA_MS   200u

static int load_one(const char *source, const char *sink, uint32_t latency,
                    const char *tag, bool capped)
{
    char cmd[480];
    char reply[32];
    char extra[96] = "";

    if (capped) {
        snprintf(extra, sizeof(extra),
                 "max_latency_msec=%u fast_adjust_threshold_msec=%u ",
                 (unsigned)(latency + LOOP_MAX_EXTRA_MS),
                 (unsigned)LOOP_FAST_ADJUST_MS);
    }
    snprintf(cmd, sizeof(cmd),
             "pactl load-module module-loopback source=%s sink=%s "
             "latency_msec=%u %ssource_dont_move=true sink_dont_move=true "
             "sink_input_properties=media.name=%s 2>/dev/null",
             source, sink, (unsigned)latency, extra, tag);

    if (!run_capture(cmd, reply, sizeof(reply)) || reply[0] == '\0') {
        return -1;
    }
    return (int)strtol(reply, NULL, 10);
}

static int load_module_loopback(const char *source, const char *sink,
                                uint32_t latency, const char *tag)
{
    static int capped_ok = -1;      /* -1 unknown, 0 not supported, 1 ok */
    int        idx = -1;

    if (capped_ok != 0) {
        idx = load_one(source, sink, latency, tag, true);
        if (idx >= 0 && capped_ok < 0) {
            capped_ok = 1;
            bt_log("loopback: latency cap on (max %u ms, fast adjust %u ms)",
                   (unsigned)(latency + LOOP_MAX_EXTRA_MS),
                   (unsigned)LOOP_FAST_ADJUST_MS);
        }
    }
    if (idx < 0) {
        /* An older module-loopback refuses the unknown arguments. */
        idx = load_one(source, sink, latency, tag, false);
        if (idx >= 0 && capped_ok < 0) {
            capped_ok = 0;
            bt_log("loopback: this PulseAudio has no latency cap, loaded without");
        }
    }
    return idx;
}

/* ---- Listening ---------------------------------------------------- */

static void set_state(bt_audio_state_t st)
{
    pthread_mutex_lock(&S.lock);
    S.state = st;
    pthread_mutex_unlock(&S.lock);
}

static void do_on(void)
{
    char sink[96];
    int  idx;

    find_sink(sink, sizeof(sink));

    pthread_mutex_lock(&S.lock);
    snprintf(S.sink, sizeof(S.sink), "%s", sink);
    pthread_mutex_unlock(&S.lock);

    if (sink[0] == '\0') {
        bt_log("listen: no headphones or speaker connected");
        set_state(BT_AUDIO_NO_SINK);
        return;
    }

    unload_module(&S.module_idx);

    idx = load_module_loopback(SRC_DEVICE, sink, S.latency_ms, "BTListen");
    bt_log("listen: %s %s", sink, idx >= 0 ? "on" : "FAILED to load loopback");
    if (idx >= 0) {
        uint32_t ms;

        S.module_idx = idx;
        ms = measure_latency();

        pthread_mutex_lock(&S.lock);
        S.measured_ms = ms;
        S.state = BT_AUDIO_ON;
        /* A new sink (first start, latency change, A2DP -> HFP) starts
         * at its own volume: send ours again. */
        if (S.vol_want >= 0) {
            S.vol_pending = true;
        }
        /* A fresh loopback plays: mute it again if a playback is on. */
        S.pause_applied = false;
        pthread_mutex_unlock(&S.lock);
    } else {
        set_state(BT_AUDIO_FAILED);
    }
}

static void do_off(void)
{
    unload_module(&S.module_idx);

    pthread_mutex_lock(&S.lock);
    S.measured_ms = 0;
    S.state = BT_AUDIO_OFF;
    pthread_mutex_unlock(&S.lock);
}

/* measure: also read the loopback delay for the window. The background
 * check passes false and only looks at the sink list. */
static void do_poll(bool measure)
{
    char             sink[96];
    uint32_t         ms = 0;
    bt_audio_state_t st;

    find_sink(sink, sizeof(sink));

    pthread_mutex_lock(&S.lock);
    st = S.state;
    snprintf(S.sink, sizeof(S.sink), "%s", sink);
    pthread_mutex_unlock(&S.lock);

    /* Listening was asked for, the headphones were missing: they are
     * back, start playing to them. */
    if (st == BT_AUDIO_NO_SINK) {
        if (sink[0] != '\0') {
            do_on();
        }
        return;
    }

    if (st != BT_AUDIO_ON) {
        return;
    }

    /* Headphones walked away: dont_move already unloaded the loopback,
     * so forget its index and say so rather than claiming to play. */
    if (sink[0] == '\0') {
        bt_log("listen: headphones gone, speaker back");
        unload_module(&S.module_idx);
        pthread_mutex_lock(&S.lock);
        S.measured_ms = 0;
        S.state = BT_AUDIO_NO_SINK;
        pthread_mutex_unlock(&S.lock);
        return;
    }

    if (!measure) {
        return;
    }
    ms = measure_latency();
    pthread_mutex_lock(&S.lock);
    S.measured_ms = ms;
    pthread_mutex_unlock(&S.lock);
}

/* ---- Speaker and volume ------------------------------------------ */

/* Called by radio.c with the volume to play at (0 when muted), from the
 * UI thread or from radio_set_volume_sink(). Only records it; the worker
 * talks to PulseAudio. Must not block. */
static void vol_cb(int32_t vol)
{
    pthread_mutex_lock(&S.lock);
    S.vol_want = (int)vol;
    S.vol_pending = true;
    pthread_cond_signal(&S.cond);
    pthread_mutex_unlock(&S.lock);
}

/* Sends the last volume to the Bluetooth sink. Turning the knob fast
 * posts many values; only the last one is sent. Worker thread only. */
static void apply_volume(void)
{
    char sink[96];
    char cmd[200];
    int  vol;
    int  pct;
    bool on;

    pthread_mutex_lock(&S.lock);
    if (!S.vol_pending) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    S.vol_pending = false;
    vol = S.vol_want;
    on = (S.state == BT_AUDIO_ON);
    snprintf(sink, sizeof(sink), "%s", S.sink);
    pthread_mutex_unlock(&S.lock);

    if (!on || vol < 0 || sink[0] == '\0') {
        return;
    }
    if (vol > RADIO_VOL_MAX) {
        vol = RADIO_VOL_MAX;
    }
    pct = (vol * 100 + RADIO_VOL_MAX / 2) / RADIO_VOL_MAX;

    snprintf(cmd, sizeof(cmd),
             "pactl set-sink-volume %s %d%% >/dev/null 2>&1", sink, pct);
    (void)run_capture(cmd, NULL, 0);
}

/* Speaker silent and volume on the headphones exactly while listening
 * plays into them. Transitional states keep what there is, so a reload
 * or the switch to HFP does not blip the speaker. Worker thread only,
 * never with S.lock held (radio_set_volume_sink calls vol_cb). */
static void speaker_follow(void)
{
    bt_audio_state_t st;
    bool             want;

    pthread_mutex_lock(&S.lock);
    st = S.state;
    pthread_mutex_unlock(&S.lock);

    if (st == BT_AUDIO_STARTING || st == BT_AUDIO_STOPPING) {
        return;
    }
    want = (st == BT_AUDIO_ON);
    if (want == S.speaker_handed) {
        return;
    }
    S.speaker_handed = want;
    radio_set_volume_sink(want ? vol_cb : NULL);
}

/* Index of the sink input the listening loopback plays through, -1 if
 * not found. "pactl list short sink-inputs": index, sink, owner module,
 * client, sample spec. */
static int loopback_input(int module)
{
    FILE *p;
    char  line[256];
    int   found = -1;

    if (module < 0) {
        return -1;
    }
    p = popen("pactl list short sink-inputs 2>/dev/null", "r");
    if (p == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), p) != NULL) {
        char *f2 = strchr(line, '\t');
        char *f3;

        if (f2 == NULL) {
            continue;
        }
        f3 = strchr(f2 + 1, '\t');
        if (f3 == NULL) {
            continue;
        }
        if (strtol(f3 + 1, NULL, 10) == module && f3[1] >= '0' && f3[1] <= '9') {
            found = (int)strtol(line, NULL, 10);
            break;
        }
    }
    pclose(p);
    return found;
}

/* Mutes or unmutes the listening loopback to match pause_want. Worker
 * thread only. */
static void apply_pause(void)
{
    char cmd[80];
    bool want;
    int  in;

    pthread_mutex_lock(&S.lock);
    want = S.pause_want;
    if (want == S.pause_applied || S.module_idx < 0) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    S.pause_applied = want;
    pthread_mutex_unlock(&S.lock);

    in = loopback_input(S.module_idx);
    if (in < 0) {
        return;
    }
    snprintf(cmd, sizeof(cmd),
             "pactl set-sink-input-mute %d %d >/dev/null 2>&1", in, want ? 1 : 0);
    (void)run_capture(cmd, NULL, 0);
}

/* ---- PTT ---------------------------------------------------------- */

static void set_ptt_state(bt_ptt_state_t st)
{
    pthread_mutex_lock(&S.lock);
    S.ptt_state = st;
    pthread_mutex_unlock(&S.lock);
}

/* Keying failed or was refused: drop the request so the worker does not
 * retry it in a loop. */
static void ptt_give_up(bt_ptt_state_t why)
{
    pthread_mutex_lock(&S.lock);
    S.ptt_want = false;
    S.ptt_on = false;
    S.ptt_state = why;
    pthread_mutex_unlock(&S.lock);
}

/* MAC of the device transmitting, for ptt_watch(). Worker thread only. */
static char tx_mac[32];

/* Puts the headset into HFP if it is not there yet and returns the name
 * of its microphone source. The listening loopback, if running, is
 * moved over to the HFP sink. Only the device listening plays to (or
 * the first headset/speaker) is touched - never a connected phone. */
static bool ensure_hfp(char *mic, size_t mic_sz)
{
    char card[96];
    char cmd[200];
    bool relisten;
    int  i;

    mic[0] = '\0';
    ptt_device(tx_mac, sizeof(tx_mac));
    if (tx_mac[0] == '\0') {
        bt_log("PTT: no headset or speaker connected");
        return false;
    }

    find_mic(tx_mac, mic, mic_sz);
    if (mic[0] != '\0') {
        return true;
    }

    find_card(tx_mac, card, sizeof(card));
    if (card[0] == '\0') {
        bt_log("PTT: no card for %s", tx_mac);
        return false;
    }
    bt_log("PTT: switching %s to HFP", card);

    /* The A2DP sink is about to vanish; take the listening loopback
     * down ourselves and bring it back on the HFP sink afterwards. */
    relisten = (S.module_idx >= 0);
    unload_module(&S.module_idx);

    snprintf(cmd, sizeof(cmd),
             "pactl set-card-profile %s handsfree_head_unit "
             ">/dev/null 2>&1", card);
    if (!run_capture(cmd, NULL, 0)) {
        snprintf(cmd, sizeof(cmd),
                 "pactl set-card-profile %s headset_head_unit "
                 ">/dev/null 2>&1", card);
        (void)run_capture(cmd, NULL, 0);
    }

    for (i = 0; i < HFP_WAIT_STEPS; i++) {
        find_mic(tx_mac, mic, mic_sz);
        if (mic[0] != '\0') {
            break;
        }
        usleep(HFP_WAIT_US);
    }

    bt_log("PTT: HFP microphone %s", mic[0] != '\0' ? mic : "NOT FOUND");

    if (relisten) {
        do_on();
    }

    return (mic[0] != '\0');
}

static void do_ptt_on(void)
{
    char mic[96];
    int  idx;

    set_ptt_state(BT_PTT_KEYING);

    if (!ensure_hfp(mic, sizeof(mic))) {
        ptt_give_up(BT_PTT_NO_MIC);
        return;
    }

    unload_module(&S.tx_module_idx);
    idx = load_module_loopback(mic, TX_DEVICE, TX_LATENCY_MS, "BTMicTX");
    bt_log("PTT: %s", idx >= 0 ? "TX on" : "FAILED to load TX loopback");
    if (idx < 0) {
        ptt_give_up(BT_PTT_FAILED);
        return;
    }
    S.tx_module_idx = idx;

    /* Microphone level into the modulator for this transmission. */
    pthread_mutex_lock(&S.lock);
    audio_set_play_vol((float)S.mic_gain_db);
    pthread_mutex_unlock(&S.lock);

    /* Audio path first, carrier last. */
    const char *src = diag_set_source("Bluetooth PTT");
    radio_set_modem(true);
    diag_set_source(src);

    pthread_mutex_lock(&S.lock);
    S.ptt_on = true;
    S.tx_since = now_s();
    S.ptt_state = BT_PTT_TX;
    pthread_mutex_unlock(&S.lock);
}

static void do_ptt_off(bt_ptt_state_t final_state)
{
    bt_log("PTT: TX off%s", final_state == BT_PTT_TIMEOUT ? " (timeout)" :
                            final_state == BT_PTT_NO_MIC ? " (microphone gone)" : "");
    set_ptt_state(BT_PTT_UNKEYING);

    /* Carrier first, audio path last. */
    const char *src = diag_set_source("Bluetooth PTT");
    radio_set_modem(false);
    diag_set_source(src);
    unload_module(&S.tx_module_idx);

    pthread_mutex_lock(&S.lock);
    /* Back to the Play gain from Settings. Under the lock, so a Mic gain
     * change racing with the unkey cannot leave the TX level behind. */
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    S.ptt_on = false;
    S.tx_since = 0;
    S.ptt_state = final_state;
    pthread_mutex_unlock(&S.lock);
}

/* Called about once a second while transmitting. */
static void ptt_watch(void)
{
    static unsigned n = 0;
    time_t          since;
    char            mic[96];

    pthread_mutex_lock(&S.lock);
    since = S.tx_since;
    pthread_mutex_unlock(&S.lock);

    if (now_s() - since >= (time_t)BT_PTT_TIMEOUT_S) {
        pthread_mutex_lock(&S.lock);
        S.ptt_want = false;
        pthread_mutex_unlock(&S.lock);
        do_ptt_off(BT_PTT_TIMEOUT);
        return;
    }

    /* Headset out of range or switched off: the TX loopback has already
     * unloaded itself (source_dont_move), so stop transmitting silence. */
    if ((++n % 2) == 0) {
        find_mic(tx_mac, mic, sizeof(mic));
        if (mic[0] == '\0') {
            S.tx_module_idx = -1;
            pthread_mutex_lock(&S.lock);
            S.ptt_want = false;
            pthread_mutex_unlock(&S.lock);
            do_ptt_off(BT_PTT_NO_MIC);
        }
    }
}

/* ---- Worker ------------------------------------------------------- */

static void *worker(void *arg)
{
    (void)arg;

    for (;;) {
        cmd_t cmd = CMD_NONE;
        bool  ptt_change;
        bool  want;
        bool  on;
        bool  bg = false;

        pthread_mutex_lock(&S.lock);
        while (!S.quit && S.cmd == CMD_NONE && S.ptt_want == S.ptt_on &&
               !S.vol_pending &&
               !(S.pause_want != S.pause_applied && S.module_idx >= 0)) {
            bool watch = (S.state == BT_AUDIO_ON ||
                          S.state == BT_AUDIO_NO_SINK);

            if (S.ptt_on || watch) {
                struct timespec ts;

                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += S.ptt_on ? 1 : BG_POLL_S;
                if (pthread_cond_timedwait(&S.cond, &S.lock, &ts)
                    == ETIMEDOUT) {
                    bg = !S.ptt_on;
                    break;
                }
            } else {
                pthread_cond_wait(&S.cond, &S.lock);
            }
        }
        if (S.quit) {
            pthread_mutex_unlock(&S.lock);
            break;
        }

        want = S.ptt_want;
        on = S.ptt_on;
        ptt_change = (want != on);

        /* PTT goes ahead of everything else: a poll must never delay
         * unkeying. Any queued command stays queued for the next turn. */
        if (!ptt_change) {
            cmd = S.cmd;
            S.cmd = CMD_NONE;
            if (cmd == CMD_ON || cmd == CMD_RELOAD) {
                S.state = BT_AUDIO_STARTING;
            } else if (cmd == CMD_OFF) {
                S.state = BT_AUDIO_STOPPING;
            }
        }
        pthread_mutex_unlock(&S.lock);

        if (ptt_change) {
            if (want) {
                do_ptt_on();
            } else {
                do_ptt_off(BT_PTT_OFF);
            }
            speaker_follow();
            apply_volume();
            apply_pause();
            continue;
        }

        if (on) {
            ptt_watch();
        }

        switch (cmd) {
            case CMD_ON:
            case CMD_RELOAD:
                do_on();
                break;
            case CMD_OFF:
                do_off();
                break;
            case CMD_POLL:
                do_poll(true);
                break;
            default:
                if (bg) {
                    do_poll(false);
                }
                break;
        }

        speaker_follow();
        apply_volume();
        apply_pause();
    }

    /* Never leave the transmitter keyed behind a dead thread. */
    if (S.ptt_on) {
        do_ptt_off(BT_PTT_OFF);
    }
    /* Nor the speaker silent. */
    if (S.speaker_handed) {
        S.speaker_handed = false;
        radio_set_volume_sink(NULL);
    }
    return NULL;
}

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

    if (start && pthread_create(&S.thread, NULL, worker, NULL) == 0) {
        S.thread_valid = true;
    }
}

static void post(cmd_t cmd)
{
    ensure_thread();

    pthread_mutex_lock(&S.lock);
    S.cmd = cmd;
    pthread_cond_signal(&S.cond);
    pthread_mutex_unlock(&S.lock);
}

/* ------------------------------------------------------------------ */

static void headset_start(void);
static void headset_stop(void);

void bt_audio_init(void)
{
    ensure_thread();
    headset_start();
}

void bt_audio_deinit(void)
{
    if (!S.thread_valid) {
        return;
    }

    bt_ptt_request(false);

    /* Leaving a loopback behind would keep feeding a sink nobody is
     * managing any more. */
    post(CMD_OFF);
    usleep(200000);

    pthread_mutex_lock(&S.lock);
    S.quit = true;
    pthread_cond_signal(&S.cond);
    pthread_mutex_unlock(&S.lock);

    pthread_join(S.thread, NULL);
    S.thread_valid = false;

    headset_stop();
}

void bt_audio_request(bool on)
{
    post(on ? CMD_ON : CMD_OFF);
}

bt_audio_state_t bt_audio_state(void)
{
    bt_audio_state_t st;

    ensure_thread();

    pthread_mutex_lock(&S.lock);
    st = S.state;
    pthread_mutex_unlock(&S.lock);
    return st;
}

const char *bt_audio_sink(void)
{
    return S.sink;
}

uint32_t bt_audio_measured_ms(void)
{
    uint32_t ms;

    pthread_mutex_lock(&S.lock);
    ms = S.measured_ms;
    pthread_mutex_unlock(&S.lock);
    return ms;
}

uint32_t bt_audio_latency(void)
{
    return S.latency_ms;
}

void bt_audio_set_latency(uint32_t ms)
{
    bool running;

    if (ms < LAT_MIN || ms > LAT_MAX) {
        return;
    }

    pthread_mutex_lock(&S.lock);
    S.latency_ms = ms;
    running = (S.state == BT_AUDIO_ON);
    pthread_mutex_unlock(&S.lock);

    conf_save();

    if (running) {
        post(CMD_RELOAD);
    }
}

void bt_audio_poll(void)
{
    /* A poll must not overwrite a pending On/Off. */
    ensure_thread();

    pthread_mutex_lock(&S.lock);
    if (S.cmd == CMD_NONE) {
        S.cmd = CMD_POLL;
        pthread_cond_signal(&S.cond);
    }
    pthread_mutex_unlock(&S.lock);
}

void bt_ptt_request(bool tx)
{
    ensure_thread();

    pthread_mutex_lock(&S.lock);
    S.ptt_want = tx;
    if (tx && S.ptt_state != BT_PTT_TX) {
        S.ptt_state = BT_PTT_KEYING;
    }
    pthread_cond_signal(&S.cond);
    pthread_mutex_unlock(&S.lock);
}

bt_ptt_state_t bt_ptt_state(void)
{
    bt_ptt_state_t st;

    pthread_mutex_lock(&S.lock);
    st = S.ptt_state;
    pthread_mutex_unlock(&S.lock);
    return st;
}

uint32_t bt_ptt_seconds(void)
{
    time_t since;
    bool   on;

    pthread_mutex_lock(&S.lock);
    since = S.tx_since;
    on = S.ptt_on;
    pthread_mutex_unlock(&S.lock);

    if (!on || since == 0) {
        return 0;
    }
    return (uint32_t)(now_s() - since);
}

bool bt_audio_headset_ptt(void)
{
    bool on;

    ensure_thread();
    pthread_mutex_lock(&S.lock);
    on = S.headset_ptt;
    pthread_mutex_unlock(&S.lock);
    return on;
}

/* ==================================================================
 *  Headset button as PTT
 * ==================================================================
 *
 * Measured with btmon on a Jabra headset (2026-10-04): a short press of
 * the main button sends, at once,
 *
 *     AT+ANDROID=MGNP,0904ss081A08vvbb<CR>
 *
 * on the HFP RFCOMM channel - ss a sequence number, vv the volume keys
 * (01 up, 02 down), bb the main button (00 short, 01 long, 02 double).
 * The same in A2DP and in HFP. HSP headsets send AT+CKPD=200 instead.
 * Only a long press (about two seconds) produces an AVRCP command, far
 * too slow for PTT.
 *
 * The RFCOMM channel belongs to PulseAudio, which answers OK and drops
 * the line. So the line is read where btmon reads it: the kernel's HCI
 * monitor channel, a read-only copy of all Bluetooth traffic. Nothing is
 * sent and nothing PulseAudio does changes. Only ACL packets received
 * from a device are looked at; L2CAP fragments are put back together
 * first (the first press in a session arrived split in two).
 *
 * The monitor sees every device, so a press counts only when the frame
 * came over the ACL link of the headset PTT uses (ptt_device()). The
 * link's handle is asked from the kernel (HCIGETCONNINFO) at the press
 * itself, so a headset connected before this thread started is found as
 * well. Anything else that sends the same bytes, paired or not, is
 * logged and ignored.
 *
 * The headset sends no release, so the button is a toggle: press to
 * key, press again to unkey. BT_PTT_TIMEOUT_S still drops a forgotten
 * transmission. Every AT line seen goes to /mnt/bt_audio.log, which is
 * how another headset's button would be found and added.
 */

#define HS_DEBOUNCE_MS  400u
#define HS_REASM_MAX    1024u

/* Monitor channel packet header (kernel: struct hci_mon_hdr). */
#define HS_MON_ACL_RX   5u

static struct {
    pthread_t        thread;
    bool             thread_valid;
    volatile bool    quit;
    uint64_t         last_press_ms;
    char             last_seq[3];

    /* L2CAP reassembly of one ACL handle at a time. */
    uint16_t         dev;           /* controller index (hci0 = 0) */
    uint16_t         handle;
    uint16_t         want;          /* total L2CAP frame length */
    uint16_t         have;
    uint8_t          buf[HS_REASM_MAX];
} H;

static uint64_t hs_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void hs_log(const char *line, const char *what)
{
    bt_log("headset: %s: %s", line, what);
}

/* ACL handle of `mac` ("AA_BB_CC_DD_EE_FF" as PulseAudio names it) on
 * controller `dev`, or -1 when it has no ACL link there. */
static int hs_acl_handle(uint16_t dev, const char *mac)
{
    char                      addr[18];
    struct sockaddr_hci       a;
    struct hci_conn_info_req *cr;
    int                       fd;
    int                       handle = -1;

    if (mac == NULL || strlen(mac) != 17) {
        return -1;
    }
    for (size_t i = 0; i < 17; i++) {
        addr[i] = (mac[i] == '_') ? ':' : mac[i];
    }
    addr[17] = '\0';

    cr = calloc(1, sizeof(*cr) + sizeof(struct hci_conn_info));
    if (cr == NULL) {
        return -1;
    }
    if (str2ba(addr, &cr->bdaddr) < 0) {
        free(cr);
        return -1;
    }
    cr->type = ACL_LINK;

    fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd >= 0) {
        memset(&a, 0, sizeof(a));
        a.hci_family = AF_BLUETOOTH;
        a.hci_dev = dev;
        a.hci_channel = HCI_CHANNEL_RAW;
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
            ioctl(fd, HCIGETCONNINFO, (unsigned long)cr) == 0) {
            handle = cr->conn_info->handle & 0x0fff;
        }
        close(fd);
    }
    free(cr);
    return handle;
}

/* True when a frame on (dev, handle) came from the headset PTT uses. */
static bool hs_from_headset(const char *line, uint16_t dev, uint16_t handle)
{
    char mac[32];
    char what[96];
    int  h;

    ptt_device(mac, sizeof(mac));
    if (mac[0] == '\0') {
        hs_log(line, "ignored (no headset)");
        return false;
    }
    h = hs_acl_handle(dev, mac);
    if (h != (int)handle) {
        snprintf(what, sizeof(what),
                 "ignored (handle 0x%03X is not the headset %s)",
                 handle, mac);
        hs_log(line, what);
        return false;
    }
    return true;
}

static void hs_toggle(const char *line)
{
    uint64_t       now = hs_now_ms();
    bt_ptt_state_t st;

    if (now - H.last_press_ms < HS_DEBOUNCE_MS) {
        hs_log(line, "ignored (debounce)");
        return;
    }
    H.last_press_ms = now;

    if (!bt_audio_headset_ptt()) {
        hs_log(line, "ignored (headset_ptt=0)");
        return;
    }

    st = bt_ptt_state();
    if (st == BT_PTT_TX || st == BT_PTT_KEYING) {
        bt_ptt_request(false);
        msg_schedule_text_fmt("PTT off (headset)");
        hs_log(line, "PTT off");
    } else {
        bt_ptt_request(true);
        msg_schedule_text_fmt("PTT on (headset)");
        hs_log(line, "PTT on");
    }
}

/* One AT line from a device, CR/LF stripped. */
static void hs_at_line(const char *line, uint16_t dev, uint16_t handle)
{
    static const char jabra[] = "AT+ANDROID=MGNP,";
    size_t n = strlen(line);

    if (strncmp(line, jabra, sizeof(jabra) - 1) == 0) {
        const char *arg = line + sizeof(jabra) - 1;

        /* 0904 ss 081A08 vv bb: 16 hex digits */
        if (strlen(arg) != 16) {
            hs_log(line, "unknown layout");
            return;
        }
        if (strcmp(arg + 12, "0000") != 0) {
            hs_log(line, "not PTT");            /* long, double, volume */
            return;
        }
        if (!hs_from_headset(line, dev, handle)) {
            return;
        }
        /* A repeated sequence number is the same event again. */
        if (strncmp(arg + 4, H.last_seq, 2) == 0) {
            hs_log(line, "ignored (repeat)");
            return;
        }
        memcpy(H.last_seq, arg + 4, 2);
        H.last_seq[2] = '\0';

        hs_toggle(line);                        /* short press */
        return;
    }

    if (strcmp(line, "AT+CKPD=200") == 0) {
        if (hs_from_headset(line, dev, handle)) {
            hs_toggle(line);                    /* HSP headset button */
        }
        return;
    }

    /* The rest of the HFP/HSP chatter (AT+VGS, AT+BRSF, ...): all of it,
     * an unknown headset's PTT button is found this way. */
    if (n > 0) {
        hs_log(line, "-");
    }
}

/* AVRCP button from a headset: AVCTP header (transaction, packet type,
 * C/R, IPID), PID 0x110E, then the AV/C frame: ctype, subunit 0x48
 * (panel), opcode 0x7C (PASS THROUGH), operation id with bit 7 set on
 * release. Logged only, so a headset whose PTT button speaks AVRCP (the
 * Icom VS-3 sent no AT line at all) can be identified from the log. */
static const char *avrcp_op_name(uint8_t op)
{
    switch (op) {
        case 0x41: return "VOLUME_UP";
        case 0x42: return "VOLUME_DOWN";
        case 0x43: return "MUTE";
        case 0x44: return "PLAY";
        case 0x45: return "STOP";
        case 0x46: return "PAUSE";
        case 0x48: return "REWIND";
        case 0x49: return "FAST_FORWARD";
        case 0x4B: return "FORWARD";
        case 0x4C: return "BACKWARD";
        default:   return NULL;
    }
}

static bool hs_avrcp(const uint8_t *p, size_t n)
{
    char        what[48];
    const char *name;
    uint8_t     op;

    if (n < 7 || p[1] != 0x11 || p[2] != 0x0E) {
        return false;
    }
    if ((p[0] & 0x02) != 0) {       /* a response, not a button */
        return true;
    }
    if (p[4] != 0x48 || p[5] != 0x7C) {
        return true;                /* other AVRCP traffic */
    }
    op = p[6] & 0x7F;
    name = avrcp_op_name(op);
    if (name != NULL) {
        snprintf(what, sizeof(what), "AVRCP %s %s", name,
                 (p[6] & 0x80) ? "released" : "pressed");
    } else {
        snprintf(what, sizeof(what), "AVRCP op 0x%02X %s", op,
                 (p[6] & 0x80) ? "released" : "pressed");
    }
    bt_log("headset: %s", what);
    return true;
}

/* A whole L2CAP frame from a device: look for AT lines in it. RFCOMM
 * carries them as plain text inside a UIH frame, so a scan for "AT"
 * up to CR is enough and needs no RFCOMM parsing. */
static void hs_l2cap(const uint8_t *p, size_t n, uint16_t dev, uint16_t handle)
{
    if (hs_avrcp(p, n)) {
        return;
    }

    for (size_t i = 0; i + 3 <= n; i++) {
        /* AT+..., AT*... and also ATA (answer) / ATD (dial): an HFP
         * headset's button may send those instead. */
        if (p[i] != 'A' || p[i + 1] != 'T' ||
            (p[i + 2] != '+' && p[i + 2] != '*' &&
             (p[i + 2] < 'A' || p[i + 2] > 'Z'))) {
            continue;
        }

        char   line[96];
        size_t o = 0;
        size_t j = i;

        while (j < n && p[j] != '\r' && p[j] != '\n' && o + 1 < sizeof(line)) {
            if (p[j] < 0x20 || p[j] > 0x7e) {
                break;
            }
            line[o++] = (char)p[j++];
        }
        line[o] = '\0';
        if (j < n && (p[j] == '\r' || p[j] == '\n')) {
            hs_at_line(line, dev, handle);
        }
        i = j;
    }
}

/* One ACL data packet received from a device on controller `dev`
 * (monitor payload). */
static void hs_acl_rx(uint16_t dev, const uint8_t *p, size_t n)
{
    uint16_t hf, dlen, handle, pb;

    if (n < 4) {
        return;
    }
    hf = (uint16_t)(p[0] | (p[1] << 8));
    dlen = (uint16_t)(p[2] | (p[3] << 8));
    handle = hf & 0x0fff;
    pb = (hf >> 12) & 0x3;
    p += 4;
    n -= 4;
    if (dlen < n) {
        n = dlen;
    }

    if (pb != 0x1) {                    /* start of an L2CAP frame */
        if (n < 4) {
            H.want = 0;
            return;
        }
        H.dev = dev;
        H.handle = handle;
        H.want = (uint16_t)(4 + (p[0] | (p[1] << 8)));
        H.have = 0;
        if (H.want > HS_REASM_MAX) {    /* A2DP media: not for us */
            H.want = 0;
            return;
        }
    } else if (H.want == 0 || handle != H.handle || dev != H.dev) {
        return;                         /* continuation of something else */
    }

    if ((size_t)H.have + n > H.want) {
        H.want = 0;
        return;
    }
    memcpy(H.buf + H.have, p, n);
    H.have = (uint16_t)(H.have + n);

    if (H.have == H.want) {
        hs_l2cap(H.buf + 4, H.want - 4u, H.dev, H.handle);
        H.want = 0;
    }
}

static int hs_open_monitor(void)
{
    struct sockaddr_hci a;
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);

    if (fd < 0) {
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.hci_family = AF_BLUETOOTH;
    a.hci_dev = HCI_DEV_NONE;
    a.hci_channel = HCI_CHANNEL_MONITOR;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *hs_thread(void *arg)
{
    uint8_t pkt[2048];
    int     fd = -1;

    (void)arg;

    while (!H.quit) {
        struct pollfd pfd;
        ssize_t       r;

        if (fd < 0) {
            fd = hs_open_monitor();
            if (fd < 0) {
                hs_log("monitor", strerror(errno));
                sleep(5);
                continue;
            }
            hs_log("monitor", "open");
        }

        /* Half a second, so quit is noticed. */
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 500) <= 0) {
            continue;
        }

        r = read(fd, pkt, sizeof(pkt));
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            hs_log("monitor", strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        if (r < 6) {
            continue;
        }

        /* opcode, controller index, payload length - little endian */
        uint16_t op = (uint16_t)(pkt[0] | (pkt[1] << 8));
        uint16_t idx = (uint16_t)(pkt[2] | (pkt[3] << 8));
        uint16_t len = (uint16_t)(pkt[4] | (pkt[5] << 8));

        if (op != HS_MON_ACL_RX) {
            continue;
        }
        if ((size_t)len > (size_t)r - 6) {
            len = (uint16_t)(r - 6);
        }
        hs_acl_rx(idx, pkt + 6, len);
    }

    if (fd >= 0) {
        close(fd);
    }
    return NULL;
}

static void headset_start(void)
{
    if (H.thread_valid) {
        return;
    }
    H.quit = false;
    if (pthread_create(&H.thread, NULL, hs_thread, NULL) == 0) {
        H.thread_valid = true;
    }
}

static void headset_stop(void)
{
    if (!H.thread_valid) {
        return;
    }
    H.quit = true;
    pthread_join(H.thread, NULL);
    H.thread_valid = false;
}

int bt_audio_mic_gain(void)
{
    int db;

    ensure_thread();
    pthread_mutex_lock(&S.lock);
    db = S.mic_gain_db;
    pthread_mutex_unlock(&S.lock);
    return db;
}

int bt_audio_set_mic_gain(int db)
{
    ensure_thread();

    if (db < MIC_GAIN_MIN) db = MIC_GAIN_MIN;
    if (db > MIC_GAIN_MAX) db = MIC_GAIN_MAX;

    pthread_mutex_lock(&S.lock);
    S.mic_gain_db = db;
    conf_save();
    /* While transmitting the change is heard at once. */
    if (S.ptt_on) {
        audio_set_play_vol((float)db);
    }
    pthread_mutex_unlock(&S.lock);
    return db;
}

bool bt_audio_playback_begin(char *sink, size_t sink_sz)
{
    bool on;

    if (sink == NULL || sink_sz == 0) {
        return false;
    }
    sink[0] = '\0';
    if (!S.thread_valid) {
        return false;
    }

    pthread_mutex_lock(&S.lock);
    on = (S.state == BT_AUDIO_ON && S.sink[0] != '\0');
    if (on) {
        snprintf(sink, sink_sz, "%s", S.sink);
        S.pause_want = true;
        pthread_cond_signal(&S.cond);
    }
    pthread_mutex_unlock(&S.lock);
    return on;
}

void bt_audio_playback_end(void)
{
    if (!S.thread_valid) {
        return;
    }
    pthread_mutex_lock(&S.lock);
    if (S.pause_want) {
        S.pause_want = false;
        pthread_cond_signal(&S.cond);
    }
    pthread_mutex_unlock(&S.lock);
}
