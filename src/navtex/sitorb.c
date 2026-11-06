/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * SITOR-B (FEC mode B) receiver for NAVTEX - see sitorb.h.
 *
 * Written from the ITU recommendations, not from other decoders:
 *   ITU-R M.476 / M.625  7-unit code, FEC mode B (DX/RX time diversity)
 *   ITU-R M.540          NAVTEX
 *
 * The chain, all in this file:
 *
 *   audio -> mix to 0 Hz at the centre frequency (with AFC) -> low pass,
 *   decimate to 2000 Hz -> two tone correlators, one bit long (B at +85 Hz,
 *   Y at -85 Hz) -> soft bit value d in -1..+1 -> bit clock (where the
 *   transitions are) -> one soft bit every 10 ms
 *
 *   soft bits -> 7-bit characters at the 7 possible bit phases -> phasing
 *   signals or DX/RX repetitions say where the characters start and which
 *   ones are DX -> each DX/RX pair gives one character -> letters/figures
 */
#include "sitorb.h"
#include "decoder.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- signal ------------------------------------------------------------------ */

#define FS          2000            /* internal sample rate */
#define SPB         20              /* samples per bit (100 Bd) */
#define TONE_HZ     85.0f           /* half of the 170 Hz shift */
#define AFC_MAX_HZ  80.0f
#ifndef LPF_CUT_HZ
#define LPF_CUT_HZ  260.0f
#endif
#ifndef AFC_GAIN
#define AFC_GAIN    0.003f
#endif
#ifndef CLK_LEVEL
#define CLK_LEVEL   0.65f           /* below it the bit clock holds */
#endif
#ifndef CLK_DECAY
#define CLK_DECAY   0.995f
#endif

/* ---- the 7-unit code (ITU-R M.625, tables 1 and 2) ------------------------------
 * In order of transmission, B = 1 (the higher tone), Y = 0. All 35 patterns
 * with four B and three Y are used. */

enum {
    C_CR = 26, C_LF, C_LTRS, C_FIGS, C_SPACE, C_NUL,
    C_BETA, C_ALPHA, C_RQ,
    N_CODES
};

static const char *const code_bits[N_CODES] = {
    "BBBYYYB", "YBYYBBB", "BYBBBYY", "BBYYBYB", "YBBYBYB", "BBYBBYY", /* A-F */
    "BYBYBBY", "BYYBYBB", "BYBBYYB", "BBBYBYY", "YBBBBYY", "BYBYYBB", /* G-L */
    "BYYBBBY", "BYYBBYB", "BYYYBBB", "BYBBYBY", "YBBBYBY", "BYBYBYB", /* M-R */
    "BBYBYYB", "YYBYBBB", "YBBBYYB", "YYBBBBY", "BBBYYBY", "YBYBBBY", /* S-X */
    "BBYBYBY", "BBYYYBB",                                             /* Y-Z */
    "YYYBBBB", /* CR */          "YYBBYBB", /* LF */
    "YBYBBYB", /* letters */     "YBBYBBY", /* figures */
    "YYBBBYB", /* space */       "YBYBYBB", /* no information */
    "BBYYBBY", /* idle beta */
    "BBBBYYY", /* idle alpha = phasing signal 1 (RX position) */
    "YBBYYBB", /* RQ = phasing signal 2 (DX position) */
};

/* figures case of A..Z; 0 = nothing printed */
static const char figures[26] = {
    '-', '?', ':', '$', '3', '!', '&', '#', '8', 0,   '(', ')', '.',
    ',', '9', '0', '1', '4', '\'', '5', '7', '=', '2', '/', '6', '+',
};

#define IS_SERVICE(c) ((c) >= C_BETA)

/* ---- sizes --------------------------------------------------------------------- */

#define BIT_RING    1024            /* soft bits kept (10 s) */
#define SLOT_HIST   32              /* characters kept per bit phase */
#define HUNT_SLOTS  20              /* window of the FEC search */
#define HUNT_PAIRS  4               /* DX/RX repetitions needed to lock */
#define OUT_DELAY   150             /* bits a character waits before it is
                                       printed: a slip found in this time is
                                       corrected without printing garbage */
