#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <setjmp.h>
#ifdef HAVE_LIBPNG
#include <png.h>
#endif
#ifdef HAVE_LIBJPEG
#include <stdio.h>
#include <jpeglib.h>
#endif
#include "imgdec.h"
#include "fs.h"
#include "log.h"

#define MAX_PIXELS 2400000UL

#include "imgextra.h"
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

#ifdef HAVE_LIBPNG
static unsigned char *dec_png(const unsigned char *d, unsigned n, int *w, int *h, const char **err)
{
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&im, d, n)) { *err = "invalid PNG"; return NULL; }
    if ((unsigned long)im.width * im.height > MAX_PIXELS) { png_image_free(&im); *err = "PNG too large (max about 2.4 megapixels)"; return NULL; }
    im.format = PNG_FORMAT_RGBA;
    unsigned char *px = malloc(PNG_IMAGE_SIZE(im));
    if (!px) { png_image_free(&im); *err = "out of memory"; return NULL; }
    if (!png_image_finish_read(&im, NULL, px, 0, NULL)) { free(px); png_image_free(&im); *err = "PNG decode failed"; return NULL; }
    *w = (int)im.width; *h = (int)im.height;
    return px;
}
#endif

#ifdef HAVE_LIBJPEG
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } JErr;
static void jerr_exit(j_common_ptr c) { longjmp(((JErr *)c->err)->jb, 1); }
static void js_init(j_decompress_ptr c) { (void)c; }
static boolean js_fill(j_decompress_ptr c)
{
    static const JOCTET eoi[2] = { 0xFF, JPEG_EOI };
    c->src->next_input_byte = eoi; c->src->bytes_in_buffer = 2;
    return TRUE;
}
static void js_skip(j_decompress_ptr c, long n)
{
    if (n <= 0) return;
    if ((size_t)n > c->src->bytes_in_buffer) n = (long)c->src->bytes_in_buffer;
    c->src->next_input_byte += n; c->src->bytes_in_buffer -= (size_t)n;
}
static void js_term(j_decompress_ptr c) { (void)c; }

static unsigned char *dec_jpeg(const unsigned char *d, unsigned n, int *w, int *h, const char **err)
{
    struct jpeg_decompress_struct ci;
    struct jpeg_source_mgr sm;
    JErr je;
    unsigned char *volatile px = NULL;
    memset(&ci, 0, sizeof ci);
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    if (setjmp(je.jb)) { jpeg_destroy_decompress(&ci); free(px); *err = "invalid JPEG"; return NULL; }
    jpeg_create_decompress(&ci);
    sm.init_source = js_init; sm.fill_input_buffer = js_fill; sm.skip_input_data = js_skip;
    sm.resync_to_restart = jpeg_resync_to_restart; sm.term_source = js_term;
    sm.next_input_byte = d; sm.bytes_in_buffer = n;
    ci.src = &sm;
    jpeg_read_header(&ci, TRUE);
    int denom = 1;                                   /* decode at 1/2, 1/4, 1/8 size when the picture is big */
    while (denom < 8 && (ci.image_width / (denom * 2) >= IMG_CAN_W || ci.image_height / (denom * 2) >= IMG_CAN_H)) denom *= 2;
    ci.scale_num = 1; ci.scale_denom = denom;
    ci.out_color_space = JCS_RGB;
    ci.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&ci);
    int ow = (int)ci.output_width, oh = (int)ci.output_height;
    if ((unsigned long)ow * oh > 3000000UL) { jpeg_destroy_decompress(&ci); *err = "JPEG too large"; return NULL; }
    px = malloc((size_t)ow * oh * 4);
    if (!px) { jpeg_destroy_decompress(&ci); *err = "out of memory"; return NULL; }
    JSAMPARRAY rows = (*ci.mem->alloc_sarray)((j_common_ptr)&ci, JPOOL_IMAGE, (JDIMENSION)(ow * 3), 1);
    while (ci.output_scanline < ci.output_height) {
        int y = (int)ci.output_scanline;
        jpeg_read_scanlines(&ci, rows, 1);
        unsigned char *s = rows[0], *o = px + (size_t)y * ow * 4;
        for (int x = 0; x < ow; x++) { o[4 * x] = s[3 * x]; o[4 * x + 1] = s[3 * x + 1]; o[4 * x + 2] = s[3 * x + 2]; o[4 * x + 3] = 255; }
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    *w = ow; *h = oh;
    return px;
}
#endif

