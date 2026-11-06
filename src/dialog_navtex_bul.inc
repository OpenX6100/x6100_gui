/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 * See NOTICE-1KO125.md.
 */
/*
 * Xiegu X6100 - NAVTEX bulletins, included by dialog_navtex.c.
 *
 * Mode NAVTEX: the decoded characters (with their reliability, see
 * navtex/decoder.h) are assembled into bulletins (navtex/bulletin.c),
 * saved in NAVTEX_SAVE_DIR and shown as a grid of cards, newest first.
 * A card opens the bulletin: header with subject, station, date and
 * quality, then the text with the uncertain characters, the lost ones,
 * the positions and the digits put back after a lost FIGS shift in colour.
 *
 * Mode SITORB: the plain text view of the original app.  Bulletins are
 * assembled in both modes; the mode only changes what is shown.
 */

#include "navtex/bulletin.h"

#define NB_PREFIX           "bul-"
#define NB_EXT              ".txt"
#define NB_MODE_FILE        NAVTEX_SAVE_DIR "/mode.txt"
#define NB_STORE_MAX        100             /* bulletins kept in the list */
#define NB_KEEP_FILES       300             /* bulletins kept on the SD card: older ones */
                                            /* are deleted, except SAR / piracy (D) */
#define NB_FIFO_LEN         2048
#define NB_W                795
#define NB_H                350
#define NB_COLS             2
#define NB_ROWS             3
#define NB_GAP              6
#define NB_HDR_H            84              /* detail header */
#define NB_SCROLL_PX        52              /* detail: 2 lines per MFK step */

/* colours (as DSC) */
#define NB_C_PANEL          0x1c2229
#define NB_C_PANEL_SEL      0x222932
#define NB_C_LINE           0x343d48
#define NB_C_TEXT           0xe8edf2
#define NB_C_SUB            0x96a3b0
#define NB_C_SELECT         0x5aaaff
#define NB_C_OK             0x3aaa6e
#define NB_C_WARN           0xe2b028
#define NB_C_BAD            0xe04848
#define NB_C_SITOR          0x8a5cd0    /* SITOR-B message without ZCZC / NNNN */

/* text colours in the bulletin (LVGL recolour, no '#') */
#define NB_T_GUESS          "e2b028"        /* reconstructed character */
#define NB_T_LOST           "ff5a5a"        /* lost character '*' */
#define NB_T_COORD          "6ad0ff"        /* position */
#define NB_T_FIXED          "5ee08a"        /* digit put back */

typedef struct {
    nb_bulletin_t b;
    char file[64];
} nav_item_t;

static bool nav_mode = true;                /* true: NAVTEX bulletins, false: SITOR-B text */
static nb_assembler_t *nav_asm = NULL;
static nav_item_t *nav_items[NB_STORE_MAX]; /* newest first */
static int nav_count = 0;
static int nav_sel = 0, nav_top = 0;
static bool nav_detail = false;
static bool nav_receiving = false;          /* a bulletin is being received */

static lv_obj_t *nav_frame = NULL;
static lv_obj_t *nav_list = NULL;
static lv_obj_t *nav_hdr = NULL;
static lv_obj_t *nav_body = NULL;
static lv_obj_t *nav_body_label = NULL;
static char *nav_body_text = NULL;

/* audio thread -> UI */
typedef struct { int16_t c; int8_t q; } nav_ev_t;
static pthread_mutex_t nav_fifo_mutex = PTHREAD_MUTEX_INITIALIZER;
static nav_ev_t nav_fifo[NB_FIFO_LEN];
static int nav_fifo_head = 0, nav_fifo_count = 0;

static void nav_show_list(void);
static void mode_cb(button_data_t *data);

/* ---- buttons -------------------------------------------------------------------- */

static void nav_page2_cb(button_data_t *d);
static void nav_page1_cb(button_data_t *d);
static void nav_open_cb(button_data_t *d);
static void nav_delete_cb(button_data_t *d);
static void nav_back_cb(button_data_t *d);
static void nav_newer_cb(button_data_t *d);
static void nav_older_cb(button_data_t *d);

static button_data_t btn_nav_p1    = { .type = BTN_TEXT, .label = "(NAVTEX 1:2)", .press = nav_page2_cb };
static button_data_t btn_nav_mode  = { .type = BTN_TEXT, .label = "Mode:\nNAVTEX", .press = mode_cb };
static button_data_t btn_nav_open  = { .type = BTN_TEXT, .label = "Open", .press = nav_open_cb };
static button_data_t btn_nav_del   = { .type = BTN_TEXT, .label = "Delete", .press = nav_delete_cb };
static button_data_t btn_nav_p2    = { .type = BTN_TEXT, .label = "(NAVTEX 2:2)", .press = nav_page1_cb };
static button_data_t btn_nav_pd    = { .type = BTN_TEXT, .label = "(Bulletin)", .press = NULL };
static button_data_t btn_nav_back  = { .type = BTN_TEXT, .label = "Back", .press = nav_back_cb };
static button_data_t btn_nav_newer = { .type = BTN_TEXT, .label = "Newer", .press = nav_newer_cb };
static button_data_t btn_nav_older = { .type = BTN_TEXT, .label = "Older", .press = nav_older_cb };