#define PENDING     64

typedef struct {
    int64_t end;                    /* last bit of the RX copy */
    int8_t code, q, agreed;         /* agreed: DX and RX were the same */
} pending_t;

typedef struct { float re, im; } cpx;

struct sitorb {
    sitorb_cb_t cb;
    void *user;
    int reverse;

    /* front end */
    float centre_hz;
    float in_rate;
    int decim;
    int taps;
    float *h;                       /* low pass, taps */
    cpx *fir;                       /* mixed input, 2 * taps (mirrored ring) */
    int fir_pos;
    int decim_phase;
    double nco_phase;
    float afc_hz;

    /* tone correlators at FS */
    cpx rot_b, rot_y;               /* per-sample rotation of the tone references */
    cpx ph_b, ph_y;
    cpx corr_b[SPB], corr_y[SPB];
    cpx sum_b, sum_y;
    float fq[SPB];                  /* instantaneous frequency, one bit */
    float fq_sum;
    cpx prev_z;
    int k;                          /* index in the one-bit windows */
    float prev_d;

    /* bit clock */
    float trans[SPB];               /* transition energy at each phase */
    int phase;                      /* FS sample index modulo SPB */
    int countdown;                  /* FS samples to the next bit */
    float level;                    /* mean |d| at the sampling instants */
    unsigned clk_bits;
    int clk_far;                    /* bits with the clock far from the target */
    pending_t pend[PENDING];        /* decided characters not printed yet */
    int npend;
    int64_t last_flushed;           /* end of the last printed character */

    /* soft bits */
    float bits[BIT_RING];
    int64_t nbits;

    /* characters at the 7 bit phases (hard decisions, -1 = not a code) */
    int8_t hist[7][SLOT_HIST];

    /* lock */
    int locked;
    int lock_phase;                 /* bit phase of the characters, 0..6 */
    int lock_dx;                    /* parity of the DX slots, 0..1 */
    int64_t next_rx_slot;           /* next RX slot to decode */
    int64_t last_out_bit;           /* last bit of the last RX slot decoded */
    float bad;                      /* running share of lost characters */
    int alpha_dx;                   /* consecutive idle alpha in DX */
    int fresh;                      /* just locked: no character decided yet */
    int lock_score;                 /* score() of the locked phase */
    float margin_min;               /* below it a guess is a lost character */
    /* soft evidence of each alignment (bit phase, DX parity): running mean
     * of how much the RX copy looks like the DX copy, and of how much the
     * pair looks like phasing signals */
    float ev_rep[7][2], ev_ph[7][2];
    int ev_n[7][2];
    float energy;                   /* mean tone energy at the sampling instants */
    float pk_h, pk_l, floor_;       /* peak of each tone, floor (fades) */
    int figs;
    int doubt;                      /* a lost or guessed character since the last shift */
};

/* code lookup: 7-bit word (bit 0 = first transmitted, 1 = B) -> code or -1 */
static int8_t code_of[128];
static uint8_t word_of[N_CODES];
static uint8_t alpha_rot[N_CODES];  /* idle alpha read at another bit */
static int tables_ready;

static void make_tables(void)
{
    if (tables_ready)
        return;
    memset(code_of, -1, sizeof(code_of));
    for (int c = 0; c < N_CODES; c++) {
        int w = 0;
        for (int i = 0; i < 7; i++)
            if (code_bits[c][i] == 'B')
                w |= 1 << i;
        word_of[c] = (uint8_t)w;
        code_of[w] = (int8_t)c;
    }
    for (int r = 0; r < 7; r++) {
        int w = word_of[C_ALPHA];
        w = ((w >> r) | (w << (7 - r))) & 0x7f;
        alpha_rot[code_of[w]] = 1;
    }
    tables_ready = 1;
}

static inline cpx cmul(cpx a, cpx b)
{
    cpx r = { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re };
    return r;
}

static inline cpx cpolar(double ang)
{
    cpx r = { (float)cos(ang), (float)sin(ang) };
    return r;
}

/* ---- output -------------------------------------------------------------------- */

static void emit(sitorb_t *s, int c, int q)
{
    if (s->cb)
        s->cb(c, q, s->user);
}

