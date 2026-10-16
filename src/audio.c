/* Music engine. Everything is converted to 48 kHz / 16-bit / stereo before it reaches audsrv. Sources: 16-bit PCM WAV (any rate/mono/stereo), FLAC (own decoder) and MP1/MP2/MP3 (if libmad). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <kernel.h>
#include <audsrv.h>
#include "audio.h"
#include "fs.h"
#include "log.h"
#include "flacdec.h"
#ifdef HAVE_LIBMAD
#include <mad.h>
#endif

#define OUT_RATE    48000
#define OUT_FRAMES  2048
#define OUT_BYTES   (OUT_FRAMES * 4)
#define STAGE       512
#define MAX_FILE    (24 * 1024 * 1024)

typedef enum { SK_NONE, SK_WAV, SK_MP3, SK_FLAC } SrcKind;
static struct {
    SrcKind kind;
    unsigned char *buf; unsigned size; int own;      /* the file in memory */
    int rate, ch, loop;
    const unsigned char *pcm; unsigned frames, pos;  /* WAV */
    FlacDec *fl;                                     /* FLAC */
#ifdef HAVE_LIBMAD
    struct mad_stream st; struct mad_frame fr; struct mad_synth sy;
    unsigned off0; short pend[1152 * 2]; int npend, ppos; int bad;
#endif
} S;

static volatile int run, started, park_req, parked, muted, paused, ended, suspended, exited;
static int vol_pct = 100;
static char stack[0x10000] __attribute__((aligned(16)));
static int tid = -1, fmt_set;
static char title[64] = "";
static char default_title[64] = "";
static int using_default = 1;
extern void *_gp;

#ifdef HAVE_LIBMAD
static short mad_scale(mad_fixed_t s)
{
    s += (1L << (MAD_F_FRACBITS - 16));
    if (s >= MAD_F_ONE) s = MAD_F_ONE - 1; else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
    return (short)(s >> (MAD_F_FRACBITS + 1 - 16));
}
static void mp3_rewind(void)
{
    mad_stream_finish(&S.st); mad_frame_finish(&S.fr); mad_synth_finish(&S.sy);
    mad_stream_init(&S.st); mad_frame_init(&S.fr); mad_synth_init(&S.sy);
    mad_stream_buffer(&S.st, S.buf + S.off0, S.size - S.off0);
}
/* decode one MP3 frame into S.pend; returns 0 ok, <0 at end/error */
static int mp3_next(void)
{
    for (;;) {
        if (mad_frame_decode(&S.fr, &S.st)) {
            if (MAD_RECOVERABLE(S.st.error)) continue;
            return -1;
        }
        mad_synth_frame(&S.sy, &S.fr);
        int n = S.sy.pcm.length, c = S.sy.pcm.channels;
        if (n > 1152) n = 1152;
        for (int i = 0; i < n; i++) {
            short l = mad_scale(S.sy.pcm.samples[0][i]);
            short r = c > 1 ? mad_scale(S.sy.pcm.samples[1][i]) : l;
            S.pend[2 * i] = l; S.pend[2 * i + 1] = r;
        }
        S.npend = n; S.ppos = 0;
        return 0;
    }
}
#endif

