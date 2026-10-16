/* File layer: USB/host via fileXio, memory cards via libmc (ROM XMCMAN/XMCSERV). */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <iox_stat.h>
#include <io_common.h>
#include <fileio.h>
#ifdef HAVE_CDVD
#include <libcdvd.h>
#endif
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif
#include "fs.h"
#include "sevenz.h"
#include "log.h"

#define MC_ATTR_DIR 0x20
static unsigned char cbuf[128 * 1024] __attribute__((aligned(64)));
static sceMcTblGetDir mc_tbl[MAX_ENTRIES] __attribute__((aligned(64)));

static int is_mcpath(const char *p) { return p[0] == 'm' && p[1] == 'c' && (p[2] == '0' || p[2] == '1') && p[3] == ':'; }
/* host: (PCSX2 / emulator shared folder) is served by the classic fio driver, not fileXio. */
static int is_host(const char *p) { return !strncmp(p, "host:", 5); }

/* Emulators differ on whether "host:/dir/" or "host:dir" is accepted: try the path as given, then without the slash after the colon / trailing slash. */
static int host_dopen(const char *path)
{
    int fd = fioDopen(path);
    if (fd >= 0) return fd;
    char alt[PATH_MAX_LEN];
    if (path[5] == '/') { snprintf(alt, sizeof alt, "host:%s", path + 6); fd = fioDopen(alt); if (fd >= 0) return fd; }
    size_t l = strlen(path);
    if (l > 6 && path[l - 1] == '/') { snprintf(alt, sizeof alt, "%.*s", (int)(l - 1), path); fd = fioDopen(alt); if (fd >= 0) return fd; }
    if (!strcmp(path, "host:/") || !strcmp(path, "host:")) { fd = fioDopen("host:."); if (fd >= 0) return fd; fd = fioDopen("host:/."); }
    return fd;
}

int fs_host_available(void)
{
    int fd = host_dopen("host:/");
    LOGI("fs: host probe -> %d", fd);
    if (fd < 0) return 0;
    fioDclose(fd);
    return 1;
}
static int mc_wait(void) { int r = 0; mcSync(0, NULL, &r); return r; }

static void mc_split(const char *p, int *port, char *out, size_t n)
{
    *port = p[2] - '0';
    const char *q = p + 4;
    if (!*q) q = "/";
    snprintf(out, n, "%s", q);
    size_t l = strlen(out);
    if (l > 1 && out[l - 1] == '/') out[l - 1] = 0;
}

static int mc_probe(int port)
{
    int r = -99;
    for (int i = 0; i < 3; i++) {                 /* first answer is usually -1 (card changed): ask again so the next command does not re-detect */
        int type = 0, freeb = 0, fmt = 0;
        mcGetInfo(port, 0, &type, &freeb, &fmt);
        r = mc_wait();
        LOGI("mc: probe port %d try %d -> %d (type %d)", port, i, r, type);
        if (r == 0) return 0;
        if (r < -1) return -1;                    /* no card / unformatted / error */
    }
    return r >= -1 ? 0 : -1;
}

int fs_is_root(const char *path)
{
    const char *c = strchr(path, ':');
    if (!c) return 1;
    return c[1] == 0 || (c[1] == '/' && c[2] == 0);
}

static int mc_list(const char *path, Entry *out, int max, int show_hidden)
{
    int port; char dir[PATH_MAX_LEN], pat[PATH_MAX_LEN];
    mc_split(path, &port, dir, sizeof dir);
    if (mc_probe(port) != 0) return -1;
    if (!strcmp(dir, "/")) strcpy(pat, "/*"); else snprintf(pat, sizeof pat, "%s/*", dir);
    LOGI("mc: getdir %s", pat);
    mcGetDir(port, 0, pat, 0, MAX_ENTRIES, mc_tbl);
    int cnt = mc_wait();
    LOGI("mc: getdir -> %d", cnt);
    if (cnt < 0) return -1;
    int n = 0;
    if (!fs_is_root(path)) { strcpy(out[0].name, ".."); out[0].is_dir = 1; out[0].size = 0; out[0].marked = 0; n = 1; }
    for (int i = 0; i < cnt && n < max; i++) {
        const char *nm = (const char *)mc_tbl[i].EntryName;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (!show_hidden && nm[0] == '.') continue;
        strncpy(out[n].name, nm, sizeof out[n].name - 1); out[n].name[sizeof out[n].name - 1] = 0;
        out[n].is_dir = (mc_tbl[i].AttrFile & MC_ATTR_DIR) ? 1 : 0;
        out[n].size = mc_tbl[i].FileSizeByte;
        out[n].marked = 0;
        n++;
    }
    return n;
}