static unsigned char *dec_bmp(const unsigned char *d, unsigned n, int *w, int *h, const char **err)
{
    if (n < 54) { *err = "invalid BMP"; return NULL; }
    unsigned off = rd32(d + 10), hs = rd32(d + 14);
    int bw = (int)rd32(d + 18), bh = (int)rd32(d + 22);
    unsigned bpp = rd16(d + 28), comp = rd32(d + 30);
    int topdown = bh < 0; if (bh < 0) bh = -bh;
    if (hs < 40 || bw <= 0 || bh <= 0 || (comp != 0 && !(comp == 3 && bpp == 32)) || (bpp != 8 && bpp != 24 && bpp != 32)) { *err = "unsupported BMP (need uncompressed 8/24/32-bit)"; return NULL; }
    if ((unsigned long)bw * bh > MAX_PIXELS) { *err = "BMP too large"; return NULL; }
    unsigned stride = (((unsigned)bw * bpp + 31) / 32) * 4;
    if ((unsigned long)off + (unsigned long)stride * bh > n) { *err = "truncated BMP"; return NULL; }
    const unsigned char *pal = d + 14 + hs;
    unsigned char *px = malloc((size_t)bw * bh * 4);
    if (!px) { *err = "out of memory"; return NULL; }
    for (int y = 0; y < bh; y++) {
        const unsigned char *row = d + off + (size_t)(topdown ? y : bh - 1 - y) * stride;
        unsigned char *o = px + (size_t)y * bw * 4;
        for (int x = 0; x < bw; x++) {
            if (bpp == 8) { const unsigned char *c = pal + row[x] * 4; o[4*x] = c[2]; o[4*x+1] = c[1]; o[4*x+2] = c[0]; o[4*x+3] = 255; }
            else if (bpp == 24) { o[4*x] = row[3*x+2]; o[4*x+1] = row[3*x+1]; o[4*x+2] = row[3*x]; o[4*x+3] = 255; }
            else { o[4*x] = row[4*x+2]; o[4*x+1] = row[4*x+1]; o[4*x+2] = row[4*x]; o[4*x+3] = 255; }
        }
    }
    *w = bw; *h = bh;
    return px;
}

int imgdec_load(const char *path, unsigned short *dst, int *fw, int *fh, int *sw, int *sh, const char **err)
{
    unsigned len = 0;
    *err = "cannot read file (missing or over 12 MB)";
    unsigned char *file = fs_read_all(path, &len, 12u << 20);
    if (!file) return -1;
    unsigned char *px = NULL;
    int w = 0, h = 0;
    *err = "unsupported image format";
    if (len > 8 && !memcmp(file, "\x89PNG", 4)) {
#ifdef HAVE_LIBPNG
        px = dec_png(file, len, &w, &h, err);
#else
        *err = "PNG support not in this build (libpng missing)";
#endif
    } else if (len > 4 && file[0] == 0xFF && file[1] == 0xD8) {
#ifdef HAVE_LIBJPEG
        px = dec_jpeg(file, len, &w, &h, err);
#else
        *err = "JPEG support not in this build (libjpeg missing)";
#endif
    } else if (len > 2 && file[0] == 'B' && file[1] == 'M') {
        px = dec_bmp(file, len, &w, &h, err);
    } else if (len > 6 && !memcmp(file, "GIF8", 4)) {
        px = dec_gif(file, len, &w, &h, err);                 /* first frame only */
    } else {
        const char *dot = strrchr(path, '.');                 /* TGA has no signature: go by the extension */
        if (dot && !strcasecmp(dot, ".tga")) px = dec_tga(file, len, &w, &h, err);
        else if (dot && (!strcasecmp(dot, ".tif") || !strcasecmp(dot, ".tiff"))) *err = "TIFF is not supported";
    }
    free(file);
    if (!px) { LOGW("img: %s: %s", path, *err); return -2; }

    int ow = w, oh = h;                                  /* fit inside 512x384, never enlarge */
    if (w > IMG_CAN_W || h > IMG_CAN_H) {
        if ((long)w * IMG_CAN_H > (long)h * IMG_CAN_W) { ow = IMG_CAN_W; oh = (int)((long)h * IMG_CAN_W / w); }
        else { oh = IMG_CAN_H; ow = (int)((long)w * IMG_CAN_H / h); }
    }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    for (int dy = 0; dy < oh; dy++) {
        int y0 = (int)((long)dy * h / oh), y1 = (int)((long)(dy + 1) * h / oh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < ow; dx++) {
            int x0 = (int)((long)dx * w / ow), x1 = (int)((long)(dx + 1) * w / ow);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, cnt = 0;
            for (int y = y0; y < y1 && y < h; y++) {
                const unsigned char *s = px + ((size_t)y * w + x0) * 4;
                for (int x = x0; x < x1 && x < w; x++, s += 4) {
                    unsigned a = s[3];                   /* composite over a dark grey so transparency looks right */
                    r += (s[0] * a + 32 * (255 - a)) / 255;
                    g += (s[1] * a + 32 * (255 - a)) / 255;
                    b += (s[2] * a + 40 * (255 - a)) / 255;
                    cnt++;
                }
            }
            if (!cnt) cnt = 1;
            dst[dy * IMG_CAN_W + dx] = (unsigned short)(((r / cnt) >> 3) | (((g / cnt) >> 3) << 5) | (((b / cnt) >> 3) << 10) | 0x8000);
        }
    }
    free(px);
    *fw = ow; *fh = oh; *sw = w; *sh = h;
    LOGI("img: %s %dx%d -> %dx%d", path, w, h, ow, oh);
    return 0;
}
