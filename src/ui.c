#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <kernel.h>
#include <unistd.h>
#include "gfx.h"
#include "pad.h"
#include "fs.h"
#include "launch.h"
#include "audio.h"
#include "ftype.h"
#include "compress.h"
#include "viewer.h"
#include "settings.h"
#include "log.h"
#include "ui.h"

extern unsigned char logo_rgba[], bg_rgba[], splashbg_rgba[], file_rgba[], folder_rgba[], elf_rgba[],
                     audio_rgba[], image_rgba[], video_rgba[], text_rgba[], archive_rgba[];
extern void *_gp;
static Img logo, bgimg, splash_bg, icon_file, icon_dir, icon_elf, icon_audio, icon_image, icon_video, icon_text, icon_archive;
static PadState pd;
static unsigned frame, prev_held;
static int usb_was_ok, bgm_pending;
static int last_sel = -1; static unsigned sel_frame, toast_start;
static int chord, chord2;

typedef enum { S_LIST, S_MENU, S_CONFIRM, S_OSK, S_HEX, S_TEXT, S_LOADING, S_CREDITS, S_VIEWER, S_SETTINGS, S_USB, S_FORMAT, S_UNZIP, S_OPENWITH } State;
static State st = S_LIST;

static const char *devs[] = { "mc0:/", "mc1:/", "mass:/" };
typedef struct { const char *root, *label; int always; } Dev;
static const Dev devtab[] = {
    { "mc0:/",  "Memory Card 1 (mc0:)",  1 }, { "mc1:/",  "Memory Card 2 (mc1:)", 1 }, { "mass:/", "USB Storage (mass:)", 1 },
    { "hdd0:/", "Internal HDD (hdd0:)",  0 }, { "cdfs:/", "CD / DVD (cdfs:)",     0 }, { "host:/", "Host PC (host:)",     0 },
    { "rom0:/", "BIOS ROM (rom0:)",      0 }, { "rom1:/", "DVD player ROM (rom1:)", 0 } };
#define NDEV ((int)(sizeof devtab / sizeof devtab[0]))
static char dev_note[NDEV][20], dev_prev[NDEV][20];   /* short status shown on the right of each device row */
static int disc_type, disc_poll_ok = 1;                  /* what the drive reports (0 = no disc) */
static volatile int dev_scan;                            /* a devices rescan is waiting for the memory card check */
static int dev_idx = 2;                       /* start on USB: the memory card is only touched when you ask for it */
static char cwd[PATH_MAX_LEN] = "mass:/";
static Entry ents[MAX_ENTRIES];
static int n_ents, sel, top;
static char keep_name[256];                   /* after a refresh, put the cursor back on this entry */
static int holdf;
static char toast[96]; static int toast_t;

static char clip[256][PATH_MAX_LEN]; static int n_clip, clip_move;
static char tg[256][PATH_MAX_LEN];   static int n_tg;

static const char *menu_items[16]; static int n_menu, menu_sel;
static char osk_buf[64]; static int osk_len, osk_x, osk_y, osk_mode;   /* 0 rename, 1 mkdir */
static const char *osk_rows[] = { "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz", "0123456789._-+()[]~" };
static char hex_path[PATH_MAX_LEN]; static u32 hex_off; static unsigned char hex_buf[8 * 18]; static int hex_len;
static char text_lines[24][72]; static int n_text; static char text_title[40]; static int text_small;

/* background directory reads (memory cards) */
static Entry job_ents[MAX_ENTRIES];
static char job_path[PATH_MAX_LEN];
static volatile int job_done, job_result, job_running, job_abandoned;
static char job_stack[0x8000] __attribute__((aligned(16)));
static unsigned job_start;
static char prev_good[PATH_MAX_LEN] = "mass:/";
static int usb_retry = 600;

/* settings: auto-saved a moment after any change; USB preferred, memory card as fallback (written in the background) */
static Settings cfg_snap;
static int cfg_dirty; static unsigned cfg_dirty_frame;
static volatile int sjob_running, sjob_done, sjob_ok; static int sjob_mode;      /* 0 load, 1 save */
static char sjob_buf[512]; static int sjob_len; static Settings sjob_cfg;
static char sjob_stack[0x8000] __attribute__((aligned(16)));
static int set_sel;
static unsigned mc_quiet_until;
#define SET_FILE_USB "mass:/PrismELF/settings.cfg"
static const char *const SET_FILE_MC[2] = { "mc0:/PrismELF/settings.cfg", "mc1:/PrismELF/settings.cfg" };

static int W(void) { return gs->Width; }
static int H(void) { return gs->Height; }
static void say_t(const char *m, int frames) { snprintf(toast, sizeof toast, "%s", m); toast_t = frames; toast_start = frame; }
static void say(const char *m) { say_t(m, 180); }
static int at_devices(void) { return !strcmp(cwd, "root:"); }
static int is_mc(const char *p) { return p[0] == 'm' && p[1] == 'c'; }
static int is_cd(const char *p) { return !strncmp(p, "cdfs:", 5); }

/* memory card formatter: state + background job */
static volatile int fjob_running, fjob_done; static int fjob_mode, fjob_port, fjob_rc;   /* mode 0 scan, 1 quick erase, 2 full format */
static char fjob_stack[0x10000] __attribute__((aligned(16)));
static int fm_step, fm_card, fm_method, fm_conf;                 /* step: 0 pick card, 1 pick method, 2 warning, 3 working, 4 result */
static int fm_status[2] = { -2, -2 }, fm_type[2], fm_free[2];    /* status: 0 formatted, 1 not formatted, -1 no card, -2 checking */
static char fm_result[96]; static int fm_ok; static unsigned fm_start;

static int mc_busy(void) { return job_running || sjob_running || fjob_running; }

static void apply_cfg(void)
{
    bgm_set_muted(!cfg.music_on);
    bgm_set_volume(cfg.volume);
    log_set_files(cfg.write_log);
}

/* names from the card can hold odd bytes the font cannot draw: show them safely */
static void safe_name(char *dst, const char *src, int max)
{
    int i = 0;
    for (; src[i] && i < max; i++) { unsigned char c = (unsigned char)src[i]; dst[i] = (c < 32 || c > 126) ? '?' : (char)c; }
    dst[i] = 0;
}

static void sjob_thread(void *arg)
{
    (void)arg;
    sjob_ok = 0;
    if (iop_load_mc() == 0) {
        if (sjob_mode == 0) {
            for (int i = 0; i < 2 && !sjob_ok; i++) {
                unsigned len = 0; unsigned char *b = fs_read_all(SET_FILE_MC[i], &len, 4096);
                if (b) { settings_from_text(&sjob_cfg, (char *)b); free(b); sjob_ok = 1; }
            }
        } else {
            for (int i = 0; i < 2 && !sjob_ok; i++) {
                fs_mkdir(i == 0 ? "mc0:/PrismELF" : "mc1:/PrismELF");
                if (fs_write_all(SET_FILE_MC[i], sjob_buf, sjob_len) == 0) sjob_ok = 1;
            }
        }
    }
    sjob_done = 1;
    ExitDeleteThread();
}

static void start_sjob(int mode)
{
    ee_thread_t t; ee_thread_status_t ts;
    ReferThreadStatus(GetThreadId(), &ts);
    memset(&t, 0, sizeof t);
    t.func = sjob_thread; t.stack = sjob_stack; t.stack_size = sizeof sjob_stack; t.gp_reg = &_gp;
    t.initial_priority = ts.current_priority + 4; if (t.initial_priority > 126) t.initial_priority = 126;
    sjob_mode = mode; sjob_done = 0; sjob_running = 1;
    int id = CreateThread(&t);
    if (id < 0) { sjob_running = 0; LOGE("settings: CreateThread failed"); return; }
    StartThread(id, NULL);
}

/* returns 1 if saved (or a background save was started), 0 if it must be retried later */
static int cfg_save(int blocking)
{
    char text[640];
    int n = settings_to_text(&cfg, text, sizeof text);
    if (fs_dev_ready("mass:/")) {
        fs_mkdir("mass:/PrismELF");
        if (fs_write_all(SET_FILE_USB, text, n) == 0) { LOGI("settings: saved to USB"); cfg_dirty = 0; return 1; }
        LOGW("settings: USB write failed");
    }
    if (mc_busy()) return 0;
    if (blocking) {                                   /* leaving the app: do it now */
        if (iop_load_mc() == 0)
            for (int i = 0; i < 2; i++) {
                fs_mkdir(i == 0 ? "mc0:/PrismELF" : "mc1:/PrismELF");
                if (fs_write_all(SET_FILE_MC[i], text, n) == 0) { LOGI("settings: saved to %s", i == 0 ? "mc0" : "mc1"); cfg_dirty = 0; return 1; }
            }
        return 0;
    }
    memcpy(sjob_buf, text, n); sjob_len = n;
    start_sjob(1);
    cfg_dirty = 0;
    LOGI("settings: saving to the memory card in the background");
    return 1;
}

/* Called while the splash is up, BEFORE the music starts, so a muted/low-volume setup never blips on.
   Returns 1 if USB is present (settings known, music may start now) or 0 if the cards must be read first. */
int ui_boot_settings(void)
{
    settings_defaults(&cfg);
    int usb = 0;
    for (int i = 0; i < 12 && !usb; i++) { usb = fs_dev_ready("mass:/"); if (!usb) usleep(100000); }
    if (usb) {
        unsigned len = 0; unsigned char *b = fs_read_all(SET_FILE_USB, &len, 4096);
        if (b) { settings_from_text(&cfg, (char *)b); free(b); LOGI("settings: loaded from USB"); }
        else LOGI("settings: no file on USB, using defaults");
    } else {
        sjob_cfg = cfg;
        bgm_pending = 1;                              /* music waits for the card read so a muted setup stays silent */
        start_sjob(0);
        LOGI("settings: no USB, reading the memory cards in the background (music waits)");
    }
    cfg_snap = cfg;
    apply_cfg();
    return usb;
}

void ui_load_images(void)
{
    img_from_rgba(&logo, logo_rgba, 256, 128);
    img_from_rgba(&bgimg, bg_rgba, 128, 112);
    img_from_rgba(&splash_bg, splashbg_rgba, 256, 224);
    img_from_rgba(&icon_file, file_rgba, 16, 16);
    img_from_rgba(&icon_dir, folder_rgba, 16, 16);
    img_from_rgba(&icon_elf, elf_rgba, 16, 16);
    img_from_rgba(&icon_audio, audio_rgba, 16, 16);
    img_from_rgba(&icon_image, image_rgba, 16, 16);
    img_from_rgba(&icon_video, video_rgba, 16, 16);
    img_from_rgba(&icon_text, text_rgba, 16, 16);
    img_from_rgba(&icon_archive, archive_rgba, 16, 16);
    viewer_init();
}

static Img *icon_for(const Entry *e)
{
    if (e->is_dir) return &icon_dir;
    switch (ftype_of(e->name)) {
    case FK_ELF: return &icon_elf;
    case FK_AUDIO: return &icon_audio;
    case FK_IMAGE: return &icon_image;
    case FK_VIDEO: return &icon_video;
    case FK_TEXT: case FK_MD: return &icon_text;
    case FK_ARCHIVE: return &icon_archive;
    default: return &icon_file;
    }
}

static void draw_bg(void)
{
    gfx_clear(RGBA(8, 8, 20, 0x80));
    img_draw(&bgimg, 0, 0, W(), H(), 0x80);
    rect(0, 0, W(), H(), RGBA(0, 0, 0, 0x28));
}

