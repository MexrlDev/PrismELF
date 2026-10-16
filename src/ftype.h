#ifndef FTYPE_H
#define FTYPE_H
typedef enum { FK_OTHER, FK_ELF, FK_AUDIO, FK_IMAGE, FK_VIDEO, FK_TEXT, FK_MD, FK_ARCHIVE } FKind;
FKind ftype_of(const char *name);
int   ftype_external(const char *name);    /* 1 if a PS2 media player (not this app) can play it: video, AAC, Ogg, WMA, AC-3, DTS */
int   ftype_supported(const char *name);   /* 1 if the app can launch/view/play it (elf, txt/md, png/jpg/bmp, wav/mp3) */          /* by extension, case-insensitive */
int   ftype_looks_like_text(const unsigned char *buf, int n);   /* 1 if the bytes look like a text file (for files with no usable extension) */
#endif
