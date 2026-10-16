#ifndef FS_H
#define FS_H
#include <tamtypes.h>
#define PATH_MAX_LEN 256
#define MAX_ENTRIES  2048
typedef struct {
    char name[96];
    int  is_dir;
    u64  size;
    int  marked;
} Entry;
/* progress callback: return non-zero to cancel */
typedef int (*ProgressCb)(const char *what, u64 done, u64 total);

int  fs_list(const char *path, Entry *out, int max, int show_hidden);   /* adds ".." if not at root */
void fs_join(char *out, const char *dir, const char *name);
void fs_parent(char *path);
int  fs_is_root(const char *path);
int  fs_same_device(const char *a, const char *b);
int  fs_exists(const char *path);
const char *fs_basename(const char *path);
void fs_unique(char *path);                                  /* appends _copy until free */
int  fs_copy(const char *src, const char *dst, ProgressCb cb);   /* file or folder */
int  fs_delete(const char *path, ProgressCb cb);                 /* file or folder */
int  fs_move(const char *src, const char *dst, ProgressCb cb);
int  fs_rename(const char *path, const char *newname);
int  fs_mkdir(const char *path);
int  fs_is_dir(const char *path);
int  fs_unzip(const char *zip, const char *dest, ProgressCb cb, int *n_ok, int *n_bad);   /* extract a .zip into dest (must exist); 0 ok, -1 open, -2 not a zip, -3 zip64/too big, -4 memory, -5 no zlib in this build, -9 cancelled */
int  fs_un7z(const char *file, const char *dest, ProgressCb cb, int *n_ok, int *n_bad);   /* extract a .7z into dest (must exist); 0 ok, -1 open, -2 not 7z/damaged, -3 unsupported (password, PPMd, BZip2...), -4 memory, -9 cancelled */
const char *fs_un7z_msg(void);                                    /* short reason for the last fs_un7z problem */
typedef struct FsFile FsFile;                                   /* streaming access to one file on any device (used by the archive writers) */
FsFile *fs_fopen(const char *path, int write);                  /* write=1: create / truncate. NULL on failure */
int  fs_fread(FsFile *f, void *buf, int n);                     /* bytes read (<= 32 KB per call), 0 at end, <0 error */
int  fs_fwrite(FsFile *f, const void *buf, int n);              /* bytes written (n unless the disk is full) */
int  fs_fseek(FsFile *f, int off, int whence);                  /* whence 0 = from start, 2 = from end; returns the new position or <0 */
void fs_fclose(FsFile *f);
int  fs_mc_info(int port, int *type, int *free_kb);               /* memory card in port: 0 formatted, 1 not formatted, -1 no card */
int  fs_mc_format(int port);                                      /* full format (rebuilds the whole card); 0 = ok */
int  fs_mc_wipe(int port);                                        /* quick erase: delete everything, keep the format; 0 = ok */
int  fs_cd_type(void);                                             /* disc in the drive: 0 none, 1 detecting, 0x10-0x14 PS1/PS2 CD/DVD, 0xFD audio CD, 0xFE DVD video, 0xFF bad, -1 unsupported build */
const char *fs_cd_name(int type);
int  fs_host_available(void);                                      /* 1 if an emulator host: filesystem answers */
int  fs_dev_ready(const char *rootpath);                          /* can this device be opened right now? (USB: dopen) */
int  fs_write_all(const char *path, const void *data, int len);   /* create/overwrite; 0 ok */
int  fs_size(const char *path);                                   /* file size or -1 */
unsigned char *fs_read_all(const char *path, unsigned *len, unsigned cap);   /* malloc'd (+16 zero bytes), NULL on error/too big */
int  fs_read_at(const char *path, u32 off, void *buf, int len);  /* bytes read or <0 */
#endif