void ui_splash(void (*work)(void))
{
    /* black -> dark-blue backdrop + logo. Fade in (45 frames), setup while the logo is fully visible, short hold,
       fade out. X or O skips once setup is done. */
    float sy = (float)H() / 480.0f;
    int ready = 0, released = 0, done_f = -1;
    LOGI("splash: start (%dx%d)", W(), H());
    for (int f = 0; f < 600; f++) {
        int a;
        if (f < 45) a = f * 0x80 / 45;
        else if (done_f < 0) a = 0x80;
        else { int d = f - done_f; if (d >= 80) break; a = d < 50 ? 0x80 : (80 - d) * 0x80 / 30; }
        log_frame = f;
        gfx_clear(RGBA(0, 0, 0, 0x80));
        img_draw(&splash_bg, 0, 0, W(), H(), a);
        img_draw(&logo, (W() - 512) / 2, (H() - 256 * sy) / 2, 512, 256 * sy, a);
        gfx_flip();
        if (f == 45) { if (work) { LOGI("splash: setup begin"); work(); LOGI("splash: setup end"); } ready = 1; done_f = f; }
        if (ready) {
            pad_poll(&pd);
            unsigned xo = PAD_CROSS | PAD_CIRCLE;
            if (!(pd.held & xo)) released = 1;
            else if (released && (pd.pressed & xo)) { LOGI("splash: skipped by user at f=%d", f); break; }
        }
    }
    LOGI("splash: end");
}

static int cmp(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (!strcmp(x->name, "..")) return -1;
    if (!strcmp(y->name, "..")) return 1;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    if (cfg.sort_mode == 1 && x->size != y->size) return x->size < y->size ? 1 : -1;
    if (cfg.sort_mode == 2) {
        const char *ex = strrchr(x->name, '.'), *ey = strrchr(y->name, '.');
        int c = strcasecmp(ex ? ex : "", ey ? ey : "");
        if (c) return c;
    }
    return strcasecmp(x->name, y->name);
}

static int host_auto = -1;                    /* -1 = not probed yet; probed once, then remembered */

static int dev_index(const char *root)
{
    for (int i = 0; i < NDEV; i++) if (!strcmp(devtab[i].root, root)) return i;
    return -1;
}
static void note_set(const char *root, const char *txt)
{
    int i = dev_index(root);
    if (i >= 0) snprintf(dev_note[i], sizeof dev_note[i], "%s", txt);
}
static void note_cd(void) { note_set("cdfs:/", disc_type > 0 ? fs_cd_name(disc_type) : "no disc"); }
/* memory card rows come from the last card check (the format screen and the rescan both fill it) */
static void note_cards(void)
{
    for (int p = 0; p < 2; p++) {
        const char *r = p ? "mc1:/" : "mc0:/";
        switch (fm_status[p]) {
        case 0:  note_set(r, fm_type[p] == 1 ? "PS1 card" : "ready"); break;
        case 1:  note_set(r, "not formatted"); break;
        case -1: note_set(r, "no card"); break;
        default: note_set(r, dev_scan ? "checking ..." : ""); break;
        }
    }
}
static int disc_visible(void) { return disc_type > 0; }

static void show_devices(void)
{
    if (host_auto < 0) { host_auto = fs_host_available(); note_set("host:/", host_auto > 0 ? "ready" : "not found"); }
    note_cards(); note_cd();
    strcpy(cwd, "root:");
    n_ents = 0; sel = 0; top = 0;
    for (int i = 0; i < NDEV; i++) {
        if (!devtab[i].always && !cfg.show_all_devices && !((cfg.host_dev || host_auto > 0) && !strcmp(devtab[i].root, "host:/")) && !(disc_visible() && !strcmp(devtab[i].root, "cdfs:/"))) continue;
        Entry *e = &ents[n_ents++];
        snprintf(e->name, sizeof e->name, "%s", devtab[i].root);
        e->name[strlen(e->name) - 1] = 0;                 /* "mc0:/" -> "mc0:" */
        e->is_dir = 1; e->size = 0; e->marked = 0;
    }
    LOGI("ui: devices root (%d entries)", n_ents);
}

static int dev_index_of_name(const char *name)    /* "mc0:" -> table row */
{
    for (int i = 0; i < NDEV; i++) if (!strncmp(devtab[i].root, name, strlen(name))) return i;
    return -1;
}

static const char *dev_label(const char *name)
{
    for (int i = 0; i < NDEV; i++) if (!strncmp(devtab[i].root, name, strlen(name))) return devtab[i].label;
    return name;
}

static void loading_frame(int n)
{
    float sy = (float)H() / 480.0f;
    static const char *const word[3] = { "Loading.", "Loading..", "Loading..." };
    gfx_clear(RGBA(0, 0, 0, 0x80));
    img_draw(&splash_bg, 0, 0, W(), H(), 0x80);
    img_draw(&logo, (W() - 256) / 2, 90 * sy, 256, 128 * sy, 0x80);
    text_s((W() - 10 * CWIDTH(0.7f)) / 2, 270 * sy, RGBA(255, 255, 255, 0x80), word[n % 3], 0.7f);
    gfx_flip();
}

static void apply_list(int n)
{
    n_ents = n;
    if (n_ents < 0) { n_ents = 0; say("Cannot open (device not ready?)"); LOGW("list %s failed", cwd); }
    if (n_ents >= 0 && fs_is_root(cwd) && n_ents < MAX_ENTRIES - 1) {      /* ".." at a device root leads to the devices list */
        memmove(ents + 1, ents, sizeof(Entry) * n_ents);
        strcpy(ents[0].name, ".."); ents[0].is_dir = 1; ents[0].size = 0; ents[0].marked = 0;
        n_ents++;
    }
    qsort(ents, n_ents, sizeof(Entry), cmp);
    if (sel >= n_ents) sel = n_ents ? n_ents - 1 : 0;
    top = 0;
    if (keep_name[0]) {
        for (int i = 0; i < n_ents; i++) if (!strcmp(ents[i].name, keep_name)) { sel = i; break; }
        keep_name[0] = 0;
    }
}

static void job_thread(void *arg)
{
    (void)arg;
    job_result = fs_list(job_path, job_ents, MAX_ENTRIES - 1, cfg.show_hidden);
    job_done = 1;
    ExitDeleteThread();
}

static void refresh(void)
{
    if (at_devices()) { show_devices(); return; }
    if (job_running) { say("Storage busy - cancel (O) or restart"); LOGW("refresh refused: a list job is still running"); return; }
    if (!is_mc(cwd)) {                                   /* USB: quick, done inline */
        if (is_cd(cwd)) {
            if (disc_type == 0) { show_devices(); say("No disc in the drive"); return; }
            loading_frame(1);                            /* the drive may need a moment to spin up */
        }
        int n = fs_list(cwd, ents, MAX_ENTRIES - 1, cfg.show_hidden);
        apply_list(n);
        if (n >= 0) { strcpy(prev_good, cwd); if (!strncmp(cwd, "mass", 4)) usb_was_ok = 1; }
        else if (strncmp(cwd, "mass", 4)) say("Not available (driver not loaded or no device)");
        else if (usb_was_ok && !fs_dev_ready("mass:/")) { st = S_USB; LOGW("usb: device lost (list failed)"); }
        return;
    }
    if (sjob_running) { show_devices(); say("Memory card busy (saving settings) - try again in a moment"); return; }   /* back to the devices list, not a stale list under mc0: */
    if (iop_load_mc() != 0) { show_devices(); say("Memory card driver failed to load"); return; }
    if (cfg.pause_music_mc) bgm_suspend();                /* the card read stalls the IOP: keep audio traffic off it */
    n_ents = 0; sel = 0; top = 0;                         /* do not keep showing the previous list (e.g. the devices) behind the loading box */
    strncpy(job_path, cwd, sizeof job_path - 1); job_path[sizeof job_path - 1] = 0;
    job_done = 0; job_abandoned = 0; job_running = 1; job_start = frame;
    ee_thread_t t;
    ee_thread_status_t ts;
    ReferThreadStatus(GetThreadId(), &ts);
    memset(&t, 0, sizeof t);
    t.func = job_thread; t.stack = job_stack; t.stack_size = sizeof job_stack;
    t.gp_reg = &_gp; t.initial_priority = ts.current_priority + 4;
    if (t.initial_priority > 126) t.initial_priority = 126;
    int id = CreateThread(&t);
    if (id < 0) { job_running = 0; bgm_resume(); LOGE("ui: CreateThread failed for list job"); say("Could not start reader"); return; }
    StartThread(id, NULL);
    LOGI("ui: reading %s in the background", job_path);
    st = S_LOADING;
}

/* a real refresh: forget cached detection (emulator host:), read the folder again from the device, keep the cursor on the same entry */
static void rescan_devices(void);
static void refresh_real(void)
{
    if (at_devices()) { rescan_devices(); return; }
    keep_name[0] = 0;
    if (n_ents && sel >= 0 && sel < n_ents && strcmp(ents[sel].name, "..")) snprintf(keep_name, sizeof keep_name, "%s", ents[sel].name);
    refresh();
}

static int rows_visible(void) { return (H() - 44 - (cfg.show_hints ? 58 : 12)) / ROW_H; }

static void fmt_size(char *o, u64 s)
{
    if (s < 1024) snprintf(o, 16, "%u B", (unsigned)s);
    else if (s < 1048576) snprintf(o, 16, "%u KB", (unsigned)(s >> 10));
    else snprintf(o, 16, "%u.%u MB", (unsigned)(s >> 20), (unsigned)((s & 0xFFFFF) * 10 >> 20));
}

static void overlay(void) { rect(0, 0, W(), H(), RGBA(0, 0, 0, 0x40)); }
static void box(float x, float y, float w, float h)
{
    rect(x - 2, y - 2, w + 4, h + 4, RGBA(140, 120, 255, 0x80));
    rect(x, y, w, h, RGBA(18, 18, 44, 0x78));
}

