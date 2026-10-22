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
 * Saved-pictures browser (RX Pics Browser).
 *
 * This file is NOT a separate compilation unit: it is #included by
 * dialog_sstv.c and must not be added to CMakeLists.txt.  All SSTV dialog
 * code still forms ONE translation unit, so every "static" keeps its
 * original meaning and no symbol is exported.
 */

/* Saved-SSTV browser.  It is deliberately independent from the live SSTV
 * canvas/state machine so reception and decoding continue while it is open. */
static lv_obj_t *sstv_browser_table = NULL;
static lv_obj_t *sstv_browser_canvas = NULL;
static lv_color_t *sstv_browser_buffer = NULL;
static char **sstv_browser_files = NULL;
static size_t sstv_browser_file_capacity = 0;
static unsigned int sstv_browser_file_count = 0;
static bool sstv_browser_progress_was_hidden = true;
static uint8_t *sstv_browser_image = NULL;
static unsigned int sstv_browser_image_width = 0;
static unsigned int sstv_browser_image_height = 0;

static int sstv_browser_name_compare(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sb, *sa);   /* newest timestamped filename first */
}

static void sstv_browser_free_files(void)
{
    for (unsigned int i = 0; i < sstv_browser_file_count; ++i)
        free(sstv_browser_files[i]);

    free(sstv_browser_files);
    sstv_browser_files = NULL;
    sstv_browser_file_count = 0;
    sstv_browser_file_capacity = 0;
}

static void sstv_browser_refresh(void)
{
    sstv_browser_free_files();

    DIR *dir = opendir(SSTV_SAVE_DIR);
    if (!dir)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);

        if (len < 5 || strcmp(name + len - 4, ".png") != 0)
            continue;
        if (strncmp(name, "sstv-", 5) != 0)
            continue;

        if (sstv_browser_file_count == sstv_browser_file_capacity) {
            size_t new_capacity = sstv_browser_file_capacity ?
                                  sstv_browser_file_capacity * 2 : 128;
            char **new_files = realloc(sstv_browser_files,
                                       new_capacity * sizeof(*new_files));
            if (!new_files)
                break;

            sstv_browser_files = new_files;
            sstv_browser_file_capacity = new_capacity;
        }

        char *copy = strdup(name);
        if (copy)
            sstv_browser_files[sstv_browser_file_count++] = copy;
    }
    closedir(dir);

    qsort(sstv_browser_files, sstv_browser_file_count,
          sizeof(sstv_browser_files[0]), sstv_browser_name_compare);
}

/* Row contents for the browser list: "n | date | time". */
static void sstv_browser_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;

    if (index < 0 || (unsigned int)index >= sstv_browser_file_count)
        return;

    const char *name = sstv_browser_files[index];

    snprintf(row->cell[0], UI_LIST_CELL_LEN, "%d", index + 1);

    if (ui_list_parse_stamp_ext(name, "sstv-", "_dn.png",          /* 1KO125: denoised */
                                row->cell[1], UI_LIST_CELL_LEN,
                                row->cell[2], UI_LIST_CELL_LEN)) {
        strncat(row->cell[2], " DN", UI_LIST_CELL_LEN - strlen(row->cell[2]) - 1);
    } else if (!ui_list_parse_stamp(name, "sstv-",
                             row->cell[1], UI_LIST_CELL_LEN,
                             row->cell[2], UI_LIST_CELL_LEN)) {
        snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", name);
        row->cell[2][0] = '\0';
    }
}

static void sstv_browser_activate_cb(void *user, int index)
{
    (void)user;

    if (index < 0 || (unsigned int)index >= sstv_browser_file_count)
        return;

    sstv_browser_open_png(sstv_browser_files[index]);
}

/* Tell the list how many pictures there are (selection kept if valid). */
static void sstv_browser_update_list(void)
{
    if (!sstv_browser_table)
        return;

    ui_list_set_count(sstv_browser_table, (int)sstv_browser_file_count);
}

