/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */

/*
 * 1KO125 WEFAX: full-screen view of a saved fax (included by dialog_wefax.c).
 *
 * From the Fax Browser (FAX 3:4), F2 Full Screen shows the fax open in the
 * browser (or the one selected in the list) over the whole screen, status
 * bar and buttons included:
 *   MFK turned    scroll sideways
 *   VOL turned    scroll up and down
 *   MFK pressed   next size: 1:1, Medium, Width, Page (then 1:1 again)
 *   F4            Original / Processed, when NAME_pp.png exists
 *   F5            close: back to the browser
 * The Align and Tilt set in the browser are applied.  Below 1:1 each pixel
 * of the screen is the mean of the pixels of the fax it covers, so thin
 * lines stay visible.
 */

#define FS_W            800
#define FS_H            480
#define FS_ZOOMS          4
#define FS_STEP_X       120     /* screen pixels per MFK click */
#define FS_STEP_Y        80     /* screen pixels per VOL click */
#define FS_CHIP_MS     2500

static lv_obj_t *fs_win = NULL;
static lv_obj_t *fs_canvas = NULL;
static lv_obj_t *fs_chip = NULL;
static lv_obj_t *fs_key4 = NULL;
static lv_obj_t *fs_help = NULL;        /* how to move: until MFK or VOL is used */
static lv_color_t *fs_buf = NULL;
static uint8_t *fs_row = NULL;          /* one aligned row of the fax */
static uint8_t *fs_pp = NULL;           /* NAME_pp.png, if any */
static unsigned int fs_pp_w = 0, fs_pp_h = 0;
static bool fs_show_pp = false;
static float fs_scale[FS_ZOOMS];
static const char *fs_zoom_name[FS_ZOOMS];
static int fs_nzoom = 0, fs_zoom = 0;
static float fs_cx = 0, fs_cy = 0;      /* centre of the view, pixels of the fax */
static lv_timer_t *fs_chip_timer = NULL;
static buttons_page_t *fs_prev_page = NULL;
static lv_obj_t *fs_prev_focus = NULL;
static bool fs_prev_editing = false;

static void fs_switch_cb(button_data_t *d);
static void fs_close_cb(button_data_t *d) { (void)d; fs_close(); }

static button_data_t fs_btn_title = { .type = BTN_TEXT, .label = "", .press = NULL };
static button_data_t fs_btn_switch = { .type = BTN_TEXT, .label = "Processed", .press = fs_switch_cb };
static button_data_t fs_btn_close = { .type = BTN_TEXT, .label = "Close", .press = fs_close_cb };
static buttons_page_t fs_page = { .items = { &fs_btn_title, NULL, NULL, NULL, &fs_btn_close } };

/* ---- the cleaned version of a fax ------------------------------------------------ */

/* path of NAME_pp.png for NAME.png (full path or name); false if too long */
static bool fs_pp_path(const char *png, char *out, size_t n)
{
    const char *dot = strrchr(png, '.');
    if (!dot) return false;
    const int len = (int)(dot - png);
    return snprintf(out, n, "%.*s_pp.png", len, png) < (int)n;
}

static bool fax_browser_has_pp(const char *name)
{
    char path[300], full[256];
    snprintf(full, sizeof(full), "%s/%s", FAX_SAVE_DIR, name);
    return fs_pp_path(full, path, sizeof(path)) && access(path, F_OK) == 0;
}

/* an 8-bit gray copy of a PNG; NULL if it cannot be read */
static uint8_t *fs_load_png(const char *path, unsigned int *w, unsigned int *h)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    uint8_t *img = NULL;
    if (!png || !info) goto fail;
    if (setjmp(png_jmpbuf(png))) {
        free(img);
        img = NULL;
        goto fail;
    }
    png_init_io(png, fp);
    png_read_info(png, info);
    const png_uint_32 pw = png_get_image_width(png, info), ph = png_get_image_height(png, info);
    if (pw < 8 || ph < 8 || pw > 4096 || ph > 8192) goto fail;
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_strip_alpha(png);
    const int ct = png_get_color_type(png, info);
    if (ct == PNG_COLOR_TYPE_RGB || ct == PNG_COLOR_TYPE_RGB_ALPHA || ct == PNG_COLOR_TYPE_PALETTE)
        png_set_rgb_to_gray_fixed(png, 1, -1, -1);
    png_read_update_info(png, info);
    if (png_get_channels(png, info) != 1 || png_get_rowbytes(png, info) != pw) goto fail;
    img = malloc((size_t)pw * ph);
    if (!img) goto fail;
    for (png_uint_32 y = 0; y < ph; y++) png_read_row(png, img + (size_t)y * pw, NULL);
    png_read_end(png, NULL);
    *w = pw;
    *h = ph;
