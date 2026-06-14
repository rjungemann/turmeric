/* Phase S0: Tree-walk evaluator for the Turmeric eval core (libturi).
 *
 * Design:
 *  - Each turi_eval call re-elaborates ALL accumulated source plus the new
 *    source, then walks only the NEW top-level expressions.
 *  - Per-call arenas are kept alive in TuriEnv so that closures can hold
 *    Expr* pointers into them indefinitely.
 *  - Variable lookup uses name strings (const char*), not Binding* addresses,
 *    so lookup survives re-elaboration across calls.
 *  - A "return signal" is propagated via env->returning + env->return_value.
 */

/* Platform macros before any system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
/* ucontext on macOS requires _XOPEN_SOURCE */
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "eval.h"
#include "fiber.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef __EMSCRIPTEN__
#  include <sys/mman.h>
/* ucontext is POSIX and deprecated on macOS but still functional.
 * Suppress the deprecation warning so -Werror doesn't fail the build. */
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
#  include <ucontext.h>
#  if defined(__APPLE__)
#    pragma clang diagnostic pop
#  endif
#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif
/* WASM: Emscripten Fiber shims provided by fiber.h/env.h (included above). */
#else
#  include <emscripten.h>
#endif

/* Pull in all the compiler internal headers from the parent src/ directory.
 * CMake adds src/ to the include path so these resolve correctly. */
#include "arena.h"
#include "buf.h"
#include "builtins.h"
#include "diag.h"
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "reader.h"
#include "symbols.h"
#include "types.h"
#include "../passes/effect_check.h"
#include "../passes/borrow_check.h"

/* T1 (turi-eval-trampoline-plan): small inline arg/field buffer with a heap
 * spill above it.  Keeps the per-call scratch off the C stack for the common
 * low-arity case while bounding deep-recursion frame growth -- the dominant
 * per-frame cost was several TuriValue[MAX_EVAL_ARGS] (1 KB) arrays.  8 covers
 * the overwhelmingly common 0..8-arg call with zero allocation. */
#define EVAL_SCRATCH_INLINE 8

/* T1: language-wide function/effect arity cap (mirrors MAX_FN_ARITY in
 * compiler/types.h).  Used as a fixed inline buffer size where the arity is
 * provably bounded -- closure-application TCO buffers and effect-perform args --
 * so those scratch arrays drop from MAX_EVAL_ARGS (1 KB) to 256 B with no heap
 * spill.  A defensive guard rejects anything above it (unreachable for
 * elaboration-checked programs). */
#define EVAL_MAX_FN_ARITY 16

/* -------------------------------------------------------------------------
 * Internal closure representation
 * The public value.h declares TuriClosure as an opaque struct; here we
 * define it properly.
 * ---------------------------------------------------------------------- */

/* Forward-declared in value.c as having fn/captured void*; we redefine: */
typedef struct EvalFrame EvalFrame;

struct TuriClosure {
    FnDef          *fn;        /* FnDef* in some per-call arena (kept alive) */
    EvalFrame      *captured;  /* captured lexical frame (NULL for top-level defn) */
    /* Phase S7: native function support (fn == NULL when native is set) */
    TuriNativeFn    native;    /* non-NULL for native C builtins */
    void           *native_ud; /* user data passed to native */
    /* EX_CLOSURE closures have a synthetic __env_p first param for codegen;
     * the interpreter skips it and uses the captured frame instead. */
    bool            skip_env_param;
    /* Owning module name (for a top-level defn inside a defmodule), or NULL.
     * eval_apply publishes this as env->current_module while the body runs so
     * intra-module calls can resolve module-private "<module>/<name>" bindings. */
    const char     *module;
};

/* Forward declaration — defined after TuriStruct is fully defined below. */
static TuriValue native_panic_pred(TuriEnv *env, TuriValue *args, uint32_t n, void *ud);

/* workflow.tur save-cont!/resume-cont! native overrides (defined after the
 * context-capturing continuation machinery below).  Registered in
 * turi_eval_register_builtins so they shadow the inline-C defns the moment
 * stdlib/workflow.tur is loaded. */
static TuriValue native_save_cont(TuriEnv *env, TuriValue *args, uint32_t n, void *ud);
static TuriValue native_resume_cont(TuriEnv *env, TuriValue *args, uint32_t n, void *ud);

/* Register a native C function as a global binding in env.
 * Declared in eval.h; implemented here because TuriClosure is internal. */
void turi_env_register_native(TuriEnv *env, const char *name,
                               TuriNativeFn fn, void *ud) {
    TuriClosure *cl = (TuriClosure *)calloc(1, sizeof(TuriClosure));
    cl->fn        = NULL;
    cl->captured  = NULL;
    cl->native    = fn;
    cl->native_ud = ud;
    turi_env_set(env, name, turi_closure(cl));
}

/* Phase TI2: native overrides for stdlib/gen.tur's inline-C helpers, which
 * cannot be interpreted directly.  They operate on the ptr<void> ABI produced
 * by EX_GEN_NEXT: a TURI_INT boxing either NULL (0) or a pointer to an int64_t
 * holding the yielded value. */
static TuriValue native_gen_some(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    return turi_bool(args[0].as_int != 0);
}
static TuriValue native_gen_unwrap(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || args[0].as_int == 0) return turi_int(0);
    return turi_int(*(int64_t *)(intptr_t)args[0].as_int);
}
static TuriValue native_gen_none(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_int(0);
}

/* Register eval-layer native builtins (struct-aware predicates, etc.).
 * Called from turi_env_new after async builtins are registered. */
void turi_eval_register_builtins(TuriEnv *env) {
    turi_env_register_native(env, "panic?", native_panic_pred, NULL);
    /* workflow.tur serial-continuation wrappers (inline-C in stdlib; the
     * interpreter cannot run the inline-C body, so override with natives over
     * the cont machinery). */
    turi_env_register_native(env, "save-cont!",   native_save_cont,   NULL);
    turi_env_register_native(env, "resume-cont!", native_resume_cont, NULL);
    /* TI2: generator ptr<void> helpers (override gen.tur inline-C). */
    turi_env_register_native(env, "gen-some?",  native_gen_some,   NULL);
    turi_env_register_native(env, "gen-unwrap", native_gen_unwrap, NULL);
    turi_env_register_native(env, "gen-none",   native_gen_none,   NULL);
}

/* -------------------------------------------------------------------------
 * Phase S7: Async fiber thunk
 * ---------------------------------------------------------------------- */

_Thread_local TuriFiber *g_pending_async_fiber;

/* Forward declarations needed by the thunk. */
static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e);
static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args);

#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
static void async_fiber_thunk(void) {
    TuriFiber *fiber = g_pending_async_fiber;
    TuriEnv   *env   = fiber->env;

    /* Call the pre-evaluated closure with no arguments. */
    TuriValue result = eval_apply(env, fiber->fn_closure_val.as_closure, NULL, 0);

    /* Settle the fiber's own future. */
    if (fiber->cancelled) {
        turi_future_reject(env, fiber->own_future,
                           turi_error("task cancelled"));
    } else if (env->throwing) {
        TuriValue err = env->throw_value;
        env->throwing = false;
        turi_future_reject(env, fiber->own_future, err);
    } else if (turi_is_error(result)) {
        turi_future_reject(env, fiber->own_future, result);
    } else {
        if (env->returning) {
            result = env->return_value;
            env->returning = false;
        }
        turi_future_resolve(env, fiber->own_future, result);
    }

    fiber->state = TURI_FIBER_DONE;
    swapcontext(&fiber->ctx, &env->sched_ctx);
    abort(); /* unreachable */
}
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

/* -------------------------------------------------------------------------
 * Nil stub for extern-c functions: returns nil in interpreter mode.
 * ---------------------------------------------------------------------- */
static TuriValue native_nil_stub(TuriEnv *env, TuriValue *args, uint32_t n,
                                  void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_nil();
}

/* Native stubs for well-known libc functions declared via extern-c. */
static TuriValue native_extern_exit(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int code = (n > 0) ? (int)args[0].as_int : 0;
    fflush(NULL);
    _exit(code);
}
static TuriValue native_extern_free(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0) { void *p = (void *)(intptr_t)args[0].as_int; if (p) free(p); }
    return turi_nil();
}
static TuriValue native_extern_strlen(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && args[0].tag == TURI_CSTR && args[0].as_cstr)
        return turi_int((int64_t)strlen(args[0].as_cstr));
    return turi_int(0);
}
static TuriValue native_extern_getenv(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && args[0].tag == TURI_CSTR && args[0].as_cstr) {
        const char *v = getenv(args[0].as_cstr);
        return v ? turi_cstr(v) : turi_nil();
    }
    return turi_nil();
}

/* printf: supports one argument (%lld for int, %s for cstr, %f/%g for float). */
static TuriValue native_extern_printf(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || args[0].tag != TURI_CSTR || !args[0].as_cstr) return turi_int(0);
    const char *fmt = args[0].as_cstr;
    int ret = 0;
    if (n >= 2) {
        TuriValue arg = args[1];
        if (arg.tag == TURI_CSTR)
            ret = printf(fmt, arg.as_cstr);
        else if (arg.tag == TURI_FLOAT)
            ret = printf(fmt, arg.as_float);
        else
            ret = printf(fmt, (long long)arg.as_int);
    } else {
        ret = printf("%s", fmt);
    }
    return turi_int((int64_t)ret);
}

/* puts: write the cstr followed by a newline (libc semantics). */
static TuriValue native_extern_puts(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && args[0].tag == TURI_CSTR && args[0].as_cstr)
        return turi_int((int64_t)puts(args[0].as_cstr));
    return turi_int(0);
}

static void register_extern_c_known(TuriEnv *env, const char *fname) {
    struct { const char *name; TuriNativeFn fn; } known[] = {
        { "exit",     native_extern_exit     },
        { "free",     native_extern_free     },
        { "strlen",   native_extern_strlen   },
        { "getenv",   native_extern_getenv   },
        { "printf",   native_extern_printf   },
        { "printf_s", native_extern_printf   },
        { "puts",     native_extern_puts     },
        { NULL, NULL }
    };
    for (int i = 0; known[i].name; i++) {
        if (strcmp(fname, known[i].name) == 0) {
            turi_env_register_native(env, fname, known[i].fn, NULL);
            return;
        }
    }
    turi_env_register_native(env, fname, native_nil_stub, NULL);
}

/* -------------------------------------------------------------------------
 * Local variable frame (stack-allocated linked list)
 * ---------------------------------------------------------------------- */

typedef struct EvalBinding {
    const char       *name;   /* points into sym_arena — never freed */
    TuriValue         value;
    struct EvalBinding *next;
} EvalBinding;

struct EvalFrame {
    EvalBinding  *bindings;
    EvalFrame    *parent;
};

static EvalFrame *eval_frame_new(EvalFrame *parent) {
    EvalFrame *f = (EvalFrame *)malloc(sizeof(EvalFrame));
    f->bindings = NULL;
    f->parent   = parent;
    return f;
}

static void eval_frame_free(EvalFrame *f) {
    /* Frames are intentionally not freed: closures may capture frame pointers
     * and outlive the scope that created them.  Worker processes are short-lived
     * (one fixture per fork), so leaking frames is acceptable. */
    (void)f;
}

static void frame_bind(EvalFrame *f, const char *name, TuriValue value) {
    EvalBinding *b = (EvalBinding *)malloc(sizeof(EvalBinding));
    b->name  = name;
    b->value = value;
    b->next  = f->bindings;
    f->bindings = b;
}

/* Returns true and updates the value if the name is found in the frame chain. */
static bool eval_frame_update(EvalFrame *f, const char *name, TuriValue value) {
    for (EvalFrame *cur = f; cur; cur = cur->parent) {
        for (EvalBinding *b = cur->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                b->value = value;
                return true;
            }
        }
    }
    return false;
}

static TuriValue eval_lookup(TuriEnv *env, EvalFrame *frame, const char *name) {
    for (EvalFrame *f = frame; f; f = f->parent) {
        for (EvalBinding *b = f->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) return b->value;
        }
    }
    /* Module-private resolution: a defn whose body is running inside module M
     * can see M's private (non-exported) helpers, which are registered under the
     * qualified key "M/name". Probe that before the flat global name so two
     * modules' same-named privates do not collide. */
    if (env->current_module && env->current_module[0]) {
        char keybuf[256];
        int kn = snprintf(keybuf, sizeof keybuf, "%s/%s", env->current_module, name);
        if (kn > 0 && kn < (int)sizeof keybuf) {
            TuriValue qv = turi_env_get(env, keybuf);
            if (qv.tag != TURI_ERROR) return qv;
        }
    }
    return turi_env_get(env, name);
}

/* Reword an unbound call-head error into the compiler's "unknown function or
 * operator" diagnostic (plus the stdlib load-hint).  In interpret mode the
 * elaborator defers an unknown call head to runtime dispatch (elab_call.c
 * UCH1) so runtime-registered natives can resolve it; when the head is
 * genuinely unbound, the generic EX_VAR lookup yields a bare "unbound
 * variable: NAME".  This rewrites that to match the compiled path's wording so
 * `tur --interpret` reports the same diagnostic.  Only an EX_VAR head can be an
 * unresolved call head, and its sole error path is the unbound case, so the
 * rewrite never masks an unrelated failure. */
static TuriValue reword_unbound_call_head(TuriValue fn_val, const Expr *fn_expr) {
    if (!turi_is_error(fn_val) || !fn_expr || fn_expr->kind != EX_VAR)
        return fn_val;
    const char *nm   = fn_expr->as.var.binding->name->name;
    const char *hint = tur_stdlib_load_hint(nm);
    if (hint)
        return turi_errorf(
            "unknown function or operator '%s'\n"
            "'%s' lives in %s and is not auto-loaded\n"
            "(load \"%s\")", nm, nm, hint, hint);
    return turi_errorf("unknown function or operator '%s'", nm);
}

/* vec/carrier closure readback.  A closure stored into an int64-carrier
 * container (Vec via vec-push!, slice, ...) is held as the raw TuriClosure*
 * bits, and vec-get reads it back as a bare TURI_INT (the carrier never
 * preserved the TURI_CLOSURE tag).  When that carrier is then *called* through
 * a binding the elaborator declared `^fat` or function-typed -- the canonical
 * `(call1 (:: (vec-get v i) :ptr<void>) x)` idiom -- recover the closure tag so
 * the call finds a callable instead of erroring "expected function, got tag 2".
 * The static type guards the reinterpret: it only fires when the call head's
 * binding is fat / TY_FN / TY_PTR_VOID, i.e. a context where a bare int *is* a
 * closure carrier (closures are heap-allocated and process-lifetime under the
 * interpreter, so the recovered pointer stays valid). */
static TuriValue recover_carrier_closure(TuriValue fn_val, const Binding *b) {
    if (fn_val.tag == TURI_INT && b && fn_val.as_int != 0 &&
        (b->is_fat || b->type.kind == TY_FN || b->type.kind == TY_PTR_VOID)) {
        TuriValue r = {0};
        r.tag        = TURI_CLOSURE;
        r.as_closure = (TuriClosure *)(intptr_t)fn_val.as_int;
        return r;
    }
    return fn_val;
}

/* Early forward declaration (needed by fire_defers_to_mark below) */
static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e);

/* -------------------------------------------------------------------------
 * Phase S4: Struct, throw, and defer runtime types
 * ---------------------------------------------------------------------- */

/* Full definition of TuriStruct (forward-declared in value.h).
 * Fields are stored in order matching StructDef->fields[]. */
struct TuriStruct {
    const char  *name;     /* struct name (for debugging) */
    uint32_t     n_fields;
    TuriValue   *fields;   /* heap-allocated array */
    StructDef   *def;      /* compiler's struct definition (for field name lookup); may be NULL */
};

static TuriValue make_struct_val_def(const char *name, uint32_t n, TuriValue *fields, StructDef *def) {
    TuriStruct *s = (TuriStruct *)malloc(sizeof(TuriStruct));
    s->name     = name;
    s->n_fields = n;
    s->def      = def;
    s->fields   = (TuriValue *)malloc(n * sizeof(TuriValue));
    for (uint32_t i = 0; i < n; i++) s->fields[i] = fields[i];
    return turi_struct_val(s);
}

/* W1b: see eval.h.  Read field idx of a struct value (TuriStruct is opaque to
 * main.c, so the Result/Option native shims call this to accept a make-struct
 * value as well as their native int64 box). */
TuriValue turi_struct_field(TuriValue v, uint32_t idx, bool *found) {
    if (v.tag == TURI_STRUCT && v.as_struct && idx < v.as_struct->n_fields) {
        if (found) *found = true;
        return v.as_struct->fields[idx];
    }
    if (found) *found = false;
    return turi_nil();
}

const char *turi_struct_name(TuriValue v) {
    if (v.tag == TURI_STRUCT && v.as_struct) return v.as_struct->name;
    return NULL;
}

TuriValue turi_make_struct(const char *name, TuriValue *fields, uint32_t n) {
    return make_struct_val_def(name, n, fields, NULL);
}

static TuriValue make_struct_val(const char *name, uint32_t n, TuriValue *fields) {
    return make_struct_val_def(name, n, fields, NULL);
}

/* panic? : (val) -> bool — true if val is the (panic) struct from catch-unwind */
static TuriValue native_panic_pred(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n != 1) return turi_bool(false);
    TuriValue v = args[0];
    if (v.tag != TURI_STRUCT || !v.as_struct || !v.as_struct->name) return turi_bool(false);
    return turi_bool(strcmp(v.as_struct->name, "panic") == 0);
}

/* Native callback for ADT constructors registered by EX_DEFDATA/EX_DEFGADT. */
static TuriValue adt_ctor_native(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env;
    CtorDef *ctor = (CtorDef *)ud;
    return make_struct_val(ctor->name, n, args);
}

/* Full definition of TuriThrow (forward-declared in value.h). */
struct TuriThrow {
    TuriValue   value;      /* the thrown Turmeric value */
    TypeKind    type_kind;  /* type hint for typed catch clauses */
};

static TuriValue make_throw_val(TuriValue v, TypeKind tk) {
    TuriThrow *t = (TuriThrow *)malloc(sizeof(TuriThrow));
    t->value     = v;
    t->type_kind = tk;
    return turi_throw_val(t);
}

/* Throw a catchable exception from a native (TuriNativeFn) function.
 * The native should return turi_nil() immediately after calling this. */
void turi_native_throw(TuriEnv *env, const char *msg) {
    TuriValue tv  = make_throw_val(turi_cstr(msg), TY_UNKNOWN);
    env->throwing    = true;
    env->throw_value = tv;
}

/* Defer item: body expression + snapshot frame of captured values. */
typedef struct DeferItem {
    Expr             *body;
    EvalFrame        *snapshot;   /* captured variable values at defer-call time */
    struct DeferItem *next;
} DeferItem;

/* Execute all defers pushed above mark (LIFO) and free them.
 * Errors inside defers are silently discarded (like in most languages). */
static void fire_defers_to_mark(TuriEnv *env, DeferItem *mark,
                                  EvalFrame *fallback_frame) {
    bool saved_throwing  = env->throwing;
    TuriValue saved_tv   = env->throw_value;
    bool saved_returning = env->returning;
    TuriValue saved_rv   = env->return_value;

    while (env->defer_stack != mark) {
        DeferItem *item = (DeferItem *)env->defer_stack;
        env->defer_stack = item->next;

        /* Reset signals so defer body runs cleanly. */
        env->throwing  = false;
        env->returning = false;

        /* Evaluate with snapshot frame; fall back to fallback for globals. */
        EvalFrame *dframe = item->snapshot;
        if (dframe) dframe->parent = fallback_frame;
        eval_expr(env, dframe, item->body);

        eval_frame_free(item->snapshot);
        free(item);
    }

    /* Restore signals (caller decides what to do with them). */
    env->throwing     = saved_throwing;
    env->throw_value  = saved_tv;
    env->returning    = saved_returning;
    env->return_value = saved_rv;
}

/* Fire defers in reversed (oldest-first / outer-first) order.
 * Used at function exit to match compiled tur_frame_fire_chain semantics:
 * on early-return, outer defers fire before inner defers. */
static void fire_defers_to_mark_reversed(TuriEnv *env, DeferItem *mark,
                                          EvalFrame *fallback_frame) {
    /* Count items */
    size_t n = 0;
    for (DeferItem *it = (DeferItem *)env->defer_stack; it != mark; it = it->next) n++;
    if (n == 0) return;

    /* Collect into array (index 0 = newest / innermost) */
    DeferItem **items = (DeferItem **)malloc(n * sizeof(DeferItem *));
    DeferItem *cur = (DeferItem *)env->defer_stack;
    for (size_t i = 0; i < n; i++) { items[i] = cur; cur = cur->next; }
    env->defer_stack = mark;

    bool saved_throwing  = env->throwing;
    TuriValue saved_tv   = env->throw_value;
    bool saved_returning = env->returning;
    TuriValue saved_rv   = env->return_value;

    /* Fire reversed: oldest (outermost) first */
    for (size_t i = n; i-- > 0; ) {
        DeferItem *item = items[i];
        env->throwing  = false;
        env->returning = false;
        EvalFrame *dframe = item->snapshot;
        if (dframe) dframe->parent = fallback_frame;
        eval_expr(env, dframe, item->body);
        eval_frame_free(item->snapshot);
        free(item);
    }
    free(items);

    env->throwing     = saved_throwing;
    env->throw_value  = saved_tv;
    env->returning    = saved_returning;
    env->return_value = saved_rv;
}

/* Phase R2: shared interpreter panic entry point.  Mirrors the EX_PANIC eval
 * case so native functions (result-must, option-must, option-expect, ...) raise
 * a *catchable* panic -- recoverable by catch-unwind and carrying the standard
 * panic message format and double-panic guard -- instead of calling _exit(1)
 * directly (which bypassed both catch-unwind and the defer chain).  Never
 * returns: it either longjmps to a catch boundary, or fires defers and exits. */
void turi_runtime_panic(TuriEnv *env, const char *msg) {
    const char *s = msg ? msg : "(no message)";
    if (env->panicking) {
        /* Double panic: a defer (or a panic during unwinding) panicked again. */
        fprintf(stderr, "double panic: aborting\n");
        fflush(stderr);
        fflush(stdout);
        abort();
    }
    /* If a catch-unwind boundary is active, longjmp to it. */
    if (env->catch_jmp && !env->in_no_unwind) {
        strncpy(env->catch_panic_msg, s, sizeof(env->catch_panic_msg) - 1);
        env->catch_panic_msg[sizeof(env->catch_panic_msg) - 1] = '\0';
        /* TI5: a plain panic carries a cstr payload (the message), so
         * catch-panic-of :cstr matches it. */
        env->catch_panic_type  = TY_CSTR;
        env->catch_panic_value = turi_cstr(env->catch_panic_msg);
        env->catch_panic_file  = NULL;
        env->catch_panic_line  = 0;
        env->panicking = true;
        longjmp(*env->catch_jmp, 1);
    }
    env->panicking = true;
    if (env->in_no_unwind) {
        fprintf(stderr, "panic (no unwind): %s\n", s);
    } else {
        fprintf(stderr, "panic at\npanic: %s\n", s);
    }
    fflush(stderr);
    /* Fire all pending defers before exiting (outer-first order). */
    if (!env->in_no_unwind)
        fire_defers_to_mark_reversed(env, NULL, NULL);
    fflush(stdout);
    exit(1);
}

/* -------------------------------------------------------------------------
 * Phase TI5: typed panic payloads + catch-panic-of.
 *
 * The compiled runtime carries a `tur_panic_payload { type_tag; value; file;
 * line }` across the catch boundary so catch-panic-of can filter by type and
 * the panic-payload-* accessors can read the panicked value.  The interpreter
 * mirrors this: a panic stashes its type/value/file/line in the TuriEnv
 * (catch_panic_*), and a caught result boxes a heap TuriPanicPayload whose
 * pointer is the err-val of the (err payload) Result.  The panic-payload-*
 * accessors cast that pointer back and read the fields.
 * ---------------------------------------------------------------------- */

typedef struct TuriPanicPayload {
    int         type_tag;   /* TypeKind of the panicked value */
    TuriValue   value;      /* the panicked value (cstr for a plain panic) */
    const char *file;       /* source file, or NULL */
    int         line;       /* source line, or 0 */
} TuriPanicPayload;

/* Build the 3-int Result box { is_ok, ok_val, err_val } the native ok/err
 * helpers also use, in its Ok shape. */
static TuriValue turi_ok_result_box(int64_t ok_val) {
    int64_t *box = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!box) return turi_error("eval: catch: oom");
    box[0] = 1; box[1] = ok_val; box[2] = 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)box;
    return v;
}

/* Build the Err shape of the Result box, with err_val pointing at a heap
 * TuriPanicPayload snapshotted from the env's in-flight panic fields. */
static TuriValue turi_err_result_box(TuriEnv *env) {
    TuriPanicPayload *pp = (TuriPanicPayload *)malloc(sizeof(TuriPanicPayload));
    if (!pp) return turi_error("eval: catch: oom");
    pp->type_tag = env->catch_panic_type;
    pp->value    = env->catch_panic_value;
    pp->file     = env->catch_panic_file
                   ? strdup(env->catch_panic_file) : NULL;
    pp->line     = env->catch_panic_line;
    int64_t *box = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!box) { free(pp); return turi_error("eval: catch: oom"); }
    box[0] = 0; box[1] = 0; box[2] = (int64_t)(intptr_t)pp;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)box;
    return v;
}

/* -------------------------------------------------------------------------
 * Phase S3: Algebraic effects — TuriEffectCont and TuriHandlerFrame
 *
 * Design:
 *  - Each (handle BODY cases...) creates a TuriEffectCont that runs BODY in
 *    a separate ucontext fiber.  When BODY performs an effect, the fiber
 *    yields back to the handle frame, which dispatches the matching case.
 *  - (resume k v) switches back into the body fiber with v as the result of
 *    perform.  The body may then perform again (handled recursively) or
 *    finish, at which point its result propagates back through resume.
 *  - Deep handler semantics: the handler remains in scope during body
 *    execution, but is not in scope while the handler case body runs.
 *  - handler_stack in TuriEnv is a linked list of TuriHandlerFrame; cast
 *    from void* to TuriHandlerFrame* inside eval.c.
 * ---------------------------------------------------------------------- */

#define EFFECT_CONT_STACK_SIZE (256 * 1024)  /* 256 KB per body fiber */

/* Work-stack (trampoline) continuation -- the heap-owned, delimited-control
 * representation used by capturable handles (DC plan).  Defined fully near the
 * driver (after DriveCont); here we only need the pointer in TuriEffectCont. */
typedef struct TuriWsCont TuriWsCont;

/* Full definition of TuriEffectCont (forward-declared in value.h). */
struct TuriEffectCont {
    /* Non-NULL when this continuation is a work-stack continuation (captured by
     * the trampoline driver, not a ucontext fiber).  EX_RESUME branches on it. */
    TuriWsCont        *ws;

    ucontext_t         body_ctx;      /* fiber context for body execution */
    ucontext_t         handler_ctx;   /* most-recent handler context */
    char              *body_stack;    /* mmap'd body fiber stack */

    /* body → handler communication (valid while body is suspended) */
    const char        *perf_name;     /* NULL when body finishes normally */
    TuriValue         *perf_args;     /* points into body's stack frame */
    uint8_t            n_perf_args;

    /* handler → body communication */
    TuriValue          resume_val;

    /* result when body finishes */
    TuriValue          body_result;
    bool               done;

    /* context for body evaluation (set before first swapcontext) */
    TuriEnv           *env;
    EvalFrame         *body_frame;
    const Expr        *body_expr;

    /* handle expression and frame for re-dispatching on multiple performs */
    const HandleExpr  *handle_expr;
    EvalFrame         *handle_frame;
};

/* Internal handler frame stored on TuriEnv.handler_stack. */
typedef struct TuriHandlerFrame {
    HandleCase              *cases;
    uint8_t                  n_cases;
    TuriEffectCont          *cont;
    struct TuriHandlerFrame *prev;
} TuriHandlerFrame;

/* Thread-local used to pass TuriEffectCont* into the body thunk.
 * makecontext only supports int arguments, so we use this side channel.
 * Not needed in WASM: emscripten_fiber_init accepts a void* arg directly. */
#ifndef __EMSCRIPTEN__
static _Thread_local TuriEffectCont *g_pending_cont;
#endif

/* Forward-declare eval_handle_inner (eval_expr is declared above). */
static TuriValue eval_handle_inner(TuriEnv *env, EvalFrame *frame,
                                   const HandleExpr *h,
                                   TuriEffectCont *cont);

/* Body thunk: runs inside the body fiber; yields back when perform fires or done. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
static void eval_body_thunk(void *arg) {
    TuriEffectCont *cont = (TuriEffectCont *)arg;
    cont->body_result = eval_expr(cont->env, cont->body_frame, cont->body_expr);
    cont->done        = true;
    swapcontext(&cont->body_ctx, &cont->handler_ctx);
    abort();
}
#else
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
static void eval_body_thunk(void) {
    TuriEffectCont *cont = g_pending_cont;
    cont->body_result = eval_expr(cont->env, cont->body_frame, cont->body_expr);
    cont->done        = true;
    swapcontext(&cont->body_ctx, &cont->handler_ctx);
    abort();
}
#  if defined(__APPLE__)
#    pragma clang diagnostic pop
#  endif
#endif

/* Resume body fiber with value val; return body's next result (or final). */
static TuriValue eval_resume_cont(TuriEnv *env, EvalFrame *frame,
                                   TuriEffectCont *cont, TuriValue val) {
    cont->resume_val = val;

    /* Re-install handler frame around the body re-entry (deep semantics). */
    TuriHandlerFrame hf;
    hf.cases          = cont->handle_expr->cases;
    hf.n_cases        = cont->handle_expr->n_cases;
    hf.cont           = cont;
    hf.prev           = (TuriHandlerFrame *)env->handler_stack;
    env->handler_stack = &hf;

#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    swapcontext(&cont->handler_ctx, &cont->body_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

    /* Pop handler frame now that body has yielded control back. */
    env->handler_stack = hf.prev;

    if (cont->done) {
        return cont->body_result;
    }

    /* Body performed again; dispatch handler body recursively. */
    return eval_handle_inner(env, frame, cont->handle_expr, cont);
}

/* Dispatch the handler case for the current perform signal on cont. */
static TuriValue eval_handle_inner(TuriEnv *env, EvalFrame *frame,
                                    const HandleExpr *h,
                                    TuriEffectCont *cont) {
    /* Find matching case. */
    HandleCase *matched = NULL;
    for (uint8_t i = 0; i < h->n_cases; i++) {
        if (strcmp(h->cases[i].effect_name->name, cont->perf_name) == 0) {
            matched = &h->cases[i];
            break;
        }
    }
    if (!matched) {
        /* Effect not handled by this handler — propagate to the next outer handler.
         * We're currently in the handler context (the outer body fiber or main thread).
         * env->handler_stack has the outer handler frames (current handler was popped). */
        TuriHandlerFrame *outer = (TuriHandlerFrame *)env->handler_stack;
        while (outer) {
            for (uint8_t j = 0; j < outer->n_cases; j++) {
                if (strcmp(outer->cases[j].effect_name->name, cont->perf_name) == 0) {
                    /* Found matching outer handler. Yield to it. */
                    TuriEffectCont *outer_cont = outer->cont;
                    /* Copy signal to outer cont. */
                    outer_cont->perf_name   = cont->perf_name;
                    outer_cont->perf_args   = cont->perf_args;
                    outer_cont->n_perf_args = cont->n_perf_args;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
                    swapcontext(&outer_cont->body_ctx, &outer_cont->handler_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
                    /* Outer handler ran and resumed us. Forward resume value to inner body. */
                    TuriValue resume_val = outer_cont->resume_val;
                    cont->resume_val = resume_val;
                    /* Re-push inner handler frame and resume inner body. */
                    TuriHandlerFrame inner_hf;
                    inner_hf.cases   = cont->handle_expr->cases;
                    inner_hf.n_cases = cont->handle_expr->n_cases;
                    inner_hf.cont    = cont;
                    inner_hf.prev    = (TuriHandlerFrame *)env->handler_stack;
                    env->handler_stack = &inner_hf;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
                    swapcontext(&cont->handler_ctx, &cont->body_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
                    env->handler_stack = inner_hf.prev;
                    if (cont->done)
                        return cont->body_result;
                    /* Inner body performed again; dispatch (possibly propagate again). */
                    return eval_handle_inner(env, frame, cont->handle_expr, cont);
                }
            }
            outer = outer->prev;
        }
        return turi_errorf("eval: unhandled effect: %s", cont->perf_name);
    }

    /* Bind params and k in a new frame. */
    EvalFrame *hframe = eval_frame_new(frame);
    for (uint8_t i = 0; i < matched->n_params && i < cont->n_perf_args; i++) {
        const char *pname = matched->param_bindings[i]->name->name;
        frame_bind(hframe, pname, cont->perf_args[i]);
    }
    frame_bind(hframe, matched->k_binding->name->name, turi_effect_cont(cont));

    TuriValue result = eval_expr(env, hframe, matched->body);
    eval_frame_free(hframe);
    return result;
}

/* Fiber perform: signal the innermost ucontext handler and block until resumed.
 * Shared by the eval_expr_impl EX_PERFORM path and the driver's work-stack
 * fallback (a perform that finds no DK_PROMPT on the current work-stack -- e.g.
 * a black-boxed perform, or one whose handler is an enclosing fiber handle).
 * `args` must stay live in the caller's frame across the swapcontext (the
 * handler reads cont->perf_args during dispatch). */
static TuriValue eval_perform_fiber(TuriEnv *env, const char *effect_name,
                                    TuriValue *args, uint8_t n_args) {
    TuriHandlerFrame *top = (TuriHandlerFrame *)env->handler_stack;
    if (!top) return turi_errorf("eval: unhandled effect: %s", effect_name);
    TuriEffectCont *cont = top->cont;
    cont->perf_name   = effect_name;
    cont->perf_args   = args;
    cont->n_perf_args = n_args;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    swapcontext(&cont->body_ctx, &cont->handler_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
    return cont->resume_val;
}

/* -------------------------------------------------------------------------
 * Phase TI6: First-class handler values (handler / with-handler /
 * compose-handlers).
 *
 * A handler *value* is a detached dispatch table -- a list of HandleCase
 * pointers without a body.  `with-handler` later pairs such a value with a
 * body and runs it exactly like a (handle BODY cases...) form: we materialise
 * a contiguous HandleCase array, synthesise a HandleExpr, and reuse the
 * existing eval_handle machinery (fiber body + perform/resume dispatch).
 *
 * The HandleCase structs are arena-allocated by the elaborator and outlive any
 * single evaluation, so the value only needs to borrow pointers to them.
 * `compose-handlers` concatenates two tables (h1's cases first -- h1 is the
 * outer handler per FH0.1; the elaborator already rejects overlapping effect
 * sets, so first-match dispatch order is unobservable across the two).
 * ---------------------------------------------------------------------- */

#define TURI_MAX_HANDLER_CASES 32

struct TuriHandlerVal {
    uint8_t            n_cases;
    const HandleCase  *cases[TURI_MAX_HANDLER_CASES];
};

/* Evaluate a (handle BODY cases...) expression. */
static TuriValue eval_handle(TuriEnv *env, EvalFrame *frame,
                              const HandleExpr *h) {
    /* Allocate and initialise the continuation. */
    TuriEffectCont *cont = (TuriEffectCont *)calloc(1, sizeof(TuriEffectCont));
    if (!cont) return turi_error("eval: out of memory allocating continuation");

    cont->env          = env;
    cont->body_frame   = frame;
    cont->body_expr    = h->body;
    cont->handle_expr  = h;
    cont->handle_frame = frame;
    cont->done         = false;

    /* Allocate body fiber stack. */
#ifndef __EMSCRIPTEN__
    cont->body_stack = (char *)mmap(NULL, EFFECT_CONT_STACK_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cont->body_stack == MAP_FAILED) {
        free(cont);
        return turi_error("eval: mmap failed for continuation stack");
    }
#else
    cont->body_stack = (char *)malloc(EFFECT_CONT_STACK_SIZE);
    if (!cont->body_stack) {
        free(cont);
        return turi_error("eval: malloc failed for continuation stack");
    }
#endif

    /* Set up execution contexts. */
#ifndef __EMSCRIPTEN__
    /* Native: template body_ctx from current context, configure stack, set entry. */
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
    getcontext(&cont->body_ctx);
    cont->body_ctx.uc_stack.ss_sp   = cont->body_stack;
    cont->body_ctx.uc_stack.ss_size = EFFECT_CONT_STACK_SIZE;
    cont->body_ctx.uc_link          = NULL;
    g_pending_cont = cont;
    makecontext(&cont->body_ctx, eval_body_thunk, 0);
#else
    /* WASM: init handler from current execution context; init body as new fiber. */
    getcontext(&cont->handler_ctx);
    emscripten_fiber_init(&cont->body_ctx.fiber, eval_body_thunk, cont,
                          cont->body_stack, EFFECT_CONT_STACK_SIZE,
                          cont->body_ctx.asyncify_stack, TURI_ASYNCIFY_STACK_SIZE);
#endif

    /* Push handler frame so body can find our cases during perform. */
    TuriHandlerFrame hf;
    hf.cases           = h->cases;
    hf.n_cases         = h->n_cases;
    hf.cont            = cont;
    hf.prev            = (TuriHandlerFrame *)env->handler_stack;
    env->handler_stack = &hf;

    /* Switch to body; saves current context into handler_ctx. */
    swapcontext(&cont->handler_ctx, &cont->body_ctx);
#if !defined(__EMSCRIPTEN__) && defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

    /* Body has yielded control; pop handler frame. */
    env->handler_stack = hf.prev;

    TuriValue result;
    if (cont->done) {
        /* Body finished without performing — return body result directly. */
        result = cont->body_result;
    } else {
        /* Body performed an effect; dispatch handler case. */
        result = eval_handle_inner(env, frame, h, cont);
    }

#ifndef __EMSCRIPTEN__
    munmap(cont->body_stack, EFFECT_CONT_STACK_SIZE);
#else
    free(cont->body_stack);
#endif
    free(cont);
    return result;
}

/* -------------------------------------------------------------------------
 * Phase TI2: Generators (gen / yield / gen-next / gen-done?)
 *
 * A generator is a resumable coroutine.  The compiled path lowers each
 * (gen ...) body to a C state machine; the interpreter instead runs the
 * body on its own ucontext stack (the same fiber primitives that back
 * effect continuations) and swaps control on each `yield`.
 *
 * Lifecycle (matching the compiled __state ABI):
 *   - fresh:     started = false, done = false
 *   - suspended: started = true,  done = false  (parked at a yield)
 *   - exhausted: done = true                     (body ran to completion)
 *
 * `gen-next` resumes the body until the next yield (returning a non-NULL
 * pointer to the yielded value) or until completion (returning NULL and
 * marking the generator done).  `gen-done?` reports the `done` flag, which
 * only flips after a `gen-next` drives the body off its end -- so the
 * idiomatic `(while (not (gen-done? g)) ...)` loop terminates exactly as
 * it does on the compiled path.
 * ---------------------------------------------------------------------- */

#define GEN_STACK_SIZE (256 * 1024)  /* 256 KB per generator coroutine */

struct TuriGen {
    ucontext_t   gen_ctx;     /* coroutine context for the gen body */
    ucontext_t   caller_ctx;  /* where to swap back on yield / completion */
    char        *stack;       /* mmap'd / malloc'd coroutine stack */
    bool         started;     /* has the body begun executing? */
    bool         done;        /* has the body run to completion? */
    int64_t      box;         /* storage for the yielded value; gen-next
                               * returns &box as the ptr<void> ABI result */
    /* eval context (valid for the generator's whole lifetime) */
    TuriEnv     *env;
    EvalFrame   *frame;       /* body scope (child of the creating frame) */
    const Expr  *body;        /* gen body expression */
    /* error / throw propagation out of the body */
    bool         had_error;
    TuriValue    error_val;
};

/* The generator currently being resumed (read once by gen_body_thunk on first
 * entry).  makecontext cannot portably pass a pointer argument, so we use the
 * same thread-local side-channel pattern as the effect-continuation thunk. */
#ifndef __EMSCRIPTEN__
static _Thread_local TuriGen *g_pending_gen;
#endif

/* The generator the active `yield` should suspend.  Pushed/popped around each
 * resume so a nested generator's yield finds its own coroutine. */
static _Thread_local TuriGen *g_current_gen;

/* Body thunk: runs the generator body to completion, then swaps back to the
 * caller with done = true.  Each `yield` swaps back mid-body. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
static void gen_body_thunk(void *arg) {
    TuriGen *g = (TuriGen *)arg;
#else
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
static void gen_body_thunk(void) {
    TuriGen *g = g_pending_gen;
#endif
    TuriValue r = eval_expr(g->env, g->frame, g->body);
    if (turi_is_error(r)) { g->had_error = true; g->error_val = r; }
    else if (g->env->throwing) {
        g->had_error = true; g->error_val = g->env->throw_value;
        g->env->throwing = false;
    }
    /* A `(return)` inside the generator body (e.g. seq/take-while's early stop)
     * terminates the generator; its value is discarded.  The body runs on this
     * coroutine but shares the consumer's TuriEnv, so a leaked `returning` flag
     * would otherwise propagate into gen-next's caller (the driver loop would
     * bail and hand back env->return_value, 0, instead of its accumulator).
     * Reset it here, mirroring the throwing reset above. */
    g->env->returning = false;
    g->done = true;
    swapcontext(&g->gen_ctx, &g->caller_ctx);
    abort(); /* unreachable */
#if !defined(__EMSCRIPTEN__) && defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
}

/* Resume the generator body until the next yield or completion.  Returns the
 * ptr<void> ABI value (boxed in a TURI_INT): non-NULL when a value was
 * yielded, 0 (NULL) when exhausted.  Propagates a body error/throw. */
static TuriValue gen_advance(TuriEnv *env, TuriGen *g) {
    if (g->done) return turi_int(0);

    if (!g->started) {
        /* Lazily set up the coroutine context on first advance. */
#ifndef __EMSCRIPTEN__
        g->stack = (char *)mmap(NULL, GEN_STACK_SIZE, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g->stack == MAP_FAILED) {
            g->done = true;
            return turi_error("eval: mmap failed for generator stack");
        }
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
        getcontext(&g->gen_ctx);
        g->gen_ctx.uc_stack.ss_sp   = g->stack;
        g->gen_ctx.uc_stack.ss_size = GEN_STACK_SIZE;
        g->gen_ctx.uc_link          = NULL;
        g_pending_gen = g;
        makecontext(&g->gen_ctx, gen_body_thunk, 0);
#  if defined(__APPLE__)
#    pragma clang diagnostic pop
#  endif
#else
        g->stack = (char *)malloc(GEN_STACK_SIZE);
        if (!g->stack) { g->done = true; return turi_error("eval: malloc failed for generator stack"); }
        getcontext(&g->caller_ctx);
        emscripten_fiber_init(&g->gen_ctx.fiber, gen_body_thunk, g,
                              g->stack, GEN_STACK_SIZE,
                              g->gen_ctx.asyncify_stack, TURI_ASYNCIFY_STACK_SIZE);
#endif
        g->started = true;
    }

    /* Swap into the body; the previously-current generator (if any) is
     * restored when control returns here. */
    TuriGen *prev_gen = g_current_gen;
    g_current_gen = g;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    swapcontext(&g->caller_ctx, &g->gen_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
    g_current_gen = prev_gen;

    if (g->done) {
        /* Body finished while we were swapped in. */
        if (g->had_error) {
            g->had_error = false;
            if (g->error_val.tag == TURI_THROW) {
                env->throwing    = true;
                env->throw_value = g->error_val;
            }
            return g->error_val;
        }
        return turi_int(0); /* NULL: exhausted */
    }
    /* Body yielded; g->box already holds the value. */
    return turi_int((int64_t)(intptr_t)&g->box);
}

/* -------------------------------------------------------------------------
 * Phase TI3: delimited control -- reset / shift / shift0 (abortive)
 *
 * Turmeric's shift/reset are ABORTIVE.  The compiled path lowers (shift f body)
 * to: evaluate body to v, compute f(v), then abort the computation up to the
 * nearest enclosing reset, which yields f(v) as its value (see the runtime
 * `__dk_abort_body` in emitted C and src/runtime/cps_prompt.c -- the captured
 * sub-continuation is never resumed).  Because the continuation is discarded, a
 * plain setjmp/longjmp prompt boundary models the semantics exactly, with no
 * fiber capture needed.  shift0 differs from shift only in prompt
 * re-installation when the continuation is *resumed*; since it is never
 * resumed here, shift0 behaves identically to shift.
 *
 * The context-capturing variants (serial-shift / cloneable-shift), which DO
 * hand a resumable continuation to f, are a separate, larger piece of work and
 * remain a documented interpreter carve-out -- see
 * docs/archive/turi-capturing-shift-unimplemented.md.  serial-reset and
 * cloneable-reset establish a prompt boundary so the no-shift passthrough case
 * (e.g. (serial-reset 42)) evaluates correctly; a *-shift inside them still
 * errors cleanly until the capturing work lands.
 * ---------------------------------------------------------------------- */
typedef enum { PROMPT_PLAIN, PROMPT_SERIAL, PROMPT_CLONEABLE } TuriPromptKind;

typedef struct TuriResetBoundary {
    jmp_buf                   jmp;
    TuriPromptKind            kind;
    TuriValue                 result;       /* value an abortive shift delivers */
    /* env state to restore after a longjmp unwinds the intervening C frames */
    uint32_t                  saved_depth;
    void                     *saved_handler_stack;
    void                     *saved_defer_stack;
    struct TuriResetBoundary *prev;
} TuriResetBoundary;

static _Thread_local TuriResetBoundary *g_reset_stack;

/* Nearest enclosing boundary of the given kind, or NULL. */
static TuriResetBoundary *reset_find(TuriPromptKind kind) {
    for (TuriResetBoundary *b = g_reset_stack; b; b = b->prev)
        if (b->kind == kind) return b;
    return NULL;
}

/* Establish a delimited-control prompt and evaluate body within it.  Returns
 * body's value normally, or the value delivered by an abortive shift that
 * targeted this boundary. */
static TuriValue eval_reset_boundary(TuriEnv *env, EvalFrame *frame,
                                     const Expr *body, TuriPromptKind kind) {
    TuriResetBoundary b;
    b.kind                = kind;
    b.result              = turi_nil();
    b.saved_depth         = env->eval_depth;
    b.saved_handler_stack = env->handler_stack;
    b.saved_defer_stack   = env->defer_stack;
    b.prev                = g_reset_stack;
    g_reset_stack = &b;

    if (setjmp(b.jmp) == 0) {
        TuriValue v = eval_expr(env, frame, body);
        g_reset_stack = b.prev;
        return v;
    }
    /* An abortive shift longjmp'd here.  Restore the env state that the
     * unwound eval_expr frames would otherwise have decremented/popped. */
    g_reset_stack      = b.prev;
    env->eval_depth    = b.saved_depth;
    env->handler_stack = b.saved_handler_stack;
    env->defer_stack   = b.saved_defer_stack;
    return b.result;
}

/* Evaluate an abortive (shift f body) / (shift0 f body): r = f(body), then
 * abort to the nearest plain reset boundary, which returns r. */
static TuriValue eval_abortive_shift(TuriEnv *env, EvalFrame *frame,
                                     const Expr *k_fn, const Expr *body,
                                     const char *form) {
    TuriValue v = eval_expr(env, frame, body);
    if (turi_is_error(v) || env->returning || env->throwing) return v;
    TuriValue fn = eval_expr(env, frame, k_fn);
    if (turi_is_error(fn) || env->returning || env->throwing) return fn;
    TuriValue r = turi_call(env, fn, &v, 1);
    if (turi_is_error(r) || env->returning || env->throwing) return r;

    TuriResetBoundary *b = reset_find(PROMPT_PLAIN);
    if (!b)
        return turi_errorf("eval: %s used outside of any reset boundary", form);
    b->result = r;
    longjmp(b->jmp, 1);
    /* unreachable */
}

/* -------------------------------------------------------------------------
 * Context-capturing delimited control: serial-shift / cloneable-shift
 *
 * Unlike the abortive shift above, these hand a *resumable* continuation k to
 * their receiver f.  The compiled path reifies the delimited context (the
 * frames between the shift and its enclosing reset) at compile time into a DK
 * chain (src/compiler/emit_cps.c collect_ctx; src/runtime/cps_prompt.c).  The
 * interpreter has no compile phase, so it reifies the same context at *runtime*:
 * when evaluating (serial-reset BODY) / (cloneable-reset BODY) it walks BODY --
 * following the unique shift-reaching child through the supported grammar
 * (single-hole int +,-,*,/ binops; 1- and 2-arg top-level call frames; pure
 * `let` bindings; an `if` with one shift-bearing arm) -- evaluating each
 * non-hole operand once at capture time and recording it as a frame.  The
 * captured continuation is that frame array; resuming with w folds the frames
 * innermost-first (frames[0] is outermost), so k is replayable / multi-shot for
 * cloneable and marshalable (in-process) for serial.  This mirrors collect_ctx;
 * shapes it does not model (do-sequence prelude, struct envs, call/cc*) fall
 * through to a clean error, matching the compiled grammar's own NULL returns.
 * See docs/archive/turi-capturing-shift-unimplemented.md.
 * ---------------------------------------------------------------------- */
#define TS_MAX_CTX_FRAMES 64

typedef struct TsFrame {
    uint8_t   kind;        /* 0 = arith binop, 1 = call frame */
    char      op;          /* arith operator: '+','-','*','/' */
    uint8_t   hole_index;  /* hole slot: arith 0=left 1=right; call arg index */
    uint8_t   n_args;      /* call arity (0, 1, or 2) */
    bool      ignore_value;/* call: run for side effect, ignore the resume value
                            * (a `do`-sequence tail item) */
    int64_t   env;         /* arith other operand (int) */
    TuriValue env_val;     /* call non-hole / sole env operand (any TuriValue) */
    TuriValue fn;          /* call-frame function closure */
} TsFrame;

typedef struct TuriCont {
    TsFrame  *frames;      /* frames[0] = outermost */
    uint32_t  n;
    bool      serial;      /* serial vs cloneable (behaviourally identical here) */
} TuriCont;

/* call/cc / escape (EX_CALLCC): an undelimited, one-shot, upward escape
 * continuation.  The handle f receives is a pointer to this stack-allocated
 * landing pad; invoking it (tur_escape_resume, the lowering of the (k v)
 * application sugar in elab_call.c) longjmps back to the call/cc site with the
 * value, abandoning f's pending computation.  One-shot upward only: k is valid
 * just while its call/cc frame is live, which is exactly when an escape can
 * fire -- so a setjmp/longjmp pad models it precisely, no fiber capture. */
typedef struct TuriEscapeBoundary {
    jmp_buf   jmp;
    TuriValue result;       /* value delivered by (k v) */
    uint32_t  saved_depth;
    void     *saved_handler_stack;
    void     *saved_defer_stack;
} TuriEscapeBoundary;

/* Does e contain a shift of `target` in evaluation position?  Mirrors the
 * compiler's reaches_shift_kind (emit_cps.c): nested resets / shifts / fns
 * self-delimit, so they do not count once the outer target has been matched. */
static bool ts_reaches_shift(const Expr *e, ExprKind target) {
    if (!e) return false;
    if ((int)e->kind == (int)target) return true;
    /* Nested resets / shifts / fns self-delimit (the compiler's
     * reaches_shift_kind names them explicitly; here they fall to `default` and
     * return false -- listing them as explicit arms would make the turi-parity
     * ratchet read e.g. EX_CPS_CONT_APP as interpreter-handled). */
    switch (e->kind) {
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (ts_reaches_shift(e->as.builtin.args[i], target)) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (ts_reaches_shift(e->as.call_.args[i], target)) return true;
            return false;
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (ts_reaches_shift(e->as.let_.bindings[i].init, target)) return true;
            return ts_reaches_shift(e->as.let_.body, target);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (ts_reaches_shift(e->as.do_.items[i], target)) return true;
            return false;
        case EX_IF:
            return ts_reaches_shift(e->as.if_.cond, target) ||
                   ts_reaches_shift(e->as.if_.then_, target) ||
                   ts_reaches_shift(e->as.if_.else_or_null, target);
        case EX_RETURN: return ts_reaches_shift(e->as.return_.value, target);
        case EX_SET:    return ts_reaches_shift(e->as.set_.value, target);
        default:        return false;
    }
}

/* A single-char arithmetic operator the context grammar supports. */
static bool ts_arith_op(const char *op) {
    return op && op[0] && op[1] == '\0' &&
           (op[0] == '+' || op[0] == '-' || op[0] == '*' || op[0] == '/');
}

static TuriCont *ts_cont_copy(const TuriCont *c) {
    TuriCont *d = (TuriCont *)malloc(sizeof(TuriCont));
    d->n = c->n;
    d->serial = c->serial;
    d->frames = (TsFrame *)malloc(sizeof(TsFrame) * (c->n ? c->n : 1));
    if (c->n) memcpy(d->frames, c->frames, sizeof(TsFrame) * c->n);
    return d;
}

/* Resume continuation c with value w: fold the frames innermost-first.  Each
 * frame's captured env was evaluated once at capture time, so this is pure with
 * respect to the context (re-runnable for multi-shot). */
static TuriValue ts_cont_resume(TuriEnv *env, TuriCont *c, int64_t w) {
    int64_t v = w;
    for (int32_t i = (int32_t)c->n - 1; i >= 0; i--) {
        TsFrame *fr = &c->frames[i];
        if (fr->kind == 0) {
            int64_t e0 = fr->env;
            switch (fr->op) {
            case '+': v = e0 + v; break;
            case '*': v = e0 * v; break;
            case '-': v = (fr->hole_index == 0) ? (v - e0) : (e0 - v); break;
            case '/':
                if (fr->hole_index == 0) {
                    if (e0 == 0) return turi_error("eval: cont resume: division by zero");
                    v = v / e0;
                } else {
                    if (v == 0) return turi_error("eval: cont resume: division by zero");
                    v = e0 / v;
                }
                break;
            default: return turi_errorf("eval: cont resume: bad op '%c'", fr->op);
            }
        } else {
            TuriValue args[2];
            uint32_t  na = fr->n_args;
            if (fr->ignore_value) {
                /* do-sequence tail item: run for effect, ignore the resume
                 * value; its sole argument (if any) is the captured env. */
                if (na >= 1) { args[0] = fr->env_val; na = 1; }
            } else if (na == 1) {
                args[0] = turi_int(v);
            } else if (fr->hole_index == 0) {
                args[0] = turi_int(v); args[1] = fr->env_val;
            } else {
                args[0] = fr->env_val; args[1] = turi_int(v);
            }
            TuriValue r = turi_call(env, fr->fn, args, na);
            if (turi_is_error(r) || env->returning || env->throwing) return r;
            if (r.tag != TURI_INT)
                return turi_errorf("eval: cont call frame returned non-int (tag %d)", r.tag);
            v = r.as_int;
        }
    }
    return turi_int(v);
}

/* Dispatch the continuation-resume / clone / marshal builtins (BS_FUNC_CALL
 * with a tur_*_cont_* c_op).  Returns true and sets *out when handled.  The
 * handle is an int64 boxing a TuriCont*; serialize/deserialize round-trip
 * in-process (the "bytes" is a deep copy of the chain -- a native stack
 * snapshot would not be serializable, which is the point of the serial flavor,
 * and a real byte codec cannot encode the call-frame closures, so an in-process
 * deep copy faithfully reproduces a direct resume). */
static bool ts_try_cont_builtin(TuriEnv *env, const BuiltinSpec *spec,
                                TuriValue *args, uint32_t n, TuriValue *out) {
    const char *name = spec->c_op ? spec->c_op : "";
    if (strcmp(name, "tur_cloneable_cont_resume") == 0 ||
        strcmp(name, "tur_serial_cont_resume") == 0) {
        if (n < 2 || args[0].as_int == 0) { *out = turi_int(0); return true; }
        TuriCont *c = (TuriCont *)(intptr_t)args[0].as_int;
        *out = ts_cont_resume(env, c, args[1].as_int);
        return true;
    }
    if (strcmp(name, "tur_cloneable_cont_clone") == 0 ||
        strcmp(name, "tur_continuation_snapshot") == 0 ||
        strcmp(name, "tur_serial_cont_serialize") == 0 ||
        strcmp(name, "tur_serial_cont_deserialize") == 0) {
        if (n < 1 || args[0].as_int == 0) { *out = turi_int(0); return true; }
        TuriCont *c = (TuriCont *)(intptr_t)args[0].as_int;
        *out = turi_int((int64_t)(intptr_t)ts_cont_copy(c));
        return true;
    }
    if (strcmp(name, "tur_cloneable_cont_drop") == 0) { *out = turi_nil(); return true; }
    if (strcmp(name, "tur_escape_resume") == 0) {
        /* (k v) on an escape continuation: longjmp back to the call/cc landing
         * pad with v.  Does not return. */
        if (n < 2 || args[0].as_int == 0) { *out = turi_int(0); return true; }
        TuriEscapeBoundary *b = (TuriEscapeBoundary *)(intptr_t)args[0].as_int;
        b->result = args[1];
        longjmp(b->jmp, 1);
    }
    return false;
}

/* The body reaches the shift but its delimited context falls outside the
 * supported grammar, so the continuation cannot be reified.  For serial this is
 * exactly the compiled path's TUR-E0706 (emit_effects.c / emit_stmt.c); emit the
 * same diagnostic here so the interpreter rejects it rather than silently
 * miscompiling -- recovering the not-capturable negative fixtures on the
 * interpret path (the "decouple the TUR-E0706 negative path" slice of
 * docs/archive/turi-capturing-shift-unimplemented.md).  Returns an
 * "elaboration error" sentinel so cmd_eval does not re-print the message. */
static TuriValue ts_not_capturable(bool serial, Span span) {
    if (serial) {
        diag_emit_with_code(DIAG_ERROR, span,
            TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE,
            "serial-shift context is not capturable\n"
            "  = note: the delimited context falls outside the supported "
            "lowering grammar, so the continuation cannot be reified\n"
            "  = help: restructure into a supported shape (scalar let prelude / "
            "arithmetic / 1- or 2-arg call / if / do-tail)");
        return turi_error("elaboration error");
    }
    return turi_error("eval: cloneable-shift context is not capturable "
                      "(unsupported delimited-context shape)");
}

/* Evaluate (serial-reset BODY) / (cloneable-reset BODY) when BODY performs the
 * matching capturing shift: reify the delimited context, hand the receiver a
 * resumable continuation, and return the receiver's value as the reset value. */
static TuriValue ts_capture_and_run(TuriEnv *env, EvalFrame *frame,
                                    const Expr *body, ExprKind shift_kind,
                                    bool serial) {
    TsFrame     frames[TS_MAX_CTX_FRAMES];
    uint32_t    n = 0;
    EvalFrame  *let_frames[TS_MAX_CTX_FRAMES];
    uint32_t    n_let = 0;
    EvalFrame  *cur_frame = frame;
    const Expr *cur = body;
    const Expr *shift = NULL;
    TuriValue   result = turi_nil();
    bool        done = false;         /* result is set */
    bool        is_pure = false;
    TuriValue   pure_val = turi_nil();

    for (;;) {
        if (n >= TS_MAX_CTX_FRAMES) {
            result = turi_error("eval: capturing context too deep"); done = true; break;
        }
        if ((int)cur->kind == (int)shift_kind) { shift = cur; break; }
        if (!ts_reaches_shift(cur, shift_kind)) {
            /* Pure terminal (e.g. the non-shift arm of an `if`): the reset value
             * is the surrounding context applied to this value. */
            pure_val = eval_expr(env, cur_frame, cur);
            if (turi_is_error(pure_val) || env->returning || env->throwing) {
                result = pure_val; done = true;
            } else {
                is_pure = true;
            }
            break;
        }
        if (cur->kind == EX_LET) {
            EvalFrame *nf = eval_frame_new(cur_frame);
            let_frames[n_let++] = nf;
            bool err = false;
            for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                TuriValue v = eval_expr(env, nf, cur->as.let_.bindings[i].init);
                if (turi_is_error(v) || env->returning || env->throwing) {
                    result = v; done = true; err = true; break;
                }
                frame_bind(nf, cur->as.let_.bindings[i].binding->name->name, v);
            }
            if (err) break;
            cur_frame = nf;
            cur = cur->as.let_.body;
            continue;
        }
        if (cur->kind == EX_IF) {
            /* The condition is pure (grammar); the shift lives in one arm. */
            TuriValue cv = eval_expr(env, cur_frame, cur->as.if_.cond);
            if (turi_is_error(cv) || env->returning || env->throwing) {
                result = cv; done = true; break;
            }
            cur = turi_is_truthy(cv) ? cur->as.if_.then_ : cur->as.if_.else_or_null;
            if (!cur) {
                result = turi_error("eval: capturing if: missing arm"); done = true; break;
            }
            continue;
        }
        if (cur->kind == EX_BUILTIN && cur->as.builtin.n == 2 &&
            cur->as.builtin.spec && ts_arith_op(cur->as.builtin.spec->c_op)) {
            const Expr *a0 = cur->as.builtin.args[0];
            const Expr *a1 = cur->as.builtin.args[1];
            bool h0 = ts_reaches_shift(a0, shift_kind);
            bool h1 = ts_reaches_shift(a1, shift_kind);
            if (h0 == h1) {
                result = ts_not_capturable(serial, cur->span);   /* arith: not exactly one hole */
                done = true; break;
            }
            const Expr *other = h0 ? a1 : a0;
            TuriValue ov = eval_expr(env, cur_frame, other);
            if (turi_is_error(ov) || env->returning || env->throwing) {
                result = ov; done = true; break;
            }
            memset(&frames[n], 0, sizeof(TsFrame));
            frames[n].kind = 0;
            frames[n].op = cur->as.builtin.spec->c_op[0];
            frames[n].hole_index = h0 ? 0 : 1;
            frames[n].env = ov.as_int;
            n++;
            cur = h0 ? a0 : a1;
            continue;
        }
        if (cur->kind == EX_DO) {
            /* (do PRELUDE... (shift k v) TAIL...): the prelude runs once at
             * capture; each tail item is an ignore-value call frame run on
             * resume in source order (the last tail item yields the reset's
             * value).  Mirrors collect_ctx's do branch (emit_cps.c). */
            uint32_t N = cur->as.do_.n;
            int32_t  m = -1;
            for (uint32_t i = 0; i < N; i++) {
                if (ts_reaches_shift(cur->as.do_.items[i], shift_kind))
                    m = (m == -1) ? (int32_t)i : -2;
            }
            if (m < 0 || (int)cur->as.do_.items[m]->kind != (int)shift_kind) {
                result = ts_not_capturable(serial, cur->span);   /* do: shift not in statement position */
                done = true; break;
            }
            bool bad = false;
            for (int32_t i = 0; i < m; i++) {   /* prelude: side effects, once */
                TuriValue pv = eval_expr(env, cur_frame, cur->as.do_.items[i]);
                if (turi_is_error(pv) || env->returning || env->throwing) {
                    result = pv; done = true; bad = true; break;
                }
            }
            if (bad) break;
            for (int32_t i = (int32_t)N - 1; i > m; i--) {   /* tail: outermost first */
                const Expr *tail = cur->as.do_.items[i];
                if (tail->kind != EX_CALL || !tail->as.call_.fn_binding ||
                    tail->as.call_.fn_expr || tail->as.call_.n_args > 1) {
                    result = ts_not_capturable(serial, tail->span);   /* unsupported do-tail item */
                    done = true; bad = true; break;
                }
                if (n >= TS_MAX_CTX_FRAMES) {
                    result = turi_error("eval: capturing context too deep");
                    done = true; bad = true; break;
                }
                TuriValue fn = eval_lookup(env, cur_frame,
                                           tail->as.call_.fn_binding->name->name);
                if (turi_is_error(fn) || fn.tag != TURI_CLOSURE) {
                    result = turi_errorf("eval: capturing do tail: '%s' is not a function",
                                         tail->as.call_.fn_binding->name->name);
                    done = true; bad = true; break;
                }
                TuriValue ev = turi_nil();
                if (tail->as.call_.n_args == 1) {
                    const Expr *arg = tail->as.call_.args[0];
                    if (ts_reaches_shift(arg, shift_kind)) {
                        result = ts_not_capturable(serial, tail->span);   /* do-tail arg reaches the shift */
                        done = true; bad = true; break;
                    }
                    ev = eval_expr(env, cur_frame, arg);
                    if (turi_is_error(ev) || env->returning || env->throwing) {
                        result = ev; done = true; bad = true; break;
                    }
                }
                memset(&frames[n], 0, sizeof(TsFrame));
                frames[n].kind = 1;
                frames[n].ignore_value = true;
                frames[n].n_args = (uint8_t)tail->as.call_.n_args;
                frames[n].env_val = ev;
                frames[n].fn = fn;
                n++;
            }
            if (bad) break;
            cur = cur->as.do_.items[m];   /* descend into the shift */
            continue;
        }
        if (cur->kind == EX_CALL && cur->as.call_.fn_binding && !cur->as.call_.fn_expr &&
            (cur->as.call_.n_args == 1 || cur->as.call_.n_args == 2)) {
            uint32_t na = cur->as.call_.n_args;
            int hole = -1;
            for (uint32_t i = 0; i < na; i++) {
                if (ts_reaches_shift(cur->as.call_.args[i], shift_kind)) {
                    hole = (hole == -1) ? (int)i : -2;
                }
            }
            if (hole < 0) {
                result = ts_not_capturable(serial, cur->span);   /* call: not exactly one hole */
                done = true; break;
            }
            TuriValue fn = eval_lookup(env, cur_frame, cur->as.call_.fn_binding->name->name);
            if (turi_is_error(fn) || fn.tag != TURI_CLOSURE) {
                result = turi_is_error(fn) ? fn
                       : turi_errorf("eval: capturing call frame: '%s' is not a function",
                                     cur->as.call_.fn_binding->name->name);
                done = true; break;
            }
            TuriValue envv = turi_nil();
            if (na == 2) {
                const Expr *other = cur->as.call_.args[hole == 0 ? 1 : 0];
                envv = eval_expr(env, cur_frame, other);
                if (turi_is_error(envv) || env->returning || env->throwing) {
                    result = envv; done = true; break;
                }
            }
            memset(&frames[n], 0, sizeof(TsFrame));
            frames[n].kind = 1;
            frames[n].n_args = (uint8_t)na;
            frames[n].hole_index = (uint8_t)hole;
            frames[n].env_val = envv;
            frames[n].fn = fn;
            n++;
            cur = cur->as.call_.args[hole];
            continue;
        }
        result = ts_not_capturable(serial, cur->span);   /* unsupported context shape */
        done = true;
        break;
    }

    if (!done) {
        if (is_pure) {
            if (n == 0 || pure_val.tag != TURI_INT) {
                result = pure_val;   /* empty context, or non-int passthrough */
            } else {
                TuriCont c = { frames, n, serial };
                result = ts_cont_resume(env, &c, pure_val.as_int);
            }
        } else {
            /* shift found: build a heap cont, call the receiver with it. */
            TuriCont *cont = (TuriCont *)malloc(sizeof(TuriCont));
            cont->n = n;
            cont->serial = serial;
            cont->frames = (TsFrame *)malloc(sizeof(TsFrame) * (n ? n : 1));
            if (n) memcpy(cont->frames, frames, sizeof(TsFrame) * n);
            const Expr *kfn = (shift_kind == EX_SERIAL_SHIFT)
                ? shift->as.serial_shift_.k_fn
                : shift->as.cloneable_shift_.k_fn;
            TuriValue fn = eval_expr(env, cur_frame, kfn);
            if (turi_is_error(fn) || fn.tag != TURI_CLOSURE) {
                result = turi_is_error(fn) ? fn
                       : turi_error("eval: shift receiver is not a function");
            } else {
                TuriValue kval = turi_int((int64_t)(intptr_t)cont);
                result = turi_call(env, fn, &kval, 1);
            }
        }
    }

    for (int32_t i = (int32_t)n_let - 1; i >= 0; i--) eval_frame_free(let_frames[i]);
    return result;
}

/* Evaluate (call/cc f) / (escape f): establish an escape landing pad, hand f a
 * continuation handle, and run f.  If f returns normally, that is the call/cc
 * value; if f invokes (k v) -- lowered to tur_escape_resume -- control longjmps
 * back here and v is the call/cc value.  call/cc and escape are both one-shot
 * upward escapes in Turmeric (is_escape only distinguishes prompt re-install on
 * *resume*, which never happens for these), so they share this path. */
static TuriValue eval_callcc_escape(TuriEnv *env, EvalFrame *frame,
                                    const Expr *fn_expr) {
    TuriValue fn = eval_expr(env, frame, fn_expr);
    if (turi_is_error(fn) || env->returning || env->throwing) return fn;
    if (fn.tag != TURI_CLOSURE)
        return turi_errorf("eval: call/cc expects a function, got tag %d", fn.tag);

    TuriEscapeBoundary b;
    b.result              = turi_nil();
    b.saved_depth         = env->eval_depth;
    b.saved_handler_stack = env->handler_stack;
    b.saved_defer_stack   = env->defer_stack;

    if (setjmp(b.jmp) == 0) {
        TuriValue kval = turi_int((int64_t)(intptr_t)&b);
        return turi_call(env, fn, &kval, 1);   /* f returned without escaping */
    }
    /* (k v) longjmp'd here.  Restore the env state the unwound frames would
     * otherwise have left dangling (mirrors eval_reset_boundary). */
    env->eval_depth    = b.saved_depth;
    env->handler_stack = b.saved_handler_stack;
    env->defer_stack   = b.saved_defer_stack;
    return b.result;
}

/* stdlib/workflow.tur save-cont! -- serialise a serial continuation to "bytes".
 * In-process the bytes is a deep copy of the chain (see ts_try_cont_builtin). */
static TuriValue native_save_cont(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || args[0].as_int == 0) return turi_int(0);
    return turi_int((int64_t)(intptr_t)ts_cont_copy((TuriCont *)(intptr_t)args[0].as_int));
}

/* stdlib/workflow.tur resume-cont! -- rebuild the chain from bytes and resume. */
static TuriValue native_resume_cont(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2 || args[0].as_int == 0) return turi_int(0);
    return ts_cont_resume(env, (TuriCont *)(intptr_t)args[0].as_int, args[1].as_int);
}

/* -------------------------------------------------------------------------
 * Phase TI4: Software Transactional Memory (single-threaded model)
 *
 * The interpreter is single-threaded, so STM collapses to a transaction-log
 * model with no real concurrency (matching the plan's TI4 design and the
 * compiled runtime in src/runtime/stm.c):
 *
 *  - A TVar is a heap cell { value; version }.  Values are int64 boxed as
 *    ptr<void> (the compiled ABI stores `(void*)(intptr_t)init`), which is a
 *    TURI_INT here.
 *  - `atomically` runs the stm body against a write-log transaction.  Reads see
 *    buffered writes (read-your-writes); on normal completion the log is
 *    committed (writes applied, versions bumped).  With no concurrent writers,
 *    read-set validation always succeeds, so commit never fails.
 *  - `check`/`retry` request a retry.  Re-running a serial transaction can never
 *    make progress (nothing else mutates the TVars), so an unguarded retry that
 *    reaches `atomically` is a logical deadlock -- the compiled path blocks on a
 *    condvar forever; the interpreter errors out instead of hanging.
 *  - `or-else` clears a retry from stm1 and runs stm2 in the same transaction,
 *    so well-formed or-else never reaches the atomically retry check.
 * ---------------------------------------------------------------------- */
typedef struct TuriTVar { int64_t value; uint64_t version; } TuriTVar;

typedef struct TuriStmTx {
    TuriTVar **w_tv;     /* write-set TVars */
    int64_t   *w_val;    /* parallel buffered values */
    int        w_count;
    int        w_cap;
    bool       retry_requested;
    bool       aborted;
    struct TuriStmTx *prev;   /* nesting (atomically within atomically) */
} TuriStmTx;

static _Thread_local TuriStmTx *g_stm_tx;

static int stm_write_find(TuriStmTx *tx, TuriTVar *tv) {
    for (int i = 0; i < tx->w_count; i++)
        if (tx->w_tv[i] == tv) return i;
    return -1;
}

static void stm_log_write(TuriStmTx *tx, TuriTVar *tv, int64_t val) {
    int i = stm_write_find(tx, tv);
    if (i >= 0) { tx->w_val[i] = val; return; }
    if (tx->w_count == tx->w_cap) {
        int nc = tx->w_cap ? tx->w_cap * 2 : 8;
        tx->w_tv  = (TuriTVar **)realloc(tx->w_tv,  (size_t)nc * sizeof(*tx->w_tv));
        tx->w_val = (int64_t  *)realloc(tx->w_val, (size_t)nc * sizeof(*tx->w_val));
        tx->w_cap = nc;
    }
    tx->w_tv[tx->w_count]  = tv;
    tx->w_val[tx->w_count] = val;
    tx->w_count++;
}

/* Log-aware read: buffered writes win, else the committed value. */
static int64_t stm_read(TuriStmTx *tx, TuriTVar *tv) {
    int i = stm_write_find(tx, tv);
    return i >= 0 ? tx->w_val[i] : tv->value;
}

/* Resolve a (tvar ...) sub-expression to a live TVar cell, or NULL on error. */
static TuriTVar *stm_eval_tvar(TuriEnv *env, EvalFrame *frame,
                               const Expr *tvar_expr, TuriValue *err_out) {
    TuriValue v = eval_expr(env, frame, tvar_expr);
    if (turi_is_error(v) || env->returning || env->throwing) {
        *err_out = v;
        return NULL;
    }
    return (TuriTVar *)(intptr_t)v.as_int;
}

/* (atomically stm-expr): run the EX_STM body against a fresh transaction and
 * commit on success.  Returns the stm block's last value. */
static TuriValue eval_atomically(TuriEnv *env, EvalFrame *frame,
                                 const Expr *stm_expr) {
    TuriStmTx tx;
    memset(&tx, 0, sizeof(tx));
    tx.prev = g_stm_tx;
    g_stm_tx = &tx;

    TuriValue v = eval_expr(env, frame, stm_expr);

    if (turi_is_error(v) || env->returning || env->throwing) {
        g_stm_tx = tx.prev;
        free(tx.w_tv); free(tx.w_val);
        return v;
    }
    if (tx.retry_requested || tx.aborted) {
        g_stm_tx = tx.prev;
        free(tx.w_tv); free(tx.w_val);
        return turi_error("eval: atomically: transaction requested retry with no "
                          "way to make progress (single-threaded interpreter "
                          "cannot block on another writer)");
    }
    /* Commit: single-threaded validation always succeeds. */
    for (int i = 0; i < tx.w_count; i++) {
        tx.w_tv[i]->value = tx.w_val[i];
        tx.w_tv[i]->version++;
    }
    g_stm_tx = tx.prev;
    free(tx.w_tv); free(tx.w_val);
    return v;
}

/* -------------------------------------------------------------------------
 * Builtin dispatch
 * ---------------------------------------------------------------------- */

/* Returns true if the builtin is blocked for the given capability set. */
static bool is_blocked_builtin(TuriCaps caps, BuiltinShape shape) {
    switch (shape) {
    /* I/O builtins require TURI_CAP_IO */
    case BS_PRINTLN_INT:
    case BS_PRINTLN_FLOAT:
    case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR:
    case BS_PRINTLN_UINT:
    case BS_PRINTLN_FLOAT32:
        return !(caps & TURI_CAP_IO);
    /* FFI builtins require TURI_CAP_FFI */
    case BS_DLOPEN:
    case BS_DLSYM:
    case BS_DLCLOSE:
        return !(caps & TURI_CAP_FFI);
    /* Unsafe memory/pointer builtins require TURI_CAP_UNSAFE */
    case BS_RAW_MALLOC:
    case BS_RAW_FREE:
    case BS_RAW_REALLOC:
    case BS_PTR_DEREF:
    case BS_PTR_WRITE:
    case BS_PTR_ARITH:
    case BS_RAW_MEMSET:
    case BS_RAW_MEMCPY:
    case BS_ARRAY_GET_UNCHECKED:
    case BS_ARRAY_SET_UNCHECKED:
    case BS_UNSAFE_CAST:
    case BS_REINTERPRET:
    case BS_TRANSMUTE:
        return !(caps & TURI_CAP_UNSAFE);
    default:
        return false;
    }
}

static TuriValue eval_builtin(TuriEnv *env, const BuiltinSpec *spec,
                               TuriValue *args, uint32_t n) {
    BuiltinShape shape = spec->shape;

    if (is_blocked_builtin(env->caps, shape)) {
        return turi_error("eval: builtin not allowed in sandboxed environment");
    }

    switch (shape) {

    case BS_VARIADIC_FOLD: {
        if (n == 0) return turi_error("eval: variadic builtin requires ≥1 arg");
        const char *op = spec->c_op;
        bool is_float = (args[0].tag == TURI_FLOAT);
        if (strcmp(op, "+") == 0) {
            if (!is_float) {
                int64_t acc = args[0].as_int;
                for (uint32_t i = 1; i < n; i++) acc += args[i].as_int;
                return turi_int(acc);
            } else {
                double acc = args[0].as_float;
                for (uint32_t i = 1; i < n; i++) acc += args[i].as_float;
                return turi_float(acc);
            }
        }
        if (strcmp(op, "-") == 0) {
            if (!is_float) {
                int64_t acc = args[0].as_int;
                for (uint32_t i = 1; i < n; i++) acc -= args[i].as_int;
                return turi_int(acc);
            } else {
                double acc = args[0].as_float;
                for (uint32_t i = 1; i < n; i++) acc -= args[i].as_float;
                return turi_float(acc);
            }
        }
        if (strcmp(op, "*") == 0) {
            if (!is_float) {
                int64_t acc = args[0].as_int;
                for (uint32_t i = 1; i < n; i++) acc *= args[i].as_int;
                return turi_int(acc);
            } else {
                double acc = args[0].as_float;
                for (uint32_t i = 1; i < n; i++) acc *= args[i].as_float;
                return turi_float(acc);
            }
        }
        return turi_errorf("eval: unknown variadic builtin '%s'", op);
    }

    case BS_DIV_CHECK: {
        bool is_float = (args[0].tag == TURI_FLOAT);
        if (!is_float) {
            if (args[1].as_int == 0) return turi_error("eval: division by zero");
            return turi_int(args[0].as_int / args[1].as_int);
        } else {
            return turi_float(args[0].as_float / args[1].as_float);
        }
    }

    case BS_BIN_INFIX: {
        const char *op = spec->c_op;
        if (strcmp(op, "==") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   == args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float == args[1].as_float);
            if (args[0].tag == TURI_BOOL)  return turi_bool(args[0].as_bool  == args[1].as_bool);
            if (args[0].tag == TURI_NIL)   return turi_bool(args[1].tag == TURI_NIL);
            if (args[0].tag == TURI_CSTR)  {
                if (args[1].tag == TURI_CSTR) {
                    const char *s0 = args[0].as_cstr ? args[0].as_cstr : "";
                    const char *s1 = args[1].as_cstr ? args[1].as_cstr : "";
                    return turi_bool(strcmp(s0, s1) == 0);
                }
                return turi_bool(false);
            }
            /* Fallback: compare as integer representation */
            return turi_bool(args[0].as_int == args[1].as_int);
        }
        if (strcmp(op, "!=") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   != args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float != args[1].as_float);
            if (args[0].tag == TURI_BOOL)  return turi_bool(args[0].as_bool  != args[1].as_bool);
            if (args[0].tag == TURI_NIL)   return turi_bool(args[1].tag != TURI_NIL);
            /* Fallback: compare as integer representation */
            return turi_bool(args[0].as_int != args[1].as_int);
        }
        if (strcmp(op, "<") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   < args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float < args[1].as_float);
        }
        if (strcmp(op, ">") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   > args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float > args[1].as_float);
        }
        if (strcmp(op, "<=") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   <= args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float <= args[1].as_float);
        }
        if (strcmp(op, ">=") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   >= args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float >= args[1].as_float);
        }
        if (strcmp(op, "%") == 0) {
            if (args[1].as_int == 0) return turi_error("eval: modulo by zero");
            return turi_int(args[0].as_int % args[1].as_int);
        }
        /* Phase C bitwise ops (bit-and/or/xor/shl/shr).  Builtins.c registers
         * these for every integer kind; under --interpret every integer is an
         * int64 carrier, so the kind-preserving width masking the compiled path
         * applies is a no-op here -- the int64 result matches for in-range
         * values (the same convention the arithmetic fold path uses). */
        if (strcmp(op, "&")  == 0) return turi_int(args[0].as_int &  args[1].as_int);
        if (strcmp(op, "|")  == 0) return turi_int(args[0].as_int |  args[1].as_int);
        if (strcmp(op, "^")  == 0) return turi_int(args[0].as_int ^  args[1].as_int);
        if (strcmp(op, "<<") == 0) return turi_int(args[0].as_int << args[1].as_int);
        if (strcmp(op, ">>") == 0) return turi_int(args[0].as_int >> args[1].as_int);
        return turi_errorf("eval: unknown infix builtin '%s'", op);
    }

    case BS_PREFIX_UNARY: {
        const char *op = spec->c_op;
        if (op && strcmp(op, "!") == 0) return turi_bool(!args[0].as_bool);
        if (op && strcmp(op, "&") == 0) {
            /* ptr-of: emulate &var by boxing the value into a heap cell */
            int64_t *cell = (int64_t *)malloc(sizeof(int64_t));
            if (!cell) return turi_nil();
            *cell = args[0].as_int;
            TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)cell;
            return v;
        }
        return turi_nil();
    }

    case BS_UNSAFE_CAST:
    case BS_REINTERPRET:
    case BS_TRANSMUTE:
        /* All three are C-style casts; in the interpreter, pass the value through. */
        return args[0];

    case BS_PREFIX_UNARY_FREE:
        /* (drop! x) — no-op in the evaluator */
        return turi_nil();

    case BS_AND_SC: {
        /* Note: args are already evaluated (not truly short-circuit here),
         * but the semantic result is correct for pure boolean expressions. */
        for (uint32_t i = 0; i < n; i++) {
            if (!turi_is_truthy(args[i])) return turi_bool(false);
        }
        return turi_bool(true);
    }

    case BS_OR_SC: {
        for (uint32_t i = 0; i < n; i++) {
            if (turi_is_truthy(args[i])) return turi_bool(true);
        }
        return turi_bool(false);
    }

    case BS_PRINTLN_INT:
    case BS_PRINTLN_FLOAT:
    case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR:
    case BS_PRINTLN_UINT:
    case BS_PRINTLN_FLOAT32: {
        /* Dispatch on runtime tag so eval mode works despite type-inference gaps. */
        TuriValue a = args[0];
        switch (a.tag) {
        case TURI_CSTR:  puts(a.as_cstr ? a.as_cstr : ""); break;
        case TURI_BOOL:  puts(a.as_bool ? "true" : "false"); break;
        case TURI_FLOAT: printf("%g\n", a.as_float); break;
        case TURI_INT:
        default:
            if (spec->shape == BS_PRINTLN_UINT)
                printf("%llu\n", (unsigned long long)(uint64_t)a.as_int);
            else if (spec->shape == BS_PRINTLN_FLOAT32)
                printf("%.7g\n", a.as_float);
            else
                printf("%lld\n", (long long)a.as_int);
            break;
        }
        return turi_nil();
    }

    /* --- Unsafe pointer/memory operations -------------------------------- */
    case BS_RAW_MALLOC: {
        int64_t sz = args[0].as_int;
        void *p = malloc((size_t)(sz > 0 ? sz : 0));
        TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p;
        return v;
    }
    case BS_RAW_FREE: {
        void *p = (void *)(intptr_t)args[0].as_int;
        if (p) free(p);
        return turi_nil();
    }
    case BS_PTR_DEREF: {
        int64_t *p = (int64_t *)(intptr_t)args[0].as_int;
        if (!p) return turi_nil();
        TuriValue v = {0}; v.tag = TURI_INT; v.as_int = *p;
        return v;
    }
    case BS_PTR_WRITE: {
        int64_t *p = (int64_t *)(intptr_t)args[0].as_int;
        if (p) *p = args[1].as_int;
        return turi_nil();
    }
    case BS_PTR_ARITH: {
        /* ptr-add/ptr-sub: raw byte arithmetic, matching emitted C: (char*)p +/- off */
        intptr_t base = (intptr_t)args[0].as_int;
        int64_t  off  = args[1].as_int;
        intptr_t res  = (spec->c_op && spec->c_op[0] == '-')
                        ? base - (intptr_t)off
                        : base + (intptr_t)off;
        TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)res;
        return v;
    }
    case BS_RAW_MEMSET: {
        void *p = (void *)(intptr_t)args[0].as_int;
        int  c  = (int)args[1].as_int;
        size_t n = (size_t)args[2].as_int;
        if (p) memset(p, c, n);
        return turi_nil();
    }
    case BS_RAW_MEMCPY: {
        void *dst = (void *)(intptr_t)args[0].as_int;
        void *src = (void *)(intptr_t)args[1].as_int;
        size_t n  = (size_t)args[2].as_int;
        if (dst && src) memcpy(dst, src, n);
        return turi_nil();
    }
    case BS_ARRAY_GET_UNCHECKED: {
        /* *((int64_t *)arr + idx) */
        int64_t *arr = (int64_t *)(intptr_t)args[0].as_int;
        int64_t  idx = args[1].as_int;
        TuriValue v = {0}; v.tag = TURI_INT;
        v.as_int = arr ? arr[idx] : 0;
        return v;
    }
    case BS_ARRAY_SET_UNCHECKED: {
        /* *((int64_t *)arr + idx) = val */
        int64_t *arr = (int64_t *)(intptr_t)args[0].as_int;
        int64_t  idx = args[1].as_int;
        int64_t  val = args[2].as_int;
        if (arr) arr[idx] = val;
        return turi_nil();
    }

    default: {
        /* Context-capturing continuation resume / clone / marshal builtins
         * (tur_*_cont_*) arrive here as BS_FUNC_CALL; handle them before the
         * generic nil fallback. */
        TuriValue cont_out;
        if (ts_try_cont_builtin(env, spec, args, n, &cont_out)) return cont_out;
        /* Silently return nil for unsupported builtins (unsafe ops, STM, etc.) */
        return turi_nil();
    }
    }
}

/* =========================================================================
 * Minimal inline-C body executor for common patterns.
 *
 * Recognizes and executes: free, switch-case-string, simple constructors
 * (malloc + field assignments), and simple accessors (cast ptr + return field).
 * Returns true and sets *out if a pattern was recognized; false otherwise.
 * ========================================================================= */

/* Process C string escape sequences in-place: "\\n" -> "\n" etc. */
static void ic_unescape_str(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
                case 'n':  *w++ = '\n'; r++; break;
                case 't':  *w++ = '\t'; r++; break;
                case 'r':  *w++ = '\r'; r++; break;
                case '\\': *w++ = '\\'; r++; break;
                case '"':  *w++ = '"';  r++; break;
                case '0':  *w++ = '\0'; r++; break;
                default:   *w++ = '\\'; *w++ = *r++; break;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static const char *ic_skip_ws(const char *p) {
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    return p;
}

static bool ic_word_eq(const char *p, const char *word) {
    size_t n = strlen(word);
    return strncmp(p, word, n) == 0 && !isalnum((unsigned char)p[n]) && p[n] != '_';
}

static int ic_read_ident(const char **p, char *out, int out_max) {
    *p = ic_skip_ws(*p);
    const char *s = *p;
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    int n = 0;
    while ((isalnum((unsigned char)*s) || *s == '_') && n < out_max - 1)
        out[n++] = *s++;
    out[n] = '\0';
    *p = s;
    return n;
}

static int ic_param_idx(FnDef *fn, const char *name, uint32_t param_offset) {
    for (uint32_t i = param_offset; i < fn->n_params; i++) {
        if (strcmp(fn->params[i]->name->name, name) == 0)
            return (int)(i - param_offset);
    }
    return -1;
}

/* Forward declarations for struct field helpers used by ic_eval_assign_expr */
#define IC_MAX_FIELDS 10
static int ic_extract_struct_fields_typed(const char *body,
                                           const char *out_names[IC_MAX_FIELDS],
                                           size_t      out_lens[IC_MAX_FIELDS],
                                           int         out_types[IC_MAX_FIELDS]);
static int ic_field_index(const char *fname, size_t flen,
                           const char *names[], size_t lens[], int n);
static int ic_common_field_idx(const char *fname, size_t flen);

/* Precedence-climbing evaluator forward decl (mutually recursive with the
 * operand reader for unary operators and parenthesised sub-expressions). */
static bool ic_eval_binexpr(const char **pp, int min_prec,
                            FnDef *fn, uint32_t param_offset,
                            TuriValue *args, uint32_t n_args,
                            int64_t *out_val, const char *ic_body);

/* Classify a binary operator at p.  Returns its C-like precedence (higher
 * binds tighter) and sets *oplen to the token length; 0 means "not a binary
 * operator we evaluate."  Two-character operators are matched before their
 * one-character prefixes so `<=`/`<<` are not mistaken for `<`. */
static int ic_binop_prec(const char *p, int *oplen) {
    switch (p[0]) {
    case '|': if (p[1] == '|') { *oplen = 2; return 1; } *oplen = 1; return 3;
    case '&': if (p[1] == '&') { *oplen = 2; return 2; } *oplen = 1; return 5;
    case '^': *oplen = 1; return 4;
    case '=': if (p[1] == '=') { *oplen = 2; return 6; } return 0;
    case '!': if (p[1] == '=') { *oplen = 2; return 6; } return 0;
    case '<': if (p[1] == '=') { *oplen = 2; return 7; }
              if (p[1] == '<') { *oplen = 2; return 8; }
              *oplen = 1; return 7;
    case '>': if (p[1] == '=') { *oplen = 2; return 7; }
              if (p[1] == '>') { *oplen = 2; return 8; }
              *oplen = 1; return 7;
    case '+': *oplen = 1; return 9;
    case '-': *oplen = 1; return 9;
    case '*': case '/': case '%': *oplen = 1; return 10;
    default: return 0;
    }
}

/* Apply a binary operator (integer semantics). */
static int64_t ic_apply_binop(const char *op, int oplen, int64_t a, int64_t b) {
    if (oplen == 2) {
        if (op[0] == '=' && op[1] == '=') return a == b;
        if (op[0] == '!' && op[1] == '=') return a != b;
        if (op[0] == '<' && op[1] == '=') return a <= b;
        if (op[0] == '>' && op[1] == '=') return a >= b;
        if (op[0] == '<' && op[1] == '<') return a << b;
        if (op[0] == '>' && op[1] == '>') return a >> b;
        if (op[0] == '&' && op[1] == '&') return a && b;
        if (op[0] == '|' && op[1] == '|') return a || b;
        return 0;
    }
    switch (op[0]) {
    case '+': return a + b;  case '-': return a - b;
    case '*': return a * b;  case '/': return b ? a / b : 0;
    case '%': return b ? a % b : 0;
    case '<': return a < b;  case '>': return a > b;
    case '&': return a & b;  case '|': return a | b;  case '^': return a ^ b;
    default:  return 0;
    }
}

/* Evaluate a single primary operand at *pp, advancing *pp past it.  Handles
 * unary !/~/-, casts like (T)/(int64_t)(intptr_t), true/false/NULL/int
 * literals, and a parameter reference with optional .field / ->field access.
 * ic_body is the full inline-C body (for struct field extraction); may be NULL. */
static bool ic_eval_operand(const char **pp,
                            FnDef *fn, uint32_t param_offset,
                            TuriValue *args, uint32_t n_args,
                            int64_t *out_val, const char *ic_body) {
    const char *p = ic_skip_ws(*pp);

    /* Unary operators. */
    if (*p == '!' && p[1] != '=') {
        const char *np = p + 1; int64_t v;
        if (!ic_eval_operand(&np, fn, param_offset, args, n_args, &v, ic_body)) return false;
        *out_val = !v; *pp = np; return true;
    }
    if (*p == '~') {
        const char *np = p + 1; int64_t v;
        if (!ic_eval_operand(&np, fn, param_offset, args, n_args, &v, ic_body)) return false;
        *out_val = ~v; *pp = np; return true;
    }
    if (*p == '-' && !isdigit((unsigned char)ic_skip_ws(p + 1)[0])) {
        const char *np = p + 1; int64_t v;
        if (!ic_eval_operand(&np, fn, param_offset, args, n_args, &v, ic_body)) return false;
        *out_val = -v; *pp = np; return true;
    }

    /* Strip any number of casts like (T), (T*), (int64_t)(intptr_t), etc.  A
     * leading '(' followed by an identifier is treated as a cast; genuine
     * grouping parens are not common in these inline-C bodies. */
    while (*p == '(') {
        const char *q = p + 1; q = ic_skip_ws(q);
        if (isalpha((unsigned char)*q) || *q == '_') {
            const char *q2 = q;
            while (*q2 && *q2 != ')') q2++;
            if (*q2 == ')') { p = ic_skip_ws(q2 + 1); continue; }
        }
        break;
    }

    /* Literals. */
    if (ic_word_eq(p, "true"))  { *out_val = 1; *pp = p + 4; return true; }
    if (ic_word_eq(p, "false")) { *out_val = 0; *pp = p + 5; return true; }
    if (ic_word_eq(p, "NULL"))  { *out_val = 0; *pp = p + 4; return true; }
    if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
        char *end; int64_t v = strtoll(p, &end, 0);
        if (end > p) { *out_val = v; *pp = end; return true; }
    }

    /* Parameter reference, optionally followed by .field / ->field access. */
    char ident[64];
    const char *q = p;
    if (ic_read_ident(&q, ident, sizeof(ident)) > 0) {
        int idx = ic_param_idx(fn, ident, param_offset);
        if (idx >= 0 && (uint32_t)idx < n_args) {
            TuriValue *arg = &args[idx];
            /* Check for param.field (dot access on TuriStruct value) */
            const char *r = ic_skip_ws(q);
            if (*r == '.' && r[1] != '.') {
                r++;
                char field_name[64];
                const char *r2 = r;
                if (ic_read_ident(&r2, field_name, sizeof(field_name)) > 0) {
                    size_t flen = strlen(field_name);
                    if (arg->tag == TURI_STRUCT && arg->as_struct) {
                        /* TuriStruct: look up field index from StructDef, body struct, or common table */
                        int fidx = -1;
                        /* 1. Use StructDef field names if available */
                        if (fidx < 0 && arg->as_struct->def) {
                            StructDef *sdef = arg->as_struct->def;
                            for (uint32_t fi = 0; fi < sdef->n_fields && fi < arg->as_struct->n_fields; fi++) {
                                if (sdef->fields[fi].name && strcmp(sdef->fields[fi].name, field_name) == 0) {
                                    fidx = (int)fi; break;
                                }
                            }
                        }
                        /* 2. Try body struct definition */
                        if (fidx < 0) {
                            const char *snames[IC_MAX_FIELDS]; size_t slens[IC_MAX_FIELDS]; int stypes[IC_MAX_FIELDS];
                            int sn = ic_body ? ic_extract_struct_fields_typed(ic_body, snames, slens, stypes) : 0;
                            if (sn > 0) fidx = ic_field_index(field_name, flen, snames, slens, sn);
                        }
                        /* 3. Common field name table */
                        if (fidx < 0) fidx = ic_common_field_idx(field_name, flen);
                        if (fidx >= 0 && (uint32_t)fidx < arg->as_struct->n_fields) {
                            *out_val = arg->as_struct->fields[fidx].as_int;
                            *pp = r2; return true;
                        }
                    } else if (arg->tag == TURI_INT && arg->as_int != 0) {
                        /* Pointer to struct, access via common field index */
                        int fidx = ic_common_field_idx(field_name, flen);
                        if (fidx >= 0) {
                            int64_t *ptr = (int64_t*)(intptr_t)arg->as_int;
                            *out_val = ptr[fidx];
                            *pp = r2; return true;
                        }
                    }
                }
            }
            /* Check for param->field (arrow access on pointer parameter) */
            if (r[0] == '-' && r[1] == '>') {
                r += 2;
                char field_name[64];
                const char *r2 = r;
                if (ic_read_ident(&r2, field_name, sizeof(field_name)) > 0) {
                    size_t flen = strlen(field_name);
                    if (arg->tag == TURI_INT && arg->as_int != 0) {
                        const char *snames[IC_MAX_FIELDS]; size_t slens[IC_MAX_FIELDS];
                        int stypes[IC_MAX_FIELDS];
                        int sn = ic_body ? ic_extract_struct_fields_typed(ic_body, snames, slens, stypes) : 0;
                        int fidx = (sn > 0) ? ic_field_index(field_name, flen, snames, slens, sn) : -1;
                        if (fidx < 0) fidx = ic_common_field_idx(field_name, flen);
                        if (fidx >= 0) {
                            int64_t *ptr = (int64_t*)(intptr_t)arg->as_int;
                            *out_val = ptr[fidx];
                            *pp = r2; return true;
                        }
                    }
                }
            }
            *out_val = arg->as_int;
            *pp = q; return true;
        }
    }
    return false;
}

/* Precedence-climbing evaluator: parse `operand (op operand)*` honouring C
 * operator precedence, left-associatively.  Returns false (no partial result)
 * the moment any sub-operand fails to parse. */
static bool ic_eval_binexpr(const char **pp, int min_prec,
                            FnDef *fn, uint32_t param_offset,
                            TuriValue *args, uint32_t n_args,
                            int64_t *out_val, const char *ic_body) {
    int64_t lhs;
    if (!ic_eval_operand(pp, fn, param_offset, args, n_args, &lhs, ic_body))
        return false;
    for (;;) {
        const char *p = ic_skip_ws(*pp);
        int oplen = 0;
        int prec  = ic_binop_prec(p, &oplen);
        if (prec == 0 || prec < min_prec) break;
        *pp = p + oplen;
        int64_t rhs;
        if (!ic_eval_binexpr(pp, prec + 1, fn, param_offset, args, n_args, &rhs, ic_body))
            return false;
        lhs = ic_apply_binop(p, oplen, lhs, rhs);
    }
    *out_val = lhs;
    return true;
}

/* Parse and evaluate a simple inline-C value expression: a parameter, a
 * literal, a field access, or a binary-operator expression over those.
 * ic_body is the full inline-C body (for struct field extraction); may be NULL.
 *
 * Fail-closed: if anything other than trailing whitespace / a single ';'
 * remains after the expression, return false so the caller falls back to the
 * honest "inline-C not supported" path rather than silently returning a wrong
 * value.  (Previously this dropped any trailing binary operator -- e.g.
 * `return p != 0;` evaluated to `p` -- a silent miscompile; see
 * docs/reported/turi-inline-c-ignores-comparison-operator.md.) */
static bool ic_eval_assign_expr(const char *expr,
                                 FnDef *fn, uint32_t param_offset,
                                 TuriValue *args, uint32_t n_args,
                                 int64_t *out_val,
                                 const char *ic_body) {
    const char *p = expr;
    int64_t v;
    if (!ic_eval_binexpr(&p, 0, fn, param_offset, args, n_args, &v, ic_body))
        return false;
    p = ic_skip_ws(p);
    if (*p != '\0' && *p != ';') return false;
    *out_val = v;
    return true;
}

/* Extract struct field names from a "struct { ... }" definition in body. */
/* Field type hint for ic_extract_struct_fields */
#define IC_FT_INT  0  /* int64_t, size_t, int, uint32_t, etc. */
#define IC_FT_BOOL 1  /* bool, _Bool */
#define IC_FT_CSTR 2  /* const char *, char * */

static int ic_extract_struct_fields_typed(const char *body,
                                           const char *out_names[IC_MAX_FIELDS],
                                           size_t      out_lens[IC_MAX_FIELDS],
                                           int         out_types[IC_MAX_FIELDS]) {
    int n = 0;
    const char *p = strstr(body, "struct");
    if (!p) return 0;
    p = strchr(p, '{'); if (!p) return 0; p++;
    const char *end = strchr(p, '}'); if (!end) return 0;
    while (p < end && n < IC_MAX_FIELDS) {
        p = ic_skip_ws(p); if (p >= end) break;
        const char *semi = (const char *)memchr(p, ';', (size_t)(end - p));
        if (!semi) break;
        /* Determine field type from declaration start */
        int ftype = IC_FT_INT;
        const char *decl = ic_skip_ws(p);
        if (ic_word_eq(decl,"bool")||ic_word_eq(decl,"_Bool")) ftype=IC_FT_BOOL;
        else if ((ic_word_eq(decl,"const")&&strstr(decl,"char")&&strchr(decl,'*')) ||
                 (ic_word_eq(decl,"char")&&strchr(decl,'*'))) ftype=IC_FT_CSTR;
        /* Extract field name: last identifier before ';' */
        const char *fe = semi;
        while (fe>p&&(fe[-1]==' '||fe[-1]=='\t'||fe[-1]=='\n'||fe[-1]=='\r')) fe--;
        const char *fs = fe;
        while (fs>p&&(isalnum((unsigned char)fs[-1])||fs[-1]=='_')) fs--;
        if (fs < fe) {
            out_names[n] = fs; out_lens[n] = (size_t)(fe-fs);
            if (out_types) out_types[n] = ftype;
            n++;
        }
        p = semi + 1;
    }
    return n;
}

static int ic_field_index(const char *fname, size_t flen,
                           const char *names[], size_t lens[], int n) {
    for (int i = 0; i < n; i++)
        if (lens[i]==flen && strncmp(names[i],fname,flen)==0) return i;
    return -1;
}

/* Common field-name → index fallback table */
static int ic_common_field_idx(const char *fname, size_t flen) {
    static const struct { const char *name; int idx; } tbl[] = {
        {"is_some",0},{"value",1},{"is_ok",0},{"ok_val",1},{"err_val",2},
        {"message",0},{"what",0},{"path",1},{"field",1},
        {"p",0},{"len",1},{"data",0},{"n",1},{"items",0},{"cap",2},
        {"errno_",1},{"cx",3},{"cy",4},{"width",1},{"height",2},
        {NULL,-1}
    };
    for (int i = 0; tbl[i].name; i++) {
        size_t nl = strlen(tbl[i].name);
        if (flen == nl && strncmp(fname, tbl[i].name, nl) == 0) return tbl[i].idx;
    }
    return -1;
}

/* Execute free pattern */
static TuriValue ic_exec_free(TuriValue *args, uint32_t n_args) {
    if (n_args >= 1) { void *p = (void*)(intptr_t)args[0].as_int; if (p) free(p); }
    return turi_nil();
}

/* True iff `body` contains a standalone `free(` call (the destructor shape this
 * matcher can evaluate) -- i.e. a `free(` token whose preceding character is not
 * part of a longer identifier.  Without the word-boundary check a plain substring
 * match also claims bodies that merely call `tur_hamt_iter_free(`, `xfree(`, etc.
 * -- which `ic_exec_free` would then mis-handle by free()ing arg0 and returning
 * nil, a silent miscompile and (for HAMT iterators) a heap-use-after-free. */
static bool ic_has_standalone_free(const char *body) {
    const char *p = body;
    while ((p = strstr(p, "free(")) != NULL) {
        unsigned char prev = (p == body) ? '\0' : (unsigned char)p[-1];
        if (!(isalnum(prev) || prev == '_')) return true;
        p += 5;
    }
    return false;
}

/* Execute switch-case string: switch(arg0) { case V: return "str"; ... } */
static TuriValue ic_exec_switch_string(const char *body,
                                        TuriValue *args, uint32_t n_args) {
    if (n_args < 1) return turi_nil();
    int64_t sel = args[0].as_int;
    const char *p = strstr(body, "switch"); if (!p) return turi_nil();
    p = strchr(p, '{'); if (!p) return turi_nil(); p++;
    const char *default_str = NULL; size_t default_len = 0;
    while (*p) {
        p = ic_skip_ws(p);
        if (*p == '}') break;
        if (ic_word_eq(p, "case")) {
            p += 4; p = ic_skip_ws(p);
            char *end; long long v = strtoll(p, &end, 0); p = end;
            p = ic_skip_ws(p); if (*p == ':') p++;
            p = ic_skip_ws(p);
            if (ic_word_eq(p, "return")) {
                p += 6; p = ic_skip_ws(p);
                if (*p == '"') {
                    const char *ss = p+1;
                    const char *se = ss;
                    while (*se && *se!='"') { if(*se=='\\') se++; se++; }
                    if (v == sel) {
                        char *buf = (char*)malloc((size_t)(se-ss)+1);
                        if (!buf) return turi_nil();
                        memcpy(buf, ss, (size_t)(se-ss));
                        buf[se-ss] = '\0';
                        return turi_cstr(buf);
                    }
                    p = se+1;
                    while (*p && *p!=';') { p++; } if(*p) { p++; }
                    continue;
                }
            }
        } else if (ic_word_eq(p, "default")) {
            p += 7; p = ic_skip_ws(p); if(*p==':') p++;
            p = ic_skip_ws(p);
            if (ic_word_eq(p, "return")) {
                p += 6; p = ic_skip_ws(p);
                if (*p == '"') {
                    default_str = p+1;
                    const char *se = default_str;
                    while (*se && *se!='"') { if(*se=='\\') se++; se++; }
                    default_len = (size_t)(se - default_str);
                }
            }
            while (*p && *p!='}') p++;
        } else { while (*p && *p!=';' && *p!='}') p++; if(*p==';') p++; }
    }
    if (default_str) {
        char *buf = (char*)malloc(default_len+1);
        if (!buf) return turi_nil();
        memcpy(buf, default_str, default_len); buf[default_len] = '\0';
        return turi_cstr(buf);
    }
    return turi_nil();
}

/* Execute constructor pattern: malloc + field assignments + return ptr. */
/* W4 (turi-inline-c-silent-miscompiles.md): helpers for the constructor
 * matcher's refuse-rather-than-guess guards. */

/* Whole-word presence of `kw` in `body`. */
static bool ic_body_has_word(const char *body, const char *kw) {
    size_t n = strlen(kw);
    for (const char *p = body; (p = strstr(p, kw)) != NULL; p += n) {
        bool l = (p == body) || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
        bool r = (!isalnum((unsigned char)p[n]) && p[n] != '_');
        if (l && r) return true;
    }
    return false;
}

/* Count of substring occurrences of `sub` in `body`. */
static int ic_body_count_sub(const char *body, const char *sub) {
    int c = 0; size_t n = strlen(sub);
    for (const char *p = body; (p = strstr(p, sub)) != NULL; p += n) c++;
    return c;
}

/* W4: true if a malloc/calloc-based body contains control flow or multi-step
 * constructs a *flat* constructor never has.  The constructor matcher models
 * only "alloc one struct, assign its fields, return it"; a body with a loop,
 * branch (multiple returns), a second allocation, atomics/I-O, or a
 * function-pointer apply means the real semantics exceed that, and silently
 * building a single struct from the first alloc would miscompile (rc=0, wrong
 * answer).  Declining lets try_exec_simple_inline_c fall through to the clean
 * "inline-C not supported" error instead. */
static bool ic_constructor_unmodelable(const char *body) {
    if (ic_body_has_word(body, "while") || ic_body_has_word(body, "for")  ||
        ic_body_has_word(body, "do")    || ic_body_has_word(body, "switch")||
        ic_body_has_word(body, "goto"))
        return true;
    if (strstr(body, "__atomic") || strstr(body, "TUR_APPLY"))
        return true;
    /* >1 allocation => a loop/branch builds many cells, not one flat struct.
     * (A single early `return 0;` OOM guard before one malloc is fine -- it is a
     * dead path when the allocation succeeds -- so we do NOT reject on return
     * count; the loop/foreign-pointer guards already catch the real multi-cell
     * builders.) */
    if (ic_body_count_sub(body, "malloc(") + ic_body_count_sub(body, "calloc(") > 1)
        return true;
    return false;
}

/* W4: true if the body dereferences (via `->`) any pointer OTHER than the
 * allocated target `varname`.  A flat constructor only writes `varname->field`;
 * reading another pointer's fields (e.g. `r->e1 = s->e1 + 100;`) is
 * pointer-chasing the matcher cannot faithfully evaluate -- it would read the
 * wrong value -- so decline. */
static bool ic_constructor_chases_foreign_ptr(const char *body, const char *varname) {
    size_t vlen = strlen(varname);
    for (const char *p = strstr(body, "->"); p; p = strstr(p + 2, "->")) {
        const char *e = p;
        while (e > body && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) e--;
        const char *s = e;
        while (s > body && (isalnum((unsigned char)s[-1]) || s[-1]=='_')) s--;
        size_t len = (size_t)(e - s);
        if (len == 0) continue;  /* e.g. `)->` from a cast; conservatively skip */
        if (len != vlen || strncmp(s, varname, len) != 0)
            return true;
    }
    return false;
}

static TuriValue ic_exec_constructor(const char *body,
                                      TuriValue *args, uint32_t n_args,
                                      FnDef *fn, uint32_t param_offset) {
    /* Special case: string fat-pointer constructor (->p and ->len via strlen/while) */
    if ((strstr(body,"strlen")||strstr(body,"while")) &&
         strstr(body,"->p") && strstr(body,"->len") && n_args >= 1) {
        const char *cstr = (args[0].tag==TURI_CSTR) ? args[0].as_cstr
                                                     : (const char*)(intptr_t)args[0].as_int;
        size_t len = cstr ? strlen(cstr) : 0;
        int64_t *s = (int64_t*)malloc(2*sizeof(int64_t));
        if (!s) return turi_nil();
        s[0] = (int64_t)(intptr_t)cstr; s[1] = (int64_t)len;
        TuriValue v={0}; v.tag=TURI_INT; v.as_int=(int64_t)(intptr_t)s; return v;
    }

    /* Find malloc call and the variable name preceding it */
    const char *malloc_p = strstr(body, "malloc(");
    if (!malloc_p) malloc_p = strstr(body, "calloc(");
    if (!malloc_p) return turi_nil();
    /* W4: decline bodies whose semantics exceed a flat constructor. */
    if (ic_constructor_unmodelable(body)) return turi_nil();
    /* Scan backwards for variable name.
     * Pattern: "TYPE *varname = (optional_cast)malloc("
     * Start from the char before 'm' in malloc. */
    const char *q = malloc_p;
    if (q > body) q--; /* step before 'm' */
    /* Skip whitespace and closing parens of a cast like (ValidationError*) */
    while (q > body && (*q==' '||*q=='\t'||*q=='\n'||*q=='\r')) q--;
    while (q > body && *q == ')') {
        q--; int d=1;
        while (q>body && d>0) { if(*q==')') d++; else if(*q=='(') d--; q--; }
        while (q>body && (*q==' '||*q=='\t'||*q=='\n'||*q=='\r')) q--;
    }
    /* Now skip '=' and leading whitespace and pointer stars */
    if (q > body && *q == '=') q--;
    while (q > body && (*q==' '||*q=='\t'||*q=='\n'||*q=='\r'||*q=='*')) q--;
    const char *name_end = q + 1;
    while (q > body && (isalnum((unsigned char)q[-1])||q[-1]=='_')) q--;
    int vname_len = (int)(name_end - q);
    char varname[64];
    if (vname_len<=0||vname_len>=64) return turi_nil();
    memcpy(varname, q, (size_t)vname_len); varname[vname_len]='\0';

    /* W4: decline if the body chases a pointer other than the alloc target. */
    if (ic_constructor_chases_foreign_ptr(body, varname)) return turi_nil();

    /* Scan past malloc line */
    const char *p = malloc_p;
    p = strchr(p,'('); if (!p) return turi_nil();
    int depth = 1; p++;
    while (*p && depth>0) { if(*p=='(') depth++; else if(*p==')') depth--; p++; }
    while (*p && *p!=';') { p++; } if (*p) { p++; }

    /* Collect field assignments: varname->field = expr; */
    char arrow_buf[80];
    if (snprintf(arrow_buf,sizeof(arrow_buf),"%s->",varname) >= (int)sizeof(arrow_buf))
        return turi_nil();

    int64_t field_vals[IC_MAX_FIELDS]; int n_fields=0;

    /* Also check for p[i] = constant; (array index constructor) */
    char idx_buf[80];
    snprintf(idx_buf,sizeof(idx_buf),"%s[",varname);

    while (*p) {
        p = ic_skip_ws(p);
        if (!*p || *p=='}') break;
        if (ic_word_eq(p,"return")) break;

        /* varname->field = expr; */
        if (strncmp(p, arrow_buf, strlen(arrow_buf))==0) {
            if (n_fields>=IC_MAX_FIELDS) break;
            p += strlen(arrow_buf);
            while (isalnum((unsigned char)*p)||*p=='_') p++; /* skip field name */
            p = ic_skip_ws(p);
            if (*p!='=') { while(*p&&*p!=';') p++; if(*p) p++; continue; }
            p++; p = ic_skip_ws(p);
            const char *expr_start = p;
            int d=0;
            while (*p&&(*p!=';'||d>0)) {
                if(*p=='(') d++; else if(*p==')') d--;
                else if(*p=='"') {p++;while(*p&&*p!='"'){if(*p=='\\')p++;p++;}}
                p++;
            }
            char expr_buf[256];
            int elen=(int)(p-expr_start);
            while (elen>0&&(expr_start[elen-1]==' '||expr_start[elen-1]=='\t'||
                            expr_start[elen-1]=='\n'||expr_start[elen-1]=='\r')) elen--;
            int64_t fval=0;
            if (elen>0&&elen<(int)sizeof(expr_buf)) {
                memcpy(expr_buf,expr_start,(size_t)elen); expr_buf[elen]='\0';
                ic_eval_assign_expr(expr_buf,fn,param_offset,args,n_args,&fval,body);
            }
            field_vals[n_fields++]=fval;
            if(*p) p++;
            continue;
        }

        /* varname[i] = constant; (array index constructor) */
        if (strncmp(p, idx_buf, strlen(idx_buf))==0) {
            if (n_fields>=IC_MAX_FIELDS) break;
            p += strlen(idx_buf);
            char *end; long long idx = strtoll(p, &end, 0); p = end;
            if (*p!=']') { while(*p&&*p!=';') p++; if(*p) p++; continue; }
            p++; p=ic_skip_ws(p);
            if (*p!='=') { while(*p&&*p!=';') p++; if(*p) p++; continue; }
            p++; p=ic_skip_ws(p);
            const char *expr_start=p;
            int d=0;
            while(*p&&(*p!=';'||d>0)) {
                if(*p=='(') d++; else if(*p==')') d--;
                else if(*p=='"') {p++;while(*p&&*p!='"'){if(*p=='\\')p++;p++;}}
                p++;
            }
            char expr_buf[256];
            int elen=(int)(p-expr_start);
            while(elen>0&&(expr_start[elen-1]==' '||expr_start[elen-1]=='\t'||
                           expr_start[elen-1]=='\n'||expr_start[elen-1]=='\r')) elen--;
            int64_t fval=0;
            if (elen>0&&elen<(int)sizeof(expr_buf)) {
                memcpy(expr_buf,expr_start,(size_t)elen); expr_buf[elen]='\0';
                ic_eval_assign_expr(expr_buf,fn,param_offset,args,n_args,&fval,body);
            }
            /* idx must match n_fields (sequential) */
            if ((long long)n_fields==idx) field_vals[n_fields++]=fval;
            else if (idx>=0&&idx<IC_MAX_FIELDS) {
                /* non-sequential index: grow to idx+1 */
                while (n_fields <= (int)idx) field_vals[n_fields++]=0;
                field_vals[idx]=fval;
            }
            if(*p) p++;
            continue;
        }

        /* Skip other statements */
        int d2=0;
        while(*p&&(*p!=';'||d2>0)) {
            if(*p=='(') d2++; else if(*p==')') d2--;
            else if(*p=='"') {p++;while(*p&&*p!='"'){if(*p=='\\')p++;p++;}}
            else if(*p=='{') d2++;
            else if(*p=='}') { if(d2>0) d2--; else break; }
            p++;
        }
        if(*p==';') p++;
    }

    if (n_fields==0) return turi_nil();
    int64_t *mem = (int64_t*)malloc((size_t)n_fields*sizeof(int64_t));
    if (!mem) return turi_nil();
    for (int i=0;i<n_fields;i++) mem[i]=field_vals[i];
    TuriValue v={0}; v.tag=TURI_INT; v.as_int=(int64_t)(intptr_t)mem; return v;
}

/* Execute accessor: cast arg to ptr, return field[idx].
 * fn is used to check the declared return type. */
static TuriValue ic_exec_accessor(const char *body,
                                   TuriValue *args, uint32_t n_args,
                                   FnDef *fn) {
    if (n_args < 1) return turi_nil();

    /* W4 (turi-inline-c-silent-miscompiles.md): refuse-rather-than-guess.
     * A body that *applies* a function pointer (TUR_APPLY*) is not a field
     * accessor -- e.g. ArrowApply's `return TUR_APPLY1(s->e1, s->e2);` applies
     * the arrow in slot e1 to the arg in slot e2.  The accessor matcher would
     * mis-read it as a bare `s->e1` field and return the raw function pointer
     * (rc=0, wrong: arrow-instance-apply printed pointer addresses instead of
     * 42/42/1007).  We cannot perform the application here, so decline and let
     * the clean "inline-C not supported" error fire. */
    if (strstr(body, "TUR_APPLY")) return turi_nil();

    /* W4: decline accessors with statement-level branching / side effects the
     * single-field-read model cannot follow: a side-effecting error path
     * (`fprintf`, e.g. ls-get's bounds check) or an `if (...) return ...;` EARLY
     * return (panic-msg, ls-get) -- the matcher models only one field read or a
     * `?:` ternary (a single return), so with >1 return it silently picks the
     * wrong branch (rc=0, wrong answer).  The kept ternary/`field ? field : def`
     * shapes have exactly one return. */
    {
        int nret = 0;
        for (const char *p = body; (p = strstr(p, "return")) != NULL; p += 6) {
            bool l = (p == body) || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
            bool rr = (!isalnum((unsigned char)p[6]) && p[6] != '_');
            if (l && rr) nret++;
        }
        if (strstr(body, "fprintf") || nret > 1) return turi_nil();
    }

    /* Extract struct field names and types from body */
    const char *fnames[IC_MAX_FIELDS]; size_t flens[IC_MAX_FIELDS];
    int ftypes[IC_MAX_FIELDS];
    int nf = ic_extract_struct_fields_typed(body, fnames, flens, ftypes);

    /* Find return statement */
    const char *ret = strstr(body, "return "); if (!ret) return turi_nil();
    ret += 7; ret = ic_skip_ws(ret);

    /* Strip casts */
    while (*ret == '(') {
        const char *q2 = ret+1; q2=ic_skip_ws(q2);
        if (isalpha((unsigned char)*q2)||*q2=='_') {
            const char *e=q2; while(*e&&*e!=')') e++;
            if(*e==')') { ret=ic_skip_ws(e+1); continue; }
        }
        break;
    }

    /* Handle ternary: "return var ? var->field : fallback;"
     * The var is null-checked before deref */
    {
        const char *t = ret;
        char var1[64], var2[64], field2[64];
        const char *t2 = t;
        int n1 = ic_read_ident(&t2, var1, sizeof(var1));
        if (n1 > 0) {
            const char *t3 = ic_skip_ws(t2);
            if (*t3 == '?') {
                t3++; t3 = ic_skip_ws(t3);
                /* skip truthy part: var2->field */
                const char *t4 = t3;
                int n2 = ic_read_ident(&t4, var2, sizeof(var2));
                if (n2 > 0 && t4[0]=='-' && t4[1]=='>') {
                    t4 += 2;
                    const char *t5 = t4;
                    int n3 = ic_read_ident(&t5, field2, sizeof(field2));
                    if (n3 > 0) {
                        /* skip to colon */
                        while (*t5 && *t5 != ':') t5++;
                        if (*t5 == ':') {
                            t5++; t5 = ic_skip_ws(t5);
                            /* parse fallback value */
                            int64_t fallback = 0;
                            if (isdigit((unsigned char)*t5) || (*t5=='-'&&isdigit((unsigned char)t5[1]))) {
                                char *end2; fallback = strtoll(t5, &end2, 0);
                            }
                            /* Check if args[0] is null */
                            int64_t ptr_val = args[0].as_int;
                            if (ptr_val == 0) {
                                TuriValue rv={0}; rv.tag=TURI_INT; rv.as_int=fallback; return rv;
                            }
                            /* Access field on the non-null pointer */
                            size_t flen3 = strlen(field2);
                            int fidx3 = (nf>0) ? ic_field_index(field2,flen3,fnames,flens,nf) : -1;
                            if (fidx3 < 0) fidx3 = ic_common_field_idx(field2, flen3);
                            if (fidx3 >= 0) {
                                int64_t *ptr3 = (int64_t*)(intptr_t)ptr_val;
                                int64_t fv3 = ptr3[fidx3];
                                int ft3 = (fidx3 < nf) ? ftypes[fidx3] : IC_FT_INT;
                                if (ft3 == IC_FT_BOOL) { TuriValue rv={0}; rv.tag=TURI_BOOL; rv.as_bool=(bool)(fv3!=0); return rv; }
                                if (ft3 == IC_FT_CSTR) { TuriValue rv={0}; rv.tag=TURI_CSTR; rv.as_cstr=(const char*)(intptr_t)fv3; return rv; }
                                TuriValue rv={0}; rv.tag=TURI_INT; rv.as_int=fv3; return rv;
                            }
                        }
                    }
                }
            }
        }
    }

    /* W4 (turi-interpreter-gap-closure-plan): refuse-rather-than-miscompile.
     * The field-access paths below extract a single `ptr->field` value.  If the
     * return expression is actually a boolean/relational combination over that
     * field -- e.g. `return p == NULL || !p->is_ok;` -- reading the bare field
     * silently drops the `== NULL`, `||`, and `!` and returns the WRONG answer
     * (rc=0).  We cannot faithfully evaluate such expressions here, so decline:
     * returning turi_nil makes try_exec_simple_inline_c fall through to the
     * clean "inline-C not supported" error instead of a silent miscompile.
     * (The `var ? var->field : fallback` and `field ? field : "def"` shapes are
     * already handled above / below and contain none of these operators.)
     * See docs/reported/turi-inline-c-accessor-miscompiles-boolean-returns.md. */
    for (const char *p = ret; *p && *p != ';'; p++) {
        if (p[0] == '-' && p[1] == '>') { p++; continue; }   /* skip the arrow */
        if ((p[0] == '|' && p[1] == '|') ||                  /* || */
            (p[0] == '&' && p[1] == '&') ||                  /* && */
            (p[0] == '=' && p[1] == '=') ||                  /* == */
            p[0] == '!' ||                                   /* != or unary ! */
            p[0] == '<' || p[0] == '>') {                    /* < > (arrow skipped) */
            return turi_nil();
        }
    }

    /* Skip variable name and look for -> or . */
    const char *varstart = ret;
    while (isalnum((unsigned char)*ret)||*ret=='_') ret++;
    size_t varlen = (size_t)(ret - varstart);

    /* Handle "return var.field;" (TuriStruct value field access) */
    if (*ret == '.' && ret[1] != '.') {
        ret++;
        const char *fn_start2 = ret;
        while (isalnum((unsigned char)*ret)||*ret=='_') ret++;
        size_t fn_len2 = (size_t)(ret - fn_start2);
        /* Find which param is named 'var' */
        char varname2[64];
        if (varlen < sizeof(varname2)) {
            memcpy(varname2, varstart, varlen); varname2[varlen]='\0';
            int pidx2 = ic_param_idx(fn, varname2, 0);
            if (pidx2 < 0 && fn) pidx2 = ic_param_idx(fn, varname2, fn->n_params > 0 ? 0 : 0);
            TuriValue *src = (pidx2 >= 0 && (uint32_t)pidx2 < n_args) ? &args[pidx2] : &args[0];
            int fidx2 = (nf>0) ? ic_field_index(fn_start2, fn_len2, fnames, flens, nf) : -1;
            if (fidx2 < 0) fidx2 = ic_common_field_idx(fn_start2, fn_len2);
            if (fidx2 >= 0) {
                int64_t fv2; int ft2;
                if (src->tag == TURI_STRUCT && src->as_struct && (uint32_t)fidx2 < src->as_struct->n_fields) {
                    fv2 = src->as_struct->fields[fidx2].as_int;
                    ft2 = IC_FT_INT;
                } else if (src->tag == TURI_INT && src->as_int != 0) {
                    int64_t *ptr2 = (int64_t*)(intptr_t)src->as_int;
                    fv2 = ptr2[fidx2];
                    ft2 = (fidx2 < nf) ? ftypes[fidx2] : IC_FT_INT;
                } else { return turi_nil(); }
                if (ft2 == IC_FT_BOOL) { TuriValue rv={0}; rv.tag=TURI_BOOL; rv.as_bool=(bool)(fv2!=0); return rv; }
                if (ft2 == IC_FT_CSTR) { TuriValue rv={0}; rv.tag=TURI_CSTR; rv.as_cstr=(const char*)(intptr_t)fv2; return rv; }
                TuriValue rv={0}; rv.tag=TURI_INT; rv.as_int=fv2; return rv;
            }
        }
        return turi_nil();
    }

    /* Fallback: find -> anywhere in the return expression; try to find param via (intptr_t)PARAM */
    if (ret[0]!='-'||ret[1]!='>') {
        /* Look for "->field_name" in the return expression */
        const char *arrow_pos = strstr(varstart, "->");
        if (!arrow_pos) return turi_nil();
        const char *fns2 = arrow_pos+2;
        char fn_name2[64]; int fn_len2_i = 0;
        while ((isalnum((unsigned char)*fns2)||*fns2=='_') && fn_len2_i<63) fn_name2[fn_len2_i++]=*fns2++;
        fn_name2[fn_len2_i] = '\0';
        if (fn_len2_i == 0) return turi_nil();
        size_t fn_len2 = (size_t)fn_len2_i;
        int fidx2 = (nf>0)?ic_field_index(fn_name2,fn_len2,fnames,flens,nf):-1;
        if (fidx2<0) fidx2=ic_common_field_idx(fn_name2,fn_len2);
        if (fidx2<0) return turi_nil();
        /* Look for which param is used: scan for (intptr_t)PARAM in the return expr */
        int64_t ptr_val = n_args > 0 ? args[0].as_int : 0;
        const char *itp = strstr(varstart, "intptr_t)");
        if (itp) {
            itp += 9;
            while (*itp==' '||*itp=='\t') itp++;
            char pname[64]; int plen=0;
            while ((isalnum((unsigned char)*itp)||*itp=='_') && plen<63) pname[plen++]=*itp++;
            pname[plen]='\0';
            if (plen>0) {
                int pidx2=ic_param_idx(fn,pname,0);
                if (pidx2>=0&&(uint32_t)pidx2<n_args) ptr_val=args[pidx2].as_int;
            }
        }
        if (ptr_val == 0) return turi_nil();
        int64_t *ptr2=(int64_t*)(intptr_t)ptr_val;
        int64_t fv2=ptr2[fidx2];
        int ft2=(fidx2<nf)?ftypes[fidx2]:IC_FT_INT;
        if (ft2==IC_FT_BOOL){TuriValue rv={0};rv.tag=TURI_BOOL;rv.as_bool=(bool)(fv2!=0);return rv;}
        if (ft2==IC_FT_CSTR){TuriValue rv={0};rv.tag=TURI_CSTR;rv.as_cstr=(const char*)(intptr_t)fv2;return rv;}
        TuriValue rv={0};rv.tag=TURI_INT;rv.as_int=fv2;return rv;
    }
    /* Expect -> for pointer dereference */
    if (ret[0]!='-'||ret[1]!='>') return turi_nil();
    ret += 2;

    const char *fn_start = ret;
    while (isalnum((unsigned char)*ret)||*ret=='_') ret++;
    size_t fn_len = (size_t)(ret - fn_start);

    /* Find field index */
    int fidx = (nf>0) ? ic_field_index(fn_start,fn_len,fnames,flens,nf) : -1;
    /* If arg is TURI_STRUCT with StructDef, use field names from StructDef */
    if (fidx < 0 && args[0].tag == TURI_STRUCT && args[0].as_struct && args[0].as_struct->def) {
        StructDef *sdef = args[0].as_struct->def;
        for (uint32_t fi = 0; fi < sdef->n_fields; fi++) {
            if (sdef->fields[fi].name && strncmp(sdef->fields[fi].name, fn_start, fn_len) == 0
                && strlen(sdef->fields[fi].name) == fn_len) { fidx = (int)fi; break; }
        }
    }
    if (fidx < 0) fidx = ic_common_field_idx(fn_start, fn_len);
    if (fidx < 0) return turi_nil();

    /* Get pointer value: from TURI_INT (raw pointer) or TURI_STRUCT */
    int64_t field_val;
    int ftype;
    if (args[0].tag == TURI_STRUCT && args[0].as_struct) {
        /* TuriStruct: use struct field directly */
        if ((uint32_t)fidx >= args[0].as_struct->n_fields) return turi_nil();
        field_val = args[0].as_struct->fields[fidx].as_int;
        ftype = IC_FT_INT;
    } else {
        if (args[0].as_int == 0) return turi_nil();
        int64_t *ptr = (int64_t*)(intptr_t)args[0].as_int;
        field_val = ptr[fidx];
        ftype = (fidx < nf) ? ftypes[fidx] : IC_FT_INT;
    }
    /* Determine field type: from struct definition or field-name heuristic */
    /* Heuristic overrides for well-known bool/cstr fields */
    if ((fn_len==5&&strncmp(fn_start,"is_ok",5)==0)||
        (fn_len==7&&strncmp(fn_start,"is_some",7)==0))  ftype = IC_FT_BOOL;

    /* Null-or-default pattern: "return ptr->field ? ptr->field : \"default\";" */
    const char *q = ic_skip_ws(ret);
    if (*q == '?') {
        if (field_val == 0) {
            q++; q=ic_skip_ws(q);
            /* Skip truthy part to colon */
            int d=0;
            while(*q&&(*q!=':'||d>0)) {
                if(*q=='(') d++; else if(*q==')') d--;
                else if(*q=='"') {q++;while(*q&&*q!='"'){if(*q=='\\')q++;q++;}}
                q++;
            }
            if(*q==':') { q++; q=ic_skip_ws(q); }
            if(*q=='"') {
                const char *se=q+1;
                while(*se&&*se!='"') { if(*se=='\\') se++; se++; }
                char *buf=(char*)malloc((size_t)(se-(q+1))+1);
                if (!buf) return turi_nil();
                size_t slen=(size_t)(se-(q+1));
                memcpy(buf,q+1,slen); buf[slen]='\0';
                return turi_cstr(buf);
            }
        }
        /* field_val != 0: fall through to return field_val with type */
    }

    if (ftype == IC_FT_BOOL) {
        TuriValue rv={0}; rv.tag=TURI_BOOL; rv.as_bool=(bool)(field_val!=0); return rv;
    }
    if (ftype == IC_FT_CSTR) {
        TuriValue rv={0}; rv.tag=TURI_CSTR;
        rv.as_cstr=(const char*)(intptr_t)field_val; return rv;
    }
    TuriValue rv={0}; rv.tag=TURI_INT; rv.as_int=field_val; return rv;
}

/* Execute snprintf-based formatter: handles bodies that do conditional early returns and
 * then snprintf(buf, size, "format", args...) + return buf. Useful for Show instances. */
/* Format a single snprintf(bufvar, size, "format", args...) call whose 's' is
 * at fp ("snprintf(" expected). Returns the formatted cstr TuriValue, or
 * turi_nil() if the call does not target bufvar or cannot be parsed. */
/* W4: true if a printf-style format string contains a FLOAT conversion
 * (e/E/f/F/g/G/a/A).  ic_format_snprintf_call passes every arg as (long long),
 * so a float conversion would read integer bits as a double (garbage, e.g.
 * show-float printed 2.122e-314 for 3.14).  The matcher cannot format floats,
 * so such a body is declined. */
static bool ic_fmt_has_float_conv(const char *fmt) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*lhLqjzt", *p)) p++;
        if (*p && strchr("eEfFgGaA", *p)) return true;
    }
    return false;
}

static TuriValue ic_format_snprintf_call(const char *fp,
                                         const char *bufvar, int bvlen,
                                         TuriValue *args, uint32_t n_args,
                                         FnDef *fn, uint32_t param_offset,
                                         const char *body) {
    const char *fq = fp + 9;
    fq = ic_skip_ws(fq);
    const char *bvs = fq;
    while (isalnum((unsigned char)*fq)||*fq=='_') fq++;
    if ((int)(fq-bvs)!=bvlen || strncmp(bvs,bufvar,(size_t)bvlen)!=0) return turi_nil();
    /* skip comma + size arg */
    fq=ic_skip_ws(fq); if(*fq==',') fq++;
    fq=ic_skip_ws(fq); int d=0;
    while(*fq&&(*fq!=','||d>0)){if(*fq=='(')d++;else if(*fq==')')d--;fq++;}
    if(*fq==',') fq++;
    fq=ic_skip_ws(fq);
    if(*fq!='"') return turi_nil();
    /* extract format string */
    const char *fmts=fq+1, *fmte=fmts;
    while(*fmte&&*fmte!='"'){if(*fmte=='\\')fmte++;fmte++;}
    char fmt_str[512]; size_t fmt_len=(size_t)(fmte-fmts);
    if(fmt_len>=sizeof(fmt_str)) return turi_nil();
    memcpy(fmt_str,fmts,fmt_len); fmt_str[fmt_len]='\0';
    ic_unescape_str(fmt_str);
    /* W4: decline float conversions -- args are passed as (long long). */
    if (ic_fmt_has_float_conv(fmt_str)) return turi_nil();
    fq=fmte+1;
    /* parse snprintf arguments */
    int64_t sn_args[8]; int sn_argc=0;
    while(*fq&&*fq!=')'&&sn_argc<8) {
        if(*fq==',') fq++;
        fq=ic_skip_ws(fq); if(*fq==')') break;
        const char *as=fq; int d2=0;
        while(*fq&&((*fq!=','&&*fq!=')')||d2>0)) {
            if(*fq=='(')d2++;else if(*fq==')')d2--;
            else if(*fq=='"'){fq++;while(*fq&&*fq!='"'){if(*fq=='\\')fq++;fq++;}}
            if(*fq)fq++;
        }
        int alen=(int)(fq-as);
        while(alen>0&&(as[alen-1]==' '||as[alen-1]=='\t'))alen--;
        if(alen>0&&alen<256){
            char abuf[256]; memcpy(abuf,as,(size_t)alen); abuf[alen]='\0';
            int64_t val=0;
            ic_eval_assign_expr(abuf,fn,param_offset,args,n_args,&val,body);
            sn_args[sn_argc++]=val;
        }
    }
    /* format the result */
    char result_buf[1024]; int rlen=0;
    switch(sn_argc){
        case 0: rlen=snprintf(result_buf,sizeof(result_buf),"%s",fmt_str); break;
        case 1: rlen=snprintf(result_buf,sizeof(result_buf),fmt_str,(long long)sn_args[0]); break;
        case 2: rlen=snprintf(result_buf,sizeof(result_buf),fmt_str,(long long)sn_args[0],(long long)sn_args[1]); break;
        case 3: rlen=snprintf(result_buf,sizeof(result_buf),fmt_str,(long long)sn_args[0],(long long)sn_args[1],(long long)sn_args[2]); break;
        case 4: rlen=snprintf(result_buf,sizeof(result_buf),fmt_str,(long long)sn_args[0],(long long)sn_args[1],(long long)sn_args[2],(long long)sn_args[3]); break;
        default: return turi_nil();
    }
    if(rlen<0) return turi_nil();
    char *out=(char*)malloc((size_t)rlen+1);
    if(!out) return turi_nil();
    memcpy(out,result_buf,(size_t)rlen+1);
    TuriValue rv={0}; rv.tag=TURI_CSTR; rv.as_cstr=out; return rv;
}

/* Resolve  if (COND) snprintf(bufvar ...); else snprintf(bufvar ...);  by
 * evaluating COND and formatting only the live branch's snprintf. Returns the
 * formatted cstr, or turi_nil() if no such guarded snprintf pair is found (the
 * caller then falls back to the linear first-match scan). */
static TuriValue ic_snprintf_cond_branch(const char *body,
                                         const char *bufvar, int bvlen,
                                         TuriValue *args, uint32_t n_args,
                                         FnDef *fn, uint32_t param_offset) {
    const char *p = body;
    while ((p = strstr(p, "if")) != NULL) {
        /* word-boundary check around "if" */
        bool lb = (p==body) || (!isalnum((unsigned char)p[-1]) && p[-1]!='_');
        bool rb = (!isalnum((unsigned char)p[2]) && p[2]!='_');
        if (!lb || !rb) { p += 2; continue; }
        const char *q = ic_skip_ws(p+2);
        if (*q != '(') { p += 2; continue; }
        /* extract the condition between the matched parens */
        q++;
        const char *cstart = q; int depth = 1;
        while (*q && depth) { if (*q=='(') depth++; else if (*q==')') depth--; if (depth) q++; }
        if (*q != ')') { p += 2; continue; }
        int clen = (int)(q - cstart);
        if (clen <= 0 || clen >= 256) { p += 2; continue; }
        char condbuf[256]; memcpy(condbuf, cstart, (size_t)clen); condbuf[clen] = '\0';
        int64_t cv = 0; const char *cp = condbuf;
        if (!ic_eval_binexpr(&cp, 0, fn, param_offset, args, n_args, &cv, body)) { p += 2; continue; }
        cp = ic_skip_ws(cp);
        if (*cp != '\0') { p += 2; continue; }  /* condition not fully understood */
        const char *cons = ic_skip_ws(q + 1);   /* consequent */
        const char *cons_sf = strstr(cons, "snprintf(");
        if (!cons_sf) { p += 2; continue; }
        const char *chosen_sf;
        if (cv) {
            chosen_sf = cons_sf;
        } else {
            /* take the snprintf after the matching else */
            const char *e = strstr(cons, "else");
            if (!e) { p += 2; continue; }
            chosen_sf = strstr(e + 4, "snprintf(");
            if (!chosen_sf) { p += 2; continue; }
        }
        TuriValue rv = ic_format_snprintf_call(chosen_sf, bufvar, bvlen, args, n_args, fn, param_offset, body);
        if (rv.tag == TURI_CSTR) return rv;
        p += 2;
    }
    return turi_nil();
}

static TuriValue ic_exec_snprintf_fmt(const char *body,
                                       TuriValue *args, uint32_t n_args,
                                       FnDef *fn, uint32_t param_offset) {
    /* Step 1: Find buffer variable name by scanning all return statements for
     * one that returns a variable (not a string literal or 0). Take the last such return. */
    char bufvar[64]; int bvlen = 0;
    {
        const char *scan = body;
        while ((scan = strstr(scan, "return ")) != NULL) {
            const char *r2 = scan + 7; r2 = ic_skip_ws(r2);
            /* strip casts */
            while (*r2 == '(') {
                const char *q2 = r2+1; q2 = ic_skip_ws(q2);
                if (isalpha((unsigned char)*q2) || *q2 == '_') {
                    const char *e2 = q2; while (*e2 && *e2 != ')') e2++;
                    if (*e2 == ')') { r2 = ic_skip_ws(e2+1); continue; }
                }
                break;
            }
            /* If this return is an identifier (variable name), remember it */
            if (isalpha((unsigned char)*r2) || *r2 == '_') {
                char tmp[64]; int tlen = 0;
                while ((isalnum((unsigned char)*r2)||*r2=='_') && tlen < 63) tmp[tlen++] = *r2++;
                tmp[tlen] = '\0';
                /* Skip keywords */
                if (strcmp(tmp,"true")&&strcmp(tmp,"false")&&strcmp(tmp,"NULL")) {
                    memcpy(bufvar, tmp, (size_t)tlen+1); bvlen = tlen;
                }
            }
            scan++;
        }
    }
    if (bvlen == 0) return turi_nil();

    /* W4 (turi-inline-c-silent-miscompiles.md): refuse-rather-than-guess for
     * shapes this single-write formatter cannot model faithfully:
     *   - a loop formats many elements (show-list: "[1" instead of "[1]");
     *   - a pointer-chasing arg the arg-evaluator cannot resolve (exg5: "0"
     *     instead of "99", from `*(int64_t*)((Rc*)x)->value`);
     *   - concatenation via `snprintf(bufvar + off, ...)` where only the first
     *     write is modeled (show-pair: "(0" instead of "(1, 2)").
     * A guarded if/else snprintf PAIR (range-bound) writes to `bufvar` (no
     * offset) and is resolved by ic_snprintf_cond_branch below, so it is kept. */
    if (ic_body_has_word(body, "while") || ic_body_has_word(body, "for"))
        return turi_nil();
    if (strstr(body, "->"))
        return turi_nil();
    for (const char *sc = strstr(body, "snprintf("); sc; sc = strstr(sc + 9, "snprintf(")) {
        const char *a = ic_skip_ws(sc + 9);
        if (strncmp(a, bufvar, (size_t)bvlen) == 0) {
            const char *aa = ic_skip_ws(a + bvlen);
            if (*aa == '+') return turi_nil();  /* offset write => concatenation */
        }
    }

    /* Step 2: Handle conditional early returns: if (COND) return "str"; */
    const char *p = body;
    while (*p) {
        p = ic_skip_ws(p);
        if (!ic_word_eq(p, "if")) { while (*p && *p != ';' && *p != '{') p++; if(*p) p++; continue; }
        const char *q = p + 2; q = ic_skip_ws(q);
        if (*q != '(') { p++; continue; }
        q++; q = ic_skip_ws(q);
        bool negate = false;
        if (*q == '!') { negate = true; q++; q = ic_skip_ws(q); }
        char cond_param[64], cond_field[64]; cond_field[0] = '\0';
        int cp_len = 0, cf_len = 0;
        while ((isalnum((unsigned char)*q)||*q=='_') && cp_len<63) cond_param[cp_len++]=*q++;
        cond_param[cp_len] = '\0';
        if (*q == '.') { q++; while ((isalnum((unsigned char)*q)||*q=='_') && cf_len<63) cond_field[cf_len++]=*q++; cond_field[cf_len]='\0'; }
        while (*q && *q != ')') q++;
        if (*q == ')') q++;
        q = ic_skip_ws(q);
        if (!ic_word_eq(q, "return")) { p += 2; continue; }
        q += 6; q = ic_skip_ws(q);
        if (*q != '"') { p += 2; continue; }
        const char *ss = q+1, *se = ss;
        while (*se && *se != '"') { if (*se == '\\') se++; se++; }
        /* Evaluate condition */
        int64_t cond_val = 0;
        if (cond_field[0]) {
            int pidx = ic_param_idx(fn, cond_param, param_offset);
            if (pidx >= 0 && (uint32_t)pidx < n_args) {
                TuriValue *arg = &args[pidx];
                size_t flen = strlen(cond_field);
                const char *snames[IC_MAX_FIELDS]; size_t slens[IC_MAX_FIELDS]; int stypes[IC_MAX_FIELDS];
                int sn = ic_extract_struct_fields_typed(body, snames, slens, stypes);
                int fidx = (sn>0)?ic_field_index(cond_field,flen,snames,slens,sn):-1;
                /* Also try StructDef field names */
                if (fidx<0 && arg->tag==TURI_STRUCT && arg->as_struct && arg->as_struct->def) {
                    StructDef *sdef=arg->as_struct->def;
                    for (uint32_t fi=0; fi<sdef->n_fields; fi++) {
                        if (sdef->fields[fi].name && strcmp(sdef->fields[fi].name,cond_field)==0) { fidx=(int)fi; break; }
                    }
                }
                if (fidx<0) fidx=ic_common_field_idx(cond_field,flen);
                if (fidx>=0) {
                    if (arg->tag==TURI_STRUCT&&arg->as_struct&&(uint32_t)fidx<arg->as_struct->n_fields)
                        cond_val=arg->as_struct->fields[fidx].as_int;
                    else if (arg->tag==TURI_INT&&arg->as_int)
                        cond_val=((int64_t*)(intptr_t)arg->as_int)[fidx];
                }
            }
        } else if (cp_len > 0) {
            int pidx = ic_param_idx(fn, cond_param, param_offset);
            if (pidx >= 0 && (uint32_t)pidx < n_args) cond_val = args[pidx].as_int;
        }
        if (negate ? (cond_val==0) : (cond_val!=0)) {
            size_t elen = (size_t)(se-ss);
            char *buf = (char*)malloc(elen+1);
            if (!buf) return turi_nil();
            memcpy(buf, ss, elen); buf[elen] = '\0';
            TuriValue rv={0}; rv.tag=TURI_CSTR; rv.as_cstr=buf; return rv;
        }
        p += 2;
    }

    /* Step 3: Find the snprintf(bufvar, size, "format", args...) that actually
     * runs.  A bare scan-for-first-snprintf is wrong when the body guards two
     * snprintf calls behind an if/else (e.g. range-bound's
     *   if (kind == 1) snprintf(buf, 32, "[%lld", v);
     *   else           snprintf(buf, 32, "(%lld", v);
     * ) -- always taking the first emits "[7" where the Exclusive branch wants
     * "(7" (see docs/reported/turi-pure-turi-silent-miscompiles.md). So first
     * try to resolve a guarding if/else and format only the live branch; fall
     * back to the linear "first matching snprintf" scan otherwise. */
    {
        TuriValue cond = ic_snprintf_cond_branch(body, bufvar, bvlen, args, n_args, fn, param_offset);
        if (cond.tag == TURI_CSTR) return cond;
    }
    const char *sp = body;
    while (*sp) {
        const char *fp = strstr(sp, "snprintf(");
        if (!fp) break;
        TuriValue rv = ic_format_snprintf_call(fp, bufvar, bvlen, args, n_args, fn, param_offset, body);
        if (rv.tag == TURI_CSTR) return rv;
        sp=fp+1;
    }
    return turi_nil();
}

/* Execute string comparison of two fat-pointer {const char *p; size_t len;} structs */
static TuriValue ic_exec_str_cmp(TuriValue *args, uint32_t n_args) {
    if (n_args < 2) return turi_bool(false);
    int64_t *a = (int64_t*)(intptr_t)args[0].as_int;
    int64_t *b = (int64_t*)(intptr_t)args[1].as_int;
    if (!a && !b) return turi_bool(true);
    if (!a || !b) return turi_bool(false);
    int64_t la = a[1], lb = b[1];
    if (la != lb) return turi_bool(false);
    const char *pa = (const char*)(intptr_t)a[0];
    const char *pb = (const char*)(intptr_t)b[0];
    if (!pa && !pb) return turi_bool(true);
    if (!pa || !pb) return turi_bool(false);
    return turi_bool(memcmp(pa, pb, (size_t)la) == 0);
}

/* Execute a while-loop linked-list traversal with printf.
 * Pattern: Cell *var = (cast)param; while(var){ printf(fmt, var->field); var=(cast)var->next; } */
static TuriValue ic_exec_linked_list_print(const char *body,
                                            TuriValue *args, uint32_t n_args,
                                            FnDef *fn, uint32_t param_offset) {
    /* Find while keyword */
    const char *wp = body;
    while ((wp = strstr(wp, "while")) != NULL) {
        if (wp > body && (isalnum((unsigned char)wp[-1])||wp[-1]=='_')) { wp++; continue; }
        break;
    }
    if (!wp) return turi_nil();
    const char *wq = wp + 5; wq = ic_skip_ws(wq);
    if (*wq != '(') return turi_nil();
    wq++; wq = ic_skip_ws(wq);
    /* Extract loop variable name (condition is just the pointer: while (c)) */
    char loopvar[64]; int lvlen = 0;
    while ((isalnum((unsigned char)*wq)||*wq=='_') && lvlen<63) loopvar[lvlen++] = *wq++;
    loopvar[lvlen] = '\0';
    if (lvlen == 0) return turi_nil();
    while (*wq && *wq != ')') wq++;
    if (*wq == ')') wq++;
    wq = ic_skip_ws(wq);
    if (*wq != '{') return turi_nil();
    wq++;

    /* Extract struct field definitions for index lookup */
    const char *snames[IC_MAX_FIELDS]; size_t slens[IC_MAX_FIELDS]; int stypes[IC_MAX_FIELDS];
    int sn = ic_extract_struct_fields_typed(body, snames, slens, stypes);

    /* Find printf(fmt, (cast)loopvar->print_field); */
    const char *pp = strstr(wq, "printf(");
    if (!pp) return turi_nil();
    const char *pfq = pp + 7; pfq = ic_skip_ws(pfq);
    if (*pfq != '"') return turi_nil();
    const char *fmts = pfq+1, *fmte = fmts;
    while (*fmte && *fmte != '"') { if (*fmte=='\\') fmte++; fmte++; }
    char fmt_str[128]; size_t fmt_len = (size_t)(fmte-fmts);
    if (fmt_len >= sizeof(fmt_str)) return turi_nil();
    memcpy(fmt_str, fmts, fmt_len); fmt_str[fmt_len] = '\0';
    ic_unescape_str(fmt_str);
    pfq = fmte+1;
    if (*pfq == ',') pfq++;
    pfq = ic_skip_ws(pfq);
    /* Strip casts */
    while (*pfq == '(') {
        const char *cq = pfq+1; cq=ic_skip_ws(cq);
        if (isalpha((unsigned char)*cq)||*cq=='_') {
            const char *ce=cq; while(*ce&&*ce!=')') ce++;
            if (*ce==')') { pfq=ic_skip_ws(ce+1); continue; }
        }
        break;
    }
    /* loopvar->print_field */
    char pvar[64]; int pvlen=0;
    while ((isalnum((unsigned char)*pfq)||*pfq=='_')&&pvlen<63) pvar[pvlen++]=*pfq++;
    pvar[pvlen]='\0';
    if (strcmp(pvar,loopvar)!=0||pfq[0]!='-'||pfq[1]!='>') return turi_nil();
    pfq+=2;
    char print_field[64]; int pflen=0;
    while ((isalnum((unsigned char)*pfq)||*pfq=='_')&&pflen<63) print_field[pflen++]=*pfq++;
    print_field[pflen]='\0';

    /* Find advance: loopvar = (cast)loopvar->next_field; */
    const char *ap = wq;
    while ((ap = strstr(ap, loopvar)) != NULL) {
        if (ap > wq && (isalnum((unsigned char)ap[-1])||ap[-1]=='_')) { ap++; continue; }
        const char *aq = ap + lvlen; aq=ic_skip_ws(aq);
        if (*aq != '=') { ap++; continue; }
        aq++; aq=ic_skip_ws(aq);
        /* strip casts */
        while (*aq=='(') {
            const char *cq=aq+1; cq=ic_skip_ws(cq);
            if (isalpha((unsigned char)*cq)||*cq=='_') {
                const char *ce=cq; while(*ce&&*ce!=')') ce++;
                if(*ce==')') { aq=ic_skip_ws(ce+1); continue; }
            }
            break;
        }
        char avar[64]; int avlen=0;
        while ((isalnum((unsigned char)*aq)||*aq=='_')&&avlen<63) avar[avlen++]=*aq++;
        avar[avlen]='\0';
        if (strcmp(avar,loopvar)!=0||aq[0]!='-'||aq[1]!='>') { ap++; continue; }
        aq+=2;
        char next_field[64]; int nflen=0;
        while ((isalnum((unsigned char)*aq)||*aq=='_')&&nflen<63) next_field[nflen++]=*aq++;
        next_field[nflen]='\0';
        if (nflen==0) { ap++; continue; }

        /* Get field indices */
        size_t pfl=strlen(print_field), nfl=strlen(next_field);
        int pidx=(sn>0)?ic_field_index(print_field,pfl,snames,slens,sn):-1;
        if (pidx<0) pidx=ic_common_field_idx(print_field,pfl);
        int nidx=(sn>0)?ic_field_index(next_field,nfl,snames,slens,sn):-1;
        if (nidx<0) nidx=ic_common_field_idx(next_field,nfl);
        if (pidx<0||nidx<0) { ap++; continue; }

        /* Find initial value: search for "loopvar = (cast)param" before while */
        int64_t init_val = n_args > 0 ? args[0].as_int : 0;
        /* Search for "*loopvar = (cast)param_name;" in body before wp */
        char init_pat[80]; snprintf(init_pat, sizeof(init_pat), "*%s =", loopvar);
        const char *ip = strstr(body, init_pat);
        if (ip && ip < wp) {
            ip += strlen(init_pat); ip=ic_skip_ws(ip);
            while (*ip=='(') {
                const char *cq=ip+1; cq=ic_skip_ws(cq);
                if (isalpha((unsigned char)*cq)||*cq=='_') {
                    const char *ce=cq; while(*ce&&*ce!=')') ce++;
                    if(*ce==')') { ip=ic_skip_ws(ce+1); continue; }
                }
                break;
            }
            char ivar[64]; int ivlen=0;
            while ((isalnum((unsigned char)*ip)||*ip=='_')&&ivlen<63) ivar[ivlen++]=*ip++;
            ivar[ivlen]='\0';
            if (ivlen>0) {
                int64_t tmp;
                if (ic_eval_assign_expr(ivar,fn,param_offset,args,n_args,&tmp,body)) init_val=tmp;
            }
        }

        /* Execute linked list traversal */
        int64_t *cur=(int64_t*)(intptr_t)init_val;
        int safety=100000;
        while (cur && safety-->0) {
            int64_t pval=cur[pidx];
            char line[256];
            int llen=snprintf(line,sizeof(line),fmt_str,(long long)pval);
            if (llen>0) { fwrite(line,1,(size_t)llen,stdout); fflush(stdout); }
            cur=(int64_t*)(intptr_t)cur[nidx];
        }
        return turi_nil();
    }
    return turi_nil();
}

/* Top-level dispatcher */
/* Diagnostic groundwork for the inline-C silent-miscompile tightening (W4, see
 * docs/reported/turi-inline-c-silent-miscompiles.md): when TUR_IC_TRACE is set,
 * log which ic_exec_* matcher claimed a body and what it returned. This makes
 * the per-cluster "refuse-rather-than-guess" work tractable -- you can see at a
 * glance which matcher mis-claims each fixture, and confirm a tightening flips a
 * mis-claim to "unclaimed" (clean error) without disturbing a correct claim.
 * Off the trace path it is a single cached-flag branch -- zero normal overhead. */
static bool ic_claim(const char *pattern, FnDef *fn, const TuriValue *out) {
    static int on = -1;
    if (on < 0) on = getenv("TUR_IC_TRACE") ? 1 : 0;
    if (on) {
        const char *nm = (fn && fn->binding) ? fn->binding->name->name : "<fn>";
        if (out)
            fprintf(stderr, "[ic-trace] %-24s claimed by %-18s -> tag=%d int=%lld\n",
                    nm, pattern, (int)out->tag, (long long)out->as_int);
        else
            fprintf(stderr, "[ic-trace] %-24s claimed by %s\n", nm, pattern);
    }
    return true;
}

static bool try_exec_simple_inline_c(TuriEnv *env,
                                      const char *body, size_t blen,
                                      TuriValue *args, uint32_t n_args,
                                      FnDef *fn, uint32_t param_offset,
                                      TuriValue *out) {
    (void)env; (void)blen;
    if (!body || !*body) return false;

    bool has_malloc = strstr(body,"malloc(") || strstr(body,"calloc(");
    bool has_free   = ic_has_standalone_free(body);
    bool has_arrow  = strstr(body,"->");
    bool has_fptr   = strstr(body,"(*)(");
    bool has_switch = strstr(body,"switch") && strstr(body,"case ");
    bool has_return = strstr(body,"return ");

    /* Pattern 1: Free -- only a bare destructor (`free(p);`), not a body that
     * also computes/returns a value or fat-dispatches a closure (those merely
     * happen to contain a `*_free(` token and must not be reduced to free(arg0)). */
    if (has_free && !has_malloc && !has_return && !has_fptr) {
        *out = ic_exec_free(args, n_args);
        return ic_claim("free", fn, out);
    }

    /* Pattern 2: Switch-case string */
    if (has_switch && strstr(body,"return \"") && !has_fptr) {
        *out = ic_exec_switch_string(body, args, n_args);
        return ic_claim("switch-string", fn, out);
    }

    /* Pattern 3: String fat-pointer comparison (->len && ->p[i]) */
    if (!has_malloc && has_arrow && n_args>=2 &&
        strstr(body,"->len") && strstr(body,"->p") &&
        (strstr(body,"return true")||strstr(body,"return false"))) {
        *out = ic_exec_str_cmp(args, n_args);
        return ic_claim("str-cmp", fn, out);
    }

    /* Pattern 4a: snprintf formatter (malloc + snprintf + return cstr, no arrow in return) */
    if (has_malloc && !has_fptr && strstr(body,"snprintf(")) {
        TuriValue r = ic_exec_snprintf_fmt(body, args, n_args, fn, param_offset);
        if (r.tag != TURI_NIL) { *out = r; return ic_claim("snprintf", fn, out); }
    }

    /* Pattern 4: Constructor (malloc + arrow or index assignments, no fptr cast) */
    if (has_malloc && !has_fptr) {
        TuriValue r = ic_exec_constructor(body, args, n_args, fn, param_offset);
        if (r.tag != TURI_NIL) { *out = r; return ic_claim("constructor", fn, out); }
    }

    /* Pattern 5: Accessor (no malloc, has arrow, has return) */
    if (!has_malloc && has_return && !has_fptr) {
        TuriValue r = ic_exec_accessor(body, args, n_args, fn);
        if (r.tag != TURI_NIL) { *out = r; return ic_claim("accessor", fn, out); }
    }

    /* Pattern 6: Linked list traversal with printf (while loop + printf + ->next) */
    bool has_printf = strstr(body,"printf(") != NULL;
    bool has_while  = strstr(body,"while") != NULL;
    if (!has_malloc && has_arrow && has_while && has_printf && !has_fptr) {
        TuriValue r = ic_exec_linked_list_print(body, args, n_args, fn, param_offset);
        /* Always claim handled if we have a while+printf pattern (even if nil) */
        if (has_while && has_printf) { *out = r; return ic_claim("linked-list-print", fn, out); }
    }

    /* Pattern 7: Simple return of constant or single param (no malloc, no arrow)
     *
     * W4 (turi-inline-c-silent-miscompiles.md): refuse-rather-than-guess.  This
     * pattern only models `return <simple-expr>;`.  Decline bodies that:
     *   - have side effects the matcher silently drops (`printf`) -- e.g.
     *     inline-c-cname-splice `return 0;` after two printfs whose output IS
     *     the program's result ("1\n42");
     *   - splice sibling-defn calls (`__TUR_CNAME_...__(...)`) the matcher cannot
     *     invoke;
     *   - return a function-pointer CALL (`)(` -- a parenthesized callee applied
     *     to args) -- e.g. closure-capture-byptr-struct-param's
     *     `return ((fn1_t)fat[0])((void*)fat);`, which evaluated to 0.
     * Each previously produced rc=0 with the wrong answer; declining flips them
     * to the clean "inline-C not supported" error. */
    bool sr_has_call = strstr(body, "printf") || strstr(body, "__TUR_CNAME_") ||
                       strstr(body, ")(");
    if (!has_malloc && !has_arrow && has_return && !has_fptr && !has_switch &&
        !sr_has_call) {
        const char *r = strstr(body, "return "); if (r) {
            r += 7; r = ic_skip_ws(r);
            int64_t val = 0;
            if (ic_eval_assign_expr(r, fn, param_offset, args, n_args, &val, body)) {
                TuriValue rv={0}; rv.tag=TURI_INT; rv.as_int=val; *out=rv;
                return ic_claim("simple-return", fn, out);
            }
        }
    }

    return false;
}

/* -------------------------------------------------------------------------
 * T2 (turi-eval-trampoline-plan): explicit-stack driver for the linear control
 * forms.  Flattens directly-nested EX_IF branch chains and EX_DO/EX_PROGRAM
 * sequences onto a heap work-stack instead of the C stack, so a long branch
 * chain or sequence no longer grows one C frame per level.  Every other expr
 * kind -- including function application (EX_CALL) and builtins -- is evaluated
 * as a black box via the (still recursive) eval_expr, so this is a
 * behaviour-preserving refactor; the non-tail FUNCTION-call ceiling is removed
 * later (T3, which folds eval_apply into this loop).  Control-flow signals
 * (error / returning / throwing) and the EX_DO "defers don't count as the last
 * value" rule are preserved exactly.
 * ---------------------------------------------------------------------- */

/* Inline work-stack depth before spilling to heap.  Kept small: eval_drive is
 * now on hot paths (if/let/match/builtin), so its C frame should stay lean;
 * deeper nesting spills to a heap buffer (bounded only by heap). */
#define DRIVE_INLINE 8

typedef enum {
    DK_DONE,
    DK_IF_BRANCH,
    DK_DO_SEQ,
    DK_LET_BIND,     /* EX_LET/EX_LETREC: evaluating binding[index]'s init */
    DK_LET_BODY,     /* EX_LET/EX_LETREC: evaluating the body (frame owned) */
    DK_MAKE_STRUCT,  /* EX_MAKE_STRUCT: evaluating field[index] (fields owned) */
    DK_MATCH_BODY,   /* EX_MATCH: evaluating the winning arm body (frame owned) */
    DK_BUILTIN_ARG,  /* EX_BUILTIN: evaluating arg[index] (acc owned; or short-circuit) */
    DK_CALL_ARG,     /* EX_CALL: evaluating arg[index] (acc owned; callee in `last`) */
    DK_CALL_RET,     /* EX_CALL (T3.2b): non-tail turi-body callee, body in the loop */
    DK_PROMPT,       /* EX_HANDLE (DC): delimited-control prompt; aux = HandleExpr*,
                      * frame = handler lexical frame, index = active flag (1/0),
                      * prompt_cont = TuriWsCont* to cache into (resume prompt) or
                      * NULL (original handle prompt). */
} DriveKind;

typedef struct {
    DriveKind   kind;
    const Expr *expr;   /* the EX_IF / EX_DO|EX_PROGRAM / EX_LET / EX_MAKE_STRUCT / EX_CALL */
    EvalFrame  *frame;  /* lexical env: DO/MAKE_STRUCT=enclosing; LET/MATCH=owned */
    uint32_t    index;  /* DO: next item; LET: next binding; MAKE_STRUCT: next field */
    TuriValue   last;   /* DO/PROGRAM: last non-defer value; CALL_ARG: callee closure */
    void       *aux;    /* LET_BODY/CALL_RET: DeferItem* defer-stack mark;
                         * MAKE_STRUCT / *_ARG: TuriValue* heap accumulator */
    /* T3.2b: tail-position threading + folded-call (DK_CALL_RET) saved state. */
    bool        tail;          /* tail-ness to apply to this form's tail child */
    const char *saved_module;  /* DK_CALL_RET / DK_PROMPT: module to restore */
    bool        was_returning; /* DK_CALL_RET: env->returning at call entry */
    bool        was_no_unwind; /* DK_CALL_RET / DK_PROMPT: env->in_no_unwind to restore */
    void       *prompt_cont;   /* DK_PROMPT: TuriWsCont* to cache into, or NULL */
} DriveCont;

/* F4: activation seed for eval_drive_ex.  When non-NULL, the driver pushes a
 * DK_CALL_RET carrying this saved caller-state beneath DK_DONE and descends the
 * fn body in tail position; the DK_CALL_RET epilogue runs when the (possibly
 * tail-extended) activation completes.  The caller (eval_apply_driven) owns the
 * prologue: it builds the call frame, binds args, and publishes the callee's
 * module / no_unwind / cleared `returning` before invoking the driver. */
typedef struct {
    DeferItem  *defer_mark;     /* env->defer_stack at activation entry */
    const char *saved_module;   /* env->current_module to restore at chain end */
    bool        was_returning;  /* env->returning at activation entry */
    bool        was_no_unwind;  /* env->in_no_unwind at activation entry */
} DriveSeed;

/* -------------------------------------------------------------------------
 * DC (turi-interpreter-delimited-control-plan): work-stack delimited control.
 *
 * A capturable handle runs its body ON the driver work-stack behind a DK_PROMPT
 * frame, instead of on a ucontext fiber.  When the body performs, the driver
 * captures the slice of work-stack frames between the perform and the matching
 * prompt as a heap-owned TuriWsCont (the continuation `k`).  Because the slice
 * is heap-owned it survives the handle frame returning (escaping k) and a
 * resume re-installs the captured prompt(s), so a perform inside a resumed
 * continuation still finds the enclosing handlers (nested resume).
 *
 * Multishot note: to match the COMPILED path's (degenerate) effect-multishot
 * semantics -- where every resume after the first returns the first resume's
 * result -- the first resume runs the slice and caches its result, and
 * subsequent resumes return the cache.  See
 * docs/reported/turi-effect-multishot-degenerate-resume.md.
 * ---------------------------------------------------------------------- */
struct TuriWsCont {
    DriveCont   *frames;        /* captured slice, work-stack order (bottom..top) */
    size_t       n_frames;
    HandleExpr  *handler;       /* cases for re-dispatch on resume */
    EvalFrame   *handler_frame; /* lexical frame the handler closes over */
    /* env snapshot at the perform/capture point (restored before running the
     * slice on resume; the slice's own DK_CALL_RET epilogues unwind from there). */
    const char  *perf_module;
    bool         perf_no_unwind;
    void        *perf_defer;    /* DeferItem* */
    /* cached-replay multishot (see note above). */
    bool         resumed;
    TuriValue    cached;
};

/* Wrap a work-stack continuation in a TURI_EFFECT_CONT value (discriminated by
 * the ws pointer being non-NULL). */
static TuriValue turi_ws_cont_val(TuriWsCont *wc) {
    TuriEffectCont *c = (TuriEffectCont *)calloc(1, sizeof(TuriEffectCont));
    c->ws = wc;
    return turi_effect_cont(c);
}

/* Conservative "does evaluating e synchronously perform an effect?" -- returns
 * true unless e is provably perform-free.  Used to reject black-box positions
 * (match scrutinee/guards, resume k/value, set!/return values, get-field
 * receivers, and any non-modelled form) whose performs the driver cannot
 * capture.  Over-reports (any call / unmodelled form -> true) so it never
 * misses a perform; the cost is only that more handles fall back to fibers. */
static bool ws_has_perform(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_PERFORM: return true;
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT: case EX_FLOAT_LIT:
    case EX_CSTR_LIT: case EX_SYM_LIT: case EX_VAR: case EX_DEFAULT_OF:
    case EX_FN: case EX_FN_DEF: case EX_EXTERN_C:
        return false;
    case EX_IF:
        return ws_has_perform(e->as.if_.cond) ||
               ws_has_perform(e->as.if_.then_) ||
               ws_has_perform(e->as.if_.else_or_null);
    case EX_DO:
        for (uint32_t i = 0; i < e->as.do_.n; i++)
            if (ws_has_perform(e->as.do_.items[i])) return true;
        return false;
    case EX_PROGRAM:
        for (uint32_t i = 0; i < e->as.program.n; i++)
            if (ws_has_perform(e->as.program.items[i])) return true;
        return false;
    case EX_LET: case EX_LETREC:
        for (uint32_t i = 0; i < e->as.let_.n; i++)
            if (ws_has_perform(e->as.let_.bindings[i].init)) return true;
        return ws_has_perform(e->as.let_.body);
    case EX_BUILTIN:
        for (uint32_t i = 0; i < e->as.builtin.n; i++)
            if (ws_has_perform(e->as.builtin.args[i])) return true;
        return false;
    case EX_MAKE_STRUCT:
        for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
            if (ws_has_perform(e->as.make_struct_.field_values[i])) return true;
        return false;
    case EX_SET:        return ws_has_perform(e->as.set_.value);
    case EX_RETURN:     return ws_has_perform(e->as.return_.value);
    case EX_GET_FIELD:  return ws_has_perform(e->as.get_field_.struct_expr);
    case EX_RESUME:     return ws_has_perform(e->as.resume_.resume->k) ||
                               ws_has_perform(e->as.resume_.resume->value);
    default:
        /* EX_CALL, EX_HANDLE, EX_WHILE, EX_TRY_CATCH, ... -- conservatively
         * assume a perform may run synchronously. */
        return true;
    }
}

/* Decide whether a handle body can run on the work-stack: true iff every
 * perform reachable from `e` is reached through driver-transparent forms (so
 * the perform lands in eval_drive_ex's descending switch with the prompt
 * visible on `st`).  Conservative: anything uncertain -> false -> fiber path.
 * `depth` bounds recursion through direct callee bodies. */
static bool ws_capturable(TuriEnv *env, EvalFrame *frame, const Expr *e, int depth) {
    if (!e) return true;
    if (depth <= 0) return false;   /* give up on very deep / cyclic chains */
    switch (e->kind) {
    case EX_PERFORM: {
        PerformExpr *pe = e->as.perform_.perform;
        for (uint8_t i = 0; i < pe->n_args; i++)
            if (ws_has_perform(pe->args[i])) return false;  /* args run via eval_expr */
        return true;
    }
    case EX_IF:
        return ws_capturable(env, frame, e->as.if_.cond, depth) &&
               ws_capturable(env, frame, e->as.if_.then_, depth) &&
               ws_capturable(env, frame, e->as.if_.else_or_null, depth);
    case EX_DO:
        for (uint32_t i = 0; i < e->as.do_.n; i++)
            if (!ws_capturable(env, frame, e->as.do_.items[i], depth)) return false;
        return true;
    case EX_PROGRAM:
        for (uint32_t i = 0; i < e->as.program.n; i++)
            if (!ws_capturable(env, frame, e->as.program.items[i], depth)) return false;
        return true;
    case EX_LET: case EX_LETREC:
        for (uint32_t i = 0; i < e->as.let_.n; i++)
            if (!ws_capturable(env, frame, e->as.let_.bindings[i].init, depth)) return false;
        return ws_capturable(env, frame, e->as.let_.body, depth);
    case EX_BUILTIN:
        for (uint32_t i = 0; i < e->as.builtin.n; i++)
            if (!ws_capturable(env, frame, e->as.builtin.args[i], depth)) return false;
        return true;
    case EX_MAKE_STRUCT:
        for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
            if (!ws_capturable(env, frame, e->as.make_struct_.field_values[i], depth)) return false;
        return true;
    case EX_MATCH:
        /* Scrutinee + guards run via eval_match_resolve -> eval_expr (black box). */
        if (ws_has_perform(e->as.match_.scrutinee)) return false;
        for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
            if (ws_has_perform(e->as.match_.arms[i].guard)) return false;
            if (!ws_capturable(env, frame, e->as.match_.arms[i].body, depth)) return false;
        }
        return true;
    case EX_HANDLE: {
        const HandleExpr *h = e->as.handle_.handle;
        if (!ws_capturable(env, frame, h->body, depth)) return false;
        for (uint8_t i = 0; i < h->n_cases; i++)
            if (!ws_capturable(env, frame, h->cases[i].body, depth)) return false;
        return true;
    }
    case EX_WITH_HANDLER: {
        /* Capturable only if the handler value is a static (handler ...) literal
         * whose case bodies are themselves capturable. */
        const Expr *hexpr = e->as.with_handler_.handler;
        if (!hexpr || hexpr->kind != EX_HANDLER_LIT) return false;
        const HandleExpr *h = hexpr->as.handler_lit_.handle;
        for (uint8_t i = 0; i < h->n_cases; i++)
            if (!ws_capturable(env, frame, h->cases[i].body, depth)) return false;
        return ws_capturable(env, frame, e->as.with_handler_.body, depth);
    }
    case EX_RESUME:
        return !ws_has_perform(e->as.resume_.resume->k) &&
               !ws_has_perform(e->as.resume_.resume->value);
    case EX_CALL: {
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            if (!ws_capturable(env, frame, e->as.call_.args[i], depth)) return false;
        /* A direct, foldable turi callee runs its body on the work-stack
         * (DK_CALL_RET), so its performs are capturable -- recurse into it. */
        if (e->as.call_.fn_binding) {
            TuriValue fv = eval_lookup(env, frame, e->as.call_.fn_binding->name->name);
            if (fv.tag == TURI_CLOSURE && fv.as_closure && !fv.as_closure->native &&
                fv.as_closure->fn && ((FnDef *)fv.as_closure->fn)->body) {
                FnDef *fn = (FnDef *)fv.as_closure->fn;
                if (fn->body->kind == EX_INLINE_C) return true;  /* leaf, no perform */
                return ws_capturable(env, fv.as_closure->captured, fn->body, depth - 1);
            }
        }
        /* Native / indirect callee: cannot fold; safe iff no performing value is
         * passed into it (a native HOF could black-box a performing closure). */
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            if (ws_has_perform(e->as.call_.args[i])) return false;
        return true;
    }
    /* Closure-creating forms: capturable to *evaluate* (they just build a
     * closure), but a performing body could later be invoked under a black box,
     * so reject those outright -- keeps the native-HOF case on the fiber path. */
    case EX_FN:      return !ws_has_perform(((FnDef *)e->as.fn_.fn)->body);
    case EX_FN_DEF:  return !ws_has_perform(((FnDef *)e->as.fn_def_.fn)->body);
    /* Leaves / values with no synchronous perform. */
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT: case EX_FLOAT_LIT:
    case EX_CSTR_LIT: case EX_SYM_LIT: case EX_VAR: case EX_DEFAULT_OF:
    case EX_HANDLER_LIT: case EX_EXTERN_C:
        return true;
    case EX_SET:        return !ws_has_perform(e->as.set_.value);
    case EX_RETURN:     return !ws_has_perform(e->as.return_.value);
    case EX_GET_FIELD:  return !ws_has_perform(e->as.get_field_.struct_expr);
    default:
        /* Any other form is a black box for the driver: capturable only if it
         * contains no perform at all. */
        return !ws_has_perform(e);
    }
}

/* T3.0: resolve an EX_MATCH to its winning arm frame + body WITHOUT evaluating
 * the body, so eval_drive can descend the body in the loop (flattening
 * match-recursive callee bodies later folded by T3).  The scrutinee and any
 * arm guards are evaluated here via eval_expr (shallow, not the deep-recursion
 * site).  Returns:
 *   1  -> matched: *out_frame (caller frees after the body) and *out_body set
 *   0  -> no arm matched
 *  -1  -> a signal/error during scrutinee or guard eval: *out_val holds it
 * Mirrors the recursive EX_MATCH in eval_expr_impl exactly. */
static int eval_match_resolve(TuriEnv *env, EvalFrame *frame, const Expr *e,
                              EvalFrame **out_frame, const Expr **out_body,
                              TuriValue *out_val) {
    TuriValue val = eval_expr(env, frame, e->as.match_.scrutinee);
    if (turi_is_error(val) || env->returning || env->throwing) {
        *out_val = val; return -1;
    }
    MatchArm *arms   = e->as.match_.arms;
    uint32_t  n_arms = e->as.match_.n_arms;
    for (uint32_t ai = 0; ai < n_arms; ai++) {
        MatchArm     *arm = &arms[ai];
        MatchPattern *pat = &arm->pattern;
        bool          matched   = false;
        EvalFrame    *arm_frame = NULL;

        if (pat->is_wildcard) {
            matched = true; arm_frame = eval_frame_new(frame);
        } else if (pat->is_var && pat->union_member_idx >= 0) {
            bool tag_ok = false;
            if (pat->n_bindings >= 1 && pat->bindings[0]) {
                TypeKind tk = pat->bindings[0]->type.kind;
                switch (tk) {
                case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
                case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
                    tag_ok = (val.tag == TURI_INT); break;
                case TY_BOOL:  tag_ok = (val.tag == TURI_BOOL); break;
                case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
                    tag_ok = (val.tag == TURI_FLOAT); break;
                case TY_CSTR:  tag_ok = (val.tag == TURI_CSTR); break;
                case TY_NIL:   tag_ok = (val.tag == TURI_NIL); break;
                default:       tag_ok = (val.tag == TURI_STRUCT || val.tag == TURI_CLOSURE); break;
                }
            } else {
                tag_ok = true;
            }
            if (tag_ok) {
                matched = true; arm_frame = eval_frame_new(frame);
                if (pat->var_sym) frame_bind(arm_frame, pat->var_sym->name, val);
            }
        } else if (pat->is_literal) {
            switch (pat->lit_kind) {
            case F_INT:   matched = (val.tag == TURI_INT   && val.as_int   == pat->lit_int);   break;
            case F_BOOL:  matched = (val.tag == TURI_BOOL  && val.as_bool  == pat->lit_bool);  break;
            case F_FLOAT: matched = (val.tag == TURI_FLOAT && val.as_float == pat->lit_float); break;
            case F_STR:   matched = (val.tag == TURI_CSTR  && val.as_cstr  && pat->lit_cstr &&
                                     strcmp(val.as_cstr, pat->lit_cstr) == 0);                break;
            case F_NIL:   matched = (val.tag == TURI_NIL); break;
            default: break;
            }
            if (matched) arm_frame = eval_frame_new(frame);
        } else if (pat->is_var) {
            matched = true; arm_frame = eval_frame_new(frame);
            frame_bind(arm_frame, pat->var_sym->name, val);
        } else {
            CtorDef *ctor = pat->ctor;
            if (ctor && val.tag == TURI_STRUCT &&
                strcmp(val.as_struct->name, ctor->name) == 0) {
                matched = true; arm_frame = eval_frame_new(frame);
                for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                    Binding *b = pat->bindings[bi];
                    if (b && bi < val.as_struct->n_fields)
                        frame_bind(arm_frame, b->name->name, val.as_struct->fields[bi]);
                }
            }
        }

        if (matched) {
            if (arm->guard) {
                TuriValue gv = eval_expr(env, arm_frame, arm->guard);
                if (turi_is_error(gv) || env->returning || env->throwing) {
                    eval_frame_free(arm_frame); *out_val = gv; return -1;
                }
                if (gv.tag != TURI_BOOL || !gv.as_bool) {
                    eval_frame_free(arm_frame); continue;  /* guard failed */
                }
            }
            *out_frame = arm_frame; *out_body = arm->body; return 1;
        }
    }
    return 0;  /* no arm matched */
}

/* do/program item accessors (both kinds share this shape). */
static inline Expr **drive_seq_items(const Expr *e) {
    return (e->kind == EX_PROGRAM) ? e->as.program.items : e->as.do_.items;
}
static inline uint32_t drive_seq_n(const Expr *e) {
    return (e->kind == EX_PROGRAM) ? e->as.program.n : e->as.do_.n;
}

static TuriValue eval_drive_ex(TuriEnv *env, EvalFrame *frame, const Expr *e,
                               const DriveSeed *seed) {
    DriveCont  inl[DRIVE_INLINE];
    DriveCont *st  = inl;
    size_t     len = 0, cap = DRIVE_INLINE;
    TuriValue  result;

    /* push helper: grows from the inline buffer onto the heap as needed. */
    #define DRIVE_PUSH(C) do {                                                  \
        if (len == cap) {                                                       \
            size_t ncap = cap * 2;                                             \
            DriveCont *ni = (DriveCont *)malloc(ncap * sizeof(DriveCont));     \
            if (!ni) { result = turi_error("eval: out of memory (driver stack)"); \
                       goto done; }                                            \
            memcpy(ni, st, len * sizeof(DriveCont));                           \
            if (st != inl) free(st);                                           \
            st = ni; cap = ncap;                                              \
        }                                                                      \
        st[len++] = (C);                                                       \
    } while (0)

    DRIVE_PUSH(((DriveCont){ .kind = DK_DONE }));

    const Expr *control    = e;
    EvalFrame  *cf         = frame;
    TuriValue   cur        = turi_nil();
    bool        descending = true;
    /* T3.2b/F4: tail-ness of `control`.  The plain expression entry
     * (eval_drive, seed == NULL) starts non-tail; tail-ness becomes true only
     * inside a folded callee body.  The activation entry (eval_apply_driven,
     * seed != NULL) seeds a DK_CALL_RET for the activation and descends the fn
     * body in tail position, so the body's tail calls reuse that slot. */
    bool        tail       = false;
    if (seed) {
        DRIVE_PUSH(((DriveCont){ .kind = DK_CALL_RET, .frame = frame,
                                 .aux = (void *)seed->defer_mark,
                                 .saved_module  = seed->saved_module,
                                 .was_returning = seed->was_returning,
                                 .was_no_unwind = seed->was_no_unwind }));
        tail = true;
    }

    for (;;) {
        if (descending) {
            switch (control->kind) {
            case EX_IF:
                DRIVE_PUSH(((DriveCont){ .kind = DK_IF_BRANCH, .expr = control,
                                         .frame = cf, .tail = tail }));
                control = control->as.if_.cond;   /* descend into the test (non-tail) */
                tail = false;
                break;                            /* still descending */
            case EX_DO:
            case EX_PROGRAM: {
                const Expr *de = control;
                uint32_t    n  = drive_seq_n(de);
                if (n == 0) { cur = turi_nil(); descending = false; break; }
                bool   do_tail = tail;
                Expr **items   = drive_seq_items(de);
                if (n == 1 && items[0]->kind != EX_DEFER && do_tail) {
                    /* F2: the sole (tail) item carries the do's tail-ness with no
                     * continuation, so a tail call in it exposes the enclosing
                     * DK_CALL_RET directly (no DK_DO_SEQ to pop first). */
                    control = items[0];   /* cf unchanged; tail stays true */
                    break;
                }
                DRIVE_PUSH(((DriveCont){ .kind = DK_DO_SEQ, .expr = de,
                                         .frame = cf, .tail = do_tail }));
                control = items[0];
                /* multi-item / non-tail-do: item 0 is never in tail position;
                 * the final item's tail-ness is decided in the DK_DO_SEQ path. */
                tail = false;
                break;
            }
            case EX_LET:
            case EX_LETREC: {
                /* Owns a fresh frame; bindings then body run in it.  EX_LETREC
                 * pre-binds every name to nil so RHS closures see each other. */
                EvalFrame *nf = eval_frame_new(cf);
                uint32_t   n  = control->as.let_.n;
                if (control->kind == EX_LETREC) {
                    for (uint32_t i = 0; i < n; i++)
                        frame_bind(nf,
                            control->as.let_.bindings[i].binding->name->name,
                            turi_nil());
                }
                if (n > 0) {
                    DRIVE_PUSH(((DriveCont){ .kind = DK_LET_BIND, .expr = control,
                                             .frame = nf, .tail = tail }));
                    control = control->as.let_.bindings[0].init;
                    cf = nf; tail = false;   /* binding inits are non-tail */
                } else if (tail) {
                    /* F1: tail let body -- descend directly (leak nf), no separate
                     * defer scope.  Body defers fire at function exit via the
                     * enclosing DK_CALL_RET, matching the retired eval_body_tco's tail leak
                     * (frame = new_frame; goto restart).  Keeping nf-free here is
                     * what lets a tail call in the body reuse the activation. */
                    control = control->as.let_.body;
                    cf = nf;   /* tail stays true */
                } else {
                    /* Non-tail let: own the frame on the work-stack so the
                     * body's defers fire (and nf frees) at let-scope exit. */
                    DRIVE_PUSH(((DriveCont){ .kind = DK_LET_BODY, .expr = control,
                                             .frame = nf, .tail = tail,
                                             .aux = env->defer_stack }));
                    control = control->as.let_.body;
                    cf = nf;
                }
                break;
            }
            case EX_MAKE_STRUCT: {
                uint32_t n = control->as.make_struct_.n_fields;
                if (n == 0) {
                    StructDef *sd = control->as.make_struct_.def;
                    cur = make_struct_val_def(sd ? sd->name : "<struct>", 0, NULL, sd);
                    descending = false;
                    break;
                }
                /* Heap field accumulator persists across the per-field descents;
                 * freed when the struct is built or on signal-unwind. */
                TuriValue *fields = (TuriValue *)malloc((size_t)n * sizeof(TuriValue));
                if (!fields) {
                    result = turi_error("eval: out of memory evaluating struct fields");
                    goto done;
                }
                DRIVE_PUSH(((DriveCont){ .kind = DK_MAKE_STRUCT, .expr = control,
                                         .frame = cf, .aux = fields }));
                control = control->as.make_struct_.field_values[0];
                tail = false;   /* struct fields are non-tail */
                break;
            }
            case EX_BUILTIN: {
                const BuiltinSpec *spec = control->as.builtin.spec;
                uint32_t n  = control->as.builtin.n;
                bool     sc = (spec->shape == BS_AND_SC || spec->shape == BS_OR_SC);
                if (n == 0) {
                    /* (and)->true, (or)->false; other 0-ary builtins eval directly. */
                    cur = sc ? turi_bool(spec->shape == BS_AND_SC)
                             : eval_builtin(env, spec, NULL, 0);
                    descending = false;
                    break;
                }
                TuriValue *acc = NULL;
                if (!sc) {  /* regular builtins accumulate all args, then apply */
                    acc = (TuriValue *)malloc((size_t)n * sizeof(TuriValue));
                    if (!acc) {
                        result = turi_error("eval: out of memory evaluating builtin arguments");
                        goto done;
                    }
                }
                DRIVE_PUSH(((DriveCont){ .kind = DK_BUILTIN_ARG, .expr = control,
                                         .frame = cf, .aux = acc }));
                control = control->as.builtin.args[0];
                tail = false;   /* builtin args are non-tail */
                break;
            }
            case EX_CALL: {
                /* T3.2a: resolve the callee here, accumulate args on the
                 * work-stack, then apply via eval_apply (still recursive -- the
                 * body-in-loop + TCO fold is T3.2b).  The closure value rides in
                 * `last`; the arg accumulator in `aux`. */
                TuriValue fn_val;
                if (control->as.call_.fn_binding) {
                    fn_val = eval_lookup(env, cf,
                                 control->as.call_.fn_binding->name->name);
                } else if (control->as.call_.fn_expr) {
                    fn_val = eval_expr(env, cf, control->as.call_.fn_expr);
                    fn_val = reword_unbound_call_head(fn_val, control->as.call_.fn_expr);
                } else {
                    cur = turi_error("eval: call with no function");
                    descending = false; break;
                }
                if (turi_is_error(fn_val) || env->returning || env->throwing) {
                    cur = fn_val; descending = false; break;
                }
                if (control->as.call_.fn_binding)
                    fn_val = recover_carrier_closure(fn_val, control->as.call_.fn_binding);
                if (fn_val.tag != TURI_CLOSURE) {
                    cur = turi_errorf("eval: expected function, got tag %d", fn_val.tag);
                    descending = false; break;
                }
                uint32_t n = control->as.call_.n_args;
                /* T3.2c: zero-arg calls flow through DK_CALL_ARG too (acc==NULL),
                 * so a zero-arg tail call reuses the enclosing activation and a
                 * zero-arg non-tail call folds -- both stay off the C stack. */
                TuriValue *acc = NULL;
                if (n > 0) {
                    acc = (TuriValue *)malloc((size_t)n * sizeof(TuriValue));
                    if (!acc) {
                        result = turi_error("eval: out of memory evaluating call arguments");
                        goto done;
                    }
                }
                DRIVE_PUSH(((DriveCont){ .kind = DK_CALL_ARG, .expr = control,
                                         .frame = cf, .last = fn_val, .aux = acc,
                                         .tail = tail }));
                if (n > 0) {
                    control = control->as.call_.args[0];
                    tail = false;   /* call args are non-tail */
                } else {
                    cur = turi_nil(); descending = false;  /* args ready: run handler */
                }
                break;
            }
            case EX_MATCH: {
                /* Resolve scrutinee + arm + guard synchronously (shallow), then
                 * descend the winning arm body in the loop. */
                EvalFrame  *af   = NULL;
                const Expr *body = NULL;
                TuriValue   sv   = turi_nil();
                int mr = eval_match_resolve(env, cf, control, &af, &body, &sv);
                if (mr == 1) {
                    if (tail) {
                        /* F1: tail match arm body -- descend directly (leak af);
                         * exposes the enclosing DK_CALL_RET for a tail call in the
                         * arm body, matching the retired eval_body_tco's tail leak. */
                        control = body; cf = af;   /* tail stays true */
                    } else {
                        DRIVE_PUSH(((DriveCont){ .kind = DK_MATCH_BODY, .expr = control,
                                                 .frame = af, .tail = tail }));
                        control = body; cf = af;   /* arm body: inherits match's tail-ness */
                    }
                } else if (mr == 0) {
                    cur = turi_error("eval: match: no arm matched");
                    descending = false;
                } else {  /* mr == -1: signal during scrutinee/guard */
                    cur = sv; descending = false;
                }
                break;
            }
            case EX_HANDLE: {
                /* DC: a capturable handle installs a DK_PROMPT and runs its body
                 * on the work-stack; otherwise fall back to the fiber path. */
                const HandleExpr *h = control->as.handle_.handle;
                if (ws_capturable(env, cf, h->body, 64)) {
                    bool ok = true;
                    for (uint8_t i = 0; i < h->n_cases && ok; i++)
                        ok = ws_capturable(env, cf, h->cases[i].body, 64);
                    if (ok) {
                        DRIVE_PUSH(((DriveCont){ .kind = DK_PROMPT, .aux = (void *)h,
                                                 .frame = cf, .tail = tail, .index = 1,
                                                 .prompt_cont = NULL,
                                                 .saved_module = env->current_module,
                                                 .was_no_unwind = env->in_no_unwind }));
                        control = h->body; tail = false;   /* body runs under the prompt */
                        break;                             /* keep descending */
                    }
                }
                cur = eval_handle(env, cf, h);   /* fiber fallback */
                descending = false;
                break;
            }
            case EX_WITH_HANDLER: {
                /* DC: materialise a HandleExpr from the handler value and run it
                 * on the work-stack when capturable; else fiber-fallback. */
                TuriValue hvv = eval_expr(env, cf, control->as.with_handler_.handler);
                if (turi_is_error(hvv) || env->returning || env->throwing) {
                    cur = hvv; descending = false; break;
                }
                if (hvv.tag != TURI_HANDLER) {
                    cur = turi_error("eval: with-handler: first argument must be a handler value");
                    descending = false; break;
                }
                TuriHandlerVal *hv = hvv.as_handler;
                /* Heap-own the HandleExpr + cases: a captured continuation may
                 * outlive this driver frame, so they cannot live on the C stack. */
                HandleExpr *h = (HandleExpr *)malloc(sizeof(HandleExpr));
                HandleCase *cs = (HandleCase *)malloc((size_t)hv->n_cases * sizeof(HandleCase));
                for (uint8_t i = 0; i < hv->n_cases; i++) cs[i] = *hv->cases[i];
                h->body = control->as.with_handler_.body;
                h->cases = cs; h->n_cases = hv->n_cases;
                bool ok = ws_capturable(env, cf, h->body, 64);
                for (uint8_t i = 0; i < h->n_cases && ok; i++)
                    ok = ws_capturable(env, cf, h->cases[i].body, 64);
                if (ok) {
                    DRIVE_PUSH(((DriveCont){ .kind = DK_PROMPT, .aux = (void *)h,
                                             .frame = cf, .tail = tail, .index = 1,
                                             .prompt_cont = NULL,
                                             .saved_module = env->current_module,
                                             .was_no_unwind = env->in_no_unwind }));
                    control = h->body; tail = false;
                    break;
                }
                cur = eval_handle(env, cf, h);   /* fiber fallback (borrows h) */
                descending = false;
                break;
            }
            case EX_PERFORM: {
                PerformExpr *pe = control->as.perform_.perform;
                const char  *effect_name = pe->effect_name->name;
                TuriValue pargs[EVAL_MAX_FN_ARITY];
                uint8_t n = pe->n_args;
                if (n > EVAL_MAX_FN_ARITY) {
                    cur = turi_errorf("eval: too many effect arguments (%u)", n);
                    descending = false; break;
                }
                bool sig = false;
                for (uint8_t i = 0; i < n; i++) {
                    pargs[i] = eval_expr(env, cf, pe->args[i]);
                    if (turi_is_error(pargs[i]) || env->returning || env->throwing) {
                        cur = pargs[i]; sig = true; break;
                    }
                }
                if (sig) { descending = false; break; }
                /* Scan downward for the nearest active prompt that handles this
                 * effect (propagating past prompts that don't). */
                long pidx = -1; HandleCase *matched = NULL;
                for (long i = (long)len - 1; i >= 0 && !matched; i--) {
                    if (st[i].kind != DK_PROMPT || !st[i].index) continue;
                    HandleExpr *h = (HandleExpr *)st[i].aux;
                    for (uint8_t c = 0; c < h->n_cases; c++)
                        if (strcmp(h->cases[c].effect_name->name, effect_name) == 0) {
                            matched = &h->cases[c]; pidx = i; break;
                        }
                }
                if (pidx < 0) {
                    /* No work-stack prompt: black-boxed perform or an enclosing
                     * fiber handler -- use the fiber path (we are physically on
                     * that fiber's stack when one exists). */
                    cur = eval_perform_fiber(env, effect_name, pargs, n);
                    descending = false; break;
                }
                /* Capture the slice st[pidx+1 .. len-1] as a heap continuation. */
                size_t nf = (len - 1) - (size_t)pidx;
                TuriWsCont *wc = (TuriWsCont *)calloc(1, sizeof(TuriWsCont));
                wc->n_frames = nf;
                if (nf) {
                    wc->frames = (DriveCont *)malloc(nf * sizeof(DriveCont));
                    memcpy(wc->frames, &st[pidx + 1], nf * sizeof(DriveCont));
                }
                wc->handler        = (HandleExpr *)st[pidx].aux;
                wc->handler_frame  = st[pidx].frame;
                wc->perf_module    = env->current_module;
                wc->perf_no_unwind = env->in_no_unwind;
                wc->perf_defer     = env->defer_stack;
                /* Unwind to the prompt and run its matched case body; its value
                 * becomes the handle's value (received by DK_PROMPT@pidx). */
                len = (size_t)pidx + 1;
                st[pidx].index = 0;   /* disable while its own case body runs */
                EvalFrame *hf = eval_frame_new(st[pidx].frame);
                for (uint8_t i = 0; i < matched->n_params && i < n; i++)
                    frame_bind(hf, matched->param_bindings[i]->name->name, pargs[i]);
                frame_bind(hf, matched->k_binding->name->name, turi_ws_cont_val(wc));
                env->current_module = st[pidx].saved_module;
                env->in_no_unwind   = st[pidx].was_no_unwind;
                control = matched->body; cf = hf; tail = st[pidx].tail;
                /* descending stays true */
                break;
            }
            case EX_RESUME: {
                ResumeExpr *re = control->as.resume_.resume;
                TuriValue k = eval_expr(env, cf, re->k);
                if (turi_is_error(k) || env->returning || env->throwing) {
                    cur = k; descending = false; break;
                }
                TuriValue v = eval_expr(env, cf, re->value);
                if (turi_is_error(v) || env->returning || env->throwing) {
                    cur = v; descending = false; break;
                }
                if (k.tag != TURI_EFFECT_CONT) {
                    cur = turi_error("eval: resume: not a continuation");
                    descending = false; break;
                }
                if (!k.as_cont->ws) {
                    cur = eval_resume_cont(env, cf, k.as_cont, v);  /* fiber cont */
                    descending = false; break;
                }
                TuriWsCont *wc = k.as_cont->ws;
                if (wc->resumed) {
                    /* cached-replay multishot: matches the compiled path's
                     * degenerate semantics (subsequent resume == first result). */
                    cur = wc->cached; descending = false; break;
                }
                /* First resume: re-install the prompt (for re-dispatch + result
                 * caching) and push the captured slice, then feed v to the hole. */
                DRIVE_PUSH(((DriveCont){ .kind = DK_PROMPT, .aux = (void *)wc->handler,
                                         .frame = wc->handler_frame, .tail = tail,
                                         .index = 1, .prompt_cont = wc,
                                         .saved_module = env->current_module,
                                         .was_no_unwind = env->in_no_unwind }));
                for (size_t i = 0; i < wc->n_frames; i++)
                    DRIVE_PUSH(wc->frames[i]);
                env->current_module = wc->perf_module;
                env->in_no_unwind   = wc->perf_no_unwind;
                env->defer_stack    = wc->perf_defer;
                cur = v; descending = false;
                break;
            }
            default:
                /* Black box: evaluate any other kind via the recursive path. */
                cur = eval_expr(env, cf, control);
                descending = false;
                break;
            }
        } else {
            /* Returning `cur` to the frame on top of the work-stack.  Each
             * continuation runs its own cleanup when a control signal
             * (error/returning/throwing) passes through it -- LET frames must be
             * freed (and their body defers fired) on unwind, so this is handled
             * per-kind rather than by a blanket unwind-to-DONE. */
            DriveCont *top = &st[len - 1];
            bool signaled = turi_is_error(cur) || env->returning || env->throwing;
            switch (top->kind) {
            case DK_DONE:
                result = cur;
                goto done;
            case DK_IF_BRANCH: {
                const Expr *ie   = top->expr;
                EvalFrame  *icf  = top->frame;
                bool        itl  = top->tail;   /* branch inherits the if's tail-ness */
                len--;  /* pop: the chosen branch's value becomes the if's value */
                if (signaled) break;  /* propagate cur unchanged */
                if (turi_is_truthy(cur)) {
                    control = ie->as.if_.then_; cf = icf; tail = itl; descending = true;
                } else if (ie->as.if_.else_or_null) {
                    control = ie->as.if_.else_or_null; cf = icf; tail = itl; descending = true;
                } else {
                    cur = turi_nil();  /* no else: result is nil, keep returning */
                }
                break;
            }
            case DK_DO_SEQ: {
                if (signaled) { len--; break; }  /* abandon the rest of the seq */
                const Expr *de = top->expr;
                Expr     **items = drive_seq_items(de);
                uint32_t   n     = drive_seq_n(de);
                if (items[top->index]->kind != EX_DEFER) top->last = cur;
                top->index++;
                if (top->index < n) {
                    bool is_tail_item = (top->index == n - 1 &&
                                         items[top->index]->kind != EX_DEFER &&
                                         top->tail);
                    if (is_tail_item) {
                        /* F2: pop DK_DO_SEQ before descending the final (tail)
                         * item so a tail call in it exposes the enclosing
                         * DK_CALL_RET.  The last value IS the do's value, so the
                         * cont's bookkeeping is no longer needed. */
                        const Expr *item = items[top->index];
                        EvalFrame  *f    = top->frame;
                        len--;
                        control = item; cf = f; tail = true; descending = true;
                    } else {
                        /* trailing-defer do or non-tail do: the value item runs
                         * non-tail (its value is captured in top->last). */
                        control = items[top->index]; cf = top->frame;
                        tail = false; descending = true;
                    }
                } else {
                    cur = top->last; len--;  /* pop; keep returning the last value */
                }
                break;
            }
            case DK_LET_BIND: {
                EvalFrame *nf = top->frame;
                if (signaled) {  /* binding init errored/returned/threw */
                    eval_frame_free(nf); len--;  /* free frame, propagate */
                    break;
                }
                const Expr *le = top->expr;
                uint32_t    n  = le->as.let_.n;
                const char *nm = le->as.let_.bindings[top->index].binding->name->name;
                if (le->kind == EX_LETREC) {
                    /* Re-home a captureless fn literal onto this frame so siblings
                     * resolve by name (mirrors the recursive EX_LETREC case). */
                    if (cur.tag == TURI_CLOSURE && cur.as_closure &&
                        cur.as_closure->captured == NULL) {
                        TuriClosure *copy = (TuriClosure *)malloc(sizeof(TuriClosure));
                        *copy = *cur.as_closure;
                        copy->captured = nf;
                        cur = turi_closure(copy);
                    }
                    eval_frame_update(nf, nm, cur);
                } else {
                    frame_bind(nf, nm, cur);
                }
                top->index++;
                if (top->index < n) {
                    control = le->as.let_.bindings[top->index].init;
                    cf = nf; tail = false; descending = true;  /* inits are non-tail */
                } else if (top->tail) {
                    /* F1: tail let body -- pop DK_LET_BIND and descend the body
                     * directly (leak nf); body defers fire at function exit
                     * (matches the retired eval_body_tco).  Exposes the enclosing DK_CALL_RET
                     * for a tail call in the body. */
                    const Expr *body = le->as.let_.body;
                    len--;
                    control = body; cf = nf; tail = true; descending = true;
                } else {
                    /* Non-tail let -> body.  Mark defers AFTER bindings so only
                     * body-registered defers fire on this scope's exit. */
                    top->kind       = DK_LET_BODY;
                    top->aux = env->defer_stack;
                    control = le->as.let_.body;
                    cf = nf; tail = false; descending = true;
                }
                break;
            }
            case DK_LET_BODY: {
                EvalFrame *nf = top->frame;
                /* On normal exit fire this scope's defers; on early-return/throw
                 * leave them for eval_apply (matches the recursive EX_LET). */
                if (!env->returning && !env->throwing)
                    fire_defers_to_mark(env, (DeferItem *)top->aux, NULL);
                eval_frame_free(nf);
                len--;  /* pop; propagate cur (body value or signal) */
                break;
            }
            case DK_MATCH_BODY: {
                /* Arm body produced cur (value or signal): free the arm frame
                 * and propagate, matching the recursive EX_MATCH which frees
                 * arm_frame before returning the body's result. */
                eval_frame_free(top->frame);
                len--;
                break;
            }
            case DK_CALL_ARG: {
                TuriValue *acc = (TuriValue *)top->aux;
                uint32_t n = top->expr->as.call_.n_args;
                if (signaled) { free(acc); len--; break; }
                if (n > 0) {
                    acc[top->index] = cur;
                    top->index++;
                    if (top->index < n) {
                        control = top->expr->as.call_.args[top->index];
                        cf = top->frame; tail = false; descending = true;  /* args non-tail */
                        break;
                    }
                }
                /* All args ready (a zero-arg call reaches here directly with
                 * acc == NULL). */
                TuriClosure *cl = top->last.as_closure;
                FnDef       *fn = (FnDef *)cl->fn;
                bool foldable = !cl->native && fn && fn->body &&
                                fn->body->kind != EX_INLINE_C;
                if (!foldable) {
                    /* Leaf: native / inline-C, dispatched inside eval_apply
                     * (no driver re-entry).  eval_apply copies args, so free. */
                    cur = eval_apply(env, cl, acc, n);
                    free(acc); len--;
                    break;
                }
                /* Turi-body closure: shared prologue (arity + step-fuel checks,
                 * build the call frame, bind args). */
                uint32_t param_offset     = cl->skip_env_param ? 1u : 0u;
                uint32_t effective_params = (uint32_t)fn->n_params - param_offset;
                if (effective_params != n) {
                    cur = turi_errorf("eval: arity mismatch: %s expects %u args, got %u",
                                      fn->binding ? fn->binding->name->name : "<fn>",
                                      (unsigned)effective_params, (unsigned)n);
                    free(acc); len--; break;
                }
                if (env->step_fuel_limit > 0) {  /* SB3: step-fuel, as the retired eval_apply_inner charged it */
                    if (env->step_fuel == 0) {
                        cur = turi_error("eval: step fuel exhausted");
                        free(acc); len--; break;
                    }
                    env->step_fuel--;
                }
                EvalFrame *call_frame = eval_frame_new((EvalFrame *)cl->captured);
                for (uint32_t i = 0; i < n; i++)
                    frame_bind(call_frame,
                               fn->params[param_offset + i]->name->name, acc[i]);
                free(acc);

                if (top->tail) {
                    /* F3: tail call -- REUSE the enclosing activation's
                     * DK_CALL_RET instead of pushing a new one, so a tail chain
                     * stays O(1) on the work-stack.  F1/F2 guarantee that no
                     * cleanup continuation sits between this DK_CALL_ARG and the
                     * enclosing DK_CALL_RET, so st[len-2] IS that activation.
                     * The sequence reproduces the per-iteration pre-bounce
                     * cleanup + top-of-loop re-entry of the retired TcoFrame
                     * trampoline (eval_apply_inner), now folded into the driver. */
                    assert(len >= 2 && st[len - 2].kind == DK_CALL_RET);
                    DriveCont *ret = &st[len - 2];
                    /* (1) finish the current activation: restore its no_unwind
                     * and fire its defers (LIFO), exactly as before a bounce. */
                    env->in_no_unwind = ret->was_no_unwind;
                    fire_defers_to_mark_reversed(env, (DeferItem *)ret->aux, NULL);
                    /* (2) re-enter the callee in the same slot.  saved_module is
                     * left as captured by the chain head (restored once at the
                     * chain's end); was_returning / was_no_unwind are recaptured
                     * per iteration, as the retired trampoline loop did. */
                    env->current_module = cl->module;
                    ret->frame         = call_frame;
                    ret->aux           = (void *)env->defer_stack; /* new defer mark */
                    ret->was_returning = env->returning;
                    ret->was_no_unwind = env->in_no_unwind;        /* = caller's */
                    env->returning      = false;
                    env->in_no_unwind   = fn->binding && fn->binding->no_unwind;
                    len--;  /* pop DK_CALL_ARG; ret is now st[len-1] */
                    control = fn->body; cf = call_frame; tail = true; descending = true;
                    break;
                }

                /* Non-tail turi-body closure: FOLD -- reuse THIS slot as
                 * DK_CALL_RET and descend the body in the loop so deep non-tail
                 * recursion stays off the C stack.  Reproduces the
                 * single-activation prologue of the retired eval_apply_inner. */
                top->kind          = DK_CALL_RET;
                top->frame         = call_frame;
                top->aux           = env->defer_stack;       /* defer mark */
                top->saved_module  = env->current_module;
                top->was_returning = env->returning;
                top->was_no_unwind = env->in_no_unwind;
                env->current_module = cl->module;
                env->returning      = false;
                env->in_no_unwind   = fn->binding && fn->binding->no_unwind;
                control = fn->body; cf = call_frame; tail = true; descending = true;
                break;
            }
            case DK_CALL_RET: {
                /* Folded callee body produced cur.  Epilogue = the retired
                 * eval_apply_inner's single-activation tail (restore
                 * in_no_unwind, fire this call's
                 * defers, resolve the return/throw/value, consume an early
                 * `return` at the function boundary) + the eval_apply wrapper's
                 * module restore. */
                env->in_no_unwind = top->was_no_unwind;
                fire_defers_to_mark_reversed(env, (DeferItem *)top->aux, NULL);
                TuriValue ret;
                if (env->returning) {
                    ret = env->return_value;
                    env->returning = top->was_returning;  /* boundary consumes return */
                } else if (env->throwing) {
                    ret = env->throw_value;
                } else {
                    ret = cur;
                }
                env->current_module = top->saved_module;
                cur = ret; len--;   /* pop; propagate the call's value */
                break;
            }
            case DK_BUILTIN_ARG: {
                const Expr        *be   = top->expr;
                const BuiltinSpec *spec = be->as.builtin.spec;
                uint32_t           n    = be->as.builtin.n;
                TuriValue         *acc  = (TuriValue *)top->aux;  /* NULL if short-circuit */
                if (signaled) { if (acc) free(acc); len--; break; }
                if (spec->shape == BS_AND_SC) {
                    if (!turi_is_truthy(cur)) { cur = turi_bool(false); len--; break; }
                } else if (spec->shape == BS_OR_SC) {
                    if (turi_is_truthy(cur)) { cur = turi_bool(true); len--; break; }
                } else {
                    acc[top->index] = cur;
                }
                top->index++;
                if (top->index < n) {
                    control = be->as.builtin.args[top->index];
                    cf = top->frame; tail = false; descending = true;  /* args non-tail */
                } else if (spec->shape == BS_AND_SC) {
                    cur = turi_bool(true);  len--;   /* all operands truthy */
                } else if (spec->shape == BS_OR_SC) {
                    cur = turi_bool(false); len--;   /* no operand truthy */
                } else {
                    cur = eval_builtin(env, spec, acc, n);
                    free(acc); len--;
                }
                break;
            }
            case DK_MAKE_STRUCT: {
                TuriValue *fields = (TuriValue *)top->aux;
                if (signaled) { free(fields); len--; break; }  /* abandon, propagate */
                const Expr *me = top->expr;
                uint32_t    n  = me->as.make_struct_.n_fields;
                fields[top->index] = cur;
                top->index++;
                if (top->index < n) {
                    control = me->as.make_struct_.field_values[top->index];
                    cf = top->frame; tail = false; descending = true;  /* fields non-tail */
                } else {
                    StructDef *sd = me->as.make_struct_.def;
                    /* make_struct_val_def copies the fields, so free after. */
                    cur = make_struct_val_def(sd ? sd->name : "<struct>",
                                              n, fields, sd);
                    free(fields);
                    len--;  /* pop; keep returning the struct value */
                }
                break;
            }
            case DK_PROMPT: {
                /* The handle body (or a resumed slice) finished with `cur`,
                 * which is the handle's / resume's value.  Restore the env
                 * boundary and, for a resume prompt, cache the first result. */
                env->current_module = top->saved_module;
                env->in_no_unwind   = top->was_no_unwind;
                if (!signaled && top->prompt_cont) {
                    TuriWsCont *wc = (TuriWsCont *)top->prompt_cont;
                    if (!wc->resumed) { wc->resumed = true; wc->cached = cur; }
                }
                len--;   /* pop; propagate cur (value or signal) */
                break;
            }
            }
        }
    }
done:
    if (st != inl) free(st);
    #undef DRIVE_PUSH
    return result;
}

/* Plain expression entry: descend `e` non-tail (no activation seed). */
static TuriValue eval_drive(TuriEnv *env, EvalFrame *frame, const Expr *e) {
    return eval_drive_ex(env, frame, e, NULL);
}

/* -------------------------------------------------------------------------
 * Function application (F4: unified on the driver)
 *
 * eval_apply_driven dispatches leaf closures (native / inline-C) directly and
 * runs every turi-body closure through eval_drive_ex with an activation seed,
 * so the body -- and the whole tail-call chain reachable from it -- is driven
 * by the single explicit-stack evaluator.  Tail recursion stays O(1) via the
 * DK_CALL_RET frame reuse in eval_drive_ex (F3); there is no longer a separate
 * TcoFrame trampoline.  The DK_CALL_RET epilogue restores the caller's module,
 * so eval_apply is just the (idempotent) module-save wrapper retained for the C
 * call sites (turi_call, thunks, the fiber thunk, the driver's leaf path).
 *
 * Note: a native HOF that re-enters evaluation via turi_call still C-recurses
 * (one eval_apply_driven -> eval_drive_ex per re-entry); the env->eval_depth
 * guard remains load-bearing for that path (trampoline plan T3.4).
 * ---------------------------------------------------------------------- */
static TuriValue eval_apply_driven(TuriEnv *env, TuriClosure *cl,
                                   TuriValue *args, uint32_t n_args) {
    /* SB3: step-fuel check for the activation (TCO iterations are charged in
     * the DK_CALL_ARG reuse path).  Mirrors the retired eval_apply_inner's per-call charge,
     * which preceded even the native dispatch. */
    if (env->step_fuel_limit > 0) {
        if (env->step_fuel == 0)
            return turi_error("eval: step fuel exhausted");
        env->step_fuel--;
    }

    /* Phase S7: native function dispatch -- leaf, no driver. */
    if (cl->native)
        return cl->native(env, args, n_args, cl->native_ud);

    FnDef *fn = (FnDef *)cl->fn;
    /* EX_CLOSURE adds a synthetic __env_p first param for codegen; skip it. */
    uint32_t param_offset     = cl->skip_env_param ? 1u : 0u;
    uint32_t effective_params = (uint32_t)fn->n_params - param_offset;
    if (effective_params != n_args) {
        return turi_errorf("eval: arity mismatch: %s expects %u args, got %u",
                           fn->binding ? fn->binding->name->name : "<fn>",
                           (unsigned)effective_params, (unsigned)n_args);
    }

    /* Native override for inline-C functions (registered by worker/stdlib). */
    if (fn->body && fn->body->kind == EX_INLINE_C && fn->binding) {
        /* SB: block inline-C in sandboxed environments before native lookup. */
        if (!turi_env_has_cap(env, TURI_CAP_INLINE_C))
            return turi_error("eval: inline-C not allowed in sandboxed environment");
        const char *fname = fn->binding->name->name;
        TuriValue native_v = turi_env_get(env, fname);
        if (native_v.tag == TURI_CLOSURE && native_v.as_closure &&
            native_v.as_closure->native) {
            return native_v.as_closure->native(env, args, n_args,
                                               native_v.as_closure->native_ud);
        }
    }

    /* Inline-C pattern executor (leaf). */
    if (fn->body && fn->body->kind == EX_INLINE_C) {
        /* SB: block inline-C in sandboxed environments. */
        if (!turi_env_has_cap(env, TURI_CAP_INLINE_C))
            return turi_error("eval: inline-C not allowed in sandboxed environment");
        InlineC *ic = fn->body->as.inline_c_.inline_c;
        const char *body = ic->code.p;
        size_t blen = (size_t)ic->code.len;
        char *body_copy = NULL;
        if (body && blen > 0) {
            body_copy = (char*)malloc(blen + 1);
            if (body_copy) { memcpy(body_copy, body, blen); body_copy[blen] = '\0'; }
        }
        if (body_copy) {
            TuriValue inline_result;
            bool handled = try_exec_simple_inline_c(env, body_copy, blen,
                                                     args, n_args, fn,
                                                     param_offset, &inline_result);
            free(body_copy);
            if (handled) {
                /* ADT/struct carrier re-tag: an inline-C fn declared to return a
                 * user ADT/struct round-trips the TuriStruct* through an int64_t
                 * field, so the simple executor hands it back as a bare TURI_INT;
                 * reinterpret it so a downstream `match` (tag == TURI_STRUCT)
                 * still finds its arm.  Guard on non-null to leave a genuine
                 * 0/nil carrier alone. */
                if (inline_result.tag == TURI_INT && inline_result.as_int != 0 &&
                    (fn->return_type.kind == TY_ADT ||
                     fn->return_type.kind == TY_STRUCT)) {
                    inline_result = turi_struct_val(
                        (TuriStruct *)(intptr_t)inline_result.as_int);
                }
                return inline_result;
            }
        }
    }

    /* Defensive: an empty body evaluates to nil with no scope effects (matches
     * the retired eval_body_tco(NULL)). */
    if (!fn->body)
        return turi_nil();

    /* Turi body: prologue (build call frame, bind args, publish callee state),
     * then drive the body in tail position with an activation seed so its tail
     * calls reuse this activation's DK_CALL_RET. */
    EvalFrame *call_frame = eval_frame_new((EvalFrame *)cl->captured);
    for (uint32_t i = 0; i < n_args; i++)
        frame_bind(call_frame, fn->params[param_offset + i]->name->name, args[i]);

    DriveSeed seed = {
        .defer_mark    = (DeferItem *)env->defer_stack,
        .saved_module  = env->current_module,
        .was_returning = env->returning,
        .was_no_unwind = env->in_no_unwind,
    };
    env->current_module = cl->module;
    env->returning      = false;
    env->in_no_unwind   = fn->binding && fn->binding->no_unwind;

    return eval_drive_ex(env, call_frame, fn->body, &seed);
}

/* Module-save wrapper around the driven evaluator: saves and restores
 * env->current_module across the whole call (including any tail-call chain), so
 * a callee's module context never leaks back to its caller.  The DK_CALL_RET
 * epilogue already restores it to the same value, so this is idempotent for the
 * turi-body path and the safety net for the native/inline-C leaf path.
 *
 * F5: this is also where the eval_depth recursion guard now lives.  Before the
 * unification, the guard rode on eval_expr, which the OLD eval_body_tco called
 * once per HOF re-entry level (evaluating the recursive call's args).  The
 * driver evaluates control flow and call args on its heap work-stack WITHOUT
 * re-entering eval_expr, so a native HOF that re-applies a recursing closure
 * (turi_call -> eval_apply -> eval_apply_driven -> eval_drive_ex -> leaf native
 * -> turi_call -> ...) would otherwise never bump eval_depth and would
 * stack-overflow instead of tripping the limit.  eval_apply is on every such
 * C-recursion cycle (and is bypassed entirely by the folded non-tail and reused
 * tail paths, which stay heap-/O(1)-bounded), so guarding it bounds exactly the
 * residual native-re-entry recursion -- nothing else. */
static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args) {
    if (env->eval_depth >= env->max_eval_depth)
        return turi_error("eval: recursion limit exceeded");
    env->eval_depth++;
    const char *saved_module = env->current_module;
    TuriValue r = eval_apply_driven(env, cl, args, n_args);
    env->current_module = saved_module;
    env->eval_depth--;
    return r;
}

/* -------------------------------------------------------------------------
 * Expression evaluator
 * ---------------------------------------------------------------------- */

#define MAX_EVAL_ARGS 64

static TuriValue eval_expr_impl(TuriEnv *env, EvalFrame *frame, const Expr *e);

static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e) {
    if (!e) return turi_nil();
    if (env->returning) return env->return_value;
    if (env->throwing)  return env->throw_value;
    /* SB3: step-fuel check (skipped when limit == 0, i.e. unrestricted envs) */
    if (env->step_fuel_limit > 0) {
        if (env->step_fuel == 0)
            return turi_error("eval: step fuel exhausted");
        env->step_fuel--;
    }
    if (env->eval_depth >= env->max_eval_depth)
        return turi_error("eval: recursion limit exceeded");
    env->eval_depth++;
    TuriValue r = eval_expr_impl(env, frame, e);
    env->eval_depth--;
    return r;
}

static TuriValue eval_expr_impl(TuriEnv *env, EvalFrame *frame, const Expr *e) {
    if (!e) return turi_nil();

    switch (e->kind) {

    /* --- Literals -------------------------------------------------------- */
    case EX_NIL_LIT:
        return turi_nil();

    case EX_BOOL_LIT:
        return turi_bool(e->as.b);

    case EX_INT_LIT:
        return turi_int(e->as.i);

    case EX_FLOAT_LIT:
        return turi_float(e->as.f);

    case EX_DEFAULT_OF: {
        /* M2b: (default-of T) yields a zero-valued T.  The interpreter uses
         * int64_t as the universal scalar carrier and zero structs/aggregates
         * are not first-class here -- every Turmeric type the tree-walker
         * actually evaluates fits in TURI_INT/FLOAT/NIL/BOOL with a zero
         * bit-pattern.  Match the result type's broad kind to pick the
         * carrier; default to integer zero (a NULL pointer also fits). */
        if (e->type.kind == TY_FLOAT32 || e->type.kind == TY_FLOAT64) {
            return turi_float(0.0);
        }
        if (e->type.kind == TY_BOOL) {
            return turi_bool(false);
        }
        if (e->type.kind == TY_NIL) {
            return turi_nil();
        }
        return turi_int(0);
    }

    case EX_CSTR_LIT: {
        /* Intern the string so that identical literals share the same pointer.
         * This is required for HAMT key comparison (pointer equality) to work
         * correctly when the same string literal appears multiple times. */
        StrSlice sl = { e->as.s.p, e->as.s.len };
        const Symbol *sym = symtab_intern(&env->st, sl);
        return turi_cstr(sym->name);
    }

    case EX_SYM_LIT: {
        /* SYM (turi): a first-class :Sym value (-Xsymbols).  Compiled code lowers
         * it to a static `struct __tur_sym *`; the interpreter instead re-interns
         * the name into env->st and carries the stable `const Symbol *` as the
         * int64 carrier.  Interning by name guarantees pointer identity for
         * identical names (so Eq[Sym]/Hash[Sym] -- pointer identity -- and the
         * str->sym round-trip all agree with the literal), and the native sym ops
         * (native_sym_to_str/_eq/_hash in main.c, registered as overrides for the
         * inline-C sym.tur bodies the tree-walker cannot run) read this Symbol*. */
        const Symbol *s = e->as.sym_lit_.sym;
        const Symbol *isym = s;
        if (s) {
            StrSlice sl = { s->name, s->len };
            isym = symtab_intern(&env->st, sl);
        }
        TuriValue v = {0};
        v.tag = TURI_INT;
        v.as_int = (int64_t)(intptr_t)isym;
        return v;
    }

    /* --- Variable -------------------------------------------------------- */
    case EX_VAR:
        return eval_lookup(env, frame, e->as.var.binding->name->name);

    /* --- Let / Letrec ---------------------------------------------------- */
    /* T2: delegated to the explicit-stack driver, which owns the new frame and
     * its defer scope on the work-stack (DK_LET_BIND/DK_LET_BODY) so directly
     * nested lets do not grow the C stack.  EX_LETREC pre-binds its names to nil
     * and re-homes captureless fn literals onto the frame, identically to the
     * recursive version preserved in git history. */
    case EX_LET:
    case EX_LETREC:
        return eval_drive(env, frame, e);

    /* --- If / Do / Program ----------------------------------------------- */
    /* T2: the explicit-stack driver flattens directly-nested branch chains and
     * sequences onto a heap work-stack (see eval_drive).  Semantics are
     * identical to the former per-case recursion preserved in git history. */
    case EX_IF:
    case EX_DO:
    case EX_PROGRAM:
        return eval_drive(env, frame, e);

    /* --- While ----------------------------------------------------------- */
    case EX_WHILE: {
        while (1) {
            TuriValue cond = eval_expr(env, frame, e->as.while_.cond);
            if (turi_is_error(cond) || env->returning || env->throwing) return cond;
            if (!turi_is_truthy(cond)) break;
            TuriValue body = eval_expr(env, frame, e->as.while_.body);
            if (turi_is_error(body) || env->returning) return body;
        }
        return turi_nil();
    }

    /* --- Set ------------------------------------------------------------- */
    case EX_SET: {
        TuriValue v = eval_expr(env, frame, e->as.set_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        const char *name = e->as.set_.target->name->name;
        if (!eval_frame_update(frame, name, v)) {
            turi_env_set(env, name, v);
        }
        return turi_nil();
    }

    /* --- Def (top-level binding) ---------------------------------------- */
    case EX_DEF: {
        TuriValue v;
        if (e->as.def_.struct_def) {
            v = turi_struct_type_val(e->as.def_.struct_def->name);
        } else {
            v = eval_expr(env, frame, e->as.def_.init);
            if (turi_is_error(v) || env->returning || env->throwing) return v;
        }
        turi_env_set(env, e->as.def_.binding->name->name, v);
        return v;
    }

    /* --- Builtin --------------------------------------------------------- */
    case EX_BUILTIN:
        /* T3.1: delegated to the explicit-stack driver (DK_BUILTIN_ARG), which
         * accumulates args on the work-stack and handles the and/or
         * short-circuit, then applies eval_builtin.  Semantics match the former
         * inline loop (incl. the empty-and->true / empty-or->false edge). */
        return eval_drive(env, frame, e);

    /* --- Named function definition (defn) -------------------------------- */
    case EX_FN_DEF: {
        FnDef *fndef = e->as.fn_def_.fn;
        const char *fname = fndef->binding->name->name;

        /* Module-private mangling: a defn inside a defmodule that is NOT in the
         * module's export list is registered under "<module>/<name>" so two
         * modules' same-named privates do not clobber one another in the flat
         * global namespace (codegen mangles these per-module too). Exported and
         * non-module defns keep the bare name. The closure is tagged with its
         * owning module either way, so its body can resolve sibling privates. */
        const DefModule *dm = (const DefModule *)env->defining_mod;
        const char *modname = (dm && dm->name) ? dm->name->name : NULL;
        bool exported = false;
        if (dm) {
            for (uint32_t i = 0; i < dm->n_exports; i++) {
                if (dm->exports[i] && strcmp(dm->exports[i]->name, fname) == 0) {
                    exported = true; break;
                }
            }
        }
        const char *qkey = NULL;
        bool is_private = (modname && !exported);
        if (is_private) {
            size_t need = strlen(modname) + 1 + strlen(fname) + 1;
            char *qk = (char *)malloc(need);  /* env keeps the pointer; leak ok */
            if (qk) { snprintf(qk, need, "%s/%s", modname, fname); qkey = qk; }
        }
        /* The key whose existing native (if any) must not be clobbered. For a
         * private defn that is the qualified key; otherwise the bare name. */
        const char *primary_key = qkey ? qkey : fname;

        /* If the body is inline-C and a native override is already registered
         * under the primary key, keep the native rather than overwriting it. */
        if (fndef->body && fndef->body->kind == EX_INLINE_C) {
            TuriValue existing = turi_env_get(env, primary_key);
            if (existing.tag == TURI_CLOSURE && existing.as_closure &&
                existing.as_closure->native) {
                return existing; /* keep native override */
            }
        }
        TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl)); /* zero native/skip_env_param/native_ud */
        cl->fn       = fndef;
        cl->captured = NULL; /* top-level defn has no captured environment */
        cl->module   = modname; /* publish owning module while the body runs */
        TuriValue v  = turi_closure(cl);
        if (qkey) {
            /* Private: the qualified key is authoritative for intra-module
             * resolution. Also publish under the bare name as a flat-namespace
             * fallback, but only when it is still free -- so a second module's
             * same-named private (or a real native) is never clobbered. The
             * bare alias keeps the entry-point `main` and legacy cross-module
             * bare references reachable. */
            turi_env_set(env, qkey, v);
            if (turi_env_get(env, fname).tag == TURI_ERROR)
                turi_env_set(env, fname, v);
        } else {
            turi_env_set(env, fname, v);
        }
        return v;
    }

    /* --- Extern C declaration -- register nil stub in interpreter mode ----- */
    case EX_EXTERN_C: {
        ExternC *ec = e->as.extern_c_.ext;
        if (ec && ec->binding) {
            const char *fname = ec->binding->name->name;
            /* Only register if not already bound (avoid overwriting native impls). */
            TuriValue existing = turi_env_get(env, fname);
            if (existing.tag == TURI_ERROR)
                register_extern_c_known(env, fname);
        }
        return turi_nil();
    }

    /* --- Anonymous function (fn) ---------------------------------------- */
    case EX_FN: {
        TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl));
        cl->fn       = e->as.fn_.fn;
        cl->captured = frame; /* capture lexical scope */
        return turi_closure(cl);
    }

    /* --- Closure with captured variables (fn with captures) --------------- */
    case EX_CLOSURE: {
        TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl));
        cl->fn             = e->as.closure_.closure->fn;
        cl->captured       = frame; /* interpreter uses lexical frame */
        cl->skip_env_param = true;  /* codegen added __env_p as first param */
        return turi_closure(cl);
    }

    /* --- Function call --------------------------------------------------- */
    case EX_CALL:
        /* T3.2a: delegated to the explicit-stack driver (DK_CALL_ARG), which
         * resolves the callee and accumulates args on the work-stack, then
         * applies via eval_apply.  Semantics match the former inline loop; the
         * body-in-loop + TCO fold that removes the non-tail ceiling is T3.2b. */
        return eval_drive(env, frame, e);

    /* --- Early return ---------------------------------------------------- */
    case EX_RETURN: {
        TuriValue v = turi_nil();
        if (e->as.return_.value) {
            v = eval_expr(env, frame, e->as.return_.value);
            if (turi_is_error(v)) return v;
        }
        env->returning    = true;
        env->return_value = v;
        return v;
    }

    /* --- Typeclass/instance definitions — no runtime action -------------- */
    case EX_TYPECLASS_DEF:
    case EX_INSTANCE_DEF:
        return turi_nil();

    /* --- Module — evaluate body ----------------------------------------- */
    case EX_DEFMODULE: {
        DefModule *mod = e->as.defmodule_.mod;
        TuriValue  last = turi_nil();
        /* Publish the module being defined so EX_FN_DEF can register private
         * (non-exported) helpers under a per-module qualified key. */
        const void *saved_defining = env->defining_mod;
        env->defining_mod = mod;
        for (uint32_t i = 0; i < mod->n_body; i++) {
            last = eval_expr(env, frame, mod->body[i]);
            if (turi_is_error(last) || env->returning || env->throwing) {
                env->defining_mod = saved_defining;
                return last;
            }
        }
        env->defining_mod = saved_defining;
        return last;
    }

    /* --- Phase S4: Structs ---------------------------------------------- */

    case EX_MAKE_STRUCT:
        /* T2: delegated to the explicit-stack driver (DK_MAKE_STRUCT), which
         * accumulates the evaluated fields on a heap buffer hung off the
         * work-stack so a struct whose field is itself a struct literal does not
         * grow the C stack.  Semantics match the former per-field loop.  The
         * EVAL_SCRATCH_INLINE inline buffer added in T1 no longer applies here
         * (the accumulator must persist across the per-field descents). */
        return eval_drive(env, frame, e);

    case EX_SET_LIT: {
        /* Build a sorted, deduplicated int64_t set stored as {int64_t *items, int64_t n}.
         * Represented as TURI_INT (opaque pointer) matching tur_set_t layout. */
        uint32_t raw_n = e->as.set_lit_.n;
        int64_t *raw = raw_n ? (int64_t*)malloc(raw_n * sizeof(int64_t)) : NULL;
        uint32_t k = 0;
        for (uint32_t si = 0; si < raw_n; si++) {
            TuriValue iv = eval_expr(env, frame, e->as.set_lit_.items[si]);
            if (turi_is_error(iv) || env->returning || env->throwing) {
                free(raw); return iv;
            }
            raw[k++] = iv.as_int;
        }
        /* Sort */
        if (k > 1) {
            /* Simple insertion sort (k is small in tests) */
            for (uint32_t i = 1; i < k; i++) {
                int64_t key = raw[i]; int32_t j = (int32_t)i - 1;
                while (j >= 0 && raw[j] > key) { raw[j+1]=raw[j]; j--; }
                raw[j+1] = key;
            }
        }
        /* Deduplicate in-place */
        uint32_t uniq = 0;
        for (uint32_t i = 0; i < k; i++) {
            if (uniq == 0 || raw[uniq-1] != raw[i]) raw[uniq++] = raw[i];
        }
        /* Allocate set struct: {int64_t *items; int64_t n} */
        int64_t *s = (int64_t*)malloc(2 * sizeof(int64_t));
        if (!s) { free(raw); return turi_nil(); }
        s[0] = uniq ? (int64_t)(intptr_t)raw : 0;
        s[1] = (int64_t)uniq;
        if (uniq == 0) free(raw);
        TuriValue sv = {0}; sv.tag = TURI_INT; sv.as_int = (int64_t)(intptr_t)s;
        return sv;
    }

    case EX_GET_FIELD: {
        TuriValue sv = eval_expr(env, frame, e->as.get_field_.struct_expr);
        if (turi_is_error(sv) || env->returning || env->throwing) return sv;
        uint32_t idx = e->as.get_field_.field_idx;
        /* W1b: a struct can reach a field access via the int64 carrier ABI
         * rather than as a TuriStruct -- e.g. a Result that flowed as :int and
         * was ascribed back to (Result A B) with `(:: carrier ...)`.  The
         * carrier is a pointer to an int64[n] box laid out one word per field
         * (this is how native ok/err/result-map build a Result), so read word
         * idx and tag it by the field's static type.  This is what lets
         * result.tur's field-access accessors (ok-val/err-val/...) work on the
         * native box, not just on make-struct TuriStructs. */
        if (sv.tag == TURI_INT) {
            if (sv.as_int == 0)
                return turi_error("eval: field access on null carrier");
            int64_t w = ((int64_t *)(intptr_t)sv.as_int)[idx];
            switch (e->type.kind) {
            case TY_BOOL:  return turi_bool(w != 0);
            case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32: {
                /* carrier stores the raw bits of the field */
                double d; memcpy(&d, &w, sizeof(d)); return turi_float(d);
            }
            default: return turi_int(w);
            }
        }
        if (sv.tag != TURI_STRUCT)
            return turi_errorf("eval: field access on non-struct (tag %d)", sv.tag);
        if (idx >= sv.as_struct->n_fields)
            return turi_errorf("eval: field index %u out of bounds (%u fields)",
                               idx, sv.as_struct->n_fields);
        return sv.as_struct->fields[idx];
    }

    /* --- Phase DS3: (set! (.field s) v) — struct field write ------------- */
    case EX_SET_FIELD: {
        TuriValue recv = eval_expr(env, frame, e->as.set_field_.receiver);
        if (turi_is_error(recv) || env->returning || env->throwing) return recv;
        /* Resolve the underlying struct.  A mutable borrow is represented as a
         * TURI_REF to the binding holding the struct; an rc<Struct> stores the
         * struct in the __rc payload (field[1]). */
        if (recv.tag == TURI_REF && recv.as_ref)
            recv = ((EvalBinding *)recv.as_ref)->value;
        if (recv.tag != TURI_STRUCT || !recv.as_struct)
            return turi_errorf("eval: set-field on non-struct (tag %d)", recv.tag);
        TuriStruct *s = recv.as_struct;
        if (e->as.set_field_.receiver_is_rc &&
            s->name && strcmp(s->name, "__rc") == 0 && s->n_fields >= 2 &&
            s->fields[1].tag == TURI_STRUCT && s->fields[1].as_struct)
            s = s->fields[1].as_struct;
        uint32_t idx = e->as.set_field_.field_idx;
        if (idx >= s->n_fields)
            return turi_errorf("eval: set-field index %u out of bounds (%u fields)",
                               idx, s->n_fields);
        TuriValue v = eval_expr(env, frame, e->as.set_field_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        s->fields[idx] = v;
        return turi_nil();
    }

    /* --- Phase S4: Defer ------------------------------------------------ */

    /* (defer body) — register body to fire at enclosing function exit. */
    case EX_DEFER: {
        /* Snapshot the captured bindings at defer-call time. */
        EvalFrame *snap = eval_frame_new(NULL);
        for (uint8_t i = 0; i < e->as.defer_.n_captures; i++) {
            Binding *b = e->as.defer_.captures[i];
            TuriValue v = eval_lookup(env, frame, b->name->name);
            frame_bind(snap, b->name->name, v);
        }
        DeferItem *item = (DeferItem *)malloc(sizeof(DeferItem));
        item->body     = e->as.defer_.body;
        item->snapshot = snap;
        item->next     = (DeferItem *)env->defer_stack;
        env->defer_stack = item;
        return turi_nil();
    }

    /* --- Phase S4: Algebraic effects ------------------------------------ */

    /* (defeffect Name [...] :type) — type-level only; no runtime action. */
    case EX_DEFECT:
        return turi_nil();

    /* --- G0/G1: ADT and GADT definitions --------------------------------- */
    case EX_DEFDATA:
    case EX_DEFGADT: {
        AdtDef *adt = (e->kind == EX_DEFDATA)
            ? e->as.defdata_.def : e->as.defgadt_.def;
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            CtorDef *ctor = adt->ctors[ci];
            /* Register each constructor as a native that builds a TuriStruct. */
            turi_env_register_native(env, ctor->name,
                                     adt_ctor_native, (void *)ctor);
        }
        return turi_nil();
    }

    /* --- G0: match expression -------------------------------------------- */
    case EX_MATCH:
        /* T3.0: delegated to the explicit-stack driver, which resolves the arm
         * via eval_match_resolve and descends the arm body in the loop so a
         * match-recursive callee body does not grow the C stack (prerequisite
         * for the T3 call fold).  Semantics match the former inline version. */
        return eval_drive(env, frame, e);

    /* --- DV0/DV1: Dynamic variables -------------------------------------- */
    case EX_DEFDYNAMIC: {
        DynVarEntry *entry = e->as.defdynamic_.entry;
        TuriValue root = eval_expr(env, frame, e->as.defdynamic_.root_expr);
        if (turi_is_error(root) || env->returning || env->throwing) return root;
        turi_env_set(env, entry->name->name, root);
        return turi_nil();
    }
    case EX_DYNVAR_READ: {
        DynVarEntry *entry = e->as.dynvar_read_.entry;
        return turi_env_get(env, entry->name->name);
    }
    case EX_DYNVAR_BINDING: {
        uint32_t   n_pairs = e->as.dynvar_binding_.n_pairs;
        DynBinding *pairs  = e->as.dynvar_binding_.pairs;

        TuriValue *saved = (TuriValue *)malloc(n_pairs * sizeof(TuriValue));
        for (uint32_t pi = 0; pi < n_pairs; pi++)
            saved[pi] = turi_env_get(env, pairs[pi].entry->name->name);

        uint32_t installed = 0;
        for (; installed < n_pairs; installed++) {
            TuriValue ov = eval_expr(env, frame, pairs[installed].override_expr);
            if (turi_is_error(ov) || env->returning || env->throwing) {
                for (uint32_t ri = 0; ri < installed; ri++)
                    turi_env_set(env, pairs[ri].entry->name->name, saved[ri]);
                free(saved);
                return ov;
            }
            turi_env_set(env, pairs[installed].entry->name->name, ov);
        }

        TuriValue result = eval_expr(env, frame, e->as.dynvar_binding_.body);

        for (uint32_t pi = 0; pi < n_pairs; pi++)
            turi_env_set(env, pairs[pi].entry->name->name, saved[pi]);
        free(saved);
        return result;
    }
    case EX_DYNVAR_SET: {
        DynVarEntry *entry = e->as.dynvar_set_.entry;
        TuriValue v = eval_expr(env, frame, e->as.dynvar_set_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        turi_env_set(env, entry->name->name, v);
        return turi_nil();
    }

    /* (perform (EffectName arg1 ...)) — yield to nearest handler. */
    case EX_PERFORM: {
        PerformExpr *pe = e->as.perform_.perform;
        const char  *effect_name = pe->effect_name->name;

        /* T1: effect arity is capped at MAX_FN_ARITY; a fixed inline buffer (not
         * heap) keeps `args` alive across the swapcontext below, which borrows
         * it via cont->perf_args. */
        TuriValue args[EVAL_MAX_FN_ARITY];
        uint8_t   n_args = pe->n_args;
        if (n_args > EVAL_MAX_FN_ARITY)
            return turi_errorf("eval: too many effect arguments (%u)", n_args);

        for (uint8_t i = 0; i < n_args; i++) {
            args[i] = eval_expr(env, frame, pe->args[i]);
            if (turi_is_error(args[i]) || env->returning || env->throwing) return args[i];
        }

        /* Always yield to the innermost handler so nested handlers can
         * propagate unmatched effects upward (see eval_handle_inner).  This
         * path is reached for performs black-boxed away from the driver's
         * work-stack; capturable handles intercept perform in eval_drive_ex. */
        return eval_perform_fiber(env, effect_name, args, n_args);
    }

    /* (handle BODY cases...) — install handler, run BODY in a fiber. */
    case EX_HANDLE:
        return eval_handle(env, frame, e->as.handle_.handle);

    /* (handler (E [params] k) body) — build a detached handler value (TI6). */
    case EX_HANDLER_LIT: {
        const HandleExpr *h = e->as.handler_lit_.handle;
        TuriHandlerVal *hv = (TuriHandlerVal *)calloc(1, sizeof(TuriHandlerVal));
        if (!hv) return turi_error("eval: out of memory (handler value)");
        if (h->n_cases > TURI_MAX_HANDLER_CASES) {
            free(hv);
            return turi_errorf("eval: handler has too many cases (%u)", h->n_cases);
        }
        hv->n_cases = h->n_cases;
        for (uint8_t i = 0; i < h->n_cases; i++)
            hv->cases[i] = &h->cases[i];
        return turi_handler_val(hv);
    }

    /* (compose-handlers h1 h2) — concat two handler tables (TI6). */
    case EX_COMPOSE_HANDLERS: {
        TuriValue v1 = eval_expr(env, frame, e->as.compose_handlers_.h1);
        if (turi_is_error(v1) || env->returning || env->throwing) return v1;
        TuriValue v2 = eval_expr(env, frame, e->as.compose_handlers_.h2);
        if (turi_is_error(v2) || env->returning || env->throwing) return v2;
        if (v1.tag != TURI_HANDLER || v2.tag != TURI_HANDLER)
            return turi_error("eval: compose-handlers: operands must be handler values");
        TuriHandlerVal *a = v1.as_handler, *b = v2.as_handler;
        if ((uint32_t)a->n_cases + b->n_cases > TURI_MAX_HANDLER_CASES)
            return turi_error("eval: composed handler has too many cases");
        TuriHandlerVal *hv = (TuriHandlerVal *)calloc(1, sizeof(TuriHandlerVal));
        if (!hv) return turi_error("eval: out of memory (composed handler)");
        for (uint8_t i = 0; i < a->n_cases; i++) hv->cases[hv->n_cases++] = a->cases[i];
        for (uint8_t i = 0; i < b->n_cases; i++) hv->cases[hv->n_cases++] = b->cases[i];
        return turi_handler_val(hv);
    }

    /* (with-handler hv body) — apply a handler value to a body (TI6). */
    case EX_WITH_HANDLER: {
        TuriValue hvv = eval_expr(env, frame, e->as.with_handler_.handler);
        if (turi_is_error(hvv) || env->returning || env->throwing) return hvv;
        if (hvv.tag != TURI_HANDLER)
            return turi_error("eval: with-handler: first argument must be a handler value");
        TuriHandlerVal *hv = hvv.as_handler;

        /* Materialise a contiguous HandleCase array + HandleExpr so we can
         * reuse eval_handle.  Both live on this frame -- safe because
         * eval_handle runs the body (and all resumes) to completion before
         * returning, and continuations never escape it. */
        HandleCase cases[TURI_MAX_HANDLER_CASES];
        for (uint8_t i = 0; i < hv->n_cases; i++) cases[i] = *hv->cases[i];
        HandleExpr h;
        h.body    = e->as.with_handler_.body;
        h.cases   = cases;
        h.n_cases = hv->n_cases;
        return eval_handle(env, frame, &h);
    }

    /* (select ...) — multi-channel select (TI6, carved out).
     * Channels in Turmeric are inline-C structs (pthread mutex/condvar); the
     * interpreter has no channel runtime, and every existing select fixture is
     * inline-C-bound (a TI7 carve-out).  Fail cleanly rather than falling
     * through to the generic "unhandled expression kind" default.  See
     * docs/archive/turi-select-needs-channel-primitives.md and the
     * "Not interpreted: carve-outs" section of docs/guides/eval-api.md. */
    case EX_SELECT:
        return turi_error("eval: select is not supported in interpreter mode "
                          "(channels require native primitives; use the compiled path)");

    /* (resume k value) — resume a live continuation with a value. */
    case EX_RESUME: {
        ResumeExpr *re = e->as.resume_.resume;
        TuriValue k   = eval_expr(env, frame, re->k);
        if (turi_is_error(k) || env->returning || env->throwing) return k;
        TuriValue val = eval_expr(env, frame, re->value);
        if (turi_is_error(val) || env->returning || env->throwing) return val;

        if (k.tag != TURI_EFFECT_CONT)
            return turi_error("eval: resume: not a continuation");

        /* A work-stack continuation can only be resumed from inside the driver
         * (eval_drive_ex EX_RESUME), where the captured slice is pushed back
         * onto the live work-stack.  Reaching here means a capture/resume
         * crossed a black box (e.g. a native HOF callback) -- the deferred SR
         * case.  Fail cleanly rather than treating it as a fiber cont. */
        if (k.as_cont->ws)
            return turi_error("eval: cannot resume a work-stack continuation "
                              "through a non-driver (native HOF) frame");

        return eval_resume_cont(env, frame, k.as_cont, val);
    }

    /* (discontinue k exception) — abort the body with an error. */
    case EX_DISCONTINUE: {
        DiscontinueExpr *de = e->as.discontinue_.discontinue;
        TuriValue k = eval_expr(env, frame, de->k);
        if (turi_is_error(k) || env->returning || env->throwing) return k;
        TuriValue exc = eval_expr(env, frame, de->exception);
        if (turi_is_error(exc) || env->returning || env->throwing) return exc;

        if (k.tag != TURI_EFFECT_CONT)
            return turi_error("eval: discontinue: not a continuation");

        TuriEffectCont *cont = k.as_cont;
        cont->body_result = turi_is_error(exc)
                            ? exc
                            : turi_errorf("eval: discontinue: %s",
                                          exc.tag == TURI_CSTR ? exc.as_cstr : "exception");
        cont->done = true;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        swapcontext(&cont->body_ctx, &cont->handler_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
        return turi_nil(); /* unreachable from caller's perspective */
    }

    /* (cont? k) — true if k is an unconsumed continuation. */
    case EX_CONT_PRED: {
        TuriValue k = eval_expr(env, frame, e->as.cont_pred_.expr);
        if (turi_is_error(k) || env->returning || env->throwing) return k;
        return turi_bool(k.tag == TURI_EFFECT_CONT && k.as_cont != NULL);
    }

    /* --- Phase S5: inline-C is not executable in the tree-walk eval ------- */
    case EX_INLINE_C:
        if (!turi_env_has_cap(env, TURI_CAP_INLINE_C))
            return turi_error("eval: inline-C not allowed in sandboxed environment");
        /* R2 (turi-interpret-flip-residual-plan): gc!/gc-enable!/gc-disable!
         * lower (elab_memory.c) to these exact captureless inline-C one-liners.
         * Call the linked runtime (src/runtime/gc.c) so cycle collection runs
         * under --interpret instead of the clean carve below. */
        {
            InlineC *ic = e->as.inline_c_.inline_c;
            if (ic && ic->n_captures == 0 && ic->n_val_exprs == 0 && ic->code.p) {
                extern void gc_force(void);
                extern void gc_enable(void);
                extern void gc_disable(void);
                StrSlice c = ic->code;
                if (c.len == 11 && memcmp(c.p, "gc_force();", 11) == 0)   { gc_force();   return turi_nil(); }
                if (c.len == 12 && memcmp(c.p, "gc_enable();", 12) == 0)  { gc_enable();  return turi_nil(); }
                if (c.len == 13 && memcmp(c.p, "gc_disable();", 13) == 0) { gc_disable(); return turi_nil(); }
            }
        }
        /* This is the documented clean carve for any inline-C-backed function
         * the tree-walker cannot run -- e.g. a content-keyed map's synthesized
         * MapKey comparator, whose body returns a captured C function-pointer
         * address (turi-map-set-hamt-interpreter-gap.md, Tier B / prereq 2c).
         * It is a clean rc=1 error, never a crash or silent miscompile; point
         * the user at the compiled path, which implements these natively. */
        return turi_error("eval: inline-C not supported in interpreter mode "
                          "(function uses a native C implementation; run it with "
                          "`tur build`/`tur run` instead of `--interpret`)");

    /* --- Phase S7: async / await ----------------------------------------- */

    /* (async fn-expr) — spawn a fiber that evaluates fn-expr; return Future. */
    case EX_ASYNC: {
        if (!turi_env_has_cap(env, TURI_CAP_ASYNC))
            return turi_error("eval: async not allowed in sandboxed environment");

        /* Allocate the fiber's own future. */
        TuriFuture *f = turi_future_new(env);

        /* Pre-evaluate fn_expr in the current (main) context to get a closure.
         * This avoids binding-name lookup issues inside the fiber. */
        TuriValue cl_val = eval_expr(env, frame, e->as.async_.fn_expr);
        if (turi_is_error(cl_val) || env->returning || env->throwing) {
            return cl_val;
        }
        if (cl_val.tag != TURI_CLOSURE) {
            /* R4 (turi-interpret-flip-residual-plan): the elaborator does not
             * wrap a non-fn async body in a thunk -- `(async EXPR)` stores EXPR
             * directly (elab_concurrent.c).  When EXPR is not a `(fn ...)`
             * literal, the pre-evaluation above already ran the body to
             * completion in the current context (e.g. `(async (with-handler
             * ...))` settles to its int result).  Under the single-threaded
             * interpreter that is observationally a resolved future: settle it
             * with the value and hand back the future so `await` returns it,
             * instead of erroring "expected a function".  A `(fn [] ...)` thunk
             * still takes the fiber-spawn path below. */
            turi_future_resolve(env, f, cl_val);
            return turi_future_val(f);
        }

        /* Allocate and initialise the fiber struct. */
        TuriFiber *fiber = (TuriFiber *)calloc(1, sizeof(TuriFiber));
        if (!fiber) { return turi_error("eval: out of memory (async fiber)"); }

        fiber->own_future    = f;
        f->owner             = fiber;
        fiber->env           = env;
        fiber->fn_closure_val = cl_val;
        fiber->state         = TURI_FIBER_READY;
        fiber->cancelled     = false;

        /* Allocate fiber stack. */
#ifndef __EMSCRIPTEN__
        fiber->stack = (char *)mmap(NULL, TURI_ASYNC_STACK_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (fiber->stack == MAP_FAILED) {
            free(fiber);
            return turi_error("eval: mmap failed for async fiber stack");
        }
#else
        fiber->stack = (char *)malloc(TURI_ASYNC_STACK_SIZE);
        if (!fiber->stack) {
            free(fiber);
            return turi_error("eval: malloc failed for async fiber stack");
        }
#endif

#if !defined(__EMSCRIPTEN__) && defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        getcontext(&fiber->ctx);
#ifndef __EMSCRIPTEN__
        fiber->ctx.uc_stack.ss_sp   = fiber->stack;
        fiber->ctx.uc_stack.ss_size = TURI_ASYNC_STACK_SIZE;
        fiber->ctx.uc_link          = NULL;
#endif
        /* NOTE: g_pending_async_fiber is set right before swapcontext in the
         * scheduler, not here, so that creating multiple fibers before running
         * any of them doesn't overwrite the pointer prematurely. */
        makecontext(&fiber->ctx, async_fiber_thunk, 0);
#if !defined(__EMSCRIPTEN__) && defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

        /* Enqueue in the scheduler ready queue. */
        turi_sched_enqueue(env, fiber);

        return turi_future_val(f);
    }

    /* (await fut-expr) — wait for a Future to resolve; return its value. */
    case EX_AWAIT: {
        TuriValue fv = eval_expr(env, frame, e->as.await_.fut_expr);
        if (turi_is_error(fv) || env->returning || env->throwing) return fv;

        if (fv.tag != TURI_FUTURE)
            return turi_errorf("eval: await: expected a future, got tag %d", fv.tag);

        TuriFuture *f = fv.as_future;

        /* Already settled? */
        if (f->state == TURI_FUTURE_RESOLVED) return f->result;
        if (f->state == TURI_FUTURE_REJECTED) {
            if (f->result.tag == TURI_THROW) {
                env->throwing    = true;
                env->throw_value = f->result;
                return f->result;
            }
            if (turi_is_error(f->result)) {
                env->throwing    = true;
                env->throw_value = make_throw_val(f->result, TY_UNKNOWN);
                return env->throw_value;
            }
            return turi_error("eval: await: future rejected");
        }

        /* Pending: check if we are inside an async fiber. */
        TuriFiber *cur = env->current_fiber;
        if (cur) {
            /* Suspend current fiber until future resolves. */
            turi_future_add_waker(f, cur);
            cur->awaiting_future = f;
            cur->state = TURI_FIBER_SUSPENDED;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            swapcontext(&cur->ctx, &env->sched_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
            /* Resumed: future has settled. */
            if (f->state == TURI_FUTURE_RESOLVED) return f->result;
            if (f->state == TURI_FUTURE_REJECTED) {
                if (f->result.tag == TURI_THROW) {
                    env->throwing    = true;
                    env->throw_value = f->result;
                    return f->result;
                }
                if (turi_is_error(f->result)) {
                    env->throwing    = true;
                    env->throw_value = make_throw_val(f->result, TY_UNKNOWN);
                    return env->throw_value;
                }
                return turi_error("eval: await: future rejected");
            }
            return turi_error("eval: await: unexpected future state");
        } else {
            /* Main (non-fiber) context: run event loop until future resolves. */
            return turi_await_future(env, f);
        }
    }

    /* Phase S4: throw / try-catch */
    case EX_THROW: {
        TuriValue val = eval_expr(env, frame, e->as.throw_.value);
        if (turi_is_error(val) || env->returning || env->throwing) return val;
        /* Box the value as a TURI_THROW */
        TuriValue tv = make_throw_val(val, TY_UNKNOWN);
        env->throwing    = true;
        env->throw_value = tv;
        return tv;
    }
    case EX_TRY_CATCH: {
        /* Evaluate the body; if it throws, match the first catch clause */
        bool      saved_throwing  = env->throwing;
        TuriValue saved_throw_val = env->throw_value;
        env->throwing = false;

        TuriValue result = eval_expr(env, frame, e->as.try_catch_.body);

        if (env->throwing && e->as.try_catch_.n_catches > 0) {
            TuriValue thrown = env->throw_value;
            env->throwing    = false;

            /* Use the first matching catch clause (simple: always the first) */
            EvalFrame *cf = eval_frame_new(frame);
            if (e->as.try_catch_.catch_bindings[0]) {
                TuriValue caught_val = thrown;
                /* Unwrap TURI_THROW to get the inner value */
                if (thrown.tag == TURI_THROW && thrown.as_throw) {
                    caught_val = thrown.as_throw->value;
                }
                frame_bind(cf, e->as.try_catch_.catch_bindings[0]->name->name, caught_val);
            }
            result = eval_expr(env, cf, e->as.try_catch_.catch_handlers[0]);
            eval_frame_free(cf);
        } else if (!env->throwing) {
            /* No exception: restore saved state */
            env->throwing    = saved_throwing;
            env->throw_value = saved_throw_val;
        }
        /* If still throwing (unmatched or re-thrown), leave env->throwing set */
        return result;
    }

    /* --- Phase H §1: typeclass dictionary — return method closure ---------- */
    case EX_DICT: {
        TypeClassInstance *inst = e->as.dict_.instance;
        const char *mname = e->as.dict_.method_name;
        if (!inst || !mname || mname[0] == '\0') return turi_nil();
        TypeClass *tc = inst->typeclass;
        if (!tc) return turi_nil();
        /* Find the method index by comparing sanitized method names.
         * Sanitization: replace non-alphanumeric chars with '_'. */
        for (uint32_t mi = 0; mi < tc->n_methods; mi++) {
            const char *orig = tc->methods[mi].name->name;
            /* Build sanitized version of orig for comparison. */
            char san[128];
            size_t olen = strlen(orig);
            if (olen >= sizeof(san)) olen = sizeof(san) - 1;
            for (size_t k = 0; k < olen; k++)
                san[k] = (orig[k] >= '0' && orig[k] <= '9') ||
                         (orig[k] >= 'a' && orig[k] <= 'z') ||
                         (orig[k] >= 'A' && orig[k] <= 'Z') ? orig[k] : '_';
            san[olen] = '\0';
            if (strcmp(san, mname) != 0) continue;
            /* Found matching method. */
            if (mi >= inst->n_method_impls || !inst->method_impls[mi]) break;
            FnDef *impl = inst->method_impls[mi];
            TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
            memset(cl, 0, sizeof(*cl));
            cl->fn       = impl;
            cl->captured = NULL;
            return turi_closure(cl);
        }
        return turi_errorf("eval: EX_DICT: method '%s' not found in instance", mname);
    }

    /* --- Phase N: numeric type cast ---------------------------------------- */
    case EX_CAST: {
        TuriValue v = eval_expr(env, frame, e->as.cast_.expr);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        switch (e->as.cast_.target_kind) {
        case TY_INT:
        case TY_INT64:
            if (v.tag == TURI_FLOAT) return turi_int((int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int(v.as_int);
            return turi_int(0);
        case TY_INT8:
            if (v.tag == TURI_FLOAT) return turi_int((int8_t)(int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int((int8_t)v.as_int);
            return turi_int(0);
        case TY_INT16:
            if (v.tag == TURI_FLOAT) return turi_int((int16_t)(int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int((int16_t)v.as_int);
            return turi_int(0);
        case TY_INT32:
            if (v.tag == TURI_FLOAT) return turi_int((int32_t)(int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int((int32_t)v.as_int);
            return turi_int(0);
        case TY_UINT8:
            if (v.tag == TURI_FLOAT) return turi_int((int64_t)(uint8_t)(int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int((int64_t)(uint8_t)v.as_int);
            return turi_int(0);
        case TY_UINT16:
            if (v.tag == TURI_FLOAT) return turi_int((int64_t)(uint16_t)(int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int((int64_t)(uint16_t)v.as_int);
            return turi_int(0);
        case TY_UINT32:
            if (v.tag == TURI_FLOAT) return turi_int((int64_t)(uint32_t)(int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int((int64_t)(uint32_t)v.as_int);
            return turi_int(0);
        case TY_UINT64:
            if (v.tag == TURI_FLOAT) return turi_int((int64_t)(uint64_t)(int64_t)v.as_float);
            if (v.tag == TURI_INT)   return turi_int((int64_t)(uint64_t)v.as_int);
            return turi_int(0);
        case TY_FLOAT:
        case TY_FLOAT64:
            if (v.tag == TURI_INT)   return turi_float((double)v.as_int);
            if (v.tag == TURI_FLOAT) return turi_float(v.as_float);
            return turi_float(0.0);
        case TY_FLOAT32:
            if (v.tag == TURI_INT)   return turi_float((double)(float)v.as_int);
            if (v.tag == TURI_FLOAT) return turi_float((double)(float)v.as_float);
            return turi_float(0.0);
        case TY_BOOL:
            if (v.tag == TURI_INT)   return turi_bool(v.as_int != 0);
            return turi_bool(false);
        default:
            return v;
        }
    }
    case EX_REINTERPRET:
        /* A same-size scalar bit-reinterpret on the compiled path (raw int64
         * carrier).  It stays TRANSPARENT here -- and must.  The interpreter is
         * tag-preserving: a float boxed into an int64 carrier (ADT/cons/tyvar
         * slot, or `(:: f int)`) stays a TURI_FLOAT and is read back directly,
         * with no unbox reinterpret.  Any attempt to actually reinterpret one
         * direction breaks the round-trips that rely on tag preservation
         * (typed-slots/cons-float reads `.head` of a Cons[float] with no
         * reinterpret; typed-slots/ascribe-reinterpret round-trips i32<->f32 and
         * float<->carrier).  Because a `TURI_INT` cannot be distinguished as
         * "a genuine integer" vs "a carrier holding float bits", there is no
         * self-consistent partial reinterpret -- fully transparent is the only
         * correct choice.  Consequence: a literal `(:: 7 :float)` prints `7`
         * here but the bit pattern `3.45846e-323` compiled.  That divergence is
         * inherent to the tagged value model (same class as `(:: 7.1 :int)`
         * giving `7.1`), not a bug -- see
         * docs/reported/turi-map-nonint-value-carrier-ascription.md. */
        return eval_expr(env, frame, e->as.reinterpret_.expr);

    /* --- Phase 2: type ascription is transparent at runtime, except that it
     * must reconcile the runtime value tag with the ascribed primitive type.
     * The interpreter dispatches println/show on the runtime tag, so an int
     * literal ascribed to :bool (e.g. a return-type-directed Default[bool]
     * instance that returns `1`, then `(:: (default-of) bool)`) must become a
     * TURI_BOOL or it prints as `1` instead of `true`.  Mirror EX_CAST's
     * primitive coercions, but only when the tag actually mismatches so struct/
     * closure/ADT ascriptions stay transparent. */
    case EX_ASCRIBE: {
        TuriValue v = eval_expr(env, frame, e->as.ascribe_.inner);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        /* `::` is a representation assertion, NOT a numeric conversion (that is
         * EX_CAST / explicit int->float).  On the compiled path the carrier word
         * is reinterpreted bit-for-bit to the ascribed type -- so an int64 that
         * carries a double's bits (e.g. a type-erased Map[int float] value read
         * back as :float, or a char* read back as :cstr) must REINTERPRET, not
         * convert.  The previous numeric coercion here diverged from the
         * compiled path ((:: 7 :float) gave 7 under --interpret but 3.45e-323
         * compiled).  Act only on a tag mismatch so struct/closure/ADT
         * ascriptions stay transparent. */
        switch (e->type.kind) {
        case TY_BOOL:
            if (v.tag == TURI_INT) return turi_bool(v.as_int != 0);
            return v;
        case TY_FLOAT: case TY_FLOAT64:
            if (v.tag == TURI_INT) {
                union { int64_t i; double d; } u; u.i = v.as_int;
                return turi_float(u.d);
            }
            return v;
        case TY_FLOAT32:
            /* The compiled float32 ascription numeric-converts the carrier
             * (unlike float64); keep parity with it. */
            if (v.tag == TURI_INT) return turi_float((double)v.as_int);
            return v;
        case TY_INT: case TY_INT64:
            if (v.tag == TURI_FLOAT) {
                union { int64_t i; double d; } u; u.d = v.as_float;
                return turi_int(u.i);
            }
            return v;
        case TY_INT8: case TY_INT16: case TY_INT32:
            if (v.tag == TURI_FLOAT) return turi_int((int64_t)v.as_float);
            return v;
        case TY_CSTR:
            /* int64 carrier holds a char*; reinterpret so println shows text. */
            if (v.tag == TURI_INT) return turi_cstr((const char *)(intptr_t)v.as_int);
            return v;
        default:
            return v;
        }
    }

    /* --- Phase N: poly wrap is transparent in the interpreter -------------- */
    case EX_POLY_WRAP:
        return eval_expr(env, frame, e->as.poly_wrap_.inner);

    /* --- A#1: fat-closure shim is transparent in the interpreter ----------- */
    case EX_FN_TO_FAT:
        return eval_expr(env, frame, e->as.fn_to_fat_.inner);

    /* --- SC7: tur_poly_fn_t -> fat-handle conversion is transparent here ---- */
    case EX_POLY_TO_FAT:
        return eval_expr(env, frame, e->as.poly_to_fat_.inner);

    /* --- Phase 12: borrows --- */
    case EX_BORROW_IMMUT:
        return eval_expr(env, frame, e->as.borrow_immut_.expr);
    case EX_BORROW_MUT: {
        /* Return a TURI_REF pointing to the EvalBinding so set! can mutate it. */
        const Expr *inner = e->as.borrow_mut_.expr;
        if (inner->kind == EX_VAR) {
            const char *vname = inner->as.var.binding->name->name;
            for (EvalFrame *f = frame; f; f = f->parent) {
                for (EvalBinding *b = f->bindings; b; b = b->next) {
                    if (strcmp(b->name, vname) == 0) {
                        TuriValue ref; ref.tag = TURI_REF; ref.as_ref = b;
                        return ref;
                    }
                }
            }
        }
        /* Fallback: transparent */
        return eval_expr(env, frame, e->as.borrow_mut_.expr);
    }
    case EX_DEREF: {
        TuriValue r = eval_expr(env, frame, e->as.deref_.expr);
        if (turi_is_error(r) || env->returning || env->throwing) return r;
        if (r.tag == TURI_REF && r.as_ref)
            return ((EvalBinding *)r.as_ref)->value;
        return r;
    }
    case EX_SET_DEREF: {
        TuriValue ref = eval_expr(env, frame, e->as.set_deref_.ref);
        if (turi_is_error(ref) || env->returning || env->throwing) return ref;
        TuriValue v = eval_expr(env, frame, e->as.set_deref_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        if (ref.tag == TURI_REF && ref.as_ref)
            ((EvalBinding *)ref.as_ref)->value = v;
        return turi_nil();
    }

    /* --- Phase R2: panic terminates the program (or unwinds to catch-unwind) */
    case EX_PANIC: {
        TuriValue msg = eval_expr(env, frame, e->as.panic_.payload);
        const char *s = (msg.tag == TURI_CSTR && msg.as_cstr) ? msg.as_cstr : "(no message)";
        turi_runtime_panic(env, s);
        return turi_nil(); /* unreachable: turi_runtime_panic never returns */
    }

    case EX_PANIC_WITH: {
        /* TI5: typed panic payload.  Evaluate the value and stash its TypeKind,
         * value, and source line so catch-panic-of can filter by type and the
         * panic-payload-* accessors can read it. */
        TuriValue pv = eval_expr(env, frame, e->as.panic_with_.payload);
        if (turi_is_error(pv) || env->returning || env->throwing) return pv;
        if (env->panicking) {
            fprintf(stderr, "double panic: aborting\n");
            fflush(stderr);
            fflush(stdout);
            abort();
        }
        /* If a catch boundary is active, longjmp to it carrying the payload. */
        if (env->catch_jmp && !env->in_no_unwind) {
            strncpy(env->catch_panic_msg, "typed panic", sizeof(env->catch_panic_msg) - 1);
            env->catch_panic_msg[sizeof(env->catch_panic_msg) - 1] = '\0';
            env->catch_panic_type  = e->as.panic_with_.payload->type.kind;
            env->catch_panic_value = pv;
            env->catch_panic_file  = NULL;
            env->catch_panic_line  = (int)e->span.line;
            env->panicking = true;
            longjmp(*env->catch_jmp, 1);
        }
        env->panicking = true;
        fprintf(stderr, "panic at\n");
        fflush(stderr);
        fire_defers_to_mark_reversed(env, NULL, NULL);
        fflush(stdout);
        exit(1);
    }

    /* --- Phase R2: catch-unwind — catch interpreter panics at a boundary --- */
    case EX_CATCH_UNWIND: {
        /* Evaluate the thunk expression to get a closure value. */
        TuriValue thunk_val = eval_expr(env, frame, e->as.catch_unwind_.thunk);
        if (turi_is_error(thunk_val) || env->returning || env->throwing) return thunk_val;
        if (thunk_val.tag != TURI_CLOSURE)
            return turi_error("eval: catch-unwind: thunk must be a closure");

        /* Save env state that may be clobbered by longjmp. */
        jmp_buf  jb;
        jmp_buf *prev_jmp       = env->catch_jmp;
        bool     prev_returning = env->returning;
        bool     prev_throwing  = env->throwing;
        TuriValue prev_throw    = env->throw_value;
        TuriValue prev_ret      = env->return_value;
        bool     prev_panicking = env->panicking;
        /* W4: remember the defer stack so a caught panic fires the unwound
         * frames' defers *before* catch-unwind returns (the compiled path runs
         * defers during unwinding).  The longjmp out of a panic skips the normal
         * scope-exit defer firing, leaving them on the stack. */
        DeferItem *defer_mark = (DeferItem *)env->defer_stack;
        env->catch_jmp = &jb;

        TuriValue result;
        if (setjmp(jb) == 0) {
            /* Normal execution path — call the thunk with no arguments. */
            result = eval_apply(env, thunk_val.as_closure, NULL, 0);
            env->catch_jmp = prev_jmp;
            if (turi_is_error(result) || env->returning || env->throwing)
                return result;
            /* Wrap the successful result in (ok value).  Use the same 3-int
             * Result-box layout { is_ok, ok_val, err_val } that the native
             * ok/err/ok?/err? helpers produce and read, so the value composes
             * with ok?/err?/ok-val just like any other Result (Phase R2). */
            return turi_ok_result_box(result.as_int);
        } else {
            /* A panic was caught — restore env and return (err payload).  TI5:
             * the err slot carries a boxed TuriPanicPayload so panic-payload-*
             * accessors recover the caught value/type/file/line (the message is
             * the cstr value for a plain panic). */
            env->catch_jmp    = prev_jmp;
            env->panicking    = false;
            env->returning    = prev_returning;
            env->throwing     = prev_throwing;
            env->throw_value  = prev_throw;
            env->return_value = prev_ret;
            (void)prev_panicking;
            /* W4: fire the unwound frames' defers (LIFO) before returning, so
             * `(defer ...)` in the panicking thunk runs during the unwind. */
            fire_defers_to_mark_reversed(env, defer_mark, frame);
            return turi_err_result_box(env);
        }
    }

    /* --- Phase TI5: catch-panic-of — type-filtered panic catch ------------- */
    case EX_CATCH_PANIC_OF: {
        TuriValue thunk_val = eval_expr(env, frame, e->as.catch_panic_of_.thunk);
        if (turi_is_error(thunk_val) || env->returning || env->throwing) return thunk_val;
        if (thunk_val.tag != TURI_CLOSURE)
            return turi_error("eval: catch-panic-of: thunk must be a closure");
        int want_type = (int)e->as.catch_panic_of_.type_kind;

        jmp_buf  jb;
        jmp_buf *prev_jmp       = env->catch_jmp;
        bool     prev_returning = env->returning;
        bool     prev_throwing  = env->throwing;
        TuriValue prev_throw    = env->throw_value;
        TuriValue prev_ret      = env->return_value;
        env->catch_jmp = &jb;

        TuriValue result;
        if (setjmp(jb) == 0) {
            result = eval_apply(env, thunk_val.as_closure, NULL, 0);
            env->catch_jmp = prev_jmp;
            if (turi_is_error(result) || env->returning || env->throwing)
                return result;
            return turi_ok_result_box(result.as_int);
        } else {
            /* A panic reached this boundary.  Restore the saved control state
             * (the payload fields in env still describe the in-flight panic). */
            env->catch_jmp    = prev_jmp;
            env->returning    = prev_returning;
            env->throwing     = prev_throwing;
            env->throw_value  = prev_throw;
            env->return_value = prev_ret;

            if (env->catch_panic_type == want_type) {
                /* Matching type: consume the panic, return (err payload). */
                env->panicking = false;
                return turi_err_result_box(env);
            }
            /* Type mismatch: re-raise to the next outer boundary, or abort.
             * env->panicking stays true and the payload fields are untouched so
             * the outer boundary sees the original panic. */
            if (prev_jmp && !env->in_no_unwind)
                longjmp(*prev_jmp, 1);
            fprintf(stderr, "panic at\npanic: %s\n", env->catch_panic_msg);
            fflush(stderr);
            fire_defers_to_mark_reversed(env, NULL, NULL);
            fflush(stdout);
            exit(1);
        }
    }

    /* --- Phase TI5: panic-payload-* accessors ------------------------------ */
    case EX_PANIC_PAYLOAD_TYPE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_type_.payload);
        if (turi_is_error(p) || env->returning || env->throwing) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return turi_int(pp ? pp->type_tag : 0);
    }
    case EX_PANIC_PAYLOAD_VALUE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_value_.payload);
        if (turi_is_error(p) || env->returning || env->throwing) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return pp ? pp->value : turi_nil();
    }
    case EX_PANIC_PAYLOAD_FILE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_file_.payload);
        if (turi_is_error(p) || env->returning || env->throwing) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return turi_cstr(pp && pp->file ? pp->file : "");
    }
    case EX_PANIC_PAYLOAD_LINE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_line_.payload);
        if (turi_is_error(p) || env->returning || env->throwing) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return turi_int(pp ? pp->line : 0);
    }
    case EX_PANIC_PAYLOAD_DOWNS: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_downs_.payload);
        if (turi_is_error(p) || env->returning || env->throwing) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        if (pp && pp->type_tag == (int)e->as.panic_payload_downs_.target_type)
            return pp->value;
        return turi_nil();
    }

    /* --- Phase 9: rc<T> with shared reference counter in interpreter ------- */
    case EX_RC_OF: {
        /* Allocate the shared control block and wrap the value in an __rc struct.
         * The control block is a 2-slot counter: cnt[0] = strong count,
         * cnt[1] = weak count.  field[0] points at cnt[0], so the existing
         * `*cnt` strong-count readers stay correct; cnt[1] backs the weak count
         * that ref/from-rc's uniqueness check consults (see EX_REF_FROM_RC). */
        TuriValue v = eval_expr(env, frame, e->as.rc_of_.expr);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        int64_t *cnt = (int64_t *)malloc(2 * sizeof(int64_t));
        cnt[0] = 1; /* strong */
        cnt[1] = 0; /* weak */
        TuriValue fields[2];
        fields[0] = turi_int((int64_t)(intptr_t)cnt); /* pointer-as-int */
        fields[1] = v;
        return make_struct_val("__rc", 2, fields);
    }
    case EX_RC_CLONE: {
        if (e->as.rc_clone_.elide)
            return eval_expr(env, frame, e->as.rc_clone_.expr);
        TuriValue r = eval_expr(env, frame, e->as.rc_clone_.expr);
        if (turi_is_error(r) || env->returning || env->throwing) return r;
        if (r.tag == TURI_STRUCT && r.as_struct
            && r.as_struct->name && strcmp(r.as_struct->name, "__rc") == 0
            && r.as_struct->n_fields >= 2) {
            int64_t *cnt = (int64_t *)(intptr_t)r.as_struct->fields[0].as_int;
            (*cnt)++;
        }
        return r; /* return copy — same pointer-as-int, so shares counter */
    }
    case EX_RC_DROP: {
        if (e->as.rc_drop_.elide) return turi_nil();
        TuriValue r = eval_expr(env, frame, e->as.rc_drop_.expr);
        if (!turi_is_error(r) && r.tag == TURI_STRUCT && r.as_struct
            && r.as_struct->name && strcmp(r.as_struct->name, "__rc") == 0
            && r.as_struct->n_fields >= 2) {
            int64_t *cnt = (int64_t *)(intptr_t)r.as_struct->fields[0].as_int;
            if (*cnt > 0) {
                (*cnt)--;
                /* When count reaches 0, recursively drop any inner __rc values. */
                if (*cnt == 0) {
                    TuriValue inner = r.as_struct->fields[1];
                    /* Walk the chain of nested __rc structs. */
                    while (inner.tag == TURI_STRUCT && inner.as_struct
                           && inner.as_struct->name
                           && strcmp(inner.as_struct->name, "__rc") == 0
                           && inner.as_struct->n_fields >= 2) {
                        int64_t *icnt = (int64_t *)(intptr_t)inner.as_struct->fields[0].as_int;
                        if (*icnt > 0) (*icnt)--;
                        if (*icnt > 0) break; /* still alive — stop recursing */
                        inner = inner.as_struct->fields[1];
                    }
                }
            }
        }
        return turi_nil();
    }
    case EX_RC_PTR: {
        TuriValue r = eval_expr(env, frame, e->as.rc_ptr_.expr);
        if (turi_is_error(r) || env->returning || env->throwing) return r;
        if (r.tag == TURI_STRUCT && r.as_struct
            && r.as_struct->name && strcmp(r.as_struct->name, "__rc") == 0
            && r.as_struct->n_fields >= 2)
            return r.as_struct->fields[1];
        return r;
    }
    case EX_RC_COUNT: {
        TuriValue r = eval_expr(env, frame, e->as.rc_count_.expr);
        if (turi_is_error(r) || env->returning || env->throwing) return r;
        if (r.tag == TURI_STRUCT && r.as_struct
            && r.as_struct->name && strcmp(r.as_struct->name, "__rc") == 0
            && r.as_struct->n_fields >= 2) {
            int64_t *cnt = (int64_t *)(intptr_t)r.as_struct->fields[0].as_int;
            return turi_int(*cnt);
        }
        return turi_int(1);
    }
    case EX_RC_FROM_REF:
        return eval_expr(env, frame, e->as.rc_from_ref_.expr);
    case EX_REF_FROM_RC: {
        /* Convert rc<T> to ref<T>: the compiled tur_ref_from_rc requires the rc
         * to be *unique* -- strong_count==1 and weak_count==0 -- and aborts
         * otherwise (a live alias would leave a dangling ref). Mirror that check
         * here instead of silently extracting the value (rc-unique-violation). */
        TuriValue rv = eval_expr(env, frame, e->as.ref_from_rc_.expr);
        if (turi_is_error(rv) || env->returning || env->throwing) return rv;
        if (rv.tag == TURI_STRUCT && rv.as_struct
            && rv.as_struct->name && strcmp(rv.as_struct->name, "__rc") == 0
            && rv.as_struct->n_fields >= 2) {
            int64_t *cnt = (int64_t *)(intptr_t)rv.as_struct->fields[0].as_int;
            int64_t strong = cnt ? cnt[0] : 1;
            int64_t weak   = cnt ? cnt[1] : 0;
            if (strong != 1 || weak != 0) {
                char msg[160];
                snprintf(msg, sizeof msg,
                    "ref/from-rc requires unique rc (strong_count==1 and weak_count==0), got strong=%lld weak=%lld",
                    (long long)strong, (long long)weak);
                turi_runtime_panic(env, msg); /* does not return */
                return turi_nil();
            }
            return rv.as_struct->fields[1];
        }
        return rv;
    }

    /* --- Phase 5: ref<T> — transparent in interpreter --------------------- */
    case EX_REF:
        return eval_expr(env, frame, e->as.ref_.expr);
    case EX_REF_PRED: {
        TuriValue _rpv = eval_expr(env, frame, e->as.ref_pred_.expr);
        if (turi_is_error(_rpv) || env->returning || env->throwing) return _rpv;
        return turi_bool(_rpv.tag != TURI_NIL);
    }

    /* --- Phase 9: weak<T> — simplified in interpreter --------------------- */
    case EX_WEAK: {
        /* weak is transparent (returns the underlying rc value), but it must
         * bump the control block's weak count so ref/from-rc's uniqueness check
         * can see the live weak alias (rc-unique-violation). */
        TuriValue wv = eval_expr(env, frame, e->as.weak_.expr);
        if (turi_is_error(wv) || env->returning || env->throwing) return wv;
        if (wv.tag == TURI_STRUCT && wv.as_struct
            && wv.as_struct->name && strcmp(wv.as_struct->name, "__rc") == 0
            && wv.as_struct->n_fields >= 2) {
            int64_t *cnt = (int64_t *)(intptr_t)wv.as_struct->fields[0].as_int;
            if (cnt) cnt[1]++; /* weak count */
        }
        return wv;
    }
    case EX_WEAK_UPGRADE: {
        TuriValue _wuv = eval_expr(env, frame, e->as.weak_upgrade_.expr);
        if (turi_is_error(_wuv) || env->returning || env->throwing) return _wuv;
        /* In the interpreter all weak refs are always valid; wrap in some(value). */
        return make_struct_val("some", 1, &_wuv);
    }
    case EX_WEAK_PRED:
        return turi_bool(true);

    /* --- Phase HRT0: exists pack/open — erase existential box ------------- */
    case EX_EXISTS_PACK:
        return eval_expr(env, frame, e->as.exists_pack_.value);
    case EX_EXISTS_OPEN: {
        TuriValue packed = eval_expr(env, frame, e->as.exists_open_.packed);
        if (turi_is_error(packed) || env->returning || env->throwing) return packed;
        EvalFrame *ef = eval_frame_new(frame);
        if (e->as.exists_open_.var_binding)
            frame_bind(ef, e->as.exists_open_.var_binding->name->name, packed);
        TuriValue r = eval_expr(env, ef, e->as.exists_open_.body);
        eval_frame_free(ef);
        return r;
    }

    /* --- IT0: union inject — tag a value for union type -------------------- */
    case EX_UNION_INJECT:
        return eval_expr(env, frame, e->as.union_inject_.value);

    /* --- IT4: any-typed cast and type-of --------------------------------- */
    case EX_ANY_CAST: {
        /* (cast x T) is a *checked* downcast: the compiled path panics when the
         * any box does not hold a T (__tur_any_cast_check).  W4: do the same
         * here -- 'any' is untagged in the interpreter, but the runtime
         * TuriValue tag still records the held kind, so verify it matches the
         * target and panic on mismatch.  Conservative: only panic on a clear
         * primitive/struct/ADT mismatch; pass through ambiguous targets
         * (type vars, unknown) so a valid cast never spuriously panics. */
        TuriValue v = eval_expr(env, frame, e->as.any_cast_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        bool ok = true;
        switch (e->as.any_cast_.target_kind) {
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            ok = (v.tag == TURI_INT); break;
        case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:
            ok = (v.tag == TURI_FLOAT); break;
        case TY_BOOL: ok = (v.tag == TURI_BOOL); break;
        case TY_CSTR: ok = (v.tag == TURI_CSTR); break;
        case TY_STRUCT:
            ok = (v.tag == TURI_STRUCT && v.as_struct && v.as_struct->def != NULL);
            if (ok && e->as.any_cast_.target_struct &&
                v.as_struct->name && e->as.any_cast_.target_struct->name)
                ok = (strcmp(v.as_struct->name,
                             e->as.any_cast_.target_struct->name) == 0);
            break;
        case TY_ADT:
            ok = (v.tag == TURI_STRUCT && v.as_struct && v.as_struct->def == NULL);
            break;
        default: ok = true; break;
        }
        if (!ok) {
            turi_runtime_panic(env, "cast: any holds a value of a different type");
            return turi_nil(); /* unreachable: turi_runtime_panic never returns */
        }
        return v;
    }

    case EX_ANY_TYPE_OF: {
        TuriValue v = eval_expr(env, frame, e->as.any_type_of_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        const char *tname = "unknown";
        switch (v.tag) {
        case TURI_INT:     tname = "int";     break;
        case TURI_FLOAT:   tname = "float";   break;
        case TURI_BOOL:    tname = "bool";    break;
        case TURI_CSTR:    tname = "cstr";    break;
        case TURI_NIL:     tname = "nil";     break;
        case TURI_CLOSURE: tname = "fn";      break;
        case TURI_STRUCT:
            /* W4: match the compiled __tur_any_type_name, which carries only
             * kind granularity -- every struct is "struct" and every ADT is
             * "adt" (emit_module.c), NOT the specific type name.  A plain
             * (make-struct T ...) carries its StructDef (def != NULL); an ADT
             * constructor builds via make_struct_val with def == NULL. */
            tname = (v.as_struct && v.as_struct->def) ? "struct" : "adt";
            break;
        default: break;
        }
        return turi_cstr(tname);
    }

    /* --- TY3: (is? x T) — runtime type test ------------------------------ */
    case EX_ANY_IS: {
        TuriValue v = eval_expr(env, frame, e->as.any_is_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        /* Map the runtime TuriValue tag to a TypeKind and compare to test_tag. */
        TypeKind vk = TY_UNKNOWN;
        switch (v.tag) {
        case TURI_INT:    vk = TY_INT;      break;
        case TURI_FLOAT:  vk = TY_FLOAT;    break;
        case TURI_BOOL:   vk = TY_BOOL;     break;
        case TURI_CSTR:   vk = TY_CSTR;     break;
        case TURI_NIL:    vk = TY_NIL;      break;
        case TURI_STRUCT: vk = TY_STRUCT;   break;
        default: break;
        }
        return turi_bool((int64_t)vk == e->as.any_is_.test_tag);
    }

    /* --- Phase TI2: generators ------------------------------------------- */

    /* (gen [] body) -- construct a lazy generator coroutine. */
    case EX_GEN: {
        const GenDef *def = e->as.gen_.def;
        TuriGen *g = (TuriGen *)calloc(1, sizeof(TuriGen));
        if (!g) return turi_error("eval: out of memory (generator)");
        g->env     = env;
        g->body    = def->body;
        /* The body resolves its captures through a fresh child of the creating
         * frame; like a closure, this frame is intentionally not freed (the
         * generator may outlive the creating scope). */
        g->frame   = eval_frame_new(frame);
        g->started = false;
        g->done    = false;
        return turi_gen_val(g);
    }

    /* (yield v) -- suspend the active generator, handing v back to gen-next. */
    case EX_YIELD: {
        TuriGen *g = g_current_gen;
        if (!g)
            return turi_error("eval: yield outside of a generator body");
        TuriValue v = eval_expr(env, frame, e->as.yield_.value);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        g->box = v.as_int;
        /* Swap back to the caller (gen-next); resumes here on the next advance. */
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        swapcontext(&g->gen_ctx, &g->caller_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
        return turi_nil();
    }

    /* (gen-next g) -- advance a generator; returns ptr<void> (NULL = exhausted). */
    case EX_GEN_NEXT: {
        TuriValue gv = eval_expr(env, frame, e->as.gen_next_.gen_expr);
        if (turi_is_error(gv) || env->returning || env->throwing) return gv;
        if (gv.tag != TURI_GEN)
            return turi_errorf("eval: gen-next: expected a generator, got tag %d", gv.tag);
        return gen_advance(env, gv.as_gen);
    }

    /* (gen-done? g) -- has the generator been driven off its end? */
    case EX_GEN_DONE: {
        TuriValue gv = eval_expr(env, frame, e->as.gen_done_.gen_expr);
        if (turi_is_error(gv) || env->returning || env->throwing) return gv;
        if (gv.tag != TURI_GEN)
            return turi_errorf("eval: gen-done?: expected a generator, got tag %d", gv.tag);
        return turi_bool(gv.as_gen->done);
    }

    /* Phase TI3: delimited control. */
    case EX_RESET:
        return eval_reset_boundary(env, frame, e->as.reset_.body, PROMPT_PLAIN);

    case EX_SHIFT:
        return eval_abortive_shift(env, frame, e->as.shift_.k_fn,
                                   e->as.shift_.body, "shift");

    case EX_SHIFT0:
        return eval_abortive_shift(env, frame, e->as.shift0_.k_fn,
                                   e->as.shift0_.body, "shift0");

    /* serial-reset / cloneable-reset.  When the body performs the matching
     * capturing shift, reify the delimited context and hand the receiver a
     * resumable continuation (ts_capture_and_run).  Otherwise fall back to the
     * plain prompt boundary, which handles the no-shift passthrough and any
     * abortive shift inside. */
    case EX_SERIAL_RESET:
        if (ts_reaches_shift(e->as.serial_reset_.body, EX_SERIAL_SHIFT))
            return ts_capture_and_run(env, frame, e->as.serial_reset_.body,
                                      EX_SERIAL_SHIFT, /*serial=*/true);
        return eval_reset_boundary(env, frame, e->as.serial_reset_.body,
                                   PROMPT_SERIAL);

    case EX_CLONEABLE_RESET:
        if (ts_reaches_shift(e->as.cloneable_reset_.body, EX_CLONEABLE_SHIFT))
            return ts_capture_and_run(env, frame, e->as.cloneable_reset_.body,
                                      EX_CLONEABLE_SHIFT, /*serial=*/false);
        return eval_reset_boundary(env, frame, e->as.cloneable_reset_.body,
                                   PROMPT_CLONEABLE);

    /* A capturing shift reached here is standalone: the enclosing reset's
     * descent (ts_capture_and_run) consumes the shift in place and never calls
     * eval_expr on it, so hitting these arms means the shift is outside any
     * matching reset. */
    case EX_SERIAL_SHIFT:
        return turi_error("eval: serial-shift used outside of any serial-reset");
    case EX_CLONEABLE_SHIFT:
        return turi_error("eval: cloneable-shift used outside of any cloneable-reset");

    /* call/cc / escape: undelimited one-shot upward escape continuation. */
    case EX_CALLCC:
        return eval_callcc_escape(env, frame, e->as.callcc_.fn);

    /* Phase TI4: Software Transactional Memory. */
    case EX_STM: {
        /* Evaluate the body sequence; the value is the last expression. */
        TuriValue v = turi_nil();
        for (uint32_t i = 0; i < e->as.stm_.n_body; i++) {
            v = eval_expr(env, frame, e->as.stm_.body[i]);
            if (turi_is_error(v) || env->returning || env->throwing) return v;
            /* A retry/abort request short-circuits the rest of the block. */
            if (g_stm_tx && (g_stm_tx->retry_requested || g_stm_tx->aborted))
                return v;
        }
        return v;
    }

    case EX_ATOMICALLY:
        return eval_atomically(env, frame, e->as.atomically_.stm_expr);

    case EX_RETRY:
        if (!g_stm_tx)
            return turi_error("eval: retry used outside of an atomically block");
        g_stm_tx->retry_requested = true;
        return turi_nil();

    case EX_CHECK: {
        if (!g_stm_tx)
            return turi_error("eval: check used outside of an atomically block");
        TuriValue c = eval_expr(env, frame, e->as.check_.cond);
        if (turi_is_error(c) || env->returning || env->throwing) return c;
        /* Matches the compiled runtime: a failed check requests a retry. */
        if (!c.as_bool) g_stm_tx->retry_requested = true;
        return turi_nil();
    }

    case EX_OR_ELSE: {
        if (!g_stm_tx)
            return turi_error("eval: or-else used outside of an atomically block");
        bool retry_before = g_stm_tx->retry_requested;
        TuriValue v = eval_expr(env, frame, e->as.or_else_.stm1);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        /* If stm1 (and not a prior op) requested a retry, fall back to stm2. */
        if (!retry_before && g_stm_tx->retry_requested) {
            g_stm_tx->retry_requested = false;
            v = eval_expr(env, frame, e->as.or_else_.stm2);
        }
        return v;
    }

    case EX_TVAR_NEW: {
        TuriValue init = eval_expr(env, frame, e->as.tvar_new_.init);
        if (turi_is_error(init) || env->returning || env->throwing) return init;
        TuriTVar *tv = (TuriTVar *)calloc(1, sizeof(TuriTVar));
        if (!tv) return turi_error("eval: tvar/new: out of memory");
        tv->value   = init.as_int;
        tv->version = 1;
        return turi_int((int64_t)(intptr_t)tv);
    }

    case EX_TVAR_READ: {
        if (!g_stm_tx)
            return turi_error("eval: tvar/read used outside of an atomically block");
        TuriValue err = turi_nil();
        TuriTVar *tv = stm_eval_tvar(env, frame, e->as.tvar_read_.tvar, &err);
        if (!tv) return err;
        return turi_int(stm_read(g_stm_tx, tv));
    }

    case EX_TVAR_WRITE: {
        if (!g_stm_tx)
            return turi_error("eval: tvar/write used outside of an atomically block");
        TuriValue err = turi_nil();
        TuriTVar *tv = stm_eval_tvar(env, frame, e->as.tvar_write_.tvar, &err);
        if (!tv) return err;
        TuriValue val = eval_expr(env, frame, e->as.tvar_write_.value);
        if (turi_is_error(val) || env->returning || env->throwing) return val;
        stm_log_write(g_stm_tx, tv, val.as_int);
        return turi_nil();
    }

    case EX_TVAR_SWAP: {
        if (!g_stm_tx)
            return turi_error("eval: tvar/swap used outside of an atomically block");
        TuriValue err = turi_nil();
        TuriTVar *tv = stm_eval_tvar(env, frame, e->as.tvar_swap_.tvar, &err);
        if (!tv) return err;
        TuriValue nv = eval_expr(env, frame, e->as.tvar_swap_.new_val);
        if (turi_is_error(nv) || env->returning || env->throwing) return nv;
        int64_t old = stm_read(g_stm_tx, tv);
        stm_log_write(g_stm_tx, tv, nv.as_int);
        return turi_int(old);
    }

    case EX_TVAR_CAS: {
        if (!g_stm_tx)
            return turi_error("eval: tvar/cas used outside of an atomically block");
        TuriValue err = turi_nil();
        TuriTVar *tv = stm_eval_tvar(env, frame, e->as.tvar_cas_.tvar, &err);
        if (!tv) return err;
        TuriValue ov = eval_expr(env, frame, e->as.tvar_cas_.old_val);
        if (turi_is_error(ov) || env->returning || env->throwing) return ov;
        TuriValue nv = eval_expr(env, frame, e->as.tvar_cas_.new_val);
        if (turi_is_error(nv) || env->returning || env->throwing) return nv;
        if (stm_read(g_stm_tx, tv) == ov.as_int) {
            stm_log_write(g_stm_tx, tv, nv.as_int);
            return turi_bool(true);
        }
        return turi_bool(false);
    }

    case EX_TVAR_MODIFY: {
        /* Read-modify-write: r = fn(old); write r; return old.  NOTE: the
         * compiled path (emit_expr.c EX_TVAR_MODIFY) currently emits a no-op
         * stub returning NULL -- a latent bug filed in
         * docs/reported/stm-tvar-modify-codegen-stub.md.  The interpreter
         * implements the intended semantics. */
        if (!g_stm_tx)
            return turi_error("eval: tvar/modify used outside of an atomically block");
        TuriValue err = turi_nil();
        TuriTVar *tv = stm_eval_tvar(env, frame, e->as.tvar_modify_.tvar, &err);
        if (!tv) return err;
        TuriValue fn = eval_expr(env, frame, e->as.tvar_modify_.fn);
        if (turi_is_error(fn) || env->returning || env->throwing) return fn;
        int64_t old = stm_read(g_stm_tx, tv);
        TuriValue arg = turi_int(old);
        TuriValue r = turi_call(env, fn, &arg, 1);
        if (turi_is_error(r) || env->returning || env->throwing) return r;
        stm_log_write(g_stm_tx, tv, r.as_int);
        return turi_int(old);
    }

    /* --- Everything else — unimplemented expression kind ------------------ */
    default:
        return turi_errorf("eval: unhandled expression kind %d "
                           "(not yet implemented in interpreter)", (int)e->kind);
    }
}

/* -------------------------------------------------------------------------
 * turi_eval / turi_eval_typed: public entry points
 * ---------------------------------------------------------------------- */

/* Convert the elaborated Type of an expression to a human-readable tag. */
static void extract_type_tag(Type t, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    switch (t.kind) {
    case TY_NIL:     snprintf(buf, cap, "nil");      break;
    case TY_BOOL:    snprintf(buf, cap, "bool");     break;
    case TY_INT:
    case TY_INT64:   snprintf(buf, cap, "int");      break;
    case TY_INT8:    snprintf(buf, cap, "int8");     break;
    case TY_INT16:   snprintf(buf, cap, "int16");    break;
    case TY_INT32:   snprintf(buf, cap, "int32");    break;
    case TY_FLOAT:
    case TY_FLOAT64: snprintf(buf, cap, "float");    break;
    case TY_FLOAT32: snprintf(buf, cap, "float32");  break;
    case TY_CSTR:    snprintf(buf, cap, "cstr");     break;
    case TY_PTR_VOID: snprintf(buf, cap, "ptr<void>"); break;
    case TY_FN:      snprintf(buf, cap, "fn");       break;
    case TY_STRUCT:
        if (t.as.struct_.def && t.as.struct_.def->name)
            snprintf(buf, cap, "%s", t.as.struct_.def->name);
        else
            snprintf(buf, cap, "struct");
        break;
    default:         snprintf(buf, cap, "unknown");  break;
    }
}

static TuriValue turi_eval_impl(TuriEnv *env, const char *src,
                                 char *out_type_tag, size_t tag_cap);

TuriValue turi_eval(TuriEnv *env, const char *src) {
    return turi_eval_impl(env, src, NULL, 0);
}

TuriValue turi_eval_typed(TuriEnv *env, const char *src,
                           char *out_type_tag, size_t tag_cap) {
    return turi_eval_impl(env, src, out_type_tag, tag_cap);
}

static TuriValue turi_eval_impl(TuriEnv *env, const char *src,
                                 char *out_type_tag, size_t tag_cap) {
    if (!env || !src) return turi_error("turi_eval: null argument");
    if (out_type_tag && tag_cap > 0) out_type_tag[0] = '\0';

    /* Phase S2: Detect #lang directive at the top of the new source.
     * This lets the web REPL and turi_eval_file pick up the reader mode from an
     * inline #lang line without requiring the caller to pre-process it.
     * detect_lang advances the pointer only when a #lang line is actually found;
     * when the pointer is unchanged no directive was present. */
    const char *src_body  = src;
    size_t      body_len  = strlen(src);

    /* Strip a Racket-style shebang line (`#!/usr/bin/env tur`) at the top of
     * the new source.  The reader's own shebang skip only fires at byte 0 of
     * the buffer it parses, but under --interpret the user file is appended to
     * the accumulated <eval> blob (after macros.tur / contract.tur), so its
     * `#!` is no longer at byte 0 and would lex as "unexpected character '#'".
     * detect_lang skips a shebang internally only to find a following `#lang`;
     * a shebang-only file leaves out_rest == src.  Drop the line here so both
     * the shebang-only and shebang+`#lang` cases reach the reader cleanly.
     * Mirror the reader's recognition rule (`#!` + `/` / whitespace / EOL). */
    if (body_len >= 2 && src_body[0] == '#' && src_body[1] == '!' &&
        (body_len < 3 || src_body[2] == '/' || src_body[2] == ' ' ||
         src_body[2] == '\t' || src_body[2] == '\n' || src_body[2] == '\r')) {
        const char *nl = (const char *)memchr(src_body, '\n', body_len);
        if (nl) {
            size_t skip = (size_t)(nl - src_body) + 1;  /* past the newline */
            src_body += skip;
            body_len -= skip;
        } else {
            src_body += body_len;  /* shebang-only file, no newline */
            body_len  = 0;
        }
    }

    {
        const char *rest     = src_body;
        size_t      rest_len = body_len;
        ReaderType  detected = detect_lang(src_body, body_len, &rest, &rest_len);
        if (rest != src_body) {
            /* A #lang directive was found.  Reject an unknown / not-yet-
             * implemented reader the same way the compiled entry points do
             * (src/main.c detect_and_adjust_lang) instead of silently running
             * the program under the default reader -- otherwise `#lang foo`
             * would just execute as plain Turmeric under --interpret. */
            if (!reader_type_is_implemented(detected)) {
                return turi_errorf("error: #lang %s is not yet implemented",
                                   reader_type_name(detected));
            }
            /* Strip the directive from the source body. */
            if (detected != env->reader_type) {
                /* Reader type is changing: discard accumulated source so that
                 * prior input isn't re-parsed under an incompatible reader. */
                env->src_acc.len    = 0;
                env->prior_toplevel = 0;
                env->reader_type    = detected;
            }
            src_body = rest;
            body_len = rest_len;
        }
    }

    /* 1. Build combined source: all prior definitions + new source (sans #lang). */
    Buf combined;
    buf_init(&combined);
    if (env->src_acc.len > 0) {
        buf_write(&combined, env->src_acc.data, env->src_acc.len);
        buf_putc(&combined, '\n');
    }
    buf_write(&combined, src_body, body_len);

    /* 2. Create a new per-call arena and link it into env. */
    ArenaNode *node = (ArenaNode *)malloc(sizeof(ArenaNode));
    arena_init(&node->arena, 0);
    node->next      = env->eval_arenas;
    env->eval_arenas = node;
    Arena *eval_arena = &node->arena;

    /* 3. Copy the combined source into the arena so it survives this call. */
    size_t src_len  = combined.len;
    char  *src_copy = arena_strdup(eval_arena, combined.data, src_len);
    buf_free(&combined);

    /* 4. Reset diagnostics; register the eval source file. */
    diag_reset();

    SourceFile *sfile = (SourceFile *)arena_alloc(eval_arena, sizeof(SourceFile));
    /* arena_alloc does not zero; clear so xform_map/orig_src start NULL.
     * Otherwise diagnostic snippet rendering dereferences uninitialized
     * memory (the sweet-exp xform map) and crashes. */
    memset(sfile, 0, sizeof(*sfile));
    sfile->path        = "<eval>";
    /* Resolve in-source relative paths (#use-reader-macros) against the script's
     * directory: the eval blob's path is the synthetic "<eval>" (no dirname), so
     * without this a directive like `#use-reader-macros "macros.tur"` would look
     * in cwd instead of beside the script. */
    sfile->base_dir    = env->module_base_dir;
    sfile->src         = src_copy;
    sfile->len         = src_len;
    sfile->file_id     = 0;
    sfile->reader_type = env->reader_type;
    diag_register_file(sfile);

    /* 5. Parse. RM Q#5: pass env->reader_macros so reader-macros defined
     * in earlier eval calls remain visible. */
    uint32_t  nforms = 0;
    Form    **forms  = read_all_with_registry(eval_arena, &env->st, sfile,
                                              env->reader_macros, &nforms);
    if (!forms || diag_had_error()) {
        return turi_error("parse error");
    }

    /* 5b. REPL implicit-do: if the new turn's forms contain a top-level
     * (define ...), wrap them in a single (do ...) form so the splice
     * helper handles them.  This makes `define` usable at the REPL prompt
     * without erroring.  Only fires when the new turn has at least one
     * define form; top-level defn/def forms are unaffected. */
    uint32_t prior = env->prior_toplevel;
    {
        bool new_has_define = false;
        for (uint32_t i = prior; i < nforms; i++) {
            Form *f = forms[i];
            if (f->tag == F_LIST && f->as.list.len >= 1) {
                Form *h = f->as.list.items[0];
                if (h->tag == F_SYM &&
                    strcmp(h->as.sym->name, "define") == 0) {
                    new_has_define = true;
                    break;
                }
            }
        }
        if (new_has_define) {
            uint32_t nn   = nforms - prior;
            Arena   *a    = eval_arena;
            Span     sp   = (nn > 0) ? forms[prior]->span : (Span){0};
            Form   **wrap = (Form **)arena_alloc(a, (nn + 1) * sizeof(Form *));
            /* Build sym "do" in the existing symbol table. */
            /* Intern "do" through the symbol table already in the arena. */
            StrSlice sl_do = { "do", 2 };
            const Symbol *sym_do_s = symtab_intern(&env->st, sl_do);
            wrap[0] = form_sym(a, sp, sym_do_s);
            for (uint32_t i = 0; i < nn; i++) wrap[i + 1] = forms[prior + i];
            Form *do_form = form_list(a, sp, wrap, nn + 1);
            /* Replace new-turn forms with the single do wrapper. */
            forms[prior] = do_form;
            nforms = prior + 1;
            /* Rebuild the combined source string to match the updated nforms
             * so src_acc stays consistent on the next turn. */
            Buf wrapped_src;
            buf_init(&wrapped_src);
            buf_write(&wrapped_src, "(do ", 4);
            buf_write(&wrapped_src, src_body, body_len);
            buf_putc(&wrapped_src, ')');
            src_body = arena_strdup(eval_arena, wrapped_src.data, wrapped_src.len);
            body_len = wrapped_src.len;
            buf_free(&wrapped_src);
        }
    }

    /* 6. Elaborate (read-only path: no borrow-check, no CPS, no emit).
     * Pass prior_toplevel as stdlib_prefix so the elaborator resets
     * has_defmodule after each defmodule in the already-accumulated forms.
     * This lets multiple stdlib files (each with their own defmodule) be
     * preloaded via successive turi_eval_file calls without hitting the
     * "only one defmodule per file" error on re-elaboration. */
    const char *mbase = env->module_base_dir ? env->module_base_dir : ".";
    uint32_t actual_n_fsd = 0;
    bool import_blocked = !turi_env_has_cap(env, TURI_CAP_IMPORT);
    /* Allocate a TypeClassEnv slot in the per-call arena so it outlives this
     * stack frame.  elaborate_program fills it; we save the pointer to
     * env->last_tc_env for later use by turi_try_show. */
    TypeClassEnv *tc_env_slot =
        (TypeClassEnv *)arena_alloc(eval_arena, sizeof(TypeClassEnv));
    memset(tc_env_slot, 0, sizeof(*tc_env_slot));
    Expr *prog = elaborate_program(eval_arena, &env->st,
                                   forms, nforms,
                                   /*stdlib_prefix=*/prior,
                                   /*module_base_dir=*/mbase,
                                   /*separate_compilation=*/false,
                                   /*sandboxed=*/import_blocked,
                                   /*out_tc_env=*/tc_env_slot,
                                   /*include_dirs=*/NULL,
                                   /*n_include_dirs=*/0,
                                   /*out_n_file_scope_defs=*/&actual_n_fsd,
                                   /* RM transitive: REPL/eval reuses the
                                    * env-owned registry so module loads
                                    * see the same macros the entry did. */
                                   env->reader_macros);
    if (!prog || diag_had_error()) {
        return turi_error("elaboration error");
    }

    /* 6b. Run effect-row check pass to emit warnings (e.g. TUR-W0033). */
    {
        EffectEnv eff_env;
        memset(&eff_env, 0, sizeof(eff_env));
        effect_check_pass(eval_arena, prog, &eff_env);
        /* Warnings are emitted as a side-effect; ignore hard errors in
         * interpreter mode (they would already be caught as elaboration errors). */
    }

    /* 6c. Run the always-on lifetime pass (elision + outlives-cycle check).
     * The compiled pipeline runs this in PASS_BORROW_CHECK (src/main.c); the
     * interpreter shares the elaborator but not the later passes, so a cyclic
     * explicit signature (&'a &'b / &'b &'a) was previously accepted under
     * --interpret.  lifetime_check_program only emits TUR-E0106 on a genuine
     * outlives cycle -- a shape no positive program contains -- so running it
     * here closes the parity gap without affecting any well-formed program.
     * The full move/borrow checker is intentionally left to the elaborator
     * (which the interpreter already shares). */
    if (!lifetime_check_program(prog)) {
        return turi_error("elaboration error");
    }

    /* 7. Evaluate the new top-level expressions.
     *
     * elaborate_program prepends actual file-scope defs (EX_DEFMODULE nodes
     * from imported modules) before the parsed forms.  It also expands any
     * (load ...) directives inline, which increases prog->as.program.n
     * beyond nforms without contributing to n_fsd.  We use the count returned
     * via out_n_file_scope_defs to correctly separate the two:
     *
     *   [0 .. n_fsd-1]               <- file_scope_defs (imported modules)
     *   [n_fsd .. n_fsd+prior-1]     <- previously-accumulated forms (already run)
     *   [n_fsd+prior .. total-1]     <- new user forms (must run; includes load-expanded)
     *
     * We must also run the file_scope_defs on every call because they are
     * freshly elaborated and not yet registered in the runtime env.  Since
     * EX_FN_DEF is idempotent (just overwrites the env binding), re-running
     * them on subsequent calls is harmless.
     *
     * prior_toplevel tracks *parsed* form count (not total), so the formula
     * stays consistent across calls even as n_fsd fluctuates. */
    uint32_t total = prog->as.program.n;
    uint32_t n_fsd = actual_n_fsd;

#define EVAL_TOPLEVEL_RANGE(lo, hi) do {                                      \
    for (uint32_t _i = (lo); _i < (hi); _i++) {                              \
        last = eval_expr(env, NULL, prog->as.program.items[_i]);             \
        if (env->returning) {                                                 \
            last = env->return_value;                                         \
            env->returning = false;                                           \
        }                                                                     \
        if (env->throwing) {                                                  \
            env->throwing = false;                                            \
            TuriValue _tv = env->throw_value;                                \
            if (_tv.tag == TURI_THROW && _tv.as_throw) {                     \
                TuriValue _inner = _tv.as_throw->value;                      \
                if (_inner.tag == TURI_CSTR && _inner.as_cstr)               \
                    last = turi_errorf("uncaught exception: %s",             \
                                       _inner.as_cstr);                      \
                else last = turi_error("uncaught exception");                 \
            } else last = turi_error("uncaught exception");                   \
            goto eval_done;                                                   \
        }                                                                     \
        if (turi_is_error(last)) goto eval_done;                             \
    }                                                                         \
} while (0)

    TuriValue last = turi_nil();
    EVAL_TOPLEVEL_RANGE(0, n_fsd);              /* imported module bodies */
    EVAL_TOPLEVEL_RANGE(n_fsd + prior, total);  /* new user forms         */
eval_done:;
#undef EVAL_TOPLEVEL_RANGE

    /* 8. Update accumulated state only on success.
     * Store nforms (parsed count) rather than total so the n_fsd formula
     * remains correct on subsequent calls that re-elaborate the same imports. */
    if (!turi_is_error(last)) {
        /* Append new source (without any leading #lang line) to accumulator */
        if (env->src_acc.len > 0) buf_putc(&env->src_acc, '\n');
        buf_write(&env->src_acc, src_body, body_len);
        env->prior_toplevel = nforms;  /* track parsed count, not total */
        /* SI4: persist TypeClassEnv for turi_try_show dispatch. */
        env->last_tc_env = tc_env_slot;
        /* SI4: extract type tag from the last new top-level expression. */
        if (out_type_tag && tag_cap > 0 && total > n_fsd + prior) {
            Expr *last_expr = prog->as.program.items[total - 1];
            if (last_expr) extract_type_tag(last_expr->type, out_type_tag, tag_cap);
        }
    }

    return last;
}

/* -------------------------------------------------------------------------
 * turi_eval_file: read a file and evaluate it
 * ---------------------------------------------------------------------- */

TuriValue turi_eval_file(TuriEnv *env, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return turi_errorf("cannot open '%s': %s", path, strerror(errno));
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return turi_error("fseek failed"); }
    long size = ftell(f);
    if (size < 0) { fclose(f); return turi_error("ftell failed"); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return turi_error("fseek failed"); }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return turi_error("out of memory"); }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return turi_error("read error"); }
    buf[size] = '\0';

    /* Phase S2: Apply reader type from file extension before evaluating.
     * An inline #lang directive inside the file takes precedence (turi_eval
     * will detect and apply it, overriding the extension-derived type). */
    ReaderType ext_type = reader_type_from_extension(path);
    if (ext_type != READER_TURMERIC && ext_type != env->reader_type) {
        env->reader_type    = ext_type;
        env->src_acc.len    = 0;
        env->prior_toplevel = 0;
    }

    TuriValue v = turi_eval(env, buf);
    free(buf);
    return v;
}

/* -------------------------------------------------------------------------
 * turi_call: directly invoke a closure value
 * ---------------------------------------------------------------------- */

TuriValue turi_call(TuriEnv *env, TuriValue fn, TuriValue *args, uint32_t n_args) {
    if (!env) return turi_error("turi_call: null env");
    if (fn.tag != TURI_CLOSURE || !fn.as_closure)
        return turi_errorf("turi_call: expected closure, got tag %d", fn.tag);
    return eval_apply(env, fn.as_closure, args, n_args);
}

/* SEQ (stdlib/seq): public bridges so the seq inline-C natives in main.c can
 * drive an interpreter generator (TuriGen is defined in this file).  `gen` is a
 * generator value (the int64 carrier holds the TuriGen*); advance it one step,
 * setting *done to 1 if it just ran off its end (no value yielded). */
TuriValue turi_gen_advance_val(TuriEnv *env, TuriValue gen, int *done) {
    TuriGen *g = (TuriGen *)(intptr_t)gen.as_int;
    if (!g) { if (done) *done = 1; return turi_int(0); }
    TuriValue v = gen_advance(env, g);
    if (done) *done = g->done ? 1 : 0;
    return v;
}
bool turi_gen_done_val(TuriValue gen) {
    TuriGen *g = (TuriGen *)(intptr_t)gen.as_int;
    return !g || g->done;
}

/* Fire all remaining deferred actions (those registered at module/top level).
 * Call this after turi_call(main) to honour module-level (defer ...) forms. */
void turi_run_pending_defers(TuriEnv *env) {
    if (env) fire_defers_to_mark(env, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * turi_init / turi_value_repr
 * ---------------------------------------------------------------------- */

void turi_init(bool use_color) {
    diag_init(use_color);
}

/* OQ2: depth-limited repr — truncate deeply nested structs to avoid runaway output. */
static void turi_value_repr_d(char *buf, size_t cap, TuriValue v, int depth) {
    if (!buf || cap == 0) return;
    switch (v.tag) {
    case TURI_NIL:
        snprintf(buf, cap, "nil");
        break;
    case TURI_BOOL:
        snprintf(buf, cap, "%s", v.as_bool ? "true" : "false");
        break;
    case TURI_INT:
        snprintf(buf, cap, "%lld", (long long)v.as_int);
        break;
    case TURI_FLOAT:
        snprintf(buf, cap, "%g", v.as_float);
        break;
    case TURI_CSTR:
        snprintf(buf, cap, "\"%s\"", v.as_cstr ? v.as_cstr : "");
        break;
    case TURI_CLOSURE: {
        FnDef *fn = v.as_closure ? (FnDef *)v.as_closure->fn : NULL;
        if (fn && fn->binding) {
            snprintf(buf, cap, "#<fn %s>", fn->binding->name->name);
        } else {
            snprintf(buf, cap, "#<fn>");
        }
        break;
    }
    case TURI_ERROR:
        snprintf(buf, cap, "#<error: %s>", v.as_error ? v.as_error : "");
        break;
    case TURI_EFFECT_CONT:
        snprintf(buf, cap, "#<continuation>");
        break;
    case TURI_STRUCT: {
        TuriStruct *s = v.as_struct;
        if (!s) { snprintf(buf, cap, "#<struct>"); break; }
        const char *n = s->name ? s->name : "?";
        if (!s->def || s->n_fields == 0) {
            snprintf(buf, cap, "#<struct %s>", n);
            break;
        }
        /* OQ2: hard depth limit -- truncate rather than recurse forever */
        if (depth <= 0) {
            snprintf(buf, cap, "#<struct %s>", n);
            break;
        }
        /* Print as "TypeName { field1 = val1, field2 = val2 }" */
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, cap - pos, "%s {", n);
        for (uint32_t i = 0; i < s->n_fields && pos < cap - 1; i++) {
            char fval[128];
            turi_value_repr_d(fval, sizeof(fval), s->fields[i], depth - 1);
            const char *fname = (i < s->def->n_fields) ? s->def->fields[i].name : "?";
            const char *sep = (i == 0) ? " " : ", ";
            pos += (size_t)snprintf(buf + pos, cap - pos, "%s%s = %s", sep, fname, fval);
        }
        if (pos < cap - 1) snprintf(buf + pos, cap - pos, " }");
        break;
    }
    case TURI_THROW:
        snprintf(buf, cap, "#<exception>");
        break;
    case TURI_FUTURE: {
        const char *state = "pending";
        if (v.as_future) {
            if (v.as_future->state == TURI_FUTURE_RESOLVED)  state = "resolved";
            if (v.as_future->state == TURI_FUTURE_REJECTED)  state = "rejected";
        }
        snprintf(buf, cap, "#<future:%s>", state);
        break;
    }
    case TURI_REF:
        snprintf(buf, cap, "#<ref>");
        break;
    case TURI_STRUCT_TYPE:
        snprintf(buf, cap, "#<struct-type %s>", v.as_cstr ? v.as_cstr : "?");
        break;
    case TURI_GEN:
        snprintf(buf, cap, "#<generator>");
        break;
    case TURI_HANDLER:
        snprintf(buf, cap, "#<handler>");
        break;
    }
}

void turi_value_repr(char *buf, size_t cap, TuriValue v) {
    turi_value_repr_d(buf, cap, v, 4);
}

/* -------------------------------------------------------------------------
 * SI4: turi_try_show — call Show typeclass instance for TURI_STRUCT values
 * ---------------------------------------------------------------------- */

const char *turi_try_show(TuriEnv *env, TuriValue val) {
    if (!env || !env->last_tc_env) return NULL;
    if (val.tag != TURI_STRUCT || !val.as_struct || !val.as_struct->def)
        return NULL;
    StructDef *sdef = val.as_struct->def;
    TypeClassEnv *tc_env = (TypeClassEnv *)env->last_tc_env;

    /* Find the Show typeclass in the registry. */
    TypeClass *show_tc = NULL;
    for (TypeClass *tc = tc_env->typeclasses; tc; tc = tc->next) {
        if (tc->name && strcmp(tc->name->name, "Show") == 0) {
            show_tc = tc;
            break;
        }
    }
    if (!show_tc) return NULL;

    /* Find the "show" method index in the typeclass. */
    uint8_t show_mi = 0;
    bool found_method = false;
    for (uint8_t mi = 0; mi < show_tc->n_methods; mi++) {
        if (show_tc->methods[mi].name &&
            strcmp(show_tc->methods[mi].name->name, "show") == 0) {
            show_mi = mi;
            found_method = true;
            break;
        }
    }
    if (!found_method) return NULL;

    /* Find the Show [StructType] instance. */
    FnDef *show_impl = NULL;
    for (TypeClassInstance *inst = tc_env->instances; inst; inst = inst->next) {
        if (inst->typeclass != show_tc) continue;
        if (inst->n_type_args < 1) continue;
        Type t = inst->type_args[0];
        if (t.kind != TY_STRUCT || !t.as.struct_.def) continue;
        if (t.as.struct_.def != sdef) continue;
        if (show_mi < inst->n_method_impls && inst->method_impls[show_mi])
            show_impl = inst->method_impls[show_mi];
        break;
    }
    if (!show_impl) return NULL;

    /* Create a temporary closure wrapping the Show method and call it. */
    TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
    if (!cl) return NULL;
    memset(cl, 0, sizeof(*cl));
    cl->fn = show_impl;
    TuriValue fn_val = turi_closure(cl);

    TuriValue result = turi_call(env, fn_val, &val, 1);
    free(cl);

    if (result.tag != TURI_CSTR || !result.as_cstr) return NULL;
    return strdup(result.as_cstr);
}

/* -------------------------------------------------------------------------
 * SI4-C: turi_show_result — show heap-pointer stdlib types by type tag
 * ---------------------------------------------------------------------- */

/* Pair pointer: struct { int64_t first; int64_t second; } */
static char *show_pair_ptr(int64_t ptr_val) {
    typedef struct { int64_t first; int64_t second; } PairCell;
    if (!ptr_val) return strdup("nil");
    PairCell *p = (PairCell *)(intptr_t)ptr_val;
    char *buf = (char *)malloc(64);
    if (!buf) return NULL;
    snprintf(buf, 64, "(%lld, %lld)", (long long)p->first, (long long)p->second);
    return buf;
}

/* Cons pointer: struct { int64_t value; int64_t next; } -- formats as "[v1, v2, ...]" */
static char *show_cons_ptr(int64_t ptr_val) {
    typedef struct { int64_t value; int64_t next; } ConsCell;
    if (!ptr_val) return strdup("[]");
    ConsCell *cell = (ConsCell *)(intptr_t)ptr_val;

    /* First pass: compute buffer size */
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)cell->value);
    size_t total = 1 + (n > 0 ? (size_t)n : 0) + 1; /* "[" + v1 + "]" */
    ConsCell *cur = (ConsCell *)(intptr_t)cell->next;
    while (cur) {
        n = snprintf(tmp, sizeof(tmp), "%lld", (long long)cur->value);
        total += 2 + (n > 0 ? (size_t)n : 0); /* ", " + v */
        cur = (ConsCell *)(intptr_t)cur->next;
    }

    /* Second pass: build the string */
    char *buf = (char *)malloc(total + 1);
    if (!buf) return NULL;
    int off = snprintf(buf, total + 1, "[%lld", (long long)cell->value);
    cur = (ConsCell *)(intptr_t)cell->next;
    while (cur) {
        off += snprintf(buf + off, (int)(total + 1) - off, ", %lld",
                        (long long)cur->value);
        cur = (ConsCell *)(intptr_t)cur->next;
    }
    snprintf(buf + off, (int)(total + 1) - off, "]");
    return buf;
}

const char *turi_show_result(TuriEnv *env, TuriValue val, const char *type_tag) {
    (void)env;
    if (!type_tag || val.tag != TURI_INT) return NULL;
    /* "Pair" kept for backwards compat; "PairPtr" is the defopaque name */
    if (strcmp(type_tag, "Pair") == 0 || strcmp(type_tag, "PairPtr") == 0)
        return show_pair_ptr(val.as_int);
    /* "Cons" kept for backwards compat; "ConsPtr" is the defopaque name */
    if (strcmp(type_tag, "Cons") == 0 || strcmp(type_tag, "ConsPtr") == 0)
        return show_cons_ptr(val.as_int);
    return NULL;
}

/* -------------------------------------------------------------------------
 * SB3 / SB4: Resource-limit and capability API
 * ---------------------------------------------------------------------- */

void turi_env_set_fuel(TuriEnv *env, uint64_t steps) {
    if (!env) return;
    env->step_fuel_limit = steps;
    env->step_fuel       = steps;
}

void turi_env_set_max_depth(TuriEnv *env, uint32_t depth) {
    if (!env) return;
    env->max_eval_depth = depth;
}

void turi_env_allow(TuriEnv *env, TuriCaps cap) {
    if (!env) return;
    env->caps |= cap;
}

void turi_env_deny(TuriEnv *env, TuriCaps cap) {
    if (!env) return;
    env->caps &= ~cap;
}

bool turi_env_has_cap(TuriEnv *env, TuriCaps cap) {
    if (!env) return false;
    return (env->caps & cap) != 0;
}
