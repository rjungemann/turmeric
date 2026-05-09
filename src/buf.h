#ifndef TUR_BUF_H
#define TUR_BUF_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct Buf {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_putc(Buf *b, char c);
void  buf_write(Buf *b, const char *s, size_t n);
void  buf_puts(Buf *b, const char *s); /* NUL-terminated */
void  buf_printf(Buf *b, const char *fmt, ...);
void  buf_vprintf(Buf *b, const char *fmt, va_list ap);
int   buf_to_file(const Buf *b, FILE *f); /* returns 0 on success */

#endif
