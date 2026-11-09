/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#pragma once

#include <ft8lib/constants.h>

#include <complex.h>
#include <stdbool.h>
#include <stdint.h>

/// @brief Callback for decoded message
typedef void (*decoded_msg_cb)(const char *text, int snr, float freq_hz, float time_sec, void *user_data);

/// @brief Init worker structures
/// @param[in] sample_rate Input audio sample rate
/// @param[in] protocol protocol (FT8/FT4)
void ftx_worker_init(int sample_rate, ftx_protocol_t protocol);

/// @brief Free internal structures
void ftx_worker_free();

/// @brief Reset state before receiving new time slot
void ftx_worker_reset();

/**
 * Own callsign, kept in the callsign hash table so a reply to a
 * non-standard call ("<KF0QGT/6Y> W1ABC -05") decodes. Set before
 * ftx_worker_init().
 */
void ftx_worker_set_own_call(const char *call);

/// @brief What the extra decoding passes (FT8/FT4, as WSJT-X) have found so far
/// in the slot just ended: what was decoded is taken out of its audio and the
/// rest searched again, on a thread of their own. Call often from the audio
/// thread; nothing to do for FT2. Each find comes with the slot it was heard in,
/// even one that arrives after the next boundary.
typedef void (*extra_msg_cb)(const char *text, int snr, float freq_hz, float time_sec, bool odd, int64_t start_ms,
                             void *user_data);
void ftx_worker_decode_more(extra_msg_cb msg_cb, void *user_data);

/// @brief The slot about to end, for its extra passes' finds. Call before the
/// slot's last ftx_worker_decode().
void ftx_worker_extra_slot(bool odd, int64_t start_ms);

/// @brief Whether the extra passes are still working (PC tests on recordings)
bool ftx_worker_extra_busy(void);

/// @brief Off: the extra decoding passes have no time limit (PC tests on recordings)
void ftx_worker_set_realtime(bool on);

/// @brief Generate audio samples for TX
/// @param[in] text message to send
/// @param[in] signal_freq base signal frequency
/// @param[in] sample_rate output sample rate
/// @param[out] samples pointer to generated audio samples
/// @param[out] n_samples count of samples
/// @return success flag
bool ftx_worker_generate_tx_samples(const char *text, const uint16_t signal_freq, const uint32_t sample_rate,
                                    int16_t **samples, uint32_t *n_samples);

/// @brief Process RX audio samples
/// @param[in] samples audio samples
/// @param[in] n_samples count of samples
void ftx_worker_put_rx_samples(float *samples, uint32_t n_samples);

/// @brief Decode messages
/// @param[in] msg_cb callback for decoded messages
/// @param[in] last flag to perform more heavy search of messages
/// @param[in] user_data pointer to any information to pass to `msg_cb`
void ftx_worker_decode(decoded_msg_cb msg_cb, bool last, void *user_data);

/// @brief Return block size
int ftx_worker_get_block_size();

/// @brief Check that wf is full
bool ftx_worker_is_full();

