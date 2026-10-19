/*
 * psfont.h - font resolution and glyph emission for pdftops-lite
 *
 * A psfont_t is created per (page /Resources, resource name) pair.  What it can
 * do depends on how the PDF embeds the font:
 *
 *   PSF_SIMPLE_STD     a base-14 face, byte codes pass through unchanged
 *   PSF_SIMPLE_MAPPED  a simple font whose codes are remapped onto a base-14
 *                      face (subset fonts, /Differences, Symbol-ish fonts)
 *   PSF_TYPE3          the glyphs are themselves content streams
 *   PSF_TRUETYPE_CID   Identity CMap + embedded TrueType: real 'glyf'
 *                      outlines are emitted as PostScript paths
 *
 * The distinction that decides output quality: the first two hand the job to
 * one of the printer's built-in faces, while the last two send the document's
 * own outlines.  psfont_fallback_count tells you how often we settled for the
 * first two, so a caller can warn instead of silently printing the wrong face.
 *
 * The struct layout is private to psfont.c; callers only use the functions.
 */
#ifndef PSFONT_H
#define PSFONT_H

#include <stddef.h>
#include <stdio.h>
#include "pdfobj.h"

#define PSF_MAX_GLYPH 256

typedef struct psfont psfont_t;

/* Counters for reporting: real vector work done vs. faces substituted. */
extern unsigned long psfont_glyph_draws;
extern unsigned long psfont_fallback_count;

/* Drop every cached font.  Must be called between pages: a psfont_t owns
 * buffers that are only valid while its page's resources are alive. */
void psfont_cache_reset(void);
void psfont_cache_free(void);

/* Resolve /Resources /Font /<resname>.  NULL when the resource is missing. */
psfont_t *psfont_resolve(pdf_doc *doc, pdf_value *resources, const char *resname);

/* Select the font on the printer side at `size`; no-op for path-emitted kinds. */
void psfont_set(psfont_t *f, FILE *out, double size);

/* Emit glyphs for `s`; advances `tm` (PDF order: a b c d e f) in place. */
void psfont_show(psfont_t *f, FILE *out, const unsigned char *s, size_t n, double *tm);

#endif
