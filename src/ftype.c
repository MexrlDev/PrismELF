#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "ftype.h"

static int in_list(const char *e, const char *list)
{
    size_t n = strlen(e);
    if (!n || n > 8) return 0;
    char low[10];
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)e[i]);
    for (const char *p = list; *p;) {
        const char *sp = strchr(p, ' ');
        size_t l = sp ? (size_t)(sp - p) : strlen(p);
        if (l == n && !strncmp(p, low, n)) return 1;
        if (!sp) break;
        p = sp + 1;
    }
    return 0;
}

/* Well-known text files that have no (or a non-text) extension: Makefile, README, LICENSE, ... (case-insensitive) */
static int is_text_name(const char *name)
{
    static const char *const names =
        " makefile gnumakefile readme license licence copying changelog changes authors notice todo install news"
        " dockerfile procfile rakefile gemfile vagrantfile jenkinsfile cmakelists.txt configure"
        " .gitignore .gitattributes .gitmodules .editorconfig .bashrc .profile .env .htaccess ";
    char low[40];
    size_t n = strlen(name);
    if (n == 0 || n > 30) return 0;
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)name[i]);
    if (!strncmp(low, "makefile.", 9) || !strncmp(low, "readme.", 7)) return 1;      /* Makefile.pref, Makefile.in, README.1st ... */
    char key[44];
    snprintf(key, sizeof key, " %s ", low);
    return strstr(names, key) != NULL;
}

FKind ftype_of(const char *name)
{
    const char *d = strrchr(name, '.');
    if (d && d != name && in_list(d + 1, "md markdown")) return FK_MD;      /* README.md is Markdown, not just "a README" */
    if (is_text_name(name)) return FK_TEXT;
    if (!d || d == name) return FK_OTHER;
    const char *e = d + 1;
    if (in_list(e, "elf")) return FK_ELF;
    if (in_list(e, "mp3 mp2 mpa wav ogg flac aac m4a wma ac3 dts mid it xm s3m mod")) return FK_AUDIO;
    if (in_list(e, "png jpg jpeg bmp gif tga webp tif tiff ico")) return FK_IMAGE;
    if (in_list(e, "mp4 m4v avi divx xvid mkv mov wmv mpg mpeg mpe m2v ogv flv webm")) return FK_VIDEO;
    if (in_list(e, "md markdown")) return FK_MD;
    if (in_list(e, "txt log cfg ini conf nfo json xml csv lua c h cpp py js sh bat yml yaml toml mk mak cmake am in ac s asm ld rs go ts html htm css java rb php pl rst tex diff patch sql cc cxx hpp hh inc svg")) return FK_TEXT;
    if (in_list(e, "zip rar 7z gz tar bz2 xz iso")) return FK_ARCHIVE;
    return FK_OTHER;
}

int ftype_supported(const char *name)
{
    FKind k = ftype_of(name);
    if (k == FK_TEXT || k == FK_MD) return 1;                  /* includes Makefile, README ... (no extension) */
    const char *d = strrchr(name, '.');
    if (!d || d == name) return 0;
    const char *e = d + 1;
    if (k == FK_ELF) return 1;
    if (k == FK_IMAGE) return in_list(e, "png jpg jpeg bmp gif tga");
    if (k == FK_AUDIO) return in_list(e, "wav mp3 mp2 mpa flac");
    return 0;
}

/* formats this app cannot decode itself but a PS2 media player can: video (MPEG-1/2, MPEG-4 ASP in AVI/MP4/DivX/Xvid) and AAC, Ogg, WMA, AC-3, DTS */
int ftype_external(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d || d == name) return 0;
    const char *e = d + 1;
    return in_list(e, "mp4 m4v avi divx xvid mpg mpeg mpe m2v ogg aac m4a wma ac3 dts");
}

/* Content sniffing for files whose name says nothing (no extension, dotfiles, unknown extensions).
   Looks at the first bytes only: text if there is no NUL, almost no control characters and not too many high bytes.
   UTF-8 (with or without BOM) and plain Latin-1 pass; UTF-16 with a BOM passes too. */
int ftype_looks_like_text(const unsigned char *b, int n)
{
    if (n <= 0) return 0;
    if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) return 1;   /* UTF-16 BOM */
    if (n >= 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') return 0;
    int ctrl = 0, high = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 0) return 0;
        if (c >= 0x80) high++;
        else if (c < 32 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != 0x1B && c != '\b') ctrl++;
    }
    return ctrl * 100 <= n && high * 10 <= n * 3;                  /* <=1% control bytes, <=30% high bytes */
}