static void flush(sitorb_t *s, int all);

static void set_sync(sitorb_t *s, int on)
{
    if (s->locked == on)
        return;
    if (!on)
        flush(s, 1);
    s->locked = on;
    if (!on) {
        /* start the search from scratch: the evidence of the transmission
         * that has just ended must not lock on its idle signals */
        memset(s->ev_rep, 0, sizeof(s->ev_rep));
        memset(s->ev_ph, 0, sizeof(s->ev_ph));
        memset(s->ev_n, 0, sizeof(s->ev_n));
    }
    emit(s, on ? NAVTEX_EV_SYNC_ON : NAVTEX_EV_SYNC_OFF, NAVTEX_Q_EVENT);
}

/* a decided character */
static void put_code(sitorb_t *s, int c, int q)
{
    if (q == NAVTEX_Q_LOST) {
        s->doubt = 1;               /* it could have been a shift */
        emit(s, '*', NAVTEX_Q_LOST);
        return;
    }
    if (q == NAVTEX_Q_GUESS)
        s->doubt = 1;
    if (c < 26) {
        /* figures that NAVTEX text almost never has, after a doubtful
         * character: the letters shift was lost, back to letters */
        if (s->figs && s->doubt && strchr("$!&#'=+?", figures[c] ? figures[c] : '#'))
            s->figs = 0;
        int ch = s->figs ? figures[c] : 'A' + c;
        if (ch)
            emit(s, ch, q);
        return;
    }
    if ((c == C_LTRS || c == C_FIGS) && q != NAVTEX_Q_GUESS)
        s->doubt = 0;
    switch (c) {
    case C_CR:    emit(s, '\r', q); break;
    case C_LF:    emit(s, '\n', q); break;
    case C_SPACE: emit(s, ' ', q); break;
    case C_LTRS:  s->figs = 0; break;
    case C_FIGS:  s->figs = 1; break;
    default:      break;            /* no information, service signals */
    }
}

/* print the decided characters older than OUT_DELAY bits (all if all) */
static void flush(sitorb_t *s, int all)
{
    int k = 0;
    while (k < s->npend && (all || s->pend[k].end < s->nbits - OUT_DELAY)) {
        put_code(s, s->pend[k].code, s->pend[k].q);
        s->last_flushed = s->pend[k].end;
        k++;
    }
    if (k) {
        memmove(s->pend, s->pend + k, sizeof(pending_t) * (size_t)(s->npend - k));
        s->npend -= k;
    }
}

/* a decided character, printed OUT_DELAY bits later */
static void queue_code(sitorb_t *s, int64_t end, int c, int q, int agreed)
{
    if (s->npend == PENDING)
        flush(s, 1);
    pending_t *p = &s->pend[s->npend++];
    p->end = end;
    p->code = (int8_t)c;
    p->q = (int8_t)q;
    p->agreed = (int8_t)agreed;
}

/* ---- characters ---------------------------------------------------------------- */

static inline float bit_at(const sitorb_t *s, int64_t b)
{
    return s->bits[b & (BIT_RING - 1)];
}

/* 7 soft bits of the character that starts at bit b0 -> hard code or -1 */
static int hard_code(const sitorb_t *s, int64_t b0)
{
    int w = 0;
    for (int i = 0; i < 7; i++)
        if (bit_at(s, b0 + i) > 0)
            w |= 1 << i;
    return code_of[w];
}

/* the code closest to the soft bits (sum of one or two copies);
 * *margin = distance to the second best */
static int soft_code(const float *v, float *margin)
{
    float best = -1e30f, second = -1e30f;
    int bc = -1;
    for (int c = 0; c < N_CODES; c++) {
        float m = 0;
        for (int i = 0; i < 7; i++)
            m += (word_of[c] >> i & 1) ? v[i] : -v[i];
        if (m > best) {
            second = best;
            best = m;
            bc = c;
        } else if (m > second) {
            second = m;
        }
    }
    *margin = best - second;
    return bc;
}

/* first bit of slot n at the locked phase */
static inline int64_t slot_bit(const sitorb_t *s, int64_t n)
{
    return (int64_t)s->lock_phase + 7 * n;
}

