/* FLAC decoder: verbatim, constant, fixed and LPC subframes, Rice partitions, all stereo modes. No CRC checks:
   a damaged frame is skipped by searching for the next frame header. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "flacdec.h"

typedef struct { const uint8_t *p; unsigned size; uint64_t pos; int err; } Bits;   /* pos in bits */

static uint32_t br_bits(Bits *b, int n)             /* n <= 32 */
{
    uint32_t v = 0;
    while (n > 0) {
        unsigned byte = (unsigned)(b->pos >> 3);
        if (byte >= b->size) { b->err = 1; return 0; }
        int avail = 8 - (int)(b->pos & 7), take = n < avail ? n : avail;
        v = (v << take) | (((unsigned)b->p[byte] >> (avail - take)) & ((1u << take) - 1));
        b->pos += take; n -= take;
    }
    return v;
}
static int32_t br_signed(Bits *b, int n) { if (n == 0) return 0; uint32_t v = br_bits(b, n); return (int32_t)(v << (32 - n)) >> (32 - n); }
static uint32_t br_unary(Bits *b)                    /* count zero bits up to the next one */
{
    uint32_t q = 0;
    for (;;) {
        unsigned byte = (unsigned)(b->pos >> 3);
        if (byte >= b->size) { b->err = 1; return 0; }
        unsigned cur = (unsigned)(b->p[byte] << (b->pos & 7)) & 0xFF;
        if (cur == 0) { q += 8 - (unsigned)(b->pos & 7); b->pos = (b->pos | 7) + 1; if (q > (1u << 24)) { b->err = 1; return 0; } continue; }
        int z = 0; while (!(cur & 0x80)) { cur <<= 1; z++; }
        q += z; b->pos += z + 1;
        return q;
    }
}

struct FlacDec {
    const uint8_t *buf; unsigned size, first;   /* first = offset of the first audio frame */
    int rate, ch, bps; unsigned maxblock;
    Bits b;
    int32_t *chan[2];
    unsigned blen, bpos;                         /* decoded block (blen samples per channel) */
    int done;
};

static int decode_residual(Bits *b, int32_t *out, unsigned blocksize, unsigned order)
{
    unsigned method = br_bits(b, 2);
    if (method > 1) return -1;
    unsigned porder = br_bits(b, 4);
    unsigned nparts = 1u << porder, psamples = blocksize >> porder;
    if ((blocksize & (nparts - 1)) || psamples < order) return -1;
    unsigned idx = order;
    for (unsigned p = 0; p < nparts; p++) {
        unsigned n = psamples - (p == 0 ? order : 0);
        unsigned k = br_bits(b, method ? 5 : 4);
        if (k == (method ? 31u : 15u)) {                       /* escape: raw samples */
            unsigned rb = br_bits(b, 5);
            for (unsigned i = 0; i < n; i++) out[idx++] = br_signed(b, (int)rb);
        } else {
            for (unsigned i = 0; i < n; i++) {
                uint32_t q = br_unary(b);
                uint32_t v = (q << k) | (k ? br_bits(b, (int)k) : 0);
                out[idx++] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
            }
        }
        if (b->err) return -1;
    }
    return 0;
}