/* produce exactly `frames` stereo frames from the current source (loops forever) */
static void src_pull(short *dst, int frames)
{
    if (S.kind == SK_WAV) {
        for (int i = 0; i < frames; i++) {
            if (S.pos >= S.frames) {
                if (S.loop) S.pos = 0;
                else { ended = 1; memset(dst + 2 * i, 0, (size_t)(frames - i) * 4); return; }
            }
            const unsigned char *p = S.pcm + (size_t)S.pos * S.ch * 2;
            short l = (short)(p[0] | p[1] << 8);
            short r = S.ch == 2 ? (short)(p[2] | p[3] << 8) : l;
            dst[2 * i] = l; dst[2 * i + 1] = r;
            S.pos++;
        }
        return;
    }
    if (S.kind == SK_FLAC) {
        int i = 0, empty = 0;
        while (i < frames) {
            int k = flacdec_read(S.fl, dst + 2 * i, frames - i);
            i += k;
            if (k == 0) {                                   /* end of file */
                if (!S.loop) { ended = 1; memset(dst + 2 * i, 0, (size_t)(frames - i) * 4); return; }
                flacdec_rewind(S.fl);
                if (++empty > 2) { memset(dst + 2 * i, 0, (size_t)(frames - i) * 4); return; }   /* nothing decodable: do not spin */
            }
        }
        return;
    }
#ifdef HAVE_LIBMAD
    if (S.kind == SK_MP3) {
        int i = 0;
        while (i < frames) {
            if (S.ppos >= S.npend) {
                if (mp3_next() < 0) {                       /* end of file */
                    if (!S.loop) { ended = 1; memset(dst + 2 * i, 0, (size_t)(frames - i) * 4); return; }
                    mp3_rewind();
                    if (mp3_next() < 0) { if (++S.bad > 3) { memset(dst + 2 * i, 0, (size_t)(frames - i) * 4); return; } }
                    else S.bad = 0;
                }
            }
            while (i < frames && S.ppos < S.npend) { dst[2 * i] = S.pend[2 * S.ppos]; dst[2 * i + 1] = S.pend[2 * S.ppos + 1]; i++; S.ppos++; }
        }
        return;
    }
#endif
    memset(dst, 0, (size_t)frames * 4);
}

/* linear resampler to 48 kHz */
static short stg[STAGE * 2];
static int stg_n, stg_pos;
static int pl, pr, nl, nr;
static unsigned phase, step;

static void pull1(int *l, int *r)
{
    if (stg_pos >= stg_n) { src_pull(stg, STAGE); stg_n = STAGE; stg_pos = 0; }
    *l = stg[2 * stg_pos]; *r = stg[2 * stg_pos + 1]; stg_pos++;
}
static void rs_reset(void)
{
    stg_n = stg_pos = 0; phase = 0;
    step = (unsigned)(((unsigned long long)S.rate << 16) / OUT_RATE);
    pull1(&pl, &pr); pull1(&nl, &nr);
}
static void render(short *out, int frames)
{
    if (S.kind == SK_NONE || muted || paused || suspended) { memset(out, 0, (size_t)frames * 4); return; }
    for (int i = 0; i < frames; i++) {
        out[2 * i]     = (short)(pl + (((nl - pl) * (int)phase) >> 16));
        out[2 * i + 1] = (short)(pr + (((nr - pr) * (int)phase) >> 16));
        phase += step;
        while (phase >= 65536u) { phase -= 65536u; pl = nl; pr = nr; pull1(&nl, &nr); }
    }
}

static void bgm_thread(void *arg)
{
    static short out[OUT_FRAMES * 2] __attribute__((aligned(64)));
    (void)arg;
    while (run) {
        if (park_req) { parked = 1; while (park_req && run) usleep(2000); parked = 0; continue; }
        render(out, OUT_FRAMES);                                   /* prepare the next chunk while the ring still has audio */
        if (audsrv_wait_audio(OUT_BYTES) < 0) { usleep(20000); continue; }
        if (audsrv_play_audio((char *)out, OUT_BYTES) < 0) usleep(20000);
        usleep(4000);                                              /* always give the UI thread a turn (an emulator can accept audio instantly and would otherwise starve it) */
    }
    exited = 1;
}

static void park(void)
{
    if (!started) return;
    park_req = 1;
    for (int i = 0; i < 300 && !parked; i++) usleep(2000);
}
static void unpark(void) { park_req = 0; }

