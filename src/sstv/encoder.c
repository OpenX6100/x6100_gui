/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 SSTV encoder
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
*/



#include "encoder.h"
#include "decoder.h"

#include <png.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_W 320
#define SCOTTIE_H 256
#define ROBOT36_H 240
#define PI 3.14159265358979323846

typedef struct {
    FILE *fp;
    uint32_t samples;
    double phase;
} wav_writer_t;

static void put_u16(FILE *fp, uint16_t v) { uint8_t b[2]={(uint8_t)v,(uint8_t)(v>>8)}; fwrite(b,1,2,fp); }
static void put_u32(FILE *fp, uint32_t v) { uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; fwrite(b,1,4,fp); }

static int wav_begin(wav_writer_t *w, const char *path)
{
    memset(w,0,sizeof(*w));
    w->fp=fopen(path,"wb"); if(!w->fp) return -1;
    fwrite("RIFF",1,4,w->fp); put_u32(w->fp,0); fwrite("WAVEfmt ",1,8,w->fp);
    put_u32(w->fp,16); put_u16(w->fp,1); put_u16(w->fp,1);
    put_u32(w->fp,SSTV_SAMPLE_RATE); put_u32(w->fp,SSTV_SAMPLE_RATE*2);
    put_u16(w->fp,2); put_u16(w->fp,16); fwrite("data",1,4,w->fp); put_u32(w->fp,0);
    return 0;
}

static void wav_tone_samples(wav_writer_t *w, double hz, long count)
{
    const double step=2.0*PI*hz/(double)SSTV_SAMPLE_RATE;
    for(long i=0;i<count;++i){
        int16_t s=(int16_t)lrint(sin(w->phase)*24575.0); /* -2.5 dBFS */
        put_u16(w->fp,(uint16_t)s); w->phase+=step;
        if(w->phase>=2.0*PI) w->phase-=2.0*PI;
    }
    w->samples+=(uint32_t)count;
}

static void wav_tone_ms(wav_writer_t *w, double hz, double ms)
{
    long n=(long)llround(ms*(double)SSTV_SAMPLE_RATE/1000.0);
    wav_tone_samples(w,hz,n);
}

static void wav_channel(wav_writer_t *w, const uint8_t *rgb, int row, int channel)
{
    /* Scottie S1: 138.240 ms / 320 = 0.432 ms per pixel.  Accumulate
     * fractional sample boundaries so the complete channel has exact timing. */
    const double total=138.240*(double)SSTV_SAMPLE_RATE/1000.0;
    long prev=0;
    for(int x=0;x<OUT_W;++x){
        long next=(long)llround(total*(double)(x+1)/(double)OUT_W);
        uint8_t v=rgb[((size_t)row*OUT_W+x)*3u+(size_t)channel];
        double hz=1500.0+(800.0*(double)v/255.0);
        wav_tone_samples(w,hz,next-prev); prev=next;
    }
}

static void wav_vis(wav_writer_t *w, int vis)
{
    wav_tone_ms(w,1900,300); wav_tone_ms(w,1200,10); wav_tone_ms(w,1900,300);
    wav_tone_ms(w,1200,30); /* VIS start bit */
    int ones=0;
    for(int i=0;i<7;++i){ int bit=(vis>>i)&1; ones+=bit; wav_tone_ms(w,bit?1100:1300,30); }
    wav_tone_ms(w,(ones&1)?1100:1300,30); /* even parity */
    wav_tone_ms(w,1200,30); /* VIS stop bit */
}