/* one character from the DX slot n - 5 and the RX slot n */
static void decode_pair(sitorb_t *s, int64_t n)
{
    int64_t bd = slot_bit(s, n - 5), br = slot_bit(s, n);
    int hd = hard_code(s, bd), hr = hard_code(s, br);

    int dv = hd >= 0 && !IS_SERVICE(hd);   /* DX is a character */
    int rv = hr >= 0 && !IS_SERVICE(hr);   /* RX is a character */

    /* phasing and idle signals in DX: nothing to print */
    if (hd == C_RQ || hd == C_BETA)
        return;
    if (hd == C_ALPHA) {
        /* end of the transmission: idle alpha in two DX positions (not in
         * the old pairs decoded at the lock, which can be from before) */
        if (!s->fresh && ++s->alpha_dx >= 2) {
            set_sync(s, 0);
            return;
        }
        if (!rv)
            return;
    } else if (hd >= 0) {
        s->alpha_dx = 0;
    }

    /* the code closest to the sum of the two copies (the RX only when it is
     * not a phasing signal): with soft bits this is the best decision, and
     * a copy received during a fade weighs less than the other one */
    int two = !(hr >= 0 && IS_SERVICE(hr));
    float v[7], margin;
    for (int i = 0; i < 7; i++)
        v[i] = bit_at(s, bd + i) + (two ? bit_at(s, br + i) : 0);
    int c = soft_code(v, &margin), q;
    if (dv && hr == hd)
        q = NAVTEX_Q_CLEAN;         /* received twice */
    else if (c == hd)
        q = NAVTEX_Q_CLEAN;         /* the DX copy */
    else if (c == hr)
        q = NAVTEX_Q_REP;           /* the RX copy */
    else
        q = margin > s->margin_min ? NAVTEX_Q_GUESS : NAVTEX_Q_LOST;
    if (IS_SERVICE(c)) {
        /* a phasing or idle signal: nothing to print when it is clear or
         * the RX copy is one too; otherwise it is a character lost in a
         * fade, and a '*' keeps its place in the text */
        if (!two || margin > 2 * s->margin_min)
            return;
        q = NAVTEX_Q_LOST;
    }

    if (!s->fresh)
        s->bad += ((q == NAVTEX_Q_LOST ? 1.0f : 0.0f) - s->bad) * 0.08f;
    /* the oldest pairs, decoded at the lock, can be from before the signal:
     * nothing is printed until the first character received twice */
    if (s->fresh) {
        if (!(dv && hr == hd))
            return;                 /* starts with two copies that agree */
        s->fresh = 0;
    }
    queue_code(s, br + 6, c, q, dv && hr == hd);
}

/* soft evidence of the alignment (p, dx) */
static float evidence(const sitorb_t *s, int p, int dx)
{
    float a = s->ev_rep[p][dx], b = s->ev_ph[p][dx];
    return a > b ? a : b;
}

/* the pair DX slot n - 5, RX slot n at bit phase p, for the soft evidence */
static void update_evidence(sitorb_t *s, int p, int64_t n)
{
    if (n < 5)
        return;
    int dx = !(n & 1);
    int hd = s->hist[p][(n - 5) & (SLOT_HIST - 1)];
    int hr = s->hist[p][n & (SLOT_HIST - 1)];
    if (hd >= 0 && hd == hr && alpha_rot[hd])
        return;                     /* idle alpha looks the same at every bit */
    int64_t bd = (int64_t)p + 7 * (n - 5), br = (int64_t)p + 7 * n;
    float xy = 0, xx = 0, yy = 0, ph = 0, ax = 0;
    for (int i = 0; i < 7; i++) {
        float x = bit_at(s, bd + i), y = bit_at(s, br + i);
        xy += x * y;
        xx += x * x;
        yy += y * y;
        ph += ((word_of[C_RQ] >> i & 1) ? x : -x) + ((word_of[C_ALPHA] >> i & 1) ? y : -y);
        ax += fabsf(x) + fabsf(y);
    }
    float rep = xy / (sqrtf(xx * yy) + 1e-20f);
    float phs = ph / (ax + 1e-20f);
    s->ev_rep[p][dx] += (rep - s->ev_rep[p][dx]) * (1.0f / 12);
    s->ev_ph[p][dx] += (phs - s->ev_ph[p][dx]) * (1.0f / 12);
    s->ev_n[p][dx]++;
}

