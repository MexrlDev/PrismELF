#ifndef SEVENZ_H
#define SEVENZ_H
/* .7z extraction (own code, no 7-Zip sources): LZMA, LZMA2, Deflate (needs zlib), Copy, BCJ x86/ARM/ARMT/PPC/SPARC and Delta
   filters, compressed headers, solid archives, empty files and folders, CRC checks.
   Not supported: passwords (AES), PPMd, BZip2, BCJ2, Deflate64, multi-volume archives. Those entries are skipped and counted as failed.
   All file access goes through the callbacks below so the code is the same on the PS2 and in the PC tests. */

typedef struct SzIO {
    void *ctx;
    int  (*read)(void *ctx, void *buf, int n);                    /* sequential read at the current position: bytes read, <=0 on error / end */
    int  (*seek)(void *ctx, unsigned long long off);              /* absolute position in the archive; 0 = ok */
    unsigned long long (*size)(void *ctx);                        /* archive size */
    int  (*mkdir)(void *ctx, const char *rel);                    /* create a folder below the destination (errors ignored: it may exist) */
    void *(*create)(void *ctx, const char *rel);                  /* create / truncate a file for writing; NULL = failed */
    int  (*write)(void *ctx, void *h, const void *buf, int n);    /* bytes written */
    void (*close)(void *ctx, void *h);
    int  (*progress)(void *ctx, const char *name, unsigned long long done, unsigned long long total);   /* non-zero = cancel */
    void (*log)(void *ctx, int level, const char *msg);           /* optional: level 0 info, 1 warning, 2 error */
} SzIO;

/* returns 0 ok (look at *n_bad), -1 cannot read, -2 not a 7z file / damaged header, -3 unsupported (encrypted header, ...),
   -4 out of memory, -9 cancelled. msg (optional) receives a short explanation of the last problem. */
int sz_extract(const SzIO *io, int *n_ok, int *n_bad, char *msg, int msglen);

#endif
