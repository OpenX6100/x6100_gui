/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  WSPR message encoder
 *
 *  Algorithm source: Andy Talbot G4JNT, "The WSPR Coding Process"
 *  (2009), a public vendor independent specification written for
 *  implementers. Nothing here comes from WSJT-X / wsprd (GPL-3).
 */

#include "wspr_encode.h"

#include <ctype.h>
#include <string.h>

/*
 * The public WSPR sync vector: a 162 bit pseudo random sequence with
 * good autocorrelation. It is a fixed published constant shared by
 * every WSPR implementation - without it no receiver could lock onto
 * any transmitter.
 */
const uint8_t wspr_sync_vector[WSPR_SYMBOLS] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,0,0,
    0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,0,0,1,0,
    1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
    1,1,0,0,0,0,0,1,0,1,0,0,1,1,0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,0,0
};

/* ------------------------------------------------------------------ */
/* Callsign packing                                                    */

static int char_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    if (c == ' ')
        return 36;
    return -1;
}

int64_t wspr_pack_callsign(const char *callsign)
{
    char buf[7];
    size_t len;
    int i;

    if (!callsign)
        return -1;

    len = strlen(callsign);
    if (len == 0 || len > 6)
        return -1;

    for (i = 0; i < (int)len; i++)
        buf[i] = (char)toupper((unsigned char)callsign[i]);
    buf[len] = '\0';

    /*
     * The third character must be a digit. If it is not, pad with a
     * space at the front, exactly as the spec example shows:
     * "G4JNT" (third char 'J') -> "[sp]G4JNT" (third char '4'),
     * while "GD4JNT" (third char already '4') is left alone.
     */
    if (len < 3 || !isdigit((unsigned char)buf[2])) {
        if (len >= 6)
            return -1;             /* would not fit after padding */
        memmove(buf + 1, buf, len + 1);
        buf[0] = ' ';
        len++;
    }

    /* Pad with spaces to 6 characters. */
    while (len < 6)
        buf[len++] = ' ';
    buf[6] = '\0';

    if (!isdigit((unsigned char)buf[2]))
        return -1;                 /* should not happen */

    /*
     * The last three characters can only be a letter or a space
     * (values 10..36); there is no room for a digit in those slots.
     */
    for (i = 3; i < 6; i++) {
        if (isdigit((unsigned char)buf[i]))
            return -1;
    }

    {
        int64_t n1, n2, n3, n4, n5, n6;
        int v0 = char_value(buf[0]);
        int v1 = char_value(buf[1]);
        int v2 = char_value(buf[2]);
        int v3 = char_value(buf[3]);
        int v4 = char_value(buf[4]);
        int v5 = char_value(buf[5]);

        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0 || v4 < 0 || v5 < 0)
            return -1;
        if (v1 == 36)               /* second character cannot be a space */
            return -1;

        n1 = v0;
        n2 = n1 * 36 + v1;
        n3 = n2 * 10 + v2;
        n4 = 27 * n3 + (v3 - 10);
        n5 = 27 * n4 + (v4 - 10);
        n6 = 27 * n5 + (v5 - 10);

        return n6;
    }
}

/* ------------------------------------------------------------------ */
/* Locator and power packing                                           */

int64_t wspr_pack_locator_power(const char *locator, int power_dbm)
{
    int loc1, loc2, loc3, loc4;
    int64_t m1, m;
    char l0, l1, l2, l3;

    if (!locator || strlen(locator) != 4)
        return -1;
    if (power_dbm < 0 || power_dbm > 60)
        return -1;

    l0 = (char)toupper((unsigned char)locator[0]);
    l1 = (char)toupper((unsigned char)locator[1]);
    l2 = locator[2];
    l3 = locator[3];

    if (l0 < 'A' || l0 > 'R' || l1 < 'A' || l1 > 'R')
        return -1;
    if (!isdigit((unsigned char)l2) || !isdigit((unsigned char)l3))
        return -1;

    loc1 = l0 - 'A';
    loc2 = l1 - 'A';
    loc3 = l2 - '0';
    loc4 = l3 - '0';

    m1 = (int64_t)(179 - 10 * loc1 - loc3) * 180 + 10 * loc2 + loc4;
    m = m1 * 128 + power_dbm + 64;

    return m;
}

/* ------------------------------------------------------------------ */
/* Building the 81 bit source message                                  */

