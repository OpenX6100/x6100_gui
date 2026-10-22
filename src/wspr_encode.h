/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  WSPR message encoder: packing, FEC, interleaving, sync merge
 *
 *  Written from scratch against the public, vendor independent
 *  specification: Andy Talbot G4JNT, "The WSPR Coding Process", 2009,
 *  http://www.g4jnt.com/Coding/WSPR_Coding_Process.pdf
 *  That document was written for implementers. The reference wsprd /
 *  WSJT-X sources are GPL-3 and were NOT copied.
 *
 *  Only the standard type 1 message is supported: callsign (up to 6
 *  characters) + 4 character locator + power in dBm. Compound callsigns
 *  and 6 character locators (types 2 and 3) are not handled.
 */

#ifndef WSPR_ENCODE_H
#define WSPR_ENCODE_H

#include <stdint.h>
#include <stddef.h>

#define WSPR_SYMBOLS 162

/*
 * Encode a message into 162 symbols with values 0..3.
 *
 * callsign  : up to 6 characters, A-Z 0-9, case insensitive. The third
 *             character must end up being a digit after optional space
 *             padding at the front, which ordinary callsigns satisfy.
 * locator    : exactly 4 characters, e.g. "JO91". First two A-R, last
 *             two 0-9.
 * power_dbm  : 0..60. WSJT-X only accepts values ending in 0, 3 or 7
 *             (x1, x2, x5), but any value in range is encoded here and
 *             the format check is left to the caller.
 * symbols    : output buffer of WSPR_SYMBOLS bytes, values 0..3.
 *
 * Returns 0 on success, -1 on invalid input.
 */
int wspr_encode(const char *callsign, const char *locator, int power_dbm,
                uint8_t *symbols);

/*
 * Helpers exposed for unit tests and for inspecting intermediate
 * results, which is useful when chasing an over the air mismatch.
 */

/* Pack the callsign into N (28 significant bits). -1 on error. */
int64_t wspr_pack_callsign(const char *callsign);

/* Pack locator and power into M (22 significant bits). -1 on error. */
int64_t wspr_pack_locator_power(const char *locator, int power_dbm);

/*
 * Build the 81 bit source message (N and M concatenated plus 31 tail
 * zeros) into 11 bytes, most significant bit first, as the spec says.
 * Bits 82..88 are padding and unused.
 */
void wspr_pack_message_bits(int64_t n, int64_t m, uint8_t bytes11[11]);

/*
 * Convolutional encoder, K=32 r=1/2. Reads 81 bits from bytes11 (MSB
 * first) and writes 162 parity bits into raw162, before interleaving.
 */
void wspr_convolve(const uint8_t bytes11[11], uint8_t raw162[WSPR_SYMBOLS]);

/*
 * Interleave by bit reversing an 8 bit address (256 space, values >=162
 * discarded).
 */
void wspr_interleave(const uint8_t in162[WSPR_SYMBOLS],
                     uint8_t out162[WSPR_SYMBOLS]);

/*
 * Inverse of the interleave. NOTE: the WSPR interleave is NOT self
 * inverse (verified by test), so the decoder must use this rather than
 * calling wspr_interleave() a second time.
 */
void wspr_deinterleave(const uint8_t in162[WSPR_SYMBOLS],
                       uint8_t out162[WSPR_SYMBOLS]);

/* Same permutation for soft values. */
void wspr_deinterleave_soft(const float in162[WSPR_SYMBOLS],
                            float out162[WSPR_SYMBOLS]);

/* The public 162 bit WSPR synchronisation vector. */
extern const uint8_t wspr_sync_vector[WSPR_SYMBOLS];

#endif /* WSPR_ENCODE_H */