static buttons_page_t nav_page_1 = { .items = { &btn_nav_p1, &btn_nav_mode, &btn_nav_open, &btn_nav_del, &btn_freq } };
static buttons_page_t nav_page_2 = { .items = { &btn_nav_p2, NULL, &btn_scan, &btn_info, &btn_close } };
static buttons_page_t nav_page_detail = { .items = { &btn_nav_pd, &btn_nav_back, &btn_nav_newer, &btn_nav_older, &btn_nav_del } };

/* ---- decoder side ------------------------------------------------------------- */

static void nav_char_q_cb(int c, int quality, void *user)
{
    (void)user;
    pthread_mutex_lock(&nav_fifo_mutex);
    if (nav_fifo_count < NB_FIFO_LEN) {
        nav_ev_t *e = &nav_fifo[(nav_fifo_head + nav_fifo_count) % NB_FIFO_LEN];
        e->c = (int16_t)c;
        e->q = (int8_t)quality;
        nav_fifo_count++;
    }
    pthread_mutex_unlock(&nav_fifo_mutex);
}

/* ---- store ------------------------------------------------------------------------- */

static bool nav_ensure_dir(void)
{
    struct stat st;
    if (stat(NAVTEX_SAVE_DIR, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(NAVTEX_SAVE_DIR, 0755) == 0;
}

static void nav_save(nav_item_t *it)
{
    if (!nav_ensure_dir()) return;
    if (!it->file[0]) {
        struct tm tm;
        time_t t = it->b.first_rx ? it->b.first_rx : time(NULL);
        gmtime_r(&t, &tm);
        char ts[24];
        strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
        snprintf(it->file, sizeof(it->file), NB_PREFIX "%s-%c%c%02d" NB_EXT, ts,
                 it->b.b1 ? it->b.b1 : 'X', it->b.b2 ? it->b.b2 : 'X', it->b.num >= 0 ? it->b.num : 99);
    }
    char path[160];
    snprintf(path, sizeof(path), NAVTEX_SAVE_DIR "/%s", it->file);
    if (!nb_save(&it->b, path)) fprintf(stderr, "NAVTEX: cannot write %s\n", path);
}

static void nav_move_to_top(int i)
{
    nav_item_t *it = nav_items[i];
    memmove(&nav_items[1], &nav_items[0], sizeof(nav_items[0]) * (size_t)i);
    nav_items[0] = it;
}

/* a finished bulletin (assembler callback, UI thread) */
static void nav_done_cb(const nb_bulletin_t *b, void *user)
{
    (void)user;

    /* received again: improve the stored copy */
    for (int i = 0; i < nav_count; ++i) {
        if (nb_merge(&nav_items[i]->b, b)) {
            nav_save(nav_items[i]);
            nav_move_to_top(i);
            fprintf(stderr, "NAVTEX: %c%c%02d received again (%d copies, quality %d%%)\n",
                    b->b1 ? b->b1 : '?', b->b2 ? b->b2 : '?', b->num >= 0 ? b->num : 99,
                    nav_items[0]->b.receptions, nb_quality(&nav_items[0]->b));
            goto shown;
        }
    }

    nav_item_t *it = malloc(sizeof(*it));
    if (!it) return;
    it->b = *b;
    it->file[0] = '\0';
    nav_save(it);
    if (nav_count == NB_STORE_MAX) free(nav_items[--nav_count]);    /* the file stays */
    memmove(&nav_items[1], &nav_items[0], sizeof(nav_items[0]) * (size_t)nav_count);
    nav_items[0] = it;
    nav_count++;
    fprintf(stderr, "NAVTEX: %s %c%c%02d, %d chars, quality %d%%%s%s\n", b->sitor ? "SITOR-B message" : "bulletin",
            b->b1 ? b->b1 : '?', b->b2 ? b->b2 : '?', b->num >= 0 ? b->num : 99, b->len,
            nb_quality(b), b->lost_head ? ", start lost" : "", b->lost_tail ? ", end lost" : "");
    if (nav_mode && !nav_detail) {
        if (b->sitor)
            msg_update_text_fmt("SITOR-B message");
        else
            msg_update_text_fmt("NAVTEX %c%c%02d - %s", b->b1 ? b->b1 : '?', b->b2 ? b->b2 : '?',
                                b->num >= 0 ? b->num : 99, b->b2 ? nb_subject_name(b->b2) : "start lost");
    }

shown:
    /* the selection follows the newest bulletin, unless one is open */
    if (!nav_detail) { nav_sel = 0; nav_top = 0; }
    else if (nav_sel < nav_count - 1) nav_sel++;    /* keep the open one */
    if (nav_list) lv_obj_invalidate(nav_list);
}

/* saved bulletin: name and last save (a bulletin received again is rewritten) */
typedef struct { char *name; time_t mtime; } nav_file_t;

static int nav_file_cmp(const void *a, const void *b)
{
    const nav_file_t *x = a, *y = b;
    if (x->mtime != y->mtime) return (y->mtime > x->mtime) - (y->mtime < x->mtime);   /* newest first */
    return strcmp(y->name, x->name);
}

/* "bul-20261012-003712-MA64.txt": B2 is the second letter of the code */
static char nav_file_subject(const char *name)
{
    size_t L = strlen(name), e = strlen(NB_EXT);
    return L >= e + 4 ? name[L - e - 3] : 0;
}

static int nav_item_cmp(const void *a, const void *b)
{
    const nav_item_t *x = *(nav_item_t *const *)a, *y = *(nav_item_t *const *)b;
    return (y->b.last_rx > x->b.last_rx) - (y->b.last_rx < x->b.last_rx);
}

static void nav_load_dir(void)
{
    DIR *d = opendir(NAVTEX_SAVE_DIR);
    if (!d) return;
    nav_file_t *files = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t L = strlen(de->d_name);
        if (strncmp(de->d_name, NB_PREFIX, strlen(NB_PREFIX)) || L < 8 ||
            strcmp(de->d_name + L - strlen(NB_EXT), NB_EXT) || L >= sizeof(nav_items[0]->file))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            nav_file_t *t = realloc(files, sizeof(nav_file_t) * (size_t)cap);
            if (!t) break;
            files = t;
        }
        char path[160];
        struct stat st;
        snprintf(path, sizeof(path), NAVTEX_SAVE_DIR "/%s", de->d_name);
        files[n].name = strdup(de->d_name);
        files[n].mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
        n++;
    }
    closedir(d);
    if (files) qsort(files, (size_t)n, sizeof(nav_file_t), nav_file_cmp);

    int deleted = 0, kept = 0;
    for (int i = 0; i < n; ++i) {
        char path[160];
        if (!files[i].name) continue;
        snprintf(path, sizeof(path), NAVTEX_SAVE_DIR "/%s", files[i].name);
        if (nav_count < NB_STORE_MAX) {
            nav_item_t *it = malloc(sizeof(*it));
            if (it && nb_load(&it->b, path)) {
                snprintf(it->file, sizeof(it->file), "%s", files[i].name);
                nav_items[nav_count++] = it;
            } else {
                free(it);
            }
        } else if (i >= NB_KEEP_FILES) {
            /* too old: deleted, except search and rescue / piracy (D) */
            if (nav_file_subject(files[i].name) == 'D') kept++;
            else if (unlink(path) == 0) deleted++;
        }
        free(files[i].name);
    }
    free(files);
    qsort(nav_items, (size_t)nav_count, sizeof(nav_items[0]), nav_item_cmp);
    fprintf(stderr, "NAVTEX: %d saved bulletins\n", nav_count);
    if (deleted || kept)
        fprintf(stderr, "NAVTEX: %d old bulletins deleted (newest %d kept, %d SAR/piracy kept)\n",
                deleted, NB_KEEP_FILES, kept);
}

