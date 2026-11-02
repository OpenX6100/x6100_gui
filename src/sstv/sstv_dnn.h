/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */

/*
 * SSTV ML denoiser: inference of a small convolutional network (DnCNN-like:
 * 3x3 convolutions and ReLU, no batch norm) in plain C, no libraries.  The
 * same file runs in the firmware and in the test programs on the computer.
 *
 * Model file (little endian):
 *   char     magic[8]  "SSTVDNN1"
 *   uint32   layers
 *   uint32   flags     bit 0: residual - the network gives the noise, the
 *                      output is input - network
 *   per layer:
 *     uint32 cin, cout, k (3), act (0 none, 1 ReLU)
 *     float  weight[cout][cin][k][k]
 *     float  bias[cout]
 * The first layer takes R, G, B in 0..1, the last gives 3 channels.
 */
#ifndef SSTV_DNN_H
#define SSTV_DNN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sstv_dnn sstv_dnn_t;

/* Reads a model file; NULL on error (message on stderr) */
sstv_dnn_t *sstv_dnn_load(const char *path);
void        sstv_dnn_free(sstv_dnn_t *m);

/* Multiply-adds for a picture of w x h (to tell the time it will take) */
double sstv_dnn_macs(const sstv_dnn_t *m, int w, int h);

/* Cleans rgb (w x h, 3 bytes per pixel) in place, with threads threads
 * (1..8).  Returns 0, or <0 if out of memory (rgb untouched). */
int sstv_dnn_run(const sstv_dnn_t *m, uint8_t *rgb, int w, int h, int threads);

/* Progress of the sstv_dnn_run() in course, 0..1000 (for a progress bar) */
extern volatile int sstv_dnn_progress;

/* Tests: 1 if the NEON inner loop is built in; sstv_dnn_generic = 1 makes
 * the next runs use the plain C loop instead (to compare the results) */
int sstv_dnn_has_neon(void);
extern int sstv_dnn_generic;

#ifdef __cplusplus
}
#endif

#endif