/* the alignment with the most soft evidence, if it stands out:
 * returns its evidence, -1 if none */
static float best_alignment(const sitorb_t *s, int *bp, int *bdx)
{
    float best = -1, second = -1;
    for (int p = 0; p < 7; p++)
        for (int dx = 0; dx < 2; dx++) {
            if (s->ev_n[p][dx] < 16)
                continue;
            float e = evidence(s, p, dx);
            if (e > best) {
                second = best;
                best = e;
                *bp = p;
                *bdx = dx;
            } else if (e > second) {
                second = e;
            }
        }
    return best > 1.2f * second + 0.02f ? best : -1;
}

/* decode every RX slot complete up to the last received bit */
static void decode_locked(sitorb_t *s)
{
    for (;;) {
        int64_t n = s->next_rx_slot;
        int64_t end = slot_bit(s, n) + 6;
        if (end >= s->nbits)
            break;
        s->next_rx_slot += 2;
        if (end <= s->last_out_bit + 7)
            continue;               /* already decoded before a relock */
        s->last_out_bit = end;
        decode_pair(s, n);
        if (!s->locked)
            return;
        /* the signal is gone: nearly every character lost, or the copies
         * do not look alike any more */
        if (s->bad > 0.85f || (s->ev_n[s->lock_phase][s->lock_dx] >= 16 &&
                                evidence(s, s->lock_phase, s->lock_dx) < 0.12f)) {
            set_sync(s, 0);
            return;
        }
    }
}

/* lock on the characters at bit phase p, DX slots of parity dx; the slot
 * just completed is n. Decoding starts with the oldest pair still in memory. */
static void lock_on(sitorb_t *s, int p, int dx, int64_t n)
{
    s->lock_phase = p;
    s->lock_dx = dx;
    int64_t first = n - (HUNT_SLOTS - 1);
    if (((first & 1) == dx))
        first++;                    /* an RX slot */
    while (first - 5 < 0)
        first += 2;
    s->next_rx_slot = first;
    s->alpha_dx = 0;
    s->lock_score = HUNT_PAIRS;
    if (s->locked) {
        /* a slip: the characters decided since the last one received twice
         * are probably wrong; they are decided again at the new alignment */
        int k = s->npend;
        while (k > 0 && !s->pend[k - 1].agreed)
            k--;
        if (k < s->npend) {
            s->last_out_bit = k ? s->pend[k - 1].end : s->last_flushed;
            s->npend = k;
        }
        return;
    }
    s->bad = 0;
    s->figs = 0;
    s->fresh = 1;
    set_sync(s, 1);
}

/* evidence that the characters at bit phase p start there, with DX slots
 * of parity dx, in the last HUNT_SLOTS slots up to n: DX/RX repetitions
 * and phasing pairs (RQ in DX, alpha in RX). *distinct = different
 * characters among the repetitions. */
static int score(const sitorb_t *s, int p, int dx, int64_t n, int *distinct, int *phasing)
{
    const int8_t *h = s->hist[p];
#define H(i) h[(i) & (SLOT_HIST - 1)]
    int pairs = 0, ph = 0;
    uint64_t seen = 0;
    for (int64_t i = n - HUNT_SLOTS + 1; i + 5 <= n; i++) {
        if ((i & 1) != dx)
            continue;
        int a = H(i);
        if (a >= 0 && a < C_BETA && a == H(i + 5)) {
            pairs++;
            seen |= 1ull << a;
        } else if (a == C_RQ && H(i + 5) == C_ALPHA) {
            ph++;
        }
    }
#undef H
    *distinct = __builtin_popcountll(seen);
    *phasing = ph;
    return pairs + ph;
}

/* four phasing signals in a row, in the right positions */
static int phasing_run(const sitorb_t *s, int p, int dx, int64_t n)
{
    for (int64_t i = n - 3; i <= n; i++) {
        int want = ((i & 1) == dx) ? C_RQ : C_ALPHA;
        if (s->hist[p][i & (SLOT_HIST - 1)] != want)
            return 0;
    }
    return 1;
}

