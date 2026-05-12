#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

#define MAX_FILES 64
#define MAX_NOTES 8
#define MAX_SECONDARY_SPANS 4

static const SourceFile *files_[MAX_FILES];
static size_t            file_count_;
static bool              had_error_;
static bool              use_color_ = false;
static bool              json_output_ = false;  /* Phase 8: JSON diagnostics mode */

/* ANSI color codes for diagnostics */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

void diag_init(bool use_color) {
    use_color_ = use_color;
}

bool diag_use_color(void) {
    return use_color_;
}

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

/* Check if stderr is a TTY (for auto-color detection) */
bool stderr_is_tty(void) {
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 1
    return isatty(fileno(stderr));
#else
    return false; /* Default to no color on non-POSIX systems */
#endif
}

/* Get the color code for a diagnostic level */
static const char *color_for_level(DiagLevel level) {
    if (!use_color_) return "";
    switch (level) {
        case DIAG_ERROR:   return COLOR_RED;
        case DIAG_WARNING: return COLOR_YELLOW;
        case DIAG_NOTE:    return COLOR_CYAN;
        case DIAG_HELP:    return COLOR_GREEN;
    }
    return "";
}

static const char *level_name(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
        case DIAG_HELP:    return "help";
    }
    return "?";
}

/* Get the underline character for a style */
static char underline_char(UnderlineStyle style) {
    switch (style) {
        case UNDERLINE_PRIMARY:   return '^';
        case UNDERLINE_SECONDARY: return '~';
        case UNDERLINE_GAP:       return '-';
    }
    return '^';
}

/* Get error code string for display */
const char *diag_code_to_string(DiagCode code) {
    switch (code) {
        case TUR_E0001_TYPE_MISMATCH:     return "TUR-E0001";
        case TUR_E0002_ARITY_MISMATCH:    return "TUR-E0002";
        case TUR_E0003_UNBOUND_SYMBOL:   return "TUR-E0003";
        case TUR_E0004_INVALID_SCOPE:    return "TUR-E0004";
        case TUR_E0005_USE_AFTER_MOVE:    return "TUR-E0005";
        case TUR_E0006_OPERATOR_LOOKUP_FAILED: return "TUR-E0006";
        case TUR_E0007_CAPTURE_ERROR:     return "TUR-E0007";
        case TUR_E0009_EFFECT_ROW_MISMATCH: return "TUR-E0009";
        default:                          return "";
    }
}



/* Render a multi-line snippet with context, underlines, and colors.
   Phase 8: Enhanced with configurable options and multi-span support. */
static void render_snippet_ex(const SourceFile *f, Span span, const SnippetOpts *opts) {
    if (!f || span.off_start > f->len) return;
    
    const SnippetOpts default_opts = SNIPPET_OPTS_DEFAULT;
    const SnippetOpts *o = opts ? opts : &default_opts;
    
    const char *color = use_color_ ? COLOR_BOLD : "";
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Calculate the range of lines to show */
    uint32_t error_line = span.line;
    uint32_t context = o->context_lines;
    uint32_t start_line = (error_line > context + 1) ? error_line - context - 1 : 1;
    uint32_t end_line = error_line + context;
    
    /* Get the width for line numbers (padding) */
    int line_num_width = 1;
    uint32_t temp = end_line;
    while (temp >= 10) {
        line_num_width++;
        temp /= 10;
    }
    
    /* Print each line with line number and content */
    uint32_t current_line = 1;
    uint32_t line_start = 0;
    
    for (uint32_t i = 0; i <= f->len; i++) {
        if (i == f->len || f->src[i] == '\n') {
            /* Check if we should stop before processing this line */
            if (current_line > end_line) break;
            
            if (current_line >= start_line && current_line <= end_line) {
                /* Skip empty lines at the end of the file */
                uint32_t line_len = i - line_start;
                if (line_len == 0 && i == f->len) {
                    /* Empty line at end of file - skip it */
                } else {
                    /* Print line number and gutter */
                    if (o->show_line_numbers) {
                        if (current_line == error_line) {
                            fprintf(stderr, "%s%*u %s|%s ", color, line_num_width, current_line, reset, color);
                        } else {
                            fprintf(stderr, "%*u | ", line_num_width, current_line);
                        }
                    } else {
                        if (current_line == error_line) {
                            fprintf(stderr, "%s|%s ", color, reset);
                        } else {
                            fprintf(stderr, " | ");
                        }
                    }
                    
                    /* Print the line content */
                    if (f->src[line_start] != '\n') {
                        fwrite(f->src + line_start, 1, line_len, stderr);
                    }
                    fprintf(stderr, "%s\n", reset);
                    
                    /* Print underline for the error line */
                    if (current_line == error_line) {
                        /* Calculate column position (0-based in the line) */
                        uint32_t col_start_0 = span.col_start - 1;
                        uint32_t col_end_0 = span.col_end - 1;
                        
                        /* Print gutter space */
                        if (o->show_line_numbers) {
                            fprintf(stderr, "%*s | ", line_num_width, "");
                        } else {
                            fprintf(stderr, " | ");
                        }
                        
                        /* Print spaces up to the start column */
                        for (uint32_t c = 0; c < col_start_0; c++) {
                            char ch = (c < line_len) ? f->src[line_start + c] : ' ';
                            if (ch == '\t') {
                                fputc('\t', stderr);
                            } else {
                                fputc(' ', stderr);
                            }
                        }
                        
                        /* Print underline with the appropriate style */
                        uint32_t underline_len = (col_end_0 > col_start_0) ? col_end_0 - col_start_0 : 1;
                        fprintf(stderr, "%s", color);
                        char underline_ch = underline_char(o->primary_style);
                        for (uint32_t c = 0; c < underline_len; c++) {
                            fputc(underline_ch, stderr);
                        }
                        fprintf(stderr, "%s\n", reset);
                    }
                }
            }
            
            if (f->src[i] == '\n') {
                line_start = i + 1;
                current_line++;
            }
            
            /* If we just processed the last line, check if we should stop */
            if (i == f->len) break;
        }
    }
}

