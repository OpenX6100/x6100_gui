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
 * RX: live capture, VIS detection, decoding threads, PNG saving, audio callback.
 *
 * This file is NOT a separate compilation unit: it is #included by
 * dialog_sstv.c and must not be added to CMakeLists.txt.  All SSTV dialog
 * code still forms ONE translation unit, so every "static" keeps its
 * original meaning and no symbol is exported.
 */
static struct timespec live_dbg_last_cb_ts;
static uint64_t live_dbg_cb_calls = 0;
static uint64_t live_dbg_cb_samples = 0;
static uint64_t live_dbg_capture_cb_samples = 0;
static size_t live_dbg_min_n = 0;
static size_t live_dbg_max_n = 0;
static double live_dbg_sum_gap_ms = 0.0;
static double live_dbg_max_gap_ms = 0.0;
static unsigned int live_dbg_gap_count = 0;
static int live_dbg_next_report_sec = 5;
static bool live_dbg_have_last_cb = false;

static double sstv_dbg_elapsed(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static void sstv_dbg_capture_start(long prebuffer)
{
    clock_gettime(CLOCK_MONOTONIC, &live_dbg_capture_ts);
    live_dbg_capture_cb_samples = 0;
    live_dbg_next_report_sec = 5;
    fprintf(stderr,
            "SSTV DBG: CAPTURE START mode=%s SR=%d target=%ld (%.3fs) "
            "prebuffer=%ld (%.3fs) remaining=%ld (%.3fs) "
            "cb_total_samples=%llu cb_calls=%llu\n",
            live_info.name, SSTV_SAMPLE_RATE,
            live_target, (double)live_target / SSTV_SAMPLE_RATE,
            prebuffer, (double)prebuffer / SSTV_SAMPLE_RATE,
            live_target - prebuffer,
            (double)(live_target - prebuffer) / SSTV_SAMPLE_RATE,
            (unsigned long long)live_dbg_cb_samples,
            (unsigned long long)live_dbg_cb_calls);
}

static void sstv_dbg_capture_report(const char *tag)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double wall = sstv_dbg_elapsed(&live_dbg_capture_ts, &now);
    double appended = (double)live_dbg_capture_cb_samples / SSTV_SAMPLE_RATE;
    double buffered = (double)live_written / SSTV_SAMPLE_RATE;
    double pct = live_target > 0 ?
        100.0 * (double)live_written / (double)live_target : 0.0;
    double rate = wall > 0.0 ?
        (double)live_dbg_capture_cb_samples / wall : 0.0;
    double avg_gap = live_dbg_gap_count ?
        live_dbg_sum_gap_ms / live_dbg_gap_count : 0.0;

    fprintf(stderr,
            "SSTV DBG: %-8s wall=%.3fs appended=%llu (%.3fs@SR) "
            "buffered=%ld (%.3fs@SR) progress=%.1f%% "
            "effective_rate=%.1f samp/s "
            "cb[min=%zu max=%zu total_calls=%llu gap_avg=%.2fms gap_max=%.2fms]\n",
            tag, wall,
            (unsigned long long)live_dbg_capture_cb_samples, appended,
            live_written, buffered, pct, rate,
            live_dbg_min_n, live_dbg_max_n,
            (unsigned long long)live_dbg_cb_calls,
            avg_gap, live_dbg_max_gap_ms);
}

static const char *cont_mode_name(sstv_mode_t mode)
{
    switch (mode) {
        case SSTV_MODE_ROBOT8_BW: return "Robot 8";
        case SSTV_MODE_ROBOT36: return "Robot 36";
        case SSTV_MODE_ROBOT72: return "Robot 72";
        case SSTV_MODE_MARTIN_M1: return "Martin M1";
        case SSTV_MODE_MARTIN_M2: return "Martin M2";
        case SSTV_MODE_SCOTTIE_S1: return "Scottie S1";
        case SSTV_MODE_SCOTTIE_S2: return "Scottie S2";
        case SSTV_MODE_SCOTTIE_DX: return "Scottie DX";
        default: return "Unknown";
    }
}

/* Label of the Manual RX mode selection. */
static const char *cont_rx_mode_name(void)
{
    return cont_rx_mode == SSTV_MODE_UNKNOWN ? "Autodetect" : cont_mode_name(cont_rx_mode);
}

/* Nominal image duration for Manual RX.  The capture limit adds five seconds
 * so starting Manual RX slightly before the SSTV transmission does not cut
 * off the last rows.  SSTV_CONT_MAX_SECONDS remains the absolute safety cap. */
static float cont_mode_seconds(sstv_mode_t mode)
{
    switch (mode) {
        case SSTV_MODE_ROBOT8_BW:  return 8.0f;
        case SSTV_MODE_ROBOT36:    return 36.5f;
        case SSTV_MODE_ROBOT72:    return 72.5f;
        case SSTV_MODE_MARTIN_M1:  return 115.0f;
        case SSTV_MODE_MARTIN_M2:  return 59.0f;
        case SSTV_MODE_SCOTTIE_S1: return 111.0f;
        case SSTV_MODE_SCOTTIE_S2: return 72.0f;
        case SSTV_MODE_SCOTTIE_DX: return 270.0f;
        default:                    return (float)SSTV_CONT_MAX_SECONDS - 5.0f;
    }
}

static bool write_sstv_png(const char *path, const uint8_t *rgb, int width, int height)
{
    FILE *fp=fopen(path,"wb"); if(!fp){fprintf(stderr,"SSTV SAVE: fopen %s failed: %s\n",path,strerror(errno));return false;}
    png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop info=png ? png_create_info_struct(png) : NULL;
    if(!png || !info){if(png)png_destroy_write_struct(&png,NULL);fclose(fp);return false;}
    if(setjmp(png_jmpbuf(png))){png_destroy_write_struct(&png,&info);fclose(fp);return false;}
    png_init_io(png,fp);
    png_set_IHDR(png,info,width,height,8,PNG_COLOR_TYPE_RGB,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png,info);
    for(int y=0;y<height;++y) png_write_row(png,(png_bytep)(rgb+(size_t)y*width*3));
    png_write_end(png,NULL); png_destroy_write_struct(&png,&info); fclose(fp);
    fprintf(stderr,"SSTV SAVE: image saved to %s (%dx%d)\n",path,width,height);
    return true;
}

static bool save_sstv_png(const uint8_t *rgb, int width, int height, char *saved_name, size_t saved_name_size)
{
    if (!rgb || width <= 0 || height <= 0) return false;
    if (mkdir(SSTV_SAVE_DIR, 0775) != 0 && errno != EEXIST) {
        fprintf(stderr, "SSTV SAVE: mkdir failed: %s\n", strerror(errno)); return false;
    }
    time_t now=time(NULL); struct tm tm_now;
    if (!localtime_r(&now,&tm_now)) return false;
    char path[160];
    snprintf(path,sizeof(path),SSTV_SAVE_DIR "/sstv-%04d%02d%02d-%02d%02d%02d.png",
             tm_now.tm_year+1900,tm_now.tm_mon+1,tm_now.tm_mday,
             tm_now.tm_hour,tm_now.tm_min,tm_now.tm_sec);
    if (saved_name && saved_name_size > 0) {
        const char *base = strrchr(path, '/');
        snprintf(saved_name, saved_name_size, "%s", base ? base + 1 : path);
    }
    return write_sstv_png(path, rgb, width, height);
}

