/*
 * psfont.c - font resolution and glyph emission for pdftops-lite
 *
 * Design notes
 * ------------
 * Every font is resolved through the page's /Resources /Font dictionary by
 * *resource name* ("F1"), never by comparing /BaseFont against it. The old
 * code did the latter and therefore never matched anything: a Skia-generated
 * BaseFont looks like "AAAAAA+NotoSans", so the Type3 path was dead code and
 * subset fonts silently fell back to Helvetica while keeping GID byte codes,
 * which is exactly the garbled output users saw.
 *
 * Three emission strategies, chosen from /Subtype:
 *
 *   simple (Type1/TrueType/Type3)   one byte per glyph
 *   composite (Type0)               CMap-driven, usually 2-byte Identity-H
 *
 * For a composite font whose descendant is CIDFontType2 with an embedded
 * TrueType program we pull real outlines out of the 'glyf' table, so the
 * printer receives vectors instead of a substituted face. When that is not
 * possible (no FontFile2, exotic CMap, subset without loca) we fall back to a
 * standard face and *say so* through psfont_fallback_count, because silent
 * substitution is what produced the unreadable documents in the first place.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pdfobj.h"
#include "buf.h"
#include "psfont.h"

unsigned long psfont_glyph_draws = 0;
unsigned long psfont_fallback_count = 0;

/* ------------------------------------------------------------------ */
/* TrueType program: just enough of sfnt + glyf/loca                   */
/* ------------------------------------------------------------------ */

typedef struct {
    buf_t   data;        /* whole font program, decompressed            */
    int     valid;
    int     numGlyphs;
    int     indexToLocFormat;
    size_t  loca_off, loca_len;
    size_t  glyf_off, glyf_len;
    size_t  head_off, hhea_off, maxp_off;
    int     unitsPerEm;
    int     ascent, descent;   /* font units */
} ttfont_t;

static unsigned rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static long   rd32(const unsigned char *p) {
    return ((long)p[0] << 24) | ((long)p[1] << 16) | ((long)p[2] << 8) | p[3];
}
static int    rds16(const unsigned char *p) { return (int)(short)rd16(p); }

static void tt_free(ttfont_t *tt) {
    if (!tt) return;
    buf_free(&tt->data);
    memset(tt, 0, sizeof(*tt));
}

/*
 * Locate a table by tag in the sfnt directory. Handles both classic sfnt
 * (0x00010000 / 'true') and the Apple 'ttcf' collection header by preferring
 * the first face, which is what a PDF subset always contains.
 */
static int tt_find_table(ttfont_t *tt, const char tag[4],
                         size_t *off, size_t *len) {
    const unsigned char *b = tt->data.data;
    size_t n = tt->data.len;
    unsigned numTables, i;
    size_t dir = 12;

    if (n < 12) return 0;
    if (b[0] == 't' && b[1] == 't' && b[2] == 'c' && b[3] == 'f') {
        long first;
        if (n < 16) return 0;
        first = rd32(b + 12);
        if (first < 0 || (size_t)first + 12 > n) return 0;
        dir = (size_t)first + 12;
        numTables = rd16(b + (size_t)first + 4);
    } else {
        numTables = rd16(b + 4);
    }
    if (numTables == 0 || numTables > 512) return 0;

    for (i = 0; i < numTables; i++) {
        size_t e = dir + (size_t)i * 16;
        if (e + 16 > n) return 0;
        if (memcmp(b + e, tag, 4) == 0) {
            size_t o = (size_t)rd32(b + e + 8);
            size_t l = (size_t)rd32(b + e + 12);
            if (o > n) return 0;
            if (o + l > n) l = n - o;
            *off = o; *len = l;
            return 1;
        }
    }
    return 0;
}

static int tt_load(ttfont_t *tt, buf_t *program) {
    size_t off, len;

    memset(tt, 0, sizeof(*tt));
    tt->data = *program;          /* take ownership */
    memset(program, 0, sizeof(*program));

    if (!tt_find_table(tt, "maxp", &off, &len) || len < 6) goto bad;
    tt->maxp_off = off;
    tt->numGlyphs = rd16(tt->data.data + off + 4);
    if (tt->numGlyphs <= 0 || tt->numGlyphs > 65535) goto bad;

    if (!tt_find_table(tt, "head", &off, &len) || len < 54) goto bad;
    tt->head_off = off;
    tt->indexToLocFormat = rds16(tt->data.data + off + 50);
    tt->unitsPerEm = rd16(tt->data.data + off + 18);
    if (tt->unitsPerEm <= 0) tt->unitsPerEm = 1000;

    if (tt_find_table(tt, "hhea", &off, &len) && len >= 36) {
        tt->hhea_off = off;
        tt->ascent  = rds16(tt->data.data + off + 4);
        tt->descent = rds16(tt->data.data + off + 6);
    }

    if (!tt_find_table(tt, "loca", &off, &len)) goto bad;
    tt->loca_off = off; tt->loca_len = len;
    /* A CIDFont may ship no glyph outlines at all (bitmap-only or a stub);
     * that is not fatal, it just means "no vector data available". */
    if (!tt_find_table(tt, "glyf", &off, &len)) {
        tt->glyf_off = 0; tt->glyf_len = 0;
    } else {
        tt->glyf_off = off; tt->glyf_len = len;
    }
    tt->valid = 1;
    return 1;

bad:
    buf_free(&tt->data);
    return 0;
}

