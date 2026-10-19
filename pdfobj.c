/*
 * pdfobj.c - PDF object model, lexer, xref parsing, object/stream decoding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdfobj.h"

/* ---------------- value constructors ---------------- */

static pdf_value *val_new(pdf_type_t t) {
    pdf_value *v = (pdf_value *)calloc(1, sizeof(pdf_value));
    if (v) v->type = t;
    return v;
}

void val_free(pdf_value *v);

void val_free(pdf_value *v) {
    int i;
    if (!v) return;
    switch (v->type) {
        case PDF_STRING:
            free(v->u.str.data);
            break;
        case PDF_NAME:
            free(v->u.nm.name);
            break;
        case PDF_ARRAY:
            for (i = 0; i < v->u.arr.count; i++) val_free(v->u.arr.items[i]);
            free(v->u.arr.items);
            break;
        case PDF_DICT:
            for (i = 0; i < v->u.dict.count; i++) {
                free(v->u.dict.pairs[i].key);
                val_free(v->u.dict.pairs[i].value);
            }
            free(v->u.dict.pairs);
            break;
        case PDF_STREAM:
            val_free(v->u.stream.dict);
            free(v->sdata);
            break;
        default:
            break;
    }
    free(v);
}

/* ---------------- lexer ---------------- */

static int is_ws(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

static int is_delim(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}

static const unsigned char *skip_ws(const unsigned char *p, const unsigned char *end) {
    while (p < end) {
        if (is_ws(*p)) { p++; continue; }
        if (*p == '%') {
            while (p < end && *p != '\n' && *p != '\r') p++;
            continue;
        }
        break;
    }
    return p;
}

typedef struct {
    const unsigned char *p;
    const unsigned char *end;
} lexer_t;

static int read_word(lexer_t *lx, char *out, size_t cap) {
    size_t n = 0;
    const unsigned char *p = lx->p;
    while (p < lx->end && !is_ws(*p) && !is_delim(*p)) {
        if (n + 1 < cap) out[n++] = (char)*p;
        p++;
    }
    out[n] = 0;
    lx->p = p;
    return (int)n;
}

static void read_name(lexer_t *lx, char **out) {
    size_t n = 0;
    const unsigned char *p = lx->p;
    while (p < lx->end && !is_ws(*p) && !is_delim(*p)) { p++; n++; }
    *out = (char *)malloc(n + 1);
    if (*out) {
        memcpy(*out, lx->p, n);
        (*out)[n] = 0;
    }
    lx->p = p;
}

static void read_literal_string(lexer_t *lx, buf_t *out) {
    const unsigned char *p = lx->p;
    int depth = 1;
    p++; /* skip '(' */
    while (p < lx->end && depth > 0) {
        int c = *p++;
        if (c == '\\') {
            int e;
            if (p >= lx->end) break;
            e = *p++;
            switch (e) {
                case 'n': buf_putc(out, '\n'); break;
                case 'r': buf_putc(out, '\r'); break;
                case 't': buf_putc(out, '\t'); break;
                case 'b': buf_putc(out, '\b'); break;
                case 'f': buf_putc(out, '\f'); break;
                case '\r':
                    if (p < lx->end && *p == '\n') p++;  /* line continuation */
                    break;
                case '\n':
                    break;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0', k = 0;
                        while (k < 2 && p < lx->end && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                        buf_putc(out, v & 0xFF);
                    } else {
                        buf_putc(out, e);
                    }
            }
        } else if (c == '(') {
            depth++;
            buf_putc(out, c);
        } else if (c == ')') {
            depth--;
            if (depth > 0) buf_putc(out, c);
        } else {
            buf_putc(out, c);
        }
    }
    lx->p = p;
}

static void read_hex_string(lexer_t *lx, buf_t *out) {
    const unsigned char *p = lx->p;
    int hi = -1;
    p++; /* skip '<' */
    while (p < lx->end && *p != '>') {
        int c = *p++;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else { buf_putc(out, (hi << 4) | v); hi = -1; }
    }
    if (hi >= 0) buf_putc(out, hi << 4);
    if (p < lx->end && *p == '>') p++;
    lx->p = p;
}

static pdf_value *parse_object(lexer_t *lx, int depth);

static pdf_value *parse_dict_or_stream(lexer_t *lx, int depth) {
    pdf_value *dict = val_new(PDF_DICT);
    pdf_pair *pairs = NULL;
    int count = 0, cap = 0;
    if (!dict) return NULL;
    lx->p += 2; /* skip '<<' */

    for (;;) {
        char *key = NULL;
        pdf_value *val;
        lx->p = skip_ws(lx->p, lx->end);
        if (lx->p >= lx->end) break;
        if (lx->p + 1 < lx->end && lx->p[0] == '>' && lx->p[1] == '>') { lx->p += 2; break; }
        if (*lx->p != '/') {
            char junk[128];
            if (read_word(lx, junk, sizeof junk) == 0) lx->p++;
            continue;
        }
        lx->p++;
        read_name(lx, &key);
        lx->p = skip_ws(lx->p, lx->end);
        val = parse_object(lx, depth + 1);
        if (count + 1 > cap) {
            pdf_pair *np;
            cap = cap ? cap * 2 : 8;
            np = (pdf_pair *)realloc(pairs, (size_t)cap * sizeof(pdf_pair));
            if (!np) { free(key); val_free(val); break; }
            pairs = np;
        }
        pairs[count].key = key;
        pairs[count].value = val;
        count++;
    }
    dict->u.dict.pairs = pairs;
    dict->u.dict.count = count;

    /* check for stream */
    lx->p = skip_ws(lx->p, lx->end);
    if (lx->p + 6 <= lx->end && memcmp(lx->p, "stream", 6) == 0) {
        pdf_value *stream = val_new(PDF_STREAM);
        long length = -1;
        if (!stream) return dict;
        stream->u.stream.dict = dict;
        stream->sdata = NULL;
        stream->slen = 0;
        {
            pdf_value *lenv = pdf_dict_get(dict, "Length");
            if (lenv && lenv->type == PDF_INT && lenv->u.i >= 0) length = lenv->u.i;
        }
        lx->p += 6;
        if (lx->p < lx->end && *lx->p == '\r') lx->p++;
        if (lx->p < lx->end && *lx->p == '\n') lx->p++;
        if (length >= 0 && lx->p + length <= lx->end) {
            stream->sdata = (unsigned char *)malloc((size_t)(length > 0 ? length : 1));
            if (stream->sdata) {
                if (length > 0) memcpy(stream->sdata, lx->p, (size_t)length);
                stream->slen = (size_t)length;
            }
            lx->p += length;
        } else {
            /* unknown length: scan for endstream */
            const unsigned char *q = lx->p;
            const unsigned char *es = NULL;
            while (q + 9 <= lx->end) {
                if (memcmp(q, "endstream", 9) == 0) { es = q; break; }
                q++;
            }
            if (es) {
                size_t n = (size_t)(es - lx->p);
                while (n > 0 && (lx->p[n - 1] == '\r' || lx->p[n - 1] == '\n')) n--;
                stream->sdata = (unsigned char *)malloc(n ? n : 1);
                if (stream->sdata) {
                    if (n) memcpy(stream->sdata, lx->p, n);
                    stream->slen = n;
                }
                lx->p = es + 9;
            } else {
                lx->p = lx->end;
            }
        }
        return stream;
    }
    return dict;
}

static pdf_value *parse_object(lexer_t *lx, int depth) {
    char tok[256];
    const unsigned char *save;
    pdf_value *v;

    if (depth > 64) return NULL;
    lx->p = skip_ws(lx->p, lx->end);
    if (lx->p >= lx->end) return NULL;

    if (*lx->p == '<' && lx->p + 1 < lx->end && lx->p[1] == '<')
        return parse_dict_or_stream(lx, depth);

    if (*lx->p == '[') {
        pdf_value **items = NULL;
        int count = 0, cap = 0;
        v = val_new(PDF_ARRAY);
        if (!v) return NULL;
        lx->p++;
        for (;;) {
            pdf_value *it;
            lx->p = skip_ws(lx->p, lx->end);
            if (lx->p >= lx->end) break;
            if (*lx->p == ']') { lx->p++; break; }
            it = parse_object(lx, depth + 1);
            if (!it) break;
            if (count + 1 > cap) {
                pdf_value **np;
                cap = cap ? cap * 2 : 8;
                np = (pdf_value **)realloc(items, (size_t)cap * sizeof(pdf_value *));
                if (!np) { val_free(it); break; }
                items = np;
            }
            items[count++] = it;
        }
        v->u.arr.items = items;
        v->u.arr.count = count;
        return v;
    }

    if (*lx->p == '/') {
        char *nm = NULL;
        lx->p++;
        read_name(lx, &nm);
        v = val_new(PDF_NAME);
        if (v) v->u.nm.name = nm;
        else free(nm);
        return v;
    }

    if (*lx->p == '(') {
        buf_t sb;
        buf_init(&sb);
        read_literal_string(lx, &sb);
        v = val_new(PDF_STRING);
        if (v) {
            v->u.str.data = sb.data;
            v->u.str.len = sb.len;
        } else {
            buf_free(&sb);
        }
        return v;
    }

    if (*lx->p == '<') {
        buf_t sb;
        buf_init(&sb);
        read_hex_string(lx, &sb);
        v = val_new(PDF_STRING);
        if (v) {
            v->u.str.data = sb.data;
            v->u.str.len = sb.len;
        } else {
            buf_free(&sb);
        }
        return v;
    }

    save = lx->p;
    read_word(lx, tok, sizeof tok);
    if (tok[0] == 0) {
        lx->p++;
        return parse_object(lx, depth + 1);
    }
    if (!strcmp(tok, "true")) { v = val_new(PDF_BOOL); if (v) v->u.b = 1; return v; }
    if (!strcmp(tok, "false")) { v = val_new(PDF_BOOL); if (v) v->u.b = 0; return v; }
    if (!strcmp(tok, "null")) return NULL;

    /* number, possibly a reference "N G R" */
    {
        char *endp = NULL;
        long iv = strtol(tok, &endp, 10);
        if (endp && *endp == 0 && endp != tok) {
            const unsigned char *after = lx->p;
                char tok2[64];
                lexer_t lx2;
                lx2.p = skip_ws(after, lx->end);
                lx2.end = lx->end;
                if (read_word(&lx2, tok2, sizeof tok2) > 0 &&
                    strspn(tok2, "0123456789") == strlen(tok2)) {
                    const unsigned char *after2 = lx2.p;
                    char tok3[8];
                    lexer_t lx3;
                    long gen = atol(tok2);
                    lx3.p = skip_ws(after2, lx->end);
                    lx3.end = lx->end;
                    if (read_word(&lx3, tok3, sizeof tok3) > 0 && !strcmp(tok3, "R")) {
                        lx->p = lx3.p;
                        v = val_new(PDF_REF);
                        if (v) { v->u.ref.num = (int)iv; v->u.ref.gen = (int)gen; }
                        return v;
                    }
                }
            v = val_new(PDF_INT);
            if (v) v->u.i = iv;
            return v;
        }
        {
            char *r = NULL;
            double dv = strtod(tok, &r);
            if (r && *r == 0) {
                v = val_new(PDF_REAL);
                if (v) v->u.r = dv;
                return v;
            }
        }
    }
    lx->p = save;
    read_word(lx, tok, sizeof tok);
    if (lx->p == save) lx->p++;
    return NULL;
}

/* ---------------- dict accessors ---------------- */

pdf_value *pdf_dict_get(pdf_value *dict, const char *key) {
    int i;
    if (!dict || dict->type != PDF_DICT) return NULL;
    for (i = 0; i < dict->u.dict.count; i++) {
        if (dict->u.dict.pairs[i].key && !strcmp(dict->u.dict.pairs[i].key, key))
            return dict->u.dict.pairs[i].value;
    }
    return NULL;
}

const char *pdf_name(pdf_value *v) {
    if (!v || v->type != PDF_NAME) return NULL;
    return v->u.nm.name;
}

double pdf_number(pdf_value *v) {
    if (!v) return 0.0;
    if (v->type == PDF_INT) return (double)v->u.i;
    if (v->type == PDF_REAL) return v->u.r;
    return 0.0;
}

int pdf_intval(pdf_value *v) {
    if (!v) return 0;
    if (v->type == PDF_INT) return (int)v->u.i;
    if (v->type == PDF_REAL) return (int)v->u.r;
    return 0;
}

/* ---------------- document ---------------- */

static void ensure_xref_capacity(pdf_doc *doc, int size) {
    int i;
    /*
     * xref_size is 'one past the highest usable index', i.e. valid indices
     * are 0..xref_size-1. Writing index `size` therefore requires a table
     * of at least size+1 entries, so only a strictly smaller index can be
     * served by the current allocation. Using `<=` here let index
     * `xref_size` be written out of bounds.
     */
    if (size < doc->xref_size) return;
    {
        xref_entry *nx = (xref_entry *)realloc(doc->xref, (size_t)(size + 1) * sizeof(xref_entry));
        if (!nx) return;
        for (i = doc->xref_size; i <= size; i++) {
            nx[i].num = 0;
            nx[i].gen = 0;
            nx[i].offset = 0;
            nx[i].in_object_stream = 0;
            nx[i].objstm_num = 0;
        }
        doc->xref = nx;
        doc->xref_size = size + 1;
    }
}

void pdf_note_xref_stream(pdf_doc *doc, pdf_value *st) {
    pdf_value *w;
    pdf_value *idx;
    pdf_value *sizev;
    pdf_value *typev;
    int size, w0 = 1, w1 = 2, w2 = 1;

    if (!st || st->type != PDF_STREAM) return;
    typev = pdf_dict_get(st->u.stream.dict, "Type");
    if (typev && pdf_name(typev) && strcmp(pdf_name(typev), "XRef")) return;

    sizev = pdf_dict_get(st->u.stream.dict, "Size");
    size = sizev ? pdf_intval(sizev) : 0;
    if (size <= 0) return;
    ensure_xref_capacity(doc, size);

    w = pdf_dict_get(st->u.stream.dict, "W");
    if (w && w->type == PDF_ARRAY && w->u.arr.count == 3) {
        w0 = pdf_intval(w->u.arr.items[0]);
        w1 = pdf_intval(w->u.arr.items[1]);
        w2 = pdf_intval(w->u.arr.items[2]);
    }
    if (w0 < 0) w0 = 0;
    if (w1 < 0) w1 = 0;
    if (w2 < 0) w2 = 0;
    if (w0 + w1 + w2 == 0) return;

    idx = pdf_dict_get(st->u.stream.dict, "Index");
    {
        int entry = w0 + w1 + w2;
        size_t pos = 0;
        int total = 1;
        int k;
        if (idx && idx->type == PDF_ARRAY && idx->u.arr.count >= 2)
            total = idx->u.arr.count / 2;
        for (k = 0; k < total; k++) {
            long startobj = 0, cnt = size;
            int j;
            if (idx && idx->type == PDF_ARRAY && idx->u.arr.count >= (k + 1) * 2) {
                startobj = pdf_intval(idx->u.arr.items[k * 2]);
                cnt = pdf_intval(idx->u.arr.items[k * 2 + 1]);
            }
            for (j = 0; j < cnt; j++) {
                long type = 0, f2 = 0, f3 = 0;
                int a;
                long num;
                if (st->sdata == NULL) break;
                if (pos + (size_t)entry > st->slen) break;
                for (a = 0; a < w0; a++) type = (type << 8) | st->sdata[pos++];
                for (a = 0; a < w1; a++) f2 = (f2 << 8) | st->sdata[pos++];
                for (a = 0; a < w2; a++) f3 = (f3 << 8) | st->sdata[pos++];
                num = startobj + j;
                if (num > 0 && num < doc->xref_size) {
                    if (type == 1) {
                        if (doc->xref[num].offset == 0 && doc->xref[num].in_object_stream == 0) {
                            doc->xref[num].offset = f2;
                            doc->xref[num].num = (int)num;
                        }
                    } else if (type == 2) {
                        if (doc->xref[num].in_object_stream == 0 && doc->xref[num].offset == 0) {
                            doc->xref[num].objstm_num = (int)f2;
                            doc->xref[num].in_object_stream = (int)f3 + 1;
                            doc->xref[num].num = (int)num;
                        }
                    }
                }
            }
        }
    }
}

/* collect top-level object offsets by scanning "N G obj" markers */
static void scan_object_offsets(pdf_doc *doc) {
    const unsigned char *p = doc->data;
    const unsigned char *end = doc->data + doc->len;
    while (p < end) {
        long num = 0, gen = 0;
        const unsigned char *q = p;
        if (*q < '0' || *q > '9') { p++; continue; }
        while (q < end && *q >= '0' && *q <= '9') { num = num * 10 + (*q - '0'); q++; }
        if (q >= end || !is_ws(*q)) { p++; continue; }
        while (q < end && is_ws(*q)) q++;
        if (q >= end || *q < '0' || *q > '9') { p++; continue; }
        while (q < end && *q >= '0' && *q <= '9') { gen = gen * 10 + (*q - '0'); q++; }
        if (q >= end || !is_ws(*q)) { p++; continue; }
        while (q < end && is_ws(*q)) q++;
        if (q + 3 > end || memcmp(q, "obj", 3)) { p++; continue; }
        (void)gen;
        if (num > 0) {
            ensure_xref_capacity(doc, (int)num);
            if (num < doc->xref_size) {
                if (doc->xref[num].offset == 0 && doc->xref[num].in_object_stream == 0) {
                    doc->xref[num].offset = (long)(p - doc->data);
                    doc->xref[num].num = (int)num;
                }
            }
        }
        p = q + 3;
    }
}

/* locate trailer dictionary (classic "trailer" keyword or last object) */
static void find_trailer(pdf_doc *doc) {
    const unsigned char *p = doc->data;
    const unsigned char *last = NULL;
    while ((size_t)(p - doc->data) < doc->len) {
        if (doc->len - (size_t)(p - doc->data) >= 7 && !memcmp(p, "trailer", 7)) last = p;
        p++;
    }
    if (last) {
        lexer_t lx;
        lx.p = last + 7;
        lx.end = doc->data + doc->len;
        doc->trailer = parse_object(&lx, 0);
    }
    if (!doc->trailer) {
        /*
         * No "trailer" keyword. Either the file is xref-stream based (the
         * /Root lives in the xref stream dict), or it is malformed and the
         * caller has to locate the catalog itself (see main.c).
         */
        int i;
        for (i = 1; i < doc->xref_size; i++) {
            pdf_value *v, *type;
            if (doc->xref[i].offset <= 0) continue;
            v = pdf_get(doc, i);
            if (!v || v->type != PDF_DICT) continue;
            type = pdf_dict_get(v, "Type");
            if (type && pdf_name(type) && !strcmp(pdf_name(type), "XRef")) {
                doc->trailer = v;          /* xref stream dict carries /Root */
                break;
            }
        }
    }
}

pdf_doc *pdf_open(unsigned char *data, size_t len) {
    pdf_doc *doc = (pdf_doc *)calloc(1, sizeof(pdf_doc));
    if (!doc) return NULL;
    doc->data = data;
    doc->len = len;
    doc->xref_size = 1;
    doc->xref = (xref_entry *)calloc(2, sizeof(xref_entry));
    doc->cache_size = 1;
    doc->cache = (pdf_value **)calloc(2, sizeof(pdf_value *));
    if (!doc->xref || !doc->cache) { pdf_close(doc); return NULL; }

    scan_object_offsets(doc);
    if (doc->xref_size > doc->cache_size) {
        pdf_value **nc = (pdf_value **)realloc(doc->cache, (size_t)doc->xref_size * sizeof(pdf_value *));
        if (nc) {
            size_t k;
            for (k = (size_t)doc->cache_size; k < (size_t)doc->xref_size; k++) nc[k] = NULL;
            doc->cache = nc;
            doc->cache_size = doc->xref_size;
        }
    }
    find_trailer(doc);
    return doc;
}

void pdf_close(pdf_doc *doc) {
    int i;
    if (!doc) return;
    if (doc->trailer) {
        /* trailer is normally one of the cached values: drop cache first,
         * then only free the trailer if it is not in the cache. */
        int in_cache = 0;
        for (i = 0; i < doc->cache_size; i++)
            if (doc->cache[i] == doc->trailer) { in_cache = 1; break; }
        for (i = 0; i < doc->cache_size; i++) {
            if (doc->cache[i]) { val_free(doc->cache[i]); doc->cache[i] = NULL; }
        }
        if (!in_cache) val_free(doc->trailer);
    } else {
        for (i = 0; i < doc->cache_size; i++)
            if (doc->cache[i]) val_free(doc->cache[i]);
    }
    free(doc->cache);
    free(doc->xref);
    free(doc->data);
    free(doc);
}

/* ---------- object loading ---------- */

static pdf_value *load_from_offset(pdf_doc *doc, long offset) {
    lexer_t lx;
    char tok[64];
    if (offset <= 0 || (size_t)offset >= doc->len) return NULL;
    lx.p = doc->data + offset;
    lx.end = doc->data + doc->len;
    if (read_word(&lx, tok, sizeof tok) == 0) return NULL;
    lx.p = skip_ws(lx.p, lx.end);
    if (read_word(&lx, tok, sizeof tok) == 0) return NULL;
    lx.p = skip_ws(lx.p, lx.end);
    if (read_word(&lx, tok, sizeof tok) == 0) return NULL;
    if (strcmp(tok, "obj")) return NULL;
    return parse_object(&lx, 0);
}

static pdf_value *load_from_objstm(pdf_doc *doc, int stmnum, int index0) {
    pdf_value *stm;
    pdf_value *n;
    pdf_value *firstv;
    long first = 0;
    long offset = 0;
    int i;
    char tok[64];

    stm = pdf_get(doc, stmnum);
    if (!stm || stm->type != PDF_STREAM || !stm->sdata) return NULL;
    n = pdf_dict_get(stm->u.stream.dict, "N");
    firstv = pdf_dict_get(stm->u.stream.dict, "First");
    if (!n) return NULL;
    if (firstv) first = (long)pdf_intval(firstv);

    /* header holds N pairs of "<objnum> <offset>" */
    {
        lexer_t lx;
        lx.p = stm->sdata;
        lx.end = stm->sdata + stm->slen;
        for (i = 0; i < pdf_intval(n); i++) {
            lx.p = skip_ws(lx.p, lx.end);
            if (read_word(&lx, tok, sizeof tok) == 0) return NULL;
            lx.p = skip_ws(lx.p, lx.end);
            if (read_word(&lx, tok, sizeof tok) == 0) return NULL;
            if (i == index0) {
                offset = atol(tok);
                break;
            }
        }
    }
    {
        lexer_t lx;
        size_t start = (size_t)(first + offset);
        if (start >= stm->slen) return NULL;
        lx.p = stm->sdata + start;
        lx.end = stm->sdata + stm->slen;
        return parse_object(&lx, 0);
    }
}

pdf_value *pdf_get(pdf_doc *doc, int num) {
    pdf_value *v = NULL;
    if (!doc || num <= 0 || num >= doc->xref_size) return NULL;
    if (doc->cache[num]) return doc->cache[num];
    if (doc->xref[num].in_object_stream > 0) {
        v = load_from_objstm(doc, doc->xref[num].objstm_num, doc->xref[num].in_object_stream - 1);
    } else if (doc->xref[num].offset > 0) {
        v = load_from_offset(doc, doc->xref[num].offset);
    }
    if (v) doc->cache[num] = v;
    return v;
}

pdf_value *pdf_resolve(pdf_doc *doc, pdf_value *v) {
    int guard = 0;
    while (v && v->type == PDF_REF && guard++ < 32) {
        v = pdf_get(doc, v->u.ref.num);
    }
    return v;
}


/* =====================================================================
 * Stream decoding
 *
 * Font programs, ICC-lite payloads and content streams all arrive
 * compressed, and a font we cannot decompress is a font we cannot embed.
 * The decoders below are deliberately total: every one of them bounds-checks
 * its input and reports failure instead of trusting /Length, because a
 * malformed font program must degrade to a fallback face, not kill the job.
 * ===================================================================== */

/* ---------- RunLengthDecode (PDF 7.4.5) ---------- */
static int runlength_decode(const unsigned char *in, size_t inlen, buf_t *out) {
    size_t i = 0;
    while (i < inlen) {
        unsigned len = in[i++];
        if (len == 128) break;                  /* EOD */
        if (len < 128) {
            size_t n = (size_t)len + 1;
            if (i + n > inlen) n = inlen - i;
            if (!buf_put(out, in + i, n)) return 0;
            i += n;
        } else {
            size_t n = 257u - (size_t)len;
            int c;
            if (i >= inlen) break;
            c = in[i++];
            while (n--) { if (!buf_putc(out, c)) return 0; }
        }
    }
    return 1;
}

/* ---------- LZWDecode (PDF 7.4.4, variable 9..12 bit codes) ---------- */
#define LZW_MAX_CODES  4096
#define LZW_MAX_BYTES  (1u << 20)

static int lzw_decode(const unsigned char *in, size_t inlen, int early,
                      buf_t *out) {
    /* dictionary: each entry is (prefix, first_byte, length) so rebuilding a
     * string walks the prefix chain without keeping 4096 variable buffers. */
    static short   prefix[LZW_MAX_CODES];
    static unsigned char suffix[LZW_MAX_CODES];
    static unsigned short entlen[LZW_MAX_CODES];
    unsigned char *stack;
    int code_bits = 9;
    int next = 258;
    int prev = -1;
    unsigned bitbuf = 0;
    int bitcnt = 0;
    size_t i = 0;
    int k;

    for (k = 0; k < 256; k++) { prefix[k] = -1; suffix[k] = (unsigned char)k; entlen[k] = 1; }
    prefix[256] = prefix[257] = -1; entlen[256] = entlen[257] = 0;

    stack = (unsigned char *)malloc(LZW_MAX_BYTES);
    if (!stack) return 0;

    while (1) {
        int code;
        while (bitcnt < code_bits && i < inlen) {
            bitbuf = (bitbuf << 8) | in[i++];
            bitcnt += 8;
        }
        if (bitcnt < code_bits) break;
        code = (int)((bitbuf >> (bitcnt - code_bits)) & ((1u << code_bits) - 1u));
        bitcnt -= code_bits;

        if (code == 256) {                       /* clear table */
            code_bits = 9;
            next = 258;
            prev = -1;
            continue;
        }
        if (code == 257) break;                  /* EOD */
        if (code > next) break;                  /* corrupt */

        {
            int emit_code = code;
            size_t sp = 0;

            if (code == next) {
                /* KwKwK case: prev string + its own first byte */
                if (prev < 0) break;
                {
                    int c = prev;
                    while (c >= 0 && sp < LZW_MAX_BYTES) { stack[sp++] = suffix[c]; c = prefix[c]; }
                }
                /* reverse in place, then append first byte again */
                {
                    size_t a = 0, b = sp ? sp - 1 : 0;
                    while (a < b) { unsigned char t = stack[a]; stack[a] = stack[b]; stack[b] = t; a++; b--; }
                }
                if (sp >= LZW_MAX_BYTES) break;
                stack[sp] = stack[0];
                sp++;
            } else {
                int c = emit_code;
                while (c >= 0 && sp < LZW_MAX_BYTES) { stack[sp++] = suffix[c]; c = prefix[c]; }
                {
                    size_t a = 0, b = sp ? sp - 1 : 0;
                    while (a < b) { unsigned char t = stack[a]; stack[a] = stack[b]; stack[b] = t; a++; b--; }
                }
            }
            if (!buf_put(out, stack, sp)) { free(stack); return 0; }

            /* add prev+first_byte(stack) to the table */
            if (prev >= 0 && next < LZW_MAX_CODES) {
                prefix[next] = (short)prev;
                suffix[next] = stack[0];
                entlen[next] = (unsigned short)(entlen[prev] + 1);
                next++;
                if (next + early >= (1 << code_bits) && code_bits < 12) code_bits++;
            }
        }
        prev = code;
    }
    free(stack);
    return 1;
}

/* ---------- PNG / TIFF predictors (PDF 7.4.4.4) ---------- */
static int apply_predictor(const unsigned char *in, size_t inlen, buf_t *out,
                           int predictor, int colors, int bpc, int columns) {
    size_t bpp, rowbytes;
    size_t y;

    if (predictor <= 1) return buf_put(out, in, inlen);
    if (colors < 1) colors = 1;
    if (bpc < 1) bpc = 8;
    if (columns < 1) columns = 1;

    bpp = ((size_t)colors * (size_t)bpc + 7u) / 8u;
    if (bpp < 1) bpp = 1;
    rowbytes = ((size_t)colors * (size_t)bpc * (size_t)columns + 7u) / 8u;
    if (rowbytes == 0) return buf_put(out, in, inlen);

    if (predictor == 2) {
        /* TIFF predictor 2: per-component horizontal differencing. Only the
         * 8-bit case is common in PDF fonts and images. */
        if (bpc != 8) return buf_put(out, in, inlen);
        if (!buf_put(out, in, inlen)) return 0;
        for (y = 0; y + rowbytes <= out->len; y += rowbytes) {
            size_t x;
            for (x = (size_t)colors; x < rowbytes; x++) {
                out->data[y + x] = (unsigned char)(out->data[y + x] + out->data[y + x - (size_t)colors]);
            }
        }
        return 1;
    }

    /* PNG predictors: every row is prefixed with a filter-type byte. */
    {
        unsigned char *prev = NULL;
        size_t pos = 0;
        unsigned char *cur = (unsigned char *)malloc(rowbytes);
        if (!cur) return 0;
        while (pos < inlen) {
            int ft = in[pos++];
            size_t n = rowbytes;
            size_t x;
            if (pos + n > inlen) n = inlen - pos;
            if (n == 0) break;
            memcpy(cur, in + pos, n);
            pos += n;
            for (x = 0; x < n; x++) {
                int a = (x >= bpp) ? cur[x - bpp] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
                switch (ft) {
                    case 1: cur[x] = (unsigned char)(cur[x] + a); break;
                    case 2: cur[x] = (unsigned char)(cur[x] + b); break;
                    case 3: cur[x] = (unsigned char)(cur[x] + ((a + b) >> 1)); break;
                    case 4: {
                        int p = a + b - c;
                        int pa = p > a ? p - a : a - p;
                        int pb = p > b ? p - b : b - p;
                        int pc = p > c ? p - c : c - p;
                        cur[x] = (unsigned char)(cur[x] +
                                 ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c)));
                        break;
                    }
                    default: break;              /* filter 0 = None */
                }
            }
            if (!buf_put(out, cur, n)) { free(cur); return 0; }
            if (!prev) prev = (unsigned char *)malloc(rowbytes);
            else if (rowbytes) { /* reuse */ }
            if (prev) {
                size_t c2;
                for (c2 = 0; c2 < n; c2++) prev[c2] = cur[c2];
                for (; c2 < rowbytes; c2++) prev[c2] = 0;
            }
            (void)y;
        }
        free(cur);
        free(prev);
        return 1;
    }
}