/* 1KO125: ML denoiser on the received picture (not on the classic result),
 * only if it is noisy.  About 2 s on the X6100: 4 threads at lower priority
 * (they inherit the nice value of this decoder thread), so the UI and the
 * audio keep going.  Returns false if it could not run (then: classic). */
static bool denoise_ml(int rows, bool *cleaned)
{
    *cleaned = false;
    sstv_dnn_t *m = denoise_model();
    if (!m) return false;
    sstv_denoise_stats_t st;
    int noisy = sstv_denoise_measure(test_rgb, test_info.width, rows, &st);
    if (noisy < 0) return false;
    if (!noisy || st.rows < 5) {
        fprintf(stderr, "SSTV DENOISE ML: noise Y %.1f Cb %.1f Cr %.1f: clean, left as it is\n",
                st.sigma_y, st.sigma_cb, st.sigma_cr);
        return true;
    }
    pid_t tid = (pid_t)syscall(SYS_gettid);
    int old = getpriority(PRIO_PROCESS, tid);
    setpriority(PRIO_PROCESS, tid, 10);
    struct timespec t0, t1;
    sstv_dnn_progress = 0;
    denoise_running = DENOISE_ML;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = sstv_dnn_run(m, test_rgb, test_info.width, st.rows, 4);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    denoise_running = DENOISE_OFF;
    setpriority(PRIO_PROCESS, tid, old);
    if (r != 0) {
        fprintf(stderr, "SSTV DENOISE ML: out of memory, classic used\n");
        return false;
    }
    fprintf(stderr, "SSTV DENOISE ML: %dx%d in %.0f ms, noise Y %.1f Cb %.1f Cr %.1f\n", test_info.width, st.rows,
            sstv_dbg_elapsed(&t0, &t1) * 1000.0, st.sigma_y, st.sigma_cb, st.sigma_cr);
    *cleaned = true;
    return true;
}

/* 1KO125: the received picture in test_rgb is saved as it is; with Denoise
 * CLASSIC or ML it is then cleaned in place (that is what the screen shows)
 * and saved again as <name>_dn.png.  Decoder threads only, before test_ready. */
static void save_rx_picture(int rows)
{
    char saved_name[96] = {0};
    if (rows > test_info.height) rows = test_info.height;
    bool saved = save_sstv_png(test_rgb, test_info.width, test_info.height, saved_name, sizeof(saved_name));
    int mode = denoise_mode;
    if (mode != DENOISE_OFF && rows > 0) {
        bool cleaned = true;
        if (mode != DENOISE_ML || !denoise_ml(rows, &cleaned)) {
            struct timespec t0, t1;
            sstv_denoise_stats_t st;
            denoise_running = DENOISE_CLASSIC;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            sstv_denoise(test_rgb, test_info.width, rows, &st);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            denoise_running = DENOISE_OFF;
            fprintf(stderr, "SSTV DENOISE: %dx%d in %.0f ms, noise Y %.1f Cb %.1f Cr %.1f, %d values + %d line pieces repaired\n",
                    test_info.width, rows, sstv_dbg_elapsed(&t0, &t1) * 1000.0, st.sigma_y, st.sigma_cb, st.sigma_cr,
                    st.repaired, st.bands);
            cleaned = true;
        }
        if (saved && cleaned) {
            char path[160];
            size_t l = strlen(saved_name);
            if (l > 4) saved_name[l - 4] = '\0';           /* without ".png" */
            snprintf(path, sizeof(path), SSTV_SAVE_DIR "/%s_dn.png", saved_name);
            write_sstv_png(path, test_rgb, test_info.width, test_info.height);
            strncat(saved_name, ".png", sizeof(saved_name) - strlen(saved_name) - 1);
        }
    }
    if (saved) {
        snprintf(saved_overlay_name, sizeof(saved_overlay_name), "%s", saved_name);
        saved_overlay_pending = true;
    }
}


static float live_goertzel(const float *x, int n, float hz)
{
    int k = (int)(0.5f + (float)n * hz / (float)SSTV_SAMPLE_RATE);
    float w = 2.0f * 3.14159265358979323846f * (float)k / (float)n;
    float coeff = 2.0f * cosf(w), s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; ++i) { s0 = x[i] + coeff*s1 - s2; s2=s1; s1=s0; }
    return s1*s1 + s2*s2 - coeff*s1*s2;
}

/* VIS tones are nominally 1100/1200/1300/1900 Hz.  Real transmitters and
 * SSB tuning can move them a little, so score a small frequency neighbourhood
 * instead of one exact Goertzel bin.  Keep the range well below the 100-Hz
 * spacing of the VIS data tones. */
static float live_tone_power(const float *x, int n, float sec, float dur, int center_hz)
{
    int a=(int)(sec*SSTV_SAMPLE_RATE), cnt=(int)(dur*SSTV_SAMPLE_RATE);
    if (a < 0 || a+cnt > n || cnt < 32) return -1.0f;
    return live_goertzel(x+a,cnt,(float)center_hz);
}

static int live_tone(const float *x, int n, float sec, float dur)
{
    static const int hz[] = {1100,1200,1300,1900};
    float best=-1.0f, second=-1.0f; int tone=0;
    for (unsigned i=0;i<sizeof(hz)/sizeof(hz[0]);++i) {
        float q=live_tone_power(x,n,sec,dur,hz[i]);
        if (q>best) { second=best; best=q; tone=hz[i]; }
        else if (q>second) second=q;
    }
    /* Do not reject a VIS symbol merely because the runner-up is close.
     * With 15-18 ms symbol windows, 1100 and 1300 Hz are intrinsically
     * difficult to separate by a large power ratio on noisy RF.  Frame-level
     * validation (leader + start + parity + stop + supported VIS) provides
     * the rejection against false triggers instead. */
    return tone;
}

static bool live_break_near(const float *x, int n, float center)
{
    for (float d=-0.020f; d<=0.0201f; d+=0.004f)
        if (live_tone(x,n,center+d,0.010f)==1200) return true;
    return false;
}


static int live_tone_offset(const float *x, int n, float sec, float dur, int offset_hz)
{
    static const int hz[] = {1100,1200,1300,1900};
    float best=-1.0f; int tone=0;
    for (unsigned i=0;i<sizeof(hz)/sizeof(hz[0]);++i) {
        float q=live_tone_power(x,n,sec,dur,hz[i]+offset_hz);
        if (q>best) { best=q; tone=hz[i]; }
    }
    return tone;
}

static bool live_break_near_offset(const float *x, int n, float center, int offset_hz)
{
    for (float d=-0.020f; d<=0.0201f; d+=0.004f)
        if (live_tone_offset(x,n,center+d,0.010f,offset_hz)==1200) return true;
    return false;
}


/* V5: estimate the frequency offset from the two long VIS leader tones.
 * The offset is no longer a free search variable.  A real SSTV header must
 * first provide two strong, mutually consistent leader-frequency estimates;
 * only then is that measured offset frozen for break/start/data/parity/stop. */