int fs_list(const char *path, Entry *out, int max, int show_hidden)
{
    if (is_mcpath(path)) return mc_list(path, out, max, show_hidden);
    if (is_host(path)) {
        int hfd = host_dopen(path);
        LOGI("fs: host dopen %s -> %d", path, hfd);
        if (hfd < 0) return -1;
        int hn = 0;
        if (!fs_is_root(path)) { strcpy(out[0].name, ".."); out[0].is_dir = 1; out[0].size = 0; out[0].marked = 0; hn = 1; }
        io_dirent_t hd;
        while (hn < max && fioDread(hfd, &hd) > 0) {
            if (!strcmp(hd.name, ".") || !strcmp(hd.name, "..")) continue;
            if (!show_hidden && hd.name[0] == '.') continue;
            strncpy(out[hn].name, hd.name, sizeof out[hn].name - 1);
            out[hn].name[sizeof out[hn].name - 1] = 0;
            out[hn].is_dir = FIO_S_ISDIR(hd.stat.mode) ? 1 : 0;
            out[hn].size = hd.stat.size;
            out[hn].marked = 0;
            hn++;
        }
        fioDclose(hfd);
        return hn;
    }
    LOGI("fs: dopen %s", path);
    int fd = fileXioDopen(path);
    LOGI("fs: dopen -> %d", fd);
    if (fd < 0) return -1;
    int n = 0;
    if (!fs_is_root(path)) { strcpy(out[0].name, ".."); out[0].is_dir = 1; out[0].size = 0; out[0].marked = 0; n = 1; }
    iox_dirent_t de;
    while (n < max && fileXioDread(fd, &de) > 0) {
        if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        if (!show_hidden && de.name[0] == '.') continue;
        strncpy(out[n].name, de.name, sizeof out[n].name - 1);
        out[n].name[sizeof out[n].name - 1] = 0;
        out[n].is_dir = FIO_S_ISDIR(de.stat.mode) ? 1 : 0;
        out[n].size = de.stat.size | ((u64)de.stat.hisize << 32);
        out[n].marked = 0;
        n++;
    }
    fileXioDclose(fd);
    LOGI("fs: %s -> %d entries", path, n);
    return n;
}

void fs_join(char *out, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    snprintf(out, PATH_MAX_LEN, "%s%s%s", dir, (l && dir[l - 1] == '/') ? "" : "/", name);
}

void fs_parent(char *path)
{
    if (fs_is_root(path)) return;
    size_t l = strlen(path);
    if (path[l - 1] == '/') path[--l] = 0;
    char *c = strchr(path, ':');
    char *s = strrchr(path, '/');
    if (s && s > c + 1) s[1] = 0;      /* "mc0:/A/B" -> "mc0:/A/" */
    else { c[1] = '/'; c[2] = 0; }     /* back to root "mc0:/" */
}

int fs_same_device(const char *a, const char *b)
{
    const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
    if (!ca || !cb || (ca - a) != (cb - b)) return 0;
    return strncmp(a, b, ca - a) == 0;
}

const char *fs_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

int fs_exists(const char *path)
{
    if (is_mcpath(path)) {
        int port; char p[PATH_MAX_LEN];
        mc_split(path, &port, p, sizeof p);
        if (!strcmp(p, "/")) return 1;
        if (mc_probe(port) != 0) return 0;
        mcGetDir(port, 0, p, 0, 1, mc_tbl);
        return mc_wait() > 0;
    }
    if (is_host(path)) { int h = fioOpen(path, FIO_O_RDONLY); if (h >= 0) { fioClose(h); return 1; } h = host_dopen(path); if (h >= 0) { fioDclose(h); return 1; } return 0; }
    iox_stat_t st;
    return fileXioGetStat(path, &st) >= 0;
}

int fs_is_dir(const char *path)
{
    if (is_mcpath(path)) {
        int port; char p[PATH_MAX_LEN];
        mc_split(path, &port, p, sizeof p);
        if (!strcmp(p, "/")) return 1;
        if (mc_probe(port) != 0) return 0;
        mcGetDir(port, 0, p, 0, 1, mc_tbl);
        return mc_wait() > 0 && (mc_tbl[0].AttrFile & MC_ATTR_DIR);
    }
    if (is_host(path)) { int h = host_dopen(path); if (h >= 0) { fioDclose(h); return 1; } return 0; }
    iox_stat_t st;
    return fileXioGetStat(path, &st) >= 0 && FIO_S_ISDIR(st.mode);
}

