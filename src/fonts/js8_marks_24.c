/*
 * The two glyphs the JS8 message list needs that sony_24 lacks: the
 * end-of-message diamond (U+2662, desktop JS8Call's) and the degree sign
 * (U+00B0). Used as sony_24's fallback there. From Adwaita Sans and Adwaita
 * Mono Regular (SIL Open Font License 1.1), converted with lv_font_conv 1.5.2:
 *   --font AdwaitaSans-Regular.ttf -r 0xB0 --font AdwaitaMono-Regular.ttf -r 0x2662
 *   --size 24 --bpp 4 --no-compress --format lvgl
 */
/*******************************************************************************
 * Size: 24 px
 * Bpp: 4
 * Opts: --font /usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf -r 0xB0 --font /usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf -r 0x2662 --size 24 --bpp 4 --no-compress --format lvgl -o js8_marks_24.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef JS8_MARKS_24
#define JS8_MARKS_24 1
#endif

#if JS8_MARKS_24

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+00B0 "°" */
    0x0, 0x29, 0xb9, 0x20, 0x0, 0x5f, 0xff, 0xff,
    0x40, 0x1f, 0xf4, 0x5, 0xff, 0x6, 0xf6, 0x0,
    0x7, 0xf5, 0x6f, 0x50, 0x0, 0x6f, 0x53, 0xfc,
    0x10, 0x1d, 0xf2, 0x9, 0xfe, 0xcf, 0xf8, 0x0,
    0x7, 0xdf, 0xd6, 0x0,

    /* U+2662 "♢" */
    0x0, 0x0, 0x0, 0x3, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x9e, 0x10, 0x0, 0x0, 0x0, 0x0,
    0x5, 0xff, 0xb0, 0x0, 0x0, 0x0, 0x0, 0x2f,
    0xd9, 0xf8, 0x0, 0x0, 0x0, 0x0, 0xdf, 0x30,
    0xcf, 0x40, 0x0, 0x0, 0x9, 0xf6, 0x0, 0x1e,
    0xe1, 0x0, 0x0, 0x6f, 0xa0, 0x0, 0x4, 0xfc,
    0x0, 0x2, 0xfd, 0x0, 0x0, 0x0, 0x8f, 0x80,
    0xd, 0xf2, 0x0, 0x0, 0x0, 0xc, 0xf4, 0xa,
    0xf6, 0x0, 0x0, 0x0, 0x1e, 0xe1, 0x0, 0xdf,
    0x20, 0x0, 0x0, 0xbf, 0x40, 0x0, 0x2f, 0xd0,
    0x0, 0x7, 0xf8, 0x0, 0x0, 0x6, 0xfa, 0x0,
    0x4f, 0xc0, 0x0, 0x0, 0x0, 0x9f, 0x61, 0xee,
    0x10, 0x0, 0x0, 0x0, 0xd, 0xfc, 0xf4, 0x0,
    0x0, 0x0, 0x0, 0x2, 0xff, 0x80, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x5b, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 175, .box_w = 9, .box_h = 8, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 36, .adv_w = 230, .box_w = 14, .box_h = 18, .ofs_x = 0, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x25b2
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 176, .range_length = 9651, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 2, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t js8_marks_24 = {
#else
lv_font_t js8_marks_24 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 19,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -4,
    .underline_thickness = 2,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if JS8_MARKS_24*/

