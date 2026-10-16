/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "globals.h"

/* Spectrum renders SCREEN_WIDTH bins; the DSP decimates its single
 * DSP_MAX_NFFT transform down to this size. */
#define SPECTRUM_NFFT SCREEN_WIDTH

lv_obj_t *spectrum_init(lv_obj_t *overlay_parent, lv_coord_t y, lv_coord_t h);

/* Reposition/resize the spectrum strip. Only the direct-render geometry changes;
 * the NFFT-sized buffers are unaffected. */
void spectrum_set_geometry(lv_coord_t y, lv_coord_t h);
void      spectrum_data(const float *data_buf, uint16_t size, bool tx, uint32_t base_freq, uint8_t fft_dec, float min, float max);
void      spectrum_clear();

/* Pause/resume spectrum frame delivery (e.g. while FT8 owns the screen). The
 * DSP keeps running the FFT, S-meter and noise floor; only this subscription
 * stops accumulating and delivering. */
void spectrum_set_enabled(bool enabled);

/* Repaint the overlay (filter, notch, RTTY markers). UI thread only. */
void spectrum_refresh_overlay(void);

/* Direct-render entry point. Call from the main loop between lv_timer_handler()
 * and drm_flip(). Renders the spectrum into the DRM primary back-buffer when new
 * data arrived or the render conditions changed. Returns true if it rendered. */
bool spectrum_process(void);
// void spectrum_update_filters();
// void spectrum_update_factor();