void fs_unique(char *path)
{
    char base[PATH_MAX_LEN];
    strncpy(base, path, sizeof base - 1); base[sizeof base - 1] = 0;
    for (int i = 0; i < 99 && fs_exists(path); i++) {
        if (i == 0) snprintf(path, PATH_MAX_LEN, "%s_copy", base);
        else snprintf(path, PATH_MAX_LEN, "%s_copy%d", base, i + 1);
    }
}

int fs_mkdir(const char *path)
{
    if (is_mcpath(path)) {
        int port; char p[PATH_MAX_LEN];
        mc_split(path, &port, p, sizeof p);
        if (mc_probe(port) != 0) return -1;
        mcMkDir(port, 0, p);
        return mc_wait() >= 0 ? 0 : -1;
    }
    if (is_host(path)) return fioMkdir(path) >= 0 ? 0 : -1;
    return fileXioMkdir(path, 0777);
}

int fs_rename(const char *path, const char *newpath)
{
    if (is_mcpath(path)) {
        int port, port2; char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
        mc_split(path, &port, a, sizeof a); mc_split(newpath, &port2, b, sizeof b);
        if (port != port2 || mc_probe(port) != 0) return -1;
        mcRename(port, 0, a, b);
        return mc_wait() >= 0 ? 0 : -1;
    }
    if (is_host(path)) return -1;                 /* the emulator host driver has no rename */
    return fileXioRename(path, newpath);
}

/* ---- tiny file abstraction over fileXio / libmc ---- */
typedef struct { int mc, host, port, fd; } VF;

static int vf_open(VF *f, const char *path, int flags)
{
    if (is_mcpath(path)) {
        char p[PATH_MAX_LEN];
        mc_split(path, &f->port, p, sizeof p);
        if (mc_probe(f->port) != 0) return -1;
        f->mc = 1;
        mcOpen(f->port, 0, p, flags);
        f->fd = mc_wait();
        return f->fd >= 0 ? 0 : -1;
    }
    f->mc = 0;
    f->host = is_host(path);
    if (f->host) { f->fd = fioOpen(path, flags); return f->fd >= 0 ? 0 : -1; }
    f->fd = fileXioOpen(path, flags, 0666);
    return f->fd >= 0 ? 0 : -1;
}
static int vf_read(VF *f, void *b, int n)        { if (f->mc) { mcRead(f->fd, b, n); return mc_wait(); } if (f->host) return fioRead(f->fd, b, n); return fileXioRead(f->fd, b, n); }
static int vf_write(VF *f, const void *b, int n) { if (f->mc) { mcWrite(f->fd, b, n); return mc_wait(); } if (f->host) return fioWrite(f->fd, (void *)b, n); return fileXioWrite(f->fd, (void *)b, n); }
static int vf_seek(VF *f, int off, int wh)       { if (f->mc) { mcSeek(f->fd, off, wh); return mc_wait(); } if (f->host) return fioLseek(f->fd, off, wh); return fileXioLseek(f->fd, off, wh); }
static void vf_close(VF *f)                      { if (f->mc) { mcClose(f->fd); mc_wait(); } else if (f->host) fioClose(f->fd); else fileXioClose(f->fd); }

int fs_dev_ready(const char *rootpath)
{
    if (is_mcpath(rootpath)) { int port; char p[PATH_MAX_LEN]; mc_split(rootpath, &port, p, sizeof p); return mc_probe(port) == 0; }
    if (is_host(rootpath)) { int h = host_dopen(rootpath); if (h < 0) return 0; fioDclose(h); return 1; }
    int fd = fileXioDopen(rootpath);
    if (fd < 0) return 0;
    fileXioDclose(fd);
    return 1;
}

int fs_write_all(const char *path, const void *data, int len)
{
    VF f;
    if (vf_open(&f, path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC) != 0) return -1;
    int off = 0, rc = 0;
    while (off < len) {
        int n = len - off > (int)sizeof cbuf ? (int)sizeof cbuf : len - off;
        memcpy(cbuf, (const char *)data + off, n);
        if (vf_write(&f, cbuf, n) != n) { rc = -2; break; }
        off += n;
    }
    vf_close(&f);
    return rc;
}

