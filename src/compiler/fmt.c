/* fmt.c — Turmeric source code pretty-printer.
 *
 * Architecture:
 *   fmt_print()       public entry point; iterates top-level forms
 *   fmt_form()        dispatches on FormTag
 *   fmt_list()        handles F_LIST: tries inline, falls back to special or
 *                     generic layout
 *   fmt_form_flat()   renders a form compactly onto a Buf (for measuring)
 *   fmt_measure()     returns flat width of a form (UINT32_MAX if it has \n)
 *
 * Comment handling (option a from plan):
 *   The source text is re-scanned between form spans to extract comments.
 *   If opts.src is NULL, comments are dropped and one blank line is emitted
 *   between top-level forms.
 */

#include "fmt.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------------
 */

typedef struct FmtState {
    Buf       *buf;
    uint32_t   col;   /* current column (0-based) */
    FmtOptions opts;
} FmtState;

/* ---------------------------------------------------------------------------
 * Low-level emit helpers
 * ---------------------------------------------------------------------------
 */

static void fs_putc(FmtState *s, char c) {
    buf_putc(s->buf, c);
    if (c == '\n') s->col = 0;
    else s->col++;
}

static void fs_write(FmtState *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) fs_putc(s, p[i]);
}

static void fs_puts(FmtState *s, const char *str) {
    fs_write(s, str, strlen(str));
}

static void fs_printf(FmtState *s, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) fs_write(s, tmp, (size_t)n);
}

/* Emit a newline then `col` spaces. */
static void fs_newline_indent(FmtState *s, uint32_t col) {
    fs_putc(s, '\n');
    for (uint32_t i = 0; i < col; i++) fs_putc(s, ' ');
}

/* ---------------------------------------------------------------------------
 * Symbol comparison
 * ---------------------------------------------------------------------------
 */

static bool sym_eq(const Symbol *sym, const char *name) {
    size_t n = strlen(name);
    return (size_t)sym->len == n && memcmp(sym->name, name, n) == 0;
}

/* ---------------------------------------------------------------------------
 * Flat printer — renders a form inline (no line breaks) into a Buf.
 * Used for measuring and for emitting forms that fit on the current line.
 * ---------------------------------------------------------------------------
 */

static void fmt_form_flat(Buf *b, const Form *f);

static void print_str_escaped_b(Buf *b, StrSlice s) {
    buf_putc(b, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        char c = s.p[i];
        switch (c) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\t': buf_puts(b, "\\t");  break;
            case '\r': buf_puts(b, "\\r");  break;
            default:
                if ((unsigned char)c < 0x20)
                    buf_printf(b, "\\x%02x", (unsigned char)c);
                else
                    buf_putc(b, c);
        }
    }
    buf_putc(b, '"');
}

static void fmt_form_flat(Buf *b, const Form *f) {
    switch (f->tag) {
        case F_NIL:   buf_puts(b, "nil"); break;
        case F_BOOL:  buf_puts(b, f->as.b ? "true" : "false"); break;
        case F_INT:   buf_printf(b, "%lld", (long long)f->as.i); break;
        case F_FLOAT: buf_printf(b, "%g", f->as.f); break;
        case F_STR:   print_str_escaped_b(b, f->as.s); break;
        case F_SYM:
            buf_write(b, f->as.sym->name, f->as.sym->len);
            break;
        case F_KEYWORD:
            buf_putc(b, ':');
            buf_write(b, f->as.sym->name, f->as.sym->len);
            break;
        case F_LIST:
            buf_putc(b, '(');
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_VEC:
            buf_putc(b, '[');
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, ']');
            break;
        case F_MAP:
            buf_puts(b, "#{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_SET:
            buf_puts(b, "#s(");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_CBLOCK:
            buf_puts(b, "```c ");
            buf_write(b, f->as.cblock.p, f->as.cblock.len);
            buf_puts(b, "```");
            break;
        case F_QUOTE:
            buf_putc(b, '\'');
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_QUASIQUOTE:
            buf_putc(b, '`');
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_UNQUOTE:
            buf_putc(b, '~');
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_UNQUOTE_SPLICING:
            buf_puts(b, "~@");
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_TYPE_ANN:
            buf_puts(b, ": ");
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        /* CT0: Contract type { var : T | pred } */
        case F_CONTRACT_TYPE:
            buf_puts(b, "{ ");
            for (uint32_t _i = 0; _i < f->as.list.len; _i++) {
                if (_i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[_i]);
            }
            buf_puts(b, " }");
            break;
    }
}

