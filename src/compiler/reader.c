#include "reader.h"
#include "reader_macros.h"
#include "types.h"   /* Phase N: TypeKind constants for literal suffixes */

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
    /* Phase S1: Curly-infix support */
    bool              curly_infix_enabled;
    /* Phase S2: Neoteric support */
    bool              neoteric_enabled;
    /* RM0/RM1: User-defined #-dispatch macros. May be NULL (no user macros). */
    const ReaderMacroRegistry *user_macros;
} Reader;

/* Forward declaration; the RM1 implementation lives further down, after the
 * basic Reader helpers (peek, advance, span_from_to, ...) it depends on. */
static Form *try_read_user_macro(Reader *r);

/* Forward declarations for neoteric support */
static int peek_neoteric_bracket(const Reader *r);
static Form *read_neoteric_bracket(Reader *r, Form *atom, int bracket);
static Form *read_seq(Reader *r, char open, char close, FormTag tag,
                      const char *unterminated_msg);
/* INT-1: Reader conditional */
static Form *read_reader_cond(Reader *r);

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

/* Phase N: Peek at character n positions ahead (0-based from current pos). */
static int peek_at(const Reader *r, size_t n) {
    if (r->pos + n >= r->len) return -1;
    return (unsigned char)r->src[r->pos + n];
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

/* Skip a #| ... |# block comment (supports nesting). */
static bool skip_block_comment(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    /* Consume opening #| */
    advance(r);
    advance(r);

    int depth = 1;
    while (peek(r) != -1) {
        if (peek(r) == '#' && peek2(r) == '|') {
            advance(r);
            advance(r);
            depth++;
            continue;
        }
        if (peek(r) == '|' && peek2(r) == '#') {
            advance(r);
            advance(r);
            depth--;
            if (depth == 0) return true;
            continue;
        }
        advance(r);
    }

    diag_emit(DIAG_ERROR,
              span_from_to(r, start_line, start_col, start_off, r->pos),
              "unterminated block comment");
    r->error = true;
    return false;
}

static void skip_ws_and_comments(Reader *r) {
    for (;;) {
        if (r->error) return;
        int c = peek(r);
        if (c == -1) return;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            advance(r);
        } else if (c == ';') {
            while ((c = peek(r)) != -1 && c != '\n') advance(r);
        } else if (c == '#' && peek2(r) == '|') {
            if (!skip_block_comment(r)) return;
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
        case '|':            /* enables |>, |||, and similar pipe-shaped operators */
        case 39:             /* single quote ' - enables lifetime annotations like 'a */
        /* UTF-8 leading bytes for λ (U+03BB: 0xCE 0xBB),
           ∀ (U+2200: 0xE2 0x88 0x80), ∃ (U+2203: 0xE2 0x88 0x83) */
        case 0xCE: case 0xE2:
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
        case '|':            /* enables |>, |||, and similar pipe-shaped operators */
        case 39:             /* single quote ' - allows ' in lifetime symbols like 'a */
        /* UTF-8 leading bytes */
        case 0xCE: case 0xE2:
        /* UTF-8 continuation bytes for λ (0xBB), ∀ (0x88, 0x80), ∃ (0x88, 0x83) */
        case 0x80: case 0x83: case 0x88: case 0xBB:
            return true;
    }
    return false;
}

static Form *read_form(Reader *r);
static Form *read_attribute(Reader *r);

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
    
    /* Phase S2: Check for neoteric bracket immediately following string */
    if (r->neoteric_enabled) {
        int bracket = peek_neoteric_bracket(r);
        if (bracket != -1) {
            return read_neoteric_bracket(r, f, bracket);
        }
    }
    
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

    bool is_float = false;
    double fval = 0.0;
    int64_t ival = 0;
    bool any = false;

    /* 0x / 0b prefixes - these are always integers */
    if (peek(r) == '0' && (peek2(r) == 'x' || peek2(r) == 'X')) {
        advance(r); advance(r);
        int d;
        while ((d = hex_digit(peek(r))) >= 0) {
            ival = ival * 16 + d;
            advance(r);
            any = true;
        }
    } else if (peek(r) == '0' && (peek2(r) == 'b' || peek2(r) == 'B')) {
        advance(r); advance(r);
        while (peek(r) == '0' || peek(r) == '1') {
            ival = ival * 2 + (advance(r) - '0');
            any = true;
        }
    } else {
        /* Decimal number. The loops below only *delimit* the lexeme (advance
         * the cursor, accumulate ival for the integer/suffix paths, and set
         * is_float). The float value is recovered with strtod over the matched
         * characters -- a hand-rolled digit accumulator drifts from the
         * correctly-rounded double and previously dropped negative exponents
         * outright (e.g. 1e-10 parsed as 1.0). See
         * docs/reader-float-parsing-plan.md. */
        size_t num_start = r->pos;

        /* Parse integer part */
        while (peek(r) >= '0' && peek(r) <= '9') {
            int digit = advance(r) - '0';
            ival = ival * 10 + (int64_t)digit;
            any = true;
        }

        /* Check for fractional part */
        if (peek(r) == '.') {
            is_float = true;
            advance(r);  /* consume '.' */
            while (peek(r) >= '0' && peek(r) <= '9') {
                advance(r);
                any = true;
            }
        }

        /* Check for exponent part */
        if ((peek(r) == 'e' || peek(r) == 'E') && any) {
            is_float = true;
            advance(r);  /* consume 'e' or 'E' */
            if (peek(r) == '+' || peek(r) == '-') {
                advance(r);
            }
            bool exp_any = false;
            while (peek(r) >= '0' && peek(r) <= '9') {
                advance(r);
                exp_any = true;
            }
            if (!exp_any) {
                Span s = span_point(r);
                diag_emit(DIAG_ERROR, s, "expected exponent digits in float literal");
                r->error = true;
                return NULL;
            }
        }

        /* Recover the float value from the delimited lexeme [num_start, pos).
         * The type suffix (f32/f64) has not been consumed yet, so the slice is
         * pure decimal-float syntax and strtod stops exactly at its end. The
         * leading sign is applied below via the `sign < 0` negation. */
        if (is_float) {
            size_t num_len = r->pos - num_start;
            char stackbuf[64];
            char *nb = stackbuf;
            if (num_len >= sizeof(stackbuf)) {
                nb = (char *)arena_alloc(r->arena, num_len + 1);
            }
            memcpy(nb, r->src + num_start, num_len);
            nb[num_len] = '\0';
            fval = strtod(nb, NULL);
        }
    }

    if (!any) {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected digits in numeric literal");
        r->error = true;
        return NULL;
    }

    /* Phase N: Scan optional type suffix (i8, i16, i32, i64, u8, u16, u32, u64, f32, f64). */
    LiteralSuffix lit_suf = LIT_SUF_NONE;
    if (!is_float) {
        /* Integer suffixes */
        if      (peek(r) == 'i' && peek2(r) == '8'  && !is_sym_cont(peek_at(r, 2))) { advance(r); advance(r); lit_suf = LIT_SUF_I8; }
        else if (peek(r) == 'i' && peek2(r) == '1'  && peek3(r) == '6' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_I16; }
        else if (peek(r) == 'i' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_I32; }
        else if (peek(r) == 'i' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_I64; }
        else if (peek(r) == 'u' && peek2(r) == '8'  && !is_sym_cont(peek_at(r, 2))) { advance(r); advance(r); lit_suf = LIT_SUF_U8; }
        else if (peek(r) == 'u' && peek2(r) == '1'  && peek3(r) == '6' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_U16; }
        else if (peek(r) == 'u' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_U32; }
        else if (peek(r) == 'u' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_U64; }
        /* Float suffixes on integer-looking literals (e.g. 1f32) */
        else if (peek(r) == 'f' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F32; is_float = true; fval = (double)ival; }
        else if (peek(r) == 'f' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F64; is_float = true; fval = (double)ival; }
    } else {
        /* Float suffixes */
        if      (peek(r) == 'f' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F32; }
        else if (peek(r) == 'f' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F64; }
    }


    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *atom;

    if (is_float) {
        if (sign < 0) fval = -fval;
        atom = form_float(r->arena, span, fval);
    } else {
        if (sign < 0) ival = -ival;
        atom = form_int(r->arena, span, ival);
        /* Overflow checks for small integer types */
        if (lit_suf == LIT_SUF_I8 && (ival < -128 || ival > 127)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows int8 range (-128..127)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_I16 && (ival < -32768 || ival > 32767)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows int16 range (-32768..32767)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_I32 && (ival < -2147483648LL || ival > 2147483647LL)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows int32 range");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U8 && (ival < 0 || ival > 255)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint8 range (0..255)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U16 && (ival < 0 || ival > 65535)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint16 range (0..65535)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U32 && (ival < 0 || ival > 4294967295LL)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint32 range (0..4294967295)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U64 && ival < 0) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint64 range (must be non-negative)");
            r->error = true;
            return NULL;
        }
    }
    atom->lit_suffix = lit_suf;


    /* Phase S2: Check for neoteric bracket immediately following number */
    if (r->neoteric_enabled) {
        int bracket = peek_neoteric_bracket(r);
        if (bracket != -1) {
            return read_neoteric_bracket(r, atom, bracket);
        }
    }
    
    return atom;
}

