/* .7z extraction. See sevenz.h. Written from the public 7z / LZMA format descriptions. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sevenz.h"
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#define IBUF       32768
#define OBUF       32768
#define MAX_HEADER (32u * 1024 * 1024)
#define MAX_PATH_LEN 240

/* ------------------------------------------------------------------ crc32 */
static uint32_t crc_tab[256]; static int crc_ready;
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1))); crc_tab[i] = v; }
        crc_ready = 1;
    }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* ------------------------------------------------------------------ byte cursor over a header */
typedef struct { const uint8_t *p, *e; int bad; } Cur;
static uint8_t c8(Cur *c) { if (c->p >= c->e) { c->bad = 1; return 0; } return *c->p++; }
static uint32_t c32(Cur *c) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)c8(c) << (8 * i); return v; }
static uint64_t cnum(Cur *c)
{
    uint8_t first = c8(c), mask = 0x80; uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (!(first & mask)) { v += (uint64_t)(first & (mask - 1)) << (8 * i); return v; }
        v |= (uint64_t)c8(c) << (8 * i);
        mask >>= 1;
    }
    return v;
}
static void cskip(Cur *c, uint64_t n) { if (n > (uint64_t)(c->e - c->p)) { c->bad = 1; c->p = c->e; } else c->p += n; }
static uint8_t *bitvec(Cur *c, uint32_t n)
{
    uint8_t *v = calloc(n ? n : 1, 1); uint8_t b = 0, m = 0;
    if (!v) { c->bad = 2; return NULL; }
    for (uint32_t i = 0; i < n; i++) { if (!m) { b = c8(c); m = 0x80; } v[i] = (b & m) ? 1 : 0; m >>= 1; }
    return v;
}
static uint8_t *bitvec2(Cur *c, uint32_t n)       /* "allDefined" byte, then a bit vector if not all */
{
    if (c8(c)) { uint8_t *v = malloc(n ? n : 1); if (!v) { c->bad = 2; return NULL; } memset(v, 1, n); return v; }
    return bitvec(c, n);
}

/* ------------------------------------------------------------------ structures */
typedef struct { uint64_t id; uint32_t nin, nout; const uint8_t *props; uint32_t nprops; } Coder;
typedef struct {
    uint32_t ncoders; Coder *coders;
    uint32_t nbind; uint32_t *bind_in, *bind_out;
    uint32_t npacked; uint32_t *packed;
    uint32_t nin_total, nout_total; uint64_t *unp;
    int crc_def; uint32_t crc;
    uint32_t first_pack;
    uint32_t nsub;
} Folder;
typedef struct {
    uint64_t pack_pos; uint32_t npack; uint64_t *pack_size;
    uint32_t nfold; Folder *f;
    uint32_t nsub_total; uint64_t *sub_size; uint32_t *sub_crc; uint8_t *sub_crc_def;
} Streams;

static uint64_t folder_unpack(const Folder *f)       /* size of the final output */
{
    for (uint32_t o = 0; o < f->nout_total; o++) {
        int bound = 0;
        for (uint32_t b = 0; b < f->nbind; b++) if (f->bind_out[b] == o) bound = 1;
        if (!bound) return f->unp[o];
    }
    return 0;
}

static void streams_free(Streams *s)
{
    for (uint32_t i = 0; i < s->nfold; i++) {
        Folder *f = &s->f[i];
        free(f->coders); free(f->bind_in); free(f->bind_out); free(f->packed); free(f->unp);
    }
    free(s->f); free(s->pack_size); free(s->sub_size); free(s->sub_crc); free(s->sub_crc_def);
    memset(s, 0, sizeof *s);
}

static int read_digests(Cur *c, uint32_t n, uint8_t **def, uint32_t **crc)
{
    *def = bitvec2(c, n); *crc = calloc(n ? n : 1, 4);
    if (!*def || !*crc) return -4;
    for (uint32_t i = 0; i < n; i++) if ((*def)[i]) (*crc)[i] = c32(c);
    return 0;
}