static void draw_list(void)
{
    draw_bg();
    rect(0, 0, W(), 40, RGBA(10, 10, 30, 0x60));
    img_draw(&logo, 6, 2, 64, 32, 0x80);
    mtext(80, 3, RGBA(255, 255, 255, 0x80), at_devices() ? "Devices" : cwd, 38, frame, TEXT_SCALE);
    if (bgm_active()) {
        char m[120];
        snprintf(m, sizeof m, "%sMusic: %s%s%s", bgm_looping() ? "Looped " : "", bgm_title(), bgm_paused() ? " [paused]" : "", bgm_muted() ? " [mute]" : "");
        mtext(80, 22, RGBA(120, 255, 170, 0x80), m, 48, frame, 0.45f);
    }
    int vis = rows_visible();
    int namecols = (int)((W() - 135 - 38 - 6) / CWIDTH(TEXT_SCALE));
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    for (int i = 0; i < vis && top + i < n_ents; i++) {
        Entry *e = &ents[top + i];
        float y = 44 + i * ROW_H;
        int is_sel = (top + i == sel);
        if (is_sel) {
            int p = (frame / 2) % 32; if (p > 16) p = 32 - p;
            rect(8, y, W() - 16, ROW_H - 2, RGBA(110, 90, 255, 0x30 + p * 2));
        }
        img_draw(icon_for(e), 14, y + 2, 16, 16, 0x80);
        u64 col = e->marked ? RGBA(255, 230, 90, 0x80) : RGBA(235, 235, 255, 0x80);
        char nm[130], full[140];
        if (at_devices()) snprintf(full, sizeof full, "%s", dev_label(e->name));
        else { safe_name(nm, e->name, 120); snprintf(full, sizeof full, "%s%s", e->marked ? "* " : "", nm); }
        if (is_sel) mtext(38, y + 2, col, full, namecols, frame - sel_frame, TEXT_SCALE);   /* long names scroll */
        else ttext(38, y + 2, col, full, namecols, TEXT_SCALE);
        if (at_devices()) {
            int di = dev_index_of_name(e->name);
            if (di >= 0 && dev_note[di][0]) {
                int bad = !strcmp(dev_note[di], "not found") || !strcmp(dev_note[di], "no card") || !strcmp(dev_note[di], "no disc") || !strcmp(dev_note[di], "not formatted") || !strcmp(dev_note[di], "unreadable");
                text_s(W() - 200, y + 4, bad ? RGBA(255, 160, 140, 0x80) : RGBA(130, 255, 170, 0x80), dev_note[di], 0.42f);
            }
        } else {
            if (!e->is_dir) { char sz[16]; fmt_size(sz, e->size); text(W() - 125, y + 2, RGBA(170, 190, 220, 0x80), sz); }
            else if (strcmp(e->name, "..")) text(W() - 125, y + 2, RGBA(170, 190, 220, 0x80), "<DIR>");
        }
    }
    if (n_ents == 0 && st != S_LOADING) text_s(30, 60, RGBA(255, 160, 160, 0x80), is_mc(cwd) ? "(empty or not available)" : is_cd(cwd) ? "(empty, or the disc cannot be read)" : "(no USB found yet - R3: retry, L1/R1: memory card)", 0.42f);
    if (cfg.show_hints) {
        rect(0, H() - 58, W(), 58, RGBA(10, 10, 30, 0x70));
        text_s(10, H() - 54, RGBA(215, 215, 240, 0x80), "X: Open O: Back /\\:File Menu [ ]: Mark L1+R1: All", 0.42f);
        text_s(10, H() - 38, RGBA(170, 180, 215, 0x80), "Select: Devices    Start: Menu    L1/R1: Device", 0.42f);
        text_s(10, H() - 22, RGBA(170, 180, 215, 0x80), "L2:Mute R2:Hidden L3:Sort R3:Refresh L2+R2:Credits", 0.42f);
    }
    if (toast_t > 0) {
        toast_t--;
        float ty = H() - (cfg.show_hints ? 112 : 64);
        box(60, ty, W() - 120, 30);
        mtext(74, ty + 7, RGBA(255, 255, 255, 0x80), toast, (int)((W() - 120 - 28) / CWIDTH(TEXT_SCALE)), frame - toast_start, TEXT_SCALE);
    }
    if (!pd.connected) text_s(W() - 280, 46, RGBA(255, 120, 120, 0x80), "controller not connected", 0.42f);
}

static int ui_progress(const char *what, u64 done, u64 total)
{
    draw_list(); overlay();
    box(60, H() / 2 - 48, W() - 120, 96);
    text(76, H() / 2 - 38, RGBA(255, 255, 255, 0x80), "Working ...");
    mtext(76, H() / 2 - 14, RGBA(200, 220, 255, 0x80), what, (int)((W() - 152) / CWIDTH(0.45f)), frame, 0.45f);
    float f = total ? (float)((double)done / (double)total) : 0.0f;
    rect(76, H() / 2 + 12, W() - 152, 12, RGBA(60, 60, 90, 0x80));
    rect(76, H() / 2 + 12, (W() - 152) * f, 12, RGBA(120, 255, 160, 0x80));
    text_s(76, H() / 2 + 30, RGBA(200, 200, 230, 0x80), "O: Cancel", 0.42f);
    gfx_flip();
    pad_poll(&pd);
    frame++;
    return (pd.pressed & PAD_CIRCLE) ? 1 : 0;
}

/* mode 0 = none, 1 = all, 2 = toggle (everything if anything is unmarked, else none), 3 = invert */
static void select_all(int mode)
{
    if (at_devices() || !n_ents) { say("Nothing to select here"); return; }
    int unmarked = 0, count = 0;
    for (int i = 0; i < n_ents; i++) if (strcmp(ents[i].name, "..") && !ents[i].marked) unmarked++;
    for (int i = 0; i < n_ents; i++) {
        if (!strcmp(ents[i].name, "..")) continue;
        if (mode == 0) ents[i].marked = 0;
        else if (mode == 1) ents[i].marked = 1;
        else if (mode == 2) ents[i].marked = unmarked ? 1 : 0;
        else ents[i].marked ^= 1;
        count += ents[i].marked;
    }
    char m[48]; snprintf(m, sizeof m, count ? "%d item(s) selected" : "Selection cleared", count); say(m);
}

static void gather_targets(void)
{
    n_tg = 0;
    for (int i = 0; i < n_ents && n_tg < 256; i++)
        if (ents[i].marked) fs_join(tg[n_tg++], cwd, ents[i].name);
    if (!n_tg && n_ents && strcmp(ents[sel].name, ".."))
        fs_join(tg[n_tg++], cwd, ents[sel].name);
}

/* load and play a song in the background: the UI keeps working while a big MP3 is read and opened */
static char song_name[64];
static void start_song(const char *path, const char *name, int loop)
{
    int rc = bgm_play_file_async(path, loop);
    if (rc == 0) { snprintf(song_name, sizeof song_name, "%.60s", name); say_t(loop ? "Loading song (will loop) ..." : "Loading song ...", 360); }
    else if (rc == -1) say("Still loading the previous song - one moment");
    else say("Could not start the song loader");
}
static void poll_song(void)
{
    int rc = 0;
    if (bgm_load_poll(&rc) != 2) return;
    if (rc == 0) { char m[96]; snprintf(m, sizeof m, "%s: %.40s", bgm_looping() ? "Playing (looped)" : "Playing", song_name); say(m); }
    else if (!strcmp(song_name, "default music")) say("No default music found");
    else say(rc == -10 ? "MP3/MP2 need libmad: not in this build (WAV and FLAC work)" : "Cannot play this file (see log)");
}
static int is_7z_name(const char *n)
{
    size_t l = strlen(n);
    return l > 3 && !strcasecmp(n + l - 3, ".7z");
}
static int is_zip_name(const char *n)          /* any archive this app can extract: .zip or .7z */
{
    size_t l = strlen(n);
    return (l > 4 && !strcasecmp(n + l - 4, ".zip")) || is_7z_name(n);
}

static char uz_src[PATH_MAX_LEN];                        /* the zip waiting for a yes / no */
static char uz_name[96];

static void show_text(const char *title, const char *l1, const char *l2, const char *l3, const char *l4);
static char ow_file[PATH_MAX_LEN], ow_name[96];          /* a video / audio file waiting to be handed to the media player */

static const char *player_base(void) { return cfg.player[0] ? fs_basename(cfg.player) : "(none)"; }

static void ask_openwith(const char *path, const char *name)
{
    if (!cfg.player[0]) {
        const char *ex = strrchr(name, '.'); ex = ex ? ex + 1 : "";
        char l1[64]; snprintf(l1, sizeof l1, "%.20s  (.%.8s)", name, ex);
        show_text("Needs a media player", l1, "This app cannot decode this format itself.",
                  "Pick your player ELF (SMS, for example), press Triangle", "and choose \"Set as media player\". Then open this file again.");
        return;
    }
    snprintf(ow_file, sizeof ow_file, "%s", path);
    snprintf(ow_name, sizeof ow_name, "%s", name);
    st = S_OPENWITH;
}

static void do_openwith(void)
{
    if (!fs_exists(cfg.player)) { say("The media player ELF was not found"); LOGW("openwith: missing %s", cfg.player); return; }
    LOGI("openwith: %s -> %s", cfg.player, ow_file);
    loading_frame(0);
    if (cfg_dirty) cfg_save(1);
    loading_frame(1);
    char *av[2] = { cfg.player, ow_file };
    int rc = launch_elf_args(cfg.player, 2, av);
    LOGE("openwith: launch failed rc=%d", rc);
    say("Could not start the media player (see log)");
}

static void ask_unzip(const char *name)
{
    if (is_cd(cwd)) { say("The disc is read-only: copy the zip to USB first"); return; }
    fs_join(uz_src, cwd, name);
    snprintf(uz_name, sizeof uz_name, "%s", name);
    st = S_UNZIP;
}

static void do_unzip(void)
{
    char base[96]; snprintf(base, sizeof base, "%s", uz_name);
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    if (!base[0]) strcpy(base, "unzipped");
    char dest[PATH_MAX_LEN];
    fs_join(dest, cwd, base);
    if ((is_mc(cwd) || is_mc(uz_src)) && iop_load_mc() != 0) { say("Memory card driver failed to load"); return; }
    if (fs_exists(dest)) fs_unique(dest);                 /* never mix into an existing folder */
    fs_mkdir(dest);
    if (!fs_is_dir(dest)) { say("Could not create the folder"); LOGE("unzip: mkdir %s failed", dest); return; }
    int ok = 0, bad = 0;
    int is7 = is_7z_name(uz_name);
    int rc = is7 ? fs_un7z(uz_src, dest, ui_progress, &ok, &bad) : fs_unzip(uz_src, dest, ui_progress, &ok, &bad);
    char m[96];
    if (rc == -9)      snprintf(m, sizeof m, "Unzip cancelled (%d files done)", ok);
    else if (rc == -2) snprintf(m, sizeof m, is7 ? "Not a valid or damaged 7z file" : "Not a valid zip file");
    else if (rc == -3 && is7) snprintf(m, sizeof m, "7z: %.60s", fs_un7z_msg()[0] ? fs_un7z_msg() : "unsupported archive");
    else if (rc == -3) snprintf(m, sizeof m, "ZIP64 or very large zips are not supported");
    else if (rc == -4 && is7) snprintf(m, sizeof m, "Not enough memory for this 7z (big dictionary)");
    else if (rc == 0 && bad && is7 && fs_un7z_msg()[0]) snprintf(m, sizeof m, "Extracted %d, %d failed: %.40s", ok, bad, fs_un7z_msg());
    else if (rc == -5) snprintf(m, sizeof m, "This build has no zip support");
    else if (rc < 0)   snprintf(m, sizeof m, "Could not read the zip (%d)", rc);
    else if (bad)      snprintf(m, sizeof m, "Unzipped %d, skipped or failed %d", ok, bad);
    else               snprintf(m, sizeof m, "Unzipped %d item(s)", ok);
    say(m); LOGI("%s", m);
    snprintf(keep_name, sizeof keep_name, "%s", fs_basename(dest));
    refresh();
}