static void nav_free_all(void)
{
    for (int i = 0; i < nav_count; ++i) free(nav_items[i]);
    nav_count = 0;
}

/* ---- drawing helpers (as DSC) ----------------------------------------------------- */

static void nav_rect(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2,
                     uint32_t color, lv_coord_t radius, uint32_t border, lv_coord_t bw)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(color);
    r.bg_opa = LV_OPA_COVER;
    r.radius = radius;
    if (bw > 0) {
        r.border_color = lv_color_hex(border);
        r.border_width = bw;
        r.border_opa = LV_OPA_COVER;
    }
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(ctx, &r, &a);
}

static lv_coord_t nav_text_w(const lv_font_t *font, const char *t)
{
    lv_point_t sz;
    lv_txt_get_size(&sz, t, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* text at (x, y), cut with ".." to w (w > 0); returns the end x */
static lv_coord_t nav_text(lv_draw_ctx_t *ctx, const lv_font_t *font, uint32_t color,
                           lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_text_align_t align, const char *t)
{
    char buf[160];
    if (w > 0 && nav_text_w(font, t) > w) {
        size_t n = strlen(t);
        if (n > sizeof(buf) - 3) n = sizeof(buf) - 3;
        memcpy(buf, t, n);
        buf[n] = '\0';
        while (n > 0) {
            buf[n] = '\0';
            strcat(buf, "..");
            if (nav_text_w(font, buf) <= w) break;
            buf[--n] = '\0';
        }
        t = buf;
    }
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = font;
    l.color = lv_color_hex(color);
    l.align = align;
    lv_coord_t tw = nav_text_w(font, t);
    lv_area_t a;
    if (align == LV_TEXT_ALIGN_RIGHT) { a.x1 = x - tw - 2; a.x2 = x; }
    else { a.x1 = x; a.x2 = x + tw + 2; }
    a.y1 = y;
    a.y2 = y + lv_font_get_line_height(font);
    lv_draw_label(ctx, &l, &a, t, NULL);
    return align == LV_TEXT_ALIGN_RIGHT ? x - tw : x + tw;
}

/* ---- what a bulletin looks like ------------------------------------------------------- */

static uint32_t nav_subject_color(char b2)
{
    switch (b2) {
    case 'D': return 0xcd3030;                  /* SAR, piracy */
    case 'B': return 0xe04848;                  /* meteorological warning */
    case 'A': case 'L': return 0xe88014;        /* navigational warning */
    case 'E': return 0x3a78c0;                  /* forecast */
    case 'C': return 0x5ab4d8;                  /* ice */
    case 'Z': case 'T': return 0x5a6672;        /* nothing / test */
    case 0:   return 0x5a6672;
    default:  return 0x46688c;
    }
}

/* side bar / subject block of a bulletin */
static uint32_t nav_color(const nb_bulletin_t *b)
{
    return b->sitor ? NB_C_SITOR : nav_subject_color(b->b2);
}

/* subject in words, or what is known of it */
static const char *nav_subject(const nb_bulletin_t *b, const char *start_lost)
{
    if (b->sitor) return "SITOR-B message";
    return b->b2 ? nb_subject_name(b->b2) : start_lost;
}

static void nav_code(const nb_bulletin_t *b, char *dst, size_t sz)
{
    if (b->sitor) { snprintf(dst, sz, "SB"); return; }
    char n[4] = "--";
    if (b->num >= 0) snprintf(n, sizeof(n), "%02d", b->num);
    snprintf(dst, sz, "%c%c%s", b->b1 ? b->b1 : '-', b->b2 ? b->b2 : '-', n);
}

static void nav_level(const nb_bulletin_t *b, const char **name, uint32_t *color)
{
    switch (nb_level(b)) {
    case NB_LEVEL_OK:      *name = "OK";         *color = NB_C_OK;   break;
    case NB_LEVEL_DAMAGED: *name = "DAMAGED";    *color = NB_C_WARN; break;
    default:               *name = "UNREADABLE"; *color = NB_C_BAD;  break;
    }
}

/* "12 Oct 23:20" */
static void nav_time(time_t t, char *dst, size_t sz)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(dst, sz, "%d %b %H:%M", &tm);
}

/* quality pill "OK 97%" right-aligned at x; returns its left x */
static lv_coord_t nav_pill(lv_draw_ctx_t *ctx, const nb_bulletin_t *b, lv_coord_t x, lv_coord_t y,
                           const lv_font_t *font)
{
    const char *ln;
    uint32_t lc;
    nav_level(b, &ln, &lc);
    char t[32];
    snprintf(t, sizeof(t), "%s %d%%", ln, nb_quality(b));
    lv_coord_t w = nav_text_w(font, t) + 16, h = lv_font_get_line_height(font) + 4;
    nav_rect(ctx, x - w, y, x, y + h, lc, h / 2, 0, 0);
    nav_text(ctx, font, lc == NB_C_WARN ? 0x141414 : 0xffffff, x - 8, y + 2, 0, LV_TEXT_ALIGN_RIGHT, t);
    return x - w;
}

static void nav_draw_card(lv_draw_ctx_t *ctx, const nb_bulletin_t *b, lv_coord_t x1, lv_coord_t y1,
                          lv_coord_t x2, lv_coord_t y2, bool sel)
{
    nav_rect(ctx, x1, y1, x2, y2, sel ? NB_C_PANEL_SEL : NB_C_PANEL, 6,
             sel ? NB_C_SELECT : NB_C_LINE, sel ? 3 : 1);
    uint32_t sc = nav_color(b);
    nav_rect(ctx, x1 + (sel ? 3 : 1), y1 + 8, x1 + 10, y2 - 8, sc, 2, 0, 0);

    char code[8], ts[24], st[80], dtg[80], t1[80], t2[80];
    nav_code(b, code, sizeof(code));
    nav_time(b->last_rx, ts, sizeof(ts));
    nb_summary(b, st, dtg, t1, t2, sizeof(t1));

    lv_coord_t x = x1 + 20, r = x2 - 10;
    /* 1: code, subject, time */
    lv_coord_t e = nav_text(ctx, &sony_22, NB_C_TEXT, x, y1 + 6, 0, LV_TEXT_ALIGN_LEFT, code);
    lv_coord_t tl = nav_text(ctx, &sony_16, NB_C_SUB, r, y1 + 10, 0, LV_TEXT_ALIGN_RIGHT, ts);
    const char *subj = nav_subject(b, "Start lost");
    nav_text(ctx, &sony_20, b->b2 || b->sitor ? sc : NB_C_WARN, e + 10, y1 + 10, tl - e - 22, LV_TEXT_ALIGN_LEFT, subj);
    /* 2, 3: the message */
    nav_text(ctx, &sony_20, NB_C_TEXT, x, y1 + 34, r - x, LV_TEXT_ALIGN_LEFT, t1[0] ? t1 : "(no text)");
    nav_text(ctx, &sony_18, NB_C_SUB, x, y1 + 58, r - x, LV_TEXT_ALIGN_LEFT, t2);
    /* 4: station, copies, quality */
    lv_coord_t px = nav_pill(ctx, b, r, y2 - 26, &sony_16);
    char info[96];
    snprintf(info, sizeof(info), "%s%s%s", st[0] ? st : "",
             b->receptions > 1 ? (st[0] ? "  x" : "x") : "", "");
    if (b->receptions > 1) snprintf(info + strlen(info), sizeof(info) - strlen(info), "%d", b->receptions);
    if (b->lost_tail) snprintf(info + strlen(info), sizeof(info) - strlen(info), "%send lost", info[0] ? "  " : "");
    nav_text(ctx, &sony_18, NB_C_SUB, x, y2 - 24, px - x - 10, LV_TEXT_ALIGN_LEFT, info);
}

/* ---- the grid ------------------------------------------------------------------------- */

static int nav_cells(void)
{
    return NB_COLS * NB_ROWS;
}

static void nav_fix_top(void)
{
    if (nav_sel >= nav_count) nav_sel = nav_count - 1;
    if (nav_sel < 0) nav_sel = 0;
    /* whole rows */
    if (nav_sel < nav_top) nav_top = nav_sel / NB_COLS * NB_COLS;
    if (nav_sel >= nav_top + nav_cells()) nav_top = (nav_sel / NB_COLS - NB_ROWS + 1) * NB_COLS;
    if (nav_top < 0) nav_top = 0;
}

static void nav_list_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    lv_coord_t W = box.x2 - box.x1 + 1, H = box.y2 - box.y1 + 1;

    if (nav_count == 0) {
        const char *t = test_mode ? "Playing " NAVTEX_TEST_WAV "..." : "Listening - no NAVTEX bulletins yet";
        nav_text(ctx, &sony_22, NB_C_SUB, box.x1 + (W - nav_text_w(&sony_22, t)) / 2, box.y1 + H / 2 - 14,
                 0, LV_TEXT_ALIGN_LEFT, t);
        return;
    }

    lv_coord_t cw = (W - NB_GAP * (NB_COLS - 1)) / NB_COLS;
    lv_coord_t ch = (H - NB_GAP * (NB_ROWS - 1) - 22) / NB_ROWS;
    for (int i = 0; i < nav_cells(); ++i) {
        int k = nav_top + i;
        if (k >= nav_count) break;
        lv_coord_t x = box.x1 + (i % NB_COLS) * (cw + NB_GAP);
        lv_coord_t y = box.y1 + (i / NB_COLS) * (ch + NB_GAP);
        nav_draw_card(ctx, &nav_items[k]->b, x, y, x + cw - 1, y + ch - 1, k == nav_sel);
    }

    /* bottom line: position in the list, reception in progress */
    char t[64];
    snprintf(t, sizeof(t), "%d / %d", nav_sel + 1, nav_count);
    nav_text(ctx, &sony_16, NB_C_SUB, box.x2 - 4, box.y2 - 19, 0, LV_TEXT_ALIGN_RIGHT, t);
    if (nav_receiving)
        nav_text(ctx, &sony_16, NB_C_SELECT, box.x1 + 4, box.y2 - 19, 0, LV_TEXT_ALIGN_LEFT,
                 "Receiving a bulletin...");
}

