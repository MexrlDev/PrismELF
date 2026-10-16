/* .zip and .7z writers. See compress.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "compress.h"
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#define IOBUF 32768
#define MAX_REL 240
#define MAX_ITEMS 60000

/* ------------------------------------------------------------------ helpers */
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

typedef struct { char *rel; uint64_t size; int dir; int ok; uint32_t crc; uint64_t csize; uint32_t lho; int method; } Item;
typedef struct { Item *v; int n, cap; } Items;

static int items_add(Items *it, const char *rel, uint64_t size, int dir)
{
    if (it->n >= MAX_ITEMS) return -5;
    if (it->n == it->cap) {
        int nc = it->cap ? it->cap * 2 : 256;
        Item *nv = realloc(it->v, (size_t)nc * sizeof(Item));
        if (!nv) return -4;
        it->v = nv; it->cap = nc;
    }
    Item *x = &it->v[it->n];
    memset(x, 0, sizeof *x);
    x->rel = malloc(strlen(rel) + 1);
    if (!x->rel) return -4;
    strcpy(x->rel, rel);
    x->size = size; x->dir = dir; x->ok = 1;
    it->n++;
    return 0;
}
static void items_free(Items *it) { for (int i = 0; i < it->n; i++) free(it->v[i].rel); free(it->v); memset(it, 0, sizeof *it); }

/* all entries below the selected names, breadth first (one directory listing buffer is enough) */
static int scan(Items *it, const char *base, const char *const *names, int n, uint64_t *total, char *msg, int mlen)
{
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX_LEN];
        fs_join(full, base, names[i]);
        int d = fs_is_dir(full);
        int sz = d ? 0 : fs_size(full);
        int rc = items_add(it, names[i], sz < 0 ? 0 : (uint64_t)sz, d);
        if (rc) { snprintf(msg, mlen, rc == -5 ? "too many files" : "out of memory"); return rc; }
        if (!d && sz > 0) *total += (uint64_t)sz;
    }
    Entry *ents = malloc(sizeof(Entry) * MAX_ENTRIES);
    if (!ents) { snprintf(msg, mlen, "out of memory"); return -4; }
    for (int i = 0; i < it->n; i++) {
        if (!it->v[i].dir) continue;
        char full[PATH_MAX_LEN], dir[MAX_REL + 8];
        snprintf(dir, sizeof dir, "%s", it->v[i].rel);
        fs_join(full, base, dir);
        int c = fs_list(full, ents, MAX_ENTRIES, 1);
        for (int k = 0; k < c; k++) {
            if (!strcmp(ents[k].name, "..") || !strcmp(ents[k].name, ".")) continue;
            char rel[MAX_REL + 8];
            if ((int)strlen(dir) + 1 + (int)strlen(ents[k].name) >= MAX_REL) continue;      /* path too long for the archive: left out */
            snprintf(rel, sizeof rel, "%s/%s", dir, ents[k].name);
            int rc = items_add(it, rel, ents[k].size, ents[k].is_dir);
            if (rc) { free(ents); snprintf(msg, mlen, rc == -5 ? "too many files" : "out of memory"); return rc; }
            if (!ents[k].is_dir) *total += ents[k].size;
        }
    }
    free(ents);
    return 0;
}

