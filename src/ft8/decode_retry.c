/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4: more tries at a signal that didn't decode
 *
 *  The tries and their limits are JS8's (js8core legacy_decoder.cpp, from
 *  JS8Call, GPL-3.0). Reading the symbols out of the waterfall follows
 *  ft8_lib's decode.c (MIT, Karlis Goba), whose functions for it are static.
 */

#include "decode_retry.h"

#include <ft8lib/constants.h>
#include <ft8lib/crc.h>
#include <ft8lib/ldpc.h>
#include <math.h>
#include <string.h>

#define TRIES 3

/* The most bits each try's result may differ from what was received */
static const int max_hard_errors[TRIES] = {59, 39, 30};
/* How many bits at the start each try marks unknown */
static const int erased_bits[TRIES] = {0, 24, 48};

static float max2(float a, float b) {
    return a >= b ? a : b;
}

static float max4(float a, float b, float c, float d) {
    return max2(max2(a, b), max2(c, d));
}

/* The tone's power, as JS8's first metric (ft8_lib's is the dB) */
static float tone_power(WF_ELEM_T e) {
    return powf(10.0f, WF_ELEM_MAG(e) / 10.0f);
}

static void ft8_symbol(const WF_ELEM_T *mag, float *logl) {
    float s[8];
    for (int j = 0; j < 8; j++) s[j] = tone_power(mag[kFT8_Gray_map[j]]);
    logl[0] = max4(s[4], s[5], s[6], s[7]) - max4(s[0], s[1], s[2], s[3]);
    logl[1] = max4(s[2], s[3], s[6], s[7]) - max4(s[0], s[1], s[4], s[5]);
    logl[2] = max4(s[1], s[3], s[5], s[7]) - max4(s[0], s[2], s[4], s[6]);
}

static void ft4_symbol(const WF_ELEM_T *mag, float *logl) {
    float s[4];
    for (int j = 0; j < 4; j++) s[j] = tone_power(mag[kFT4_Gray_map[j]]);
    logl[0] = max2(s[2], s[3]) - max2(s[0], s[1]);
    logl[1] = max2(s[1], s[3]) - max2(s[0], s[2]);
}

static void extract(const ftx_waterfall_t *wf, const ftx_candidate_t *c, float *logl) {
    int offset = c->time_offset;
    offset     = offset * wf->time_osr + c->time_sub;
    offset     = offset * wf->freq_osr + c->freq_sub;
    offset     = offset * wf->num_bins + c->freq_offset;
    const WF_ELEM_T *mag = wf->mag + offset;
    bool ft4 = wf->protocol == FTX_PROTOCOL_FT4;

    for (int k = 0; k < (ft4 ? FT4_ND : FT8_ND); k++) {
        /* Past the sync symbols */
        int sym = ft4 ? k + (k < 29 ? 5 : (k < 58 ? 9 : 13)) : k + (k < 29 ? 7 : 14);
        int bit = (ft4 ? 2 : 3) * k;
        int block = c->time_offset + sym;
        if (block < 0 || block >= wf->num_blocks) {
            logl[bit] = logl[bit + 1] = 0;
            if (!ft4) logl[bit + 2] = 0;
        } else if (ft4) {
            ft4_symbol(mag + sym * wf->block_stride, logl + bit);
        } else {
            ft8_symbol(mag + sym * wf->block_stride, logl + bit);
        }
    }
}

/* As ft8_lib scales its likelihoods for this decoder */
static void normalize(float *logl) {
    float sum = 0, sum2 = 0;
    for (int i = 0; i < FTX_LDPC_N; i++) {
        sum += logl[i];
        sum2 += logl[i] * logl[i];
    }
    float variance = (sum2 - sum * sum / FTX_LDPC_N) / FTX_LDPC_N;
    if (variance <= 0) return;
    float k = sqrtf(24.0f / variance);
    for (int i = 0; i < FTX_LDPC_N; i++) logl[i] *= k;
}

static bool to_message(const ftx_waterfall_t *wf, const uint8_t *plain, ftx_message_t *message) {
    uint8_t a91[FTX_LDPC_K_BYTES] = {0};
    for (int i = 0; i < FTX_LDPC_K; i++)
        if (plain[i]) a91[i / 8] |= (uint8_t)(0x80 >> (i % 8));

    uint16_t crc = ftx_extract_crc(a91);
    a91[9] &= 0xF8;
    a91[10] &= 0x00;
    if (crc != ftx_compute_crc(a91, 96 - 14)) return false;

    message->hash = crc;
    for (int i = 0; i < 10; i++)
        message->payload[i] = wf->protocol == FTX_PROTOCOL_FT4 ? a91[i] ^ kFT4_XOR_sequence[i] : a91[i];
    return true;
}

bool ftx_decode_retry(const ftx_waterfall_t *wf, const ftx_candidate_t *cand, int max_iterations,
                      ftx_message_t *message) {
    float   logl[FTX_LDPC_N], tried[FTX_LDPC_N];
    uint8_t plain[FTX_LDPC_N];

    extract(wf, cand, logl);
    normalize(logl);

    for (int t = 0; t < TRIES; t++) {
        int errors;
        memcpy(tried, logl, sizeof(tried));
        memset(tried, 0, sizeof(float) * erased_bits[t]);
        bp_decode(tried, max_iterations, plain, &errors);
        if (errors > 0) continue;

        int hard = 0, ones = 0;
        for (int i = 0; i < FTX_LDPC_N; i++) {
            ones += plain[i];
            if (tried[i] != 0 && (tried[i] > 0) != (plain[i] != 0)) hard++;
        }
        if (ones == 0 || hard > max_hard_errors[t]) continue;
        if (to_message(wf, plain, message)) return true;
    }
    return false;
}
