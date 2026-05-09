#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES 64

static const SourceFile *files_[MAX_FILES];
static size_t            file_count_;
static bool              had_error_;

void diag_register_file(const SourceFile *file) {
    if (file->file_id >= MAX_FILES) {
        fprintf(stderr, "tur: too many source files\n");
        abort();
    }
    files_[file->file_id] = file;
    if (file->file_id >= file_count_) file_count_ = (size_t)file->file_id + 1;
}

bool diag_had_error(void) { return had_error_; }

void diag_reset(void) {
    had_error_ = false;
    file_count_ = 0;
    for (size_t i = 0; i < MAX_FILES; i++) files_[i] = NULL;
}

static const char *level_name(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
    }
    return "?";
}

/* Find the start of the line containing offset off; returns pointer into src.
 * Sets *line_off to the byte offset of the line start. */
static const char *line_start(const SourceFile *f, uint32_t off, uint32_t *line_off) {
    if (off > f->len) off = (uint32_t)f->len;
    uint32_t i = off;
    while (i > 0 && f->src[i - 1] != '\n') i--;
    *line_off = i;
    return f->src + i;
}

void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap) {
    if (level == DIAG_ERROR) had_error_ = true;

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    fprintf(stderr, "%s:%u:%u: %s: ", path, span.line, span.col_start, level_name(level));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    /* Source snippet with caret. */
    if (f && span.off_start <= f->len) {
        uint32_t loff;
        const char *ls = line_start(f, span.off_start, &loff);
        const char *le = ls;
        while ((size_t)(le - f->src) < f->len && *le != '\n') le++;

        fprintf(stderr, "  %.*s\n", (int)(le - ls), ls);

        fputs("  ", stderr);
        uint32_t col = (span.col_start > 0) ? span.col_start - 1 : 0;
        for (uint32_t i = 0; i < col; i++) {
            char c = (i < (le - ls)) ? ls[i] : ' ';
            fputc(c == '\t' ? '\t' : ' ', stderr);
        }
        uint32_t caret_len = 1;
        if (span.col_end > span.col_start) caret_len = span.col_end - span.col_start;
        for (uint32_t i = 0; i < caret_len; i++) fputc('^', stderr);
        fputc('\n', stderr);
    }
}

void diag_emit(DiagLevel level, Span span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_emitv(level, span, fmt, ap);
    va_end(ap);
}