static int has_ext(const char *name, const char *list)       /* list: " zip 7z ..." */
{
    const char *d = strrchr(name, '.'), *s = strrchr(name, '/');
    if (!d || (s && d < s) || strlen(d) > 6) return 0;
    char key[12]; snprintf(key, sizeof key, " %s ", d + 1);
    for (char *c = key; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    return strstr(list, key) != NULL;
}
#define PACKED_EXT " zip 7z rar gz bz2 xz png jpg jpeg gif webp mp3 mp4 m4a ogg flac aac mkv avi mov wma "

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* output file with a write counter */
typedef struct { FsFile *f; uint64_t pos; int err; } Out;
static void out_w(Out *o, const void *b, int n)
{
    if (o->err || n <= 0) return;
    if (fs_fwrite(o->f, b, n) != n) o->err = 1;
    o->pos += (uint64_t)n;
}
static void out_seek(Out *o, uint64_t p) { if (o->err) return; if (fs_fseek(o->f, (int)p, 0) < 0) o->err = 1; }

/* ------------------------------------------------------------------ ZIP */
static void utf8_ok(const char *s, uint32_t *flags) { for (; *s; s++) if ((unsigned char)*s >= 0x80) { *flags = 0x0800; return; } }

static int make_zip(Out *o, const char *base, Items *it, uint64_t total, ProgressCb cb, int *n_bad, int *n_ok, char *msg, int mlen)
{
    uint8_t *ib = malloc(IOBUF), *ob = malloc(IOBUF);
    if (!ib || !ob) { free(ib); free(ob); return -4; }
    int rc = 0; uint64_t done = 0;
    if (it->n > 65535) { free(ib); free(ob); snprintf(msg, mlen, "too many files for a zip (max 65535)"); return -5; }
    for (int i = 0; i < it->n && !rc; i++) {
        Item *x = &it->v[i];
        char full[PATH_MAX_LEN]; fs_join(full, base, x->rel);
        size_t nl = strlen(x->rel);
        uint32_t flags = 0; utf8_ok(x->rel, &flags);
        x->lho = (uint32_t)o->pos;
        if (o->pos > 0xFFFF0000ull) { rc = -5; snprintf(msg, mlen, "zip would be over 4 GB"); break; }
        uint8_t lh[30]; memset(lh, 0, sizeof lh);
        le32(lh, 0x04034b50); le16(lh + 4, 20); le16(lh + 6, flags); le16(lh + 10, 0); le16(lh + 12, 23073);
        le16(lh + 26, (uint32_t)nl + (x->dir ? 1 : 0));
        FsFile *src = NULL;
        if (!x->dir) {
            src = fs_fopen(full, 0);
            if (!src) { x->ok = 0; (*n_bad)++; continue; }
        }
        out_w(o, lh, 30); out_w(o, x->rel, (int)nl); if (x->dir) out_w(o, "/", 1);
        if (x->dir) { x->method = 0; (*n_ok)++; continue; }
        int method = (has_ext(x->rel, PACKED_EXT) || x->size < 64) ? 0 : 8;
#ifndef HAVE_ZLIB
        method = 0;
#endif
        x->method = method;
        uint32_t crc = 0; uint64_t usz = 0, csz = 0;
#ifdef HAVE_ZLIB
        z_stream z; int zi = 0;
        if (method == 8) { memset(&z, 0, sizeof z); if (deflateInit2(&z, 5, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) { rc = -4; fs_fclose(src); break; } zi = 1; }
#endif
        int eof = 0, fail = 0;
        while (!eof && !fail && !rc) {
            int r = fs_fread(src, ib, IOBUF);
            if (r < 0) { fail = 1; break; }
            if (r == 0) eof = 1;
            crc = crc32_upd(crc, ib, (size_t)r); usz += (uint64_t)r; done += (uint64_t)r;
            if (usz > 0xFFFF0000ull) { rc = -5; snprintf(msg, mlen, "file over 4 GB"); break; }
#ifdef HAVE_ZLIB
            if (method == 8) {
                z.next_in = ib; z.avail_in = (uInt)r;
                do {
                    z.next_out = ob; z.avail_out = IOBUF;
                    int zr = deflate(&z, eof ? Z_FINISH : Z_NO_FLUSH);
                    if (zr == Z_STREAM_ERROR) { fail = 1; break; }
                    int got = IOBUF - (int)z.avail_out;
                    out_w(o, ob, got); csz += (uint64_t)got;
                } while (z.avail_out == 0 && !fail);
            } else
#endif
            { out_w(o, ib, r); csz += (uint64_t)r; }
            if (o->err) { rc = -3; break; }
            if (cb && cb(x->rel, done, total ? total : 1)) rc = -9;
        }
#ifdef HAVE_ZLIB
        if (zi) deflateEnd(&z);
#endif
        fs_fclose(src);
        if (rc) break;
        if (fail) {                                           /* unreadable file: roll the entry back by blanking it (keeps the archive valid) */
            x->ok = 0; (*n_bad)++; continue;
        }
        x->crc = crc; x->size = usz; x->csize = csz;
        /* patch the local header now that crc and sizes are known */
        uint64_t endpos = o->pos;
        uint8_t fix[12]; le32(fix, crc); le32(fix + 4, (uint32_t)csz); le32(fix + 8, (uint32_t)usz);
        le16(lh + 8, (uint32_t)method);
        out_seek(o, x->lho + 8); out_w(o, lh + 8, 2);
        out_seek(o, x->lho + 14); out_w(o, fix, 12);
        o->pos = endpos; out_seek(o, endpos);
        if (o->err) { rc = -3; break; }
        (*n_ok)++;
    }
    if (!rc) {
        /* central directory */
        uint64_t cd0 = o->pos; int cnt = 0;
        for (int i = 0; i < it->n && !o->err; i++) {
            Item *x = &it->v[i];
            if (!x->ok) continue;                             /* skipped files were never written, so nothing points at them */
            uint32_t flags = 0; utf8_ok(x->rel, &flags);
            size_t nl = strlen(x->rel);
            uint8_t h[46]; memset(h, 0, sizeof h);
            le32(h, 0x02014b50); le16(h + 4, 20); le16(h + 6, 20); le16(h + 8, flags); le16(h + 10, (uint32_t)x->method); le16(h + 14, 23073);
            le32(h + 16, x->crc); le32(h + 20, (uint32_t)x->csize); le32(h + 24, (uint32_t)x->size);
            le16(h + 28, (uint32_t)nl + (x->dir ? 1 : 0));
            le32(h + 38, x->dir ? 0x10 : 0); le32(h + 42, x->lho);
            out_w(o, h, 46); out_w(o, x->rel, (int)nl); if (x->dir) out_w(o, "/", 1);
            cnt++;
        }
        uint8_t e[22]; memset(e, 0, sizeof e);
        le32(e, 0x06054b50); le16(e + 8, (uint32_t)cnt); le16(e + 10, (uint32_t)cnt); le32(e + 12, (uint32_t)(o->pos - cd0)); le32(e + 16, (uint32_t)cd0);
        out_w(o, e, 22);
        if (o->err) rc = -3;
    }
    free(ib); free(ob);
    return rc;
}

/* ------------------------------------------------------------------ LZMA encoder (greedy, hash chains, rep matches) */
enum { E_ISMATCH = 0, E_ISREP = E_ISMATCH + (12 << 4), E_G0 = E_ISREP + 12, E_G1 = E_G0 + 12, E_G2 = E_G1 + 12, E_REP0LONG = E_G2 + 12,
       E_SLOT = E_REP0LONG + (12 << 4), E_SPEC = E_SLOT + 4 * 64, E_ALIGN = E_SPEC + 128, E_LEN = E_ALIGN + 16, E_REPLEN = E_LEN + 514, E_LIT = E_REPLEN + 514 };
#define LC 3
#define PB 2
#define HBITS 16
#define MAXLOOK 300
#define MATCH_MAX 273
#define CHAIN_DEPTH 24

typedef struct {
    uint16_t *probs;
    uint64_t low; uint32_t range; uint8_t cache; uint64_t cache_size;
    uint8_t *ob; int ol; Out *out; int err;
    /* window */
    uint8_t *buf; uint32_t dict, bsz, len, p; uint32_t base; int eof;
    uint32_t *head, *prev;
    uint32_t state, rep[4]; uint64_t processed;
    /* input */
    Items *it; const char *basedir; int cur; FsFile *curf; uint64_t done, total; ProgressCb cb; int cancel;
    uint32_t file_crc; uint64_t file_size; int *nbad;
} Enc;

static void rc_flush_buf(Enc *e) { if (e->ol) { out_w(e->out, e->ob, e->ol); e->ol = 0; } if (e->out->err) e->err = 1; }
static void rc_byte(Enc *e, uint8_t b) { e->ob[e->ol++] = b; if (e->ol == IOBUF) rc_flush_buf(e); }
static void rc_shift(Enc *e)
{
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t t = e->cache;
        do { rc_byte(e, (uint8_t)(t + (uint8_t)(e->low >> 32))); t = 0xFF; } while (--e->cache_size);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}
static inline void rc_bit(Enc *e, uint16_t *p, uint32_t bit)
{
    uint32_t v = *p, bound = (e->range >> 11) * v;
    if (!bit) { e->range = bound; *p = (uint16_t)(v + ((2048 - v) >> 5)); }
    else { e->low += bound; e->range -= bound; *p = (uint16_t)(v - (v >> 5)); }
    while (e->range < (1u << 24)) { e->range <<= 8; rc_shift(e); }
}
static void rc_direct(Enc *e, uint32_t v, int nb)
{
    for (int i = nb - 1; i >= 0; i--) {
        e->range >>= 1;
        if ((v >> i) & 1) e->low += e->range;
        while (e->range < (1u << 24)) { e->range <<= 8; rc_shift(e); }
    }
}
static void tree_enc(Enc *e, int base, int nb, uint32_t sym)
{
    uint32_t m = 1;
    for (int i = nb - 1; i >= 0; i--) { uint32_t b = (sym >> i) & 1; rc_bit(e, &e->probs[base + m], b); m = (m << 1) | b; }
}
static void rtree_enc(Enc *e, int base, int nb, uint32_t sym)
{
    uint32_t m = 1;
    for (int i = 0; i < nb; i++) { uint32_t b = (sym >> i) & 1; rc_bit(e, &e->probs[base + m], b); m = (m << 1) | b; }
}
static void len_enc(Enc *e, int base, uint32_t len, uint32_t ps)         /* len = real length - 2 */
{
    if (len < 8) { rc_bit(e, &e->probs[base], 0); tree_enc(e, base + 2 + (int)ps * 8, 3, len); }
    else if (len < 16) { rc_bit(e, &e->probs[base], 1); rc_bit(e, &e->probs[base + 1], 0); tree_enc(e, base + 2 + 128 + (int)ps * 8, 3, len - 8); }
    else { rc_bit(e, &e->probs[base], 1); rc_bit(e, &e->probs[base + 1], 1); tree_enc(e, base + 2 + 256, 8, len - 16); }
}
static void dist_enc(Enc *e, uint32_t dist, uint32_t len)                  /* dist = real distance - 1 */
{
    uint32_t ls = len < 3 ? len : 3, slot;
    if (dist < 4) slot = dist;
    else { int msb = 31; while (!((dist >> msb) & 1)) msb--; slot = (uint32_t)(2 * msb) + ((dist >> (msb - 1)) & 1); }
    tree_enc(e, E_SLOT + (int)ls * 64, 6, slot);
    if (slot >= 4) {
        uint32_t nd = (slot >> 1) - 1, bs = (2 | (slot & 1)) << nd, rem = dist - bs;
        if (slot < 14) rtree_enc(e, E_SPEC + (int)(bs - slot), (int)nd, rem);
        else { rc_direct(e, rem >> 4, (int)nd - 4); rtree_enc(e, E_ALIGN, 4, rem & 15); }
    }
}

/* read more input into the window (all selected files, one after another) */
static void enc_fill(Enc *e)
{
    if (e->eof) return;
    if (e->len + IOBUF > e->bsz) {                                          /* slide: keep the last `dict` bytes behind the cursor */
        uint32_t keep = e->p < e->dict ? e->p : e->dict, drop = e->p - keep;
        if (drop) { memmove(e->buf, e->buf + drop, e->len - drop); e->len -= drop; e->p -= drop; e->base += drop; }
        if (e->len + IOBUF > e->bsz) return;
    }
    for (;;) {
        if (!e->curf) {
            while (e->cur + 1 < e->it->n) {
                e->cur++;
                Item *x = &e->it->v[e->cur];
                if (x->dir || !x->ok) continue;
                char full[PATH_MAX_LEN]; fs_join(full, e->basedir, x->rel);
                e->curf = fs_fopen(full, 0);
                e->file_crc = 0; e->file_size = 0;
                if (!e->curf) { x->ok = 0; (*e->nbad)++; continue; }
                break;
            }
            if (!e->curf) { e->eof = 1; return; }
        }
        int r = fs_fread(e->curf, e->buf + e->len, IOBUF);
        Item *x = &e->it->v[e->cur];
        if (r > 0) {
            e->file_crc = crc32_upd(e->file_crc, e->buf + e->len, (size_t)r); e->file_size += (uint64_t)r;
            e->len += (uint32_t)r; e->done += (uint64_t)r;
            if (e->cb && e->cb(x->rel, e->done, e->total ? e->total : 1)) { e->cancel = 1; e->eof = 1; }
            return;
        }
        /* end of this file (or a read error: the bytes already taken stay in the stream, size/crc describe what was stored) */
        x->crc = e->file_crc; x->size = e->file_size;
        fs_fclose(e->curf); e->curf = NULL;
    }
}

static inline uint32_t hash3(const uint8_t *q) { return (((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16)) * 2654435761u) >> (32 - HBITS); }
static inline void insert_at(Enc *e, uint32_t idx)                         /* idx: index in buf, needs 3 bytes */
{
    if (idx + 3 > e->len) return;
    uint32_t h = hash3(e->buf + idx), abs = e->base + idx;
    e->prev[abs & (e->dict - 1)] = e->head[h];
    e->head[h] = abs + 1;
}
static inline uint32_t mlen(const uint8_t *a, const uint8_t *b, uint32_t max) { uint32_t l = 0; while (l < max && a[l] == b[l]) l++; return l; }

static void enc_lit(Enc *e, uint32_t posState)
{
    uint8_t cur = e->buf[e->p];
    uint32_t prev = e->processed ? e->buf[e->p - 1] : 0;
    int base = E_LIT + 0x300 * (int)(prev >> (8 - LC));
    rc_bit(e, &e->probs[E_ISMATCH + (e->state << 4) + posState], 0);
    uint32_t sym = 1;
    if (e->state >= 7) {
        uint32_t mb = e->buf[e->p - e->rep[0] - 1];
        int i = 7;
        for (; i >= 0; i--) {
            uint32_t mbit = (mb >> 7) & 1, bit = (cur >> i) & 1; mb <<= 1;
            rc_bit(e, &e->probs[base + ((1 + mbit) << 8) + sym], bit);
            sym = (sym << 1) | bit;
            if (mbit != bit) { i--; break; }
        }
        for (; i >= 0; i--) { uint32_t bit = (cur >> i) & 1; rc_bit(e, &e->probs[base + sym], bit); sym = (sym << 1) | bit; }
    } else for (int i = 7; i >= 0; i--) { uint32_t bit = (cur >> i) & 1; rc_bit(e, &e->probs[base + sym], bit); sym = (sym << 1) | bit; }
    e->state = e->state < 4 ? 0 : (e->state < 10 ? e->state - 3 : e->state - 6);
}

/* compress everything; returns 0 ok, -3 write error, -4 memory, -9 cancel */
static int lzma_run(Enc *e)
{
    uint32_t pm = (1u << PB) - 1;
    for (;;) {
        if (e->len - e->p < MAXLOOK) enc_fill(e);
        if (e->cancel) return -9;
        if (e->err) return -3;
        uint32_t avail = e->len - e->p;
        if (avail == 0) break;
        uint32_t ps = (uint32_t)e->processed & pm;
        uint32_t maxl = avail < MATCH_MAX ? avail : MATCH_MAX;
        /* longest match from the hash chain */
        uint32_t best = 0, bestd = 0;
        if (avail >= 3 && e->processed) {
            uint32_t h = hash3(e->buf + e->p), cur = e->base + e->p, c = e->head[h];
            for (int depth = 0; c && depth < CHAIN_DEPTH; depth++) {
                uint32_t pos = c - 1;
                if (pos >= cur || cur - pos > e->dict - 1 || pos < e->base) break;
                uint32_t l = mlen(e->buf + e->p, e->buf + (pos - e->base), maxl);
                if (l > best) { best = l; bestd = cur - pos; if (l >= 64 || l == maxl) break; }
                uint32_t nx = e->prev[pos & (e->dict - 1)];
                if (nx && nx - 1 >= pos) break;
                c = nx;
            }
        }
        /* rep matches */
        uint32_t rl = 0; int ri = 0;
        if (e->processed) for (int r = 0; r < 4; r++) {
            uint32_t d = e->rep[r] + 1;
            if (d > e->processed || d > e->dict - 1 || d > e->p) continue;
            uint32_t l = mlen(e->buf + e->p, e->buf + e->p - d, maxl);
            if (l > rl) { rl = l; ri = r; }
        }
        uint32_t adv;
        if (rl >= 2 && rl + 1 >= best) {
            rc_bit(e, &e->probs[E_ISMATCH + (e->state << 4) + ps], 1);
            rc_bit(e, &e->probs[E_ISREP + e->state], 1);
            if (ri == 0) { rc_bit(e, &e->probs[E_G0 + e->state], 0); rc_bit(e, &e->probs[E_REP0LONG + (e->state << 4) + ps], 1); }
            else {
                rc_bit(e, &e->probs[E_G0 + e->state], 1);
                if (ri == 1) rc_bit(e, &e->probs[E_G1 + e->state], 0);
                else { rc_bit(e, &e->probs[E_G1 + e->state], 1); rc_bit(e, &e->probs[E_G2 + e->state], ri == 3); }
                uint32_t d = e->rep[ri];
                for (int k = ri; k > 0; k--) e->rep[k] = e->rep[k - 1];
                e->rep[0] = d;
            }
            len_enc(e, E_REPLEN, rl - 2, ps);
            e->state = e->state < 7 ? 8 : 11;
            adv = rl;
        } else if (best >= 3 && !(best == 3 && bestd > 8192)) {
            rc_bit(e, &e->probs[E_ISMATCH + (e->state << 4) + ps], 1);
            rc_bit(e, &e->probs[E_ISREP + e->state], 0);
            len_enc(e, E_LEN, best - 2, ps);
            dist_enc(e, bestd - 1, best - 2);
            e->rep[3] = e->rep[2]; e->rep[2] = e->rep[1]; e->rep[1] = e->rep[0]; e->rep[0] = bestd - 1;
            e->state = e->state < 7 ? 7 : 10;
            adv = best;
        } else if (e->processed && e->rep[0] + 1 <= e->p && e->buf[e->p] == e->buf[e->p - e->rep[0] - 1] && e->rep[0] + 1 < e->dict) {
            rc_bit(e, &e->probs[E_ISMATCH + (e->state << 4) + ps], 1);
            rc_bit(e, &e->probs[E_ISREP + e->state], 1);
            rc_bit(e, &e->probs[E_G0 + e->state], 0);
            rc_bit(e, &e->probs[E_REP0LONG + (e->state << 4) + ps], 0);
            e->state = e->state < 7 ? 9 : 11;
            adv = 1;
        } else { enc_lit(e, ps); adv = 1; }
        for (uint32_t k = 0; k < adv; k++) insert_at(e, e->p + k);
        e->p += adv; e->processed += adv;
        if (e->err) return -3;
    }
    for (int i = 0; i < 5; i++) rc_shift(e);
    rc_flush_buf(e);
    return e->err ? -3 : 0;
}

/* ------------------------------------------------------------------ 7z container */
typedef struct { uint8_t *b; size_t n, cap; int err; } WB;
static void wb(WB *w, const void *d, size_t n)
{
    if (w->err) return;
    if (w->n + n > w->cap) { size_t nc = w->cap ? w->cap * 2 : 4096; while (nc < w->n + n) nc *= 2; uint8_t *nb = realloc(w->b, nc); if (!nb) { w->err = 1; return; } w->b = nb; w->cap = nc; }
    memcpy(w->b + w->n, d, n); w->n += n;
}
static void wb8(WB *w, unsigned v) { uint8_t c = (uint8_t)v; wb(w, &c, 1); }
static void wb32(WB *w, uint32_t v) { uint8_t c[4]; le32(c, v); wb(w, c, 4); }
static void wbnum(WB *w, uint64_t v)                                        /* 7z variable-length number */
{
    uint8_t first = 0, mask = 0x80; int i;
    uint8_t extra[8];
    for (i = 0; i < 8; i++) {
        if (v < ((uint64_t)1 << (7 * (i + 1)))) { first |= (uint8_t)(v >> (8 * i)); break; }
        first |= mask; mask >>= 1;
    }
    for (int k = 0; k < i; k++) extra[k] = (uint8_t)(v >> (8 * k));
    wb8(w, first); wb(w, extra, (size_t)i);
}
static void wbbits(WB *w, const uint8_t *v, int n)
{
    uint8_t b = 0, m = 0x80;
    for (int i = 0; i < n; i++) { if (v[i]) b |= m; m >>= 1; if (!m) { wb8(w, b); b = 0; m = 0x80; } }
    if (m != 0x80) wb8(w, b);
}
static void wb_name(WB *w, const char *s)                                   /* UTF-8 -> UTF-16LE, terminated */
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
        if (c >= 0x10000) { c -= 0x10000; uint8_t u[4] = { (uint8_t)(0xD800 + (c >> 10)), (uint8_t)((0xD800 + (c >> 10)) >> 8), (uint8_t)(0xDC00 + (c & 0x3FF)), (uint8_t)((0xDC00 + (c & 0x3FF)) >> 8) }; wb(w, u, 4); }
        else { uint8_t u[2] = { (uint8_t)c, (uint8_t)(c >> 8) }; wb(w, u, 2); }
    }
    uint8_t z[2] = { 0, 0 }; wb(w, z, 2);
}