static int load_png_scaled(const char *path, uint8_t *out, int out_h)
{
    FILE *fp=fopen(path,"rb"); if(!fp) return -1;
    png_structp p=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop info=p?png_create_info_struct(p):NULL;
    if(!p||!info){if(p)png_destroy_read_struct(&p,NULL,NULL);fclose(fp);return -2;}
    if(setjmp(png_jmpbuf(p))){png_destroy_read_struct(&p,&info,NULL);fclose(fp);return -3;}
    png_init_io(p,fp); png_read_info(p,info);
    png_uint_32 w=png_get_image_width(p,info),h=png_get_image_height(p,info);
    int depth=png_get_bit_depth(p,info), type=png_get_color_type(p,info);
    if(!w||!h||w>10000||h>10000){png_destroy_read_struct(&p,&info,NULL);fclose(fp);return -4;}
    if(depth==16) png_set_strip_16(p);
    if(type==PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(p);
    if(type==PNG_COLOR_TYPE_GRAY&&depth<8)png_set_expand_gray_1_2_4_to_8(p);
    if(png_get_valid(p,info,PNG_INFO_tRNS))png_set_tRNS_to_alpha(p);
    if(type==PNG_COLOR_TYPE_GRAY||type==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(p);
    png_read_update_info(p,info);
    int ch=png_get_channels(p,info); png_size_t rb=png_get_rowbytes(p,info);
    uint8_t *raw=malloc((size_t)rb*h); png_bytep *rows=malloc(sizeof(*rows)*h);
    if(!raw||!rows){free(raw);free(rows);png_destroy_read_struct(&p,&info,NULL);fclose(fp);return -5;}
    for(png_uint_32 y=0;y<h;++y)rows[y]=raw+(size_t)y*rb;
    png_read_image(p,rows); png_read_end(p,NULL); png_destroy_read_struct(&p,&info,NULL); fclose(fp); free(rows);
    if(ch<3){free(raw);return -6;}
    for(int y=0;y<out_h;++y){png_uint_32 sy=(png_uint_32)((uint64_t)y*h/(uint64_t)out_h);if(sy>=h)sy=h-1;
        for(int x=0;x<OUT_W;++x){png_uint_32 sx=(png_uint_32)((uint64_t)x*w/OUT_W);if(sx>=w)sx=w-1;
            const uint8_t *s=raw+(size_t)sy*rb+(size_t)sx*ch; uint8_t *d=out+((size_t)y*OUT_W+x)*3u;
            d[0]=s[0];d[1]=s[1];d[2]=s[2];}}
    free(raw); return 0;
}

static uint8_t clamp_byte(double v)
{
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return (uint8_t)lrint(v);
}

static void wav_values(wav_writer_t *w, const uint8_t *values, int count, double total_ms)
{
    const double total = total_ms * (double)SSTV_SAMPLE_RATE / 1000.0;
    long prev = 0;
    for (int x = 0; x < count; ++x) {
        long next = (long)llround(total * (double)(x + 1) / (double)count);
        double hz = 1500.0 + 800.0 * (double)values[x] / 255.0;
        wav_tone_samples(w, hz, next - prev);
        prev = next;
    }
}


/* Martin M2: VIS 40, 320x256.  Each line is 4.862 ms sync, 0.572 ms
 * porch, then G/B/R channels of 73.216 ms separated by 0.572 ms. */
static void wav_rgb_channel(wav_writer_t *w, const uint8_t *rgb, int row,
                            int channel, double total_ms)
{
    const double total = total_ms * (double)SSTV_SAMPLE_RATE / 1000.0;
    long prev = 0;
    for (int x = 0; x < OUT_W; ++x) {
        long next = (long)llround(total * (double)(x + 1) / (double)OUT_W);
        uint8_t v = rgb[((size_t)row * OUT_W + x) * 3u + (size_t)channel];
        double hz = 1500.0 + 800.0 * (double)v / 255.0;
        wav_tone_samples(w, hz, next - prev);
        prev = next;
    }
}

int sstv_encode_martin_m2_png_wav(const char *png_path, const char *wav_path)
{
    const int height = 256;
    const double channel_ms = 73.216;
    uint8_t *rgb = malloc((size_t)OUT_W * height * 3u);
    if (!rgb) return -10;
    int rc = load_png_scaled(png_path, rgb, height);
    if (rc < 0) { free(rgb); return rc; }

    wav_writer_t w;
    if (wav_begin(&w, wav_path) < 0) { free(rgb); return -11; }
    wav_vis(&w, 40);

    for (int y = 0; y < height; ++y) {
        wav_tone_ms(&w, 1200, 4.862);
        wav_tone_ms(&w, 1500, 0.572);
        wav_rgb_channel(&w, rgb, y, 1, channel_ms); /* G */
        wav_tone_ms(&w, 1500, 0.572);
        wav_rgb_channel(&w, rgb, y, 2, channel_ms); /* B */
        wav_tone_ms(&w, 1500, 0.572);
        wav_rgb_channel(&w, rgb, y, 0, channel_ms); /* R */
        wav_tone_ms(&w, 1500, 0.572);
    }

    free(rgb);
    uint32_t data = w.samples * 2u;
    fseek(w.fp, 4, SEEK_SET); put_u32(w.fp, 36u + data);
    fseek(w.fp, 40, SEEK_SET); put_u32(w.fp, data);
    fclose(w.fp);
    fprintf(stderr, "SSTV TX TEST: Martin M2 WAV written to %s (%u samples, %.3f s)\n",
            wav_path, w.samples, (double)w.samples / SSTV_SAMPLE_RATE);
    return 0;
}

int sstv_encode_robot36_png_wav(const char *png_path, const char *wav_path)
{
    uint8_t *rgb = malloc((size_t)OUT_W * ROBOT36_H * 3u);
    if (!rgb) return -10;
    int rc = load_png_scaled(png_path, rgb, ROBOT36_H);
    if (rc < 0) { free(rgb); return rc; }

    wav_writer_t w;
    if (wav_begin(&w, wav_path) < 0) { free(rgb); return -11; }

    wav_vis(&w, 8); /* Robot 36 */

    uint8_t yline[OUT_W];
    uint8_t chroma[OUT_W];

    for (int y = 0; y < ROBOT36_H; ++y) {
        const uint8_t *row = rgb + (size_t)y * OUT_W * 3u;

        /* Full-resolution luminance for the current scan line. */
        for (int x = 0; x < OUT_W; ++x) {
            double r = row[(size_t)x * 3u + 0];
            double g = row[(size_t)x * 3u + 1];
            double b = row[(size_t)x * 3u + 2];
            yline[x] = clamp_byte(0.299 * r + 0.587 * g + 0.114 * b);
        }

        /* Robot 36 is 4:2:0 vertically: one chroma component is sent on
         * each line, averaged over the even/odd line pair.  Even lines carry
         * R-Y (Cr), odd lines carry B-Y (Cb).  The 44 ms chroma sweep still
         * spans all 320 horizontal pixels; its shorter duration provides the
         * horizontal chroma bandwidth reduction. */
        int pair0 = y & ~1;
        int pair1 = pair0 + 1;
        if (pair1 >= ROBOT36_H) pair1 = pair0;
        const uint8_t *row0 = rgb + (size_t)pair0 * OUT_W * 3u;
        const uint8_t *row1 = rgb + (size_t)pair1 * OUT_W * 3u;

        for (int x = 0; x < OUT_W; ++x) {
            double sum = 0.0;
            const uint8_t *rows[2] = { row0, row1 };
            for (int k = 0; k < 2; ++k) {
                const uint8_t *p = rows[k] + (size_t)x * 3u;
                double r = p[0], g = p[1], b = p[2];
                double yy = 0.299 * r + 0.587 * g + 0.114 * b;
                sum += (y & 1) ? ((b - yy) / 1.772 + 128.0)
                               : ((r - yy) / 1.402 + 128.0);
            }
            chroma[x] = clamp_byte(sum * 0.5);
        }

        /* Nominal Robot 36 line = 150 ms:
         * sync 9, porch 3, Y 88, separator 4.5, porch 1.5, chroma 44.
         * The separator identifies the chroma component: 1500 Hz on even
         * (R-Y) lines, 2300 Hz on odd (B-Y) lines. */
        wav_tone_ms(&w, 1200, 9.0);
        wav_tone_ms(&w, 1500, 3.0);
        wav_values(&w, yline, OUT_W, 88.0);
        wav_tone_ms(&w, (y & 1) ? 2300 : 1500, 4.5);
        wav_tone_ms(&w, 1900, 1.5);
        wav_values(&w, chroma, OUT_W, 44.0);
    }

    free(rgb);
    uint32_t data = w.samples * 2u;
    fseek(w.fp, 4, SEEK_SET); put_u32(w.fp, 36u + data);
    fseek(w.fp, 40, SEEK_SET); put_u32(w.fp, data);
    fclose(w.fp);
    fprintf(stderr, "SSTV TX TEST: Robot 36 WAV written to %s (%u samples, %.3f s)\n",
            wav_path, w.samples, (double)w.samples / SSTV_SAMPLE_RATE);
    return 0;
}

int sstv_encode_scottie_s1_png_wav(const char *png_path, const char *wav_path)
{
    uint8_t *rgb=malloc((size_t)OUT_W*SCOTTIE_H*3u); if(!rgb)return -10;
    int rc=load_png_scaled(png_path,rgb,SCOTTIE_H); if(rc<0){free(rgb);return rc;}
    wav_writer_t w; if(wav_begin(&w,wav_path)<0){free(rgb);return -11;}
    wav_vis(&w,60); /* Scottie S1 */
    wav_tone_ms(&w,1200,9.0); /* extra starting sync */
    for(int y=0;y<SCOTTIE_H;++y){
        wav_tone_ms(&w,1500,1.5); wav_channel(&w,rgb,y,1); /* G */
        wav_tone_ms(&w,1500,1.5); wav_channel(&w,rgb,y,2); /* B */
        wav_tone_ms(&w,1200,9.0); wav_tone_ms(&w,1500,1.5); wav_channel(&w,rgb,y,0); /* R */
    }
    free(rgb);
    uint32_t data=w.samples*2u; fseek(w.fp,4,SEEK_SET);put_u32(w.fp,36u+data);fseek(w.fp,40,SEEK_SET);put_u32(w.fp,data);
    fclose(w.fp);
    fprintf(stderr,"SSTV TX TEST: Scottie S1 WAV written to %s (%u samples, %.3f s)\n",wav_path,w.samples,(double)w.samples/SSTV_SAMPLE_RATE);
    return 0;
}

/* -------------------------------------------------------------------------
 * Direct PCM Robot 36 encoder for radio TX.  This deliberately lives beside
 * the already validated WAV encoder: the protocol math is identical, only
 * the output sink/sample rate differ. */
typedef struct {
    int16_t *data;
    size_t count;
    size_t capacity;
    unsigned rate;
    double phase;
    double timeline_samples; /* ideal absolute sample position */
} pcm_writer_t;

static int pcm_reserve(pcm_writer_t *w, size_t extra)
{
    if (extra > SIZE_MAX - w->count) return -1;
    size_t need = w->count + extra;
    if (need <= w->capacity) return 0;
    size_t cap = w->capacity ? w->capacity : (size_t)w->rate * 4u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) { cap = need; break; }
        cap *= 2u;
    }
    int16_t *p = realloc(w->data, cap * sizeof(*p));
    if (!p) return -1;
    w->data = p;
    w->capacity = cap;
    return 0;
}

