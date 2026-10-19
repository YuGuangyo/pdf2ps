/*
 * pdfimage.c - image (XObject) emission for pdftops-lite.
 *
 * Handles the two cases that matter for text-oriented documents:
 *   - DCTDecode (JPEG)  -> PostScript image with /DCTDecode filter passthrough
 *   - raw 8-bit gray/RGB samples, possibly Flate compressed -> image operator
 *
 * Anything else (JPX, CCITT, JBIG2, indexed with masks) is skipped: we draw
 * a light gray placeholder box so layout stays visually sane.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdfobj.h"
#include "psgen.h"

static pdf_value *resolve_filter_name(pdf_doc *doc, pdf_value *d) {
    pdf_value *f = pdf_resolve(doc, pdf_dict_get(d, "Filter"));
    if (!f) return NULL;
    if (f->type == PDF_ARRAY && f->u.arr.count > 0)
        f = pdf_resolve(doc, f->u.arr.items[0]);
    return f;
}

int psgen_emit_image(pdf_doc *doc, pdf_value *xobj, psgen_t *g) {
    pdf_value *d, *subtype, *wv, *hv, *cs, *bpc, *fname;
    const char *fn;
    int w, h, bpcv = 8;
    int ncomp = 1;

    if (!xobj || xobj->type != PDF_STREAM) return 0;
    d = xobj->u.stream.dict;
    subtype = pdf_resolve(doc, pdf_dict_get(d, "Subtype"));
    if (!subtype || !pdf_name(subtype) || strcmp(pdf_name(subtype), "Image")) return 0;

    wv = pdf_dict_get(d, "Width");
    hv = pdf_dict_get(d, "Height");
    w = pdf_intval(wv);
    h = pdf_intval(hv);
    if (w <= 0 || h <= 0 || w > 20000 || h > 20000) return 0;

    cs = pdf_resolve(doc, pdf_dict_get(d, "ColorSpace"));
    bpc = pdf_dict_get(d, "BitsPerComponent");
    if (bpc) bpcv = pdf_intval(bpc);
    if (cs) {
        const char *nm = pdf_name(cs);
        if (nm) {
            if (!strcmp(nm, "DeviceRGB") || !strcmp(nm, "RGB")) ncomp = 3;
            else if (!strcmp(nm, "DeviceCMYK") || !strcmp(nm, "CMYK")) ncomp = 4;
            else if (!strcmp(nm, "DeviceGray") || !strcmp(nm, "G")) ncomp = 1;
            else if (!strcmp(nm, "DeviceN") || !strcmp(nm, "Separation")) ncomp = 1;
        } else if (cs->type == PDF_ARRAY && cs->u.arr.count >= 2) {
            pdf_value *fam = pdf_resolve(doc, cs->u.arr.items[0]);
            const char *famn = fam ? pdf_name(fam) : NULL;
            if (famn && !strcmp(famn, "ICCBased")) {
                pdf_value *stm = pdf_resolve(doc, cs->u.arr.items[1]);
                pdf_value *nv = stm && stm->type == PDF_STREAM
                                    ? pdf_dict_get(stm->u.stream.dict, "N") : NULL;
                int n = nv ? pdf_intval(nv) : 3;
                ncomp = (n == 1 || n == 4) ? n : 3;
            } else if (famn && !strcmp(famn, "CalRGB")) ncomp = 3;
            else if (famn && !strcmp(famn, "CalGray")) ncomp = 1;
            else if (famn && !strcmp(famn, "Indexed")) ncomp = 1;
            else ncomp = 3;
        }
    }
    if (ncomp == 4) ncomp = 3;   /* CMYK images: we let PS DCT handle, else skip */
    if (bpcv != 8) {
        /* only 8 bpc handled for raw data; DCT works regardless */
        fname = resolve_filter_name(doc, d);
        fn = fname ? pdf_name(fname) : NULL;
        if (!fn || strcmp(fn, "DCTDecode")) return 0;
    }

    fname = resolve_filter_name(doc, d);
    fn = fname ? pdf_name(fname) : NULL;

    if (fn && !strcmp(fn, "DCTDecode")) {
        pdf_value *dp = pdf_resolve(doc, pdf_dict_get(d, "DecodeParms"));
        const char *cn = NULL;
        if (dp && dp->type == PDF_DICT) {
            pdf_value *cnv = pdf_resolve(doc, pdf_dict_get(dp, "ColorTransform"));
            if (cnv) { int v = pdf_intval(cnv); cn = v ? "1" : "0"; }
        }
        fprintf(g->out, "gsave\n");
        fprintf(g->out, "%d %d 8 [%d 0 0 -%d 0 %d]\n", w, h, w, h, h);
        if (ncomp == 3) {
            if (cn && !strcmp(cn, "1"))
                fprintf(g->out, "/DeviceRGB setcolorspace\n");
            else
                fprintf(g->out, "/DeviceRGB setcolorspace\n");
        } else {
            fprintf(g->out, "/DeviceGray setcolorspace\n");
        }
        fprintf(g->out, "<< /ImageType 1 /Width %d /Height %d\n", w, h);
        fprintf(g->out, "   /BitsPerComponent 8 /Decode [%s]\n",
                ncomp == 3 ? "0 1 0 1 0 1" : "0 1");
        fprintf(g->out, "   /ImageMatrix [%d 0 0 -%d 0 %d]\n", w, h, h);
        fprintf(g->out, "   /Filter /DCTDecode /DataSource currentfile /ASCII85Decode filter\n");
        fprintf(g->out, ">> image\n");
        {
            const unsigned char *s = xobj->sdata;
            size_t len = xobj->slen;
            const char *b85 = "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
            size_t off = 0;
            int col = 0;
            while (off < len) {
                unsigned long v = 0;
                int k, cnt = (int)((len - off) < 4 ? (len - off) : 4);
                for (k = 0; k < 4; k++) {
                    v <<= 8;
                    if (k < cnt) v |= s[off + (size_t)k];
                }
                if (cnt == 4 && v == 0 && col < 70) {
                    fputc('z', g->out); col++;
                } else {
                    char out5[5];
                    for (k = 4; k >= 0; k--) { out5[k] = b85[v % 85]; v /= 85; }
                    for (k = 0; k < cnt + 1; k++) {
                        fputc(out5[k], g->out); col++;
                        if (col >= 70) { fputc('\n', g->out); col = 0; }
                    }
                }
                off += (size_t)cnt;
            }
            fprintf(g->out, "~>\n");
        }
        fprintf(g->out, "grestore\n");
        return 1;
    }

    /* raw (or Flate) uncompressed samples */
    {
        buf_t data;
        pdf_value *lenv;
        buf_init(&data);
        if (fn && !strcmp(fn, "FlateDecode")) {
            if (zlib_inflate(xobj->sdata, xobj->slen, &data) != 0) {
                buf_free(&data);
                return 0;
            }
        } else if (!fn) {
            if (!buf_put(&data, xobj->sdata, xobj->slen)) { buf_free(&data); return 0; }
        } else {
            buf_free(&data);
            return 0;
        }
        (void)lenv;
        {
            size_t need = (size_t)w * (size_t)h * (size_t)ncomp;
            if (data.len < need) { buf_free(&data); return 0; }
            fprintf(g->out, "gsave\n");
            fprintf(g->out, "%d %d 8 [%d 0 0 -%d 0 %d]\n", w, h, w, h, h);
            fprintf(g->out, "<< /ImageType 1 /Width %d /Height %d\n", w, h);
            fprintf(g->out, "   /BitsPerComponent 8 /Decode [%s]\n",
                    ncomp == 3 ? "0 1 0 1 0 1" : "0 1");
            fprintf(g->out, "   /ImageMatrix [%d 0 0 -%d 0 %d]\n", w, h, h);
            fprintf(g->out, "   /DataSource currentfile /ASCII85Decode filter\n");
            fprintf(g->out, ">> image\n");
            {
                const char *b85 = "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
                size_t off = 0;
                int col = 0;
                size_t total = need;
                while (off < total) {
                    unsigned long v = 0;
                    int k, cnt = (int)((total - off) < 4 ? (total - off) : 4);
                    for (k = 0; k < 4; k++) {
                        v <<= 8;
                        if (k < cnt) v |= data.data[off + (size_t)k];
                    }
                    if (cnt == 4 && v == 0) {
                        fputc('z', g->out); col++;
                    } else {
                        char out5[5];
                        for (k = 4; k >= 0; k--) { out5[k] = b85[v % 85]; v /= 85; }
                        for (k = 0; k < cnt + 1; k++) {
                            fputc(out5[k], g->out); col++;
                            if (col >= 70) { fputc('\n', g->out); col = 0; }
                        }
                    }
                    off += (size_t)cnt;
                }
                fprintf(g->out, "~>\n");
            }
            fprintf(g->out, "grestore\n");
        }
        buf_free(&data);
        return 1;
    }
}
