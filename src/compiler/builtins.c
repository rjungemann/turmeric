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
    { "/",   NULL, 2,  2, {.kind=TY_INT},  {.kind=TY_INT},  BS_DIV_CHECK,    "/" },
    { "mod", NULL, 2,  2, {.kind=TY_INT},  {.kind=TY_INT},  BS_BIN_INFIX,     "%" },

    /* Arithmetic — float. */
    { "+",   NULL, 2, -1, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_VARIADIC_FOLD, "+" },
    { "-",   NULL, 2, -1, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_VARIADIC_FOLD, "-" },
    { "*",   NULL, 2, -1, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_VARIADIC_FOLD, "*" },
    { "/",   NULL, 2,  2, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_DIV_CHECK,     "/" },
    { "+.",  NULL, 2, -1, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_VARIADIC_FOLD, "+" },
    { "-.",  NULL, 2, -1, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_VARIADIC_FOLD, "-" },
    { "*.",  NULL, 2, -1, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_VARIADIC_FOLD, "*" },
    { "/.",  NULL, 2,  2, {.kind=TY_FLOAT}, {.kind=TY_FLOAT}, BS_DIV_CHECK,     "/" },

    /* Comparison — int. */
    { "=",    NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "==" },
    { "<",    NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "<"  },
    { ">",    NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, ">"  },
    { "<=",   NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "<=" },
    { ">=",   NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, ">=" },
    { "not=", NULL, 2, 2, {.kind=TY_INT},  {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" },

    /* Comparison — float. */
    { "=",    NULL, 2, 2, {.kind=TY_FLOAT}, {.kind=TY_BOOL}, BS_BIN_INFIX, "==" },
    { "<",    NULL, 2, 2, {.kind=TY_FLOAT}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<"  },
    { ">",    NULL, 2, 2, {.kind=TY_FLOAT}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">"  },
    { "<=",   NULL, 2, 2, {.kind=TY_FLOAT}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<=" },
    { ">=",   NULL, 2, 2, {.kind=TY_FLOAT}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">=" },
    { "not=", NULL, 2, 2, {.kind=TY_FLOAT}, {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" },

    /* Comparison — bool. */
    { "=",    NULL, 2, 2, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_BIN_INFIX, "==" },
    { "not=", NULL, 2, 2, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" },

    /* Logical. */
    { "and", NULL, 2, -1, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_AND_SC,      NULL },
    { "or",  NULL, 2, -1, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_OR_SC,       NULL },
    { "not", NULL, 1,  1, {.kind=TY_BOOL}, {.kind=TY_BOOL}, BS_PREFIX_UNARY, "!" },

    /* Phase N: Arithmetic for signed integer types int8/int16/int32. */
#define SINT_OPS(TK) \
    { "+",   NULL, 2, -1, {.kind=TK}, {.kind=TK}, BS_VARIADIC_FOLD, "+" }, \
    { "-",   NULL, 2, -1, {.kind=TK}, {.kind=TK}, BS_VARIADIC_FOLD, "-" }, \
    { "*",   NULL, 2, -1, {.kind=TK}, {.kind=TK}, BS_VARIADIC_FOLD, "*" }, \
    { "/",   NULL, 2,  2, {.kind=TK}, {.kind=TK}, BS_DIV_CHECK,    "/" }, \
    { "mod", NULL, 2,  2, {.kind=TK}, {.kind=TK}, BS_BIN_INFIX,    "%" }, \
    { "=",    NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "==" }, \
    { "<",    NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<"  }, \
    { ">",    NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">"  }, \
    { "<=",   NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<=" }, \
    { ">=",   NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">=" }, \
    { "not=", NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" }
    SINT_OPS(TY_INT8),
    SINT_OPS(TY_INT16),
    SINT_OPS(TY_INT32),
#undef SINT_OPS

    /* Phase N: Arithmetic for unsigned integer types. */
#define UINT_OPS(TK) \
    { "+",   NULL, 2, -1, {.kind=TK}, {.kind=TK}, BS_VARIADIC_FOLD, "+" }, \
    { "-",   NULL, 2, -1, {.kind=TK}, {.kind=TK}, BS_VARIADIC_FOLD, "-" }, \
    { "*",   NULL, 2, -1, {.kind=TK}, {.kind=TK}, BS_VARIADIC_FOLD, "*" }, \
    { "/",   NULL, 2,  2, {.kind=TK}, {.kind=TK}, BS_DIV_CHECK,    "/" }, \
    { "mod", NULL, 2,  2, {.kind=TK}, {.kind=TK}, BS_BIN_INFIX,    "%" }, \
    { "=",    NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "==" }, \
    { "<",    NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<"  }, \
    { ">",    NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">"  }, \
    { "<=",   NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<=" }, \
    { ">=",   NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">=" }, \
    { "not=", NULL, 2, 2, {.kind=TK}, {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" }
    UINT_OPS(TY_UINT8),
    UINT_OPS(TY_UINT16),
    UINT_OPS(TY_UINT32),
    UINT_OPS(TY_UINT64),
#undef UINT_OPS

    /* Phase C: Bitwise operations for all integer types (kind-preserving). */
#define BITWISE_OPS(TK) \
    { "bit-and", NULL, 2, 2, {.kind=TK}, {.kind=TK}, BS_BIN_INFIX, "&"  }, \
    { "bit-or",  NULL, 2, 2, {.kind=TK}, {.kind=TK}, BS_BIN_INFIX, "|"  }, \
    { "bit-xor", NULL, 2, 2, {.kind=TK}, {.kind=TK}, BS_BIN_INFIX, "^"  }, \
    { "bit-shl", NULL, 2, 2, {.kind=TK}, {.kind=TK}, BS_BIN_INFIX, "<<" }, \
    { "bit-shr", NULL, 2, 2, {.kind=TK}, {.kind=TK}, BS_BIN_INFIX, ">>" }
    BITWISE_OPS(TY_INT),
    BITWISE_OPS(TY_INT8),
    BITWISE_OPS(TY_INT16),
    BITWISE_OPS(TY_INT32),
    BITWISE_OPS(TY_INT64),
    BITWISE_OPS(TY_UINT8),
    BITWISE_OPS(TY_UINT16),
    BITWISE_OPS(TY_UINT32),
    BITWISE_OPS(TY_UINT64),
#undef BITWISE_OPS

    /* Phase N: Arithmetic for float32. */
    { "+",   NULL, 2, -1, {.kind=TY_FLOAT32}, {.kind=TY_FLOAT32}, BS_VARIADIC_FOLD, "+" },
    { "-",   NULL, 2, -1, {.kind=TY_FLOAT32}, {.kind=TY_FLOAT32}, BS_VARIADIC_FOLD, "-" },
    { "*",   NULL, 2, -1, {.kind=TY_FLOAT32}, {.kind=TY_FLOAT32}, BS_VARIADIC_FOLD, "*" },
    { "/",   NULL, 2,  2, {.kind=TY_FLOAT32}, {.kind=TY_FLOAT32}, BS_DIV_CHECK,    "/" },
    { "=",    NULL, 2, 2, {.kind=TY_FLOAT32}, {.kind=TY_BOOL}, BS_BIN_INFIX, "==" },
    { "<",    NULL, 2, 2, {.kind=TY_FLOAT32}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<"  },
    { ">",    NULL, 2, 2, {.kind=TY_FLOAT32}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">"  },
    { "<=",   NULL, 2, 2, {.kind=TY_FLOAT32}, {.kind=TY_BOOL}, BS_BIN_INFIX, "<=" },
    { ">=",   NULL, 2, 2, {.kind=TY_FLOAT32}, {.kind=TY_BOOL}, BS_BIN_INFIX, ">=" },
    { "not=", NULL, 2, 2, {.kind=TY_FLOAT32}, {.kind=TY_BOOL}, BS_BIN_INFIX, "!=" },

    /* println — separate entries per arg type, dispatched on arg type. */
    { "println", NULL, 1, 1, {.kind=TY_INT},  {.kind=TY_NIL}, BS_PRINTLN_INT,  NULL },
    { "println", NULL, 1, 1, {.kind=TY_FLOAT}, {.kind=TY_NIL}, BS_PRINTLN_FLOAT, NULL },
    { "println", NULL, 1, 1, {.kind=TY_BOOL}, {.kind=TY_NIL}, BS_PRINTLN_BOOL, NULL },
    { "println", NULL, 1, 1, {.kind=TY_CSTR}, {.kind=TY_NIL}, BS_PRINTLN_CSTR, NULL },
    /* Phase N: fixed-width numeric println — signed reuse BS_PRINTLN_INT, unsigned BS_PRINTLN_UINT */
    { "println", NULL, 1, 1, {.kind=TY_INT8},   {.kind=TY_NIL}, BS_PRINTLN_INT,     NULL },
    { "println", NULL, 1, 1, {.kind=TY_INT16},  {.kind=TY_NIL}, BS_PRINTLN_INT,     NULL },
    { "println", NULL, 1, 1, {.kind=TY_INT32},  {.kind=TY_NIL}, BS_PRINTLN_INT,     NULL },
    { "println", NULL, 1, 1, {.kind=TY_INT64},  {.kind=TY_NIL}, BS_PRINTLN_INT,     NULL },
    { "println", NULL, 1, 1, {.kind=TY_UINT8},  {.kind=TY_NIL}, BS_PRINTLN_UINT,    NULL },
    { "println", NULL, 1, 1, {.kind=TY_UINT16}, {.kind=TY_NIL}, BS_PRINTLN_UINT,    NULL },
    { "println", NULL, 1, 1, {.kind=TY_UINT32}, {.kind=TY_NIL}, BS_PRINTLN_UINT,    NULL },
    { "println", NULL, 1, 1, {.kind=TY_UINT64}, {.kind=TY_NIL}, BS_PRINTLN_UINT,    NULL },
    { "println", NULL, 1, 1, {.kind=TY_FLOAT32},{.kind=TY_NIL}, BS_PRINTLN_FLOAT32, NULL },
    { "println", NULL, 1, 1, {.kind=TY_FLOAT64},{.kind=TY_NIL}, BS_PRINTLN_FLOAT,   NULL },
    /* Phase 5: ref drop */
    { "drop!", NULL, 1, 1, {.kind=TY_UNKNOWN}, {.kind=TY_NIL}, BS_PREFIX_UNARY_FREE, "free" },

    /* prelude-macros (Defect B / F3): user-callable runtime cons-cell builder.
     * `(cons h t)` allocates a `{int64_t head; int64_t tail;}` cell -- the same
     * layout produced by `tcons` / `__tur_cons_of` and consumed by the
     * `head`/`tail` walkers in stdlib/list.tur -- and returns its pointer as
     * :int.  Registered as a builtin (rather than a stdlib defn) so it resolves
     * in BOTH single-file and project/separate-compilation mode without the
     * stdlib auto-load that project mode skips.  The C helper `cons` is emitted
     * into the preamble of every TU that references it (gated on g_uses_cons).
     * Distinct from the compile-time `cons` form in elab_macros.c, which only
     * fires during macro expansion. */
    /* arg_type=TY_UNKNOWN: the head can be any 64-bit-sized value (int, cstr,
     * opaque handle, pointer) and the tail can be either nil (0) or another
     * cons-cell pointer (int).  Codegen casts both args via (int64_t)(intptr_t)
     * so pointer-typed args don't trip C's "int from pointer" warning. */
    { "cons", NULL, 2, 2, {.kind=TY_UNKNOWN}, {.kind=TY_INT}, BS_FUNC_CALL, "cons" },

    /* Phase 9: rc<T> operations */
    /* (rc/of x) - create a new rc<T> with x as the value */
    { "rc/of", NULL, 1, 1, {.kind=TY_UNKNOWN}, {.kind=TY_RC}, BS_PREFIX_UNARY, NULL },
    /* (rc/clone r) - increment strong count, return new rc<T> */
    { "rc/clone", NULL, 1, 1, {.kind=TY_RC}, {.kind=TY_RC}, BS_PREFIX_UNARY, NULL },
    /* (rc/drop r) - decrement strong count */
    { "rc/drop", NULL, 1, 1, {.kind=TY_RC}, {.kind=TY_NIL}, BS_PREFIX_UNARY_FREE, NULL },
    /* (rc/strong-count r) - get the strong count */
    { "rc/strong-count", NULL, 1, 1, {.kind=TY_RC}, {.kind=TY_INT}, BS_PREFIX_UNARY, NULL },

    /* Phase 9: weak<T> operations */
    /* (weak r) - create a weak<T> from an rc<T> */
    { "weak", NULL, 1, 1, {.kind=TY_RC}, {.kind=TY_WEAK}, BS_PREFIX_UNARY, NULL },
    /* (upgrade w) - upgrade weak<T> to option<rc<T>> */
    { "upgrade", NULL, 1, 1, {.kind=TY_WEAK}, {.kind=TY_UNKNOWN}, BS_PREFIX_UNARY, NULL },
    /* (weak? w) - check if w is a weak<T> */
    { "weak?", NULL, 1, 1, {.kind=TY_UNKNOWN}, {.kind=TY_BOOL}, BS_PREFIX_UNARY, NULL },
    /* Phase U3: Unsafe primitives - pointer operations */
    { "ptr-deref", NULL, 1, 1, {.kind=TY_PTR_VOID}, {.kind=TY_INT}, BS_PTR_DEREF, "*((int64_t *)" },
    { "ptr-null?", NULL, 1, 1, {.kind=TY_PTR_VOID}, {.kind=TY_BOOL}, BS_PREFIX_UNARY, "!" },
    { "ptr-of", NULL, 1, 1, {.kind=TY_UNKNOWN}, {.kind=TY_PTR_VOID}, BS_PREFIX_UNARY, "&" },
    { "ptr-add", NULL, 2, 2, {.kind=TY_PTR_VOID}, {.kind=TY_PTR_VOID}, BS_PTR_ARITH, "+" },
    { "ptr-sub", NULL, 2, 2, {.kind=TY_PTR_VOID}, {.kind=TY_PTR_VOID}, BS_PTR_ARITH, "-" },
    { "ptr-write", NULL, 2, 2, {.kind=TY_PTR_VOID}, {.kind=TY_NIL}, BS_PTR_WRITE, NULL },
    /* Phase U3: Unsafe primitives - type casting */
    { "unsafe-cast", NULL, 2, 2, {.kind=TY_UNKNOWN}, {.kind=TY_UNKNOWN}, BS_UNSAFE_CAST, NULL },
    { "reinterpret", NULL, 2, 2, {.kind=TY_UNKNOWN}, {.kind=TY_UNKNOWN}, BS_REINTERPRET, NULL },
    { "transmute", NULL, 2, 2, {.kind=TY_UNKNOWN}, {.kind=TY_UNKNOWN}, BS_TRANSMUTE, NULL },
    /* Phase U3: Unsafe primitives - unchecked array ops */
    { "array-get-unchecked", NULL, 2, 2, {.kind=TY_PTR_VOID}, {.kind=TY_INT}, BS_ARRAY_GET_UNCHECKED, NULL },
    { "array-set-unchecked", NULL, 3, 3, {.kind=TY_PTR_VOID}, {.kind=TY_NIL}, BS_ARRAY_SET_UNCHECKED, NULL },
    /* Phase U3: Unsafe primitives - raw memory */
    { "raw-malloc", NULL, 1, 1, {.kind=TY_INT}, {.kind=TY_PTR_VOID}, BS_RAW_MALLOC, NULL },
    { "raw-free", NULL, 1, 1, {.kind=TY_PTR_VOID}, {.kind=TY_NIL}, BS_RAW_FREE, NULL },
    { "raw-realloc", NULL, 2, 2, {.kind=TY_PTR_VOID}, {.kind=TY_PTR_VOID}, BS_RAW_REALLOC, NULL },
    { "raw-memcpy", NULL, 3, 3, {.kind=TY_PTR_VOID}, {.kind=TY_NIL}, BS_RAW_MEMCPY, NULL },
    { "raw-memset", NULL, 3, 3, {.kind=TY_PTR_VOID}, {.kind=TY_NIL}, BS_RAW_MEMSET, NULL },
    /* Phase U3: Unsafe primitives - FFI */
    { "dlopen", NULL, 1, 1, {.kind=TY_CSTR}, {.kind=TY_PTR_VOID}, BS_DLOPEN, NULL },
    { "dlsym", NULL, 2, 2, {.kind=TY_PTR_VOID}, {.kind=TY_PTR_VOID}, BS_DLSYM, NULL },
    { "dlclose", NULL, 1, 1, {.kind=TY_PTR_VOID}, {.kind=TY_INT}, BS_DLCLOSE, NULL },

    /* Cloneable continuation operations */
    { "tur_cloneable_cont_resume", NULL, 2, 2, {.kind=TY_INT}, {.kind=TY_INT}, BS_FUNC_CALL, "tur_cloneable_cont_resume" },
    { "tur_cloneable_cont_clone",  NULL, 1, 1, {.kind=TY_INT}, {.kind=TY_INT}, BS_FUNC_CALL, "tur_cloneable_cont_clone" },
    { "tur_cloneable_cont_drop",   NULL, 1, 1, {.kind=TY_INT}, {.kind=TY_NIL}, BS_FUNC_CALL, "tur_cloneable_cont_drop" },
    /* MS0: Snapshot a cloneable continuation for independent resumption */
    { "tur_continuation_snapshot", NULL, 1, 1, {.kind=TY_INT}, {.kind=TY_INT}, BS_FUNC_CALL, "tur_continuation_snapshot" },
    /* call-cc-completion: resume an undelimited escape continuation captured by
     * (call/cc f)/(escape f).  k is the int64_t landing handle f received;
     * invoking it returns v at the call/cc site (one-shot, upward escape). */
    { "tur_escape_resume", NULL, 2, 2, {.kind=TY_INT}, {.kind=TY_INT}, BS_FUNC_CALL, "tur_escape_resume" },
    /* cps-transform-plan (CPS10 / CPS5.4): serializable continuations captured
     * by (serial-shift f v) on the DK machine.  The handle f receives is a DK
     * chain (int64); resume runs it, serialize marshals it to a length-prefixed
     * buffer (tag + env per frame), deserialize rebuilds a runnable chain. */
    { "tur_serial_cont_resume",      NULL, 2, 2, {.kind=TY_INT}, {.kind=TY_INT}, BS_FUNC_CALL, "tur_serial_cont_resume" },
    { "tur_serial_cont_serialize",   NULL, 1, 1, {.kind=TY_INT}, {.kind=TY_INT}, BS_FUNC_CALL, "tur_serial_cont_serialize" },
    { "tur_serial_cont_deserialize", NULL, 1, 1, {.kind=TY_INT}, {.kind=TY_INT}, BS_FUNC_CALL, "tur_serial_cont_deserialize" },
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

bool builtin_div_is_ieee(const BuiltinSpec *spec) {
    if (!spec || spec->shape != BS_DIV_CHECK) return false;
    return spec->arg_type.kind == TY_FLOAT || spec->arg_type.kind == TY_FLOAT32;
}

uint32_t builtin_collect_with_name(const Symbol *name,
                                   const BuiltinSpec **out,
                                   uint32_t max_out) {
    if (!out || max_out == 0) return 0;
    uint32_t n = 0;
    for (size_t i = 0; i < TABLE_LEN; i++) {
        if (table_[i].name_sym != name) continue;
        if (n < max_out) {
            out[n++] = &table_[i];
        } else {
            break;
        }
    }
    return n;
}