static int pcm_tone_samples(pcm_writer_t *w, double hz, size_t count)
{
    if (pcm_reserve(w, count) < 0) return -1;
    const double step = 2.0 * PI * hz / (double)w->rate;
    for (size_t i = 0; i < count; ++i) {
        w->data[w->count++] = (int16_t)lrint(sin(w->phase) * 24575.0);
        w->phase += step;
        if (w->phase >= 2.0 * PI) w->phase -= 2.0 * PI;
    }
    return 0;
}

static int pcm_tone_ms(pcm_writer_t *w, double hz, double ms)
{
    /* Keep one continuous ideal timeline for the whole transmission.
     * Rounding each SSTV segment independently creates a repeatable error on
     * every scan line and therefore visible slant.  Round only the absolute
     * boundary, so fractional samples are carried into the following segment. */
    w->timeline_samples += ms * (double)w->rate / 1000.0;
    long target = (long)llround(w->timeline_samples);
    if (target < (long)w->count) return -1;
    return pcm_tone_samples(w, hz, (size_t)(target - (long)w->count));
}

static int pcm_values(pcm_writer_t *w, const uint8_t *values, int count, double total_ms)
{
    const double start = w->timeline_samples;
    const double total = total_ms * (double)w->rate / 1000.0;
    for (int x = 0; x < count; ++x) {
        double boundary = start + total * (double)(x + 1) / (double)count;
        long target = (long)llround(boundary);
        if (target < (long)w->count) return -1;
        double hz = 1500.0 + 800.0 * (double)values[x] / 255.0;
        if (pcm_tone_samples(w, hz, (size_t)(target - (long)w->count)) < 0) return -1;
    }
    w->timeline_samples = start + total;
    return 0;
}