/* ---- the bulletin --------------------------------------------------------------------- */

static void nav_hdr_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    if (nav_sel < 0 || nav_sel >= nav_count) return;
    const nb_bulletin_t *b = &nav_items[nav_sel]->b;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    uint32_t sc = nav_color(b);
    nav_rect(ctx, a.x1, a.y1, a.x2, a.y2, NB_C_PANEL, 6, NB_C_LINE, 1);
    /* subject block */
    lv_coord_t bw = 112;
    nav_rect(ctx, a.x1, a.y1, a.x1 + bw, a.y2, sc, 6, 0, 0);
    nav_rect(ctx, a.x1 + bw - 8, a.y1, a.x1 + bw, a.y2, sc, 0, 0, 0);
    char code[8];
    nav_code(b, code, sizeof(code));
    uint32_t tc = sc == 0x5ab4d8 ? 0x141414 : 0xffffff;
    nav_text(ctx, &sony_30, tc, a.x1 + (bw - nav_text_w(&sony_30, code)) / 2, a.y1 + 10, 0,
             LV_TEXT_ALIGN_LEFT, code);
    char sn[24];
    snprintf(sn, sizeof(sn), "%d / %d", nav_sel + 1, nav_count);
    nav_text(ctx, &sony_20, tc, a.x1 + (bw - nav_text_w(&sony_16, sn)) / 2, a.y1 + 52, 0,
             LV_TEXT_ALIGN_LEFT, sn);

    lv_coord_t x = a.x1 + bw + 14, r = a.x2 - 12;
    char st[80], dtg[80], t1[80], t2[80], line[200], ts[24];
    nb_summary(b, st, dtg, t1, t2, sizeof(t1));

    /* 1: subject, quality */
    lv_coord_t pl = nav_pill(ctx, b, r, a.y1 + 6, &sony_20);
    const char *subj = nav_subject(b, "Bulletin (start lost)");
    nav_text(ctx, &sony_24, NB_C_TEXT, x, a.y1 + 4, pl - x - 10, LV_TEXT_ALIGN_LEFT, subj);
    /* 2: station, date-time group */
    snprintf(line, sizeof(line), "%s%s%s", st[0] ? st : (b->b1 ? "" : ""), st[0] && dtg[0] ? "   " : "", dtg);
    if (!line[0] && b->b1) snprintf(line, sizeof(line), "Station %c", b->b1);
    nav_text(ctx, &sony_20, NB_C_SUB, x, a.y1 + 33, r - x, LV_TEXT_ALIGN_LEFT, line);
    /* 3: reception */
    nav_time(b->last_rx, ts, sizeof(ts));
    snprintf(line, sizeof(line), "Received %s UTC", ts);
    if (b->receptions > 1)
        snprintf(line + strlen(line), sizeof(line) - strlen(line), " - %d copies merged", b->receptions);
    lv_coord_t ex = nav_text(ctx, &sony_20, NB_C_SUB, x, a.y1 + 58, 0, LV_TEXT_ALIGN_LEFT, line);
    if (b->lost_head || b->lost_tail) {
        const char *f = b->lost_head && b->lost_tail ? "START AND END LOST" :
                        b->lost_head ? "START LOST" : "END LOST";
        nav_text(ctx, &sony_20, NB_C_WARN, ex + 14, a.y1 + 58, 0, LV_TEXT_ALIGN_LEFT, f);
    }
    /* legend */
    lv_coord_t lx = r;
    lx = nav_text(ctx, &sony_18, 0x5ee08a, lx, a.y1 + 60, 0, LV_TEXT_ALIGN_RIGHT, "repaired") - 10;
    lx = nav_text(ctx, &sony_18, 0x6ad0ff, lx, a.y1 + 60, 0, LV_TEXT_ALIGN_RIGHT, "position") - 10;
    lx = nav_text(ctx, &sony_18, 0xff5a5a, lx, a.y1 + 60, 0, LV_TEXT_ALIGN_RIGHT, "* lost") - 10;
    nav_text(ctx, &sony_18, 0xe2b028, lx, a.y1 + 60, 0, LV_TEXT_ALIGN_RIGHT, "uncertain");
}