static Form *read_keyword(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume first ':' */

    /* Phase HRT1: '::' is the type ascription operator — emit as a symbol named "::" */
    if (peek(r) == ':') {
        advance(r); /* consume second ':' */
        size_t end = r->pos;
        Span span = span_from_to(r, start_line, start_col, start_off, end);
        StrSlice name = strslice(r->src + start_off, (uint32_t)(end - start_off)); /* "::" */
        const Symbol *sym = symtab_intern(r->st, name);
        return form_sym(r->arena, span, sym);
    }

    /* `: type-expr` — space-separated or fused-paren compound type annotation */
    {
        int c2 = peek(r);
        if (c2 == ' ' || c2 == '\t' || c2 == '\n' || c2 == '\r'
            || c2 == '(' || c2 == '[' || c2 == -1) {
            skip_ws_and_comments(r);
            if (peek(r) == -1) {
                Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
                diag_emit(DIAG_ERROR, s, "expected type expression after ':'");
                r->error = true;
                return NULL;
            }
            Form *inner = read_form(r);
            if (!inner) return NULL;
            size_t end = r->pos;
            Span sp = span_from_to(r, start_line, start_col, start_off, end);
            return form_type_ann(r->arena, sp, inner);
        }
    }

    if (!is_sym_cont(peek(r)) && !isalpha(peek(r))) {
        /* Phase G1: bare ':' (not followed by an identifier) is a type-annotation
         * separator symbol used in defgadt constructor forms.  Emit it as F_SYM(":")
         * instead of an error so that `(CtorName field : return-type)` parses. */
        size_t end = r->pos;
        Span span = span_from_to(r, start_line, start_col, start_off, end);
        StrSlice name = strslice(r->src + start_off, (uint32_t)(end - start_off)); /* ":" */
        const Symbol *sym = symtab_intern(r->st, name);
        return form_sym(r->arena, span, sym);
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

static Form *read_at(struct Reader *r) {
    /* '@' prefix has two roles:
     *   1) Deref sugar: @x / @ x -> (deref x)
     *   2) Effect-row annotation sugar: @{...} / @ {...} -> F_MAP {...}
     */
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '@' */
    skip_ws_and_comments(r);

    /* Effect-row annotation sugar for signatures. */
    if (peek(r) == '{') {
        return read_seq(r, '{', '}', F_MAP, "unterminated effect row (missing '}')");
    }

    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "@ requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create a list: (deref inner). */
    Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
    const Symbol *deref_sym = symtab_intern(r->st, strslice("deref", 5));
    items[0] = form_sym(r->arena, span_from_to(r, start_line, start_col, start_off, start_off + 1), deref_sym);
    items[1] = inner;
    
    Span span = span_from_to(r, start_line, start_col, start_off, inner->span.off_end);
    return form_list(r->arena, span, items, 2);
}

/* Phase 12: & / &mut borrow prefix sugar.
 *
 * Rules:
 *   '&' followed by whitespace/EOF/closer → bare '&' symbol (preserves (& x) form)
 *   '&' followed by 'm','u','t' + delimiter → (&mut <next-form>) sugar
 *   '&' followed by anything else           → (& <next-form>) sugar
 *
 * This means:
 *   (& x)    still works: '& ' has space → bare symbol, list reads normally
 *   &x       → (& x)
 *   &mut x   → (&mut x)   [note: (&mut x) explicit form must be written as &mut x]
 */
static bool is_borrow_no_sugar(int c) {
    /* Characters after '&' that suppress sugar and return a bare '&' symbol */
    if (c == -1) return true;
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
            c == ')' || c == ']' || c == '}' || c == ';' || c == '"');
}

static bool is_token_delim(int c) {
    /* Characters that end a token — used to check what follows 'mut' */
    if (c == -1) return true;
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
            c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
            c == ';' || c == '"' || c == ':' || c == '@' || c == '`' || c == '~');
}

static Form *read_borrow(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '&' */

    /* No-sugar: '&' followed by whitespace/closer → return bare '&' symbol.
     * This preserves the (& x) explicit call form. */
    if (is_borrow_no_sugar(peek(r))) {
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        const Symbol *sym = symtab_intern(r->st, strslice("&", 1));
        return form_sym(r->arena, span, sym);
    }

    /* Check for &mut sugar: next chars 'm','u','t' + token delimiter */
    int c1 = peek(r), c2 = peek2(r), c3 = peek3(r);
    int c4 = (r->pos + 3 < r->len) ? (unsigned char)r->src[r->pos + 3] : -1;
    bool is_mut = (c1 == 'm' && c2 == 'u' && c3 == 't' && is_token_delim(c4));

    const char *op_str;
    uint32_t op_len;
    if (is_mut) {
        advance(r); advance(r); advance(r); /* consume 'm','u','t' */
        op_str = "&mut";
        op_len = 4;
    } else {
        op_str = "&";
        op_len = 1;
    }
    Span op_span = span_from_to(r, start_line, start_col, start_off, r->pos);

    /* Allow whitespace between operator and operand (e.g. &mut x) */
    skip_ws_and_comments(r);

    if (peek(r) == -1 || peek(r) == ')' || peek(r) == ']' || peek(r) == '}') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "%s requires an expression after it", op_str);
        r->error = true;
        return NULL;
    }

    Form *inner = read_form(r);
    if (!inner) return NULL;

    const Symbol *op_sym = symtab_intern(r->st, strslice(op_str, op_len));
    Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
    items[0] = form_sym(r->arena, op_span, op_sym);
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
    Form *atom = form_sym(r->arena, span, sym);
    
    /* Phase S2: Check for neoteric bracket immediately following atom */
    if (r->neoteric_enabled) {
        int bracket = peek_neoteric_bracket(r);
        if (bracket != -1) {
            return read_neoteric_bracket(r, atom, bracket);
        }
    }
    
    return atom;
}

/* INT-1: Read #?(:tur expr :turi expr) reader conditional. */
static Form *read_reader_cond(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '#' */
    advance(r); /* consume '?' */

    skip_ws_and_comments(r);
    if (peek(r) != '(') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "#? must be followed by '('");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '(' */

    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        if (r->error) { free(items); return NULL; }
        if (peek(r) == ')') { advance(r); break; }
        if (peek(r) == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "unterminated reader conditional (missing ')')");
            r->error = true;
            free(items);
            return NULL;
        }
        Form *child = read_form(r);
        if (!child) { free(items); return NULL; }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = (Form **)realloc(items, cap * sizeof(Form *));
        }
        items[n++] = child;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form **arena_items = (Form **)arena_alloc(r->arena, n * sizeof(Form *));
    for (size_t i = 0; i < n; i++) arena_items[i] = items[i];
    free(items);
    return form_reader_cond(r->arena, span, arena_items, (uint32_t)n);
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
    if (tag == F_LIST) {
        seq = form_list(r->arena, span, items, (uint32_t)n);
    } else if (tag == F_VEC) {
        seq = form_vec(r->arena, span, items, (uint32_t)n);
    } else if (tag == F_SET) {
        seq = form_set(r->arena, span, items, (uint32_t)n);
    } else {
        seq = form_map(r->arena, span, items, (uint32_t)n);
    }
    free(items);
    return seq;
}