static int pcm_vis(pcm_writer_t *w, int vis)
{
#define PT(hz,ms) do { if (pcm_tone_ms(w,(hz),(ms)) < 0) return -1; } while (0)
    PT(1900,300); PT(1200,10); PT(1900,300); PT(1200,30);
    int ones = 0;
    for (int i = 0; i < 7; ++i) {
        int bit = (vis >> i) & 1;
        ones += bit;
        PT(bit ? 1100 : 1300, 30);
    }
    PT((ones & 1) ? 1100 : 1300, 30);
    PT(1200,30);
#undef PT
    return 0;
}


static int pcm_rgb_channel(pcm_writer_t *w, const uint8_t *rgb, int row,
                           int channel, double total_ms)
{
    uint8_t values[OUT_W];
    for (int x = 0; x < OUT_W; ++x)
        values[x] = rgb[((size_t)row * OUT_W + x) * 3u + (size_t)channel];
    return pcm_values(w, values, OUT_W, total_ms);
}

int sstv_encode_martin_m2_png_pcm(const char *png_path, unsigned sample_rate,
                                  int16_t **samples, size_t *sample_count)
{
    const int height = 256;
    const double channel_ms = 73.216;
    if (!png_path || !samples || !sample_count || sample_rate < 8000u) return -20;
    *samples = NULL;
    *sample_count = 0;

    uint8_t *rgb = malloc((size_t)OUT_W * height * 3u);
    if (!rgb) return -10;
    int rc = load_png_scaled(png_path, rgb, height);
    if (rc < 0) { free(rgb); return rc; }

    pcm_writer_t w = { .rate = sample_rate };
    if (pcm_vis(&w, 40) < 0) { free(rgb); free(w.data); return -21; }

#define MARTIN_T(hz,ms) do { if (pcm_tone_ms(&w,(hz),(ms)) < 0) { rc=-21; goto out_martin_m2; } } while (0)
    for (int y = 0; y < height; ++y) {
        MARTIN_T(1200, 4.862);
        MARTIN_T(1500, 0.572);
        if (pcm_rgb_channel(&w, rgb, y, 1, channel_ms) < 0) { rc=-21; goto out_martin_m2; }
        MARTIN_T(1500, 0.572);
        if (pcm_rgb_channel(&w, rgb, y, 2, channel_ms) < 0) { rc=-21; goto out_martin_m2; }
        MARTIN_T(1500, 0.572);
        if (pcm_rgb_channel(&w, rgb, y, 0, channel_ms) < 0) { rc=-21; goto out_martin_m2; }
        MARTIN_T(1500, 0.572);
    }
    rc = 0;
#undef MARTIN_T

out_martin_m2:
    free(rgb);
    if (rc < 0) { free(w.data); return rc; }
    *samples = w.data;
    *sample_count = w.count;
    fprintf(stderr, "SSTV TX: Martin M2 PCM generated (%zu samples @ %u Hz, %.3f s)\n",
            w.count, sample_rate, (double)w.count / (double)sample_rate);
    return 0;
}