/* the text with LVGL colour commands: "#rrggbb text#" */
static void nav_build_body(const nb_bulletin_t *b)
{
    free(nav_body_text);
    nav_body_text = malloc((size_t)b->len * 16 + 64);
    if (!nav_body_text) return;
    static char fixed[NB_MAX_TEXT];
    static uint8_t mark[NB_MAX_TEXT];
    nb_mark_positions(b, fixed, mark);

    char *o = nav_body_text;
    const char *open = NULL;
    for (int i = 0; i < b->len; ++i) {
        char c = fixed[i];
        const char *col = NULL;
        if (c == '\n') col = NULL;
        else if (b->q[i] >= NAVTEX_Q_LOST || c == '*') col = NB_T_LOST;
        else if (mark[i] == NB_MARK_FIXED) col = NB_T_FIXED;
        else if (b->q[i] == NAVTEX_Q_GUESS) col = NB_T_GUESS;
        else if (mark[i] == NB_MARK_COORD) col = NB_T_COORD;
        if (col != open) {
            if (open) *o++ = '#';
            if (col) o += sprintf(o, "#%s ", col);
            open = col;
        }
        *o++ = c == '#' ? '?' : c;
    }
    if (open) *o++ = '#';
    *o = '\0';
    if (b->len == 0) strcpy(nav_body_text, "(no text)");
}

