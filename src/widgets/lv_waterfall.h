/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#ifndef LV_WATERFALL_H
#define LV_WATERFALL_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "lvgl/lvgl.h"

#include <time.h>

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    lv_img_t        obj;
    lv_img_dsc_t    *dsc;       /* the image LVGL draws: a window into ring */

    uint32_t        line_len;
    uint8_t         *line_buf;

    uint8_t         *ring;      /* 2 x height lines: each line is also kept one height below */
    uint32_t        head;       /* ring line of the newest row, 0 .. height - 1 */

    lv_color_t      *palette;
    uint16_t        palette_cnt;

    int16_t         min;
    int16_t         max;

    struct timespec frame_ts;   /* wall-clock timestamp of most recent PSD frame */
    /* 1KO125: noise floor of the lines, see lv_waterfall_add_data_with_ts() */
    bool            auto_levels;    /* off: the fixed min/max levels */
    float           *floor_buf;
    uint16_t        floor_buf_cnt;
    float           floor_db;
    bool            floor_valid;
} lv_waterfall_t;

extern const lv_obj_class_t lv_waterfall_class;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

lv_obj_t * lv_waterfall_create(lv_obj_t * parent);

/*=====================
 * Setter functions
 *====================*/

void lv_waterfall_set_palette(lv_obj_t * obj, lv_color_t * palette, uint16_t cnt);
void lv_waterfall_set_size(lv_obj_t * obj, lv_coord_t w, lv_coord_t h);

void lv_waterfall_clear_data(lv_obj_t * obj);
void lv_waterfall_add_data(lv_obj_t * obj, float * data, uint16_t cnt);

/* Add data with wall-clock timestamp for time-aligned processing (e.g. DNF). */
void lv_waterfall_add_data_with_ts(lv_obj_t * obj, float * data, uint16_t cnt, struct timespec ts);

/* Return the timestamp of the most recent PSD frame, or {0,0} if none yet. */
struct timespec lv_waterfall_get_frame_ts(lv_obj_t * obj);

void lv_waterfall_set_min(lv_obj_t *obj, int16_t val);
void lv_waterfall_set_max(lv_obj_t *obj, int16_t val);
/* 1KO125: colours follow the noise floor of the data instead of min/max
 * (for audio after the AGC, as FT8's). Off by default. */
void lv_waterfall_set_auto_levels(lv_obj_t *obj, bool on);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
