#include "builtins.h"

#include <string.h>

/* Static table — entries get their `name_sym` filled in by builtins_init.
 * Order matters for lookup: the first matching entry wins, so list more
 * specific entries before broader ones (e.g., println/cstr before any future
 * "any-printable" fallback). */
static BuiltinSpec table_[] = {
    /* Arithmetic — variadic, int. */
    { "+",   NULL, 2, -1, {.kind=TY_INT},  {.kind=TY_INT},  BS_VARIADIC_FOLD, "+" },
    { "-",   NULL, 2, -1, {.kind=TY_INT},  {.kind=TY_INT},  BS_VARIADIC_FOLD, "-" },
    { "*",   NULL, 2, -1, {.kind=TY_INT},  {.kind=TY_INT},  BS_VARIADIC_FOLD, "*" },
    { "/",   NULL, 2,  2, {.kind=TY_INT},  {.kind=TY_INT},  BS_BIN_INFIX,     "/" },
    { "mod", NULL, 2,  2, {.kind=TY_INT},  {.kind=TY_INT},  BS_BIN_INFIX,     "%" },

    /* Comparison — int. */
    { "=",    NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "==" },
    { "<",    NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "<"  },
    { ">",    NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, ">"  },
    { "<=",   NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "<=" },
    { ">=",   NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, ">=" },
    { "not=", NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" },

    /* Comparison — bool. */
    { "=",    NULL, 2, 2, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_BIN_INFIX, "==" },
    { "not=", NULL, 2, 2, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" },

    /* Logical. */
    { "and", NULL, 2, -1, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_AND_SC,      NULL },
    { "or",  NULL, 2, -1, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_OR_SC,       NULL },
    { "not", NULL, 1,  1, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_PREFIX_UNARY, "!" },

    /* println — separate entries per arg type, dispatched on arg type. */
    { "println", NULL, 1, 1, {.kind=TY_INT},  {.kind=TY_NIL}, BS_PRINTLN_INT,  NULL },
    { "println", NULL, 1, 1, {.kind=TY_BOOL}, {.kind=TY_NIL}, BS_PRINTLN_BOOL, NULL },
    { "println", NULL, 1, 1, {.kind=TY_CSTR}, {.kind=TY_NIL}, BS_PRINTLN_CSTR, NULL },
};

#define TABLE_LEN (sizeof(table_) / sizeof(table_[0]))

void builtins_init(SymbolTable *st) {
    for (size_t i = 0; i < TABLE_LEN; i++) {
        StrSlice name = strslice(table_[i].name, (uint32_t)strlen(table_[i].name));
        table_[i].name_sym = symtab_intern(st, name);
    }
}

const BuiltinSpec *builtin_lookup(const Symbol *name, Type first_arg_type,
                                  uint32_t n_args) {
    for (size_t i = 0; i < TABLE_LEN; i++) {
        const BuiltinSpec *s = &table_[i];
        if (s->name_sym != name) continue;
        if ((int)n_args < s->min_arity) continue;
        if (s->max_arity >= 0 && (int)n_args > s->max_arity) continue;
        if (s->arg_type.kind != TY_UNKNOWN && !type_eq(s->arg_type, first_arg_type)) continue;
        return s;
    }
    return NULL;
}

const BuiltinSpec *builtin_first_with_name(const Symbol *name) {
    for (size_t i = 0; i < TABLE_LEN; i++) {
        if (table_[i].name_sym == name) return &table_[i];
    }
    return NULL;
}
