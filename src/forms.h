#ifndef TUR_FORMS_H
#define TUR_FORMS_H

#include <stdint.h>
#include <stdbool.h>

#include "arena.h"
#include "symbols.h"
#include "buf.h"

typedef struct Span {
    uint16_t file_id;
    uint32_t line;       /* 1-based */
    uint32_t col_start;  /* 1-based, inclusive */
    uint32_t col_end;    /* 1-based, exclusive */
    uint32_t off_start;  /* byte offset, inclusive */
    uint32_t off_end;    /* byte offset, exclusive */
} Span;

typedef enum FormTag {
    F_NIL  = 0,
    F_BOOL,
    F_INT,
    F_STR,
    F_SYM,
    F_KEYWORD,    /* :foo */
    F_LIST,       /* (a b c) */
    F_VEC,        /* [a b c]  — same payload as F_LIST */
} FormTag;

struct Form;
typedef struct Form Form;

typedef struct FormList {
    Form    **items;
    uint32_t  len;
} FormList;

struct Form {
    FormTag tag;
    Span    span;
    union {
        bool          b;
        int64_t       i;
        StrSlice      s;        /* string literal contents (escapes resolved) */
        const Symbol *sym;
        FormList      list;
    } as;
};

Form *form_new(Arena *a, FormTag tag, Span span);
Form *form_nil    (Arena *a, Span span);
Form *form_bool   (Arena *a, Span span, bool b);
Form *form_int    (Arena *a, Span span, int64_t i);
Form *form_str    (Arena *a, Span span, const char *p, uint32_t len);
Form *form_sym    (Arena *a, Span span, const Symbol *sym);
Form *form_keyword(Arena *a, Span span, const Symbol *sym);
Form *form_list   (Arena *a, Span span, Form **items, uint32_t len);
Form *form_vec    (Arena *a, Span span, Form **items, uint32_t len);

void  form_print(Buf *b, const Form *f);

#endif