static int int_dict(pdf_value *dict, const char *key, int dflt) {
    pdf_value *v = pdf_dict_get(dict, key);
    if (!v) return dflt;
    if (v->type == PDF_INT) return (int)v->u.i;
    if (v->type == PDF_REAL) return (int)v->u.r;
    return dflt;
}

int pdf_stream_decode(pdf_doc *doc, pdf_value *stream, buf_t *out) {
    pdf_value *filter, *parms, *dict;
    unsigned char *data;
    size_t len;
    int nfilters = 1, fi;

    if (!out) return 0;
    if (!stream || stream->type != PDF_STREAM) return 0;
    dict = stream->u.stream.dict;
    data = stream->sdata;
    len = stream->slen;
    if (!data) { data = (unsigned char *)""; len = 0; }

    filter = pdf_resolve(doc, pdf_dict_get(dict, "Filter"));
    parms  = pdf_resolve(doc, pdf_dict_get(dict, "DecodeParms"));
    if (!filter || filter->type == PDF_NULL) return buf_put(out, data, len);

    if (filter->type == PDF_ARRAY) {
        nfilters = filter->u.arr.count;
        if (nfilters <= 0) return buf_put(out, data, len);
    }

    {
        buf_t cur, next;
        int ok = 1;

        buf_init(&cur);
        if (!buf_put(&cur, data, len)) { buf_free(&cur); return 0; }

        for (fi = 0; fi < nfilters && ok; fi++) {
            pdf_value *f = (filter->type == PDF_ARRAY)
                         ? pdf_resolve(doc, filter->u.arr.items[fi])
                         : filter;
            pdf_value *p = parms;
            const char *nm = pdf_name(f);

            buf_init(&next);
            if (!nm) { ok = 0; break; }

            if (!strcmp(nm, "FlateDecode") || !strcmp(nm, "Fl")) {
                ok = (zlib_inflate(cur.data, cur.len, &next) == 0);
            } else if (!strcmp(nm, "ASCII85Decode") || !strcmp(nm, "A85")) {
                ok = ascii85_decode(cur.data, cur.len, &next);
            } else if (!strcmp(nm, "ASCIIHexDecode") || !strcmp(nm, "AHx")) {
                ok = asciihex_decode(cur.data, cur.len, &next);
            } else if (!strcmp(nm, "LZWDecode") || !strcmp(nm, "LZW")) {
                ok = lzw_decode(cur.data, cur.len,
                                int_dict(p, "EarlyChange", 1), &next);
            } else if (!strcmp(nm, "RunLengthDecode") || !strcmp(nm, "RL")) {
                ok = runlength_decode(cur.data, cur.len, &next);
            } else if (!strcmp(nm, "Crypt")) {
                ok = buf_put(&next, cur.data, cur.len);   /* no-op */
            } else {
                ok = 0;                            /* image codec: refuse */
            }

            if (ok && p && p->type == PDF_DICT) {
                int pred = int_dict(p, "Predictor", 1);
                if (pred > 1) {
                    buf_t unp;
                    buf_init(&unp);
                    ok = apply_predictor(next.data, next.len, &unp, pred,
                                         int_dict(p, "Colors", 1),
                                         int_dict(p, "BitsPerComponent", 8),
                                         int_dict(p, "Columns", 1));
                    buf_free(&next);
                    next = unp;
                }
            }

            buf_free(&cur);
            cur = next;
            /* DecodeParms may itself be an array parallel to /Filter */
            if (parms && parms->type == PDF_ARRAY && fi < parms->u.arr.count)
                p = pdf_resolve(doc, parms->u.arr.items[fi]);
            (void)p;
        }

        if (ok && !buf_put(out, cur.data, cur.len)) ok = 0;
        buf_free(&cur);
        return ok;
    }
}