static void nav_show_detail(void)
{
    if (!nav_frame || nav_count == 0) return;
    nav_fix_top();
    nav_detail = true;
    nav_build_body(&nav_items[nav_sel]->b);
    lv_obj_add_flag(nav_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_hdr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_body, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_static(nav_body_label, nav_body_text ? nav_body_text : "");
    lv_obj_scroll_to_y(nav_body, 0, LV_ANIM_OFF);
    lv_obj_invalidate(nav_hdr);
    if (lv_obj_get_group(nav_list)) lv_group_remove_obj(nav_list);
    focus_obj(nav_body);
    buttons_load_page(&nav_page_detail);
}

static void nav_show_list(void)
{
    if (!nav_frame) return;
    nav_detail = false;
    lv_obj_add_flag(nav_hdr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_body, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_static(nav_body_label, "");
    free(nav_body_text);
    nav_body_text = NULL;
    lv_obj_clear_flag(nav_list, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_get_group(nav_body)) lv_group_remove_obj(nav_body);
    nav_fix_top();
    lv_obj_invalidate(nav_list);
    focus_obj(nav_list);
    buttons_load_page(&nav_page_1);
}

/* ---- keys ------------------------------------------------------------------------------ */

/* MFK in NAVTEX mode; true when used */
static bool nav_key(uint32_t key)
{
    if (!nav_mode || !nav_frame) return false;
    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) return false;
    int dir = key == LV_KEY_LEFT ? -1 : 1;
    if (nav_detail) {
        lv_coord_t room = dir > 0 ? lv_obj_get_scroll_bottom(nav_body) : lv_obj_get_scroll_y(nav_body);
        lv_coord_t step = room < NB_SCROLL_PX ? room : NB_SCROLL_PX;
        if (step > 0) lv_obj_scroll_by(nav_body, 0, -dir * step, LV_ANIM_OFF);
    } else if (nav_count > 0) {
        nav_sel += dir;
        nav_fix_top();
        lv_obj_invalidate(nav_list);
    }
    return true;
}

static void nav_list_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (nav_mode && !nav_detail && nav_count > 0) nav_show_detail();
}

