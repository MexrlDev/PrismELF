#ifndef IMGEXTRA_H
#define IMGEXTRA_H
/* Extra image decoders. Both return malloc'd RGBA (w*h*4) or NULL with *err set. */
unsigned char *dec_gif(const unsigned char *d, unsigned n, int *w, int *h, const char **err);   /* first frame only */
unsigned char *dec_tga(const unsigned char *d, unsigned n, int *w, int *h, const char **err);   /* types 1,2,3 and their RLE forms */
#endif