static int make_7z(Out *o, const char *base, Items *it, uint64_t total, ProgressCb cb, int *n_bad, int *n_ok, char *msg, int mlen)
{
    uint8_t sig[32]; memset(sig, 0, sizeof sig);
    out_w(o, sig, 32);                                                      /* placeholder, rewritten at the end */
    Enc *e = calloc(1, sizeof *e);
    if (!e) return -4;
    uint32_t dict = 1u << 20;
    for (;;) {                                                              /* 1 MB window; 256 KB if memory is tight */
        e->dict = dict; e->bsz = dict + 256 * 1024;
        e->buf = malloc(e->bsz); e->prev = malloc((size_t)dict * 4); e->head = calloc(1u << HBITS, 4);
        e->probs = malloc(sizeof(uint16_t) * (E_LIT + (0x300u << LC))); e->ob = malloc(IOBUF);
        if (e->buf && e->prev && e->head && e->probs && e->ob) break;
        free(e->buf); free(e->prev); free(e->head); free(e->probs); free(e->ob);
        e->buf = NULL; e->prev = NULL; e->head = NULL; e->probs = NULL; e->ob = NULL;
        if (dict <= (1u << 18)) { free(e); return -4; }
        dict >>= 2;
    }
    memset(e->prev, 0, (size_t)dict * 4);
    for (uint32_t i = 0; i < E_LIT + (0x300u << LC); i++) e->probs[i] = 1024;
    e->range = 0xFFFFFFFFu; e->cache_size = 1; e->out = o;
    e->it = it; e->basedir = base; e->cur = -1; e->total = total; e->cb = cb; e->nbad = n_bad;
    uint64_t pack0 = o->pos;
    int rc = lzma_run(e);
    uint64_t packsize = o->pos - pack0;
    if (e->curf) fs_fclose(e->curf);
    uint64_t unp = e->processed;
    free(e->buf); free(e->prev); free(e->head); free(e->probs); free(e->ob); free(e);
    if (rc) return rc;
    if (o->err) return -3;

    /* header */
    int ndata = 0, nfiles = 0, nempty = 0;
    for (int i = 0; i < it->n; i++) {
        Item *x = &it->v[i];
        if (!x->ok) continue;
        nfiles++;
        if (!x->dir && x->size > 0) ndata++; else nempty++;
    }
    WB w; memset(&w, 0, sizeof w);
    wb8(&w, 0x01);
    if (ndata) {
        wb8(&w, 0x04);
        wb8(&w, 0x06); wbnum(&w, 0); wbnum(&w, 1); wb8(&w, 0x09); wbnum(&w, packsize); wb8(&w, 0x00);
        wb8(&w, 0x07); wb8(&w, 0x0B); wbnum(&w, 1); wb8(&w, 0);
        wbnum(&w, 1); wb8(&w, 0x23); wb8(&w, 0x03); wb8(&w, 0x01); wb8(&w, 0x01); wbnum(&w, 5);
        wb8(&w, (PB * 5 + 0) * 9 + LC); wb32(&w, dict);
        wb8(&w, 0x0C); wbnum(&w, unp); wb8(&w, 0x00);
        wb8(&w, 0x08); wb8(&w, 0x0D); wbnum(&w, (uint64_t)ndata);
        if (ndata > 1) { wb8(&w, 0x09); int k = 0; for (int i = 0; i < it->n; i++) { Item *x = &it->v[i]; if (!x->ok || x->dir || x->size == 0) continue; if (++k < ndata) wbnum(&w, x->size); } }
        wb8(&w, 0x0A); wb8(&w, 1); for (int i = 0; i < it->n; i++) { Item *x = &it->v[i]; if (!x->ok || x->dir || x->size == 0) continue; wb32(&w, x->crc); }
        wb8(&w, 0x00);
        wb8(&w, 0x00);
    }
    wb8(&w, 0x05); wbnum(&w, (uint64_t)nfiles);
    if (nempty) {
        uint8_t *bits = calloc((size_t)nfiles, 1), *ef = calloc((size_t)nempty, 1);
        if (!bits || !ef) { free(bits); free(ef); free(w.b); return -4; }
        int fi = 0, ei = 0, anyfile = 0;
        for (int i = 0; i < it->n; i++) {
            Item *x = &it->v[i];
            if (!x->ok) continue;
            if (x->dir || x->size == 0) { bits[fi] = 1; ef[ei++] = x->dir ? 0 : 1; if (!x->dir) anyfile = 1; }
            fi++;
        }
        WB t; memset(&t, 0, sizeof t); wbbits(&t, bits, nfiles);
        wb8(&w, 0x0E); wbnum(&w, t.n); wb(&w, t.b, t.n); free(t.b);
        if (anyfile) { WB u; memset(&u, 0, sizeof u); wbbits(&u, ef, nempty); wb8(&w, 0x0F); wbnum(&w, u.n); wb(&w, u.b, u.n); free(u.b); }
        free(bits); free(ef);
    }
    { WB nm; memset(&nm, 0, sizeof nm); wb8(&nm, 0); for (int i = 0; i < it->n; i++) if (it->v[i].ok) wb_name(&nm, it->v[i].rel);
      wb8(&w, 0x11); wbnum(&w, nm.n); wb(&w, nm.b, nm.n); free(nm.b); }
    wb8(&w, 0x00);
    wb8(&w, 0x00);
    if (w.err) { free(w.b); return -4; }
    out_w(o, w.b, (int)w.n);
    /* signature header */
    memcpy(sig, "7z\xBC\xAF\x27\x1C\x00\x04", 8);
    le32(sig + 12, (uint32_t)packsize); le32(sig + 16, (uint32_t)(packsize >> 32));
    le32(sig + 20, (uint32_t)w.n); le32(sig + 24, 0);
    le32(sig + 28, crc32_upd(0, w.b, w.n));
    le32(sig + 8, crc32_upd(0, sig + 12, 20));
    free(w.b);
    out_seek(o, 0); out_w(o, sig, 32);
    if (o->err) return -3;
    for (int i = 0; i < it->n; i++) if (it->v[i].ok) (*n_ok)++;
    (void)msg; (void)mlen;
    return 0;
}

