#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include "pad.h"
#include "log.h"

static char padBuf[256] __attribute__((aligned(64)));
static unsigned prev;

void pad_init(void)
{
    padInit(0);
    if (!padPortOpen(0, 0, padBuf)) { LOGE("padPortOpen failed"); return; }
    for (int i = 0; i < 300; i++) {            /* wait for the pad to be ready */
        int s = padGetState(0, 0);
        if (s == PAD_STATE_STABLE || s == PAD_STATE_FINDCTP1) break;
        nopdelay();
    }
    padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    LOGI("pad ready");
}

void pad_close(void) { padPortClose(0, 0); padEnd(); }

void pad_poll(PadState *p)
{
    struct padButtonStatus b;
    int s = padGetState(0, 0);
    if (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1) {
        p->connected = 0; p->held = p->pressed = 0; p->lx = p->ly = p->rx = p->ry = 0;
        prev = 0;
        return;
    }
    p->connected = 1;
    if (!padRead(0, 0, &b)) return;
    p->held = 0xffff ^ b.btns;
    p->pressed = p->held & ~prev;
    prev = p->held;
    p->lx = (int)b.ljoy_h - 128; p->ly = (int)b.ljoy_v - 128;
    p->rx = (int)b.rjoy_h - 128; p->ry = (int)b.rjoy_v - 128;
}

int nav_step(int dir, int *holdf)
{
    if (!dir) { *holdf = 0; return 0; }
    (*holdf)++;
    int h = *holdf;
    if (h == 1) return dir;
    if (h < 18) return 0;
    if (h < 48)  return (h % 6 == 0) ? dir : 0;
    if (h < 90)  return (h % 3 == 0) ? dir : 0;
    return dir * 3;                      /* turbo after ~1.5 s */
}