int sstv_encode_robot36_png_pcm(const char *png_path, unsigned sample_rate,
                                int16_t **samples, size_t *sample_count)
{
    if (!png_path || !samples || !sample_count || sample_rate < 8000u) return -20;
    *samples = NULL;
    *sample_count = 0;

    uint8_t *rgb = malloc((size_t)OUT_W * ROBOT36_H * 3u);
    if (!rgb) return -10;
    int rc = load_png_scaled(png_path, rgb, ROBOT36_H);
    if (rc < 0) { free(rgb); return rc; }

    pcm_writer_t w = { .rate = sample_rate };
    if (pcm_vis(&w, 8) < 0) { free(rgb); free(w.data); return -21; }

    uint8_t yline[OUT_W];
    uint8_t chroma[OUT_W];
    for (int y = 0; y < ROBOT36_H; ++y) {
        const uint8_t *row = rgb + (size_t)y * OUT_W * 3u;
        for (int x = 0; x < OUT_W; ++x) {
            double r=row[(size_t)x*3u], g=row[(size_t)x*3u+1], b=row[(size_t)x*3u+2];
            yline[x] = clamp_byte(0.299*r + 0.587*g + 0.114*b);
        }

        int pair0 = y & ~1;
        int pair1 = pair0 + 1;
        if (pair1 >= ROBOT36_H) pair1 = pair0;
        const uint8_t *row0 = rgb + (size_t)pair0 * OUT_W * 3u;
        const uint8_t *row1 = rgb + (size_t)pair1 * OUT_W * 3u;
        for (int x = 0; x < OUT_W; ++x) {
            double sum = 0.0;
            const uint8_t *rows[2] = { row0, row1 };
            for (int k = 0; k < 2; ++k) {
                const uint8_t *q = rows[k] + (size_t)x * 3u;
                double r=q[0], g=q[1], b=q[2];
                double yy=0.299*r + 0.587*g + 0.114*b;
                sum += (y & 1) ? ((b-yy)/1.772 + 128.0)
                               : ((r-yy)/1.402 + 128.0);
            }
            chroma[x] = clamp_byte(sum * 0.5);
        }

#define PCMT(hz,ms) do { if (pcm_tone_ms(&w,(hz),(ms)) < 0) { rc=-21; goto out; } } while (0)
        PCMT(1200,9.0);
        PCMT(1500,3.0);
        if (pcm_values(&w,yline,OUT_W,88.0)<0) { rc=-21; goto out; }
        PCMT((y & 1) ? 2300 : 1500,4.5);
        PCMT(1900,1.5);
        if (pcm_values(&w,chroma,OUT_W,44.0)<0) { rc=-21; goto out; }
#undef PCMT
    }
    rc = 0;
out:
    free(rgb);
    if (rc < 0) { free(w.data); return rc; }
    *samples = w.data;
    *sample_count = w.count;
    fprintf(stderr,"SSTV TX: Robot 36 PCM generated (%zu samples @ %u Hz, %.3f s)\n",
            w.count, sample_rate, (double)w.count/(double)sample_rate);
    return 0;
}


