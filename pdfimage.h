/*
 * pdfimage.h - image XObject emission
 */
#ifndef PDFIMAGE_H
#define PDFIMAGE_H

#include "pdfobj.h"
#include "psgen.h"

int psgen_emit_image(pdf_doc *doc, pdf_value *xobj, psgen_t *g);

#endif
