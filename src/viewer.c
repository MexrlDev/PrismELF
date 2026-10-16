/* Text / Markdown / image viewer. Left/Right = previous/next file of the same kind, O = close. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "viewer.h"
#include "gfx.h"
#include "ftype.h"
#include "imgdec.h"
#include "log.h"

#define VSCALE   0.42f
#define VCOLS    52
#define LINE_H   16
#define MAXL     6000
#define TEXT_CAP (192 * 1024)

static Img slot;
static int slot_ok;
static int active, mode;                 /* mode 0 = text/md, 1 = image */
static char vdir[PATH_MAX_LEN], vpath[PATH_MAX_LEN];
static const Entry *E; static int N, idx;
static char msg[64]; static int msg_t;
static int holdf; static unsigned vframe;

/* text */
static char *tbuf; static int tlen;
static int *ls; static short *ll; static unsigned char *lst; static int nl, top, is_md;
/* image */
static int fw, fh, sw, sh, img_ok; static const char *img_err = "";

static int forced = -1;                  /* index of a file opened as text because its content looks like text (no usable extension) */
static FKind kind_at(int i) { return i == forced ? FK_TEXT : ftype_of(E[i].name); }
static int grp(FKind k) { return (k == FK_TEXT || k == FK_MD) ? 1 : (k == FK_IMAGE ? 2 : 0); }
static int W(void) { return gs->Width; }
static int H(void) { return gs->Height; }

void viewer_init(void)
{
    img_alloc16(&slot, IMG_CAN_W, IMG_CAN_H);
    slot_ok = slot.ok;
    LOGI("viewer: image slot %s", slot_ok ? "ok" : "FAILED (no VRAM)");
}

int viewer_active(void) { return active; }

static void free_text(void)
{
    free(tbuf); free(ls); free(ll); free(lst);
    tbuf = NULL; ls = NULL; ll = NULL; lst = NULL; nl = tlen = 0;
}

static void textf_s_view(float x, float y, u64 col, int a, int b) { char t[24]; snprintf(t, sizeof t, "%d / %d", a, b); text_s(x, y, col, t, 0.5f); }

static void say(const char *m) { snprintf(msg, sizeof msg, "%s", m); msg_t = 120; }

/* ---------- text loading ---------- */
static void add_line(int start, int len, int style)
{
    if (nl >= MAXL) return;
    ls[nl] = start; ll[nl] = (short)len; lst[nl] = (unsigned char)style; nl++;
}

/* markdown inline cleanup: **bold**, __bold__, `code`, [text](url) -> text (url) */
static int md_clean(char *s, int n)
{
    char *o = s;
    for (int i = 0; i < n; i++) {
        if ((s[i] == '*' && i + 1 < n && s[i + 1] == '*') || (s[i] == '_' && i + 1 < n && s[i + 1] == '_')) { i++; continue; }
        if (s[i] == '`') continue;
        *o++ = s[i];
    }
    return (int)(o - s);
}

static void build_lines(void)
{
    nl = 0; int incode = 0;
    int pos = 0;
    while (pos < tlen && nl < MAXL) {
        int e = pos;
        while (e < tlen && tbuf[e] != '\n') e++;
        int len = e - pos;
        if (len > 0 && tbuf[pos + len - 1] == '\r') len--;
        char *s = tbuf + pos;
        /* keep ASCII only: tabs -> space, drop UTF-8 continuation bytes, other high bytes -> '?' */
        int k = 0;
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '\t') s[k++] = ' ';
            else if (c >= 0x80) { if (c >= 0xC0) s[k++] = '?'; }
            else if (c >= 32) s[k++] = (char)c;
        }
        len = k;
        int style = 0, skip = 0;
        if (is_md) {
            if (len >= 3 && !strncmp(s, "```", 3)) { incode = !incode; pos = e + 1; continue; }
            if (incode) style = 4;
            else {
                int h = 0; while (h < len && s[h] == '#') h++;
                if (h > 0 && h <= 6 && h < len && s[h] == ' ') { style = h == 1 ? 1 : (h == 2 ? 2 : 3); skip = h + 1; }
                else if (len >= 3 && (!strncmp(s, "---", 3) || !strncmp(s, "***", 3) || !strncmp(s, "___", 3))) style = 7;
                else if (len >= 2 && s[0] == '>' ) { style = 5; skip = s[1] == ' ' ? 2 : 1; }
                else if (len >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') { style = 6; s[0] = '*'; }
                if (style != 7) { len = skip + md_clean(s + skip, len - skip); }
            }
        }
        int start = (int)(s - tbuf) + skip;
        int rem = len - skip;
        if (style == 7) { add_line(start, 0, 7); }
        else if (rem <= 0) add_line(start, 0, style);
        else {
            while (rem > 0 && nl < MAXL) {
                int take = rem > VCOLS ? VCOLS : rem;
                if (rem > VCOLS && style != 4) {            /* wrap at the last space */
                    int sp = take;
                    while (sp > 8 && tbuf[start + sp] != ' ') sp--;
                    if (sp > 8) take = sp;
                }
                add_line(start, take, style);
                start += take; rem -= take;
                while (rem > 0 && tbuf[start] == ' ' && style != 4) { start++; rem--; }
            }
        }
        pos = e + 1;
    }
}