/* Measure the flat width of a form.
 * Returns UINT32_MAX if the form contains a literal newline character. */
static uint32_t fmt_measure(const Form *f) {
    Buf tmp;
    buf_init(&tmp);
    fmt_form_flat(&tmp, f);
    bool has_nl = false;
    for (size_t i = 0; i < tmp.len; i++) {
        if (tmp.data[i] == '\n') { has_nl = true; break; }
    }
    uint32_t w = has_nl ? UINT32_MAX : (uint32_t)tmp.len;
    buf_free(&tmp);
    return w;
}

/* Emit a form via the flat printer into the state (always inline). */
static void fmt_emit_inline(FmtState *s, const Form *f) {
    Buf tmp;
    buf_init(&tmp);
    fmt_form_flat(&tmp, f);
    fs_write(s, tmp.data, tmp.len);
    buf_free(&tmp);
}

/* ---------------------------------------------------------------------------
 * Special-form classification
 * ---------------------------------------------------------------------------
 */

typedef enum SpecialForm {
    SF_NONE,
    SF_DEFPACKAGE,          /* (defpackage ...) and (deflockfile ...) */
    SF_DEFN, SF_DEFMACRO,
    SF_FN,
    SF_LET, SF_LOOP,
    SF_IF,
    SF_WHEN, SF_UNLESS,
    SF_DO,
    SF_CASE,
    SF_HANDLE,
    SF_DEFCLASS,
    SF_DEFINSTANCE,
    SF_DEFEFFECT,
} SpecialForm;

static const Symbol *list_head_sym(const Form *f) {
    if (f->tag != F_LIST || f->as.list.len == 0) return NULL;
    const Form *h = f->as.list.items[0];
    if (h->tag != F_SYM) return NULL;
    return h->as.sym;
}

static SpecialForm classify_list(const Form *f) {
    const Symbol *h = list_head_sym(f);
    if (!h) return SF_NONE;
    if (sym_eq(h, "defpackage"))  return SF_DEFPACKAGE;
    if (sym_eq(h, "deflockfile")) return SF_DEFPACKAGE;
    if (sym_eq(h, "defn"))        return SF_DEFN;
    if (sym_eq(h, "defmacro"))    return SF_DEFMACRO;
    if (sym_eq(h, "fn"))          return SF_FN;
    if (sym_eq(h, "let"))         return SF_LET;
    if (sym_eq(h, "if"))          return SF_IF;
    if (sym_eq(h, "when"))        return SF_WHEN;
    if (sym_eq(h, "unless"))      return SF_UNLESS;
    if (sym_eq(h, "do"))          return SF_DO;
    if (sym_eq(h, "case"))        return SF_CASE;
    if (sym_eq(h, "loop"))        return SF_LOOP;
    if (sym_eq(h, "handle"))      return SF_HANDLE;
    if (sym_eq(h, "defclass"))    return SF_DEFCLASS;
    if (sym_eq(h, "definstance")) return SF_DEFINSTANCE;
    if (sym_eq(h, "defeffect"))   return SF_DEFEFFECT;
    return SF_NONE;
}

/* ---------------------------------------------------------------------------
 * Pretty-printer — forward declarations
 * ---------------------------------------------------------------------------
 */

static void fmt_form(FmtState *s, const Form *f);
static void fmt_list(FmtState *s, const Form *f);

/* ---------------------------------------------------------------------------
 * Special-form layouts
 * ---------------------------------------------------------------------------
 */