static int decode_subframe(Bits *b, int32_t *out, unsigned bs, int bps)
{
    if (br_bits(b, 1)) return -1;
    unsigned type = br_bits(b, 6);
    int wasted = 0;
    if (br_bits(b, 1)) { wasted = 1 + (int)br_unary(b); if (wasted >= bps) return -1; bps -= wasted; }
    if (type == 0) {
        int32_t v = br_signed(b, bps);
        for (unsigned i = 0; i < bs; i++) out[i] = v;
    } else if (type == 1) {
        for (unsigned i = 0; i < bs; i++) out[i] = br_signed(b, bps);
    } else if (type >= 8 && type <= 12) {
        unsigned order = type - 8;
        if (order > bs) return -1;
        for (unsigned i = 0; i < order; i++) out[i] = br_signed(b, bps);
        if (decode_residual(b, out, bs, order)) return -1;
        for (unsigned i = order; i < bs; i++) {
            int64_t a;
            switch (order) {
            case 0: a = 0; break;
            case 1: a = out[i - 1]; break;
            case 2: a = 2 * (int64_t)out[i - 1] - out[i - 2]; break;
            case 3: a = 3 * (int64_t)out[i - 1] - 3 * (int64_t)out[i - 2] + out[i - 3]; break;
            default: a = 4 * (int64_t)out[i - 1] - 6 * (int64_t)out[i - 2] + 4 * (int64_t)out[i - 3] - out[i - 4]; break;
            }
            out[i] = (int32_t)(out[i] + a);
        }
    } else if (type >= 32) {
        unsigned order = (type & 31) + 1;
        if (order > bs) return -1;
        for (unsigned i = 0; i < order; i++) out[i] = br_signed(b, bps);
        unsigned prec = br_bits(b, 4) + 1;
        if (prec == 16) return -1;
        int shift = br_signed(b, 5);
        if (shift < 0) return -1;
        int32_t coef[32];
        for (unsigned i = 0; i < order; i++) coef[i] = br_signed(b, (int)prec);
        if (decode_residual(b, out, bs, order)) return -1;
        for (unsigned i = order; i < bs; i++) {
            int64_t s = 0;
            for (unsigned j = 0; j < order; j++) s += (int64_t)coef[j] * out[i - 1 - j];
            out[i] = (int32_t)(out[i] + (s >> shift));
        }
    } else return -1;
    if (wasted) for (unsigned i = 0; i < bs; i++) out[i] = (int32_t)((uint32_t)out[i] << wasted);
    return b->err ? -1 : 0;
}