fail:
    if (png) png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    fclose(fp);
    return img;
}

/* Save Fax in the browser: the Align and Tilt just saved in NAME.png are
 * applied to NAME_pp.png too, so the two versions stay the same picture */
static void fax_browser_save_pp_too(void)
{
    char path[300], tmp[310];
    if (!fs_pp_path(fax_browser_image_path, path, sizeof(path)) || access(path, F_OK) != 0) return;
    if (fax_browser_align == 0 && fax_browser_tilt == 0) return;
    unsigned int w, h;
    uint8_t *img = fs_load_png(path, &w, &h);
    if (!img) return;
    uint8_t *out = malloc((size_t)w * h);
    if (!out) {
        free(img);
        return;
    }
    for (unsigned int y = 0; y < h; y++) {
        /* as fax_browser_save_edited_png() */
        const int64_t row_from_newest = (int64_t)y - (int64_t)(h - 1);
        const int tilt_shift =
            (int)((row_from_newest * (int64_t)fax_browser_tilt * (int64_t)PREVIEW_WIDTH) / (1000LL * (int64_t)w));
        const uint8_t *src = img + (size_t)y * w;
        uint8_t *dst = out + (size_t)y * w;
        for (unsigned int x = 0; x < w; x++) {
            int sx = (int)x - fax_browser_align - tilt_shift;
            while (sx < 0) sx += (int)w;
            while (sx >= (int)w) sx -= (int)w;
            dst[x] = src[sx];
        }
    }
    free(img);

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    volatile bool ok = false;           /* set after setjmp() */
    FILE *fp = fopen(tmp, "wb");
    png_structp png = fp ? png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL) : NULL;
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (png && info && !setjmp(png_jmpbuf(png))) {
        png_init_io(png, fp);
        png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                     PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (unsigned int y = 0; y < h; y++) png_write_row(png, (png_bytep)(out + (size_t)y * w));
        png_write_end(png, NULL);
        ok = true;
    }
    if (png) png_destroy_write_struct(&png, info ? &info : NULL);
    if (fp && fclose(fp) != 0) ok = false;
    free(out);
    if (ok && rename(tmp, path) == 0) {
        printf("WEFAX BROWSER SAVE: saved %s too, align=%d tilt=%d\n", path, fax_browser_align, fax_browser_tilt);
    } else {
        remove(tmp);
        printf("WEFAX BROWSER SAVE: %s not updated\n", path);
    }
}

/* ---- drawing --------------------------------------------------------------------- */

static void fs_source(const uint8_t **img, unsigned int *w, unsigned int *h)
{
    if (fs_show_pp && fs_pp) {
        *img = fs_pp;
        *w = fs_pp_w;
        *h = fs_pp_h;
    } else {
        *img = fax_browser_image;
        *w = fax_browser_image_width;
        *h = fax_browser_image_height;
    }
}

/* the sizes for this fax: 1:1, Medium, Width (the line fills the screen),
 * Page (the whole fax), largest first, without two almost equal */
static void fs_make_zooms(unsigned int w, unsigned int h)
{
    const float width = (float)FS_W / (float)w;
    float page = (float)FS_H / (float)h;
    if (width < page) page = width;
    const float s[FS_ZOOMS] = { 1.0f, 0.7f, width, page };
    static const char *const n[FS_ZOOMS] = { "1:1", "Medium", "Width", "Page" };
    fs_nzoom = 0;
    for (int i = 0; i < FS_ZOOMS; i++) {
        if (s[i] > 1.0f) continue;
        if (fs_nzoom && s[i] > fs_scale[fs_nzoom - 1] * 0.9f) continue;
        fs_scale[fs_nzoom] = s[i];
        fs_zoom_name[fs_nzoom] = n[i];
        fs_nzoom++;
    }
}