/* Advance width in font units, from hmtx. */
static int tt_advance(ttfont_t *tt, int gid) {
    size_t off, len;
    unsigned nlong;
    if (!tt->valid || gid < 0 || gid >= tt->numGlyphs) return 0;
    if (!tt_find_table(tt, "hmtx", &off, &len)) return 0;
    nlong = (tt->hhea_off && tt->hhea_off + 36 <= tt->data.len)
          ? (unsigned)rd16(tt->data.data + tt->hhea_off + 34) : 0;
    if (nlong == 0) return 0;
    {
        unsigned idx = ((unsigned)gid < nlong) ? (unsigned)gid : (nlong - 1);
        size_t pos = off + (size_t)idx * 4;
        if (pos + 2 > off + len) return 0;
        return rd16(tt->data.data + pos);
    }
}

/* Glyph byte range inside glyf, via loca. */
static int tt_glyph_range(ttfont_t *tt, int gid, size_t *start, size_t *end) {
    size_t a, b;
    if (!tt->valid || gid < 0 || gid >= tt->numGlyphs || !tt->glyf_len) return 0;
    if (tt->indexToLocFormat == 0) {
        a = tt->loca_off + (size_t)gid * 2;
        b = a + 2;
        if (b + 2 > tt->loca_off + tt->loca_len) return 0;
        *start = (size_t)rd16(tt->data.data + a) * 2u;
        *end   = (size_t)rd16(tt->data.data + b) * 2u;
    } else {
        a = tt->loca_off + (size_t)gid * 4;
        b = a + 4;
        if (b + 4 > tt->loca_off + tt->loca_len) return 0;
        *start = (size_t)rd32(tt->data.data + a);
        *end   = (size_t)rd32(tt->data.data + b);
    }
    if (*end <= *start) return 0;                 /* empty glyph */
    if (*start >= tt->glyf_len) return 0;
    if (*end > tt->glyf_len) *end = tt->glyf_len;
    return 1;
}

/*
 * Emit one TrueType glyph as PostScript path operators.
 *
 * quadratic curves are raised to cubics for PostScript's curveto:
 *   cubic control points for a quadratic (P0, P1, P2) are
 *     C1 = P0 + 2/3 (P1 - P0),  C2 = P2 + 2/3 (P1 - P2)
 *
 * Returns 1 if any point was emitted.
 */
static int tt_emit_glyph(ttfont_t *tt, int gid, FILE *out) {
    const unsigned char *g;
    size_t start, end, n;
    int ncontours, nc, i;
    int x = 0, y = 0;
    int emitted = 0;
    int startPts[256];
    int endPts[256];

    if (!tt_glyph_range(tt, gid, &start, &end)) return 0;
    g = tt->data.data + tt->glyf_off + start;
    n = end - start;
    if (n < 10) return 0;

    ncontours = rds16(g);
    if (ncontours < 0) return 0;                  /* composite: skip */
    if (ncontours == 0 || ncontours > 256) return 0;

    {
        int npts = 0;
        size_t p = 10 + (size_t)ncontours * 2 + 2;
        if (p > n) return 0;
        for (nc = 0; nc < ncontours; nc++) {
            int e = rd16(g + 10 + (size_t)nc * 2);
            if (e < npts) return 0;
            endPts[nc] = e;
            npts = e + 1;
        }
        if (npts <= 0 || npts > 4096) return 0;

        {
            unsigned char *flags = (unsigned char *)malloc((size_t)npts);
            int *xs = (int *)malloc(sizeof(int) * (size_t)npts);
            int *ys = (int *)malloc(sizeof(int) * (size_t)npts);
            int *oncurve = (int *)malloc(sizeof(int) * (size_t)npts);
            int ok = (flags && xs && ys && oncurve);

            if (!ok) { free(flags); free(xs); free(ys); free(oncurve); return 0; }

            /* flags, with repeat compression */
            {
                int fi = 0;
                while (fi < npts && p < n) {
                    int f = g[p++];
                    flags[fi++] = (unsigned char)f;
                    if (f & 8) {
                        int rep = (p < n) ? g[p++] : 0;
                        while (rep-- > 0 && fi < npts) flags[fi++] = (unsigned char)f;
                    }
                }
                if (fi < npts) { free(flags); free(xs); free(ys); free(oncurve); return 0; }
            }

            /* x deltas */
            {
                int vi = 0;
                while (vi < npts && p < n) {
                    unsigned f = flags[vi];
                    if (f & 2) {
                        int d = g[p++];
                        x += (f & 16) ? d : -d;
                    } else if (!(f & 16)) {
                        if (p + 2 > n) break;
                        x += rds16(g + p); p += 2;
                    }
                    xs[vi] = x;
                    vi++;
                }
            }
            /* y deltas */
            {
                int vi = 0;
                while (vi < npts && p < n) {
                    unsigned f = flags[vi];
                    if (f & 4) {
                        int d = g[p++];
                        y += (f & 32) ? d : -d;
                    } else if (!(f & 32)) {
                        if (p + 2 > n) break;
                        y += rds16(g + p); p += 2;
                    }
                    ys[vi] = y;
                    vi++;
                }
            }
            for (i = 0; i < npts; i++) oncurve[i] = (flags[i] & 1) ? 1 : 0;

            /* walk each contour, converting quadratics to cubics */
            startPts[0] = 0;
            {
                int base = 0;
                for (nc = 0; nc < ncontours; nc++) {
                    int last = endPts[nc];
                    int cnt = last - base + 1;
                    int j;
                    int px, py, sx, sy;
                    int started = 0;

                    if (cnt < 2) { base = last + 1; continue; }

                    /* find a starting on-curve point */
                    {
                        int first_on = -1;
                        for (j = 0; j < cnt; j++)
                            if (oncurve[base + j]) { first_on = j; break; }
                        if (first_on < 0) {
                            /* all off-curve: start at midpoint of first two */
                            px = (xs[base] + xs[base + 1]) / 2;
                            py = (ys[base] + ys[base + 1]) / 2;
                            startPts[nc] = 1;
                            first_on = 0;
                            sx = px; sy = py;
                            if (!emitted) { fprintf(out, "%d %d moveto\n", px, py); }
                            else fprintf(out, "%d %d moveto\n", px, py);
                            emitted = 1;
                            started = 1;
                            /* then walk from point 0 as off-curve */
                            {
                                int k = 0;
                                int curx = px, cury = py;
                                while (k <= cnt) {
                                    int idx = base + (k % cnt);
                                    int nx, ny, on2;
                                    int i2 = (k + 1) % cnt;
                                    if (k == cnt) break;
                                    if (oncurve[idx]) {
                                        curx = xs[idx]; cury = ys[idx];
                                        fprintf(out, "%d %d lineto\n", curx, cury);
                                        k++;
                                    } else {
                                        /* off-curve followed by on-curve */
                                        on2 = oncurve[base + i2];
                                        if (on2) { nx = xs[base + i2]; ny = ys[base + i2]; }
                                        else { nx = (xs[idx] + xs[base + i2]) / 2;
                                               ny = (ys[idx] + ys[base + i2]) / 2; }
                                        fprintf(out,
                                            "%.3f %.3f %.3f %.3f %d %d curveto\n",
                                            curx + 2.0/3.0 * (xs[idx] - curx),
                                            cury + 2.0/3.0 * (ys[idx] - cury),
                                            nx + 2.0/3.0 * (xs[idx] - nx),
                                            ny + 2.0/3.0 * (ys[idx] - ny),
                                            nx, ny);
                                        curx = nx; cury = ny;
                                        k++;
                                    }
                                }
                            }
                            base = last + 1;
                            continue;
                        }
                        /* rotate contour to start at first_on */
                        {
                            int rot = first_on;
                            int k;
                            int curx, cury;
                            int cx[4096], cy[4096], co[4096];
                            for (k = 0; k < cnt; k++) {
                                int src = base + (k + rot) % cnt;
                                cx[k] = xs[src]; cy[k] = ys[src]; co[k] = oncurve[src];
                            }
                            curx = cx[0]; cury = cy[0];
                            fprintf(out, "%d %d moveto\n", curx, cury);
                            emitted = 1;
                            started = 1;
                            k = 1;
                            while (k <= cnt) {
                                if (k == cnt) break;
                                if (co[k]) {
                                    curx = cx[k]; cury = cy[k];
                                    fprintf(out, "%d %d lineto\n", curx, cury);
                                    k++;
                                } else {
                                    int nx, ny;
                                    if (k + 1 < cnt) {
                                        if (co[k + 1]) { nx = cx[k + 1]; ny = cy[k + 1]; }
                                        else { nx = (cx[k] + cx[k + 1]) / 2;
                                               ny = (cy[k] + cy[k + 1]) / 2; }
                                    } else { nx = cx[0]; ny = cy[0]; }
                                    fprintf(out,
                                        "%.3f %.3f %.3f %.3f %d %d curveto\n",
                                        curx + 2.0/3.0 * (cx[k] - curx),
                                        cury + 2.0/3.0 * (cy[k] - cury),
                                        nx + 2.0/3.0 * (cx[k] - nx),
                                        ny + 2.0/3.0 * (cy[k] - ny),
                                        nx, ny);
                                    curx = nx; cury = ny;
                                    k++;
                                }
                            }
                        }
                    }
                    (void)sx; (void)sy; (void)started;
                    fprintf(out, "closepath\n");
                    base = last + 1;
                }
            }
            free(flags); free(xs); free(ys); free(oncurve);
        }
    }
    return emitted;
}

