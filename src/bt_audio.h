/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_audio.h - Bluetooth headset audio: listening and PTT.
 *
 * Listening: a PulseAudio loopback from the radio's capture device to
 * the Bluetooth sink (A2DP, or HFP once PTT has been used).
 * While it plays into connected headphones the radio's own speaker is
 * silent and the VOL knob (and mute) set the headphone volume, through
 * radio_set_volume_sink(). If the headphones go away the speaker comes
 * back, and listening resumes by itself when they reconnect.
 *
 * PTT: the headset microphone goes to the radio's transmitter. The card
 * is switched to HFP (handsfree_head_unit), a second loopback carries
 * the HFP source into the codec output and the BASE is keyed with
 * radio_set_modem(), as the voice messages do. Record mode
 * (AUDIO_PLAY_ON) is not used: with it set the radio did not key.
 *
 * Headset button: a short press of the headset's main button toggles
 * PTT (see bt_audio.c).
 *
 * HFP needs the musb patch in the kernel (AetherX6100Buildroot
 * 07a8d25, "Add BT patch for HSP/HFP duplex support"); without it the
 * SCO link dies after a few seconds. The card stays in HFP after the
 * first PTT, because switching profiles takes one to two seconds and
 * would eat the start of every over.
 *
 * All pactl calls run on one worker thread. The UI only reads cached
 * state and posts requests; every function here is safe to call from
 * the LVGL thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_AUDIO_OFF = 0,
    BT_AUDIO_STARTING,
    BT_AUDIO_ON,
    BT_AUDIO_STOPPING,
    BT_AUDIO_NO_SINK,   /* nothing connected to send audio to */
    BT_AUDIO_FAILED,
} bt_audio_state_t;

typedef enum {
    BT_PTT_OFF = 0,     /* receiving */
    BT_PTT_KEYING,      /* profile switch / loopback load in progress */
    BT_PTT_TX,          /* transmitting from the headset microphone */
    BT_PTT_UNKEYING,
    BT_PTT_NO_MIC,      /* no HFP source: headset gone or no HFP */
    BT_PTT_FAILED,      /* loopback could not be loaded */
    BT_PTT_TIMEOUT,     /* unkeyed by the TX timeout */
} bt_ptt_state_t;

/* TX is dropped after this many seconds even if nobody unkeys. */
#define BT_PTT_TIMEOUT_S 180u

void bt_audio_init(void);
void bt_audio_deinit(void);

/* Asynchronous; watch bt_audio_state(). */
void bt_audio_request(bool on);

bt_audio_state_t bt_audio_state(void);

/* Sink the loopback feeds, "" when none was found. */
const char *bt_audio_sink(void);

/* Measured delay of the listening loopback, 0 when not running. */
uint32_t bt_audio_measured_ms(void);

/* Requested buffer depth of the listening loopback, from
 * /mnt/bt_audio.conf (latency_msec=). No longer on a button. */
uint32_t bt_audio_latency(void);
void     bt_audio_set_latency(uint32_t ms);

/* Asks the worker to re-read the sink list and the measured latency.
 * Call from a UI timer, not from anywhere hot. */
void bt_audio_poll(void);

/* PTT from the Bluetooth headset microphone. Asynchronous and safe to
 * call repeatedly; the worker applies the last requested state. */
void           bt_ptt_request(bool tx);
bt_ptt_state_t bt_ptt_state(void);

/* Seconds since keying, 0 when not transmitting. */
uint32_t bt_ptt_seconds(void);

/* Headset microphone level while transmitting: the codec playback
 * volume in dB (-10..10, default 0), restored to the Play gain from
 * Settings after each transmission. Kept in /mnt/bt_audio.conf.
 * Returns the value actually set. */
int bt_audio_mic_gain(void);
int bt_audio_set_mic_gain(int db);

/* A short press of the headset's main button toggles PTT (read from the
 * HCI monitor channel, see bt_audio.c). /mnt/bt_audio.conf
 * headset_ptt=0 turns it off; on by default. */
bool bt_audio_headset_ptt(void);

/* Local playback (TTS, voice message preview, recordings). While
 * listening plays into connected headphones, copies the Bluetooth sink
 * name into `sink`, mutes the listening loopback and returns true: open
 * the player on that sink (audio_create_player_on()) instead of using
 * AUDIO_PLAY_ON, and call bt_audio_playback_end() when done. Returns
 * false otherwise: play locally as before. Never blocks. */
bool bt_audio_playback_begin(char *sink, size_t sink_sz);
void bt_audio_playback_end(void);

#ifdef __cplusplus
}
#endif
