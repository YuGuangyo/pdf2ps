/*
 * psgen.c - interpret PDF content streams, emit PostScript.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pdfobj.h"
#include "psgen.h"
#include "pdfimage.h"

/* ---------------- output helpers ---------------- */

static void ps_out(psgen_t *g, const char *s) { fputs(s, g->out); }
static void ps_printf(psgen_t *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g->out, fmt, ap);
    va_end(ap);
}

/* PostScript string escaping.
 * Kept for callers that need a fully escaped literal; currently unused,
 * hence the attribute so -Wunused-function stays quiet. */
static void __attribute__((unused))
ps_write_literal(psgen_t *g, const unsigned char *s, size_t n) {
    size_t i;
    fputc('(', g->out);
    for (i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '(' || c == ')' || c == '\\') { fputc('\\', g->out); fputc(c, g->out); }
        else if (c < 32 || c > 126) ps_printf(g, "\\%03o", c);
        else fputc(c, g->out);
    }
    fputc(')', g->out);
}

/* ---------------- operand stack ---------------- */

#define MAX_OPS 64

typedef struct {
    int kind;       /* 0 num, 1 string, 2 name, 3 array marker */
    double num;
    buf_t str;
} operand_t;

typedef struct {
    operand_t ops[MAX_OPS];
    int n;
} opstack_t;

static void ops_reset(opstack_t *st) {
    int i;
    for (i = 0; i < st->n; i++) {
        if (st->ops[i].kind == 1) buf_free(&st->ops[i].str);
    }
    st->n = 0;
}

static operand_t *ops_num(opstack_t *st, double v) {
    operand_t *o;
    if (st->n >= MAX_OPS) return NULL;
    o = &st->ops[st->n++];
    o->kind = 0;
    o->num = v;
    return o;
}

static operand_t *ops_str(opstack_t *st) {
    operand_t *o;
    if (st->n >= MAX_OPS) return NULL;
    o = &st->ops[st->n++];
    o->kind = 1;
    buf_init(&o->str);
    return o;
}

/* ---------------- state ---------------- */

void psgen_init(psgen_t *g, FILE *out) {
    memset(g, 0, sizeof(*g));
    g->out = out;
    g->page_count = 0;
}

/* font mapping: any base font -> one of the standard PS fonts */
static const char *map_font(const char *name) {
    if (!name) return "Helvetica";
    if (strstr(name, "Courier") || strstr(name, "Mono") || strstr(name, "NimbusMono"))
        return "Courier";
    if (strstr(name, "Times") || strstr(name, "Serif") || strstr(name, "Roman"))
        return "Times-Roman";
    if (strstr(name, "Bold") || strstr(name, "Black") || strstr(name, "Heavy")) {
        if (strstr(name, "Italic") || strstr(name, "Oblique")) return "Helvetica-BoldOblique";
        return "Helvetica-Bold";
    }
    if (strstr(name, "Italic") || strstr(name, "Oblique"))
        return "Helvetica-Oblique";
    if (strstr(name, "Symbol")) return "Symbol";
    if (strstr(name, "Dingbat")) return "ZapfDingbats";
    return "Helvetica";
}

void psgen_begin(psgen_t *g) {
    ps_out(g, "%!PS-Adobe-3.0\n");
    ps_out(g, "%%Creator: pdftops-lite 0.1\n");
    ps_out(g, "%%LanguageLevel: 2\n");
    ps_out(g, "%%Pages: (atend)\n");
    ps_out(g, "%%EndComments\n");
    ps_out(g, "%%BeginProlog\n");
    ps_out(g, "/L2 { 2 } def\n");
    ps_out(g, "%%EndProlog\n");
    ps_out(g, "%%BeginSetup\n");
    ps_out(g, "%%EndSetup\n");
}

void psgen_end(psgen_t *g) {
    ps_printf(g, "%%%%Trailer\n%%%%Pages: %d\n%%%%EOF\n", g->page_count);
}