static Form *read_map(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '#' */
    if (peek(r) != '{') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "expected '{' after '#' for map literal");
        r->error = true;
        return NULL;
    }

    return read_seq(r, '{', '}', F_MAP, "unterminated map (missing '}')");
}

static Form *read_set(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 's' */
    return read_seq(r, '(', ')', F_SET, "unterminated set (missing ')')");
}

/* RR: Is form an operator symbol (<, <=, >, >=, =)? */
static bool rr_is_op(const Form *f) {
    if (f->tag != F_SYM) return false;
    const char *n = f->as.sym->name;
    return (strcmp(n, "<") == 0 || strcmp(n, "<=") == 0 ||
            strcmp(n, ">") == 0 || strcmp(n, ">=") == 0 ||
            strcmp(n, "=") == 0);
}

/* RR: Is form a single-letter variable (a-z or A-Z, not an operator)? */
static bool rr_is_var(const Form *f) {
    if (f->tag != F_SYM) return false;
    const char *n = f->as.sym->name;
    return (n[1] == '\0' &&
            ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= 'A' && n[0] <= 'Z')));
}

/* RR0/RR1/RR2/RR4: Read a #r{...} range literal and desugar to a constructor call. */
static Form *read_range_literal(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '#' */
    advance(r); /* consume 'r' */

    if (peek(r) != '{') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "#r must be followed by '{' for range literal");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '{' */

    Form *toks[5];
    size_t ntoks = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "#r{...}: unterminated range literal (missing '}')");
            r->error = true;
            return NULL;
        }
        if (c == '}') {
            advance(r);
            break;
        }
        if (ntoks == 5) {
            Span s = span_point(r);
            diag_emit(DIAG_ERROR, s,
                      "#r{...} expects 'var op form' or 'form op var op form', "
                      "got more than 5 tokens");
            r->error = true;
            return NULL;
        }
        Form *child = read_form(r);
        if (!child) return NULL;
        toks[ntoks++] = child;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);

    if (ntoks == 0) {
        diag_emit(DIAG_ERROR, span, "#r{} requires a range expression");
        r->error = true;
        return NULL;
    }
    if (ntoks != 3 && ntoks != 5) {
        diag_emit(DIAG_ERROR, span,
                  "#r{...} expects 'var op form' or 'form op var op form', "
                  "got %zu tokens", ntoks);
        r->error = true;
        return NULL;
    }

/* Intern a C string as an F_SYM using the whole range span. */
#define RR_SYM(name_cstr) \
    form_sym(r->arena, span, \
             symtab_intern(r->st, strslice((name_cstr), \
                                           (uint32_t)strlen(name_cstr))))

/* Promote an F_INT bound to F_FLOAT when use_float is set. */
#define RR_PROMOTE(fp) \
    ((use_float && (fp)->tag == F_INT) \
        ? form_float(r->arena, (fp)->span, (double)(fp)->as.i) \
        : (fp))

    if (ntoks == 3) {
        /* One-sided: var op form  |  form op var */
        Form *a = toks[0], *op_f = toks[1], *b = toks[2];

        if (!rr_is_op(op_f)) {
            diag_emit(DIAG_ERROR, op_f->span,
                      "#r{...}: expected a comparison operator, got '%s'",
                      op_f->tag == F_SYM ? op_f->as.sym->name : "<expr>");
            r->error = true;
            return NULL;
        }
        const char *op = op_f->as.sym->name;

        bool a_var = rr_is_var(a);
        bool b_var = rr_is_var(b);

        if (!a_var && !b_var) {
            diag_emit(DIAG_ERROR, span,
                      "#r{...}: expected a single-letter variable on one side");
            r->error = true;
            return NULL;
        }

        /* If both look like vars, treat left as var, right as the bound form. */
        Form *bound = a_var ? b : a;
        bool var_first = a_var;

        /* Canonicalize to var-op-form: flip operator when form is on the left. */
        if (!var_first) {
            if      (strcmp(op, ">")  == 0) op = "<";
            else if (strcmp(op, ">=") == 0) op = "<=";
            else if (strcmp(op, "<")  == 0) op = ">";
            else if (strcmp(op, "<=") == 0) op = ">=";
            /* "=" is symmetric -- no flip needed */
        }

        bool use_float = (bound->tag == F_FLOAT);
        bound = RR_PROMOTE(bound);

        const char *ctor;
        if      (strcmp(op, "=")  == 0) ctor = use_float ? "float-singleton-range"    : "singleton-range";
        else if (strcmp(op, "<")  == 0) ctor = use_float ? "float-less-than-range"    : "less-than-range";
        else if (strcmp(op, "<=") == 0) ctor = use_float ? "float-at-most-range"      : "at-most-range";
        else if (strcmp(op, ">")  == 0) ctor = use_float ? "float-greater-than-range" : "greater-than-range";
        else                             ctor = use_float ? "float-at-least-range"     : "at-least-range";

        const Symbol *var_sym = (a_var ? a : b)->as.sym;
        Form **out = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
        out[0] = RR_SYM(ctor);
        out[1] = bound;
        Form *range = form_list(r->arena, span, out, 2);
        return form_range_var(r->arena, span, var_sym, range);
    }

    /* ntoks == 5: two-sided: left_f op1 var op2 right_f */
    Form *left_f  = toks[0];
    Form *op1_f   = toks[1];
    Form *var_f   = toks[2];
    Form *op2_f   = toks[3];
    Form *right_f = toks[4];

    if (!rr_is_op(op1_f)) {
        diag_emit(DIAG_ERROR, op1_f->span,
                  "#r{...}: expected a comparison operator, got '%s'",
                  op1_f->tag == F_SYM ? op1_f->as.sym->name : "<expr>");
        r->error = true;
        return NULL;
    }
    if (!rr_is_op(op2_f)) {
        diag_emit(DIAG_ERROR, op2_f->span,
                  "#r{...}: expected a comparison operator, got '%s'",
                  op2_f->tag == F_SYM ? op2_f->as.sym->name : "<expr>");
        r->error = true;
        return NULL;
    }
    if (!rr_is_var(var_f)) {
        diag_emit(DIAG_ERROR, var_f->span,
                  "#r{...}: expected a single-letter variable in the middle position");
        r->error = true;
        return NULL;
    }

    const char *op1 = op1_f->as.sym->name;
    const char *op2 = op2_f->as.sym->name;

    if (strcmp(op1, "=") == 0 || strcmp(op2, "=") == 0) {
        diag_emit(DIAG_ERROR, span, "#r{...}: '=' is only valid in a one-sided range");
        r->error = true;
        return NULL;
    }

    bool op1_fwd = (strcmp(op1, "<") == 0 || strcmp(op1, "<=") == 0);
    bool op2_fwd = (strcmp(op2, "<") == 0 || strcmp(op2, "<=") == 0);

    if (op1_fwd != op2_fwd) {
        diag_emit(DIAG_ERROR, span,
                  "#r{...}: cannot mix '<'/'<=' and '>'/>=' in a two-sided range");
        r->error = true;
        return NULL;
    }

    bool use_float = (left_f->tag == F_FLOAT || right_f->tag == F_FLOAT);

    /* Note when one literal bound is float but the other is a runtime expression. */
    if (use_float && (left_f->tag != F_INT && left_f->tag != F_FLOAT)) {
        diag_emit(DIAG_NOTE, span,
                  "#r{...}: mixed literal types; using float constructors -- "
                  "ensure the expression produces a float");
    } else if (use_float && (right_f->tag != F_INT && right_f->tag != F_FLOAT)) {
        diag_emit(DIAG_NOTE, span,
                  "#r{...}: mixed literal types; using float constructors -- "
                  "ensure the expression produces a float");
    }

    Form *ctor_lo, *ctor_hi;
    bool lo_incl, hi_incl;

    if (op1_fwd) {
        /* Left-to-right: left_f op1 var op2 right_f  (e.g. 0 <= n < 10) */
        ctor_lo = left_f;
        ctor_hi = right_f;
        lo_incl = (strcmp(op1, "<=") == 0);
        hi_incl = (strcmp(op2, "<=") == 0);
    } else {
        /* Right-to-left: left_f op1 var op2 right_f  (e.g. 10 > n >= 0)
         * left_f is the hi bound in the source; right_f is the lo bound.
         * Constructor always receives (ctor lo hi). */
        ctor_lo = right_f;
        ctor_hi = left_f;
        hi_incl = (strcmp(op1, ">=") == 0); /* op1: hi op1 var */
        lo_incl = (strcmp(op2, ">=") == 0); /* op2: var op2 lo */
    }

    /* Compile-time empty-range warning for integer literal bounds (RR1). */
    if (ctor_lo->tag == F_INT && ctor_hi->tag == F_INT) {
        int64_t lo_v = ctor_lo->as.i;
        int64_t hi_v = ctor_hi->as.i;
        bool empty = (lo_incl && hi_incl) ? (lo_v > hi_v) : (lo_v >= hi_v);
        if (empty) {
            diag_emit(DIAG_WARNING, span, "#r{...}: range is provably empty");
        }
    }

    ctor_lo = RR_PROMOTE(ctor_lo);
    ctor_hi = RR_PROMOTE(ctor_hi);

    const char *ctor;
    if      (lo_incl && hi_incl)   ctor = use_float ? "float-closed-range"      : "closed-range";
    else if (!lo_incl && !hi_incl)  ctor = use_float ? "float-open-range"        : "open-range";
    else if (lo_incl && !hi_incl)   ctor = use_float ? "float-closed-open-range" : "closed-open-range";
    else                             ctor = use_float ? "float-open-closed-range" : "open-closed-range";

    Form **out = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    out[0] = RR_SYM(ctor);
    out[1] = ctor_lo;
    out[2] = ctor_hi;
    Form *range = form_list(r->arena, span, out, 3);
    return form_range_var(r->arena, span, var_f->as.sym, range);