static void fs_clamp(unsigned int w, unsigned int h)
{
    const float s = fs_scale[fs_zoom];
    const float vw = FS_W / s, vh = FS_H / s;
    if (vw >= w) fs_cx = w / 2.0f;
    else if (fs_cx < vw / 2) fs_cx = vw / 2;
    else if (fs_cx > w - vw / 2) fs_cx = w - vw / 2;
    if (vh >= h) fs_cy = h / 2.0f;
    else if (fs_cy < vh / 2) fs_cy = vh / 2;
    else if (fs_cy > h - vh / 2) fs_cy = h - vh / 2;
}

/* row sy of the fax with the browser's Align and Tilt (as
 * fax_browser_render_image()) */
static void fs_aligned_row(const uint8_t *img, unsigned int w, unsigned int h, unsigned int sy)
{
    const int64_t row_from_newest = (int64_t)sy - (int64_t)(h - 1);
    const int tilt_shift =
        (int)((row_from_newest * (int64_t)fax_browser_tilt * (int64_t)PREVIEW_WIDTH) / (1000LL * (int64_t)w));
    /* out[x] = src[(x - align - tilt_shift) mod w] */
    int k = -(fax_browser_align + tilt_shift);
    k %= (int)w;
    if (k < 0) k += (int)w;
    const uint8_t *src = img + (size_t)sy * w;
    memcpy(fs_row, src + k, w - (unsigned int)k);
    memcpy(fs_row + (w - (unsigned int)k), src, (unsigned int)k);
}

static void fs_render(void)
{
    const uint8_t *img;
    unsigned int w, h;
    fs_source(&img, &w, &h);
    if (!fs_buf || !img) return;
    fs_clamp(w, h);

    const float s = fs_scale[fs_zoom];
    int cw = (int)(w * s + 0.5f), ch = (int)(h * s + 0.5f);
    if (cw > FS_W) cw = FS_W;
    if (ch > FS_H) ch = FS_H;
    const int ox = (FS_W - cw) / 2, oy = (FS_H - ch) / 2;
    const float x0 = fs_cx - (cw / s) / 2, y0 = fs_cy - (ch / s) / 2;

    for (int i = 0; i < FS_W * FS_H; i++) fs_buf[i] = lv_color_black();

    /* the pixels of the fax under each column of the screen */
    static int ca[FS_W], cb[FS_W];
    for (int X = 0; X < cw; X++) {
        int a = (int)(x0 + X / s), b = (int)(x0 + (X + 1) / s);
        if (a < 0) a = 0;
        if (a >= (int)w) a = (int)w - 1;
        if (b <= a) b = a + 1;
        if (b > (int)w) b = (int)w;
        ca[X] = a;
        cb[X] = b;
    }
    static uint32_t acc[FS_W];
    for (int Y = 0; Y < ch; Y++) {
        int a = (int)(y0 + Y / s), b = (int)(y0 + (Y + 1) / s);
        if (a < 0) a = 0;
        if (a >= (int)h) a = (int)h - 1;
        if (b <= a) b = a + 1;
        if (b > (int)h) b = (int)h;
        memset(acc, 0, sizeof(uint32_t) * cw);
        for (int sy = a; sy < b; sy++) {
            fs_aligned_row(img, w, h, (unsigned int)sy);
            for (int X = 0; X < cw; X++)
                for (int x = ca[X]; x < cb[X]; x++) acc[X] += fs_row[x];
        }
        lv_color_t *out = fs_buf + (size_t)(oy + Y) * FS_W + ox;
        for (int X = 0; X < cw; X++) {
            const uint32_t n = (uint32_t)(b - a) * (uint32_t)(cb[X] - ca[X]);
            const uint8_t g = (uint8_t)((acc[X] + n / 2) / n);
            out[X] = lv_color_make(g, g, g);
        }
    }
    if (fs_canvas) lv_obj_invalidate(fs_canvas);
}

/* ---- the chips: size and version at the top, F4 / F5 at the bottom ---------------- */

static lv_obj_t *fs_make_chip(lv_coord_t w)
{
    lv_obj_t *c = lv_label_create(fs_win);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_70, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 4, 0);
    lv_obj_set_style_text_color(c, lv_color_white(), 0);
    lv_obj_set_style_text_font(c, &sony_22, 0);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    if (w > 0) lv_obj_set_width(c, w);
    return c;
}

static void fs_chip_hide_cb(lv_timer_t *t)
{
    (void)t;
    fs_chip_timer = NULL;
    if (fs_chip) lv_obj_add_flag(fs_chip, LV_OBJ_FLAG_HIDDEN);
}