static void sstv_browser_update_buttons(void)
{
    if (sstv_browser_preview) {
        btn_delete_picture.label = "Done";
        buttons_disabled(&btn_delete_picture, false);
    } else {
        btn_delete_picture.label = "Delete\npicture";
        buttons_disabled(&btn_delete_picture,
                         !sstv_browser_open || sstv_browser_file_count == 0);
    }

    if (sstv_browser_open)
        btn_browser.label = "Close\nBrowser";
    else
        btn_browser.label = "RX Pics\nBrowser";

    if (btn_browser.disp_btn)
        buttons_refresh(&btn_browser);
    if (btn_delete_picture.disp_btn)
        buttons_refresh(&btn_delete_picture);
}

static void sstv_browser_free_image(void)
{
    free(sstv_browser_image);
    sstv_browser_image = NULL;
    sstv_browser_image_width = 0;
    sstv_browser_image_height = 0;
    sstv_browser_preview = false;
}

static void sstv_browser_render_image(void)
{
    if (!sstv_browser_buffer || !sstv_browser_image ||
        !sstv_browser_image_width || !sstv_browser_image_height)
        return;

    const unsigned int width = sstv_browser_image_width;
    const unsigned int height = sstv_browser_image_height;
    unsigned int out_w = SSTV_BROWSER_W;
    unsigned int out_h = (unsigned int)(((uint64_t)height * SSTV_BROWSER_W) / width);

    if (out_h > SSTV_BROWSER_H) {
        out_h = SSTV_BROWSER_H;
        out_w = (unsigned int)(((uint64_t)width * SSTV_BROWSER_H) / height);
    }
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    const unsigned int x0 = (SSTV_BROWSER_W - out_w) / 2;
    const unsigned int y0 = (SSTV_BROWSER_H - out_h) / 2;
    const lv_color_t black = lv_color_make(0, 0, 0);

    for (unsigned int i = 0; i < SSTV_BROWSER_W * SSTV_BROWSER_H; ++i)
        sstv_browser_buffer[i] = black;

    for (unsigned int oy = 0; oy < out_h; ++oy) {
        unsigned int sy = (unsigned int)(((uint64_t)oy * height) / out_h);
        if (sy >= height) sy = height - 1;

        for (unsigned int ox = 0; ox < out_w; ++ox) {
            unsigned int sx = (unsigned int)(((uint64_t)ox * width) / out_w);
            if (sx >= width) sx = width - 1;

            const uint8_t *p = &sstv_browser_image[
                ((size_t)sy * width + sx) * 3u
            ];
            sstv_browser_buffer[(y0 + oy) * SSTV_BROWSER_W + x0 + ox] =
                lv_color_make(p[0], p[1], p[2]);
        }
    }

    if (sstv_browser_canvas)
        lv_obj_invalidate(sstv_browser_canvas);
}