static bool live_measure_leader(const float *x, int n, float sec, float dur,
                                int *freq_hz, float *quality)
{
    float best=-1.0f, runner=-1.0f;
    int best_hz=0;
    for (int hz=1740; hz<=2060; hz+=10) {
        float q=live_tone_power(x,n,sec,dur,hz);
        if (q>best) { runner=best; best=q; best_hz=hz; }
        else if (q>runner) runner=q;
    }
    if (best <= 0.0f) return false;

    /* Recompute the runner while ignoring the immediate main-lobe bins.
     * This measures whether there is an actual narrow leader tone rather
     * than merely choosing the largest bin in broadband noise. */
    runner=-1.0f;
    for (int hz=1740; hz<=2060; hz+=10) {
        if (abs(hz-best_hz) < 40) continue;
        float q=live_tone_power(x,n,sec,dur,hz);
        if (q>runner) runner=q;
    }
    float qratio = best / (runner > 1.0e-20f ? runner : 1.0e-20f);
    if (freq_hz) *freq_hz=best_hz;
    if (quality) *quality=qratio;
    return qratio >= 1.8f;
}

static const char *live_vis_mode_name(int vis)
{
    /* Standard 7-bit VIS codes for modes we may hear but do not decode yet.
     * Supported modes are included too so diagnostics always have a name. */
    switch (vis) {
        case 0:   return "Robot Color 12";
        case 1: case 2: case 3:   return "Robot B&W 8";
        case 4:   return "Robot Color 24";
        case 5: case 6: case 7:   return "Robot B&W 12";
        case 8:   return "Robot 36";
        case 9: case 10: case 11: return "Robot B&W 24";
        case 12:  return "Robot 72";
        case 13: case 14: case 15:return "Robot B&W 36";

        case 40:  return "Martin M2";
        case 44:  return "Martin M1";

        case 48:  return "Scottie S4";
        case 51:  return "SC2-30";
        case 52:  return "Scottie S3";
        case 55:  return "SC2-180";
        case 56:  return "Scottie S2";
        case 59:  return "SC2-60";          /* 1KO125 */
        case 60:  return "Scottie S1";
        case 63:  return "SC2-120";
        case 76:  return "Scottie DX";

        case 93:  return "PD 50";
        case 94:  return "PD 290";
        case 95:  return "PD 120";
        case 96:  return "PD 180";
        case 97:  return "PD 240";
        case 98:  return "PD 160";
        case 99:  return "PD 90";
        case 100: return "Proskan J120";
        case 104: return "MSCAN TV-1";
        case 105: return "MSCAN TV-2";
        case 113: return "Pasokon P3";
        case 114: return "Pasokon P5";
        case 115: return "Pasokon P7";
        default:  return NULL;
    }
}

static void live_queue_unsupported_vis(int vis, const char *detector)
{
    /* Only report VIS codes that map to a known SSTV mode.
     * A syntactically valid but unassigned/unknown 7-bit value may simply
     * be a false decode caused by noise, so it is ignored silently. */
    const char *name = live_vis_mode_name(vis);
    if (!name)
        return;

    /* 1KO125: not shown at once, see unsupported_vis_tick() */
    if (!unsupported_vis_pending) {
        unsupported_vis_since = lv_tick_get();
        unsupported_vis_starts = live_capture_starts;
    }
    unsupported_vis_code = vis;
    snprintf(unsupported_vis_name, sizeof(unsupported_vis_name), "%s", name);
    unsupported_vis_pending = true;

    fprintf(stderr,
            "SSTV LIVE: valid known but unsupported VIS=%d -> %s (%s detector)\n",
            vis, name, detector ? detector : "VIS");
}

static bool live_supported_vis(int vis, sstv_info_t *out)
{
    memset(out,0,sizeof(*out)); out->vis=vis;
    switch(vis) {
        case 2:
            out->mode=SSTV_MODE_ROBOT8_BW;
            out->name="Robot 8";
            return true;
        case 8:  out->mode=SSTV_MODE_ROBOT36; out->name="Robot 36"; out->width=320; out->height=240; break;
        case 12: out->mode=SSTV_MODE_ROBOT72; out->name="Robot 72"; out->width=320; out->height=240; break;
        case 40: out->mode=SSTV_MODE_MARTIN_M2; out->name="Martin M2"; out->width=320; out->height=256; break;
        case 44: out->mode=SSTV_MODE_MARTIN_M1; out->name="Martin M1"; out->width=320; out->height=256; break;
        case 56: out->mode=SSTV_MODE_SCOTTIE_S2; out->name="Scottie S2"; out->width=320; out->height=256; break;
        case 60: out->mode=SSTV_MODE_SCOTTIE_S1; out->name="Scottie S1"; out->width=320; out->height=256; break;
        case 76: out->mode=SSTV_MODE_SCOTTIE_DX; out->name="Scottie DX"; out->width=320; out->height=256; break;
        default: return false;
    }
    return true;
}

