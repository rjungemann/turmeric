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

/* -------------------------------------------------------------------------
 * Tail-call trampoline types (eval.c-internal only; never exposed in headers)
 * ---------------------------------------------------------------------- */

/* Internal-only tag for tail-call bounce values.  Never stored in TuriEnv
 * or returned from turi_eval().  eval_apply consumes all bounces. */
#define TURI_TAG_TCO ((TuriTag)0x7F)

typedef struct TcoFrame {
    TuriClosure *cl;
    TuriValue    args[64]; /* MAX_EVAL_ARGS */
    uint32_t     n_args;
} TcoFrame;

static inline TuriValue tco_bounce(TuriClosure *cl, TuriValue *args, uint32_t n) {
    TcoFrame *tc = (TcoFrame *)malloc(sizeof(TcoFrame));
    tc->cl = cl;
    tc->n_args = n;
    memcpy(tc->args, args, n * sizeof(TuriValue));
    TuriValue v;
    v.tag = TURI_TAG_TCO;
    v.as_ref = tc;
    return v;
}

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
};

/* Forward declaration — defined after TuriStruct is fully defined below. */
static TuriValue native_panic_pred(TuriEnv *env, TuriValue *args, uint32_t n, void *ud);

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

/* Register eval-layer native builtins (struct-aware predicates, etc.).
 * Called from turi_env_new after async builtins are registered. */
void turi_eval_register_builtins(TuriEnv *env) {
    turi_env_register_native(env, "panic?", native_panic_pred, NULL);
}

/* -------------------------------------------------------------------------
 * Phase S7: Async fiber thunk
 * ---------------------------------------------------------------------- */

_Thread_local TuriFiber *g_pending_async_fiber;

/* Forward declarations needed by the thunk. */
static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e);
static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args);
static TuriValue eval_body_tco(TuriEnv *env, EvalFrame *frame, const Expr *e);

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