/* returns 0 ok, -2 damaged, -3 unsupported, -4 memory */
static int parse_streams(Cur *c, Streams *s)
{
    memset(s, 0, sizeof *s);
    int have_sub = 0;
    for (;;) {
        uint64_t id = cnum(c);
        if (c->bad) return c->bad == 2 ? -4 : -2;
        if (id == 0) break;
        if (id == 6) {                                         /* PackInfo */
            s->pack_pos = cnum(c); uint64_t n = cnum(c);
            if (n > 0x1000000) return -2;
            s->npack = (uint32_t)n; s->pack_size = calloc(n ? n : 1, 8);
            if (!s->pack_size) return -4;
            for (;;) {
                uint64_t t = cnum(c); if (c->bad) return -2; if (t == 0) break;
                if (t == 9) { for (uint32_t i = 0; i < s->npack; i++) s->pack_size[i] = cnum(c); }
                else if (t == 0x0A) { uint8_t *d; uint32_t *cr; int r = read_digests(c, s->npack, &d, &cr); free(d); free(cr); if (r) return r; }
                else return -2;
            }
        } else if (id == 7) {                                  /* UnpackInfo */
            if (cnum(c) != 0x0B) return -2;
            uint64_t nf = cnum(c); if (nf > 0x1000000 || c->bad) return -2;
            if (c8(c) != 0) return -3;                         /* external */
            s->nfold = (uint32_t)nf; s->f = calloc(nf ? nf : 1, sizeof(Folder));
            if (!s->f) return -4;
            uint32_t packidx = 0;
            for (uint32_t i = 0; i < s->nfold; i++) {
                Folder *f = &s->f[i];
                uint64_t nc = cnum(c); if (nc == 0 || nc > 32 || c->bad) return -2;
                f->ncoders = (uint32_t)nc; f->coders = calloc(nc, sizeof(Coder));
                if (!f->coders) return -4;
                for (uint32_t k = 0; k < f->ncoders; k++) {
                    Coder *cd = &f->coders[k];
                    uint8_t fl = c8(c); unsigned idsz = fl & 15;
                    if (idsz > 8) return -3;
                    uint64_t id2 = 0; for (unsigned b = 0; b < idsz; b++) id2 = (id2 << 8) | c8(c);
                    cd->id = id2; cd->nin = cd->nout = 1;
                    if (fl & 0x10) { cd->nin = (uint32_t)cnum(c); cd->nout = (uint32_t)cnum(c); }
                    if (cd->nin > 32 || cd->nout > 32) return -3;
                    if (fl & 0x20) { cd->nprops = (uint32_t)cnum(c); cd->props = c->p; cskip(c, cd->nprops); }
                    if (fl & 0x80) return -3;
                    f->nin_total += cd->nin; f->nout_total += cd->nout;
                }
                if (c->bad) return -2;
                if (f->nout_total == 0) return -2;
                f->nbind = f->nout_total - 1;
                f->bind_in = calloc(f->nbind ? f->nbind : 1, 4); f->bind_out = calloc(f->nbind ? f->nbind : 1, 4);
                if (!f->bind_in || !f->bind_out) return -4;
                for (uint32_t b = 0; b < f->nbind; b++) { f->bind_in[b] = (uint32_t)cnum(c); f->bind_out[b] = (uint32_t)cnum(c); }
                if (f->nin_total < f->nbind) return -2;
                f->npacked = f->nin_total - f->nbind;
                f->packed = calloc(f->npacked ? f->npacked : 1, 4);
                if (!f->packed) return -4;
                if (f->npacked == 1) {
                    for (uint32_t k = 0; k < f->nin_total; k++) {
                        int bound = 0; for (uint32_t b = 0; b < f->nbind; b++) if (f->bind_in[b] == k) bound = 1;
                        if (!bound) { f->packed[0] = k; break; }
                    }
                } else for (uint32_t k = 0; k < f->npacked; k++) f->packed[k] = (uint32_t)cnum(c);
                f->first_pack = packidx; packidx += f->npacked;
                f->unp = calloc(f->nout_total, 8);
                if (!f->unp) return -4;
            }
            if (cnum(c) != 0x0C) return -2;
            for (uint32_t i = 0; i < s->nfold; i++) for (uint32_t o = 0; o < s->f[i].nout_total; o++) s->f[i].unp[o] = cnum(c);
            for (;;) {
                uint64_t t = cnum(c); if (c->bad) return -2; if (t == 0) break;
                if (t == 0x0A) {
                    uint8_t *d; uint32_t *cr; int r = read_digests(c, s->nfold, &d, &cr);
                    if (r) { free(d); free(cr); return r; }
                    for (uint32_t i = 0; i < s->nfold; i++) { s->f[i].crc_def = d[i]; s->f[i].crc = cr[i]; }
                    free(d); free(cr);
                } else return -2;
            }
            for (uint32_t i = 0; i < s->nfold; i++) s->f[i].nsub = 1;
        } else if (id == 8) {                                  /* SubStreamsInfo */
            have_sub = 1;
            uint64_t t = cnum(c);
            if (t == 0x0D) { for (uint32_t i = 0; i < s->nfold; i++) { uint64_t n = cnum(c); if (n > 0x4000000) return -2; s->f[i].nsub = (uint32_t)n; } t = cnum(c); }
            uint64_t tot = 0; for (uint32_t i = 0; i < s->nfold; i++) tot += s->f[i].nsub;
            if (tot > 0x4000000 || c->bad) return -2;
            s->nsub_total = (uint32_t)tot;
            s->sub_size = calloc(tot ? tot : 1, 8); s->sub_crc = calloc(tot ? tot : 1, 4); s->sub_crc_def = calloc(tot ? tot : 1, 1);
            if (!s->sub_size || !s->sub_crc || !s->sub_crc_def) return -4;
            uint32_t k = 0;
            for (uint32_t i = 0; i < s->nfold; i++) {
                Folder *f = &s->f[i];
                if (f->nsub == 0) continue;
                uint64_t sum = 0;
                if (t == 9) for (uint32_t j = 0; j + 1 < f->nsub; j++) { uint64_t z = cnum(c); s->sub_size[k + j] = z; sum += z; }
                else if (f->nsub > 1) return -2;
                uint64_t u = folder_unpack(f);
                if (sum > u) return -2;
                s->sub_size[k + f->nsub - 1] = u - sum;
                k += f->nsub;
            }
            if (t == 9) t = cnum(c);
            uint32_t unknown = 0;
            for (uint32_t i = 0; i < s->nfold; i++) if (!(s->f[i].nsub == 1 && s->f[i].crc_def)) unknown += s->f[i].nsub;
            uint8_t *d = NULL; uint32_t *cr = NULL; int got = 0;
            for (;;) {
                if (c->bad) return -2;
                if (t == 0) break;
                if (t == 0x0A) { int r = read_digests(c, unknown, &d, &cr); if (r) return r; got = 1; }
                else { uint64_t z = cnum(c); cskip(c, z); }
                t = cnum(c);
            }
            uint32_t u = 0; k = 0;
            for (uint32_t i = 0; i < s->nfold; i++) {
                Folder *f = &s->f[i];
                if (f->nsub == 1 && f->crc_def) { s->sub_crc_def[k] = 1; s->sub_crc[k] = f->crc; k++; continue; }
                for (uint32_t j = 0; j < f->nsub; j++, k++) if (got && d[u]) { s->sub_crc_def[k] = 1; s->sub_crc[k] = cr[u++]; } else if (got) u++;
            }
            free(d); free(cr);
        } else return -2;
    }
    if (!have_sub) {                                           /* one stream per folder */
        s->nsub_total = s->nfold;
        s->sub_size = calloc(s->nfold ? s->nfold : 1, 8); s->sub_crc = calloc(s->nfold ? s->nfold : 1, 4); s->sub_crc_def = calloc(s->nfold ? s->nfold : 1, 1);
        if (!s->sub_size || !s->sub_crc || !s->sub_crc_def) return -4;
        for (uint32_t i = 0; i < s->nfold; i++) { s->f[i].nsub = 1; s->sub_size[i] = folder_unpack(&s->f[i]); s->sub_crc_def[i] = (uint8_t)s->f[i].crc_def; s->sub_crc[i] = s->f[i].crc; }
    }
    return c->bad ? -2 : 0;
}

/* ------------------------------------------------------------------ decoder stages (pull model) */
typedef struct St St;
struct St { int (*rd)(St *, uint8_t *, int); void (*fin)(St *); St *src; };   /* rd: bytes, 0 = end, <0 = error */
static void st_free(St *s) { while (s) { St *n = s->src; if (s->fin) s->fin(s); free(s); s = n; } }

/* --- archive file, limited to one packed stream */
typedef struct { St st; const SzIO *io; uint64_t rem; } PackSrc;
static int pack_rd(St *s, uint8_t *o, int n)
{
    PackSrc *p = (PackSrc *)s;
    if (p->rem == 0) return 0;
    if ((uint64_t)n > p->rem) n = (int)p->rem;
    int r = p->io->read(p->io->ctx, o, n);
    if (r <= 0) return -1;
    p->rem -= (uint64_t)r;
    return r;
}

/* --- copy */
static int copy_rd(St *s, uint8_t *o, int n) { return s->src->rd(s->src, o, n); }

/* --- BCJ / branch filters and delta */
typedef enum { F_X86, F_ARM, F_ARMT, F_PPC, F_SPARC, F_DELTA } FKindF;
typedef struct { St st; FKindF k; uint8_t *buf; uint32_t cap, filled, pos, conv; int eof; uint32_t ip, x86state; uint8_t hist[256]; uint32_t dist, hpos; } Flt;