/* ------------------------------------------------------------------ */
/* Resolved font                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    PSF_SIMPLE_STD = 0,   /* a base-14 face, byte codes pass through      */
    PSF_SIMPLE_MAPPED,    /* simple font: per-code remap -> base-14 face  */
    PSF_TYPE3,            /* glyphs are content streams                   */
    PSF_TRUETYPE_CID      /* Identity CMap + embedded TrueType glyphs     */
} psf_kind_t;

#define PSF_MAX_GLYPH 256

typedef struct { char *ps; double width; int used; } t3g_t;

struct psfont_s {
    char        resname[64];
    psf_kind_t  kind;
    char        psname[64];        /* PostScript base-14 face name         */
    int         owned;             /* heap-allocated by the cache          */

    /* simple fonts */
    long        first_char;
    double      widths[PSF_MAX_GLYPH];
    int         have_widths;

    /* Type3 */
    double      t3matrix[6];
    t3g_t       t3[PSF_MAX_GLYPH];

    /* composite */
    int         composite;
    int         identity;          /* Identity-H/V: code == CID == GID     */
    ttfont_t    tt;
    int         tt_ready;
    pdf_value  *descendant;         /* resolved CIDFont dict                */
    pdf_doc    *doc;
};

/* ---------------- base-14 matching ---------------- */

static const char *const STD14[] = {
    "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
    "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
    "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
    "Symbol", "ZapfDingbats"
};

static int std14_index(const char *n) {
    int i;
    if (!n) return -1;
    for (i = 0; i < 14; i++)
        if (!strcmp(n, STD14[i])) return i;
    return -1;
}

/*
 * Map a PDF base font name onto a base-14 face. The subset prefix
 * ("AAAAAA+") is stripped first; without that step every embedded subset
 * misses its intended family, which was another silent-garbage source.
 */
