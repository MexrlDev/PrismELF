#ifndef LOG_H
#define LOG_H
/* Persistent, crash-safe log.
   Every line is appended to disk immediately (open-append-close), so a hang or crash keeps
   everything up to the last line. Folder: FileExplorer-Logs/LOGn/LOG.txt on USB (mass:),
   with mc0:/mc1: only as a legacy fallback (memory cards now use the ROM driver, so USB is the reliable log device). Lines logged before the disk is ready are kept in memory
   and written as soon as a device accepts the file. */
#define LOG_LINES 48
#define LOG_LEN   120
extern unsigned log_frame;                        /* set by the UI so lines carry a time (60 per second) */
void log_set_files(int on);                       /* settings: write the log to disk or not */
void log_init(void);                              /* call first thing in main() */
void log_write(const char *lv, const char *fmt, ...);
void log_flush(void);                             /* (re)try to open/upgrade the log device: USB > mc0 > mc1 */
const char *log_path(void);                       /* "" until a device accepted the file */
const char *log_line(int back);                   /* 0 = newest, NULL if none (in-memory ring, for the viewer) */
int  log_count(void);
int  log_dest(void);                              /* 0 = USB, 1 = mc0, 2 = mc1, -1 = none yet */
#define LOGI(...) log_write("I", __VA_ARGS__)
#define LOGW(...) log_write("W", __VA_ARGS__)
#define LOGE(...) log_write("E", __VA_ARGS__)
#endif