int fs_size(const char *path)
{
    VF f;
    if (vf_open(&f, path, FIO_O_RDONLY) != 0) return -1;
    int sz = vf_seek(&f, 0, FIO_SEEK_END);
    vf_close(&f);
    return sz;
}

unsigned char *fs_read_all(const char *path, unsigned *len, unsigned cap)
{
    VF f;
    if (vf_open(&f, path, FIO_O_RDONLY) != 0) return NULL;
    int sz = vf_seek(&f, 0, FIO_SEEK_END);
    vf_seek(&f, 0, FIO_SEEK_SET);
    if (sz <= 0 || (unsigned)sz > cap) { vf_close(&f); return NULL; }
    unsigned char *buf = malloc((size_t)sz + 16);
    if (!buf) { vf_close(&f); return NULL; }
    int got = 0;
    while (got < sz) {
        int want = sz - got > (int)sizeof cbuf ? (int)sizeof cbuf : sz - got;
        int r = vf_read(&f, cbuf, want);
        if (r <= 0) break;
        memcpy(buf + got, cbuf, r);
        got += r;
    }
    vf_close(&f);
    memset(buf + got, 0, 16);
    if (got != sz) { free(buf); return NULL; }
    if (len) *len = (unsigned)sz;
    return buf;
}

int fs_read_at(const char *path, u32 off, void *buf, int len)
{
    VF f;
    if (vf_open(&f, path, FIO_O_RDONLY) != 0) return -1;
    vf_seek(&f, (int)off, FIO_SEEK_SET);
    if (len > (int)sizeof cbuf) len = sizeof cbuf;
    int r = vf_read(&f, cbuf, len);
    if (r > 0) memcpy(buf, cbuf, r);
    vf_close(&f);
    return r;
}

static int copy_file(const char *src, const char *dst, ProgressCb cb)
{
    VF in, out;
    if (vf_open(&in, src, FIO_O_RDONLY) != 0) { LOGE("open %s failed", src); return -1; }
    int total = vf_seek(&in, 0, FIO_SEEK_END);
    vf_seek(&in, 0, FIO_SEEK_SET);
    if (vf_open(&out, dst, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC) != 0) { vf_close(&in); LOGE("create %s failed", dst); return -2; }
    u64 done = 0; int rc = 0, tick = 0;
    for (;;) {
        int r = vf_read(&in, cbuf, sizeof cbuf);
        if (r < 0) { rc = -3; break; }
        if (r == 0) break;
        if (vf_write(&out, cbuf, r) != r) { rc = -4; LOGE("write error (disk full?)"); break; }
        done += r;
        if (cb && (++tick % 2 == 0) && cb(fs_basename(src), done, total > 0 ? total : 0)) { rc = -9; break; }
    }
    vf_close(&in); vf_close(&out);
    if (rc) {                                    /* never leave a half-written file */
        if (is_mcpath(dst)) { int p; char q[PATH_MAX_LEN]; mc_split(dst, &p, q, sizeof q); mcDelete(p, 0, q); mc_wait(); }
        else fileXioRemove(dst);
    }
    return rc;
}

int fs_copy(const char *src, const char *dst, ProgressCb cb)
{
    if (!fs_is_dir(src)) return copy_file(src, dst, cb);
    if (fs_mkdir(dst) < 0 && !fs_exists(dst)) return -2;
    Entry *l = malloc(sizeof(Entry) * 1024);
    if (!l) return -5;
    int n = fs_list(src, l, 1024, 1), rc = 0;
    for (int i = 0; i < n && !rc; i++) {
        if (!strcmp(l[i].name, "..")) continue;
        char s[PATH_MAX_LEN], d[PATH_MAX_LEN];
        fs_join(s, src, l[i].name); fs_join(d, dst, l[i].name);
        rc = fs_copy(s, d, cb);
    }
    free(l);
    return rc;
}

static int remove_one(const char *path, int is_dir)
{
    if (is_mcpath(path)) {
        int p; char q[PATH_MAX_LEN];
        mc_split(path, &p, q, sizeof q);
        mcDelete(p, 0, q);
        return mc_wait() >= 0 ? 0 : -1;
    }
    if (is_host(path)) return is_dir ? fioRmdir(path) : fioRemove(path);
    return is_dir ? fileXioRmdir(path) : fileXioRemove(path);
}

