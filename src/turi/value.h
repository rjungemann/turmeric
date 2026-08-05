#ifndef TURI_VALUE_H
#define TURI_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declarations */
typedef struct TuriClosure    TuriClosure;
typedef struct TuriEffectCont TuriEffectCont;  /* defined in eval.c */
typedef struct TuriGen        TuriGen;         /* defined in eval.c (TI2) */
typedef struct TuriHandlerVal TuriHandlerVal;  /* defined in eval.c (TI6) */
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
    TURI_STRUCT_TYPE,    /* struct type descriptor: as_cstr holds the name */
    TURI_GEN,            /* generator instance: TuriGen* (Phase TI2) */
    TURI_HANDLER,        /* first-class handler value: TuriHandlerVal* (TI6) */
    TURI_REJECTION,      /* async-task rejection: as_error holds message.
                          * Distinct from TURI_ERROR so the universal
                          * turi_is_error short-circuit does NOT propagate
                          * a rejected future value out of `(let [r (await
                          * ...)] ...)` -- callers observe it via (error?
                          * r) / (error-message r) and keep evaluating.
                          * (DEPR-R0; throw-deprecation-plan) */
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
        TuriGen          *as_gen;     /* generator instance (Phase TI2) */
        TuriHandlerVal   *as_handler; /* first-class handler value (TI6) */
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
static inline TuriValue turi_struct_type_val(const char *name) {
    return (TuriValue){TURI_STRUCT_TYPE, .as_cstr = name};
}
static inline TuriValue turi_gen_val(TuriGen *g) {
    return (TuriValue){TURI_GEN, .as_gen = g};
}
static inline TuriValue turi_handler_val(TuriHandlerVal *h) {
    return (TuriValue){TURI_HANDLER, .as_handler = h};
}
static inline bool turi_is_throw(TuriValue v) { return v.tag == TURI_THROW; }

/* ---------------------------------------------------------------------------
 * Env-owned value-payload pool (turi-env-owned-value-arena-pool-plan)
 *
 * All heap payloads that *escape* an eval -- closures, structs, captured
 * frames/bindings, cons cells, boxes, ... -- are bump-allocated from the env's
 * value pool instead of raw libc, so turi_env_free reclaims them en masse.
 * Pool memory is NEVER individually freed: do not pass a turi_val_* pointer to
 * free().  Transients that are explicitly freed within one C call keep using
 * malloc/free.
 *
 * turi-value-pool-scratch-promotion-plan splits the pool into a scratch region
 * (the default target here) and a permanent region (turi_val_perm_*).  With
 * scratch promotion disabled -- the default -- scratch is never rewound, so this
 * is a transparent rename of the old single value_arena.
 * --------------------------------------------------------------------------- */

/* Bump-allocate n uninitialised bytes from env's scratch value pool. */
void *turi_val_alloc(TuriEnv *env, size_t n);
/* Bump-allocate n zeroed bytes from env's scratch value pool. */
void *turi_val_calloc(TuriEnv *env, size_t n);
/* Bump-allocate n zeroed bytes from env's scratch value pool, aligned to
 * `align` (power of two).  For payloads whose leading member has a stricter
 * alignment than the default pointer size -- e.g. a TuriFiber leading with a
 * ucontext_t that requires 16-byte alignment. */
void *turi_val_calloc_aligned(TuriEnv *env, size_t n, size_t align);
/* Copy s into env's scratch value pool (returns NULL when s is NULL). */
char *turi_val_strdup(TuriEnv *env, const char *s);

/* turi-value-pool-scratch-promotion-plan: allocate into env's PERMANENT value
 * pool instead of scratch.  Reserved for the promotion walk, which deep-copies
 * escaping values here so they survive the scratch rewind.  Ordinary eval code
 * never calls these -- it allocates from scratch and lets promotion relocate
 * whatever escapes. */
void *turi_val_perm_alloc(TuriEnv *env, size_t n);
void *turi_val_perm_calloc(TuriEnv *env, size_t n);
char *turi_val_perm_strdup(TuriEnv *env, const char *s);

/* Env-less fallback pool for the error/rejection constructors below, whose
 * signatures carry no env.  Backed by a process-global arena that turi_env_free
 * adopts and reclaims (the single-env embedding pattern the plan targets). */
char *turi_val_strdup_global(const char *s);
/* Reclaim the process-global fallback pool.  Called from turi_env_free. */
void  turi_val_global_pool_free(void);

/* Create an error value (message is strdup'd) */
TuriValue turi_error(const char *msg);
TuriValue turi_errorf(const char *fmt, ...);

/* Create a rejection value (message is strdup'd).  Distinct from
 * turi_error so the interpreter does not short-circuit through it. */
TuriValue turi_rejection(const char *msg);

/* Predicates */
static inline bool turi_is_error(TuriValue v)   { return v.tag == TURI_ERROR; }
static inline bool turi_is_rejection(TuriValue v) { return v.tag == TURI_REJECTION; }

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
