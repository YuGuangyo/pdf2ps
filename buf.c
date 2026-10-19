/*
 * buf.c - growable buffer, zlib inflate (RFC 1951), ASCII85 / ASCIIHex decoders.
 * No external dependencies: deliberately self-contained for musl/mipsel builds.
 */
#include <stdlib.h>
#include <string.h>
#include "buf.h"

void buf_init(buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/*
 * Ensure at least `need` bytes are addressable past b->data.
 * Returns 1 on success, 0 on allocation failure.
 * NOTE: may realloc b->data, so callers must re-read b->data afterwards.
 */
int buf_reserve(buf_t *b, size_t need) {
    size_t ncap;
    unsigned char *nd;
    if (need <= b->cap) return 1;
    ncap = b->cap ? b->cap : 4096;
    while (ncap < need) ncap *= 2;
    nd = (unsigned char *)realloc(b->data, ncap);
    if (!nd) return 0;
    b->data = nd;
    b->cap = ncap;
    return 1;
}

int buf_put(buf_t *b, const void *p, size_t n) {
    if (n == 0) return 1;
    if (!buf_reserve(b, b->len + n)) return 0;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

int buf_putc(buf_t *b, int c) {
    unsigned char ch = (unsigned char)c;
    return buf_put(b, &ch, 1);
}

int buf_puts(buf_t *b, const char *s) {
    return buf_put(b, s, strlen(s));
}

/* ---------------- inflate ---------------- */

typedef struct {
    const unsigned char *in;
    size_t inlen;
    size_t inpos;
    unsigned bitbuf;
    int bitcnt;
} bitstream_t;

static int bs_getbits(bitstream_t *bs, int n) {
    int v;
    while (bs->bitcnt < n) {
        if (bs->inpos >= bs->inlen) return -1;
        bs->bitbuf |= ((unsigned)bs->in[bs->inpos++]) << bs->bitcnt;
        bs->bitcnt += 8;
    }
    v = (int)(bs->bitbuf & ((1u << n) - 1u));
    bs->bitbuf >>= n;
    bs->bitcnt -= n;
    return v;
}

typedef struct {
    short count[16];
    short symbol[288];
} huff_t;

static int huff_build(huff_t *h, const short *length, int n) {
    int i, left;
    short offs[16];
    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
    return 0;
}

static int huff_decode(bitstream_t *bs, const huff_t *h) {
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        int b = bs_getbits(bs, 1);
        int count;
        if (b < 0) return -1;
        code |= b;
        count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_block(buf_t *out, bitstream_t *bs, const huff_t *lit, const huff_t *dist) {
    static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = huff_decode(bs, lit);
        int len, ex, dsym, d;
        if (sym < 0) return -1;
        if (sym < 256) {
            if (!buf_putc(out, sym)) return -1;
            continue;
        }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -1;
        len = lbase[sym];
        ex = lext[sym];
        if (ex) {
            int v = bs_getbits(bs, ex);
            if (v < 0) return -1;
            len += v;
        }
        dsym = huff_decode(bs, dist);
        if (dsym < 0 || dsym >= 30) return -1;
        d = dbase[dsym];
        ex = dext[dsym];
        if (ex) {
            int v = bs_getbits(bs, ex);
            if (v < 0) return -1;
            d += v;
        }
        if ((size_t)d > out->len) return -1;
        {
            /* LZ77 back-reference: copy len bytes starting at out->len - d.
             * The source range may overlap the destination (that is legal),
             * so data must be read after any single reallocation. */
            size_t start = out->len - (size_t)d;
            int k;
            if (!buf_reserve(out, (size_t)len)) return -1;
            for (k = 0; k < len; k++) {
                out->data[out->len] = out->data[start + (size_t)k];
                out->len++;
            }
        }
    }
}

int zlib_inflate(const unsigned char *in, size_t inlen, buf_t *out) {
    static const short lens_tab[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    bitstream_t bs;
    int last;
    if (inlen < 2) return -1;
    bs.in = in + 2;         /* skip zlib header */
    bs.inlen = inlen - 2;
    bs.inpos = 0;
    bs.bitbuf = 0;
    bs.bitcnt = 0;

    do {
        int type;
        last = bs_getbits(&bs, 1);
        if (last < 0) return -1;
        type = bs_getbits(&bs, 2);
        if (type < 0) return -1;
        if (type == 0) {
            int n;
            bs.bitbuf = 0;
            bs.bitcnt = 0;
            if (bs.inpos + 4 > bs.inlen) return -1;
            n = bs.in[bs.inpos] | (bs.in[bs.inpos + 1] << 8);
            bs.inpos += 4;
            if (bs.inpos + (size_t)n > bs.inlen) return -1;
            if (!buf_put(out, bs.in + bs.inpos, (size_t)n)) return -1;
            bs.inpos += (size_t)n;
        } else if (type == 1) {
            short l[288];
            short dl[30];
            huff_t lit, dist;
            int i;
            for (i = 0; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            if (huff_build(&lit, l, 288) < 0) return -1;
            for (i = 0; i < 30; i++) dl[i] = 5;
            if (huff_build(&dist, dl, 30) < 0) return -1;
            if (inflate_block(out, &bs, &lit, &dist) < 0) return -1;
        } else if (type == 2) {
            int hlit = bs_getbits(&bs, 5);
            int hdist = bs_getbits(&bs, 5);
            int hclen = bs_getbits(&bs, 4);
            short cl[19];
            short lengths[288 + 30];
            int i, n;
            huff_t clh, lit, dist;
            if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
            hlit += 257; hdist += 1; hclen += 4;
            if (hlit > 286 || hdist > 30) return -1;
            for (i = 0; i < 19; i++) cl[i] = 0;
            for (i = 0; i < hclen; i++) {
                int v = bs_getbits(&bs, 3);
                if (v < 0) return -1;
                cl[lens_tab[i]] = (short)v;
            }
            if (huff_build(&clh, cl, 19) < 0) return -1;
            n = 0;
            while (n < hlit + hdist) {
                int sym = huff_decode(&bs, &clh);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[n++] = (short)sym;
                } else if (sym == 16) {
                    int rep, prev;
                    if (n == 0) return -1;
                    rep = bs_getbits(&bs, 2);
                    if (rep < 0) return -1;
                    rep += 3;
                    prev = lengths[n - 1];
                    while (rep-- && n < hlit + hdist) lengths[n++] = (short)prev;
                } else if (sym == 17) {
                    int rep = bs_getbits(&bs, 3);
                    if (rep < 0) return -1;
                    rep += 3;
                    while (rep-- && n < hlit + hdist) lengths[n++] = 0;
                } else {
                    int rep = bs_getbits(&bs, 7);
                    if (rep < 0) return -1;
                    rep += 11;
                    while (rep-- && n < hlit + hdist) lengths[n++] = 0;
                }
            }
            if (huff_build(&lit, lengths, hlit) < 0) return -1;
            if (huff_build(&dist, lengths + hlit, hdist) < 0) return -1;
            if (inflate_block(out, &bs, &lit, &dist) < 0) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return 0;
}

/* ---------------- ASCII85 ---------------- */

int ascii85_decode(const unsigned char *in, size_t inlen, buf_t *out) {
    unsigned long v = 0;
    int cnt = 0;
    size_t i;
    for (i = 0; i < inlen; i++) {
        int c = in[i];
        if (c == '~') break;
        if (c == 'z' && cnt == 0) {
            unsigned char z[4] = { 0, 0, 0, 0 };
            if (!buf_put(out, z, 4)) return 0;
            continue;
        }
        if (c < '!' || c > 'u') continue;
        v = v * 85 + (unsigned long)(c - '!');
        if (++cnt == 5) {
            unsigned char o[4];
            o[0] = (unsigned char)(v >> 24);
            o[1] = (unsigned char)(v >> 16);
            o[2] = (unsigned char)(v >> 8);
            o[3] = (unsigned char)v;
            if (!buf_put(out, o, 4)) return 0;
            v = 0;
            cnt = 0;
        }
    }
    if (cnt > 1) {
        int k;
        for (k = cnt; k < 5; k++) v = v * 85 + 84;
        {
            unsigned char o[4];
            o[0] = (unsigned char)(v >> 24);
            o[1] = (unsigned char)(v >> 16);
            o[2] = (unsigned char)(v >> 8);
            o[3] = (unsigned char)v;
            if (!buf_put(out, o, (size_t)(cnt - 1))) return 0;
        }
    }
    return 1;
}

/* ---------------- ASCIIHex ---------------- */

int asciihex_decode(const unsigned char *in, size_t inlen, buf_t *out) {
    int hi = -1;
    size_t i;
    for (i = 0; i < inlen; i++) {
        int c = in[i];
        int v;
        if (c == '>') break;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else {
            if (!buf_putc(out, (hi << 4) | v)) return 0;
            hi = -1;
        }
    }
    if (hi >= 0) {
        if (!buf_putc(out, hi << 4)) return 0;
    }
    return 1;
}