int fs_delete(const char *path, ProgressCb cb)
{
    if (cb && cb(fs_basename(path), 0, 0)) return -9;
    int dir = fs_is_dir(path);
    if (!dir) return remove_one(path, 0);
    Entry *l = malloc(sizeof(Entry) * 1024);
    if (!l) return -5;
    int n = fs_list(path, l, 1024, 1), rc = 0;
    for (int i = 0; i < n && !rc; i++) {
        if (!strcmp(l[i].name, "..")) continue;
        char p[PATH_MAX_LEN];
        fs_join(p, path, l[i].name);
        rc = fs_delete(p, cb);
    }
    free(l);
    return rc ? rc : remove_one(path, 1);
}

int fs_move(const char *src, const char *dst, ProgressCb cb)
{
    if (fs_same_device(src, dst) && fs_rename(src, dst) >= 0) return 0;
    int rc = fs_copy(src, dst, cb);
    if (rc) return rc;
    return fs_delete(src, NULL);          /* delete only after a complete copy */
}


/* ---- ZIP extraction (stored + deflate, via zlib's raw inflate) ---- */
#define ZIP_IN   (16 * 1024)
#define ZIP_OUT  (32 * 1024)
static unsigned zr16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned zr32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

/* Make "a/b/c" below dest, one level at a time (errors ignored: it usually exists already). */
static void zip_mkparents(const char *dest, const char *rel)
{
    char p[PATH_MAX_LEN];
    for (const char *s = rel; (s = strchr(s, '/')) != NULL; s++) {
        int n = (int)(s - rel);
        if ((int)strlen(dest) + 1 + n >= PATH_MAX_LEN) return;
        snprintf(p, sizeof p, "%s/%.*s", dest, n, rel);
        fs_mkdir(p);
    }
}