static void free_src(void)
{
    if (S.kind == SK_NONE) return;
    if (S.kind == SK_FLAC) flacdec_close(S.fl);
#ifdef HAVE_LIBMAD
    if (S.kind == SK_MP3) { mad_synth_finish(&S.sy); mad_frame_finish(&S.fr); mad_stream_finish(&S.st); }
#endif
    if (S.own && S.buf) free(S.buf);
    memset(&S, 0, sizeof S);
}

static int ensure_thread(void)
{
    if (!fmt_set) {
        struct audsrv_fmt_t f = { .bits = 16, .freq = OUT_RATE, .channels = 2 };
        if (audsrv_set_format(&f) != 0) { LOGE("bgm: audsrv_set_format failed"); return -1; }
        audsrv_set_volume(MAX_VOLUME * vol_pct / 100);
        fmt_set = 1;
    }
    if (started) return 0;
    ee_thread_t t;
    ee_thread_status_t st;
    ReferThreadStatus(GetThreadId(), &st);
    int mp = st.current_priority;
    if (mp < 8) { ChangeThreadPriority(GetThreadId(), 16); mp = 16; }   /* UI thread below audio */
    memset(&t, 0, sizeof t);
    t.func = bgm_thread; t.stack = stack; t.stack_size = sizeof stack; t.gp_reg = &_gp;
    t.initial_priority = mp - 4;                                          /* audio runs ABOVE the UI thread: no more starved ring */
    run = 1; park_req = 0; exited = 0;
    tid = CreateThread(&t);
    if (tid < 0) { run = 0; LOGE("bgm: CreateThread failed"); return -2; }
    started = 1;
    StartThread(tid, NULL);
    return 0;
}