/* ------------------------------------------------------------------ entry point */
int compress_make(int kind, const char *outp, const char *basedir, const char *const *names, int n,
                  ProgressCb cb, int *n_ok, int *n_bad, char *msg, int msglen)
{
    Items it; memset(&it, 0, sizeof it);
    int ok = 0, bad = 0, rc;
    uint64_t total = 0;
    char lm[96]; lm[0] = 0;
    if (n_ok) *n_ok = 0;
    if (n_bad) *n_bad = 0;
    if (msg && msglen) msg[0] = 0;
    rc = scan(&it, basedir, names, n, &total, lm, sizeof lm);
    if (rc) { items_free(&it); if (msg) snprintf(msg, msglen, "%s", lm); return rc == -5 ? -5 : rc; }
    if (it.n == 0) { items_free(&it); return -1; }
    Out o; memset(&o, 0, sizeof o);
    o.f = fs_fopen(outp, 1);
    if (!o.f) { items_free(&it); return -2; }
    rc = kind == 0 ? make_zip(&o, basedir, &it, total, cb, &bad, &ok, lm, sizeof lm) : make_7z(&o, basedir, &it, total, cb, &bad, &ok, lm, sizeof lm);
    fs_fclose(o.f);
    if (rc == 0 && o.err) rc = -3;
    if (rc == 0 && ok == 0) rc = -1;
    if (rc) fs_delete(outp, NULL);
    items_free(&it);
    if (n_ok) *n_ok = ok;
    if (n_bad) *n_bad = bad;
    if (msg && msglen && lm[0]) snprintf(msg, msglen, "%s", lm);
    return rc;
}
