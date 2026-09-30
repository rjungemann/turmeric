#ifndef TUR_BUF_H
#define TUR_BUF_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

/* printf-style format checking for the wrappers below: under -Wformat=2 the
 * compiler checks every call's arguments against its format and refuses a
 * non-literal one (security audit WP5).  Off on Windows, where MinGW's printf
 * archetype disagrees with the C99 specifiers used here and -Werror is off. */
#ifndef TUR_PRINTF_FMT
#  if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
#    define TUR_PRINTF_FMT(fmt_idx, first_arg) \
         __attribute__((format(printf, fmt_idx, first_arg)))
#  else
#    define TUR_PRINTF_FMT(fmt_idx, first_arg)
#  endif
#endif

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
void  buf_printf(Buf *b, const char *fmt, ...) TUR_PRINTF_FMT(2, 3);
void  buf_vprintf(Buf *b, const char *fmt, va_list ap) TUR_PRINTF_FMT(2, 0);
int   buf_to_file(const Buf *b, FILE *f); /* returns 0 on success */
int   buf_to_path(const Buf *b, const char *path); /* write to file by name, returns 0 on success */
char *tur_strdup(const char *s);

#endif