static bool live_find_vis(const float *x, int n, sstv_info_t *out)
{
    const float limit=(float)n/SSTV_SAMPLE_RATE-0.90f;

    /* V3 first pass: preserve the historical 30-ms VIS detector exactly.
     * Signals accepted by the old detector therefore keep priority. */
    for (float t=0.0f; t<limit; t+=0.005f) {
        if (live_tone(x,n,t+0.10f,0.020f)!=1900 ||
            live_tone(x,n,t+0.295f,0.010f)!=1200 ||
            live_tone(x,n,t+0.42f,0.020f)!=1900) continue;
        int vis=0, valid=1;
        for (int i=0;i<7;++i) {
            int f=live_tone(x,n,t+0.640f+i*0.030f+0.005f,0.020f);
            if (f==1100) vis|=1<<i;
            else if (f!=1300) { valid=0; break; }
        }
        if (!valid) continue;
        sstv_info_t candidate;
        if (!live_supported_vis(vis,&candidate)) continue;
        *out=candidate;
        fprintf(stderr,"SSTV LIVE: legacy VIS header found in RF audio\n");
        for (int i=0;i<7;++i)
            fprintf(stderr,"SSTV LIVE: VIS bit %d = %d\n",i,(vis>>i)&1);
        fprintf(stderr,"SSTV LIVE: VIS=%d -> %s (%dx%d)\n",vis,out->name,out->width,out->height);
        return true;
    }

    /* V3 fallback: only after the historical detector has failed, search
     * modest phase and bit-period errors and validate the complete VIS frame. */
    for (float t=0.0f; t<limit; t+=0.005f) {
        if (live_tone(x,n,t+0.10f,0.025f)!=1900 ||
            !live_break_near(x,n,t+0.295f) ||
            live_tone(x,n,t+0.42f,0.025f)!=1900) continue;

        for (float bit=0.028f; bit<=0.0321f; bit+=0.001f) {
            for (float phase=-0.006f; phase<=0.0061f; phase+=0.003f) {
                float start=t+0.610f+phase;
                float win=bit*0.55f;
                float inset=(bit-win)*0.5f;
                if (live_tone(x,n,start+inset,win)!=1200) continue;

                int vis=0, ones=0, valid=1;
                for (int i=0;i<7;++i) {
                    int f=live_tone(x,n,start+(i+1)*bit+inset,win);
                    if (f==1100) { vis|=1<<i; ++ones; }
                    else if (f!=1300) { valid=0; break; }
                }
                if (!valid) continue;
                int parity=live_tone(x,n,start+8*bit+inset,win);
                int expected_parity=(ones&1)?1100:1300;
                if (parity!=expected_parity) continue;
                if (live_tone(x,n,start+9*bit+inset,win)!=1200) continue;

                sstv_info_t candidate;
                if (!live_supported_vis(vis,&candidate)) {
                    live_queue_unsupported_vis(vis, "robust");
                    continue;
                }
                *out=candidate;
                fprintf(stderr,"SSTV LIVE: robust VIS fallback found (bit %.1f ms, phase %+.1f ms)\n",bit*1000.0f,phase*1000.0f);
                for (int i=0;i<7;++i)
                    fprintf(stderr,"SSTV LIVE: VIS bit %d = %d\n",i,(vis>>i)&1);
                fprintf(stderr,"SSTV LIVE: VIS=%d -> %s (%dx%d)\n",vis,out->name,out->width,out->height);
                return true;
            }
        }
    }
    /* V5 final fallback: frequency-offset-aware, but leader-locked.
     * Unlike V4, do NOT sweep offset until some supported VIS happens to fit.
     * Measure it independently from both long 1900-Hz leaders and freeze it. */
    for (float t=0.0f; t<limit; t+=0.005f) {
        int lf1=0, lf2=0; float lq1=0.0f, lq2=0.0f;
        if (!live_measure_leader(x,n,t+0.055f,0.140f,&lf1,&lq1) ||
            !live_measure_leader(x,n,t+0.375f,0.140f,&lf2,&lq2)) continue;
        if (abs(lf1-lf2)>20) continue;
        int off=((lf1+lf2+1)/2)-1900;
        off=(int)lroundf((float)off/10.0f)*10;
        if (off < -150 || off > 150) continue;
        if (off>=-20 && off<=20) continue; /* V3 already owns nominal tuning. */
        if (!live_break_near_offset(x,n,t+0.295f,off)) continue;

        for (float bit=0.028f; bit<=0.0321f; bit+=0.001f) {
            for (float phase=-0.006f; phase<=0.0061f; phase+=0.003f) {
                float start=t+0.610f+phase;
                float win=bit*0.55f, inset=(bit-win)*0.5f;
                if (live_tone_offset(x,n,start+inset,win,off)!=1200) continue;
                int vis=0, ones=0, valid=1;
                for (int i=0;i<7;++i) {
                    int f=live_tone_offset(x,n,start+(i+1)*bit+inset,win,off);
                    if (f==1100) { vis|=1<<i; ++ones; }
                    else if (f!=1300) { valid=0; break; }
                }
                if (!valid) continue;
                int parity=live_tone_offset(x,n,start+8*bit+inset,win,off);
                if (parity != ((ones&1)?1100:1300)) continue;
                if (live_tone_offset(x,n,start+9*bit+inset,win,off)!=1200) continue;
                sstv_info_t candidate;
                if (!live_supported_vis(vis,&candidate)) {
                    live_queue_unsupported_vis(vis, "V5");
                    continue;
                }
                *out=candidate;
                fprintf(stderr,"SSTV LIVE: V5 leader-locked VIS fallback found (leaders %d/%d Hz, offset %+d Hz, quality %.2f/%.2f, bit %.1f ms, phase %+.1f ms)\n",lf1,lf2,off,lq1,lq2,bit*1000.0f,phase*1000.0f);
                for (int i=0;i<7;++i)
                    fprintf(stderr,"SSTV LIVE: VIS bit %d = %d\n",i,(vis>>i)&1);
                fprintf(stderr,"SSTV LIVE: VIS=%d -> %s (%dx%d)\n",vis,out->name,out->width,out->height);
                return true;
            }
        }
    }
    return false;
}

/* Window scheduling for the VIS scans in ui_timer_cb().  Both functions are
 * called with live_mutex held.
 *
 * live_vis_window_ready(): true when the 1-s window ending at *next_end is
 * completely inside live_ring.  If the window is no longer in the ring (ring
 * reset at capture start/end, or the GUI stalled for about a second), the
 * schedule moves forward to the oldest complete window. */
static bool live_vis_window_ready(uint64_t *next_end, const char *who)
{
    uint64_t oldest = live_ring_total - live_ring_count;
    if (*next_end < oldest + SSTV_VIS_WINDOW_SAMPLES) {
        if (*next_end != 0 && live_ring_count == SSTV_PREBUF_SAMPLES)
            fprintf(stderr, "SSTV LIVE: %s VIS scan fell behind, skipped %.2f s of audio\n",
                    who, (double)(oldest + SSTV_VIS_WINDOW_SAMPLES - *next_end) / SSTV_SAMPLE_RATE);
        *next_end = oldest + SSTV_VIS_WINDOW_SAMPLES;
    }
    return *next_end <= live_ring_total;
}

/* Copy the 1-s window ending at absolute sample index end into live_vis_window. */
static void live_vis_copy_window(uint64_t end)
{
    size_t back = (size_t)(live_ring_total - (end - SSTV_VIS_WINDOW_SAMPLES));
    size_t start = (live_ring_pos + SSTV_PREBUF_SAMPLES - back) % SSTV_PREBUF_SAMPLES;
    for (size_t i = 0; i < SSTV_VIS_WINDOW_SAMPLES; ++i)
        live_vis_window[i] = live_ring[(start + i) % SSTV_PREBUF_SAMPLES];
}

/* 1KO125: minimum VIS frame score (sstv/vis.c accepts from 0.50).  Picture
 * content can pass for a header with a score of 0.50..0.57 (a flat area reads
 * as leader, a sync pulse as start bit), real headers score far higher. */
#define SSTV_VIS_MIN_LISTEN       0.65f   /* listening, no picture coming in      */
#define SSTV_VIS_MIN_RESTART      0.80f   /* a new VIS over the picture coming in */
#define SSTV_VIS_MIN_UNSUPPORTED  0.70f   /* "Unsupported SSTV mode" message      */
#define SSTV_VIS_MIN_LEADER_RESTART 0.25f /* ... and its leader (real: >= 0.25 to -6 dB) */
#define SSTV_RESTART_CONFIRM_S    3.0     /* line syncs looked at after a Restart VIS, s */
#define SSTV_VIS_RETRY_SHIFT      (SSTV_SAMPLE_RATE / 40)  /* 25 ms, see live_detect_vis_retry() */

/* VIS detection for Auto RX (initial and restart scans).  min_score: one of
 * SSTV_VIS_MIN_LISTEN / SSTV_VIS_MIN_RESTART. */
static bool live_detect_vis(const float *x, int n, sstv_info_t *out, float min_score)
{
    if (SSTV_USE_LEGACY_VIS) {
        live_vis_last_start_s = -1.0f;      /* position unknown: see live_preview_start() */
        live_vis_last_off = 0.0f;           /* 1KO125: not measured */
        live_vis_last_leader = 1.0f;
        live_vis_last_near = false;
        return live_find_vis(x, n, out);
    }
    sstv_vis_result_t res;
    live_vis_last_start_s = -1.0f;
    bool found = sstv_vis_detect(x, n, out, &res);
    live_vis_last_near = res.near_miss;     /* 1KO125: see live_detect_vis_retry() */
    if (found && res.score < min_score) {
        fprintf(stderr, "SSTV VIS: %s (VIS %d) ignored, score %.2f < %.2f\n",
                out->name, out->vis, res.score, min_score);
        return false;
    }
    if (found) {
        /* A gap inside the header delays its end (and the picture) by
         * slip_s: live_preview_start() uses start + 0.910 s as the end. */
        live_vis_last_start_s = res.start_s + res.slip_s;
        live_vis_last_off = res.offset_hz;      /* 1KO125 */
        live_vis_last_leader = res.leader_q;
        fprintf(stderr, "SSTV VIS: %s (VIS %d) offset %+.1f Hz, score %.2f, leader %.3f, gap %.0f ms\n",
                out->name, out->vis, res.offset_hz, res.score, res.leader_q, res.slip_s * 1000.0f);
        return true;
    }
    if (res.unsupported_vis >= 0) {
        if (res.score >= SSTV_VIS_MIN_UNSUPPORTED)
            live_queue_unsupported_vis(res.unsupported_vis, "VIS");
        else
            fprintf(stderr, "SSTV VIS: unsupported VIS %d ignored, score %.2f < %.2f\n",
                    res.unsupported_vis, res.score, SSTV_VIS_MIN_UNSUPPORTED);
    }
    return false;
}

