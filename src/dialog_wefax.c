/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 WeFax receiver
 *
 * (c) 2026 Hany El Imam
 * www.hanynet.com - www.murusfirewall.com - https://github.com/TheMurusTeam
 *
 */

#include "main_screen.h"
#include "dialog_wefax.h"
#include "wefax/decoder.h"
#include "wefax/stations.h"
#include "keyboard.h"
#include "styles.h"
#include "radio.h"
#include "events.h"
#include "dsp.h"
#include "msg.h"
#include "ui_list.h"
#include "app_info.h"
#include "spectrum.h"

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <png.h>


/*
 * LCD preview dimensions.
 */
#define PREVIEW_WIDTH       750
#define PREVIEW_HEIGHT      320

/*
 * LVGL refresh period.
 */
#define PREVIEW_TIMER_MS     30

/*
 * Full-resolution fax history kept for Save.
 * 1809 * 1500 bytes = ~2.59 MiB. (only horizontal fax)
 * 1809 * 1900 bytes = ~3.28 MiB. (including vertical fax)
 */
#define FAX_BUFFER_ROWS     2400
#define FAX_SAVE_DIR        "/mnt/wefax"


/*
 * WAV test.
 */


/*
 * Monotonic counter incremented by the decoder for every accepted
 * provisional APT START.  Kept local here to avoid changing decoder.h.
 */
uint32_t wefax_decoder_auto_start_generation(
    const wefax_decoder_t *decoder
);

/* True only after the provisional START has been confirmed by the black band. */
bool wefax_decoder_auto_fax_confirmed(
    const wefax_decoder_t *decoder
);


static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void fax_page_1_cb(button_data_t *data);
static void fax_page_2_cb(button_data_t *data);
static void fax_page_3_cb(button_data_t *data);
static void fax_page_4_cb(button_data_t *data);

/* Stations list and schedules ("Freq.", FAX 1:4), dialog_wefax_freq.inc */
static void freq_open_cb(button_data_t *data);
static const char *freq_label_getter(void);
static void freq_close(void);
static void freq_start(void);
static void freq_stop(void);

static void fax_browser_cb(button_data_t *data);
static void fix_align_cb(button_data_t *data);
static void fix_tilt_cb(button_data_t *data);
static void save_fax_cb(button_data_t *data);
static void save_fax_delayed_cb(lv_timer_t *timer);

static void fax_browser_build(void);
static void fax_browser_close(void);
static void fax_browser_refresh(void);
static bool fax_browser_open_png(const char *filename);
static void fax_browser_free_image(void);
static void fax_browser_render_image(void);
static bool fax_browser_save_edited_png(void);
static void fax_browser_return_to_list(void);
static void fax_browser_set_editor_buttons(bool enabled);
static void fax_browser_update_modified_state(void);
static void fax_browser_update_list(void);
static void fax_browser_delete_selected(void);

static void close_cb(button_data_t *data);
static void cont_cb(button_data_t *data);
static void auto_cb(button_data_t *data);
static void zoom_cb(button_data_t *data);
static void clear_cb(button_data_t *data);

/* Start/Stop Manual RX, Start/Stop Auto RX, Clear: they follow the decoder */
static const char *cont_label_getter(void);
static const char *auto_label_getter(void);
static void rx_buttons_sync(bool force);

static const char *align_label_getter(void);
static const char *tilt_label_getter(void);
static void align_cb(button_data_t *data);
static void tilt_cb(button_data_t *data);

/* Auto Slant, see dialog_wefax_slant.inc */
static const char *slant_label_getter(void);
static void slant_auto_cb(button_data_t *data);
static void slant_load(void);
static void slant_flush(void);
static void slant_poll(void);
static bool slant_fax_started(void);
static void slant_user_tilt_changed(void);
static void save_cb(button_data_t *data);

static void audio_cb(
    size_t n,
    float *samples
);

static void row_cb(
    const uint8_t *row,
    unsigned int width,
    unsigned int row_number,
    void *user_data
);

static void update_preview_cb(
    lv_timer_t *timer
);

static void clear_preview(void);
static void clear_fax_buffer(void);
static bool ensure_fax_buffer(void);
static void store_fax_row(const uint8_t *row);
static bool save_fax_png_async(void);
static void *save_fax_worker(void *arg);
static void check_async_save_completion(void);
static void auto_finish_fax(void);


/*
 * Decoder.
 */
static wefax_decoder_t *decoder = NULL;

/* Last provisional APT START already handled by the GUI thread. */
static uint32_t auto_start_generation_seen = 0;

/* GUI-side latch: provisional AUTO rows stay hidden until confirmation. */
static bool auto_fax_confirmed_seen = false;

/*
 * AUTO STOP reset handoff.
 *
 * auto_finish_fax() runs in LVGL context, while the decoder normally runs
 * in the RX audio thread.  Keep the decoder reset out of the GUI thread:
 * the audio callback consumes this request before processing its next block.
 */
static atomic_bool auto_reset_pending = ATOMIC_VAR_INIT(false);


/*
 * LVGL objects.
 */
static lv_obj_t *fax_canvas = NULL;
static lv_obj_t *fax_viewport = NULL;
static lv_obj_t *fax_small_panel = NULL;
static lv_obj_t *small_top_mask = NULL;

/* Saved-fax browser (FAX 3:4). */
#define FAX_BROWSER_INITIAL_CAPACITY 128
static lv_obj_t *fax_browser_table = NULL;
static char **fax_browser_files = NULL;
static unsigned int fax_browser_file_count = 0;
static unsigned int fax_browser_file_capacity = 0;
static bool fax_browser_open = false;
static bool fax_browser_forced_big = false;
static int fax_browser_saved_zoom = 0;
static bool fax_browser_editing = false;
static bool fax_browser_align_active = false;
static bool fax_browser_tilt_active = false;
static int fax_browser_align = 0;
static int fax_browser_tilt = 0;
static bool fax_browser_modified = false;
static uint8_t *fax_browser_image = NULL;
static unsigned int fax_browser_image_width = 0;
static unsigned int fax_browser_image_height = 0;
static char fax_browser_image_path[256];
static bool align_mfk_active = false;
static bool tilt_mfk_active = false;
static int wefax_align = 0;
static int wefax_tilt = 0;
static uint32_t dsp_audio_sub_id = AUDIO_SUB_INVALID;

/*
 * Original (unaligned) source rows corresponding to the LCD rows
 * currently visible.  275 * 1809 = ~486 KiB.
 */
static uint8_t preview_source_rows[PREVIEW_HEIGHT][WEFAX_WIDTH];
static unsigned int preview_source_count = 0;
static unsigned int preview_source_head = 0;

/*
 * Tilt anchor.  Absolute LCD-row number (counted from the start of the
 * fax, i.e. from the last clear_preview()) of every row kept in
 * preview_source_rows.  Tilt is applied relative to the FIRST row of the
 * fax, so rows already on screen never move when new rows arrive, and the
 * phasing-aligned top of the fax stays at x=0.
 */
static uint32_t preview_source_index[PREVIEW_HEIGHT];
static uint32_t preview_row_counter = 0;

/*
 * Full-resolution grayscale ring buffer.
 *
 * Rows are stored unmodified (before Align/Tilt), so Save can apply the
 * current correction settings to the complete retained fax.
 */
static uint8_t *fax_buffer = NULL;
static unsigned int fax_buffer_count = 0;
static unsigned int fax_buffer_head = 0;

/*
 * Total rows stored since the fax buffer was cleared.  The buffer is a
 * ring, so the absolute number of its oldest retained row is
 * fax_rows_total - fax_buffer_count (Tilt anchor for zoom and save).
 */
static uint32_t fax_rows_total = 0;

/*
 * Asynchronous full-fax PNG saving.
 *
 * The LVGL thread only copies the retained fax into an independent
 * snapshot.  libpng compression and filesystem I/O are performed by a
 * detached worker thread, so the GUI/event queue never stalls while a
 * large fax is being written.
 */
typedef struct {
    uint8_t *image;
    unsigned int rows;
    uint32_t row_base;   /* absolute number of image row 0 (Tilt anchor) */
    int align;
    int tilt;
    char path[128];
} wefax_save_job_t;

static pthread_mutex_t save_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool async_save_running = false;
static bool async_save_completed = false;
static bool async_save_success = false;

typedef enum {
    WEFAX_ZOOM_SMALL = 0,
    WEFAX_ZOOM_BIG,
    WEFAX_ZOOM_FULL
} wefax_zoom_t;

static wefax_zoom_t zoom_mode = WEFAX_ZOOM_SMALL;

static void render_zoomed_fax(void);
static void rerender_preview(void);
static void apply_zoom_geometry(void);
static lv_timer_t *preview_timer = NULL;
static lv_timer_t *save_fax_timer = NULL;



/*
 * Canvas framebuffer.
 */
static lv_color_t preview_buffer[
    PREVIEW_WIDTH * PREVIEW_HEIGHT
];


/*
 * Communication between audio thread and
 * LVGL thread.
 */
static uint8_t pending_row[WEFAX_WIDTH];

static bool pending_row_valid = false;

static pthread_mutex_t row_mutex =
    PTHREAD_MUTEX_INITIALIZER;


/*
 * Vertical preview scaling accumulator.
 *
 * Horizontal scale:
 *
 *     710 / 1809
 *
 * We use exactly the same ratio vertically.
 *
 * For every received WEFAX row we add 710.
 * When the accumulator reaches 1809 we emit
 * one LCD row and subtract 1809.
 *
 * This gives exactly:
 *
 *     710 / 1809 = 0.39248...
 *
 * LCD rows per WEFAX row.
 */
static unsigned int vertical_accumulator = 0;


/*
 * WEFAX button pages.
 *
 * F1 cycles locally:
 *
 *     FAX 1:3 -> FAX 2:3 -> FAX 3:3 -> FAX 1:3
 *
 * This deliberately does not modify the global button page groups
 * in buttons.cpp.
 */

/*
 * FAX 1:3 - Reception
 */
static button_data_t btn_fax_page_1 = {
    .type = BTN_TEXT,
    .label = "(FAX 1:4)",
    .press = fax_page_2_cb
};

static button_data_t btn_close = {
    .type = BTN_TEXT,
    .label = "Close\nWeFax",
    .press = close_cb
};

static button_data_t btn_cont = {
    .type = BTN_TEXT_FN,
    .label_fn = cont_label_getter,
    .press = cont_cb
};

