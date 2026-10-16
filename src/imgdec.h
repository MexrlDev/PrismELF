#ifndef IMGDEC_H
#define IMGDEC_H
#define IMG_CAN_W 512
#define IMG_CAN_H 384
/* Decode PNG / JPEG / BMP, shrink to fit 512x384 and store as GS CT16 (5551) pixels in dst
   (stride IMG_CAN_W). fw/fh = fitted size, sw/sh = original size. Returns 0 or <0 with *err set. */
int imgdec_load(const char *path, unsigned short *dst, int *fw, int *fh, int *sw, int *sh, const char **err);
#endif
