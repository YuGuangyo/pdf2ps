/*
 * main.c - pdftops-lite entry point : page tree walk + PS emission.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buf.h"
#include "pdfobj.h"
#include "psgen.h"

#define VERSION "0.1"

/* collect pages (leaf nodes of the page tree) */
typedef struct {
    pdf_value **pages;
    int count;
    int cap;
} pagelist_t;

static void collect_pages(pdf_doc *doc, pdf_value *node, pagelist_t *pl, int depth) {
    pdf_value *kids, *type;
    if (!node || depth > 32) return;
    node = pdf_resolve(doc, node);
    if (!node || node->type != PDF_DICT) return;

    type = pdf_resolve(doc, pdf_dict_get(node, "Type"));
    kids = pdf_resolve(doc, pdf_dict_get(node, "Kids"));

    if (kids && kids->type == PDF_ARRAY) {
        int i;
        for (i = 0; i < kids->u.arr.count; i++)
            collect_pages(doc, kids->u.arr.items[i], pl, depth + 1);
        return;
    }

    if (type && pdf_name(type) && !strcmp(pdf_name(type), "Page")) {
        if (pl->count + 1 > pl->cap) {
            int ncap = pl->cap ? pl->cap * 2 : 32;
            pdf_value **np = (pdf_value **)realloc(pl->pages, (size_t)ncap * sizeof(pdf_value *));
            if (!np) return;
            pl->pages = np;
            pl->cap = ncap;
        }
        pl->pages[pl->count++] = node;
    }
}

/* find an inheritable attribute by walking /Parent */
static pdf_value *inherited(pdf_doc *doc, pdf_value *page, const char *key) {
    int depth = 0;
    pdf_value *cur = page;
    while (cur && depth++ < 32) {
        pdf_value *v = pdf_dict_get(cur, key);
        if (v) return pdf_resolve(doc, v);
        cur = pdf_resolve(doc, pdf_dict_get(cur, "Parent"));
        if (cur && cur->type != PDF_DICT) break;
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *file = NULL;
    FILE *in = stdin;
    buf_t raw;
    unsigned char tmp[16384];
    size_t n;
    pdf_doc *doc;
    pagelist_t pl;
    psgen_t g;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            printf("pdftops-lite %s\n", VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: pdftops-lite [job-id user title copies options [file]]\n");
            printf("       pdftops-lite file.pdf\n");
            printf("Converts PDF on stdin (or file) to PostScript on stdout.\n");
            return 0;
        }
    }

    if (argc >= 6) file = argv[5];
    else if (argc == 2 && argv[1][0] != '-') file = argv[1];

    if (file) {
        in = fopen(file, "rb");
        if (!in) {
            fprintf(stderr, "pdftops-lite: cannot open %s\n", file);
            return 1;
        }
    }

    buf_init(&raw);
    while ((n = fread(tmp, 1, sizeof tmp, in)) > 0) {
        if (!buf_put(&raw, tmp, n)) {
            fprintf(stderr, "pdftops-lite: out of memory\n");
            if (file) fclose(in);
            return 1;
        }
    }
    if (file) fclose(in);

    if (raw.len < 5 || memcmp(raw.data, "%PDF", 4) != 0) {
        fprintf(stderr, "pdftops-lite: input is not a PDF file\n");
        buf_free(&raw);
        return 1;
    }

    doc = pdf_open(raw.data, raw.len);
    if (!doc) {
        fprintf(stderr, "pdftops-lite: cannot parse PDF\n");
        return 1;
    }

    pl.pages = NULL;
    pl.count = 0;
    pl.cap = 0;

    {
        pdf_value *root = NULL;
        int debug = getenv("PDFTOPS_DEBUG") != NULL;
        if (debug) {
            int k, live = 0;
            for (k = 1; k < doc->xref_size; k++)
                if (doc->xref[k].offset > 0 || doc->xref[k].in_object_stream > 0) live++;
            fprintf(stderr, "[dbg] len=%zu xref_size=%d live_objects=%d trailer=%s\n",
                    doc->len, doc->xref_size, live, doc->trailer ? "present" : "NULL");
            if (doc->trailer && doc->trailer->type == PDF_DICT)
                fprintf(stderr, "[dbg] trailer keys=%d\n", doc->trailer->u.dict.count);
        }
        if (doc->trailer) {
            pdf_value *r = pdf_dict_get(doc->trailer, "Root");
            if (debug)
                fprintf(stderr, "[dbg] /Root ref=%s\n", r ? "found" : "absent");
            root = pdf_resolve(doc, r);
            if (debug)
                fprintf(stderr, "[dbg] /Root resolved=%s\n",
                        root ? (root->type == PDF_DICT ? "dict" : "other") : "NULL");
        }
        if (!root) {
            /*
             * Truncated / hand-written PDFs sometimes lack a trailer or the
             * /Root link. Fall back to scanning for the catalog object.
             */
            int i;
            for (i = 1; i < doc->xref_size && !root; i++) {
                pdf_value *v, *type;
                if (doc->xref[i].offset <= 0 && doc->xref[i].in_object_stream == 0) continue;
                v = pdf_get(doc, i);
                if (debug && v)
                    fprintf(stderr, "[dbg] obj %d type=%d\n", i, (int)v->type);
                if (!v || v->type != PDF_DICT) continue;
                type = pdf_dict_get(v, "Type");
                if (type && pdf_name(type) && !strcmp(pdf_name(type), "Catalog")) root = v;
            }
        }
        if (!root) {
            fprintf(stderr, "pdftops-lite: missing document catalog\n");
            pdf_close(doc);
            return 1;
        }
        {
            pdf_value *pages = pdf_resolve(doc, pdf_dict_get(root, "Pages"));
            if (!pages) {
                fprintf(stderr, "pdftops-lite: missing page tree\n");
                pdf_close(doc);
                return 1;
            }
            collect_pages(doc, pages, &pl, 0);
        }
    }

    if (pl.count == 0) {
        fprintf(stderr, "pdftops-lite: no pages found\n");
        pdf_close(doc);
        return 1;
    }

    psgen_init(&g, stdout);
    psgen_begin(&g);

    for (i = 0; i < pl.count; i++) {
        pdf_value *page = pl.pages[i];
        pdf_value *mb = inherited(doc, page, "MediaBox");
        pdf_value *res = inherited(doc, page, "Resources");
        double x0 = 0, y0 = 0, x1 = 612, y1 = 792;
        pdf_value *contents;
        if (mb && mb->type == PDF_ARRAY && mb->u.arr.count == 4) {
            x0 = pdf_number(mb->u.arr.items[0]);
            y0 = pdf_number(mb->u.arr.items[1]);
            x1 = pdf_number(mb->u.arr.items[2]);
            y1 = pdf_number(mb->u.arr.items[3]);
            if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
            if (y1 < y0) { double t = y0; y0 = y1; y1 = t; }
        }
        psgen_page_begin(&g, x0, y0, x1, y1);
        g.resources = res;
        contents = pdf_dict_get(page, "Contents");
        if (contents) psgen_page_content(doc, &g, contents);
        g.resources = NULL;
        psgen_page_end(&g);
    }

    psgen_end(&g);
    fflush(stdout);

    fprintf(stderr, "pdftops-lite: converted %d page(s)\n", pl.count);

    free(pl.pages);
    pdf_close(doc);
    return 0;
}