/* search for the character phase */
static void hunt(sitorb_t *s, int p, int64_t n)
{
    if (n < HUNT_SLOTS)
        return;
    for (int dx = 0; dx < 2; dx++) {
        int distinct, phasing;
        int sc = score(s, p, dx, n, &distinct, &phasing);
        /* at least three different characters: a repeated pattern (idle
         * alpha read at the wrong bit, for example) is all the same */
        if (phasing_run(s, p, dx, n) ||
            (sc >= HUNT_PAIRS && (distinct >= 3 || phasing >= 2))) {
            lock_on(s, p, dx, n);
            return;
        }
    }
}

/* while locked: another phase with much more evidence means that the bit
 * clock has slipped, or that the lock was wrong */
static void check_slip(sitorb_t *s, int p, int64_t n)
{
    if (n < HUNT_SLOTS)
        return;
    for (int dx = 0; dx < 2; dx++) {
        int distinct, phasing;
        int sc = score(s, p, dx, n, &distinct, &phasing);
        if (sc >= HUNT_PAIRS + 1 && sc >= s->lock_score + 3 && (distinct >= 3 || phasing >= 2)) {
            lock_on(s, p, dx, n);
            return;
        }
    }
}

/* a new soft bit */
static void new_bit(sitorb_t *s, float d)
{
    s->bits[s->nbits & (BIT_RING - 1)] = d;
    s->nbits++;

    /* the character that ends with this bit */
    int64_t b0 = s->nbits - 7;
    if (b0 < 0)
        return;
    int p = (int)(b0 % 7);
    int64_t n = b0 / 7;
    s->hist[p][n & (SLOT_HIST - 1)] = (int8_t)hard_code(s, b0);
    update_evidence(s, p, n);

    int bp = 0, bdx = 0;
    float be = best_alignment(s, &bp, &bdx);
    if (s->locked) {
        if (p == s->lock_phase) {
            int distinct, phasing;
            s->lock_score = score(s, p, s->lock_dx, n, &distinct, &phasing);
        } else {
            check_slip(s, p, n);
        }
        /* soft: another alignment clearly better than the locked one */
        if (s->locked && be > 0.3f && (bp != s->lock_phase || bdx != s->lock_dx) &&
            be > 1.25f * evidence(s, s->lock_phase, s->lock_dx) + 0.05f && p == bp) {
            lock_on(s, bp, bdx, n);
        }
        decode_locked(s);
        flush(s, 0);
    } else {
        hunt(s, p, n);
        /* soft: a weak signal that the hard search does not catch */
        if (!s->locked && be > 0.45f && p == bp)
            lock_on(s, bp, bdx, n);
    }
}

/* ---- demodulator ----------------------------------------------------------------- */

