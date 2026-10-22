/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Fano sequential decoder for the WSPR convolutional code
 *
 *  Written from scratch against the public description of the Fano
 *  algorithm (sequential decoding of convolutional codes, R. M. Fano
 *  1963) and the public G4JNT WSPR specification. The reference fano.c
 *  (KA9Q / K1JT, part of wsprd) is GPL and was NOT copied.
 *
 *  Why Fano rather than Viterbi: with constraint length K=32 Viterbi
 *  would need 2^31 states, which is out of the question. Sequential
 *  decoding explores only promising branches, at the price of variable
 *  run time - very fast on a strong signal, and it may give up once the
 *  step budget runs out on a weak one.
 */

#ifndef WSPR_FANO_H
#define WSPR_FANO_H

#include <stdint.h>

#include "wspr_encode.h"

/* Information bits entering the encoder: 50 data plus 31 zero tail
 * bits that flush the shift register. */
#define WSPR_INFO_BITS 81
#define WSPR_DATA_BITS 50

/* Default work limit in visited nodes. A clean signal needs a few
 * hundred; the limit keeps the decoder from grinding forever on noise. */
#define WSPR_FANO_MAX_NODES 200000

/*
 * Decode 162 soft values into 81 information bits.
 *
 * soft162   : soft values in CODE order (already deinterleaved).
 *             Positive means the bit is probably 1, negative 0, and the
 *             magnitude is the confidence. The absolute scale does not
 *             matter, the metric is normalised internally.
 * bits81    : output, 81 bits. The last 31 should come out zero; if
 *             they do not, the decode failed.
 * max_nodes : work limit, 0 means WSPR_FANO_MAX_NODES.
 * nodes_used: optional (may be NULL), how many nodes were visited,
 *             a useful measure of how hard the signal was.
 *
 * Returns 0 on success, -1 if the decoder gave up.
 *
 * NOTE: reaching the end of the tree does NOT mean the result is right.
 * WSPR carries no checksum and the tail bits are forced, so on a noisy
 * input the decoder will find some path. The result must be checked
 * with wspr_verify(); otherwise false decodes get through, which in
 * WSPR is worse than no decode because they end up in the global
 * spotting database.
 */
int wspr_fano_decode(const float soft162[WSPR_SYMBOLS],
                     uint8_t bits81[WSPR_INFO_BITS],
                     long max_nodes,
                     long *nodes_used);

/*
 * Acceptance threshold for wspr_verify(). Chosen by measurement:
 * correct decodes never exceeded 23 mismatching bits even at -33 dB
 * SNR, while false decodes never fell below 29. A threshold of 26
 * separates the two cleanly across the whole tested range.
 */
#define WSPR_VERIFY_MAX_ERRORS 26

/*
 * Re-encode bits81 and compare against hard decisions taken from
 * soft162. Returns the number of mismatching bits (0..162).
 *
 * For a correct decode the mismatches are just channel errors. For a
 * false decode they run near half of 162, because the path has nothing
 * to do with the received signal.
 */
int wspr_verify(const float soft162[WSPR_SYMBOLS],
                const uint8_t bits81[WSPR_INFO_BITS]);

/*
 * Unpack the 50 data bits back into a message.
 * callsign : buffer of at least 7 characters
 * locator  : buffer of at least 5 characters
 * power    : power in dBm
 * Returns 0 on success, -1 if the bits do not form a valid type 1
 * message.
 */
int wspr_unpack(const uint8_t bits81[WSPR_INFO_BITS],
                char *callsign, char *locator, int *power);

/*
 * Convenience wrapper: decode, verify and unpack in one call. This is
 * the proper entry point for the layer above - wspr_fano_decode() on
 * its own would let false decodes through.
 *
 * callsign : buffer >= 7 characters, locator : buffer >= 5 characters
 * errors   : optional (may be NULL), the mismatch count, useful as a
 *            quality indicator
 *
 * Returns 0 on success, -1 if decoding failed or the result did not
 * pass verification.
 */
int wspr_decode_message(const float soft162[WSPR_SYMBOLS],
                        char *callsign, char *locator, int *power,
                        int *errors);

#endif /* WSPR_FANO_H */
