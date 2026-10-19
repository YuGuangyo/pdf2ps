/*
 * buf.h - growable byte buffer + inflate prototype
 */
#ifndef BUF_H
#define BUF_H

#include <stddef.h>

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
int  buf_reserve(buf_t *b, size_t need);
int  buf_put(buf_t *b, const void *p, size_t n);
int  buf_putc(buf_t *b, int c);
int  buf_puts(buf_t *b, const char *s);

int zlib_inflate(const unsigned char *in, size_t inlen, buf_t *out);
int ascii85_decode(const unsigned char *in, size_t inlen, buf_t *out);
int asciihex_decode(const unsigned char *in, size_t inlen, buf_t *out);

#endif
