#ifndef COMPRESS_H
#define COMPRESS_H
#include "fs.h"
/* Create a .zip (kind 0) or .7z (kind 1) from files / folders. names[] are entries inside basedir; folders are added with everything below them.
   Own code: zip uses zlib's deflate (stored only when the build has no zlib); 7z uses a small LZMA encoder (one solid block, header not compressed).
   Returns 0 ok, -1 cannot read the sources, -2 cannot create the archive, -3 write error (disk full?), -4 out of memory,
   -5 too big for this format (zip: 4 GB / 65535 entries), -9 cancelled. On any error the partial archive is deleted.
   *n_ok = files and folders stored, *n_bad = files that could not be read (left out). */
int compress_make(int kind, const char *out, const char *basedir, const char *const *names, int n,
                  ProgressCb cb, int *n_ok, int *n_bad, char *msg, int msglen);
#endif
