#include "reader.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef struct Reader {
    const SourceFile *file;
    Arena            *arena;
    SymbolTable      *st;
    const char       *src;
    size_t            len;
    size_t            pos;
    uint32_t          line;
    uint32_t          col;
    bool              error;
} Reader;

static Span span_from_to(const Reader *r,
                         uint32_t start_line, uint32_t start_col,
                         size_t   start_off,  size_t end_off) {
    Span s;
    s.file_id = r->file->file_id;
    s.line = start_line;
    s.col_start = start_col;
    s.col_end = start_col + (uint32_t)(end_off - start_off);
    s.off_start = (uint32_t)start_off;
    s.off_end = (uint32_t)end_off;
    return s;
}

static Span span_point(const Reader *r) {
    Span s;
    s.file_id = r->file->file_id;
    s.line = r->line;
    s.col_start = r->col;
    s.col_end = r->col + 1;
    s.off_start = (uint32_t)r->pos;
    s.off_end = (uint32_t)r->pos + 1;
    return s;
}

static int peek(const Reader *r) {
    if (r->pos >= r->len) return -1;
    return (unsigned char)r->src[r->pos];
}

static int peek2(const Reader *r) {
    if (r->pos + 1 >= r->len) return -1;
    return (unsigned char)r->src[r->pos + 1];
}

static int peek3(const Reader *r) {
    if (r->pos + 2 >= r->len) return -1;
    return (unsigned char)r->src[r->pos + 2];
}

static int advance(Reader *r) {
    if (r->pos >= r->len) return -1;
    int c = (unsigned char)r->src[r->pos++];
    if (c == '\n') {
        r->line++;
        r->col = 1;
    } else {
        r->col++;
    }
    return c;
}

static void skip_ws_and_comments(Reader *r) {
    for (;;) {
        int c = peek(r);
        if (c == -1) return;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            advance(r);
        } else if (c == ';') {
            while ((c = peek(r)) != -1 && c != '\n') advance(r);
        } else {
            return;
        }
    }
}

static bool is_sym_start(int c) {
    if (c == -1) return false;
    if (isalpha(c)) return true;
    switch (c) {
        case '+': case '-': case '*': case '/':
        case '=': case '<': case '>':
        case '!': case '?':
        case '_': case '$': case '&':
        case '.':            /* Phase 15: enables .method for typeclass method calls */
        case '^':            /* enables ^mut, ^int as sym-shaped metadata */
        case 39:             /* single quote ' - enables lifetime annotations like 'a */
            return true;
    }
    return false;
}

static bool is_sym_cont(int c) {
    if (c == -1) return false;
    if (isalnum(c)) return true;
    switch (c) {
        case '+': case '-': case '*': case '/':
        case '=': case '<': case '>':
        case '!': case '?':
        case '_': case '$': case '&':
        case '.': case '#': case '^':
        case 39:             /* single quote ' - allows ' in lifetime symbols like 'a */
            return true;
    }
    return false;
}

static Form *read_form(Reader *r);

static Form *read_string(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume opening " */

    /* Build into a temporary buffer; copy into arena at the end. */
    /* Two-pass: first scan to find end and unescaped length. */
    size_t scan = r->pos;
    size_t out_len = 0;
    bool   ok = false;
    while (scan < r->len) {
        char c = r->src[scan];
        if (c == '"') { ok = true; break; }
        if (c == '\\') {
            if (scan + 1 >= r->len) break;
            scan += 2;
            out_len++;
            continue;
        }
        scan++;
        out_len++;
    }
    if (!ok) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "unterminated string literal");
        r->error = true;
        return NULL;
    }

    char *buf = (char *)arena_alloc_aligned(r->arena, out_len + 1, 1);
    size_t bi = 0;
    while (peek(r) != -1 && peek(r) != '"') {
        int c = advance(r);
        if (c == '\\') {
            int e = advance(r);
            char out;
            switch (e) {
                case 'n':  out = '\n'; break;
                case 't':  out = '\t'; break;
                case 'r':  out = '\r'; break;
                case '0':  out = '\0'; break;
                case '\\': out = '\\'; break;
                case '"':  out = '"';  break;
                default:
                    diag_emit(DIAG_ERROR, span_point(r), "unknown string escape '\\%c'", e);
                    r->error = true;
                    out = (char)e;
            }
            buf[bi++] = out;
        } else {
            buf[bi++] = (char)c;
        }
    }
    advance(r); /* closing " */
    buf[bi] = '\0';

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *f = form_new(r->arena, F_STR, span);
    f->as.s.p = buf;
    f->as.s.len = (uint32_t)bi;
    return f;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static Form *read_number(Reader *r, int sign) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    if (sign != 0) {
        /* sign already at peek; consume it for the lexeme range */
        advance(r);
    }

    int64_t val = 0;
    bool any = false;

    /* 0x / 0b prefixes */
    if (peek(r) == '0' && (peek2(r) == 'x' || peek2(r) == 'X')) {
        advance(r); advance(r);
        int d;
        while ((d = hex_digit(peek(r))) >= 0) {
            val = val * 16 + d;
            advance(r);
            any = true;
        }
    } else if (peek(r) == '0' && (peek2(r) == 'b' || peek2(r) == 'B')) {
        advance(r); advance(r);
        while (peek(r) == '0' || peek(r) == '1') {
            val = val * 2 + (advance(r) - '0');
            any = true;
        }
    } else {
        while (peek(r) >= '0' && peek(r) <= '9') {
            val = val * 10 + (int64_t)(advance(r) - '0');
            any = true;
        }
    }

    if (!any) {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected digits in numeric literal");
        r->error = true;
        return NULL;
    }
    if (sign < 0) val = -val;
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    return form_int(r->arena, span, val);
}