static void nav_body_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (nav_mode && nav_detail) nav_show_list();
}

/* ---- button callbacks ----------------------------------------------------------------- */

static void nav_page2_cb(button_data_t *d) { (void)d; buttons_load_page(&nav_page_2); }
static void nav_page1_cb(button_data_t *d) { (void)d; buttons_load_page(&nav_page_1); }
static void nav_open_cb(button_data_t *d) { (void)d; nav_show_detail(); }
static void nav_back_cb(button_data_t *d) { (void)d; nav_show_list(); }

static void nav_step(int dir)
{
    int n = nav_sel + dir;
    if (n < 0 || n >= nav_count) return;
    nav_sel = n;
    nav_show_detail();
}

static void nav_newer_cb(button_data_t *d) { (void)d; nav_step(-1); }
static void nav_older_cb(button_data_t *d) { (void)d; nav_step(1); }

static void nav_delete_cb(button_data_t *d)
{
    (void)d;
    if (nav_sel < 0 || nav_sel >= nav_count) return;
    nav_item_t *it = nav_items[nav_sel];
    if (it->file[0]) {
        char path[160];
        snprintf(path, sizeof(path), NAVTEX_SAVE_DIR "/%s", it->file);
        if (unlink(path) != 0) fprintf(stderr, "NAVTEX: cannot delete %s\n", path);
    }
    free(it);
    memmove(&nav_items[nav_sel], &nav_items[nav_sel + 1], sizeof(nav_items[0]) * (size_t)(nav_count - nav_sel - 1));
    nav_count--;
    msg_update_text_fmt("Bulletin deleted");
    if (nav_detail && nav_count > 0) {
        if (nav_sel >= nav_count) nav_sel = nav_count - 1;
        nav_show_detail();
    } else {
        nav_show_list();
    }
}

/* ---- mode --------------------------------------------------------------------------- */

static void nav_mode_load(void)
{
    FILE *f = fopen(NB_MODE_FILE, "r");
    if (!f) { nav_mode = true; return; }
    char s[16] = "";
    if (fgets(s, sizeof(s), f)) nav_mode = strncmp(s, "sitorb", 6) != 0;
    fclose(f);
}

