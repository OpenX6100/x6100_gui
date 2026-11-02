/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdbool.h>
#include <aether_radio/x6100_control/control.h>

#include "lvgl/lvgl.h"
#include "cfg/subject_api.h"

#define RADIO_SAMPLES   (512)

typedef enum {
    RADIO_RX = 0,
    RADIO_TX,
    RADIO_ATU_START,
    RADIO_ATU_WAIT,
    RADIO_ATU_RUN,
    RADIO_SWRSCAN,

    RADIO_POWEROFF,
    RADIO_OFF
} radio_state_t;

typedef enum {
    RADIO_CHARGER_OFF = 0,
    RADIO_CHARGER_ON,
    RADIO_CHARGER_SHADOW
} radio_charger_t;

typedef void (*radio_rx_tx_change_t) (bool tx);

void radio_init();
void radio_start();
void radio_shutdown();
void radio_bb_reset();
radio_state_t radio_get_state();

extern SubjectInt *radio_fg_freq_subj; // FG freq according split and tx
extern SubjectInt *radio_bg_freq_subj; // BG freq according split and tx

/**
 * Set freq for radio without updating corresponding subject.
 * Useful for FT8 TX freq change and SWR scan
 */
void radio_set_freq(int32_t freq);
bool radio_check_freq(int32_t freq);

x6100_vfo_t radio_toggle_vfo();

uint16_t radio_change_vol(int16_t df);

void radio_change_mute();

/* Bluetooth listening (bt_audio.c): while fn is set the radio's own
 * speaker stays silent and every volume change - VOL knob, Volume
 * button, CAT, mute - goes to fn instead, as 0..55 (0 when muted).
 * fn is also called at once with the current volume. NULL gives the
 * speaker its volume back. fn must not block: it runs on the thread
 * that changed the volume. */
typedef void (*radio_vol_sink_t)(int32_t vol);
void radio_set_volume_sink(radio_vol_sink_t fn);

void radio_set_pwr(float d);
/* Radio only, not saved: the settings keep cfg.dsp.tx_filter_low/high. */
void radio_set_tx_filter(uint16_t low, uint16_t high);
/* Noise reduction, noise blanker, notch and auto-notch off (the base
 * applies them in every mode, DIGI included), or back to the settings.
 * Radio only, not saved. */
void radio_set_rx_dsp_off(bool off);

void radio_set_charger(bool on);

void radio_start_atu();

bool radio_start_swrscan();
void radio_stop_swrscan();

/**
 * Register a callback invoked from the radio thread for each SWR-scan sample.
 * The callback must return quickly and must not take the radio lock itself
 * beyond what radio_set_freq already does. Registering NULL unregisters; the
 * call waits for any in-flight callback to finish, so the consumer's state is
 * guaranteed to be unused once radio_swrscan_set_cb(NULL) returns.
 */
typedef void (*radio_swrscan_cb_t)(float vswr);
void radio_swrscan_set_cb(radio_swrscan_cb_t cb);

/**
 * Power telemetry published by the radio thread (external/battery voltage,
 * battery capacity and charging flag). Consumers (e.g. the clock widget)
 * register a callback via radio_power_set_cb() and copy the scalars into their
 * own state; they must not do UI work in the callback, it runs on the radio
 * thread.
 */
typedef struct {
    float   vext;
    float   vbat;
    uint8_t cap;
    bool    charging;
} radio_power_t;

typedef void (*radio_power_cb_t)(const radio_power_t *power);

/**
 * Register the power-telemetry callback. The callback must return quickly and
 * must not take the radio lock. Registering NULL unregisters; the call waits
 * for any in-flight callback to finish, so the consumer's state is guaranteed
 * to be unused once radio_power_set_cb(NULL) returns.
 */
void radio_power_set_cb(radio_power_cb_t cb);

void radio_poweroff();
void radio_set_ptt(bool tx);
void radio_set_modem(bool tx);
/* The base plays this app's audio on the speaker (as voice prompts and the
 * recorder do) - and while it does, the audio it sends us isn't the
 * receiver's. Off puts the mics back. */
void radio_speaker_play(bool on);

void radio_set_morse_key(bool on);

int8_t radio_get_audio_in_lvl();
