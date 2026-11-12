/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 TX player
 *
 *  Copyright (c) 2026
 */

/*
 *  Synthesises an FT8/FT4 TX waveform from tx_text and pushes it to the
 *  audio output with per-block ALC-based gain correction. Blocks for the
 *  duration of one slot, so it is invoked from the decode thread between
 *  RX frames.
 *
 *  The caller supplies an abort-check callback that the worker polls each
 *  block, so dialog state (e.g. dialog_ft8.c's ft8_state_t) can preempt
 *  TX without this module owning that state itself.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../ports/app_ports.h"

/* FT8 audio tone offset inside the radio passband: the VFO moves by
 * tx_freq - FT8_TX_AUDIO_HZ while sending. */
#define FT8_TX_AUDIO_HZ 1325

#ifdef __cplusplus
extern "C" {
#endif

void tx_worker_construct(const app_ports_t *ports, uint32_t sample_rate);
void tx_worker_destruct();

/* Abort-check callback. Return true to stop TX after the current block. */
typedef bool (*tx_abort_fn_t)(void *ctx);

/* Transmit tx_text. Returns true on normal completion (or no-op when
 * sample generation fails); returns false if the abort callback fired
 * mid-transmit. */
bool tx_worker_run(const char    *tx_text,
                   float          base_gain_offset,
                   tx_abort_fn_t  abort_check,
                   void          *abort_check_ctx);

#ifdef __cplusplus
}
#endif
