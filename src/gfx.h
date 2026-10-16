#ifndef GFX_H
#define GFX_H
#include <tamtypes.h>
#include <gsKit.h>
#define RGBA(r,g,b,a) GS_SETREG_RGBAQ(r,g,b,a,0x00)   /* alpha 0x80 = opaque */
#define TEXT_SCALE 0.55f      /* tune for your gsKit default font */
#define CWIDTH(sc) (24.7f * (sc))   /* measured: the default font is ~13.6 px per character at 0.55 (26 px cell x 0.95 spacing) */
#define ROW_H      22
typedef struct { GSTEXTURE t; int ok; } Img;
extern GSGLOBAL *gs;
void gfx_init(void);
void gfx_clear(u64 col);
void gfx_flip(void);
void img_from_rgba(Img *im, const unsigned char *rgba, int w, int h);
void img_draw(Img *im, float x, float y, float w, float h, u8 alpha);
void img_alloc16(Img *im, int w, int h);                 /* reusable RGB555 texture slot (viewer) */
void img_upload(Img *im);                                /* re-send Mem to VRAM after you changed it */
void img_draw_uv(Img *im, float x, float y, float w, float h, float u2, float v2, u8 alpha);
void rect(float x, float y, float w, float h, u64 col);
void text(float x, float y, u64 col, const char *s);
void textf(float x, float y, u64 col, const char *fmt, ...);
void text_s(float x, float y, u64 col, const char *s, float scale);
void ttext(float x, float y, u64 col, const char *s, int maxcols, float scale);                 /* too long: cut with .. */
void mtext(float x, float y, u64 col, const char *s, int maxcols, unsigned t, float scale);      /* too long: scrolls (t = frames since start) */
#endif