static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* take ownership of buf (if own) and make it the playing source */
static int install(unsigned char *buf, unsigned size, int own, const char *name, int is_default, int loop)
{
    SrcKind kind = SK_NONE;
    int rate = 0, ch = 0;
    unsigned dpos = 0, dlen = 0, off0 = 0; (void)off0;
    FlacDec *fd = NULL;
    if (size > 44 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {
        unsigned fmt = 0, bits = 0, p = 12;
        while (p + 8 <= size) {
            unsigned len = rd32(buf + p + 4);
            if (!memcmp(buf + p, "fmt ", 4) && p + 24 <= size) { fmt = rd16(buf + p + 8); ch = (int)rd16(buf + p + 10); rate = (int)rd32(buf + p + 12); bits = rd16(buf + p + 22); }
            else if (!memcmp(buf + p, "data", 4)) { dpos = p + 8; dlen = len; break; }
            p += 8 + len + (len & 1);
        }
        if (fmt != 1 || bits != 16 || (ch != 1 && ch != 2) || !dpos || rate < 4000 || rate > 96000) { LOGW("bgm: %s must be 16-bit PCM WAV", name); if (own) free(buf); return -5; }
        if (dpos + dlen > size) dlen = size - dpos;
        if (dlen < (unsigned)ch * 2 * 2048) { LOGW("bgm: %s too short", name); if (own) free(buf); return -6; }
        kind = SK_WAV;
    } else if ((fd = flacdec_open(buf, size, &rate, &ch)) != NULL) {
        kind = SK_FLAC;
    } else {
#ifdef HAVE_LIBMAD
        if (size > 10 && !memcmp(buf, "ID3", 3)) off0 = 10 + ((buf[6] << 21) | (buf[7] << 14) | (buf[8] << 7) | buf[9]);
        kind = SK_MP3;
#else
        LOGW("bgm: %s is not a WAV and this build has no MP3 decoder (libmad missing)", name);
        if (own) free(buf);
        return -10;
#endif
    }
    if (ensure_thread() != 0) { flacdec_close(fd); if (own) free(buf); return -3; }
    park();
    free_src();
    S.kind = kind; S.buf = buf; S.size = size; S.own = own; S.rate = rate; S.ch = ch; S.loop = loop;
    ended = 0;
    if (kind == SK_FLAC) S.fl = fd;
    if (kind == SK_WAV) { S.pcm = buf + dpos; S.frames = dlen / (ch * 2); S.pos = 0; }
#ifdef HAVE_LIBMAD
    if (kind == SK_MP3) {
        S.off0 = off0;
        mad_stream_init(&S.st); mad_frame_init(&S.fr); mad_synth_init(&S.sy);
        mad_stream_buffer(&S.st, S.buf + S.off0, S.size - S.off0);
        if (mp3_next() < 0) { LOGW("bgm: %s is not a valid MP3", name); free_src(); unpark(); return -11; }
        S.rate = (int)S.fr.header.samplerate;
        S.ch = S.sy.pcm.channels;
    }
#endif
    rs_reset();
    snprintf(title, sizeof title, "%s", name);
    using_default = is_default;
    if (is_default) snprintf(default_title, sizeof default_title, "%s", name);
    paused = 0;
    unpark();
    LOGI("bgm: playing %s (%d Hz, %dch, %s)", name, S.rate, S.ch, kind == SK_WAV ? "wav" : kind == SK_FLAC ? "flac" : "mp3/mp2");
    return 0;
}

#ifdef HAVE_EMBEDDED_BGM
extern unsigned char bgm_wav[];
extern unsigned int size_bgm_wav;
#endif

int bgm_start_embedded(void)
{
#ifdef HAVE_EMBEDDED_BGM
    return install(bgm_wav, size_bgm_wav, 0, "embedded music", 1, 1);
#else
    return -1;
#endif
}

static int load_file(const char *path, int is_default, int loop)
{
    unsigned len = 0;
    unsigned char *b = fs_read_all(path, &len, MAX_FILE);
    if (!b) return -1;
    return install(b, len, 1, fs_basename(path), is_default, loop);
}

int bgm_play_file(const char *path) { return load_file(path, 0, 0); }

/* ---- background loader: reading + opening mp3 ---- */
static volatile int al_state;                    /* 0 idle, 1 loading, 2 finished (waiting to be collected) */
static int al_rc, al_mode, al_loop, al_quiet, al_slot;
static char al_path[PATH_MAX_LEN];
static char al_stack[2][0x10000] __attribute__((aligned(16)));
static int find_default(int wait_usb, int create_folder);

static void al_thread(void *arg)
{
    (void)arg;
    if (al_mode == 0) al_rc = load_file(al_path, 0, al_loop);
    else al_rc = find_default(0, 0);
    al_state = 2;
    ExitDeleteThread();
}

static int al_start(int mode, const char *path, int loop, int quiet)
{
    if (al_state != 0) return -1;
    al_mode = mode; al_loop = loop; al_quiet = quiet; al_rc = -1;
    snprintf(al_path, sizeof al_path, "%s", path ? path : "");
    ee_thread_t t; ee_thread_status_t ts;
    ReferThreadStatus(GetThreadId(), &ts);
    memset(&t, 0, sizeof t);
    al_slot ^= 1;
    t.func = al_thread; t.stack = al_stack[al_slot]; t.stack_size = sizeof al_stack[0]; t.gp_reg = &_gp;
    /* Just ABOVE the UI thread (and below the audio feeder): the UI loop never sleeps long enough for a lower-priority thread to get
       real work done, which made loading crawl. The loader mostly waits on the disk, so the UI still runs smoothly. */
    t.initial_priority = ts.current_priority - 1; if (t.initial_priority < 2) t.initial_priority = 2;
    int id = CreateThread(&t);
    if (id < 0) { LOGE("bgm: loader CreateThread failed"); return -2; }
    al_state = 1;
    StartThread(id, NULL);
    return 0;
}
int bgm_play_file_async(const char *path, int loop) { return al_start(0, path, loop, 0); }
int bgm_restore_async(void) { return al_start(1, NULL, 0, 0); }
int bgm_load_poll(int *rc)
{
    if (al_state == 0) return 0;
    if (al_state == 1) return 1;
    int r = al_rc;
    al_state = 0;
    if (al_quiet) return 0;
    if (rc) *rc = r;
    return 2;
}

static const char *const usb_bgm[] = { "mass:/BGM/bgm.wav", "mass:/BGM/bgm.mp3", "mass:/PrismELF/bgm.wav", "mass:/PrismELF/bgm.mp3" };

static void make_bgm_folder(void)
{
    static const char readme[] =
        "PrismELF background music\r\n"
        "-------------------------\r\n"
        "Put your own music in this folder and name it:\r\n"
        "    bgm.wav   (16-bit PCM WAV)   or   bgm.mp3\r\n"
        "It is played at start-up instead of the built-in music.\r\n"
        "Delete the file to go back to the built-in music.\r\n"
        "Playing another song from the file list returns to this music when it ends.\r\n";
    fs_mkdir("mass:/BGM");
    if (!fs_exists("mass:/BGM/README.txt")) fs_write_all("mass:/BGM/README.txt", readme, (int)sizeof readme - 1);
    LOGI("bgm: created mass:/BGM with a README (no bgm file found)");
}

/* USB first (the user's own bgm wins), then the music built into the ELF */
static int find_default(int wait_usb, int create_folder)
{
    int usb = 0;
    for (int i = 0; i < (wait_usb ? 12 : 1) && !usb; i++) { usb = fs_dev_ready("mass:/"); if (!usb && wait_usb) usleep(100000); }
    if (usb) {
        for (unsigned i = 0; i < sizeof usb_bgm / sizeof usb_bgm[0]; i++) if (load_file(usb_bgm[i], 1, 1) == 0) return 0;
        if (create_folder) make_bgm_folder();
    }
    return bgm_start_embedded();
}

int bgm_boot(int usb_already_checked) { return find_default(!usb_already_checked, 1); }
int bgm_try_files(void)
{
    if (!fs_dev_ready("mass:/")) return -1;
    for (unsigned i = 0; i < sizeof usb_bgm / sizeof usb_bgm[0]; i++) if (load_file(usb_bgm[i], 1, 1) == 0) return 0;
    return -1;
}
int bgm_restore_default(void) { return find_default(0, 0); }

int bgm_active(void) { return started && S.kind != SK_NONE; }
const char *bgm_title(void) { return title; }
void bgm_toggle_mute(void) { muted = !muted; }
void bgm_set_muted(int m) { muted = m; }
void bgm_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    vol_pct = pct;
    if (fmt_set) audsrv_set_volume(MAX_VOLUME * vol_pct / 100);
}
void bgm_suspend(void) { suspended = 1; }      /* silence only: the feeder keeps running so audsrv never stalls */
void bgm_resume(void)  { suspended = 0; }
void bgm_poll(void)
{
    if (!ended) return;
    if (using_default) { ended = 0; return; }
    if (al_state != 0) return;                   /* a load is running: try again next frame */
    ended = 0;
    LOGI("bgm: track finished, back to the default music");
    if (al_start(1, NULL, 0, 1) != 0) LOGW("bgm: could not start the default music");
}
int  bgm_looping(void) { return bgm_active() && S.loop && !using_default; }   /* a user-chosen song that repeats */
int  bgm_muted(void) { return muted || !bgm_active(); }
void bgm_toggle_pause(void) { paused = !paused; }
int  bgm_paused(void) { return paused; }

void bgm_stop(void)
{
    if (!started) return;
    run = 0; park_req = 0;
    for (int i = 0; i < 100 && !exited; i++) usleep(2000);          /* the feeder leaves its loop within one chunk (~40 ms) */
    if (tid >= 0) { TerminateThread(tid); DeleteThread(tid); tid = -1; }
    audsrv_stop_audio();
    audsrv_quit();
    started = 0;
}