static Form *read_keyword(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume ':' */
    if (!is_sym_cont(peek(r)) && !isalpha(peek(r))) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "expected keyword name after ':'");
        r->error = true;
        return NULL;
    }
    while (is_sym_cont(peek(r))) advance(r);
    size_t end = r->pos;
    Span span = span_from_to(r, start_line, start_col, start_off, end);
    /* keyword name is the slice after the colon */
    StrSlice name = strslice(r->src + start_off + 1, (uint32_t)(end - start_off - 1));
    const Symbol *sym = symtab_intern(r->st, name);
    return form_keyword(r->arena, span, sym);
}

static Form *read_quote(Reader *r) {
    /* Phase 6: ('x) reader macro - read as (quote x) */
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '\'' */
    skip_ws_and_comments(r);
    
    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "' requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create a quote form */
    return form_quote(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
}

/* Phase 6: Read unquote (~) or unquote-splicing (~@) - only valid inside quasiquote */
static Form *read_unquote(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '~' */
    
    /* Check for ~@ (unquote-splicing) */
    if (peek(r) == '@') {
        advance(r); /* consume '@' */
        skip_ws_and_comments(r);
        
        if (peek(r) == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "~@ requires an expression after it");
            r->error = true;
            return NULL;
        }
        
        Form *inner = read_form(r);
        if (!inner) return NULL;
        
        /* Create an unquote-splicing form */
        return form_unquote_splicing(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
    }
    
    skip_ws_and_comments(r);
    
    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "~ requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create an unquote form */
    return form_unquote(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
}

static Form *read_quasiquote(Reader *r) {
    /* Phase 6: (`x) reader macro - read as (quasiquote x) */
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '`' */
    skip_ws_and_comments(r);
    
    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "` requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create a quasiquote form */
    return form_quasiquote(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
}

static Form *read_deref(struct Reader *r) {
    /* Phase 5: (@ expr) reader macro - read as (deref expr) */
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '@' */
    skip_ws_and_comments(r);
    
    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "@ requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create a list: (deref inner) */
    Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
    const Symbol *deref_sym = symtab_intern(r->st, strslice("deref", 5));
    items[0] = form_sym(r->arena, span_from_to(r, start_line, start_col, start_off, start_off + 1), deref_sym);
    items[1] = inner;
    
    Span span = span_from_to(r, start_line, start_col, start_off, inner->span.off_end);
    return form_list(r->arena, span, items, 2);
}

static Form *read_symbol_or_minus(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    /* Special case: '-' followed by digit is a negative integer. */
    if (peek(r) == '-' && peek2(r) >= '0' && peek2(r) <= '9') {
        return read_number(r, -1);
    }

    while (is_sym_cont(peek(r))) advance(r);
    size_t end = r->pos;
    if (end == start_off) {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "unexpected character '%c'", (char)peek(r));
        r->error = true;
        advance(r);
        return NULL;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, end);
    StrSlice name = strslice(r->src + start_off, (uint32_t)(end - start_off));

    /* Recognize literal keywords. */
    if (name.len == 3 && memcmp(name.p, "nil", 3) == 0)
        return form_nil(r->arena, span);
    if (name.len == 4 && memcmp(name.p, "true", 4) == 0)
        return form_bool(r->arena, span, true);
    if (name.len == 5 && memcmp(name.p, "false", 5) == 0)
        return form_bool(r->arena, span, false);

    const Symbol *sym = symtab_intern(r->st, name);
    return form_sym(r->arena, span, sym);
}

static Form *read_seq(Reader *r, char open, char close, FormTag tag,
                      const char *unterminated_msg) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume opener */

    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "%s", unterminated_msg);
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == close) {
            advance(r);
            break;
        }
        /* Defensive: a stray opposite-bracket inside should be a clear error,
         * not silently consumed by our caller. */
        if ((open == '(' && c == ']') || (open == '[' && c == ')')) {
            Span s = span_point(r);
            diag_emit(DIAG_ERROR, s, "mismatched closer '%c' inside %c..%c",
                      (char)c, open, close);
            r->error = true;
            free(items);
            return NULL;
        }
        Form *child = read_form(r);
        if (!child) {
            free(items);
            return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = (Form **)realloc(items, cap * sizeof(Form *));
            if (!items) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        items[n++] = child;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *seq;
    if (tag == F_LIST) seq = form_list(r->arena, span, items, (uint32_t)n);
    else               seq = form_vec (r->arena, span, items, (uint32_t)n);
    free(items);
    return seq;
}

