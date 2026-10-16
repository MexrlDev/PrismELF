#ifndef PAD_H
#define PAD_H
#include <libpad.h>
typedef struct {
    int connected;
    unsigned held, pressed;   /* PAD_* bit masks, 1 = down */
    int lx, ly, rx, ry;       /* -128..127, 0 = centre */
} PadState;
void pad_init(void);
void pad_poll(PadState *p);
void pad_close(void);
/* Returns rows to move this frame (0, +-1, +-4...) with hold acceleration. */
int  nav_step(int dir, int *holdf);
#endif