/* Compress the marked items (or the highlighted one) into a .zip / .7z next to them */
static void do_compress(int kind)
{
    if (at_devices()) { say("Open a device first"); return; }
    if (is_cd(cwd)) { say("The disc is read-only: copy the files to USB first"); return; }
    gather_targets();
    if (!n_tg) { say("Nothing selected"); return; }
    if ((is_mc(cwd)) && iop_load_mc() != 0) { say("Memory card driver failed to load"); return; }
    static const char *names[256];
    for (int i = 0; i < n_tg; i++) names[i] = fs_basename(tg[i]);
    char base[96];
    if (n_tg == 1) {
        snprintf(base, sizeof base, "%.90s", names[0]);
        if (!fs_is_dir(tg[0])) { char *d = strrchr(base, '.'); if (d && d != base) *d = 0; }      /* photo.png -> photo.zip */
    } else {
        char t[PATH_MAX_LEN]; snprintf(t, sizeof t, "%s", cwd);
        size_t l = strlen(t); while (l && t[l - 1] == '/') t[--l] = 0;
        const char *b = strrchr(t, '/'); b = b ? b + 1 : t;
        if (!*b || strchr(b, ':')) b = "archive";
        snprintf(base, sizeof base, "%.90s", b);
    }
    char out[PATH_MAX_LEN], file[110];
    snprintf(file, sizeof file, "%s%s", base, kind ? ".7z" : ".zip");
    fs_join(out, cwd, file);
    if (fs_exists(out)) {                                    /* never overwrite: name_2.zip, name_3.zip ... */
        for (int i = 2; i < 100; i++) {
            snprintf(file, sizeof file, "%s_%d%s", base, i, kind ? ".7z" : ".zip");
            fs_join(out, cwd, file);
            if (!fs_exists(out)) break;
        }
    }
    LOGI("compress: %d item(s) -> %s", n_tg, out);
    int ok = 0, bad = 0; char em[96];
    int rc = compress_make(kind, out, cwd, names, n_tg, ui_progress, &ok, &bad, em, sizeof em);
    char m[96];
    if (rc == 0 && bad) snprintf(m, sizeof m, "Created %.40s (%d files could not be read)", file, bad);
    else if (rc == 0)   snprintf(m, sizeof m, "Created %.50s (%d item(s))", file, ok);
    else if (rc == -9)  snprintf(m, sizeof m, "Compression cancelled");
    else if (rc == -2)  snprintf(m, sizeof m, "Cannot create the archive here (read-only or full?)");
    else if (rc == -3)  snprintf(m, sizeof m, "Write error - disk full? (archive removed)");
    else if (rc == -4)  snprintf(m, sizeof m, "Not enough memory to compress this");
    else if (rc == -5)  snprintf(m, sizeof m, "Too big: %.60s", em[0] ? em : "limit reached");
    else                snprintf(m, sizeof m, "Could not read the selected items");
    say(m); LOGI("%s", m);
    snprintf(keep_name, sizeof keep_name, "%s", rc == 0 ? file : names[0]);
    refresh();
}

static int is_mp3_name(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".mp3");
}

static void fjob_thread(void *arg)
{
    (void)arg;
    fjob_rc = -1;
#ifndef HAVE_CDVD
    if (dev_scan) disc_type = fs_dev_ready("cdfs:/") ? 0x7F : 0;      /* no disc-type query in this build: ask the file system */
#endif
    if (iop_load_mc() == 0) {
        if (fjob_mode == 0) { for (int p = 0; p < 2; p++) fm_status[p] = fs_mc_info(p, &fm_type[p], &fm_free[p]); fjob_rc = 0; }
        else if (fjob_mode == 1) fjob_rc = fs_mc_wipe(fjob_port);
        else fjob_rc = fs_mc_format(fjob_port);
    }
    fjob_done = 1;
    ExitDeleteThread();
}

static int start_fjob(int mode, int port)
{
    ee_thread_t t; ee_thread_status_t ts;
    ReferThreadStatus(GetThreadId(), &ts);
    memset(&t, 0, sizeof t);
    t.func = fjob_thread; t.stack = fjob_stack; t.stack_size = sizeof fjob_stack; t.gp_reg = &_gp;
    t.initial_priority = ts.current_priority - 1; if (t.initial_priority < 2) t.initial_priority = 2;
    fjob_mode = mode; fjob_port = port; fjob_done = 0; fjob_running = 1;
    if (mode == 0) { fm_status[0] = fm_status[1] = -2; }
    if (cfg.pause_music_mc) bgm_suspend();                       /* card access stalls the IOP: keep audio traffic off it */
    int id = CreateThread(&t);
    if (id < 0) { fjob_running = 0; bgm_resume(); LOGE("format: CreateThread failed"); return -1; }
    StartThread(id, NULL);
    return 0;
}

static void open_format(void)
{
    if (mc_busy()) { say("Memory card busy - try again in a moment"); return; }
    fm_step = 0; fm_card = 0; fm_method = 0; fm_conf = 0;
    if (start_fjob(0, 0) != 0) { say("Could not start the card check"); return; }
    st = S_FORMAT;
}

/* rescan everything: USB, emulator host, disc drive and both memory cards, then say what changed */
static void rescan_devices(void)
{
    memcpy(dev_prev, dev_note, sizeof dev_prev);
    host_auto = -1;
    int t = fs_cd_type(); if (t >= 0) disc_type = t;
    note_set("mass:/", fs_dev_ready("mass:/") ? "ready" : "not found");
    host_auto = fs_host_available(); note_set("host:/", host_auto > 0 ? "ready" : "not found");
    if (mc_busy() || start_fjob(0, 0) != 0) {
        dev_scan = 0; show_devices();
        say("Rescanned (memory cards busy - not checked)");
        return;
    }
    dev_scan = 1;
    show_devices();
    say_t("Rescanning devices ...", 600);
    LOGI("devices: rescan started");
}

static void rescan_done(void)
{
    dev_scan = 0;
    char keep[96]; keep[0] = 0;
    if (at_devices() && n_ents) snprintf(keep, sizeof keep, "%s", ents[sel].name);
    if (at_devices()) {
        show_devices();
        for (int i = 0; i < n_ents; i++) if (!strcmp(ents[i].name, keep)) { sel = i; break; }
    } else note_cards();
    char msg[96]; msg[0] = 0; int n = 0;
    for (int i = 0; i < NDEV; i++) {
        if (!dev_prev[i][0] || !strcmp(dev_prev[i], dev_note[i])) continue;
        size_t l = strlen(msg);
        if (l < sizeof msg - 24) snprintf(msg + l, sizeof msg - l, "%s%.4s %s", n ? ", " : "", devtab[i].root, dev_note[i][0] ? dev_note[i] : "?");
        n++;
    }
    if (n) { char m2[110]; snprintf(m2, sizeof m2, "Changed: %s", msg); say(m2); }
    else say("Devices rescanned - nothing changed");
    LOGI("devices: rescan done (%d changes)", n);
}

static void open_menu(int main_menu)
{
    n_menu = 0; menu_sel = 0;
    if (main_menu) {
        menu_items[n_menu++] = "Settings"; menu_items[n_menu++] = "Refresh"; menu_items[n_menu++] = "Devices (root)"; menu_items[n_menu++] = "Rescan devices"; menu_items[n_menu++] = "Format memory card"; menu_items[n_menu++] = "Music: pause / resume";
        menu_items[n_menu++] = "Music: restore default"; menu_items[n_menu++] = "Log viewer"; if (cfg.player[0]) menu_items[n_menu++] = "Clear media player";
        menu_items[n_menu++] = "Sort: next mode"; menu_items[n_menu++] = "Credits";
        menu_items[n_menu++] = "Exit to OSD"; menu_items[n_menu++] = "Cancel";
    } else {
        menu_items[n_menu++] = "Copy"; menu_items[n_menu++] = "Cut (move)"; menu_items[n_menu++] = "Paste here";
        menu_items[n_menu++] = "Delete"; menu_items[n_menu++] = "Rename"; menu_items[n_menu++] = "New folder";
        menu_items[n_menu++] = "Select all"; menu_items[n_menu++] = "Select none"; menu_items[n_menu++] = "Invert selection";
        menu_items[n_menu++] = "Compress to .zip"; menu_items[n_menu++] = "Compress to .7z";
        if (n_ents && !ents[sel].is_dir && is_mp3_name(ents[sel].name)) menu_items[n_menu++] = "Loop MP3";
        if (n_ents && !ents[sel].is_dir && is_zip_name(ents[sel].name)) menu_items[n_menu++] = "Unzip";
        if (n_ents && !ents[sel].is_dir && ftype_of(ents[sel].name) == FK_ELF) menu_items[n_menu++] = "Set as media player";
        if (n_ents && !ents[sel].is_dir && ftype_external(ents[sel].name)) menu_items[n_menu++] = "Open with player";
        menu_items[n_menu++] = "Info"; menu_items[n_menu++] = "Hex view"; menu_items[n_menu++] = "Refresh"; menu_items[n_menu++] = "Cancel";
    }
    st = S_MENU;
}

static void show_text(const char *title, const char *l1, const char *l2, const char *l3, const char *l4)
{
    snprintf(text_title, sizeof text_title, "%s", title);
    n_text = 0; text_small = 0;
    const char *ls[] = { l1, l2, l3, l4 };
    for (int i = 0; i < 4; i++) if (ls[i]) snprintf(text_lines[n_text++], 72, "%.70s", ls[i]);
    st = S_TEXT;
}

static void do_copy(int move)
{
    gather_targets();
    n_clip = n_tg; clip_move = move;
    memcpy(clip, tg, sizeof(char[PATH_MAX_LEN]) * n_tg);
    char m[64]; snprintf(m, sizeof m, "%s %d item(s)", move ? "Cut" : "Copied", n_clip); say(m);
}

static void do_paste(void)
{
    if (!n_clip) { say("Clipboard empty"); return; }
    if (at_devices()) { say("Open a device first"); return; }
    int ok = 0, bad = 0;
    for (int i = 0; i < n_clip; i++) {
        char d[PATH_MAX_LEN]; size_t sl = strlen(clip[i]);
        fs_join(d, cwd, fs_basename(clip[i]));
        if (!strncmp(d, clip[i], sl) && (d[sl] == '/' || d[sl] == 0)) { bad++; LOGW("refuse paste into itself"); continue; }
        if ((is_mc(cwd) || is_mc(clip[i])) && iop_load_mc() != 0) { bad++; continue; }
        if (fs_exists(d)) fs_unique(d);
        int rc = clip_move ? fs_move(clip[i], d, ui_progress) : fs_copy(clip[i], d, ui_progress);
        if (rc == 0) ok++; else { bad++; LOGE("paste %s rc=%d", clip[i], rc); }
        if (rc == -9) break;
    }
    if (clip_move && ok) n_clip = 0;
    char m[64]; snprintf(m, sizeof m, "Pasted %d, failed %d", ok, bad); say(m); LOGI("%s", m);
    refresh();
}

static void do_delete(void)
{
    int ok = 0, bad = 0;
    for (int i = 0; i < n_tg; i++) { if (fs_delete(tg[i], ui_progress) == 0) ok++; else bad++; }
    char m[64]; snprintf(m, sizeof m, "Deleted %d, failed %d", ok, bad); say(m); LOGI("%s", m);
    refresh();
}

static void hex_load(void) { hex_len = fs_read_at(hex_path, hex_off, hex_buf, sizeof hex_buf); if (hex_len < 0) hex_len = 0; }
static void open_hex(void)
{
    if (!n_ents || ents[sel].is_dir) { say("Select a file"); return; }
    fs_join(hex_path, cwd, ents[sel].name); hex_off = 0; hex_load(); st = S_HEX;
}

static void open_osk(int mode)
{
    osk_mode = mode; osk_len = 0; osk_buf[0] = 0; osk_x = osk_y = 0;
    if (mode == 0 && n_ents && strcmp(ents[sel].name, "..")) { strncpy(osk_buf, ents[sel].name, 63); osk_len = strlen(osk_buf); }
    st = S_OSK;
}

static void osk_done(void)
{
    char p[PATH_MAX_LEN];
    if (!osk_len) { st = S_LIST; return; }
    fs_join(p, cwd, osk_buf);
    if (fs_exists(p)) { say("Name already exists"); st = S_LIST; return; }
    if (osk_mode == 1) { say(fs_mkdir(p) >= 0 ? "Folder created" : "mkdir failed"); }
    else { char o[PATH_MAX_LEN]; fs_join(o, cwd, ents[sel].name); say(fs_rename(o, p) >= 0 ? "Renamed" : "Rename failed"); }
    st = S_LIST; refresh();
}