/* 1KO125: a header near the edge of the 1-s window can be rejected in one
 * window position and taken in the next; with the windows 50 ms apart a weak
 * one was sometimes rejected in both.  When the leaders and the start bit
 * were found but the frame was not taken, the window 25 ms earlier is tried
 * too (rare: almost every window stops at the leader test).  *win_end is
 * moved to the window that found it.  Called without live_mutex. */
static bool live_detect_vis_retry(bool found, uint64_t *win_end, sstv_info_t *out, float min_score)
{
    if (found || !live_vis_last_near) return found;
    uint64_t end = *win_end - SSTV_VIS_RETRY_SHIFT;
    bool have = false;
    pthread_mutex_lock(&live_mutex);
    if (end - SSTV_VIS_WINDOW_SAMPLES >= live_ring_total - live_ring_count && end <= live_ring_total) {
        live_vis_copy_window(end);
        have = true;
    }
    pthread_mutex_unlock(&live_mutex);
    if (!have) return false;
    found = live_detect_vis(live_vis_window, SSTV_VIS_WINDOW_SAMPLES, out, min_score);
    if (found) {
        fprintf(stderr, "SSTV VIS: found again in the window 25 ms earlier\n");
        *win_end = end;
    }
    return found;
}

/* ---- Progressive display (Auto RX) ---------------------------------------
 * live_preview_start(): called with live_mutex held right after a capture
 * was committed in live_samples[0..live_written).  win_end is the absolute
 * sample index (live_ring_total scale) of the end of the 1-s window in which
 * the VIS was found, buf0_abs the absolute index of live_samples[0]. */
static void live_preview_start(uint64_t win_end, uint64_t buf0_abs)
{
    live_buf0_abs = buf0_abs;                   /* 1KO125: see rc_* */
    live_capture_off = live_vis_last_off;
    double row0 = -1.0, off = sstv_row0_anchor_after_vis(live_info.mode);
    if (live_vis_last_start_s >= 0.0f && win_end >= SSTV_VIS_WINDOW_SAMPLES) {
        double vis_abs = (double)(win_end - SSTV_VIS_WINDOW_SAMPLES) + live_vis_last_start_s * SSTV_SAMPLE_RATE;
        double vis_end = vis_abs - (double)buf0_abs + 0.910 * SSTV_SAMPLE_RATE;   /* as sstv_vis_find_end() */
        if (off >= 0.0 && vis_end > 0.0) row0 = vis_end + off * SSTV_SAMPLE_RATE;
    } else if (off >= 0.0) {
        /* legacy detector: no position, search the VIS in the prebuffer */
        long ve = sstv_vis_find_end(live_samples, live_written, live_info.vis, live_written);
        if (ve >= 0) row0 = (double)ve + off * SSTV_SAMPLE_RATE;
    }
    sstv_linesync_init(&live_ls, live_info.mode);
    sstv_linesync_process(&live_ls, live_samples, (size_t)live_written);
    sstv_preview_init(&live_pv, live_info.mode, row0);
    live_pv_on = sstv_row_width(live_info.mode) > 0;
    fprintf(stderr, "SSTV PREVIEW: %s, row 0 at %.3f s of the capture%s\n", live_info.name,
            row0 / SSTV_SAMPLE_RATE, row0 < 0.0 ? " (unknown: from the line syncs)" : "");
}

/* Draw picture row `row` (width x 3 bytes) into the canvas, scaled exactly
 * like render_scaled_image() so the final picture lands on top of it. */
static void live_preview_draw_row(int row, const uint8_t *rgb, int width, int height)
{
    if (!sstv_canvas || width <= 0 || height <= 0) return;
    int out_h = SSTV_IMAGE_H;
    int out_w = (width * out_h) / height;
    if (out_w > SSTV_IMAGE_W) { out_w = SSTV_IMAGE_W; out_h = (height * out_w) / width; }
    int x0 = (SSTV_IMAGE_W - out_w) / 2, y0 = (SSTV_IMAGE_H - out_h) / 2;
    int oy0 = (int)(((int64_t)row * out_h + height - 1) / height);          /* rows with sy == row */
    int oy1 = (int)(((int64_t)(row + 1) * out_h + height - 1) / height);
    for (int oy = oy0; oy < oy1 && oy < out_h; ++oy)
        for (int ox = 0; ox < out_w; ++ox) {
            int sx = (int)((int64_t)ox * width / out_w);
            const uint8_t *q = rgb + (size_t)sx * 3;
            lv_canvas_set_px(sstv_canvas, x0 + ox, y0 + oy, lv_color_make(q[0], q[1], q[2]));
        }
    lv_obj_invalidate(sstv_canvas);
}

/* Called by ui_timer_cb(): draw the rows whose audio has arrived.  The
 * slice is copied under live_mutex (the audio thread may append or a
 * Restart may replace live_samples), the row is demodulated outside it. */
static void live_preview_tick(void)
{
    uint8_t row_rgb[320 * 3];
    for (int k = 0; k < SSTV_PV_ROWS_PER_TICK && live_pv_on; ++k) {
        long a = 0, b = 0;
        bool ok = false;
        pthread_mutex_lock(&live_mutex);
        if (!live_capturing || !live_samples) {
            live_pv_on = false;               /* capture over: the final decode follows */
        } else {
            sstv_ls_status_t st = *sstv_linesync_status(&live_ls);
            if (sstv_preview_need(&live_pv, live_written, &st, &a, &b)) {
                if (b - a <= SSTV_PV_SLICE_MAX) {
                    memcpy(live_pv_slice, live_samples + a, (size_t)(b - a) * sizeof(float));
                    ok = true;
                } else {
                    live_pv_on = false;       /* cannot happen with the supported modes */
                }
            }
        }
        pthread_mutex_unlock(&live_mutex);
        if (!ok) break;
        int r = sstv_preview_render(&live_pv, live_pv_slice, row_rgb);
        if (r >= 0) live_preview_draw_row(r, row_rgb, live_pv.width, live_pv.height);
    }
}

/* Progressive display for Manual RX: same scheme as live_preview_tick()
 * (slice copied under live_mutex, row drawn outside it), on the Manual
 * capture and its tracker.  The final Manual decode replaces it. */
