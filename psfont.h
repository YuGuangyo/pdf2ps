/*
 * psfont.h - font resolution for pdftops-lite
 *
 * PDF fonts fall into four cases for a PostScript target:
 *
 *   1. Type1 / MMType1 / TrueType, base font is one of the 14 standard faces
 *      -> emit the matching PostScript font directly, byte codes pass through.
 *
 *   2. Type1 / TrueType simple font with an /Encoding + /Widths and no usable
 *      standard mapping (subset, custom encoding)
 *      -> emit the face plus a /Encoding override derived from /Differences.
 *
 *   3. Type0 (composite) with Identity-H + CIDFontType2 (embedded TrueType)
 *      -> 2-byte codes address glyph IDs; shape them with TrueType 'glyf'
 *         outlines pulled from /FontFile2.
 *
 *   4. Type3
 *      -> glyphs are arbitrary content streams, replayed as paths.
 *
 * The old code looked fonts up by /BaseFont and compared it with the resource
 * name ("F1"), which never matched a real PDF; resolution now goes through the
 * page /Resources /Font dictionary as the spec requires.
 */
#ifndef PSFONT_H
#define PSFONT_H

#include <stdio.h>
#include "pdfobj.h"

/* One resolved font, cached per page. */
typedef struct psfont_s psfont_t;

void psfont_cache_reset(void);
void psfont_cache_free(void);

/*
 * Look up /Resources /Font /<resname> and return a prepared font, or NULL if
 * the resource does not exist. The returned object stays valid until the next
 * psfont_cache_reset(); it is owned by the cache.
 */
psfont_t *psfont_resolve(pdf_doc *doc, pdf_value *resources, const char *resname);

/* Emit the PostScript setup for this font at the given size. */
void psfont_set(psfont_t *f, FILE *out, double size);

/* Emit the glyphs for a PDF string operand. */
void psfont_show(psfont_t *f, FILE *out, const unsigned char *s, size_t n,
                 double *tm);

/* Number of glyph draws emitted so far, for tests that assert real glyphs
 * rather than advance-only text. */
extern unsigned long psfont_glyph_draws;

/* Give-up counter, for tests: fonts that needed a fallback face. */
extern unsigned long psfont_fallback_count;

#endif