static void sync_dev_from_cwd(void)
{
    for (int i = 0; i < 3; i++) if (!strncmp(cwd, devs[i], 4)) dev_idx = i;
}

static void change_dev(int d)
{
    dev_idx = (dev_idx + d + 3) % 3;
    strcpy(cwd, devs[dev_idx]); sel = 0; refresh();
    LOGI("device: %s", cwd);
}

static void go_parent(void)
{
    if (at_devices()) return;
    if (fs_is_root(cwd)) { show_devices(); return; }
    fs_parent(cwd); sel = 0; refresh();
}

static void enter_or_launch(void)
{
    if (!n_ents) return;
    Entry *e = &ents[sel];
    if (at_devices()) {
        if (!strcmp(e->name, "cdfs:")) {
            if (disc_type == 0) { say("No disc in the drive"); return; }
            if (disc_type == 0xFD) { say("Audio CD: no files to browse"); return; }
            if (disc_type == 0xFF) { say("This disc cannot be read"); return; }
        }
        snprintf(cwd, sizeof cwd, "%s/", e->name);
        sync_dev_from_cwd(); sel = 0; refresh();
        LOGI("nav: %s", cwd);
        return;
    }
    if (e->is_dir) {
        if (!strcmp(e->name, "..")) go_parent();
        else { char p[PATH_MAX_LEN]; fs_join(p, cwd, e->name); strcpy(cwd, p); size_t l = strlen(cwd); if (cwd[l - 1] != '/') strcat(cwd, "/"); sel = 0; refresh(); LOGI("nav: %s", cwd); }
        return;
    }
    char p[PATH_MAX_LEN];
    fs_join(p, cwd, e->name);
    FKind fk = ftype_of(e->name);
    if (is_zip_name(e->name)) { ask_unzip(e->name); return; }
    if (ftype_external(e->name)) { ask_openwith(p, e->name); return; }
    if (fk == FK_OTHER) {                              /* name says nothing (no extension, dotfile, odd extension): look inside */
        static unsigned char sniff[1024];
        int sn = fs_read_at(p, 0, sniff, sizeof sniff);
        if (sn > 0 && ftype_looks_like_text(sniff, sn)) {
            if (viewer_open_text(cwd, ents, n_ents, sel) == 0) { st = S_VIEWER; LOGI("viewer: %s (text by content)", p); }
            else say("Cannot open this file");
            return;
        }
    }
    if (!ftype_supported(e->name)) {
        const char *ex = strrchr(e->name, '.'); ex = ex ? ex + 1 : "";
        char l1[64], l2[64];
        snprintf(l1, sizeof l1, "%.20s  (.%.8s)", e->name, ex);
        if (fk == FK_VIDEO)        snprintf(l2, sizeof l2, "Video playback is not supported.");
        else if (fk == FK_ARCHIVE) snprintf(l2, sizeof l2, !strcasecmp(ex, "rar") ? "RAR is not supported (use .zip or .7z)." : "Only .zip and .7z can be extracted.");
        else if (fk == FK_IMAGE)   snprintf(l2, sizeof l2, !strcasecmp(ex, "tif") || !strcasecmp(ex, "tiff") ? "TIFF is not supported." : "Images: PNG, JPG, BMP, GIF and TGA.");
        else if (fk == FK_AUDIO)   snprintf(l2, sizeof l2, "Audio: WAV, MP3, MP2 and FLAC.");
        else                       snprintf(l2, sizeof l2, "Unknown file type (not text either).");
        LOGI("unsupported: %s", e->name);
        show_text("Unsupported file", l1, l2, fk == FK_OTHER ? "Tip: Menu > Hex view shows the bytes." : "This file was not opened.", NULL);
        return;
    }
    switch (fk) {
    case FK_ELF: {
        LOGI("open elf %s", p);
        loading_frame(0);                          /* instead of the console showing garbage while the next program loads */
        if (cfg_dirty) cfg_save(1);
        loading_frame(1);
        loading_frame(2);
        int rc = launch_elf(p);
        LOGE("launch failed rc=%d", rc);
        say("Launch failed (see log)");
        break; }
    case FK_TEXT: case FK_MD: case FK_IMAGE:
        if (viewer_open(cwd, ents, n_ents, sel) == 0) { st = S_VIEWER; LOGI("viewer: %s", p); }
        else say("Cannot open this file");
        break;
    case FK_AUDIO: {
        start_song(p, e->name, 0);
        break; }
    default: break;
    }
}

static void menu_pick(void)
{
    const char *it = menu_items[menu_sel];
    LOGI("menu: %s", it);
    st = S_LIST;
    int ro = is_cd(cwd);                                   /* a disc cannot be changed */
    if (!strcmp(it, "Copy")) do_copy(0);
    else if (ro && (!strcmp(it, "Cut (move)") || !strcmp(it, "Paste here") || !strcmp(it, "Delete") || !strcmp(it, "Rename") || !strcmp(it, "New folder"))) say("The disc is read-only");
    else if (!strcmp(it, "Cut (move)")) do_copy(1);
    else if (!strcmp(it, "Paste here")) do_paste();
    else if (!strcmp(it, "Delete")) { gather_targets(); if (!n_tg) say("Nothing selected"); else if (cfg.confirm_delete) st = S_CONFIRM; else do_delete(); }
    else if (!strcmp(it, "Rename")) open_osk(0);
    else if (!strcmp(it, "New folder")) open_osk(1);
    else if (!strcmp(it, "Select all")) select_all(1);
    else if (!strcmp(it, "Select none")) select_all(0);
    else if (!strcmp(it, "Invert selection")) select_all(3);
    else if (!strcmp(it, "Compress to .zip") || !strcmp(it, "Compress to .7z")) {
        if (ro) say("The disc is read-only"); else do_compress(!strcmp(it, "Compress to .7z"));
    }
    else if (!strcmp(it, "Hex view")) open_hex();
    else if (!strcmp(it, "Set as media player")) {
        if (n_ents && !ents[sel].is_dir) { char pp[PATH_MAX_LEN]; fs_join(pp, cwd, ents[sel].name); snprintf(cfg.player, sizeof cfg.player, "%s", pp); char m[96]; snprintf(m, sizeof m, "Media player: %s", ents[sel].name); say(m); LOGI("player set: %s", pp); }
    }
    else if (!strcmp(it, "Clear media player")) { cfg.player[0] = 0; say("Media player cleared"); }
    else if (!strcmp(it, "Open with player")) { if (n_ents && !ents[sel].is_dir) { char pp[PATH_MAX_LEN]; fs_join(pp, cwd, ents[sel].name); ask_openwith(pp, ents[sel].name); } }
    else if (!strcmp(it, "Unzip")) { if (n_ents && !ents[sel].is_dir && is_zip_name(ents[sel].name)) ask_unzip(ents[sel].name); }
    else if (!strcmp(it, "Loop MP3")) { if (n_ents && !ents[sel].is_dir) { char lp[PATH_MAX_LEN]; fs_join(lp, cwd, ents[sel].name); start_song(lp, ents[sel].name, 1); } }
    else if (!strcmp(it, "Info")) {
        if (n_ents && strcmp(ents[sel].name, "..")) {
            char a[72], b[72], s[16]; fmt_size(s, ents[sel].size);
            fs_join(a, cwd, ents[sel].name); snprintf(b, sizeof b, "%s  %s", ents[sel].is_dir ? "Folder" : "File", ents[sel].is_dir ? "" : s);
            show_text("Info", ents[sel].name, a, b, NULL);
        }
    }
    else if (!strcmp(it, "Devices (root)")) { show_devices(); }
    else if (!strcmp(it, "Rescan devices")) rescan_devices();
    else if (!strcmp(it, "Format memory card")) open_format();
    else if (!strcmp(it, "Music: pause / resume")) { bgm_toggle_pause(); say(bgm_paused() ? "Music paused" : "Music resumed"); }
    else if (!strcmp(it, "Music: restore default")) { if (bgm_restore_async() == 0) { snprintf(song_name, sizeof song_name, "default music"); say("Restoring default music ..."); } else say("Still loading - one moment"); }
    else if (!strcmp(it, "Log viewer")) {
        n_text = 0;
        { const char *dn[] = { "USB", "mc0", "mc1" }; int ld = log_dest(); snprintf(text_title, sizeof text_title, "Log -> %s (newest first)", ld >= 0 ? dn[ld] : "memory only"); }
        text_small = 1;
        for (int i = 0; i < 20 && log_line(i); i++) snprintf(text_lines[n_text++], 72, "%.70s", log_line(i));
        st = S_TEXT;
    }
    else if (!strcmp(it, "Sort: next mode")) { cfg.sort_mode = (cfg.sort_mode + 1) % 3; refresh(); say(cfg.sort_mode == 0 ? "Sort: name" : cfg.sort_mode == 1 ? "Sort: size" : "Sort: type"); }
    else if (!strcmp(it, "Credits")) st = S_CREDITS;
    else if (!strcmp(it, "Settings")) { st = S_SETTINGS; set_sel = 0; }
    else if (!strcmp(it, "Refresh")) { refresh_real(); if (st == S_LIST) say("Refreshed"); }
    else if (!strcmp(it, "Exit to OSD")) { if (cfg_dirty) cfg_save(1); exit_to_osd(); }
}

static void update_list(void)
{
    int dir = 0;
    if (pd.held & PAD_UP || pd.ly < -50) dir = -1;
    if (pd.held & PAD_DOWN || pd.ly > 50) dir = 1;
    int step = nav_step(dir, &holdf);
    if (step && n_ents) {                       /* first press past either end wraps around; holding stops at the end */
        int ns = sel + step;
        if (ns < 0) ns = n_ents - 1;                /* wraps around, also while the button is held */
        else if (ns >= n_ents) ns = 0;
        sel = ns;
    }
    if (pd.pressed & PAD_LEFT)  { sel -= 10; if (sel < 0) sel = 0; }
    if (pd.pressed & PAD_RIGHT) { sel += 10; if (sel >= n_ents) sel = n_ents ? n_ents - 1 : 0; }
    if (pd.pressed & PAD_CROSS) enter_or_launch();
    if (st != S_LIST) return;
    if (pd.pressed & PAD_CIRCLE) go_parent();
    if (st != S_LIST) return;
    if (pd.pressed & PAD_TRIANGLE) open_menu(0);
    if (pd.pressed & PAD_SQUARE && n_ents && strcmp(ents[sel].name, "..") && !at_devices()) { ents[sel].marked ^= 1; if (sel + 1 < n_ents) sel++; }
    /* L2 = mute, R2 = hidden files (acted on release), L2+R2 together = credits */
    unsigned rel = prev_held & ~pd.held;
    /* L1 / R1 = previous / next device (acted on release); L1+R1 together = select all / select none */
    if ((pd.held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1) && !chord2) { chord2 = 1; select_all(2); }
    if (!chord2) {
        if (rel & PAD_L1) change_dev(-1);
        if (st != S_LIST) return;
        if (rel & PAD_R1) change_dev(1);
        if (st != S_LIST) return;
    }
    if (!(pd.held & (PAD_L1 | PAD_R1))) chord2 = 0;
    if ((pd.held & (PAD_L2 | PAD_R2)) == (PAD_L2 | PAD_R2) && !chord) { chord = 1; st = S_CREDITS; LOGI("credits shown"); return; }
    if (!chord) {
        if (rel & PAD_L2) { cfg.music_on = !cfg.music_on; say(cfg.music_on ? "Music on" : "Music muted"); }
        if (rel & PAD_R2) { cfg.show_hidden ^= 1; refresh(); say(cfg.show_hidden ? "Hidden files: shown" : "Hidden files: hidden"); }
    }
    if (!(pd.held & (PAD_L2 | PAD_R2))) chord = 0;
    if (st != S_LIST) return;
    if (pd.pressed & PAD_L3) { cfg.sort_mode = (cfg.sort_mode + 1) % 3; refresh(); }
    if (pd.pressed & PAD_R3) { refresh_real(); if (st == S_LIST) say("Refreshed"); }
    if (st != S_LIST) return;
    if (pd.pressed & PAD_SELECT) { show_devices(); }
    if (pd.pressed & PAD_START) open_menu(1);
}

