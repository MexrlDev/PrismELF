#include <stdlib.h>
#include <string.h>
#include "imgextra.h"

#define MAX_PX 2400000UL

static unsigned u16(const unsigned char *p) { return p[0] | (p[1] << 8); }

/* ---- GIF: first frame, LZW, interlace, transparency, global or local palette ---- */
unsigned char *dec_gif(const unsigned char *d, unsigned n, int *w, int *h, const char **err)
{
    if (n < 13 || memcmp(d, "GIF8", 4)) { *err = "invalid GIF"; return NULL; }
    unsigned sw = u16(d + 6), sh = u16(d + 8), flags = d[10], p = 13;
    if (!sw || !sh || sw * (unsigned long)sh > MAX_PX) { *err = "GIF too large"; return NULL; }
    unsigned char gct[256 * 3]; int gn = 0;
    if (flags & 0x80) { gn = 2 << (flags & 7); if (p + gn * 3 > n) { *err = "truncated GIF"; return NULL; } memcpy(gct, d + p, gn * 3); p += gn * 3; }
    int trans = -1;
    while (p < n) {
        unsigned b = d[p++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (p >= n) break;
            unsigned lab = d[p++];
            if (lab == 0xF9 && p + 5 < n && d[p] >= 4) trans = (d[p + 1] & 1) ? (int)d[p + 4] : -1;
            while (p < n && d[p]) p += d[p] + 1;     /* skip sub-blocks */
            p++;
            continue;
        }
        if (b != 0x2C) { *err = "invalid GIF"; return NULL; }
        if (p + 9 > n) break;
        unsigned fx = u16(d + p), fy = u16(d + p + 2), fw = u16(d + p + 4), fh = u16(d + p + 6), fl = d[p + 8];
        p += 9;
        unsigned char lct[256 * 3]; const unsigned char *pal = gct; int pn = gn;
        if (fl & 0x80) { pn = 2 << (fl & 7); if (p + pn * 3 > n) break; memcpy(lct, d + p, pn * 3); p += pn * 3; pal = lct; }
        if (!pn) { *err = "GIF has no palette"; return NULL; }
        if (p >= n) break;
        int minc = d[p++];
        if (minc < 2 || minc > 11) { *err = "invalid GIF"; return NULL; }
        /* gather the sub-blocks */
        unsigned char *data = malloc(n - p + 1); unsigned dl = 0;
        if (!data) { *err = "out of memory"; return NULL; }
        while (p < n && d[p]) { unsigned l = d[p++]; if (p + l > n) l = n - p; memcpy(data + dl, d + p, l); dl += l; p += l; }
        unsigned char *px = calloc((size_t)sw * sh, 4);
        unsigned char *idx = malloc((size_t)fw * fh + 1);
        short *prefix = malloc(4096 * sizeof(short)); unsigned char *suffix = malloc(4096), *stack = malloc(4097);
        if (!px || !idx || !prefix || !suffix || !stack) { free(data); free(px); free(idx); free(prefix); free(suffix); free(stack); *err = "out of memory"; return NULL; }
        unsigned clear = 1u << minc, eoi = clear + 1, next = clear + 2, size = (unsigned)minc + 1;
        int prev = -1; unsigned bits = 0, nb = 0, di = 0; size_t o = 0, total = (size_t)fw * fh;
        for (unsigned i = 0; i < clear; i++) { prefix[i] = -1; suffix[i] = (unsigned char)i; }
        while (o < total) {
            while (nb < size && di < dl) { bits |= (unsigned)data[di++] << nb; nb += 8; }
            if (nb < size) break;
            unsigned code = bits & ((1u << size) - 1); bits >>= size; nb -= size;
            if (code == clear) { next = clear + 2; size = (unsigned)minc + 1; prev = -1; continue; }
            if (code == eoi) break;
            if (prev < 0) { if (code >= clear) break; idx[o++] = (unsigned char)code; prev = (int)code; continue; }
            if (code > next) break;                                   /* corrupt stream */
            unsigned cur = code == next ? (unsigned)prev : code;      /* code == next is the "KwKwK" case */
            int sp = 0;
            while (prefix[cur] >= 0 && sp < 4096) { stack[sp++] = suffix[cur]; cur = (unsigned)prefix[cur]; }
            stack[sp++] = suffix[cur];
            unsigned char first = suffix[cur];
            while (sp > 0 && o < total) idx[o++] = stack[--sp];
            if (code == next && o < total) idx[o++] = first;
            if (next < 4096) { prefix[next] = (short)prev; suffix[next] = first; next++; if (next == (1u << size) && size < 12) size++; }
            prev = (int)code;
        }
        if (o < total) memset(idx + o, 0, total - o);
        /* place into the canvas, undoing interlacing */
        static const int ys[4] = { 0, 4, 2, 1 }, st[4] = { 8, 8, 4, 2 };
        unsigned *order = malloc(sizeof(unsigned) * (fh + 1)), cnt = 0;     /* canvas row of each stored row */
        if (!order) { free(data); free(px); free(idx); free(prefix); free(suffix); free(stack); *err = "out of memory"; return NULL; }
        if (fl & 0x40) { for (int ps = 0; ps < 4; ps++) for (unsigned yy = (unsigned)ys[ps]; yy < fh; yy += (unsigned)st[ps]) order[cnt++] = yy; }
        else for (unsigned yy = 0; yy < fh; yy++) order[cnt++] = yy;
        for (unsigned i = 0; i < cnt; i++) {
            unsigned cy = fy + order[i];
            if (cy >= sh) continue;
            for (unsigned x = 0; x < fw; x++) {
                unsigned cx = fx + x; if (cx >= sw) break;
                unsigned c = idx[(size_t)i * fw + x];
                if ((int)c == trans || (int)c >= pn) continue;          /* transparent pixel stays alpha 0 */
                unsigned char *o4 = px + ((size_t)cy * sw + cx) * 4;
                o4[0] = pal[c * 3]; o4[1] = pal[c * 3 + 1]; o4[2] = pal[c * 3 + 2]; o4[3] = 255;
            }
        }
        free(order);
        free(data); free(idx); free(prefix); free(suffix); free(stack);
        *w = (int)sw; *h = (int)sh;
        return px;
    }
    *err = "GIF has no image";
    return NULL;
}

