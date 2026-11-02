/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY protocol constants, CRC-12 and
 *  tail-biting convolutional encoder
 */

#include "jtty_proto.h"

/* 4-FSK synchronization sequence at the start of every frame. */
const uint8_t jtty_sync[JTTY_NSYNC] = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};

static unsigned parity(unsigned v)
{
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

uint16_t jtty_crc12(jtty_payload_t payload)
{
    unsigned crc = 0;

    for (int i = JTTY_PAYLOAD_BITS - 1; i >= 0; i--) {
        crc ^= (unsigned)((payload >> i) & 1u) << (JTTY_CRC_BITS - 1);
        if (crc & (1u << (JTTY_CRC_BITS - 1))) {
            crc = (crc << 1) ^ JTTY_CRC_POLY;
        } else {
            crc <<= 1;
        }
        crc &= (1u << JTTY_CRC_BITS) - 1;
    }
    return (uint16_t)crc;
}

void jtty_info_bits(jtty_payload_t payload, uint8_t info[JTTY_INFO_BITS])
{
    uint16_t crc = jtty_crc12(payload);

    for (int i = 0; i < JTTY_PAYLOAD_BITS; i++) {
        info[i] = (uint8_t)((payload >> (JTTY_PAYLOAD_BITS - 1 - i)) & 1u);
    }
    for (int i = 0; i < JTTY_CRC_BITS; i++) {
        info[JTTY_PAYLOAD_BITS + i] = (uint8_t)((crc >> (JTTY_CRC_BITS - 1 - i)) & 1u);
    }
}

bool jtty_info_payload(const uint8_t info[JTTY_INFO_BITS], jtty_payload_t *payload)
{
    jtty_payload_t p = 0;
    unsigned       crc = 0;

    for (int i = 0; i < JTTY_PAYLOAD_BITS; i++) {
        p = (p << 1) | (info[i] & 1u);
    }
    for (int i = 0; i < JTTY_CRC_BITS; i++) {
        crc = (crc << 1) | (info[JTTY_PAYLOAD_BITS + i] & 1u);
    }
    *payload = p;
    return crc == jtty_crc12(p);
}

uint8_t jtty_branch_tone(unsigned state, unsigned dropped)
{
    static const uint8_t gray[4] = {0, 1, 3, 2};
    unsigned reg = (state & (JTTY_NSTATES - 1)) | ((dropped & 1u) << JTTY_NU);
    unsigned b0 = parity(reg & JTTY_G0);
    unsigned b1 = parity(reg & JTTY_G1);

    return gray[(b0 << 1) | b1];
}

void jtty_frame_tones(jtty_payload_t payload, uint8_t tones[JTTY_NSYM])
{
    uint8_t  info[JTTY_INFO_BITS];
    unsigned state = 0;

    jtty_info_bits(payload, info);

    for (int i = 0; i < JTTY_NSYNC; i++) {
        tones[i] = jtty_sync[i];
    }

    /* Tail-biting: the register starts with the frame's own last NU
     * information bits, so it ends in the state it started in. */
    for (int i = JTTY_INFO_BITS - JTTY_NU; i < JTTY_INFO_BITS; i++) {
        state = ((state << 1) | info[i]) & (JTTY_NSTATES - 1);
    }
    for (int t = 0; t < JTTY_INFO_BITS; t++) {
        unsigned dropped = (state >> (JTTY_NU - 1)) & 1u;

        state = ((state << 1) | info[t]) & (JTTY_NSTATES - 1);
        tones[JTTY_NSYNC + t] = jtty_branch_tone(state, dropped);
    }
}