/* Read a C code block: ```c ... ``` or ``` c ... ``` */
static Form *read_cblock(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    
    /* Expect ``` */
    if (peek(r) != '`') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected '`' for C code block");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '`' */
    if (peek(r) != '`') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected '`' for C code block");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '`' */
    if (peek(r) != '`') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected '`' for C code block");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '`' */
    
    /* Now expect 'c' (possibly after whitespace) */
    skip_ws_and_comments(r);
    if (peek(r) != 'c') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected 'c' for C code block (use ```c ... ```)");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume 'c' */
    skip_ws_and_comments(r);
    
    /* Now read the C code until we find ``` */
    size_t code_start = r->pos;
    
    while (peek(r) != -1) {
        if (peek(r) == '`' && peek2(r) == '`' && peek3(r) == '`') {
            /* Found ``` */
            advance(r); advance(r); advance(r);
            break;
        }
        advance(r);
    }
    
    if (peek(r) == -1) {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "unterminated C code block (missing ```)");
        r->error = true;
        return NULL;
    }
    
    size_t code_end = r->pos - 3; /* don't include the ``` */
    StrSlice code = { r->src + code_start, (uint32_t)(code_end - code_start) };
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    return form_cblock(r->arena, span, code);
}

static Form *read_form(Reader *r) {
    skip_ws_and_comments(r);
    int c = peek(r);
    if (c == -1) return NULL;

    if (c == '(') return read_seq(r, '(', ')', F_LIST, "unterminated list (missing ')')");
    if (c == '[') return read_seq(r, '[', ']', F_VEC,  "unterminated vector (missing ']')");
    if (c == ')' || c == ']') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "unexpected '%c'", (char)c);
        r->error = true;
        advance(r);
        return NULL;
    }
    if (c == '"') return read_string(r);
    if (c == ':') return read_keyword(r);
    if (c == '`') {
        /* Phase 6: Check for triple backtick (C block) vs single backtick (quasiquote) */
        if (peek2(r) == '`' && peek3(r) == '`') {
            return read_cblock(r); /* C code block ``` */
        } else {
            /* Single backtick - quasiquote */
            return read_quasiquote(r);
        }
    }
    if (c >= '0' && c <= '9') return read_number(r, 0);
    /* Phase 5: @ as deref operator */
    if (c == '@') return read_deref(r);
    /* Phase 6: ' as quote operator */
    if (c == '\'') return read_quote(r);
    /* Phase 6: ~ as unquote operator (only valid inside quasiquote) */
    if (c == '~') return read_unquote(r);
    if (is_sym_start(c)) return read_symbol_or_minus(r);

    Span s = span_point(r);
    diag_emit(DIAG_ERROR, s, "unexpected character '%c' (0x%02x)", (char)c, c);
    r->error = true;
    advance(r);
    return NULL;
}

#include <stdio.h>

Form **read_all(Arena *arena, SymbolTable *st, const SourceFile *file,
                uint32_t *out_count) {
    Reader r;
    r.file = file;
    r.arena = arena;
    r.st = st;
    r.src = file->src;
    r.len = file->len;
    r.pos = 0;
    r.line = 1;
    r.col = 1;
    r.error = false;

    Form **forms = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(&r);
        if (peek(&r) == -1) break;
        Form *f = read_form(&r);
        if (!f) {
            free(forms);
            return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            forms = (Form **)realloc(forms, cap * sizeof(Form *));
            if (!forms) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        forms[n++] = f;
    }

    Form **out = (Form **)arena_alloc(arena, sizeof(Form *) * (n + 1));
    for (size_t i = 0; i < n; i++) out[i] = forms[i];
    out[n] = NULL;
    free(forms);

    *out_count = (uint32_t)n;
    return out;
}