#undef RR_SYM
#undef RR_PROMOTE
}

/* Phase R5: Read an attribute form: #[...] */
static Form *read_attribute(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '#' */
    if (peek(r) != '[') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "expected '[' after '#' for attribute");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '[' */

    /* Read the attribute name */
    skip_ws_and_comments(r);
    Form *attr_name = read_form(r);
    if (!attr_name) {
        r->error = true;
        return NULL;
    }

    /* Check for closing ] */
    skip_ws_and_comments(r);
    if (peek(r) != ']') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "expected ']' to close attribute");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume ']' */

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    
    if (attr_name->tag != F_SYM) {
        diag_emit(DIAG_ERROR, attr_name->span, "attribute name must be a symbol");
        r->error = true;
        return NULL;
    }
    
    /* Create a symbol with "#" prefix: #no-unwind */
    char attr_name_buf[64];
    int written = snprintf(attr_name_buf, sizeof(attr_name_buf), "#%s", attr_name->as.sym->name);
    if (written >= (int)sizeof(attr_name_buf)) {
        diag_emit(DIAG_ERROR, span, "attribute name too long");
        r->error = true;
        return NULL;
    }
    StrSlice attr_slice = strslice(attr_name_buf, (uint32_t)written);
    const Symbol *attr_sym = symtab_intern(r->st, attr_slice);
    return form_sym(r->arena, span, attr_sym);
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

/* Phase S2: Helper to check if neoteric bracket follows */
/* Returns the bracket char if found (with no whitespace), or -1 if not */
static int peek_neoteric_bracket(const Reader *r) {
    int c = peek(r);
    if (c == '(' || c == '{' || c == '[') {
        return c;
    }
    return -1;
}

/* Phase S2: Read neoteric bracketed expression after an atom */
/* f(expr) -> (f expr), f{expr} -> (f expr), f[expr] -> (bracketapply f expr) */
static Form *read_neoteric_bracket(Reader *r, Form *atom, int bracket) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    
    /* Consume the opening bracket */
    advance(r);
    
    /* Read the content inside the bracket */
    Form **items = NULL;
    size_t cap = 0, n = 0;
    char close_bracket;
    
    switch (bracket) {
        case '(': close_bracket = ')'; break;
        case '{': close_bracket = '}'; break;
        case '[': close_bracket = ']'; break;
        default: return atom; /* Shouldn't happen */
    }
    
    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            const char *bracket_name = (bracket == '(') ? "parentheses" : 
                                       (bracket == '{') ? "braces" : "brackets";
            diag_emit(DIAG_ERROR, s, "unterminated neoteric %s (missing '%c')", 
                      bracket_name, close_bracket);
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == close_bracket) {
            advance(r);
            break;
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
    
    /* Create the final call form */
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    
    /* For neoteric: f(x y z) -> (f x y z), not (f (x y z)) */
    /* We need to spread the items as separate arguments */
    
    Form **call_items = (Form **)arena_alloc(r->arena, (n + 1) * sizeof(Form *));
    call_items[0] = atom;
    for (uint32_t i = 0; i < n; i++) {
        call_items[i + 1] = items[i];
    }
    free(items);
    
    if (bracket == '[' && n == 1) {
        /* Special case: f[x] -> (bracketapply f x) */
        const Symbol *bracketapply_sym = symtab_intern(r->st, strslice("bracketapply", 12));
        Form **ba_items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
        ba_items[0] = form_sym(r->arena, span, bracketapply_sym);
        ba_items[1] = atom;
        ba_items[2] = call_items[1]; /* the single argument */
        free(call_items);
        return form_list(r->arena, span, ba_items, 3);
    }
    
    return form_list(r->arena, span, call_items, n + 1);
}

/* Phase S1: Read curly-infix expression {a + b} -> (+ a b) */
static Form *read_curly_infix(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '{' */

    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "unterminated curly-infix expression (missing '}')");
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == '}') {
            advance(r);
            break;
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

    /* Handle special cases per SRFI-105 */
    if (n == 0) {
        /* Empty { } */
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        return form_list(r->arena, span, NULL, 0);
    } else if (n == 1) {
        /* {e} -> e */
        Form *only = items[0];
        free(items);
        return only;
    } else if (n == 2) {
        /* {e1 e2} -> (e1 e2) - function call */
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        Form *result = form_list(r->arena, span, items, (uint32_t)n);
        free(items);
        return result;
    }

    /* Check if all operators are the same (homogeneous infix) */
    /* Per SRFI-105: {a + b + c} -> (+ a b c) if all operators are + */
    /* Mixed operators: {a + b * c} -> ($nfx$ a + b * c) */
    bool all_same_op = true;
    const Symbol *first_op = NULL;
    
    for (uint32_t i = 0; i < n; i++) {
        if (items[i]->tag == F_SYM) {
            if (first_op == NULL) {
                first_op = items[i]->as.sym;
            } else if (items[i]->as.sym != first_op) {
                all_same_op = false;
                break;
            }
        }
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    
    if (all_same_op && first_op != NULL) {
        /* Homogeneous: all operators are the same */
        /* Rearrange: {a + b + c} where + is the operator
         * We need to identify which items are operators vs operands
         * In SRFI-105, operators alternate with operands: a + b + c
         * So operators are at odd indices (1, 3, ...) if we start with operand
         * But we need to handle both cases */
        
        /* Collect all operator positions */
        Form **operands = (Form **)arena_alloc(r->arena, n * sizeof(Form *));
        uint32_t op_count = 0;
        
        for (uint32_t i = 0; i < n; i++) {
            if (items[i]->tag == F_SYM && items[i]->as.sym == first_op) {
                /* This is the operator - skip it, it's the same for all */
            } else {
                operands[op_count++] = items[i];
            }
        }
        
        /* Create the call: (op arg1 arg2 ...) */
        Form **call_items = (Form **)arena_alloc(r->arena, (op_count + 1) * sizeof(Form *));
        call_items[0] = form_sym(r->arena, span, first_op);
        for (uint32_t i = 0; i < op_count; i++) {
            call_items[i + 1] = operands[i];
        }
        
        free(items);
        return form_list(r->arena, span, call_items, op_count + 1);
    } else {
        /* Mixed operators or no operators - use $nfx$ macro */
        /* Create ($nfx$ a + b * c) */
        const Symbol *nfx_sym = symtab_intern(r->st, strslice("$nfx$", 5));
        Form **nfx_items = (Form **)arena_alloc(r->arena, (n + 1) * sizeof(Form *));
        nfx_items[0] = form_sym(r->arena, span, nfx_sym);
        for (uint32_t i = 0; i < n; i++) {
            nfx_items[i + 1] = items[i];
        }
        free(items);
        return form_list(r->arena, span, nfx_items, n + 1);
    }
}

/* CT0: Read a contract type annotation: { var : T | pred }
 * Produces an F_CONTRACT_TYPE form with items:
 *   [0] = F_SYM(var)
 *   [1] = F_TYPE_ANN(T)  (or F_KEYWORD(:T))
 *   [2] = F_SYM("|")
 *   [3] = predicate form (any expression)
 * The outer braces are consumed here. */
static Form *read_contract_type(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '{' */

    /* Collect items until '}' */
    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "unterminated contract type (missing '}')");
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == '}') {
            advance(r);
            break;
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
    Form *f = form_contract_type(r->arena, span, items, (uint32_t)n);
    free(items);
    return f;
}

