/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 SSTV app
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
 * TX text rendering: 5x7 bitmap font, scaled/aligned text, color combinations.
 *
 * This file is NOT a separate compilation unit: it is #included by
 * dialog_sstv_tx.inc and must not be added to CMakeLists.txt.  All SSTV dialog
 * code still forms ONE translation unit, so every "static" keeps its
 * original meaning and no symbol is exported.
 */

typedef enum {
    TX_TEXT_ALIGN_LEFT = 0,
    TX_TEXT_ALIGN_CENTER,
    TX_TEXT_ALIGN_RIGHT
} tx_text_align_t;

/* Compact 5x7 bitmap font for the pixels that are actually transmitted.
 * This deliberately does not depend on LVGL rendering: tx_final_rgb is the
 * single WYSIWYG framebuffer used by both preview and, later, the encoder. */
static const uint8_t *tx_glyph5x7(char ch)
{
    static const uint8_t blank[5]={0,0,0,0,0};
    static const uint8_t glyphs[][5] = {
        {0x7E,0x11,0x11,0x11,0x7E}, /* A */
        {0x7F,0x49,0x49,0x49,0x36}, /* B */
        {0x3E,0x41,0x41,0x41,0x22}, /* C */
        {0x7F,0x41,0x41,0x22,0x1C}, /* D */
        {0x7F,0x49,0x49,0x49,0x41}, /* E */
        {0x7F,0x09,0x09,0x09,0x01}, /* F */
        {0x3E,0x41,0x49,0x49,0x7A}, /* G */
        {0x7F,0x08,0x08,0x08,0x7F}, /* H */
        {0x00,0x41,0x7F,0x41,0x00}, /* I */
        {0x20,0x40,0x41,0x3F,0x01}, /* J */
        {0x7F,0x08,0x14,0x22,0x41}, /* K */
        {0x7F,0x40,0x40,0x40,0x40}, /* L */
        {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
        {0x7F,0x04,0x08,0x10,0x7F}, /* N */
        {0x3E,0x41,0x41,0x41,0x3E}, /* O */
        {0x7F,0x09,0x09,0x09,0x06}, /* P */
        {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
        {0x7F,0x09,0x19,0x29,0x46}, /* R */
        {0x46,0x49,0x49,0x49,0x31}, /* S */
        {0x01,0x01,0x7F,0x01,0x01}, /* T */
        {0x3F,0x40,0x40,0x40,0x3F}, /* U */
        {0x1F,0x20,0x40,0x20,0x1F}, /* V */
        {0x3F,0x40,0x38,0x40,0x3F}, /* W */
        {0x63,0x14,0x08,0x14,0x63}, /* X */
        {0x07,0x08,0x70,0x08,0x07}, /* Y */
        {0x61,0x51,0x49,0x45,0x43}, /* Z */
        {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
        {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
        {0x42,0x61,0x51,0x49,0x46}, /* 2 */
        {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
        {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
        {0x27,0x45,0x45,0x45,0x39}, /* 5 */
        {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
        {0x01,0x71,0x09,0x05,0x03}, /* 7 */
        {0x36,0x49,0x49,0x49,0x36}, /* 8 */
        {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
        {0x20,0x10,0x08,0x04,0x02}  /* / */
    };
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 'A' && ch <= 'Z') return glyphs[ch-'A'];
    if (ch >= '0' && ch <= '9') return glyphs[26 + ch-'0'];
    if (ch == '/') return glyphs[36];
    return blank;
}

static void tx_rgb_pixel(uint8_t *rgb, unsigned int w, unsigned int h,
                         int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return;
    size_t o=((size_t)y*w+(unsigned int)x)*3;
    rgb[o]=r; rgb[o+1]=g; rgb[o+2]=b;
}

static int tx_text_width(const char *text, int scale)
{
    size_t n = text ? strlen(text) : 0;
    return n ? (int)(n * 6 - 1) * scale : 0;
}

static void tx_draw_text_aligned_color(uint8_t *rgb, unsigned int w, unsigned int h,
                                       int y, const char *text, int scale,
                                       tx_text_align_t align, tx_rgb_t color)
{
    if (!text || !*text) return;
    int tw=tx_text_width(text,scale);
    while (scale>1 && tw>(int)w-8) {
        scale--;
        tw=tx_text_width(text,scale);
    }

    const int margin=15;
    int x0;
    if (align==TX_TEXT_ALIGN_LEFT)
        x0=margin;
    else if (align==TX_TEXT_ALIGN_RIGHT)
        x0=(int)w-tw-margin;
    else
        x0=((int)w-tw)/2;

    if (x0<0) x0=0;

    for (int pass=0;pass<2;++pass) {
        for (size_t ci=0;text[ci];++ci) {
            const uint8_t *g=tx_glyph5x7(text[ci]);
            int gx=x0+(int)ci*6*scale;
            for (int x=0;x<5;x++) for (int yy=0;yy<7;yy++) {
                if (!(g[x]&(1u<<yy))) continue;
                for (int sy=0;sy<scale;sy++) for (int sx=0;sx<scale;sx++) {
                    int px=gx+x*scale+sx,py=y+yy*scale+sy;
                    if (pass==0) {
                        for (int oy=-4;oy<=4;oy++)
                            for (int ox=-4;ox<=4;ox++)
                                if (ox*ox+oy*oy<=17)
                                    tx_rgb_pixel(rgb,w,h,px+ox,py+oy,0,0,0);
                    } else {
                        tx_rgb_pixel(rgb,w,h,px,py,color.r,color.g,color.b);
                        tx_rgb_pixel(rgb,w,h,px+1,py,color.r,color.g,color.b);
                    }
                }
            }
        }
    }
}

/* Middle template text: slightly smaller than top/bottom and automatically
 * wrapped at a word boundary to at most two centered lines. */
static void tx_draw_middle_wrapped(uint8_t *rgb, unsigned int w, unsigned int h,
                                   const char *text)
{
    if (!text || !*text) return;
    int scale=(w<=160)?2:3;
    int max_chars=((int)w-12)/(6*scale);
    if (max_chars < 4) max_chars=4;

    char line1[256]={0}, line2[256]={0};
    size_t len=strlen(text);
    if ((int)len <= max_chars) {
        snprintf(line1,sizeof(line1),"%s",text);
    } else {
        int cut=max_chars;
        while (cut>0 && text[cut]!=' ') cut--;
        if (cut==0) cut=max_chars;
        snprintf(line1,sizeof(line1),"%.*s",cut,text);
        const char *p=text+cut;
        while (*p==' ') p++;
        snprintf(line2,sizeof(line2),"%.*s",max_chars,p);
    }

    int text_h=7*scale;
    int gap=scale+4;
    int total=line2[0] ? text_h*2+gap : text_h;
    int y=((int)h-total)/2;
    if (tx_message_type == SSTV_TX_MESSAGE_REPLAY)
        y -= (int)h * 34 / 256;
    if (y < 5) y = 5;
    tx_draw_text_aligned_color(rgb,w,h,y,line1,scale,
                               TX_TEXT_ALIGN_CENTER,tx_text_middle);
    if (line2[0])
        tx_draw_text_aligned_color(rgb,w,h,y+text_h+gap,line2,scale,
                                   TX_TEXT_ALIGN_CENTER,tx_text_middle);
}
