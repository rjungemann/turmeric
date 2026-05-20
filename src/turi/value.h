#ifndef TURI_VALUE_H
#define TURI_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declarations */
typedef struct TuriClosure    TuriClosure;
typedef struct TuriEffectCont TuriEffectCont;  /* defined in eval.c */
typedef struct TuriStruct     TuriStruct;      /* defined in eval.c */
typedef struct TuriThrow      TuriThrow;       /* defined in eval.c */
typedef struct TuriFuture     TuriFuture;      /* defined in turi/fiber.h */
typedef struct TuriEnv        TuriEnv;         /* from env.h */

/* Tagged value type for the eval runtime. */
typedef enum TuriTag {
    TURI_NIL,
    TURI_BOOL,
    TURI_INT,
    TURI_FLOAT,
    TURI_CSTR,
    TURI_CLOSURE,
    TURI_ERROR,          /* parse/runtime error: as_error holds message */
    TURI_EFFECT_CONT,    /* live continuation from handle/perform (Phase S3) */
    TURI_STRUCT,         /* struct instance: TuriStruct* (Phase S4) */
    TURI_THROW,          /* in-flight exception: TuriThrow* (Phase S4) */
    TURI_FUTURE,         /* async future handle: TuriFuture* (Phase S7) */
    TURI_REF,            /* mutable borrow reference: void* to EvalBinding */
} TuriTag;

typedef struct TuriValue {
    TuriTag tag;
    union {
        bool              as_bool;
        int64_t           as_int;
        void             *as_ref;   /* TURI_REF: EvalBinding* in interpreter */
        double            as_float;
        const char       *as_cstr;    /* borrowed or malloc'd string */
        TuriClosure      *as_closure;
        const char       *as_error;   /* malloc'd error message */
        TuriEffectCont   *as_cont;    /* live effect continuation */
        TuriStruct       *as_struct;  /* struct instance */
        TuriThrow        *as_throw;   /* in-flight exception */
        TuriFuture       *as_future;  /* async future handle (Phase S7) */
    };
} TuriValue;

/* Value constructors */
static inline TuriValue turi_nil(void)          { return (TuriValue){.tag = TURI_NIL, .as_int = 0}; }
static inline TuriValue turi_bool(bool b)       { return (TuriValue){TURI_BOOL, .as_bool = b}; }
static inline TuriValue turi_int(int64_t i)     { return (TuriValue){TURI_INT, .as_int = i}; }
static inline TuriValue turi_float(double f)    { return (TuriValue){TURI_FLOAT, .as_float = f}; }
static inline TuriValue turi_cstr(const char *s){ return (TuriValue){TURI_CSTR, .as_cstr = s}; }
static inline TuriValue turi_closure(TuriClosure *cl) {
    return (TuriValue){TURI_CLOSURE, .as_closure = cl};
}
static inline TuriValue turi_effect_cont(TuriEffectCont *c) {
    return (TuriValue){TURI_EFFECT_CONT, .as_cont = c};
}
static inline TuriValue turi_struct_val(TuriStruct *s) {
    return (TuriValue){TURI_STRUCT, .as_struct = s};
}
static inline TuriValue turi_throw_val(TuriThrow *t) {
    return (TuriValue){TURI_THROW, .as_throw = t};
}
static inline bool turi_is_throw(TuriValue v) { return v.tag == TURI_THROW; }

/* Create an error value (message is strdup'd) */
TuriValue turi_error(const char *msg);
TuriValue turi_errorf(const char *fmt, ...);

/* Predicates */
static inline bool turi_is_error(TuriValue v)   { return v.tag == TURI_ERROR; }

/* A value is truthy if it is not nil and not false */
static inline bool turi_is_truthy(TuriValue v) {
    if (v.tag == TURI_NIL)  return false;
    if (v.tag == TURI_BOOL) return v.as_bool;
    return true;
}

/* Accessors (return zero-value if wrong type) */
static inline bool        turi_as_bool(TuriValue v)  { return v.as_bool; }
static inline int64_t     turi_as_int(TuriValue v)   { return v.as_int; }
static inline double      turi_as_float(TuriValue v) { return v.as_float; }
static inline const char *turi_as_cstr(TuriValue v)  { return v.as_cstr; }
static inline const char *turi_error_message(TuriValue v) { return v.as_error; }

/* Print a TuriValue in REPL repr format (e.g. "42", "\"hello\"", "#<fn foo>") */
void turi_print_value(FILE *out, TuriValue v);

/* Phase S7: native function pointer type (for async builtins registered at runtime).
 * Must come after TuriValue and TuriEnv are fully declared. */
typedef TuriValue (*TuriNativeFn)(TuriEnv *env, TuriValue *args, uint32_t n, void *ud);

#endif /* TURI_VALUE_H */