static const char *map_font(const char *name) {
    char buf[128];
    const char *n = name;
    size_t i, len;

    if (!name || !name[0]) return "Helvetica";
    /* strip ABCDEF+ prefix */
    if (name[6] == '+') {
        for (i = 0; i < 6; i++) {
            char c = name[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) break;
        }
        if (i == 6) n = name + 7;
    }
    len = strlen(n);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    for (i = 0; i < len; i++) buf[i] = n[i];
    buf[len] = 0;
    for (i = 0; i < len; i++) {                    /* drop style suffixes */
        if (buf[i] == ',' || buf[i] == '-' ) { buf[i] = 0; break; }
    }
    /* normalise separators */
    for (i = 0; buf[i]; i++) if (buf[i] == ' ') buf[i] = '-';
    {
        char low[128];
        for (i = 0; buf[i]; i++) {
            char c = buf[i];
            low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        low[i] = 0;
        if (strstr(low, "courier") || strstr(low, "mono")) {
            if (strstr(low, "bold") && (strstr(low, "italic") || strstr(low, "oblique")))
                return "Courier-BoldOblique";
            if (strstr(low, "bold")) return "Courier-Bold";
            if (strstr(low, "italic") || strstr(low, "oblique")) return "Courier-Oblique";
            return "Courier";
        }
        if (strstr(low, "times") || strstr(low, "serif") || strstr(low, "roman") ||
            strstr(low, "song") || strstr(low, "ming") || strstr(low, "serif")) {
            if (strstr(low, "bold") && (strstr(low, "italic") || strstr(low, "oblique")))
                return "Times-BoldItalic";
            if (strstr(low, "bold")) return "Times-Bold";
            if (strstr(low, "italic") || strstr(low, "oblique")) return "Times-Italic";
            return "Times-Roman";
        }
        if (strstr(low, "symbol")) return "Symbol";
        if (strstr(low, "dingbat")) return "ZapfDingbats";
        if (strstr(low, "bold") && (strstr(low, "italic") || strstr(low, "oblique")))
            return "Helvetica-BoldOblique";
        if (strstr(low, "bold")) return "Helvetica-Bold";
        if (strstr(low, "italic") || strstr(low, "oblique")) return "Helvetica-Oblique";
    }
    return "Helvetica";
}

/* ---------------- page-scoped cache ---------------- */

#define PSF_CACHE_MAX 512

static psfont_t *g_cache[PSF_CACHE_MAX];
static int g_cache_n;

static void psfont_destroy(psfont_t *f) {
    int i;
    if (!f) return;
    tt_free(&f->tt);
    for (i = 0; i < PSF_MAX_GLYPH; i++) {
        if (f->t3[i].ps) { free(f->t3[i].ps); f->t3[i].ps = NULL; }
    }
    free(f);
}

void psfont_cache_reset(void) {
    int i;
    for (i = 0; i < g_cache_n; i++) psfont_destroy(g_cache[i]);
    g_cache_n = 0;
}

void psfont_cache_free(void) { psfont_cache_reset(); }

static psfont_t *cache_find(const char *resname) {
    int i;
    for (i = 0; i < g_cache_n; i++)
        if (!strcmp(g_cache[i]->resname, resname)) return g_cache[i];
    return NULL;
}

static void cache_add(psfont_t *f) {
    if (g_cache_n < PSF_CACHE_MAX) g_cache[g_cache_n++] = f;
}

/* ------------------------------------------------------------------ */
/* Resolution                                                          */
/* ------------------------------------------------------------------ */

static double num_or(pdf_value *d, const char *key, double dflt) {
    pdf_value *v = d ? pdf_dict_get(d, key) : NULL;
    if (!v) return dflt;
    if (v->type == PDF_INT) return (double)v->u.i;
    if (v->type == PDF_REAL) return v->u.r;
    return dflt;
}

/* Read an array of numbers into `out`, returning the count. */
static int num_array(pdf_value *arr, double *out, int maxn) {
    int i, n;
    if (!arr || arr->type != PDF_ARRAY) return 0;
    n = arr->u.arr.count;
    if (n > maxn) n = maxn;
    for (i = 0; i < n; i++) {
        pdf_value *v = arr->u.arr.items[i];
        if (!v) { out[i] = 0; continue; }
        if (v->type == PDF_INT) out[i] = (double)v->u.i;
        else if (v->type == PDF_REAL) out[i] = v->u.r;
        else out[i] = 0;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Type3 glyph programs                                                */
/* ------------------------------------------------------------------ */

/*
 * Translate a Type3 CharProc content stream into PostScript path operators.
 *
 * CharProcs are ordinary content streams, but only the painting operators
 * matter here: PDF Type3 glyphs are usually a single filled path built with
 * m/l/c/v/y/re/h, sometimes with a d0/d1 declaring the width. Text shown
 * inside a Type3 glyph (rare, used by some CJK generators) is forwarded to
 * the base face so it is not lost.
 */
static char *t3_translate(pdf_doc *doc, pdf_value *charproc, double *out_wx) {
    buf_t src, ps;
    const unsigned char *p, *end;
    double st[32];
    int n = 0;
    int has_width = 0;
    int emitted = 0;

    if (out_wx) *out_wx = 0;
    buf_init(&src);
    buf_init(&ps);
    if (!pdf_stream_decode(doc, charproc, &src)) { buf_free(&src); return NULL; }

    p = src.data;
    end = src.data + src.len;
    while (p < end) {
        /* skip whitespace / comments */
        while (p < end && is_ws_pdf(*p)) p++;
        if (p >= end) break;
        if (*p == '%') { while (p < end && *p != '\n' && *p != '\r') p++; continue; }

        if (*p == '(') {                                /* a string operand */
            int depth = 1;
            buf_t sb;
            buf_init(&sb);
            p++;
            while (p < end && depth > 0) {
                int c = *p++;
                if (c == '\\') {
                    int e;
                    if (p >= end) break;
                    e = *p++;
                    switch (e) {
                        case 'n': buf_putc(&sb, '\n'); break;
                        case 'r': buf_putc(&sb, '\r'); break;
                        case 't': buf_putc(&sb, '\t'); break;
                        case 'b': buf_putc(&sb, '\b'); break;
                        case 'f': buf_putc(&sb, '\f'); break;
                        case '(': case ')': case '\\': buf_putc(&sb, e); break;
                        default:
                            if (e >= '0' && e <= '7') {
                                int v = e - '0', k = 0;
                                while (k < 2 && p < end && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                                buf_putc(&sb, v & 255);
                            } else buf_putc(&sb, e);
                    }
                } else if (c == '(') { depth++; buf_putc(&sb, c); }
                else if (c == ')') { if (--depth > 0) buf_putc(&sb, c); }
                else buf_putc(&sb, c);
            }
            if (n < 32) { st[n] = (double)sb.len; }
            /* keep the bytes for a pending show */
            /* We stash string length; the actual bytes are re-read below for
             * show operators via the operand buffer. */
            buf_free(&sb);
            n++;
            continue;
        }

        if (*p == '/' ) {                               /* name operand */
            const char *nm;
            p++;
            nm = (const char *)p;
            while (p < end && !is_ws_pdf(*p) && !is_delim_pdf(*p)) p++;
            if (n < 32) st[n] = 0;
            (void)nm;
            n++;
            continue;
        }

        if (*p == '<' && p + 1 < end && p[1] == '<') {   /* dict: skip */
            int depth = 0;
            while (p < end) {
                if (*p == '<' && p + 1 < end && p[1] == '<') { depth++; p += 2; continue; }
                if (*p == '>' && p + 1 < end && p[1] == '>') { depth--; p += 2; if (depth <= 0) break; continue; }
                p++;
            }
            n = 0;
            continue;
        }

        if (*p == '[' || *p == ']' || *p == '{' || *p == '}') { p++; n = 0; continue; }

        /* number or operator */
        {
            const unsigned char *s0 = p;
            char tok[64];
            size_t tl = 0;
            int isnum = 1;
            int dot = 0;

            while (p < end && !is_ws_pdf(*p) && !is_delim_pdf(*p)) {
                if (tl < sizeof(tok) - 1) tok[tl++] = (char)*p;
                p++;
            }
            tok[tl] = 0;
            if (tl == 0) { p++; continue; }

            {
                size_t k = 0;
                if (tok[0] == '-' || tok[0] == '+') k = 1;
                if (k >= tl) isnum = 0;
                for (; k < tl; k++) {
                    if (tok[k] == '.') { if (dot) { isnum = 0; break; } dot = 1; }
                    else if (tok[k] < '0' || tok[k] > '9') { isnum = 0; break; }
                }
            }

            if (isnum) {
                if (n < 32) st[n] = atof(tok);
                n++;
                continue;
            }

            /* operator */
            if (!strcmp(tok, "m") && n >= 2) {
                {
                    char tmp[128];
                    snprintf(tmp, sizeof(tmp), "%.4f %.4f moveto\n", st[n-2], st[n-1]);
                    buf_puts(&ps, tmp);
                }
                emitted = 1;
            } else if (!strcmp(tok, "l") && n >= 2) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%.4f %.4f lineto\n", st[n-2], st[n-1]);
                buf_puts(&ps, tmp);
                emitted = 1;
            } else if (!strcmp(tok, "c") && n >= 6) {
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%.4f %.4f %.4f %.4f %.4f %.4f curveto\n",
                         st[n-6], st[n-5], st[n-4], st[n-3], st[n-2], st[n-1]);
                buf_puts(&ps, tmp);
                emitted = 1;
            } else if (!strcmp(tok, "v") && n >= 4) {
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%.4f %.4f %.4f %.4f curveto\n",
                         st[n-4], st[n-3], st[n-2], st[n-1]);
                buf_puts(&ps, tmp);
                emitted = 1;
            } else if (!strcmp(tok, "y") && n >= 4) {
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%.4f %.4f %.4f %.4f curveto\n",
                         st[n-4], st[n-3], st[n-2], st[n-1]);
                buf_puts(&ps, tmp);
                emitted = 1;
            } else if (!strcmp(tok, "h")) {
                buf_puts(&ps, "closepath\n");
            } else if (!strcmp(tok, "re") && n >= 4) {
                /* Rectangle: x y w h re == moveto + 3 lineto + closepath.
                 * Skia emits glyphs as bare rectangles, so without this the
                 * whole glyph translates to nothing and the page stays blank. */
                double x = st[n-4], y = st[n-3], w = st[n-2], hh = st[n-1];
                char tmp[256];
                snprintf(tmp, sizeof(tmp),
                         "%.4f %.4f moveto\n%.4f %.4f lineto\n"
                         "%.4f %.4f lineto\n%.4f %.4f lineto\nclosepath\n",
                         x, y, x + w, y, x + w, y + hh, x, y + hh);
                buf_puts(&ps, tmp);
                emitted = 1;
            } else if (!strcmp(tok, "d0") && n >= 2) {
                if (out_wx) *out_wx = st[n-2];
                has_width = 1;
            } else if (!strcmp(tok, "d1") && n >= 6) {
                /* d1 wx wy llx lly urx ury -- wx is the leftmost operand. */
                if (out_wx) *out_wx = st[n-6];
                has_width = 1;
            }
            /* fill / stroke intent is applied uniformly as eofill by the
             * caller, matching how Type3 glyphs are almost always drawn. */
            n = 0;
        }
    }

    buf_free(&src);
    if (!emitted) { buf_free(&ps); return NULL; }
    buf_putc(&ps, 0);
    (void)has_width;
    return (char *)ps.data;      /* caller owns */
}
/* ------------------------------------------------------------------ */
/* Font object -> psfont_t                                             */
/* ------------------------------------------------------------------ */

