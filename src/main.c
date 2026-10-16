#include <stdio.h>
#include <string.h>
#include "log.h"
#include "gfx.h"
#include "pad.h"
#include "fs.h"
#include "launch.h"
#include "audio.h"
#include "ui.h"

static void boot_work(void)
{
    LOGI("boot: iop_init");   /* no flush yet: fileXio is not loaded before iop_init() */
    iop_init();
    LOGI("boot: iop ok");
    pad_init();
    LOGI("boot: pad ok"); log_flush();
    /* music: the user's USB BGM/bgm.wav|mp3 wins (BGM/ + README are created if missing), else the music built into the ELF */
    if (ui_boot_settings()) bgm_boot(1);        /* settings first (mute/volume), then the music; with no USB the music waits for the card read */
    LOGI("boot: bgm step done (active=%d)", bgm_active()); log_flush();
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    log_init();
    LOGI("PrismELF starting");
    LOGI("gfx_init");
    gfx_init();
    LOGI("gfx ok; loading images");
    ui_load_images();
    LOGI("images ok");
    ui_splash(boot_work);
    LOGI("boot: splash done");
    ui_run();
    return 0;
}