static int menu_prev = -1; static unsigned menu_f0;
static void draw_menu(void)
{
    if (menu_sel != menu_prev) { menu_prev = menu_sel; menu_f0 = frame; }
    draw_list(); overlay();
    int longest = 0;
    for (int i = 0; i < n_menu; i++) { int l = (int)strlen(menu_items[i]); if (l > longest) longest = l; }
    float w = longest * CWIDTH(TEXT_SCALE) * 1.08f + 44; if (w > W() - 60) w = W() - 60;
    int mcols = (int)((w - 40) / CWIDTH(TEXT_SCALE));
    float h = n_menu * ROW_H + 16, x = (W() - w) / 2, y = (H() - h) / 2;
    box(x, y, w, h);
    for (int i = 0; i < n_menu; i++) {
        if (i == menu_sel) rect(x + 4, y + 8 + i * ROW_H, w - 8, ROW_H - 2, RGBA(110, 90, 255, 0x60));
        if (i == menu_sel) mtext(x + 20, y + 10 + i * ROW_H, RGBA(255, 255, 255, 0x80), menu_items[i], mcols, frame - menu_f0, TEXT_SCALE);
        else ttext(x + 20, y + 10 + i * ROW_H, RGBA(255, 255, 255, 0x80), menu_items[i], mcols, TEXT_SCALE);
    }
}

static void draw_confirm(void)
{
    draw_list(); overlay(); box(70, H() / 2 - 60, W() - 140, 120);
    textf(90, H() / 2 - 46, RGBA(255, 220, 160, 0x80), "Delete %d item(s)?", n_tg);
    text(90, H() / 2 - 20, RGBA(255, 200, 160, 0x80), "This cannot be undone.");
    text(90, H() / 2 + 20, RGBA(255, 255, 255, 0x80), "X: Yes        O: No");
}

static void draw_openwith(void)
{
    draw_list(); overlay(); box(50, H() / 2 - 70, W() - 100, 140);
    int cols = (int)((W() - 100 - 40) / CWIDTH(TEXT_SCALE));
    char l[110]; snprintf(l, sizeof l, "With: %s", player_base());
    text(70, H() / 2 - 58, RGBA(255, 220, 160, 0x80), "Play this in your media player?");
    ttext(70, H() / 2 - 32, RGBA(160, 255, 200, 0x80), ow_name, cols, TEXT_SCALE);
    ttext(70, H() / 2 - 6, RGBA(200, 210, 240, 0x80), l, cols, TEXT_SCALE);
    text(70, H() / 2 + 34, RGBA(255, 255, 255, 0x80), "X: Yes        O: No");
}

static void draw_unzip(void)
{
    draw_list(); overlay(); box(50, H() / 2 - 70, W() - 100, 140);
    int cols = (int)((W() - 100 - 40) / CWIDTH(TEXT_SCALE));
    char base[96]; snprintf(base, sizeof base, "%s", uz_name);
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    char into[110]; snprintf(into, sizeof into, "Into folder: %s/", base);
    text(70, H() / 2 - 58, RGBA(255, 220, 160, 0x80), "Do you want to unzip");
    ttext(70, H() / 2 - 32, RGBA(160, 255, 200, 0x80), uz_name, cols, TEXT_SCALE);
    ttext(70, H() / 2 - 6, RGBA(200, 210, 240, 0x80), into, cols, TEXT_SCALE);
    text(70, H() / 2 + 34, RGBA(255, 255, 255, 0x80), "X: Yes        O: No");
}

static void draw_osk(void)
{
    draw_list(); overlay(); box(50, 50, W() - 100, H() - 100);
    text(70, 62, RGBA(255, 255, 255, 0x80), osk_mode ? "New folder name:" : "Rename to:");
    const char *tail = osk_len > 30 ? osk_buf + (osk_len - 30) : osk_buf;
    char shown[40]; snprintf(shown, sizeof shown, "%s_", tail);
    text(70, 90, RGBA(120, 255, 160, 0x80), shown);
    for (int r = 0; r < 5; r++)
        for (int c = 0; osk_rows[r][c]; c++) {
            float x = 74 + c * 27, y = 134 + r * 30;
            if (r == osk_y && c == osk_x) rect(x - 5, y - 3, 26, 28, RGBA(110, 90, 255, 0x70));
            char sc[2] = { osk_rows[r][c], 0 }; text(x, y, RGBA(255, 255, 255, 0x80), sc);
        }
    text_s(70, H() - 84, RGBA(200, 200, 230, 0x80), "X: Type    O: Delete    /\\: Confirm", 0.42f);
    text_s(70, H() - 66, RGBA(170, 180, 215, 0x80), "L1: Space    Start: Cancel", 0.42f);
}

static void draw_hex(void)
{
    draw_bg(); overlay();
    char head[64]; snprintf(head, sizeof head, "%s  @%08X", fs_basename(hex_path), (unsigned)hex_off);
    mtext(10, 6, RGBA(255, 255, 255, 0x80), head, 46, frame, 0.5f);
    for (int r = 0; r < 18; r++) {
        char line[64]; int o = 0;
        o += snprintf(line + o, sizeof line - o, "%06X ", (unsigned)(hex_off + r * 8) & 0xFFFFFF);
        for (int c = 0; c < 8; c++) o += (r * 8 + c < hex_len) ? snprintf(line + o, sizeof line - o, "%02X ", hex_buf[r * 8 + c]) : snprintf(line + o, sizeof line - o, "   ");
        for (int c = 0; c < 8 && r * 8 + c < hex_len; c++) { unsigned char b = hex_buf[r * 8 + c]; line[o++] = (b >= 32 && b < 127) ? b : '.'; }
        line[o] = 0;
        text_s(10, 34 + r * 20, RGBA(200, 230, 255, 0x80), line, 0.5f);
    }
    text_s(10, H() - 20, RGBA(200, 200, 230, 0x80), "Up/Down: Line    Left/Right: Page    O: Back", 0.42f);
}

static void draw_text(void)
{
    draw_list(); overlay();
    float sc = text_small ? 0.38f : TEXT_SCALE, step = text_small ? 14.0f : 22.0f;
    float bx = 40, bw = W() - 80, bh = H() - 90;
    box(bx, 44, bw, bh);
    int cols = (int)((bw - 32) / CWIDTH(sc));
    mtext(bx + 16, 54, RGBA(255, 255, 160, 0x80), text_title, (int)((bw - 32) / CWIDTH(TEXT_SCALE)), frame, TEXT_SCALE);
    for (int i = 0; i < n_text && i < 24; i++) mtext(bx + 16, 86 + i * step, RGBA(255, 255, 255, 0x80), text_lines[i], cols, frame, sc);
    text_s(bx + 16, 44 + bh - 22, RGBA(200, 200, 230, 0x80), "O: Close", 0.42f);
}

static void draw_credits(void)
{
    draw_list(); overlay();
    float x = 40, y = H() / 2 - 112, w = W() - 80, h = 224;
    box(x, y, w, h);
    text(x + 22, y + 14, RGBA(255, 230, 90, 0x80), "PrismELF  v1.0");
    text_s(x + 22, y + 44, RGBA(235, 235, 255, 0x80), "PS2 file manager and ELF launcher", 0.5f);
    text(x + 22, y + 80, RGBA(120, 255, 170, 0x80), "Developed by MexrlDev");
    text_s(x + 22, y + 112, RGBA(190, 200, 255, 0x80), "MIT License", 0.5f);
    text_s(x + 22, y + 134, RGBA(190, 200, 255, 0x80), "github.com/MexrlDev/PrismELF", 0.5f);
    text_s(x + 22, y + 164, RGBA(150, 150, 190, 0x80), "Built with PS2SDK and gsKit", 0.45f);
    text_s(x + 22, y + 192, RGBA(200, 200, 230, 0x80), "Press any button to close", 0.42f);
}

#define N_SET 14
static const char *set_label[N_SET] = { "Show .hidden files", "Sort files by", "Music", "Music volume", "Confirm delete",
    "Button hints", "Debug log file", "Pause music on card", "Start screen", "Show all devices", "Emulator host: (PCSX2)",
    "Save settings now", "Reset to defaults", "Back" };

static void set_value(int i, char *o, int n)
{
    switch (i) {
    case 0: snprintf(o, n, "%s", cfg.show_hidden ? "ON" : "OFF"); break;
    case 1: snprintf(o, n, "%s", cfg.sort_mode == 0 ? "Name" : cfg.sort_mode == 1 ? "Size" : "Type"); break;
    case 2: snprintf(o, n, "%s", cfg.music_on ? "ON" : "OFF"); break;
    case 3: snprintf(o, n, "%d%%", cfg.volume); break;
    case 4: snprintf(o, n, "%s", cfg.confirm_delete ? "ON" : "OFF"); break;
    case 5: snprintf(o, n, "%s", cfg.show_hints ? "ON" : "OFF"); break;
    case 6: snprintf(o, n, "%s", cfg.write_log ? "ON" : "OFF"); break;
    case 7: snprintf(o, n, "%s", cfg.pause_music_mc ? "ON" : "OFF"); break;
    case 8: snprintf(o, n, "%s", cfg.start_screen ? "Devices" : "USB"); break;
    case 9: snprintf(o, n, "%s", cfg.show_all_devices ? "ON" : "OFF"); break;
    case 10: snprintf(o, n, "%s", cfg.host_dev ? "ON" : "OFF"); break;
    default: o[0] = 0;
    }
}

static void set_change(int i, int d)
{
    switch (i) {
    case 0: cfg.show_hidden ^= 1; refresh(); break;
    case 1: cfg.sort_mode = (cfg.sort_mode + d + 3) % 3; refresh(); break;
    case 2: cfg.music_on ^= 1; break;
    case 3: cfg.volume += d * 10; if (cfg.volume < 0) cfg.volume = 0; if (cfg.volume > 100) cfg.volume = 100; break;
    case 4: cfg.confirm_delete ^= 1; break;
    case 5: cfg.show_hints ^= 1; break;
    case 6: cfg.write_log ^= 1; break;
    case 7: cfg.pause_music_mc ^= 1; break;
    case 8: cfg.start_screen ^= 1; break;
    case 9: cfg.show_all_devices ^= 1; break;
    case 10: cfg.host_dev ^= 1; host_auto = -1; break;
    }
}

