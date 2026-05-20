#include "reader.h"
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
} Reader;

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
        /* Parse integer part */
        while (peek(r) >= '0' && peek(r) <= '9') {
            int digit = advance(r) - '0';
            ival = ival * 10 + (int64_t)digit;
            fval = fval * 10.0 + (double)digit;
            any = true;
        }

        /* Check for fractional part */
        if (peek(r) == '.') {
            is_float = true;
            advance(r);  /* consume '.' */
            double frac = 0.0;
            double scale = 0.1;
            while (peek(r) >= '0' && peek(r) <= '9') {
                int digit = advance(r) - '0';
                frac += (double)digit * scale;
                scale *= 0.1;
                any = true;
            }
            fval += frac;
        }

        /* Check for exponent part */
        if ((peek(r) == 'e' || peek(r) == 'E') && any) {
            is_float = true;
            advance(r);  /* consume 'e' or 'E' */
            int exp_sign = 1;
            if (peek(r) == '+') {
                advance(r);
            } else if (peek(r) == '-') {
                advance(r);
                exp_sign = -1;
            }
            int exp_val = 0;
            bool exp_any = false;
            while (peek(r) >= '0' && peek(r) <= '9') {
                exp_val = exp_val * 10 + (advance(r) - '0');
                exp_any = true;
            }
            if (!exp_any) {
                Span s = span_point(r);
                diag_emit(DIAG_ERROR, s, "expected exponent digits in float literal");
                r->error = true;
                return NULL;
            }
            /* Apply exponent using pow(10, exp) */
            double exp_factor = 1.0;
            int abs_exp = exp_sign < 0 ? -exp_val : exp_val;
            for (int i = 0; i < abs_exp; i++) {
                if (exp_sign > 0) exp_factor *= 10.0;
                else exp_factor /= 10.0;
            }
            fval *= exp_factor;
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
        return items[0];
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

static Form *read_form(Reader *r) {
    skip_ws_and_comments(r);
    if (r->error) return NULL;
    int c = peek(r);
    if (c == -1) return NULL;

    if (c == '#' && peek2(r) == '{') {
        return read_map(r);
    }
    if (c == '#' && peek2(r) == 's' && peek3(r) == '(') {
        return read_set(r);
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
    /* Phase S1: Set syntax feature flags based on reader type from SourceFile */
    r.curly_infix_enabled = false;
    r.neoteric_enabled = false;
    
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