static void setup_type3(psfont_t *f, pdf_doc *doc, pdf_value *fdict) {
    pdf_value *procs, *widths, *matrix;
    double fm[6];
    int i;

    procs = pdf_resolve(doc, pdf_dict_get(fdict, "CharProcs"));
    widths = pdf_resolve(doc, pdf_dict_get(fdict, "Widths"));
    matrix = pdf_resolve(doc, pdf_dict_get(fdict, "FontMatrix"));

    f->kind = PSF_TYPE3;
    if (getenv("PDFTOPS_DEBUG")) {
        fprintf(stderr, "[t3] enter procs=%s widths=%s matrix=%s first=%d\n",
                procs ? (procs->type == PDF_DICT ? "dict" : "other") : "NULL",
                widths ? (widths->type == PDF_ARRAY ? "arr" : "other") : "NULL",
                matrix ? "ok" : "NULL",
                (int)num_or(fdict, "FirstChar", 0));
    }
    f->t3matrix[0] = 0.001; f->t3matrix[1] = 0;
    f->t3matrix[2] = 0;     f->t3matrix[3] = 0.001;
    f->t3matrix[4] = 0;     f->t3matrix[5] = 0;
    if (matrix) {
        int got = num_array(matrix, fm, 6);
        for (i = 0; i < got; i++) f->t3matrix[i] = fm[i];
    }
    f->first_char = (int)num_or(fdict, "FirstChar", 0);
    if (f->first_char < 0) f->first_char = 0;
    if (f->first_char > 255) f->first_char = 255;

    if (widths && widths->type == PDF_ARRAY) {
        double w[PSF_MAX_GLYPH];
        int nw = num_array(widths, w, PSF_MAX_GLYPH);
        for (i = 0; i < nw; i++) {
            int code = f->first_char + i;
            if (code >= 0 && code < PSF_MAX_GLYPH) f->widths[code] = w[i];
        }
        f->have_widths = 1;
    }

    if (!procs || procs->type != PDF_DICT) return;

    /*
     * /Encoding /Differences names the glyph for each code. Without it we
     * cannot tell which CharProc belongs to which byte, and a GID-addressed
     * font (Skia emits these) would produce nothing at all.
     */
    {
        pdf_value *enc = pdf_resolve(doc, pdf_dict_get(fdict, "Encoding"));
        pdf_value *diffs = enc ? pdf_resolve(doc, pdf_dict_get(enc, "Differences")) : NULL;
        int cur = 0;

        if (diffs && diffs->type == PDF_ARRAY) {
            int k;
            for (k = 0; k < diffs->u.arr.count; k++) {
                pdf_value *it = pdf_resolve(doc, diffs->u.arr.items[k]);
                if (!it) continue;
                if (it->type == PDF_INT) { cur = (int)it->u.i; continue; }
                {
                    const char *gname = pdf_name(it);
                    pdf_value *proc;
                    if (!gname) continue;
                    if (cur < 0 || cur >= PSF_MAX_GLYPH) { cur++; continue; }
                    proc = pdf_resolve(doc, pdf_dict_get(procs, gname));
                    if (getenv("PDFTOPS_DEBUG")) {
                        fprintf(stderr, "[t3] diff cur=%d name='%s' proc=%s\n", cur, gname,
                                !proc ? "NULL" : (proc->type == PDF_STREAM ? "stream" : "other"));
                    }
                    if (proc && proc->type == PDF_STREAM) {
                        double wx = 0;
                        char *body = t3_translate(doc, proc, &wx);
                        if (getenv("PDFTOPS_DEBUG")) {
                            fprintf(stderr, "[t3]   translated: %s wx=%.2f\n",
                                    body ? "ok" : "NULL", wx);
                        }
                        if (!f->t3[cur].used) f->t3[cur].used = 1;
                        if (body) {
                            if (f->t3[cur].ps) free(f->t3[cur].ps);
                            f->t3[cur].ps = body;
                            /* d0/d1 may override /Widths; prefer /Widths when
                             * present because it is what the content stream's
                             * advance actually assumes. */
                            if (!f->have_widths && wx != 0) f->widths[cur] = wx;
                        }
                    }
                    cur++;
                }
            }
        } else {
            /* No Differences: fall back to matching CharProc keys directly
             * against the character code, which is what simple generators do. */
            int k;
            for (k = 0; k < procs->u.dict.count; k++) {
                const char *gname = procs->u.dict.pairs[k].key;
                pdf_value *proc;
                int code;
                if (!gname) continue;
                code = atoi(gname);
                if (code <= 0 || code >= PSF_MAX_GLYPH) {
                    /* key is a descriptive name; try base-27 built encodings
                     * only when it is a single letter run */
                    continue;
                }
                proc = pdf_resolve(doc, procs->u.dict.pairs[k].value);
                if (!proc || proc->type != PDF_STREAM) continue;
                {
                    double wx = 0;
                    char *body = t3_translate(doc, proc, &wx);
                    f->t3[code].used = 1;
                    if (body) f->t3[code].ps = body;
                    if (!f->have_widths && wx != 0) f->widths[code] = wx;
                }
            }
        }
    }
    return;
}