int sstv_encode_scottie_s1_png_pcm(const char *png_path, unsigned sample_rate,
                                   int16_t **samples, size_t *sample_count)
{
    if (!png_path || !samples || !sample_count || sample_rate < 8000u) return -20;
    *samples = NULL;
    *sample_count = 0;

    uint8_t *rgb = malloc((size_t)OUT_W * SCOTTIE_H * 3u);
    if (!rgb) return -10;
    int rc = load_png_scaled(png_path, rgb, SCOTTIE_H);
    if (rc < 0) { free(rgb); return rc; }

    pcm_writer_t w = { .rate = sample_rate };
    if (pcm_vis(&w, 60) < 0) { free(rgb); free(w.data); return -21; }

    /* Scottie S1 starts with an additional 9 ms sync pulse, then each line is
     * G, B, sync, R.  Keep exactly the same nominal timings as the already
     * validated WAV encoder, but render them at the radio playback rate. */
    if (pcm_tone_ms(&w, 1200, 9.0) < 0) { rc = -21; goto out_scottie; }
    for (int y = 0; y < SCOTTIE_H; ++y) {
        uint8_t channel[OUT_W];

#define SCT(hz,ms) do { if (pcm_tone_ms(&w,(hz),(ms)) < 0) { rc=-21; goto out_scottie; } } while (0)
        SCT(1500, 1.5);
        for (int x=0; x<OUT_W; ++x) channel[x]=rgb[((size_t)y*OUT_W+x)*3u+1u];
        if (pcm_values(&w,channel,OUT_W,138.240)<0) { rc=-21; goto out_scottie; }

        SCT(1500, 1.5);
        for (int x=0; x<OUT_W; ++x) channel[x]=rgb[((size_t)y*OUT_W+x)*3u+2u];
        if (pcm_values(&w,channel,OUT_W,138.240)<0) { rc=-21; goto out_scottie; }

        SCT(1200, 9.0);
        SCT(1500, 1.5);
        for (int x=0; x<OUT_W; ++x) channel[x]=rgb[((size_t)y*OUT_W+x)*3u+0u];
        if (pcm_values(&w,channel,OUT_W,138.240)<0) { rc=-21; goto out_scottie; }
#undef SCT
    }
    rc = 0;

out_scottie:
    free(rgb);
    if (rc < 0) { free(w.data); return rc; }
    *samples = w.data;
    *sample_count = w.count;
    fprintf(stderr,"SSTV TX: Scottie S1 PCM generated (%zu samples @ %u Hz, %.3f s)\n",
            w.count, sample_rate, (double)w.count/(double)sample_rate);
    return 0;
}

