#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <gsToolkit.h>
#include "gfx.h"

GSGLOBAL *gs;
static GSFONTM *fontm;

void gfx_init(void)
{
    gs = gsKit_init_global();
    gs->PSM = GS_PSM_CT32;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_ON;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    fontm = gsKit_init_fontm();
    gsKit_fontm_upload(gs, fontm);
    fontm->Spacing = 0.95f;
}

void gfx_clear(u64 col) { gsKit_clear(gs, col); }

void gfx_flip(void)
{
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}

void img_from_rgba(Img *im, const unsigned char *rgba, int w, int h)
{
    memset(&im->t, 0, sizeof im->t);
    im->t.Width = w; im->t.Height = h;
    im->t.PSM = GS_PSM_CT32;
    im->t.Filter = GS_FILTER_LINEAR;
    int bytes = w * h * 4;
    unsigned char *m = memalign(128, bytes);
    memcpy(m, rgba, bytes);
    for (int i = 3; i < bytes; i += 4) m[i] = m[i] >> 1;   /* PNG alpha 0-255 -> GS alpha 0-128 */
    im->t.Mem = (u32 *)m;
    im->t.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
    gsKit_texture_upload(gs, &im->t);
    im->ok = 1;
}

void img_draw(Img *im, float x, float y, float w, float h, u8 alpha)
{
    gsKit_prim_sprite_texture(gs, &im->t, x, y, 0, 0, x + w, y + h,
                              im->t.Width, im->t.Height, 1, RGBA(0x80, 0x80, 0x80, alpha));
}

void rect(float x, float y, float w, float h, u64 col)
{
    gsKit_prim_sprite(gs, x, y, x + w, y + h, 1, col);
}

void text(float x, float y, u64 col, const char *s)
{
    gsKit_fontm_print_scaled(gs, fontm, x, y, 3, TEXT_SCALE, col, s);
}

void textf(float x, float y, u64 col, const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    text(x, y, col, b);
}

void img_alloc16(Img *im, int w, int h)
{
    memset(&im->t, 0, sizeof im->t);
    im->t.Width = w; im->t.Height = h;
    im->t.PSM = GS_PSM_CT16;
    im->t.Filter = GS_FILTER_LINEAR;
    im->t.Mem = memalign(128, (size_t)w * h * 2);
    if (im->t.Mem) memset(im->t.Mem, 0, (size_t)w * h * 2);
    im->t.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, GS_PSM_CT16), GSKIT_ALLOC_USERBUFFER);
    im->ok = (im->t.Mem != NULL && im->t.Vram != 0xFFFFFFFFu);
}

void img_upload(Img *im) { if (im->ok) gsKit_texture_upload(gs, &im->t); }

void img_draw_uv(Img *im, float x, float y, float w, float h, float u2, float v2, u8 alpha)
{
    if (!im->ok) return;
    gsKit_prim_sprite_texture(gs, &im->t, x, y, 0, 0, x + w, y + h, u2, v2, 1, RGBA(0x80, 0x80, 0x80, alpha));
}

void text_s(float x, float y, u64 col, const char *s, float scale)
{
    gsKit_fontm_print_scaled(gs, fontm, x, y, 3, scale, col, s);
}

void ttext(float x, float y, u64 col, const char *s, int maxcols, float scale)
{
    int len = (int)strlen(s);
    if (len <= maxcols) { text_s(x, y, col, s, scale); return; }
    char buf[128];
    int n = maxcols - 2; if (n < 1) n = 1; if (n > 124) n = 124;
    memcpy(buf, s, n); buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = 0;
    text_s(x, y, col, buf, scale);
}

void mtext(float x, float y, u64 col, const char *s, int maxcols, unsigned t, float scale)
{
    int len = (int)strlen(s);
    if (len <= maxcols) { text_s(x, y, col, s, scale); return; }
    int span = len - maxcols;                           /* pause at the start, scroll 1 char / 6 frames, pause at the end, repeat */
    unsigned period = 60u + (unsigned)span * 6u + 60u;
    unsigned p = t % period;
    int off = p < 60u ? 0 : (p < 60u + (unsigned)span * 6u ? (int)((p - 60u) / 6u) : span);
    char buf[128];
    int n = maxcols < 127 ? maxcols : 127;
    memcpy(buf, s + off, n); buf[n] = 0;
    text_s(x, y, col, buf, scale);
}