/* Original snippet rendering (backward compatible) */
static void render_snippet(const SourceFile *f, Span span) {
    render_snippet_ex(f, span, NULL);
}

/* Public snippet rendering function (Phase 8) */
void diag_render_snippet(const SourceFile *f, Span span, const SnippetOpts *opts) {
    render_snippet_ex(f, span, opts);
}

void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        diag_emit_json(level, span, DIAG_CODE_NONE, msg);
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    /* Color the level name */
    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Phase 8: Rust-style diagnostics with --> pointing to file */
    fprintf(stderr, "%s%s:%u:%u: %s%s: ", color, path, span.line, span.col_start, level_name(level), reset);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    /* Multi-line source snippet with context */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
}

void diag_emit(DiagLevel level, Span span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_emitv(level, span, fmt, ap);
    va_end(ap);
}

/* Emit diagnostic with error code (Phase 8) */
void diag_emit_with_code(DiagLevel level, Span span, DiagCode code, const char *fmt, ...) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        va_list ap;
        va_start(ap, fmt);
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        diag_emit_json(level, span, code, msg);
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    const char *code_str = diag_code_to_string(code);
    
    va_list ap;
    va_start(ap, fmt);
    
    if (code != DIAG_CODE_NONE) {
        fprintf(stderr, "%s%s:%u:%u: %s [%s]%s: ", color, path, span.line, span.col_start, level_name(level), code_str, reset);
    } else {
        fprintf(stderr, "%s%s:%u:%u: %s%s: ", color, path, span.line, span.col_start, level_name(level), reset);
    }
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    
    va_end(ap);

    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
}

/* Emit diagnostic with related notes (Phase 8) */
void diag_emit_with_notes(DiagLevel level, Span span, const char *message,
                          DiagNote *notes, size_t note_count) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit primary message in JSON format */
    if (json_output_) {
        diag_emit_json(level, span, DIAG_CODE_NONE, message);
        /* Also emit notes in JSON format */
        for (size_t i = 0; i < note_count; i++) {
            diag_emit_json(notes[i].level, notes[i].span, DIAG_CODE_NONE, notes[i].message);
        }
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Print primary error */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, span.line, span.col_start, level_name(level), reset, message);

    /* Print primary snippet */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
    
    /* Print notes */
    for (size_t i = 0; i < note_count; i++) {
        const SourceFile *note_f = NULL;
        if (notes[i].span.file_id < MAX_FILES) note_f = files_[notes[i].span.file_id];
        const char *note_path = note_f ? note_f->path : "<unknown>";
        const char *note_color = color_for_level(notes[i].level);
        
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", note_color, note_path, 
                notes[i].span.line, notes[i].span.col_start,
                level_name(notes[i].level), reset, notes[i].message);
        
        if (note_f && notes[i].span.off_start <= note_f->len) {
            SnippetOpts note_opts = SNIPPET_OPTS_DEFAULT;
            note_opts.primary_style = UNDERLINE_SECONDARY;
            render_snippet_ex(note_f, notes[i].span, &note_opts);
        }
    }
}