static void set_bit(uint8_t *bytes, int pos, int val)
{
    int byte_idx = pos / 8;
    int bit_idx = 7 - (pos % 8);       /* MSB first within the byte */

    if (val)
        bytes[byte_idx] |= (uint8_t)(1u << bit_idx);
    else
        bytes[byte_idx] &= (uint8_t)~(1u << bit_idx);
}

static int get_bit(const uint8_t *bytes, int pos)
{
    int byte_idx = pos / 8;
    int bit_idx = 7 - (pos % 8);

    return (bytes[byte_idx] >> bit_idx) & 1;
}

void wspr_pack_message_bits(int64_t n, int64_t m, uint8_t bytes11[11])
{
    int i;

    memset(bytes11, 0, 11);

    /* N occupies bits 0..27, MSB first. */
    for (i = 0; i < 28; i++)
        set_bit(bytes11, i, (int)((n >> (27 - i)) & 1));

    /* M occupies bits 28..49, MSB first. */
    for (i = 0; i < 22; i++)
        set_bit(bytes11, 28 + i, (int)((m >> (21 - i)) & 1));

    /* Bits 50..80 are the 31 tail zeros, already cleared by memset.
     * Bits 81..87 are padding to 11 bytes and unused. */
}

/* ------------------------------------------------------------------ */
/* Convolutional encoder, K=32 r=1/2                                   */

#define WSPR_POLY_0 0xF2D05351u
#define WSPR_POLY_1 0xE4613C47u

static int parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (int)(v & 1u);
}

void wspr_convolve(const uint8_t bytes11[11], uint8_t raw162[WSPR_SYMBOLS])
{
    uint32_t reg0 = 0, reg1 = 0;
    int i, out = 0;

    for (i = 0; i < 81; i++) {
        int bit = get_bit(bytes11, i);

        reg0 = (reg0 << 1) | (uint32_t)bit;
        reg1 = (reg1 << 1) | (uint32_t)bit;

        raw162[out++] = (uint8_t)parity32(reg0 & WSPR_POLY_0);
        raw162[out++] = (uint8_t)parity32(reg1 & WSPR_POLY_1);
    }
}

/* ------------------------------------------------------------------ */
/* Interleave by 8 bit address reversal                                */

static uint8_t bit_reverse8(uint8_t v)
{
    v = (uint8_t)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
    v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

void wspr_interleave(const uint8_t in162[WSPR_SYMBOLS],
                     uint8_t out162[WSPR_SYMBOLS])
{
    int i, p = 0;

    for (i = 0; i < 256 && p < WSPR_SYMBOLS; i++) {
        uint8_t j = bit_reverse8((uint8_t)i);

        if (j < WSPR_SYMBOLS) {
            out162[j] = in162[p];
            p++;
        }
    }
}

void wspr_deinterleave(const uint8_t in162[WSPR_SYMBOLS],
                       uint8_t out162[WSPR_SYMBOLS])
{
    int i, p = 0;

    for (i = 0; i < 256 && p < WSPR_SYMBOLS; i++) {
        uint8_t j = bit_reverse8((uint8_t)i);

        if (j < WSPR_SYMBOLS) {
            out162[p] = in162[j];      /* reversed compared to interleave */
            p++;
        }
    }
}

void wspr_deinterleave_soft(const float in162[WSPR_SYMBOLS],
                            float out162[WSPR_SYMBOLS])
{
    int i, p = 0;

    for (i = 0; i < 256 && p < WSPR_SYMBOLS; i++) {
        uint8_t j = bit_reverse8((uint8_t)i);

        if (j < WSPR_SYMBOLS) {
            out162[p] = in162[j];
            p++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */

int wspr_encode(const char *callsign, const char *locator, int power_dbm,
                uint8_t *symbols)
{
    int64_t n, m;
    uint8_t bytes11[11];
    uint8_t raw162[WSPR_SYMBOLS];
    uint8_t data162[WSPR_SYMBOLS];
    int i;

    n = wspr_pack_callsign(callsign);
    if (n < 0)
        return -1;

    m = wspr_pack_locator_power(locator, power_dbm);
    if (m < 0)
        return -1;

    wspr_pack_message_bits(n, m, bytes11);
    wspr_convolve(bytes11, raw162);
    wspr_interleave(raw162, data162);

    for (i = 0; i < WSPR_SYMBOLS; i++)
        symbols[i] = (uint8_t)(wspr_sync_vector[i] + 2 * data162[i]);

    return 0;
}