static uint32_t x86_convert(uint8_t *data, uint32_t size, uint32_t ip, uint32_t *state)
{
    static const uint8_t allowed[8] = { 1, 1, 1, 0, 1, 0, 0, 0 }, bitnum[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
#define MSB(b) ((b) == 0 || (b) == 0xFF)
    uint32_t pos = 0, prevPosT; uint32_t prevMask = *state & 7;
    if (size < 5) return 0;
    ip += 5; prevPosT = (uint32_t)0 - 1;
    for (;;) {
        uint8_t *p = data + pos, *limit = data + size - 4;
        for (; p < limit; p++) if ((*p & 0xFE) == 0xE8) break;
        pos = (uint32_t)(p - data);
        if (p >= limit) break;
        prevPosT = pos - prevPosT;
        if (prevPosT > 3) prevMask = 0;
        else {
            prevMask = (prevMask << ((int)prevPosT - 1)) & 7;
            if (prevMask != 0) {
                uint8_t b = p[4 - bitnum[prevMask]];
                if (!allowed[prevMask] || MSB(b)) { prevPosT = pos; prevMask = ((prevMask << 1) & 7) | 1; pos++; continue; }
            }
        }
        prevPosT = pos;
        if (MSB(p[4])) {
            uint32_t src = ((uint32_t)p[4] << 24) | ((uint32_t)p[3] << 16) | ((uint32_t)p[2] << 8) | p[1], dest;
            for (;;) {
                dest = src - (ip + pos);
                if (prevMask == 0) break;
                int index = bitnum[prevMask] * 8;
                uint8_t b = (uint8_t)(dest >> (24 - index));
                if (!MSB(b)) break;
                src = dest ^ ((1u << (32 - index)) - 1);
            }
            p[4] = (uint8_t)(~(((dest >> 24) & 1) - 1)); p[3] = (uint8_t)(dest >> 16); p[2] = (uint8_t)(dest >> 8); p[1] = (uint8_t)dest;
            pos += 5;
        } else { prevMask = ((prevMask << 1) & 7) | 1; pos++; }
    }
    prevPosT = pos - prevPosT;
    *state = prevPosT > 3 ? 0 : ((prevMask << ((int)prevPosT - 1)) & 7);
    return pos;
#undef MSB
}

static uint32_t branch_convert(FKindF k, uint8_t *d, uint32_t size, uint32_t ip)
{
    uint32_t i;
    if (k == F_ARM) {
        for (i = 0; i + 4 <= size; i += 4) if (d[i + 3] == 0xEB) {
            uint32_t s = ((uint32_t)d[i + 2] << 16) | ((uint32_t)d[i + 1] << 8) | d[i];
            s <<= 2; uint32_t t = s - (ip + i + 8); t >>= 2;
            d[i + 2] = (uint8_t)(t >> 16); d[i + 1] = (uint8_t)(t >> 8); d[i] = (uint8_t)t;
        }
        return i;
    }
    if (k == F_ARMT) {
        for (i = 0; i + 4 <= size; i += 2) if ((d[i + 1] & 0xF8) == 0xF0 && (d[i + 3] & 0xF8) == 0xF8) {
            uint32_t s = (((uint32_t)d[i + 1] & 7) << 19) | ((uint32_t)d[i] << 11) | (((uint32_t)d[i + 3] & 7) << 8) | d[i + 2];
            s <<= 1; uint32_t t = s - (ip + i + 4); t >>= 1;
            d[i + 1] = (uint8_t)(0xF0 | ((t >> 19) & 7)); d[i] = (uint8_t)(t >> 11);
            d[i + 3] = (uint8_t)(0xF8 | ((t >> 8) & 7)); d[i + 2] = (uint8_t)t;
            i += 2;
        }
        return i;
    }
    if (k == F_PPC) {
        for (i = 0; i + 4 <= size; i += 4) if ((d[i] >> 2) == 0x12 && (d[i + 3] & 3) == 1) {
            uint32_t s = ((uint32_t)(d[i] & 3) << 24) | ((uint32_t)d[i + 1] << 16) | ((uint32_t)d[i + 2] << 8) | (d[i + 3] & ~3u);
            uint32_t t = s - (ip + i);
            d[i] = (uint8_t)(0x48 | ((t >> 24) & 3)); d[i + 1] = (uint8_t)(t >> 16); d[i + 2] = (uint8_t)(t >> 8);
            d[i + 3] &= 3; d[i + 3] |= (uint8_t)t;
        }
        return i;
    }
    /* SPARC */
    for (i = 0; i + 4 <= size; i += 4) if ((d[i] == 0x40 && (d[i + 1] & 0xC0) == 0) || (d[i] == 0x7F && (d[i + 1] & 0xC0) == 0xC0)) {
        uint32_t s = ((uint32_t)d[i] << 24) | ((uint32_t)d[i + 1] << 16) | ((uint32_t)d[i + 2] << 8) | d[i + 3];
        s <<= 2; uint32_t t = s - (ip + i); t >>= 2;
        t = (((0 - ((t >> 22) & 1)) << 22) & 0x3FFFFFFF) | (t & 0x3FFFFF) | 0x40000000;
        d[i] = (uint8_t)(t >> 24); d[i + 1] = (uint8_t)(t >> 16); d[i + 2] = (uint8_t)(t >> 8); d[i + 3] = (uint8_t)t;
    }
    return i;
}

static void flt_fin(St *s) { free(((Flt *)s)->buf); }
static int flt_rd(St *s, uint8_t *o, int n)
{
    Flt *f = (Flt *)s;
    if (f->k == F_DELTA) {
        int r = s->src->rd(s->src, o, n);
        if (r <= 0) return r;
        for (int i = 0; i < r; i++) { uint8_t v = (uint8_t)(o[i] + f->hist[(f->hpos - f->dist) & 255]); f->hist[f->hpos & 255] = v; f->hpos++; o[i] = v; }
        return r;
    }
    for (;;) {
        if (f->pos < f->conv) {
            uint32_t k = f->conv - f->pos; if (k > (uint32_t)n) k = (uint32_t)n;
            memcpy(o, f->buf + f->pos, k); f->pos += k;
            return (int)k;
        }
        /* keep the unconverted tail, read more */
        uint32_t tail = f->filled - f->conv;
        if (tail) memmove(f->buf, f->buf + f->conv, tail);
        f->ip += f->conv; f->filled = tail; f->pos = f->conv = 0;
        while (!f->eof && f->filled < f->cap) {
            int r = s->src->rd(s->src, f->buf + f->filled, (int)(f->cap - f->filled));
            if (r < 0) return -1;
            if (r == 0) f->eof = 1; else f->filled += (uint32_t)r;
        }
        if (f->filled == 0) return 0;
        uint32_t done = f->k == F_X86 ? x86_convert(f->buf, f->filled, f->ip, &f->x86state) : branch_convert(f->k, f->buf, f->filled, f->ip);
        f->conv = f->eof ? f->filled : done;                    /* at the end the last few bytes pass through unchanged */
        if (f->conv == 0 && !f->eof) return -1;                 /* cannot happen: buffer is much larger than the look-ahead */
    }
}

static St *make_filter(FKindF k, St *src, const Coder *cd)
{
    Flt *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->k = k; f->st.rd = flt_rd; f->st.fin = flt_fin; f->st.src = src;
    if (k == F_DELTA) f->dist = (cd->nprops >= 1 ? cd->props[0] : 0) + 1u;
    else { f->cap = 16384; f->buf = malloc(f->cap); if (!f->buf) { free(f); return NULL; } }
    return &f->st;
}

/* --- Deflate (zlib raw inflate) */
#ifdef HAVE_ZLIB
typedef struct { St st; z_stream z; uint8_t *in; int init, end; } Dfl;
static void dfl_fin(St *s) { Dfl *d = (Dfl *)s; if (d->init) inflateEnd(&d->z); free(d->in); }
static int dfl_rd(St *s, uint8_t *o, int n)
{
    Dfl *d = (Dfl *)s;
    if (d->end) return 0;
    d->z.next_out = o; d->z.avail_out = (uInt)n;
    while (d->z.avail_out == (uInt)n) {
        if (d->z.avail_in == 0) {
            int r = s->src->rd(s->src, d->in, IBUF);
            if (r < 0) return -1;
            if (r == 0) return -1;                              /* stream ended without the final block */
            d->z.next_in = d->in; d->z.avail_in = (uInt)r;
        }
        int zr = inflate(&d->z, Z_NO_FLUSH);
        if (zr == Z_STREAM_END) { d->end = 1; break; }
        if (zr != Z_OK && zr != Z_BUF_ERROR) return -1;
    }
    return n - (int)d->z.avail_out;
}
static St *make_deflate(St *src)
{
    Dfl *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->st.rd = dfl_rd; d->st.fin = dfl_fin; d->st.src = src;
    d->in = malloc(IBUF);
    if (!d->in || inflateInit2(&d->z, -15) != Z_OK) { free(d->in); free(d); return NULL; }
    d->init = 1;
    return &d->st;
}
#endif

/* --- LZMA / LZMA2 */
typedef struct {
    St st;
    int is2;
    uint8_t *ibuf; uint32_t ip, il; int in_err; uint64_t consumed;
    uint8_t *dict; uint32_t dsize, dpos; uint64_t processed;     /* processed: bytes since the last dictionary reset */
    uint16_t *probs; uint32_t nprobs_alloc;
    uint32_t lc, lp, pb;
    uint32_t state, rep0, rep1, rep2, rep3;
    uint32_t range, code;
    uint32_t rem_len;                                             /* bytes of a match still to copy */
    uint64_t remain;                                              /* output still wanted from this stage */
    /* lzma2 */
    int need_dict, have_props, chunk_raw; uint64_t chunk_unp, chunk_pack;
} Lz;

/* probability array layout (offsets into probs[]) */
enum { P_ISMATCH = 0, P_ISREP = P_ISMATCH + (12 << 4), P_G0 = P_ISREP + 12, P_G1 = P_G0 + 12, P_G2 = P_G1 + 12, P_REP0LONG = P_G2 + 12,
       P_SLOT = P_REP0LONG + (12 << 4), P_SPEC = P_SLOT + 4 * 64, P_ALIGN = P_SPEC + 115,
       P_LEN = P_ALIGN + 16,                                       /* len decoder: choice, choice2, low[16][8], mid[16][8], high[256] = 2 + 128 + 128 + 256 */
       P_REPLEN = P_LEN + 514, P_LIT = P_REPLEN + 514 };

static int lz_in(Lz *L)
{
    if (L->ip == L->il) {
        int r = L->st.src->rd(L->st.src, L->ibuf, IBUF);
        if (r <= 0) { L->in_err = 1; return 0; }
        L->ip = 0; L->il = (uint32_t)r;
    }
    L->consumed++;
    return L->ibuf[L->ip++];
}

static int lz_probs(Lz *L)
{
    uint32_t n = P_LIT + (0x300u << (L->lc + L->lp));
    if (n > L->nprobs_alloc) {
        free(L->probs); L->probs = malloc(n * sizeof(uint16_t)); L->nprobs_alloc = L->probs ? n : 0;
        if (!L->probs) return -1;
    }
    return 0;
}
static void lz_state_reset(Lz *L)
{
    uint32_t n = P_LIT + (0x300u << (L->lc + L->lp));
    for (uint32_t i = 0; i < n; i++) L->probs[i] = 1024;
    L->state = 0; L->rep0 = L->rep1 = L->rep2 = L->rep3 = 0; L->rem_len = 0;
}
static int lz_rc_init(Lz *L)
{
    if (lz_in(L) != 0) return -1;
    uint32_t c = 0; for (int i = 0; i < 4; i++) c = (c << 8) | (uint32_t)lz_in(L);
    L->code = c; L->range = 0xFFFFFFFFu;
    return L->in_err ? -1 : 0;
}

static inline void lz_put(Lz *L, uint8_t b, uint8_t *o)
{
    L->dict[L->dpos] = b; if (++L->dpos == L->dsize) L->dpos = 0;
    L->processed++; L->remain--; if (L->is2) L->chunk_unp--;
    *o = b;
}

#define NORM() do { if (range < (1u << 24)) { range <<= 8; code = (code << 8) | (uint32_t)lz_in(L); } } while (0)
#define BIT(pp, out) do { uint16_t *p_ = (pp); uint32_t v_ = *p_, bd_ = (range >> 11) * v_; \
    if (code < bd_) { range = bd_; *p_ = (uint16_t)(v_ + ((2048 - v_) >> 5)); out = 0; } \
    else { range -= bd_; code -= bd_; *p_ = (uint16_t)(v_ - (v_ >> 5)); out = 1; } NORM(); } while (0)
