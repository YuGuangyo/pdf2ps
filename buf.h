/*
 * buf.h - growable byte buffer + inflate prototype
 */
#ifndef BUF_H
#define BUF_H

#include <stddef.h>

/*
 * Hard ceiling for any single buffer. The router is 32-bit with ~200MB of
 * free RAM, so refusing anything above 256MB keeps the doubling code in
 * buf_reserve() far away from size_t overflow while still accepting every
 * realistic content stream.
 */
#define BUF_MAX_SIZE ((size_t)256u * 1024u * 1024u)

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
/*
 * buf_reserve(b, need) grows the *absolute* capacity to at least `need`.
 * To append n bytes pass b->len + n.
 */
int  buf_reserve(buf_t *b, size_t need);
int  buf_put(buf_t *b, const void *p, size_t n);
int  buf_putc(buf_t *b, int c);
int  buf_puts(buf_t *b, const char *s);

int zlib_inflate(const unsigned char *in, size_t inlen, buf_t *out);
int ascii85_decode(const unsigned char *in, size_t inlen, buf_t *out);
int asciihex_decode(const unsigned char *in, size_t inlen, buf_t *out);

#endif
