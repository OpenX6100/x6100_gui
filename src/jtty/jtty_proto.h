/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY protocol constants, CRC-12 and
 *  tail-biting convolutional encoder
 *
 *  Written from the JTTY protocol description (WSJT-X 3.2+,
 *  lib/jtty/jtty_source_encoding.txt): no WSJT-X code is used.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---- Waveform ------------------------------------------------------------
 *
 * 4-GFSK, BT 2.0, modulation index 1: the tone spacing equals the symbol
 * rate, 12000 / 384 = 31.25 Hz. A frame is 13 sync symbols and 46 coded
 * symbols, 1.888 s. Frames of one message follow each other without a
 * gap. */
#define JTTY_RATE           12000
#define JTTY_NSPS           384
#define JTTY_BAUD           (1.0f * JTTY_RATE / JTTY_NSPS)
#define JTTY_TONES          4
#define JTTY_NSYNC          13
#define JTTY_NDATA          46
#define JTTY_NSYM           (JTTY_NSYNC + JTTY_NDATA)
#define JTTY_FRAME_SAMPLES  (JTTY_NSYM * JTTY_NSPS)
#define JTTY_BW_HZ          (JTTY_TONES * JTTY_BAUD)

/* ---- Bits ----------------------------------------------------------------
 *
 * 34 source bits per frame, numbered 1..34 from the left: 1-32 grammar,
 * 33 reserved (zero), 34 end of message. A CRC-12 makes 46 information
 * bits; a rate-1/2 tail-biting code with K = 10 turns each of them into
 * two coded bits, i.e. one 4-FSK symbol. */
#define JTTY_PAYLOAD_BITS   34
#define JTTY_CRC_BITS       12
#define JTTY_INFO_BITS      (JTTY_PAYLOAD_BITS + JTTY_CRC_BITS)

#define JTTY_NU             9
#define JTTY_NSTATES        (1 << JTTY_NU)
#define JTTY_G0             0x277   /* octal 1167 */
#define JTTY_G1             0x365   /* octal 1545 */
#define JTTY_CRC_POLY       0x80F

/* Payload as an integer: bit 1 is the most significant of 34. */
typedef uint64_t jtty_payload_t;

#define JTTY_EOM_BIT        ((jtty_payload_t)1)          /* bit 34 */
#define JTTY_RESERVED_BIT   ((jtty_payload_t)2)          /* bit 33 */
#define JTTY_GRAMMAR(p)     ((uint32_t)((p) >> 2))       /* bits 1-32 */

extern const uint8_t jtty_sync[JTTY_NSYNC];

/* CRC-12 of the 34 payload bits. */
uint16_t jtty_crc12(jtty_payload_t payload);

/* 46 information bits (payload then CRC, most significant first) from a
 * payload, and back: false when the CRC does not match. */
void jtty_info_bits(jtty_payload_t payload, uint8_t info[JTTY_INFO_BITS]);
bool jtty_info_payload(const uint8_t info[JTTY_INFO_BITS], jtty_payload_t *payload);

/* Tone (0-3) emitted on the transition into `state` (the last 9 input
 * bits, newest in bit 0) when `dropped` was the bit that left the
 * register. Gray mapping of the coded bit pair: 00 01 11 10 -> 0 1 2 3. */
uint8_t jtty_branch_tone(unsigned state, unsigned dropped);

/* Channel tones of one frame: sync, then the 46 coded symbols. */
void jtty_frame_tones(jtty_payload_t payload, uint8_t tones[JTTY_NSYM]);
