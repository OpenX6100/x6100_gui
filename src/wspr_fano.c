/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Fano sequential decoder
 *
 *  The algorithm in short: the decoder walks the code tree, choosing
 *  one of two branches (bit 0 or 1) at each node. It keeps a running
 *  threshold T and a path metric, moving forward while the metric stays
 *  above T. When it drops below, the decoder tries to back up and take
 *  the other branch; if that fails too it lowers T by delta and tries
 *  again.
 *
 *  The key difference from Viterbi is that only one path plus the
 *  ability to backtrack is stored, never all states. That is what makes
 *  K=32 tractable.
 *
 *  Branch metric: how well the soft value agrees with the expected
 *  parity bit, minus a constant bias. The bias is what makes the metric
 *  of a correct path grow while a wrong path shrinks; without it the
 *  decoder could not tell a good path from a random one.
 */

#include "wspr_fano.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WSPR_POLY_0 0xF2D05351u
#define WSPR_POLY_1 0xE4613C47u

/*
 * Metric bias, chosen by measurement. Too small and the decoder wanders
 * down wrong branches; too large and it rejects the correct path on a
 * weak signal. Sweeping it showed a sharp optimum: at 0.5 the decoder
 * managed 15 of 60 at the working point, at 0.85 it manages 59 of 60.
 * That single constant was worth about 4 dB of sensitivity.
 */
#ifndef FANO_BIAS
#define FANO_BIAS 0.85f
#endif

/* Threshold step. Too small is slow, too large costs sensitivity. */
#ifndef FANO_DELTA
#define FANO_DELTA 8.0f
#endif

static int parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (int)(v & 1u);
}

/* One path node per tree depth. */
typedef struct {
    uint32_t reg;        /* shift register contents after this bit  */
    float metric;        /* path metric up to and including this node */
    float branch[2];     /* metrics of both branches from the parent  */
    uint8_t bit;         /* chosen bit                                */
    uint8_t tried;       /* branches tried so far (0, 1 or 2)         */
} fano_node_t;

/*
 * Branch metric for hypothesised information bit b at a node holding
 * register reg, given soft values s0 and s1 for the two parity bits.
 */
static float branch_metric(uint32_t reg, int b, float s0, float s1,
                           uint32_t *new_reg)
{
    uint32_t r = (reg << 1) | (uint32_t)b;
    int p0 = parity32(r & WSPR_POLY_0);
    int p1 = parity32(r & WSPR_POLY_1);
    float m;

    *new_reg = r;

    /* Agreement: parity bit 1 matches a positive soft value. */
    m  = (p0 ? s0 : -s0) - FANO_BIAS;
    m += (p1 ? s1 : -s1) - FANO_BIAS;

    return m;
}

int wspr_fano_decode(const float soft162[WSPR_SYMBOLS],
                     uint8_t bits81[WSPR_INFO_BITS],
                     long max_nodes,
                     long *nodes_used)
{
    /*
     * The canonical Fano algorithm. Three moves:
     *   forward  - the best untried branch fits within the threshold
     *   backward - it does not, but the previous node is above threshold
     *   lower T  - neither forward nor backward is possible
     *
     * The threshold is tightened ONLY on the first visit to a node.
     * Without that condition the decoder loops during backtracking,
     * raising and lowering the threshold forever (it still decodes, but
     * takes ~30000 nodes where this takes 82).
     */
    fano_node_t node[WSPR_INFO_BITS + 1];
    uint8_t first_visit[WSPR_INFO_BITS + 1];
    float norm[WSPR_SYMBOLS];
    float threshold = 0.0f;
    int depth = 0;
    long visits = 0;
    int i;
    float scale = 0.0f;

    if (max_nodes <= 0)
        max_nodes = WSPR_FANO_MAX_NODES;

    for (i = 0; i < WSPR_SYMBOLS; i++)
        scale += fabsf(soft162[i]);
    scale /= WSPR_SYMBOLS;
    if (scale < 1e-9f)
        return -1;

    for (i = 0; i < WSPR_SYMBOLS; i++)
        norm[i] = soft162[i] / scale;

    memset(node, 0, sizeof(node));
    memset(first_visit, 1, sizeof(first_visit));
    node[0].reg = 0;
    node[0].metric = 0.0f;

    while (visits < max_nodes) {
        fano_node_t *cur = &node[depth];
        int allowed = (depth >= WSPR_DATA_BITS) ? 1 : 2;
        uint32_t r0 = 0, r1 = 0;
        float m0, m1;
        int best_b, second_b;
        int take = -1;
        float take_m = 0.0f;
        uint32_t take_r = 0;

        visits++;

        if (depth >= WSPR_INFO_BITS)
            break;                     /* end of tree, success */

        m0 = branch_metric(cur->reg, 0, norm[2 * depth], norm[2 * depth + 1],
                           &r0);
        if (allowed == 2)
            m1 = branch_metric(cur->reg, 1, norm[2 * depth],
                               norm[2 * depth + 1], &r1);
        else
            m1 = -1e30f;

        if (m0 >= m1) {
            best_b = 0;
            second_b = 1;
        } else {
            best_b = 1;
            second_b = 0;
        }

        /* Which branch to take given how many have been tried. */
        if (cur->tried == 0) {
            take = best_b;
        } else if (cur->tried == 1 && allowed == 2) {
            take = second_b;
        }

        if (take >= 0) {
            take_m = (take == 0) ? m0 : m1;
            take_r = (take == 0) ? r0 : r1;
        }

        if (take >= 0 && cur->metric + take_m >= threshold) {
            /* --- move forward --- */
            cur->tried++;
            cur->bit = (uint8_t)take;

            node[depth + 1].reg = take_r;
            node[depth + 1].metric = cur->metric + take_m;
            node[depth + 1].tried = 0;

            depth++;

            if (first_visit[depth]) {
                first_visit[depth] = 0;
                /* Tighten only on first arrival at this node. */
                while (node[depth].metric >= threshold + FANO_DELTA)
                    threshold += FANO_DELTA;
            }
            continue;
        }

        /* --- cannot move forward --- */
        if (depth == 0 || node[depth - 1].metric < threshold) {
            /* Cannot go back either: lower the threshold and start
             * trying this node's branches again. */
            threshold -= FANO_DELTA;
            cur->tried = 0;
            continue;
        }

        /* --- move backward --- */
        depth--;
        /* Back to the parent; its tried counter already records which
         * branch was used, so next time it takes the other one. */
    }

    if (nodes_used)
        *nodes_used = visits;

    if (depth < WSPR_INFO_BITS)
        return -1;

    for (i = 0; i < WSPR_INFO_BITS; i++)
        bits81[i] = node[i].bit;

    return 0;
}