#define TREE(base, nb, res) do { uint32_t m_ = 1; for (uint32_t i_ = 0; i_ < (nb); i_++) { uint32_t b_; BIT(&probs[(base) + m_], b_); m_ = (m_ << 1) | b_; } res = m_ - (1u << (nb)); } while (0)
#define RTREE(base, nb, res) do { uint32_t m_ = 1; res = 0; for (uint32_t i_ = 0; i_ < (nb); i_++) { uint32_t b_; BIT(&probs[(base) + m_], b_); m_ = (m_ << 1) | b_; res |= b_ << i_; } } while (0)

/* decode one literal or the header of one match into L (rem_len / rep0 set). Returns 1 literal (byte in *lit), 2 match started, 0 end marker, -1 error */
static int lz_symbol(Lz *L, uint8_t *lit)
{
    uint16_t *probs = L->probs;
    uint32_t range = L->range, code = L->code, state = L->state;
    uint32_t pmask = (1u << L->pb) - 1, posState = (uint32_t)L->processed & pmask, b;
    int res;
    BIT(&probs[P_ISMATCH + (state << 4) + posState], b);
    if (b == 0) {
        uint32_t prev = L->processed ? L->dict[L->dpos ? L->dpos - 1 : L->dsize - 1] : 0;
        uint32_t ls = (((uint32_t)L->processed & ((1u << L->lp) - 1)) << L->lc) + (prev >> (8 - L->lc));
        uint32_t base = P_LIT + 0x300u * ls, sym = 1;
        if (state >= 7) {
            uint32_t dist = L->rep0 + 1; if (dist > L->dsize || dist > L->processed) return -1;
            uint32_t mb = L->dict[L->dpos >= dist ? L->dpos - dist : L->dpos + L->dsize - dist];
            do {
                uint32_t mbit = (mb >> 7) & 1; mb <<= 1;
                BIT(&probs[base + ((1 + mbit) << 8) + sym], b);
                sym = (sym << 1) | b;
                if (mbit != b) break;
            } while (sym < 0x100);
        }
        while (sym < 0x100) { BIT(&probs[base + sym], b); sym = (sym << 1) | b; }
        *lit = (uint8_t)sym;
        L->state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
        res = 1;
    } else {
        uint32_t lenbase, len;
        BIT(&probs[P_ISREP + state], b);
        if (b) {
            if (L->processed == 0) return -1;
            BIT(&probs[P_G0 + state], b);
            if (b == 0) {
                BIT(&probs[P_REP0LONG + (state << 4) + posState], b);
                if (b == 0) {                                      /* short rep: one byte from rep0 */
                    uint32_t dist = L->rep0 + 1; if (dist > L->dsize || dist > L->processed) return -1;
                    *lit = L->dict[L->dpos >= dist ? L->dpos - dist : L->dpos + L->dsize - dist];
                    L->state = state < 7 ? 9 : 11;
                    L->range = range; L->code = code;
                    return 1;
                }
            } else {
                uint32_t dist;
                BIT(&probs[P_G1 + state], b);
                if (b == 0) dist = L->rep1;
                else { BIT(&probs[P_G2 + state], b); if (b == 0) dist = L->rep2; else { dist = L->rep3; L->rep3 = L->rep2; } L->rep2 = L->rep1; }
                L->rep1 = L->rep0; L->rep0 = dist;
            }
            lenbase = P_REPLEN; state = state < 7 ? 8 : 11;
        } else {
            L->rep3 = L->rep2; L->rep2 = L->rep1; L->rep1 = L->rep0;
            lenbase = P_LEN; state = state < 7 ? 7 : 10;
        }
        /* length */
        BIT(&probs[lenbase], b);
        if (b == 0) { uint32_t r; TREE(lenbase + 2 + posState * 8 - 1 + 0, 3, r); len = r; }
        else {
            BIT(&probs[lenbase + 1], b);
            if (b == 0) { uint32_t r; TREE(lenbase + 2 + 128 + posState * 8 - 1 + 0, 3, r); len = 8 + r; }
            else { uint32_t r; TREE(lenbase + 2 + 256 - 1 + 0, 8, r); len = 16 + r; }
        }
        if (lenbase == P_LEN) {                                    /* plain match: decode the distance */
            uint32_t ls = len < 3 ? len : 3, slot, dist;
            TREE(P_SLOT + ls * 64 - 1 + 0, 6, slot);
            if (slot < 4) dist = slot;
            else {
                uint32_t nd = (slot >> 1) - 1; dist = (2 | (slot & 1)) << nd;
                if (slot < 14) { uint32_t r; RTREE(P_SPEC + dist - slot, nd, r); dist += r; }
                else {
                    uint32_t d = 0;
                    for (uint32_t i = 0; i < nd - 4; i++) {
                        range >>= 1; code -= range; uint32_t t = 0u - (code >> 31); code += range & t;
                        if (code == range) return -1;
                        NORM(); d = (d << 1) + (t + 1);
                    }
                    dist += d << 4;
                    uint32_t r; RTREE(P_ALIGN - 1 + 0, 4, r); dist += r;
                }
            }
            L->rep0 = dist;
            if (dist == 0xFFFFFFFFu) { L->range = range; L->code = code; L->state = state; return 0; }   /* end marker */
        }
        L->state = state;
        uint32_t dist = L->rep0 + 1;
        if (dist > L->dsize || dist > L->processed) return -1;
        L->rem_len = len + 2;
        res = 2;
    }
    L->range = range; L->code = code;
    return res;
}