/* Additional direct-PCM encoders used by SSTV Test TX. */
static int encode_martin_pcm_common(const char *png_path, unsigned sample_rate,
                                    int vis, double channel_ms,
                                    int16_t **samples, size_t *sample_count)
{
    const int height=256;
    if(!png_path||!samples||!sample_count||sample_rate<8000u)return -20;
    *samples=NULL; *sample_count=0;
    uint8_t *rgb=malloc((size_t)OUT_W*height*3u); if(!rgb)return -10;
    int rc=load_png_scaled(png_path,rgb,height); if(rc<0){free(rgb);return rc;}
    pcm_writer_t w={.rate=sample_rate}; if(pcm_vis(&w,vis)<0){free(rgb);free(w.data);return -21;}
#define MT(hz,ms) do{if(pcm_tone_ms(&w,(hz),(ms))<0){rc=-21;goto mout;}}while(0)
    for(int y=0;y<height;++y){
        MT(1200,4.862); MT(1500,0.572);
        if(pcm_rgb_channel(&w,rgb,y,1,channel_ms)<0){rc=-21;goto mout;}
        MT(1500,0.572);
        if(pcm_rgb_channel(&w,rgb,y,2,channel_ms)<0){rc=-21;goto mout;}
        MT(1500,0.572);
        if(pcm_rgb_channel(&w,rgb,y,0,channel_ms)<0){rc=-21;goto mout;}
        MT(1500,0.572);
    }
    rc=0;
#undef MT
mout:
    free(rgb); if(rc<0){free(w.data);return rc;}
    *samples=w.data; *sample_count=w.count; return 0;
}

int sstv_encode_martin_m1_png_pcm(const char *p,unsigned r,int16_t **s,size_t *n)
{ return encode_martin_pcm_common(p,r,44,146.432,s,n); }

static int encode_scottie_pcm_common(const char *png_path,unsigned sample_rate,
                                     int vis,double channel_ms,
                                     int16_t **samples,size_t *sample_count)
{
    if(!png_path||!samples||!sample_count||sample_rate<8000u)return -20;
    *samples=NULL; *sample_count=0;
    uint8_t *rgb=malloc((size_t)OUT_W*SCOTTIE_H*3u); if(!rgb)return -10;
    int rc=load_png_scaled(png_path,rgb,SCOTTIE_H); if(rc<0){free(rgb);return rc;}
    pcm_writer_t w={.rate=sample_rate}; if(pcm_vis(&w,vis)<0){free(rgb);free(w.data);return -21;}
    if(pcm_tone_ms(&w,1200,9.0)<0){rc=-21;goto sout;}
    for(int y=0;y<SCOTTIE_H;++y){uint8_t c[OUT_W];
#define ST(hz,ms) do{if(pcm_tone_ms(&w,(hz),(ms))<0){rc=-21;goto sout;}}while(0)
        ST(1500,1.5); for(int x=0;x<OUT_W;++x)c[x]=rgb[((size_t)y*OUT_W+x)*3u+1]; if(pcm_values(&w,c,OUT_W,channel_ms)<0){rc=-21;goto sout;}
        ST(1500,1.5); for(int x=0;x<OUT_W;++x)c[x]=rgb[((size_t)y*OUT_W+x)*3u+2]; if(pcm_values(&w,c,OUT_W,channel_ms)<0){rc=-21;goto sout;}
        ST(1200,9.0); ST(1500,1.5); for(int x=0;x<OUT_W;++x)c[x]=rgb[((size_t)y*OUT_W+x)*3u]; if(pcm_values(&w,c,OUT_W,channel_ms)<0){rc=-21;goto sout;}
#undef ST
    } rc=0;
sout:
    free(rgb); if(rc<0){free(w.data);return rc;} *samples=w.data;*sample_count=w.count;return 0;
}