/* ------------------------------------------------------------------------
 * RM1: user-defined #-dispatch reader macros (template-only, raw bodies).
 * Placed here, below the basic Reader helpers (peek/advance/span_*), and
 * above read_form, which calls try_read_user_macro on the '#' branch.
 * ------------------------------------------------------------------------ */

/* Identifier char class for macro names. Per the plan's open question #2,
 * we delegate to the symbol reader so names like `#if+`, `#?some`,
 * `#<-bind` work, with one carve-out: `#` is *not* a continuation byte for
 * macro names — otherwise `#foo#bar` would parse as a single macro name
 * `foo#bar` instead of two adjacent `#`-dispatches. */
static bool is_macro_id_start(int c) {
    return is_sym_start(c);
}
static bool is_macro_id_cont(int c) {
    if (c == '#') return false;
    return is_sym_cont(c);
}

static int matching_close_for(int open) {
    switch (open) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        default:  return 0;
    }
}

/* Read a raw (verbatim) macro body. The reader is positioned at the
 * open-delim; on success leaves it past the matched close-delim. Tracks
 * nesting and recognises `\X` escapes (the backslash is dropped, the next
 * byte is kept literally — including the close-delim, which does not
 * affect depth). Returns the body bytes (arena-allocated, NUL-terminated)
 * and sets *out_len. On unterminated input, emits a diagnostic and returns
 * NULL. */
static char *read_raw_body(Reader *r, int open, int close, uint32_t *out_len) {
    uint32_t open_line = r->line;
    uint32_t open_col  = r->col;
    size_t   open_off  = r->pos;
    advance(r); /* consume open delim */

    /* Two-pass: find the matched close + the unescaped length, then copy. */
    size_t scan      = r->pos;
    size_t scan_end  = 0;
    size_t out_n     = 0;
    int    depth     = 1;
    bool   ok        = false;
    while (scan < r->len) {
        char c = r->src[scan];
        if (c == '\\' && scan + 1 < r->len) {
            scan += 2;
            out_n += 1;
            continue;
        }
        if (c == open && open != close) {
            depth++; scan++; out_n++; continue;
        }
        if (c == close) {
            depth--;
            if (depth == 0) { ok = true; scan_end = scan; break; }
            scan++; out_n++; continue;
        }
        scan++; out_n++;
    }
    if (!ok) {
        diag_emit(DIAG_ERROR,
                  span_from_to(r, open_line, open_col, open_off, r->pos),
                  "unterminated reader macro body (missing '%c')",
                  (char)close);
        r->error = true;
        return NULL;
    }

    char *buf = (char *)arena_alloc_aligned(r->arena, out_n + 1, 1);
    size_t bi = 0;
    while (r->pos < scan_end) {
        int c = peek(r);
        if (c == '\\' && r->pos + 1 < r->len) {
            advance(r); /* drop the backslash */
            buf[bi++] = (char)advance(r);
            continue;
        }
        buf[bi++] = (char)advance(r);
    }
    buf[bi] = '\0';
    advance(r); /* consume close delim */
    *out_len = (uint32_t)bi;
    return buf;
}

/* Deep-clone a form tree, overriding every node's span with `span`. Used
 * by the macro expanders so that the expanded tree carries the call-site
 * span (e.g. the `#foo{...}` location), not the registration-site span
 * the template was originally parsed with. The payload (literal value,
 * symbol pointer, raw bytes, ...) is shared with the source form — only
 * the `Form` envelope and any child `Form *` slots are freshly allocated.
 */
static Form *form_clone_with_span(Arena *a, const Form *f, Span span) {
    if (!f) return NULL;
    Form *out = form_new(a, f->tag, span);
    out->lit_suffix = f->lit_suffix;
    memcpy(&out->as, &f->as, sizeof(f->as));
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_MAP: case F_SET:
        case F_QUOTE: case F_QUASIQUOTE:
        case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_TYPE_ANN: case F_CONTRACT_TYPE:
        case F_READER_COND: case F_RANGE_VAR: {
            uint32_t n = f->as.list.len;
            Form **items = (Form **)arena_alloc(
                a, sizeof(Form *) * (n ? n : 1));
            for (uint32_t i = 0; i < n; ++i) {
                items[i] = form_clone_with_span(a, f->as.list.items[i], span);
            }
            out->as.list.items = items;
            out->as.list.len = n;
            break;
        }
        default:
            /* Scalar payload was copied by the memcpy above. */
            break;
    }
    return out;
}

/* RM2: parse a template splice marker `$N` (N >= 1). Returns N, or 0 if
 * `f` is not such a marker. Leading zeros are rejected so the syntax
 * stays unambiguous. */
static int try_dollar_index(const Form *f) {
    if (!f || f->tag != F_SYM || !f->as.sym) return 0;
    const Symbol *s = f->as.sym;
    if (s->len < 2 || s->name[0] != '$') return 0;
    if (s->name[1] < '1' || s->name[1] > '9') return 0;
    int n = 0;
    for (uint32_t i = 1; i < s->len; ++i) {
        char c = s->name[i];
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
        if (n > 9999) return 0;
    }
    return n;
}

static bool is_dollar_body_form(const Form *f) {
    return f && f->tag == F_SYM && f->as.sym
        && f->as.sym->len == 5
        && memcmp(f->as.sym->name, "$body", 5) == 0;
}

/* Walk a template form tree, replacing the symbol `$body` with a string
 * literal of the body. Sub-trees are always rebuilt so that every node in
 * the result carries the call-site span (so diagnostics on the expansion
 * point at the `#foo{...}` use site, not the `(reader-macros/define ...)`
 * registration site). Only F_LIST and F_VEC are recursed into for
 * substitution — quoted / quasiquoted sub-trees are span-rewritten but
 * their symbols are left intact, matching the "templates are dumb"
 * stance in the plan. */
static Form *expand_raw_template(Reader *r, const Form *t,
                                 const char *body, uint32_t body_len,
                                 Span call_site) {
    if (!t) return NULL;
    if (t->tag == F_SYM && t->as.sym
        && t->as.sym->len == 5
        && memcmp(t->as.sym->name, "$body", 5) == 0) {
        return form_str(r->arena, call_site, body, body_len);
    }
    if (t->tag == F_LIST || t->tag == F_VEC) {
        uint32_t n = t->as.list.len;
        Form **items = (Form **)arena_alloc(
            r->arena, sizeof(Form *) * (n ? n : 1));
        for (uint32_t i = 0; i < n; ++i) {
            items[i] = expand_raw_template(r, t->as.list.items[i],
                                           body, body_len, call_site);
        }
        return (t->tag == F_LIST)
            ? form_list(r->arena, call_site, items, n)
            : form_vec(r->arena, call_site, items, n);
    }
    /* Anything else — including quoted sub-trees, keywords, literals —
     * gets a structural clone with the call-site span everywhere. */
    return form_clone_with_span(r->arena, t, call_site);
}

/* RM2: walk a template tree replacing splice markers with parsed body
 * forms. Two replacement rules apply inside any F_LIST/F_VEC child slot:
 *
 *   - the symbol `$body` is replaced by *all* body forms, spliced into
 *     the surrounding sequence (so `(foo $body)` with body `[a b c]`
 *     becomes `(foo a b c)`);
 *   - the symbol `$N` (N >= 1) is replaced in-place by the N-th body
 *     form (1-indexed). Referring past the body is a diagnostic.
 *
 * At the top level (template position itself), `$body` produces a
 * synthesized F_LIST of the items and `$N` produces a single item.
 *
 * Template-derived nodes are rebuilt with the call-site span; body forms
 * retain their original spans (they came from user code at the call site
 * so their existing parse-time spans already point at the right place). */