static int load_text(void)
{
    free_text();
    unsigned len = 0;
    tbuf = (char *)fs_read_all(vpath, &len, TEXT_CAP);
    if (!tbuf) {
        int sz = fs_size(vpath);
        if (sz > TEXT_CAP) {                              /* big file: show the first part */
            tbuf = malloc(TEXT_CAP + 16);
            if (tbuf) { int r = fs_read_at(vpath, 0, tbuf, 120 * 1024); if (r < 0) { free(tbuf); tbuf = NULL; } else { len = (unsigned)r; memset(tbuf + r, 0, 16); say("File is large: showing the beginning"); } }
        }
        if (!tbuf) return -1;
    }
    tlen = (int)len;
    if (tlen >= 3 && !memcmp(tbuf, "\xEF\xBB\xBF", 3)) { memmove(tbuf, tbuf + 3, tlen - 3); tlen -= 3; }
    ls = malloc(sizeof(int) * MAXL); ll = malloc(sizeof(short) * MAXL); lst = malloc(MAXL);
    if (!ls || !ll || !lst) { free_text(); return -2; }
    is_md = kind_at(idx) == FK_MD;
    build_lines();
    top = 0;
    LOGI("viewer: text %s -> %d lines%s", vpath, nl, is_md ? " (markdown)" : "");
    return 0;
}

static void draw_loading(void)
{
    gfx_clear(RGBA(8, 8, 20, 0x80));
    textf(40, 60, RGBA(255, 255, 255, 0x80), "Loading %.50s ...", E[idx].name);
    gfx_flip();
}

static int load_current(void)
{
    fs_join(vpath, vdir, E[idx].name);
    FKind k = kind_at(idx);
    draw_loading();
    if (grp(k) == 1) { mode = 0; return load_text(); }
    mode = 1; img_ok = 0;
    if (!slot_ok) { img_err = "no video memory for previews"; return 0; }
    if (imgdec_load(vpath, (unsigned short *)slot.t.Mem, &fw, &fh, &sw, &sh, &img_err) == 0) { img_upload(&slot); img_ok = 1; }
    return 0;
}

static int open_impl(const char *dir, const Entry *ents, int n, int i, int force_text)
{
    forced = force_text ? i : -1;
    snprintf(vdir, sizeof vdir, "%s", dir);
    E = ents; N = n; idx = i; msg_t = 0; holdf = 0;
    if (grp(kind_at(idx)) == 0) return -1;
    if (load_current() != 0) { say("Cannot open file"); LOGW("viewer: cannot open %s", vpath); if (mode == 0) return -2; }
    active = 1;
    return 0;
}

int viewer_open(const char *dir, const Entry *ents, int n, int i) { return open_impl(dir, ents, n, i, 0); }
int viewer_open_text(const char *dir, const Entry *ents, int n, int i) { return open_impl(dir, ents, n, i, 1); }

static void close_viewer(void) { free_text(); active = 0; }

static int switch_file(int dir)
{
    int g = grp(kind_at(idx));
    for (int i = idx + dir; i >= 0 && i < N; i += dir) {
        if (!E[i].is_dir && grp(kind_at(i)) == g) { idx = i; return load_current(); }
    }
    say("No more files");
    return 1;
}

static void pos_in_group(int *a, int *b)
{
    int g = grp(kind_at(idx)); *a = 0; *b = 0;
    for (int i = 0; i < N; i++) if (!E[i].is_dir && grp(kind_at(i)) == g) { (*b)++; if (i <= idx) *a = *b; }
}