static void draw_settings(void)
{
    const float sc = 0.5f, cw = CWIDTH(sc);
    draw_bg(); overlay();
    rect(0, 0, W(), 32, RGBA(20, 20, 50, 0x80));
    text(12, 6, RGBA(255, 230, 90, 0x80), "Settings");
    const char *state = cfg_dirty ? "unsaved - auto-saves" : "saved";
    text_s(W() - 14 - (float)strlen(state) * CWIDTH(0.42f), 10, cfg_dirty ? RGBA(255, 200, 120, 0x80) : RGBA(120, 255, 170, 0x80), state, 0.42f);
    for (int i = 0; i < N_SET; i++) {
        float y = 42 + i * 24;
        if (i == set_sel) rect(8, y - 3, W() - 16, 23, RGBA(110, 90, 255, 0x50));
        char v[24], vv[32]; set_value(i, v, sizeof v);
        text_s(18, y, i >= 11 ? RGBA(160, 230, 255, 0x80) : RGBA(235, 235, 255, 0x80), set_label[i], sc);
        if (v[0]) {                                                  /* values sit against the right edge */
            snprintf(vv, sizeof vv, "< %s >", v);
            text_s(W() - 18 - (float)strlen(vv) * cw, y, RGBA(255, 230, 120, 0x80), vv, sc);
        }
    }
    rect(0, H() - 44, W(), 44, RGBA(10, 10, 30, 0x70));
    text_s(12, H() - 36, RGBA(200, 200, 230, 0x80), "Left/Right: Change    X: Select    Start: Save", 0.42f);
    text_s(12, H() - 18, RGBA(170, 180, 215, 0x80), "O: Back    (changes save automatically)", 0.42f);
    if (toast_t > 0) {
        toast_t--;
        box(60, H() - 90, W() - 120, 30);
        mtext(74, H() - 83, RGBA(255, 255, 255, 0x80), toast, (int)((W() - 120 - 28) / CWIDTH(TEXT_SCALE)), frame - toast_start, TEXT_SCALE);
    }
}

static void update_settings(void)
{
    {   /* press and hold repeats (same timing as the file list); first option + up goes to the last, last + down to the first */
        static int set_holdf;
        int dir = 0;
        if (pd.held & PAD_UP || pd.ly < -50) dir = -1;
        if (pd.held & PAD_DOWN || pd.ly > 50) dir = 1;
        int step = nav_step(dir, &set_holdf);
        if (step) {
            int ns = set_sel + step;
            while (ns < 0) ns += N_SET;
            set_sel = ns % N_SET;
        }
    }
    if (set_sel < 11) {
        if (pd.pressed & PAD_LEFT) set_change(set_sel, -1);
        if (pd.pressed & (PAD_RIGHT | PAD_CROSS)) set_change(set_sel, 1);
    } else if (pd.pressed & PAD_CROSS) {
        if (set_sel == 11) { if (cfg_save(0)) say("Settings saved"); else say("Could not save yet - memory card busy"); }
        else if (set_sel == 12) { settings_defaults(&cfg); apply_cfg(); refresh(); say("Settings reset to defaults"); }
        else st = S_LIST;
    }
    if (pd.pressed & PAD_START) { if (cfg_save(0)) say("Settings saved"); }
    if (pd.pressed & PAD_CIRCLE) st = S_LIST;
}

static const char *fm_status_text(int p, char *buf, size_t n)
{
    switch (fm_status[p]) {
    case 0:
        if (fm_type[p] == 1) return "PS1 card (cannot be formatted here)";
        snprintf(buf, n, "PS2 card, %u.%u MB free", (unsigned)(fm_free[p] / 1024), (unsigned)((fm_free[p] % 1024) * 10 / 1024));
        return buf;
    case 1: return "Inserted, not formatted";
    case -1: return "No card";
    default: return "Checking ...";
    }
}

static void update_format(void)
{
    if (fjob_running && fjob_done) {
        fjob_running = 0;
        if (cfg.pause_music_mc) bgm_resume();
        if (fjob_mode != 0) {
            fm_ok = (fjob_rc == 0);
            if (fm_ok) snprintf(fm_result, sizeof fm_result, "Memory Card %d is now %s.", fjob_port + 1, fjob_mode == 1 ? "empty" : "formatted");
            else snprintf(fm_result, sizeof fm_result, "Failed (error %d). Check the card and try again.", fjob_rc);
            LOGI("format: finished, rc=%d", fjob_rc);
            fm_step = 4;
        }
    }
    int up = (pd.pressed & PAD_UP) != 0, dn = (pd.pressed & PAD_DOWN) != 0;
    switch (fm_step) {
    case 0:
        if (up || dn) fm_card ^= 1;
        if (pd.pressed & PAD_SQUARE && !fjob_running) start_fjob(0, 0);                       /* rescan */
        if (pd.pressed & PAD_CIRCLE) { st = S_LIST; break; }
        if (pd.pressed & PAD_CROSS && !fjob_running) {
            if (fm_status[fm_card] == -1) say("No memory card in that slot");
            else if (fm_status[fm_card] == 0 && fm_type[fm_card] == 1) say("PS1 cards are not supported");
            else if (fm_status[fm_card] < 0) say("Cannot read that card - rescan with []");
            else { fm_step = 1; fm_method = (fm_status[fm_card] == 1) ? 1 : 0; }               /* an unformatted card can only be fully formatted */
        }
        break;
    case 1:
        if ((up || dn) && fm_status[fm_card] == 0) fm_method ^= 1;
        if (pd.pressed & PAD_CIRCLE) fm_step = 0;
        if (pd.pressed & PAD_CROSS) { fm_step = 2; fm_conf = 0; }
        break;
    case 2:
        if (pd.pressed & (PAD_LEFT | PAD_RIGHT | PAD_UP | PAD_DOWN)) fm_conf ^= 1;
        if (pd.pressed & PAD_CIRCLE) fm_step = 1;
        if (pd.pressed & PAD_CROSS) {
            if (fm_conf == 1) {
                LOGW("format: user confirmed %s of mc%d", fm_method ? "FULL format" : "quick erase", fm_card);
                if (start_fjob(fm_method ? 2 : 1, fm_card) == 0) { fm_step = 3; fm_start = frame; }
                else say("Could not start");
            } else fm_step = 0;
        }
        break;
    case 4:
        if (pd.pressed & (PAD_CROSS | PAD_CIRCLE)) {
            if (is_mc(cwd)) show_devices();                    /* what was listed from the card is gone */
            fm_step = 0; start_fjob(0, 0);
        }
        break;
    default: break;
    }
}

static void draw_format(void)
{
    draw_list(); overlay();
    float bx = 40, bw = W() - 80, bh = 290, by = (H() - bh) / 2;
    box(bx, by, bw, bh);
    char card[24]; snprintf(card, sizeof card, "Memory Card %d (mc%d:)", fm_card + 1, fm_card);
    text(bx + 16, by + 10, RGBA(255, 230, 90, 0x80), "Format Memory Card");
    const u64 white = RGBA(235, 235, 255, 0x80), dim = RGBA(170, 180, 215, 0x80), red = RGBA(255, 120, 110, 0x80);
    float y = by + 46;
    char tmp[48];
    switch (fm_step) {
    case 0:
        text_s(bx + 16, y, white, "Choose the card to format:", 0.45f); y += 28;
        for (int p = 0; p < 2; p++) {
            float ry = y + p * 54;
            if (p == fm_card) rect(bx + 8, ry - 4, bw - 16, 48, RGBA(110, 90, 255, 0x60));
            snprintf(tmp, sizeof tmp, "Memory Card %d (mc%d:)", p + 1, p);
            text_s(bx + 20, ry, white, tmp, 0.5f);
            const char *stt = fm_status_text(p, tmp, sizeof tmp);
            text_s(bx + 20, ry + 22, fm_status[p] == 0 && fm_type[p] != 1 ? RGBA(120, 255, 170, 0x80) : (fm_status[p] == -2 ? dim : RGBA(255, 200, 120, 0x80)), stt, 0.42f);
        }
        text_s(bx + 16, by + bh - 50, white, "X: Select    O: Back    []: Rescan", 0.42f);
        text_s(bx + 16, by + bh - 28, dim, "Only plugged-in cards can be formatted.", 0.42f);
        break;
    case 1:
        text_s(bx + 16, y, white, card, 0.5f); y += 30;
        text_s(bx + 16, y, white, "Choose the type of format:", 0.45f); y += 28;
        for (int m = 0; m < 2; m++) {
            float ry = y + m * 62;
            int off = (m == 0 && fm_status[fm_card] != 0);
            if (m == fm_method) rect(bx + 8, ry - 4, bw - 16, 56, RGBA(110, 90, 255, 0x60));
            text_s(bx + 20, ry, off ? dim : white, m == 0 ? "Fast format" : "Full format", 0.5f);
            text_s(bx + 20, ry + 22, dim, off ? "Not available: the card is not formatted yet" : (m == 0 ? "Erases every file, keeps the card's format. Quick." : "Rebuilds the whole card. Slow (can take minutes)."), 0.4f);
        }
        text_s(bx + 16, by + bh - 28, white, "X: Continue    O: Back", 0.42f);
        break;
    case 2:
        text_s(bx + 16, y, red, "WARNING", 0.6f); y += 34;
        text_s(bx + 16, y, white, "Everything on this card will be erased:", 0.45f); y += 22;
        text_s(bx + 16, y, white, "saves, games and settings. This cannot", 0.45f); y += 22;
        text_s(bx + 16, y, white, "be undone.", 0.45f); y += 26;
        snprintf(tmp, sizeof tmp, "%s - %s", card, fm_method ? "FULL format" : "FAST format");
        text_s(bx + 16, y, RGBA(255, 230, 120, 0x80), tmp, 0.45f); y += 34;
        rect(bx + 30, y, 200, 34, fm_conf == 0 ? RGBA(110, 90, 255, 0x90) : RGBA(50, 50, 80, 0x70));
        text_s(bx + 30 + (200 - 6 * CWIDTH(0.5f)) / 2, y + 8, white, "Cancel", 0.5f);
        rect(bx + bw - 230, y, 200, 34, fm_conf == 1 ? RGBA(200, 50, 50, 0xA0) : RGBA(80, 40, 40, 0x70));
        text_s(bx + bw - 230 + (200 - 6 * CWIDTH(0.5f)) / 2, y + 8, white, "FORMAT", 0.5f);
        text_s(bx + 16, by + bh - 28, dim, "<- ->: Choose    X: Confirm    O: Back", 0.42f);
        break;
    case 3: {
        unsigned secs = (frame - fm_start) / 60;
        text_s(bx + 16, y, white, fm_method ? "Formatting (full) ..." : "Erasing (fast) ...", 0.55f); y += 34;
        text_s(bx + 16, y, white, card, 0.5f); y += 30;
        text_s(bx + 16, y, red, "Do not remove the card or turn off the PS2.", 0.45f); y += 28;
        snprintf(tmp, sizeof tmp, "Working%s   %um %02us", (frame / 20) % 4 == 0 ? "" : ((frame / 20) % 4 == 1 ? "." : ((frame / 20) % 4 == 2 ? ".." : "...")), secs / 60, secs % 60);
        text_s(bx + 16, y, dim, tmp, 0.45f);
        break; }
    case 4:
        text_s(bx + 16, y, fm_ok ? RGBA(120, 255, 170, 0x80) : red, fm_ok ? "Done" : "Failed", 0.6f); y += 40;
        text_s(bx + 16, y, white, fm_result, 0.42f);
        text_s(bx + 16, by + bh - 28, white, "Press X to continue", 0.42f);
        break;
    }
    if (toast_t > 0) { toast_t--; text_s(bx + 16, by + bh - 74, RGBA(255, 255, 160, 0x80), toast, 0.42f); }
}

static void draw_usb_lost(void)
{
    draw_list(); overlay(); box(50, H() / 2 - 70, W() - 100, 140);
    text(66, H() / 2 - 58, RGBA(255, 180, 120, 0x80), "USB device not found");
    text_s(66, H() / 2 - 26, RGBA(235, 235, 255, 0x80), "It may have been removed.", 0.5f);
    text_s(66, H() / 2 - 4, RGBA(235, 235, 255, 0x80), "Reinsert it, then press X to refresh.", 0.5f);
    text_s(66, H() / 2 + 28, RGBA(170, 180, 215, 0x80), "O: Go to Devices", 0.45f);
    if (toast_t > 0) { toast_t--; text_s(66, H() / 2 + 48, RGBA(255, 255, 160, 0x80), toast, 0.45f); }
}

