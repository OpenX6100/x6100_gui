/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */

/*
 * WEFAX postprocessing: the received fax through the WEFAX denoiser network
 * (a small U-Net, trained by tests/wefax/ml), in plain C, no libraries.  The
 * same file runs in the firmware and in the test program on the computer.
 *
 * The network: 3x3 convolutions with ReLU in blocks of two, 2x2 max pooling
 * and nearest upsampling, three levels (see tests/wefax/ml/model.py).  The
 * fax is cleaned in overlapping tiles, as run_full() of model.py does, so
 * the memory needed does not depend on the size of the fax.
 *
 * Model file (little endian), written by tests/wefax/ml/export_model.py:
 *   char     magic[8]  "WFXUNET1"
 *   uint32   base      channels of the first level (c1 = base, c2 = 2 base,
 *                      c3 = c4 = 4 base)
 *   15 convolutions, in this order: e1 a/b, e2 a/b, e3 a/b, mid a/b,
 *   d3 a/b, d2 a/b, d1 a/b, out (1x1), each:
 *     uint32 cin, cout, k
 *     float  weight[cout][cin][k][k]
 *     float  bias[cout]
 */
#ifndef WEFAX_PP_H
#define WEFAX_PP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wefax_pp wefax_pp_t;

/* Reads a model file; NULL on error (message on stderr) */
wefax_pp_t *wefax_pp_load(const char *path);
void        wefax_pp_free(wefax_pp_t *m);

int    wefax_pp_base(const wefax_pp_t *m);

/* Multiply-adds for a fax of w x h, tiles included (to tell the time) */
double wefax_pp_macs(const wefax_pp_t *m, int w, int h, int tile);

/* Cleans gray (w x h, 0 black .. 255 white) into out (the same size; may be
 * gray itself), with threads threads (1..8), in tiles of tile x tile pixels
 * (a multiple of 8; 0 = the default).  Returns 0, -1 on bad arguments, -2
 * if out of memory, -3 if stopped by wefax_pp_cancel (out is then
 * incomplete). */
int wefax_pp_run(const wefax_pp_t *m, const uint8_t *gray, uint8_t *out, int w, int h,
                 int threads, int tile);

#define WEFAX_PP_TILE   256     /* default tile, pixels */
#define WEFAX_PP_PAD     48     /* rows/columns of context around a tile */

/* Progress of the wefax_pp_run() in course, 0..1000 */
extern volatile int wefax_pp_progress;

/* Set to 1 (from any thread) to stop the wefax_pp_run() in course */
extern volatile int wefax_pp_cancel;

/* Tests: 1 if the NEON inner loop is built in; wefax_pp_generic = 1 makes
 * the next runs use the plain C loop instead (to compare the results) */
int wefax_pp_has_neon(void);
extern int wefax_pp_generic;

#ifdef __cplusplus
}
#endif

#endif