static void register_extern_c_known(TuriEnv *env, const char *fname) {
    struct { const char *name; TuriNativeFn fn; } known[] = {
        { "exit",     native_extern_exit     },
        { "free",     native_extern_free     },
        { "strlen",   native_extern_strlen   },
        { "getenv",   native_extern_getenv   },
        { "printf",   native_extern_printf   },
        { "printf_s", native_extern_printf   },
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
    return turi_env_get(env, name);
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

/* Full definition of TuriEffectCont (forward-declared in value.h). */
struct TuriEffectCont {
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

    default:
        /* Silently return nil for unsupported builtins (unsafe ops, STM, etc.) */
        return turi_nil();
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

/* Parse a simple field assignment expression: param name, bool/null/int literal, or cast.
 * ic_body is the full inline-C body (for struct field extraction); may be NULL. */
static bool ic_eval_assign_expr(const char *expr,
                                 FnDef *fn, uint32_t param_offset,
                                 TuriValue *args, uint32_t n_args,
                                 int64_t *out_val,
                                 const char *ic_body) {
    const char *p = ic_skip_ws(expr);
    /* Strip any number of casts like (T), (T*), (int64_t)(intptr_t), etc. */
    while (*p == '(') {
        const char *q = p + 1; q = ic_skip_ws(q);
        if (isalpha((unsigned char)*q) || *q == '_') {
            /* consume type tokens and * until ) */
            const char *q2 = q;
            while (*q2 && *q2 != ')') q2++;
            if (*q2 == ')') { p = ic_skip_ws(q2 + 1); continue; }
        }
        break;
    }
    if (ic_word_eq(p, "true"))  { *out_val = 1; return true; }
    if (ic_word_eq(p, "false")) { *out_val = 0; return true; }
    if (ic_word_eq(p, "NULL"))  { *out_val = 0; return true; }
    if (*p == '0' && !isdigit((unsigned char)p[1])) { *out_val = 0; return true; }
    if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
        char *end; *out_val = strtoll(p, &end, 0);
        if (end > p) return true;
    }
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
                            return true;
                        }
                    } else if (arg->tag == TURI_INT && arg->as_int != 0) {
                        /* Pointer to struct, access via common field index */
                        int fidx = ic_common_field_idx(field_name, flen);
                        if (fidx >= 0) {
                            int64_t *ptr = (int64_t*)(intptr_t)arg->as_int;
                            *out_val = ptr[fidx];
                            return true;
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
                            return true;
                        }
                    }
                }
            }
            *out_val = arg->as_int;
            return true;
        }
    }
    return false;
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

    /* Step 3: Find snprintf(bufvar, size, "format", args...) */
    const char *sp = body;
    while (*sp) {
        const char *fp = strstr(sp, "snprintf(");
        if (!fp) break;
        const char *fq = fp + 9;
        fq = ic_skip_ws(fq);
        const char *bvs = fq;
        while (isalnum((unsigned char)*fq)||*fq=='_') fq++;
        if ((int)(fq-bvs)==bvlen && strncmp(bvs,bufvar,bvlen)==0) {
            /* skip comma + size arg */
            fq=ic_skip_ws(fq); if(*fq==',') fq++;
            fq=ic_skip_ws(fq); int d=0;
            while(*fq&&(*fq!=','||d>0)){if(*fq=='(')d++;else if(*fq==')')d--;fq++;}
            if(*fq==',') fq++;
            fq=ic_skip_ws(fq);
            if(*fq!='"') { sp=fp+1; continue; }
            /* extract format string */
            const char *fmts=fq+1, *fmte=fmts;
            while(*fmte&&*fmte!='"'){if(*fmte=='\\')fmte++;fmte++;}
            char fmt_str[512]; size_t fmt_len=(size_t)(fmte-fmts);
            if(fmt_len>=sizeof(fmt_str)){sp=fp+1;continue;}
            memcpy(fmt_str,fmts,fmt_len); fmt_str[fmt_len]='\0';
            ic_unescape_str(fmt_str);
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
static bool try_exec_simple_inline_c(TuriEnv *env,
                                      const char *body, size_t blen,
                                      TuriValue *args, uint32_t n_args,
                                      FnDef *fn, uint32_t param_offset,
                                      TuriValue *out) {
    (void)env; (void)blen;
    if (!body || !*body) return false;

    bool has_malloc = strstr(body,"malloc(") || strstr(body,"calloc(");
    bool has_free   = strstr(body,"free(");
    bool has_arrow  = strstr(body,"->");
    bool has_fptr   = strstr(body,"(*)(");
    bool has_switch = strstr(body,"switch") && strstr(body,"case ");
    bool has_return = strstr(body,"return ");

    /* Pattern 1: Free */
    if (has_free && !has_malloc) {
        *out = ic_exec_free(args, n_args);
        return true;
    }

    /* Pattern 2: Switch-case string */
    if (has_switch && strstr(body,"return \"") && !has_fptr) {
        *out = ic_exec_switch_string(body, args, n_args);
        return true;
    }

    /* Pattern 3: String fat-pointer comparison (->len && ->p[i]) */
    if (!has_malloc && has_arrow && n_args>=2 &&
        strstr(body,"->len") && strstr(body,"->p") &&
        (strstr(body,"return true")||strstr(body,"return false"))) {
        *out = ic_exec_str_cmp(args, n_args);
        return true;
    }

    /* Pattern 4a: snprintf formatter (malloc + snprintf + return cstr, no arrow in return) */
    if (has_malloc && !has_fptr && strstr(body,"snprintf(")) {
        TuriValue r = ic_exec_snprintf_fmt(body, args, n_args, fn, param_offset);
        if (r.tag != TURI_NIL) { *out = r; return true; }
    }

    /* Pattern 4: Constructor (malloc + arrow or index assignments, no fptr cast) */
    if (has_malloc && !has_fptr) {
        TuriValue r = ic_exec_constructor(body, args, n_args, fn, param_offset);
        if (r.tag != TURI_NIL) { *out = r; return true; }
    }

    /* Pattern 5: Accessor (no malloc, has arrow, has return) */
    if (!has_malloc && has_return && !has_fptr) {
        TuriValue r = ic_exec_accessor(body, args, n_args, fn);
        if (r.tag != TURI_NIL) { *out = r; return true; }
    }

    /* Pattern 6: Linked list traversal with printf (while loop + printf + ->next) */
    bool has_printf = strstr(body,"printf(") != NULL;
    bool has_while  = strstr(body,"while") != NULL;
    if (!has_malloc && has_arrow && has_while && has_printf && !has_fptr) {
        TuriValue r = ic_exec_linked_list_print(body, args, n_args, fn, param_offset);
        /* Always claim handled if we have a while+printf pattern (even if nil) */
        if (has_while && has_printf) { *out = r; return true; }
    }

    /* Pattern 7: Simple return of constant or single param (no malloc, no arrow) */
    if (!has_malloc && !has_arrow && has_return && !has_fptr && !has_switch) {
        const char *r = strstr(body, "return "); if (r) {
            r += 7; r = ic_skip_ws(r);
            int64_t val = 0;
            if (ic_eval_assign_expr(r, fn, param_offset, args, n_args, &val, body)) {
                TuriValue rv={0}; rv.tag=TURI_INT; rv.as_int=val; *out=rv; return true;
            }
        }
    }

    return false;
}

/* -------------------------------------------------------------------------
 * eval_body_tco: tail-call-optimised body evaluator.
 *
 * Called from eval_apply for the function body in tail position.  Traverses
 * control-flow expressions (if / do / let / match) without growing the C
 * stack, and returns a TURI_TAG_TCO bounce for tail calls instead of
 * recursing into eval_apply.  eval_apply's trampoline loop consumes bounces.
 *
 * Rules:
 *  - EX_IF:    evaluate condition normally; loop on the taken branch.
 *  - EX_DO:    evaluate all items; the absolute last item is the tail.
 *              If the last item is EX_DEFER, evaluate it normally and return.
 *  - EX_LET:   bind variables normally; loop on the body.
 *  - EX_MATCH: find the matching arm; loop on the arm body.
 *  - EX_CALL:  evaluate fn + args normally; return a TCO bounce.
 *  - everything else: fall through to eval_expr (non-tail).
 * ---------------------------------------------------------------------- */
static TuriValue eval_body_tco(TuriEnv *env, EvalFrame *frame, const Expr *e) {
restart:
    if (!e)              return turi_nil();
    if (env->returning)  return env->return_value;
    if (env->throwing)   return env->throw_value;

    switch (e->kind) {

    /* --- if: tail on both branches --------------------------------------- */
    case EX_IF: {
        TuriValue cond = eval_expr(env, frame, e->as.if_.cond);
        if (turi_is_error(cond) || env->returning || env->throwing) return cond;
        if (turi_is_truthy(cond)) {
            e = e->as.if_.then_; goto restart;
        } else if (e->as.if_.else_or_null) {
            e = e->as.if_.else_or_null; goto restart;
        }
        return turi_nil();
    }

    /* --- do / program: tail on last non-defer item ----------------------- */
    case EX_DO:
    case EX_PROGRAM: {
        Expr   **items = (e->kind == EX_PROGRAM) ? e->as.program.items
                                                  : e->as.do_.items;
        uint32_t n     = (e->kind == EX_PROGRAM) ? e->as.program.n
                                                  : e->as.do_.n;
        if (n == 0) return turi_nil();
        /* Evaluate all items except the last non-defer one. */
        uint32_t tail_idx = n - 1;
        /* Scan back to find the last non-DEFER item. */
        while (tail_idx > 0 && items[tail_idx]->kind == EX_DEFER)
            tail_idx--;
        /* Evaluate items before the tail. */
        for (uint32_t i = 0; i < tail_idx; i++) {
            TuriValue v = eval_expr(env, frame, items[i]);
            if (turi_is_error(v) || env->returning || env->throwing) return v;
        }
        if (tail_idx + 1 < n) {
            /* Trailing EX_DEFER items exist after the tail expression.
             * Evaluate the tail normally (no TCO) so defers fire correctly. */
            TuriValue result = eval_expr(env, frame, items[tail_idx]);
            if (turi_is_error(result) || env->returning || env->throwing)
                return result;
            for (uint32_t i = tail_idx + 1; i < n; i++)
                eval_expr(env, frame, items[i]);
            return result;
        }
        e = items[tail_idx];
        goto restart;
    }

    /* --- let: bind variables, tail on body ------------------------------- */
    case EX_LET: {
        EvalFrame *new_frame = eval_frame_new(frame);
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            TuriValue v = eval_expr(env, new_frame, e->as.let_.bindings[i].init);
            if (turi_is_error(v)) { return v; }
            if (env->throwing)   { return env->throw_value; }
            if (env->returning)  { return env->return_value; }
            frame_bind(new_frame, e->as.let_.bindings[i].binding->name->name, v);
        }
        /* Move into let scope and loop on the body.
         * RC-cleanup defers for let bindings are no-ops in the interpreter
         * so firing them early (via the trampoline's defer mark) is harmless. */
        frame = new_frame;
        e     = e->as.let_.body;
        goto restart;
    }

    /* --- match: find arm, tail on arm body ------------------------------ */
    case EX_MATCH: {
        TuriValue val = eval_expr(env, frame, e->as.match_.scrutinee);
        if (turi_is_error(val) || env->returning || env->throwing) return val;

        MatchArm  *arms   = e->as.match_.arms;
        uint32_t   n_arms = e->as.match_.n_arms;

        for (uint32_t ai = 0; ai < n_arms; ai++) {
            MatchArm    *arm = &arms[ai];
            MatchPattern *pat = &arm->pattern;
            bool matched = false;
            EvalFrame *arm_frame = NULL;

            if (pat->is_wildcard) {
                matched   = true;
                arm_frame = eval_frame_new(frame);
            } else if (pat->is_var && pat->union_member_idx >= 0) {
                bool tag_ok = false;
                if (pat->n_bindings >= 1 && pat->bindings[0]) {
                    TypeKind tk = pat->bindings[0]->type.kind;
                    switch (tk) {
                    case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
                    case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
                        tag_ok = (val.tag == TURI_INT); break;
                    case TY_BOOL: tag_ok = (val.tag == TURI_BOOL); break;
                    case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
                        tag_ok = (val.tag == TURI_FLOAT); break;
                    case TY_CSTR: tag_ok = (val.tag == TURI_CSTR); break;
                    case TY_NIL:  tag_ok = (val.tag == TURI_NIL); break;
                    default:
                        tag_ok = (val.tag == TURI_STRUCT || val.tag == TURI_CLOSURE);
                        break;
                    }
                } else {
                    tag_ok = true;
                }
                if (tag_ok) {
                    matched   = true;
                    arm_frame = eval_frame_new(frame);
                    if (pat->var_sym)
                        frame_bind(arm_frame, pat->var_sym->name, val);
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
                matched   = true;
                arm_frame = eval_frame_new(frame);
                frame_bind(arm_frame, pat->var_sym->name, val);
            } else {
                CtorDef *ctor = pat->ctor;
                if (ctor && val.tag == TURI_STRUCT &&
                    strcmp(val.as_struct->name, ctor->name) == 0) {
                    matched   = true;
                    arm_frame = eval_frame_new(frame);
                    for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                        Binding *b = pat->bindings[bi];
                        if (b && bi < val.as_struct->n_fields)
                            frame_bind(arm_frame, b->name->name,
                                       val.as_struct->fields[bi]);
                    }
                }
            }

            if (matched) {
                if (arm->guard) {
                    TuriValue gv = eval_expr(env, arm_frame, arm->guard);
                    if (turi_is_error(gv) || env->returning || env->throwing) {
                        return gv;
                    }
                    if (gv.tag != TURI_BOOL || !gv.as_bool) {
                        continue; /* guard failed */
                    }
                }
                frame = arm_frame;
                e     = arm->body;
                goto restart;
            }
        }
        return turi_error("eval: match: no arm matched");
    }

    /* --- call: return a TCO bounce instead of recursing into eval_apply -- */
    case EX_CALL: {
        TuriValue fn_val;
        if (e->as.call_.fn_binding) {
            fn_val = eval_lookup(env, frame, e->as.call_.fn_binding->name->name);
        } else if (e->as.call_.fn_expr) {
            fn_val = eval_expr(env, frame, e->as.call_.fn_expr);
        } else {
            return turi_error("eval: call with no function");
        }
        if (turi_is_error(fn_val) || env->returning || env->throwing) return fn_val;
        if (fn_val.tag != TURI_CLOSURE)
            return turi_errorf("eval: expected function, got tag %d", fn_val.tag);

        TuriClosure *tcl = fn_val.as_closure;

        TuriValue tco_args[64]; /* MAX_EVAL_ARGS */
        uint32_t  n_args = e->as.call_.n_args;
        if (n_args > 64)
            return turi_errorf("eval: too many call arguments (%u)", n_args);
        for (uint32_t i = 0; i < n_args; i++) {
            tco_args[i] = eval_expr(env, frame, e->as.call_.args[i]);
            if (turi_is_error(tco_args[i]) || env->returning || env->throwing)
                return tco_args[i];
        }

        /* Native functions can't participate in the trampoline -- call directly. */
        if (tcl->native)
            return tcl->native(env, tco_args, n_args, tcl->native_ud);

        return tco_bounce(tcl, tco_args, n_args);
    }

    /* --- everything else: evaluate normally (non-tail) ------------------- */
    default:
        return eval_expr(env, frame, e);
    }
}

/* -------------------------------------------------------------------------
 * Function application
 * ---------------------------------------------------------------------- */

static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args) {
    /* TCO trampoline: loops instead of growing the C call stack for tail calls.
     * Work area for args copied across iterations. */
    TuriValue args_buf[64]; /* MAX_EVAL_ARGS */
    if (args && n_args > 0 && args != args_buf)
        memcpy(args_buf, args, n_args * sizeof(TuriValue));
    args = args_buf;

    for (;;) {
        /* SB3: step-fuel check for TCO iterations (tail calls bypass eval_expr). */
        if (env->step_fuel_limit > 0) {
            if (env->step_fuel == 0)
                return turi_error("eval: step fuel exhausted");
            env->step_fuel--;
        }

        /* Phase S7: native function dispatch -- no TCO for natives. */
        if (cl->native)
            return cl->native(env, args, n_args, cl->native_ud);

        FnDef *fn = (FnDef *)cl->fn;
        /* EX_CLOSURE adds a synthetic __env_p first param for codegen; skip it. */
        uint32_t param_offset    = cl->skip_env_param ? 1u : 0u;
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

        /* Inline-C pattern executor. */
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
                if (handled) return inline_result;
            }
        }

        /* Build call frame on top of the captured environment. */
        EvalFrame *call_frame = eval_frame_new((EvalFrame *)cl->captured);
        for (uint32_t i = 0; i < n_args; i++)
            frame_bind(call_frame, fn->params[param_offset + i]->name->name, args[i]);

        /* Mark defer stack; defers registered in this call fire before we
         * either return a value or jump to the next tail-call iteration. */
        DeferItem *defer_mark   = (DeferItem *)env->defer_stack;
        bool       was_returning = env->returning;
        bool       was_no_unwind = env->in_no_unwind;
        env->returning    = false;
        env->in_no_unwind = fn->binding && fn->binding->no_unwind;

        /* Evaluate body with TCO support. */
        TuriValue result = eval_body_tco(env, call_frame, fn->body);

        env->in_no_unwind = was_no_unwind;

        /* Fire defers accumulated in this call iteration (LIFO). */
        fire_defers_to_mark_reversed(env, defer_mark, NULL);

        /* --- Trampoline: bounce to next tail call -------------------------- */
        if (result.tag == TURI_TAG_TCO) {
            TcoFrame *tc = (TcoFrame *)result.as_ref;
            cl     = tc->cl;
            n_args = tc->n_args;
            memcpy(args_buf, tc->args, n_args * sizeof(TuriValue));
            free(tc);
            /* env->returning was cleared at start of this iteration; clear
             * again in case the TCO body set it (shouldn't happen, but safe). */
            env->returning = false;
            continue; /* loop -- no C stack growth */
        }

        /* --- Normal return ------------------------------------------------ */
        TuriValue ret;
        if (env->returning) {
            ret            = env->return_value;
            env->returning = was_returning;
        } else if (env->throwing) {
            ret = env->throw_value;
        } else {
            ret = result;
        }

        /* Do NOT free call_frame: closures may have captured it.  The
         * interpreter runs in a short-lived worker process; leaking is fine. */
        (void)call_frame;
        return ret;
    }
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

    case EX_CSTR_LIT: {
        /* Intern the string so that identical literals share the same pointer.
         * This is required for HAMT key comparison (pointer equality) to work
         * correctly when the same string literal appears multiple times. */
        StrSlice sl = { e->as.s.p, e->as.s.len };
        const Symbol *sym = symtab_intern(&env->st, sl);
        return turi_cstr(sym->name);
    }

    /* --- Variable -------------------------------------------------------- */
    case EX_VAR:
        return eval_lookup(env, frame, e->as.var.binding->name->name);

    /* --- Let ------------------------------------------------------------- */
    case EX_LET: {
        EvalFrame *new_frame = eval_frame_new(frame);
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            TuriValue v = eval_expr(env, new_frame, e->as.let_.bindings[i].init);
            if (turi_is_error(v)) { eval_frame_free(new_frame); return v; }
            if (env->throwing)   { eval_frame_free(new_frame); return env->throw_value; }
            if (env->returning)  { eval_frame_free(new_frame); return env->return_value; }
            frame_bind(new_frame, e->as.let_.bindings[i].binding->name->name, v);
        }
        /* Mark defer stack so defers registered in this let-scope fire on exit. */
        DeferItem *let_defer_mark = (DeferItem *)env->defer_stack;
        TuriValue result = eval_expr(env, new_frame, e->as.let_.body);
        /* Fire this scope's defers on normal exit only.
         * On early-return or throw, leave defers on the stack; eval_apply will
         * fire them all in outer-first order (matching tur_frame_fire_chain). */
        if (!env->returning && !env->throwing)
            fire_defers_to_mark(env, let_defer_mark, NULL);
        eval_frame_free(new_frame);
        return result;
    }

    /* --- If -------------------------------------------------------------- */
    case EX_IF: {
        TuriValue cond = eval_expr(env, frame, e->as.if_.cond);
        if (turi_is_error(cond) || env->returning || env->throwing) return cond;
        if (turi_is_truthy(cond)) {
            return eval_expr(env, frame, e->as.if_.then_);
        } else if (e->as.if_.else_or_null) {
            return eval_expr(env, frame, e->as.if_.else_or_null);
        }
        return turi_nil();
    }

    /* --- Do / Program ---------------------------------------------------- */
    case EX_DO:
    case EX_PROGRAM: {
        Expr   **items = (e->kind == EX_PROGRAM) ? e->as.program.items : e->as.do_.items;
        uint32_t n     = (e->kind == EX_PROGRAM) ? e->as.program.n     : e->as.do_.n;
        /* Defers appended at the end (by rc auto-drop injection) must not
         * count as the "last value" — only the last non-defer item does. */
        TuriValue last = turi_nil();
        for (uint32_t i = 0; i < n; i++) {
            TuriValue v = eval_expr(env, frame, items[i]);
            if (turi_is_error(v) || env->returning || env->throwing) return v;
            if (items[i]->kind != EX_DEFER)
                last = v;
        }
        return last;
    }

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
    case EX_BUILTIN: {
        TuriValue args[MAX_EVAL_ARGS];
        uint32_t  n = e->as.builtin.n;
        if (n > MAX_EVAL_ARGS)
            return turi_errorf("eval: too many builtin arguments (%u)", n);

        /* and/or must short-circuit before evaluating remaining args. */
        if (e->as.builtin.spec->shape == BS_AND_SC) {
            for (uint32_t i = 0; i < n; i++) {
                TuriValue v = eval_expr(env, frame, e->as.builtin.args[i]);
                if (turi_is_error(v) || env->returning || env->throwing) return v;
                if (!turi_is_truthy(v)) return turi_bool(false);
            }
            return turi_bool(true);
        }
        if (e->as.builtin.spec->shape == BS_OR_SC) {
            for (uint32_t i = 0; i < n; i++) {
                TuriValue v = eval_expr(env, frame, e->as.builtin.args[i]);
                if (turi_is_error(v) || env->returning || env->throwing) return v;
                if (turi_is_truthy(v)) return turi_bool(true);
            }
            return turi_bool(false);
        }

        for (uint32_t i = 0; i < n; i++) {
            args[i] = eval_expr(env, frame, e->as.builtin.args[i]);
            if (turi_is_error(args[i]) || env->returning || env->throwing) return args[i];
        }
        return eval_builtin(env, e->as.builtin.spec, args, n);
    }

    /* --- Named function definition (defn) -------------------------------- */
    case EX_FN_DEF: {
        FnDef *fndef = e->as.fn_def_.fn;
        /* If the body is inline-C and a native override is already registered,
         * keep the native rather than overwriting it with the inline-C closure. */
        if (fndef->body && fndef->body->kind == EX_INLINE_C) {
            const char *fname = fndef->binding->name->name;
            TuriValue existing = turi_env_get(env, fname);
            if (existing.tag == TURI_CLOSURE && existing.as_closure &&
                existing.as_closure->native) {
                return existing; /* keep native override */
            }
        }
        TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl)); /* zero native/skip_env_param/native_ud */
        cl->fn       = fndef;
        cl->captured = NULL; /* top-level defn has no captured environment */
        TuriValue v  = turi_closure(cl);
        turi_env_set(env, fndef->binding->name->name, v);
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
    case EX_CALL: {
        TuriValue fn_val;
        if (e->as.call_.fn_binding) {
            fn_val = eval_lookup(env, frame,
                                 e->as.call_.fn_binding->name->name);
        } else if (e->as.call_.fn_expr) {
            fn_val = eval_expr(env, frame, e->as.call_.fn_expr);
        } else {
            return turi_error("eval: call with no function");
        }
        if (turi_is_error(fn_val) || env->returning || env->throwing) return fn_val;
        if (fn_val.tag != TURI_CLOSURE)
            return turi_errorf("eval: expected function, got tag %d", fn_val.tag);

        TuriValue args[MAX_EVAL_ARGS];
        uint32_t  n_args = e->as.call_.n_args;
        if (n_args > MAX_EVAL_ARGS)
            return turi_errorf("eval: too many call arguments (%u)", n_args);

        for (uint32_t i = 0; i < n_args; i++) {
            args[i] = eval_expr(env, frame, e->as.call_.args[i]);
            if (turi_is_error(args[i]) || env->returning || env->throwing) return args[i];
        }
        return eval_apply(env, fn_val.as_closure, args, n_args);
    }

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
        for (uint32_t i = 0; i < mod->n_body; i++) {
            last = eval_expr(env, frame, mod->body[i]);
            if (turi_is_error(last) || env->returning || env->throwing) return last;
        }
        return last;
    }

    /* --- Phase S4: Structs ---------------------------------------------- */

    case EX_MAKE_STRUCT: {
        uint32_t  n = e->as.make_struct_.n_fields;
        TuriValue fields[MAX_EVAL_ARGS];
        if (n > MAX_EVAL_ARGS)
            return turi_errorf("eval: struct has too many fields (%u)", n);

        for (uint32_t i = 0; i < n; i++) {
            fields[i] = eval_expr(env, frame, e->as.make_struct_.field_values[i]);
            if (turi_is_error(fields[i]) || env->returning || env->throwing)
                return fields[i];
        }
        StructDef *sdef = e->as.make_struct_.def;
        const char *sname = sdef ? sdef->name : "<struct>";
        return make_struct_val_def(sname, n, fields, sdef);
    }

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
        if (sv.tag != TURI_STRUCT)
            return turi_errorf("eval: field access on non-struct (tag %d)", sv.tag);
        uint32_t idx = e->as.get_field_.field_idx;
        if (idx >= sv.as_struct->n_fields)
            return turi_errorf("eval: field index %u out of bounds (%u fields)",
                               idx, sv.as_struct->n_fields);
        return sv.as_struct->fields[idx];
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
    case EX_MATCH: {
        TuriValue val = eval_expr(env, frame, e->as.match_.scrutinee);
        if (turi_is_error(val) || env->returning || env->throwing) return val;

        MatchArm  *arms   = e->as.match_.arms;
        uint32_t   n_arms = e->as.match_.n_arms;

        for (uint32_t ai = 0; ai < n_arms; ai++) {
            MatchArm    *arm = &arms[ai];
            MatchPattern *pat = &arm->pattern;

            bool matched = false;
            EvalFrame *arm_frame = NULL;

            if (pat->is_wildcard) {
                matched   = true;
                arm_frame = eval_frame_new(frame);
            } else if (pat->is_var && pat->union_member_idx >= 0) {
                /* IT4: Union type-narrowing arm — check runtime tag against member type */
                bool tag_ok = false;
                if (pat->n_bindings >= 1 && pat->bindings[0]) {
                    TypeKind tk = pat->bindings[0]->type.kind;
                    switch (tk) {
                    case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
                    case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
                        tag_ok = (val.tag == TURI_INT); break;
                    case TY_BOOL:
                        tag_ok = (val.tag == TURI_BOOL); break;
                    case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
                        tag_ok = (val.tag == TURI_FLOAT); break;
                    case TY_CSTR:
                        tag_ok = (val.tag == TURI_CSTR); break;
                    case TY_NIL:
                        tag_ok = (val.tag == TURI_NIL); break;
                    default:
                        /* For struct/closure/pointer types, accept any non-primitive. */
                        tag_ok = (val.tag == TURI_STRUCT || val.tag == TURI_CLOSURE);
                        break;
                    }
                } else {
                    tag_ok = true; /* no type info — treat as var capture */
                }
                if (tag_ok) {
                    matched   = true;
                    arm_frame = eval_frame_new(frame);
                    if (pat->var_sym)
                        frame_bind(arm_frame, pat->var_sym->name, val);
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
                matched   = true;
                arm_frame = eval_frame_new(frame);
                frame_bind(arm_frame, pat->var_sym->name, val);
            } else {
                CtorDef *ctor = pat->ctor;
                if (ctor && val.tag == TURI_STRUCT &&
                    strcmp(val.as_struct->name, ctor->name) == 0) {
                    matched   = true;
                    arm_frame = eval_frame_new(frame);
                    for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                        Binding *b = pat->bindings[bi];
                        if (b && bi < val.as_struct->n_fields)
                            frame_bind(arm_frame, b->name->name,
                                       val.as_struct->fields[bi]);
                    }
                }
            }

            if (matched) {
                /* Optional guard: re-check if the arm has a when-guard. */
                if (arm->guard) {
                    TuriValue gv = eval_expr(env, arm_frame, arm->guard);
                    if (turi_is_error(gv) || env->returning || env->throwing) {
                        eval_frame_free(arm_frame);
                        return gv;
                    }
                    if (gv.tag != TURI_BOOL || !gv.as_bool) {
                        eval_frame_free(arm_frame);
                        continue;
                    }
                }
                TuriValue result = eval_expr(env, arm_frame, arm->body);
                eval_frame_free(arm_frame);
                return result;
            }
        }
        return turi_error("eval: match: no arm matched");
    }

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

        TuriValue args[MAX_EVAL_ARGS];
        uint8_t   n_args = pe->n_args;
        if (n_args > MAX_EVAL_ARGS)
            return turi_errorf("eval: too many effect arguments (%u)", n_args);

        for (uint8_t i = 0; i < n_args; i++) {
            args[i] = eval_expr(env, frame, pe->args[i]);
            if (turi_is_error(args[i]) || env->returning || env->throwing) return args[i];
        }

        /* Always yield to the innermost handler so nested handlers can
         * propagate unmatched effects upward (see eval_handle_inner). */
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
        /* Resumed: return the value sent by resume k v. */
        return cont->resume_val;
    }

    /* (handle BODY cases...) — install handler, run BODY in a fiber. */
    case EX_HANDLE:
        return eval_handle(env, frame, e->as.handle_.handle);

    /* (resume k value) — resume a live continuation with a value. */
    case EX_RESUME: {
        ResumeExpr *re = e->as.resume_.resume;
        TuriValue k   = eval_expr(env, frame, re->k);
        if (turi_is_error(k) || env->returning || env->throwing) return k;
        TuriValue val = eval_expr(env, frame, re->value);
        if (turi_is_error(val) || env->returning || env->throwing) return val;

        if (k.tag != TURI_EFFECT_CONT)
            return turi_error("eval: resume: not a continuation");

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
        return turi_error("eval: inline-C not supported in interpreter mode "
                          "(function uses native C implementation)");

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
        if (cl_val.tag != TURI_CLOSURE)
            return turi_errorf("eval: async: expected a function, got tag %d", cl_val.tag);

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
        /* Compiler-only in TS2. Keep interpreter traversal exhaustive until a
         * later phase needs runtime reinterpret semantics too. */
        return eval_expr(env, frame, e->as.reinterpret_.expr);

    /* --- Phase 2: type ascription is transparent at runtime ---------------- */
    case EX_ASCRIBE:
        return eval_expr(env, frame, e->as.ascribe_.inner);

    /* --- Phase N: poly wrap is transparent in the interpreter -------------- */
    case EX_POLY_WRAP:
        return eval_expr(env, frame, e->as.poly_wrap_.inner);

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
        if (env->panicking) {
            /* Double panic: a defer itself panicked. */
            fprintf(stderr, "double panic: aborting\n");
            fflush(stderr);
            fflush(stdout);
            abort();
        }
        /* If a catch-unwind boundary is active, longjmp to it. */
        if (env->catch_jmp && !env->in_no_unwind) {
            strncpy(env->catch_panic_msg, s, sizeof(env->catch_panic_msg) - 1);
            env->catch_panic_msg[sizeof(env->catch_panic_msg) - 1] = '\0';
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

    case EX_PANIC_WITH: {
        /* Typed panic payload — just needs "panic at" in stderr and nonzero exit. */
        if (env->panicking) {
            fprintf(stderr, "double panic: aborting\n");
            fflush(stderr);
            fflush(stdout);
            abort();
        }
        /* If a catch-unwind boundary is active, longjmp to it. */
        if (env->catch_jmp && !env->in_no_unwind) {
            strncpy(env->catch_panic_msg, "typed panic", sizeof(env->catch_panic_msg) - 1);
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
        env->catch_jmp = &jb;

        TuriValue result;
        if (setjmp(jb) == 0) {
            /* Normal execution path — call the thunk with no arguments. */
            result = eval_apply(env, thunk_val.as_closure, NULL, 0);
            env->catch_jmp = prev_jmp;
            if (turi_is_error(result) || env->returning || env->throwing)
                return result;
            /* Wrap successful result in (ok value). */
            return make_struct_val("ok", 1, &result);
        } else {
            /* A panic was caught — restore env and return (panic). */
            env->catch_jmp    = prev_jmp;
            env->panicking    = false;
            env->returning    = prev_returning;
            env->throwing     = prev_throwing;
            env->throw_value  = prev_throw;
            env->return_value = prev_ret;
            (void)prev_panicking;
            return make_struct_val("panic", 0, NULL);
        }
    }

    /* --- Phase 9: rc<T> with shared reference counter in interpreter ------- */
    case EX_RC_OF: {
        /* Allocate shared counter, wrap value in __rc struct. */
        TuriValue v = eval_expr(env, frame, e->as.rc_of_.expr);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        int64_t *cnt = (int64_t *)malloc(sizeof(int64_t)); *cnt = 1;
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
        /* Convert rc<T> to ref<T>: extract the inner value from the __rc struct. */
        TuriValue rv = eval_expr(env, frame, e->as.ref_from_rc_.expr);
        if (turi_is_error(rv) || env->returning || env->throwing) return rv;
        if (rv.tag == TURI_STRUCT && rv.as_struct
            && rv.as_struct->name && strcmp(rv.as_struct->name, "__rc") == 0
            && rv.as_struct->n_fields >= 2)
            return rv.as_struct->fields[1];
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
    case EX_WEAK:
        return eval_expr(env, frame, e->as.weak_.expr);
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
    case EX_ANY_CAST:
        /* In the interpreter, 'any' values are untagged (union injection is
         * transparent), so (cast x T) just returns the inner value. */
        return eval_expr(env, frame, e->as.any_cast_.value);

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
            tname = (v.as_struct && v.as_struct->name) ? v.as_struct->name : "struct";
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

    /* serial-reset: Phase 21 not yet implemented in interpreter. */
    case EX_SERIAL_RESET:
        return turi_errorf("eval: EX_SERIAL_RESET (Phase 21 serial-shift/reset) is not yet "
                           "implemented in the interpreter");

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
    {
        const char *rest     = src;
        size_t      rest_len = body_len;
        ReaderType  detected = detect_lang(src, body_len, &rest, &rest_len);
        if (rest != src) {
            /* A #lang directive was found — strip it from the source body. */
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
    sfile->path        = "<eval>";
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