static void draw_loading(unsigned secs)
{
    draw_bg(); box(50, H() / 2 - 76, W() - 100, 152);
    int cols = (int)((W() - 100 - 32) / CWIDTH(TEXT_SCALE));
    text(66, H() / 2 - 64, RGBA(255, 255, 255, 0x80), "Reading memory card ...");
    mtext(66, H() / 2 - 36, RGBA(200, 220, 255, 0x80), job_path, cols, frame, TEXT_SCALE);
    textf(66, H() / 2 - 8, RGBA(255, 230, 120, 0x80), "%us      O: Cancel", secs);
    text_s(66, H() / 2 + 24, RGBA(160, 170, 205, 0x80), "Memory cards can be slow.", 0.5f);
    text_s(66, H() / 2 + 46, RGBA(160, 170, 205, 0x80), "Please wait.", 0.5f);
}

void ui_run(void)
{
    draw_bg(); text(40, 60, RGBA(255, 255, 255, 0x80), "Loading..."); gfx_flip();
    LOGI("ui: listing %s", cwd); log_flush();
    { int t = fs_cd_type(); if (t < 0) disc_poll_ok = 0; else disc_type = t; }
    note_set("mass:/", fs_dev_ready("mass:/") ? "ready" : "not found");
    LOGI("disc: %s, type %d", disc_poll_ok ? "drive polling on" : "no disc-type support in this build", disc_type);
    if (cfg.start_screen == 1) show_devices(); else refresh();
    LOGI("ui: %d entries, entering main loop", n_ents); log_flush();
    for (;;) {
        if (!(st == S_LOADING && frame % 4)) pad_poll(&pd);      /* poll the pad less while a card is read: less SIO2 traffic */
        else pd.pressed = 0;
        frame++; log_frame = frame;
        bgm_poll();
        poll_song();
        if (sel != last_sel) { last_sel = sel; sel_frame = frame; }
        if (sjob_running && sjob_done) {                         /* background settings job finished */
            sjob_running = 0;
            if (sjob_mode == 0) {
                if (sjob_ok) { cfg = sjob_cfg; cfg_snap = cfg; apply_cfg(); LOGI("settings: loaded from a memory card"); } else LOGI("settings: none on the memory cards");
                if (bgm_pending) { bgm_pending = 0; bgm_restore_default(); }          /* now that mute/volume are known */
            }
            else LOGI("settings: memory card save %s", sjob_ok ? "ok" : "FAILED");
            mc_quiet_until = frame + 600;
        }
        if (dev_scan && fjob_running && fjob_done) {              /* devices rescan: the memory card check came back */
            fjob_running = 0;
            if (cfg.pause_music_mc) bgm_resume();
            rescan_done();
        }
        if (disc_poll_ok && st == S_LIST && !job_running && frame % 60 == 30) {   /* disc inserted / removed */
            int t = fs_cd_type();
            if (t < 0) disc_poll_ok = 0;
            else if (t != disc_type) {
                int old = disc_type; disc_type = t;
                LOGI("disc: type %d -> %d (%s)", old, t, fs_cd_name(t));
                if (old <= 1 && t > 1) { char m[64]; snprintf(m, sizeof m, "Disc inserted: %s", fs_cd_name(t)); say(m); }
                else if (t == 0 && old > 0) say("Disc removed");
                if (t == 0 && is_cd(cwd)) show_devices();
                else if (at_devices()) {
                    char keep[96]; snprintf(keep, sizeof keep, "%s", n_ents ? ents[sel].name : "");
                    show_devices();
                    for (int i = 0; i < n_ents; i++) if (!strcmp(ents[i].name, keep)) { sel = i; break; }
                } else note_cd();
            }
        }
        if (bgm_pending && frame > 900) { bgm_pending = 0; bgm_restore_default(); LOGW("settings: card read took too long, music started anyway"); }
        if (st == S_LIST && usb_was_ok && !at_devices() && !strncmp(cwd, "mass", 4) && frame % 90 == 0 && !fs_dev_ready("mass:/")) { st = S_USB; LOGW("usb: device lost"); }
        if (memcmp(&cfg, &cfg_snap, sizeof cfg)) { cfg_snap = cfg; cfg_dirty = 1; cfg_dirty_frame = frame; apply_cfg(); }
        if (cfg_dirty && frame - cfg_dirty_frame > 90 && st != S_LOADING && !mc_busy()) cfg_save(0);   /* auto-save after a short idle */
        if (frame < mc_quiet_until && frame % 120 == 0) LOGI("ui: alive, state %d", (int)st);
        if (frame == 300 && !bgm_active()) bgm_try_files();       /* USB may have mounted late */
        if (job_running && job_abandoned && job_done) { job_running = 0; LOGI("ui: abandoned list job finally returned"); }
        if (st == S_LIST && usb_retry > 0 && n_ents == 0 && !is_mc(cwd) && !at_devices() && frame % 60 == 0) {   /* USB can take a few seconds to appear */
            int n = fs_list(cwd, ents, MAX_ENTRIES - 1, cfg.show_hidden);
            usb_retry -= 60;
            if (n > 0) { apply_list(n); strcpy(prev_good, cwd); usb_retry = 0; usb_was_ok = 1; LOGI("ui: USB appeared"); }
        }
        if (frame == 2 || frame == 120 || frame == 600) { LOGI("ui: frame %u ok (log=%s)", frame, log_path()); log_flush(); }
        else if (frame % 900 == 0 && frame <= 9000) LOGI("ui: heartbeat frame %u state %d sel %d/%d", frame, (int)st, sel, n_ents);
        switch (st) {
        case S_LIST: update_list(); if (st == S_LIST) draw_list(); else if (st == S_LOADING) draw_list(); break;
        case S_MENU:
            if (pd.pressed & PAD_UP) menu_sel = (menu_sel + n_menu - 1) % n_menu;
            if (pd.pressed & PAD_DOWN) menu_sel = (menu_sel + 1) % n_menu;
            if (pd.pressed & PAD_CROSS) menu_pick();
            else if (pd.pressed & (PAD_CIRCLE | PAD_TRIANGLE)) st = S_LIST;
            if (st == S_MENU) draw_menu(); else if (st == S_LIST || st == S_LOADING) draw_list();
            break;
        case S_CONFIRM:
            if (pd.pressed & PAD_CROSS) { st = S_LIST; do_delete(); }
            else if (pd.pressed & PAD_CIRCLE) st = S_LIST;
            if (st == S_CONFIRM) draw_confirm(); else draw_list();
            break;
        case S_OPENWITH:
            if (pd.pressed & PAD_CROSS) { st = S_LIST; do_openwith(); }
            else if (pd.pressed & PAD_CIRCLE) st = S_LIST;
            if (st == S_OPENWITH) draw_openwith(); else draw_list();
            break;
        case S_UNZIP:
            if (pd.pressed & PAD_CROSS) { st = S_LIST; do_unzip(); }
            else if (pd.pressed & PAD_CIRCLE) st = S_LIST;
            if (st == S_UNZIP) draw_unzip(); else draw_list();
            break;
        case S_OSK: {
            int len = strlen(osk_rows[osk_y]);
            if (pd.pressed & PAD_LEFT) osk_x = (osk_x + len - 1) % len;
            if (pd.pressed & PAD_RIGHT) osk_x = (osk_x + 1) % len;
            if (pd.pressed & PAD_UP) osk_y = (osk_y + 4) % 5;
            if (pd.pressed & PAD_DOWN) osk_y = (osk_y + 1) % 5;
            if (osk_x >= (int)strlen(osk_rows[osk_y])) osk_x = strlen(osk_rows[osk_y]) - 1;
            if ((pd.pressed & PAD_CROSS) && osk_len < 62) { osk_buf[osk_len++] = osk_rows[osk_y][osk_x]; osk_buf[osk_len] = 0; }
            if ((pd.pressed & PAD_L1) && osk_len < 62) { osk_buf[osk_len++] = ' '; osk_buf[osk_len] = 0; }
            if ((pd.pressed & PAD_CIRCLE) && osk_len) osk_buf[--osk_len] = 0;
            if (pd.pressed & PAD_TRIANGLE) osk_done();
            if (pd.pressed & PAD_START) st = S_LIST;
            if (st == S_OSK) draw_osk(); else draw_list();
            break; }
        case S_HEX:
            if (pd.pressed & PAD_DOWN) { hex_off += 8; hex_load(); }
            if ((pd.pressed & PAD_UP) && hex_off >= 8) { hex_off -= 8; hex_load(); }
            if (pd.pressed & PAD_RIGHT) { hex_off += 8 * 18; hex_load(); }
            if ((pd.pressed & PAD_LEFT) && hex_off >= 8 * 18) { hex_off -= 8 * 18; hex_load(); }
            if (pd.pressed & PAD_CIRCLE) st = S_LIST;
            draw_hex(); break;
        case S_TEXT:
            if (pd.pressed & (PAD_CIRCLE | PAD_CROSS)) st = S_LIST;
            draw_text(); break;
        case S_CREDITS:
            if (pd.pressed & (PAD_CROSS | PAD_CIRCLE | PAD_TRIANGLE | PAD_SQUARE | PAD_START)) st = S_LIST;
            draw_credits(); break;
        case S_FORMAT: update_format(); draw_format(); break;
        case S_USB:
            if (pd.pressed & PAD_CROSS) {
                if (fs_dev_ready("mass:/")) {
                    st = S_LIST; refresh();
                    if (n_ents == 0 && strcmp(cwd, "mass:/")) { strcpy(cwd, "mass:/"); sel = 0; refresh(); }
                    if (st == S_LIST) say("USB reconnected - refreshed");
                    LOGI("usb: reconnected");
                } else say("USB still not found");
            } else if (pd.pressed & PAD_CIRCLE) { show_devices(); st = S_LIST; }
            if (st == S_USB) draw_usb_lost(); else draw_list();
            break;
        case S_SETTINGS: update_settings(); if (st == S_SETTINGS) draw_settings(); else draw_list(); break;
        case S_VIEWER:
            viewer_frame(&pd);
            if (!viewer_active()) { st = S_LIST; draw_list(); }
            break;
        case S_LOADING: {
            unsigned secs = (frame - job_start) / 60;
            if (job_done && !job_abandoned) {
                int n = job_result;
                if (n > 0) memcpy(ents, job_ents, sizeof(Entry) * n);
                apply_list(n);
                if (n >= 0) strcpy(prev_good, cwd);
                job_running = 0; st = S_LIST; bgm_resume(); mc_quiet_until = frame + 1200;
                LOGI("ui: list done %s -> %d", cwd, n);
            } else if (pd.pressed & PAD_CIRCLE) {
                job_abandoned = 1;                       /* the reader thread stays parked; never reuse its buffer */
                strcpy(cwd, prev_good); sync_dev_from_cwd(); st = S_LIST; bgm_resume();
                say("Cancelled - the memory card did not answer");
                LOGW("ui: list of %s cancelled after %us", job_path, secs);
            } else if ((frame - job_start) % 120 == 0 && frame != job_start) {
                LOGI("ui: still reading %s (%us)", job_path, secs);
            }
            if (st == S_LOADING) draw_loading(secs); else draw_list();
            break; }
        }
        prev_held = pd.held;
        gfx_flip();
    }
}