static bool sstv_browser_open_png(const char *filename)
{
    if (!filename || !*filename)
        return false;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SSTV_SAVE_DIR, filename);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "SSTV BROWSER: unable to open %s: %s\n",
                path, strerror(errno));
        return false;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                                 NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(fp);
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        fprintf(stderr, "SSTV BROWSER: libpng read error for %s\n", path);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = png_get_image_width(png_ptr, info_ptr);
    png_uint_32 height = png_get_image_height(png_ptr, info_ptr);
    int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);

    if (!width || !height || width > 10000 || height > 10000) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);

    color_type = png_get_color_type(png_ptr, info_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    /* 1KO125: interlaced (Adam7) PNG: the rows come in several passes */
    int passes = png_set_interlace_handling(png_ptr);

    png_read_update_info(png_ptr, info_ptr);
    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    int channels = png_get_channels(png_ptr, info_ptr);
    if (channels < 3 || !rowbytes) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    uint8_t *image = malloc((size_t)width * (size_t)height * 3u);
    /* interlaced: the whole picture, the passes fill it in; else one row */
    uint8_t *raw = malloc(passes > 1 ? rowbytes * height : rowbytes);
    if (!image || !raw) {
        free(image);
        free(raw);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    for (int pass = 0; pass < passes; ++pass)
    for (png_uint_32 y = 0; y < height; ++y) {
        uint8_t *row = passes > 1 ? raw + (size_t)y * rowbytes : raw;
        png_read_row(png_ptr, row, NULL);
        if (pass != passes - 1) continue;
        for (png_uint_32 x = 0; x < width; ++x) {
            const uint8_t *sp = row + (size_t)x * (size_t)channels;
            uint8_t *dp = image + ((size_t)y * width + x) * 3u;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
        }
    }

    png_read_end(png_ptr, NULL);
    free(raw);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    sstv_browser_free_image();
    sstv_browser_image = image;
    sstv_browser_image_width = (unsigned int)width;
    sstv_browser_image_height = (unsigned int)height;

    if (!sstv_browser_buffer) {
        sstv_browser_buffer = malloc(
            (size_t)SSTV_BROWSER_W * SSTV_BROWSER_H * sizeof(lv_color_t)
        );
        if (!sstv_browser_buffer) {
            sstv_browser_free_image();
            return false;
        }
    }

    if (!sstv_browser_canvas) {
        sstv_browser_canvas = lv_canvas_create(dialog.obj);
        lv_canvas_set_buffer(sstv_browser_canvas, sstv_browser_buffer,
                             SSTV_BROWSER_W, SSTV_BROWSER_H,
                             LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(sstv_browser_canvas, SSTV_BROWSER_W, SSTV_BROWSER_H);
        lv_obj_align(sstv_browser_canvas, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_event_cb(sstv_browser_canvas,
                            sstv_browser_preview_pressed_cb,
                            LV_EVENT_PRESSED, NULL);
        /* VOL changes the volume while a picture is shown. */
        lv_obj_add_event_cb(sstv_browser_canvas, key_cb, LV_EVENT_KEY, NULL);
    }

    sstv_browser_render_image();

    if (sstv_browser_table) {
        lv_group_remove_obj(sstv_browser_table);
        lv_obj_add_flag(sstv_browser_table, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(sstv_browser_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_group_add_obj(keyboard_group, sstv_browser_canvas);
    lv_group_focus_obj(sstv_browser_canvas);
    lv_group_set_editing(keyboard_group, true);

    sstv_browser_preview = true;
    sstv_browser_update_buttons();

    fprintf(stderr, "SSTV BROWSER: opened %s (%u x %u)\n",
            path, sstv_browser_image_width, sstv_browser_image_height);
    return true;
}

static void sstv_browser_return_to_list(void)
{
    if (!sstv_browser_open)
        return;

    if (sstv_browser_canvas) {
        lv_group_remove_obj(sstv_browser_canvas);
        lv_obj_add_flag(sstv_browser_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    sstv_browser_free_image();
    sstv_browser_refresh();

    if (sstv_browser_table) {
        lv_obj_clear_flag(sstv_browser_table, LV_OBJ_FLAG_HIDDEN);
        sstv_browser_update_list();
        lv_group_add_obj(keyboard_group, sstv_browser_table);
        lv_group_focus_obj(sstv_browser_table);
        lv_group_set_editing(keyboard_group, true);
        lv_obj_invalidate(sstv_browser_table);
    }
    sstv_browser_update_buttons();
}

static void sstv_browser_preview_pressed_cb(lv_event_t *e)
{
    (void)e;
    /* MFK press on the preview returns to the file list. */
    sstv_browser_return_to_list();
}

static void sstv_browser_delete_selected(void)
{
    if (!sstv_browser_open || sstv_browser_preview ||
        !sstv_browser_table || sstv_browser_file_count == 0)
        return;

    const int row = ui_list_get_selected(sstv_browser_table);

    if (row < 0 || (unsigned int)row >= sstv_browser_file_count)
        return;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s",
             SSTV_SAVE_DIR, sstv_browser_files[row]);

    if (unlink(path) != 0) {
        fprintf(stderr, "SSTV BROWSER: unable to delete %s: %s\n",
                path, strerror(errno));
        return;
    }

    fprintf(stderr, "SSTV BROWSER: deleted %s\n", path);

    /* Re-scan every time: deleting the last picture immediately changes the
     * table to "No saved pictures", exactly like the WEFAX browser. */
    sstv_browser_refresh();
    sstv_browser_update_list();
    sstv_browser_update_buttons();

    lv_group_focus_obj(sstv_browser_table);
    lv_group_set_editing(keyboard_group, true);
}

static void sstv_browser_build(void)
{
    if (sstv_browser_open)
        return;

    sstv_browser_refresh();

    /* Reuse the native dialog frame (the same one used by the main SSTV view).
     * Hide only the normal RX contents while the browser is open; the browser
     * table/preview are then drawn directly on dialog.obj, exactly over the
     * existing stock 1.0.2 frame. */
    if (sstv_canvas)
        lv_obj_add_flag(sstv_canvas, LV_OBJ_FLAG_HIDDEN);
    if (status_label)
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    if (progress_bar) {
        /* The browser temporarily hides the progress bar.  Remember its
         * previous state so closing the browser cannot make an idle progress
         * indicator appear behind the normal SSTV status text. */
        sstv_browser_progress_was_hidden =
            lv_obj_has_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* Pixel-for-pixel WEFAX BIG browser geometry. */
    ui_list_cfg_t cfg = {
        .width = 700,
        .height = 270,
        .col_count = 3,
        .cols = {
            { .x = 12,  .w = 44,  .align = UI_LIST_ALIGN_RIGHT, .muted = true },
            { .x = 76,  .w = 190, .align = UI_LIST_ALIGN_LEFT },
            { .x = 290, .w = 220, .align = UI_LIST_ALIGN_LEFT },
        },
        .row_cb = sstv_browser_row_cb,
        .activate_cb = sstv_browser_activate_cb,
        .empty_text = "No saved pictures",
        .vol_coarse = true,
    };

    sstv_browser_table = ui_list_create(dialog.obj, &cfg);
    lv_obj_align(sstv_browser_table, LV_ALIGN_TOP_MID, 0, 40);

    sstv_browser_update_list();
    lv_group_add_obj(keyboard_group, sstv_browser_table);
    lv_group_focus_obj(sstv_browser_table);
    lv_group_set_editing(keyboard_group, true);

    sstv_browser_open = true;
    sstv_browser_preview = false;
    sstv_browser_update_buttons();
}

static void sstv_browser_close(void)
{
    if (sstv_browser_table) {
        lv_group_remove_obj(sstv_browser_table);
        lv_obj_del(sstv_browser_table);
        sstv_browser_table = NULL;
    }

    if (sstv_browser_canvas) {
        lv_group_remove_obj(sstv_browser_canvas);
        lv_obj_del(sstv_browser_canvas);
        sstv_browser_canvas = NULL;
    }

    /* Restore the normal SSTV contents; dialog.obj itself was never replaced,
     * so the same native 1.0.2 frame remains in place throughout. */
    if (sstv_canvas) {
        lv_obj_clear_flag(sstv_canvas, LV_OBJ_FLAG_HIDDEN);

        /* Give the keys back to the SSTV canvas.  When the browser objects
         * left the group the canvas was still hidden, so LVGL could not move
         * the focus to it and the group was left with no focused object:
         * VOL (and MFK) stopped working until SSTV was reopened. */
        if (lv_obj_get_group(sstv_canvas) != keyboard_group)
            lv_group_add_obj(keyboard_group, sstv_canvas);
        lv_group_focus_obj(sstv_canvas);
        lv_group_set_editing(keyboard_group, true);
    }
    if (status_label)
        lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    if (progress_bar) {
        if (sstv_browser_progress_was_hidden)
            lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
    }

    free(sstv_browser_buffer);
    sstv_browser_buffer = NULL;
    sstv_browser_free_image();
    sstv_browser_free_files();

    sstv_browser_open = false;
    sstv_browser_preview = false;

    if (dialog.obj)
        lv_obj_invalidate(dialog.obj);

    sstv_browser_update_buttons();
}