/* LZMA2 chunk header. 1 = chunk ready, 0 = end of stream, -1 = error */
static int lz2_header(Lz *L)
{
    int c = lz_in(L);
    if (L->in_err) return -1;
    if (c == 0) return 0;
    if (c == 1 || c == 2) {
        if (c == 1) { L->processed = 0; L->dpos = 0; L->need_dict = 0; } else if (L->need_dict) return -1;
        uint32_t hi = (uint32_t)lz_in(L); uint32_t lo = (uint32_t)lz_in(L);
        L->chunk_unp = ((hi << 8) | lo) + 1; L->chunk_raw = 1;
        return L->in_err ? -1 : 1;
    }
    if (c < 0x80) return -1;
    uint64_t unp = ((uint64_t)(c & 0x1F) << 16);
    uint32_t b1 = (uint32_t)lz_in(L), b2 = (uint32_t)lz_in(L), b3 = (uint32_t)lz_in(L), b4 = (uint32_t)lz_in(L);
    unp += (b1 << 8) + b2 + 1;
    uint64_t pack = ((b3 << 8) | b4) + 1;
    int mode = (c >> 5) & 3;
    if (mode == 3) { L->processed = 0; L->dpos = 0; L->need_dict = 0; } else if (L->need_dict) return -1;
    if (mode >= 2) {
        uint32_t d = (uint32_t)lz_in(L);
        if (d >= 9 * 5 * 5) return -1;
        L->lc = d % 9; d /= 9; L->lp = d % 5; L->pb = d / 5;
        if (L->lc + L->lp > 4) return -1;
        if (lz_probs(L)) return -1;
        L->have_props = 1;
    } else if (!L->have_props) return -1;
    if (mode >= 1) lz_state_reset(L);
    if (L->in_err) return -1;
    L->consumed = 0;
    if (lz_rc_init(L)) return -1;
    L->chunk_unp = unp; L->chunk_pack = pack; L->chunk_raw = 0;
    return 1;
}

static int lz_rd(St *s, uint8_t *out, int n)
{
    Lz *L = (Lz *)s;
    if (L->remain == 0) return 0;
    if ((uint64_t)n > L->remain) n = (int)L->remain;
    int got = 0;
    while (got < n) {
        if (L->rem_len) {                                          /* continue a match */
            uint32_t dist = L->rep0 + 1;
            while (L->rem_len && got < n && (!L->is2 || L->chunk_unp) && L->remain) {
                uint8_t b = L->dict[L->dpos >= dist ? L->dpos - dist : L->dpos + L->dsize - dist];
                lz_put(L, b, out + got); got++; L->rem_len--;
            }
            if (L->rem_len && L->is2 && L->chunk_unp == 0) return -1;      /* match crosses a chunk end */
            continue;
        }
        if (L->is2 && L->chunk_unp == 0) {
            if (!L->chunk_raw && L->chunk_pack) {                  /* finished an LZMA chunk: input must be used up exactly */
                if (L->consumed > L->chunk_pack) return -1;
                while (L->consumed < L->chunk_pack) { lz_in(L); if (L->in_err) return -1; }
                L->chunk_pack = 0;
            }
            int h = lz2_header(L);
            if (h <= 0) return got > 0 ? got : -1;                 /* stream ended before the expected size */
            continue;
        }
        if (L->is2 && L->chunk_raw) {                               /* uncompressed chunk: straight copy */
            while (got < n && L->chunk_unp) { uint8_t b = (uint8_t)lz_in(L); if (L->in_err) return -1; lz_put(L, b, out + got); got++; }
            continue;
        }
        uint8_t lit;
        int r = lz_symbol(L, &lit);
        if (L->in_err || r <= 0) return -1;
        if (r == 1) { lz_put(L, lit, out + got); got++; }
    }
    return got;
}

static void lz_fin(St *s) { Lz *L = (Lz *)s; free(L->ibuf); free(L->dict); free(L->probs); }

static St *make_lzma(St *src, const Coder *cd, uint64_t unp, int is2)
{
    uint64_t dsz;
    if (is2) { if (cd->nprops < 1 || cd->props[0] > 40) return NULL; uint32_t p = cd->props[0]; dsz = p == 40 ? 0xFFFFFFFFull : ((uint64_t)(2 | (p & 1)) << (p / 2 + 11)); }
    else { if (cd->nprops < 5) return NULL; dsz = cd->props[1] | (cd->props[2] << 8) | (cd->props[3] << 16) | ((uint32_t)cd->props[4] << 24); }
    if (dsz > unp) dsz = unp;
    if (dsz < 4096) dsz = 4096;
    Lz *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->st.rd = lz_rd; L->st.fin = lz_fin; L->st.src = src;
    L->is2 = is2; L->remain = unp; L->dsize = (uint32_t)dsz; L->need_dict = is2;
    L->ibuf = malloc(IBUF); L->dict = malloc(L->dsize);
    if (!L->ibuf || !L->dict) { lz_fin(&L->st); free(L); return NULL; }
    if (is2) { L->lc = 3; L->lp = 0; L->pb = 2; if (lz_probs(L)) { lz_fin(&L->st); free(L); return NULL; } }
    else {
        uint32_t d = cd->props[0]; if (d >= 9 * 5 * 5) { lz_fin(&L->st); free(L); return NULL; }
        L->lc = d % 9; d /= 9; L->lp = d % 5; L->pb = d / 5;
        if (L->lc + L->lp > 12 || lz_probs(L)) { lz_fin(&L->st); free(L); return NULL; }
        lz_state_reset(L);
        if (lz_rc_init(L)) { lz_fin(&L->st); free(L); return NULL; }
    }
    return &L->st;
}