static Form *expand_datum_template(Reader *r, const Form *t,
                                   Form **items, uint32_t body_n,
                                   Span call_site) {
    if (!t) return NULL;
    if (is_dollar_body_form(t)) {
        return form_list(r->arena, call_site, items, body_n);
    }
    int idx;
    if ((idx = try_dollar_index(t)) > 0) {
        if ((uint32_t)idx > body_n) {
            diag_emit(DIAG_ERROR, t->span,
                      "reader macro template references $%d but body has "
                      "only %u form%s",
                      idx, (unsigned)body_n, body_n == 1 ? "" : "s");
            r->error = true;
            return NULL;
        }
        return items[idx - 1];  /* keep the body form's own span */
    }
    if (t->tag == F_LIST || t->tag == F_VEC) {
        uint32_t in_n = t->as.list.len;
        /* Worst case: every child is `$body` and there are body_n items. */
        uint32_t cap  = in_n + body_n + 1;
        Form  **out   = (Form **)arena_alloc(r->arena, sizeof(Form *) * cap);
        uint32_t out_n = 0;
        for (uint32_t i = 0; i < in_n; ++i) {
            const Form *child = t->as.list.items[i];
            if (is_dollar_body_form(child)) {
                if (out_n + body_n > cap) {
                    cap = (out_n + body_n) * 2;
                    Form **bigger = (Form **)arena_alloc(
                        r->arena, sizeof(Form *) * cap);
                    memcpy(bigger, out, sizeof(Form *) * out_n);
                    out = bigger;
                }
                for (uint32_t j = 0; j < body_n; ++j) out[out_n++] = items[j];
                continue;
            }
            int cidx = try_dollar_index(child);
            if (cidx > 0) {
                if ((uint32_t)cidx > body_n) {
                    diag_emit(DIAG_ERROR, child->span,
                              "reader macro template references $%d but "
                              "body has only %u form%s",
                              cidx, (unsigned)body_n,
                              body_n == 1 ? "" : "s");
                    r->error = true;
                    return NULL;
                }
                out[out_n++] = items[cidx - 1];
                continue;
            }
            Form *expanded =
                expand_datum_template(r, child, items, body_n, call_site);
            if (r->error) return NULL;
            out[out_n++] = expanded;
        }
        return (t->tag == F_LIST)
            ? form_list(r->arena, call_site, out, out_n)
            : form_vec(r->arena, call_site, out, out_n);
    }
    /* Anything else (atoms, quotes, keywords, ...) — clone with call_site. */
    return form_clone_with_span(r->arena, t, call_site);
}

/* User-macro dispatch hook. Called from read_form when it sees a '#'.
 *
 * Returns:
 *   - a Form*  → consumed the input, produced an expansion
 *   - NULL with r->error=false → no user macro matched; rolled back to the
 *                                '#' so the caller can try built-ins
 *   - NULL with r->error=true  → matched but body was malformed
 *
 * In RM1 only RM_BODY_NONE (bare) and RM_BODY_RAW are honored end-to-end;
 * RM_BODY_DATUM lands in RM2. */
static Form *try_read_user_macro(Reader *r) {
    /* We always need to attempt parsing here because built-in named string
     * macros (currently `#rx"..."`) dispatch through this hook too — a
     * blanket "skip when registry empty" would miss them. */

    size_t   save_pos  = r->pos;
    uint32_t save_line = r->line;
    uint32_t save_col  = r->col;

    uint32_t call_line = r->line;
    uint32_t call_col  = r->col;
    size_t   call_off  = r->pos;

    advance(r); /* consume '#' */

    if (!is_macro_id_start(peek(r))) {
        r->pos = save_pos; r->line = save_line; r->col = save_col;
        return NULL;
    }
    size_t id_start = r->pos;
    while (is_macro_id_cont(peek(r))) advance(r);
    StrSlice name = strslice(r->src + id_start,
                             (uint32_t)(r->pos - id_start));

    int delim_char = peek(r);
    int try_delim  = 0;
    if (delim_char == '(' || delim_char == '[' || delim_char == '{'
        || delim_char == '"') {
        try_delim = delim_char;
    }

    /* Built-in named string macro: #rx"..." → (re/compile "..."). The name
     * `rx` is reserved (see reader_macros.c::kReserved) so no user macro
     * can shadow it. */
    if (try_delim == '"' && name.len == 2
        && name.p[0] == 'r' && name.p[1] == 'x') {
        bool save_neo = r->neoteric_enabled;
        r->neoteric_enabled = false;
        Form *str = read_string(r);
        r->neoteric_enabled = save_neo;
        if (!str || r->error) return NULL;
        Span full_site =
            span_from_to(r, call_line, call_col, call_off, r->pos);
        const Symbol *re_compile =
            symtab_intern(r->st, strslice("re/compile", 10));
        Form *head = form_sym(r->arena, full_site, re_compile);
        Form **items = (Form **)arena_alloc(r->arena, sizeof(Form *) * 2);
        items[0] = head;
        items[1] = str;
        return form_list(r->arena, full_site, items, 2);
    }

    /* From here on we need a user-registry hit. */
    const ReaderMacroEntry *e = NULL;
    if (r->user_macros && r->user_macros->len > 0) {
        if (try_delim != 0) {
            e = reader_macros_lookup(r->user_macros, name, try_delim);
        }
        if (!e) {
            e = reader_macros_lookup(r->user_macros, name, 0);
        }
    }
    if (!e) {
        /* No exact (name, delim) match. If a macro with this name exists
         * under a different delimiter, the user almost certainly meant it —
         * emit a targeted diagnostic instead of silently rewinding into
         * the generic "unexpected character" path. */
        const ReaderMacroEntry *any = (r->user_macros && r->user_macros->len > 0)
            ? reader_macros_lookup_any(r->user_macros, name) : NULL;
        if (any) {
            if (any->mode == RM_BODY_NONE) {
                diag_emit(DIAG_ERROR, span_point(r),
                          "reader macro '#%.*s' takes no body, "
                          "but a '%c' delimiter follows",
                          (int)name.len, name.p, (char)delim_char);
            } else if (any->mode == RM_BODY_STRING) {
                diag_emit(DIAG_ERROR, span_point(r),
                          "reader string macro '#%.*s' expects string body",
                          (int)name.len, name.p);
            } else {
                diag_emit(DIAG_ERROR, span_point(r),
                          "reader macro '#%.*s' expects '%c' body, got '%c'",
                          (int)name.len, name.p,
                          (char)any->delim,
                          delim_char == -1 ? '?' : (char)delim_char);
            }
            r->error = true;
            return NULL;
        }
        /* A `#name"..."` invocation that didn't match anything is almost
         * certainly an intended string macro the user forgot to register —
         * emit a targeted diagnostic rather than silently rewinding (which
         * would surface as the generic "unexpected character" later). */
        if (try_delim == '"') {
            diag_emit(DIAG_ERROR, span_point(r),
                      "unknown reader string macro '#%.*s'",
                      (int)name.len, name.p);
            r->error = true;
            return NULL;
        }
        /* Truly unknown — rewind so the caller's built-in checks (and the
         * final "unexpected character" fallback) still apply. */
        r->pos = save_pos; r->line = save_line; r->col = save_col;
        return NULL;
    }

    Span name_site = span_from_to(r, call_line, call_col, call_off, r->pos);

    if (e->mode == RM_BODY_NONE) {
        /* Bare form: any following delimiter is not part of this macro. */
        return expand_raw_template(r, e->template, "", 0, name_site);
    }

    if (try_delim == 0 || try_delim != e->delim) {
        if (e->mode == RM_BODY_STRING) {
            diag_emit(DIAG_ERROR, span_point(r),
                      "reader string macro '#%.*s' expects string body",
                      (int)name.len, name.p);
        } else {
            diag_emit(DIAG_ERROR, span_point(r),
                      "reader macro '#%.*s' expects '%c' body",
                      (int)name.len, name.p, (char)e->delim);
        }
        r->error = true;
        return NULL;
    }

    if (e->mode == RM_BODY_STRING) {
        bool save_neo = r->neoteric_enabled;
        r->neoteric_enabled = false;
        Form *str = read_string(r);
        r->neoteric_enabled = save_neo;
        if (!str || r->error) return NULL;
        Span full_site =
            span_from_to(r, call_line, call_col, call_off, r->pos);
        return expand_raw_template(r, e->template,
                                   str->as.s.p, str->as.s.len, full_site);
    }

    int close = matching_close_for(e->delim);

    if (e->mode == RM_BODY_RAW) {
        uint32_t body_len = 0;
        char *body = read_raw_body(r, e->delim, close, &body_len);
        if (r->error) return NULL;
        Span full_site =
            span_from_to(r, call_line, call_col, call_off, r->pos);
        return expand_raw_template(r, e->template, body, body_len, full_site);
    }

    /* RM_BODY_DATUM: recursively read forms until the matched close. */
    const char *unterm_msg;
    switch (e->delim) {
        case '(': unterm_msg =
            "unterminated reader macro body (missing ')')"; break;
        case '[': unterm_msg =
            "unterminated reader macro body (missing ']')"; break;
        case '{': unterm_msg =
            "unterminated reader macro body (missing '}')"; break;
        default:  unterm_msg = "unterminated reader macro body";
    }
    /* read_seq picks the underlying container type from `tag`; for the
     * purposes of splicing into the template we only need access to its
     * items + len, so F_LIST for paren/brace and F_VEC for bracket is a
     * reasonable choice (preserves the natural sequence shape). */
    FormTag body_tag = (e->delim == '[') ? F_VEC : F_LIST;
    Form *seq = read_seq(r, (char)e->delim, (char)close, body_tag, unterm_msg);
    if (!seq || r->error) return NULL;
    Span full_site = span_from_to(r, call_line, call_col, call_off, r->pos);
    return expand_datum_template(r, e->template,
                                 seq->as.list.items, seq->as.list.len,
                                 full_site);
}