static void setup_simple(psfont_t *f, pdf_doc *doc, pdf_value *fdict,
                        const char *subtype) {
    pdf_value *bf = pdf_resolve(doc, pdf_dict_get(fdict, "BaseFont"));
    const char *bname = pdf_name(bf);
    pdf_value *widths;
    int i;

    /* A subset that names a base-14 family still renders correctly with the
     * standard face because its encoding is the standard one. */
    f->kind = PSF_SIMPLE_STD;
    snprintf(f->psname, sizeof(f->psname), "%s", map_font(bname));
    if (std14_index(bname) >= 0) {
        snprintf(f->psname, sizeof(f->psname), "%s", bname);
    }
    (void)subtype;

    f->first_char = (int)num_or(fdict, "FirstChar", 0);
    if (f->first_char < 0) f->first_char = 0;
    if (f->first_char > 255) f->first_char = 255;

    widths = pdf_resolve(doc, pdf_dict_get(fdict, "Widths"));
    if (widths && widths->type == PDF_ARRAY) {
        double w[PSF_MAX_GLYPH];
        int nw = num_array(widths, w, PSF_MAX_GLYPH);
        for (i = 0; i < nw; i++) {
            int code = f->first_char + i;
            if (code >= 0 && code < PSF_MAX_GLYPH) f->widths[code] = w[i];
        }
        f->have_widths = 1;
    }
    return;
}