static void nav_mode_save(void)
{
    if (!nav_ensure_dir()) return;
    FILE *f = fopen(NB_MODE_FILE, "w");
    if (!f) return;
    fputs(nav_mode ? "navtex\n" : "sitorb\n", f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* show the view of the current mode */
static void nav_apply_mode(void)
{
    if (!nav_frame || !scroll) return;
    if (nav_mode) {
        browser_close(false);
        if (lv_obj_get_group(scroll)) lv_group_remove_obj(scroll);
        lv_obj_add_flag(scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(nav_frame, LV_OBJ_FLAG_HIDDEN);
        nav_show_list();
    } else {
        if (nav_list && lv_obj_get_group(nav_list)) lv_group_remove_obj(nav_list);
        if (nav_body && lv_obj_get_group(nav_body)) lv_group_remove_obj(nav_body);
        lv_obj_add_flag(nav_frame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scroll, LV_OBJ_FLAG_HIDDEN);
        focus_obj(scroll);
        if (follow_tail) {
            lv_obj_update_layout(scroll);
            lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
        }
        buttons_load_page(&btn_page_1);
    }
}

static void mode_cb(button_data_t *data)
{
    (void)data;
    nav_mode = !nav_mode;
    nav_mode_save();
    nav_apply_mode();
}

/* ---- life cycle (called from dialog_navtex.c) ------------------------------------------- */

static void nav_construct(lv_obj_t *parent)
{
    nav_mode_load();
    nav_count = 0;
    nav_sel = nav_top = 0;
    nav_detail = false;
    nav_receiving = false;
    nav_load_dir();
    if (!nav_asm) nav_asm = nb_create(nav_done_cb, NULL);

    pthread_mutex_lock(&nav_fifo_mutex);
    nav_fifo_head = nav_fifo_count = 0;
    pthread_mutex_unlock(&nav_fifo_mutex);

    /* the WEFAX / DSC big frame */
    nav_frame = lv_obj_create(parent);
    lv_obj_remove_style_all(nav_frame);
    lv_obj_add_style(nav_frame, &style.dialog.base, 0);
    lv_obj_set_size(nav_frame, NB_W, NB_H);
    lv_obj_center(nav_frame);
    lv_obj_set_style_translate_y(nav_frame, 4, 0);
    lv_obj_clear_flag(nav_frame, LV_OBJ_FLAG_SCROLLABLE);

    nav_list = lv_obj_create(nav_frame);
    lv_obj_remove_style_all(nav_list);
    lv_obj_set_size(nav_list, NB_W - 16, NB_H - 14);
    lv_obj_center(nav_list);
    lv_obj_clear_flag(nav_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(nav_list, nav_list_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(nav_list, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(nav_list, nav_list_pressed_cb, LV_EVENT_PRESSED, NULL);

    nav_hdr = lv_obj_create(nav_frame);
    lv_obj_remove_style_all(nav_hdr);
    lv_obj_set_size(nav_hdr, NB_W - 16, NB_HDR_H);
    lv_obj_align(nav_hdr, LV_ALIGN_TOP_MID, 0, 7);
    lv_obj_clear_flag(nav_hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(nav_hdr, nav_hdr_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(nav_hdr, LV_OBJ_FLAG_HIDDEN);

    nav_body = lv_obj_create(nav_frame);
    lv_obj_remove_style_all(nav_body);
    lv_obj_set_size(nav_body, NB_W - 16, NB_H - NB_HDR_H - 22);
    lv_obj_align(nav_body, LV_ALIGN_TOP_MID, 0, NB_HDR_H + 13);
    lv_obj_set_style_pad_hor(nav_body, 10, 0);
    lv_obj_set_scroll_dir(nav_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nav_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(nav_body, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(nav_body, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(nav_body, nav_body_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(nav_body, LV_OBJ_FLAG_HIDDEN);

    nav_body_label = lv_label_create(nav_body);
    lv_obj_set_width(nav_body_label, lv_pct(100));
    lv_label_set_long_mode(nav_body_label, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(nav_body_label, true);
    /* 1KO125: Settings > Interface > Decoder text, as RTTY and the SITORB
     * text (navtex_font(), text_size_cb()); the colours stay (recolor) */
    lv_obj_set_style_text_font(nav_body_label, navtex_font(), 0);
    lv_obj_set_style_text_color(nav_body_label, lv_color_hex(NB_C_TEXT), 0);
    lv_label_set_text_static(nav_body_label, "");

    nav_apply_mode();
}

/* every UI tick: characters -> bulletins */
static void nav_poll(void)
{
    nav_ev_t ev[256];
    int n = 0;
    pthread_mutex_lock(&nav_fifo_mutex);
    while (nav_fifo_count > 0 && n < (int)(sizeof(ev) / sizeof(ev[0]))) {
        ev[n++] = nav_fifo[nav_fifo_head];
        nav_fifo_head = (nav_fifo_head + 1) % NB_FIFO_LEN;
        nav_fifo_count--;
    }
    pthread_mutex_unlock(&nav_fifo_mutex);

    time_t now = time(NULL);
    if (nav_asm) {
        for (int i = 0; i < n; ++i) nb_feed(nav_asm, ev[i].c, ev[i].q, now);
        nb_tick(nav_asm, now);
        bool rx = nb_active(nav_asm);
        if (rx != nav_receiving) {
            nav_receiving = rx;
            if (nav_list && nav_mode && !nav_detail) lv_obj_invalidate(nav_list);
        }
    }
}

/* decoder replaced (test WAV, clear): close what was being received */
static void nav_flush(void)
{
    nav_poll();
    if (nav_asm) nb_flush(nav_asm);
    nav_receiving = false;
}

static void nav_destruct(void)
{
    nav_flush();
    if (nav_list && lv_obj_get_group(nav_list)) lv_group_remove_obj(nav_list);
    if (nav_body && lv_obj_get_group(nav_body)) lv_group_remove_obj(nav_body);
    if (nav_frame) lv_obj_del(nav_frame);
    nav_frame = nav_list = nav_hdr = nav_body = nav_body_label = NULL;
    free(nav_body_text);
    nav_body_text = NULL;
    nb_destroy(nav_asm);
    nav_asm = NULL;
    nav_free_all();
}