/* one sample at FS (complex, centred on the centre frequency) */
static void demod(sitorb_t *s, cpx z)
{
    /* the two tones, correlated over one bit */
    cpx b = cmul(z, s->ph_b), y = cmul(z, s->ph_y);
    s->ph_b = cmul(s->ph_b, s->rot_b);
    s->ph_y = cmul(s->ph_y, s->rot_y);
    int k = s->k;
    s->sum_b.re += b.re - s->corr_b[k].re;
    s->sum_b.im += b.im - s->corr_b[k].im;
    s->sum_y.re += y.re - s->corr_y[k].re;
    s->sum_y.im += y.im - s->corr_y[k].im;
    s->corr_b[k] = b;
    s->corr_y[k] = y;

    /* instantaneous frequency, for the AFC */
    cpx dz = cmul(z, (cpx){ s->prev_z.re, -s->prev_z.im });
    s->prev_z = z;
    float f = atan2f(dz.im, dz.re) * (FS / (2.0f * (float)M_PI));
    s->fq_sum += f - s->fq[k];
    s->fq[k] = f;
    s->k = (k + 1) % SPB;

    float eb = s->sum_b.re * s->sum_b.re + s->sum_b.im * s->sum_b.im;
    float ey = s->sum_y.re * s->sum_y.re + s->sum_y.im * s->sum_y.im;
    float d_raw = (eb - ey) / (eb + ey + 1e-20f);   /* > 0: the higher tone */
    float d = s->reverse ? -d_raw : d_raw;           /* > 0: B */

    /* per-tone level tracking, for fades: the peak of each tone rises
     * quickly and falls slowly (it remembers the tone through a short
     * fade), the floor of the two falls quickly and rises slowly (it
     * follows the noise). Each tone is judged between its floor and its
     * own peak, so a tone that fades alone does not decide the bit. */
    float ah = sqrtf(eb), al = sqrtf(ey);
    s->pk_h += (ah - s->pk_h) * (ah > s->pk_h ? 0.2f : 1.0f / 320);
    s->pk_l += (al - s->pk_l) * (al > s->pk_l ? 0.2f : 1.0f / 320);
    float lo = ah < al ? ah : al;
    s->floor_ += (lo - s->floor_) * (lo < s->floor_ ? 0.2f : 1.0f / 960);

    /* bit clock: d changes the least where the one-bit correlators cover
     * exactly one bit, which is the best sampling instant */
    int ph = s->phase;
    /* in a fade or in noise the transitions count less */
    float cw = s->locked ? (s->level - 0.5f) * 5 : 1;
    if (cw > 1) cw = 1;
    if (cw < 0.05f) cw = 0.05f;
    s->trans[ph] = s->trans[ph] * (1 - (1 - (s->locked ? CLK_DECAY : 0.97f)) * cw) + cw * fabsf(d - s->prev_d);
    s->prev_d = d;
    s->phase = (ph + 1) % SPB;

    if (--s->countdown > 0)
        return;

    /* sample this bit, and move the next sample one step towards the
     * phase where d is steadiest */
    int target = 0;
    float bv = 1e30f;
    for (int i = 0; i < SPB; i++) {
        float v = 0;
        for (int j = -2; j <= 2; j++)
            v += s->trans[(i + j + SPB) % SPB];
        if (v < bv) {
            bv = v;
            target = i;
        }
    }
    int now = ph;                            /* phase of this sample */
    int diff = (target - now + SPB + SPB / 2) % SPB - SPB / 2;
    /* the bit rate is exact (30 ppm): when locked the clock moves at most
     * one sample every 8 bits, every 64 bits in a fade, so that a fade
     * cannot drag it away */
    int step = diff > 0 ? 1 : diff < 0 ? -1 : 0;
    ++s->clk_bits;
    if (s->locked && ((s->clk_bits & 7) || (s->level <= CLK_LEVEL && (s->clk_bits & 63))))
        step = 0;
    /* but a jump of the signal timing (audio lost in the recording or in
     * the radio) is followed at once, when it lasts */
    s->clk_far = (diff >= 5 || diff <= -5) && s->level > CLK_LEVEL ? s->clk_far + 1 : 0;
    if (s->clk_far >= 24) {
        step = diff;
        s->clk_far = 0;
    }
    s->countdown = SPB + step;

    s->level += (fabsf(d) - s->level) * 0.02f;
    float e = eb + ey;
    int strong = e > 0.3f * s->energy;
    s->energy += (e - s->energy) * 0.02f;

    /* AFC: the mean frequency over this bit against the tone that is
     * there, when the bit is clear and the signal is good */
    if (strong && fabsf(d) > 0.7f && (s->locked || s->level > 0.75f)) {
        float err = s->fq_sum / SPB - (d_raw > 0 ? TONE_HZ : -TONE_HZ);
        s->afc_hz += err * AFC_GAIN;
        if (s->afc_hz > AFC_MAX_HZ) s->afc_hz = AFC_MAX_HZ;
        if (s->afc_hz < -AFC_MAX_HZ) s->afc_hz = -AFC_MAX_HZ;
    }

    /* the soft bit: d, weighed down when the tones are weaker than usual
     * (a fade), but never weighed up (a burst of noise must not outweigh
     * the other copy of the character) */
    float w = s->energy > 0 ? e / s->energy : 1;
    float soft = d * (w < 1 ? w : 1);
    {
        float f = s->floor_;
        float ph = s->pk_h - f, pl = s->pk_l - f;
        if (ph > 0 && pl > 0) {
            float ch = fminf(fmaxf(ah - f, 0), ph), cl = fminf(fmaxf(al - f, 0), pl);
            /* which tone is closer to being on, each against its own peak:
             * the decision threshold moves with the two peaks */
            float v = (ch * ph - cl * pl - 0.5f * (ph * ph - pl * pl)) / (0.5f * (ph * ph + pl * pl));
            soft = s->reverse ? -v : v;
        }
    }
    new_bit(s, soft);
}

