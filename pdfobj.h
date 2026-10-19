/*
 * pdfobj.h - object model + cross-reference parsing for pdftops-lite
 */
#ifndef PDFOBJ_H
#define PDFOBJ_H

#include <stddef.h>
#include "buf.h"

typedef enum {
    PDF_NULL = 0,
    PDF_BOOL,
    PDF_INT,
    PDF_REAL,
    PDF_STRING,
    PDF_NAME,
    PDF_ARRAY,
    PDF_DICT,
    PDF_REF,
    PDF_STREAM
} pdf_type_t;

typedef struct pdf_value pdf_value;

typedef struct {
    char *key;
    pdf_value *value;
} pdf_pair;

struct pdf_value {
    pdf_type_t type;
    union {
        int b;
        long i;
        double r;
        struct { unsigned char *data; size_t len; } str;
        struct { char *name; } nm;
        struct { pdf_value **items; int count; } arr;
        struct { pdf_pair *pairs; int count; } dict;
        struct { int num; int gen; } ref;
        struct { pdf_value *dict; } stream;
    } u;
    /* stream payload (raw, undecoded) */
    unsigned char *sdata;
    size_t slen;
};

typedef struct {
    int num;
    int gen;
    long offset;
    int in_object_stream;   /* >0: 1-based index inside object stream */
    int objstm_num;
} xref_entry;

typedef struct {
    unsigned char *data;
    size_t len;
    xref_entry *xref;
    int xref_size;
    pdf_value *trailer;
    pdf_value **cache;
    int cache_size;
} pdf_doc;

pdf_doc *pdf_open(unsigned char *data, size_t len);
void pdf_close(pdf_doc *doc);

pdf_value *pdf_get(pdf_doc *doc, int num);
pdf_value *pdf_resolve(pdf_doc *doc, pdf_value *v);

void val_free(pdf_value *v);
void pdf_note_xref_stream(pdf_doc *doc, pdf_value *st);

pdf_value *pdf_dict_get(pdf_value *dict, const char *key);
const char *pdf_name(pdf_value *v);
double pdf_number(pdf_value *v);
int pdf_intval(pdf_value *v);

/* character classification (shared by lexers and content interpreter) */
static int is_ws_pdf(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

static int is_delim_pdf(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}

static int isdigit_pdf(int c) {
    return c >= '0' && c <= '9';
}

#endif