/* ------------------------------------------------------------------ building a folder's decoder chain */
#define ID_COPY 0x00ull
#define ID_DELTA 0x03ull
#define ID_LZMA 0x030101ull
#define ID_LZMA2 0x21ull
#define ID_DEFLATE 0x040108ull

static const char *method_name(uint64_t id)
{
    switch (id) {
    case 0x030401: return "PPMd"; case 0x040202: return "BZip2"; case 0x040109: return "Deflate64"; case 0x06F10701: return "AES (password)";
    case 0x0303011B: return "BCJ2"; case 0x0A: return "ARM64"; case 0x0B: return "RISCV"; case 0x04F71101: return "Zstandard"; default: return "unknown method";
    }
}

static St *build_coder(const SzIO *io, const Folder *f, uint32_t coder, uint64_t pack_off, const uint64_t *pack_size, const uint64_t *unp_of_coder, char *msg, int mlen, int depth)
{
    if (depth > 8) return NULL;
    const Coder *cd = &f->coders[coder];
    if (cd->nin != 1 || cd->nout != 1) { snprintf(msg, mlen, "unsupported: %s", method_name(cd->id)); return NULL; }
    uint32_t inbase = 0, outbase = 0;
    for (uint32_t k = 0; k < coder; k++) { inbase += f->coders[k].nin; outbase += f->coders[k].nout; }
    (void)outbase;
    St *src = NULL;
    int bound = -1;
    for (uint32_t b = 0; b < f->nbind; b++) if (f->bind_in[b] == inbase) bound = (int)b;
    if (bound >= 0) {
        uint32_t oidx = f->bind_out[bound], acc = 0, owner = 0;
        for (uint32_t k = 0; k < f->ncoders; k++) { if (oidx < acc + f->coders[k].nout) { owner = k; break; } acc += f->coders[k].nout; }
        src = build_coder(io, f, owner, pack_off, pack_size, unp_of_coder, msg, mlen, depth + 1);
        if (!src) return NULL;
    } else {
        uint32_t j = 0; for (; j < f->npacked; j++) if (f->packed[j] == inbase) break;
        if (j >= f->npacked) return NULL;
        uint64_t off = pack_off; for (uint32_t q = 0; q < j; q++) off += pack_size[f->first_pack + q];
        PackSrc *p = calloc(1, sizeof *p);
        if (!p) return NULL;
        p->st.rd = pack_rd; p->io = io; p->rem = pack_size[f->first_pack + j];
        if (io->seek(io->ctx, off) != 0) { free(p); return NULL; }
        src = &p->st;
    }
    uint32_t myout = 0; for (uint32_t k = 0; k < coder; k++) myout += f->coders[k].nout;
    uint64_t unp = f->unp[myout];
    St *r = NULL; uint64_t id = cd->id;
    if (id == ID_COPY) { r = calloc(1, sizeof(St)); if (r) { r->rd = copy_rd; r->src = src; } }
    else if (id == ID_LZMA) r = make_lzma(src, cd, unp, 0);
    else if (id == ID_LZMA2) r = make_lzma(src, cd, unp, 1);
    else if (id == 0x04 || id == 0x03030103ull) r = make_filter(F_X86, src, cd);
    else if (id == 0x05 || id == 0x03030205ull) r = make_filter(F_PPC, src, cd);
    else if (id == 0x07 || id == 0x03030501ull) r = make_filter(F_ARM, src, cd);
    else if (id == 0x08 || id == 0x03030701ull) r = make_filter(F_ARMT, src, cd);
    else if (id == 0x09 || id == 0x03030805ull) r = make_filter(F_SPARC, src, cd);
    else if (id == ID_DELTA) r = make_filter(F_DELTA, src, cd);
#ifdef HAVE_ZLIB
    else if (id == ID_DEFLATE) r = make_deflate(src);
#endif
    else { snprintf(msg, mlen, "unsupported: %s", method_name(id)); st_free(src); return NULL; }
    if (!r) { if (!msg[0]) snprintf(msg, mlen, "out of memory / bad coder properties"); st_free(src); return NULL; }
    (void)unp_of_coder;
    return r;
}

static St *build_folder(const SzIO *io, const Streams *s, uint32_t fi, char *msg, int mlen, int *rc)
{
    const Folder *f = &s->f[fi];
    uint64_t off = 32 + s->pack_pos;
    for (uint32_t i = 0; i < f->first_pack; i++) off += s->pack_size[i];
    /* the coder whose output is not bound is the last one in the chain */
    uint32_t acc = 0, last = 0;
    for (uint32_t k = 0; k < f->ncoders; k++) {
        int bound = 0; for (uint32_t b = 0; b < f->nbind; b++) if (f->bind_out[b] == acc) bound = 1;
        if (!bound) { last = k; break; }
        acc += f->coders[k].nout;
    }
    for (uint32_t k = 0; k < f->ncoders; k++) {
        uint64_t id = f->coders[k].id;
        if (id == 0x06F10701ull) { *rc = -3; snprintf(msg, mlen, "encrypted archive (password) is not supported"); return NULL; }
    }
    msg[0] = 0;
    St *st = build_coder(io, f, last, off, s->pack_size, NULL, msg, mlen, 0);
    if (!st) { *rc = -3; if (!msg[0]) snprintf(msg, mlen, "cannot set up the decoder"); }
    return st;
}

/* ------------------------------------------------------------------ headers */
static int read_exact(const SzIO *io, void *buf, uint64_t n)
{
    uint8_t *p = buf;
    while (n) { int k = n > 65536 ? 65536 : (int)n; int r = io->read(io->ctx, p, k); if (r <= 0) return -1; p += r; n -= (uint64_t)r; }
    return 0;
}

typedef struct { uint8_t *empty, *emptyfile, *attr_def; uint32_t *attr; uint32_t nfiles; const uint8_t *names; uint32_t *name_off; uint32_t nnames_u16; } Files;