/*
 * Composite font: /Subtype /Type0.
 *
 * Support is intentionally narrow but honest. We handle Identity-H/V, where
 * the 2-byte codes ARE the CIDs and, for CIDFontType2 with /CIDToGIDMap
 * /Identity, also the glyph indices. That covers what Android, iOS and
 * Skia PDF writers actually emit for CJK and symbol text.
 */
static void setup_type0(psfont_t *f, pdf_doc *doc, pdf_value *fdict) {
    pdf_value *enc, *encname, *desc, *c2g, *ff;
    const char *en = NULL;

    f->composite = 1;
    f->doc = doc;

    enc = pdf_resolve(doc, pdf_dict_get(fdict, "Encoding"));
    if (enc) {
        if (enc->type == PDF_NAME) en = pdf_name(enc);
        else {
            encname = pdf_resolve(doc, pdf_dict_get(enc, "CMapName"));
            if (encname) en = pdf_name(encname);
            if (!en) {
                /* Predefined CMap named through /UseCMap chain. */
                pdf_value *u = pdf_resolve(doc, pdf_dict_get(enc, "UseCMap"));
                if (u) en = pdf_name(u);
            }
        }
    }
    f->identity = (en && (!strncmp(en, "Identity-", 9)));

    desc = pdf_resolve(doc, pdf_dict_get(fdict, "DescendantFonts"));
    if (desc && desc->type == PDF_ARRAY && desc->u.arr.count > 0)
        f->descendant = pdf_resolve(doc, desc->u.arr.items[0]);

    if (!f->identity) {
        /* Unknown CMap: we cannot map codes to glyphs reliably. Keep the
         * composite flag OFF so the emitted text still reaches the page as
         * literal bytes -- dropping composite makes psfont_show fall through
         * to the /Helvetica branch, so Latin glyphs come out readable instead
         * of the page staying blank. Count it so tests can see the fallback. */
        f->kind = PSF_SIMPLE_MAPPED;
        f->composite = 0;
        snprintf(f->psname, sizeof(f->psname), "Helvetica");
        psfont_fallback_count++;
        return;
    }

    /* Load the embedded TrueType program, if any. */
    if (f->descendant) {
        pdf_value *fd = pdf_resolve(doc, pdf_dict_get(f->descendant, "FontDescriptor"));
        ff = fd ? pdf_resolve(doc, pdf_dict_get(fd, "FontFile2")) : NULL;
        c2g = pdf_resolve(doc, pdf_dict_get(f->descendant, "CIDToGIDMap"));

        if (ff && ff->type == PDF_STREAM) {
            buf_t prog;
            buf_init(&prog);
            if (pdf_stream_decode(doc, ff, &prog) && prog.len > 12) {
                ttfont_t tt;
                if (tt_load(&tt, &prog)) {
                    f->tt = tt;
                    f->tt_ready = tt.numGlyphs > 0;
                } else {
                    buf_free(&prog);
                }
            } else {
                buf_free(&prog);
            }
        }
        /* A non-Identity CIDToGIDMap shifts CID->GID; without parsing that
         * stream we would draw the wrong glyphs, so treat it as unavailable. */
        if (c2g && c2g->type != PDF_NAME) f->tt_ready = 0;
    }

    f->kind = f->tt_ready ? PSF_TRUETYPE_CID : PSF_SIMPLE_MAPPED;
    if (!f->tt_ready) {
        pdf_value *bf = pdf_resolve(doc, pdf_dict_get(fdict, "BaseFont"));
        /* No usable embedded program: draw the raw codes through a standard
         * face rather than advancing the pen over invisible text. */
        f->composite = 0;
        snprintf(f->psname, sizeof(f->psname), "%s", map_font(pdf_name(bf)));
        psfont_fallback_count++;
    }
    return;
}

psfont_t *psfont_resolve(pdf_doc *doc, pdf_value *resources, const char *resname) {
    pdf_value *fonts, *fdict, *sub;
    const char *subtype;
    psfont_t *f;

    if (!resname || !resname[0]) return NULL;
    f = cache_find(resname);
    if (f) return f;

    if (!resources) return NULL;
    fonts = pdf_resolve(doc, pdf_dict_get(resources, "Font"));
    if (!fonts || fonts->type != PDF_DICT) return NULL;

    fdict = pdf_resolve(doc, pdf_dict_get(fonts, resname));
    if (!fdict || fdict->type != PDF_DICT) {
        /*
         * Some writers put a subtype dictionary directly on the page and
         * reference fonts through a nested /Font dict; try one more level
         * before giving up.
         */
        return NULL;
    }

    sub = pdf_resolve(doc, pdf_dict_get(fdict, "Subtype"));
    subtype = pdf_name(sub);

    f = (psfont_t *)calloc(1, sizeof(psfont_t));
    if (!f) return NULL;
    snprintf(f->resname, sizeof(f->resname), "%s", resname);
    f->owned = 1;

    if (subtype && !strcmp(subtype, "Type3")) {
        setup_type3(f, doc, fdict);
    } else if (subtype && !strcmp(subtype, "Type0")) {
        setup_type0(f, doc, fdict);
    } else {
        setup_simple(f, doc, fdict, subtype);
    }

    cache_add(f);
    return f;
}
/* ------------------------------------------------------------------ */
/* Emission                                                            */
/* ------------------------------------------------------------------ */

