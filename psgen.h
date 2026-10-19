/*
 * psgen.h - PostScript emitter interface
 */
#ifndef PSGEN_H
#define PSGEN_H

#include <stdio.h>
#include <stdarg.h>
#include "pdfobj.h"

/* One Type3 glyph: the CharProc body, already extracted as PostScript path
 * source, plus the advance width taken from /Widths. */
typedef struct {
    char  code;        /* single-byte character code, 0..255 */
    double width;      /* advance in glyph space (× FontMatrix for text space) */
    char *ps;          /* PostScript path source, may be NULL for empty glyphs */
    int   used;
} t3glyph_t;

#define T3_MAX_GLYPHS 256

/* A Type3 font flattened for output. */
typedef struct {
    char  resname[64];     /* resource name, e.g. "F82" */
    double matrix[6];      /* /FontMatrix, default .001 0 0 .001 0 0 */
    int   nglyphs;
    t3glyph_t glyphs[T3_MAX_GLYPHS];
} t3font_t;

#define T3_MAX_FONTS 256

typedef struct {
    FILE *out;
    int page_count;
    double media_x0, media_y0, media_x1, media_y1;
    pdf_value *resources;   /* current page /Resources, for XObject lookup */
    /* Type3 handling: fonts found in the current page's /Font resource. */
    t3font_t *fonts;        /* fonts actually used on this page */
    int nfonts;
    int fonts_cap;
} psgen_t;

void psgen_init(psgen_t *g, FILE *out);
void psgen_begin(psgen_t *g);
void psgen_end(psgen_t *g);
void psgen_page_begin(psgen_t *g, double x0, double y0, double x1, double y1);
void psgen_page_end(psgen_t *g);
void psgen_page_content(pdf_doc *doc, psgen_t *g, pdf_value *contents);
int  psgen_emit_image(pdf_doc *doc, pdf_value *xobj, psgen_t *g);

#endif