void sitorb_process(sitorb_t *s, const float *x, unsigned int n)
{
    if (!s || !x)
        return;
    double w = 2.0 * M_PI * (s->centre_hz + s->afc_hz) / s->in_rate;
    for (unsigned int i = 0; i < n; i++) {
        /* mix down */
        cpx m = { x[i] * (float)cos(s->nco_phase), -x[i] * (float)sin(s->nco_phase) };
        s->nco_phase += w;
        if (s->nco_phase > 2.0 * M_PI)
            s->nco_phase -= 2.0 * M_PI;

        int pos = s->fir_pos;
        s->fir[pos] = m;
        s->fir[pos + s->taps] = m;
        s->fir_pos = (pos + 1) % s->taps;

        if (++s->decim_phase < s->decim)
            continue;
        s->decim_phase = 0;

        /* low pass and decimate */
        const cpx *v = &s->fir[s->fir_pos];
        cpx z = { 0, 0 };
        for (int t = 0; t < s->taps; t++) {
            z.re += v[t].re * s->h[t];
            z.im += v[t].im * s->h[t];
        }
        demod(s, z);
        w = 2.0 * M_PI * (s->centre_hz + s->afc_hz) / s->in_rate;
    }
}

/* ---- life -------------------------------------------------------------------------- */

sitorb_t *sitorb_create(float sample_rate, float centre_hz, int reverse)
{
    int decim = (int)lrintf(sample_rate / FS);
    if (decim < 1 || fabsf(sample_rate - (float)decim * FS) > 0.5f)
        return NULL;                /* the rate must be a multiple of 2000 Hz */

    make_tables();
    sitorb_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->reverse = reverse != 0;
    s->centre_hz = centre_hz;
    s->in_rate = sample_rate;
    s->decim = decim;
    s->taps = 16 * decim + 1;
    s->h = malloc(sizeof(float) * (size_t)s->taps);
    s->fir = calloc((size_t)(2 * s->taps), sizeof(cpx));
    if (!s->h || !s->fir) {
        sitorb_destroy(s);
        return NULL;
    }

    /* windowed sinc (Blackman), unity gain at 0 Hz */
    double fc = LPF_CUT_HZ / sample_rate, sum = 0;
    int m = s->taps - 1;
    for (int i = 0; i <= m; i++) {
        double t = i - m / 2.0;
        double sinc = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        double win = 0.42 - 0.5 * cos(2 * M_PI * i / m) + 0.08 * cos(4 * M_PI * i / m);
        s->h[i] = (float)(sinc * win);
        sum += s->h[i];
    }
    for (int i = 0; i <= m; i++)
        s->h[i] /= (float)sum;

    s->rot_b = cpolar(-2.0 * M_PI * TONE_HZ / FS);
    s->rot_y = cpolar(2.0 * M_PI * TONE_HZ / FS);
    s->ph_b = s->ph_y = (cpx){ 1, 0 };
    s->countdown = SPB;
    s->margin_min = 0.5f;
    s->last_out_bit = -1;
    memset(s->hist, -1, sizeof(s->hist));
    return s;
}

void sitorb_destroy(sitorb_t *s)
{
    if (!s)
        return;
    free(s->h);
    free(s->fir);
    free(s);
}

void sitorb_set_callback(sitorb_t *s, sitorb_cb_t cb, void *user)
{
    if (!s)
        return;
    s->cb = cb;
    s->user = user;
}

int sitorb_in_sync(const sitorb_t *s)
{
    return s ? s->locked : 0;
}

float sitorb_freq_offset(const sitorb_t *s)
{
    return s ? s->afc_hz : 0;
}