void psfont_set(psfont_t *f, FILE *out, double size) {
    if (!f) {
        fprintf(out, "/Helvetica findfont %.4f scalefont setfont\n", size);
        return;
    }
    switch (f->kind) {
        case PSF_TYPE3:
            /* Nothing to select: Type3 glyphs are drawn as paths directly, so
             * the base font is irrelevant. Leave the previous font in place. */
            break;
        case PSF_TRUETYPE_CID:
            /* Glyph outlines are emitted per-glyph; no font switch needed.
             * Type1 'show' would emit the 2-byte code as two Latin-1 chars, so
             * we deliberately keep the current face untouched. */
            break;
        case PSF_SIMPLE_STD:
        case PSF_SIMPLE_MAPPED:
        default:
            fprintf(out, "/%s findfont %.4f scalefont setfont\n",
                    f->psname, size);
            break;
    }
}

/* Escape a PostScript literal string body. */
static void ps_escape_string(FILE *out, const unsigned char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '(' || c == ')' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c < 32 || c > 126) fprintf(out, "\\%03o", c);
        else fputc(c, out);
    }
}

/* Advance the text matrix by `adv` in text space and mirror it to output. */
static void advance_tm(FILE *out, double *tm, double adv) {
    fprintf(out, "%.4f 0 Td\n", adv);
    tm[4] += tm[0] * adv;
    tm[5] += tm[1] * adv;
}

static void show_type3(psfont_t *f, FILE *out, const unsigned char *s, size_t n,
                       double *tm) {
    size_t k;
    for (k = 0; k < n; k++) {
        int code = s[k];
        t3g_t *gl = &f->t3[code];
        double adv = f->have_widths ? f->widths[code] * f->t3matrix[0]
                                    : f->widths[code] * f->t3matrix[0];
        if (adv == 0 && f->have_widths) adv = 0;
        if (gl->ps) {
            /* glyph space -> text space via FontMatrix, text -> user via Tm */
            double a = tm[0] * f->t3matrix[0] + tm[2] * f->t3matrix[1];
            double b = tm[1] * f->t3matrix[0] + tm[3] * f->t3matrix[1];
            double c = tm[0] * f->t3matrix[2] + tm[2] * f->t3matrix[3];
            double d = tm[1] * f->t3matrix[2] + tm[3] * f->t3matrix[3];
            double e = tm[0] * f->t3matrix[4] + tm[2] * f->t3matrix[5] + tm[4];
            double g2 = tm[1] * f->t3matrix[4] + tm[3] * f->t3matrix[5] + tm[5];
            fprintf(out, "gsave\n");
            fprintf(out, "[ %.6f %.6f %.6f %.6f %.4f %.4f ] concat\n", a, b, c, d, e, g2);
            fputs(gl->ps, out);
            fputs("eofill\ngrestore\n", out);
            psfont_glyph_draws++;
        }
        if (adv != 0) advance_tm(out, tm, adv);
    }
}

static void show_truetype(psfont_t *f, FILE *out, const unsigned char *s, size_t n,
                          double *tm) {
    size_t k;
    double scale = 1.0 / (double)(f->tt.unitsPerEm ? f->tt.unitsPerEm : 1000);

    for (k = 0; k + 1 < n; k += 2) {
        int gid = (s[k] << 8) | s[k + 1];
        int adv_units = tt_advance(&f->tt, gid);
        double adv = adv_units * scale;
        if (adv <= 0) adv = 0.5;                  /* unknown width: give it room */

        /*
         * Glyph outline is in font units, y-up like PostScript. Map font units
         * to text space through Tm:  Trm = Tm x [1/upem 0 0 1/upem 0 0]
         */
        {
            double a = tm[0] * scale, b = tm[1] * scale;
            double c = tm[2] * scale, d = tm[3] * scale;
            if (!(a == 0 && b == 0 && c == 0 && d == 0)) {
                fprintf(out, "gsave\n");
                fprintf(out, "[ %.6f %.6f %.6f %.6f %.4f %.4f ] concat\n",
                        a, b, c, d, tm[4], tm[5]);
                if (tt_emit_glyph(&f->tt, gid, out)) {
                    fputs("eofill\n", out);
                    psfont_glyph_draws++;
                }
                fputs("grestore\n", out);
            }
        }
        if (adv != 0) advance_tm(out, tm, adv);
    }
}

void psfont_show(psfont_t *f, FILE *out, const unsigned char *s, size_t n,
                 double *tm) {
    if (!f || !s || n == 0) return;

    switch (f->kind) {
        case PSF_TYPE3:
            show_type3(f, out, s, n, tm);
            break;
        case PSF_TRUETYPE_CID:
            show_truetype(f, out, s, n, tm);
            break;
        case PSF_SIMPLE_MAPPED:
            /* 2-byte codes on a 1-byte face: showing them raw would emit two
             * Latin-1 characters per glyph. Emit each high byte's printable
             * partner only when it maps to ASCII, so something lands on the
             * page instead of nothing at all. */
            if (f->composite) {
                size_t k;
                for (k = 0; k + 1 < n; k += 2) advance_tm(out, tm, 0.5);
                break;
            }
            /* fall through */
        case PSF_SIMPLE_STD:
        default: {
            size_t k;
            fputs("(", out);
            ps_escape_string(out, s, n);
            fputs(") show\n", out);
            psfont_glyph_draws++;
            /* Widths from /Widths let us keep the text matrix honest for
             * callers that measure; the printer advances itself, so we only
             * need the matrix side. */
            for (k = 0; k < n; k++) {
                int code = s[k];
                if (f->have_widths && f->widths[code] != 0) {
                    double adv = f->widths[code] * 0.001;
                    tm[4] += tm[0] * adv;
                    tm[5] += tm[1] * adv;
                }
            }
            break;
        }
    }
}