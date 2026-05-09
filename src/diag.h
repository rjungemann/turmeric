#ifndef TUR_DIAG_H
#define TUR_DIAG_H

#include <stdarg.h>
#include <stdbool.h>

#include "forms.h"

typedef enum DiagLevel {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE,
} DiagLevel;

typedef struct SourceFile {
    const char *path;
    const char *src;     /* full source text */
    size_t      len;
    uint16_t    file_id;
} SourceFile;

void diag_register_file(const SourceFile *file);
void diag_emit(DiagLevel level, Span span, const char *fmt, ...);
void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap);
bool diag_had_error(void);
void diag_reset(void);

#endif
