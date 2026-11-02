/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY 4-GFSK waveform
 *
 *  The instantaneous frequency is the tone sequence smoothed with a
 *  Gaussian filter (BT = 2.0), so a symbol's frequency pulse spreads over
 *  its neighbours by a fraction of a symbol. The phase is its running
 *  sum, continuous across symbols and frames.
 */

#include "jtty_mod.h"

#include <math.h>
#include <stdlib.h>

#define BT 2.0

/* Frequency pulse of one symbol at time t (in symbols, 0 = symbol
 * centre): a rectangle of one symbol convolved with a Gaussian. */
static double gfsk_pulse(double t)
{
    const double k = M_PI * sqrt(2.0 / log(2.0)) * BT;

    return 0.5 * (erf(k * (t + 0.5)) - erf(k * (t - 0.5)));
}

int jtty_mod_length(int nsym)
{
    return nsym * JTTY_NSPS + 2 * JTTY_MOD_RAMP;
}

int jtty_mod_wave(const uint8_t *tones, int nsym, float f0, int rate, float *out)
{
    const int    up = rate / JTTY_RATE;
    const int    nsps = JTTY_NSPS * up;
    const int    ramp = JTTY_MOD_RAMP * up;
    const int    n = nsym * nsps + 2 * ramp;
    const double dt = 1.0 / rate;
    double       phi = 0.0;

    if (up < 1 || nsym < 1) {
        return 0;
    }

    for (int i = 0; i < n; i++) {
        /* time in symbols from the start of the first symbol */
        double t = (double)(i - ramp) / nsps;
        int    j0 = (int)floor(t) - 2;
        double f = f0;
        double env = 1.0;

        /* the pulse is negligible beyond two symbols at BT = 2 */
        for (int j = j0; j <= j0 + 4; j++) {
            int s = j < 0 ? 0 : (j >= nsym ? nsym - 1 : j);

            f += JTTY_BAUD * tones[s] * gfsk_pulse(t - j - 0.5);
        }

        out[i] = (float)sin(phi);
        phi += 2.0 * M_PI * f * dt;
        if (phi > 2.0 * M_PI) {
            phi -= 2.0 * M_PI;
        }

        if (i < ramp) {
            env = 0.5 * (1.0 - cos(M_PI * i / ramp));
        } else if (i >= n - ramp) {
            env = 0.5 * (1.0 - cos(M_PI * (n - 1 - i) / ramp));
        }
        out[i] *= (float)env;
    }
    return n;
}