int fs_unzip(const char *zip, const char *dest, ProgressCb cb, int *n_ok, int *n_bad)
{
    int ok = 0, bad = 0;
    if (n_ok) *n_ok = 0;
    if (n_bad) *n_bad = 0;
#ifndef HAVE_ZLIB
    (void)zip; (void)dest; (void)cb;
    return -5;
#else
    VF zf;
    if (vf_open(&zf, zip, FIO_O_RDONLY) != 0) return -1;
    int zsize = vf_seek(&zf, 0, FIO_SEEK_END);
    if (zsize < 22) { vf_close(&zf); return -2; }
    int rc = -2;
    unsigned char *inb = memalign(64, ZIP_IN), *outb = memalign(64, ZIP_OUT), *tail = memalign(64, 65536 + 64), *cd = NULL;
    if (!inb || !outb || !tail) { rc = -4; goto done; }

    /* end-of-central-directory record: in the last 64 KB + 22 bytes */
    int tl = zsize < 65558 ? zsize : 65558, tpos = zsize - tl;
    vf_seek(&zf, tpos, FIO_SEEK_SET);
    int got = 0;
    while (got < tl) { int r = vf_read(&zf, tail + got, tl - got); if (r <= 0) break; got += r; }
    if (got != tl) goto done;
    int e = -1;
    for (int i = tl - 22; i >= 0; i--) if (zr32(tail + i) == 0x06054b50) { e = i; break; }
    if (e < 0) goto done;
    unsigned nent = zr16(tail + e + 10), cdsize = zr32(tail + e + 12), cdoff = zr32(tail + e + 16);
    if (nent == 0xFFFF || cdsize == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) { rc = -3; goto done; }   /* zip64 */
    if (cdsize > 4u * 1024 * 1024 || cdoff + cdsize > (unsigned)zsize) { rc = -3; goto done; }
    cd = malloc(cdsize + 8);
    if (!cd) { rc = -4; goto done; }
    vf_seek(&zf, (int)cdoff, FIO_SEEK_SET);
    got = 0;
    while ((unsigned)got < cdsize) { int r = vf_read(&zf, outb, (cdsize - got) > ZIP_OUT ? ZIP_OUT : (int)(cdsize - got)); if (r <= 0) break; memcpy(cd + got, outb, r); got += r; }
    if ((unsigned)got != cdsize) goto done;

    /* total size, for the progress bar */
    u64 total = 0, done_b = 0;
    { unsigned o = 0; for (unsigned i = 0; i < nent && o + 46 <= cdsize; i++) {
        if (zr32(cd + o) != 0x02014b50) break;
        total += zr32(cd + o + 24);
        o += 46 + zr16(cd + o + 28) + zr16(cd + o + 30) + zr16(cd + o + 32);
    } }

    rc = 0;
    unsigned o = 0;
    for (unsigned i = 0; i < nent && o + 46 <= cdsize; i++) {
        const unsigned char *h = cd + o;
        if (zr32(h) != 0x02014b50) break;
        unsigned flags = zr16(h + 8), method = zr16(h + 10), crc = zr32(h + 16), csize = zr32(h + 20), usize = zr32(h + 24);
        unsigned nl = zr16(h + 28), xl = zr16(h + 30), cl = zr16(h + 32), lho = zr32(h + 42);
        o += 46 + nl + xl + cl;
        if (o > cdsize || nl == 0 || nl > 200) { bad++; continue; }

        char rel[256]; int rl = 0, skip = 0;
        for (unsigned k = 0; k < nl; k++) { char c = (char)h[46 + k]; if (c == '\\') c = '/'; if (c == ':') skip = 1; rel[rl++] = c; }
        rel[rl] = 0;
        char *r0 = rel; while (*r0 == '/') r0++;
        if (strstr(r0, "../") || !strcmp(r0, "..") || !*r0) skip = 1;           /* never write outside the target folder */
        if (skip || (int)strlen(dest) + 1 + (int)strlen(r0) >= PATH_MAX_LEN) { bad++; LOGW("unzip: skipped %.60s", rel); continue; }

        int isdir = r0[strlen(r0) - 1] == '/';
        char out[PATH_MAX_LEN];
        if (isdir) { r0[strlen(r0) - 1] = 0; if (!*r0) continue; }
        snprintf(out, sizeof out, "%s/%s", dest, r0);
        if (isdir) { zip_mkparents(dest, r0); fs_mkdir(out); ok++; continue; }
        if ((flags & 1) || (method != 0 && method != 8)) { bad++; LOGW("unzip: unsupported entry %s (flags %u method %u)", r0, flags, method); done_b += usize; continue; }

        zip_mkparents(dest, r0);
        unsigned char lh[30];
        vf_seek(&zf, (int)lho, FIO_SEEK_SET);
        if (vf_read(&zf, lh, 30) != 30 || zr32(lh) != 0x04034b50) { bad++; continue; }
        vf_seek(&zf, (int)(lho + 30 + zr16(lh + 26) + zr16(lh + 28)), FIO_SEEK_SET);

        VF of;
        if (vf_open(&of, out, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC) != 0) { bad++; LOGE("unzip: cannot create %s", out); continue; }
        z_stream z; int zinit = 0, fail = 0; unsigned long c = crc32(0L, Z_NULL, 0); unsigned remaining = csize; u64 wrote = 0;
        if (method == 8) { memset(&z, 0, sizeof z); if (inflateInit2(&z, -15) != Z_OK) fail = 1; else zinit = 1; }
        while (!fail) {
            int n, last = 0;
            if (method == 0) {
                if (!remaining) break;
                int want = remaining > ZIP_OUT ? ZIP_OUT : (int)remaining;
                n = vf_read(&zf, outb, want);
                if (n <= 0) { fail = 1; break; }
                remaining -= n;
            } else {
                if (z.avail_in == 0 && remaining) {
                    int want = remaining > ZIP_IN ? ZIP_IN : (int)remaining;
                    int r = vf_read(&zf, inb, want);
                    if (r <= 0) { fail = 1; break; }
                    z.next_in = inb; z.avail_in = r; remaining -= r;
                }
                z.next_out = outb; z.avail_out = ZIP_OUT;
                int zr = inflate(&z, Z_NO_FLUSH);
                n = ZIP_OUT - z.avail_out;
                if (zr == Z_STREAM_END) last = 1;
                else if (zr != Z_OK && !(zr == Z_BUF_ERROR && n > 0)) { fail = 1; }
                else if (zr == Z_BUF_ERROR && z.avail_in == 0 && !remaining) { fail = 1; }
            }
            if (n > 0) {
                if (vf_write(&of, outb, n) != n) { fail = 1; LOGE("unzip: write error (full?)"); break; }
                c = crc32(c, outb, n); wrote += n; done_b += n;
                if (cb && cb(r0, done_b, total ? total : 1)) { vf_close(&of); if (zinit) inflateEnd(&z); rc = -9; goto done; }
            }
            if (last || (method == 0 && !remaining)) break;
        }
        if (zinit) inflateEnd(&z);
        vf_close(&of);
        if (!fail && (wrote != usize || (unsigned)c != crc)) { fail = 1; LOGW("unzip: size/CRC mismatch in %s", r0); }
        if (fail) { bad++; LOGE("unzip: failed on %s", r0); } else ok++;
    }
done:
    if (zf.fd >= 0) vf_close(&zf);
    free(inb); free(outb); free(tail); free(cd);
    if (n_ok) *n_ok = ok;
    if (n_bad) *n_bad = bad;
    LOGI("unzip %s -> %s: rc=%d ok=%d bad=%d", zip, dest, rc, ok, bad);
    return rc;
#endif
}