int wspr_verify(const float soft162[WSPR_SYMBOLS],
                const uint8_t bits81[WSPR_INFO_BITS])
{
    uint32_t reg = 0;
    int i, bad = 0, out = 0;

    for (i = 0; i < WSPR_INFO_BITS; i++) {
        int p0, p1;

        reg = (reg << 1) | (uint32_t)(bits81[i] & 1);
        p0 = parity32(reg & WSPR_POLY_0);
        p1 = parity32(reg & WSPR_POLY_1);

        /* Hard decision from the soft value: positive means 1. */
        if (p0 != (soft162[out] > 0.0f ? 1 : 0))
            bad++;
        out++;
        if (p1 != (soft162[out] > 0.0f ? 1 : 0))
            bad++;
        out++;
    }

    return bad;
}

/* ------------------------------------------------------------------ */
/* Message unpacking                                                   */

static char value_char(int v)
{
    if (v >= 0 && v <= 9)
        return (char)('0' + v);
    if (v >= 10 && v <= 35)
        return (char)('A' + v - 10);
    return ' ';
}

int wspr_unpack(const uint8_t bits81[WSPR_INFO_BITS],
                char *callsign, char *locator, int *power)
{
    int64_t n = 0, m = 0;
    int i;
    int c[6];
    int64_t x;
    int64_t m1;
    int loc1, loc2, loc3, loc4;
    int pwr;

    for (i = 0; i < 28; i++)
        n = (n << 1) | bits81[i];
    for (i = 0; i < 22; i++)
        m = (m << 1) | bits81[28 + i];

    /* Undo the successive multiplications from the encoder. */
    x = n;
    c[5] = (int)(x % 27) + 10;  x /= 27;
    c[4] = (int)(x % 27) + 10;  x /= 27;
    c[3] = (int)(x % 27) + 10;  x /= 27;
    c[2] = (int)(x % 10);       x /= 10;
    c[1] = (int)(x % 36);       x /= 36;
    c[0] = (int)x;

    if (c[0] < 0 || c[0] > 36)
        return -1;

    for (i = 0; i < 6; i++)
        callsign[i] = value_char(c[i]);
    callsign[6] = '\0';

    /* Strip leading and trailing spaces. */
    {
        char tmp[7];
        int a = 0, b = 5;

        while (a <= b && callsign[a] == ' ')
            a++;
        while (b >= a && callsign[b] == ' ')
            b--;
        if (a > b)
            return -1;
        memcpy(tmp, callsign + a, (size_t)(b - a + 1));
        tmp[b - a + 1] = '\0';
        strcpy(callsign, tmp);
    }

    pwr = (int)((m % 128) - 64);
    m1 = m / 128;

    if (pwr < 0 || pwr > 60)
        return -1;

    loc4 = (int)(m1 % 10);
    loc2 = (int)((m1 / 10) % 18);
    loc3 = (int)((179 - m1 / 180) % 10);
    loc1 = (int)((179 - m1 / 180) / 10);

    if (loc1 < 0 || loc1 > 17 || loc2 < 0 || loc2 > 17)
        return -1;
    if (loc3 < 0 || loc3 > 9 || loc4 < 0 || loc4 > 9)
        return -1;

    locator[0] = (char)('A' + loc1);
    locator[1] = (char)('A' + loc2);
    locator[2] = (char)('0' + loc3);
    locator[3] = (char)('0' + loc4);
    locator[4] = '\0';

    *power = pwr;
    return 0;
}

/* ------------------------------------------------------------------ */

int wspr_decode_message(const float soft162[WSPR_SYMBOLS],
                        char *callsign, char *locator, int *power,
                        int *errors)
{
    uint8_t bits[WSPR_INFO_BITS];
    int bad;

    if (errors)
        *errors = -1;

    if (wspr_fano_decode(soft162, bits, 0, NULL) != 0)
        return -1;

    bad = wspr_verify(soft162, bits);
    if (errors)
        *errors = bad;

    if (bad > WSPR_VERIFY_MAX_ERRORS)
        return -1;             /* false decode, reject it */

    return wspr_unpack(bits, callsign, locator, power);
}
