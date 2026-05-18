#include "forms.h"

#include <string.h>

Form *form_new(Arena *a, FormTag tag, Span span) {
    Form *f = (Form *)arena_alloc(a, sizeof(Form));
    memset(f, 0, sizeof(*f));
    f->tag = tag;
    f->span = span;
    return f;
}

Form *form_nil(Arena *a, Span span) {
    return form_new(a, F_NIL, span);
}

Form *form_bool(Arena *a, Span span, bool b) {
    Form *f = form_new(a, F_BOOL, span);
    f->as.b = b;
    return f;
}

Form *form_int(Arena *a, Span span, int64_t i) {
    Form *f = form_new(a, F_INT, span);
    f->as.i = i;
    return f;
}

Form *form_float(Arena *a, Span span, double f) {
    Form *form = form_new(a, F_FLOAT, span);
    form->as.f = f;
    return form;
}

Form *form_str(Arena *a, Span span, const char *p, uint32_t len) {
    Form *f = form_new(a, F_STR, span);
    f->as.s.p = arena_strdup(a, p, len);
    f->as.s.len = len;
    return f;
}

Form *form_sym(Arena *a, Span span, const Symbol *sym) {
    Form *f = form_new(a, F_SYM, span);
    f->as.sym = sym;
    return f;
}

Form *form_keyword(Arena *a, Span span, const Symbol *sym) {
    Form *f = form_new(a, F_KEYWORD, span);
    f->as.sym = sym;
    return f;
}

static Form *form_seq(Arena *a, FormTag tag, Span span, Form **items, uint32_t len) {
    Form *f = form_new(a, tag, span);
    if (len) {
        Form **copy = (Form **)arena_alloc(a, sizeof(Form *) * len);
        memcpy(copy, items, sizeof(Form *) * len);
        f->as.list.items = copy;
    } else {
        f->as.list.items = NULL;
    }
    f->as.list.len = len;
    return f;
}

Form *form_list(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_LIST, span, items, len);
}

Form *form_vec(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_VEC, span, items, len);
}

Form *form_map(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_MAP, span, items, len);
}

Form *form_set(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_SET, span, items, len);
}

Form *form_cblock(Arena *a, Span span, StrSlice code) {
    Form *f = form_new(a, F_CBLOCK, span);
    /* The code slice points into the source file; we need to copy it */
    f->as.cblock.p = arena_strdup(a, code.p, code.len);
    f->as.cblock.len = code.len;
    return f;
}

/* Phase 6 */
Form *form_quote(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_QUOTE, span);
    /* Allocate a proper list with one item */
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_quasiquote(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_QUASIQUOTE, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_unquote(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_UNQUOTE, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_unquote_splicing(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_UNQUOTE_SPLICING, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_type_ann(Arena *a, Span span, Form *inner) {
    Form *f = form_new(a, F_TYPE_ANN, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = inner;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

/* CT0: Contract type { var : T | pred } — stores items as a list */
Form *form_contract_type(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_CONTRACT_TYPE, span, items, len);
}

/* Return the name of a FormTag as a string */
const char *form_tag_name(FormTag tag) {
    switch (tag) {
        case F_NIL: return "nil";
        case F_BOOL: return "bool";
        case F_INT: return "int";
        case F_FLOAT: return "float";
        case F_STR: return "string";
        case F_SYM: return "symbol";
        case F_KEYWORD: return "keyword";
        case F_LIST: return "list";
        case F_VEC: return "vector";
        case F_MAP: return "map";
        case F_SET: return "set";
        case F_CBLOCK: return "c-block";
        case F_QUOTE: return "quote";
        case F_QUASIQUOTE: return "quasiquote";
        case F_UNQUOTE: return "unquote";
        case F_UNQUOTE_SPLICING: return "unquote-splicing";
        case F_TYPE_ANN: return "type-ann";
        case F_CONTRACT_TYPE: return "contract-type";
        default: return "unknown";
    }
}

static void print_str_escaped(Buf *b, StrSlice s) {
    buf_putc(b, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        char c = s.p[i];
        switch (c) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n"); break;
            case '\t': buf_puts(b, "\\t"); break;
            case '\r': buf_puts(b, "\\r"); break;
            default:
                if ((unsigned char)c < 0x20) {
                    buf_printf(b, "\\x%02x", (unsigned char)c);
                } else {
                    buf_putc(b, c);
                }
        }
    }
    buf_putc(b, '"');
}

void form_print(Buf *b, const Form *f) {
    switch (f->tag) {
        case F_NIL:  buf_puts(b, "nil"); break;
        case F_BOOL: buf_puts(b, f->as.b ? "true" : "false"); break;
        case F_INT:  buf_printf(b, "%lld", (long long)f->as.i); break;
        case F_FLOAT: buf_printf(b, "%g", f->as.f); break;
        case F_STR:  print_str_escaped(b, f->as.s); break;
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
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_VEC:
            buf_putc(b, '[');
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, ']');
            break;
        case F_MAP:
            buf_puts(b, "#{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_SET:
            buf_puts(b, "#s(");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_CBLOCK:
            buf_puts(b, "```c ");
            buf_write(b, f->as.cblock.p, f->as.cblock.len);
            buf_puts(b, "```");
            break;
        /* Phase 6 */
        case F_QUOTE:
            buf_puts(b, "(quote ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_QUASIQUOTE:
            buf_puts(b, "(quasiquote ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_UNQUOTE:
            buf_puts(b, "(unquote ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_UNQUOTE_SPLICING:
            buf_puts(b, "(unquote-splicing ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_TYPE_ANN:
            buf_puts(b, ": ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            break;
        /* CT0: Contract type { var : T | pred } */
        case F_CONTRACT_TYPE:
            buf_puts(b, "{ ");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_puts(b, " }");
            break;
    }
}
