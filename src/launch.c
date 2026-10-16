#include <stdio.h>
#include <string.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopheap.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <libpad.h>
#include <audsrv.h>
#include <libmc.h>
#ifndef USE_SIFLOADELF
#include <elf-loader.h>
#endif
#include "launch.h"
#include "audio.h"
#include "pad.h"
#include "log.h"

#define IRX(n) extern unsigned char n##_irx[]; extern unsigned int size_##n##_irx
IRX(iomanX); IRX(fileXio); IRX(sio2man); IRX(padman);
IRX(usbd); IRX(bdm); IRX(bdmfs_fatfs); IRX(usbmass_bd); IRX(freesd); IRX(audsrv);
#ifdef HAVE_CDFS_IRX
IRX(cdfs);
#endif

static int mod(const char *name, void *p, unsigned sz)
{
    int ret = 0;
    int id = SifExecModuleBuffer(p, sz, 0, NULL, &ret);
    if (id < 0 || ret == 1) { LOGE("module %s failed (id=%d ret=%d)", name, id, ret); return -1; }
    LOGI("module %s ok", name);
    return 0;
}
#define LOADMOD(n) mod(#n, n##_irx, size_##n##_irx)

static int rom(const char *name)
{
    int id = SifLoadModule(name, 0, NULL);
    LOGI("rom module %s -> %d", name, id);
    return id;
}

void iop_init(void)
{
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    LOADMOD(iomanX);
    LOADMOD(fileXio);
    fileXioInit();
    /* Pad + SIO2: the console's own ROM modules (what wLaunchELF uses); the SDK copies are only a fallback. */
    int ok = rom("rom0:XSIO2MAN") >= 0 && rom("rom0:XPADMAN") >= 0;
    if (!ok) ok = rom("rom0:SIO2MAN") >= 0 && rom("rom0:PADMAN") >= 0;
    if (!ok) { LOADMOD(sio2man); LOADMOD(padman); }
    LOADMOD(usbd);
    LOADMOD(bdm);
    LOADMOD(bdmfs_fatfs);
    LOADMOD(usbmass_bd);
    LOADMOD(freesd);
    LOADMOD(audsrv);
#ifdef HAVE_CDFS_IRX
    LOADMOD(cdfs);                         /* disc drive as "cdfs:" (the console's own CDVDMAN is already running after the IOP reset) */
#endif
    if (audsrv_init() != 0) LOGE("audsrv_init failed");
}

/* Memory card: ROM XMCMAN/XMCSERV + libmc (the SDK's own mcman hangs on directory reads on some consoles). */
int iop_load_mc(void)
{
    static int state = 0;                 /* 0 = not tried, 1 = ready, -1 = failed */
    if (state) return state > 0 ? 0 : -1;
    LOGI("mc: loading ROM memory card modules");
    int type = MC_TYPE_XMC;
    int ok = rom("rom0:XMCMAN") >= 0 && rom("rom0:XMCSERV") >= 0;
    if (!ok) { type = MC_TYPE_MC; ok = rom("rom0:MCMAN") >= 0 && rom("rom0:MCSERV") >= 0; }
    if (ok && mcInit(type) < 0) { LOGE("mc: mcInit failed"); ok = 0; }
    state = ok ? 1 : -1;
    LOGI("mc: %s", ok ? "ready" : "FAILED");
    return ok ? 0 : -1;
}

int launch_elf(const char *path) { return launch_elf_args(path, 0, NULL); }

int launch_elf_args(const char *path, int argc, char *argv[])
{
    LOGI("launch %s", path);
    log_flush();
    bgm_stop();
    pad_close();
#ifdef USE_SIFLOADELF
    (void)argc; (void)argv;
    t_ExecData ed;
    memset(&ed, 0, sizeof ed);
    SifLoadFileExit();
    SifExitRpc();
    SifInitRpc(0);
    if (SifLoadElf(path, &ed) == 0 && ed.epc) {
        SifExitRpc();
        FlushCache(0); FlushCache(2);
        ExecPS2((void *)ed.epc, (void *)ed.gp, 0, NULL);
    }
    return -1;
#else
    return LoadELFFromFile(path, argc, argv);   /* returns only on failure */
#endif
}

void exit_to_osd(void)
{
    log_flush();
    bgm_stop();
    pad_close();
    SifExitRpc();
    LoadExecPS2("rom0:OSDSYS", 0, NULL);
}
