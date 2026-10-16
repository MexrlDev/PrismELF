#ifndef VIEWER_H
#define VIEWER_H
#include "fs.h"
#include "pad.h"
void viewer_init(void);                                            /* after gfx_init: reserves the image texture slot */
int  viewer_open(const char *dir, const Entry *ents, int n, int idx);   /* text/md/image at ents[idx]; 0 = opened */
int  viewer_open_text(const char *dir, const Entry *ents, int n, int idx);   /* open ents[idx] as plain text whatever its name says */
int  viewer_active(void);
void viewer_frame(const PadState *p);                              /* update + draw one frame */
#endif
