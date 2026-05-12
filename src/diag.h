#ifndef TUR_DIAG_H
#define TUR_DIAG_H

#include <stdarg.h>
#include <stdbool.h>

#include "forms.h"

/* Error codes for diagnostics (Phase 8: diagnostics polish) */
typedef enum DiagCode {
    DIAG_CODE_NONE = 0,
    /* Type errors */
    TUR_E0001_TYPE_MISMATCH,
    TUR_E0002_ARITY_MISMATCH,
    TUR_E0003_UNBOUND_SYMBOL,
    /* Scope errors */
    TUR_E0004_INVALID_SCOPE,
    TUR_E0005_USE_AFTER_MOVE,
    /* Operator errors */
    TUR_E0006_OPERATOR_LOOKUP_FAILED,
    /* Capture errors */
    TUR_E0007_CAPTURE_ERROR,
    /* Effect-row mismatch (P19-2) */
    TUR_E0009_EFFECT_ROW_MISMATCH,
    /* Thread safety (T19-B) */
    TUR_E0010_NOT_SEND,   /* type cannot be sent across thread boundaries */
    TUR_E0011_NOT_SYNC,   /* type cannot be shared across thread boundaries */
} DiagCode;

typedef enum DiagLevel {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE,
    DIAG_HELP,
} DiagLevel;

#define DIAG_CONTEXT_LINES 2  /* Number of context lines before/after the error line */

/* Reader types for #lang dispatch (Phase S1) */
typedef enum ReaderType {
    READER_UNKNOWN = -1,   /* Unknown/invalid #lang directive */
    READER_TURMERIC,       /* Default s-expression reader */
    READER_CURLY_INFIX,    /* Turmeric + curly-infix (SRFI-105) */
    READER_NEOTERIC,       /* Turmeric + neoteric notation */
    READER_SWEET,          /* Full sweet-expressions */
} ReaderType;

typedef struct SourceFile {
    const char *path;
    const char *src;     /* full source text */
    size_t      len;
    uint16_t    file_id;
    ReaderType  reader_type;  /* Phase S1: for enabling syntax features */
} SourceFile;

/* Detect #lang directive from file source (Phase S0) */
ReaderType detect_lang(const char *src, size_t len, const char **out_rest, 
                       size_t *out_rest_len);

/* Get reader type from file extension (Phase S0) */
ReaderType reader_type_from_extension(const char *path);

/* Get reader type name as string (Phase S0) */
const char *reader_type_name(ReaderType type);

/* Check if a reader type is implemented (Phase S0) */
bool reader_type_is_implemented(ReaderType type);

/* Underline style for diagnostics */
typedef enum UnderlineStyle {
    UNDERLINE_PRIMARY,   /* ^^^ for primary span */
    UNDERLINE_SECONDARY, /* ~~~ for secondary/related spans */
    UNDERLINE_GAP,       /* - for gaps between spans */
} UnderlineStyle;

/* A diagnostic note with its own span */
typedef struct DiagNote {
    DiagLevel level;
    Span span;
    const char *message;
} DiagNote;

/* A suggestion with optional replacement text */
typedef struct DiagSuggestion {
    const char *text;            /* Suggested fix text */
    const char *replacement;     /* Optional: text to replace with */
    const char *doc_url;        /* Optional: documentation URL */
} DiagSuggestion;

/* Snippet rendering options */
typedef struct SnippetOpts {
    bool show_line_numbers;
    uint32_t context_lines;    /* lines of context before/after */
    UnderlineStyle primary_style;
    UnderlineStyle secondary_style;
} SnippetOpts;

/* Default snippet options */
#define SNIPPET_OPTS_DEFAULT ((SnippetOpts){.show_line_numbers = true, .context_lines = DIAG_CONTEXT_LINES, .primary_style = UNDERLINE_PRIMARY, .secondary_style = UNDERLINE_SECONDARY})

/* Initialize diagnostics - call once at program start */
void diag_init(bool use_color);

/* Check if colors are enabled */
bool diag_use_color(void);

/* Check if stderr is a TTY (for auto-color) */
bool stderr_is_tty(void);

void diag_register_file(const SourceFile *file);

/* Core diagnostic emission */
void diag_emit(DiagLevel level, Span span, const char *fmt, ...);
void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap);

/* Enhanced diagnostics with code and notes (Phase 8) */
void diag_emit_with_code(DiagLevel level, Span span, DiagCode code, const char *fmt, ...);
void diag_emit_with_notes(DiagLevel level, Span span, const char *message,
                          DiagNote *notes, size_t note_count);
void diag_emit_with_suggestion(DiagLevel level, Span span, const char *message,
                               const DiagSuggestion *suggestion);

/* Multi-span diagnostics for complex errors */
void diag_emit_multi_span(DiagLevel level, const char *message,
                         Span primary_span, const char *primary_label,
                         Span *secondary_spans, const char **secondary_labels,
                         size_t secondary_count);

bool diag_had_error(void);
void diag_reset(void);

/* Snippet rendering */
void diag_render_snippet(const SourceFile *f, Span span, const SnippetOpts *opts);

/* Get error code string for display */
const char *diag_code_to_string(DiagCode code);

/* JSON diagnostics support (Phase 8) */
typedef struct JsonDiag {
    const char *severity;   /* "error", "warning", "note", "help" */
    const char *code;       /* error code like "TUR-E0001" */
    const char *message;
    const char *file;
    uint32_t line;
    uint32_t col;
    uint32_t end_line;
    uint32_t end_col;
} JsonDiag;

/* Enable/disable JSON output mode */
void diag_set_json_output(bool enabled);

/* Emit a diagnostic in JSON format */
void diag_emit_json(DiagLevel level, Span span, DiagCode code, const char *message);

#endif