static Form *read_form(Reader *r) {
    skip_ws_and_comments(r);
    if (r->error) return NULL;
    int c = peek(r);
    if (c == -1) return NULL;

    /* RM0: Try user-registered #-dispatch macros before built-ins. The
     * registry rejects any name that would shadow a built-in, so order
     * is correctness-neutral; user-first just keeps the common case fast.
     * In RM0 this always returns NULL (no registry / empty registry). */
    if (c == '#') {
        Form *m = try_read_user_macro(r);
        if (m || r->error) return m;
    }

    /* DC1/DC2: Datum comment #;datum -- read and discard one form */
    if (c == '#' && peek2(r) == ';') {
        Span s = span_point(r);
        advance(r); advance(r);  /* consume '#' and ';' */
        skip_ws_and_comments(r);
        int next = peek(r);
        if (next == -1) {
            diag_emit(DIAG_ERROR, s,
                      "datum comment #; requires a following form, "
                      "got end of input");
            r->error = true;
            return NULL;
        }
        if (next == ')' || next == ']' || next == '}') {
            diag_emit(DIAG_ERROR, s,
                      "datum comment #; requires a following form, got '%c'",
                      (char)next);
            r->error = true;
            return NULL;
        }
        Form *discarded = read_form(r);
        if (r->error) return NULL;
        if (!discarded) {
            diag_emit(DIAG_ERROR, s,
                      "datum comment #; requires a following form, "
                      "got end of input");
            r->error = true;
            return NULL;
        }
        (void)discarded;
        return read_form(r);
    }
    if (c == '#' && peek2(r) == '{') {
        return read_map(r);
    }
    if (c == '#' && peek2(r) == 's' && peek3(r) == '(') {
        return read_set(r);
    }
    /* RR0: Range literal #r{...} */
    if (c == '#' && peek2(r) == 'r' && peek3(r) == '{') {
        return read_range_literal(r);
    }
    /* Phase R5: Attribute syntax #[...] */
    if (c == '#' && peek2(r) == '[') {
        return read_attribute(r);
    }
    /* INT-1: Reader conditional #?(:tur expr :turi expr) */
    if (c == '#' && peek2(r) == '?') {
        return read_reader_cond(r);
    }

    /* CT0 / Phase S1: Curly-brace handling.
     * In curly-infix mode (SRFI-105), '{' is the infix operator.
     * Otherwise '{' introduces a contract type { var : T | pred }. */
    if (c == '{') {
        if (r->curly_infix_enabled) {
            return read_curly_infix(r);
        } else {
            /* CT0: contract type annotation { var : T | pred } */
            return read_contract_type(r);
        }
    }
    
    if (c == '(') return read_seq(r, '(', ')', F_LIST, "unterminated list (missing ')')");
    if (c == '[') return read_seq(r, '[', ']', F_VEC,  "unterminated vector (missing ']')");
    if (c == ')' || c == ']' || c == '}') {
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
    /* '@' as deref/effect-row prefix */
    if (c == '@') return read_at(r);
    /* Phase 6: ' as quote operator */
    if (c == '\'') return read_quote(r);
    /* Phase 6: ~ as unquote operator (only valid inside quasiquote) */
    if (c == '~') return read_unquote(r);
    /* Phase 12: & as borrow prefix sugar (&x → (& x), &mut x → (&mut x)) */
    if (c == '&') return read_borrow(r);
    if (is_sym_start(c)) return read_symbol_or_minus(r);

    Span s = span_point(r);
    diag_emit(DIAG_ERROR, s, "unexpected character '%c' (0x%02x)", (char)c, c);
    r->error = true;
    advance(r);
    return NULL;
}

#include <stdio.h>

/* RM4: implementation backing both `#use-reader-macros "..."` and the
 * spice-manifest `:reader-macros [...]` preloader. Reads `abs_path` and
 * recursively reads its top-level forms into `registry`. Non-directive
 * forms are silently discarded. Returns 0 on success, -1 on failure
 * (diagnostic emitted at the global level). */
int reader_macros_load_file(Arena *arena, SymbolTable *st,
                            const char *abs_path,
                            struct ReaderMacroRegistry *registry) {
    FILE *fp = fopen(abs_path, "rb");
    if (!fp) {
        diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                  "reader-macros: cannot open '%s'", abs_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                  "reader-macros: cannot stat '%s'", abs_path);
        return -1;
    }
    char *buf = (char *)arena_alloc(arena, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    buf[got] = '\0';
    fclose(fp);

    SourceFile *sf = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
    sf->path        = arena_strdup(arena, abs_path, strlen(abs_path));
    sf->src         = buf;
    sf->len         = (uint32_t)got;
    /* Use a near-max file_id so we don't overwrite an existing slot.
     * MAX_FILES (in diag.c) is currently 64; pick the last slot. This is
     * best-effort — collisions across multiple preloads in the same
     * compile only affect diagnostic snippet rendering, not correctness. */
    sf->file_id     = 63;
    sf->reader_type = READER_TURMERIC;
    diag_register_file(sf);

    uint32_t sub_nforms = 0;
    Form **sub_forms = read_all_with_registry(arena, st, sf,
                                              registry, &sub_nforms);
    (void)sub_forms;
    return diag_had_error() ? -1 : 0;
}

/* RM4: resolve a relative path against the directory containing
 * `base_path`. Absolute paths pass through unchanged. The result is
 * written into `out` (at most `out_sz` bytes including the NUL). */
static void rm_resolve_relative(const char *base_path, const char *rel,
                                char *out, size_t out_sz) {
    if (rel[0] == '/') {
        snprintf(out, out_sz, "%s", rel);
        return;
    }
    const char *slash = base_path ? strrchr(base_path, '/') : NULL;
    if (slash) {
        size_t dir_len = (size_t)(slash - base_path + 1);
        snprintf(out, out_sz, "%.*s%s", (int)dir_len, base_path, rel);
    } else {
        snprintf(out, out_sz, "%s", rel);
    }
}

/* RM4: attempt to consume a `#use-reader-macros "path"` directive at the
 * current reader position. Returns:
 *   - 1 if a directive was consumed (and processed; r->error may be set
 *     on failure to load / parse the named file)
 *   - 0 if the input doesn't look like the directive (reader untouched).
 *
 * Only meaningful at the top level — the caller (read_all_with_registry)
 * invokes this between top-level forms.
 *
 * The named file is read with its own sub-Reader against `reg`, so any
 * `(reader-macros/define ...)` directives in it register into the
 * caller's registry. Non-directive forms in the loaded file are silently
 * discarded — `#use-reader-macros` is a syntax-only mechanism. */
static int try_consume_use_directive(Reader *r,
                                     struct ReaderMacroRegistry *reg) {
    static const char kKeyword[] = "use-reader-macros";
    const size_t kKwLen = sizeof(kKeyword) - 1;

    if (peek(r) != '#') return 0;
    if (r->pos + 1 + kKwLen > r->len) return 0;
    if (memcmp(r->src + r->pos + 1, kKeyword, kKwLen) != 0) return 0;
    /* Make sure the keyword isn't a prefix of a longer identifier. */
    int after = (r->pos + 1 + kKwLen < r->len)
        ? (unsigned char)r->src[r->pos + 1 + kKwLen] : -1;
    if (after != -1 && is_macro_id_cont(after)) return 0;

    /* Commit: consume '#' + keyword bytes. */
    uint32_t kw_line = r->line;
    uint32_t kw_col  = r->col;
    size_t   kw_off  = r->pos;
    advance(r);
    for (size_t i = 0; i < kKwLen; ++i) advance(r);

    skip_ws_and_comments(r);
    if (peek(r) != '"') {
        diag_emit(DIAG_ERROR, span_point(r),
                  "#use-reader-macros: expected a string literal "
                  "(file path) after the directive");
        r->error = true;
        return 1;
    }

    Form *path_form = read_string(r);
    if (!path_form || r->error) {
        r->error = true;
        return 1;
    }

    char abs_path[4096];
    rm_resolve_relative(r->file->path, path_form->as.s.p,
                        abs_path, sizeof(abs_path));

    if (reader_macros_load_file(r->arena, r->st, abs_path, reg) != 0) {
        /* reader_macros_load_file emitted its own diagnostic; flag the
         * outer reader so the calling read_all bails out. The kw_*
         * captures stay around in case we want to re-anchor later. */
        (void)kw_line; (void)kw_col; (void)kw_off;
        r->error = true;
    }
    return 1;
}

Form **read_all_with_registry(Arena *arena, SymbolTable *st,
                              const SourceFile *file,
                              struct ReaderMacroRegistry *external_reg,
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
    /* Phase S1: Set syntax feature flags based on reader type from SourceFile */
    r.curly_infix_enabled = false;
    r.neoteric_enabled = false;
    /* RM1: Reader-macro registry. If the caller supplied one, dispatch and
     * registration happen against it directly (REPL session semantics);
     * otherwise we keep a per-call local one (file semantics). */
    ReaderMacroRegistry local_reg;
    ReaderMacroRegistry *reg = external_reg;
    if (!reg) {
        reader_macros_init(&local_reg, arena);
        reg = &local_reg;
    }
    r.user_macros = reg;

    switch (file->reader_type) {
        case READER_TURMERIC:
            /* Standard s-expression syntax only */
            break;
        case READER_CURLY_INFIX:
            r.curly_infix_enabled = true;
            break;
        case READER_NEOTERIC:
            r.curly_infix_enabled = true;
            r.neoteric_enabled = true;
            break;
        case READER_SWEET:
            r.curly_infix_enabled = true;
            r.neoteric_enabled = true;
            break;
        case READER_UNKNOWN:
            break;
    }

    Form **forms = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(&r);
        if (r.error) {
            free(forms);
            return NULL;
        }
        if (peek(&r) == -1) break;
        /* RM4: top-level `#use-reader-macros "path"` directive — read
         * macros from another file before continuing this one. */
        if (try_consume_use_directive(&r, reg)) {
            if (r.error) { free(forms); return NULL; }
            continue;
        }
        Form *f = read_form(&r);
        if (!f) {
            /* A NULL return with no error flag set means the reader hit EOF
             * after consuming whitespace, comments, or a datum-comment
             * `#;<form>` whose trailing form was the last meaningful token
             * in the file.  Treat as end-of-input rather than parse error. */
            if (!r.error) break;
            free(forms);
            return NULL;
        }
        /* RM1: top-level `(reader-macros/define ...)` is a directive: it
         * registers an entry in the registry that subsequent forms can
         * dispatch off, and is stripped from the output Form stream. */
        if (reader_macros_is_define_form(f)) {
            if (reader_macros_register_from_form(reg, f) != 0) {
                r.error = true;
                free(forms);
                return NULL;
            }
            continue;
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

Form **read_all(Arena *arena, SymbolTable *st, const SourceFile *file,
                uint32_t *out_count) {
    return read_all_with_registry(arena, st, file, NULL, out_count);
}

/* #lang directive detection and reader type utilities */

/* Parse #lang directive from the first line of source */
ReaderType detect_lang(const char *src, size_t len, const char **out_rest, 
                       size_t *out_rest_len) {
    const char *p = src;
    size_t remaining = len;

    /* Skip leading whitespace (but not newlines - #lang must be first line) */
    while (remaining > 0 && (p[0] == ' ' || p[0] == '\t')) {
        p++;
        remaining--;
    }

    /* Check for #lang */
    if (remaining >= 5 && p[0] == '#' && p[1] == 'l' && p[2] == 'a' && 
        p[3] == 'n' && p[4] == 'g') {
        p += 5;
        remaining -= 5;

        /* Skip whitespace after #lang */
        while (remaining > 0 && (p[0] == ' ' || p[0] == '\t')) {
            p++;
            remaining--;
        }

        /* Extract the language name (can contain slashes) */
        const char *lang_start = p;
        size_t lang_len = 0;
        while (remaining > 0 && p[0] != ' ' && p[0] != '\t' && 
               p[0] != '\n' && p[0] != '\r') {
            p++;
            lang_len++;
            remaining--;
        }

        /* Determine reader type from language name */
        if (lang_len == 8 && memcmp(lang_start, "turmeric", 8) == 0) {
            if (out_rest) *out_rest = p;
            if (out_rest_len) *out_rest_len = remaining;
            return READER_TURMERIC;
        } else if (lang_len == 20 && memcmp(lang_start, "turmeric/curly-infix", 20) == 0) {
            if (out_rest) *out_rest = p;
            if (out_rest_len) *out_rest_len = remaining;
            return READER_CURLY_INFIX;
        } else if (lang_len == 17 && memcmp(lang_start, "turmeric/neoteric", 17) == 0) {
            if (out_rest) *out_rest = p;
            if (out_rest_len) *out_rest_len = remaining;
            return READER_NEOTERIC;
        } else if (lang_len == 9 && memcmp(lang_start, "sweet-exp", 9) == 0) {
            if (out_rest) *out_rest = p;
            if (out_rest_len) *out_rest_len = remaining;
            return READER_SWEET;
        }

        /* Unknown #lang - return a special value to indicate error */
        /* We'll use READER_TURMERIC + 1 as a sentinel, but better to add a new type */
        /* For now, just return TURMERIC and let the caller check */
        if (out_rest) *out_rest = p;
        if (out_rest_len) *out_rest_len = remaining;
        /* Return a special "unknown" type - we'll add this to the enum */
        return (ReaderType)-1; /* Unknown/invalid */
    }

    /* No #lang directive found */
    if (out_rest) *out_rest = src;
    if (out_rest_len) *out_rest_len = len;
    return READER_TURMERIC;
}

/* Get reader type from file extension */
ReaderType reader_type_from_extension(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return READER_TURMERIC;

    if (strcmp(ext, ".tursweet") == 0) {
        return READER_SWEET;
    }
    return READER_TURMERIC;
}

/* Get reader type name as string */
const char *reader_type_name(ReaderType type) {
    switch (type) {
        case READER_UNKNOWN: return "unknown";
        case READER_TURMERIC: return "turmeric";
        case READER_CURLY_INFIX: return "turmeric/curly-infix";
        case READER_NEOTERIC: return "turmeric/neoteric";
        case READER_SWEET: return "sweet-exp";
        default: return "<invalid>";
    }
}

/* Check if a reader type is implemented */
bool reader_type_is_implemented(ReaderType type) {
    switch (type) {
        case READER_UNKNOWN:
            return false; /* Unknown language is not implemented */
        case READER_TURMERIC:
        case READER_CURLY_INFIX:
        case READER_NEOTERIC:
            return true; /* Phase S2: neoteric is now implemented */
        case READER_SWEET:
            return true; /* curly-infix + neoteric are implemented; full indent-sensitive sweet-exp is not */
        default:
            return false;
    }
}