static button_data_t btn_auto = {
    .type = BTN_TEXT_FN,
    .label_fn = auto_label_getter,
    .press = auto_cb
};

static button_data_t btn_clear = {
    .type = BTN_TEXT,
    .label = "Clear",
    .press = clear_cb
};


/*
 * FAX 2:3 - Live fax adjustment
 */
static button_data_t btn_fax_page_2 = {
    .type = BTN_TEXT,
    .label = "(FAX 2:4)",
    .press = fax_page_3_cb
};

static button_data_t btn_align = {
    .type = BTN_TEXT_FN,
    .label_fn = align_label_getter,
    .press = align_cb
};

static button_data_t btn_tilt = {
    .type = BTN_TEXT_FN,
    .label_fn = tilt_label_getter,
    .press = tilt_cb
};

static button_data_t btn_zoom = {
    .type = BTN_TEXT,
    .label = "View Size\nSmall",
    .press = zoom_cb
};

static button_data_t btn_save = {
    .type = BTN_TEXT,
    .label = "Save\nand Clear",
    .press = save_cb
};


/*
 * FAX 3:3 - Saved fax browser/editor
 */
static button_data_t btn_fax_page_3 = {
    .type = BTN_TEXT,
    .label = "(FAX 3:4)",
    .press = fax_page_4_cb
};

static button_data_t btn_fax_browser = {
    .type = BTN_TEXT,
    .label = "Fax\nBrowser",
    .press = fax_browser_cb
};

static button_data_t btn_fix_align = {
    .type = BTN_TEXT,
    .label = "Fix\nAlign",
    .press = fix_align_cb,
    .disabled = true
};

static button_data_t btn_fix_tilt = {
    .type = BTN_TEXT,
    .label = "Fix\nTilt",
    .press = fix_tilt_cb,
    .disabled = true
};

static button_data_t btn_save_fax = {
    .type = BTN_TEXT,
    .label = "Done",
    .press = save_fax_cb,
    .disabled = true
};


/*
 * FAX 4:4 - Close
 */
static button_data_t btn_fax_page_4 = {
    .type = BTN_TEXT,
    .label = "(FAX 4:4)",
    .press = fax_page_1_cb
};

static button_data_t btn_freq = {
    .type = BTN_TEXT_FN,
    .label_fn = freq_label_getter,
    .press = freq_open_cb
};

static button_data_t btn_auto_slant = {
    .type = BTN_TEXT_FN,
    .label_fn = slant_label_getter,
    .press = slant_auto_cb
};


static buttons_page_t btn_page_1 = {
    .items = {
        &btn_fax_page_1,
        &btn_cont,
        &btn_auto,
        &btn_clear,
        &btn_freq
    }
};

static buttons_page_t btn_page_2 = {
    .items = {
        &btn_fax_page_2,
        &btn_align,
        &btn_tilt,
        &btn_zoom,
        &btn_save
    }
};

static buttons_page_t btn_page_3 = {
    .items = {
        &btn_fax_page_3,
        &btn_fax_browser,
        &btn_fix_align,
        &btn_fix_tilt,
        &btn_save_fax
    }
};


/* 1KO125: Info window, see app_info.c */
static void info_cb(button_data_t *data);
static button_data_t btn_info = { .type = BTN_TEXT, .label = "Info", .press = info_cb };

static buttons_page_t btn_page_4 = {
    .items = {
        &btn_fax_page_4,
        &btn_auto_slant,
        NULL,
        &btn_info,
        &btn_close
    }
};


static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .key_cb = key_cb,
    .btn_page = &btn_page_1
};


dialog_t *dialog_wefax = &dialog;

static void info_cb(button_data_t *data) { (void)data; app_info_open(dialog.obj, APP_INFO_WEFAX); }

bool dialog_wefax_is_active(void)
{
    return dialog.run;
}




/*
 * Clear LCD preview framebuffer.
 */
static void clear_preview(void)
{
    const lv_color_t white =
        lv_color_make(255, 255, 255);


    for (
        unsigned int i = 0;
        i < PREVIEW_WIDTH * PREVIEW_HEIGHT;
        i++
    ) {
        preview_buffer[i] = white;
    }


    /*
     * Restart vertical scaling from a clean
     * boundary.
     */
    vertical_accumulator = 0;

    preview_source_count = 0;
    preview_source_head = 0;
    preview_row_counter = 0;


    if (fax_canvas) {
        lv_obj_invalidate(fax_canvas);
    }


    pthread_mutex_lock(&row_mutex);

    pending_row_valid = false;

    pthread_mutex_unlock(&row_mutex);
    /*
     * Clear the complete retained fax as well.
     */
    clear_fax_buffer();
}





/*
 * Dialog construction.
 */