static void cont_preview_tick(void)
{
    uint8_t row_rgb[320 * 3];
    for (int k = 0; k < SSTV_PV_ROWS_PER_TICK && cont_pv_on; ++k) {
        long a = 0, b = 0;
        bool ok = false;
        pthread_mutex_lock(&live_mutex);
        if (!cont_capturing || !cont_samples) {
            cont_pv_on = false;               /* capture over: the final decode follows */
        } else {
            sstv_ls_status_t st = *sstv_linesync_status(&cont_ls);
            if (sstv_preview_need(&cont_pv, cont_written, &st, &a, &b)) {
                if (b - a <= SSTV_PV_SLICE_MAX) {
                    memcpy(live_pv_slice, cont_samples + a, (size_t)(b - a) * sizeof(float));
                    ok = true;
                } else {
                    cont_pv_on = false;
                }
            }
        }
        pthread_mutex_unlock(&live_mutex);
        if (!ok) break;
        int r = sstv_preview_render(&cont_pv, live_pv_slice, row_rgb);
        if (r >= 0) live_preview_draw_row(r, row_rgb, cont_pv.width, cont_pv.height);
    }
}

static float live_mode_seconds(sstv_mode_t m)
{
    switch(m) {
        case SSTV_MODE_ROBOT8_BW: return 8.0f;
        case SSTV_MODE_ROBOT36: return 36.5f;
        case SSTV_MODE_ROBOT72: return 72.5f;
        case SSTV_MODE_MARTIN_M1: return 115.0f;
        case SSTV_MODE_MARTIN_M2: return 59.0f;
        case SSTV_MODE_SCOTTIE_S1: return 111.0f;
        case SSTV_MODE_SCOTTIE_S2: return 72.0f;
        case SSTV_MODE_SCOTTIE_DX: return 270.0f;
        default: return 120.0f;
    }
}

/* 1KO125: start an Auto RX capture of info from src[0..nsrc) (absolute index
 * src_abs of src[0]); its VIS was found in the window ending at win_end,
 * start_s into it, tuning offset off.  live_mutex held; live_samples is not
 * freed here.  false: no memory, nothing changed. */
static bool live_capture_from(const sstv_info_t *info, const float *src, long nsrc, uint64_t src_abs,
                              uint64_t win_end, float start_s, float off)
{
    long target = (long)((live_mode_seconds(info->mode) + 2.0f) * SSTV_SAMPLE_RATE);
    float *buf = malloc((size_t)target * sizeof(float));
    if (!buf) {
        fprintf(stderr, "SSTV LIVE: ERROR allocating %.1f MB capture buffer\n",
                (double)target * sizeof(float) / (1024.0 * 1024.0));
        return false;
    }
    long keep = nsrc < target ? nsrc : target;
    if (keep < 0) keep = 0;
    if (keep) memcpy(buf, src, (size_t)keep * sizeof(float));
    live_info = *info;
    switch (live_info.mode) {
        case SSTV_MODE_ROBOT8_BW:
        case SSTV_MODE_ROBOT36:
        case SSTV_MODE_ROBOT72:
        case SSTV_MODE_MARTIN_M1:
        case SSTV_MODE_MARTIN_M2:
        case SSTV_MODE_SCOTTIE_S1:
        case SSTV_MODE_SCOTTIE_S2:
            cont_mode = live_info.mode;
            live_mode_ui_pending = true;
            break;
        default: break;
    }
    live_samples = buf;
    live_written = keep;
    live_target = target;
    live_capturing = true;
    live_capture_starts++;
    sstv_dbg_capture_start(live_written);
    live_vis_last_start_s = start_s;
    live_vis_last_off = off;
    live_preview_start(win_end, src_abs);
    live_ring_pos = 0;          /* the restart detector starts after this VIS */
    live_ring_count = 0;
    live_canvas_clear_pending = true;
    return true;
}

/* 1KO125: Restart candidates (rc_cand[]).  live_mutex held unless said. */
static int rc_count(void)
{
    int k = 0;
    for (int i = 0; i < RC_MAX; ++i) k += rc_cand[i].used;
    return k;
}

static void rc_clear(const char *why)
{
    for (int i = 0; i < RC_MAX; ++i) {
        if (rc_cand[i].used && why)
            fprintf(stderr, "SSTV LIVE: RESTART candidate %s dropped: %s\n", rc_cand[i].info.name, why);
        rc_cand[i].used = false;
    }
}

/* candidates of an earlier capture */
static void rc_drop_stale(void)
{
    for (int i = 0; i < RC_MAX; ++i)
        if (rc_cand[i].used && rc_cand[i].capture_starts != live_capture_starts) rc_cand[i].used = false;
}

/* A VIS heard during the capture.  The same VIS is found in a few windows
 * in a row: one whose end is within 0.5 s of a candidate's is that one.
 * With RC_MAX candidates already, the oldest one still being checked makes
 * room (more real headers than that in 3 s do not happen). */
static void rc_add(const sstv_info_t *info, uint64_t win_end, float start_s, float off)
{
    double vis_end_abs = (double)(win_end - SSTV_VIS_WINDOW_SAMPLES) + (start_s + 0.910) * SSTV_SAMPLE_RATE;
    rc_drop_stale();
    for (int i = 0; i < RC_MAX; ++i)
        if (rc_cand[i].used && fabs(rc_cand[i].vis_end_abs - vis_end_abs) < 0.5 * SSTV_SAMPLE_RATE) return;
    int slot = -1;
    for (int i = 0; i < RC_MAX && slot < 0; ++i)
        if (!rc_cand[i].used) slot = i;
    if (slot < 0) {
        for (int i = 0; i < RC_MAX; ++i)
            if (!rc_cand[i].confirmed && (slot < 0 || rc_cand[i].vis_end_abs < rc_cand[slot].vis_end_abs)) slot = i;
        if (slot < 0) return;       /* all checked and confirmed: the winner is chosen next tick */
        fprintf(stderr, "SSTV LIVE: RESTART candidate %s dropped: a newer VIS came\n", rc_cand[slot].info.name);
    }
    rc_cand[slot] = (rc_cand_t){ .used = true, .info = *info, .win_end = win_end, .start_s = start_s, .off = off,
                            .vis_end_abs = vis_end_abs, .capture_starts = live_capture_starts };
    fprintf(stderr, "SSTV LIVE: RESTART candidate VIS=%d -> %s at %.1f s of the %s capture; "
            "checking the line syncs for %.1f s\n", info->vis, info->name,
            (double)live_written / SSTV_SAMPLE_RATE, live_info.name, SSTV_RESTART_CONFIRM_S);
}

/* All candidates checked: the newest confirmed one, or -1 (also while some
 * are still being checked). */
static int rc_winner(void)
{
    int w = -1;
    for (int i = 0; i < RC_MAX; ++i) {
        if (!rc_cand[i].used) continue;
        if (!rc_cand[i].confirmed) return -1;
        if (w < 0 || rc_cand[i].vis_end_abs > rc_cand[w].vis_end_abs) w = i;
    }
    return w;
}

/* Line syncs around candidate c.  x[0..n) is the capture (x[0] at absolute
 * index x_abs); span_s of audio after the VIS are used. */
static bool rc_check(const rc_cand_t *c, const float *x, long n, uint64_t x_abs, double span_s,
                     sstv_syncgrid_verdict_t *v)
{
    double vis_end_s = (c->vis_end_abs - (double)x_abs) / SSTV_SAMPLE_RATE;
    bool ok = sstv_syncgrid_restart_check(x, n, vis_end_s, span_s, c->info.mode, c->off,
                                          live_info.mode, live_capture_off, v);
    fprintf(stderr, "SSTV LIVE: RESTART check %s over %s: new syncs %.2f (%d lines), old syncs %s%.2f -> %.2f (%d lines): %s\n",
            c->info.name, live_info.name, v->new_share, v->new_slots,
            v->old_known ? "" : "not found ", v->old_before, v->old_share, v->old_slots,
            ok ? "CONFIRMED" : "rejected");
    return ok;
}

