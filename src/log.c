#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <kernel.h>
#include <fileXio_rpc.h>
#include <iox_stat.h>
#include <io_common.h>
#include "log.h"

static char ring[LOG_LINES][LOG_LEN];
static int  head, count;
static char hist[16384];               /* full history: replayed into a newly opened log file */
static int  hist_len;
static char g_path[256];
static int  g_dest = -1;
static int  g_sema = -1;
static unsigned seq;
unsigned log_frame;
static int files_on = 1;
void log_set_files(int on) { files_on = on; }

static const char *const bases[] = { "mass:/FileExplorer-Logs", "mc0:/FileExplorer-Logs", "mc1:/FileExplorer-Logs" };

void log_init(void)
{
    ee_sema_t s;
    memset(&s, 0, sizeof s);
    s.init_count = 1; s.max_count = 1;
    g_sema = CreateSema(&s);
    head = count = hist_len = 0; seq = 0; g_path[0] = 0; g_dest = -1;
}

const char *log_path(void) { return g_path; }
int  log_dest(void) { return g_dest; }
int  log_count(void) { return count; }

const char *log_line(int back)
{
    if (back < 0 || back >= count) return NULL;
    return ring[(head - 1 - back + 2 * LOG_LINES) % LOG_LINES];
}

static int append_raw(const char *path, const char *data, int n)
{
    int fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_APPEND);
    if (fd < 0) return -1;
    int w = fileXioWrite(fd, (void *)data, n);
    fileXioClose(fd);                  /* closing commits the line to the FAT right away */
    return w == n ? 0 : -1;
}

/* Create FileExplorer-Logs/LOGn/LOG.txt on device d, replay the history into it. */
static int try_dest(int d)
{
    char dir[200], path[256], hdr[320];
    iox_stat_t st;
    fileXioMkdir(bases[d], 0777);                       /* ok if it already exists */
    int n;
    for (n = 1; n < 1000; n++) {
        snprintf(dir, sizeof dir, "%s/LOG%d", bases[d], n);
        if (fileXioGetStat(dir, &st) < 0) break;        /* first free number */
    }
    if (n >= 1000) return -1;
    if (fileXioMkdir(dir, 0777) < 0) return -1;
    snprintf(path, sizeof path, "%s/LOG.txt", dir);
    int fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (fd < 0) return -1;
    int hl = snprintf(hdr, sizeof hdr, "PrismELF log\nFolder: LOG%d\nPath: %s\n========================================\n", n, path);
    fileXioWrite(fd, hdr, hl);
    if (hist_len > 0) fileXioWrite(fd, hist, hist_len);
    fileXioClose(fd);
    strncpy(g_path, path, sizeof g_path - 1); g_path[sizeof g_path - 1] = 0;
    g_dest = d;
    return 0;
}

void log_flush(void)
{
    if (!files_on || g_dest == 0) return;               /* disabled, or already on USB */
    if (g_sema >= 0) WaitSema(g_sema);
    for (int d = 0; d < 3; d++) {
        if (g_dest >= 0 && d >= g_dest) break;          /* only try devices better than the current one */
        if (try_dest(d) == 0) break;
    }
    if (g_sema >= 0) SignalSema(g_sema);
}

void log_write(const char *lv, const char *fmt, ...)
{
    char msg[LOG_LEN - 12];
    char line[LOG_LEN + 16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (g_sema >= 0) WaitSema(g_sema);
    seq++;
    snprintf(ring[head], LOG_LEN, "%s %s", lv, msg);
    int n = snprintf(line, sizeof line, "#%04u f%u %s\n", seq, log_frame, ring[head]);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    printf("%s", line);                                  /* also visible via ps2link */
    head = (head + 1) % LOG_LINES;
    if (count < LOG_LINES) count++;

    if (hist_len + n >= (int)sizeof hist) {              /* history full: drop the oldest half */
        int cut = sizeof hist / 2;
        memmove(hist, hist + cut, hist_len - cut);
        hist_len -= cut;
    }
    memcpy(hist + hist_len, line, n);
    hist_len += n;

    if (files_on && g_path[0] && append_raw(g_path, line, n) < 0) {  /* device vanished: reopen elsewhere at next flush */
        g_path[0] = 0; g_dest = -1;
    }
    if (g_sema >= 0) SignalSema(g_sema);
}