static void construct_cb(lv_obj_t *parent)
{
    zoom_mode = WEFAX_ZOOM_SMALL;
    btn_zoom.label = " View Size\nSmall";

    slant_load();

    align_mfk_active = false;

    decoder = wefax_decoder_create();
    auto_start_generation_seen = 0;
    auto_fax_confirmed_seen = false;

    if (!decoder) {

        fprintf(
            stderr,
            "WEFAX: unable to create decoder\n"
        );
    }
    else {

        wefax_decoder_set_row_callback(
            decoder,
            row_cb,
            NULL
        );
    }

    if (dsp_audio_sub_id == AUDIO_SUB_INVALID) {
        dsp_audio_sub_id = dsp_audio_subscribe_float(audio_cb, WEFAX_SAMPLE_RATE);
    }
    dsp_audio_set_active(dsp_audio_sub_id, true);

    dialog.obj = dialog_init(parent);
    freq_start();

    /* Tuning markers on the spectrum: black / white, 1075 / 1925 Hz */
    spectrum_set_markers(WEFAX_CENTER_HZ - WEFAX_SHIFT_HZ / 2,
                         WEFAX_CENTER_HZ + WEFAX_SHIFT_HZ / 2);

    rx_buttons_sync(true);


    lv_obj_set_size(
        dialog.obj,
        795,
        350
    );

    lv_obj_center(dialog.obj);

    /*
     * Native SMALL frame: exactly the NAVTEX/RTTY construction.
     * Its geometry, position, rounded corners and theme image all come
     * from style.panels.base; nothing is drawn by WEFAX itself.
     */
    fax_small_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(fax_small_panel);
    lv_obj_add_style(fax_small_panel, &style.panels.base, 0);
    lv_obj_clear_flag(fax_small_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(fax_small_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(fax_small_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_translate_y(dialog.obj, 4, 0);
    //lv_obj_set_style_translate_y(fax_small_panel, 5, 0);

    /*
     * Fax viewport.
     *
     * This is deliberately transparent: in SMALL it clips the 710x320
     * canvas to the reduced visible area, so the real dialog background
     * remains visible as the top/bottom margin.  No fake solid-colour
     * mask is needed.
     */
    fax_viewport = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(fax_viewport);
    lv_obj_set_size(fax_viewport, 795, 350);
    lv_obj_center(fax_viewport);
    lv_obj_clear_flag(fax_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(fax_viewport, LV_OBJ_FLAG_CLICKABLE);

    /*
     * Fax canvas.
     */
    fax_canvas =
        lv_canvas_create(fax_viewport);


    lv_canvas_set_buffer(
        fax_canvas,
        preview_buffer,
        PREVIEW_WIDTH,
        PREVIEW_HEIGHT,
        LV_IMG_CF_TRUE_COLOR
    );


    lv_obj_set_size(
        fax_canvas,
        PREVIEW_WIDTH,
        PREVIEW_HEIGHT
    );


    lv_obj_align(
        fax_canvas,
        LV_ALIGN_CENTER,
        0,
        0
    );


    /*
     * SMALL no longer uses a colour mask.  The transparent fax_viewport
     * performs the clipping and exposes the dialog's real background.
     */
    small_top_mask = NULL;

    /*
     * Give the WEFAX dialog an LVGL key target for MFK events.
     */
    lv_group_add_obj(
        keyboard_group,
        fax_canvas
    );

    lv_obj_add_event_cb(
        fax_canvas,
        key_cb,
        LV_EVENT_KEY,
        NULL
    );

    lv_group_focus_obj(
        fax_canvas
    );

    lv_group_set_editing(
        keyboard_group,
        true
    );


    clear_preview();
    apply_zoom_geometry();


    /*
     * LVGL-side refresh timer.
     */
    preview_timer =
        lv_timer_create(
            update_preview_cb,
            PREVIEW_TIMER_MS,
            NULL
        );
}


/*
 * Dialog destruction.
 */
static void destruct_cb(void)
{
    app_info_abort();
    spectrum_clear_markers();
    dsp_audio_set_active(dsp_audio_sub_id, false);


    if (preview_timer) {

        lv_timer_del(preview_timer);

        preview_timer = NULL;
    }


    if (save_fax_timer) {

        lv_timer_del(save_fax_timer);

        save_fax_timer = NULL;
    }


    align_mfk_active = false;
    tilt_mfk_active = false;
    slant_flush();
    fax_browser_close();
    freq_close();
    freq_stop();
    wefax_db_free();

    /*
     * fax_small_panel is a direct child of the main-screen parent, not of
     * dialog.obj, so dialog_destruct() cannot delete it for us.
     */
    if (fax_small_panel) {
        /*
         * fax_viewport normally belongs to the SMALL panel while SMALL is
         * active. Move it back under dialog.obj before deleting the panel,
         * so the viewport/canvas remain owned by the dialog teardown.
         */
        if (fax_viewport && dialog.obj) {
            lv_obj_set_parent(fax_viewport, dialog.obj);
        }
        lv_obj_del(fax_small_panel);
        fax_small_panel = NULL;
    }

    fax_canvas = NULL;
    fax_viewport = NULL;
    small_top_mask = NULL;


    if (decoder) {

        wefax_decoder_destroy(decoder);

        decoder = NULL;
    }


    pthread_mutex_lock(&row_mutex);

    pending_row_valid = false;

    pthread_mutex_unlock(&row_mutex);


    vertical_accumulator = 0;

    clear_fax_buffer();

    if (fax_buffer) {
        free(fax_buffer);
        fax_buffer = NULL;
    }
}


/*
 * MFK handling while the WEFAX dialog is open.
 *
 * dialog_construct() disables the normal main-screen key target,
 * therefore the dialog handles MFK left/right itself.
 */
/*
 * Apply only the viewport geometry for the selected zoom mode.
 *
 * DEFAULT/FULL keep the original full-screen WEFAX dialog untouched.
 * SMALL is exactly the DEFAULT viewport cropped only from the top.
 * Its left, right and bottom edges remain pixel-identical to DEFAULT.
 * The canvas remains 750x320 and is bottom-aligned inside the shorter
 * dialog, preserving the DEFAULT horizontal scale and newest rows.
 */
static void apply_zoom_geometry(void)
{
    if (!dialog.obj || !fax_canvas || !fax_viewport || !fax_small_panel) {
        return;
    }

    if (zoom_mode == WEFAX_ZOOM_SMALL) {
        /*
         * SMALL: use the real native RTTY/NAVTEX panel, created directly
         * on the same parent exactly as NAVTEX does.
         */
        lv_obj_set_parent(fax_viewport, fax_small_panel);

        lv_obj_add_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(fax_small_panel, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(fax_viewport, PREVIEW_WIDTH, 152);
        lv_obj_align(fax_viewport, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(fax_viewport, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(fax_canvas, PREVIEW_WIDTH, PREVIEW_HEIGHT);
        lv_obj_align(fax_canvas, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else {
        /*
         * BIG/FULL: restore the viewport to the native dialog frame.
         */
        lv_obj_set_parent(fax_viewport, dialog.obj);

        lv_obj_add_flag(fax_small_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(dialog.obj, 795, 350);
        lv_obj_center(dialog.obj);

        lv_obj_set_size(fax_viewport, 795, 350);
        lv_obj_center(fax_viewport);
        lv_obj_clear_flag(fax_viewport, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(fax_canvas, PREVIEW_WIDTH, PREVIEW_HEIGHT);
        lv_obj_align(fax_canvas, LV_ALIGN_CENTER, 0, 0);
    }
}



/*
 * Rebuild the complete currently visible fax from the saved original
 * 1809-pixel rows.  Called only while the user turns Align.
 */
static void rerender_preview(void)
{
    if (zoom_mode == WEFAX_ZOOM_FULL) {
        render_zoomed_fax();
        return;
    }

    const lv_color_t white =
        lv_color_make(255, 255, 255);

    for (unsigned int i = 0;
         i < PREVIEW_WIDTH * PREVIEW_HEIGHT;
         i++) {
        preview_buffer[i] = white;
    }

    int align =
        wefax_align;

    /*
     * Tilt unit = 1/1000 native WEFAX pixel per displayed/source row.
     * The FIRST row of the fax is the anchor (zero shift); each row is
     * shifted by its absolute row number.  Anchoring on the newest row
     * made the whole image slide sideways at every new row.
     */
    int tilt =
        wefax_tilt;

    unsigned int first =
        (preview_source_head + PREVIEW_HEIGHT - preview_source_count)
        % PREVIEW_HEIGHT;

    unsigned int dst_y =
        PREVIEW_HEIGHT - preview_source_count;

    for (unsigned int r = 0;
         r < preview_source_count;
         r++) {

        const uint8_t *src =
            preview_source_rows[
                (first + r) % PREVIEW_HEIGHT
            ];

        lv_color_t *dst =
            &preview_buffer[
                (dst_y + r) * PREVIEW_WIDTH
            ];

        const int row_from_first =
            (int)preview_source_index[
                (first + r) % PREVIEW_HEIGHT
            ];

        const int tilt_shift =
            (row_from_first * tilt) / 1000;

        for (unsigned int x = 0;
             x < PREVIEW_WIDTH;
             x++) {

            unsigned int src_begin =
                (x * WEFAX_WIDTH)
                / PREVIEW_WIDTH;

            unsigned int src_end =
                ((x + 1) * WEFAX_WIDTH)
                / PREVIEW_WIDTH;

            if (src_end <= src_begin) {
                src_end = src_begin + 1;
            }

            unsigned int sum = 0;
            unsigned int count = 0;

            for (unsigned int sx = src_begin;
                 sx < src_end;
                 sx++) {

                int source_x =
                    (int)sx - align - tilt_shift;

                while (source_x < 0) {
                    source_x += WEFAX_WIDTH;
                }

                while (source_x >= WEFAX_WIDTH) {
                    source_x -= WEFAX_WIDTH;
                }

                sum += src[source_x];
                count++;
            }

            uint8_t gray =
                (uint8_t)(sum / count);

            dst[x] =
                lv_color_make(
                    gray,
                    gray,
                    gray
                );
        }
    }

    if (fax_canvas) {
        lv_obj_invalidate(fax_canvas);
    }
}


/*
 * Empty the logical full-fax history.  The allocated memory is retained
 * and reused so repeated Clear/Cont operations do not fragment the heap.
 */
static void clear_fax_buffer(void)
{
    fax_buffer_count = 0;
    fax_buffer_head = 0;
    fax_rows_total = 0;
}


/*
 * Allocate the 1500-row full-resolution buffer on first use.
 */
static bool ensure_fax_buffer(void)
{
    if (fax_buffer) {
        return true;
    }

    fax_buffer =
        (uint8_t *)malloc(
            (size_t)FAX_BUFFER_ROWS *
            (size_t)WEFAX_WIDTH
        );

    if (!fax_buffer) {
        printf(
            "WEFAX SAVE: cannot allocate %u bytes\n",
            (unsigned int)(
                FAX_BUFFER_ROWS * WEFAX_WIDTH
            )
        );
        return false;
    }

    clear_fax_buffer();

    return true;
}


/*
 * Append one original 1809-pixel decoder row to the ring buffer.
 * Once full, each new row replaces exactly the oldest row.
 */
static void store_fax_row(const uint8_t *row)
{
    if (!row) {
        return;
    }

    if (!ensure_fax_buffer()) {
        return;
    }

    memcpy(
        fax_buffer +
            ((size_t)fax_buffer_head * WEFAX_WIDTH),
        row,
        WEFAX_WIDTH
    );

    fax_buffer_head =
        (fax_buffer_head + 1) % FAX_BUFFER_ROWS;

    if (fax_buffer_count < FAX_BUFFER_ROWS) {
        fax_buffer_count++;
    }

    fax_rows_total++;
}


/*
 * Save the retained full-resolution fax as an 8-bit grayscale PNG.
 *
 * The ring buffer contains original decoder rows.  Align and Tilt are
 * applied while writing, so the PNG always reflects the CURRENT controls.
 *
 * Tilt is scaled to source-row spacing so that its visual slope matches
 * the 750-pixel preview, whose vertical scale is PREVIEW_WIDTH/WEFAX_WIDTH.
 */

/*
 * Render the retained fax into the fixed 710x320 white preview canvas.
 *
 * DEFAULT keeps the existing live renderer untouched.
 * MID shows approximately twice as much vertical history.
 * FULL fits the complete retained buffer into the canvas.
 *
 * Only the drawn fax is scaled; the white canvas always remains 710x320.
 */
static void render_zoomed_fax(void)
{
    if (!fax_canvas) {
        return;
    }

    for (unsigned int i = 0;
         i < PREVIEW_WIDTH * PREVIEW_HEIGHT;
         i++) {
        preview_buffer[i] = lv_color_white();
    }

    if (!fax_buffer || fax_buffer_count == 0) {
        lv_obj_invalidate(fax_canvas);
        return;
    }

    const unsigned int first =
        (
            fax_buffer_head +
            FAX_BUFFER_ROWS -
            fax_buffer_count
        ) % FAX_BUFFER_ROWS;

    const int align =
        wefax_align;

    const int tilt =
        wefax_tilt;

    /*
     * DEFAULT is handled by rerender_preview(); this function is used
     * only for MID/FULL.
     *
     * MID: fixed 50% of the normal 710-pixel fax width.
     * FULL: scale all retained rows to fit vertically, but never enlarge
     * beyond the normal 710-pixel width.
     */
    unsigned int out_h;
    unsigned int out_w;

    out_h = PREVIEW_HEIGHT;

    out_w =
        (
            (uint64_t)WEFAX_WIDTH *
            out_h
        ) / fax_buffer_count;

    if (out_w > PREVIEW_WIDTH) {
        out_w = PREVIEW_WIDTH;
        out_h =
            (
                (uint64_t)fax_buffer_count *
                out_w
            ) / WEFAX_WIDTH;
    }

    if (out_w == 0 || out_h == 0) {
        lv_obj_invalidate(fax_canvas);
        return;
    }

    const unsigned int x0 =
        (PREVIEW_WIDTH - out_w) / 2;

    /*
     * Keep the newest part at the bottom.  FULL normally occupies the
     * complete height; shorter images sit against the bottom just like
     * the normal live preview.
     */
    const unsigned int y0 =
        PREVIEW_HEIGHT - out_h;

    /*
     * MID may need only the newest subset of retained source rows.
     * FULL always starts from the oldest retained row.
     */
    unsigned int src_rows_needed =
        (
            (uint64_t)out_h *
            WEFAX_WIDTH +
            out_w - 1
        ) / out_w;

    if (src_rows_needed > fax_buffer_count) {
        src_rows_needed = fax_buffer_count;
    }

    const unsigned int src_start =
        fax_buffer_count - src_rows_needed;

    for (unsigned int oy = 0; oy < out_h; oy++) {
        unsigned int rel_row =
            src_start +
            (
                (uint64_t)oy *
                src_rows_needed
            ) / out_h;

        if (rel_row >= fax_buffer_count) {
            rel_row = fax_buffer_count - 1;
        }

        const uint8_t *src =
            fax_buffer +
            (
                (size_t)(
                    (first + rel_row) %
                    FAX_BUFFER_ROWS
                ) *
                WEFAX_WIDTH
            );

        const int64_t row_from_first =
            (int64_t)(fax_rows_total - fax_buffer_count) +
            (int64_t)rel_row;

        const int tilt_shift =
            (int)(
                (
                    row_from_first *
                    (int64_t)tilt *
                    (int64_t)PREVIEW_WIDTH
                ) /
                (
                    1000LL *
                    (int64_t)WEFAX_WIDTH
                )
            );

        for (unsigned int ox = 0; ox < out_w; ox++) {
            int sx =
                (int)(
                    (
                        (uint64_t)ox *
                        WEFAX_WIDTH
                    ) / out_w
                ) -
                align -
                tilt_shift;

            while (sx < 0) {
                sx += WEFAX_WIDTH;
            }

            while (sx >= WEFAX_WIDTH) {
                sx -= WEFAX_WIDTH;
            }

            uint8_t gray = src[sx];

            preview_buffer[
                (y0 + oy) * PREVIEW_WIDTH +
                x0 + ox
            ] =
                lv_color_make(
                    gray,
                    gray,
                    gray
                );
        }
    }

    lv_obj_invalidate(fax_canvas);
}


static void *save_fax_worker(void *arg)
{
    wefax_save_job_t *job = (wefax_save_job_t *)arg;
    bool success = false;
    FILE *fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    uint8_t *out_row = NULL;

    if (!job) {
        goto done;
    }

    if (mkdir(FAX_SAVE_DIR, 0775) != 0 && errno != EEXIST) {
        printf("WEFAX SAVE: mkdir failed: %s\n", strerror(errno));
        goto done;
    }

    fp = fopen(job->path, "wb");
    if (!fp) {
        printf("WEFAX SAVE: fopen failed: %s\n", strerror(errno));
        goto done;
    }

    png_ptr = png_create_write_struct(
        PNG_LIBPNG_VER_STRING,
        NULL,
        NULL,
        NULL
    );

    if (!png_ptr) {
        printf("WEFAX SAVE: png_create_write_struct failed\n");
        goto done;
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        printf("WEFAX SAVE: png_create_info_struct failed\n");
        goto done;
    }

    out_row = (uint8_t *)malloc(WEFAX_WIDTH);
    if (!out_row) {
        printf("WEFAX SAVE: row allocation failed\n");
        goto done;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        printf("WEFAX SAVE: libpng write error\n");
        goto done;
    }

    png_init_io(png_ptr, fp);

    png_set_IHDR(
        png_ptr,
        info_ptr,
        WEFAX_WIDTH,
        job->rows,
        8,
        PNG_COLOR_TYPE_GRAY,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    png_write_info(png_ptr, info_ptr);

    for (unsigned int r = 0; r < job->rows; r++) {
        const uint8_t *src =
            job->image + ((size_t)r * WEFAX_WIDTH);

        const int64_t row_from_first =
            (int64_t)job->row_base + (int64_t)r;

        const int tilt_shift =
            (int)(
                (
                    row_from_first *
                    (int64_t)job->tilt *
                    (int64_t)PREVIEW_WIDTH
                ) /
                (
                    1000LL *
                    (int64_t)WEFAX_WIDTH
                )
            );

        for (unsigned int x = 0; x < WEFAX_WIDTH; x++) {
            int sx =
                (int)x -
                job->align -
                tilt_shift;

            while (sx < 0) {
                sx += WEFAX_WIDTH;
            }

            while (sx >= WEFAX_WIDTH) {
                sx -= WEFAX_WIDTH;
            }

            out_row[x] = src[sx];
        }

        png_write_row(png_ptr, (png_bytep)out_row);
    }

    png_write_end(png_ptr, NULL);

    free(out_row);
    out_row = NULL;

    png_destroy_write_struct(&png_ptr, &info_ptr);
    png_ptr = NULL;
    info_ptr = NULL;

    if (fclose(fp) != 0) {
        fp = NULL;
        remove(job->path);
        printf("WEFAX SAVE: fclose failed\n");
        goto done;
    }
    fp = NULL;

    printf(
        "WEFAX SAVE: saved %s (%u x %u)\n",
        job->path,
        (unsigned int)WEFAX_WIDTH,
        job->rows
    );

    success = true;

 done:
    if (out_row) {
        free(out_row);
    }

    if (png_ptr) {
        png_destroy_write_struct(
            &png_ptr,
            info_ptr ? &info_ptr : NULL
        );
    }

    if (fp) {
        fclose(fp);
        if (job) {
            remove(job->path);
        }
    }

    if (job) {
        free(job->image);
        free(job);
    }

    pthread_mutex_lock(&save_mutex);
    async_save_running = false;
    async_save_success = success;
    async_save_completed = true;
    pthread_mutex_unlock(&save_mutex);

    return NULL;
}


/*
 * Take a fast RAM snapshot of the retained fax and hand it to a detached
 * worker thread.  The expensive libpng compression/write never runs in
 * the LVGL thread.
 *
 * The snapshot contains rows in chronological order (oldest -> newest),
 * so the live ring buffer can be cleared/reused immediately after this
 * function returns successfully.
 */
static bool save_fax_png_async(void)
{
    if (!fax_buffer || fax_buffer_count == 0) {
        printf("WEFAX SAVE: buffer is empty\n");
        return false;
    }

    pthread_mutex_lock(&save_mutex);
    const bool busy = async_save_running;
    pthread_mutex_unlock(&save_mutex);

    if (busy) {
        printf("WEFAX SAVE: previous asynchronous save still running\n");
        return false;
    }

    wefax_save_job_t *job =
        (wefax_save_job_t *)calloc(1, sizeof(*job));

    if (!job) {
        printf("WEFAX SAVE: job allocation failed\n");
        return false;
    }

    job->rows = fax_buffer_count;
    job->row_base = fax_rows_total - fax_buffer_count;
    job->align = wefax_align;
    job->tilt = wefax_tilt;

    const size_t image_size =
        (size_t)job->rows * (size_t)WEFAX_WIDTH;

    job->image = (uint8_t *)malloc(image_size);
    if (!job->image) {
        printf(
            "WEFAX SAVE: snapshot allocation failed (%u bytes)\n",
            (unsigned int)image_size
        );
        free(job);
        return false;
    }

    const unsigned int first =
        (
            fax_buffer_head +
            FAX_BUFFER_ROWS -
            fax_buffer_count
        ) % FAX_BUFFER_ROWS;

    for (unsigned int r = 0; r < job->rows; r++) {
        const uint8_t *src =
            fax_buffer +
            (
                (size_t)(
                    (first + r) % FAX_BUFFER_ROWS
                ) *
                WEFAX_WIDTH
            );

        memcpy(
            job->image + ((size_t)r * WEFAX_WIDTH),
            src,
            WEFAX_WIDTH
        );
    }

    time_t now = time(NULL);
    struct tm tm_now;

    /* UTC, like the stations' schedules */
    if (!gmtime_r(&now, &tm_now)) {
        printf("WEFAX SAVE: gmtime failed\n");
        free(job->image);
        free(job);
        return false;
    }

    snprintf(
        job->path,
        sizeof(job->path),
        FAX_SAVE_DIR
        "/wefax-%04d%02d%02d-%02d%02d%02d.png",
        tm_now.tm_year + 1900,
        tm_now.tm_mon + 1,
        tm_now.tm_mday,
        tm_now.tm_hour,
        tm_now.tm_min,
        tm_now.tm_sec
    );

    pthread_t thread;

    pthread_mutex_lock(&save_mutex);
    async_save_running = true;
    async_save_completed = false;
    async_save_success = false;
    pthread_mutex_unlock(&save_mutex);

    const int rc = pthread_create(
        &thread,
        NULL,
        save_fax_worker,
        job
    );

    if (rc != 0) {
        pthread_mutex_lock(&save_mutex);
        async_save_running = false;
        pthread_mutex_unlock(&save_mutex);

        printf(
            "WEFAX SAVE: pthread_create failed: %s\n",
            strerror(rc)
        );

        free(job->image);
        free(job);
        return false;
    }

    pthread_detach(thread);

    msg_schedule_text_fmt("Saving fax...");

    return true;
}


/*
 * Worker completion is consumed only from the LVGL thread.  The worker
 * never calls LVGL directly.
 */
static void check_async_save_completion(void)
{
    bool completed = false;
    bool success = false;

    pthread_mutex_lock(&save_mutex);

    if (async_save_completed) {
        completed = true;
        success = async_save_success;
        async_save_completed = false;
    }

    pthread_mutex_unlock(&save_mutex);

    if (!completed) {
        return;
    }

    if (success) {
        msg_schedule_text_fmt("Fax saved");
    } else {
        msg_schedule_text_fmt("Fax save failed");
    }
}


/*
 * AUTO: APT STOP has been received.
 *
 * Save the retained full-resolution fax, then reset the receiver and
 * immediately return to AUTO waiting for the next DWD APT START.
 * On save failure the image/buffer are deliberately kept intact.
 */
static void auto_finish_fax(void)
{
    if (!decoder) {
        return;
    }

    /*
    printf("WEFAX AUTO: fax complete - saving PNG\n");
    */


    if (!save_fax_png_async()) {
        printf(
            "WEFAX AUTO: automatic save failed - "
            "fax retained in memory\n"
        );
        return;
    }

    auto_start_generation_seen = 0;
    auto_fax_confirmed_seen = false;

    /*
     * Do not reset the decoder here: this is the LVGL thread and the RX
     * audio thread may currently be inside wefax_decoder_process().
     * The audio callback will perform the AUTO reset before its next block.
     */
    atomic_store(&auto_reset_pending, true);

    clear_preview();

    /*
    printf(
        "WEFAX AUTO: saved - waiting for next APT START "
        "(300 +/- 30 Hz)\n"
    );
    */
}


static void key_cb(lv_event_t *e)
{
    uint32_t key =
        *((uint32_t *)lv_event_get_param(e));

    /*
     * The main-screen key handler is disabled while a dialog is open,
     * so WeFax must handle the dedicated volume encoder itself.
     * Keep this independent from the MFK Align/Tilt editing modes.
     */
    switch (key) {
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            return;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            return;

        default:
            break;
    }

    if (!align_mfk_active && !tilt_mfk_active &&
        !fax_browser_align_active && !fax_browser_tilt_active) {
        return;
    }

    switch (key) {
        case LV_KEY_LEFT:
            if (fax_browser_align_active && fax_browser_editing) {
                fax_browser_align -= 8;
                while (fax_browser_align < -(int)(fax_browser_image_width / 2)) {
                    fax_browser_align += (int)fax_browser_image_width;
                }
                fax_browser_render_image();
                fax_browser_update_modified_state();
            } else if (fax_browser_tilt_active && fax_browser_editing) {
                fax_browser_tilt -= 5;
                if (fax_browser_tilt < -400) fax_browser_tilt = -400;
                fax_browser_render_image();
                fax_browser_update_modified_state();
            } else if (align_mfk_active) {
                wefax_align -= 8;
                if (wefax_align < -1300) wefax_align = -1300;
                buttons_refresh(&btn_align);
                rerender_preview();
            } else if (tilt_mfk_active) {
                wefax_tilt -= 10;
                if (wefax_tilt < -400) wefax_tilt = -400;
                buttons_refresh(&btn_tilt);
                rerender_preview();
                slant_user_tilt_changed();
            }
            break;

        case LV_KEY_RIGHT:
            if (fax_browser_align_active && fax_browser_editing) {
                fax_browser_align += 8;
                while (fax_browser_align > (int)(fax_browser_image_width / 2)) {
                    fax_browser_align -= (int)fax_browser_image_width;
                }
                fax_browser_render_image();
                fax_browser_update_modified_state();
            } else if (fax_browser_tilt_active && fax_browser_editing) {
                fax_browser_tilt += 5;
                if (fax_browser_tilt > 400) fax_browser_tilt = 400;
                fax_browser_render_image();
                fax_browser_update_modified_state();
            } else if (align_mfk_active) {
                wefax_align += 8;
                if (wefax_align > 1300) wefax_align = 1300;
                buttons_refresh(&btn_align);
                rerender_preview();
            } else if (tilt_mfk_active) {
                wefax_tilt += 10;
                if (wefax_tilt > 400) wefax_tilt = 400;
                buttons_refresh(&btn_tilt);
                rerender_preview();
                slant_user_tilt_changed();
            }
            break;

        default:
            break;
    }
}


/*
 * Local WEFAX page switching.
 */
static void fax_page_1_cb(button_data_t *data)
{
    (void)data;

    fax_browser_close();

    align_mfk_active = false;
    tilt_mfk_active = false;

    buttons_load_page(
        &btn_page_1
    );
}


static void fax_page_2_cb(button_data_t *data)
{
    (void)data;

    fax_browser_close();

    buttons_load_page(
        &btn_page_2
    );
}


static void fax_page_4_cb(button_data_t *data)
{
    (void)data;

    fax_browser_close();

    align_mfk_active = false;
    tilt_mfk_active = false;

    buttons_load_page(
        &btn_page_4
    );
}


static void fax_page_3_cb(button_data_t *data)
{
    (void)data;

    align_mfk_active = false;
    tilt_mfk_active = false;

    buttons_load_page(
        &btn_page_3
    );
}




/*
 * FAX 3:3 browser/editor callbacks.
 *
 * The UI is installed first.  The browser/editor implementation is added
 * in the next step without touching the working WEFAX decoder/DSP path.
 */
static int fax_browser_name_compare(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;

    /* Newest timestamped WEFAX filename first. */
    return strcmp(*sb, *sa);
}


static void fax_browser_free_files(void)
{
    for (unsigned int i = 0; i < fax_browser_file_count; i++) {
        free(fax_browser_files[i]);
    }

    free(fax_browser_files);
    fax_browser_files = NULL;
    fax_browser_file_count = 0;
    fax_browser_file_capacity = 0;
}


static bool fax_browser_ensure_file_capacity(void)
{
    if (fax_browser_file_count < fax_browser_file_capacity) {
        return true;
    }

    unsigned int new_capacity =
        fax_browser_file_capacity == 0
            ? FAX_BROWSER_INITIAL_CAPACITY
            : fax_browser_file_capacity * 2;

    char **new_files = realloc(
        fax_browser_files,
        new_capacity * sizeof(*fax_browser_files)
    );

    if (!new_files) {
        return false;
    }

    fax_browser_files = new_files;
    fax_browser_file_capacity = new_capacity;
    return true;
}


static void fax_browser_refresh(void)
{
    fax_browser_free_files();

    DIR *dir = opendir(FAX_SAVE_DIR);
    if (!dir) {
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {

        const char *name = entry->d_name;
        size_t len = strlen(name);

        if (len < 5 || strcmp(name + len - 4, ".png") != 0) {
            continue;
        }

        if (strncmp(name, "wefax-", 6) != 0) {
            continue;
        }

        if (!fax_browser_ensure_file_capacity()) {
            break;
        }

        char *copy = strdup(name);
        if (copy) {
            fax_browser_files[fax_browser_file_count++] = copy;
        }
    }

    closedir(dir);

    qsort(
        fax_browser_files,
        fax_browser_file_count,
        sizeof(fax_browser_files[0]),
        fax_browser_name_compare
    );
}


static void fax_browser_update_modified_state(void)
{
    /* The edited fax differs from the opened PNG only if one of the
     * two local corrections is non-zero. */
    fax_browser_modified =
        (fax_browser_align != 0 || fax_browser_tilt != 0);

    btn_save_fax.label = fax_browser_modified ? "Save\nFax" : "Done";
    if (btn_save_fax.disp_btn) {
        buttons_refresh(&btn_save_fax);
    }
}


static void fax_browser_set_editor_buttons(bool enabled)
{
    if (enabled) {
        /* Saved fax preview/editor: preserve the existing Done / Save Fax
         * behaviour exactly as before. */
        fax_browser_update_modified_state();
        buttons_disabled(&btn_fix_align, false);
        buttons_disabled(&btn_fix_tilt, false);
        buttons_disabled(&btn_save_fax, false);
        return;
    }

    /* File list: F5 becomes Delete Fax. Align/Tilt stay unavailable. */
    fax_browser_modified = false;
    btn_save_fax.label = "Delete\nFax";

    buttons_disabled(&btn_fix_align, true);
    buttons_disabled(&btn_fix_tilt, true);
    buttons_disabled(&btn_save_fax, fax_browser_file_count == 0);

    if (btn_save_fax.disp_btn) {
        buttons_refresh(&btn_save_fax);
    }
}


/* Row contents for the browser list: "n | date | time". */
static void fax_browser_row_cb(void *user, int index, ui_list_row_t *row)
{
    (void)user;

    if (index < 0 || (unsigned int)index >= fax_browser_file_count) {
        return;
    }

    const char *name = fax_browser_files[index];

    snprintf(row->cell[0], UI_LIST_CELL_LEN, "%d", index + 1);

    if (!ui_list_parse_stamp(name, "wefax-",
                             row->cell[1], UI_LIST_CELL_LEN,
                             row->cell[2], UI_LIST_CELL_LEN)) {
        snprintf(row->cell[1], UI_LIST_CELL_LEN, "%s", name);
        row->cell[2][0] = '\0';
    }
}


static void fax_browser_activate_cb(void *user, int index)
{
    (void)user;

    if (index < 0 || (unsigned int)index >= fax_browser_file_count) {
        return;
    }

    if (fax_browser_open_png(fax_browser_files[index])) {
        msg_schedule_text_fmt("Editing saved fax");
    }
}


/* Tell the list how many faxes there are (selection is kept if valid). */
static void fax_browser_update_list(void)
{
    if (!fax_browser_table) {
        return;
    }

    ui_list_set_count(fax_browser_table, (int)fax_browser_file_count);
}


static void fax_browser_delete_selected(void)
{
    if (!fax_browser_open || fax_browser_editing ||
        !fax_browser_table || fax_browser_file_count == 0) {
        return;
    }

    const int row = ui_list_get_selected(fax_browser_table);

    if (row < 0 || (unsigned int)row >= fax_browser_file_count) {
        return;
    }

    const unsigned int old_row = (unsigned int)row;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s",
             FAX_SAVE_DIR, fax_browser_files[old_row]);

    if (unlink(path) != 0) {
        printf("WEFAX BROWSER: unable to delete %s: %s\n",
               path, strerror(errno));
        return;
    }

    printf("WEFAX BROWSER: deleted %s\n", path);

    /* The list keeps the same row number: the next fax moves up into it,
     * or the new last one is selected if the last fax was deleted. */
    fax_browser_refresh();
    fax_browser_update_list();

    fax_browser_set_editor_buttons(false);
    lv_group_focus_obj(fax_browser_table);
    lv_group_set_editing(keyboard_group, true);
}


static void fax_browser_free_image(void)
{
    if (fax_browser_image) {
        free(fax_browser_image);
        fax_browser_image = NULL;
    }

    fax_browser_image_width = 0;
    fax_browser_image_height = 0;
    fax_browser_image_path[0] = '\0';
    fax_browser_editing = false;
    fax_browser_align_active = false;
    fax_browser_tilt_active = false;
    fax_browser_align = 0;
    fax_browser_tilt = 0;
    fax_browser_modified = false;
}


static void fax_browser_render_image(void)
{
    if (!fax_browser_image ||
        fax_browser_image_width == 0 ||
        fax_browser_image_height == 0) {
        return;
    }

    const unsigned int width = fax_browser_image_width;
    const unsigned int height = fax_browser_image_height;

    unsigned int out_w = PREVIEW_WIDTH;
    unsigned int out_h = (unsigned int)(
        ((uint64_t)height * PREVIEW_WIDTH) / width
    );

    if (out_h > PREVIEW_HEIGHT) {
        out_h = PREVIEW_HEIGHT;
        out_w = (unsigned int)(
            ((uint64_t)width * PREVIEW_HEIGHT) / height
        );
    }

    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    const unsigned int x0 = (PREVIEW_WIDTH - out_w) / 2;
    const unsigned int y0 = (PREVIEW_HEIGHT - out_h) / 2;
    const lv_color_t black = lv_color_make(0, 0, 0);

    for (unsigned int i = 0; i < PREVIEW_WIDTH * PREVIEW_HEIGHT; i++) {
        preview_buffer[i] = black;
    }

    for (unsigned int oy = 0; oy < out_h; oy++) {
        unsigned int sy = (unsigned int)(
            ((uint64_t)oy * height) / out_h
        );
        if (sy >= height) sy = height - 1;

        const int64_t row_from_newest =
            (int64_t)sy - (int64_t)(height - 1);

        const int tilt_shift =
            (int)(
                (
                    row_from_newest *
                    (int64_t)fax_browser_tilt *
                    (int64_t)PREVIEW_WIDTH
                ) /
                (1000LL * (int64_t)width)
            );

        for (unsigned int ox = 0; ox < out_w; ox++) {
            unsigned int dx = (unsigned int)(
                ((uint64_t)ox * width) / out_w
            );
            if (dx >= width) dx = width - 1;

            int sx =
                (int)dx -
                fax_browser_align -
                tilt_shift;
            while (sx < 0) {
                sx += (int)width;
            }
            while (sx >= (int)width) {
                sx -= (int)width;
            }

            uint8_t gray = fax_browser_image[
                (size_t)sy * (size_t)width + (unsigned int)sx
            ];

            preview_buffer[(y0 + oy) * PREVIEW_WIDTH + x0 + ox] =
                lv_color_make(gray, gray, gray);
        }
    }

    if (fax_canvas) {
        lv_obj_invalidate(fax_canvas);
    }
}


static bool fax_browser_open_png(const char *filename)
{
    if (!filename || !*filename) {
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", FAX_SAVE_DIR, filename);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("WEFAX BROWSER: unable to open %s: %s\n", path, strerror(errno));
        return false;
    }

    png_structp png_ptr = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, NULL, NULL, NULL
    );

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
        fax_browser_free_image();
        printf("WEFAX BROWSER: libpng read error for %s\n", path);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = png_get_image_width(png_ptr, info_ptr);
    png_uint_32 height = png_get_image_height(png_ptr, info_ptr);
    int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);

    if (width == 0 || height == 0 ||
        width > 10000 || height > 10000) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        printf("WEFAX BROWSER: invalid PNG dimensions\n");
        return false;
    }

    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }

    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }

    /* Convert every supported saved PNG to 8-bit RGB/RGBA, then to gray. */
    color_type = png_get_color_type(png_ptr, info_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }

    png_read_update_info(png_ptr, info_ptr);

    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    int channels = png_get_channels(png_ptr, info_ptr);

    if (channels < 3 || rowbytes == 0) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        printf("WEFAX BROWSER: unsupported PNG format\n");
        return false;
    }

    uint8_t *image = (uint8_t *)malloc((size_t)width * (size_t)height);
    uint8_t *row = (uint8_t *)malloc(rowbytes);

    if (!image || !row) {
        free(image);
        free(row);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        printf("WEFAX BROWSER: image allocation failed\n");
        return false;
    }

    for (png_uint_32 y = 0; y < height; y++) {
        png_read_row(png_ptr, row, NULL);

        for (png_uint_32 x = 0; x < width; x++) {
            const uint8_t *px = row + (size_t)x * (size_t)channels;
            image[(size_t)y * (size_t)width + x] =
                (uint8_t)((77U * px[0] + 150U * px[1] + 29U * px[2]) >> 8);
        }
    }

    png_read_end(png_ptr, NULL);
    free(row);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    fax_browser_free_image();
    fax_browser_image = image;
    fax_browser_image_width = (unsigned int)width;
    fax_browser_image_height = (unsigned int)height;
    snprintf(fax_browser_image_path, sizeof(fax_browser_image_path), "%s", path);

    /* Start every saved fax with a neutral, local alignment. */
    fax_browser_align = 0;
    fax_browser_tilt = 0;
    fax_browser_align_active = false;
    fax_browser_tilt_active = false;
    fax_browser_render_image();

    if (fax_browser_table) {
        lv_group_remove_obj(fax_browser_table);
        lv_obj_add_flag(fax_browser_table, LV_OBJ_FLAG_HIDDEN);
    }

    if (fax_canvas) {
        lv_obj_clear_flag(fax_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_group_add_obj(keyboard_group, fax_canvas);
        lv_group_focus_obj(fax_canvas);
        lv_group_set_editing(keyboard_group, true);
        apply_zoom_geometry();
        lv_obj_invalidate(fax_canvas);
    }

    fax_browser_editing = true;
    fax_browser_set_editor_buttons(true);

    printf(
        "WEFAX BROWSER: opened %s (%u x %u)\n",
        path,
        fax_browser_image_width,
        fax_browser_image_height
    );

    return true;
}


static void fax_browser_build(void)
{
    if (fax_browser_open) {
        return;
    }

    fax_browser_refresh();
    fax_browser_set_editor_buttons(false);

    /*
     * The browser always needs the BIG dialog container.  If it is opened
     * from SMALL, remember SMALL and temporarily switch the real WEFAX
     * geometry/state to BIG.  Keep the View button synchronized.
     */
    fax_browser_forced_big = false;
    fax_browser_saved_zoom = zoom_mode;

    if (zoom_mode == WEFAX_ZOOM_SMALL) {
        fax_browser_forced_big = true;
        zoom_mode = WEFAX_ZOOM_BIG;
        btn_zoom.label = "View Size\nBig";
        apply_zoom_geometry();

        if (btn_zoom.disp_btn) {
            buttons_refresh(&btn_zoom);
        }
    }

    if (fax_canvas) {
        lv_obj_add_flag(fax_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_group_remove_obj(fax_canvas);
    }
    /* Hide SMALL-only decorations while the browser owns the viewport. */
    if (small_top_mask) {
        lv_obj_add_flag(small_top_mask, LV_OBJ_FLAG_HIDDEN);
    }
    /* Browser always uses the already validated BIG/FULL geometry. */
    ui_list_cfg_t cfg = {
        .width = PREVIEW_WIDTH,
        .height = 270,
        .col_count = 3,
        .cols = {
            { .x = 12,  .w = 44,  .align = UI_LIST_ALIGN_RIGHT, .muted = true },
            { .x = 76,  .w = 190, .align = UI_LIST_ALIGN_LEFT },
            { .x = 290, .w = 220, .align = UI_LIST_ALIGN_LEFT },
        },
        .row_cb = fax_browser_row_cb,
        .activate_cb = fax_browser_activate_cb,
        .empty_text = "No saved faxes",
        .vol_coarse = true,
    };

    fax_browser_table = ui_list_create(dialog.obj, &cfg);
    lv_obj_align(fax_browser_table, LV_ALIGN_TOP_MID, 0, 40);

    fax_browser_update_list();

    lv_group_add_obj(
        keyboard_group,
        fax_browser_table
    );

    lv_group_focus_obj(fax_browser_table);
    lv_group_set_editing(keyboard_group, true);

    fax_browser_open = true;
    fax_browser_set_editor_buttons(false);
    btn_fax_browser.label = "Close\nBrowser";
    if (btn_fax_browser.disp_btn) {
        buttons_refresh(&btn_fax_browser);
    }
}


static void fax_browser_close(void)
{
    if (fax_browser_table) {
        lv_group_remove_obj(fax_browser_table);
        lv_obj_del(fax_browser_table);
        fax_browser_table = NULL;
    }

    fax_browser_free_files();
    fax_browser_set_editor_buttons(false);
    fax_browser_free_image();
    fax_browser_open = false;

    /* Outside the browser preserve the original FAX 3:3 appearance. */
    btn_save_fax.label = "Done";
    buttons_disabled(&btn_save_fax, true);
    if (btn_save_fax.disp_btn) {
        buttons_refresh(&btn_save_fax);
    }

    btn_fax_browser.label = "Fax\nBrowser";

    /*
     * If the browser temporarily promoted SMALL to BIG, restore both the
     * original zoom state and the View button before rebuilding the preview.
     * This path is shared by Close Browser and by page changes.
     */
    if (fax_browser_forced_big) {
        zoom_mode = fax_browser_saved_zoom;
        btn_zoom.label = "View Size\nSmall";
        fax_browser_forced_big = false;

        apply_zoom_geometry();

        if (btn_zoom.disp_btn) {
            buttons_refresh(&btn_zoom);
        }
    }

    /* Restore the live WEFAX preview overwritten by the saved-fax image. */
    rerender_preview();

    if (fax_canvas) {
        lv_obj_clear_flag(fax_canvas, LV_OBJ_FLAG_HIDDEN);

        /*
         * Re-apply the CURRENT zoom layout after leaving the browser.
         * This is essential for SMALL: fax_browser_build() deliberately
         * hides its top mask/fillets, so simply unhiding the canvas would
         * restore the old zero-top-margin appearance until the next Zoom.
         */
        apply_zoom_geometry();

        lv_group_add_obj(keyboard_group, fax_canvas);
        lv_group_focus_obj(fax_canvas);
        lv_group_set_editing(keyboard_group, true);

        /* Redraw the complete viewport, including the area previously
         * occupied by the browser table, so no browser background remains
         * visible in SMALL's top margin. */
        lv_obj_invalidate(dialog.obj);
        lv_obj_invalidate(fax_canvas);
    }
}


static void fax_browser_cb(button_data_t *data)
{
    (void)data;

    if (fax_browser_open) {
        fax_browser_close();
        if (btn_fax_browser.disp_btn) {
            buttons_refresh(&btn_fax_browser);
        }
        return;
    }

    fax_browser_build();
}


static void fix_align_cb(button_data_t *data)
{
    (void)data;

    if (!fax_browser_open || !fax_browser_editing || !fax_browser_image) {
        return;
    }

    fax_browser_align_active = true;
    fax_browser_tilt_active = false;
    align_mfk_active = false;
    tilt_mfk_active = false;

    if (fax_canvas) {
        lv_group_focus_obj(fax_canvas);
        lv_group_set_editing(keyboard_group, true);
    }
}


static void fix_tilt_cb(button_data_t *data)
{
    (void)data;

    if (!fax_browser_open || !fax_browser_editing || !fax_browser_image) {
        return;
    }

    fax_browser_tilt_active = true;
    fax_browser_align_active = false;
    align_mfk_active = false;
    tilt_mfk_active = false;

    if (fax_canvas) {
        lv_group_focus_obj(fax_canvas);
        lv_group_set_editing(keyboard_group, true);
    }
}


/*
 * Save the edited browser image back to the SAME PNG filename.
 * A temporary file is written first and atomically renamed only after
 * libpng and fclose() have both completed successfully.
 */
static bool fax_browser_save_edited_png(void)
{
    if (!fax_browser_image ||
        fax_browser_image_width == 0 ||
        fax_browser_image_height == 0 ||
        fax_browser_image_path[0] == '\0') {
        return false;
    }

    char temp_path[sizeof(fax_browser_image_path) + 8];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", fax_browser_image_path);

    FILE *fp = fopen(temp_path, "wb");
    if (!fp) {
        printf("WEFAX BROWSER SAVE: fopen failed: %s\n", strerror(errno));
        return false;
    }

    png_structp png_ptr = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, NULL, NULL, NULL
    );
    if (!png_ptr) {
        fclose(fp);
        remove(temp_path);
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        remove(temp_path);
        return false;
    }

    uint8_t *out_row = (uint8_t *)malloc(fax_browser_image_width);
    if (!out_row) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        remove(temp_path);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        free(out_row);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        remove(temp_path);
        printf("WEFAX BROWSER SAVE: libpng write error\n");
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(
        png_ptr,
        info_ptr,
        fax_browser_image_width,
        fax_browser_image_height,
        8,
        PNG_COLOR_TYPE_GRAY,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );
    png_write_info(png_ptr, info_ptr);

    const unsigned int width = fax_browser_image_width;
    const unsigned int height = fax_browser_image_height;

    for (unsigned int y = 0; y < height; y++) {
        const int64_t row_from_newest =
            (int64_t)y - (int64_t)(height - 1);

        const int tilt_shift =
            (int)(
                (
                    row_from_newest *
                    (int64_t)fax_browser_tilt *
                    (int64_t)PREVIEW_WIDTH
                ) /
                (1000LL * (int64_t)width)
            );

        const uint8_t *src =
            fax_browser_image + (size_t)y * (size_t)width;

        for (unsigned int x = 0; x < width; x++) {
            int sx = (int)x - fax_browser_align - tilt_shift;

            while (sx < 0) sx += (int)width;
            while (sx >= (int)width) sx -= (int)width;

            out_row[x] = src[(unsigned int)sx];
        }

        png_write_row(png_ptr, (png_bytep)out_row);
    }

    png_write_end(png_ptr, NULL);
    free(out_row);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    if (fclose(fp) != 0) {
        remove(temp_path);
        printf("WEFAX BROWSER SAVE: fclose failed\n");
        return false;
    }

    if (rename(temp_path, fax_browser_image_path) != 0) {
        remove(temp_path);
        printf("WEFAX BROWSER SAVE: rename failed: %s\n", strerror(errno));
        return false;
    }

    printf(
        "WEFAX BROWSER SAVE: saved %s (%u x %u), align=%d tilt=%d\n",
        fax_browser_image_path,
        width,
        height,
        fax_browser_align,
        fax_browser_tilt
    );

    return true;
}


static void fax_browser_return_to_list(void)
{
    fax_browser_set_editor_buttons(false);

    if (fax_canvas) {
        lv_group_remove_obj(fax_canvas);
        lv_obj_add_flag(fax_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    fax_browser_free_image();
    fax_browser_refresh();

    if (fax_browser_table) {
        lv_obj_clear_flag(fax_browser_table, LV_OBJ_FLAG_HIDDEN);

        fax_browser_update_list();
        fax_browser_set_editor_buttons(false);

        lv_group_add_obj(keyboard_group, fax_browser_table);
        lv_group_focus_obj(fax_browser_table);
        lv_group_set_editing(keyboard_group, true);
        lv_obj_invalidate(fax_browser_table);
    }
}


static void save_fax_delayed_cb(lv_timer_t *timer)
{
    /* One-shot timer: delete it before doing the synchronous PNG write. */
    lv_timer_del(timer);
    save_fax_timer = NULL;

    if (!fax_browser_open || !fax_browser_editing || !fax_browser_image) {
        return;
    }

    if (!fax_browser_save_edited_png()) {
        return;
    }

    fax_browser_return_to_list();
    msg_schedule_text_fmt("Fax saved");
}


static void save_fax_cb(button_data_t *data)
{
    (void)data;

    /* In the browser file list F5 is Delete Fax. */
    if (fax_browser_open && !fax_browser_editing) {
        fax_browser_delete_selected();
        return;
    }

    if (!fax_browser_open || !fax_browser_editing || !fax_browser_image) {
        return;
    }

    /* With no effective Align/Tilt correction this button is "Done":
     * simply leave the preview and return to the file list. */
    fax_browser_update_modified_state();
    if (!fax_browser_modified) {
        fax_browser_return_to_list();
        return;
    }

    /* Avoid scheduling the same save twice. */
    if (save_fax_timer) {
        return;
    }

    /*
     * Show immediate feedback, then let the current LVGL event return.
     * The actual synchronous PNG write starts on the next timer pass,
     * giving LVGL a chance to draw the overlay first.
     */
    msg_schedule_text_fmt("Saving fax...");
    save_fax_timer = lv_timer_create(save_fax_delayed_cb, 50, NULL);
}


/*
 * FAX 2:2 placeholders.
 */
static const char *align_label_getter(void)
{
    static char buf[24];

    snprintf(
        buf,
        sizeof(buf),
        "Align\n%+i",
        wefax_align
    );

    return buf;
}


static void align_cb(button_data_t *data)
{
    (void)data;

    align_mfk_active = true;
    tilt_mfk_active = false;


}


static const char *tilt_label_getter(void)
{
    static char buf[24];

    snprintf(
        buf,
        sizeof(buf),
        "Tilt\n%+i",
        wefax_tilt
    );

    return buf;
}


static void tilt_cb(button_data_t *data)
{
    (void)data;

    align_mfk_active = false;
    tilt_mfk_active = true;


}


static void save_cb(button_data_t *data)
{
    (void)data;

    if (!decoder) {
        return;
    }


    /*
     * Snapshot the fax and start the PNG write in a worker thread.
     * Only after the snapshot has been handed off successfully do we
     * reset the decoder and clear the live buffer.
     */
    if (save_fax_png_async()) {
        /* saved: clear the picture and stop the decoder (Manual or Auto) */
        atomic_store(&auto_reset_pending, false);
        wefax_decoder_set_mode(decoder, WEFAX_MODE_IDLE);
        auto_start_generation_seen = 0;
        auto_fax_confirmed_seen = false;
        clear_preview();
        rx_buttons_sync(false);
        msg_schedule_text_fmt("Decoder stopped");
    }
}


/*
 * F2 on FAX 1:2 - Close.
 */
static void close_cb(button_data_t *data)
{
    (void)data;


    dialog_destruct();
}


/*
 * F3 on FAX 1:2 - Continuous mode.
 */
static void cont_cb(button_data_t *data)
{
    (void)data;

    if (!decoder) {
        return;
    }

    /* Stop Manual RX: the decoder stops, the picture stays */
    if (wefax_decoder_get_mode(decoder) == WEFAX_MODE_CONT) {
        wefax_decoder_set_mode(decoder, WEFAX_MODE_IDLE);
        rx_buttons_sync(false);
        msg_schedule_text_fmt("Manual decoder stopped");
        printf("WEFAX: CONT mode stopped\n");
        return;
    }

    /* Auto RX running: Start Manual RX is disabled */
    if (wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO) {
        return;
    }

    /* Start Manual RX: starts on a clean canvas */
    atomic_store(&auto_reset_pending, false);
    auto_start_generation_seen = 0;
    auto_fax_confirmed_seen = false;

    msg_schedule_text_fmt("Manual decoder started");




    wefax_decoder_set_mode(
        decoder,
        WEFAX_MODE_CONT
    );


    clear_preview();
    rx_buttons_sync(false);


    printf(
        "WEFAX: CONT mode started\n"
    );
}


/*
 * F4 on FAX 1:2 - Automatic mode.
 */
static void zoom_cb(button_data_t *data)
{
    (void)data;

    if (zoom_mode == WEFAX_ZOOM_SMALL) {
        zoom_mode = WEFAX_ZOOM_BIG;
        btn_zoom.label = "View Size\nBig";
    } else if (zoom_mode == WEFAX_ZOOM_BIG) {
        zoom_mode = WEFAX_ZOOM_FULL;
        btn_zoom.label = "View Size\nFull";
    } else {
        zoom_mode = WEFAX_ZOOM_SMALL;
        btn_zoom.label = "View Size\nSmall";
    }

    apply_zoom_geometry();
    buttons_load_page(&btn_page_2);
    rerender_preview();
}

static void auto_cb(button_data_t *data)
{
    (void)data;

    if (!decoder) {
        return;
    }

    /* Stop Auto RX: abort the automatic session, the picture stays */
    if (wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO) {
        atomic_store(&auto_reset_pending, false);
        wefax_decoder_set_mode(decoder, WEFAX_MODE_IDLE);
        auto_start_generation_seen = 0;
        auto_fax_confirmed_seen = false;
        rx_buttons_sync(false);
        msg_schedule_text_fmt("Auto decoder stopped");
        printf("WEFAX AUTO: stopped by the user\n");
        return;
    }

    /* Manual RX running: Start Auto RX is disabled */
    if (wefax_decoder_get_mode(decoder) == WEFAX_MODE_CONT) {
        return;
    }

    /* Start Auto RX: starts on a clean canvas */
    auto_start_generation_seen = 0;

    msg_schedule_text_fmt("Awaiting WEFAX header...");


    wefax_decoder_set_mode(
        decoder,
        WEFAX_MODE_AUTO
    );

    auto_fax_confirmed_seen = false;
    clear_preview();
    rx_buttons_sync(false);

    printf(
        "WEFAX AUTO: waiting for APT START "
        "(300 +/- 30 Hz)\n"
    );
}


/*
 * F5 on FAX 1:2 - Clear.
 */
static void clear_cb(button_data_t *data)
{
    (void)data;

    if (!decoder) {
        return;
    }


    printf(
        "WEFAX: samples=%llu blocks=%llu "
        "rows=%u\n",

        (unsigned long long)
            wefax_decoder_sample_count(
                decoder
            ),

        (unsigned long long)
            wefax_decoder_block_count(
                decoder
            ),

        (unsigned int)
            wefax_decoder_row_count(
                decoder
            )
    );


    /* Disabled while Auto RX is on: an automatic fax must not be wiped */
    if (wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO) {
        rx_buttons_sync(true);
        return;
    }


    /*
     * CLEAR has different semantics in CONT and AUTO.
     *
     * CONT: preserve the existing behaviour.  Clear the current image,
     * reset the decoder state and keep decoding continuously.
     *
     * AUTO: CLEAR is a real STOP.  Abort any current/provisional fax,
     * reset the complete AUTO state machine and leave the decoder IDLE.
     * The user must press AUTO again to start a new automatic session.
     */
    if (wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO) {
        atomic_store(&auto_reset_pending, false);

        wefax_decoder_set_mode(
            decoder,
            WEFAX_MODE_IDLE
        );

        auto_start_generation_seen = 0;
        auto_fax_confirmed_seen = false;

        clear_preview();

        printf(
            "WEFAX AUTO: stopped and reset by RESET button\n"
        );

        msg_schedule_text_fmt(
            "Decoder stopped, standing by..."
        );

        return;
    }

    /* Existing CONT/IDLE CLEAR behaviour. */
    wefax_decoder_reset(decoder);
    auto_start_generation_seen = 0;
    auto_fax_confirmed_seen = false;

    clear_preview();

    msg_schedule_text_fmt(
        "Image cleared"
    );
}


/*
 * Start/Stop buttons and Clear follow the decoder mode (IDLE / CONT /
 * AUTO), which only the buttons above change.  rx_buttons_sync() is also
 * called 4 times a second (dialog_wefax_freq.inc) as a safety net.
 */
static int rx_shown_mode = -1;

static const char *cont_label_getter(void)
{
    return (decoder && wefax_decoder_get_mode(decoder) == WEFAX_MODE_CONT)
        ? "Stop\nManual RX" : "Start\nManual RX";
}

static const char *auto_label_getter(void)
{
    return (decoder && wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO)
        ? "Stop\nAuto RX" : "Start\nAuto RX";
}

static void rx_buttons_sync(bool force)
{
    int mode = decoder ? (int)wefax_decoder_get_mode(decoder) : (int)WEFAX_MODE_IDLE;

    if (!force && mode == rx_shown_mode) {
        return;
    }
    rx_shown_mode = mode;

    /* one decoder at a time: while one runs, the other cannot be started */
    buttons_disabled(&btn_clear, mode == WEFAX_MODE_AUTO);
    buttons_disabled(&btn_cont, mode == WEFAX_MODE_AUTO);
    buttons_disabled(&btn_auto, mode == WEFAX_MODE_CONT);
    if (btn_cont.disp_btn) buttons_refresh(&btn_cont);
    if (btn_auto.disp_btn) buttons_refresh(&btn_auto);
}




/*
 * RX audio callback.
 */
static void audio_cb(
    size_t n,
    float *samples)
{
    if (!decoder) {
        return;
    }




    /*
     * Complete an AUTO STOP reset in the same thread that owns normal
     * decoder processing.  Keep auto_reset_pending true until the reset is
     * finished so the GUI cannot inspect the old STOP/generation state.
     *
     * If the user switched mode meanwhile (for example AUTO -> CONT), do
     * not force the receiver back to AUTO.
     */
    if (atomic_load(&auto_reset_pending)) {
        if (wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO) {
            wefax_decoder_set_mode(
                decoder,
                WEFAX_MODE_AUTO
            );
        }

        atomic_store(&auto_reset_pending, false);
    }


    wefax_decoder_process(
        decoder,
        samples,
        n
    );
}




/*
 * Complete WEFAX row callback.
 *
 * Called from audio processing context.
 * No LVGL calls here.
 */
static void row_cb(
    const uint8_t *row,
    unsigned int width,
    unsigned int row_number,
    void *user_data)
{
    (void)row_number;
    (void)user_data;


    if (!row) {
        return;
    }


    if (width != WEFAX_WIDTH) {
        return;
    }


    /*
     * AUTO provisional rows are decoder-internal only.  They are still
     * generated and analysed for the four-row black-band confirmation,
     * but are not forwarded to the GUI/PNG buffer until the fax is real.
     */
    if (
        decoder &&
        wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO &&
        !wefax_decoder_auto_fax_confirmed(decoder)
    ) {
        return;
    }


    pthread_mutex_lock(&row_mutex);


    memcpy(
        pending_row,
        row,
        WEFAX_WIDTH
    );


    pending_row_valid = true;


    pthread_mutex_unlock(&row_mutex);
}


/*
 * LVGL timer callback.
 *
 * Runs in GUI/LVGL context.
 */
static void update_preview_cb(
    lv_timer_t *timer)
{
    (void)timer;

    /* Consume asynchronous PNG completion in LVGL context. */
    check_async_save_completion();

    /* Auto Slant: measured slant, delayed save of the Tilt per station */
    slant_poll();

    if (!fax_canvas) {
        return;
    }


    /*
     * A completed AUTO fax is waiting for the audio thread to reset the
     * decoder.  Until that reset is complete, its STOP flag and START
     * generation still belong to the previous fax and must not be handled
     * again by the GUI.
     */
    if (atomic_load(&auto_reset_pending)) {
        return;
    }


    /*
     * A provisional APT START is detected in the audio/decoder thread.
     * Handle its visual/storage reset here, in LVGL context.  The decoder
     * may detect several STARTs during the real APT sequence; each one
     * deliberately replaces any previous provisional fax.
     */
    if (
        decoder &&
        wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO
    ) {
        const uint32_t generation =
            wefax_decoder_auto_start_generation(decoder);

        if (generation != auto_start_generation_seen) {
            if (auto_fax_confirmed_seen) {
                /*
                 * The decoder has confirmed a complete new START while an
                 * AUTO fax was already in progress (300 Hz sequence plus
                 * four black confirmation bands).  The old fax is
                 * deliberately discarded; do not save it and do not reset
                 * the decoder from the GUI thread.
                 */
                clear_preview();
                msg_schedule_text_fmt("Fax restarted");
                slant_fax_started();
            }
            else {
                /* Normal provisional first START. */
                auto_fax_confirmed_seen = false;
                clear_preview();
            }

            auto_start_generation_seen = generation;
        }
    }


    /*
     * Keep AUTO visually silent while START is only provisional.  When
     * the decoder confirms the real fax from four consecutive black rows,
     * clear once more and begin accepting/displaying subsequent rows.
     */
    if (
        decoder &&
        wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO &&
        wefax_decoder_auto_fax_confirmed(decoder) &&
        !auto_fax_confirmed_seen
    ) {
        auto_fax_confirmed_seen = true;

        /*
        printf(
            "WEFAX AUTO: GUI enabled after FAX CONFIRMED\n"
        );
        */

        clear_preview();
        if (!slant_fax_started()) {
            msg_schedule_text_fmt("Fax started");
        }
    }


    /*
     * APT STOP is detected in the decoder/audio path.  Perform file I/O
     * here in the LVGL thread, never inside the audio callback.
     */
    if (
        decoder &&
        wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO &&
        wefax_decoder_auto_stop_detected(decoder)
    ) {
        auto_finish_fax();
        return;
    }


    uint8_t row[WEFAX_WIDTH];


    pthread_mutex_lock(&row_mutex);


    if (!pending_row_valid) {

        pthread_mutex_unlock(&row_mutex);

        return;
    }


    memcpy(
        row,
        pending_row,
        WEFAX_WIDTH
    );


    pending_row_valid = false;


    pthread_mutex_unlock(&row_mutex);


    /*
     * Keep every original decoder row for full-resolution Save.
     * This happens before preview vertical subsampling.
     */
    store_fax_row(row);


    /*
     * AUTO safety net: once the full-resolution buffer reaches its maximum
     * capacity, the fax is certainly beyond the supported page length.
     * Save now instead of letting the ring buffer overwrite its first rows.
     */
    if (
        decoder &&
        wefax_decoder_get_mode(decoder) == WEFAX_MODE_AUTO &&
        auto_fax_confirmed_seen &&
        fax_buffer_count >= FAX_BUFFER_ROWS
    ) {
        printf(
            "WEFAX AUTO: fax buffer full (%u rows) - forcing save\n",
            fax_buffer_count
        );
        auto_finish_fax();
        return;
    }


    /*
     * --------------------------------------------------------
     * Vertical scaling
     * --------------------------------------------------------
     *
     * Horizontal scale is:
     *
     *     PREVIEW_WIDTH / WEFAX_WIDTH
     *
     * Apply exactly the same scale vertically.
     */
    vertical_accumulator += PREVIEW_WIDTH;


    if (vertical_accumulator < WEFAX_WIDTH) {

        /*
         * This source WEFAX row does not produce
         * an LCD row.
         */
        return;
    }


    vertical_accumulator -= WEFAX_WIDTH;


    /*
     * Save the ORIGINAL unaligned row before applying Align.  Only rows
     * that actually become LCD rows are stored, so this ring buffer maps
     * exactly to the visible 275-row preview.
     */
    memcpy(
        preview_source_rows[preview_source_head],
        row,
        WEFAX_WIDTH
    );

    preview_source_index[preview_source_head] =
        preview_row_counter++;

    preview_source_head =
        (preview_source_head + 1) % PREVIEW_HEIGHT;

    if (preview_source_count < PREVIEW_HEIGHT) {
        preview_source_count++;
    }


    /*
     * While the Fax Browser is open we must keep the LIVE preview history
     * up to date, but we must not draw it over the browser/saved-fax image.
     *
     * At this point the received row has already been stored in the full
     * resolution fax buffer, the vertical preview phase has advanced, and
     * every row that belongs to the 710x320 preview has been copied into
     * preview_source_rows.  Therefore rerender_preview() can reconstruct the
     * current live decoder view immediately when the browser is closed.
     */
    if (fax_browser_open) {
        return;
    }


    /*
     * Apply current Align to the working row used by the normal
     * progressive renderer.
     */
    {
        uint8_t aligned_row[WEFAX_WIDTH];
        int align = wefax_align;

        for (unsigned int x = 0; x < WEFAX_WIDTH; x++) {
            int sx = (int)x - align;

            if (sx < 0) {
                sx += WEFAX_WIDTH;
            } else if (sx >= WEFAX_WIDTH) {
                sx -= WEFAX_WIDTH;
            }

            aligned_row[x] = row[sx];
        }

        memcpy(row, aligned_row, WEFAX_WIDTH);
    }

    /*
     * Scroll existing preview one LCD row upward.
     */
    memmove(
        preview_buffer,
        preview_buffer + PREVIEW_WIDTH,
        sizeof(lv_color_t) *
            PREVIEW_WIDTH *
            (PREVIEW_HEIGHT - 1)
    );


    /*
     * Render the selected WEFAX row at the bottom.
     *
     * 1809 source pixels -> 710 LCD pixels.
     */
    lv_color_t *dst =
        preview_buffer +
        (
            (PREVIEW_HEIGHT - 1) *
            PREVIEW_WIDTH
        );


    for (
        unsigned int x = 0;
        x < PREVIEW_WIDTH;
        x++
    ) {

        unsigned int source_start =
            (
                x *
                WEFAX_WIDTH
            ) /
            PREVIEW_WIDTH;


        unsigned int source_end =
            (
                (x + 1) *
                WEFAX_WIDTH
            ) /
            PREVIEW_WIDTH;


        if (source_end <= source_start) {
            source_end = source_start + 1;
        }


        if (source_end > WEFAX_WIDTH) {
            source_end = WEFAX_WIDTH;
        }


        unsigned int sum = 0;
        unsigned int count = 0;


        for (
            unsigned int sx = source_start;
            sx < source_end;
            sx++
        ) {

            sum += row[sx];
            count++;
        }


        uint8_t gray = 0;


        if (count > 0) {

            gray =
                (uint8_t)(
                    sum / count
                );
        }


        dst[x] =
            lv_color_make(
                gray,
                gray,
                gray
            );
    }


    /*
     * Rebuild from the ORIGINAL source-row history after every accepted
     * LCD row.  This is essential for Tilt: the correction depends on
     * each row's vertical position relative to the newest row, therefore
     * all visible rows must be re-evaluated as the fax grows.
     *
     * It also means a persisted Tilt value is effective immediately:
     * no MFK movement is required to "activate" it.
     *
     * rerender_preview() automatically selects Default or Full.
     */
    rerender_preview();
}


/* Stations list and schedules ("Freq.", FAX 1:4) */
#include "dialog_wefax_freq.inc"
#include "dialog_wefax_slant.inc"