/* Index in the capture where candidate c's new capture starts: the 2 s
 * before the end of its VIS window, as the ring gave before. */
static long rc_from_index(const rc_cand_t *c)
{
    uint64_t from_abs = c->win_end > SSTV_PREBUF_SAMPLES ? c->win_end - SSTV_PREBUF_SAMPLES : 0;
    if (from_abs < live_buf0_abs) from_abs = live_buf0_abs;
    return (long)(from_abs - live_buf0_abs);
}



static bool save_debug_wav(const char *path, const float *samples, long count)
{
    if (!path || !samples || count <= 0) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "SSTV LIVE: ERROR opening debug WAV %s\n", path);
        return false;
    }

    uint32_t data_bytes = (uint32_t)count * 2u;
    uint32_t riff_size = 36u + data_bytes;
    uint32_t sample_rate = SSTV_SAMPLE_RATE;
    uint32_t byte_rate = sample_rate * 2u;
    uint16_t audio_format = 1, channels = 1, block_align = 2, bits = 16;

    fwrite("RIFF", 1, 4, fp); fwrite(&riff_size, 4, 1, fp); fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    uint32_t fmt_size = 16; fwrite(&fmt_size, 4, 1, fp);
    fwrite(&audio_format, 2, 1, fp); fwrite(&channels, 2, 1, fp);
    fwrite(&sample_rate, 4, 1, fp); fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp); fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp); fwrite(&data_bytes, 4, 1, fp);

    int16_t pcm[1024];
    long pos = 0;
    while (pos < count) {
        long n = count - pos;
        if (n > (long)(sizeof(pcm) / sizeof(pcm[0]))) n = (long)(sizeof(pcm) / sizeof(pcm[0]));
        for (long i = 0; i < n; ++i) {
            float v = samples[pos + i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[i] = (int16_t)lrintf(v * 32767.0f);
        }
        if (fwrite(pcm, sizeof(int16_t), (size_t)n, fp) != (size_t)n) {
            fprintf(stderr, "SSTV LIVE: ERROR writing debug WAV %s\n", path);
            fclose(fp);
            return false;
        }
        pos += n;
    }
    fclose(fp);
    fprintf(stderr, "SSTV LIVE: debug WAV saved to %s (%ld samples @ %d Hz)\n",
            path, count, SSTV_SAMPLE_RATE);
    return true;
}

static void *live_decode_thread_cb(void *arg)
{
    (void)arg;
    /* 1KO125: works on dec_samples, its own copy of the capture; Auto RX is
     * already listening for the next VIS (live_samples is the next capture) */
    fprintf(stderr, "SSTV LIVE: decoding captured %s directly from RAM (%ld samples @ %d Hz)\n",
            dec_info.name, dec_written, SSTV_SAMPLE_RATE);
    struct timespec decode_t0, decode_t1;
    clock_gettime(CLOCK_MONOTONIC, &decode_t0);
    if (SSTV_SAVE_DEBUG_WAV)
        save_debug_wav(SSTV_DEBUG_WAV, dec_samples, dec_written);
    int rows = sstv_decode_samples(dec_samples, dec_written, dec_info.mode, test_rgb, &test_info);
    clock_gettime(CLOCK_MONOTONIC, &decode_t1);
    fprintf(stderr, "SSTV DBG: decoder+debug-WAV wall=%.3fs rows=%d\n",
            sstv_dbg_elapsed(&decode_t0, &decode_t1), rows);
    bool aborted = dec_abort;
    test_rows = (!aborted && rows > 0) ? rows : 0;
    if (!aborted && rows > 0)
        save_rx_picture(rows);
    test_ready = !aborted;
    dec_abort = false;
    free(dec_samples); dec_samples = NULL; dec_written = 0;
    test_running = false;
    live_decoding = false;
    fprintf(stderr,"SSTV LIVE: RAM decoder finished, rows=%d\n", rows);
    return NULL;
}

static void *cont_decode_thread_cb(void *arg)
{
    (void)arg;
    /* A-posteriori check of the mode over the whole capture: far more
     * reliable than the few lines the real-time tracker locked on. */
    long evidence = 0;
    /* Autodetect: the mode locked in real time is preferred (it is what was
     * on the air when Start was pressed); forced mode: the user's mode. */
    sstv_mode_t prefer = (cont_rx_mode != SSTV_MODE_UNKNOWN) ? cont_rx_mode
                       : (cont_locked ? cont_detected : SSTV_MODE_UNKNOWN);
    sstv_mode_t cls = sstv_linesync_classify(cont_samples, cont_written, prefer, &evidence);
    cont_class_mode = cls;
    fprintf(stderr, "SSTV CONT: whole-capture classification: %s (%ld line intervals), real-time tracker: %s\n",
            cont_mode_name(cls), evidence, cont_locked ? cont_mode_name(cont_detected) : "no lock");
    if (cont_rx_mode == SSTV_MODE_UNKNOWN) {
        if (cls != SSTV_MODE_UNKNOWN) cont_decode_mode = cls;   /* Autodetect */
    } else {
        if (!cont_locked) cont_event = SSTV_CONT_EV_NO_LOCK;
        else if (cls != SSTV_MODE_UNKNOWN && cls != cont_rx_mode) cont_event = SSTV_CONT_EV_MISMATCH;
    }
    int rows = 0;
    if (cont_decode_mode == SSTV_MODE_UNKNOWN) {
        fprintf(stderr, "SSTV CONT: no SSTV line structure in the capture; nothing to decode\n");
        cont_event = SSTV_CONT_EV_NO_SIGNAL;
        cont_no_signal = true;
        rows = -1;
    } else {
        fprintf(stderr, "SSTV CONT: decoding manual %s capture from RAM (%ld samples @ %d Hz)\n",
                cont_mode_name(cont_decode_mode), cont_written, SSTV_SAMPLE_RATE);
        rows = sstv_decode_cont_samples(cont_samples, cont_written, cont_decode_mode,
                                        test_rgb, &test_info);
    }
    test_rows = rows > 0 ? rows : 0;
    if (rows > 0)
        save_rx_picture(rows);
    test_ready = true;
    free(cont_samples); cont_samples = NULL;
    cont_written = cont_capacity = 0;
    test_running = false;
    cont_decoding = false;
    fprintf(stderr, "SSTV CONT: decoder finished, rows=%d; receiver returned to stand-by\n", rows);
    return NULL;
}