/* ---- TGA ---- */
unsigned char *dec_tga(const unsigned char *d, unsigned n, int *w, int *h, const char **err)
{
    if (n < 18) { *err = "invalid TGA"; return NULL; }
    unsigned idlen = d[0], cmt = d[1], type = d[2], cmstart = u16(d + 3), cmlen = u16(d + 5), cmbits = d[7];
    unsigned tw = u16(d + 12), th = u16(d + 14), bpp = d[16], desc = d[17];
    int rle = type >= 9, base = rle ? (int)type - 8 : (int)type;
    if (!tw || !th || tw * (unsigned long)th > MAX_PX) { *err = "TGA too large or empty"; return NULL; }
    if (base != 1 && base != 2 && base != 3) { *err = "unsupported TGA type"; return NULL; }
    if (base == 1 ? (bpp != 8 || cmt != 1) : base == 3 ? bpp != 8 : (bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)) { *err = "unsupported TGA depth"; return NULL; }
    unsigned p = 18 + idlen, cmbytes = (cmbits + 7) / 8;
    const unsigned char *cm = NULL;
    if (cmt) { if (p + cmlen * cmbytes > n) { *err = "truncated TGA"; return NULL; } cm = d + p; p += cmlen * cmbytes; }
    if (base == 1 && (!cm || (cmbytes != 3 && cmbytes != 4 && cmbytes != 2))) { *err = "unsupported TGA palette"; return NULL; }
    unsigned pb = (bpp + 7) / 8;
    unsigned char *px = malloc((size_t)tw * th * 4);
    if (!px) { *err = "out of memory"; return NULL; }
    size_t total = (size_t)tw * th, o = 0;
    unsigned char pix[4];
    #define STORE_PIXEL(src) do { \
        unsigned char *q; size_t pi = o; unsigned y0 = (unsigned)(pi / tw), x0 = (unsigned)(pi % tw); \
        if (!(desc & 0x20)) { y0 = th - 1 - y0; } \
        if (desc & 0x10) { x0 = tw - 1 - x0; } \
        q = px + ((size_t)y0 * tw + x0) * 4; const unsigned char *s_ = (src); \
        if (base == 1) { unsigned ci = s_[0]; if (ci >= cmstart) ci -= cmstart; if (ci >= cmlen) ci = 0; const unsigned char *c_ = cm + ci * cmbytes; \
            if (cmbytes == 2) { unsigned v = c_[0] | (c_[1] << 8); q[0] = ((v >> 10) & 31) * 255 / 31; q[1] = ((v >> 5) & 31) * 255 / 31; q[2] = (v & 31) * 255 / 31; q[3] = 255; } \
            else { q[0] = c_[2]; q[1] = c_[1]; q[2] = c_[0]; q[3] = cmbytes == 4 ? c_[3] : 255; } } \
        else if (base == 3) { q[0] = q[1] = q[2] = s_[0]; q[3] = 255; } \
        else if (pb == 2) { unsigned v = s_[0] | (s_[1] << 8); q[0] = ((v >> 10) & 31) * 255 / 31; q[1] = ((v >> 5) & 31) * 255 / 31; q[2] = (v & 31) * 255 / 31; q[3] = 255; } \
        else { q[0] = s_[2]; q[1] = s_[1]; q[2] = s_[0]; q[3] = (pb == 4 && (desc & 15)) ? s_[3] : 255; } \
        o++; } while (0)
    if (!rle) {
        if ((size_t)p + total * pb > n) { free(px); *err = "truncated TGA"; return NULL; }
        while (o < total) { STORE_PIXEL(d + p); p += pb; }
    } else {
        while (o < total) {
            if (p >= n) { free(px); *err = "truncated TGA"; return NULL; }
            unsigned hdr = d[p++], cnt = (hdr & 127) + 1;
            if (hdr & 128) {
                if (p + pb > n) { free(px); *err = "truncated TGA"; return NULL; }
                memcpy(pix, d + p, pb); p += pb;
                while (cnt-- && o < total) STORE_PIXEL(pix);
            } else {
                while (cnt-- && o < total) { if (p + pb > n) { free(px); *err = "truncated TGA"; return NULL; } STORE_PIXEL(d + p); p += pb; }
            }
        }
    }
    #undef STORE_PIXEL
    *w = (int)tw; *h = (int)th;
    return px;
}