static u64 style_color(int s)
{
    switch (s) {
    case 1: return RGBA(255, 230, 90, 0x80);
    case 2: return RGBA(255, 200, 120, 0x80);
    case 3: return RGBA(120, 220, 255, 0x80);
    case 4: return RGBA(140, 255, 160, 0x80);
    case 5: return RGBA(160, 170, 220, 0x80);
    case 6: return RGBA(230, 230, 255, 0x80);
    case 7: return RGBA(90, 90, 130, 0x80);
    default: return RGBA(235, 235, 245, 0x80);
    }
}

void viewer_frame(const PadState *p)
{
    if (!active) return;
    vframe++;
    if (p->pressed & PAD_CIRCLE) { close_viewer(); return; }
    if (p->pressed & PAD_LEFT)  switch_file(-1);
    if (p->pressed & PAD_RIGHT) switch_file(1);
    int rows = (H() - 64) / LINE_H;
    if (mode == 0) {
        int dir = 0;
        if (p->held & PAD_UP || p->ly < -50) dir = -1;
        if (p->held & PAD_DOWN || p->ly > 50) dir = 1;
        int step = nav_step(dir, &holdf);
        if (step) top += step * (abs(step) > 1 ? 2 : 1);
        if (p->pressed & PAD_L1) top -= rows - 1;
        if (p->pressed & PAD_R1) top += rows - 1;
        if (p->pressed & PAD_TRIANGLE) top = 0;
        if (top > nl - rows) top = nl - rows;
        if (top < 0) top = 0;
    }
    int a, b; pos_in_group(&a, &b);
    gfx_clear(RGBA(8, 8, 20, 0x80));
    rect(0, 0, W(), 32, RGBA(20, 20, 50, 0x80));
    mtext(10, 6, RGBA(255, 255, 255, 0x80), E[idx].name, 30, vframe, 0.5f);
    textf_s_view(W() - 110, 6, RGBA(170, 190, 255, 0x80), a, b);
    if (mode == 0) {
        for (int r = 0; r < rows && top + r < nl; r++) {
            int i = top + r;
            float y = 40 + r * LINE_H;
            if (lst[i] == 7) { rect(20, y + LINE_H / 2, W() - 40, 1, RGBA(90, 90, 130, 0x80)); continue; }
            if (ll[i] <= 0) continue;
            char line[VCOLS + 4];
            int n = ll[i] > VCOLS ? VCOLS : ll[i];
            memcpy(line, tbuf + ls[i], n); line[n] = 0;
            if (lst[i] == 1 || lst[i] == 2) rect(16, y + LINE_H - 3, W() - 32, 1, RGBA(90, 80, 40, 0x60));
            text_s(24, y, style_color(lst[i]), line, VSCALE);
        }
        if (nl > rows) { float f = (float)top / (float)(nl - rows); rect(W() - 8, 40, 4, rows * LINE_H, RGBA(40, 40, 70, 0x80)); rect(W() - 8, 40 + f * (rows * LINE_H - 30), 4, 30, RGBA(150, 150, 255, 0x80)); }
        text_s(10, H() - 20, RGBA(200, 200, 230, 0x80), "< >: File   ^v: Scroll   L1/R1: Page   O: Close", 0.42f);
    } else {
        float sy = (float)H() / 480.0f;
        if (img_ok) {
            float k = 616.0f / fw; if (400.0f / fh < k) k = 400.0f / fh;
            float dw = fw * k, dh = fh * k * sy;
            img_draw_uv(&slot, (W() - dw) / 2, 36 * sy + (400.0f * sy - dh) / 2, dw, dh, (float)fw, (float)fh, 0x80);
            { char inf[64]; snprintf(inf, sizeof inf, "%d x %d%s", sw, sh, (sw != fw || sh != fh) ? "  (scaled down)" : ""); text_s(10, H() - 40, RGBA(170, 190, 255, 0x80), inf, 0.42f); }
        } else {
            text(30, 90, RGBA(255, 150, 150, 0x80), "Cannot show this image:");
            mtext(30, 118, RGBA(255, 255, 255, 0x80), img_err, 42, vframe, 0.42f);
        }
        text_s(10, H() - 20, RGBA(200, 200, 230, 0x80), "< >: Switch image   O: Close", 0.42f);
    }
    if (msg_t > 0) { msg_t--; rect(W() / 2 - 160, H() / 2 - 20, 320, 40, RGBA(30, 20, 60, 0x78)); mtext(W() / 2 - 144, H() / 2 - 8, RGBA(255, 255, 160, 0x80), msg, 22, vframe, TEXT_SCALE); }
}