static void audio_cb(size_t n, float *samples)
{
    if (!samples || n == 0) return;
    /* While a decoder thread runs, audio is ignored - except when a capture
     * is in progress: after a Manual -> Auto hand-over (VIS during Manual RX)
     * the Manual picture is decoded while Auto RX captures the new one. */
    if (test_running && !live_capturing && !cont_capturing && !live_enabled) return;   /* 1KO125: Auto RX listens while decoding */

    /* AUTODETECT is deliberately independent from Auto RX / Manual RX.
     * It passively inspects the same 12-kHz RX stream and never starts a decoder. */
    if (sstv_autodetect_is_listening())
        sstv_autodetect_process(samples, n);

    if (!live_enabled && !cont_capturing) return;
    pthread_mutex_lock(&live_mutex);

    /* Diagnostic: actual callback cadence/block size. */
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        live_dbg_cb_calls++;
        live_dbg_cb_samples += n;
        if (live_dbg_min_n == 0 || n < live_dbg_min_n) live_dbg_min_n = n;
        if (n > live_dbg_max_n) live_dbg_max_n = n;
        if (live_dbg_have_last_cb) {
            double gap_ms = sstv_dbg_elapsed(&live_dbg_last_cb_ts, &now) * 1000.0;
            live_dbg_sum_gap_ms += gap_ms;
            live_dbg_gap_count++;
            if (gap_ms > live_dbg_max_gap_ms) live_dbg_max_gap_ms = gap_ms;
        }
        live_dbg_last_cb_ts = now;
        live_dbg_have_last_cb = true;
    }

    if (cont_capturing) {
        long room=cont_capacity-cont_written;
        long copy=(long)n < room ? (long)n : room;
        if (copy>0) {
            memcpy(cont_samples+cont_written,samples,(size_t)copy*sizeof(float));
            cont_written += copy;
            sstv_linesync_process(&cont_ls, samples, (size_t)copy);
        }
        /* Keep the VIS ring current: ui_timer_cb() scans it, and a VIS ends
         * the Manual capture (hand-over to Auto RX). */
        for (size_t i=0;i<n;++i) {
            live_ring[live_ring_pos]=samples[i];
            live_ring_pos=(live_ring_pos+1)%SSTV_PREBUF_SAMPLES;
            if (live_ring_count<SSTV_PREBUF_SAMPLES) ++live_ring_count;
        }
        live_ring_total += n;

        const sstv_ls_status_t *ls = sstv_linesync_status(&cont_ls);
        if (!cont_locked && ls->state != SSTV_LS_SEARCH) {
            cont_locked = true;
            cont_detected = ls->mode;
            if (cont_rx_mode == SSTV_MODE_UNKNOWN) {
                /* Autodetect: limit = full picture duration from Start */
                long lim = (long)((sstv_linesync_picture_seconds(ls->mode) + 5.0) * SSTV_SAMPLE_RATE);
                if (lim < cont_limit) cont_limit = lim;
            }
            cont_event = SSTV_CONT_EV_LOCK;
            fprintf(stderr,"SSTV CONT: line sync locked: %s after %.1f s, limit %.1f s\n",
                    cont_mode_name(ls->mode),(double)cont_written/SSTV_SAMPLE_RATE,
                    (double)cont_limit/SSTV_SAMPLE_RATE);
        }
        if (cont_rx_mode == SSTV_MODE_UNKNOWN && !cont_locked && !cont_timeout_checked &&
            cont_written >= (long)SSTV_CONT_DETECT_SECONDS * SSTV_SAMPLE_RATE) {
            cont_timeout_checked = true;         /* the check itself runs in ui_timer_cb() */
            cont_event = SSTV_CONT_EV_DETECT_TIMEOUT;
        }
        sstv_cont_event_t end = SSTV_CONT_EV_NONE;
        if (ls->state == SSTV_LS_LOST) end = SSTV_CONT_EV_END_SYNC;
        else if (cont_written >= cont_limit) end = SSTV_CONT_EV_END_LIMIT;
        if (end != SSTV_CONT_EV_NONE) {
            cont_capturing=false;
            cont_decode_mode=(cont_rx_mode == SSTV_MODE_UNKNOWN) ? cont_detected : cont_rx_mode;
            cont_decode_pending=true;
            cont_end_reason=end;
            cont_event=end;
            fprintf(stderr,"SSTV CONT: %s after %.1f s; decoding %s\n",
                    end == SSTV_CONT_EV_END_SYNC ? "end of picture (line syncs lost)" : "time limit reached",
                    (double)cont_written/SSTV_SAMPLE_RATE,
                    cont_decode_mode == SSTV_MODE_UNKNOWN ? "(mode to be classified)" : cont_mode_name(cont_decode_mode));
        }
        pthread_mutex_unlock(&live_mutex);
        return;
    }

    if (!live_enabled) { pthread_mutex_unlock(&live_mutex); return; }

    if (live_capturing) {
        long room=live_target-live_written;
        long copy=(long)n < room ? (long)n : room;
        if (copy>0) {
            memcpy(live_samples+live_written,samples,(size_t)copy*sizeof(float));
            live_written+=copy;
            sstv_linesync_process(&live_ls, samples, (size_t)copy);   /* progressive display */
            live_dbg_capture_cb_samples += (uint64_t)copy;
        }
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double wall = sstv_dbg_elapsed(&live_dbg_capture_ts, &now);
            if (wall >= (double)live_dbg_next_report_sec) {
                sstv_dbg_capture_report("RUN");
                while (wall >= (double)live_dbg_next_report_sec)
                    live_dbg_next_report_sec += 5;
            }
        }
        static int last_sec=-1;
        int sec=(int)(live_written/SSTV_SAMPLE_RATE);
        if (sec/10 != last_sec/10) {
            last_sec=sec;
            fprintf(stderr,"SSTV LIVE: receiving %s into RAM... %d / %.0f s\n",live_info.name,sec,(double)live_target/SSTV_SAMPLE_RATE);
        }
        if (live_written >= live_target) {
            sstv_dbg_capture_report("TARGET");
            live_capturing=false; live_decode_pending=true; live_enabled=false;
            fprintf(stderr,"SSTV LIVE: expected %s duration reached; RAM capture complete (%ld samples)\n",
                    live_info.name,live_written);
            pthread_mutex_unlock(&live_mutex); return;
        }

        /* Historical behaviour ends here.  With RESTART OFF we return exactly
         * as before; the parallel VIS detector below is completely bypassed. */
        if (!live_restart_enabled) {
            pthread_mutex_unlock(&live_mutex); return;
        }

        /* RESTART ON: keep the rolling detector buffer current, but NEVER run
         * live_find_vis() here.  This is the real-time audio callback: the old
         * synchronous scan starved the 12-kHz stream and created a large audio
         * backlog.  ui_timer_cb() performs the expensive scan from a snapshot. */
        for (size_t i=0;i<n;++i) {
            live_ring[live_ring_pos]=samples[i];
            live_ring_pos=(live_ring_pos+1)%SSTV_PREBUF_SAMPLES;
            if (live_ring_count<SSTV_PREBUF_SAMPLES) ++live_ring_count;
        }
        live_ring_total += n;
        pthread_mutex_unlock(&live_mutex); return;
    }

    for (unsigned int i=0;i<n;++i) {
        live_ring[live_ring_pos]=samples[i];
        live_ring_pos=(live_ring_pos+1)%SSTV_PREBUF_SAMPLES;
        if (live_ring_count<SSTV_PREBUF_SAMPLES) ++live_ring_count;
    }
    live_ring_total += n;
    live_wait_samples += n;
    if (live_wait_samples - live_wait_log_mark >= (uint64_t)SSTV_SAMPLE_RATE * 60u) {
        live_wait_log_mark = live_wait_samples;
        fprintf(stderr,"SSTV LIVE: still listening - %llu s of RX audio checked, no valid VIS yet\n",
                (unsigned long long)(live_wait_samples/SSTV_SAMPLE_RATE));
    }
    pthread_mutex_unlock(&live_mutex);
}