static void fs_show_chip(void)
{
    if (!fs_chip) return;
    lv_label_set_text_fmt(fs_chip, "%s  %s", fs_show_pp ? "Processed" : "Original", fs_zoom_name[fs_zoom]);
    lv_obj_clear_flag(fs_chip, LV_OBJ_FLAG_HIDDEN);
    if (fs_chip_timer) lv_timer_del(fs_chip_timer);
    fs_chip_timer = lv_timer_create(fs_chip_hide_cb, FS_CHIP_MS, NULL);
    lv_timer_set_repeat_count(fs_chip_timer, 1);
}

static void fs_update_key4(void)
{
    const char *l = fs_show_pp ? "Original" : "Processed";
    fs_btn_switch.label = l;
    if (fs_key4) lv_label_set_text(fs_key4, l);
}

/* ---- keys ------------------------------------------------------------------------ */

/* the help goes at the first use of MFK or VOL */
static void fs_hide_help(void)
{
    if (fs_help) {
        lv_obj_del(fs_help);
        fs_help = NULL;
    }
}

static void fs_key_cb(lv_event_t *e)
{
    const uint32_t key = *((uint32_t *)lv_event_get_param(e));
    const float s = fs_scale[fs_zoom];
    switch (key) {
    case LV_KEY_LEFT:           fs_cx -= FS_STEP_X / s; break;
    case LV_KEY_RIGHT:          fs_cx += FS_STEP_X / s; break;
    case KEY_VOL_LEFT_EDIT:
    case KEY_VOL_LEFT_SELECT:   fs_cy -= FS_STEP_Y / s; break;
    case KEY_VOL_RIGHT_EDIT:
    case KEY_VOL_RIGHT_SELECT:  fs_cy += FS_STEP_Y / s; break;
    case LV_KEY_ESC:            fs_close(); return;
    default:                    return;
    }
    fs_hide_help();
    fs_render();
}

/* MFK pressed: next size, around the same centre */
static void fs_pressed_cb(lv_event_t *e)
{
    (void)e;
    fs_hide_help();
    fs_zoom = (fs_zoom + 1) % fs_nzoom;
    fs_render();
    fs_show_chip();
}

static void fs_switch_cb(button_data_t *d)
{
    (void)d;
    if (!fs_pp) return;
    fs_show_pp = !fs_show_pp;
    fs_update_key4();
    fs_render();
    fs_show_chip();
}

/* ---- open / close ---------------------------------------------------------------- */

static void fs_destroy(void)
{
    if (fs_chip_timer) {
        lv_timer_del(fs_chip_timer);
        fs_chip_timer = NULL;
    }
    if (fs_win) {
        if (lv_obj_is_valid(fs_win)) lv_obj_del(fs_win);
        fs_win = NULL;
    }
    fs_canvas = NULL;
    fs_chip = NULL;
    fs_key4 = NULL;
    fs_help = NULL;                     /* deleted with fs_win */
    free(fs_buf);
    fs_buf = NULL;
    free(fs_row);
    fs_row = NULL;
    free(fs_pp);
    fs_pp = NULL;
    fs_pp_w = fs_pp_h = 0;
}