static int parse_files(Cur *c, Files *F)
{
    memset(F, 0, sizeof *F);
    uint64_t n = cnum(c); if (n > 0x4000000 || c->bad) return -2;
    F->nfiles = (uint32_t)n;
    uint32_t nempty = 0;
    for (;;) {
        uint64_t t = cnum(c); if (c->bad) return -2;
        if (t == 0) break;
        uint64_t size = cnum(c); if (size > (uint64_t)(c->e - c->p)) return -2;
        Cur sub = { c->p, c->p + size, 0 };
        c->p += size;
        if (t == 0x0E) { F->empty = bitvec(&sub, F->nfiles); if (!F->empty) return -4; for (uint32_t i = 0; i < F->nfiles; i++) nempty += F->empty[i]; }
        else if (t == 0x0F) { F->emptyfile = bitvec(&sub, nempty); if (!F->emptyfile) return -4; }
        else if (t == 0x11) {
            if (c8(&sub) != 0) return -3;
            F->names = sub.p; F->nnames_u16 = (uint32_t)((sub.e - sub.p) / 2);
        } else if (t == 0x15) {
            F->attr_def = bitvec2(&sub, F->nfiles);
            if (!F->attr_def) return -4;
            if (c8(&sub) != 0) { free(F->attr_def); F->attr_def = NULL; continue; }
            F->attr = calloc(F->nfiles ? F->nfiles : 1, 4); if (!F->attr) return -4;
            for (uint32_t i = 0; i < F->nfiles; i++) if (F->attr_def[i]) F->attr[i] = c32(&sub);
        }
    }
    if (!F->names) return -2;
    F->name_off = calloc(F->nfiles + 1, 4);
    if (!F->name_off) return -4;
    uint32_t u = 0;
    for (uint32_t i = 0; i < F->nfiles; i++) {
        F->name_off[i] = u;
        while (u < F->nnames_u16 && (F->names[u * 2] | F->names[u * 2 + 1])) u++;
        if (u >= F->nnames_u16) return -2;
        u++;
    }
    F->name_off[F->nfiles] = u;
    return 0;
}
static void files_free(Files *F) { free(F->empty); free(F->emptyfile); free(F->attr_def); free(F->attr); free(F->name_off); memset(F, 0, sizeof *F); }

/* UTF-16LE name -> safe relative UTF-8 path. 0 = ok, 1 = unsafe / too long (skip). ascii_only replaces non-ASCII with '_' */
static int file_path(const Files *F, uint32_t i, char *out, int cap, int ascii_only)
{
    uint32_t a = F->name_off[i], b = F->name_off[i + 1] - 1; int o = 0;
    for (uint32_t k = a; k < b; k++) {
        uint32_t ch = F->names[k * 2] | (F->names[k * 2 + 1] << 8);
        if (ch >= 0xD800 && ch < 0xDC00 && k + 1 < b) { uint32_t lo = F->names[(k + 1) * 2] | (F->names[(k + 1) * 2 + 1] << 8); if (lo >= 0xDC00 && lo < 0xE000) { ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00); k++; } }
        if (ch == '\\') ch = '/';
        if (ch == ':' || ch < 32) return 1;
        if (ascii_only && ch >= 0x80) ch = '_';
        if (o + 5 >= cap) return 1;
        if (ch < 0x80) out[o++] = (char)ch;
        else if (ch < 0x800) { out[o++] = (char)(0xC0 | (ch >> 6)); out[o++] = (char)(0x80 | (ch & 63)); }
        else if (ch < 0x10000) { out[o++] = (char)(0xE0 | (ch >> 12)); out[o++] = (char)(0x80 | ((ch >> 6) & 63)); out[o++] = (char)(0x80 | (ch & 63)); }
        else { out[o++] = (char)(0xF0 | (ch >> 18)); out[o++] = (char)(0x80 | ((ch >> 12) & 63)); out[o++] = (char)(0x80 | ((ch >> 6) & 63)); out[o++] = (char)(0x80 | (ch & 63)); }
    }
    out[o] = 0;
    char *p = out; while (*p == '/') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
    if (!out[0]) return 1;
    for (const char *q = out; *q;) {                           /* reject ".." components */
        const char *e = strchr(q, '/'); size_t l = e ? (size_t)(e - q) : strlen(q);
        if (l == 2 && q[0] == '.' && q[1] == '.') return 1;
        if (!e) break;
        q = e + 1;
    }
    return 0;
}

static void mk_parents(const SzIO *io, const char *rel, char *last)
{
    char tmp[MAX_PATH_LEN + 8];
    for (const char *s = rel; (s = strchr(s, '/')) != NULL; s++) {
        int n = (int)(s - rel); if (n <= 0 || n >= MAX_PATH_LEN) return;
        memcpy(tmp, rel, (size_t)n); tmp[n] = 0;
        if (!strcmp(tmp, last)) continue;                      /* same parent as the previous file: already made */
        io->mkdir(io->ctx, tmp);
        strcpy(last, tmp);
    }
}

/* decode a whole folder into memory (encoded headers) */
static int decode_to_mem(const SzIO *io, const Streams *s, uint32_t fi, uint8_t **out, uint64_t *len, char *msg, int mlen)
{
    uint64_t u = folder_unpack(&s->f[fi]);
    if (u > MAX_HEADER) return -4;
    int rc = 0;
    St *st = build_folder(io, s, fi, msg, mlen, &rc);
    if (!st) return rc ? rc : -3;
    uint8_t *buf = malloc(u ? u : 1);
    if (!buf) { st_free(st); return -4; }
    uint64_t got = 0;
    while (got < u) { int r = st->rd(st, buf + got, (int)(u - got > 65536 ? 65536 : u - got)); if (r <= 0) break; got += (uint64_t)r; }
    st_free(st);
    if (got != u || (s->f[fi].crc_def && crc32_upd(0, buf, (size_t)u) != s->f[fi].crc)) { free(buf); return -2; }
    *out = buf; *len = u;
    return 0;
}

#define LOG(lv, ...) do { if (io->log) { char b_[160]; snprintf(b_, sizeof b_, __VA_ARGS__); io->log(io->ctx, lv, b_); } } while (0)