/* returns 0 and fills d->chan / d->blen, or <0 */
static int decode_frame(FlacDec *d)
{
    Bits *b = &d->b;
    uint64_t start = b->pos;
    if (br_bits(b, 14) != 0x3FFE || br_bits(b, 1) != 0) return -1;
    br_bits(b, 1);                                       /* blocking strategy */
    unsigned bsc = br_bits(b, 4), src = br_bits(b, 4), cha = br_bits(b, 4), szc = br_bits(b, 3);
    if (br_bits(b, 1)) return -1;
    unsigned first = br_bits(b, 8);                      /* UTF-8 coded frame / sample number: just skip it */
    if (first >= 0xC0) { int more = first >= 0xFE ? 6 : first >= 0xFC ? 5 : first >= 0xF8 ? 4 : first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : 1; while (more--) br_bits(b, 8); }
    unsigned bs;
    if (bsc == 0) return -1;
    else if (bsc == 1) bs = 192;
    else if (bsc <= 5) bs = 576u << (bsc - 2);
    else if (bsc == 6) bs = br_bits(b, 8) + 1;
    else if (bsc == 7) bs = br_bits(b, 16) + 1;
    else bs = 256u << (bsc - 8);
    if (src == 12) br_bits(b, 8); else if (src == 13 || src == 14) br_bits(b, 16); else if (src == 15) return -1;
    br_bits(b, 8);                                       /* header CRC */
    static const int bpsv[8] = { 0, 8, 12, -1, 16, 20, 24, -1 };
    int bps = szc == 0 ? d->bps : bpsv[szc];
    if (bps <= 0 || b->err || bs > d->maxblock) return -1;
    int nch, mode = 0;                                   /* mode 0 independent, 1 L/S, 2 S/R, 3 M/S */
    if (cha < 8) nch = (int)cha + 1; else if (cha <= 10) { nch = 2; mode = (int)cha - 7; } else return -1;
    if (nch != d->ch) return -1;
    for (int c = 0; c < nch; c++) {
        int sb = bps + ((mode == 1 && c == 1) || (mode == 2 && c == 0) || (mode == 3 && c == 1) ? 1 : 0);
        if (decode_subframe(b, d->chan[c], bs, sb)) return -1;
    }
    b->pos = (b->pos + 7) & ~7ull;                       /* byte align, then skip the frame CRC-16 */
    br_bits(b, 16);
    if (b->err) return -1;
    if (mode == 1) for (unsigned i = 0; i < bs; i++) d->chan[1][i] = d->chan[0][i] - d->chan[1][i];
    else if (mode == 2) for (unsigned i = 0; i < bs; i++) d->chan[0][i] = d->chan[0][i] + d->chan[1][i];
    else if (mode == 3) for (unsigned i = 0; i < bs; i++) {
        int32_t m = d->chan[0][i], s = d->chan[1][i];
        m = (int32_t)(((uint32_t)m << 1) | ((uint32_t)s & 1));
        d->chan[0][i] = (m + s) >> 1; d->chan[1][i] = (m - s) >> 1;
    }
    /* bring everything to 16 bit */
    int sh = bps - 16;
    for (int c = 0; c < nch; c++) for (unsigned i = 0; i < bs; i++) {
        int32_t v = d->chan[c][i];
        v = sh > 0 ? v >> sh : sh < 0 ? v << -sh : v;
        d->chan[c][i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    d->blen = bs; d->bpos = 0;
    (void)start;
    return 0;
}

FlacDec *flacdec_open(const unsigned char *buf, unsigned size, int *rate, int *channels)
{
    unsigned p = 0;
    if (size > 10 && !memcmp(buf, "ID3", 3)) p = 10 + ((buf[6] << 21) | (buf[7] << 14) | (buf[8] << 7) | buf[9]);
    if (p + 42 > size || memcmp(buf + p, "fLaC", 4)) return NULL;
    p += 4;
    int have = 0, rt = 0, ch = 0, bps = 0; unsigned maxb = 0;
    for (;;) {
        if (p + 4 > size) return NULL;
        int last = buf[p] >> 7, type = buf[p] & 0x7F;
        unsigned len = (buf[p + 1] << 16) | (buf[p + 2] << 8) | buf[p + 3];
        p += 4;
        if (p + len > size) return NULL;
        if (type == 0 && len >= 34) {
            maxb = (buf[p + 2] << 8) | buf[p + 3];
            rt = (buf[p + 10] << 12) | (buf[p + 11] << 4) | (buf[p + 12] >> 4);
            ch = ((buf[p + 12] >> 1) & 7) + 1;
            bps = (((buf[p + 12] & 1) << 4) | (buf[p + 13] >> 4)) + 1;
            have = 1;
        }
        p += len;
        if (last) break;
    }
    if (!have || ch < 1 || ch > 2 || rt < 1000 || maxb < 16 || bps < 4) return NULL;
    FlacDec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->buf = buf; d->size = size; d->first = p; d->rate = rt; d->ch = ch; d->bps = bps; d->maxblock = maxb;
    for (int c = 0; c < ch; c++) { d->chan[c] = malloc(sizeof(int32_t) * maxb); if (!d->chan[c]) { flacdec_close(d); return NULL; } }
    flacdec_rewind(d);
    *rate = rt; *channels = ch;
    return d;
}

void flacdec_rewind(FlacDec *d)
{
    d->b.p = d->buf; d->b.size = d->size; d->b.pos = (uint64_t)d->first * 8; d->b.err = 0;
    d->blen = d->bpos = 0; d->done = 0;
}

int flacdec_read(FlacDec *d, short *out, int max_frames)
{
    int n = 0;
    while (n < max_frames) {
        if (d->bpos >= d->blen) {
            if (d->done) break;
            int tries = 0;
            for (;;) {
                if (((d->b.pos + 7) >> 3) + 6 >= d->size) { d->done = 1; break; }
                uint64_t at = (d->b.pos + 7) & ~7ull;
                d->b.err = 0; d->b.pos = at;
                if (decode_frame(d) == 0) break;
                /* damaged or not a frame start: look for the next sync code (0xFF 0xF8 / 0xF9) */
                unsigned q = (unsigned)(at >> 3) + 1;
                while (q + 1 < d->size && !(d->buf[q] == 0xFF && (d->buf[q + 1] & 0xFE) == 0xF8)) q++;
                if (q + 1 >= d->size || ++tries > 64) { d->done = 1; break; }
                d->b.pos = (uint64_t)q * 8;
            }
            if (d->done) break;
        }
        short l = (short)d->chan[0][d->bpos], r = d->ch == 2 ? (short)d->chan[1][d->bpos] : l;
        out[2 * n] = l; out[2 * n + 1] = r; n++; d->bpos++;
    }
    return n;
}

void flacdec_close(FlacDec *d)
{
    if (!d) return;
    free(d->chan[0]); free(d->chan[1]); free(d);
}