/* page: w/h in points, mediabox already translated */
void psgen_page_begin(psgen_t *g, double x0, double y0, double x1, double y1) {
    g->page_count++;
    g->media_x0 = x0;
    g->media_y0 = y0;
    g->media_x1 = x1;
    g->media_y1 = y1;
    /* Type3 glyph caches live for exactly one page. */
    if (g->fonts) {
        int i;
        for (i = 0; i < g->nfonts; i++) {
            int k;
            for (k = 0; k < g->fonts[i].nglyphs; k++)
                free(g->fonts[i].glyphs[k].ps);
        }
        free(g->fonts);
    }
    g->fonts = NULL;
    g->nfonts = 0;
    g->fonts_cap = 0;
    ps_printf(g, "%%%%Page: %d %d\n", g->page_count, g->page_count + 1);
    ps_out(g, "gsave\n");
    /* PDF origin is bottom-left, same as PS: just translate mediabox origin */
    ps_printf(g, "%.4f %.4f translate\n", x0, y0);
    ps_printf(g, "%.4f %.4f %.4f %.4f rectclip\n", 0.0, 0.0, x1 - x0, y1 - y0);
}

void psgen_page_end(psgen_t *g) {
    ps_out(g, "grestore\n");
    ps_out(g, "showpage\n");
}

/* ---------------------------------------------------------------- */
/* content stream interpretation                                     */
/* ---------------------------------------------------------------- */

static double num_at(opstack_t *st, int idx) {
    if (idx < 0 || idx >= st->n) return 0.0;
    return st->ops[idx].num;
}

static void run_content(pdf_doc *doc, pdf_value *stream, psgen_t *g, int depth);

/* decode a stream's data into a buffer */
static int stream_decode(pdf_doc *doc, pdf_value *stream, buf_t *out) {
    pdf_value *filter, *dp;
    unsigned char *data;
    size_t len;
    if (!stream || stream->type != PDF_STREAM) return 0;
    data = stream->sdata;
    len = stream->slen;
    if (!data) return 0;

    filter = pdf_resolve(doc, pdf_dict_get(stream->u.stream.dict, "Filter"));
    dp = pdf_resolve(doc, pdf_dict_get(stream->u.stream.dict, "DecodeParms"));
    (void)dp;

    if (!filter) {
        return buf_put(out, data, len);
    }

    {
        pdf_value *f1 = filter;
        if (filter->type == PDF_ARRAY && filter->u.arr.count > 0)
            f1 = pdf_resolve(doc, filter->u.arr.items[0]);
        {
            const char *nm = pdf_name(f1);
            if (!nm) return buf_put(out, data, len);
            if (!strcmp(nm, "FlateDecode")) {
                if (zlib_inflate(data, len, out) != 0) return 0;
                return 1;
            }
            if (!strcmp(nm, "ASCII85Decode")) {
                buf_t tmp;
                buf_init(&tmp);
                if (!ascii85_decode(data, len, &tmp)) { buf_free(&tmp); return 0; }
                /* possible chained Flate after ASCII85 */
                if (filter->type == PDF_ARRAY && filter->u.arr.count > 1) {
                    pdf_value *f2 = pdf_resolve(doc, filter->u.arr.items[1]);
                    const char *n2 = pdf_name(f2);
                    if (n2 && !strcmp(n2, "FlateDecode")) {
                        int r = zlib_inflate(tmp.data, tmp.len, out);
                        buf_free(&tmp);
                        return r == 0;
                    }
                }
                buf_put(out, tmp.data, tmp.len);
                buf_free(&tmp);
                return 1;
            }
            if (!strcmp(nm, "ASCIIHexDecode")) {
                return asciihex_decode(data, len, out);
            }
            /* unsupported (DCTDecode etc.): pass raw */
            return buf_put(out, data, len);
        }
    }
}

/* ---------------- Type3 fonts ----------------
 *
 * Chromium/Skia prints CJK text by emitting one Type3 font per glyph. Each
 * glyph's CharProc is a plain PDF path ("m", "l", "c", "h", "f") that maps
 * almost 1:1 onto PostScript, so instead of rasterising we translate the
 * path and paint it with the current text matrix. That keeps the output
 * vector-sharp on a 1200 dpi LaserJet, which is the whole point of this
 * filter.
 */

