#ifndef TUR_BUILTINS_H
#define TUR_BUILTINS_H

#include "expr.h"
#include "symbols.h"
#include "types.h"

/* The operator dispatch table per §1.1 — single source of truth for primitive
 * operator codegen. Typeclasses (§12.2) will extend this table; phase 1
 * populates it statically. */

typedef enum BuiltinShape {
    BS_BIN_INFIX,        /* "(a) <op> (b)" */
    BS_VARIADIC_FOLD,    /* "((a) <op> (b)) <op> (c) ..." (left-fold) */
    BS_PREFIX_UNARY,     /* "<op>(a)" */
    BS_PREFIX_UNARY_FREE,/* "free(a)" - special case for ref drop */
    BS_PRINTLN_INT,      /* printf("%lld\n", x)         */
    BS_PRINTLN_FLOAT,    /* printf("%.15g\n", x)       */
    BS_PRINTLN_BOOL,     /* puts(x ? "true" : "false")  */
    BS_PRINTLN_CSTR,     /* puts(x)                     */
    BS_DIV_CHECK,        /* Division with zero check: (a / b) with runtime check */
    BS_AND_SC,           /* short-circuit (handled in codegen)   */
    BS_OR_SC,            /* short-circuit (handled in codegen)   */
    /* Phase U3: Unsafe primitives */
    BS_PTR_WRITE,        /* *ptr = value - emit as statement, return nil */
    BS_PTR_ARITH,        /* ptr arithmetic: (char *)ptr op offset */
    BS_PTR_DEREF,        /* ptr dereference: *((T *)ptr) */
    BS_UNSAFE_CAST,      /* unsafe-cast: C-style cast */
    BS_REINTERPRET,      /* reinterpret: bitwise reinterpretation */
    BS_TRANSMUTE,        /* transmute: type-punning with size check */
    BS_ARRAY_GET_UNCHECKED,  /* array-get-unchecked: *(ptr + index) */
    BS_ARRAY_SET_UNCHECKED,  /* array-set-unchecked: *(ptr + index) = value */
    BS_RAW_MALLOC,       /* raw-malloc: malloc(size) */
    BS_RAW_FREE,         /* raw-free: free(ptr) */
    BS_RAW_REALLOC,      /* raw-realloc: realloc(ptr, new_size) */
    BS_RAW_MEMCPY,       /* raw-memcpy: memcpy(dest, src, n) */
    BS_RAW_MEMSET,       /* raw-memset: memset(dest, byte, n) */
    /* Phase U3: FFI */
    BS_DLOPEN,           /* dlopen: dlopen(path, RTLD_LAZY) */
    BS_DLSYM,           /* dlsym: dlsym(handle, symbol) */
    BS_DLCLOSE,          /* dlclose: dlclose(handle) */
} BuiltinShape;

struct BuiltinSpec {
    const char    *name;          /* string form, used for registration */
    const Symbol  *name_sym;      /* interned at builtins_init */
    int            min_arity;
    int            max_arity;     /* -1 = variadic */
    Type           arg_type;      /* uniform arg type; TY_UNKNOWN means "any" */
    Type           result_type;
    BuiltinShape   shape;
    const char    *c_op;
};

/* One-time setup. Interns the names of every builtin into `st`. */
void builtins_init(SymbolTable *st);

/* Resolve an operator. Returns NULL if no entry matches. */
const BuiltinSpec *builtin_lookup(const Symbol *name, Type first_arg_type,
                                  uint32_t n_args);

/* For diagnostic messages — returns the first entry with this name,
 * regardless of arity/type. NULL if no entry exists with this name. */
const BuiltinSpec *builtin_first_with_name(const Symbol *name);

/* Collect overloads for a builtin name.
 * Returns number of entries written to `out` (up to max_out). */
uint32_t builtin_collect_with_name(const Symbol *name,
                                   const BuiltinSpec **out,
                                   uint32_t max_out);

#endif