int sz_extract(const SzIO *io, int *n_ok, int *n_bad, char *msg, int msglen)
{
    char lm[160]; lm[0] = 0;
    int ok = 0, bad = 0, rc = 0;
    uint8_t *hdr = NULL; uint64_t hlen = 0;
    Streams S; memset(&S, 0, sizeof S);
    Files F; memset(&F, 0, sizeof F);
    uint8_t *obuf = NULL;
    if (n_ok) *n_ok = 0;
    if (n_bad) *n_bad = 0;
    if (msg && msglen) msg[0] = 0;

    uint8_t sig[32];
    if (io->seek(io->ctx, 0) != 0 || io->read(io->ctx, sig, 32) != 32) return -1;
    if (memcmp(sig, "7z\xBC\xAF\x27\x1C", 6)) { if (msg) snprintf(msg, msglen, "not a 7z file"); return -2; }
    if (crc32_upd(0, sig + 12, 20) != (uint32_t)(sig[8] | (sig[9] << 8) | (sig[10] << 16) | ((uint32_t)sig[11] << 24))) { if (msg) snprintf(msg, msglen, "damaged 7z header"); return -2; }
    uint64_t noff = 0, nsize = 0; uint32_t ncrc = (uint32_t)(sig[28] | (sig[29] << 8) | (sig[30] << 16) | ((uint32_t)sig[31] << 24));
    for (int i = 7; i >= 0; i--) { noff = (noff << 8) | sig[12 + i]; nsize = (nsize << 8) | sig[20 + i]; }
    if (nsize == 0) return 0;                                  /* empty archive */
    if (nsize > MAX_HEADER) { if (msg) snprintf(msg, msglen, "header too large"); return -4; }
    unsigned long long total_size = io->size ? io->size(io->ctx) : 0;
    if (total_size && 32 + noff + nsize > total_size) { if (msg) snprintf(msg, msglen, "archive is truncated"); return -2; }
    hdr = malloc((size_t)nsize);
    if (!hdr) return -4;
    if (io->seek(io->ctx, 32 + noff) != 0 || read_exact(io, hdr, nsize)) { free(hdr); return -1; }
    hlen = nsize;
    if (crc32_upd(0, hdr, (size_t)hlen) != ncrc) { free(hdr); if (msg) snprintf(msg, msglen, "damaged 7z header (CRC)"); return -2; }

    /* encoded (compressed) header: decode, maybe more than once */
    for (int depth = 0; depth < 4; depth++) {
        Cur c = { hdr, hdr + hlen, 0 };
        uint64_t id = cnum(&c);
        if (id == 0x17) {
            rc = parse_streams(&c, &S);
            if (rc == 0 && S.nfold >= 1) {
                uint8_t *nb = NULL; uint64_t nl = 0;
                rc = decode_to_mem(io, &S, 0, &nb, &nl, lm, sizeof lm);
                if (rc == 0) { free(hdr); hdr = nb; hlen = nl; }
            } else if (rc == 0) rc = -2;
            streams_free(&S);
            if (rc) { free(hdr); if (msg) snprintf(msg, msglen, "%s", lm[0] ? lm : "cannot read the archive index"); return rc; }
            continue;
        }
        if (id != 1) { free(hdr); if (msg) snprintf(msg, msglen, "unknown header"); return -2; }
        break;
    }

    Cur c = { hdr, hdr + hlen, 0 };
    if (cnum(&c) != 1) { free(hdr); return -2; }
    int have_files = 0;
    for (;;) {
        uint64_t id = cnum(&c);
        if (c.bad) { rc = -2; goto done; }
        if (id == 0) break;
        if (id == 2) { for (;;) { uint64_t t = cnum(&c); if (t == 0 || c.bad) break; cskip(&c, cnum(&c)); } }
        else if (id == 3) { rc = -3; snprintf(lm, sizeof lm, "unsupported header layout"); goto done; }
        else if (id == 4) { streams_free(&S); rc = parse_streams(&c, &S); if (rc) goto done; }
        else if (id == 5) { rc = parse_files(&c, &F); if (rc) goto done; have_files = 1; }
        else { rc = -2; goto done; }
    }
    if (!have_files) goto done;

    /* classify entries */
    uint32_t nf = F.nfiles, ei = 0;
    uint8_t *isdir = calloc(nf ? nf : 1, 1), *hasstream = calloc(nf ? nf : 1, 1), *skipf = calloc(nf ? nf : 1, 1);
    if (!isdir || !hasstream || !skipf) { free(isdir); free(hasstream); free(skipf); rc = -4; goto done; }
    uint64_t total = 0;
    for (uint32_t i = 0; i < nf; i++) {
        int e = F.empty ? F.empty[i] : 0;
        if (e) { int efile = F.emptyfile && F.emptyfile[ei]; ei++; isdir[i] = !efile; } else hasstream[i] = 1;
        if (F.attr && F.attr_def && F.attr_def[i]) {
            uint32_t a = F.attr[i];
            if (a & 0x10) isdir[i] = 1;
            if ((a & 0x8000) && ((a >> 16) & 0xF000) == 0xA000) skipf[i] = 1;             /* symbolic link: not extracted */
        }
    }
    for (uint32_t k = 0; k < S.nsub_total; k++) total += S.sub_size[k];

    char path[MAX_PATH_LEN + 8], lastdir[MAX_PATH_LEN + 8]; lastdir[0] = 0;
    /* folders and empty files first */
    for (uint32_t i = 0; i < nf; i++) {
        if (!isdir[i] && hasstream[i]) continue;
        if (file_path(&F, i, path, MAX_PATH_LEN, 0)) { bad++; LOG(1, "7z: skipped an unsafe name (entry %u)", (unsigned)i); continue; }
        if (skipf[i]) continue;
        if (isdir[i]) { mk_parents(io, path, lastdir); io->mkdir(io->ctx, path); ok++; continue; }
        mk_parents(io, path, lastdir);
        void *h = io->create(io->ctx, path);
        if (!h && !file_path(&F, i, path, MAX_PATH_LEN, 1)) h = io->create(io->ctx, path);
        if (!h) { bad++; LOG(2, "7z: cannot create %s", path); continue; }
        io->close(io->ctx, h); ok++;
    }

    obuf = malloc(OBUF);
    if (!obuf) { free(isdir); free(hasstream); free(skipf); rc = -4; goto done; }
    uint32_t fidx = 0, sidx = 0;                              /* next file with a stream / next substream */
    uint64_t done_b = 0;
    for (uint32_t fi = 0; fi < S.nfold; fi++) {
        const Folder *f = &S.f[fi];
        uint32_t list[1]; (void)list;
        uint32_t first_sub = sidx, first_file = fidx;
        /* find the files of this folder */
        uint32_t nsub = f->nsub, cnt = 0;
        uint32_t *files = malloc((nsub ? nsub : 1) * 4);
        if (!files) { rc = -4; break; }
        while (cnt < nsub && fidx < nf) { if (hasstream[fidx]) files[cnt++] = fidx; fidx++; }
        (void)first_file;
        sidx += nsub;
        if (nsub == 0) { free(files); continue; }
        int frc = 0; lm[0] = 0;
        St *st = build_folder(io, &S, fi, lm, sizeof lm, &frc);
        if (!st) {
            bad += (int)cnt; LOG(2, "7z: folder %u: %s", (unsigned)fi, lm);
            if (msg && !msg[0]) snprintf(msg, msglen, "%s", lm);
            free(files); continue;
        }
        int broken = 0;
        for (uint32_t j = 0; j < cnt; j++) {
            uint32_t fi_file = files[j]; uint64_t size = S.sub_size[first_sub + j];
            int bad_name = file_path(&F, fi_file, path, MAX_PATH_LEN, 0);
            void *h = NULL;
            if (broken) { bad++; continue; }
            if (!bad_name && !skipf[fi_file]) {
                mk_parents(io, path, lastdir);
                h = io->create(io->ctx, path);
                if (!h && !file_path(&F, fi_file, path, MAX_PATH_LEN, 1)) h = io->create(io->ctx, path);
                if (!h) LOG(2, "7z: cannot create %s", path);
            } else if (bad_name) LOG(1, "7z: skipped an unsafe name");
            uint32_t crc = 0; uint64_t left = size; int fail = 0;
            while (left) {
                int want = left > OBUF ? OBUF : (int)left;
                int r = st->rd(st, obuf, want);
                if (r <= 0) { fail = 1; broken = 1; LOG(2, "7z: data error in %s", path); break; }
                crc = crc32_upd(crc, obuf, (size_t)r);
                if (h && io->write(io->ctx, h, obuf, r) != r) { fail = 1; LOG(2, "7z: write error (disk full?)"); h = (io->close(io->ctx, h), NULL); }
                left -= (uint64_t)r; done_b += (uint64_t)r;
                if (io->progress && io->progress(io->ctx, path, done_b, total ? total : 1)) { if (h) io->close(io->ctx, h); st_free(st); free(files); free(isdir); free(hasstream); free(skipf); rc = -9; goto done; }
            }
            if (h) io->close(io->ctx, h);
            if (!fail && S.sub_crc_def[first_sub + j] && crc != S.sub_crc[first_sub + j]) { fail = 1; LOG(2, "7z: CRC mismatch in %s", path); }
            if (skipf[fi_file]) continue;
            if (fail || !h || bad_name) bad++; else ok++;
        }
        st_free(st); free(files);
    }
    free(isdir); free(hasstream); free(skipf);
done:
    free(obuf); free(hdr);
    streams_free(&S); files_free(&F);
    if (n_ok) *n_ok = ok;
    if (n_bad) *n_bad = bad;
    if (msg && msglen && !msg[0] && lm[0]) snprintf(msg, msglen, "%s", lm);
    return rc;
}