/* Append formatted text straight to a byte buffer. */
static void gbuf_printf(buf_t *out, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_put(out, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static int gbuf_puts(buf_t *out, const char *s) {
    return buf_put(out, s, strlen(s));
}

/* Translate one CharProc path program into PostScript path operators.
 * Handles the subset Skia emits: m l c v y h re f f* S s B b W n plus
 * numeric operands. Unknown operators are dropped with their operands. */
static int t3_translate_charproc(pdf_doc *doc, pdf_value *proc, buf_t *out) {
    buf_t src;
    const unsigned char *p, *end;
    double nums[8];
    int nn = 0;
    int emitted = 0;

    if (!proc) return 0;
    proc = pdf_resolve(doc, proc);
    if (!proc || proc->type != PDF_STREAM) return 0;

    buf_init(&src);
    if (!stream_decode(doc, proc, &src)) { buf_free(&src); return 0; }

    p = src.data;
    end = src.data + src.len;

    while (p < end) {
        while (p < end && is_ws_pdf(*p)) p++;
        if (p >= end) break;
        if (*p == '%') { while (p < end && *p != '\n' && *p != '\r') p++; continue; }

        /* numeric operand */
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {
            char *ep = NULL;
            double v = strtod((const char *)p, &ep);
            if (ep && ep > (const char *)p) {
                if (nn < 8) nums[nn++] = v;
                p = (const unsigned char *)ep;
                continue;
            }
        }

        {
            char op[16];
            int n = 0;
            while (p < end && !is_ws_pdf(*p) && !is_delim_pdf(*p) && n < 15) op[n++] = (char)*p++;
            op[n] = 0;
            if (n == 0) { p++; continue; }

            if (!strcmp(op, "d0") || !strcmp(op, "d1")) {
                nn = 0;                    /* advance comes from /Widths */
            } else if (!strcmp(op, "m") && nn >= 2) {
                gbuf_printf(out, "%.3f %.3f moveto\n", nums[nn - 2], nums[nn - 1]);
                emitted = 1; nn = 0;
            } else if (!strcmp(op, "l") && nn >= 2) {
                gbuf_printf(out, "%.3f %.3f lineto\n", nums[nn - 2], nums[nn - 1]);
                nn = 0;
            } else if (!strcmp(op, "c") && nn >= 6) {
                gbuf_printf(out, "%.3f %.3f %.3f %.3f %.3f %.3f curveto\n",
                            nums[nn - 6], nums[nn - 5], nums[nn - 4],
                            nums[nn - 3], nums[nn - 2], nums[nn - 1]);
                nn = 0;
            } else if (!strcmp(op, "v") && nn >= 4) {
                gbuf_printf(out, "%.3f %.3f %.3f %.3f curveto\n",
                            nums[nn - 4], nums[nn - 3], nums[nn - 2], nums[nn - 1]);
                nn = 0;
            } else if (!strcmp(op, "y") && nn >= 4) {
                gbuf_printf(out, "%.3f %.3f %.3f %.3f curveto\n",
                            nums[nn - 4], nums[nn - 3], nums[nn - 2], nums[nn - 1]);
                nn = 0;
            } else if (!strcmp(op, "h")) {
                gbuf_puts(out, "closepath\n"); nn = 0;
            } else if (!strcmp(op, "re") && nn >= 4) {
                gbuf_printf(out, "%.3f %.3f %.3f %.3f rect\n",
                            nums[nn - 4], nums[nn - 3], nums[nn - 2], nums[nn - 1]);
                nn = 0;
            } else if (!strcmp(op, "f") || !strcmp(op, "f*") || !strcmp(op, "F")) {
                gbuf_puts(out, "fill\n"); nn = 0;
            } else if (!strcmp(op, "S") || !strcmp(op, "s")) {
                gbuf_puts(out, "stroke\n"); nn = 0;
            } else if (!strcmp(op, "B") || !strcmp(op, "B*") || !strcmp(op, "b")) {
                gbuf_puts(out, "gsave fill grestore stroke\n"); nn = 0;
            } else if (!strcmp(op, "W") || !strcmp(op, "W*")) {
                gbuf_puts(out, "clip\n"); nn = 0;
            } else if (!strcmp(op, "n")) {
                gbuf_puts(out, "newpath\n"); nn = 0;
            } else {
                nn = 0;                    /* unknown operator: drop it */
            }
        }
    }

    buf_free(&src);
    return emitted;
}

/* Look up a font resource by name in the page /Font dictionary and, if it
 * is a Type3 font, flatten its glyphs into a t3font_t. Returns NULL when
 * the resource is missing or is a normal (non-Type3) font. */
static t3font_t *t3_lookup(psgen_t *g, pdf_doc *doc, const char *resname) {
    pdf_value *resources, *fonts, *fdict, *font;
    t3font_t *tf;
    pdf_value *subtype;
    int i;

    if (!g || !g->resources || !resname || !resname[0]) return NULL;

    resources = pdf_resolve(doc, g->resources);
    if (!resources || resources->type != PDF_DICT) return NULL;
    fonts = pdf_resolve(doc, pdf_dict_get(resources, "Font"));
    if (!fonts || fonts->type != PDF_DICT) return NULL;
    fdict = pdf_resolve(doc, pdf_dict_get(fonts, resname));
    if (!fdict || fdict->type != PDF_DICT) return NULL;

    subtype = pdf_resolve(doc, pdf_dict_get(fdict, "Subtype"));
    if (!subtype || !pdf_name(subtype) || strcmp(pdf_name(subtype), "Type3"))
        return NULL;

    /* already flattened? */
    for (i = 0; i < g->nfonts; i++)
        if (!strcmp(g->fonts[i].resname, resname)) return &g->fonts[i];

    if (g->nfonts >= T3_MAX_FONTS) return NULL;
    if (g->nfonts >= g->fonts_cap) {
        int ncap = g->fonts_cap ? g->fonts_cap * 2 : 8;
        t3font_t *nf = (t3font_t *)realloc(g->fonts, (size_t)ncap * sizeof(t3font_t));
        if (!nf) return NULL;
        g->fonts = nf;
        g->fonts_cap = ncap;
    }

    tf = &g->fonts[g->nfonts];
    memset(tf, 0, sizeof(*tf));
    strncpy(tf->resname, resname, sizeof tf->resname - 1);

    /* /FontMatrix, default 0.001 identity */
    tf->matrix[0] = 0.001; tf->matrix[1] = 0; tf->matrix[2] = 0;
    tf->matrix[3] = 0.001; tf->matrix[4] = 0; tf->matrix[5] = 0;
    {
        pdf_value *fm = pdf_resolve(doc, pdf_dict_get(fdict, "FontMatrix"));
        if (fm && fm->type == PDF_ARRAY && fm->u.arr.count >= 6) {
            int k;
            for (k = 0; k < 6; k++) tf->matrix[k] = pdf_number(fm->u.arr.items[k]);
        }
    }

    {
        int first = 0, last = 0;
        pdf_value *fc = pdf_resolve(doc, pdf_dict_get(fdict, "FirstChar"));
        pdf_value *lc = pdf_resolve(doc, pdf_dict_get(fdict, "LastChar"));
        pdf_value *widths = pdf_resolve(doc, pdf_dict_get(fdict, "Widths"));
        pdf_value *enc = pdf_resolve(doc, pdf_dict_get(fdict, "Encoding"));
        pdf_value *diffs = enc ? pdf_resolve(doc, pdf_dict_get(enc, "Differences")) : NULL;
        pdf_value *procs = pdf_resolve(doc, pdf_dict_get(fdict, "CharProcs"));

        if (fc) first = pdf_intval(fc);
        if (lc) last = pdf_intval(lc);
        if (first < 0) first = 0;
        if (last > 255) last = 255;
        if (last < first) {
            /* no /LastChar: infer from the Differences array */
            if (diffs && diffs->type == PDF_ARRAY) {
                int cur = first;
                int k;
                for (k = 0; k < diffs->u.arr.count; k++) {
                    pdf_value *it = diffs->u.arr.items[k];
                    if (it->type == PDF_INT) cur = (int)it->u.i;
                    else if (it->type == PDF_NAME) { cur++; if (cur - 1 > last) last = cur - 1; }
                }
            }
        }
        if (last < first) last = first;

        {
            int code;
            int dstart = 0;      /* current code index inside /Differences */
            int dcur = first;
            for (code = first; code <= last && tf->nglyphs < T3_MAX_GLYPHS; code++) {
                double w = 0;
                const char *gname = NULL;
                pdf_value *proc = NULL;

                if (widths && widths->type == PDF_ARRAY) {
                    int idx = code - first;
                    if (idx >= 0 && idx < widths->u.arr.count)
                        w = pdf_number(widths->u.arr.items[idx]);
                }

                /* walk /Differences once, tracking the running code number */
                if (diffs && diffs->type == PDF_ARRAY) {
                    while (dstart < diffs->u.arr.count && dcur <= code) {
                        pdf_value *it = diffs->u.arr.items[dstart];
                        if (it->type == PDF_INT) {
                            dcur = (int)it->u.i;
                            dstart++;
                        } else if (it->type == PDF_NAME) {
                            if (dcur == code) gname = pdf_name(it);
                            dcur++;
                            dstart++;
                        } else {
                            dstart++;
                        }
                    }
                }

                if (gname && procs && procs->type == PDF_DICT) {
                    proc = pdf_dict_get(procs, gname);
                    if (proc) {
                        buf_t psbuf;
                        buf_init(&psbuf);
                        if (t3_translate_charproc(doc, proc, &psbuf) && psbuf.len > 0) {
                            tf->glyphs[tf->nglyphs].code = (char)code;
                            tf->glyphs[tf->nglyphs].width = w;
                            tf->glyphs[tf->nglyphs].ps = (char *)malloc(psbuf.len + 1);
                            if (tf->glyphs[tf->nglyphs].ps) {
                                memcpy(tf->glyphs[tf->nglyphs].ps, psbuf.data, psbuf.len);
                                tf->glyphs[tf->nglyphs].ps[psbuf.len] = 0;
                                tf->nglyphs++;
                            }
                        }
                        buf_free(&psbuf);
                    }
                }
            }
        }
    }

    g->nfonts++;
    return tf;
}

/* Find the flattened glyph for a byte code, or NULL. */
static t3glyph_t *t3_glyph(t3font_t *tf, unsigned char code) {
    int i;
    if (!tf) return NULL;
    for (i = 0; i < tf->nglyphs; i++)
        if ((unsigned char)tf->glyphs[i].code == code) return &tf->glyphs[i];
    return NULL;
}

/* Paint a Type3 string.
 *
 * PDF Type3 glyphs are drawn in glyph space, then mapped to text space with
 * /FontMatrix, then to device space by the text matrix. PostScript has no
 * equivalent of "render this path as a glyph", so for each character we:
 *   gsave
 *   <glyph matrix> concat        -- text matrix * size * FontMatrix
 *   <translated path>
 *   grestore
 *   <width> 0 Td                 -- advance the text position
 * The text matrix is applied via `concat` because Tm/Td state is kept by
 * the PostScript interpreter, and `concat` composes with it. */
static void t3_show(psgen_t *g, t3font_t *tf, double size, opstack_t *st, double *tm) {
    int i;
    /* Composite matrix for one glyph: Trm = Tm x [size 0 0 size 0 0] x FontMatrix
     * Since size is folded into the matrix we pass the FontMatrix pre-scaled. */
    for (i = 0; i < st->n; i++) {
        size_t k;
        if (st->ops[i].kind != 1) continue;
        for (k = 0; k < st->ops[i].str.len; k++) {
            unsigned char code = st->ops[i].str.data[k];
            t3glyph_t *gl = t3_glyph(tf, code);
            if (gl && gl->ps) {
                /* glyph->text space is FontMatrix(6); text->user is Tm.
                 * concat expects a 6-element array, so multiply the two. */
                double a = tm[0] * tf->matrix[0] * size + tm[2] * tf->matrix[1] * size;
                double b = tm[1] * tf->matrix[0] * size + tm[3] * tf->matrix[1] * size;
                double c = tm[0] * tf->matrix[2] * size + tm[2] * tf->matrix[3] * size;
                double d = tm[1] * tf->matrix[2] * size + tm[3] * tf->matrix[3] * size;
                double e = tm[0] * (tf->matrix[4] * size) + tm[2] * (tf->matrix[5] * size) + tm[4];
                double f = tm[1] * (tf->matrix[4] * size) + tm[3] * (tf->matrix[5] * size) + tm[5];
                ps_out(g, "gsave\n");
                ps_printf(g, "[ %.6f %.6f %.6f %.6f %.4f %.4f ] concat\n", a, b, c, d, e, f);
                ps_out(g, gl->ps);
                ps_out(g, "grestore\n");
            }
            {
                /* advance in text space = width * FontMatrix[0] * size */
                double adv = (gl ? gl->width : 500.0) * tf->matrix[0] * size;
                if (adv <= 0) adv = (gl ? gl->width : 500.0) * size * 0.001;
                if (adv > 0) {
                    ps_printf(g, "%.4f 0 Td\n", adv);
                    tm[4] += tm[0] * adv;
                    tm[5] += tm[1] * adv;
                }
            }
        }
    }
}

/* Decode a PDF string into unicode-ish bytes for PostScript output.
 * Standard-14 fonts use Latin-1 for codes < 256, which PostScript can show
 * directly with ISO-Latin-1 encoding. */
static void emit_text(psgen_t *g, opstack_t *st) {
    int i;
    ps_out(g, "(");
    for (i = 0; i < st->n; i++) {
        if (st->ops[i].kind != 1) continue;
        {
            size_t k;
            for (k = 0; k < st->ops[i].str.len; k++) {
                unsigned char c = st->ops[i].str.data[k];
                if (c == '(' || c == ')' || c == '\\') { fputc('\\', g->out); fputc(c, g->out); }
                else if (c < 32 || c > 126) ps_printf(g, "\\%03o", c);
                else fputc(c, g->out);
            }
        }
    }
    ps_printf(g, ") show\n");
}

/* ---------------- main interpreter ---------------- */

static void run_content(pdf_doc *doc, pdf_value *stream, psgen_t *g, int depth) {
    buf_t content;
    const unsigned char *p, *end;
    opstack_t st;
    char font_name[128];
    char last_name[128];
    t3font_t *cur_t3 = NULL;
    double cur_size = 12.0;
    /* Text state tracked in C so Type3 glyph paths can be placed exactly.
     * tm is the text matrix (PDF 3x3 with the last row 0 0 1, stored as the
     * six meaningful numbers), tlm the text line matrix. */
    double tm[6], tlm[6];

    if (depth > 8) return;
    buf_init(&content);
    if (!stream_decode(doc, stream, &content)) {
        buf_free(&content);
        return;
    }
    p = content.data;
    end = content.data + content.len;
    st.n = 0;
    font_name[0] = 0;
    last_name[0] = 0;
    tm[0] = 1; tm[1] = 0; tm[2] = 0; tm[3] = 1; tm[4] = 0; tm[5] = 0;
    tlm[0] = 1; tlm[1] = 0; tlm[2] = 0; tlm[3] = 1; tlm[4] = 0; tlm[5] = 0;

    while (p < end) {
        const unsigned char *save;

        /* skip whitespace and comments */
        while (p < end && (is_ws_pdf(*p))) p++;
        if (p < end && *p == '%') {
            while (p < end && *p != '\n' && *p != '\r') p++;
            continue;
        }
        if (p >= end) break;

        save = p;

        /* strings */
        if (*p == '(') {
            buf_t sb;
            int depth2 = 1;
            operand_t *o;
            buf_init(&sb);
            p++;
            while (p < end && depth2 > 0) {
                int c = *p++;
                if (c == '\\') {
                    int e;
                    if (p >= end) break;
                    e = *p++;
                    if (e >= '0' && e <= '7') {
                        int v = e - '0', k = 0;
                        while (k < 2 && p < end && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                        buf_putc(&sb, v & 0xFF);
                    } else if (e == 'n') buf_putc(&sb, '\n');
                    else if (e == 'r') buf_putc(&sb, '\r');
                    else if (e == 't') buf_putc(&sb, '\t');
                    else buf_putc(&sb, e);
                } else if (c == '(') {
                    depth2++;
                    buf_putc(&sb, c);
                } else if (c == ')') {
                    depth2--;
                    if (depth2 > 0) buf_putc(&sb, c);
                } else {
                    buf_putc(&sb, c);
                }
            }
            o = ops_str(&st);
            if (o) { buf_put(&o->str, sb.data, sb.len); }
            buf_free(&sb);
            continue;
        }
        if (*p == '<' && p + 1 < end && p[1] == '<') {
            /* inline dictionary: skip roughly */
            p += 2;
            while (p + 1 < end && !(p[0] == '>' && p[1] == '>')) p++;
            p = (p + 1 < end) ? p + 2 : end;
            continue;
        }
        if (*p == '<') {
            /* hex string */
            operand_t *o = ops_str(&st);
            int hi = -1;
            p++;
            while (p < end && *p != '>') {
                int c = *p++;
                int v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else continue;
                if (hi < 0) hi = v;
                else { if (o) buf_putc(&o->str, (hi << 4) | v); hi = -1; }
            }
            if (hi >= 0 && o) buf_putc(&o->str, hi << 4);
            if (p < end && *p == '>') p++;
            continue;
        }
        if (*p == '[') {
            p++;
            continue;
        }
        if (*p == ']') {
            p++;
            continue;
        }

        /* numbers */
        if (isdigit_pdf(*p) || *p == '-' || *p == '+' || *p == '.') {
            char tmp[64];
            size_t n = 0;
            while (p < end && n + 1 < sizeof tmp &&
                   (isdigit_pdf(*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) {
                tmp[n++] = (char)*p++;
            }
            tmp[n] = 0;
            {
                char *ep = NULL;
                double d = strtod(tmp, &ep);
                if (ep && *ep == 0 && n > 0) {
                    ops_num(&st, d);
                } else {
                    p = save;
                    p++;
                }
            }
            continue;
        }

        /* names */
        if (*p == '/') {
            char nm[128];
            size_t n = 0;
            p++;
            while (p < end && !is_ws_pdf(*p) && !is_delim_pdf(*p) && n + 1 < sizeof nm) {
                nm[n++] = (char)*p++;
            }
            nm[n] = 0;
            strncpy(last_name, nm, sizeof last_name - 1);
            last_name[sizeof last_name - 1] = 0;
            continue;
        }

        /* operator token */
        {
            char op[32];
            size_t n = 0;
            while (p < end && !is_ws_pdf(*p) && !is_delim_pdf(*p) && n + 1 < sizeof op) {
                op[n++] = (char)*p++;
            }
            op[n] = 0;
            if (n == 0) { p++; continue; }

            if (!strcmp(op, "Tj") || !strcmp(op, "'") || !strcmp(op, "\"")) {
                if (cur_t3) t3_show(g, cur_t3, cur_size, &st, tm);
                else emit_text(g, &st);
                if (!strcmp(op, "'")) { ps_out(g, "0 -1.2 Tm 0 0 Td\n"); }
                ops_reset(&st);
            } else if (!strcmp(op, "TJ")) {
                if (cur_t3) t3_show(g, cur_t3, cur_size, &st, tm);
                else emit_text(g, &st);
                ops_reset(&st);
            } else if (!strcmp(op, "Tf")) {
                const char *resname = last_name[0] ? last_name : font_name;
                double sz = (st.n >= 1) ? num_at(&st, st.n - 1) : 0.0;
                if (sz <= 0.1) sz = 12.0;
                cur_size = sz;
                /* Type3 fonts carry their own glyph paths; normal fonts map
                 * onto a standard PostScript face. */
                cur_t3 = t3_lookup(g, doc, resname);
                if (!cur_t3) {
                    const char *psf = map_font(resname);
                    ps_printf(g, "/%s findfont %.3f scalefont setfont\n", psf, sz);
                }
                ops_reset(&st);
            } else if (!strcmp(op, "Td")) {
                if (st.n >= 2) {
                    double tx = num_at(&st, st.n - 2), ty = num_at(&st, st.n - 1);
                    ps_printf(g, "%.4f %.4f Td\n", tx, ty);
                    /* tlm = translate(tx,ty) x tlm ; tm = tlm */
                    tlm[4] = tlm[0] * tx + tlm[2] * ty + tlm[4];
                    tlm[5] = tlm[1] * tx + tlm[3] * ty + tlm[5];
                    memcpy(tm, tlm, sizeof tm);
                }
                ops_reset(&st);
            } else if (!strcmp(op, "TD")) {
                if (st.n >= 2) {
                    double tx = num_at(&st, st.n - 2), ty = num_at(&st, st.n - 1);
                    ps_printf(g, "%.4f %.4f Td\n", tx, ty);
                    tlm[4] = tlm[0] * tx + tlm[2] * ty + tlm[4];
                    tlm[5] = tlm[1] * tx + tlm[3] * ty + tlm[5];
                    memcpy(tm, tlm, sizeof tm);
                }
                ops_reset(&st);
            } else if (!strcmp(op, "Tm")) {
                if (st.n >= 6) {
                    double a = num_at(&st, st.n - 6), b = num_at(&st, st.n - 5);
                    double c = num_at(&st, st.n - 4), d = num_at(&st, st.n - 3);
                    double e = num_at(&st, st.n - 2), f = num_at(&st, st.n - 1);
                    ps_printf(g, "[ %.4f %.4f %.4f %.4f %.4f %.4f ] Tm\n", a, b, c, d, e, f);
                    tlm[0] = a; tlm[1] = b; tlm[2] = c;
                    tlm[3] = d; tlm[4] = e; tlm[5] = f;
                    memcpy(tm, tlm, sizeof tm);
                }
                ops_reset(&st);
            } else if (!strcmp(op, "BT")) {
                ps_out(g, "gsave\n");
                tm[0] = 1; tm[1] = 0; tm[2] = 0; tm[3] = 1; tm[4] = 0; tm[5] = 0;
                memcpy(tlm, tm, sizeof tm);
                ops_reset(&st);
            } else if (!strcmp(op, "ET")) {
                ps_out(g, "grestore\n");
                ops_reset(&st);
            } else if (!strcmp(op, "cm")) {
                if (st.n >= 6) {
                    ps_printf(g, "[ %.4f %.4f %.4f %.4f %.4f %.4f ] concat\n",
                              num_at(&st, st.n - 6), num_at(&st, st.n - 5),
                              num_at(&st, st.n - 4), num_at(&st, st.n - 3),
                              num_at(&st, st.n - 2), num_at(&st, st.n - 1));
                }
                ops_reset(&st);
            } else if (!strcmp(op, "q")) {
                ps_out(g, "gsave\n");
                ops_reset(&st);
            } else if (!strcmp(op, "Q")) {
                ps_out(g, "grestore\n");
                ops_reset(&st);
            } else if (!strcmp(op, "rg")) {
                if (st.n >= 3) ps_printf(g, "%.4f %.4f %.4f setrgbcolor\n",
                                         num_at(&st, st.n - 3), num_at(&st, st.n - 2), num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "g")) {
                if (st.n >= 1) ps_printf(g, "%.4f setgray\n", num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "k")) {
                if (st.n >= 4) ps_printf(g, "%.4f %.4f %.4f %.4f setcmykcolor\n",
                                         num_at(&st, st.n - 4), num_at(&st, st.n - 3),
                                         num_at(&st, st.n - 2), num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "w")) {
                if (st.n >= 1) ps_printf(g, "%.4f setlinewidth\n", num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "m")) {
                if (st.n >= 2) ps_printf(g, "%.4f %.4f moveto\n", num_at(&st, st.n - 2), num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "l")) {
                if (st.n >= 2) ps_printf(g, "%.4f %.4f lineto\n", num_at(&st, st.n - 2), num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "c")) {
                if (st.n >= 6) ps_printf(g, "%.4f %.4f %.4f %.4f %.4f %.4f curveto\n",
                                         num_at(&st, st.n - 6), num_at(&st, st.n - 5),
                                         num_at(&st, st.n - 4), num_at(&st, st.n - 3),
                                         num_at(&st, st.n - 2), num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "h")) {
                ps_out(g, "closepath\n");
                ops_reset(&st);
            } else if (!strcmp(op, "re")) {
                if (st.n >= 4) ps_printf(g, "%.4f %.4f %.4f %.4f rect\n",
                                         num_at(&st, st.n - 4), num_at(&st, st.n - 3),
                                         num_at(&st, st.n - 2), num_at(&st, st.n - 1));
                ops_reset(&st);
            } else if (!strcmp(op, "S")) {
                ps_out(g, "stroke\n");
                ops_reset(&st);
            } else if (!strcmp(op, "s")) {
                ps_out(g, "closepath stroke\n");
                ops_reset(&st);
            } else if (!strcmp(op, "f") || !strcmp(op, "F") || !strcmp(op, "f*")) {
                ps_out(g, "fill\n");
                ops_reset(&st);
            } else if (!strcmp(op, "B") || !strcmp(op, "B*") || !strcmp(op, "b")) {
                ps_out(g, "gsave fill grestore stroke\n");
                ops_reset(&st);
            } else if (!strcmp(op, "W") || !strcmp(op, "W*")) {
                ps_out(g, "clip\n");
                ops_reset(&st);
            } else if (!strcmp(op, "n")) {
                ps_out(g, "newpath\n");
                ops_reset(&st);
            } else if (!strcmp(op, "Do")) {
                /* XObject: draw image if the resource resolves to one */
                char *xo = last_name;
                if (xo[0] && g->resources) {
                    pdf_value *xdict = pdf_resolve(doc, pdf_dict_get(g->resources, "XObject"));
                    pdf_value *xobj = pdf_resolve(doc, pdf_dict_get(xdict, xo));
                    if (xobj && xobj->type == PDF_STREAM) {
                        psgen_emit_image(doc, xobj, g);
                    }
                }
                ops_reset(&st);
            } else if (!strcmp(op, "gs")) {
                ops_reset(&st);
            } else if (!strcmp(op, "cm") || !strcmp(op, "BT") || !strcmp(op, "ET")) {
                ops_reset(&st);
            } else if (!strcmp(op, "TL") || !strcmp(op, "Tc") || !strcmp(op, "Tw") ||
                       !strcmp(op, "Tz") || !strcmp(op, "Ts") || !strcmp(op, "Tr")) {
                ops_reset(&st);
            } else {
                ops_reset(&st);
            }
        }
    }
    ops_reset(&st);
    buf_free(&content);
}

void psgen_page_content(pdf_doc *doc, psgen_t *g, pdf_value *contents) {
    pdf_value *c = pdf_resolve(doc, contents);
    if (!c) return;
    if (c->type == PDF_STREAM) {
        run_content(doc, c, g, 0);
    } else if (c->type == PDF_ARRAY) {
        int i;
        for (i = 0; i < c->u.arr.count; i++) {
            pdf_value *s = pdf_resolve(doc, c->u.arr.items[i]);
            if (s && s->type == PDF_STREAM) run_content(doc, s, g, 0);
        }
    }
}