/* (defn name [params] :ret\n  body...) */
static void fmt_defn(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Header items: defn/defmacro, name, params, optional :ret keyword or type annotation */
    uint32_t header_end = 3;
    if (n > 3 && (f->as.list.items[3]->tag == F_KEYWORD
               || f->as.list.items[3]->tag == F_TYPE_ANN)) header_end = 4;

    for (uint32_t i = 0; i < header_end && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    for (uint32_t i = header_end; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (fn [params] [:ret]\n  body...) */
static void fmt_fn(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Header: fn, params, optional :ret keyword or type annotation */
    uint32_t header_end = 2;
    if (n > 2 && (f->as.list.items[2]->tag == F_KEYWORD
               || f->as.list.items[2]->tag == F_TYPE_ANN)) header_end = 3;

    for (uint32_t i = 0; i < header_end && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    for (uint32_t i = header_end; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (let [bindings]\n  body...) — also used for loop */
static void fmt_let(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Header: let/loop + bindings vector */
    for (uint32_t i = 0; i < 2 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    for (uint32_t i = 2; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (if test\n  then\n  else) */
static void fmt_if(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* "if" and test on the same first line */
    for (uint32_t i = 0; i < 2 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    /* then and optional else on separate indented lines */
    for (uint32_t i = 2; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (when test\n  body...) — also for unless */
static void fmt_when(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    for (uint32_t i = 0; i < 2 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    for (uint32_t i = 2; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (do\n  form...) */
static void fmt_do(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    if (n >= 1) fmt_form(s, f->as.list.items[0]); /* "do" */

    for (uint32_t i = 1; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (case expr\n  pat result\n  pat result...) */
static void fmt_case(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* "case" + expr on first line */
    for (uint32_t i = 0; i < 2 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    /* Arms: each pat+result pair on its own line */
    uint32_t i = 2;
    while (i < n) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
        i++;
        if (i < n) {
            fs_putc(s, ' ');
            fmt_form(s, f->as.list.items[i]);
            i++;
        }
    }

    fs_putc(s, ')');
}

/* (handle expr\n  arm...) */
static void fmt_handle(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    for (uint32_t i = 0; i < 2 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    for (uint32_t i = 2; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (defclass Name [params]\n  method...) */
static void fmt_defclass(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    for (uint32_t i = 0; i < 3 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    for (uint32_t i = 3; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* (definstance Name Class Type\n  impl...) */
static void fmt_definstance(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    for (uint32_t i = 0; i < 4 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    for (uint32_t i = 4; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* Generic function call:
 *   (f a b c)          — if inline fits
 *   (f a               — if not: head+first-arg on same line,
 *     b                   remaining args indented by indent_width
 *     c)
 */
static void fmt_call(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Head + first arg on the opening line */
    uint32_t on_first_line = (n > 2) ? 2 : n;
    for (uint32_t i = 0; i < on_first_line; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    /* Remaining args on indented lines */
    for (uint32_t i = on_first_line; i < n; i++) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]);
    }

    fs_putc(s, ')');
}

/* ---------------------------------------------------------------------------
 * Collection layouts (vector, map, set)
 * ---------------------------------------------------------------------------
 */

/* [a\n b\n c] — 1-space indent from '[' */
static void fmt_vec_broken(FmtState *s, const Form *f) {
    uint32_t inner = s->col + 1; /* one past '[' */
    fs_putc(s, '[');
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        if (i) fs_newline_indent(s, inner);
        fmt_form(s, f->as.list.items[i]);
    }
    fs_putc(s, ']');
}

/* #{k v\n  k v} */
static void fmt_map_broken(FmtState *s, const Form *f) {
    uint32_t inner = s->col + 2; /* two past '#{' */
    uint32_t n = f->as.list.len;
    fs_puts(s, "#{");
    uint32_t i = 0;
    while (i < n) {
        if (i) fs_newline_indent(s, inner);
        fmt_form(s, f->as.list.items[i]); i++;
        if (i < n) {
            fs_putc(s, ' ');
            fmt_form(s, f->as.list.items[i]); i++;
        }
    }
    fs_putc(s, '}');
}

/* #s(a\n   b) */
static void fmt_set_broken(FmtState *s, const Form *f) {
    uint32_t inner = s->col + 3; /* three past '#s(' */
    fs_puts(s, "#s(");
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        if (i) fs_newline_indent(s, inner);
        fmt_form(s, f->as.list.items[i]);
    }
    fs_putc(s, ')');
}

/* Emit a non-empty F_MAP in block style:
 *   #{
 *     k v           ← at entry_col
 *     k v
 *   }               ← closing at close_col
 * Used by fmt_defpackage for :spices / :cmake-deps values. */
static void fmt_map_block(FmtState *s, const Form *f,
                           uint32_t entry_col, uint32_t close_col) {
    uint32_t n = f->as.list.len;
    fs_puts(s, "#{");
    uint32_t i = 0;
    while (i < n) {
        fs_newline_indent(s, entry_col);
        fmt_form(s, f->as.list.items[i]); i++;
        if (i < n) {
            fs_putc(s, ' ');
            fmt_form(s, f->as.list.items[i]); i++;
        }
    }
    fs_newline_indent(s, close_col);
    fs_putc(s, '}');
}

/* (defpackage name        also handles (deflockfile ...)
 *   :key1 val1
 *   :spices #{
 *     "name" #{...}
 *   }) */
static void fmt_defpackage(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* head (defpackage/deflockfile) + package name */
    for (uint32_t i = 0; i < 2 && i < n; i++) {
        if (i) fs_putc(s, ' ');
        fmt_form(s, f->as.list.items[i]);
    }

    /* keyword-value pairs starting at index 2 */
    uint32_t i = 2;
    while (i < n) {
        fs_newline_indent(s, body_col);
        fmt_form(s, f->as.list.items[i]); /* keyword */
        i++;
        if (i < n) {
            const Form *val = f->as.list.items[i];
            fs_putc(s, ' ');
            /* Non-empty map value: try inline, fall back to block */
            if (val->tag == F_MAP && val->as.list.len > 0) {
                uint32_t w = fmt_measure(val);
                if (s->col + w <= s->opts.line_width) {
                    fmt_emit_inline(s, val);
                } else {
                    fmt_map_block(s, val,
                                  body_col + s->opts.indent_width,
                                  body_col);
                }
            } else {
                fmt_form(s, val);
            }
            i++;
        }
    }

    fs_putc(s, ')');
}

/* ---------------------------------------------------------------------------
 * Main list dispatcher
 * ---------------------------------------------------------------------------
 */

static void fmt_list(FmtState *s, const Form *f) {
    /* Always try inline first */
    uint32_t w = fmt_measure(f);
    if (s->col + w <= s->opts.line_width) {
        fmt_emit_inline(s, f);
        return;
    }

    switch (classify_list(f)) {
        case SF_DEFN:
        case SF_DEFMACRO:    fmt_defn(s, f);      break;
        case SF_FN:          fmt_fn(s, f);         break;
        case SF_LET:
        case SF_LOOP:        fmt_let(s, f);        break;
        case SF_IF:          fmt_if(s, f);         break;
        case SF_WHEN:
        case SF_UNLESS:      fmt_when(s, f);       break;
        case SF_DO:          fmt_do(s, f);         break;
        case SF_CASE:        fmt_case(s, f);       break;
        case SF_HANDLE:      fmt_handle(s, f);     break;
        case SF_DEFPACKAGE:  fmt_defpackage(s, f);  break;
        case SF_DEFCLASS:    fmt_defclass(s, f);   break;
        case SF_DEFINSTANCE: fmt_definstance(s, f);break;
        /* defeffect is usually short; if it doesn't fit, use generic layout */
        case SF_DEFEFFECT:
        default:             fmt_call(s, f);       break;
    }
}

/* ---------------------------------------------------------------------------
 * Main form dispatcher
 * ---------------------------------------------------------------------------
 */

static void fmt_form(FmtState *s, const Form *f) {
    switch (f->tag) {
        case F_NIL:   fs_puts(s, "nil");                               break;
        case F_BOOL:  fs_puts(s, f->as.b ? "true" : "false");         break;
        case F_INT:   fs_printf(s, "%lld", (long long)f->as.i);        break;
        case F_FLOAT: fs_printf(s, "%g",   f->as.f);                   break;
        case F_STR:   fmt_emit_inline(s, f);                           break;
        case F_SYM:
            fs_write(s, f->as.sym->name, f->as.sym->len);
            break;
        case F_KEYWORD:
            fs_putc(s, ':');
            fs_write(s, f->as.sym->name, f->as.sym->len);
            break;
        case F_CBLOCK:
            fs_puts(s, "```c ");
            fs_write(s, f->as.cblock.p, f->as.cblock.len);
            fs_puts(s, "```");
            break;
        case F_QUOTE:
            fs_putc(s, '\'');
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_QUASIQUOTE:
            fs_putc(s, '`');
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_UNQUOTE:
            fs_putc(s, '~');
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_UNQUOTE_SPLICING:
            fs_puts(s, "~@");
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_TYPE_ANN:
            fs_puts(s, ": ");
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        /* CT0: Contract type { var : T | pred } — format inline */
        case F_CONTRACT_TYPE: {
            uint32_t w = fmt_measure(f);
            if (s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_emit_inline(s, f); /* always inline for now */
            break;
        }
        case F_VEC: {
            uint32_t w = fmt_measure(f);
            if (s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_vec_broken(s, f);
            break;
        }
        case F_MAP: {
            uint32_t w = fmt_measure(f);
            if (s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_map_broken(s, f);
            break;
        }
        case F_SET: {
            uint32_t w = fmt_measure(f);
            if (s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_set_broken(s, f);
            break;
        }
        case F_LIST:
            fmt_list(s, f);
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Comment and blank-line extraction from source text
 * ---------------------------------------------------------------------------
 */

/* Emit any ';' comments found in src[from_off .. to_off).
 * Comments are placed on their own lines.  Returns true if anything was emitted. */
static bool emit_comments_in_gap(FmtState *s, uint32_t from_off, uint32_t to_off) {
    const char *src = s->opts.src;
    if (!src || from_off >= to_off) return false;

    const char *p   = src + from_off;
    const char *end = src + to_off;
    bool emitted = false;

    while (p < end) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }

        /* Line comment */
        if (*p == ';') {
            const char *line_end = p;
            while (line_end < end && *line_end != '\n') line_end++;
            if (s->col > 0) fs_putc(s, '\n');
            fs_write(s, p, (size_t)(line_end - p));
            emitted = true;
            p = line_end;
            continue;
        }

        /* Block comment #| ... |# */
        if (*p == '#' && p + 1 < end && p[1] == '|') {
            const char *blk = p;
            p += 2;
            while (p + 1 < end && !(p[0] == '|' && p[1] == '#')) p++;
            if (p + 1 < end) p += 2;
            if (s->col > 0) fs_putc(s, '\n');
            fs_write(s, blk, (size_t)(p - blk));
            emitted = true;
            continue;
        }

        /* Anything else is part of a form — stop */
        break;
    }
    return emitted;
}

/* Count blank lines in src[from_off .. to_off), capped at max. */
static uint32_t count_blank_lines(const char *src, uint32_t from_off,
                                   uint32_t to_off, uint32_t max) {
    if (!src || from_off >= to_off) return 0;
    const char *p = src + from_off;
    const char *end = src + to_off;
    uint32_t blanks = 0;
    bool prev_was_nl = false;
    while (p < end && blanks < max) {
        if (*p == '\n') {
            if (prev_was_nl) blanks++;
            prev_was_nl = true;
        } else if (*p != ' ' && *p != '\t' && *p != '\r') {
            prev_was_nl = false;
        }
        p++;
    }
    return blanks < max ? blanks : max;
}

/* ---------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------------
 */

int fmt_print(Buf *buf, Form **forms, uint32_t count, FmtOptions opts) {
    if (!buf || (!forms && count > 0)) return -1;

    FmtState s = {0};
    s.buf  = buf;
    s.col  = 0;
    s.opts = opts;

    if (s.opts.indent_width == 0) s.opts.indent_width = 2;
    if (s.opts.line_width   == 0) s.opts.line_width   = 80;

    uint32_t prev_end = 0; /* byte offset after the last emitted form */

    for (uint32_t i = 0; i < count; i++) {
        const Form *f = forms[i];

        if (i == 0) {
            /* Emit any leading comments before the first form */
            if (s.opts.src && f->span.off_start > 0) {
                emit_comments_in_gap(&s, 0, f->span.off_start);
                if (s.col > 0) fs_putc(&s, '\n');
            }
        } else {
            uint32_t gap_start = prev_end;
            uint32_t gap_end   = f->span.off_start;

            /* Emit comments in the gap */
            bool had_comment = emit_comments_in_gap(&s, gap_start, gap_end);

            /* Determine how many blank lines to insert */
            uint32_t blanks = 0;
            if (s.opts.src) {
                blanks = count_blank_lines(s.opts.src, gap_start, gap_end, 2);
            }
            /* At least one blank line between top-level forms */
            if (blanks == 0) blanks = 1;
            /* One extra blank line after a comment section header (;;) */
            if (had_comment && s.opts.src && gap_end > gap_start) {
                /* already handled by preserving blanks */
            }

            if (s.col > 0) fs_putc(&s, '\n');
            for (uint32_t b = 0; b < blanks; b++) fs_putc(&s, '\n');
        }

        fmt_form(&s, f);
        prev_end = f->span.off_end;
    }

    /* Trailing comments after the last form */
    if (s.opts.src && prev_end < (uint32_t)s.opts.src_len) {
        emit_comments_in_gap(&s, prev_end, (uint32_t)s.opts.src_len);
    }

    /* Ensure exactly one trailing newline */
    if (buf->len == 0 || buf->data[buf->len - 1] != '\n') {
        buf_putc(buf, '\n');
    }

    return 0;
}
