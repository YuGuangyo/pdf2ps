/*
 * psgen.h - PostScript emitter interface
 */
#ifndef PSGEN_H
#define PSGEN_H

#include <stdio.h>
#include <stdarg.h>
#include "pdfobj.h"

typedef struct {
    FILE *out;
    int page_count;
    double media_x0, media_y0, media_x1, media_y1;
    pdf_value *resources;   /* current page /Resources, for XObject lookup */
} psgen_t;

void psgen_init(psgen_t *g, FILE *out);
void psgen_begin(psgen_t *g);
void psgen_end(psgen_t *g);
void psgen_page_begin(psgen_t *g, double x0, double y0, double x1, double y1);
void psgen_page_end(psgen_t *g);
void psgen_page_content(pdf_doc *doc, psgen_t *g, pdf_value *contents);
int  psgen_emit_image(pdf_doc *doc, pdf_value *xobj, psgen_t *g);

#endif