/* the fax open in the browser, full screen */
static void fs_open(void)
{
    if (fs_win || !fax_browser_image || fax_browser_image_width < 8 || fax_browser_image_height < 8) return;

    fs_buf = malloc(sizeof(lv_color_t) * FS_W * FS_H);
    fs_row = malloc(4096);
    if (!fs_buf || !fs_row || fax_browser_image_width > 4096) {
        fs_destroy();
        msg_schedule_text_fmt("Full Screen: out of memory");
        return;
    }
    char pp[300];
    fs_pp = NULL;
    if (fs_pp_path(fax_browser_image_path, pp, sizeof(pp)) && access(pp, F_OK) == 0) {
        fs_pp = fs_load_png(pp, &fs_pp_w, &fs_pp_h);
        if (fs_pp && fs_pp_w > 4096) {
            free(fs_pp);
            fs_pp = NULL;
        }
    }
    fs_show_pp = false;

    fs_make_zooms(fax_browser_image_width, fax_browser_image_height);
    fs_zoom = 0;
    for (int i = 0; i < fs_nzoom; i++)
        if (!strcmp(fs_zoom_name[i], "Width")) fs_zoom = i;     /* start: the line fills the screen */
    fs_cx = fax_browser_image_width / 2.0f;
    fs_cy = 0;                                                  /* the top of the fax */

    fs_prev_page = buttons_get_cur_page();
    fs_prev_focus = lv_group_get_focused(keyboard_group);
    fs_prev_editing = lv_group_get_editing(keyboard_group);
    if (fs_prev_focus) lv_group_remove_obj(fs_prev_focus);

    /* over everything: status bar and buttons too */
    fs_win = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(fs_win);
    lv_obj_set_size(fs_win, FS_W, FS_H);
    lv_obj_set_pos(fs_win, 0, 0);
    lv_obj_set_style_bg_color(fs_win, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fs_win, LV_OPA_COVER, 0);
    lv_obj_clear_flag(fs_win, LV_OBJ_FLAG_SCROLLABLE);

    fs_canvas = lv_canvas_create(fs_win);
    lv_canvas_set_buffer(fs_canvas, fs_buf, FS_W, FS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(fs_canvas, 0, 0);

    fs_chip = fs_make_chip(0);
    lv_obj_align(fs_chip, LV_ALIGN_TOP_LEFT, 8, 8);

    /* the two keys that work, where F4 and F5 are */
    lv_obj_t *k5 = fs_make_chip(FS_W / 5 - 12);
    lv_label_set_text(k5, "Close");
    lv_obj_align(k5, LV_ALIGN_BOTTOM_LEFT, 4 * FS_W / 5 + 6, -6);
    if (fs_pp) {
        fs_key4 = fs_make_chip(FS_W / 5 - 12);
        lv_obj_align(fs_key4, LV_ALIGN_BOTTOM_LEFT, 3 * FS_W / 5 + 6, -6);
        fs_update_key4();
    }

    lv_obj_add_event_cb(fs_canvas, fs_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(fs_canvas, fs_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_group_add_obj(keyboard_group, fs_canvas);
    lv_group_focus_obj(fs_canvas);
    lv_group_set_editing(keyboard_group, true);

    fs_page.items[3] = fs_pp ? &fs_btn_switch : NULL;
    buttons_load_page(&fs_page);

    /* how to move, at the top, until MFK or VOL is used (the chip of
     * the size shows from the first change on) */
    fs_help = fs_make_chip(FS_W - 120);
    lv_label_set_text(fs_help, "Turn MFK: scroll sideways    Turn VOL: scroll up and down\n"
                               "Press MFK: change size");
    lv_obj_align(fs_help, LV_ALIGN_TOP_MID, 0, 10);

    fs_render();
}

/* F5: back to the browser as it was */
static void fs_close(void)
{
    if (!fs_win) return;
    fs_destroy();
    if (fs_prev_focus && lv_obj_is_valid(fs_prev_focus)) {
        lv_group_add_obj(keyboard_group, fs_prev_focus);
        lv_group_focus_obj(fs_prev_focus);
        lv_group_set_editing(keyboard_group, fs_prev_editing);
    }
    fs_prev_focus = NULL;
    if (fs_prev_page) buttons_load_page(fs_prev_page);
    fs_prev_page = NULL;
}

/* WEFAX closes: nothing to restore */
static void fs_abort(void)
{
    fs_destroy();
    fs_prev_focus = NULL;
    fs_prev_page = NULL;
}

/* ---- FAX 3:4 F1 and F2 ----------------------------------------------------------- */

/* F1: page 4, or Close Browser while the browser is open */
static void fax_page3_f1_cb(button_data_t *data)
{
    if (fax_browser_open) {
        fax_browser_cb(data);
        return;
    }
    fax_page_4_cb(data);
}

/* F2: Fax Browser, or Full Screen while the browser is open (the fax open
 * in it, or the one selected in the list) */
static void fax_browser_f2_cb(button_data_t *data)
{
    if (!fax_browser_open) {
        fax_browser_cb(data);
        return;
    }
    if (!fax_browser_editing) {
        if (!fax_browser_table || fax_browser_file_count == 0) return;
        const int row = ui_list_get_selected(fax_browser_table);
        if (row < 0 || (unsigned int)row >= fax_browser_file_count) return;
        if (!fax_browser_open_png(fax_browser_files[row])) return;
    }
    fs_open();
}