/* ---- memory card tools (used by "Format memory card") ---- */
int fs_mc_info(int port, int *type, int *free_kb)
{
    int r = -99, t = 0, fr = 0, fm = 0;
    for (int i = 0; i < 3; i++) {                      /* the first answer is usually -1 (card changed): ask again */
        t = fr = fm = 0;
        mcGetInfo(port, 0, &t, &fr, &fm);
        r = mc_wait();
        LOGI("mc: info port %d try %d -> %d (type %d free %d fmt %d)", port, i, r, t, fr, fm);
        if (r == 0 || r == -2) break;
        if (r < -2) break;                             /* no card / error */
    }
    if (type) *type = t;
    if (free_kb) *free_kb = fr;
    if (r == 0 || r == -1) return 0;
    if (r == -2) return 1;
    return -1;
}

int fs_mc_format(int port)
{
    LOGI("mc: FORMAT port %d", port);
    mcFormat(port, 0);
    int r = mc_wait();
    LOGI("mc: format -> %d", r);
    return r >= 0 ? 0 : r;
}

int fs_mc_wipe(int port)
{
    char root[8];
    snprintf(root, sizeof root, "mc%d:/", port);
    LOGI("mc: quick erase %s", root);
    if (mc_probe(port) != 0) return -1;
    Entry *l = malloc(sizeof(Entry) * 1024);
    if (!l) return -2;
    int n = fs_list(root, l, 1024, 1), bad = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(l[i].name, "..")) continue;
        char p[PATH_MAX_LEN];
        fs_join(p, root, l[i].name);
        if (fs_delete(p, NULL) != 0) { bad++; LOGW("mc: could not delete %s", p); }
    }
    free(l);
    LOGI("mc: quick erase done, %d failed", bad);
    return bad ? -3 : 0;
}

/* ---- disc drive (cdfs: is served by cdfs.irx; the disc type comes from libcdvd) ---- */
int fs_cd_type(void)
{
#ifdef HAVE_CDVD
    static int inited;
    if (!inited) { sceCdInit(SCECdINIT); inited = 1; }
    if (sceCdDiskReady(1) != SCECdComplete) return 1;          /* drive busy: still detecting */
    return sceCdGetDiskType();
#else
    return -1;
#endif
}

const char *fs_cd_name(int t)
{
    switch (t) {
    case 0x00: return "no disc";
    case 0x01: return "detecting disc";
    case 0x10: return "PS1 CD";
    case 0x11: return "PS1 CD (audio tracks)";
    case 0x12: return "PS2 CD";
    case 0x13: return "PS2 CD (audio tracks)";
    case 0x14: return "PS2 DVD";
    case 0xFD: return "audio CD";
    case 0xFE: return "DVD video";
    case 0xFF: return "unreadable disc";
    default:   return "disc";
    }
}

/* ---- plain streaming file access for the archive writers (compress.c) ---- */
struct FsFile { VF f; };
static unsigned char fbounce[32 * 1024] __attribute__((aligned(64)));
FsFile *fs_fopen(const char *path, int write)
{
    FsFile *f = malloc(sizeof *f);
    if (!f) return NULL;
    if (vf_open(&f->f, path, write ? (FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC) : FIO_O_RDONLY) != 0) { free(f); return NULL; }
    return f;
}
int fs_fread(FsFile *f, void *b, int n)
{
    if (n > (int)sizeof fbounce) n = (int)sizeof fbounce;
    int r = vf_read(&f->f, fbounce, n);
    if (r > 0) memcpy(b, fbounce, r);
    return r;
}
int fs_fwrite(FsFile *f, const void *b, int n)
{
    int done = 0;
    while (done < n) {
        int k = n - done > (int)sizeof fbounce ? (int)sizeof fbounce : n - done;
        memcpy(fbounce, (const char *)b + done, k);
        if (vf_write(&f->f, fbounce, k) != k) return done;
        done += k;
    }
    return done;
}
int fs_fseek(FsFile *f, int off, int whence) { return vf_seek(&f->f, off, whence == 2 ? FIO_SEEK_END : FIO_SEEK_SET); }
void fs_fclose(FsFile *f) { if (f) { vf_close(&f->f); free(f); } }

