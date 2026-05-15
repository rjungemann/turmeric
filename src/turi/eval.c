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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    EvalBinding *b = f->bindings;
    while (b) {
        EvalBinding *next = b->next;
        free(b);
        b = next;
    }
    free(f);
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
};

static TuriValue make_struct_val(const char *name, uint32_t n, TuriValue *fields) {
    TuriStruct *s = (TuriStruct *)malloc(sizeof(TuriStruct));
    s->name     = name;
    s->n_fields = n;
    s->fields   = (TuriValue *)malloc(n * sizeof(TuriValue));
    for (uint32_t i = 0; i < n; i++) s->fields[i] = fields[i];
    return turi_struct_val(s);
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

/* Returns true if the builtin performs I/O (used for sandboxed check). */
static bool is_io_builtin(BuiltinShape shape) {
    switch (shape) {
    case BS_PRINTLN_INT:
    case BS_PRINTLN_FLOAT:
    case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR:
    case BS_PRINTLN_UINT:
    case BS_PRINTLN_FLOAT32:
    case BS_DLOPEN:
    case BS_DLSYM:
    case BS_DLCLOSE:
        return true;
    default:
        return false;
    }
}

static TuriValue eval_builtin(TuriEnv *env, const BuiltinSpec *spec,
                               TuriValue *args, uint32_t n) {
    BuiltinShape shape = spec->shape;

    if (env->sandboxed && is_io_builtin(shape)) {
        return turi_error("eval: I/O not allowed in sandboxed environment");
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
        }
        if (strcmp(op, "!=") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   != args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float != args[1].as_float);
            if (args[0].tag == TURI_BOOL)  return turi_bool(args[0].as_bool  != args[1].as_bool);
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
        return turi_nil();
    }

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

    default:
        /* Silently return nil for unsupported builtins (unsafe ops, STM, etc.) */
        return turi_nil();
    }
}

/* -------------------------------------------------------------------------
 * Function application
 * ---------------------------------------------------------------------- */

static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args) {
    /* Phase S7: native function dispatch */
    if (cl->native) {
        return cl->native(env, args, n_args, cl->native_ud);
    }

    FnDef *fn = (FnDef *)cl->fn;
    /* EX_CLOSURE adds a synthetic __env_p first param for codegen; skip it. */
    uint32_t param_offset = cl->skip_env_param ? 1u : 0u;
    uint32_t effective_params = (uint32_t)fn->n_params - param_offset;
    if (effective_params != n_args) {
        return turi_errorf("eval: arity mismatch: %s expects %u args, got %u",
                           fn->binding ? fn->binding->name->name : "<fn>",
                           (unsigned)effective_params, (unsigned)n_args);
    }

    /* Build call frame on top of the captured environment */
    EvalFrame *call_frame = eval_frame_new((EvalFrame *)cl->captured);
    for (uint32_t i = 0; i < n_args; i++) {
        frame_bind(call_frame, fn->params[param_offset + i]->name->name, args[i]);
    }

    /* Mark the defer stack; defers registered during this call fire on exit. */
    DeferItem *defer_mark = (DeferItem *)env->defer_stack;

    /* Evaluate the body; handle early-return and throw signals. */
    bool was_returning = env->returning;
    env->returning     = false;

    TuriValue result = eval_expr(env, call_frame, fn->body);

    /* Fire defers (LIFO) registered in this call scope. */
    fire_defers_to_mark(env, defer_mark, NULL);

    TuriValue ret;
    if (env->returning) {
        ret = env->return_value;
        env->returning = was_returning; /* restore caller's return state */
    } else if (env->throwing) {
        /* Throw propagates through function calls; leave env->throwing set. */
        ret = env->throw_value;
    } else {
        ret = result;
    }

    eval_frame_free(call_frame);
    return ret;
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
        /* StrSlice — copy to a malloc'd NUL-terminated string */
        char *s = (char *)malloc(e->as.s.len + 1);
        memcpy(s, e->as.s.p, e->as.s.len);
        s[e->as.s.len] = '\0';
        return turi_cstr(s);
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
        TuriValue result = eval_expr(env, new_frame, e->as.let_.body);
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
        TuriValue last = turi_nil();
        for (uint32_t i = 0; i < n; i++) {
            last = eval_expr(env, frame, items[i]);
            if (turi_is_error(last) || env->returning || env->throwing) return last;
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
        TuriValue v = eval_expr(env, frame, e->as.def_.init);
        if (turi_is_error(v) || env->returning || env->throwing) return v;
        turi_env_set(env, e->as.def_.binding->name->name, v);
        return v;
    }

    /* --- Builtin --------------------------------------------------------- */
    case EX_BUILTIN: {
        TuriValue args[MAX_EVAL_ARGS];
        uint32_t  n = e->as.builtin.n;
        if (n > MAX_EVAL_ARGS)
            return turi_errorf("eval: too many builtin arguments (%u)", n);

        for (uint32_t i = 0; i < n; i++) {
            args[i] = eval_expr(env, frame, e->as.builtin.args[i]);
            if (turi_is_error(args[i]) || env->returning || env->throwing) return args[i];
        }
        return eval_builtin(env, e->as.builtin.spec, args, n);
    }

    /* --- Named function definition (defn) -------------------------------- */
    case EX_FN_DEF: {
        TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl)); /* zero native/skip_env_param/native_ud */
        cl->fn       = e->as.fn_def_.fn;
        cl->captured = NULL; /* top-level defn has no captured environment */
        TuriValue v  = turi_closure(cl);
        turi_env_set(env, e->as.fn_def_.fn->binding->name->name, v);
        return v;
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
        const char *sname = e->as.make_struct_.def
                            ? e->as.make_struct_.def->name : "<struct>";
        return make_struct_val(sname, n, fields);
    }

    case EX_SET_LIT:
        /* Set literals are not interpreted in the REPL; return nil. */
        return turi_nil();

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

        /* Walk handler stack for a matching case. */
        TuriHandlerFrame *hf = (TuriHandlerFrame *)env->handler_stack;
        while (hf) {
            for (uint8_t i = 0; i < hf->n_cases; i++) {
                if (strcmp(hf->cases[i].effect_name->name, effect_name) == 0) {
                    TuriEffectCont *cont = hf->cont;
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
            }
            hf = hf->prev;
        }
        return turi_errorf("eval: unhandled effect: %s", effect_name);
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
        if (env->sandboxed)
            return turi_error("eval: inline-C not allowed in sandboxed environment");
        return turi_error("eval: inline-C blocks cannot be executed by the interpreter");

    /* --- Phase S7: async / await ----------------------------------------- */

    /* (async fn-expr) — spawn a fiber that evaluates fn-expr; return Future. */
    case EX_ASYNC: {
        if (env->sandboxed)
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

    /* --- Everything else — silently return nil --------------------------- */
    default:
        return turi_nil();
    }
}