/* Emit diagnostic with suggestion (Phase 8) */
void diag_emit_with_suggestion(DiagLevel level, Span span, const char *message,
                               const DiagSuggestion *suggestion) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        diag_emit_json(level, span, DIAG_CODE_NONE, message);
        if (suggestion && suggestion->text) {
            diag_emit_json(DIAG_HELP, span, DIAG_CODE_NONE, suggestion->text);
        }
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    const char *help_color = use_color_ ? COLOR_GREEN : "";
    
    /* Print primary error */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, span.line, span.col_start, level_name(level), reset, message);

    /* Print snippet */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
    
    /* Print suggestion */
    if (suggestion && suggestion->text) {
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", help_color, path, span.line, span.col_start,
                level_name(DIAG_HELP), reset, suggestion->text);
        
        if (suggestion->replacement) {
            fprintf(stderr, "%s%s:%u:%u: %s%s: try: %s\n", help_color, path, span.line, span.col_start,
                    level_name(DIAG_HELP), reset, suggestion->replacement);
        }
        
        if (suggestion->doc_url) {
            fprintf(stderr, "%s%s:%u:%u: %s%s: see: %s\n", help_color, path, span.line, span.col_start,
                    level_name(DIAG_HELP), reset, suggestion->doc_url);
        }
    }
}

/* Emit multi-span diagnostic (Phase 8) */
void diag_emit_multi_span(DiagLevel level, const char *message,
                         Span primary_span, const char *primary_label,
                         Span *secondary_spans, const char **secondary_labels,
                         size_t secondary_count) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        diag_emit_json(level, primary_span, DIAG_CODE_NONE, message);
        for (size_t i = 0; i < secondary_count; i++) {
            diag_emit_json(DIAG_NOTE, secondary_spans[i], DIAG_CODE_NONE,
                           secondary_labels ? secondary_labels[i] : "");
        }
        return;
    }

    const SourceFile *f = NULL;
    if (primary_span.file_id < MAX_FILES) f = files_[primary_span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Print primary error with label */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, primary_span.line, primary_span.col_start,
            level_name(level), reset, message);
    
    /* Print primary snippet with label */
    if (f && primary_span.off_start <= f->len) {
        /* Render snippet */
        SnippetOpts opts = SNIPPET_OPTS_DEFAULT;
        render_snippet_ex(f, primary_span, &opts);
        
        /* Print label on its own line if we have one */
        if (primary_label) {
            int line_num_width = 1;
            uint32_t temp = primary_span.line + opts.context_lines;
            while (temp >= 10) { line_num_width++; temp /= 10; }
            
            fprintf(stderr, "%*s | %*s%s%s\n", line_num_width, "", 
                    (int)(primary_span.col_start - 1), "", color, primary_label);
            fprintf(stderr, "%s", reset);
        }
    }
    
    /* Print secondary spans */
    for (size_t i = 0; i < secondary_count; i++) {
        const SourceFile *sec_f = NULL;
        if (secondary_spans[i].file_id < MAX_FILES) sec_f = files_[secondary_spans[i].file_id];
        const char *sec_path = sec_f ? sec_f->path : "<unknown>";
        const char *note_color = color_for_level(DIAG_NOTE);
        
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", note_color, sec_path,
                secondary_spans[i].line, secondary_spans[i].col_start,
                level_name(DIAG_NOTE), reset, secondary_labels ? secondary_labels[i] : "");
        
        if (sec_f && secondary_spans[i].off_start <= sec_f->len) {
            SnippetOpts sec_opts = SNIPPET_OPTS_DEFAULT;
            sec_opts.primary_style = UNDERLINE_SECONDARY;
            render_snippet_ex(sec_f, secondary_spans[i], &sec_opts);
        }
    }
}

/* JSON diagnostics support (Phase 8) */

void diag_set_json_output(bool enabled) {
    json_output_ = enabled;
}

/* Escape a string for JSON output */
static void json_escape_string(Buf *b, const char *s) {
    if (!s) {
        buf_puts(b, "null");
        return;
    }
    buf_putc(b, '"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\b': buf_puts(b, "\\b");  break;
            case '\f': buf_puts(b, "\\f");  break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\r': buf_puts(b, "\\r");  break;
            case '\t': buf_puts(b, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    buf_printf(b, "\\u%04x", (unsigned char)*p);
                } else {
                    buf_putc(b, *p);
                }
        }
    }
    buf_putc(b, '"');
}

/* Emit a diagnostic in JSON format */
void diag_emit_json(DiagLevel level, Span span, DiagCode code, const char *message) {
    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *file = f ? f->path : "<unknown>";
    const char *severity = level_name(level);
    const char *code_str = diag_code_to_string(code);
    
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\n");
    buf_printf(&b, "  \"severity\": ");
    json_escape_string(&b, severity);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"code\": ");
    json_escape_string(&b, code != DIAG_CODE_NONE ? code_str : "");
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"message\": ");
    json_escape_string(&b, message);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"file\": ");
    json_escape_string(&b, file);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"line\": %u,\n", span.line);
    buf_printf(&b, "  \"col\": %u,\n", span.col_start);
    buf_printf(&b, "  \"endLine\": %u,\n", span.line);
    buf_printf(&b, "  \"endCol\": %u\n", span.col_end);
    buf_puts(&b, "}\n");
    
    fwrite(b.data, 1, b.len, stderr);
    buf_free(&b);
}