/* ---- .7z extraction (the decoder lives in sevenz.c; this only supplies file access) ---- */
#define SZ_BOUNCE (32 * 1024)
typedef struct { VF f; int opened; const char *dest; ProgressCb cb; unsigned char *ab; } SzCtx;
static char sz_last_msg[96];
const char *fs_un7z_msg(void) { return sz_last_msg; }

static int szc_read(void *c, void *buf, int n)
{
    SzCtx *x = c;
    if (n > SZ_BOUNCE) n = SZ_BOUNCE;
    int r = vf_read(&x->f, x->ab, n);
    if (r > 0) memcpy(buf, x->ab, r);
    return r;
}
static int szc_seek(void *c, unsigned long long off) { SzCtx *x = c; return vf_seek(&x->f, (int)off, FIO_SEEK_SET) == (int)off ? 0 : -1; }
static unsigned long long szc_size(void *c) { SzCtx *x = c; int e = vf_seek(&x->f, 0, FIO_SEEK_END); vf_seek(&x->f, 0, FIO_SEEK_SET); return e > 0 ? (unsigned long long)e : 0; }
static int szc_mkdir(void *c, const char *rel) { SzCtx *x = c; char p[PATH_MAX_LEN]; if ((int)strlen(x->dest) + 1 + (int)strlen(rel) >= PATH_MAX_LEN) return -1; snprintf(p, sizeof p, "%s/%s", x->dest, rel); return fs_mkdir(p); }
static void *szc_create(void *c, const char *rel)
{
    SzCtx *x = c; char p[PATH_MAX_LEN];
    if ((int)strlen(x->dest) + 1 + (int)strlen(rel) >= PATH_MAX_LEN) return NULL;
    snprintf(p, sizeof p, "%s/%s", x->dest, rel);
    VF *h = malloc(sizeof *h);
    if (!h) return NULL;
    if (vf_open(h, p, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC) != 0) { free(h); return NULL; }
    return h;
}
static int szc_write(void *c, void *h, const void *buf, int n)
{
    SzCtx *x = c; int done = 0;
    while (done < n) {
        int k = n - done > SZ_BOUNCE ? SZ_BOUNCE : n - done;
        memcpy(x->ab, (const char *)buf + done, k);
        if (vf_write((VF *)h, x->ab, k) != k) return done;
        done += k;
    }
    return done;
}
static void szc_close(void *c, void *h) { (void)c; vf_close((VF *)h); free(h); }
static int szc_progress(void *c, const char *name, unsigned long long d, unsigned long long t) { SzCtx *x = c; return x->cb ? x->cb(name, d, t) : 0; }
static void szc_log(void *c, int lv, const char *m) { (void)c; if (lv >= 2) LOGE("7z: %s", m); else if (lv == 1) LOGW("7z: %s", m); else LOGI("7z: %s", m); }

/* 0 ok, -1 cannot open/read, -2 not a 7z / damaged, -3 unsupported (password, PPMd, BZip2 ...), -4 memory, -9 cancelled */
int fs_un7z(const char *file, const char *dest, ProgressCb cb, int *n_ok, int *n_bad)
{
    SzCtx x; memset(&x, 0, sizeof x);
    sz_last_msg[0] = 0;
    if (n_ok) *n_ok = 0;
    if (n_bad) *n_bad = 0;
    if (vf_open(&x.f, file, FIO_O_RDONLY) != 0) return -1;
    x.ab = memalign(64, SZ_BOUNCE);
    if (!x.ab) { vf_close(&x.f); return -4; }
    x.dest = dest; x.cb = cb;
    SzIO io = { &x, szc_read, szc_seek, szc_size, szc_mkdir, szc_create, szc_write, szc_close, szc_progress, szc_log };
    int rc = sz_extract(&io, n_ok, n_bad, sz_last_msg, sizeof sz_last_msg);
    vf_close(&x.f);
    free(x.ab);
    LOGI("un7z %s -> %s: rc=%d (%s)", file, dest, rc, sz_last_msg);
    return rc;
}