/* -------------------------------------------------------------------------
 * turi_eval: public entry point
 * ---------------------------------------------------------------------- */

TuriValue turi_eval(TuriEnv *env, const char *src) {
    if (!env || !src) return turi_error("turi_eval: null argument");

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

    /* 5. Parse. */
    uint32_t  nforms = 0;
    Form    **forms  = read_all(eval_arena, &env->st, sfile, &nforms);
    if (!forms || diag_had_error()) {
        return turi_error("parse error");
    }

    /* 6. Elaborate (read-only path: no borrow-check, no CPS, no emit). */
    Expr *prog = elaborate_program(eval_arena, &env->st,
                                   forms, nforms,
                                   /*stdlib_prefix=*/0,
                                   /*module_base_dir=*/".",
                                   /*separate_compilation=*/false,
                                   /*out_tc_env=*/NULL,
                                   /*include_dirs=*/NULL,
                                   /*n_include_dirs=*/0);
    if (!prog || diag_had_error()) {
        return turi_error("elaboration error");
    }

    /* 7. Evaluate only the NEW top-level expressions. */
    uint32_t prior = env->prior_toplevel;
    uint32_t total = prog->as.program.n;

    TuriValue last = turi_nil();
    for (uint32_t i = prior; i < total; i++) {
        last = eval_expr(env, NULL, prog->as.program.items[i]);
        /* Clear any dangling return signal at the top level */
        if (env->returning) {
            last = env->return_value;
            env->returning = false;
        }
        /* Convert an uncaught throw into a TURI_ERROR */
        if (env->throwing) {
            env->throwing = false;
            TuriValue tv = env->throw_value;
            if (tv.tag == TURI_THROW && tv.as_throw) {
                TuriValue inner = tv.as_throw->value;
                if (inner.tag == TURI_CSTR && inner.as_cstr)
                    last = turi_errorf("uncaught exception: %s", inner.as_cstr);
                else
                    last = turi_error("uncaught exception");
            } else {
                last = turi_error("uncaught exception");
            }
            break;
        }
        if (turi_is_error(last)) break;
    }

    /* 8. Update accumulated state only on success. */
    if (!turi_is_error(last)) {
        /* Append new source (without any leading #lang line) to accumulator */
        if (env->src_acc.len > 0) buf_putc(&env->src_acc, '\n');
        buf_write(&env->src_acc, src_body, body_len);
        env->prior_toplevel = total;
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
 * turi_init / turi_value_repr
 * ---------------------------------------------------------------------- */

void turi_init(bool use_color) {
    diag_init(use_color);
}

void turi_value_repr(char *buf, size_t cap, TuriValue v) {
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
        const char *n = v.as_struct ? v.as_struct->name : "?";
        snprintf(buf, cap, "#<struct %s>", n ? n : "?");
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
    }
}