int sstv_encode_scottie_s2_png_pcm(const char *p,unsigned r,int16_t **s,size_t *n)
{ return encode_scottie_pcm_common(p,r,56,88.064,s,n); }

int sstv_encode_robot8_png_pcm(const char *png_path,unsigned sample_rate,int16_t **samples,size_t *sample_count)
{
    const int H=120,W=160;
    if(!png_path||!samples||!sample_count||sample_rate<8000u)return -20;
    *samples=NULL;*sample_count=0;
    uint8_t *rgb=malloc((size_t)OUT_W*H*3u);if(!rgb)return -10;
    int rc=load_png_scaled(png_path,rgb,H);if(rc<0){free(rgb);return rc;}
    pcm_writer_t w={.rate=sample_rate};if(pcm_vis(&w,2)<0){free(rgb);free(w.data);return -21;}
    uint8_t Y[W];
    for(int y=0;y<H;++y){
        /* Published timing (SSTV Handbook, table 4.1): 10 ms sync, then
         * luminance for the rest of the 66.667 ms line (900 lpm). */
        if(pcm_tone_ms(&w,1200,10.0)<0){rc=-21;goto r8out;}
        for(int x=0;x<W;++x){int sx=x*2;const uint8_t*q=rgb+((size_t)y*OUT_W+sx)*3u;Y[x]=clamp_byte(.299*q[0]+.587*q[1]+.114*q[2]);}
        if(pcm_values(&w,Y,W,1000.0/15.0-10.0)<0){rc=-21;goto r8out;}
    }rc=0;
r8out: free(rgb);if(rc<0){free(w.data);return rc;}*samples=w.data;*sample_count=w.count;return 0;
}

int sstv_encode_robot72_png_pcm(const char *png_path,unsigned sample_rate,int16_t **samples,size_t *sample_count)
{
    const int H=240;
    if(!png_path||!samples||!sample_count||sample_rate<8000u)return -20;
    *samples=NULL;*sample_count=0;
    uint8_t *rgb=malloc((size_t)OUT_W*H*3u);if(!rgb)return -10;
    int rc=load_png_scaled(png_path,rgb,H);if(rc<0){free(rgb);return rc;}
    pcm_writer_t w={.rate=sample_rate};if(pcm_vis(&w,12)<0){free(rgb);free(w.data);return -21;}
    uint8_t Y[OUT_W],Cr[OUT_W],Cb[OUT_W];
    for(int y=0;y<H;++y){const uint8_t*row=rgb+(size_t)y*OUT_W*3u;
        for(int x=0;x<OUT_W;++x){double R=row[x*3],G=row[x*3+1],B=row[x*3+2],yy=.299*R+.587*G+.114*B;Y[x]=clamp_byte(yy);Cr[x]=clamp_byte((R-yy)/1.402+128.0);Cb[x]=clamp_byte((B-yy)/1.772+128.0);}
#define RT(hz,ms) do{if(pcm_tone_ms(&w,(hz),(ms))<0){rc=-21;goto r72out;}}while(0)
        RT(1200,9.0);RT(1500,3.0);if(pcm_values(&w,Y,OUT_W,138.0)<0){rc=-21;goto r72out;}
        RT(1500,4.5);RT(1900,1.5);if(pcm_values(&w,Cr,OUT_W,69.0)<0){rc=-21;goto r72out;}
        RT(2300,4.5);RT(1500,1.5);if(pcm_values(&w,Cb,OUT_W,69.0)<0){rc=-21;goto r72out;}
#undef RT
    }rc=0;
r72out: free(rgb);if(rc<0){free(w.data);return rc;}*samples=w.data;*sample_count=w.count;return 0;
}
