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
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
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
#ifndef _WIN32
#  include <dlfcn.h>   /* jit-ffi-c2mir-plan: real dlopen/dlsym in turi */
#endif

#if defined(_WIN32)
/* Windows: <sys/mman.h> and <ucontext.h> do not exist.  Both are shimmed --
 * mmap over VirtualAlloc, ucontext over Win32 Fibers -- so the coroutine and
 * effect-handler machinery below works unchanged. */
#  include "platform_mman.h"
#  include "platform_ucontext_win.h"
#elif !defined(__EMSCRIPTEN__)
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
#include "runtime/hamt.h"   /* tur_hamt_hash_str -- struct-key content hash */
#include "runtime/tur_string.h"  /* tur_string_cstr/release -- Show returns owned String */
#include "diag.h"
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "mangle.h"
#include "reader.h"
#include "symbols.h"
#include "types.h"
#include "../passes/effect_check.h"
#include "../passes/borrow_check.h"
#include "../runtime/globals.h"  /* Gap 7: g_interpret_mode (per-env snapshot) */
#include "ffi_thunk.h"  /* jit-ffi-c2mir-plan F2: thunk-backed extern-c */
#include "jit_ffi.h"    /* jit-ffi-c2mir-plan: provider + sig vocabulary */

/* T1 (turi-eval-trampoline-plan): small inline arg/field buffer with a heap
 * spill above it.  Keeps the per-call scratch off the C stack for the common
 * low-arity case while bounding deep-recursion frame growth -- the dominant
 * per-frame cost was several TuriValue[MAX_EVAL_ARGS] (1 KB) arrays.  8 covers
 * the overwhelmingly common 0..8-arg call with zero allocation. */
#define EVAL_SCRATCH_INLINE 8

/* T1: language-wide function/effect arity cap (mirrors MAX_FN_ARITY in
 * compiler/types.h).  Used as a fixed inline buffer size where the arity is
 * provably bounded -- closure-application TCO buffers and effect-perform args --
 * so those scratch arrays stay a fixed size with no heap spill.  A defensive
 * guard rejects anything above it (unreachable for elaboration-checked
 * programs).  Raised to 64 alongside MAX_FN_ARITY (arbitrary-fn-arity Phase 1). */
#define EVAL_MAX_FN_ARITY 64  /* mirrors MAX_FN_ARITY (arbitrary-fn-arity Phase 1) */

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
#ifndef NDEBUG
    /* libturi-per-embed-env-and-peripherals Gap 6: debug-only env tag.  Closures
     * hold pointers into their originating env's arenas and must not outlive (or
     * be applied under) a different env.  Claimed on first application
     * (eval_apply_driven) and asserted on every later one, so caching a closure
     * across per-script envs trips a clean abort here instead of a heap
     * use-after-free.  Compiled out entirely in release (NDEBUG) -- zero cost.
     * Every closure is born zeroed (calloc / memset / struct-copy), so this
     * starts NULL without per-site initialisation. */
    TuriEnv        *origin_env;
#endif
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
    TuriClosure *cl = (TuriClosure *)turi_val_calloc(env, sizeof(TuriClosure));
    cl->fn        = NULL;
    cl->captured  = NULL;
    cl->native    = fn;
    cl->native_ud = ud;
    turi_env_set(env, name, turi_closure(cl));
}

/* Typed variant: install the native exactly as turi_env_register_native does,
 * then record its runtime return type in the process-global signature registry
 * so the elaborator can type calls to it (and typed wrappers over it).  See
 * docs/archive/history/untyped-native-registration-blocks-curated-facades.md. */
void turi_env_register_native_typed(TuriEnv *env, const char *name,
                                    TuriNativeFn fn, void *ud,
                                    TurNativeRetType ret) {
    turi_env_register_native(env, name, fn, ud);
    tur_native_sig_register(name, ret);
}

/* libturi-per-embed-env-and-peripherals Gap 2: true when v is a native-closure
 * binding, so turi_env_reset can distinguish embedder/builtin natives (kept)
 * from turi_eval-created defns/defs (dropped). */
bool turi_value_is_native(TuriValue v) {
    return v.tag == TURI_CLOSURE && v.as_closure && v.as_closure->native != NULL;
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

/* (error msg) -- construct a TURI_REJECTION value.  Surfaces async-task
 * rejections without going through the deprecated (throw ...) form.  Distinct
 * from TURI_ERROR so the interpreter's universal turi_is_error short-circuit
 * does NOT propagate the value out of `(let [r (await ...)] ...)` -- callers
 * keep evaluating and observe the rejection via (error? r). */
static TuriValue native_error_ctor(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_rejection("<error>");
    if (args[0].tag == TURI_CSTR)      return turi_rejection(args[0].as_cstr ? args[0].as_cstr : "<error>");
    if (args[0].tag == TURI_REJECTION) return args[0];
    if (args[0].tag == TURI_ERROR)     return turi_rejection(args[0].as_error ? args[0].as_error : "<error>");
    return turi_rejection("<error>");
}

/* (error? v) -- true if v is a rejection value. */
static TuriValue native_error_pred(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    return turi_bool(args[0].tag == TURI_REJECTION);
}

/* (error-message v) -- extract the message from a rejection value, or "" if
 * the value is not a rejection. */
static TuriValue native_error_message(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || args[0].tag != TURI_REJECTION) return turi_cstr("");
    return turi_cstr(args[0].as_error ? args[0].as_error : "");
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
    /* Error-value primitives: build/inspect TURI_ERROR values without going
     * through the deprecated (throw ...) channel. */
    turi_env_register_native(env, "error",         native_error_ctor,    NULL);
    turi_env_register_native(env, "error?",        native_error_pred,    NULL);
    turi_env_register_native(env, "error-message", native_error_message, NULL);
}

/* -------------------------------------------------------------------------
 * Phase S7: Async fiber thunk
 * ---------------------------------------------------------------------- */

TUR_THREAD_LOCAL TuriFiber *g_pending_async_fiber;

/* Forward declarations needed by the thunk. */
static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e);
static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args);

/* Debugger Phase 2: eval-loop hooks (no-ops unless a debugger is attached).
 * Defined near the end of this file, after the value printer they rely on. */
static void turi_dbg_before_node(TuriEnv *env, EvalFrame *frame,
                                  const Expr *e, bool from_driver);
static void turi_dbg_push(TuriEnv *env, const FnDef *fn, EvalFrame *cf);
static void turi_dbg_pop(TuriEnv *env);
static void turi_dbg_set_top(TuriEnv *env, const FnDef *fn, EvalFrame *cf);

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
                           turi_rejection("task cancelled"));
    } else if (env->throwing) {
        TuriValue err = env->throw_value;
        env->throwing = false;
        turi_future_reject(env, fiber->own_future, err);
    } else if (turi_is_rejection(result) || turi_is_error(result)) {
        /* (error "boom") in the async body -- DEPR-R0. */
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
    (void)env; (void)args; (void)n; (void)ud;
    /* No-op under --interpret.  An inline-C `malloc(...)` body is reproduced by
     * the tree-walker via the env value-arena (turi_val_alloc), NOT raw malloc,
     * so a program that pairs an inline-C allocation with an extern-c `free`
     * (e.g. tests/fixtures/typed/slice-basic's make-arr + free) would otherwise
     * hand a non-heap arena pointer to libc free -- a hard bad-free abort.  The
     * interpreter already runs process-lifetime (it never frees its closures or
     * registered natives), so leaking the occasional genuinely-malloc'd carrier
     * here is consistent and harmless; the arena is reclaimed at env teardown. */
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

/* Register the semantics-bearing overrides for well-known libc names.
 * Returns true when `fname` was one of them.  These win over the JIT FFI
 * thunk path deliberately: `free` must stay a no-op in turi (inline-C
 * allocations are reproduced from the env value-arena, not raw malloc),
 * `exit` must flush-and-_exit, and printf/puts marshal turi values rather
 * than trusting a variadic ABI. */
static bool register_extern_c_known(TuriEnv *env, const char *fname) {
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
            return true;
        }
    }
    return false;
}

/* Forward decls: the aggregate marshalling engine lives with eval_call_ptr
 * below; extern-c registration (the F4 follow-on) reuses it wholesale. */
static size_t agg_sig_len(const AdtDef *def);
static size_t agg_sig_render(const AdtDef *def, char *buf);
static bool   agg_collect_leaves(const AdtDef *def, TuriValue v,
                                 TuriValue *out, int max, int *n);
static TuriValue agg_build_value(TuriEnv *env, const AdtDef *def,
                                 const void *base, const size_t *offs,
                                 const char *codes, int nleaf, int *cur);
static void agg_store_member(void *base, size_t off, char code, TuriValue v);

/* The by-value record def an extern-c slot names, or NULL for a scalar
 * slot.  A full TY_ADT type only reaches ec->param_types / return_type via
 * elaboration's extern_c_aggregate_ok, so the def is already validated. */
static const AdtDef *extern_slot_agg_def(Type t) {
    if (t.kind != TY_ADT || !t.as.adt_.def) return NULL;
    return t.as.adt_.def;
}

/* jit-ffi F4 follow-on: a thunk-backed extern-c native whose signature has
 * at least one by-value aggregate slot.  The sig (with inline `{...}`
 * layouts) is precomputed at registration; each call packs record args
 * into C bytes and rebuilds a record from an aggregate return, exactly as
 * eval_call_ptr does. */
typedef struct ExternAggUd {
    void          *fn;
    const char    *name;   /* borrowed (interned symbol) */
    const ExternC *ec;     /* elaboration arena; process-lifetime under turi */
    char          *sig;
    size_t        *arg_at; /* sig offset of each parameter's slot */
} ExternAggUd;

/* Env-lifetime teardown for the payload above (turi_env_register_native_ex).
 * The ExternC itself belongs to the elaboration arena and is not ours. */
static void extern_agg_ud_free(void *p) {
    ExternAggUd *ud = (ExternAggUd *)p;
    if (!ud) return;
    free(ud->sig);
    free(ud->arg_at);
    free(ud);
}

static TuriValue extern_agg_thunk_native(TuriEnv *env, TuriValue *args,
                                         uint32_t n, void *ud) {
    const ExternAggUd *x = (const ExternAggUd *)ud;

    if (!(env->caps & TURI_CAP_FFI))
        return turi_errorf(
            "ffi: extern-c '%s' is not allowed in a sandboxed environment",
            x->name);
    const TurJitFfiProvider *jp = tur_jit_ffi_provider();
    if (!jp)
        return turi_errorf(
            "ffi: calling extern-c '%s' under --interpret requires a "
            "JIT-enabled build (-DTUR_JIT=ON)", x->name);
    if (n != x->ec->n_params)
        return turi_errorf("ffi: '%s' expects %u arg%s, got %u", x->name,
                           (unsigned)x->ec->n_params,
                           x->ec->n_params == 1 ? "" : "s", (unsigned)n);

    TuriValue result;
    int64_t *i_vals   = (int64_t *)calloc(n ? n : 1, sizeof(int64_t));
    double  *f_vals   = (double  *)calloc(n ? n : 1, sizeof(double));
    void   **s_vals   = (void   **)calloc(n ? n : 1, sizeof(void *));
    void   **agg_bufs = (void   **)calloc(n ? n : 1, sizeof(void *));
    void    *out_s_buf = NULL;
    if (!i_vals || !f_vals || !s_vals || !agg_bufs) {
        result = turi_error("ffi: out of memory marshalling call");
        goto cleanup;
    }

    for (uint32_t k = 0; k < n; k++) {
        const AdtDef *pd = extern_slot_agg_def(x->ec->param_types[k]);
        if (pd) {
            size_t size = 0, align = 1, offs[64];
            char   codes[64];
            int    nleaf = 0;
            if (!tur_jit_ffi_struct_layout(x->sig + x->arg_at[k], &size,
                                           &align, offs, codes, 64, &nleaf)) {
                result = turi_errorf("ffi: '%s' arg %u has an "
                                     "unrepresentable aggregate layout",
                                     x->name, (unsigned)k);
                goto cleanup;
            }
            TuriValue leaves[64];
            int       nl = 0;
            if (!agg_collect_leaves(pd, args[k], leaves, 64, &nl) ||
                nl != nleaf) {
                result = turi_errorf("ffi: '%s' arg %u does not match the "
                                     "shape '%s' declares", x->name,
                                     (unsigned)k,
                                     pd->name ? pd->name : "?");
                goto cleanup;
            }
            void *bytes = calloc(1, size ? size : 1);
            if (!bytes) {
                result = turi_error("ffi: out of memory");
                goto cleanup;
            }
            agg_bufs[k] = bytes;
            s_vals[k]   = bytes;
            for (int i = 0; i < nleaf; i++)
                agg_store_member(bytes, offs[i], codes[i], leaves[i]);
            continue;
        }
        char cls = tur_jit_ffi_class_for_kind(x->ec->param_types[k].kind, 0);
        if (tur_jit_ffi_class_is_int(cls)) {
            switch (args[k].tag) {
                case TURI_INT:  i_vals[k] = args[k].as_int; break;
                case TURI_BOOL: i_vals[k] = args[k].as_bool ? 1 : 0; break;
                case TURI_CSTR:
                    i_vals[k] = (int64_t)(intptr_t)args[k].as_cstr; break;
                case TURI_NIL:  i_vals[k] = 0; break;
                default:
                    result = turi_errorf(
                        "ffi: '%s' arg %u is not an int-class value",
                        x->name, (unsigned)k);
                    goto cleanup;
            }
        } else {   /* 'f' / 'F' */
            if (args[k].tag == TURI_FLOAT)    f_vals[k] = args[k].as_float;
            else if (args[k].tag == TURI_INT) f_vals[k] = (double)args[k].as_int;
            else {
                result = turi_errorf(
                    "ffi: '%s' arg %u is not a float-class value",
                    x->name, (unsigned)k);
                goto cleanup;
            }
        }
    }

    {
        char errbuf[256];
        TurJitFfiThunkFn jt = jp->thunk_for(x->sig, errbuf, sizeof errbuf);
        if (!jt) {
            result = turi_errorf("ffi: '%s': %s", x->name, errbuf);
            goto cleanup;
        }
        int64_t out_i = 0;
        double  out_f = 0.0;

        const AdtDef *rd = extern_slot_agg_def(x->ec->return_type);
        size_t roffs[64];
        char   rcodes[64];
        int    rleaf = 0;
        if (rd) {
            size_t rsize = 0, ralign = 1;
            if (!tur_jit_ffi_struct_layout(x->sig, &rsize, &ralign, roffs,
                                           rcodes, 64, &rleaf)) {
                result = turi_errorf("ffi: '%s' return has an "
                                     "unrepresentable aggregate layout",
                                     x->name);
                goto cleanup;
            }
            out_s_buf = calloc(1, rsize ? rsize : 1);
            if (!out_s_buf) {
                result = turi_error("ffi: out of memory");
                goto cleanup;
            }
        }

        jt(x->fn, i_vals, f_vals, s_vals, &out_i, &out_f, out_s_buf);

        if (rd) {
            int cur = 0;
            result = agg_build_value(env, rd, out_s_buf, roffs, rcodes,
                                     rleaf, &cur);
            if (!turi_is_error(result) && cur != rleaf)
                result = turi_errorf("ffi: '%s' return layout is longer "
                                     "than its record declares", x->name);
            goto cleanup;
        }
        switch (x->ec->return_type.kind) {
            case TY_NIL:     result = turi_nil(); break;
            case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
                result = turi_float(out_f); break;
            case TY_BOOL:    result = turi_bool(out_i != 0); break;
            case TY_CSTR:
                result = out_i ? turi_cstr((const char *)(intptr_t)out_i)
                               : turi_nil();
                break;
            default:         result = turi_int(out_i); break;
        }
    }

cleanup:
    if (agg_bufs)
        for (uint32_t k = 0; k < n; k++) free(agg_bufs[k]);
    free(agg_bufs);
    free(out_s_buf);
    free(s_vals);
    free(i_vals);
    free(f_vals);
    return result;
}

/* Build a full sig string (with inline `{...}` layouts) for a return type
 * and parameter list, recording each parameter slot's sig offset in
 * `arg_at` (n entries, may be NULL).  Returns a malloc'd sig, or NULL when
 * any slot has no representation. */
static char *agg_sig_build(Type ret, const Type *params, uint32_t n,
                           size_t *arg_at) {
    const AdtDef *rd = extern_slot_agg_def(ret);
    size_t cap = 3;
    cap += rd ? agg_sig_len(rd) : 1;
    for (uint32_t k = 0; k < n; k++) {
        const AdtDef *pd = extern_slot_agg_def(params[k]);
        cap += pd ? agg_sig_len(pd) : 1;
    }
    char *sig = (char *)malloc(cap);
    if (!sig) return NULL;

    size_t sp = 0;
    if (rd) {
        size_t w = agg_sig_render(rd, sig);
        if (!w) { free(sig); return NULL; }
        sp = w;
    } else {
        char c = tur_jit_ffi_class_for_kind(ret.kind, 1);
        if (c == '?') { free(sig); return NULL; }
        sig[sp++] = c;
    }
    sig[sp++] = ':';
    for (uint32_t k = 0; k < n; k++) {
        const AdtDef *pd = extern_slot_agg_def(params[k]);
        if (arg_at) arg_at[k] = sp;
        if (pd) {
            size_t w = agg_sig_render(pd, sig + sp);
            if (!w) { free(sig); return NULL; }
            sp += w;
        } else {
            char c = tur_jit_ffi_class_for_kind(params[k].kind, 0);
            if (c == '?' || c == 'v') { free(sig); return NULL; }
            sig[sp++] = c;
        }
    }
    sig[sp] = '\0';
    return sig;
}

/* Register an aggregate-signature extern-c as a thunk-backed native.
 * Returns 0 on success; nonzero falls back to the nil stub (an
 * unrepresentable layout or an unresolvable symbol). */
static int register_extern_c_agg(TuriEnv *env, const ExternC *ec,
                                 const char *fname) {
    const TurJitFfiProvider *jp = tur_jit_ffi_provider();
    uint32_t n = ec->n_params;
    size_t *arg_at = (size_t *)calloc(n ? n : 1, sizeof(size_t));
    char   *sig    = arg_at ? agg_sig_build(ec->return_type, ec->param_types,
                                            n, arg_at)
                            : NULL;
    if (!sig) goto fail;

    {
        void *fn = jp->resolve(ec->c_name ? ec->c_name->name : fname);
        if (!fn) goto fail;
        ExternAggUd *ud = (ExternAggUd *)calloc(1, sizeof(*ud));
        if (!ud) goto fail;
        ud->fn = fn; ud->name = fname; ud->ec = ec;
        ud->sig = sig; ud->arg_at = arg_at;
        /* Env-lifetime, not process-lifetime: a procedural macro's turi env
         * is torn down per compile, and the compile path is leak-checked. */
        turi_env_register_native_ex(env, fname, extern_agg_thunk_native, ud,
                                    extern_agg_ud_free);
        return 0;
    }

fail:
    free(sig);
    free(arg_at);
    return -1;
}

/* jit-ffi-c2mir-plan F2: give an extern-c declaration a real
 * implementation.  Precedence: the known-override table above; then, in a
 * JIT build, a thunk-backed native calling the dlsym-resolved symbol for
 * real; else today's nil stub.  The thunk upgrade is a correctness fix for
 * --interpret -- the 7-entry table used to be the whole story and
 * everything else silently returned nil. */
static void register_extern_c_binding(TuriEnv *env, const ExternC *ec,
                                      const char *fname) {
    if (register_extern_c_known(env, fname)) return;

    const TurJitFfiProvider *jp = tur_jit_ffi_provider();
    if (jp && ec && !ec->is_variadic) {
        /* F4 follow-on: a signature with a by-value aggregate slot takes
         * the aggregate-aware registration; failure falls to the stub. */
        bool any_agg = extern_slot_agg_def(ec->return_type) != NULL;
        for (uint32_t i = 0; !any_agg && i < ec->n_params; i++)
            if (extern_slot_agg_def(ec->param_types[i])) any_agg = true;
        if (any_agg) {
            if (register_extern_c_agg(env, ec, fname) == 0) return;
            turi_env_register_native(env, fname, native_nil_stub, NULL);
            return;
        }
        /* Classify the declared signature.  A '?' anywhere (ADT, carrier)
         * means the thunk vocabulary cannot express it; fall back to the
         * stub rather than mis-calling. */
        char ret = tur_jit_ffi_class_for_kind(ec->return_type.kind, 1);
        bool ok = (ret != '?');
        uint32_t n = ec->n_params;
        char inl[64];
        char *classes = (n <= sizeof inl) ? inl : (char *)malloc(n);
        if (!classes) ok = false;
        for (uint32_t i = 0; ok && i < n; i++) {
            char c = tur_jit_ffi_class_for_kind(ec->param_types[i].kind, 0);
            if (c == '?' || c == 'v') ok = false;
            else classes[i] = c;
        }
        /* Resolve against this process: the executable's exported runtime
         * (ENABLE_EXPORTS) plus anything dlopened RTLD_GLOBAL.  A symbol
         * from a lib the process never linked needs an explicit dlopen (or
         * jit autolink) first -- documented resolution order. */
        void *fn = ok ? jp->resolve(ec->c_name ? ec->c_name->name : fname)
                      : NULL;
        if (fn) {
            int rc = tur_ffi_register_extern_thunk(env, fname, fn, ret,
                                                   classes, n);
            if (classes != inl) free(classes);
            if (rc == 0) return;
        } else if (classes != inl) {
            free(classes);
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

/* generic-dict-dispatch: a runtime tyvar->concrete-type substitution captured at
 * a generic function's call site, so a typeclass method baked to the carrier
 * representative inside the body can re-resolve to the receiver's real instance.
 * See docs/archive/turi-generic-dict-dispatch-bakes-representative-instance.md. */
typedef struct TyvarBind {
    const char       *name;   /* interned tyvar name, e.g. "A" */
    Type              type;   /* the concrete type bound to it at this call site */
    struct TyvarBind *next;
} TyvarBind;

/* turi-dict-passing-plan: a runtime dictionary bound at a dict-clone's apply --
 * "constraint class TC is served by instance INST in this activation".  The
 * tree-walking analogue of the compiled clone's leading `int64_t __dict` param;
 * consulted with EXPLICIT precedence over the gde_* recovery heuristics, which
 * reconstruct the same fact from pinned tyvars / runtime tags.  Chain-walked
 * like tyvars, so a nested mapper lambda that captured the clone's frame reads
 * the dict through its parent chain (the captured-dict case for free). */
typedef struct DictBind {
    struct TypeClass         *tc;
    struct TypeClassInstance *inst;
    /* Constraint tyvar name this dictionary serves, or NULL when unkeyed (a
     * dict-clone param bind).  Distinguishes two same-class constraints on
     * different tyvars -- `map-show-loop [^Show K ^Show V]` carries a Show
     * dictionary for EACH of K and V, and a class-only key could not tell
     * them apart at the dispatch site. */
    const char               *tyvar;
    struct DictBind          *next;
} DictBind;

struct EvalFrame {
    EvalBinding  *bindings;
    EvalFrame    *parent;
    TyvarBind    *tyvars;   /* generic-dict tyvar substitutions (usually NULL) */
    DictBind     *dicts;    /* dict-clone runtime dictionaries (usually NULL) */
};

static EvalFrame *eval_frame_new(TuriEnv *env, EvalFrame *parent) {
    /* Escaping payload: a closure can capture this frame and outlive the scope
     * that created it, so frames live in env's value pool (reclaimed by
     * turi_env_free) -- eval_frame_free stays a no-op. */
    EvalFrame *f = (EvalFrame *)turi_val_alloc(env, sizeof(EvalFrame));
    f->bindings = NULL;
    f->parent   = parent;
    f->tyvars   = NULL;
    f->dicts    = NULL;
    return f;
}

/* Resolve a typeclass to its runtime dictionary through the frame chain. */
static struct TypeClassInstance *frame_lookup_dict(EvalFrame *f,
                                                   const struct TypeClass *tc) {
    for (EvalFrame *cur = f; cur; cur = cur->parent)
        for (DictBind *db = cur->dicts; db; db = db->next)
            if (db->tc == tc) return db->inst;
    return NULL;
}

/* Resolve a typeclass dictionary for a SPECIFIC constraint tyvar.  Exact
 * (class, tyvar-name) match first, so `[^Show K ^Show V]` dispatches K's
 * dictionary for a K-directed method and V's for a V-directed one; falls back
 * to the class-only walk (which also serves unkeyed dict-clone binds). */
static struct TypeClassInstance *frame_lookup_dict_tyvar(
        EvalFrame *f, const struct TypeClass *tc, const char *tyvar) {
    if (tyvar)
        for (EvalFrame *cur = f; cur; cur = cur->parent)
            for (DictBind *db = cur->dicts; db; db = db->next)
                if (db->tc == tc && db->tyvar &&
                    (db->tyvar == tyvar || strcmp(db->tyvar, tyvar) == 0))
                    return db->inst;
    return frame_lookup_dict(f, tc);
}

/* Resolve a tyvar name to its concrete type through the frame chain. */
static bool frame_lookup_tyvar(EvalFrame *f, const char *name, Type *out) {
    for (EvalFrame *cur = f; cur; cur = cur->parent)
        for (TyvarBind *tb = cur->tyvars; tb; tb = tb->next)
            if (tb->name == name || strcmp(tb->name, name) == 0) {
                *out = tb->type;
                return true;
            }
    return false;
}

static void eval_frame_free(EvalFrame *f) {
    /* Frames are intentionally not freed: closures may capture frame pointers
     * and outlive the scope that created them.  Worker processes are short-lived
     * (one fixture per fork), so leaking frames is acceptable. */
    (void)f;
}

static void frame_bind(TuriEnv *env, EvalFrame *f, const char *name, TuriValue value) {
    /* Escaping payload: bindings hang off a frame a closure may capture, so they
     * live in env's value pool (reclaimed by turi_env_free). */
    EvalBinding *b = (EvalBinding *)turi_val_alloc(env, sizeof(EvalBinding));
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
    const char *legacy = tur_legacy_form_hint(nm);
    if (legacy)
        return turi_errorf(
            "unknown function or operator '%s'\n%s", nm, legacy);
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
/* Explicit-stack entry (defined far below); eval_reset_boundary drives the
 * reset body through it so a lexical (shift ...) inside takes the work-stack
 * EX_SHIFT path (SR N3b) instead of a synchronous eval_abortive_shift. */
static TuriValue eval_drive(TuriEnv *env, EvalFrame *frame, const Expr *e);

/* -------------------------------------------------------------------------
 * Phase S4: Struct, throw, and defer runtime types
 * ---------------------------------------------------------------------- */

/* Full definition of TuriStruct (forward-declared in value.h).
 * Fields are stored in constructor order. */
struct TuriStruct {
    const char  *name;     /* struct name (for debugging) */
    uint32_t     n_fields;
    TuriValue   *fields;   /* heap-allocated array */
    const CtorDef *ctor;   /* CONV-S1: ADT constructor this value was built from
                            * (record ctors carry field names + a back-pointer to
                            * their AdtDef, incl. from_struct_lowering).  NULL for
                            * a plain make-struct value.  Lets the interpreter
                            * recover field names + struct-ness for a defstruct
                            * lowered to a single-variant record ADT. */
};

static TuriValue make_struct_val_def(TuriEnv *env, const char *name, uint32_t n, TuriValue *fields) {
    /* Escaping payload: the TuriStruct + its fields array are returned and may
     * be captured/stored, so they live in env's value pool (reclaimed by
     * turi_env_free), never individually freed. */
    TuriStruct *s = (TuriStruct *)turi_val_alloc(env, sizeof(TuriStruct));
    s->name     = name;
    s->n_fields = n;
    s->ctor     = NULL;
    s->fields   = n ? (TuriValue *)turi_val_alloc(env, n * sizeof(TuriValue)) : NULL;
    for (uint32_t i = 0; i < n; i++) s->fields[i] = fields[i];
    return turi_struct_val(s);
}

/* Copy a BY-VALUE struct argument as it is bound to a parameter.
 *
 * Turmeric passes by value: the compiled backend hands a callee its own copy of
 * a `defstruct` value, so `(set! (.f p) v)` inside the callee mutates that copy
 * and the caller never sees it.  The interpreter stores a struct as a heap
 * `TuriStruct*` and used to bind the pointer straight through, which made the
 * same write visible to the caller -- one program printing 0 compiled and 3
 * interpreted.  See
 * docs/archive/struct-param-mutation-backend-divergence.md.
 *
 * Three kinds of value must NOT be copied, because for them sharing IS the
 * semantics rather than an artifact of the representation:
 *
 *   - an `rc<T>`, represented structurally as an `__rc` wrapper.  Reference
 *     counting is the whole point; copying the wrapper would fork the count.
 *   - a `:heap` struct, which is interior-mutable by declaration -- a callee's
 *     mutation IS meant to reach the caller.
 *   - anything that is not a TURI_STRUCT.  A `&Struct` borrow arrives as
 *     TURI_REF and is a genuine reference; leave it alone.
 *
 * The copy RECURSES through by-value struct fields, because that is what the
 * compiled backend does: an `Outer` holding an `Inner` by value is one flat C
 * struct, so copying the outer copies the inner, and
 * `(set! (.n (.inner p)) v)` is just as invisible to the caller as the
 * one-level write.  The recursion uses the same three stop conditions, which
 * is exactly the by-value/shared frontier -- an `rc<T>` field copies as a
 * shared handle, matching the compiled pointer copy.
 *
 * The recursion cannot run away.  A by-value struct cannot be recursive (it
 * would have infinite size), so anything self-referential reaches an rc or a
 * `:heap` field and stops -- stdlib's `Cons` and `Vec` are both `:heap`, so a
 * list or vector argument is not walked at all.  Depth is therefore the
 * by-value nesting depth of a declared type, which is small and finite. */
static TuriValue turi_copy_byvalue_struct_arg(TuriEnv *env, TuriValue v) {
    if (v.tag != TURI_STRUCT || !v.as_struct) return v;
    const TuriStruct *src = v.as_struct;
    if (src->name && strcmp(src->name, "__rc") == 0) return v;   /* rc: shared */
    if (src->ctor && src->ctor->adt && src->ctor->adt->is_heap) return v;
    TuriStruct *s = (TuriStruct *)turi_val_alloc(env, sizeof(TuriStruct));
    s->name     = src->name;
    s->n_fields = src->n_fields;
    s->ctor     = src->ctor;
    s->fields   = src->n_fields
                    ? (TuriValue *)turi_val_alloc(env, src->n_fields * sizeof(TuriValue))
                    : NULL;
    for (uint32_t i = 0; i < src->n_fields; i++)
        s->fields[i] = turi_copy_byvalue_struct_arg(env, src->fields[i]);
    return turi_struct_val(s);
}

/* collection-multiword-element-boxing (interpreter parity): a GENERIC content
 * comparator for two struct/ADT KEYS, the interpreter analogue of the compiled
 * runtime's tur_hamt_box_key_eq.  A multi-word struct Map key / Set element uses
 * a `MapKey` `mk-cmp` #?(:turi ...) branch that returns the ADDRESS of this
 * function (via the `struct-key-cmp` native), so it is a stampable
 * bool(int64,int64) C function pointer -- exactly like the primitive cstr/float
 * comparators.  Being a real C fn ptr (not a Turmeric closure) means it is
 * stamped on the HAMT root by the _eq_o path and RECOVERED by tur_hamt_keyeq, so
 * structural Eq[Map]/Eq[Set] over struct keys work in the interpreter, not just
 * assoc/get/member.  The two int64 args are the stored key carriers, which for a
 * struct key (mk-box :turi returns the struct itself) are TuriStruct pointers;
 * compare their field content recursively (mirroring the compiled byte compare
 * over the copied key bytes). */
static bool turi_key_content_eq(TuriValue x, TuriValue y) {
    if (x.tag != y.tag) return false;
    switch (x.tag) {
        case TURI_INT:    return x.as_int == y.as_int;
        case TURI_BOOL:   return x.as_bool == y.as_bool;
        case TURI_FLOAT:  return x.as_float == y.as_float;
        case TURI_NIL:    return true;
        case TURI_CSTR:
            return x.as_cstr == y.as_cstr ||
                   (x.as_cstr && y.as_cstr && strcmp(x.as_cstr, y.as_cstr) == 0);
        case TURI_STRUCT: {
            const TuriStruct *sa = x.as_struct, *sb = y.as_struct;
            if (sa == sb) return true;
            if (!sa || !sb || sa->n_fields != sb->n_fields) return false;
            if (sa->name && sb->name && strcmp(sa->name, sb->name) != 0)
                return false;
            for (uint32_t i = 0; i < sa->n_fields; i++)
                if (!turi_key_content_eq(sa->fields[i], sb->fields[i]))
                    return false;
            return true;
        }
        default:          return x.as_int == y.as_int;   /* pointer identity */
    }
}

bool turi_struct_key_eq_c(int64_t a, int64_t b) {
    if (a == b) return true;
    if (!a || !b) return false;
    TuriValue x = turi_struct_val((TuriStruct *)(intptr_t)a);
    TuriValue y = turi_struct_val((TuriStruct *)(intptr_t)b);
    return turi_key_content_eq(x, y);
}

/* Companion generic content hash for a struct/ADT KEY (the interpreter analogue
 * of the compiled `tur_hamt_hash_xxh64(&p, sizeof p)`).  A `Hash` :turi branch
 * returns `(struct-hash p)` so the interpreter hash is uniform per struct -- no
 * per-field expression.  A splitmix64-style fold over each field's content; the
 * exact value need only be deterministic within an interpreter run (hashes never
 * cross the compiled/interpreted boundary, and equal-content keys hash equally
 * because equal fields fold identically). */
static uint64_t turi_key_content_hash(TuriValue v) {
    uint64_t h;
    switch (v.tag) {
        case TURI_INT:   h = (uint64_t)v.as_int; break;
        case TURI_BOOL:  h = v.as_bool ? 1u : 0u; break;
        case TURI_FLOAT: { union { double d; uint64_t u; } u; u.d = v.as_float; h = u.u; break; }
        case TURI_NIL:   h = 0; break;
        case TURI_CSTR:  h = v.as_cstr ? tur_hamt_hash_str(v.as_cstr) : 0; break;
        case TURI_STRUCT: {
            const TuriStruct *s = v.as_struct;
            h = s && s->name ? tur_hamt_hash_str(s->name) : 0;
            if (s) for (uint32_t i = 0; i < s->n_fields; i++) {
                h ^= turi_key_content_hash(s->fields[i]) + 0x9e3779b97f4a7c15ULL +
                     (h << 6) + (h >> 2);
            }
            break;
        }
        default:         h = (uint64_t)v.as_int; break;
    }
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL; h ^= h >> 31;
    return h;
}

int64_t turi_struct_hash_c(TuriValue v) {
    return (int64_t)turi_key_content_hash(v);
}

/* M2b: build a zero-valued TuriValue for a type T -- the interpreter's analogue
 * of the compiled `(T){0}` compound literal.  Scalars, pointers and type-param
 * fields collapse to the int64 carrier 0; floats to 0.0; and a struct T to a
 * TuriStruct with every field recursively zeroed, so `(.field (default-of T))`
 * reads a real zero instead of failing with "field access on null carrier". */
static TuriValue turi_default_of(TuriEnv *env, const Type *t) {
    if (!t) return turi_int(0);
    switch (t->kind) {
        case TY_FLOAT32: case TY_FLOAT64: return turi_float(0.0);
        case TY_BOOL: return turi_bool(false);
        case TY_NIL:  return turi_nil();
        default: break;
    }
    return turi_int(0);
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

/* Recursively run rc drop-glue on a value being released.  An rc value is a
 * "__rc" wrapper { counter-ptr, inner }: decrement its strong count and, on
 * reaching zero, drop its inner.  A plain struct/ADT being dropped by value
 * releases each of its fields (so a nested rc<T> field, e.g. Box's payload, gets
 * decremented) -- mirroring the compiled drop-glue that the by-value ADT path
 * now emits (CONV-S1 slice 2).  Other values have no drop glue. */
static void turi_rc_drop_value(TuriValue v) {
    if (v.tag != TURI_STRUCT || !v.as_struct) return;
    TuriStruct *s = v.as_struct;
    if (s->name && strcmp(s->name, "__rc") == 0 && s->n_fields >= 2) {
        int64_t *cnt = (int64_t *)(intptr_t)s->fields[0].as_int;
        if (cnt && *cnt > 0) {
            (*cnt)--;
            if (*cnt == 0) turi_rc_drop_value(s->fields[1]);
        }
        return;
    }
    for (uint32_t i = 0; i < s->n_fields; i++)
        turi_rc_drop_value(s->fields[i]);
}

/* CONV-S1: true when a TURI_STRUCT value should be observed as a "struct" at the
 * surface -- a defstruct that lowered to a single-variant record ADT (its
 * CtorDef's parent AdtDef has from_struct_lowering set).  Used by type-of / cast
 * so the ADT lowering of a defstruct stays invisible, matching the compiled
 * __tur_any_type_name. */
static bool turi_struct_is_struct_like(TuriValue v) {
    if (v.tag != TURI_STRUCT || !v.as_struct) return false;
    const CtorDef *cd = v.as_struct->ctor;
    return cd && cd->adt && cd->adt->from_struct_lowering;
}

/* type-of-cast-kind-granularity: the interpreter's counterpart of the compiled
 * per-monomorph `any` box id.  The compiled tag names the ADT/struct now, so
 * turi answers with the same name rather than "struct"/"adt" for everything.
 * An ADT value reports its ADT's name (a `(Circle 5)` is a "Shape"), not the
 * constructor's; a struct-lowered record reports its own. */
static const char *turi_any_named_type(TuriValue v) {
    if (v.tag != TURI_STRUCT || !v.as_struct) return NULL;
    if (!turi_struct_is_struct_like(v) && v.as_struct->ctor &&
        v.as_struct->ctor->adt && v.as_struct->ctor->adt->name)
        return v.as_struct->ctor->adt->name;
    return v.as_struct->name;
}

TuriValue turi_make_struct(TuriEnv *env, const char *name, TuriValue *fields, uint32_t n) {
    return make_struct_val_def(env, name, n, fields);
}

static TuriValue make_struct_val(TuriEnv *env, const char *name, uint32_t n, TuriValue *fields) {
    return make_struct_val_def(env, name, n, fields);
}

/* -------------------------------------------------------------------------
 * jit-ffi-c2mir-plan F3: (call-ptr ...) evaluation
 * ---------------------------------------------------------------------- */

/* F4 struct-by-value.  The compiled path passes a record by naming its type
 * (a defstruct already emits as the exact by-value C struct), but turi holds
 * a TuriStruct -- a boxed array of tagged TuriValues -- so it has to build
 * the C bytes itself.  The layout comes from the shared engine in
 * jit_ffi_hook.c, which is also what renders the thunk's struct declaration,
 * so both sides are computing offsets from one description.
 *
 * The per-field TYPES come from the record's own CtorFields rather than from
 * the sig, which carries layout only -- that is what lets a :bool field read
 * back as a boolean and a :cstr field as a string instead of both collapsing
 * to an integer. */

/* How one record field sits in the emitted C aggregate.  Must agree with
 * codegen's adt_field_is_inline_byval -- that predicate is what decides the
 * emitted layout, and a sig that disagrees with it describes a struct the
 * callee does not have (the exact miscall F4 exists to prevent). */
typedef enum {
    AGGF_SCALAR,       /* a scalar member, or an int64 carrier (boxed /
                        * :heap-pointer / drop-glue field) -- 8 bytes either
                        * way, member_code_for_kind(kind) describes it */
    AGGF_NESTED,       /* a by-value record inlined as a nested C struct */
    AGGF_UNSUPPORTED,  /* inlined in the emitted C, but the interpreter
                        * cannot render its layout (a TY_APP monomorph field
                        * needs per-application substitution) -- refuse
                        * rather than mis-describe */
} AggFieldClass;

static AggFieldClass agg_field_class(const CtorField *f,
                                     const AdtDef **out_def) {
    if (adt_field_is_inline_byval(f)) {
        if (f->full_type->kind == TY_ADT) {
            if (out_def) *out_def = f->full_type->as.adt_.def;
            return AGGF_NESTED;
        }
        return AGGF_UNSUPPORTED;
    }
    return AGGF_SCALAR;
}

/* Number of sig bytes an aggregate for `def` needs, including braces. */
static size_t agg_sig_len(const AdtDef *def) {
    const CtorDef *ct = def->ctors[0];
    size_t n = 2;
    for (uint32_t i = 0; i < ct->n_fields; i++) {
        const AdtDef *in = NULL;
        n += (agg_field_class(&ct->fields[i], &in) == AGGF_NESTED)
                 ? agg_sig_len(in)
                 : 1;
    }
    return n;
}

/* Render `{...}` for a record ADT into buf (which must hold agg_sig_len
 * bytes).  A nested by-value record field renders as its own inline
 * `{...}`, matching the layout codegen inlines.  Returns the number of
 * bytes written, or 0 if any field has no by-value member representation
 * the interpreter can describe. */
static size_t agg_sig_render(const AdtDef *def, char *buf) {
    const CtorDef *ct = def->ctors[0];
    size_t pos = 0;
    buf[pos++] = '{';
    for (uint32_t i = 0; i < ct->n_fields; i++) {
        const AdtDef *in = NULL;
        switch (agg_field_class(&ct->fields[i], &in)) {
            case AGGF_NESTED: {
                size_t w = agg_sig_render(in, buf + pos);
                if (!w) return 0;
                pos += w;
                break;
            }
            case AGGF_SCALAR: {
                char c = tur_jit_ffi_member_code_for_kind(ct->fields[i].kind);
                if (!c) return 0;
                buf[pos++] = c;
                break;
            }
            default:
                return 0;
        }
    }
    buf[pos++] = '}';
    return pos;
}

/* Store one TuriValue into `base + off` as member code `code`. */
static void agg_store_member(void *base, size_t off, char code, TuriValue v) {
    unsigned char *p = (unsigned char *)base + off;
    int64_t iv = 0;
    double  dv = 0.0;
    switch (v.tag) {
        case TURI_INT:   iv = v.as_int;  dv = (double)v.as_int; break;
        case TURI_BOOL:  iv = v.as_bool ? 1 : 0; dv = (double)iv; break;
        case TURI_FLOAT: dv = v.as_float; iv = (int64_t)v.as_float; break;
        case TURI_CSTR:  iv = (int64_t)(intptr_t)v.as_cstr; break;
        default:         iv = 0; break;   /* TURI_NIL and friends -> zero */
    }
    switch (code) {
        case 'b': { int8_t  x = (int8_t)iv;  memcpy(p, &x, sizeof x); break; }
        case 'h': { int16_t x = (int16_t)iv; memcpy(p, &x, sizeof x); break; }
        case 'w': { int32_t x = (int32_t)iv; memcpy(p, &x, sizeof x); break; }
        case 'q': { int64_t x = iv;          memcpy(p, &x, sizeof x); break; }
        case 'p': { void   *x = (void *)(intptr_t)iv;
                    memcpy(p, &x, sizeof x); break; }
        case 'F': { float   x = (float)dv;   memcpy(p, &x, sizeof x); break; }
        case 'f': { double  x = dv;          memcpy(p, &x, sizeof x); break; }
        default:  break;
    }
}

/* Read `base + off` back as a TuriValue, typed by the FIELD's declared kind
 * (the sig code only says how many bytes to read and whether they are FP). */
static TuriValue agg_load_member(const void *base, size_t off, char code,
                                 TypeKind field_kind) {
    const unsigned char *p = (const unsigned char *)base + off;
    switch (code) {
        case 'F': { float  x; memcpy(&x, p, sizeof x); return turi_float(x); }
        case 'f': { double x; memcpy(&x, p, sizeof x); return turi_float(x); }
        case 'p': {
            void *x; memcpy(&x, p, sizeof x);
            if (field_kind == TY_CSTR)
                return x ? turi_cstr((const char *)x) : turi_nil();
            return turi_int((int64_t)(intptr_t)x);
        }
        default: break;
    }
    /* Integer widths: sign- or zero-extend by the DECLARED field type, so a
     * :uint32 0xFFFFFFFF reads back as 4294967295 rather than -1. */
    bool is_unsigned = (field_kind == TY_UINT8  || field_kind == TY_UINT16 ||
                        field_kind == TY_UINT32 || field_kind == TY_UINT64);
    int64_t out = 0;
    switch (code) {
        case 'b': if (is_unsigned) { uint8_t x; memcpy(&x, p, sizeof x);
                                     out = x; }
                  else             { int8_t  x; memcpy(&x, p, sizeof x);
                                     out = x; }
                  break;
        case 'h': if (is_unsigned) { uint16_t x; memcpy(&x, p, sizeof x);
                                     out = x; }
                  else             { int16_t  x; memcpy(&x, p, sizeof x);
                                     out = x; }
                  break;
        case 'w': if (is_unsigned) { uint32_t x; memcpy(&x, p, sizeof x);
                                     out = x; }
                  else             { int32_t  x; memcpy(&x, p, sizeof x);
                                     out = x; }
                  break;
        case 'q': { int64_t x; memcpy(&x, p, sizeof x); out = x; break; }
        default:  break;
    }
    if (field_kind == TY_BOOL) return turi_bool(out != 0);
    return turi_int(out);
}

/* Flatten `v` (a record value of type `def`) into leaf TuriValues in
 * declaration order, recursing into nested by-value record fields -- the
 * same flattening tur_jit_ffi_struct_layout applies to the sig, so leaf i
 * here lands at offs[i]/codes[i] there.  Returns false on a shape mismatch
 * (not a record, wrong field count, or a nested field that is not the
 * record value its slot declares). */
static bool agg_collect_leaves(const AdtDef *def, TuriValue v,
                               TuriValue *out, int max, int *n) {
    if (v.tag != TURI_STRUCT || !v.as_struct) return false;
    const CtorDef *ct = def->ctors[0];
    if (v.as_struct->n_fields != ct->n_fields) return false;
    for (uint32_t i = 0; i < ct->n_fields; i++) {
        const AdtDef *in = NULL;
        if (agg_field_class(&ct->fields[i], &in) == AGGF_NESTED) {
            if (!agg_collect_leaves(in, v.as_struct->fields[i], out, max, n))
                return false;
        } else {
            if (*n >= max) return false;
            out[(*n)++] = v.as_struct->fields[i];
        }
    }
    return true;
}

/* Rebuild a record value of type `def` from the C bytes at `base`, reading
 * leaves at offs[*cur]/codes[*cur] onward (the flattened order the layout
 * engine produced) and reconstructing nested records recursively.  Advances
 * *cur past the leaves consumed. */
static TuriValue agg_build_value(TuriEnv *env, const AdtDef *def,
                                 const void *base, const size_t *offs,
                                 const char *codes, int nleaf, int *cur) {
    const CtorDef *ct = def->ctors[0];
    TuriValue fields[64];
    if (ct->n_fields > 64)
        return turi_error("call-ptr: aggregate return has too many fields");
    for (uint32_t i = 0; i < ct->n_fields; i++) {
        const AdtDef *in = NULL;
        if (agg_field_class(&ct->fields[i], &in) == AGGF_NESTED) {
            fields[i] = agg_build_value(env, in, base, offs, codes,
                                        nleaf, cur);
            if (turi_is_error(fields[i])) return fields[i];
        } else {
            if (*cur >= nleaf)
                return turi_error("call-ptr: aggregate return layout is "
                                  "shorter than its record declares");
            fields[i] = agg_load_member(base, offs[*cur], codes[*cur],
                                        ct->fields[i].kind);
            (*cur)++;
        }
    }
    TuriValue r = make_struct_val(env, ct->name, ct->n_fields, fields);
    /* Carry the ctor so field access and `type-of` see a struct, the same
     * thing adt_ctor_native does for a value built in turi. */
    if (r.tag == TURI_STRUCT && r.as_struct)
        r.as_struct->ctor = ct;
    return r;
}

/* Bridge for tur_ffi_cb_dispatch (ffi_thunk.c): rebuild a record TuriValue
 * of type `def` from the C bytes of the aggregate whose sig text begins at
 * `sig` (points at '{').  An inbound callback argument arrives as raw
 * struct bytes; this is the unpacking direction of the F4 marshaller. */
TuriValue tur_eval_agg_from_bytes(TuriEnv *env, const AdtDef *def,
                                  const char *sig, const void *bytes) {
    size_t offs[64];
    char   codes[64];
    int    nleaf = 0;
    if (!tur_jit_ffi_struct_layout(sig, NULL, NULL, offs, codes, 64, &nleaf))
        return turi_error("callback: unrepresentable aggregate layout");
    int cur = 0;
    TuriValue r = agg_build_value(env, def, bytes, offs, codes, nleaf, &cur);
    if (!turi_is_error(r) && cur != nleaf)
        return turi_error("callback: aggregate layout is longer than its "
                          "record declares");
    return r;
}

/* Bridge for tur_ffi_cb_dispatch: pack a record TuriValue into the byte
 * buffer a callback's aggregate return is stored through.  Returns false
 * on a shape mismatch (the buffer is left zeroed by the caller). */
bool tur_eval_agg_to_bytes(const AdtDef *def, const char *sig, TuriValue v,
                           void *bytes) {
    size_t offs[64];
    char   codes[64];
    int    nleaf = 0;
    if (!tur_jit_ffi_struct_layout(sig, NULL, NULL, offs, codes, 64, &nleaf))
        return false;
    TuriValue leaves[64];
    int       nl = 0;
    if (!agg_collect_leaves(def, v, leaves, 64, &nl) || nl != nleaf)
        return false;
    for (int i = 0; i < nleaf; i++)
        agg_store_member(bytes, offs[i], codes[i], leaves[i]);
    return true;
}

/* jit-ffi-c2mir-plan F5: `(callback-ptr f [sig])` under the interpreter.
 * Builds a process-lifetime context pinning the Turmeric function and asks
 * the provider for a C function pointer whose generated body calls
 * tur_ffi_cb_dispatch with that context's address baked in.  Gated on
 * TURI_CAP_FFI like the rest of the FFI surface. */
static TuriValue eval_callback_ptr(TuriEnv *env, EvalFrame *frame,
                                   const Expr *e) {
    const CallPtrSig *ps = e->as.call_.ptr_sig;

    if (!(env->caps & TURI_CAP_FFI))
        return turi_error(
            "eval: callback-ptr not allowed in sandboxed environment");
    const TurJitFfiProvider *jp = tur_jit_ffi_provider();
    if (!jp || !jp->callback_for)
        return turi_error(
            "callback-ptr under --interpret requires a JIT-enabled build "
            "(-DTUR_JIT=ON); the compiled path (tur build / tur run) "
            "supports it in every build");

    TuriValue fv = eval_expr(env, frame, e->as.call_.fn_expr);
    if (turi_is_error(fv)) return fv;

    uint32_t n = ps->n_params;
    size_t *arg_at = (size_t *)calloc(n ? n : 1, sizeof(size_t));
    char   *sig    = arg_at ? agg_sig_build(ps->return_type, ps->param_types,
                                            n, arg_at)
                            : NULL;
    if (!sig) {
        free(arg_at);
        return turi_error("callback-ptr: signature is not representable");
    }

    /* Per-arg classes for the dispatch: '{' marks an aggregate slot, whose
     * bytes arrive through the sv channel and are rebuilt into a record via
     * the sig text at arg_at[k]. */
    char inl_cls[64];
    char *classes = (n <= sizeof inl_cls) ? inl_cls : (char *)malloc(n);
    const AdtDef *inl_defs[64];
    const AdtDef **arg_defs =
        (n <= 64) ? inl_defs
                  : (const AdtDef **)malloc(n * sizeof(const AdtDef *));
    if (!classes || !arg_defs) {
        free(sig); free(arg_at);
        if (classes != inl_cls) free(classes);
        if (arg_defs != inl_defs) free((void *)arg_defs);
        return turi_error("callback-ptr: out of memory");
    }
    bool any_agg = false;
    for (uint32_t k = 0; k < n; k++) {
        arg_defs[k] = extern_slot_agg_def(ps->param_types[k]);
        classes[k]  = arg_defs[k]
                          ? '{'
                          : tur_jit_ffi_class_for_kind(ps->param_types[k].kind,
                                                       0);
        if (arg_defs[k]) any_agg = true;
    }
    const AdtDef *ret_def = extern_slot_agg_def(ps->return_type);
    char ret_class = ret_def ? '{'
                             : tur_jit_ffi_class_for_kind(ps->return_type.kind,
                                                          1);
    if (ret_def) any_agg = true;

    /* The context is never freed: its ADDRESS is compiled into the callback
     * body, and a C library holding that pointer has no way to tell us it is
     * done.  Same policy as turi's closures. */
    TurFfiCbCtx *ctx = tur_ffi_cb_ctx_new(env, fv, ret_class, classes, n);
    if (ctx && any_agg &&
        tur_ffi_cb_ctx_set_agg(ctx, sig, arg_at, arg_defs, ret_def) != 0)
        ctx = NULL;
    if (classes != inl_cls) free(classes);
    if (arg_defs != inl_defs) free((void *)arg_defs);
    if (!ctx) {
        free(sig); free(arg_at);
        return turi_error("callback-ptr: out of memory");
    }

    char errbuf[256];
    void *fn = jp->callback_for(sig, ctx, errbuf, sizeof errbuf);
    if (!any_agg) { free(sig); free(arg_at); }  /* set_agg took ownership */
    if (!fn) return turi_errorf("callback-ptr: %s", errbuf);
    return turi_int((int64_t)(intptr_t)fn);
}

/* Evaluate an EX_CALL carrying a ptr_sig: an indirect call through a raw C
 * address with the signature stated at the site.  Routes through the c2mir
 * thunk provider; a non-JIT interpreter build reports a clean diagnostic,
 * never nil.  Gated on TURI_CAP_FFI like dlopen/dlsym. */
static TuriValue eval_call_ptr(TuriEnv *env, EvalFrame *frame,
                               const Expr *e) {
    const CallPtrSig *ps = e->as.call_.ptr_sig;

    if (!(env->caps & TURI_CAP_FFI))
        return turi_error(
            "eval: call-ptr not allowed in sandboxed environment");
    const TurJitFfiProvider *jp = tur_jit_ffi_provider();
    if (!jp)
        return turi_error(
            "call-ptr under --interpret requires a JIT-enabled build "
            "(-DTUR_JIT=ON); the compiled path (tur build / tur run) "
            "supports it in every build");

    TuriValue pv = eval_expr(env, frame, e->as.call_.fn_expr);
    if (turi_is_error(pv)) return pv;
    /* dlsym results ride the int64 carrier in turi. */
    if (pv.tag != TURI_INT || pv.as_int == 0)
        return turi_error("call-ptr: pointer is nil or not an address");
    void *fn = (void *)(intptr_t)pv.as_int;

    uint32_t n = e->as.call_.n_args;
    TuriValue result;

    /* Position-indexed marshalling buffers, one slot per parameter: arg k
     * rides iv[k], fv[k] or sv[k] by its class.  sv[k]'s bytes are owned by
     * agg_bufs[k] and freed on the way out; the return aggregate's buffer is
     * out_s_buf. */
    int64_t *i_vals    = NULL;
    double  *f_vals    = NULL;
    void   **s_vals    = NULL;
    void   **agg_bufs  = NULL;
    char    *sig       = NULL;
    void    *out_s_buf = NULL;
    /* Offset into `sig` where each aggregate parameter's '{' sits, so the
     * pack step can read the layout back out of the sig it just wrote. */
    size_t  *agg_at    = NULL;

    /* Size the sig: one byte per scalar slot, `{fields}` per aggregate. */
    size_t sig_cap = 2 + 1;   /* ret + ':' + NUL, ret widened below */
    if (ps->return_type.kind == TY_ADT && ps->return_type.as.adt_.def)
        sig_cap += agg_sig_len(ps->return_type.as.adt_.def) - 1;
    for (uint32_t k = 0; k < n; k++) {
        if (ps->param_types[k].kind == TY_ADT &&
            ps->param_types[k].as.adt_.def)
            sig_cap += agg_sig_len(ps->param_types[k].as.adt_.def);
        else
            sig_cap += 1;
    }

    i_vals   = (int64_t *)calloc(n ? n : 1, sizeof(int64_t));
    f_vals   = (double  *)calloc(n ? n : 1, sizeof(double));
    s_vals   = (void   **)calloc(n ? n : 1, sizeof(void *));
    agg_bufs = (void   **)calloc(n ? n : 1, sizeof(void *));
    agg_at   = (size_t  *)calloc(n ? n : 1, sizeof(size_t));
    sig      = (char    *)malloc(sig_cap);
    if (!i_vals || !f_vals || !s_vals || !agg_bufs || !agg_at || !sig) {
        result = turi_error("call-ptr: out of memory marshalling call");
        goto cleanup;
    }

    size_t sp = 0;
    if (ps->return_type.kind == TY_ADT && ps->return_type.as.adt_.def) {
        size_t w = agg_sig_render(ps->return_type.as.adt_.def, sig);
        if (!w) {
            result = turi_error("call-ptr: return record has a field with no "
                                "by-value C member type");
            goto cleanup;
        }
        sp = w;
    } else {
        sig[sp++] = tur_jit_ffi_class_for_kind(ps->return_type.kind, 1);
    }
    sig[sp++] = ':';

    for (uint32_t k = 0; k < n; k++) {
        const AdtDef *adef = (ps->param_types[k].kind == TY_ADT)
                                 ? ps->param_types[k].as.adt_.def : NULL;
        char cls = adef ? '{'
                        : tur_jit_ffi_class_for_kind(ps->param_types[k].kind, 0);
        agg_at[k] = sp;
        if (adef) {
            size_t w = agg_sig_render(adef, sig + sp);
            if (!w) {
                result = turi_errorf("call-ptr: arg %u's record has a field "
                                     "with no by-value C member type",
                                     (unsigned)k);
                goto cleanup;
            }
            sp += w;
        } else {
            sig[sp++] = cls;
        }

        TuriValue av = eval_expr(env, frame, e->as.call_.args[k]);
        if (turi_is_error(av)) { result = av; goto cleanup; }

        if (cls == '{') {
            if (av.tag != TURI_STRUCT || !av.as_struct) {
                result = turi_errorf("call-ptr: arg %u is not a record value",
                                     (unsigned)k);
                goto cleanup;
            }
            size_t size = 0, align = 1, offs[64];
            char   codes[64];
            int    nleaf = 0;
            if (!tur_jit_ffi_struct_layout(sig + agg_at[k], &size, &align,
                                           offs, codes, 64, &nleaf)) {
                result = turi_errorf("call-ptr: arg %u has an unrepresentable "
                                     "aggregate layout", (unsigned)k);
                goto cleanup;
            }
            TuriValue leaves[64];
            int       n_leaves = 0;
            if (!agg_collect_leaves(adef, av, leaves, 64, &n_leaves) ||
                n_leaves != nleaf) {
                result = turi_errorf("call-ptr: arg %u does not match the "
                                     "shape '%s' declares", (unsigned)k,
                                     adef->name ? adef->name : "?");
                goto cleanup;
            }
            /* calloc, not malloc: tail padding is passed too, and handing
             * the callee uninitialized padding bytes is exactly the kind of
             * nondeterminism that makes an FFI bug unreproducible. */
            void *bytes = calloc(1, size ? size : 1);
            if (!bytes) {
                result = turi_error("call-ptr: out of memory");
                goto cleanup;
            }
            agg_bufs[k] = bytes;
            s_vals[k]   = bytes;
            for (int i = 0; i < nleaf; i++)
                agg_store_member(bytes, offs[i], codes[i], leaves[i]);
        } else if (tur_jit_ffi_class_is_int(cls)) {
            switch (av.tag) {
                case TURI_INT:  i_vals[k] = av.as_int; break;
                case TURI_BOOL: i_vals[k] = av.as_bool ? 1 : 0; break;
                case TURI_CSTR: i_vals[k] = (int64_t)(intptr_t)av.as_cstr; break;
                case TURI_NIL:  i_vals[k] = 0; break;
                default:
                    result = turi_errorf(
                        "call-ptr: arg %u is not an int-class value",
                        (unsigned)k);
                    goto cleanup;
            }
        } else {   /* 'f' / 'F' */
            if (av.tag == TURI_FLOAT)      f_vals[k] = av.as_float;
            else if (av.tag == TURI_INT)   f_vals[k] = (double)av.as_int;
            else {
                result = turi_errorf(
                    "call-ptr: arg %u is not a float-class value",
                    (unsigned)k);
                goto cleanup;
            }
        }
    }
    sig[sp] = '\0';

    {
        char errbuf[256];
        TurJitFfiThunkFn jt = jp->thunk_for(sig, errbuf, sizeof errbuf);
        if (!jt) {
            result = turi_errorf("call-ptr: %s", errbuf);
            goto cleanup;
        }
        int64_t out_i = 0;
        double  out_f = 0.0;

        const AdtDef *rdef = (ps->return_type.kind == TY_ADT)
                                 ? ps->return_type.as.adt_.def : NULL;
        size_t roffs[64];
        char   rcodes[64];
        int    rleaf = 0;
        if (rdef) {
            size_t rsize = 0, ralign = 1;
            if (!tur_jit_ffi_struct_layout(sig, &rsize, &ralign, roffs,
                                           rcodes, 64, &rleaf)) {
                result = turi_error("call-ptr: return record has an "
                                    "unrepresentable aggregate layout");
                goto cleanup;
            }
            out_s_buf = calloc(1, rsize ? rsize : 1);
            if (!out_s_buf) {
                result = turi_error("call-ptr: out of memory");
                goto cleanup;
            }
        }

        jt(fn, i_vals, f_vals, s_vals, &out_i, &out_f, out_s_buf);

        if (rdef) {
            int cur = 0;
            result = agg_build_value(env, rdef, out_s_buf, roffs, rcodes,
                                     rleaf, &cur);
            if (!turi_is_error(result) && cur != rleaf)
                result = turi_error("call-ptr: aggregate return layout is "
                                    "longer than its record declares");
            goto cleanup;
        }

        switch (ps->return_type.kind) {
            case TY_NIL:     result = turi_nil(); break;
            case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
                result = turi_float(out_f); break;
            case TY_BOOL:    result = turi_bool(out_i != 0); break;
            case TY_CSTR:
                result = out_i ? turi_cstr((const char *)(intptr_t)out_i)
                               : turi_nil();
                break;
            default:         result = turi_int(out_i); break;
        }
    }

cleanup:
    if (agg_bufs)
        for (uint32_t k = 0; k < n; k++) free(agg_bufs[k]);
    free(agg_bufs);
    free(agg_at);
    free(out_s_buf);
    free(s_vals);
    free(sig);
    free(i_vals);
    free(f_vals);
    return result;
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
    CtorDef *ctor = (CtorDef *)ud;
    TuriValue v = make_struct_val(env, ctor->name, n, args);
    /* Remember the constructor so the interpreter can recover record field names
     * and the from_struct_lowering flag (defstruct lowered to a record ADT). */
    if (v.tag == TURI_STRUCT && v.as_struct) v.as_struct->ctor = ctor;
    return v;
}

/* DEPR-D0: TuriThrow / make_throw_val / turi_native_throw deleted.  The
 * env->throwing / env->throw_value scratch slots remain wired through the
 * interpreter as never-set signals; no path produces a TURI_THROW value
 * after R0+D0.  See docs/archive/history/throw-deprecation-plan.md. */

/* Defer item: body expression + snapshot frame of captured values.
 *
 * A DeferItem with `body == NULL` is a *scope-boundary marker*: a sentinel
 * pushed onto the chain at each defer-scope entry (a `let`), so the firing
 * helpers can mirror the compiled `tur_frame_fire_chain` two-level ordering --
 * same-scope LIFO, and, on an early exit (return / throw / panic), scopes
 * outer-first.  Markers carry no body and are never evaluated; they are simply
 * skipped (LIFO firing) or used to delimit segments (by-scope firing) and then
 * freed.  See docs/archive/history/turi-tail-scope-defers-fire-fifo-not-lifo.md. */
typedef struct DeferItem {
    Expr             *body;       /* NULL => scope-boundary marker (not fired) */
    EvalFrame        *snapshot;   /* captured variable values at defer-call time */
    struct DeferItem *next;
} DeferItem;

/* Push a scope-boundary marker onto the defer chain.  Returns the chain head
 * that existed *before* the marker -- i.e. the parent scope's defers -- which a
 * non-tail scope stores as its fire-to mark (so its normal-exit firing consumes
 * the scope's own defers plus this marker, leaving the parent chain intact). */
static DeferItem *defer_push_scope_marker(TuriEnv *env) {
    DeferItem *parent = (DeferItem *)env->defer_stack;
    DeferItem *m = (DeferItem *)calloc(1, sizeof(DeferItem));
    m->body     = NULL;
    m->snapshot = NULL;
    m->next     = parent;
    env->defer_stack = m;
    return parent;
}

/* C1: true while defer bodies are firing during a *panic* unwind.  A panic
 * raised in a defer at that point is a double panic -- but the defer body only
 * runs because fire_defers clears env->panicking (eval short-circuits on it),
 * so the plain `env->panicking` double-panic guard would miss it.  This flag
 * preserves the detection: turi_runtime_panic / EX_PANIC_WITH treat a panic
 * raised while it is set as a double panic (matches the compiled catch_unwind +
 * Drop-panic-during-unwind abort). */
static _Thread_local bool g_firing_panic_defer;

/* Execute all defers pushed above mark (LIFO / head-first) and free them.
 * Scope-boundary markers are skipped and freed.  Head-first across nested
 * scopes yields the compiled *normal-exit* ordering: innermost scope first,
 * same-scope LIFO.  Errors inside defers are silently discarded. */
static void fire_defers_to_mark(TuriEnv *env, DeferItem *mark,
                                  EvalFrame *fallback_frame) {
    bool saved_throwing  = env->throwing;
    TuriValue saved_tv   = env->throw_value;
    bool saved_returning = env->returning;
    TuriValue saved_rv   = env->return_value;
    bool saved_panicking = env->panicking;   /* C1: defers fire during unwind */
    bool prev_fpd        = g_firing_panic_defer;

    while (env->defer_stack != mark) {
        DeferItem *item = (DeferItem *)env->defer_stack;
        env->defer_stack = item->next;

        if (item->body == NULL) {   /* scope-boundary marker: skip */
            free(item);
            continue;
        }

        /* Reset signals so defer body runs cleanly.  C1: clear `panicking` too --
         * a defer fired mid-panic-unwind must actually execute (eval_expr/eval_apply
         * short-circuit while a panic signal is in flight).  Set g_firing_panic_defer
         * so a re-panic in the body is still caught as a double panic. */
        env->throwing  = false;
        env->returning = false;
        env->panicking = false;
        g_firing_panic_defer = saved_panicking;

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
    env->panicking    = saved_panicking;
    g_firing_panic_defer = prev_fpd;
}

/* Fire defers at an *early exit* (return / throw / panic) boundary, reversing
 * by SCOPE rather than by item.  Mirrors the compiled tur_frame_fire_chain
 * early-exit semantics: scopes fire outer-first, but within a single scope the
 * defers stay LIFO.  Scope-boundary markers (body == NULL) delimit the scopes;
 * the trailing run (after the last marker, down to `mark`) is the outermost
 * scope and fires first.
 *
 * A flat item-reversal -- the previous implementation -- collapsed both axes
 * into one FIFO walk, so multiple defers in a single (e.g. tail-position) scope
 * came out oldest-first instead of LIFO.  See
 * docs/archive/history/turi-tail-scope-defers-fire-fifo-not-lifo.md. */
static void fire_defers_to_mark_by_scope(TuriEnv *env, DeferItem *mark,
                                         EvalFrame *fallback_frame) {
    /* Collect [head .. mark) into an array, head-first (index 0 = newest). */
    size_t n = 0;
    for (DeferItem *it = (DeferItem *)env->defer_stack; it != mark; it = it->next) n++;
    if (n == 0) return;

    DeferItem **items = (DeferItem **)malloc(n * sizeof(DeferItem *));
    DeferItem *cur = (DeferItem *)env->defer_stack;
    for (size_t i = 0; i < n; i++) { items[i] = cur; cur = cur->next; }
    env->defer_stack = mark;

    /* Split into per-scope runs of real (non-marker) items.  A marker closes
     * the run above it; runs are recorded innermost-first (head-first scan). */
    size_t *run_start = (size_t *)malloc(n * sizeof(size_t));
    size_t *run_end   = (size_t *)malloc(n * sizeof(size_t));
    size_t  n_runs = 0, cur_start = 0;
    for (size_t k = 0; k < n; k++) {
        if (items[k]->body == NULL) {   /* marker: close current run, free it */
            if (k > cur_start) { run_start[n_runs] = cur_start; run_end[n_runs] = k; n_runs++; }
            cur_start = k + 1;
            free(items[k]);   /* markers are never fired */
        }
    }
    if (n > cur_start) { run_start[n_runs] = cur_start; run_end[n_runs] = n; n_runs++; }

    bool saved_throwing  = env->throwing;
    TuriValue saved_tv   = env->throw_value;
    bool saved_returning = env->returning;
    TuriValue saved_rv   = env->return_value;
    bool saved_panicking = env->panicking;   /* C1: defers fire during unwind */
    bool prev_fpd        = g_firing_panic_defer;

    /* Fire runs outer-first (last recorded = outermost); within a run keep
     * head-first order (LIFO within the scope). */
    for (size_t r = n_runs; r-- > 0; ) {
        for (size_t k = run_start[r]; k < run_end[r]; k++) {
            DeferItem *item = items[k];
            /* C1: clear `panicking` too so a defer fired mid-panic-unwind runs;
             * g_firing_panic_defer keeps a re-panic detectable as a double panic. */
            env->throwing  = false;
            env->returning = false;
            env->panicking = false;
            g_firing_panic_defer = saved_panicking;
            EvalFrame *dframe = item->snapshot;
            if (dframe) dframe->parent = fallback_frame;
            eval_expr(env, dframe, item->body);
            eval_frame_free(item->snapshot);
            free(item);
        }
    }

    free(run_start);
    free(run_end);
    free(items);

    env->throwing     = saved_throwing;
    env->throw_value  = saved_tv;
    env->returning    = saved_returning;
    env->return_value = saved_rv;
    env->panicking    = saved_panicking;
    g_firing_panic_defer = prev_fpd;
}

/* SR/C1: the set of *propagating* control signals.  A signal short-circuits the
 * ordinary `turi_is_error(v) || ...` guards throughout the evaluator so an
 * in-flight return / throw / abortive-shift-abort / panic unwinds the work-stack
 * (freeing frames, firing defers) up to the boundary that consumes it, instead
 * of one C frame continuing to run past it.  `panicking` joined this set in
 * Phase C1 (turi-c-scoped-forms-heap-bounding): a panic caught by a work-stack
 * DK_CATCH_UNWIND boundary now propagates as a signal rather than longjmp-ing,
 * so deeply-nested catch-unwind folds onto the heap. */
static inline bool env_signaled(const TuriEnv *env) {
    return env->returning || env->throwing || env->aborting || env->panicking;
}

/* Phase C1: catch-unwind boundary stack.  A panic finds the nearest boundary
 * here to decide HOW to unwind to it:
 *   - a work-stack (driver) DK_CATCH_UNWIND boundary  -> raise the `panicking`
 *     signal and let it propagate through the driver's frames (heap-bounded);
 *   - a setjmp boundary (eval_expr_impl's EX_CATCH_UNWIND / EX_CATCH_PANIC_OF,
 *     reached from a non-driver / black-boxed caller) -> longjmp to its jmp_buf,
 *     exactly as before.
 * Boundaries interleave (a setjmp catch-panic-of nested inside a driver
 * catch-unwind, or vice versa), so both kinds push onto this one stack and a
 * panic always targets the innermost. */
typedef struct TuriCatchBoundary {
    bool     is_driver;             /* true: DK_CATCH_UNWIND; false: setjmp pad */
    jmp_buf *jmp;                   /* setjmp landing pad (valid iff !is_driver) */
    /* Driver-path env state to restore when the boundary consumes a panic
     * (the setjmp path restores its own saved copies on the C stack). */
    void    *saved_handler_stack;
    void    *saved_defer_stack;
    const char *saved_module;
    bool     saved_no_unwind;
    struct TuriCatchBoundary *prev;
} TuriCatchBoundary;

static _Thread_local TuriCatchBoundary *g_catch_stack;

/* Fill env's in-flight panic payload for a plain (cstr-message) panic. */
static inline void panic_fill_cstr_payload(TuriEnv *env, const char *s) {
    strncpy(env->catch_panic_msg, s, sizeof(env->catch_panic_msg) - 1);
    env->catch_panic_msg[sizeof(env->catch_panic_msg) - 1] = '\0';
    /* TI5: a plain panic carries a cstr payload (the message), so
     * catch-panic-of :cstr matches it. */
    env->catch_panic_type  = TY_CSTR;
    env->catch_panic_value = turi_cstr(env->catch_panic_msg);
    env->catch_panic_file  = NULL;
    env->catch_panic_line  = 0;
}

/* Phase R2: shared interpreter panic entry point.  Mirrors the EX_PANIC eval
 * case so native functions (result-must, option-must, option-expect, ...) raise
 * a *catchable* panic -- recoverable by catch-unwind and carrying the standard
 * panic message format and double-panic guard -- instead of calling _exit(1)
 * directly (which bypassed both catch-unwind and the defer chain).
 *
 * Phase C1: this no longer necessarily "never returns".  When the nearest
 * catch-unwind boundary is a work-stack DK_CATCH_UNWIND, it sets env->panicking
 * (the propagating signal) and RETURNS to its caller, which short-circuits via
 * env_signaled and unwinds the driver work-stack to the boundary.  Every caller
 * already does `turi_runtime_panic(env, ...); return <ignored>;`, so returning
 * is safe -- the returned value is discarded while the signal is in flight.  For
 * a setjmp boundary (or no boundary) the old behaviour is preserved (longjmp /
 * fire-defers-and-exit). */
void turi_runtime_panic(TuriEnv *env, const char *msg) {
    const char *s = msg ? msg : "(no message)";
    if (env->panicking || g_firing_panic_defer) {
        /* Double panic: a defer (or a panic during unwinding) panicked again. */
        fprintf(stderr, "double panic: aborting\n");
        fflush(stderr);
        fflush(stdout);
        abort();
    }
    /* If a catch-unwind boundary is active, unwind to it. */
    TuriCatchBoundary *cb = g_catch_stack;
    if (cb && !env->in_no_unwind) {
        panic_fill_cstr_payload(env, s);
        env->panicking = true;
        if (cb->is_driver)
            return;                 /* raise signal; the driver unwinds to DK_CATCH_UNWIND */
        longjmp(*cb->jmp, 1);       /* setjmp boundary: unwind the C stack to it */
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
        fire_defers_to_mark_by_scope(env, NULL, NULL);
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

/* Build the Result the native ok/err helpers also build, in its Ok shape.
 *
 * turi-catch-unwind-aggregate-payload: this used to take a bare `int64_t` and
 * always build the 3-int box, which FLATTENS the payload -- exactly the trap
 * `native_ok` documents and avoids.  A struct / cstr / closure / float payload
 * lost its tag, so `(ok-val r)` handed back a TURI_INT and a downstream field
 * read or `println` saw the raw handle: `(catch-unwind (fn [] : Q ...))`
 * printed a pointer where the compiled path printed the field.  Same rule as
 * `native_ok` now: a heap-tagged payload becomes a make-struct Result whose
 * fields hold full TuriValues, everything else keeps the int64 box the
 * carrier-ABI fixtures depend on.  `result_field` reads both shapes, so every
 * accessor stays uniform. */
static TuriValue turi_ok_result_box(TuriEnv *env, TuriValue ok_val) {
    if (ok_val.tag == TURI_STRUCT || ok_val.tag == TURI_CSTR ||
        ok_val.tag == TURI_CLOSURE || ok_val.tag == TURI_FLOAT) {
        TuriValue fields[3] = { turi_bool(true), ok_val, turi_int(0) };
        return turi_make_struct(env, "Result", fields, 3);
    }
    /* Escaping payload: the box is returned as a Result carrier. */
    int64_t *box = (int64_t *)turi_val_alloc(env, 3 * sizeof(int64_t));
    box[0] = 1; box[1] = ok_val.as_int; box[2] = 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)box;
    return v;
}

/* Build the Err shape of the Result box, with err_val pointing at a heap
 * TuriPanicPayload snapshotted from the env's in-flight panic fields. */
static TuriValue turi_err_result_box(TuriEnv *env) {
    /* Escaping payload: the payload + box are returned as a Result carrier and
     * read back by the panic-payload-* accessors; pool-owned, never freed. */
    TuriPanicPayload *pp = (TuriPanicPayload *)turi_val_alloc(env, sizeof(TuriPanicPayload));
    pp->type_tag = env->catch_panic_type;
    pp->value    = env->catch_panic_value;
    pp->file     = env->catch_panic_file
                   ? turi_val_strdup(env, env->catch_panic_file) : NULL;
    pp->line     = env->catch_panic_line;
    int64_t *box = (int64_t *)turi_val_alloc(env, 3 * sizeof(int64_t));
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
    /* A fiber continuation is single-shot: its body ran to completion on this
     * one ucontext, so there is no state left to re-enter.  swapcontext'ing
     * into the finished fiber lands past the end of eval_body_thunk, whose
     * only recourse is abort() -- the interpreter dying with no message, which
     * at a REPL takes the session with it.  Refuse here instead, and say which
     * shapes do support multi-shot.
     *
     * A multi-shot resume ordinarily never reaches this function: a capturable
     * handle runs on the work-stack, where each resume clones the captured
     * slice (DK_RESUME).  This is the fallback path for a handle ws_capturable
     * rejected -- one whose body or clause reaches a perform through a form the
     * driver cannot descend.  See
     * docs/archive/turi-multishot-resume-in-while-aborts.md, where a `while`
     * was such a form. */
    if (cont->done)
        return turi_error("eval: resume: this continuation has already been "
                          "resumed and its body has finished. Multi-shot resume "
                          "needs the work-stack path, which this handler falls "
                          "outside of: its body or one of its clauses reaches "
                          "`perform` through a form the interpreter cannot "
                          "descend (a native higher-order call, a "
                          "`catch-unwind`/`reset`/`atomically` boundary, a "
                          "match guard, or similar). Reaching the `perform` "
                          "through plain control flow -- `if`, `do`, `let`, "
                          "`while`, `match`, `set!`, a direct call -- keeps the "
                          "handler on the multi-shot path. Run with "
                          "TURI_TRACE_FIBER_FALLBACK=1 to see which form was "
                          "responsible");
    cont->resume_val = val;

    /* Re-install the handler frame around the body re-entry for a DEEP handler,
     * so a subsequent perform of the same effect in the resumed body is caught
     * again.  A SHALLOW handler (F2, `handle-shallow`) does NOT re-install: the
     * resumed body runs with this handler already removed, so a re-perform of the
     * same effect reaches the enclosing handler_stack entry (or is unhandled if
     * none).  This mirrors dk_perform's reinstall-vs-no-reinstall tail. */
    bool reinstall = !cont->handle_expr->shallow;
    TuriHandlerFrame hf;
    if (reinstall) {
        hf.cases          = cont->handle_expr->cases;
        hf.n_cases        = cont->handle_expr->n_cases;
        hf.cont           = cont;
        hf.prev           = (TuriHandlerFrame *)env->handler_stack;
        env->handler_stack = &hf;
    }

#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    swapcontext(&cont->handler_ctx, &cont->body_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

    /* Pop the handler frame now that body has yielded control back (deep only;
     * shallow never pushed one). */
    if (reinstall)
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
    EvalFrame *hframe = eval_frame_new(env, frame);
    for (uint32_t i = 0; i < matched->n_params && i < cont->n_perf_args; i++) {
        const char *pname = matched->param_bindings[i]->name->name;
        frame_bind(env, hframe, pname, cont->perf_args[i]);
    }
    frame_bind(env, hframe, matched->k_binding->name->name, turi_effect_cont(cont));

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
        /* turi-value-pool-residual-sites: track for reclaim in turi_env_free. */
        turi_env_track_coro_stack(g->env, g->stack, GEN_STACK_SIZE);
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
        /* turi-value-pool-residual-sites: track for reclaim in turi_env_free. */
        turi_env_track_coro_stack(g->env, g->stack, GEN_STACK_SIZE);
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
 * docs/archive/history/turi-capturing-shift-unimplemented.md.  serial-reset and
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

/* Consume a pending abort signal at a reset boundary `b` whose body evaluated
 * to `v`.  SR N4: an abortive shift targeting this prompt kind set
 * env->aborting + abort_value instead of longjmp-ing here, and the signal
 * propagated up (through `returning || throwing || aborting` guards) to this
 * boundary.  On a matching abort, deliver the abort value and restore the env
 * state the original longjmp path restored; a non-matching abort (e.g. a plain
 * abort passing through a serial boundary) is left set to propagate outward. */
static inline TuriValue reset_consume_abort(TuriEnv *env, const TuriResetBoundary *b,
                                            TuriValue v) {
    if (env->aborting && env->abort_target == NULL &&
        env->abort_prompt_kind == (int)b->kind) {
        env->aborting      = false;
        env->handler_stack = b->saved_handler_stack;
        env->defer_stack   = b->saved_defer_stack;
        return env->abort_value;
    }
    return v;
}

/* Establish a delimited-control prompt and evaluate body within it.  Returns
 * body's value normally, or the value delivered by an abortive shift that
 * targeted this boundary.  SR N4: the body is driven and an abortive shift
 * unwinds via env->aborting (a work-stack signal) rather than setjmp/longjmp,
 * so this no longer keeps a longjmp landing pad.  (The driver models a
 * reset reached on the work-stack as DK_RESET directly, with no C frame; this
 * function is the non-driver / nested-eval_expr entry.) */
static TuriValue eval_reset_boundary(TuriEnv *env, EvalFrame *frame,
                                     const Expr *body, TuriPromptKind kind) {
    TuriResetBoundary b;
    b.kind                = kind;
    b.result              = turi_nil();
    b.saved_handler_stack = env->handler_stack;
    b.saved_defer_stack   = env->defer_stack;
    b.prev                = g_reset_stack;
    g_reset_stack = &b;

    TuriValue v = eval_drive(env, frame, body);
    g_reset_stack = b.prev;
    return reset_consume_abort(env, &b, v);
}

/* Evaluate an abortive (shift f body) / (shift0 f body): r = f(body), then
 * abort to the nearest plain reset boundary, which returns r.  SR N4: the abort
 * is a work-stack signal (env->aborting), not a longjmp -- it short-circuits
 * the same `returning || throwing || aborting` propagation guards up to the
 * matching DK_RESET / eval_reset_boundary, which consumes it. */
static TuriValue eval_abortive_shift(TuriEnv *env, EvalFrame *frame,
                                     const Expr *k_fn, const Expr *body,
                                     const char *form) {
    TuriValue v = eval_expr(env, frame, body);
    if (turi_is_error(v) || env_signaled(env)) return v;
    TuriValue fn = eval_expr(env, frame, k_fn);
    if (turi_is_error(fn) || env_signaled(env)) return fn;
    TuriValue r = turi_call(env, fn, &v, 1);
    if (turi_is_error(r) || env_signaled(env)) return r;

    TuriResetBoundary *b = reset_find(PROMPT_PLAIN);
    if (!b)
        return turi_errorf("eval: %s used outside of any reset boundary", form);
    env->aborting          = true;
    env->abort_value       = r;
    env->abort_prompt_kind = (int)PROMPT_PLAIN;
    env->abort_target      = NULL;   /* prompt-kind abort, not an escape */
    return r;
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
 * See docs/archive/history/turi-capturing-shift-unimplemented.md.
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

static TuriCont *ts_cont_copy(TuriEnv *env, const TuriCont *c) {
    /* Escaping payload: the copy is boxed as an int continuation handle and
     * returned (multi-shot); pool-owned, never freed. */
    TuriCont *d = (TuriCont *)turi_val_alloc(env, sizeof(TuriCont));
    d->n = c->n;
    d->serial = c->serial;
    d->frames = (TsFrame *)turi_val_alloc(env, sizeof(TsFrame) * (c->n ? c->n : 1));
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
            if (turi_is_error(r) || env_signaled(env)) return r;
            if (r.tag != TURI_INT)
                return turi_errorf("eval: cont call frame returned non-int (tag %d)", r.tag);
            v = r.as_int;
        }
    }
    return turi_int(v);
}

/* SR N4 Slice 5: work-stack fold of a continuation resume.  ContFoldState holds
 * the continuation, the current frame index (folding innermost-first, frames[0]
 * outermost so i runs n-1..0), and the running int accumulator. */
typedef struct { TuriCont *c; int32_t i; int64_t v; } ContFoldState;

/* Advance the fold from s->i downward: apply pure arith frames to s->v in place,
 * stopping at the first call frame (which must be applied on the work-stack).
 * Returns 0 = done (s->v is final); 1 = a call frame at s->i needs applying,
 * with fn / args (malloc'd, freed by the driver's have_apply path) / n_out
 * filled; 2 = error (err set).  Mirrors ts_cont_resume's per-frame logic. */
static int cont_fold_advance(ContFoldState *s, TuriValue *fn, TuriValue **args,
                             uint32_t *n_out, TuriValue *err) {
    while (s->i >= 0) {
        TsFrame *fr = &s->c->frames[s->i];
        if (fr->kind == 0) {
            int64_t e0 = fr->env, v = s->v;
            switch (fr->op) {
            case '+': v = e0 + v; break;
            case '*': v = e0 * v; break;
            case '-': v = (fr->hole_index == 0) ? (v - e0) : (e0 - v); break;
            case '/':
                if (fr->hole_index == 0) {
                    if (e0 == 0) { *err = turi_error("eval: cont resume: division by zero"); return 2; }
                    v = v / e0;
                } else {
                    if (v == 0) { *err = turi_error("eval: cont resume: division by zero"); return 2; }
                    v = e0 / v;
                }
                break;
            default: *err = turi_errorf("eval: cont resume: bad op '%c'", fr->op); return 2;
            }
            s->v = v; s->i--; continue;
        }
        /* call frame: build args exactly as ts_cont_resume does. */
        uint32_t   na = fr->n_args;
        TuriValue *a  = (TuriValue *)malloc(2 * sizeof(TuriValue));
        if (fr->ignore_value) {
            if (na >= 1) { a[0] = fr->env_val; na = 1; }
        } else if (na == 1) {
            a[0] = turi_int(s->v);
        } else if (fr->hole_index == 0) {
            a[0] = turi_int(s->v); a[1] = fr->env_val;
        } else {
            a[0] = fr->env_val; a[1] = turi_int(s->v);
        }
        *fn = fr->fn; *args = a; *n_out = na;
        return 1;
    }
    return 0;
}

/* True for the continuation-resume builtins the driver folds on the work-stack
 * (DK_CONT_FOLD).  Clone / marshal / drop stay on the synchronous path. */
static bool is_cont_resume_builtin(const BuiltinSpec *spec) {
    const char *name = spec && spec->c_op ? spec->c_op : "";
    return strcmp(name, "tur_cloneable_cont_resume") == 0 ||
           strcmp(name, "tur_serial_cont_resume") == 0;
}

/* Begin a work-stack continuation fold of `c` resumed with `w`.  Returns
 *   0 = completed with no call frames (*out_val is the result),
 *   1 = a fold is in progress (out_state allocated; fn / args / n_out is the
 *       first call frame for the driver to apply via have_apply),
 *   2 = error (out_val set).
 * Shared by the cont-resume builtin and the resume-cont! native interceptions. */
static int cont_fold_begin(TuriCont *c, int64_t w, ContFoldState **out_state,
                           TuriValue *out_val, TuriValue *fn, TuriValue **args,
                           uint32_t *n_out) {
    if (!c) { *out_val = turi_int(0); return 0; }
    ContFoldState *s = (ContFoldState *)malloc(sizeof(ContFoldState));
    s->c = c; s->i = (int32_t)c->n - 1; s->v = w;
    TuriValue err;
    int rc = cont_fold_advance(s, fn, args, n_out, &err);
    if (rc == 2) { *out_val = err; free(s); return 2; }
    if (rc == 0) { *out_val = turi_int(s->v); free(s); return 0; }
    *out_state = s;
    return 1;
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
        *out = turi_int((int64_t)(intptr_t)ts_cont_copy(env, c));
        return true;
    }
    if (strcmp(name, "tur_cloneable_cont_drop") == 0) { *out = turi_nil(); return true; }
    if (strcmp(name, "tur_escape_resume") == 0) {
        /* (k v) on an escape continuation: SR N4 Slice 2 raises the work-stack
         * abort signal targeting this specific call/cc boundary (matched by
         * pointer), unwinding up to its DK_ESCAPE / eval_callcc_escape instead
         * of longjmp-ing -- so call/cc nesting stays on the heap. */
        if (n < 2 || args[0].as_int == 0) { *out = turi_int(0); return true; }
        TuriEscapeBoundary *b = (TuriEscapeBoundary *)(intptr_t)args[0].as_int;
        env->aborting          = true;
        env->abort_value       = args[1];
        env->abort_target      = (void *)b;
        *out = args[1];
        return true;
    }
    return false;
}

/* The body reaches the shift but its delimited context falls outside the
 * supported grammar, so the continuation cannot be reified.  For serial this is
 * exactly the compiled path's TUR-E0706 (emit_effects.c / emit_stmt.c); emit the
 * same diagnostic here so the interpreter rejects it rather than silently
 * miscompiling -- recovering the not-capturable negative fixtures on the
 * interpret path (the "decouple the TUR-E0706 negative path" slice of
 * docs/archive/history/turi-capturing-shift-unimplemented.md).  Returns an
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
    /* Cloneable mirrors serial: emit the coded TUR-E0710 diagnostic so the
     * interpreter rejects an unsupported cloneable-shift context the same way the
     * compiler does (a plain message lacked the code, so the negative fixture's
     * expected.diag `TUR-E0710` line did not match on the interpret path). */
    diag_emit_with_code(DIAG_ERROR, span,
        TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE,
        "cloneable-shift context is not capturable\n"
        "  = note: the delimited context falls outside the supported "
        "lowering grammar, so the continuation cannot be reified\n"
        "  = help: restructure into a supported shape (scalar let prelude / "
        "arithmetic / 1- or 2-arg call / if)");
    return turi_error("elaboration error");
}

/* SR N4 Slice 4: when the driver evaluates a capturing serial/cloneable reset it
 * passes a non-NULL `deferred` so the receiver application is moved onto the
 * work-stack (instead of the synchronous turi_call that C-recurses when the
 * receiver recursively triggers another capturing reset).  ts_capture_and_run
 * fills it with the reified continuation + receiver + the let-frames to free
 * after the receiver runs, and returns without applying; the driver then applies
 * the receiver via DK_NATIVE_RESUME (serial_receiver_resume frees the frames).
 * NULL `deferred` keeps the synchronous behaviour for non-driver callers. */
typedef struct {
    bool        active;       /* set when work was deferred to the driver */
    bool        is_fold;      /* true = is_pure fold (apply context to fold_w);
                               * false = apply the shift receiver to `cont` */
    int64_t     fold_w;       /* is_fold: the pure terminal value to resume with */
    TuriCont   *cont;         /* the reified continuation handle */
    TuriValue   receiver;     /* the shift receiver closure (when !is_fold) */
    EvalFrame **let_frames;   /* heap copy of capture-time let frames (or NULL) */
    uint32_t    n_let;        /* freed after the deferred work runs */
} TsDeferredReceiver;

/* Evaluate (serial-reset BODY) / (cloneable-reset BODY) when BODY performs the
 * matching capturing shift: reify the delimited context, hand the receiver a
 * resumable continuation, and return the receiver's value as the reset value. */
static TuriValue ts_capture_and_run(TuriEnv *env, EvalFrame *frame,
                                    const Expr *body, ExprKind shift_kind,
                                    bool serial, TsDeferredReceiver *deferred) {
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
            if (turi_is_error(pure_val) || env_signaled(env)) {
                result = pure_val; done = true;
            } else {
                is_pure = true;
            }
            break;
        }
        if (cur->kind == EX_LET) {
            EvalFrame *nf = eval_frame_new(env, cur_frame);
            let_frames[n_let++] = nf;
            bool err = false;
            for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                TuriValue v = eval_expr(env, nf, cur->as.let_.bindings[i].init);
                if (turi_is_error(v) || env_signaled(env)) {
                    result = v; done = true; err = true; break;
                }
                frame_bind(env, nf, cur->as.let_.bindings[i].binding->name->name, v);
            }
            if (err) break;
            cur_frame = nf;
            cur = cur->as.let_.body;
            continue;
        }
        if (cur->kind == EX_IF) {
            /* The condition is pure (grammar); the shift lives in one arm. */
            TuriValue cv = eval_expr(env, cur_frame, cur->as.if_.cond);
            if (turi_is_error(cv) || env_signaled(env)) {
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
            if (turi_is_error(ov) || env_signaled(env)) {
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
                if (turi_is_error(pv) || env_signaled(env)) {
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
                    if (turi_is_error(ev) || env_signaled(env)) {
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
                if (turi_is_error(envv) || env_signaled(env)) {
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
            } else if (deferred) {
                /* SR N4 Slice 7: defer the is_pure fold (apply the reified
                 * context to pure_val) to the driver's DK_CONT_FOLD, so a
                 * context call frame that recursively triggers another capturing
                 * reset folds onto the heap.  Same let-frame ownership transfer
                 * as the receiver case (a captured frame fn may close over one). */
                /* Escaping payload: cont stored in deferred->cont, never freed. */
                TuriCont *hc = (TuriCont *)turi_val_alloc(env, sizeof(TuriCont));
                hc->n = n; hc->serial = serial;
                hc->frames = (TsFrame *)turi_val_alloc(env, sizeof(TsFrame) * (n ? n : 1));
                if (n) memcpy(hc->frames, frames, sizeof(TsFrame) * n);
                deferred->active   = true;
                deferred->is_fold  = true;
                deferred->fold_w   = pure_val.as_int;
                deferred->cont     = hc;
                deferred->n_let    = n_let;
                deferred->let_frames = NULL;
                if (n_let) {
                    deferred->let_frames =
                        (EvalFrame **)malloc(n_let * sizeof(EvalFrame *));
                    memcpy(deferred->let_frames, let_frames,
                           n_let * sizeof(EvalFrame *));
                }
                return turi_nil();   /* sentinel; caller checks deferred->active */
            } else {
                TuriCont c = { frames, n, serial };
                result = ts_cont_resume(env, &c, pure_val.as_int);
            }
        } else {
            /* shift found: build a heap cont, call the receiver with it.
             * Escaping payload: handed to the receiver as a handle, never freed. */
            TuriCont *cont = (TuriCont *)turi_val_alloc(env, sizeof(TuriCont));
            cont->n = n;
            cont->serial = serial;
            cont->frames = (TsFrame *)turi_val_alloc(env, sizeof(TsFrame) * (n ? n : 1));
            if (n) memcpy(cont->frames, frames, sizeof(TsFrame) * n);
            const Expr *kfn = (shift_kind == EX_SERIAL_SHIFT)
                ? shift->as.serial_shift_.k_fn
                : shift->as.cloneable_shift_.k_fn;
            TuriValue fn = eval_expr(env, cur_frame, kfn);
            if (turi_is_error(fn) || fn.tag != TURI_CLOSURE) {
                result = turi_is_error(fn) ? fn
                       : turi_error("eval: shift receiver is not a function");
            } else if (deferred) {
                /* SR N4 Slice 4: hand the receiver application to the driver.
                 * Transfer let-frame ownership (freed by serial_receiver_resume
                 * after the receiver runs -- the receiver closure may capture a
                 * let frame, so they must outlive it). */
                deferred->active   = true;
                deferred->cont     = cont;
                deferred->receiver = fn;
                deferred->n_let    = n_let;
                deferred->let_frames = NULL;
                if (n_let) {
                    deferred->let_frames =
                        (EvalFrame **)malloc(n_let * sizeof(EvalFrame *));
                    memcpy(deferred->let_frames, let_frames,
                           n_let * sizeof(EvalFrame *));
                }
                return turi_nil();   /* sentinel; caller checks deferred->active */
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
    if (turi_is_error(fn) || env_signaled(env)) return fn;
    if (fn.tag != TURI_CLOSURE)
        return turi_errorf("eval: call/cc expects a function, got tag %d", fn.tag);

    TuriEscapeBoundary b;
    b.result              = turi_nil();
    b.saved_handler_stack = env->handler_stack;
    b.saved_defer_stack   = env->defer_stack;

    /* SR N4 Slice 2: no setjmp.  Run f with the boundary pointer as its handle
     * k; if f invokes (k v) it raises env->aborting with abort_target == &b,
     * which propagates back through turi_call to here. */
    TuriValue kval = turi_int((int64_t)(intptr_t)&b);
    TuriValue r = turi_call(env, fn, &kval, 1);
    if (env->aborting && env->abort_target == (void *)&b) {
        env->aborting      = false;
        env->abort_target  = NULL;
        env->handler_stack = b.saved_handler_stack;
        env->defer_stack   = b.saved_defer_stack;
        return env->abort_value;
    }
    return r;   /* f returned normally, or another signal passes through */
}

/* stdlib/workflow.tur save-cont! -- serialise a serial continuation to "bytes".
 * In-process the bytes is a deep copy of the chain (see ts_try_cont_builtin). */
static TuriValue native_save_cont(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || args[0].as_int == 0) return turi_int(0);
    return turi_int((int64_t)(intptr_t)ts_cont_copy(env, (TuriCont *)(intptr_t)args[0].as_int));
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

/* TR3: a TVar cell is malloc'd and tracked as a collection box, NOT allocated
 * from value_scratch.  Its handle escapes as an opaque int carrier that the
 * promotion walk cannot see, so a scratch-resident cell surviving an eval
 * boundary dangled the moment a promotion rewind reset the pool (the REPL
 * runs promotion by default) -- `(def t (tvar-new 0))` then a later
 * `atomically` read was a use-after-reset.  As a tracked box the cell
 * survives rewinds, is swept once no live value references its handle, and
 * its stored value joins the sweep's conservative mark (a TVar holding a vec
 * handle keeps that vec alive).  The stored value is a bare carrier with no
 * tag, so the enumeration is complete for handle-shaped references. */
static void tvar_buf_destroy(void *box) { free(box); }
static bool tvar_buf_scan(void *box, TuriCollBufMarkFn mark, void *ctx) {
    TuriTVar *tv = (TuriTVar *)box;
    mark(turi_int(tv->value), ctx);
    return true;
}

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
    if (turi_is_error(v) || env_signaled(env)) {
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

    if (turi_is_error(v) || env_signaled(env)) {
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
            if (args[0].tag == TURI_SYNTAX) {
                /* Structural equality, same semantics as the compile-time
                 * macro evaluator's `=` (forms.c form_equal). */
                if (args[1].tag != TURI_SYNTAX) return turi_bool(false);
                return turi_bool(form_equal(args[0].as_syntax, args[1].as_syntax));
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
            /* ptr-of: emulate &var by boxing the value into a pool cell that
             * escapes as an int carrier (reclaimed at turi_env_free). */
            int64_t *cell = (int64_t *)turi_val_alloc(env, sizeof(int64_t));
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
        /* ...  except when the elaborator's chosen SHAPE is strictly more
         * informative than the tag.  `println` is overload-resolved by static
         * type, so `(:: b :int)` selects BS_PRINTLN_INT -- the compiled path
         * then emits `printf("%lld", (long long)(true))` and prints 1, while
         * this switch saw a TURI_BOOL and printed `true`.  The same expression
         * printed differently on the two paths, so a fixture using the form
         * could not have one expected.stdout both harnesses accept.
         *
         * Fixed HERE, at the rendering site, and deliberately NOT at the
         * ascription: in the tree-walker a value's TAG is its type, and the
         * elaborator synthesizes an int-carrier ascription for an ordinary
         * `(vec-push! vb true)` into a `(Vec bool)`.  Re-tagging the bool to an
         * int there loses the element type and later method dispatch picks the
         * wrong instance -- measured, `(tag vb)` selected Tag[int] and printed 1
         * for 2.  Printing is the one place the static type can win without
         * anything downstream depending on the tag.  See
         * docs/archive/ascribe-bool-to-int-prints-differently-per-path.md. */
        if (a.tag == TURI_BOOL) {
            if (spec->shape == BS_PRINTLN_INT) {
                printf("%lld\n", (long long)(a.as_bool ? 1 : 0)); return turi_nil();
            }
            if (spec->shape == BS_PRINTLN_UINT) {
                printf("%llu\n", (unsigned long long)(a.as_bool ? 1u : 0u)); return turi_nil();
            }
            if (spec->shape == BS_PRINTLN_FLOAT || spec->shape == BS_PRINTLN_FLOAT32) {
                printf("%g\n", a.as_bool ? 1.0 : 0.0); return turi_nil();
            }
        }
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

    /* --- FFI: dynamic library loading (jit-ffi-c2mir-plan) ---------------- */
    /* Real dlopen/dlsym/dlclose under --interpret, mirroring the compiled
     * path's RTLD_LAZY semantics (emit_core BS_DLOPEN).  Handles and symbol
     * addresses ride the int64 carrier, which is exactly what call-ptr and
     * the thunk layer consume.  Capability-gated above (TURI_CAP_FFI). */
#ifndef _WIN32
    case BS_DLOPEN: {
        const char *path = (args[0].tag == TURI_CSTR) ? args[0].as_cstr : NULL;
        void *h = path ? dlopen(path, RTLD_LAZY) : NULL;
        return turi_int((int64_t)(intptr_t)h);
    }
    case BS_DLSYM: {
        void *h = (void *)(intptr_t)args[0].as_int;
        const char *nm = (args[1].tag == TURI_CSTR) ? args[1].as_cstr : NULL;
        void *s = (h && nm) ? dlsym(h, nm) : NULL;
        return turi_int((int64_t)(intptr_t)s);
    }
    case BS_DLCLOSE: {
        void *h = (void *)(intptr_t)args[0].as_int;
        return turi_int(h ? (int64_t)dlclose(h) : -1);
    }
#endif

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
                        /* TuriStruct: look up field index from CtorDef, body struct, or common table */
                        int fidx = -1;
                        /* 1. CONV-S1: a defstruct lowered to a record ADT carries
                         * its field names on its CtorDef. */
                        if (fidx < 0 && arg->as_struct->ctor &&
                            arg->as_struct->ctor->fields) {
                            const CtorDef *cd = arg->as_struct->ctor;
                            for (uint32_t fi = 0; fi < cd->n_fields && fi < arg->as_struct->n_fields; fi++) {
                                if (cd->fields[fi].name && strcmp(cd->fields[fi].name, field_name) == 0) {
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
 * docs/archive/history/turi-inline-c-ignores-comparison-operator.md.) */
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
static TuriValue ic_exec_switch_string(TuriEnv *env, const char *body,
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
                        char *buf = (char*)turi_val_alloc(env, (size_t)(se-ss)+1);
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
        char *buf = (char*)turi_val_alloc(env, default_len+1);
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

/* Scan the constructor body region before the malloc for semantic input guards
 * of the form `if (<cond>) return <expr>;` (optionally brace-wrapped).  Such a
 * guard short-circuits the constructor based on an argument -- e.g. ne-from?'s
 * `if (xs == 0) return 0;` -- which the flat-constructor model would otherwise
 * ignore, building the struct unconditionally (a silent miscompile).  An OOM
 * guard (`if (!o) return 0;`) sits *after* the malloc and is never seen here.
 * Returns 1 (and sets *out) if a guard's condition held, 2 if a guard is present
 * but its condition/return cannot be evaluated (the caller must then decline
 * rather than guess), or 0 if no guard fired. */
static int ic_constructor_leading_guard(const char *body, const char *limit,
                                        TuriValue *args, uint32_t n_args,
                                        FnDef *fn, uint32_t param_offset,
                                        TuriValue *out) {
    const char *p = body;
    while (p < limit) {
        p = ic_skip_ws(p);
        if (p >= limit || !ic_word_eq(p, "if")) return 0; /* first non-if -> stop */
        p += 2; p = ic_skip_ws(p);
        if (*p != '(') return 2;
        const char *cond_start = ++p;
        int d = 1;
        while (*p && d > 0) { if (*p=='(') d++; else if (*p==')') { if (--d==0) break; } p++; }
        if (*p != ')') return 2;
        const char *cond_end = p++;            /* p now past ')' */
        p = ic_skip_ws(p);
        bool brace = (*p == '{');
        if (brace) { p++; p = ic_skip_ws(p); }
        if (!ic_word_eq(p, "return")) return 2; /* only `return` guards modeled */
        p += 6; p = ic_skip_ws(p);
        const char *ret_start = p;
        while (*p && *p != ';') p++;
        if (*p != ';') return 2;
        const char *ret_end = p++;             /* p now past ';' */
        if (brace) { p = ic_skip_ws(p); if (*p == '}') p++; }
        int cl = (int)(cond_end - cond_start), rl = (int)(ret_end - ret_start);
        char condbuf[256], retbuf[256];
        if (cl <= 0 || cl >= (int)sizeof(condbuf) || rl < 0 || rl >= (int)sizeof(retbuf))
            return 2;
        memcpy(condbuf, cond_start, (size_t)cl); condbuf[cl] = '\0';
        memcpy(retbuf,  ret_start,  (size_t)rl); retbuf[rl]  = '\0';
        int64_t cv = 0;
        if (!ic_eval_assign_expr(condbuf, fn, param_offset, args, n_args, &cv, body))
            return 2;
        if (cv != 0) {
            int64_t rv = 0;
            if (rl > 0 &&
                !ic_eval_assign_expr(retbuf, fn, param_offset, args, n_args, &rv, body))
                return 2;
            TuriValue v = {0}; v.tag = TURI_INT; v.as_int = rv;
            *out = v;
            return 1;                          /* guard fired */
        }
        /* condition false -> fall through to the next leading statement */
    }
    return 0;
}

static TuriValue ic_exec_constructor(TuriEnv *env, const char *body,
                                      TuriValue *args, uint32_t n_args,
                                      FnDef *fn, uint32_t param_offset) {
    /* Special case: string fat-pointer constructor (->p and ->len via strlen/while) */
    if ((strstr(body,"strlen")||strstr(body,"while")) &&
         strstr(body,"->p") && strstr(body,"->len") && n_args >= 1) {
        const char *cstr = (args[0].tag==TURI_CSTR) ? args[0].as_cstr
                                                     : (const char*)(intptr_t)args[0].as_int;
        size_t len = cstr ? strlen(cstr) : 0;
        int64_t *s = (int64_t*)turi_val_alloc(env, 2*sizeof(int64_t));
        s[0] = (int64_t)(intptr_t)cstr; s[1] = (int64_t)len;
        TuriValue v={0}; v.tag=TURI_INT; v.as_int=(int64_t)(intptr_t)s; return v;
    }

    /* Find malloc call and the variable name preceding it */
    const char *malloc_p = strstr(body, "malloc(");
    if (!malloc_p) malloc_p = strstr(body, "calloc(");
    if (!malloc_p) return turi_nil();
    /* W4: decline bodies whose semantics exceed a flat constructor. */
    if (ic_constructor_unmodelable(body)) return turi_nil();
    /* Honor a leading `if (<cond>) return <const>;` input guard before the
     * malloc (e.g. ne-from?'s empty-list short-circuit); decline if it cannot
     * be evaluated rather than ignoring it and miscompiling. */
    {
        TuriValue gout;
        int g = ic_constructor_leading_guard(body, malloc_p, args, n_args,
                                             fn, param_offset, &gout);
        if (g == 1) return gout;
        if (g == 2) return turi_nil();
    }
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
    int64_t *mem = (int64_t*)turi_val_alloc(env, (size_t)n_fields*sizeof(int64_t));
    for (int i=0;i<n_fields;i++) mem[i]=field_vals[i];
    TuriValue v={0}; v.tag=TURI_INT; v.as_int=(int64_t)(intptr_t)mem; return v;
}

/* Execute accessor: cast arg to ptr, return field[idx].
 * fn is used to check the declared return type. */
static TuriValue ic_exec_accessor(TuriEnv *env, const char *body,
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
     * See docs/archive/history/turi-inline-c-accessor-miscompiles-boolean-returns.md. */
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

    /* generic-dict-dispatch (turi-generic-dict-dispatch-bakes-representative-
     * instance.md, divergence 2): a return expression that APPLIES a function to
     * the field -- e.g. `return tur_hamt_count(m->hamt);` -- is not a bare field
     * accessor.  The field-extraction paths below would silently drop the wrapping
     * call and return the raw field (rc=0, wrong: a Map's hamt pointer instead of
     * its count).  We cannot invoke an arbitrary C function here, so decline and
     * let the clean "inline-C not supported" error fire.  Detect an identifier run
     * immediately followed by `(` in the return expression; casts (`(Type)expr`)
     * were stripped above and never have that shape. */
    for (const char *p = ret; *p && *p != ';'; ) {
        if (isalnum((unsigned char)*p) || *p == '_') {
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            if (*ic_skip_ws(p) == '(') return turi_nil();  /* function call applied to field */
        } else {
            p++;
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
                size_t slen=(size_t)(se-(q+1));
                char *buf=(char*)turi_val_alloc(env, slen+1);
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

static TuriValue ic_format_snprintf_call(TuriEnv *env, const char *fp,
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
            /* Refuse-rather-than-guess (turi-inline-c-silent-miscompiles):
             * ic_eval_assign_expr returns false when it cannot faithfully
             * evaluate the argument (an unresolved local, a cast-deref through
             * an unmodeled struct, etc.).  Ignoring that and formatting the
             * leftover `val=0` is exactly the silent-miscompile class this
             * family of bugs is about -- decline the whole match so
             * try_exec_simple_inline_c falls through to a clean "inline-C not
             * supported" error instead of a plausible-but-wrong number. */
            if(!ic_eval_assign_expr(abuf,fn,param_offset,args,n_args,&val,body))
                return turi_nil();
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
    char *out=(char*)turi_val_alloc(env, (size_t)rlen+1);
    memcpy(out,result_buf,(size_t)rlen+1);
    TuriValue rv={0}; rv.tag=TURI_CSTR; rv.as_cstr=out; return rv;
}

/* Resolve  if (COND) snprintf(bufvar ...); else snprintf(bufvar ...);  by
 * evaluating COND and formatting only the live branch's snprintf. Returns the
 * formatted cstr, or turi_nil() if no such guarded snprintf pair is found (the
 * caller then falls back to the linear first-match scan). */
static TuriValue ic_snprintf_cond_branch(TuriEnv *env, const char *body,
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
        TuriValue rv = ic_format_snprintf_call(env, chosen_sf, bufvar, bvlen, args, n_args, fn, param_offset, body);
        if (rv.tag == TURI_CSTR) return rv;
        p += 2;
    }
    return turi_nil();
}

static TuriValue ic_exec_snprintf_fmt(TuriEnv *env, const char *body,
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
            char *buf = (char*)turi_val_alloc(env, elen+1);
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
     * "(7" (see docs/archive/history/turi-pure-turi-silent-miscompiles.md). So first
     * try to resolve a guarding if/else and format only the live branch; fall
     * back to the linear "first matching snprintf" scan otherwise. */
    {
        TuriValue cond = ic_snprintf_cond_branch(env, body, bufvar, bvlen, args, n_args, fn, param_offset);
        if (cond.tag == TURI_CSTR) return cond;
    }
    const char *sp = body;
    while (*sp) {
        const char *fp = strstr(sp, "snprintf(");
        if (!fp) break;
        TuriValue rv = ic_format_snprintf_call(env, fp, bufvar, bvlen, args, n_args, fn, param_offset, body);
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
 * docs/archive/history/turi-inline-c-silent-miscompiles.md): when TUR_IC_TRACE is set,
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
    (void)blen;
    if (!body || !*body) return false;

    bool has_malloc = strstr(body,"malloc(") || strstr(body,"calloc(");
    bool has_free   = ic_has_standalone_free(body);
    bool has_arrow  = strstr(body,"->");
    bool has_fptr   = strstr(body,"(*)(");
    bool has_switch = strstr(body,"switch") && strstr(body,"case ");
    bool has_return = strstr(body,"return ");

    /* Pattern 0: pure no-op discard body -- only `(void)<expr>;` casts, e.g.
     * list.tur's tur-list-homog__ homogeneity check (`(void)a; (void)b;`).  Such
     * a body has no return, no allocation, no pointer work, no side effects, and
     * is declared to return :nil; reproduce it as turi_nil().  Guard tightly so a
     * value-computing body that merely contains a `(void)` cast is not misread. */
    if (strstr(body,"(void)") && !has_return && !has_malloc && !has_free &&
        !has_arrow && !has_fptr && !has_switch &&
        !strstr(body,"=") && !strstr(body,"printf") &&
        !strstr(body,"while") && !strstr(body,"for")) {
        *out = turi_nil();
        return ic_claim("void-noop", fn, out);
    }

    /* Pattern 1: Free -- only a bare destructor (`free(p);`), not a body that
     * also computes/returns a value or fat-dispatches a closure (those merely
     * happen to contain a `*_free(` token and must not be reduced to free(arg0)). */
    if (has_free && !has_malloc && !has_return && !has_fptr) {
        *out = ic_exec_free(args, n_args);
        return ic_claim("free", fn, out);
    }

    /* Pattern 2: Switch-case string */
    if (has_switch && strstr(body,"return \"") && !has_fptr) {
        *out = ic_exec_switch_string(env, body, args, n_args);
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
        TuriValue r = ic_exec_snprintf_fmt(env, body, args, n_args, fn, param_offset);
        if (r.tag != TURI_NIL) { *out = r; return ic_claim("snprintf", fn, out); }
    }

    /* Pattern 4: Constructor (malloc + arrow or index assignments, no fptr cast) */
    if (has_malloc && !has_fptr) {
        TuriValue r = ic_exec_constructor(env, body, args, n_args, fn, param_offset);
        if (r.tag != TURI_NIL) { *out = r; return ic_claim("constructor", fn, out); }
    }

    /* Pattern 5: Accessor (no malloc, has arrow, has return) */
    if (!has_malloc && has_return && !has_fptr) {
        TuriValue r = ic_exec_accessor(env, body, args, n_args, fn);
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

/* ----- generic-dict-dispatch re-resolution ------------------------------------
 * The elaborator bakes the carrier representative instance (e.g. Size [int]) into
 * a generic function body whose receiver is a class-constrained tyvar, and relies
 * on emit-side per-call-site specialization to re-resolve.  The interpreter has
 * no such pass, so it must re-resolve at runtime from the tyvar's concrete type
 * captured at the call site (the call's abi_bindings).  See
 * docs/archive/turi-generic-dict-dispatch-bakes-representative-instance.md. */

/* Head constructor name of a concrete type (descends TY_APP to its base). */
static const char *gde_type_head_name(const Type *t) {
    if (!t) return NULL;
    while (t->kind == TY_APP && t->as.app.fn) t = t->as.app.fn;
    switch (t->kind) {
    case TY_ADT:    return t->as.adt_.def ? t->as.adt_.def->name : NULL;
    case TY_REC:    return t->as.rec.name;
    default:        return NULL;
    }
}


/* Build a closure for the dict node's method slot in instance `match`.  The dict
 * node's method_name is produced by the canonical injective mangler
 * (tur_mangle_ident), so a hyphen/sigil method renders as e.g. `render-to` ->
 * `render_hyto`, NOT the lossy `render_to` an underscore-collapse would give.
 * Compare the method's canonically-mangled name (and, defensively, the
 * underscore-collapsed form) so the slot resolves regardless of which spelling
 * the dict carries.  Returns nil if the slot has no impl in `match`. */
static TuriValue gde_method_closure(TuriEnv *env, const Expr *dict_arg,
                                    TypeClass *tc, TypeClassInstance *match) {
    const char *mname = dict_arg->as.dict_.method_name;
    for (uint32_t mi = 0; mi < tc->n_methods; mi++) {
        const char *orig = tc->methods[mi].name->name;
        char mang[256];
        tur_mangle_ident(orig, mang, sizeof(mang));
        char san[128]; size_t olen = strlen(orig);
        if (olen >= sizeof(san)) olen = sizeof(san) - 1;
        for (size_t k = 0; k < olen; k++) {
            char c = orig[k];
            san[k] = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z')) ? c : '_';
        }
        san[olen] = '\0';
        if (strcmp(mang, mname) != 0 && strcmp(san, mname) != 0) continue;
        if (mi >= match->n_method_impls || !match->method_impls[mi]) return turi_nil();
        TuriClosure *cl = (TuriClosure *)turi_val_alloc(env, sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl));
        cl->fn       = match->method_impls[mi];
        cl->captured = NULL;
        return turi_closure(cl);
    }
    return turi_nil();
}

/* gde_reresolve_method (receiver-directed) RETIRED 2026-08-17
 * (turi-dict-passing-plan step 4, final round).  It recovered a
 * baked-representative method's instance by head-name matching against the
 * pinned frame tyvar, with by-name retries for duplicate/stale class objects
 * (see docs/archive/lang-switch-breaks-generic-instance-resolution.md and
 * docs/archive/turi-generic-dict-dispatch-bakes-representative-instance.md).
 * Superseded by the apply-time constraint-dict path: once the `[^Class a]`
 * defn spelling registers real TypeConstraints
 * (docs/archive/caret-constraint-vector-not-registered.md),
 * frame_bind_constraint_dicts covers every shape it served -- including the
 * session-reset stale-class case, via the by-name canonical-class retry in
 * the push, and the `[^Show K ^Show V]` same-class pair, via tyvar-keyed
 * DictBinds.  Measured before removal: with the heuristic disabled, the FULL
 * interpreter corpus (run-turi 1794/0), all 28 interpreter-side ctest
 * targets (repl smoke's reader-switch scenarios included), and the hand-run
 * constrained/hkt family pass.  The by-value heuristic below is NOT retired:
 * 1 fixture (the carrier-collapsed unascribed receiver, which pins no tyvar
 * for the dict push to read) still relies on it. */

/* gde_reresolve_return_directed RETIRED 2026-08-16 (turi-dict-passing-plan
 * step 4).  It recovered a return-directed method's instance (`pure`,
 * `empty`, `default-of` -- class variable only in the result type, so no
 * receiver pins anything) by trying every concretely-bound frame tyvar
 * against the instance table.  Measured before removal: with the heuristic
 * disabled the FULL interpreter corpus passes (run-turi 1793/0 plus the
 * hand-run hkt-constrained family), and with the heuristic AND the dict
 * path both disabled the constrained fixtures regress to the baked
 * representative (`1 -1 1` / `207 207`) -- so the DictBind path is what
 * carries these shapes now, which is exactly the plan's retirement
 * criterion.  The receiver-directed and by-value heuristics followed it
 * into retirement 2026-08-17 (see the records above and below). */

/* gde_reresolve_method_by_value RETIRED 2026-08-17 (turi-dict-passing-plan
 * step 4, final heuristic).  It re-dispatched a baked-representative method
 * on the RECEIVER VALUE's runtime tag, covering the one shape the static
 * paths missed: an unascribed carrier-helper read (`(tag (vec-get v 0))`)
 * whose elaborated type collapses to the int64 carrier, so no tyvar gate
 * fires.  Superseded by the carrier-helper dispatch recovery in the driver's
 * EX_CALL (the turi mirror of emit_reresolve_disp_type's last branch): the
 * helper's own signature names the element tyvar, and the frame dictionary
 * pushed from the instance's constraints names the instance.  Measured
 * before removal: heuristic disabled -> run-turi 1798/0, all 28
 * interpreter-side ctest targets, and the hand-run constrained/hkt family
 * green; heuristic AND the recovery arm both disabled -> the unascribed
 * fixture regresses to the baked representatives (`1 1 hello 3.25` for
 * `1 2 hello F`), so the dict path is what carries the shape -- the plan's
 * retirement criterion.  With this, all three recovery heuristics the plan
 * set out to retire are gone; the map-show seeding stays as the dict push's
 * PIN SOURCE on the C auto-show tier, and gde_method_closure stays as the
 * shared method-slot resolver. */

/* Record the concrete type substitutions a call site pins onto the callee's
 * tyvars (its abi_bindings), resolving any still-abstract tyvar through the
 * caller's own substitution so nested generics compose. */
static void frame_record_abi(TuriEnv *env, EvalFrame *callee, EvalFrame *caller, const Expr *call) {
    for (uint8_t i = 0; i < call->as.call_.n_abi_bindings; i++) {
        AbiTypeBinding *ab = &call->as.call_.abi_bindings[i];
        if (!ab->name) continue;
        Type t = ab->type;
        if (t.kind == TY_TYVAR && t.as.tyvar_.name) {
            Type r;
            if (frame_lookup_tyvar(caller, t.as.tyvar_.name, &r)) t = r;
        }
        if (t.kind == TY_TYVAR) continue;  /* still abstract: nothing to pin */
        TyvarBind *tb = (TyvarBind *)turi_val_alloc(env, sizeof(TyvarBind));
        tb->name = ab->name;
        tb->type = t;
        tb->next = callee->tyvars;
        callee->tyvars = tb;
    }
}

/* Pin a callee's HKT tyvar from the STATIC type of the argument passed to it,
 * for a call that pins nothing of its own.
 *
 * A constrained generic reached through a rank-2 `forall` parameter carries no
 * abi_bindings: `(defn at-t1 [g (forall [(m :: * -> *)] ...)])` invokes it as
 * `(g (mk-t1 0))`, and the elaborator has no call-site substitution to record
 * because the callee is a parameter, not a named generic.  So nothing reaches
 * the frame, gde_reresolve_return_directed finds no concrete tyvar, and the
 * baked representative answers -- two differently-tagged Applicatives both
 * returning the second instance's `pure`.
 *
 * The substitution is nonetheless right there in the types: the callee declares
 * `x : (m int)` and the call site's argument has static type `(T1 int)`.  Match
 * the two type applications and `m -> T1` follows.
 *
 * Deliberately narrow.  Only a declared `(tyvar arg)` against a concrete-headed
 * `(Ctor arg)` is matched -- the higher-kinded shape this is about -- rather
 * than general structural unification, whose extra reach would be untested and
 * whose failure mode is a WRONG instance rather than today's conservative one.
 * Only called when the call recorded no abi_bindings, so every call that pins
 * something keeps its exact prior behaviour.
 * See docs/archive/turi-return-directed-method-keeps-baked-instance.md. */
static void frame_pin_hkt_tyvars_from_args(TuriEnv *env, EvalFrame *callee,
                                           const FnDef *fn,
                                           uint32_t param_offset,
                                           uint32_t effective_params,
                                           const Expr *call, uint32_t arg_base) {
    if (!fn || !fn->params || !call || call->kind != EX_CALL) return;
    for (uint32_t i = 0; i < effective_params; i++) {
        uint32_t ai = arg_base + i;
        if (ai >= call->as.call_.n_args || !call->as.call_.args[ai]) continue;
        const Type *pt = &fn->params[param_offset + i]->type;
        const Type *at = &call->as.call_.args[ai]->type;
        if (pt->kind != TY_APP || at->kind != TY_APP) continue;
        const Type *pf = pt->as.app.fn;
        const Type *af = at->as.app.fn;
        if (!pf || !af) continue;
        if (pf->kind != TY_TYVAR || !pf->as.tyvar_.name) continue;
        if (af->kind == TY_TYVAR) continue;   /* argument still abstract */
        Type existing;
        if (frame_lookup_tyvar(callee, pf->as.tyvar_.name, &existing)) continue;
        TyvarBind *tb = (TyvarBind *)turi_val_alloc(env, sizeof(TyvarBind));
        tb->name = pf->as.tyvar_.name;
        tb->type = *af;
        tb->next = callee->tyvars;
        callee->tyvars = tb;
    }
}

/* Bind a bare-head constrained instance's constraint tyvars onto the instance
 * body frame.  A `(definstance C [Cons] [(C A)] ...)` head pins only the class
 * param `a = Cons`; its element tyvar `A` lives solely in the `[(C A)]`
 * constraint and never appears in the call's abi_bindings, so a nested
 * typeclass dispatch inside the body (e.g. `(tag-head c)` ->
 * `(tag (:: (.head c) A))`) would find `A` unbound and fall back to the
 * int-carrier representative instance.  The compiled path recovers `A`
 * per-specialization from the receiver's static type (emit_reresolve_disp_type
 * -> emit_abi_constraint_var_bindings); mirror that here.  `recv_ty` is the
 * receiver argument's static type at the dispatch site (`(Cons (Option int))`);
 * its type-args index each constraint's `param_idx` (the same convention the
 * shared emit kernel uses). */
static void frame_bind_instance_constraint_tyvars(TuriEnv *env, EvalFrame *callee,
                                                  const FnDef *fn, const Type *recv_ty) {
    if (!fn || !fn->owner_instance || !recv_ty) return;
    const TypeClassInstance *inst = fn->owner_instance;
    if (inst->n_type_param_constraints == 0) return;
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(recv_ty, &def, args, &n_args) || !def) return;
    for (uint8_t ci = 0; ci < inst->n_type_param_constraints; ci++) {
        const TypeConstraint *tc = &inst->type_param_constraints[ci];
        if (!tc->tyvar || !tc->tyvar->name) continue;
        if (tc->param_idx < 0 || (uint8_t)tc->param_idx >= n_args) continue;
        Type bound = args[tc->param_idx];
        if (bound.kind == TY_TYVAR) continue;  /* still abstract: nothing to pin */
        /* Don't clobber a binding the call's own abi_bindings already pinned. */
        Type existing;
        if (frame_lookup_tyvar(callee, tc->tyvar->name, &existing) &&
            existing.kind != TY_TYVAR)
            continue;
        TyvarBind *tb = (TyvarBind *)turi_val_alloc(env, sizeof(TyvarBind));
        tb->name = tc->tyvar->name;
        tb->type = bound;
        tb->next = callee->tyvars;
        callee->tyvars = tb;
    }
}

/* Structurally match a declared type PATTERN against a concrete type and read
 * the subtype at `varname`'s position -- matching `(Vec R)` against the actual
 * `(Vec A)` yields `A`.  Mirror of emit_core.c:emit_pattern_extract_classvar
 * (and elab_typeclasses.c's elab-side copy), for the interpreter's
 * carrier-helper dispatch recovery below. */
static bool turi_pattern_extract_var(const Type *pattern, const Type *concrete,
                                     const char *varname, Type *out) {
    if (!pattern || !concrete || !varname) return false;
    if (pattern->kind == TY_TYVAR && pattern->as.tyvar_.name &&
        strcmp(pattern->as.tyvar_.name, varname) == 0) {
        *out = *concrete;
        return true;
    }
    if (pattern->kind == TY_APP && concrete->kind == TY_APP) {
        if (turi_pattern_extract_var(pattern->as.app.fn, concrete->as.app.fn,
                                     varname, out))
            return true;
        if (turi_pattern_extract_var(pattern->as.app.arg, concrete->as.app.arg,
                                     varname, out))
            return true;
    }
    return false;
}

/* turi-dict-passing-plan (plain constrained generics): after a call's tyvar
 * pins land on the callee frame, resolve each of the callee's typeclass
 * constraints against its pinned concrete type using the ELABORATOR'S own
 * instance lookup, and record the result as a DictBind.  This is the
 * apply-time analogue of the dict-clone param binding: method dispatch inside
 * the body then reads the frame dictionary with precedence over the gde_*
 * recovery heuristics, instead of re-deriving the instance by head-name
 * matching at every method call.
 *
 * Resolution order mirrors static dispatch: exact structural match first
 * (disambiguates same-head instances like `(Option cstr)` vs `(Option int)`),
 * then the kind-erased head-discriminated lookup (covers a bare-head
 * `[Vec]` instance answering a `(Vec int)` query), then the KIND_ARROW
 * structured key (an HKT constraint pinned to a bare carrier).
 *
 * Deliberately conservative: an unpinned tyvar or a failed lookup pushes
 * nothing, leaving those shapes to the heuristics exactly as before.  Two
 * constraints on the same class (`[^Show K ^Show V]`) each push their own
 * bind, keyed by constraint tyvar name -- frame_lookup_dict_tyvar picks the
 * one the dispatch site's tyvar names. */
static void frame_bind_constraint_dicts(TuriEnv *env, EvalFrame *callee,
                                        const TypeConstraint *constraints,
                                        uint8_t n_constraints) {
    if (!env || !env->last_tc_env || !constraints || n_constraints == 0) return;
    TypeClassEnv *tc_env = (TypeClassEnv *)env->last_tc_env;
    for (uint8_t i = 0; i < n_constraints; i++) {
        const TypeConstraint *c = &constraints[i];
        if (!c->typeclass) continue;
        const char *tvname = NULL;
        if (c->tyvar && c->tyvar->name) tvname = c->tyvar->name;
        else if (c->type_arg.kind == TY_TYVAR && c->type_arg.as.tyvar_.name)
            tvname = c->type_arg.as.tyvar_.name;
        if (!tvname) continue;
        Type concrete;
        if (!frame_lookup_tyvar(callee, tvname, &concrete) ||
            concrete.kind == TY_TYVAR)
            continue;
        TypeClass *lookup_tc = c->typeclass;
        TypeClassInstance *inst = NULL;
        for (int tc_try = 0; tc_try < 2 && !inst; tc_try++) {
            if (tc_try == 1) {
                /* All pointer-keyed lookups missed.  A session reset (#lang
                 * reader switch) or a duplicate class object leaves the
                 * constraint's TypeClass pointer pointing at a copy the live
                 * registry's instances were not registered under -- see
                 * docs/archive/lang-switch-breaks-generic-instance-resolution.md.
                 * Class NAMES are unique per program, so re-resolve the class
                 * by name and retry the same precise lookups under the
                 * canonical copy.  The DictBind still keys on the ORIGINAL
                 * pointer, which is what the body's baked dict_arg carries. */
                if (!c->typeclass->name || !c->typeclass->name->name) break;
                TypeClass *canon = NULL;
                for (TypeClass *t = tc_env->typeclasses; t; t = t->next)
                    if (t != c->typeclass && t->name && t->name->name &&
                        strcmp(t->name->name, c->typeclass->name->name) == 0) {
                        canon = t;
                        break;
                    }
                if (!canon) break;
                lookup_tc = canon;
            }
            inst = typeclass_env_lookup_instance_exact(
                tc_env, lookup_tc, &concrete, 1);
            if (!inst)
                inst = typeclass_env_lookup_instance(tc_env, lookup_tc,
                                                     &concrete, 1);
            if (!inst) {
                TypeClassDispatchKey key;
                memset(&key, 0, sizeof(key));
                key.typeclass        = lookup_tc;
                key.type_args        = &concrete;
                key.n_type_args      = 1;
                key.constructor_kind = KIND_ARROW;
                inst = typeclass_env_lookup_instance_by_key(tc_env, &key);
            }
        }
        if (!inst) continue;
        DictBind *db = (DictBind *)turi_val_alloc(env, sizeof(DictBind));
        db->tc    = c->typeclass;
        db->inst  = inst;
        db->tyvar = tvname;
        db->next  = callee->dicts;
        callee->dicts = db;
    }
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
                      * frame = handler lexical frame, index = active flag (1/0). */
    DK_UNARY,        /* SR N3: single-operand black-box form (cast/ascribe/return/
                      * set/transparent shim); expr = the form, applied via
                      * eval_unary_post when the operand value returns. */
    DK_GET_FIELD,    /* SR N2: EX_GET_FIELD receiver evaluated on the work-stack;
                      * expr = the EX_GET_FIELD, applied via get_field_extract when
                      * the receiver value returns.  Folds recursion that flows
                      * through a field accessor (e.g. option-map's .value). */
    DK_NATIVE_RESUME,/* SR (turi-cek-stackless-reentry): a native/special-form HOF
                      * suspended onto the work-stack while the driver applies a
                      * closure on its behalf.  aux = NativeResume* (resume fn +
                      * opaque state).  Sits beneath the application it requested,
                      * the structural twin of tur's DKK_FRAME. */
    DK_RESET,        /* SR N4: a (reset ...) boundary modeled on the work-stack
                      * (no setjmp).  aux = heap TuriResetBoundary* linked on
                      * g_reset_stack; the body is driven beneath it.  An abortive
                      * shift's env->aborting signal propagates up to here, which
                      * consumes it (matching prompt kind) or lets it pass.  This
                      * keeps reset/shift nesting on the heap, not one C frame per
                      * reset (the F5-guard blocker). */
    DK_ESCAPE,       /* SR N4 Slice 2: a (call/cc f) escape boundary on the
                      * work-stack (no setjmp).  aux = heap TuriEscapeBoundary*;
                      * f is applied beneath it with the boundary pointer as its
                      * handle k.  Invoking (k v) raises env->aborting with
                      * abort_target = this boundary, unwinding the work-stack
                      * here.  Keeps call/cc nesting on the heap. */
    DK_CONT_FOLD,    /* SR N4 Slice 5: folding a serial/cloneable continuation
                      * resume on the work-stack.  aux = ContFoldState* (the
                      * continuation, current frame index, accumulator).  Each
                      * captured call frame is applied via the have_apply channel
                      * and the result returns here, which advances past pure
                      * arith frames and re-requests the next call frame -- so a
                      * resumed frame that itself resumes folds onto the heap
                      * instead of C-recursing through ts_cont_resume's turi_call. */
    DK_CATCH_UNWIND, /* C1 (turi-c-scoped-forms-heap-bounding): a (catch-unwind
                      * thunk) boundary on the work-stack (no setjmp).  aux = heap
                      * TuriCatchBoundary* (registered on g_catch_stack); the
                      * thunk is applied beneath it via the have_apply channel.
                      * A panic raised under it sets env->panicking (a signal),
                      * which propagates up the work-stack to here -- consumed as
                      * (err payload).  A normal value is wrapped (ok value).
                      * Keeps deeply-nested catch-unwind on the heap instead of
                      * one setjmp/eval_apply C frame per level. */
    DK_ATOMICALLY,   /* C2 (turi-c-scoped-forms-heap-bounding): an (atomically
                      * (stm ...)) transaction boundary on the work-stack.  aux =
                      * heap TuriStmTx* (linked on g_stm_tx); the stm body is
                      * driven beneath it (DK_STM_SEQ).  On normal completion the
                      * write-log is committed; a requested retry/abort yields the
                      * single-threaded no-progress error.  Keeps deeply-nested
                      * atomically on the heap instead of one eval_atomically C
                      * frame per level. */
    DK_STM_SEQ,      /* C2: the (stm e1 e2 ...) body sequence driven on the
                      * work-stack (the recursion inside an stm item folds here).
                      * expr = the EX_STM, index = next item, frame = enclosing.
                      * A retry/abort request on g_stm_tx short-circuits the rest
                      * of the block (matching the eval_expr_impl EX_STM loop). */
    DK_WHILE,        /* A (while COND BODY) driven on the work-stack.  expr = the
                      * EX_WHILE, frame = enclosing, index = phase (0 = the
                      * value just returned is COND's, 1 = it is BODY's).  Every
                      * iteration re-descends from this one frame, so the loop
                      * costs O(1) work-stack depth however long it runs.
                      *
                      * The point is not depth -- eval_expr_impl's C `while`
                      * was already flat -- but TRANSPARENCY: a `perform` or
                      * `resume` inside the loop has to land in the driver's
                      * descending switch with the enclosing DK_PROMPT visible
                      * on `st`.  Evaluating the loop through eval_expr made it
                      * a black box, which forced any handle whose body or
                      * clause contained a `while` onto the one-shot fiber path
                      * -- where a second resume aborted the interpreter.  See
                      * docs/archive/turi-multishot-resume-in-while-aborts.md. */
    DK_RESUME,       /* C3: a (resume k value) whose `value` is driven on the
                      * work-stack (was eval_expr), so recursion in the resume
                      * value arg folds instead of C-recursing.  last = the
                      * evaluated continuation k, frame = enclosing, tail = the
                      * resume's tail-ness.  On the value's return, DK_RESUME
                      * dispatches: a ws continuation re-installs its prompt +
                      * clone (as the descend case did), a fiber continuation
                      * calls eval_resume_cont with the driven value. */
    DK_PERFORM_ARG,  /* A perform's ARGUMENTS driven on the work-stack.  expr =
                      * the EX_PERFORM, frame = enclosing, index = next arg,
                      * aux = malloc'd TuriValue accumulator, tail = the
                      * perform's tail-ness.  Pushed only when an arg may
                      * itself perform (ws_has_perform) -- e.g.
                      * `(perform (Log (perform (Ask))))` -- so the inner
                      * perform lands in the driver with the prompt visible.
                      * When the last arg returns, the accumulator hands off to
                      * the descending EX_PERFORM arm via the driver-local
                      * pargs_for/pargs_heap side channel and dispatch proceeds
                      * exactly as the synchronous path.  The accumulator is in
                      * clone_ws_slice's and the capture re-homing's
                      * duplication sets, like DK_CALL_ARG's. */
    DK_MATCH_SCRUT,  /* A match SCRUTINEE driven on the work-stack.  expr = the
                      * EX_MATCH, frame = enclosing, tail = the match's
                      * tail-ness.  Pushed only when the scrutinee may perform
                      * (ws_has_perform) -- the common perform-free match keeps
                      * the synchronous eval_match_resolve with no extra frame.
                      * On the value's return, eval_match_resolve_with selects
                      * the arm and the winning body descends (with the F1 tail
                      * leak when the match was in tail position). */
    DK_RESUME_K,     /* The phase before DK_RESUME: the resume's `k` expression
                      * itself driven on the work-stack (was eval_expr).  expr =
                      * the EX_RESUME, frame = enclosing, tail = the resume's
                      * tail-ness.  When k's value returns, validate it and
                      * hand off to DK_RESUME with the value arg descending.
                      * Exists so a perform/resume in k's own computation --
                      * rare, but expressible -- does not force the enclosing
                      * handle onto the single-shot fiber. */
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
    const void *dbg_fn;         /* debugger Phase 2: const FnDef* of the activation */
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
 * Multishot: each resume runs an INDEPENDENT clone of the captured slice (deep
 * copy of the owned frames + arg accumulators), so resuming `k` with distinct
 * values yields distinct runs -- matching the compiled path's true multishot.
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
};

/* Shallow-copy a frame's bindings into a fresh frame (parent set by caller).
 * Bindings are re-linked most-recent-first to preserve shadowing order. */
static EvalFrame *clone_frame_bindings(TuriEnv *env, EvalFrame *src, EvalFrame *parent) {
    EvalFrame *nf = (EvalFrame *)turi_val_alloc(env, sizeof(EvalFrame));
    nf->parent = parent;
    nf->bindings = NULL;
    /* Collect src bindings (head-first) then re-prepend in reverse to preserve
     * the original head-first order. */
    size_t n = 0;
    for (EvalBinding *b = src->bindings; b; b = b->next) n++;
    if (n) {
        EvalBinding **arr = (EvalBinding **)malloc(n * sizeof(EvalBinding *));
        size_t i = 0;
        for (EvalBinding *b = src->bindings; b; b = b->next) arr[i++] = b;
        for (size_t j = n; j-- > 0; ) frame_bind(env, nf, arr[j]->name, arr[j]->value);
        free(arr);
    }
    return nf;
}

/* Clone a captured slice into fresh DriveCont frames for an independent resume.
 * Owned frames (LET_BIND/LET_BODY/MATCH_BODY/CALL_RET) are deep-copied and any
 * `.frame` pointing at an owned frame is remapped to its clone; per-frame heap
 * arg accumulators (BUILTIN_ARG/CALL_ARG/MAKE_STRUCT) are duplicated so the two
 * runs never share mutable state.  `out` must hold `n` DriveConts. */
static void clone_ws_slice(TuriEnv *env, const DriveCont *src, size_t n, DriveCont *out) {
    /* Map owned source frames -> clones. */
    EvalFrame *keys[64]; EvalFrame *vals[64]; size_t nmap = 0;
    for (size_t i = 0; i < n; i++) {
        DriveKind k = src[i].kind;
        if ((k == DK_LET_BIND || k == DK_LET_BODY || k == DK_MATCH_BODY ||
             k == DK_CALL_RET) && src[i].frame && nmap < 64) {
            keys[nmap] = src[i].frame;
            vals[nmap] = clone_frame_bindings(env, src[i].frame, src[i].frame->parent);
            nmap++;
        }
    }
    /* Re-parent clones whose parent is itself an owned (cloned) frame. */
    for (size_t m = 0; m < nmap; m++)
        for (size_t p = 0; p < nmap; p++)
            if (vals[m]->parent == keys[p]) { vals[m]->parent = vals[p]; break; }
    /* Copy DriveConts, remapping frames and duplicating accumulators. */
    for (size_t i = 0; i < n; i++) {
        out[i] = src[i];
        if (out[i].frame)
            for (size_t m = 0; m < nmap; m++)
                if (out[i].frame == keys[m]) { out[i].frame = vals[m]; break; }
        /* Duplicate heap accumulators (aux) that hold partial results. */
        if (src[i].aux) {
            size_t cnt = 0;
            if (src[i].kind == DK_BUILTIN_ARG)
                cnt = src[i].expr->as.builtin.n;
            else if (src[i].kind == DK_CALL_ARG)
                cnt = src[i].expr->as.call_.n_args;
            else if (src[i].kind == DK_MAKE_STRUCT)
                cnt = src[i].expr->as.make_struct_.n_fields;
            else if (src[i].kind == DK_PERFORM_ARG)
                cnt = src[i].expr->as.perform_.perform->n_args;
            if (cnt) {
                TuriValue *acc = (TuriValue *)malloc(cnt * sizeof(TuriValue));
                memcpy(acc, src[i].aux, cnt * sizeof(TuriValue));
                out[i].aux = acc;
            }
        }
    }
}

/* Wrap a work-stack continuation in a TURI_EFFECT_CONT value (discriminated by
 * the ws pointer being non-NULL). */
static TuriValue turi_ws_cont_val(TuriEnv *env, TuriWsCont *wc) {
    /* Escaping payload: wraps a work-stack continuation in a returned value. */
    TuriEffectCont *c = (TuriEffectCont *)turi_val_calloc(env, sizeof(TuriEffectCont));
    c->ws = wc;
    return turi_effect_cont(c);
}

/* -------------------------------------------------------------------------
 * SR (turi-cek-stackless-reentry-plan): native re-entry as a work-stack
 * resume continuation.
 *
 * A re-entrant native / special form that must apply a closure stops calling
 * back through the C stack (turi_call -> eval_apply -> eval_drive_ex on a fresh
 * C frame).  Instead it requests the application from the driver: it pushes a
 * DK_NATIVE_RESUME frame carrying (state, resume), and asks the driver to apply
 * the closure on the work-stack beneath it.  When the application completes the
 * driver calls `resume(env, state, applied, &done, &out)`:
 *   - `*out`  is the value this resume yields,
 *   - `*done` is currently always true (single-shot; N2+ extends to loops where
 *     resume re-requests the next application and the slot is reused).
 * The native's C frame no longer spans the callback, so the callback's own
 * recursion is heap-bounded on the work-stack rather than nesting C frames.
 * ---------------------------------------------------------------------- */
typedef TuriValue (*TuriNativeResumeFn)(TuriEnv *env, void *state,
                                        TuriValue applied, bool *done,
                                        TuriValue *out);
typedef struct {
    TuriNativeResumeFn resume;
    void              *state;
} NativeResume;

/* tvar/modify (SR N1 conversion target): state captured between the read and
 * the commit that straddle the user fn application. */
typedef struct { TuriTVar *tv; int64_t old; } TvarModifyState;

/* resume for tvar/modify: `applied` is fn(old).  Commit it to the write-log and
 * yield the OLD value (read-modify-write returns the prior value). */
static TuriValue tvar_modify_resume(TuriEnv *env, void *state, TuriValue applied,
                                    bool *done, TuriValue *out) {
    TvarModifyState *s = (TvarModifyState *)state;
    *done = true;
    if (turi_is_error(applied) || env_signaled(env)) {
        *out = applied;                       /* fn signalled: propagate, no commit */
    } else {
        stm_log_write(g_stm_tx, s->tv, applied.as_int);
        *out = turi_int(s->old);
    }
    free(s);
    return *out;
}

/* abortive shift (SR N3b/N4): the receiver application f(body) runs on the
 * work-stack, then the result aborts to the nearest plain reset boundary.
 * `form` names the operator for the out-of-boundary diagnostic. */
typedef struct { const char *form; } AbortiveShiftState;

/* resume for shift/shift0: `applied` is f(body).  If the receiver signalled,
 * propagate without aborting; otherwise raise the work-stack abort signal
 * (env->aborting) targeting PROMPT_PLAIN.  The signal propagates up the
 * work-stack (the return path treats it as `signaled`) to the matching
 * DK_RESET, which consumes it -- no longjmp, so reset nesting stays on the heap
 * work-stack.  Returns normally; the DK_NATIVE_RESUME handler frees nr. */
static TuriValue abortive_shift_resume(TuriEnv *env, void *state, TuriValue applied,
                                       bool *done, TuriValue *out) {
    AbortiveShiftState *s = (AbortiveShiftState *)state;
    *done = true;
    if (turi_is_error(applied) || env_signaled(env)) {
        *out = applied;     /* receiver signalled: propagate, no abort raised */
        free(s);
        return *out;
    }
    if (!reset_find(PROMPT_PLAIN)) {
        *out = turi_errorf("eval: %s used outside of any reset boundary", s->form);
        free(s);
        return *out;
    }
    env->aborting          = true;
    env->abort_value       = applied;
    env->abort_prompt_kind = (int)PROMPT_PLAIN;
    env->abort_target      = NULL;   /* prompt-kind abort, not an escape */
    *out = applied;
    free(s);
    return *out;
}

/* SR N4 Slice 4: state for a deferred capturing-shift receiver application --
 * the capture-time let frames to free once the receiver completes. */
typedef struct { EvalFrame **let_frames; uint32_t n_let; } SerialReceiverState;

/* resume for a capturing serial/cloneable shift: the receiver's value IS the
 * reset's value, so just pass `applied` through; then free the capture-time let
 * frames (kept alive across the receiver, which may close over one of them). */
static TuriValue serial_receiver_resume(TuriEnv *env, void *state, TuriValue applied,
                                        bool *done, TuriValue *out) {
    (void)env;
    SerialReceiverState *s = (SerialReceiverState *)state;
    *done = true;
    *out  = applied;
    for (int32_t i = (int32_t)s->n_let - 1; i >= 0; i--)
        eval_frame_free(s->let_frames[i]);
    free(s->let_frames);
    free(s);
    return applied;
}

/* Extract field `e->as.get_field_.field_idx` from an already-resolved receiver
 * value `sv` (an EX_GET_FIELD whose struct_expr has been evaluated).  Shared by
 * eval_expr_impl's recursive EX_GET_FIELD and the driver's DK_GET_FIELD
 * continuation so the work-stack path folds the receiver instead of black-boxing
 * it through eval_expr (SR N2).  Handles both the int64 carrier ABI (Option /
 * Result flowing as :int, incl. the NULL "none"/err-less carrier) and a real
 * TuriStruct. */
static TuriValue get_field_extract(const Expr *e, TuriValue sv) {
    uint32_t idx = e->as.get_field_.field_idx;
    /* Auto-deref an rc<T> receiver: an rc value is a "__rc" wrapper struct
     * { counter-ptr, inner }, so `(.field rc-val)` (and `(.f (.rcfield s))`,
     * where the inner is itself a struct/record ADT) must resolve through the
     * wrapper to the inner value's field -- mirroring the compiled rc<Struct> /
     * rc<ADT> auto-deref (CONV-S1 slice 2/5).  Walk nested __rc wrappers. */
    while (sv.tag == TURI_STRUCT && sv.as_struct && sv.as_struct->name &&
           strcmp(sv.as_struct->name, "__rc") == 0 && sv.as_struct->n_fields >= 2)
        sv = sv.as_struct->fields[1];
    if (sv.tag == TURI_INT) {
        if (sv.as_int == 0) {
            /* NULL carrier: a none Option / err-less Result.  A field read is the
             * by-value body's tag probe; hand back the field type's zero. */
            switch (e->type.kind) {
            case TY_BOOL: return turi_bool(false);
            case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32: return turi_float(0.0);
            default: return turi_int(0);
            }
        }
        int64_t w = ((int64_t *)(intptr_t)sv.as_int)[idx];
        switch (e->type.kind) {
        case TY_BOOL:  return turi_bool(w != 0);
        case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32: {
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

/* SR N3: single-operand "black box" forms.  A family of expression kinds that
 * evaluate exactly one inner sub-expression and then either transform the value
 * (cast / reinterpret / ascribe / return / set) or pass it through unchanged
 * (the compiler-inserted transparent shims poly-wrap / fn-to-fat / ref / borrow
 * / exists-pack / union-inject).  eval_drive_ex routed all of them through the
 * recursive eval_expr `default:` path, so recursion threaded through the inner
 * operand C-recursed (e.g. `(:: (f ...) :int)` / `(return (+ n (f ...)))`).
 * Modeling them in the driver (DK_UNARY) folds the operand onto the work-stack.
 *
 * unary_operand returns the inner sub-expression to descend (NULL for a bare
 * `(return)` with no value -- the caller runs the post directly on nil).
 * eval_unary_post applies the form's post-operand logic to the resolved value,
 * shared with eval_expr_impl so the two paths cannot diverge. */
/* turi-dict-passing-plan: a rank-2 poly value wrapping a CONSTRAINED fn
 * evaluates to its dict-clone's global closure -- the callee whose leading
 * dict params the elaborated call site supplies.  Returns true (with *out
 * set) when the clone resolves; false falls back to the plain unwrap. */
static TuriValue eval_lookup(TuriEnv *env, EvalFrame *frame, const char *name);
static bool poly_wrap_dict_clone_value(TuriEnv *env, EvalFrame *frame,
                                       const Expr *e, TuriValue *out) {
    if (!e || e->kind != EX_POLY_WRAP || !e->as.poly_wrap_.dict_clone_binding)
        return false;
    const Binding *cb = e->as.poly_wrap_.dict_clone_binding;
    if (!cb->name || !cb->name->name) return false;
    TuriValue v = eval_lookup(env, frame, cb->name->name);
    if (v.tag != TURI_CLOSURE) return false;
    *out = v;
    return true;
}

static bool unary_operand(const Expr *e, const Expr **operand) {
    switch (e->kind) {
    case EX_CAST:         *operand = e->as.cast_.expr;          return true;
    case EX_REINTERPRET:  *operand = e->as.reinterpret_.expr;   return true;
    case EX_ASCRIBE:      *operand = e->as.ascribe_.inner;      return true;
    case EX_RETURN:       *operand = e->as.return_.value;       return true; /* NULLABLE */
    case EX_SET:          *operand = e->as.set_.value;          return true;
    case EX_POLY_WRAP:    *operand = e->as.poly_wrap_.inner;    return true;
    case EX_FN_TO_FAT:    *operand = e->as.fn_to_fat_.inner;    return true;
    case EX_POLY_TO_FAT:  *operand = e->as.poly_to_fat_.inner;  return true;
    case EX_BORROW_IMMUT: *operand = e->as.borrow_immut_.expr;  return true;
    case EX_RC_FROM_REF:  *operand = e->as.rc_from_ref_.expr;   return true;
    case EX_REF:          *operand = e->as.ref_.expr;           return true;
    case EX_EXISTS_PACK:  *operand = e->as.exists_pack_.value;  return true;
    case EX_UNION_INJECT: *operand = e->as.union_inject_.value; return true;
    default: return false;
    }
}

/* The concrete TypeKind an EX_ASCRIBE / EX_REINTERPRET should re-tag to.
 * Normally this is just `ty->kind`, but inside a generic body an ascription to
 * a bare type variable `A` (`(:: e A)`) reaches the interpreter with
 * `ty->kind == TY_TYVAR` -- the tree-walker never monomorphizes, so A is still
 * abstract in the elaborated AST.  Resolve it through the frame's tyvar
 * bindings (pinned per call site by frame_record_abi /
 * frame_bind_instance_constraint_tyvars) so the primitive re-tag fires on the
 * grounded type, mirroring the compiled path's per-specialization re-dispatch.
 * An unbound or still-abstract tyvar keeps TY_TYVAR (the switch's transparent
 * default). */
static TypeKind ascribe_effective_kind(EvalFrame *frame, const Type *ty) {
    if (ty->kind == TY_TYVAR && ty->as.tyvar_.name) {
        Type rt;
        if (frame_lookup_tyvar(frame, ty->as.tyvar_.name, &rt) && rt.kind != TY_TYVAR)
            return rt.kind;
    }
    return ty->kind;
}

/* collection-multiword-element-boxing (interpreter, Map struct VALUE read-back):
 * a by-value struct/ADT stored as a Map VALUE rides the int64 carrier (the map
 * macro passes the value straight through; native_map_get_eq_o hands it back as
 * turi_int(pointer)).  In the interpreter that pointer is a TuriStruct*, but
 * get_field_extract's TURI_INT path reads it as a compiled raw int64[] field
 * buffer -- so `.x` reads the TuriStruct header (name/n_fields), not the logical
 * field.  When `(:: <carrier> T)` ascribes such a carrier to a by-value
 * (non-heap) struct/ADT, retag it to TURI_STRUCT so field access takes the
 * correct TuriStruct path.  Heavily guarded so a raw Option/Result carrier or a
 * non-struct int is left untouched: Option/Result flow as APPLIED types (TY_APP,
 * excluded here -- only a bare non-parametric record ADT reaches this), the
 * pointer and its embedded name pointer must be plausible (> 0x1000), the struct
 * name must equal the ascribed type's name, and the field count must match. */
static TuriValue try_retag_carrier_struct(EvalFrame *frame, const Type *ty,
                                          TuriValue v) {
    if (v.tag != TURI_INT || v.as_int == 0) return v;
    Type rt = *ty;
    if (ty->kind == TY_TYVAR && ty->as.tyvar_.name) {
        Type r;
        if (frame_lookup_tyvar(frame, ty->as.tyvar_.name, &r) && r.kind != TY_TYVAR)
            rt = r;
    }
    if (rt.kind != TY_ADT || !rt.as.adt_.def) return v;
    const AdtDef *d = rt.as.adt_.def;
    if (d->is_heap || !d->name) return v;
    /* Only a single-ctor RECORD is stored as a carrier POINTER.  A `defopaque`
     * int newtype rides the carrier as its INLINE value (a plain int, NOT a
     * TuriStruct pointer), so dereferencing it as a struct would read arbitrary
     * memory (a large opaque int looks like a pointer).
     *
     * Increment 3 (container element protocol): a 1-FIELD defstruct-lowered
     * record IS a TuriStruct pointer in the interpreter (its constructor
     * allocates one), and the compiled protocol now boxes any-width by-value
     * elements -- so `(:: (map-get m k) FzB)` on a single-field record must
     * retag too, or the field read returns the raw pointer bits.  Admit
     * nf == 1 only for a genuine lowered defstruct (from_struct_lowering --
     * never a defopaque, which has no fields and no lowering flag); the
     * structural checks below (pointer plausibility, field-count match, name
     * strcmp) still validate the pointee before the deref commits. */
    if (d->n_ctors != 1 || !d->ctors || !d->ctors[0]) return v;
    uint32_t nf = d->ctors[0]->n_fields;
    if (nf < 2 && !(nf == 1 && d->from_struct_lowering)) return v;
    uintptr_t p = (uintptr_t)(intptr_t)v.as_int;
    if (p < 0x1000) return v;
    TuriStruct *s = (TuriStruct *)p;
    if ((uintptr_t)s->name < 0x1000) return v;   /* raw carrier: word 0 is not a name ptr */
    if (s->n_fields != nf) return v;
    if (strcmp(s->name, d->name) != 0) return v;
    return turi_struct_val(s);
}

/* Apply the post-operand logic of a single-operand form whose inner expression
 * has already evaluated to `v` (assumed non-signalled; callers check
 * error/returning/throwing first).  Transparent shims fall through to `return
 * v`. */
static TuriValue eval_unary_post(TuriEnv *env, EvalFrame *frame,
                                 const Expr *e, TuriValue v) {
    switch (e->kind) {
    case EX_CAST:
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
    case EX_REINTERPRET: {
        /* A `::`/reinterpret to a bare tyvar `A` inside a generic body carries no
         * concrete kind in the interpreter (no monomorphization).  Ground it
         * through the frame's tyvar bindings so the int-carrier->float re-tag
         * still fires when A resolves to a concrete float. */
        TypeKind rk = ascribe_effective_kind(frame, &e->type);
        if ((rk == TY_FLOAT || rk == TY_FLOAT64) && v.tag == TURI_INT) {
            union { int64_t i; double d; } u; u.i = v.as_int;
            return turi_float(u.d);
        }
        return v;
    }
    case EX_ASCRIBE:
        /* `(:: e A)` inside a generic body: the tree-walker does not
         * monomorphize, so A is still a TY_TYVAR in the elaborated AST and the
         * primitive re-tag below would be skipped (default: transparent),
         * leaving the value on its int64 carrier.  Ground A through the frame's
         * tyvar bindings (pinned per call by frame_record_abi /
         * frame_bind_instance_constraint_tyvars) so a concrete element type
         * recovered at the call site re-tags the carrier, matching the compiled
         * path's monomorphized re-dispatch.  This is what lets Show[Set] /
         * Show[Map] over cstr keys render the string rather than the raw HAMT
         * carrier word (docs/archive/history/interp-hamt-key-show-dispatches-on-carrier.md). */
        switch (ascribe_effective_kind(frame, &e->type)) {
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
            if (v.tag == TURI_INT) return turi_cstr((const char *)(intptr_t)v.as_int);
            return v;
        default:
            /* By-value struct/ADT ascription: retag an int carrier that is really
             * a TuriStruct* (e.g. a struct Map VALUE) so field access reads it as
             * a struct.  No-op for every other type (guarded). */
            return try_retag_carrier_struct(frame, &e->type, v);
        }
    case EX_RETURN:
        env->returning    = true;
        env->return_value = v;
        return v;
    case EX_SET: {
        const char *name = e->as.set_.target->name->name;
        if (!eval_frame_update(frame, name, v))
            turi_env_set(env, name, v);
        return turi_nil();
    }
    default:   /* transparent shims: value passes through unchanged */
        return v;
    }
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
    /* The remaining unary_operand forms: their post-step (eval_unary_post)
     * performs nothing itself, so each performs exactly when its operand does.
     * Keeps this scan in agreement with ws_capturable's arms for the same
     * forms -- without these, a perform-free `(:: v T)` handed to a black-box
     * position read as "may perform" via the default. */
    case EX_CAST:         return ws_has_perform(e->as.cast_.expr);
    case EX_REINTERPRET:  return ws_has_perform(e->as.reinterpret_.expr);
    case EX_ASCRIBE:      return ws_has_perform(e->as.ascribe_.inner);
    case EX_BORROW_IMMUT: return ws_has_perform(e->as.borrow_immut_.expr);
    case EX_RC_FROM_REF:  return ws_has_perform(e->as.rc_from_ref_.expr);
    case EX_REF:          return ws_has_perform(e->as.ref_.expr);
    case EX_EXISTS_PACK:  return ws_has_perform(e->as.exists_pack_.value);
    case EX_UNION_INJECT: return ws_has_perform(e->as.union_inject_.value);
    case EX_RESUME:     return ws_has_perform(e->as.resume_.resume->k) ||
                               ws_has_perform(e->as.resume_.resume->value);
    /* fn-to-fat / poly wrappers are transparent: evaluating one just builds a
     * closure value (eval unwraps to `inner`, see the EX_*_TO_FAT / EX_POLY_WRAP
     * eval cases), so a wrapped bare/poly fn is no more a synchronous perform
     * than the EX_FN / EX_FN_DEF it wraps.  Without these the default arm treats
     * the wrapper as a possible perform -- e.g. a `^multishot` receiver passed to
     * `perform` (multishot-effect-cont-kv-sugar) forced the whole handle onto the
     * one-shot fiber path, which aborts on the second resume. */
    case EX_POLY_WRAP:   return ws_has_perform(e->as.poly_wrap_.inner);
    case EX_FN_TO_FAT:   return ws_has_perform(e->as.fn_to_fat_.inner);
    case EX_POLY_TO_FAT: return ws_has_perform(e->as.poly_to_fat_.inner);
    /* The driver descends both halves of a `while` (DK_WHILE), so it is as
     * transparent as an `if` -- answer from its parts rather than defaulting
     * to "may perform". */
    case EX_WHILE:
        return ws_has_perform(e->as.while_.cond) ||
               ws_has_perform(e->as.while_.body);
    default:
        /* EX_CALL, EX_HANDLE, EX_TRY_CATCH, ... -- conservatively
         * assume a perform may run synchronously. */
        return true;
    }
}

/* TURI_TRACE_FIBER_FALLBACK=1: say which form pushed a handle off the
 * work-stack (multi-shot) path onto the single-shot fiber.  The downgrade is
 * otherwise invisible until a second resume errors -- and the offending form
 * may be pages away from the handle, inside a callee the analysis descended
 * into.  `offender` is the deepest node ws_capturable rejected (NULL when the
 * rejection came from a depth/fn budget, which record nothing). */
static void ws_trace_fiber_fallback(const Expr *handle_site, const Expr *offender) {
    if (!getenv("TURI_TRACE_FIBER_FALLBACK")) return;
    if (offender)
        fprintf(stderr,
                "turi: note: handle at %u:%u falls back to the single-shot fiber "
                "effect runtime: the form at %u:%u (EX kind %d) is not "
                "work-stack capturable\n",
                handle_site->span.line, handle_site->span.col_start,
                offender->span.line, offender->span.col_start,
                (int)offender->kind);
    else
        fprintf(stderr,
                "turi: note: handle at %u:%u falls back to the single-shot fiber "
                "effect runtime (capturability analysis hit a depth or "
                "fn-count budget)\n",
                handle_site->span.line, handle_site->span.col_start);
}

/* Does any argument of this perform itself (possibly) perform?  Decides
 * whether the driver's EX_PERFORM arm must drive the args (DK_PERFORM_ARG)
 * rather than evaluating them synchronously. */
static bool ws_has_perform_args(const PerformExpr *pe) {
    for (uint32_t i = 0; i < pe->n_args; i++)
        if (ws_has_perform(pe->args[i])) return true;
    return false;
}

/* C3: cycle guard for ws_capturable's descent into recursive callee bodies.
 * A self-/mutually-recursive foldable call folds on the work-stack (DK_CALL_RET),
 * so by the inductive hypothesis its performs are capturable -- but the static
 * `depth` budget alone rejects every recursive handler (the descent re-enters the
 * same fn until depth hits 0 -> false -> fiber path, which C-recurses one
 * ucontext per level).  Recording the fns currently on the analysis path lets a
 * re-entry short-circuit to `true`, so recursive handlers run on the bounded
 * DK_PROMPT path instead. */
#define WSCAP_MAX_FNS 128
static _Thread_local const void *g_wscap_fns[WSCAP_MAX_FNS];
static _Thread_local int         g_wscap_n;

/* TURI_TRACE_FIBER_FALLBACK: the deepest form that failed the capturability
 * query in flight.  The wrapper below records the FIRST false return, which --
 * recursion unwinding bottom-up -- is the deepest offending node, so the
 * fallback trace can name the form that forced a handle onto the fiber.  The
 * analysis is a whole-subtree scan whose verdict is invisible from source;
 * without this, every gap in it presents as a silent single-shot downgrade
 * diagnosed by staring (docs/archive/turi-multishot-resume-in-while-aborts.md
 * took exactly that staring). */
static _Thread_local const Expr *g_wscap_reject;

static bool ws_capturable_rec(TuriEnv *env, EvalFrame *frame, const Expr *e, int depth);

/* Decide whether a handle body can run on the work-stack: true iff every
 * perform reachable from `e` is reached through driver-transparent forms (so
 * the perform lands in eval_drive_ex's descending switch with the prompt
 * visible on `st`).  Conservative: anything uncertain -> false -> fiber path.
 * `depth` bounds recursion through direct callee bodies. */
static bool ws_capturable(TuriEnv *env, EvalFrame *frame, const Expr *e, int depth) {
    bool ok = ws_capturable_rec(env, frame, e, depth);
    if (!ok && e && !g_wscap_reject) g_wscap_reject = e;
    return ok;
}

static bool ws_capturable_rec(TuriEnv *env, EvalFrame *frame, const Expr *e, int depth) {
    if (!e) return true;
    if (depth <= 0) return false;   /* give up on very deep / cyclic chains */
    switch (e->kind) {
    case EX_PERFORM: {
        /* Args are driven when any may perform (DK_PERFORM_ARG), so each only
         * needs to be capturable -- `(perform (Log (perform (Ask))))` no
         * longer forces the fiber path. */
        PerformExpr *pe = e->as.perform_.perform;
        for (uint32_t i = 0; i < pe->n_args; i++)
            if (!ws_capturable(env, frame, pe->args[i], depth)) return false;
        return true;
    }
    case EX_IF:
        return ws_capturable(env, frame, e->as.if_.cond, depth) &&
               ws_capturable(env, frame, e->as.if_.then_, depth) &&
               ws_capturable(env, frame, e->as.if_.else_or_null, depth);
    case EX_WHILE:
        /* DK_WHILE descends both halves, so a perform/resume in either lands
         * in the driver with the prompt visible -- exactly like an `if`.  A
         * loop that resumes per iteration is the multi-shot fold, and it is
         * the reason this case exists: without it the whole handle fell back
         * to the one-shot fiber, which aborted on the second resume. */
        return ws_capturable(env, frame, e->as.while_.cond, depth) &&
               ws_capturable(env, frame, e->as.while_.body, depth);
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
        /* The scrutinee is driven when it may perform (DK_MATCH_SCRUT), so it
         * only needs to be capturable.  GUARDS still run via eval_expr inside
         * eval_match_resolve_with and must stay perform-free. */
        if (!ws_capturable(env, frame, e->as.match_.scrutinee, depth)) return false;
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
        /* Both halves are driven now: the value since C3 (DK_RESUME), k since
         * DK_RESUME_K.  Either may perform without forcing the fiber path. */
        return ws_capturable(env, frame, e->as.resume_.resume->k, depth) &&
               ws_capturable(env, frame, e->as.resume_.resume->value, depth);
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
                /* C3: cycle detection -- a recursive call to a fn already on the
                 * analysis path folds on the work-stack, so treat it as capturable
                 * (the surrounding non-recursive parts are still checked). */
                for (int i = 0; i < g_wscap_n; i++)
                    if (g_wscap_fns[i] == (const void *)fn) return true;
                if (g_wscap_n >= WSCAP_MAX_FNS) return false;   /* too many fns: give up */
                g_wscap_fns[g_wscap_n++] = (const void *)fn;
                bool cap = ws_capturable(env, fv.as_closure->captured, fn->body, depth - 1);
                g_wscap_n--;
                return cap;
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
    /* fn-to-fat / poly wrappers are transparent (eval unwraps to `inner`); a
     * wrapped fn value is capturable to evaluate exactly when the fn it wraps is
     * -- recurse so the wrapped EX_FN / EX_VAR receiver lands on its own case
     * rather than the perform-conservative default. */
    case EX_POLY_WRAP:   return ws_capturable(env, frame, e->as.poly_wrap_.inner, depth);
    case EX_FN_TO_FAT:   return ws_capturable(env, frame, e->as.fn_to_fat_.inner, depth);
    case EX_POLY_TO_FAT: return ws_capturable(env, frame, e->as.poly_to_fat_.inner, depth);
    /* Leaves / values with no synchronous perform. */
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT: case EX_FLOAT_LIT:
    case EX_CSTR_LIT: case EX_SYM_LIT: case EX_VAR: case EX_DEFAULT_OF:
    case EX_HANDLER_LIT: case EX_EXTERN_C:
        return true;
    /* Single-operand forms the driver drives (DK_UNARY via unary_operand, and
     * DK_GET_FIELD for the field receiver -- SR N2/N3): a perform in the
     * operand lands in the descending switch with the prompt visible, so
     * recurse rather than demanding the operand be perform-free.  These arms
     * were written before the driver grew those descents and were the "stale
     * black box" half of docs/archive/turi-ws-capturable-stale-black-box-arms.md
     * -- `(set! acc (+ acc (resume k i)))`, the natural accumulator, silently
     * downgraded its handler to the single-shot fiber. */
    case EX_SET:        return ws_capturable(env, frame, e->as.set_.value, depth);
    case EX_RETURN:     return ws_capturable(env, frame, e->as.return_.value, depth);
    case EX_GET_FIELD:  return ws_capturable(env, frame, e->as.get_field_.struct_expr, depth);
    case EX_CAST:         return ws_capturable(env, frame, e->as.cast_.expr, depth);
    case EX_REINTERPRET:  return ws_capturable(env, frame, e->as.reinterpret_.expr, depth);
    case EX_ASCRIBE:      return ws_capturable(env, frame, e->as.ascribe_.inner, depth);
    case EX_BORROW_IMMUT: return ws_capturable(env, frame, e->as.borrow_immut_.expr, depth);
    case EX_RC_FROM_REF:  return ws_capturable(env, frame, e->as.rc_from_ref_.expr, depth);
    case EX_REF:          return ws_capturable(env, frame, e->as.ref_.expr, depth);
    case EX_EXISTS_PACK:  return ws_capturable(env, frame, e->as.exists_pack_.value, depth);
    case EX_UNION_INJECT: return ws_capturable(env, frame, e->as.union_inject_.value, depth);
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
static int eval_match_resolve_with(TuriEnv *env, EvalFrame *frame, const Expr *e,
                                   TuriValue val,
                                   EvalFrame **out_frame, const Expr **out_body,
                                   TuriValue *out_val);

static int eval_match_resolve(TuriEnv *env, EvalFrame *frame, const Expr *e,
                              EvalFrame **out_frame, const Expr **out_body,
                              TuriValue *out_val) {
    TuriValue val = eval_expr(env, frame, e->as.match_.scrutinee);
    if (turi_is_error(val) || env_signaled(env)) {
        *out_val = val; return -1;
    }
    return eval_match_resolve_with(env, frame, e, val, out_frame, out_body, out_val);
}

/* The arm-selection half of eval_match_resolve, taking a pre-computed
 * scrutinee value.  Split out so the driver can DRIVE the scrutinee on the
 * work-stack (DK_MATCH_SCRUT) -- with the prompt visible to a perform inside
 * it -- and resolve the arm when the value returns.  Guards still run via
 * eval_expr here; ws_capturable's EX_MATCH arm keeps requiring them
 * perform-free. */
static int eval_match_resolve_with(TuriEnv *env, EvalFrame *frame, const Expr *e,
                                   TuriValue val,
                                   EvalFrame **out_frame, const Expr **out_body,
                                   TuriValue *out_val) {
    MatchArm *arms   = e->as.match_.arms;
    uint32_t  n_arms = e->as.match_.n_arms;

    /* turi-session-types-plan (Slice C): session offer / recv-timeout match.
     * The scrutinee (tur_session_recv_tag / tur_session_recv_timeout) returns a
     * branch tag as an int (0 = Left, 1 = Right); the matching arm binds the
     * *channel* -- not the tag -- to its arm variable (mirrors the compiled
     * TY_SESSION_OFFER match in emit_expr.c).  The channel is the scrutinee
     * inline-C's val_exprs[0], re-evaluated here (a pure endpoint read). */
    if (e->as.match_.scrutinee->type.kind == TY_SESSION_OFFER) {
        int64_t     tag   = (val.tag == TURI_INT) ? val.as_int : -1;
        const Expr *scrut = e->as.match_.scrutinee;
        TuriValue   chan  = turi_nil();
        if (scrut->kind == EX_INLINE_C) {
            InlineC *sic = scrut->as.inline_c_.inline_c;
            if (sic->n_val_exprs > 0 && sic->val_exprs[0]) {
                chan = eval_expr(env, frame, sic->val_exprs[0]);
                if (turi_is_error(chan) || env_signaled(env)) {
                    *out_val = chan; return -1;
                }
            }
        }
        for (uint32_t ai = 0; ai < n_arms; ai++) {
            MatchArm     *arm = &arms[ai];
            MatchPattern *pat = &arm->pattern;
            if (!pat->is_wildcard && pat->union_member_idx >= 0 &&
                pat->union_member_idx != (int)tag)
                continue;
            EvalFrame *arm_frame = eval_frame_new(env, frame);
            if (pat->n_bindings > 0 && pat->bindings[0])
                frame_bind(env, arm_frame, pat->bindings[0]->name->name, chan);
            if (arm->guard) {
                TuriValue gv = eval_expr(env, arm_frame, arm->guard);
                if (turi_is_error(gv) || env_signaled(env)) {
                    eval_frame_free(arm_frame); *out_val = gv; return -1;
                }
                if (gv.tag != TURI_BOOL || !gv.as_bool) {
                    eval_frame_free(arm_frame); continue;
                }
            }
            *out_frame = arm_frame; *out_body = arm->body; return 1;
        }
        return 0;
    }
    for (uint32_t ai = 0; ai < n_arms; ai++) {
        MatchArm     *arm = &arms[ai];
        MatchPattern *pat = &arm->pattern;
        bool          matched   = false;
        EvalFrame    *arm_frame = NULL;

        if (pat->is_wildcard) {
            matched = true; arm_frame = eval_frame_new(env, frame);
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
                matched = true; arm_frame = eval_frame_new(env, frame);
                if (pat->var_sym) frame_bind(env, arm_frame, pat->var_sym->name, val);
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
            if (matched) arm_frame = eval_frame_new(env, frame);
        } else if (pat->is_var) {
            matched = true; arm_frame = eval_frame_new(env, frame);
            frame_bind(env, arm_frame, pat->var_sym->name, val);
        } else {
            CtorDef *ctor = pat->ctor;
            if (ctor && val.tag == TURI_STRUCT &&
                strcmp(val.as_struct->name, ctor->name) == 0) {
                matched = true; arm_frame = eval_frame_new(env, frame);
                for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                    Binding *b = pat->bindings[bi];
                    if (b && bi < val.as_struct->n_fields)
                        frame_bind(env, arm_frame, b->name->name, val.as_struct->fields[bi]);
                }
            }
        }

        if (matched) {
            if (arm->guard) {
                TuriValue gv = eval_expr(env, arm_frame, arm->guard);
                if (turi_is_error(gv) || env_signaled(env)) {
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

/* True if a do/program block directly registers a defer (an EX_DEFER item).
 * Such a block is its own defer scope (the compiler emits a tur_frame for it),
 * so the driver pushes a scope-boundary marker on entry to delimit its defers
 * from enclosing scopes' defers when the by-scope (early-exit) walk fires them.
 * Blocks with no direct defer push no marker -- the common case stays cheap. */
static inline bool seq_has_direct_defer(const Expr *e) {
    Expr   **items = drive_seq_items(e);
    uint32_t n     = drive_seq_n(e);
    for (uint32_t i = 0; i < n; i++)
        if (items[i]->kind == EX_DEFER) return true;
    return false;
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
        if (env->debugger) turi_dbg_push(env, (const FnDef *)seed->dbg_fn, frame);
        tail = true;
    }

    /* SR: pending closure-application request from a DK_NATIVE_RESUME native.
     * When set, the loop dispatches the application of `apply_fn` to the
     * heap-owned `apply_args` onto the work-stack (feeding the DK_NATIVE_RESUME
     * frame already pushed beneath it) before resuming ordinary descent. */
    bool        have_apply = false;
    TuriValue   apply_fn   = turi_nil();
    TuriValue  *apply_args = NULL;
    uint32_t    apply_n    = 0;

    /* Side channel from DK_PERFORM_ARG back to the descending EX_PERFORM arm:
     * when a perform's args were driven on the work-stack, the return leg
     * routes control back to the EX_PERFORM with the evaluated args here.
     * Driver-instance locals (not globals) so a re-entrant eval under a
     * debugger hook or native callee cannot clobber an in-flight handoff. */
    const Expr *pargs_for  = NULL;
    TuriValue  *pargs_heap = NULL;

    for (;;) {
        if (have_apply) {
            /* Apply `apply_fn` to apply_args[0..apply_n).  The result returns to
             * the DK_NATIVE_RESUME frame beneath, so this is ALWAYS a non-tail
             * application (never reuse the enclosing activation's DK_CALL_RET). */
            have_apply = false;
            TuriValue  clv = apply_fn;
            TuriValue *acc = apply_args;
            uint32_t   n   = apply_n;
            apply_args = NULL;
            if (clv.tag != TURI_CLOSURE || !clv.as_closure) {
                cur = turi_error("eval: native-resume apply: not a closure");
                free(acc); descending = false; continue;
            }
            TuriClosure *cl = clv.as_closure;
            FnDef       *fn = (FnDef *)cl->fn;
            bool foldable = !cl->native && fn && fn->body &&
                            fn->body->kind != EX_INLINE_C;
            if (!foldable) {
                if (cl->native == native_resume_cont) {
                    /* SR N4 Slice 6: a fold-requested application that is itself
                     * resume-cont! -- start a nested fold on the work-stack
                     * (push a fresh DK_CONT_FOLD above the requester). */
                    TuriCont *c = (n >= 1 && acc[0].as_int)
                                ? (TuriCont *)(intptr_t)acc[0].as_int : NULL;
                    int64_t w = (n >= 2) ? acc[1].as_int : 0;
                    free(acc);
                    ContFoldState *s; TuriValue val, ffn, *fargs; uint32_t fn_n;
                    int rc = cont_fold_begin(c, w, &s, &val, &ffn, &fargs, &fn_n);
                    if (rc != 1) { cur = val; descending = false; continue; }
                    DRIVE_PUSH(((DriveCont){ .kind = DK_CONT_FOLD, .aux = s }));
                    apply_fn = ffn; apply_args = fargs; apply_n = fn_n;
                    have_apply = true;
                    continue;
                }
                /* Leaf (native / inline-C): dispatched synchronously via
                 * eval_apply, exactly as the DK_CALL_ARG leaf path does. */
                cur = eval_apply(env, cl, acc, n);
                free(acc); descending = false; continue;
            }
            uint32_t param_offset     = cl->skip_env_param ? 1u : 0u;
            uint32_t effective_params = (uint32_t)fn->n_params - param_offset;
            if (effective_params != n) {
                cur = turi_errorf("eval: arity mismatch: %s expects %u args, got %u",
                                  fn->binding ? fn->binding->name->name : "<fn>",
                                  (unsigned)effective_params, (unsigned)n);
                free(acc); descending = false; continue;
            }
            if (env->step_fuel_limit > 0) {
                if (env->step_fuel == 0) {
                    cur = turi_error("eval: step fuel exhausted");
                    free(acc); descending = false; continue;
                }
                env->step_fuel--;
            }
            EvalFrame *call_frame = eval_frame_new(env, (EvalFrame *)cl->captured);
            for (uint32_t i = 0; i < n; i++)
                frame_bind(env, call_frame,
                           fn->params[param_offset + i]->name->name,
                           turi_copy_byvalue_struct_arg(env, acc[i]));
            free(acc);
            DRIVE_PUSH(((DriveCont){ .kind = DK_CALL_RET, .frame = call_frame,
                                     .aux = env->defer_stack,
                                     .saved_module  = env->current_module,
                                     .was_returning = env->returning,
                                     .was_no_unwind = env->in_no_unwind }));
            if (env->debugger) turi_dbg_push(env, fn, call_frame);
            env->current_module = cl->module;
            env->returning      = false;
            env->in_no_unwind   = fn->binding && fn->binding->no_unwind;
            control = fn->body; cf = call_frame; tail = true; descending = true;
            continue;
        }
        if (descending) {
            /* Debugger Phase 2: control nodes the driver folds (nested if / do /
             * let / call / match bodies) never pass through eval_expr, so the
             * per-node check is mirrored here. */
            if (env->debugger)
                turi_dbg_before_node(env, cf, control, /*from_driver=*/true);
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
                     * DK_CALL_RET directly (no DK_DO_SEQ to pop first).  (A sole
                     * EX_DEFER item is excluded, so no scope marker is missed.) */
                    control = items[0];   /* cf unchanged; tail stays true */
                    break;
                }
                /* A do-block that registers defers is its own defer scope: push a
                 * boundary marker so its defers stay grouped (same-scope LIFO,
                 * outer scopes first) when fired at an early exit. */
                if (seq_has_direct_defer(de))
                    defer_push_scope_marker(env);
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
                EvalFrame *nf = eval_frame_new(env, cf);
                uint32_t   n  = control->as.let_.n;
                if (control->kind == EX_LETREC) {
                    for (uint32_t i = 0; i < n; i++)
                        frame_bind(env, nf,
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
                     * enclosing DK_CALL_RET (matching the retired eval_body_tco's
                     * tail leak).  The body's do-block pushes the scope-boundary
                     * marker (see EX_DO/EX_PROGRAM) when it holds defers. */
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
                    cur = make_struct_val_def(env, "<struct>", 0, NULL);
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
                /* jit-ffi-c2mir-plan F3/F5: a `(call-ptr ...)` node has no turi
                 * callee to resolve -- the target is a raw C address.
                 * Dispatch synchronously through the thunk provider; the
                 * scalar args recurse via eval_expr, which is fine at FFI
                 * arg depth. */
                if (control->as.call_.ptr_sig) {
                    cur = control->as.call_.ptr_sig->is_callback
                              ? eval_callback_ptr(env, cf, control)
                              : eval_call_ptr(env, cf, control);
                    descending = false;
                    break;
                }
                /* T3.2a: resolve the callee here, accumulate args on the
                 * work-stack, then apply via eval_apply (still recursive -- the
                 * body-in-loop + TCO fold is T3.2b).  The closure value rides in
                 * `last`; the arg accumulator in `aux`. */
                TuriValue fn_val;
                bool gde_resolved = false;
                /* turi-dict-passing-plan (Piece 2): a method call dispatched on
                 * the constraint's own type variable, inside an activation that
                 * carries a runtime dictionary for the method's class, resolves
                 * through THAT dictionary -- the caller-supplied instance --
                 * with explicit precedence over every recovery heuristic below.
                 * The tyvar gate mirrors the emit-side env-dict gate
                 * (emit_call_dict_env_dispatch_index): receiver-directed keys
                 * on a bare-tyvar receiver, return-directed on a tyvar-headed
                 * result; a concrete same-class call in the same body stays
                 * instance-resolved. */
                if (control->as.call_.dict_arg &&
                    control->as.call_.dict_arg->kind == EX_DICT &&
                    control->as.call_.dict_arg->as.dict_.instance &&
                    control->as.call_.dict_arg->as.dict_.instance->typeclass &&
                    control->as.call_.dict_arg->as.dict_.method_name[0] != '\0') {
                    bool recv_is_tyvar =
                        control->as.call_.n_args >= 1 && control->as.call_.args &&
                        control->as.call_.args[0] &&
                        control->as.call_.args[0]->type.kind == TY_TYVAR;
                    const Type *h = &control->type;
                    while (h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
                    if (recv_is_tyvar || h->kind == TY_TYVAR) {
                        TypeClass *mtc =
                            control->as.call_.dict_arg->as.dict_.instance->typeclass;
                        /* Key the lookup by the dispatch tyvar's NAME so two
                         * same-class dictionaries on the frame (`[^Show K
                         * ^Show V]`) resolve to the one this call dispatches
                         * on; a class-only walk here handed V's dictionary to
                         * a K-directed method (map keys rendered via the
                         * value instance). */
                        const char *disp_tv =
                            recv_is_tyvar
                                ? control->as.call_.args[0]->type.as.tyvar_.name
                                : h->as.tyvar_.name;
                        struct TypeClassInstance *bound =
                            frame_lookup_dict_tyvar(cf, mtc, disp_tv);
                        if (bound) {
                            TuriValue rv = gde_method_closure(
                                env, control->as.call_.dict_arg, mtc, bound);
                            if (rv.tag == TURI_CLOSURE) {
                                fn_val = rv;
                                gde_resolved = true;
                            }
                        }
                    }
                }
                /* generic-dict-dispatch: a typeclass method baked to the carrier
                 * representative (dict_arg set, receiver tyvar in abi_bindings[0])
                 * re-resolves to the receiver's real instance using the concrete
                 * type the enclosing generic call pinned onto that tyvar. */
                if (!gde_resolved && control->as.call_.dict_arg &&
                    control->as.call_.n_abi_bindings >= 1 &&
                    control->as.call_.fn_binding) {
                    AbiTypeBinding *ab0 = &control->as.call_.abi_bindings[0];
                    if (ab0->type.kind == TY_TYVAR && ab0->type.as.tyvar_.name) {
                        /* turi-dict-passing-plan (plain constrained generics):
                         * a method call still dispatched on a type variable
                         * reads the enclosing activation's frame dictionary,
                         * pushed at apply time by frame_bind_constraint_dicts
                         * and keyed by the dispatch tyvar's name.  This is the
                         * sole static re-resolution path -- the head-name
                         * recovery heuristic it used to fall back to is
                         * retired (see the note at its former definition). */
                        if (control->as.call_.dict_arg->kind == EX_DICT &&
                            control->as.call_.dict_arg->as.dict_.instance &&
                            control->as.call_.dict_arg->as.dict_.instance->typeclass) {
                            TypeClass *mtc2 = control->as.call_.dict_arg
                                                  ->as.dict_.instance->typeclass;
                            struct TypeClassInstance *bound2 =
                                frame_lookup_dict_tyvar(cf, mtc2,
                                                        ab0->type.as.tyvar_.name);
                            if (bound2) {
                                TuriValue rv = gde_method_closure(
                                    env, control->as.call_.dict_arg, mtc2, bound2);
                                if (rv.tag == TURI_CLOSURE) {
                                    fn_val = rv;
                                    gde_resolved = true;
                                }
                            }
                        }
                    }
                }
                /* unascribed-carrier-helper-read-collapses-element-tyvar
                 * (turi mirror of emit_reresolve_disp_type's last branch): the
                 * receiver may be an UNASCRIBED generic carrier-helper read --
                 * `(tag (vec-get v 0))` -- whose declared `:R` result collapsed
                 * to the int64 carrier at elaboration, so neither tyvar gate
                 * above fires.  Recover the dispatch tyvar from the helper's
                 * own signature: its `result_full_type` is the type-param `R`,
                 * and `R` also appears inside a parameter's declared full type
                 * (`v : (Vec R)`); matching that pattern against the ACTUAL
                 * argument's static type yields the enclosing body's element
                 * var (`A`), whose frame dictionary -- pushed at apply time
                 * from the instance's `[(Tag A)]` constraints -- names the
                 * right instance.  This closed the last reliance on the
                 * runtime-tag heuristic (gde_reresolve_method_by_value). */
                if (!gde_resolved && control->as.call_.dict_arg &&
                    control->as.call_.dict_arg->kind == EX_DICT &&
                    control->as.call_.dict_arg->as.dict_.instance &&
                    control->as.call_.dict_arg->as.dict_.instance->typeclass &&
                    control->as.call_.dict_arg->as.dict_.method_name[0] != '\0' &&
                    control->as.call_.n_args >= 1 && control->as.call_.args) {
                    const Expr *recv = control->as.call_.args[0];
                    while (recv && recv->kind == EX_ASCRIBE)
                        recv = recv->as.ascribe_.inner;
                    if (recv && recv->kind == EX_CALL &&
                        recv->as.call_.fn_binding && recv->as.call_.args) {
                        const Type *ft = &recv->as.call_.fn_binding->type;
                        if (ft->kind == TY_FN && ft->as.fn.arg_full_types &&
                            ft->as.fn.result_full_type &&
                            ft->as.fn.result_full_type->kind == TY_TYVAR &&
                            ft->as.fn.result_full_type->as.tyvar_.name) {
                            const char *rname =
                                ft->as.fn.result_full_type->as.tyvar_.name;
                            uint32_t np = ft->as.fn.arity;
                            Type extracted;
                            bool have_ex = false;
                            for (uint8_t pi = 0;
                                 pi < np && pi < recv->as.call_.n_args; pi++) {
                                const Type *pft = ft->as.fn.arg_full_types[pi];
                                const Expr *ae = recv->as.call_.args[pi];
                                if (!pft || !ae) continue;
                                if (turi_pattern_extract_var(pft, &ae->type,
                                                             rname, &extracted)) {
                                    have_ex = true;
                                    break;
                                }
                            }
                            if (have_ex && extracted.kind == TY_TYVAR &&
                                extracted.as.tyvar_.name) {
                                TypeClass *mtc3 = control->as.call_.dict_arg
                                                      ->as.dict_.instance->typeclass;
                                struct TypeClassInstance *bound3 =
                                    frame_lookup_dict_tyvar(
                                        cf, mtc3, extracted.as.tyvar_.name);
                                if (bound3) {
                                    TuriValue rv = gde_method_closure(
                                        env, control->as.call_.dict_arg, mtc3,
                                        bound3);
                                    if (rv.tag == TURI_CLOSURE) {
                                        fn_val = rv;
                                        gde_resolved = true;
                                    }
                                }
                            }
                        }
                    }
                }
                /* Return-directed methods (`pure`, `empty`, `default-of`) are
                 * served by the DictBind path above (turi-dict-passing-plan);
                 * the frame-tyvar recovery that used to sit here
                 * (gde_reresolve_return_directed) is retired -- see the
                 * measurement note at its former definition. */
                if (!gde_resolved && control->as.call_.fn_binding) {
                    fn_val = eval_lookup(env, cf,
                                 control->as.call_.fn_binding->name->name);
                } else if (!gde_resolved && control->as.call_.fn_expr) {
                    fn_val = eval_expr(env, cf, control->as.call_.fn_expr);
                    fn_val = reword_unbound_call_head(fn_val, control->as.call_.fn_expr);
                } else if (!gde_resolved) {
                    cur = turi_error("eval: call with no function");
                    descending = false; break;
                }
                if (turi_is_error(fn_val) || env_signaled(env)) {
                    cur = fn_val; descending = false; break;
                }
                if (!gde_resolved && control->as.call_.fn_binding)
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
                /* A scrutinee that may perform is driven on the work-stack
                 * (DK_MATCH_SCRUT), so its perform reaches the prompt scan;
                 * arm selection then runs on the value's return.  The common
                 * perform-free scrutinee keeps the synchronous resolve below
                 * -- no extra frame on the hot match path. */
                if (ws_has_perform(control->as.match_.scrutinee)) {
                    DRIVE_PUSH(((DriveCont){ .kind = DK_MATCH_SCRUT, .expr = control,
                                             .frame = cf, .tail = tail }));
                    control = control->as.match_.scrutinee;
                    tail = false; descending = true;
                    break;
                }
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
                g_wscap_reject = NULL;
                if (ws_capturable(env, cf, h->body, 64)) {
                    bool ok = true;
                    for (uint8_t i = 0; i < h->n_cases && ok; i++)
                        ok = ws_capturable(env, cf, h->cases[i].body, 64);
                    if (ok) {
                        DRIVE_PUSH(((DriveCont){ .kind = DK_PROMPT, .aux = (void *)h,
                                                 .frame = cf, .tail = tail, .index = 1,
                                                 .saved_module = env->current_module,
                                                 .was_no_unwind = env->in_no_unwind }));
                        control = h->body; tail = false;   /* body runs under the prompt */
                        break;                             /* keep descending */
                    }
                }
                ws_trace_fiber_fallback(control, g_wscap_reject);
                cur = eval_handle(env, cf, h);   /* fiber fallback */
                descending = false;
                break;
            }
            case EX_WITH_HANDLER: {
                /* DC: materialise a HandleExpr from the handler value and run it
                 * on the work-stack when capturable; else fiber-fallback. */
                TuriValue hvv = eval_expr(env, cf, control->as.with_handler_.handler);
                if (turi_is_error(hvv) || env_signaled(env)) {
                    cur = hvv; descending = false; break;
                }
                if (hvv.tag != TURI_HANDLER) {
                    cur = turi_error("eval: with-handler: first argument must be a handler value");
                    descending = false; break;
                }
                TuriHandlerVal *hv = hvv.as_handler;
                /* Heap-own the HandleExpr + cases: a captured continuation may
                 * outlive this driver frame, so they cannot live on the C stack. */
                HandleExpr *h = (HandleExpr *)turi_val_alloc(env, sizeof(HandleExpr));
                HandleCase *cs = (HandleCase *)turi_val_alloc(env, (size_t)hv->n_cases * sizeof(HandleCase));
                for (uint8_t i = 0; i < hv->n_cases; i++) cs[i] = *hv->cases[i];
                h->body = control->as.with_handler_.body;
                h->cases = cs; h->n_cases = hv->n_cases;
                h->is_unsafe_marker = false;
                g_wscap_reject = NULL;
                bool ok = ws_capturable(env, cf, h->body, 64);
                for (uint8_t i = 0; i < h->n_cases && ok; i++)
                    ok = ws_capturable(env, cf, h->cases[i].body, 64);
                if (ok) {
                    DRIVE_PUSH(((DriveCont){ .kind = DK_PROMPT, .aux = (void *)h,
                                             .frame = cf, .tail = tail, .index = 1,
                                             .saved_module = env->current_module,
                                             .was_no_unwind = env->in_no_unwind }));
                    control = h->body; tail = false;
                    break;
                }
                ws_trace_fiber_fallback(control, g_wscap_reject);
                cur = eval_handle(env, cf, h);   /* fiber fallback (borrows h) */
                descending = false;
                break;
            }
            case EX_PERFORM: {
                PerformExpr *pe = control->as.perform_.perform;
                const char  *effect_name = pe->effect_name->name;
                TuriValue pargs[EVAL_MAX_FN_ARITY];
                uint32_t n = pe->n_args;
                if (n > EVAL_MAX_FN_ARITY) {
                    cur = turi_errorf("eval: too many effect arguments (%u)", n);
                    descending = false; break;
                }
                if (pargs_for == control) {
                    /* Second visit: DK_PERFORM_ARG drove every arg and routed
                     * back here with the values in the driver-local side
                     * channel.  Copy them into the C-stack array -- the fiber
                     * path below swapcontexts away with `pargs` still read by
                     * the handler, so the storage must outlive this iteration
                     * exactly as the synchronous path's does. */
                    for (uint8_t i = 0; i < n; i++) pargs[i] = pargs_heap[i];
                    free(pargs_heap);
                    pargs_heap = NULL; pargs_for = NULL;
                } else if (n > 0 && ws_has_perform_args(pe)) {
                    /* An arg may itself perform: drive the args on the
                     * work-stack (DK_PERFORM_ARG) so the inner perform sees
                     * the prompt, then come back through the branch above. */
                    DRIVE_PUSH(((DriveCont){ .kind = DK_PERFORM_ARG, .expr = control,
                                             .frame = cf, .index = 0,
                                             .aux = malloc(n * sizeof(TuriValue)),
                                             .tail = tail }));
                    control = pe->args[0]; tail = false; descending = true;
                    break;
                } else {
                    bool sig = false;
                    for (uint8_t i = 0; i < n; i++) {
                        pargs[i] = eval_expr(env, cf, pe->args[i]);
                        if (turi_is_error(pargs[i]) || env_signaled(env)) {
                            cur = pargs[i]; sig = true; break;
                        }
                    }
                    if (sig) { descending = false; break; }
                }
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
                /* Escaping payload: bound as the multishot k; pool-owned. */
                TuriWsCont *wc = (TuriWsCont *)turi_val_calloc(env, sizeof(TuriWsCont));
                wc->n_frames = nf;
                if (nf) {
                    wc->frames = (DriveCont *)turi_val_alloc(env, nf * sizeof(DriveCont));
                    memcpy(wc->frames, &st[pidx + 1], nf * sizeof(DriveCont));
                    /* Re-home each captured frame's malloc'd arg accumulator
                     * (DK_BUILTIN_ARG / DK_CALL_ARG / DK_MAKE_STRUCT `.aux`)
                     * into pool memory.  The slice slots st[pidx+1 ..] are
                     * abandoned when `len` truncates below, so after the memcpy
                     * these malloc'd arrays are owned ONLY by wc->frames -- but
                     * wc is pool-allocated and the driver's normal free(acc)
                     * epilogue never runs for the truncated slots, so without
                     * this they leak, growing O(performs)
                     * (turi-ws-perform-capture-accumulator-leak).  A resume
                     * deep-copies each accumulator afresh via clone_ws_slice
                     * (fresh malloc, freed by the driver as usual), so the pool
                     * copy is only ever the capture-time original, reclaimed at
                     * env teardown like the rest of the continuation. */
                    for (size_t i = 0; i < nf; i++) {
                        if (!wc->frames[i].aux) continue;
                        size_t cnt = 0;
                        switch (wc->frames[i].kind) {
                        case DK_BUILTIN_ARG:
                            cnt = wc->frames[i].expr->as.builtin.n; break;
                        case DK_CALL_ARG:
                            cnt = wc->frames[i].expr->as.call_.n_args; break;
                        case DK_MAKE_STRUCT:
                            cnt = wc->frames[i].expr->as.make_struct_.n_fields; break;
                        case DK_PERFORM_ARG:
                            cnt = wc->frames[i].expr->as.perform_.perform->n_args; break;
                        default: break;  /* aux is a defer mark / boundary, not owned here */
                        }
                        if (!cnt) continue;
                        TuriValue *pool_acc =
                            (TuriValue *)turi_val_alloc(env, cnt * sizeof(TuriValue));
                        memcpy(pool_acc, wc->frames[i].aux, cnt * sizeof(TuriValue));
                        free(wc->frames[i].aux);
                        wc->frames[i].aux = pool_acc;
                    }
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
                EvalFrame *hf = eval_frame_new(env, st[pidx].frame);
                for (uint32_t i = 0; i < matched->n_params && i < n; i++)
                    frame_bind(env, hf, matched->param_bindings[i]->name->name, pargs[i]);
                frame_bind(env, hf, matched->k_binding->name->name, turi_ws_cont_val(env, wc));
                env->current_module = st[pidx].saved_module;
                env->in_no_unwind   = st[pidx].was_no_unwind;
                /* The case body runs delimited by DK_PROMPT@pidx: its value flows
                 * to the prompt (which restores the env boundary and propagates),
                 * NOT to an enclosing DK_CALL_RET.  So it must run NON-tail, even
                 * when the handle itself was in tail position (st[pidx].tail).  A
                 * tail call in the body would otherwise try to reuse st[len-2] as
                 * its activation, but st[len-2] is the DK_PROMPT, not a
                 * DK_CALL_RET -- tripping the tail-fold invariant assert
                 * (multishot-effect-cont-kv-sugar: a `^multishot` handler whose
                 * case body `(f k)` is a tail call).  The prompt still forwards
                 * the value to the true (possibly tail) continuation below it, so
                 * TCO of the enclosing call is preserved; only the one direct call
                 * in the case body costs a single DK_CALL_RET frame. */
                control = matched->body; cf = hf; tail = false;
                /* descending stays true */
                break;
            }
            case EX_RESUME: {
                /* Drive k itself first (DK_RESUME_K), then the value
                 * (DK_RESUME).  Both halves on the work-stack means neither
                 * can hide a perform from the prompt scan. */
                ResumeExpr *re = control->as.resume_.resume;
                DRIVE_PUSH(((DriveCont){ .kind = DK_RESUME_K, .expr = control,
                                         .frame = cf, .tail = tail }));
                control = re->k; tail = false; descending = true;
                break;
            }
            case EX_GET_FIELD: {
                /* SR N2: descend the receiver on the work-stack (non-tail) so
                 * recursion threaded through a field accessor stays heap-bounded
                 * -- the option-map/.value blocker.  DK_GET_FIELD applies the
                 * field extraction when the receiver value returns. */
                DRIVE_PUSH(((DriveCont){ .kind = DK_GET_FIELD, .expr = control,
                                         .frame = cf }));
                control = control->as.get_field_.struct_expr;
                tail = false;   /* receiver is non-tail */
                break;          /* keep descending */
            }
            case EX_TVAR_MODIFY: {
                /* SR N1: read-modify-write via the work-stack resume protocol.
                 * Read old, then ask the driver to apply fn(old) -- when it
                 * completes, tvar_modify_resume commits the result and yields
                 * old.  The user fn no longer runs on a re-entrant C frame; its
                 * own recursion folds onto the work-stack.  (eval_expr_impl keeps
                 * a synchronous EX_TVAR_MODIFY for non-driver callers.) */
                if (!g_stm_tx) {
                    cur = turi_error("eval: tvar/modify used outside of an atomically block");
                    descending = false; break;
                }
                TuriValue err = turi_nil();
                TuriTVar *tv = stm_eval_tvar(env, cf, control->as.tvar_modify_.tvar, &err);
                if (!tv) { cur = err; descending = false; break; }
                TuriValue fn = eval_expr(env, cf, control->as.tvar_modify_.fn);
                if (turi_is_error(fn) || env_signaled(env)) {
                    cur = fn; descending = false; break;
                }
                int64_t old = stm_read(g_stm_tx, tv);
                TvarModifyState *s = (TvarModifyState *)malloc(sizeof(TvarModifyState));
                s->tv = tv; s->old = old;
                NativeResume *nr = (NativeResume *)malloc(sizeof(NativeResume));
                nr->resume = tvar_modify_resume; nr->state = s;
                DRIVE_PUSH(((DriveCont){ .kind = DK_NATIVE_RESUME, .aux = nr }));
                apply_fn   = fn;
                apply_args = (TuriValue *)malloc(sizeof(TuriValue));
                apply_args[0] = turi_int(old);
                apply_n    = 1;
                have_apply = true;
                /* descending stays true; the have_apply branch at the loop top
                 * dispatches the application next iteration. */
                break;
            }
            case EX_ATOMICALLY: {
                /* C2: model the transaction boundary on the work-stack (no
                 * eval_atomically C frame), so nested atomically folds onto the
                 * heap.  Register a heap TuriStmTx on g_stm_tx (so the tvar/retry
                 * ops still find it), push DK_ATOMICALLY, and drive the (stm ...)
                 * body beneath it.  DK_ATOMICALLY commits (or errors on retry)
                 * when the body value returns.  (eval_expr_impl keeps a
                 * synchronous EX_ATOMICALLY -> eval_atomically for non-driver
                 * callers, e.g. an or-else arm that recurses into atomically.) */
                TuriStmTx *tx = (TuriStmTx *)calloc(1, sizeof(TuriStmTx));
                tx->prev = g_stm_tx;
                g_stm_tx = tx;
                DRIVE_PUSH(((DriveCont){ .kind = DK_ATOMICALLY, .aux = tx }));
                control = control->as.atomically_.stm_expr;   /* the EX_STM */
                tail = false; descending = true;
                break;
            }
            case EX_WHILE:
                /* Drive the loop on the work-stack so a perform/resume in the
                 * condition or the body reaches the driver with the enclosing
                 * DK_PROMPT visible.  Start in phase 0 by descending the
                 * condition; DK_WHILE alternates from there. */
                DRIVE_PUSH(((DriveCont){ .kind = DK_WHILE, .expr = control,
                                         .frame = cf, .index = 0 }));
                control = control->as.while_.cond;
                tail = false; descending = true;
                break;
            case EX_STM: {
                /* C2: drive the stm body sequence on the work-stack so recursion
                 * inside an item folds (DK_STM_SEQ).  A retry/abort request
                 * short-circuits the rest, matching the eval_expr_impl EX_STM
                 * loop.  The last item's value is the block's value. */
                uint32_t n = control->as.stm_.n_body;
                if (n == 0) { cur = turi_nil(); descending = false; break; }
                DRIVE_PUSH(((DriveCont){ .kind = DK_STM_SEQ, .expr = control,
                                         .frame = cf, .index = 0 }));
                control = control->as.stm_.body[0];
                tail = false; descending = true;   /* stm items are non-tail */
                break;
            }
            case EX_RESET: {
                /* SR N4: model the plain reset boundary on the work-stack (no
                 * setjmp), so deeply-nested resets fold onto the heap instead of
                 * one eval_reset_boundary C frame per level.  Register a heap
                 * boundary on g_reset_stack (so reset_find / abortive-shift error
                 * checks + kind matching still work), push DK_RESET, and drive
                 * the body beneath it.  DK_RESET consumes a matching env->aborting
                 * signal when the body value returns. */
                TuriResetBoundary *b = (TuriResetBoundary *)malloc(sizeof(TuriResetBoundary));
                b->kind                = PROMPT_PLAIN;
                b->result              = turi_nil();
                b->saved_handler_stack = env->handler_stack;
                b->saved_defer_stack   = env->defer_stack;
                b->prev                = g_reset_stack;
                g_reset_stack = b;
                DRIVE_PUSH(((DriveCont){ .kind = DK_RESET, .aux = b }));
                control = control->as.reset_.body;
                tail = false;   /* body is non-tail: DK_RESET must see its value */
                break;          /* keep descending */
            }
            case EX_SERIAL_RESET:
            case EX_CLONEABLE_RESET: {
                /* SR N4 Slice 3: a serial-/cloneable-reset whose body does NOT
                 * reach its capturing shift is just a prompt boundary -- model it
                 * on the work-stack (DK_RESET, kind PROMPT_SERIAL/CLONEABLE) so
                 * nested resets fold onto the heap.  The capturing path
                 * (ts_capture_and_run, reached only when the body performs the
                 * matching shift) reifies the delimited context once per reset --
                 * bounded -- so it stays a black box via eval_expr. */
                bool        is_clone = control->kind == EX_CLONEABLE_RESET;
                const Expr *body = is_clone ? control->as.cloneable_reset_.body
                                            : control->as.serial_reset_.body;
                ExprKind    sk   = is_clone ? EX_CLONEABLE_SHIFT : EX_SERIAL_SHIFT;
                if (ts_reaches_shift(body, sk)) {
                    /* SR N4 Slice 4: reify the context here (bounded), then apply
                     * the shift receiver on the work-stack so a receiver that
                     * recursively triggers another capturing reset folds onto the
                     * heap instead of C-recursing through turi_call. */
                    TsDeferredReceiver d; d.active = false; d.is_fold = false;
                    TuriValue rv = ts_capture_and_run(env, cf, body, sk,
                                                      /*serial=*/!is_clone, &d);
                    if (!d.active) { cur = rv; descending = false; break; }
                    /* Free the capture let-frames after the deferred work runs
                     * (a captured frame fn / the receiver may close over one), so
                     * push a DK_NATIVE_RESUME beneath that frees them and passes
                     * the value through. */
                    SerialReceiverState *s =
                        (SerialReceiverState *)malloc(sizeof(SerialReceiverState));
                    s->let_frames = d.let_frames; s->n_let = d.n_let;
                    NativeResume *nr = (NativeResume *)malloc(sizeof(NativeResume));
                    nr->resume = serial_receiver_resume; nr->state = s;
                    DRIVE_PUSH(((DriveCont){ .kind = DK_NATIVE_RESUME, .aux = nr }));
                    if (d.is_fold) {
                        /* SR N4 Slice 7: is_pure -- fold the reified context over
                         * the pure value on the work-stack (DK_CONT_FOLD). */
                        ContFoldState *fs; TuriValue val, ffn, *fargs; uint32_t fn_n;
                        int rc = cont_fold_begin(d.cont, d.fold_w, &fs, &val,
                                                 &ffn, &fargs, &fn_n);
                        if (rc != 1) { cur = val; descending = false; break; }
                        DRIVE_PUSH(((DriveCont){ .kind = DK_CONT_FOLD, .aux = fs }));
                        apply_fn = ffn; apply_args = fargs; apply_n = fn_n;
                        have_apply = true;
                        break;
                    }
                    /* SR N4 Slice 4: apply the shift receiver on the work-stack. */
                    apply_fn   = d.receiver;
                    apply_args = (TuriValue *)malloc(sizeof(TuriValue));
                    apply_args[0] = turi_int((int64_t)(intptr_t)d.cont);
                    apply_n    = 1;
                    have_apply = true;
                    break;
                }
                TuriResetBoundary *b = (TuriResetBoundary *)malloc(sizeof(TuriResetBoundary));
                b->kind                = is_clone ? PROMPT_CLONEABLE : PROMPT_SERIAL;
                b->result              = turi_nil();
                b->saved_handler_stack = env->handler_stack;
                b->saved_defer_stack   = env->defer_stack;
                b->prev                = g_reset_stack;
                g_reset_stack = b;
                DRIVE_PUSH(((DriveCont){ .kind = DK_RESET, .aux = b }));
                control = body;
                tail = false;
                break;
            }
            case EX_CALLCC: {
                /* SR N4 Slice 2: model the (call/cc f) escape boundary on the
                 * work-stack (no setjmp), so nested call/cc folds onto the heap.
                 * Apply f with a heap boundary pointer as its handle k beneath a
                 * DK_ESCAPE frame; invoking (k v) raises env->aborting targeting
                 * this boundary and unwinds the work-stack to DK_ESCAPE. */
                TuriValue fn = eval_expr(env, cf, control->as.callcc_.fn);
                if (turi_is_error(fn) || env_signaled(env)) {
                    cur = fn; descending = false; break;
                }
                if (fn.tag != TURI_CLOSURE) {
                    cur = turi_errorf("eval: call/cc expects a function, got tag %d", fn.tag);
                    descending = false; break;
                }
                TuriEscapeBoundary *b = (TuriEscapeBoundary *)malloc(sizeof(TuriEscapeBoundary));
                b->result              = turi_nil();
                b->saved_handler_stack = env->handler_stack;
                b->saved_defer_stack   = env->defer_stack;
                DRIVE_PUSH(((DriveCont){ .kind = DK_ESCAPE, .aux = b }));
                apply_fn   = fn;
                apply_args = (TuriValue *)malloc(sizeof(TuriValue));
                apply_args[0] = turi_int((int64_t)(intptr_t)b);
                apply_n    = 1;
                have_apply = true;
                break;
            }
            case EX_CATCH_UNWIND: {
                /* C1: model the (catch-unwind thunk) boundary on the work-stack
                 * (no setjmp), so nested catch-unwind folds onto the heap instead
                 * of one setjmp/eval_apply C frame per level.  Register a heap
                 * TuriCatchBoundary on g_catch_stack (so a panic can find it and
                 * decide to unwind via the signal), push DK_CATCH_UNWIND, and
                 * apply the 0-arg thunk beneath it.  A panic under the thunk sets
                 * env->panicking, which propagates up to DK_CATCH_UNWIND. */
                TuriValue thunk = eval_expr(env, cf, control->as.catch_unwind_.thunk);
                if (turi_is_error(thunk) || env_signaled(env)) {
                    cur = thunk; descending = false; break;
                }
                if (thunk.tag != TURI_CLOSURE) {
                    cur = turi_error("eval: catch-unwind: thunk must be a closure");
                    descending = false; break;
                }
                TuriCatchBoundary *b = (TuriCatchBoundary *)malloc(sizeof(TuriCatchBoundary));
                b->is_driver           = true;
                b->jmp                 = NULL;
                b->saved_handler_stack = env->handler_stack;
                b->saved_defer_stack   = env->defer_stack;
                b->saved_module        = env->current_module;
                b->saved_no_unwind     = env->in_no_unwind;
                b->prev                = g_catch_stack;
                g_catch_stack = b;
                DRIVE_PUSH(((DriveCont){ .kind = DK_CATCH_UNWIND, .aux = b }));
                apply_fn   = thunk;
                apply_args = NULL;
                apply_n    = 0;
                have_apply = true;
                break;
            }
            case EX_SHIFT:
            case EX_SHIFT0: {
                /* SR N3b: abortive (shift f body) / (shift0 f body) via the
                 * work-stack resume protocol.  Evaluate body and the receiver,
                 * then ask the driver to apply f(body) on the work-stack;
                 * abortive_shift_resume aborts to the nearest plain reset
                 * boundary with the result (or propagates a signalled result).
                 * The receiver no longer runs on a re-entrant C frame -- it
                 * folds onto the work-stack like any application.  (eval_expr_impl
                 * keeps a synchronous EX_SHIFT/EX_SHIFT0 for non-driver callers.) */
                bool        is0  = control->kind == EX_SHIFT0;
                const Expr *kfn  = is0 ? control->as.shift0_.k_fn : control->as.shift_.k_fn;
                const Expr *body = is0 ? control->as.shift0_.body : control->as.shift_.body;
                TuriValue v = eval_expr(env, cf, body);
                if (turi_is_error(v) || env_signaled(env)) {
                    cur = v; descending = false; break;
                }
                TuriValue fn = eval_expr(env, cf, kfn);
                if (turi_is_error(fn) || env_signaled(env)) {
                    cur = fn; descending = false; break;
                }
                AbortiveShiftState *s = (AbortiveShiftState *)malloc(sizeof(AbortiveShiftState));
                s->form = is0 ? "shift0" : "shift";
                NativeResume *nr = (NativeResume *)malloc(sizeof(NativeResume));
                nr->resume = abortive_shift_resume; nr->state = s;
                DRIVE_PUSH(((DriveCont){ .kind = DK_NATIVE_RESUME, .aux = nr }));
                apply_fn   = fn;
                apply_args = (TuriValue *)malloc(sizeof(TuriValue));
                apply_args[0] = v;
                apply_n    = 1;
                have_apply = true;
                break;
            }
            default: {
                /* SR N3: single-operand black-box forms descend their operand on
                 * the work-stack (DK_UNARY), so recursion threaded through a
                 * cast/ascribe/return/set/transparent-shim stays heap-bounded. */
                {
                    /* turi-dict-passing-plan: a constrained rank-2 poly value
                     * resolves to its dict-clone, not the unwrapped original. */
                    TuriValue dcv;
                    if (poly_wrap_dict_clone_value(env, cf, control, &dcv)) {
                        cur = dcv; descending = false; break;
                    }
                }
                const Expr *operand = NULL;
                if (unary_operand(control, &operand)) {
                    if (!operand) {
                        /* bare (return) with no value: nil, post runs directly. */
                        cur = eval_unary_post(env, cf, control, turi_nil());
                        descending = false;
                        break;
                    }
                    DRIVE_PUSH(((DriveCont){ .kind = DK_UNARY, .expr = control,
                                             .frame = cf }));
                    control = operand;
                    tail = false;   /* operand is non-tail */
                    break;          /* keep descending */
                }
                /* Black box: evaluate any other kind via the recursive path. */
                cur = eval_expr(env, cf, control);
                descending = false;
                break;
            }
            }
        } else {
            /* Returning `cur` to the frame on top of the work-stack.  Each
             * continuation runs its own cleanup when a control signal
             * (error/returning/throwing) passes through it -- LET frames must be
             * freed (and their body defers fired) on unwind, so this is handled
             * per-kind rather than by a blanket unwind-to-DONE. */
            DriveCont *top = &st[len - 1];
            bool signaled = turi_is_error(cur) || env_signaled(env);
            /* SR N4: abort unwinds like a signal; C1: so does a panic. */
            switch (top->kind) {
            case DK_DONE:
                result = cur;
                goto done;
            case DK_RESET: {
                /* SR N4: the reset body produced `cur` (a value, or a signal
                 * passing through).  Unlink the boundary; consume a matching
                 * abort (clears env->aborting, delivers the abort value, restores
                 * saved env state) or let any other signal / non-matching abort
                 * propagate. */
                TuriResetBoundary *b = (TuriResetBoundary *)top->aux;
                g_reset_stack = b->prev;
                cur = reset_consume_abort(env, b, cur);
                free(b);
                len--;
                break;
            }
            case DK_ESCAPE: {
                /* SR N4 Slice 2: f produced `cur` (its normal value), or an
                 * escape/other signal passed through.  Consume a matching escape
                 * (abort_target == this boundary): deliver the escape value and
                 * restore saved env state.  A non-matching abort (a shift abort,
                 * or an escape to an outer call/cc) propagates unchanged. */
                TuriEscapeBoundary *b = (TuriEscapeBoundary *)top->aux;
                if (env->aborting && env->abort_target == (void *)b) {
                    env->aborting      = false;
                    env->abort_target  = NULL;
                    cur                = env->abort_value;
                    env->handler_stack = b->saved_handler_stack;
                    env->defer_stack   = b->saved_defer_stack;
                }
                free(b);
                len--;
                break;
            }
            case DK_CATCH_UNWIND: {
                /* C1: the catch-unwind thunk produced `cur` (a value, or a signal
                 * passing through).  Unlink the boundary, then:
                 *   - a caught panic (env->panicking): consume it, restore saved
                 *     env state, and deliver (err payload).  Defers of the unwound
                 *     frames already fired incrementally as the signal passed
                 *     through their DK_CALL_RET / DK_LET_BODY epilogues (like a
                 *     throw), so none are re-fired here.
                 *   - any other in-flight signal (return / throw / abort) or an
                 *     error value: propagate unchanged (catch-unwind only catches
                 *     panics).
                 *   - a normal value: wrap in (ok value). */
                TuriCatchBoundary *b = (TuriCatchBoundary *)top->aux;
                g_catch_stack = b->prev;
                if (env->panicking) {
                    env->panicking      = false;
                    env->handler_stack  = b->saved_handler_stack;
                    env->in_no_unwind   = b->saved_no_unwind;
                    env->current_module = b->saved_module;
                    cur = turi_err_result_box(env);
                } else if (turi_is_error(cur) || env_signaled(env)) {
                    /* propagate cur unchanged */
                } else {
                    cur = turi_ok_result_box(env, cur);
                }
                free(b);
                len--;
                break;
            }
            case DK_WHILE: {
                /* index 0: `cur` is the condition's value.  index 1: it is the
                 * body's.  Mirrors the eval_expr_impl EX_WHILE loop exactly,
                 * including its asymmetry -- the condition bails on any signal
                 * (env_signaled), the body bails only on an error or a
                 * `return`, so a `throw`/`abort`/`panic` raised in the body
                 * propagates through the enclosing frames rather than being
                 * caught by the loop. */
                const Expr *we = top->expr;
                if (top->index == 0) {
                    if (turi_is_error(cur) || env_signaled(env)) { len--; break; }
                    if (!turi_is_truthy(cur)) { cur = turi_nil(); len--; break; }
                    top->index = 1;
                    control = we->as.while_.body; cf = top->frame;
                    tail = false; descending = true;
                } else {
                    if (turi_is_error(cur) || env->returning) { len--; break; }
                    top->index = 0;
                    control = we->as.while_.cond; cf = top->frame;
                    tail = false; descending = true;
                }
                break;
            }
            case DK_STM_SEQ: {
                /* C2: an stm body item produced `cur`.  On any propagating signal
                 * or error, abandon the block (propagate).  Otherwise record the
                 * value; a retry/abort request short-circuits the rest (matching
                 * the eval_expr_impl EX_STM loop), else advance to the next item.
                 * The last item's value is the stm block's value. */
                if (signaled) { len--; break; }
                const Expr *se = top->expr;
                uint32_t    n  = se->as.stm_.n_body;
                top->last = cur;
                if (g_stm_tx && (g_stm_tx->retry_requested || g_stm_tx->aborted)) {
                    cur = top->last; len--; break;
                }
                top->index++;
                if (top->index < n) {
                    control = se->as.stm_.body[top->index];
                    cf = top->frame; tail = false; descending = true;
                } else {
                    cur = top->last; len--;
                }
                break;
            }
            case DK_ATOMICALLY: {
                /* C2: the stm body produced `cur`.  Unlink the transaction; then
                 * propagate any error/signal, error on a requested retry/abort
                 * (single-threaded: no way to make progress), or commit the
                 * write-log.  Mirrors eval_atomically exactly. */
                TuriStmTx *tx = (TuriStmTx *)top->aux;
                g_stm_tx = tx->prev;
                if (signaled) {
                    /* fall through: propagate cur unchanged */
                } else if (tx->retry_requested || tx->aborted) {
                    cur = turi_error("eval: atomically: transaction requested retry "
                                     "with no way to make progress (single-threaded "
                                     "interpreter cannot block on another writer)");
                } else {
                    for (int i = 0; i < tx->w_count; i++) {
                        tx->w_tv[i]->value = tx->w_val[i];
                        tx->w_tv[i]->version++;
                    }
                }
                free(tx->w_tv); free(tx->w_val); free(tx);
                len--;
                break;
            }
            case DK_RESUME_K: {
                /* The resume's k expression produced `cur`.  Validate, then
                 * descend the value arg under DK_RESUME carrying k in .last --
                 * from here on identical to the former eval_expr'd-k path. */
                if (signaled) { len--; break; }
                if (cur.tag != TURI_EFFECT_CONT) {
                    cur = turi_error("eval: resume: not a continuation");
                    len--; break;
                }
                const Expr *rex = top->expr;
                EvalFrame  *rcf = top->frame;
                bool        rtl = top->tail;
                /* Repurpose this slot as the DK_RESUME frame (same stack cell:
                 * pop + push collapse). */
                top->kind = DK_RESUME;
                top->last = cur;
                top->frame = rcf;
                top->tail  = rtl;
                control = rex->as.resume_.resume->value;
                cf = rcf; tail = false; descending = true;
                break;
            }
            case DK_RESUME: {
                /* C3: the resume value arg produced `cur`.  On a signal/error,
                 * propagate.  Otherwise dispatch the resume with the driven
                 * value: a ws continuation re-installs its prompt + an
                 * independent clone of the captured slice and feeds the value to
                 * the hole (multishot); a fiber continuation calls
                 * eval_resume_cont.  Mirrors the former inline EX_RESUME code. */
                TuriValue  k    = top->last;
                EvalFrame *rcf  = top->frame;
                bool       rtl  = top->tail;
                if (signaled) { len--; break; }
                TuriValue v = cur;
                len--;   /* pop DK_RESUME */
                if (!k.as_cont->ws) {
                    cur = eval_resume_cont(env, rcf, k.as_cont, v);  /* fiber cont */
                    break;
                }
                TuriWsCont *wc = k.as_cont->ws;
                /* Re-install the captured prompt around the resumed slice.  For a
                 * DEEP handler it is re-installed ACTIVE (index = 1), so a perform
                 * of the same effect in the resumed slice is caught again.  For a
                 * SHALLOW handler (F2, `handle-shallow`) it is re-installed
                 * INACTIVE (index = 0): still a return delimiter that restores the
                 * env boundary and delivers the slice's value, but skipped by the
                 * perform scan (which ignores index == 0 prompts), so a re-perform
                 * reaches the nearest ENCLOSING active prompt -- or the fiber path
                 * / unhandled if none.  Mirrors dk_perform's no-reinstall tail. */
                int reinstall_active = (wc->handler && wc->handler->shallow) ? 0 : 1;
                DRIVE_PUSH(((DriveCont){ .kind = DK_PROMPT, .aux = (void *)wc->handler,
                                         .frame = wc->handler_frame, .tail = rtl,
                                         .index = reinstall_active,
                                         .saved_module = env->current_module,
                                         .was_no_unwind = env->in_no_unwind }));
                if (wc->n_frames) {
                    DriveCont *clone = (DriveCont *)malloc(wc->n_frames * sizeof(DriveCont));
                    clone_ws_slice(env, wc->frames, wc->n_frames, clone);
                    for (size_t i = 0; i < wc->n_frames; i++)
                        DRIVE_PUSH(clone[i]);
                    free(clone);
                }
                env->current_module = wc->perf_module;
                env->in_no_unwind   = wc->perf_no_unwind;
                env->defer_stack    = wc->perf_defer;
                cur = v;   /* feed v into the cloned slice; keep returning */
                break;
            }
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
                        TuriClosure *copy = (TuriClosure *)turi_val_alloc(env, sizeof(TuriClosure));
                        *copy = *cur.as_closure;
                        copy->captured = nf;
                        cur = turi_closure(copy);
                    }
                    eval_frame_update(nf, nm, cur);
                } else {
                    frame_bind(env, nf, nm, cur);
                }
                top->index++;
                if (top->index < n) {
                    control = le->as.let_.bindings[top->index].init;
                    cf = nf; tail = false; descending = true;  /* inits are non-tail */
                } else if (top->tail) {
                    /* F1: tail let body -- pop DK_LET_BIND and descend the body
                     * directly (leak nf); body defers fire at function exit
                     * (matches the retired eval_body_tco).  Exposes the enclosing
                     * DK_CALL_RET for a tail call in the body.  The body's
                     * do-block pushes the scope marker when it holds defers. */
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
                /* On normal exit fire this scope's defers; on early-return/throw/
                 * abort/panic leave them for the enclosing DK_CALL_RET (which
                 * fires the leaked-scope chain by-scope), matching the recursive
                 * EX_LET. */
                if (!env_signaled(env))
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
            case DK_PERFORM_ARG: {
                /* A driven perform arg produced `cur`.  Store it; descend the
                 * next arg, or hand the full set back to the descending
                 * EX_PERFORM arm via the side channel. */
                TuriValue *acc = (TuriValue *)top->aux;
                if (signaled) { free(acc); len--; break; }
                const Expr  *pex = top->expr;
                PerformExpr *pe  = pex->as.perform_.perform;
                acc[top->index] = cur;
                top->index++;
                if (top->index < pe->n_args) {
                    control = pe->args[top->index]; cf = top->frame;
                    tail = false; descending = true;
                } else {
                    pargs_for  = pex;
                    pargs_heap = acc;
                    control = pex; cf = top->frame; tail = top->tail;
                    len--;
                    descending = true;
                }
                break;
            }
            case DK_MATCH_SCRUT: {
                /* The driven scrutinee produced `cur`.  Select the arm with the
                 * value in hand, then descend the winning body exactly as the
                 * synchronous path does -- including the F1 tail leak, since
                 * this frame pops before the body descends. */
                if (signaled) { len--; break; }
                const Expr *me  = top->expr;
                EvalFrame  *mcf = top->frame;
                bool        mtl = top->tail;
                len--;   /* pop before descending: the body must see the frame
                          * beneath (DK_CALL_RET for a tail arm, etc.) */
                EvalFrame  *af   = NULL;
                const Expr *body = NULL;
                TuriValue   sv   = turi_nil();
                int mr = eval_match_resolve_with(env, mcf, me, cur, &af, &body, &sv);
                if (mr == 1) {
                    if (mtl) {
                        control = body; cf = af; tail = true;   /* F1 tail leak */
                    } else {
                        DRIVE_PUSH(((DriveCont){ .kind = DK_MATCH_BODY, .expr = me,
                                                 .frame = af, .tail = mtl }));
                        control = body; cf = af; tail = mtl;
                    }
                    descending = true;
                } else if (mr == 0) {
                    cur = turi_error("eval: match: no arm matched");
                } else {
                    cur = sv;
                }
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
                /* The runtime-tag re-dispatch that sat here
                 * (gde_reresolve_method_by_value) is retired -- the
                 * carrier-helper dispatch recovery at EX_CALL setup covers its
                 * one shape statically; see the measurement record at its
                 * former definition. */
                FnDef       *fn = (FnDef *)cl->fn;
                bool foldable = !cl->native && fn && fn->body &&
                                fn->body->kind != EX_INLINE_C;
                if (!foldable) {
                    if (cl->native == native_resume_cont) {
                        /* SR N4 Slice 6: fold resume-cont! on the work-stack like
                         * the cont-resume builtin (args: cont handle, value). */
                        TuriCont *c = (n >= 1 && acc[0].as_int)
                                    ? (TuriCont *)(intptr_t)acc[0].as_int : NULL;
                        int64_t w = (n >= 2) ? acc[1].as_int : 0;
                        free(acc);
                        ContFoldState *s; TuriValue val, ffn, *fargs; uint32_t fn_n;
                        int rc = cont_fold_begin(c, w, &s, &val, &ffn, &fargs, &fn_n);
                        if (rc != 1) { cur = val; len--; break; }
                        top->kind = DK_CONT_FOLD; top->aux = s;
                        apply_fn = ffn; apply_args = fargs; apply_n = fn_n;
                        have_apply = true;
                        break;
                    }
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
                /* forall-dict-pass interpreter parity: a call THROUGH a rank-2
                 * poly fn param (is_poly_call) prepends one implicit dictionary
                 * actual per constraint on the poly param's type -- the compiled
                 * path binds these to the callee's dict-clone params.  The
                 * tree-walker binds them as DictBinds in the apply prologue
                 * below and dispatches through the frame dictionary, so the
                 * leading dict actuals are not VALUE params.  Skip them and
                 * bind only the callee's declared value params.  See
                 * docs/archive/history/turi-interp-forall-dict-wide-consumer-arity.md. */
                uint32_t arg_base = 0;
                if (top->expr->as.call_.is_poly_call && n > effective_params)
                    arg_base = n - effective_params;
                if (effective_params != n - arg_base) {
                    cur = turi_errorf("eval: arity mismatch: %s expects %u args, got %u",
                                      fn->binding ? fn->binding->name->name : "<fn>",
                                      (unsigned)effective_params, (unsigned)(n - arg_base));
                    free(acc); len--; break;
                }
                if (env->step_fuel_limit > 0) {  /* SB3: step-fuel, as the retired eval_apply_inner charged it */
                    if (env->step_fuel == 0) {
                        cur = turi_error("eval: step fuel exhausted");
                        free(acc); len--; break;
                    }
                    env->step_fuel--;
                }
                EvalFrame *call_frame = eval_frame_new(env, (EvalFrame *)cl->captured);
                for (uint32_t i = 0; i < effective_params; i++)
                    frame_bind(env, call_frame,
                               fn->params[param_offset + i]->name->name,
                               turi_copy_byvalue_struct_arg(env, acc[arg_base + i]));
                free(acc);
                /* turi-dict-passing-plan: a DICT-CLONE's leading dict params
                 * just bound as ordinary int args carry TypeClassInstance
                 * pointers (the EX_DICT address-only value).  Record them as
                 * class->instance DictBinds on the frame so method dispatch
                 * inside the body reads the caller-supplied dictionary with
                 * precedence over the gde_* recovery heuristics. */
                if (fn->n_dict_clone > 0) {
                    for (uint8_t dk2 = 0; dk2 < fn->n_dict_clone; dk2++) {
                        Binding *dp = fn->dict_clone_params[dk2];
                        if (!dp || !dp->name || !fn->dict_clone_classes[dk2])
                            continue;
                        TuriValue dv = eval_lookup(env, call_frame,
                                                   dp->name->name);
                        if (dv.tag != TURI_INT || dv.as_int == 0) continue;
                        DictBind *db = (DictBind *)turi_val_alloc(
                            env, sizeof(DictBind));
                        db->tc    = fn->dict_clone_classes[dk2];
                        db->inst  = (struct TypeClassInstance *)(intptr_t)
                                        dv.as_int;
                        db->tyvar = NULL;  /* unkeyed: class-only match */
                        db->next  = call_frame->dicts;
                        call_frame->dicts = db;
                    }
                }
                /* generic-dict-dispatch: pin this call's concrete tyvar
                 * substitutions onto the callee frame so a baked-representative
                 * method call inside the body can re-resolve its instance. */
                if (top->expr->as.call_.n_abi_bindings > 0)
                    frame_record_abi(env, call_frame, top->frame, top->expr);
                else
                    frame_pin_hkt_tyvars_from_args(env, call_frame, fn,
                                                   param_offset, effective_params,
                                                   top->expr, arg_base);
                /* Bare-head constrained instance: bind its constraint tyvars
                 * (`(C A)`'s `A`) from the receiver arg's static type so a nested
                 * dispatch inside the body resolves the element's real instance
                 * instead of the baked int-carrier representative. */
                if (fn->owner_instance && n > 0 && top->expr->as.call_.args)
                    frame_bind_instance_constraint_tyvars(
                        env, call_frame, fn, &top->expr->as.call_.args[0]->type);
                /* turi-dict-passing-plan (plain constrained generics): with the
                 * tyvar pins in place, resolve the callee's own constraints to
                 * instances and record them as frame dictionaries, so method
                 * dispatch in the body reads a dict instead of re-deriving the
                 * instance heuristically.  Covers both a constrained defn's
                 * constraint set and a constrained instance body's
                 * type-param constraints. */
                if (fn->constraints.n_constraints > 0)
                    frame_bind_constraint_dicts(env, call_frame,
                                                fn->constraints.constraints,
                                                fn->constraints.n_constraints);
                if (fn->owner_instance &&
                    fn->owner_instance->n_type_param_constraints > 0)
                    frame_bind_constraint_dicts(
                        env, call_frame,
                        fn->owner_instance->type_param_constraints,
                        fn->owner_instance->n_type_param_constraints);

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
                     * and fire its defers.  Reaching a tail call is a *normal*
                     * frame completion, so fire head-first (innermost scope
                     * first, same-scope LIFO) -- matching the compiled
                     * normal-exit ordering. */
                    env->in_no_unwind = ret->was_no_unwind;
                    fire_defers_to_mark(env, (DeferItem *)ret->aux, NULL);
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
                    /* Debugger Phase 2: a tail call replaces the current activation
                     * in place (TCO), so retarget the top stack frame rather than
                     * pushing -- keeping backtrace depth O(1) like the runtime. */
                    if (env->debugger) turi_dbg_set_top(env, fn, call_frame);
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
                if (env->debugger) turi_dbg_push(env, fn, call_frame);
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
                /* Fire this call's defers.  On an early exit (return / throw)
                 * the chain spans multiple leaked scopes, which fire outer-first
                 * (by-scope reversal); on normal completion fire head-first
                 * (innermost scope first, same-scope LIFO).  Both mirror the
                 * compiled tur_frame_fire_chain (see
                 * docs/archive/history/turi-tail-scope-defers-fire-fifo-not-lifo.md). */
                if (env_signaled(env))
                    fire_defers_to_mark_by_scope(env, (DeferItem *)top->aux, NULL);
                else
                    fire_defers_to_mark(env, (DeferItem *)top->aux, NULL);
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
                if (env->debugger) turi_dbg_pop(env);
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
                } else if (is_cont_resume_builtin(spec)) {
                    /* SR N4 Slice 5: fold the continuation resume on the
                     * work-stack instead of synchronously in ts_cont_resume, so a
                     * resumed frame that itself resumes folds onto the heap.
                     * Reuse this slot as a DK_CONT_FOLD; apply the first call
                     * frame via have_apply (pure arith frames fold in place). */
                    TuriCont *c = (n >= 1 && acc[0].as_int)
                                ? (TuriCont *)(intptr_t)acc[0].as_int : NULL;
                    int64_t w = (n >= 2) ? acc[1].as_int : 0;
                    free(acc);
                    ContFoldState *s; TuriValue val, ffn, *fargs; uint32_t fn_n;
                    int rc = cont_fold_begin(c, w, &s, &val, &ffn, &fargs, &fn_n);
                    if (rc != 1) { cur = val; len--; break; }   /* done / error */
                    top->kind = DK_CONT_FOLD; top->aux = s;   /* reuse this slot */
                    apply_fn = ffn; apply_args = fargs; apply_n = fn_n;
                    have_apply = true;   /* result returns to this DK_CONT_FOLD */
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
                    /* make_struct_val_def copies the fields, so free after. */
                    cur = make_struct_val_def(env, "<struct>", n, fields);
                    free(fields);
                    len--;  /* pop; keep returning the struct value */
                }
                break;
            }
            case DK_PROMPT: {
                /* The handle body (or a resumed slice) finished with `cur`, which
                 * is the handle's / resume's value.  Restore the env boundary and
                 * propagate. */
                env->current_module = top->saved_module;
                env->in_no_unwind   = top->was_no_unwind;
                len--;   /* pop; propagate cur (value or signal) */
                break;
            }
            case DK_GET_FIELD: {
                /* SR N2: the receiver evaluated to `cur`; extract the field.
                 * On a control signal (error/return/throw) propagate untouched. */
                if (signaled) { len--; break; }
                cur = get_field_extract(top->expr, cur);
                len--;
                break;
            }
            case DK_UNARY: {
                /* SR N3: the operand evaluated to `cur`; apply the form's
                 * post-operand logic.  On a control signal propagate untouched. */
                if (signaled) { len--; break; }
                cur = eval_unary_post(env, top->frame, top->expr, cur);
                len--;
                break;
            }
            case DK_NATIVE_RESUME: {
                /* SR: the requested application produced `cur`.  Hand it to the
                 * native's resume callback, which yields the native's value in
                 * `out` (and, single-shot for N1, sets done=true).  The resume
                 * also handles a signalled `cur` (it propagates without
                 * committing).  N2+ will re-request another application here when
                 * done == false, reusing this slot for loop natives. */
                NativeResume *nr = (NativeResume *)top->aux;
                bool      done = true;
                TuriValue out  = turi_nil();
                nr->resume(env, nr->state, cur, &done, &out);
                free(nr);
                cur = out;
                len--;   /* pop; propagate the native's value */
                break;
            }
            case DK_CONT_FOLD: {
                /* SR N4 Slice 5: the call frame at s->i was applied, producing
                 * `cur`.  Fold it into the accumulator (unless an ignore-value
                 * do-tail frame), advance past pure arith frames, and re-request
                 * the next call frame in this same slot (so the fold stays O(1)
                 * on the work-stack); when no frames remain the resume value is
                 * turi_int(s->v). */
                ContFoldState *s = (ContFoldState *)top->aux;
                if (signaled) { free(s); len--; break; }
                /* The frame's result becomes the new accumulator (ts_cont_resume
                 * sets v = r for every call frame -- ignore_value only controls
                 * whether the resume value is passed as an *argument*, handled in
                 * cont_fold_advance, not whether the result updates v). */
                if (cur.tag != TURI_INT) {
                    cur = turi_errorf("eval: cont call frame returned non-int (tag %d)", cur.tag);
                    free(s); len--; break;
                }
                s->v = cur.as_int;
                s->i--;
                TuriValue ffn, *fargs, ferr; uint32_t fn_n;
                int rc = cont_fold_advance(s, &ffn, &fargs, &fn_n, &ferr);
                if (rc == 2) { cur = ferr; free(s); len--; break; }
                if (rc == 0) { cur = turi_int(s->v); free(s); len--; break; }
                apply_fn = ffn; apply_args = fargs; apply_n = fn_n;
                have_apply = true;   /* re-request; result returns to this slot */
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
 * Note: a native HOF that re-enters evaluation via turi_call routes through
 * eval_apply_driven -> eval_drive_ex, where the callee body (and its own
 * recursion) folds onto the heap work-stack; measured, such re-entry runs
 * 1,000,000 deep without C-stack growth.  C4 (turi-c-scoped-forms-heap-bounding)
 * retired the eval_depth guard on the strength of that -- see eval_apply.
 * ---------------------------------------------------------------------- */
static TuriValue eval_apply_driven(TuriEnv *env, TuriClosure *cl,
                                   TuriValue *args, uint32_t n_args) {
#ifndef NDEBUG
    /* Gap 6: tag-on-first-use cross-env guard.  Claim the closure for `env` the
     * first time it is applied; on any later application under a different env,
     * abort -- that is a closure cached across per-script envs, which would
     * otherwise dereference freed arena memory. */
    if (cl) {
        if (cl->origin_env == NULL) {
            cl->origin_env = env;
        } else if (cl->origin_env != env) {
            fprintf(stderr,
                    "turi: FATAL: closure applied under a different TuriEnv than "
                    "it was created in -- closures pin their originating env's "
                    "arenas and must not be cached/reused across envs "
                    "(libturi-per-embed-env-and-peripherals Gap 6)\n");
            abort();
        }
    }
#endif
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
    EvalFrame *call_frame = eval_frame_new(env, (EvalFrame *)cl->captured);
    for (uint32_t i = 0; i < n_args; i++)
        frame_bind(env, call_frame, fn->params[param_offset + i]->name->name,
                   turi_copy_byvalue_struct_arg(env, args[i]));

    DriveSeed seed = {
        .defer_mark    = (DeferItem *)env->defer_stack,
        .saved_module  = env->current_module,
        .was_returning = env->returning,
        .was_no_unwind = env->in_no_unwind,
        .dbg_fn        = fn,
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
 * C4 (turi-c-scoped-forms-heap-bounding): the eval_depth recursion guard that
 * used to live here has been retired.  After SR (trampoline) + C1-C3, every
 * interpreter recursion -- tail / non-tail, reset/shift, call/cc, serial/
 * cloneable resume, catch-unwind, atomically, and effect handlers (both body
 * and resume-value recursion) -- folds onto the heap work-stack and runs
 * 1,000,000 deep without any C-stack growth per level, so there is no residual
 * C-recursion for the guard to bound.  Sandbox resource limiting is now the
 * job of step-fuel (turi_env_set_fuel), which bounds total work regardless of
 * shape; turi_env_set_max_depth is retained as a no-op for API compatibility. */
static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args) {
    /* C1: a panic signal in flight short-circuits any application reached via a
     * native HOF's turi_call, so the callee never runs while unwinding to the
     * DK_CATCH_UNWIND boundary (the value is discarded). */
    if (env->panicking) return turi_nil();

    /* DEPR-R0 rejection primitives vs the Error typeclass.  `error-message` /
     * `error-cause` are BOTH native primitives over a builtin TURI_REJECTION
     * value AND methods of the `Error` typeclass (stdlib/typeclass.tur), whose
     * only instance -- Error[ptr<void>] -- carries an inline-C body the
     * tree-walker cannot run.  A rejection is a builtin value with no (and no
     * possible) user Error instance, so a call like `(error-message r)` on a
     * rejection wrongly dispatches into that uninterpretable instance and fails
     * with "inline-C not supported".  Route it back to the native primitive.
     * `error?` needs no such handling -- it is not a typeclass method. */
    if (n_args == 1 && args[0].tag == TURI_REJECTION && cl && !cl->native &&
        cl->fn) {
        const FnDef *fn = (const FnDef *)cl->fn;
        const char *mname = (fn->binding && fn->binding->name)
                            ? fn->binding->name->name : NULL;
        /* Instance methods are mangled `__inst_<Class>_<method>_<component>`
         * (emit_core.c), so the Error methods surface as
         * `__inst_Error_error_hymessage_*` / `__inst_Error_error_hycause_*`.
         * Match the stable class prefix, then the method by keyword -- robust
         * to the exact hyphen encoding.  A genuine ptr<void> error value is a
         * boxed TURI_INT, never a TURI_REJECTION, so real Error instances are
         * unaffected. */
        if (mname && strncmp(mname, "__inst_Error_", 13) == 0) {
            if (strstr(mname, "message"))
                return native_error_message(env, args, n_args, NULL);
            if (strstr(mname, "cause"))
                return turi_int(0);   /* a rejection carries no cause pointer */
        }
    }
    const char *saved_module = env->current_module;
    TuriValue r = eval_apply_driven(env, cl, args, n_args);
    env->current_module = saved_module;
    return r;
}

/* -------------------------------------------------------------------------
 * Cooperative session-channel runtime (turi-session-types-plan)
 *
 * The compiled runtime (emit_module.c) backs `make-session` / `send` / `recv` /
 * `close` with a pthread mutex/condvar one-slot rendezvous.  The tree-walking
 * interpreter is single-threaded but cooperative, so we mirror the same
 * `state 0/1/2` handshake discipline on the existing fiber scheduler: a blocked
 * `recv` parks its fiber (or, in the main context, pumps the scheduler) until
 * the matching `send` deposits a value and wakes it.
 *
 * A channel is a heap TuriChan smuggled through a TURI_INT holding its
 * pointer -- exactly how the type layer already lowers Session to a machine
 * pointer/int64.  Both endpoints alias the identical TuriChan (duality is
 * type-level only).
 * The struct is bump-allocated from the env value pool, so it is reclaimed en
 * masse at env teardown (leak-clean under LSan); `close` decrements the refcount
 * and marks it abandoned but never frees it individually.
 * ---------------------------------------------------------------------- */

typedef struct TuriChan {
    TuriValue  data_val;     int data_state;    /* 0=idle 1=ready 2=acked */
    TuriValue  branch_val;   int branch_state;  /* offer/choose slot (Slice C) */
    TuriFiber *send_waiter;                     /* parked sender, or NULL */
    TuriFiber *recv_waiter;                     /* parked receiver, or NULL */
    int        refcount;                        /* 2 at make-session */
    int        abandoned;                       /* a peer has closed */
} TuriChan;

#define TURI_SESS_SENDER   0
#define TURI_SESS_RECEIVER 1

/* Block the current context until progress is possible.  A fiber suspends via
 * swapcontext (recorded as the channel's waiter so its counterpart can enqueue
 * it); the main context pumps one scheduler iteration.  Returns 0 on resume,
 * -1 if the main context has nothing left to run (deadlock). */
static int session_park_or_spin(TuriEnv *env, TuriChan *ch, int role) {
    TuriFiber *cur = env->current_fiber;
    if (cur) {
        if (role == TURI_SESS_SENDER) ch->send_waiter = cur;
        else                          ch->recv_waiter = cur;
        cur->state = TURI_FIBER_SUSPENDED;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        swapcontext(&cur->ctx, &env->sched_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
        return 0;
    }
    return turi_sched_step(env) ? 0 : -1;
}

static void session_wake(TuriEnv *env, TuriFiber **slot) {
    if (*slot) { TuriFiber *w = *slot; *slot = NULL; turi_sched_enqueue(env, w); }
}

/* Generic slot send: wait for the slot to be idle, deposit, wake a parked
 * receiver, then wait for the receiver's ack (state 2) before returning --
 * preserving the compiled "send returns only after recv acks" ordering.  A
 * peer that closes the channel (abandoned) makes send drop the value silently
 * and return the channel, exactly as the compiled tur_session_send does.  The
 * `state`/`slot` pair selects the data slot (send/recv) or the branch slot
 * (choose/offer). */
static TuriValue session_send_on(TuriEnv *env, TuriChan *ch, TuriValue val,
                                 int *state, TuriValue *slot) {
    while (*state != 0 && !ch->abandoned) {
        if (session_park_or_spin(env, ch, TURI_SESS_SENDER) != 0)
            return turi_error("eval: session send deadlocked (no receiver)");
    }
    if (ch->abandoned) return turi_int((int64_t)(intptr_t)ch);  /* drop */
    *slot  = val;
    *state = 1;
    session_wake(env, &ch->recv_waiter);
    while (*state != 2 && !ch->abandoned) {
        if (session_park_or_spin(env, ch, TURI_SESS_SENDER) != 0)
            return turi_error("eval: session send deadlocked (receiver never acked)");
    }
    if (!ch->abandoned) {
        *state = 0;
        /* The slot is idle again: wake a peer parked in the next step's send
         * waiting for exactly this idle transition (e.g. a recurring RPC where
         * the channel flips sender/receiver roles across steps). */
        session_wake(env, &ch->send_waiter);
    }
    return turi_int((int64_t)(intptr_t)ch);
}

/* Generic slot recv: wait for a deposited value, take it, ack (state 2), wake
 * the sender.  Returns the received value. */
static TuriValue session_recv_on(TuriEnv *env, TuriChan *ch,
                                 int *state, TuriValue *slot) {
    while (*state != 1) {
        if (ch->abandoned)
            return turi_error("eval: recv on a closed session channel");
        if (session_park_or_spin(env, ch, TURI_SESS_RECEIVER) != 0)
            return turi_error("eval: session recv deadlocked (no sender)");
    }
    TuriValue v = *slot;
    *state = 2;
    session_wake(env, &ch->send_waiter);
    return v;
}

static TuriValue session_send(TuriEnv *env, TuriChan *ch, TuriValue val) {
    return session_send_on(env, ch, val, &ch->data_state, &ch->data_val);
}
static TuriValue session_recv(TuriEnv *env, TuriChan *ch) {
    return session_recv_on(env, ch, &ch->data_state, &ch->data_val);
}

/* choose-left/right: deposit the branch tag (0/1) on the branch slot. */
static TuriValue session_send_tag(TuriEnv *env, TuriChan *ch, int64_t tag) {
    return session_send_on(env, ch, turi_int(tag),
                           &ch->branch_state, &ch->branch_val);
}
/* offer: receive the peer's branch tag (0/1) from the branch slot. */
static TuriValue session_recv_tag(TuriEnv *env, TuriChan *ch) {
    return session_recv_on(env, ch, &ch->branch_state, &ch->branch_val);
}

/* recv-timeout: wait up to `dur_ms` for a deposited value on the data slot.
 * On success, ack, stash the value in env->session_rtv, and return tag 0
 * (Left); on timeout (or a peer that closed without sending) return tag 1
 * (Right).  The receiver runs in the main context in practice (the compiled
 * fixtures call recv-timeout from the main thread), so the timed wait pumps the
 * scheduler and re-checks the deadline; poll_io caps each step at 50 ms so the
 * deadline is observed promptly. */
static TuriValue session_recv_timeout(TuriEnv *env, TuriChan *ch, int64_t dur_ms) {
    uint64_t deadline = turi_now_ms() + (dur_ms > 0 ? (uint64_t)dur_ms : 0);
    while (ch->data_state != 1) {
        if (ch->abandoned)              return turi_int(1);  /* peer gone -> timeout */
        if (turi_now_ms() >= deadline)  return turi_int(1);  /* elapsed -> timeout */
        if (env->current_fiber) {
            /* Fiber-context timed recv would need a scheduler timer to bound the
             * park; the shipped variants call recv-timeout from the main context,
             * so park cooperatively and let a woken deposit / the deadline break
             * the loop. */
            if (session_park_or_spin(env, ch, TURI_SESS_RECEIVER) != 0)
                return turi_int(1);
        } else {
            /* Main context: pump one bounded scheduler step, then re-check the
             * deadline.  turi_sched_step returns false only when nothing is
             * runnable; the loop then just re-polls the clock until `deadline`. */
            turi_sched_step(env);
        }
    }
    TuriValue v = ch->data_val;
    ch->data_state = 2;
    session_wake(env, &ch->send_waiter);
    env->session_rtv = v;   /* tur__rtv_ analog: read by the recv-pair split */
    return turi_int(0);
}

/* close: mark abandoned, wake any blocked peer, drop the refcount. */
static TuriValue session_close(TuriEnv *env, TuriChan *ch) {
    ch->abandoned = 1;
    session_wake(env, &ch->send_waiter);
    session_wake(env, &ch->recv_waiter);
    if (ch->refcount > 0) ch->refcount--;
    return turi_nil();
}

/* -------------------------------------------------------------------------
 * Multi-party session router (turi-session-types-plan, Slice D)
 *
 * Mirrors the compiled TurRouter/TurRole (emit_module.c): an N x N grid of
 * one-slot rendezvous cells, slot[i*N + j] carrying role i -> role j.  Each
 * cell is a TuriChan, so router send/recv reuse the Slice B data-slot park/wake
 * (session_send_on / session_recv_on) verbatim -- only the slot addressing is
 * new.  A TuriRole is {router, role_idx}; both the router (with its slots array)
 * and each role are pool-allocated, so they are reclaimed at env teardown.
 * ---------------------------------------------------------------------- */

typedef struct TuriRouter {
    int       n_roles;
    int       refcount;   /* live TuriRole endpoints; role-close decrements it */
    TuriChan *slots;      /* n_roles*n_roles cells */
} TuriRouter;

typedef struct TuriRole {
    TuriRouter *router;
    int         role_idx;
} TuriRole;

/* make-protocol destructuring vi=0: allocate the router (refcount = n) and the
 * role-0 endpoint. */
static TuriValue router_make_roles(TuriEnv *env, int n, int idx) {
    if (n < 1) n = 1;
    TuriRouter *r = (TuriRouter *)turi_val_calloc(env, sizeof(TuriRouter));
    r->n_roles  = n;
    r->refcount = n;
    r->slots    = (TuriChan *)turi_val_calloc(env, (size_t)n * n * sizeof(TuriChan));
    TuriRole *role = (TuriRole *)turi_val_calloc(env, sizeof(TuriRole));
    role->router   = r;
    role->role_idx = idx;
    return turi_int((int64_t)(intptr_t)role);
}

/* make-protocol destructuring vi=k>0: a peer endpoint on the same router. */
static TuriValue router_get_role(TuriEnv *env, TuriRole *base, int peer_idx) {
    TuriRole *role = (TuriRole *)turi_val_calloc(env, sizeof(TuriRole));
    role->router   = base->router;
    role->role_idx = peer_idx;
    return turi_int((int64_t)(intptr_t)role);
}

/* send-to: rendezvous on slot (role_idx -> to_idx); returns the role endpoint. */
static TuriValue router_send(TuriEnv *env, TuriRole *role, int to_idx, TuriValue val) {
    TuriRouter *r = role->router;
    if (to_idx < 0 || to_idx >= r->n_roles || role->role_idx < 0 ||
        role->role_idx >= r->n_roles)
        return turi_error("eval: session router send: role index out of range");
    TuriChan *ch = &r->slots[role->role_idx * r->n_roles + to_idx];
    (void)session_send_on(env, ch, val, &ch->data_state, &ch->data_val);
    return turi_int((int64_t)(intptr_t)role);
}

/* recv-from: rendezvous on slot (from_idx -> role_idx); returns the value. */
static TuriValue router_recv(TuriEnv *env, TuriRole *role, int from_idx) {
    TuriRouter *r = role->router;
    if (from_idx < 0 || from_idx >= r->n_roles || role->role_idx < 0 ||
        role->role_idx >= r->n_roles)
        return turi_error("eval: session router recv: role index out of range");
    TuriChan *ch = &r->slots[from_idx * r->n_roles + role->role_idx];
    return session_recv_on(env, ch, &ch->data_state, &ch->data_val);
}

/* role-close: drop the router refcount (pool-owned; never individually freed). */
static TuriValue router_role_close(TuriEnv *env, TuriRole *role) {
    (void)env;
    if (role->router->refcount > 0) role->router->refcount--;
    return turi_nil();
}

/* Parse the decimal integer that immediately follows `needle` in the code
 * slice (which is a view into source, not NUL-terminated).  Returns -1 if the
 * needle is absent or no digits follow. */
static int session_int_after(const char *p, uint32_t n, const char *needle) {
    size_t nl = strlen(needle);
    if (nl > n) return -1;
    for (uint32_t i = 0; i + nl <= n; i++) {
        if (memcmp(p + i, needle, nl) == 0) {
            uint32_t j = i + (uint32_t)nl;
            int  val = 0;
            bool any = false;
            while (j < n && p[j] >= '0' && p[j] <= '9') {
                val = val * 10 + (p[j] - '0'); j++; any = true;
            }
            return any ? val : -1;
        }
    }
    return -1;
}

/* Intercept the session inline-C templates (elab_sessions.c / elab_forms.c) and
 * route them to the cooperative runtime above.  All templates are captureless;
 * the channel/value operands arrive as val_exprs.  Returns true (writing *out)
 * when handled, false to fall through to the clean inline-C carve. */
static bool eval_session_intercept(TuriEnv *env, EvalFrame *frame,
                                   InlineC *ic, TuriValue *out) {
    if (ic->n_captures != 0 || !ic->code.p) return false;
    const char *p = ic->code.p;
    uint32_t    n = ic->code.len;
#define SESS_PFX(s) (n >= (uint32_t)(sizeof(s) - 1) && \
                     memcmp(p, (s), sizeof(s) - 1) == 0)
#define SESS_EVAL(dst, idx)                                             \
    TuriValue dst = eval_expr(env, frame, ic->val_exprs[idx]);          \
    if (turi_is_error(dst) || env_signaled(env)) { *out = dst; return true; }

    /* make-session: fresh channel, refcount 2, both endpoints alias it. */
    if (ic->n_val_exprs == 0 && SESS_PFX("tur_session_new(")) {
        TuriChan *ch = (TuriChan *)turi_val_calloc(env, sizeof(TuriChan));
        ch->refcount = 2;
        *out = turi_int((int64_t)(intptr_t)ch);
        return true;
    }
    /* endpoint alias: the whole body is bare `__TUR_VAL_0__` -- evaluate and
     * return the operand (the second endpoint / recv-pair channel pointer). */
    if (ic->n_val_exprs == 1 && n == 13 && memcmp(p, "__TUR_VAL_0__", 13) == 0) {
        SESS_EVAL(v, 0);
        *out = v;
        return true;
    }
    /* send: eval channel then value; cooperative send; return the channel. */
    if (ic->n_val_exprs == 2 && SESS_PFX("({ tur_session_send(")) {
        SESS_EVAL(cv, 0);
        SESS_EVAL(vv, 1);
        *out = session_send(env, (TuriChan *)(intptr_t)cv.as_int, vv);
        return true;
    }
    /* recv: eval channel; cooperative recv; return the received value. */
    if (ic->n_val_exprs == 1 && SESS_PFX("tur_session_recv(__TUR_VAL_0__)")) {
        SESS_EVAL(cv, 0);
        *out = session_recv(env, (TuriChan *)(intptr_t)cv.as_int);
        return true;
    }
    /* close: eval channel; drop refcount / wake peers. */
    if (ic->n_val_exprs == 1 && SESS_PFX("tur_session_close(__TUR_VAL_0__)")) {
        SESS_EVAL(cv, 0);
        *out = session_close(env, (TuriChan *)(intptr_t)cv.as_int);
        return true;
    }
    /* offer: eval channel; recv the peer's branch tag (0/1) on the branch slot.
     * The enclosing EX_MATCH (eval_match_resolve) selects the Left/Right arm on
     * this tag and binds the channel to the arm variable. */
    if (ic->n_val_exprs == 1 && SESS_PFX("tur_session_recv_tag(__TUR_VAL_0__)")) {
        SESS_EVAL(cv, 0);
        *out = session_recv_tag(env, (TuriChan *)(intptr_t)cv.as_int);
        return true;
    }
    /* choose-left / choose-right: eval channel; send branch tag 0 / 1. */
    if (ic->n_val_exprs == 1 &&
        SESS_PFX("({ tur_session_send_tag(")) {
        SESS_EVAL(cv, 0);
        /* The tag literal is baked into the template: `..., (int64_t)0)` or
         * `..., (int64_t)1)`.  Read the integer after the "(int64_t)" cast. */
        int tag = session_int_after(p, n, "(int64_t)");
        *out = session_send_tag(env, (TuriChan *)(intptr_t)cv.as_int,
                                tag == 1 ? 1 : 0);
        return true;
    }
    /* recv-timeout: eval channel + duration; timed recv on the data slot.
     * Returns 0 (success, value stashed in env->session_rtv) or 1 (timeout);
     * the enclosing EX_MATCH selects Left (0) / Right (1). */
    if (ic->n_val_exprs == 2 && SESS_PFX("tur_session_recv_timeout(")) {
        SESS_EVAL(cv, 0);
        SESS_EVAL(dv, 1);
        int64_t dur = (dv.tag == TURI_INT) ? dv.as_int : 0;
        *out = session_recv_timeout(env, (TuriChan *)(intptr_t)cv.as_int, dur);
        return true;
    }
    /* tur__rtv_: the recv-timeout Left arm's value slot -- return the value
     * stashed by the preceding session_recv_timeout success. */
    if (ic->n_val_exprs == 0 && n == 9 && memcmp(p, "tur__rtv_", 9) == 0) {
        *out = env->session_rtv;
        return true;
    }

    /* --- Multi-party roles (Slice D) ------------------------------------- */
    /* make-protocol vi=0: tur_make_roles(N, 0) -- new router + role 0. */
    if (ic->n_val_exprs == 0 && SESS_PFX("tur_make_roles(")) {
        int nr = session_int_after(p, n, "tur_make_roles(");
        *out = router_make_roles(env, nr, 0);
        return true;
    }
    /* make-protocol vi=k: tur_get_role(__TUR_VAL_0__, K) -- peer role. */
    if (ic->n_val_exprs == 1 && SESS_PFX("tur_get_role(__TUR_VAL_0__,")) {
        SESS_EVAL(bv, 0);
        int k = session_int_after(p, n, "tur_get_role(__TUR_VAL_0__, ");
        *out = router_get_role(env, (TuriRole *)(intptr_t)bv.as_int, k);
        return true;
    }
    /* send-to: tur_router_send(__TUR_VAL_0__, TO_IDX, (int64_t)(__TUR_VAL_1__)). */
    if (ic->n_val_exprs == 2 && SESS_PFX("({ tur_router_send(")) {
        SESS_EVAL(rv, 0);
        SESS_EVAL(vv, 1);
        int to_idx = session_int_after(p, n, "tur_router_send(__TUR_VAL_0__, ");
        *out = router_send(env, (TuriRole *)(intptr_t)rv.as_int, to_idx, vv);
        return true;
    }
    /* recv-from: tur_router_recv(__TUR_VAL_0__, FROM_IDX). */
    if (ic->n_val_exprs == 1 && SESS_PFX("tur_router_recv(__TUR_VAL_0__,")) {
        SESS_EVAL(rv, 0);
        int from_idx = session_int_after(p, n, "tur_router_recv(__TUR_VAL_0__, ");
        *out = router_recv(env, (TuriRole *)(intptr_t)rv.as_int, from_idx);
        return true;
    }
    /* role-close: tur_role_close((void *)__TUR_VAL_0__). */
    if (ic->n_val_exprs == 1 && SESS_PFX("tur_role_close((void *)__TUR_VAL_0__)")) {
        SESS_EVAL(rv, 0);
        *out = router_role_close(env, (TuriRole *)(intptr_t)rv.as_int);
        return true;
    }

#undef SESS_PFX
#undef SESS_EVAL
    return false;
}

/* -------------------------------------------------------------------------
 * Expression evaluator
 * ---------------------------------------------------------------------- */

#define MAX_EVAL_ARGS 64

static TuriValue eval_expr_impl(TuriEnv *env, EvalFrame *frame, const Expr *e);

static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e) {
    if (!e) return turi_nil();
    /* Debugger Phase 2: per-node breakpoint / step check.  A single NULL-pointer
     * load when no debugger is attached -- no measurable cost on plain runs. */
    if (env->debugger) turi_dbg_before_node(env, frame, e, /*from_driver=*/false);
    if (env->returning) return env->return_value;
    if (env->throwing)  return env->throw_value;
    if (env->aborting)  return env->abort_value;   /* SR N4: abort in flight */
    if (env->panicking) return env->catch_panic_value; /* C1: panic in flight */
    /* SB3: step-fuel check (skipped when limit == 0, i.e. unrestricted envs) */
    if (env->step_fuel_limit > 0) {
        if (env->step_fuel == 0)
            return turi_error("eval: step fuel exhausted");
        env->step_fuel--;
    }
    /* C4: the eval_depth recursion guard has been retired -- see eval_apply. */
    return eval_expr_impl(env, frame, e);
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
        return turi_default_of(env, &e->type);
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
            if (turi_is_error(cond) || env_signaled(env)) return cond;
            if (!turi_is_truthy(cond)) break;
            TuriValue body = eval_expr(env, frame, e->as.while_.body);
            if (turi_is_error(body) || env->returning) return body;
        }
        return turi_nil();
    }

    /* --- Set ------------------------------------------------------------- */
    case EX_SET: {
        TuriValue v = eval_expr(env, frame, e->as.set_.value);
        if (turi_is_error(v) || env_signaled(env)) return v;
        return eval_unary_post(env, frame, e, v);   /* SR N3: writes the binding */
    }

    /* --- Def (top-level binding) ---------------------------------------- */
    case EX_DEF: {
        TuriValue v = eval_expr(env, frame, e->as.def_.init);
        if (turi_is_error(v) || env_signaled(env)) return v;
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
            /* The env keeps this key for the binding's lifetime. Allocate it from
             * the env's sym_arena (which every EnvBinding->name is expected to
             * point into and which turi_env_free reclaims wholesale) rather than a
             * bare malloc -- otherwise the string is owned by the env yet never
             * released, and LeakSanitizer reports it as a direct leak at exit
             * (forcing a sanitizer build to exit non-zero on every clean run). */
            size_t need = strlen(modname) + 1 + strlen(fname) + 1;
            char *qk = (char *)arena_alloc(&env->sym_arena, need);
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
        TuriClosure *cl = (TuriClosure *)turi_val_alloc(env, sizeof(TuriClosure));
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

    /* --- Extern C declaration -- bind a real implementation --------------- */
    case EX_EXTERN_C: {
        ExternC *ec = e->as.extern_c_.ext;
        if (ec && ec->binding) {
            const char *fname = ec->binding->name->name;
            /* Only register if not already bound (avoid overwriting native impls). */
            TuriValue existing = turi_env_get(env, fname);
            if (existing.tag == TURI_ERROR)
                register_extern_c_binding(env, ec, fname);
        }
        return turi_nil();
    }

    /* --- Anonymous function (fn) ---------------------------------------- */
    case EX_FN: {
        TuriClosure *cl = (TuriClosure *)turi_val_alloc(env, sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl));
        cl->fn       = e->as.fn_.fn;
        cl->captured = frame; /* capture lexical scope */
        return turi_closure(cl);
    }

    /* --- Closure with captured variables (fn with captures) --------------- */
    case EX_CLOSURE: {
        TuriClosure *cl = (TuriClosure *)turi_val_alloc(env, sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl));
        cl->fn             = e->as.closure_.closure->fn;
        cl->captured       = frame; /* interpreter uses lexical frame */
        cl->skip_env_param = true;  /* codegen added __env_p as first param */
        /* Retain-on-capture parity: the compiled backend's closure env owns a
         * strong reference to each rc<T> it captures -- codegen stores the handle
         * into the env with a retain, so `rc/strong-count` inside (or alongside) a
         * capturing closure sees the +1.  The interpreter shares the elaborator
         * but not codegen, so without this a captured rc read back count 1 where
         * the compiled path reads 2 (rc-auto-drop-closure-capture and siblings).
         * Increment the strong count of every captured value that is an `__rc`
         * wrapper.  There is no matching decrement: interpreter frames are never
         * freed (eval_frame_free is a no-op, process-lifetime), so the closure
         * env has no drop point -- consistent with the interpreter's leak-on-exit
         * allocation model, and the enclosing binding's own auto-drop defer still
         * runs.  Detected structurally (struct name "__rc") exactly as rc/clone,
         * rc/drop, and rc/strong-count do, so no static capture-type info needed. */
        const struct Closure *cd = e->as.closure_.closure;
        for (uint8_t i = 0; i < cd->n_captures; i++) {
            if (!cd->captures[i] || !cd->captures[i]->name) continue;
            TuriValue cv = eval_lookup(env, frame, cd->captures[i]->name->name);
            if (cv.tag == TURI_STRUCT && cv.as_struct && cv.as_struct->name &&
                strcmp(cv.as_struct->name, "__rc") == 0 && cv.as_struct->n_fields >= 2) {
                int64_t *cnt = (int64_t *)(intptr_t)cv.as_struct->fields[0].as_int;
                if (cnt) (*cnt)++;
            }
        }
        return turi_closure(cl);
    }

    /* --- Function call --------------------------------------------------- */
    case EX_CALL:
        /* jit-ffi-c2mir-plan F3/F5: a `(call-ptr ...)` call routes through the
         * c2mir thunk provider, not the work-stack driver -- the callee is a
         * raw C address, so there is no turi closure to apply. */
        if (e->as.call_.ptr_sig)
            return e->as.call_.ptr_sig->is_callback
                       ? eval_callback_ptr(env, frame, e)
                       : eval_call_ptr(env, frame, e);
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
        return eval_unary_post(env, frame, e, v);   /* SR N3: sets returning */
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
            /* A bare inline-C block in module-body (statement) position is a
             * file-scope C declaration block -- typedefs / static helper fns the
             * module's inline-C constructors point at (e.g. stdlib/time.tur's
             * __tur_mock_cap + now/sleep statics).  It is codegen-only and has no
             * interpreter value, so skip it rather than tripping the "inline-C
             * not supported" carve.  A function's inline-C *body* lives inside an
             * EX_FN_DEF (still carved there if no native override exists); only a
             * standalone declaration block reaches here directly. */
            if (mod->body[i]->kind == EX_INLINE_C) continue;
            last = eval_expr(env, frame, mod->body[i]);
            if (turi_is_error(last) || env_signaled(env)) {
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
        /* Escaping payload: items + set struct are returned as a set carrier;
         * pool-owned (never individually freed). */
        int64_t *raw = raw_n ? (int64_t*)turi_val_alloc(env, raw_n * sizeof(int64_t)) : NULL;
        uint32_t k = 0;
        for (uint32_t si = 0; si < raw_n; si++) {
            TuriValue iv = eval_expr(env, frame, e->as.set_lit_.items[si]);
            if (turi_is_error(iv) || env_signaled(env)) {
                return iv;
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
        int64_t *s = (int64_t*)turi_val_alloc(env, 2 * sizeof(int64_t));
        s[0] = uniq ? (int64_t)(intptr_t)raw : 0;
        s[1] = (int64_t)uniq;
        TuriValue sv = {0}; sv.tag = TURI_INT; sv.as_int = (int64_t)(intptr_t)s;
        return sv;
    }

    case EX_CONS_LIST: {
        /* AR8 / turi-parity TI1.3: build a right-folded cons list matching the
         * codegen ABI.  The compiler lowers a variadic `& rest` argument tail to
         * a chain of `__tur_cons_of` cells -- `{ int64_t head; int64_t tail; }`
         * with the head pointer boxed as an int64 and nil == 0 (see
         * emit_expr.c:EX_CONS_LIST / emit_module.c:__tur_cons_of).  The
         * interpreter reproduces that exact layout so `(= rest 0)`, the
         * inline-C `__tur_cons_cell` walkers, and `stdlib/args.tur` all
         * interoperate with a rest list the tree-walker produced.
         *
         * Each head is the element's raw int64 value carrier -- the same
         * 8-byte payload codegen casts with `(int64_t)(intptr_t)(elem)` for an
         * int/cstr/opaque/bool element (the documented variadic element types).
         * Cells are process-lifetime, like the rest of the interpreter's
         * heap. */
        uint32_t cn = e->as.cons_list_.n;
        int64_t tail = 0;  /* nil sentinel */
        for (int32_t i = (int32_t)cn - 1; i >= 0; i--) {
            TuriValue iv = eval_expr(env, frame, e->as.cons_list_.items[i]);
            if (turi_is_error(iv) || env_signaled(env)) {
                /* Partial chain is pool-owned; reclaimed at turi_env_free. */
                return iv;
            }
            /* Escaping payload: cons cells form the returned chain; pool-owned. */
            int64_t *cell = (int64_t *)turi_val_alloc(env, 2 * sizeof(int64_t));
            cell[0] = iv.as_int;  /* head: raw value carrier */
            cell[1] = tail;       /* tail: previously-built cell (or nil) */
            tail = (int64_t)(intptr_t)cell;
        }
        TuriValue v = {0}; v.tag = TURI_INT; v.as_int = tail;
        return v;
    }

    case EX_GET_FIELD: {
        /* W1b: a struct can reach a field access via the int64 carrier ABI
         * rather than as a TuriStruct -- e.g. a Result that flowed as :int and
         * was ascribed back to (Result A B) with `(:: carrier ...)`.  The
         * carrier is a pointer to an int64[n] box laid out one word per field
         * (this is how native ok/err/result-map build a Result); get_field_extract
         * reads word idx and tags it by the field's static type, which is what
         * lets result.tur's accessors (ok-val/err-val/...) work on the native
         * box, not just on make-struct TuriStructs.  (Driver path: DK_GET_FIELD.) */
        TuriValue sv = eval_expr(env, frame, e->as.get_field_.struct_expr);
        if (turi_is_error(sv) || env_signaled(env)) return sv;
        return get_field_extract(e, sv);
    }

    /* --- Phase DS3: (set! (.field s) v) — struct field write ------------- */
    case EX_SET_FIELD: {
        TuriValue recv = eval_expr(env, frame, e->as.set_field_.receiver);
        if (turi_is_error(recv) || env_signaled(env)) return recv;
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
        if (turi_is_error(v) || env_signaled(env)) return v;
        s->fields[idx] = v;
        return turi_nil();
    }

    /* --- Phase S4: Defer ------------------------------------------------ */

    /* (defer body) — register body to fire at enclosing function exit. */
    case EX_DEFER: {
        /* Snapshot the captured bindings at defer-call time. */
        EvalFrame *snap = eval_frame_new(env, NULL);
        for (uint8_t i = 0; i < e->as.defer_.n_captures; i++) {
            Binding *b = e->as.defer_.captures[i];
            TuriValue v = eval_lookup(env, frame, b->name->name);
            frame_bind(env, snap, b->name->name, v);
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
        if (turi_is_error(root) || env_signaled(env)) return root;
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
            if (turi_is_error(ov) || env_signaled(env)) {
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
        if (turi_is_error(v) || env_signaled(env)) return v;
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

        for (uint32_t i = 0; i < n_args; i++) {
            args[i] = eval_expr(env, frame, pe->args[i]);
            if (turi_is_error(args[i]) || env_signaled(env)) return args[i];
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
        /* Escaping payload: a first-class handler value is returned; pool-owned. */
        TuriHandlerVal *hv = (TuriHandlerVal *)turi_val_calloc(env, sizeof(TuriHandlerVal));
        if (h->n_cases > TURI_MAX_HANDLER_CASES) {
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
        if (turi_is_error(v1) || env_signaled(env)) return v1;
        TuriValue v2 = eval_expr(env, frame, e->as.compose_handlers_.h2);
        if (turi_is_error(v2) || env_signaled(env)) return v2;
        if (v1.tag != TURI_HANDLER || v2.tag != TURI_HANDLER)
            return turi_error("eval: compose-handlers: operands must be handler values");
        TuriHandlerVal *a = v1.as_handler, *b = v2.as_handler;
        if ((uint32_t)a->n_cases + b->n_cases > TURI_MAX_HANDLER_CASES)
            return turi_error("eval: composed handler has too many cases");
        TuriHandlerVal *hv = (TuriHandlerVal *)turi_val_calloc(env, sizeof(TuriHandlerVal));
        for (uint8_t i = 0; i < a->n_cases; i++) hv->cases[hv->n_cases++] = a->cases[i];
        for (uint8_t i = 0; i < b->n_cases; i++) hv->cases[hv->n_cases++] = b->cases[i];
        return turi_handler_val(hv);
    }

    /* (with-handler hv body) — apply a handler value to a body (TI6). */
    case EX_WITH_HANDLER: {
        TuriValue hvv = eval_expr(env, frame, e->as.with_handler_.handler);
        if (turi_is_error(hvv) || env_signaled(env)) return hvv;
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
     * docs/archive/history/turi-select-needs-channel-primitives.md and the
     * "Not interpreted: carve-outs" section of docs/guides/eval-api.md. */
    case EX_SELECT:
        return turi_error("eval: select is not supported in interpreter mode "
                          "(channels require native primitives; use the compiled path)");

    /* (resume k value) — resume a live continuation with a value. */
    case EX_RESUME: {
        ResumeExpr *re = e->as.resume_.resume;
        TuriValue k   = eval_expr(env, frame, re->k);
        if (turi_is_error(k) || env_signaled(env)) return k;
        TuriValue val = eval_expr(env, frame, re->value);
        if (turi_is_error(val) || env_signaled(env)) return val;

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
        if (turi_is_error(k) || env_signaled(env)) return k;
        TuriValue exc = eval_expr(env, frame, de->exception);
        if (turi_is_error(exc) || env_signaled(env)) return exc;

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
        if (turi_is_error(k) || env_signaled(env)) return k;
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
        /* turi-session-types-plan: route the session inline-C templates
         * (make-session / send / recv / close) to the cooperative channel
         * runtime instead of the carve below.  Additive to the interpreter --
         * no codegen touched. */
        {
            InlineC *ic = e->as.inline_c_.inline_c;
            TuriValue sess_out;
            if (ic && eval_session_intercept(env, frame, ic, &sess_out))
                return sess_out;
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
        if (turi_is_error(cl_val) || env_signaled(env)) {
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
        /* Escaping payload: a fiber is linked into the scheduler/future and lives
         * until env teardown; pool-owned (its stack stays mmap/malloc below). */
        /* TuriFiber leads with a ucontext_t that requires 16-byte alignment;
         * the default pointer-aligned pool would trip UBSan and can corrupt
         * makecontext/swapcontext register save areas.  Request _Alignof. */
        TuriFiber *fiber = (TuriFiber *)turi_val_calloc_aligned(
            env, sizeof(TuriFiber), _Alignof(TuriFiber));

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
            return turi_error("eval: mmap failed for async fiber stack");
        }
#else
        fiber->stack = (char *)malloc(TURI_ASYNC_STACK_SIZE);
        if (!fiber->stack) {
            return turi_error("eval: malloc failed for async fiber stack");
        }
#endif
        /* turi-value-pool-residual-sites: track for reclaim in turi_env_free.
         * turi-async-fiber-stack-reclaim: keep the node so the scheduler can
         * munmap this stack early once the fiber reaches TURI_FIBER_DONE,
         * instead of holding it until env teardown (O(N) growth otherwise). */
        fiber->stack_node =
            turi_env_track_coro_stack(env, fiber->stack, TURI_ASYNC_STACK_SIZE);

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
        if (turi_is_error(fv) || env_signaled(env)) return fv;

        if (fv.tag != TURI_FUTURE)
            return turi_errorf("eval: await: expected a future, got tag %d", fv.tag);

        TuriFuture *f = fv.as_future;

        /* Already settled?  DEPR-R0 (throw-deprecation-plan): surface
         * rejections as TURI_REJECTION values rather than throwing. */
        if (f->state == TURI_FUTURE_RESOLVED) return f->result;
        if (f->state == TURI_FUTURE_REJECTED) {
            if (turi_is_rejection(f->result)) return f->result;
            if (turi_is_error(f->result))
                return turi_rejection(turi_error_message(f->result));
            return turi_rejection("future rejected");
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
            /* Resumed: future has settled.  DEPR-R0: see the already-
             * settled branch above; same rejection surfacing here. */
            if (f->state == TURI_FUTURE_RESOLVED) return f->result;
            if (f->state == TURI_FUTURE_REJECTED) {
                if (turi_is_rejection(f->result)) return f->result;
                if (turi_is_error(f->result))
                    return turi_rejection(turi_error_message(f->result));
                return turi_rejection("future rejected");
            }
            return turi_error("eval: await: unexpected future state");
        } else {
            /* Main (non-fiber) context: run event loop until future resolves. */
            return turi_await_future(env, f);
        }
    }

    /* --- Phase H §1: typeclass dictionary — return method closure ---------- */
    case EX_DICT: {
        TypeClassInstance *inst = e->as.dict_.instance;
        const char *mname = e->as.dict_.method_name;
        /* turi-dict-passing-plan: address-only mode (method_name == "") is the
         * dict VALUE the elaborator prepends at a dict-clone call site.  The
         * compiled path spells it `(int64_t)(intptr_t)&dict_C_T_singleton`;
         * the interpreter's dictionary IS the TypeClassInstance, carried in
         * the same int64 slot so the clone's dict param binds it like any
         * other arg.  (Previously nil, which starved the dict params.) */
        if (inst && (!mname || mname[0] == '\0'))
            return turi_int((int64_t)(intptr_t)inst);
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
            TuriClosure *cl = (TuriClosure *)turi_val_alloc(env, sizeof(TuriClosure));
            memset(cl, 0, sizeof(*cl));
            cl->fn       = impl;
            cl->captured = NULL;
            return turi_closure(cl);
        }
        return turi_errorf("eval: EX_DICT: method '%s' not found in instance", mname);
    }

    /* --- Phase N: numeric type cast ---------------------------------------- */
    case EX_CAST: {
        /* SR N3: post-operand coercion shared with the driver via
         * eval_unary_post (DK_UNARY). */
        TuriValue v = eval_expr(env, frame, e->as.cast_.expr);
        if (turi_is_error(v) || env_signaled(env)) return v;
        return eval_unary_post(env, frame, e, v);
    }
    case EX_REINTERPRET: {
        /* A same-size scalar bit-reinterpret on the compiled path (raw int64
         * carrier).  The interpreter is tag-preserving, so this is transparent
         * almost everywhere: a float that survived a carrier round-trip stays a
         * TURI_FLOAT (ADT/cons/tyvar slots, `(:: f float)`) and is read back
         * directly with no unbox.
         *
         * The ONE case that must reinterpret is the int-carrier -> float
         * direction when the runtime value is ACTUALLY a TURI_INT: a genuine
         * int64 carrier whose word holds IEEE-754 float bits -- e.g. a float
         * collected into a native int64[2] cons cell by the variadic rest
         * collector, then read back with `list-head` (which hands back a
         * TURI_INT) and ascribed `(:: ... :float)`.  Here EX_ASCRIBE would
         * reinterpret the bits; match it so the two `::` lowerings agree.
         *
         * The reverse (float -> int carrier) is left TRANSPARENT on purpose: the
         * tag-preserving interpreter keeps a "boxed" float as a TURI_FLOAT, so a
         * consumer that later reads the carrier back as a float still sees the
         * right value.  Converting float->int here would strip that tag and
         * print the raw bit pattern (it regressed typed-slots/cons-float,
         * poly-closure float results, the by-value Option float payloads, ...).
         * A genuine float<->carrier<->float round-trip still works: the
         * float->carrier leg stays TURI_FLOAT, so the carrier->float leg is
         * transparent too.  i32<->f32 and same-tag ascriptions stay transparent. */
        TuriValue v = eval_expr(env, frame, e->as.reinterpret_.expr);
        if (turi_is_error(v) || env_signaled(env)) return v;
        return eval_unary_post(env, frame, e, v);   /* SR N3: shared with DK_UNARY */
    }

    /* --- Phase 2: type ascription is transparent at runtime, except that it
     * must reconcile the runtime value tag with the ascribed primitive type.
     * The interpreter dispatches println/show on the runtime tag, so an int
     * literal ascribed to :bool (e.g. a return-type-directed Default[bool]
     * instance that returns `1`, then `(:: (default-of) bool)`) must become a
     * TURI_BOOL or it prints as `1` instead of `true`.  Mirror EX_CAST's
     * primitive coercions, but only when the tag actually mismatches so struct/
     * closure/ADT ascriptions stay transparent. */
    case EX_ASCRIBE: {
        /* `::` is a representation assertion, NOT a numeric conversion (that is
         * EX_CAST / explicit int->float).  On the compiled path the carrier word
         * is reinterpreted bit-for-bit to the ascribed type -- so an int64 that
         * carries a double's bits (e.g. a type-erased Map[int float] value read
         * back as :float, or a char* read back as :cstr) must REINTERPRET, not
         * convert.  The coercion (shared with the driver via eval_unary_post /
         * DK_UNARY) acts only on a tag mismatch so struct/closure/ADT ascriptions
         * stay transparent. */
        TuriValue v = eval_expr(env, frame, e->as.ascribe_.inner);
        if (turi_is_error(v) || env_signaled(env)) return v;
        return eval_unary_post(env, frame, e, v);
    }

    /* --- Phase N: poly wrap is transparent in the interpreter -------------- */
    case EX_POLY_WRAP: {
        /* turi-dict-passing-plan: a constrained rank-2 poly value resolves to
         * its dict-clone (leading dict params supplied by the call site). */
        TuriValue dcv;
        if (poly_wrap_dict_clone_value(env, frame, e, &dcv)) return dcv;
        return eval_expr(env, frame, e->as.poly_wrap_.inner);
    }

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
        if (turi_is_error(r) || env_signaled(env)) return r;
        if (r.tag == TURI_REF && r.as_ref)
            return ((EvalBinding *)r.as_ref)->value;
        return r;
    }
    case EX_SET_DEREF: {
        TuriValue ref = eval_expr(env, frame, e->as.set_deref_.ref);
        if (turi_is_error(ref) || env_signaled(env)) return ref;
        TuriValue v = eval_expr(env, frame, e->as.set_deref_.value);
        if (turi_is_error(v) || env_signaled(env)) return v;
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
        if (turi_is_error(pv) || env_signaled(env)) return pv;
        if (env->panicking || g_firing_panic_defer) {
            fprintf(stderr, "double panic: aborting\n");
            fflush(stderr);
            fflush(stdout);
            abort();
        }
        /* If a catch boundary is active, unwind to it carrying the payload.  C1:
         * a work-stack DK_CATCH_UNWIND boundary consumes the panic via the signal
         * (set env->panicking, return); a setjmp boundary is longjmp'd to. */
        TuriCatchBoundary *cb = g_catch_stack;
        if (cb && !env->in_no_unwind) {
            strncpy(env->catch_panic_msg, "typed panic", sizeof(env->catch_panic_msg) - 1);
            env->catch_panic_msg[sizeof(env->catch_panic_msg) - 1] = '\0';
            env->catch_panic_type  = e->as.panic_with_.payload->type.kind;
            env->catch_panic_value = pv;
            env->catch_panic_file  = NULL;
            env->catch_panic_line  = (int)e->span.line;
            env->panicking = true;
            if (cb->is_driver) return pv;   /* raise signal; driver unwinds */
            longjmp(*cb->jmp, 1);
        }
        env->panicking = true;
        fprintf(stderr, "panic at\n");
        fflush(stderr);
        fire_defers_to_mark_by_scope(env, NULL, NULL);
        fflush(stdout);
        exit(1);
    }

    /* --- Phase R2: catch-unwind — catch interpreter panics at a boundary --- */
    case EX_CATCH_UNWIND: {
        /* Evaluate the thunk expression to get a closure value.  volatile: it is
         * live across the setjmp below, so mark it to avoid the compiler warning
         * about a value potentially clobbered by longjmp. */
        volatile TuriValue thunk_val = eval_expr(env, frame, e->as.catch_unwind_.thunk);
        if (turi_is_error(thunk_val) || env_signaled(env)) return thunk_val;
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
        /* C1: register a setjmp boundary so a panic (in this thread's driver or a
         * nested black box) targets the innermost catch consistently. */
        TuriCatchBoundary cbnd = { .is_driver = false, .jmp = &jb, .prev = g_catch_stack };
        g_catch_stack = &cbnd;

        TuriValue result;
        if (setjmp(jb) == 0) {
            /* Normal execution path — call the thunk with no arguments. */
            result = eval_apply(env, thunk_val.as_closure, NULL, 0);
            g_catch_stack  = cbnd.prev;
            env->catch_jmp = prev_jmp;
            if (turi_is_error(result) || env_signaled(env))
                return result;
            /* Wrap the successful result in (ok value).  Use the same 3-int
             * Result-box layout { is_ok, ok_val, err_val } that the native
             * ok/err/ok?/err? helpers produce and read, so the value composes
             * with ok?/err?/ok-val just like any other Result (Phase R2). */
            return turi_ok_result_box(env, result);
        } else {
            /* A panic was caught — restore env and return (err payload).  TI5:
             * the err slot carries a boxed TuriPanicPayload so panic-payload-*
             * accessors recover the caught value/type/file/line (the message is
             * the cstr value for a plain panic). */
            g_catch_stack     = cbnd.prev;
            env->catch_jmp    = prev_jmp;
            env->panicking    = false;
            env->returning    = prev_returning;
            env->throwing     = prev_throwing;
            env->throw_value  = prev_throw;
            env->return_value = prev_ret;
            (void)prev_panicking;
            /* W4: fire the unwound frames' defers (LIFO) before returning, so
             * `(defer ...)` in the panicking thunk runs during the unwind. */
            fire_defers_to_mark_by_scope(env, defer_mark, frame);
            return turi_err_result_box(env);
        }
    }

    /* --- Phase TI5: catch-panic-of — type-filtered panic catch ------------- */
    case EX_CATCH_PANIC_OF: {
        /* volatile: live across the setjmp below (see catch-unwind above). */
        volatile TuriValue thunk_val = eval_expr(env, frame, e->as.catch_panic_of_.thunk);
        if (turi_is_error(thunk_val) || env_signaled(env)) return thunk_val;
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
        TuriCatchBoundary cbnd = { .is_driver = false, .jmp = &jb, .prev = g_catch_stack };
        g_catch_stack = &cbnd;

        TuriValue result;
        if (setjmp(jb) == 0) {
            result = eval_apply(env, thunk_val.as_closure, NULL, 0);
            g_catch_stack  = cbnd.prev;
            env->catch_jmp = prev_jmp;
            if (turi_is_error(result) || env_signaled(env))
                return result;
            return turi_ok_result_box(env, result);
        } else {
            /* A panic reached this boundary.  Restore the saved control state
             * (the payload fields in env still describe the in-flight panic). */
            g_catch_stack     = cbnd.prev;
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
             * the outer boundary sees the original panic.  C1: if the next
             * boundary is a work-stack DK_CATCH_UNWIND, return so the panic
             * propagates as a signal; a setjmp boundary is longjmp'd to. */
            TuriCatchBoundary *outer = g_catch_stack;
            if (outer && !env->in_no_unwind) {
                if (outer->is_driver) return env->catch_panic_value;
                longjmp(*outer->jmp, 1);
            }
            fprintf(stderr, "panic at\npanic: %s\n", env->catch_panic_msg);
            fflush(stderr);
            fire_defers_to_mark_by_scope(env, NULL, NULL);
            fflush(stdout);
            exit(1);
        }
    }

    /* --- Phase TI5: panic-payload-* accessors ------------------------------ */
    case EX_PANIC_PAYLOAD_TYPE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_type_.payload);
        if (turi_is_error(p) || env_signaled(env)) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return turi_int(pp ? pp->type_tag : 0);
    }
    case EX_PANIC_PAYLOAD_VALUE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_value_.payload);
        if (turi_is_error(p) || env_signaled(env)) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return pp ? pp->value : turi_nil();
    }
    case EX_PANIC_PAYLOAD_FILE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_file_.payload);
        if (turi_is_error(p) || env_signaled(env)) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return turi_cstr(pp && pp->file ? pp->file : "");
    }
    case EX_PANIC_PAYLOAD_LINE: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_line_.payload);
        if (turi_is_error(p) || env_signaled(env)) return p;
        TuriPanicPayload *pp = (TuriPanicPayload *)(intptr_t)p.as_int;
        return turi_int(pp ? pp->line : 0);
    }
    case EX_PANIC_PAYLOAD_DOWNS: {
        TuriValue p = eval_expr(env, frame, e->as.panic_payload_downs_.payload);
        if (turi_is_error(p) || env_signaled(env)) return p;
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
        if (turi_is_error(v) || env_signaled(env)) return v;
        int64_t *cnt = (int64_t *)turi_val_alloc(env, 2 * sizeof(int64_t));
        cnt[0] = 1; /* strong */
        cnt[1] = 0; /* weak */
        TuriValue fields[2];
        fields[0] = turi_int((int64_t)(intptr_t)cnt); /* pointer-as-int */
        fields[1] = v;
        return make_struct_val(env, "__rc", 2, fields);
    }
    case EX_RC_CLONE: {
        if (e->as.rc_clone_.elide)
            return eval_expr(env, frame, e->as.rc_clone_.expr);
        TuriValue r = eval_expr(env, frame, e->as.rc_clone_.expr);
        if (turi_is_error(r) || env_signaled(env)) return r;
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
                /* On reaching 0, run drop-glue over the inner value: this both
                 * follows a chain of nested rc<rc<...>> AND descends a by-value
                 * struct/ADT inner to release its own rc<T> fields (e.g. an rc<Box>
                 * whose Box carries an rc<int> payload). */
                if (*cnt == 0) turi_rc_drop_value(r.as_struct->fields[1]);
            }
        }
        return turi_nil();
    }
    case EX_RC_PTR: {
        TuriValue r = eval_expr(env, frame, e->as.rc_ptr_.expr);
        if (turi_is_error(r) || env_signaled(env)) return r;
        if (r.tag == TURI_STRUCT && r.as_struct
            && r.as_struct->name && strcmp(r.as_struct->name, "__rc") == 0
            && r.as_struct->n_fields >= 2)
            return r.as_struct->fields[1];
        return r;
    }
    case EX_RC_COUNT: {
        TuriValue r = eval_expr(env, frame, e->as.rc_count_.expr);
        if (turi_is_error(r) || env_signaled(env)) return r;
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
        if (turi_is_error(rv) || env_signaled(env)) return rv;
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
        if (turi_is_error(_rpv) || env_signaled(env)) return _rpv;
        return turi_bool(_rpv.tag != TURI_NIL);
    }

    /* --- Phase 9: weak<T> — simplified in interpreter --------------------- */
    case EX_WEAK: {
        /* weak is transparent (returns the underlying rc value), but it must
         * bump the control block's weak count so ref/from-rc's uniqueness check
         * can see the live weak alias (rc-unique-violation). */
        TuriValue wv = eval_expr(env, frame, e->as.weak_.expr);
        if (turi_is_error(wv) || env_signaled(env)) return wv;
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
        if (turi_is_error(_wuv) || env_signaled(env)) return _wuv;
        /* In the interpreter all weak refs are always valid; wrap in some(value). */
        return make_struct_val(env, "some", 1, &_wuv);
    }
    case EX_WEAK_PRED:
        return turi_bool(true);

    /* --- Phase HRT0: exists pack/open — erase existential box ------------- */
    case EX_EXISTS_PACK:
        return eval_expr(env, frame, e->as.exists_pack_.value);
    case EX_EXISTS_OPEN: {
        TuriValue packed = eval_expr(env, frame, e->as.exists_open_.packed);
        if (turi_is_error(packed) || env_signaled(env)) return packed;
        EvalFrame *ef = eval_frame_new(env, frame);
        if (e->as.exists_open_.var_binding)
            frame_bind(env, ef, e->as.exists_open_.var_binding->name->name, packed);
        TuriValue r = eval_expr(env, ef, e->as.exists_open_.body);
        eval_frame_free(ef);
        return r;
    }
    case EX_EXISTS_DISPATCH: {
        /* Witness-indirected `open`-site method dispatch.  The compiled path
         * reads the constraint witness packed into the existential record; the
         * interpreter erases that record (EX_EXISTS_PACK returns the bare
         * value), so it re-resolves the instance structurally from the
         * receiver's runtime concrete type.  This is exact for struct/ADT
         * payloads; opaque-over-primitive newtypes collapse to their carrier
         * here, so a heterogeneous opaque collection is a compiled-only
         * fixture (requires.compiled). */
        const TypeClass *tc = e->as.exists_dispatch_.typeclass;
        uint8_t mi = e->as.exists_dispatch_.method_idx;
        uint32_t na = e->as.exists_dispatch_.n_args;
        TuriValue *argv = na ? (TuriValue *)malloc(na * sizeof(TuriValue)) : NULL;
        for (uint32_t i = 0; i < na; i++) {
            argv[i] = eval_expr(env, frame, e->as.exists_dispatch_.args[i]);
            if (turi_is_error(argv[i]) || env_signaled(env)) {
                TuriValue r = argv[i]; free(argv); return r;
            }
        }
        TypeClassEnv *tce = (TypeClassEnv *)env->last_tc_env;
        TypeClassInstance *match = NULL;
        if (tce && tc) {
            const char *head = (na > 0) ? turi_struct_name(argv[0]) : NULL;
            TypeKind prim = TY_UNKNOWN;
            if (!head && na > 0) {
                switch (argv[0].tag) {
                    case TURI_INT:   prim = TY_INT;   break;
                    case TURI_BOOL:  prim = TY_BOOL;  break;
                    case TURI_CSTR:  prim = TY_CSTR;  break;
                    case TURI_FLOAT: prim = TY_FLOAT; break;
                    case TURI_NIL:   prim = TY_NIL;   break;
                    default: break;
                }
            }
            for (TypeClassInstance *inst = tce->instances; inst; inst = inst->next) {
                if (inst->typeclass != tc || inst->n_type_args == 0) continue;
                if (head) {
                    const char *ihead =
                        (inst->type_arg_syms && inst->type_arg_syms[0])
                            ? inst->type_arg_syms[0]->name : NULL;
                    if (ihead && strcmp(ihead, head) == 0) { match = inst; break; }
                } else if (prim != TY_UNKNOWN &&
                           inst->type_args[0].kind == prim) {
                    match = inst; break;
                }
            }
            if (!match) {
                /* Fallback: first instance of the class (single-instance case). */
                for (TypeClassInstance *inst = tce->instances; inst; inst = inst->next)
                    if (inst->typeclass == tc) { match = inst; break; }
            }
        }
        if (!match || mi >= match->n_method_impls || !match->method_impls[mi]) {
            free(argv);
            return turi_errorf("eval: EX_EXISTS_DISPATCH: no instance for "
                               "method in class '%s'",
                               (tc && tc->name) ? tc->name->name : "?");
        }
        TuriClosure *cl = (TuriClosure *)turi_val_alloc(env, sizeof(TuriClosure));
        memset(cl, 0, sizeof(*cl));
        cl->fn = match->method_impls[mi];
        cl->captured = NULL;
        TuriValue r = eval_apply(env, cl, argv, na);
        free(argv);
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
        if (turi_is_error(v) || env_signaled(env)) return v;
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
        case TY_ADT: {
            /* type-of-cast-kind-granularity: compare the TYPE, not the kind.
             * Casting an `any` holding a Point to Other used to pass here and
             * on the compiled path alike, handing back a reinterpreted value.
             * `e->type` is the named target the elaborator resolved. */
            const char *have = turi_any_named_type(v);
            const char *want = type_name(e->type);
            ok = (v.tag == TURI_STRUCT && have && want && strcmp(have, want) == 0);
            break;
        }
        default: ok = true; break;
        }
        if (!ok) {
            {
                const char *have = turi_any_named_type(v);
                char msg[160];
                snprintf(msg, sizeof(msg), "cast: any holds %s, not %s",
                         have ? have : "a value of a different type",
                         type_name(e->type));
                turi_runtime_panic(env, msg);
            }
            return turi_nil(); /* unreachable: turi_runtime_panic never returns */
        }
        return v;
    }

    case EX_ANY_TYPE_OF: {
        TuriValue v = eval_expr(env, frame, e->as.any_type_of_.value);
        if (turi_is_error(v) || env_signaled(env)) return v;
        const char *tname = "unknown";
        switch (v.tag) {
        case TURI_INT:     tname = "int";     break;
        case TURI_FLOAT:   tname = "float";   break;
        case TURI_BOOL:    tname = "bool";    break;
        case TURI_CSTR:    tname = "cstr";    break;
        case TURI_NIL:     tname = "nil";     break;
        case TURI_CLOSURE: tname = "fn";      break;
        case TURI_STRUCT: {
            /* Match the compiled __tur_any_type_name, which names the specific
             * type now (type-of-cast-kind-granularity) rather than answering
             * "struct" / "adt" for every struct and every ADT alike. */
            const char *named = turi_any_named_type(v);
            tname = named ? named
                          : ((v.as_struct && turi_struct_is_struct_like(v))
                                 ? "struct" : "adt");
            break;
        }
        default: break;
        }
        return turi_cstr(tname);
    }

    /* --- TY3: (is? x T) — runtime type test ------------------------------ */
    case EX_ANY_IS: {
        TuriValue v = eval_expr(env, frame, e->as.any_is_.value);
        if (turi_is_error(v) || env_signaled(env)) return v;
        /* Map the runtime TuriValue tag to a TypeKind and compare to test_tag. */
        /* A NAMED target compares by type identity, exactly as the compiled
         * per-monomorph box id does -- `(is? a Other)` on an `any` holding a
         * Point is false, where the old TypeKind compare said true for every
         * struct.  Primitives keep the kind compare. */
        const char *named = turi_any_named_type(v);
        if (named && e->as.any_is_.test_type.kind != TY_UNKNOWN) {
            const char *want = type_name(e->as.any_is_.test_type);
            return turi_bool(want && strcmp(named, want) == 0);
        }
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
        /* Escaping payload: a generator value is returned and may outlive the
         * creating scope; pool-owned (its coroutine stack stays mmap/malloc). */
        TuriGen *g = (TuriGen *)turi_val_calloc(env, sizeof(TuriGen));
        g->env     = env;
        g->body    = def->body;
        /* The body resolves its captures through a fresh child of the creating
         * frame; like a closure, this frame is intentionally not freed (the
         * generator may outlive the creating scope). */
        g->frame   = eval_frame_new(env, frame);
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
        if (turi_is_error(v) || env_signaled(env)) return v;
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
        if (turi_is_error(gv) || env_signaled(env)) return gv;
        if (gv.tag != TURI_GEN)
            return turi_errorf("eval: gen-next: expected a generator, got tag %d", gv.tag);
        return gen_advance(env, gv.as_gen);
    }

    /* (gen-done? g) -- has the generator been driven off its end? */
    case EX_GEN_DONE: {
        TuriValue gv = eval_expr(env, frame, e->as.gen_done_.gen_expr);
        if (turi_is_error(gv) || env_signaled(env)) return gv;
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
                                      EX_SERIAL_SHIFT, /*serial=*/true, NULL);
        return eval_reset_boundary(env, frame, e->as.serial_reset_.body,
                                   PROMPT_SERIAL);

    case EX_CLONEABLE_RESET:
        if (ts_reaches_shift(e->as.cloneable_reset_.body, EX_CLONEABLE_SHIFT))
            return ts_capture_and_run(env, frame, e->as.cloneable_reset_.body,
                                      EX_CLONEABLE_SHIFT, /*serial=*/false, NULL);
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
            if (turi_is_error(v) || env_signaled(env)) return v;
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
        if (turi_is_error(c) || env_signaled(env)) return c;
        /* Matches the compiled runtime: a failed check requests a retry. */
        if (!c.as_bool) g_stm_tx->retry_requested = true;
        return turi_nil();
    }

    case EX_OR_ELSE: {
        if (!g_stm_tx)
            return turi_error("eval: or-else used outside of an atomically block");
        bool retry_before = g_stm_tx->retry_requested;
        TuriValue v = eval_expr(env, frame, e->as.or_else_.stm1);
        if (turi_is_error(v) || env_signaled(env)) return v;
        /* If stm1 (and not a prior op) requested a retry, fall back to stm2. */
        if (!retry_before && g_stm_tx->retry_requested) {
            g_stm_tx->retry_requested = false;
            v = eval_expr(env, frame, e->as.or_else_.stm2);
        }
        return v;
    }

    case EX_TVAR_NEW: {
        TuriValue init = eval_expr(env, frame, e->as.tvar_new_.init);
        if (turi_is_error(init) || env_signaled(env)) return init;
        /* Escaping payload: the tvar is returned as an opaque int carrier, so
         * it must NOT live in the rewindable scratch pool -- see the TR3 note
         * at tvar_buf_destroy.  malloc + track: teardown (or the sweep, once
         * unreachable) reclaims it. */
        TuriTVar *tv = (TuriTVar *)calloc(1, sizeof(TuriTVar));
        if (!tv) return turi_error("tvar-new: out of memory");
        turi_env_track_collection(env, tv, tvar_buf_destroy, tvar_buf_scan);
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
        if (turi_is_error(val) || env_signaled(env)) return val;
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
        if (turi_is_error(nv) || env_signaled(env)) return nv;
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
        if (turi_is_error(ov) || env_signaled(env)) return ov;
        TuriValue nv = eval_expr(env, frame, e->as.tvar_cas_.new_val);
        if (turi_is_error(nv) || env_signaled(env)) return nv;
        if (stm_read(g_stm_tx, tv) == ov.as_int) {
            stm_log_write(g_stm_tx, tv, nv.as_int);
            return turi_bool(true);
        }
        return turi_bool(false);
    }

    case EX_TVAR_MODIFY: {
        /* Read-modify-write: r = fn(old); write r; return old.  NOTE: the
         * compiled path never reaches its EX_TVAR_MODIFY arm -- elab_tvar_modify
         * (elab_concurrent.c) lowers `(tvar/modify tv f)` to
         * `(let [g tv] (tvar/swap g (f (tvar/read g))))` first, so the arm in
         * emit_expr.c is a defensive stub, not a live no-op.  The interpreter
         * implements the semantics directly. */
        if (!g_stm_tx)
            return turi_error("eval: tvar/modify used outside of an atomically block");
        TuriValue err = turi_nil();
        TuriTVar *tv = stm_eval_tvar(env, frame, e->as.tvar_modify_.tvar, &err);
        if (!tv) return err;
        TuriValue fn = eval_expr(env, frame, e->as.tvar_modify_.fn);
        if (turi_is_error(fn) || env_signaled(env)) return fn;
        int64_t old = stm_read(g_stm_tx, tv);
        TuriValue arg = turi_int(old);
        TuriValue r = turi_call(env, fn, &arg, 1);
        if (turi_is_error(r) || env_signaled(env)) return r;
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
    default: {
        /* Named ADT / struct / record / parameterised head (Vec, Set, Map, a
         * user defstruct/defgadt): emit the surface constructor name so the
         * REPL can route it through its Show instance (turi_try_show_by_tag). */
        const char *hn = gde_type_head_name(&t);
        if (hn) snprintf(buf, cap, "%s", hn);
        else    snprintf(buf, cap, "unknown");
        break;
    }
    }
}

static TuriValue turi_eval_impl(TuriEnv *env, const char *src, const char *path,
                                 char *out_type_tag, size_t tag_cap);

/* Gap 3: forward an env's diagnostics from the process-global diag sink to the
 * env-specific TuriDiagSinkFn (ud carries the TuriEnv*). */
static void turi_diag_sink_trampoline(DiagLevel level, const char *code,
                                      const char *file, uint32_t line,
                                      uint32_t col_start, uint32_t col_end,
                                      const char *message, void *ud) {
    TuriEnv *env = (TuriEnv *)ud;
    if (env && env->diag_sink)
        env->diag_sink(env, (int)level, code, file, line, col_start, col_end,
                       message, env->diag_sink_ud);
}

/* Run turi_eval_impl with this env's diagnostic sink (if any) installed into
 * the global diag layer, restoring the previous sink afterward.  Centralises
 * the save/install/restore so every public eval entry shares one code path. */
static TuriValue turi_eval_with_sink(TuriEnv *env, const char *src, const char *path,
                                     char *out_type_tag, size_t tag_cap) {
    /* Gap 7: snapshot this env's interpret-mode bit into the process-global
     * elaborator flag for the duration of the call, then restore.  Lets two
     * co-resident libturi embedders run with different modes without the later
     * one's turi_env_new (which sets g_interpret_mode = true) sticking the flag
     * for the other.  A NULL env falls through to turi_eval_impl's own guard. */
    bool saved_mode = g_interpret_mode;
    if (env) g_interpret_mode = env->interpret_mode;

    TuriValue r;
    if (!env || !env->diag_sink) {
        r = turi_eval_impl(env, src, path, out_type_tag, tag_cap);
    } else {
        void      *prev_ud = NULL;
        DiagSinkFn prev    = diag_get_sink(&prev_ud);
        diag_set_sink(turi_diag_sink_trampoline, env);
        r = turi_eval_impl(env, src, path, out_type_tag, tag_cap);
        diag_set_sink(prev, prev_ud);
    }

    g_interpret_mode = saved_mode;
    return r;
}

TuriValue turi_eval(TuriEnv *env, const char *src) {
    return turi_eval_with_sink(env, src, "<eval>", NULL, 0);
}

TuriValue turi_eval_typed(TuriEnv *env, const char *src,
                           char *out_type_tag, size_t tag_cap) {
    return turi_eval_with_sink(env, src, "<eval>", out_type_tag, tag_cap);
}

TuriValue turi_eval_with_path(TuriEnv *env, const char *src, const char *path) {
    return turi_eval_with_sink(env, src, path, NULL, 0);
}

TuriValue turi_eval_with_path_typed(TuriEnv *env, const char *src, const char *path,
                                    char *out_type_tag, size_t tag_cap) {
    return turi_eval_with_sink(env, src, path, out_type_tag, tag_cap);
}

/* ===========================================================================
 * turi-value-pool-scratch-promotion-plan: scratch/permanent value-pool split
 * with escape promotion.
 *
 * With env->scratch_promotion enabled (opt-in; off by default), turi_eval
 * rewinds env->value_scratch at each top-level boundary so transient per-eval
 * allocation does not accumulate in a long-lived env.  Any value that must
 * survive the rewind -- the eval's result and every global binding, plus the
 * closures/frames/structs they reach -- is first deep-copied into
 * env->value_perm, with pointers rewritten to the copies.
 *
 * The walk is deliberately CONSERVATIVE: it relocates only the value shapes it
 * can prove are safe to copy (scalars, strings, closures, captured
 * frames/bindings/tyvars, structs + fields).  When an escaping value reaches
 * something it cannot safely relocate -- a carrier-encoded pointer boxed as a
 * bare int, a live continuation / generator / handler / future, a mutable ref
 * or opaque native user-data resident in scratch -- the whole cycle is left
 * intact (no rewind).  Correctness never depends on catching every shape: a
 * missed shape means "this eval does not shrink", never "use-after-reset".  A
 * Debug build additionally poisons the rewound region (arena_reset) so any
 * straggler that slipped through crashes loudly under ASan instead of reading
 * stale bytes.
 * ======================================================================== */

/* Open-addressing pointer->pointer map: doubles as the check pass's visited set
 * (value ignored) and the copy pass's old->new forwarding table (Cheney-style,
 * out of band so a mid-walk bail never stamps the scratch objects). */
typedef struct { void *key; void *val; } PromoSlot;
typedef struct {
    PromoSlot *slots;
    uint32_t   cap;    /* power of two, or 0 before first insert */
    uint32_t   count;
} PromoMap;

static void promo_map_init(PromoMap *m) { m->slots = NULL; m->cap = 0; m->count = 0; }
static void promo_map_free(PromoMap *m) { free(m->slots); m->slots = NULL; m->cap = 0; m->count = 0; }

/* MurmurHash3 64-bit finalizer. Mix at a fixed 64-bit width -- a `uintptr_t`
   here would be 32 bits on wasm32, making `>> 33` an over-wide shift (UB) and
   truncating the multiplier. On LP64 this is bit-identical to the pointer-width
   form. */
static uint32_t promo_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}

static void promo_map_grow(PromoMap *m) {
    uint32_t ncap = m->cap ? m->cap * 2 : 64;
    PromoSlot *ns = (PromoSlot *)calloc(ncap, sizeof(PromoSlot));
    if (!ns) { fprintf(stderr, "tur: out of memory (promotion map)\n"); abort(); }
    for (uint32_t i = 0; i < m->cap; i++) {
        if (!m->slots[i].key) continue;
        uint32_t j = promo_hash(m->slots[i].key) & (ncap - 1);
        while (ns[j].key) j = (j + 1) & (ncap - 1);
        ns[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
}

/* Return the stored value for key, or NULL if absent. */
static void *promo_map_get(PromoMap *m, const void *key) {
    if (!m->cap) return NULL;
    uint32_t j = promo_hash(key) & (m->cap - 1);
    while (m->slots[j].key) {
        if (m->slots[j].key == key) return m->slots[j].val;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

/* Insert key->val (val may be a non-NULL marker for the visited set). */
static void promo_map_put(PromoMap *m, void *key, void *val) {
    if (!m->cap || m->count * 10 >= m->cap * 7) promo_map_grow(m);
    uint32_t j = promo_hash(key) & (m->cap - 1);
    while (m->slots[j].key) {
        if (m->slots[j].key == key) { m->slots[j].val = val; return; }
        j = (j + 1) & (m->cap - 1);
    }
    m->slots[j].key = key;
    m->slots[j].val = val;
    m->count++;
}

/* True when env is at a clean top-level boundary with no live control-flow or
 * async state that could hold scratch pointers outside the promotion roots.
 * Any such state means the rewind is unsafe -- keep scratch this cycle. */
static bool promo_env_quiescent(const TuriEnv *env) {
    return !env->handler_stack && !env->defer_stack &&
           !env->sched_ready_head && !env->sched_ready_tail &&
           !env->current_fiber && !env->all_futures &&
           !env->timers_head && !env->io_pending_head &&
           !env_signaled(env) && !env->in_no_unwind;
}

#define PROMO_SCRATCH(env, p) arena_owns(&(env)->value_scratch, (const void *)(p))

/* ---- Pass 1: read-only promotability check ---------------------------------
 * Returns false as soon as a scratch-resident value cannot be safely relocated. */
static bool promo_check(TuriEnv *env, TuriValue v, PromoMap *seen);

/* A captured work-stack frame's `aux` is an OWNED TuriValue[] argument
 * accumulator only for the three arg-evaluating kinds (mirrors the capture and
 * clone_ws_slice logic).  For every other kind a non-NULL aux is a defer-stack
 * mark, a nested prompt's HandleExpr, a catch/stm/native-resume boundary, or a
 * cont-fold state -- none of which the walk relocates -- so returning 0 makes
 * the wscont walk bail on any slice that carries one.  Only the first `index`
 * slots of an accumulator are live (filled left-to-right, see DK_BUILTIN_ARG /
 * DK_CALL_ARG / DK_MAKE_STRUCT); the caller uses `index`, not the returned
 * capacity, to bound its relocation. */
static size_t promo_wscont_aux_cap(const DriveCont *d) {
    switch (d->kind) {
    case DK_BUILTIN_ARG: return d->expr->as.builtin.n;
    case DK_CALL_ARG:    return d->expr->as.call_.n_args;
    case DK_MAKE_STRUCT: return d->expr->as.make_struct_.n_fields;
    default:             return 0;
    }
}

static bool promo_check_frame(TuriEnv *env, EvalFrame *f, PromoMap *seen);

/* A heap-owned work-stack continuation (the escaping `k` of a capturable
 * handle) is relocatable when every scratch pointer it reaches can be rewritten:
 * its captured DriveCont slice (each frame's lexical EvalFrame, its `last`
 * value, and the live prefix of any argument accumulator), plus the handler's
 * lexical frame.  `handler` is elaborator/heap AST and `perf_module` is
 * interned/AST text -- neither is walked here.  A saved defer stack
 * (`perf_defer`) or any non-accumulator `aux` in the slice would need graph we
 * do not copy, so those bail (conservative: keep scratch this cycle). */
static bool promo_check_wscont(TuriEnv *env, TuriWsCont *wc, PromoMap *seen) {
    if (!wc) return true;
    if (PROMO_SCRATCH(env, wc)) {
        if (promo_map_get(seen, wc)) return true;   /* cycle / shared */
        promo_map_put(seen, wc, wc);
    }
    if (wc->perf_defer) return false;   /* saved defer chain not relocated */
    if (!promo_check_frame(env, wc->handler_frame, seen)) return false;
    for (size_t i = 0; i < wc->n_frames; i++) {
        DriveCont *d = &wc->frames[i];
        if (!promo_check_frame(env, d->frame, seen)) return false;
        if (!promo_check(env, d->last, seen)) return false;
        if (d->aux) {
            size_t cap = promo_wscont_aux_cap(d);
            if (!cap) return false;     /* non-accumulator aux -> bail */
            TuriValue *acc = (TuriValue *)d->aux;
            for (uint32_t j = 0; j < d->index && j < cap; j++)
                if (!promo_check(env, acc[j], seen)) return false;
        }
    }
    return true;
}

static bool promo_check_frame(TuriEnv *env, EvalFrame *f, PromoMap *seen) {
    while (f) {
        if (!PROMO_SCRATCH(env, f)) return true;   /* already permanent */
        if (promo_map_get(seen, f)) return true;   /* cycle / shared */
        promo_map_put(seen, f, f);
        for (EvalBinding *b = f->bindings; b; b = b->next) {
            if (PROMO_SCRATCH(env, b) && promo_map_get(seen, b)) continue;
            if (PROMO_SCRATCH(env, b)) promo_map_put(seen, b, b);
            if (!promo_check(env, b->value, seen)) return false;
        }
        /* tyvars carry Type descriptors (elaborator memory), no TuriValues. */
        f = f->parent;
    }
    return true;
}

static bool promo_check(TuriEnv *env, TuriValue v, PromoMap *seen) {
    switch (v.tag) {
    case TURI_NIL: case TURI_BOOL: case TURI_FLOAT:
        return true;
    case TURI_INT:
        /* A bare int that happens to address the scratch region is almost
         * certainly a carrier-encoded pointer (cons cell, set, vec, ADT box)
         * we cannot walk -- refuse to rewind under it. */
        return !PROMO_SCRATCH(env, (void *)(intptr_t)v.as_int);
    case TURI_CSTR: case TURI_ERROR: case TURI_REJECTION: case TURI_STRUCT_TYPE:
        return true;   /* strings copy safely whether scratch or not */
    case TURI_CLOSURE: {
        TuriClosure *cl = v.as_closure;
        if (!cl || !PROMO_SCRATCH(env, cl)) return true;
        if (promo_map_get(seen, cl)) return true;
        promo_map_put(seen, cl, cl);
        /* Opaque native user-data resident in scratch cannot be relocated. */
        if (cl->native && PROMO_SCRATCH(env, cl->native_ud)) return false;
        return promo_check_frame(env, cl->captured, seen);
    }
    case TURI_STRUCT: {
        TuriStruct *s = v.as_struct;
        if (!s || !PROMO_SCRATCH(env, s)) return true;
        if (promo_map_get(seen, s)) return true;
        promo_map_put(seen, s, s);
        for (uint32_t i = 0; i < s->n_fields; i++)
            if (!promo_check(env, s->fields[i], seen)) return false;
        return true;
    }
    case TURI_REF:
        /* A mutable borrow into scratch aliases a slot we do not own -- unsafe. */
        return !PROMO_SCRATCH(env, v.as_ref);
    case TURI_EFFECT_CONT: {
        /* carrier-relocation-plan Part 2: a heap-owned work-stack continuation
         * (ws != NULL) is relocatable -- its captured DriveCont slice and saved
         * frames are plain pool memory the walk can rewrite.  A ucontext body-
         * fiber continuation (ws == NULL) owns a live coroutine stack whose C
         * frames reference scratch and cannot be rewritten, so it keeps bailing
         * (the same argument as a suspended generator); the fiber path also
         * calloc's its TuriEffectCont off-pool, so !PROMO_SCRATCH already lets an
         * escaped fiber cont pass without blocking the rewind. */
        TuriEffectCont *c = v.as_cont;
        if (!c) return true;
        if (!c->ws) return !PROMO_SCRATCH(env, c);   /* fiber cont: unchanged */
        if (!PROMO_SCRATCH(env, c)) return true;     /* already relocated whole */
        if (promo_map_get(seen, c)) return true;
        promo_map_put(seen, c, c);
        return promo_check_wscont(env, c->ws, seen);
    }
    case TURI_THROW:       return !PROMO_SCRATCH(env, v.as_throw);
    case TURI_FUTURE:      return !PROMO_SCRATCH(env, v.as_future);
    case TURI_GEN: {
        /* turi-value-pool-carrier-relocation-plan Part 2: a generator is
         * relocatable only when it holds NO live coroutine-stack state that
         * points into scratch.  makecontext + the mmap/malloc stack are set up
         * lazily on the first `gen-next` (see gen_advance), so an UNSTARTED
         * generator has an empty stack and only its captured frame + error slot
         * reach scratch -- both relocatable.  A COMPLETED generator never
         * resumes (gen-next returns exhausted without touching the stack), so it
         * is safe too.  A suspended generator (started, not done) has live
         * interpreter C frames on its coroutine stack that reference scratch and
         * cannot be rewritten -- it must keep bailing. */
        TuriGen *g = v.as_gen;
        if (!g) return true;
        /* The started-not-done bail is checked BEFORE the perm short-circuit: a
         * suspended generator holds scratch pointers on its coroutine stack even
         * once the TuriGen struct itself has been promoted to perm, so it must
         * block the rewind wherever it lives.  (An unstarted generator relocated
         * to perm that a later gen-next starts would otherwise escape the bail
         * via !PROMO_SCRATCH and let the next boundary rewind live coroutine
         * state.) */
        if (g->started && !g->done) return false;
        if (!PROMO_SCRATCH(env, g)) return true;
        if (promo_map_get(seen, g)) return true;
        promo_map_put(seen, g, g);
        if (!promo_check(env, g->error_val, seen)) return false;
        return promo_check_frame(env, g->frame, seen);
    }
    case TURI_HANDLER: {
        /* A detached handler value is just n_cases + an array of HandleCase
         * pointers into the elaborator AST (permanent).  The struct itself is
         * the only scratch object; copying it is always safe. */
        return true;
    }
    case TURI_SYNTAX:
        /* The wrapped Form* lives in an arena (env sym_arena / eval arena /
         * elaborator arena), never in scratch; the value itself is trivially
         * relocatable. */
        return true;
    }
    return false;  /* unknown tag: refuse to rewind */
}

/* ---- Pass 2: deep copy scratch payloads into value_perm --------------------
 * Only runs after promo_check confirmed the whole root set is relocatable, so
 * every case here is reachable-and-safe; unhandled shapes were rejected already. */
static TuriValue promo_copy(TuriEnv *env, TuriValue v, PromoMap *fwd);

static EvalBinding *promo_copy_bindings(TuriEnv *env, EvalBinding *b, PromoMap *fwd) {
    if (!b || !PROMO_SCRATCH(env, b)) return b;
    void *seen = promo_map_get(fwd, b);
    if (seen) return (EvalBinding *)seen;
    EvalBinding *nb = (EvalBinding *)turi_val_perm_alloc(env, sizeof *nb);
    promo_map_put(fwd, b, nb);
    nb->name  = b->name;                       /* interned in sym_arena */
    nb->value = promo_copy(env, b->value, fwd);
    nb->next  = promo_copy_bindings(env, b->next, fwd);
    return nb;
}

static TyvarBind *promo_copy_tyvars(TuriEnv *env, TyvarBind *t, PromoMap *fwd) {
    if (!t || !PROMO_SCRATCH(env, t)) return t;
    void *seen = promo_map_get(fwd, t);
    if (seen) return (TyvarBind *)seen;
    TyvarBind *nt = (TyvarBind *)turi_val_perm_alloc(env, sizeof *nt);
    promo_map_put(fwd, t, nt);
    nt->name = t->name;                        /* interned */
    nt->type = t->type;                        /* elaborator Type (not scratch) */
    nt->next = promo_copy_tyvars(env, t->next, fwd);
    return nt;
}

static EvalFrame *promo_copy_frame(TuriEnv *env, EvalFrame *f, PromoMap *fwd) {
    if (!f || !PROMO_SCRATCH(env, f)) return f;
    void *seen = promo_map_get(fwd, f);
    if (seen) return (EvalFrame *)seen;
    EvalFrame *nf = (EvalFrame *)turi_val_perm_alloc(env, sizeof *nf);
    promo_map_put(fwd, f, nf);
    nf->parent   = promo_copy_frame(env, f->parent, fwd);
    nf->bindings = promo_copy_bindings(env, f->bindings, fwd);
    nf->tyvars   = promo_copy_tyvars(env, f->tyvars, fwd);
    return nf;
}

/* Relocate a work-stack continuation graph into value_perm.  Only runs after
 * promo_check_wscont proved the whole slice relocatable, so every scratch
 * pointer here has a safe copy.  The DriveCont slots copy verbatim (kind / expr
 * / index / tail / *_module AST text) with the three scratch-reachable members
 * rewritten: the lexical frame, the `last` value, and -- for the live prefix of
 * an argument accumulator -- each filled slot.  Unfilled accumulator slots keep
 * their raw bytes (overwritten before they are read on resume, exactly as
 * clone_ws_slice leaves them). */
static TuriWsCont *promo_copy_wscont(TuriEnv *env, TuriWsCont *wc, PromoMap *fwd) {
    if (!wc || !PROMO_SCRATCH(env, wc)) return wc;
    void *seen = promo_map_get(fwd, wc);
    if (seen) return (TuriWsCont *)seen;
    TuriWsCont *nw = (TuriWsCont *)turi_val_perm_alloc(env, sizeof *nw);
    promo_map_put(fwd, wc, nw);
    *nw = *wc;   /* perf_no_unwind, perf_defer(NULL); handler/module fixed below */
    if (nw->perf_module && PROMO_SCRATCH(env, nw->perf_module))
        nw->perf_module = turi_val_perm_strdup(env, nw->perf_module);
    /* The outer prompt's HandleExpr: for an inline (handle ...) it is elaborator
     * AST (permanent), but (with-handler hv body) heap-owns a fresh HandleExpr +
     * cases array in the value pool (EX_WITH_HANDLER), so a scratch one must be
     * relocated or it dangles into the rewound region.  Its cases carry only
     * AST/interned pointers, so the struct + cases array copy verbatim; the fwd
     * map shares one copy across every ws cont captured under the same prompt. */
    if (nw->handler && PROMO_SCRATCH(env, nw->handler)) {
        void *hseen = promo_map_get(fwd, nw->handler);
        if (hseen) {
            nw->handler = (HandleExpr *)hseen;
        } else {
            HandleExpr *nh = (HandleExpr *)turi_val_perm_alloc(env, sizeof *nh);
            promo_map_put(fwd, wc->handler, nh);
            *nh = *wc->handler;
            if (wc->handler->n_cases && wc->handler->cases &&
                PROMO_SCRATCH(env, wc->handler->cases)) {
                size_t nb = (size_t)wc->handler->n_cases * sizeof(HandleCase);
                HandleCase *ncs = (HandleCase *)turi_val_perm_alloc(env, nb);
                memcpy(ncs, wc->handler->cases, nb);
                nh->cases = ncs;
            }
            nw->handler = nh;
        }
    }
    nw->handler_frame = promo_copy_frame(env, wc->handler_frame, fwd);
    if (wc->n_frames && wc->frames) {
        nw->frames = (DriveCont *)turi_val_perm_alloc(env, wc->n_frames * sizeof(DriveCont));
        for (size_t i = 0; i < wc->n_frames; i++) {
            DriveCont d = wc->frames[i];   /* verbatim: kind/expr/index/tail/... */
            d.frame = promo_copy_frame(env, wc->frames[i].frame, fwd);
            d.last  = promo_copy(env, wc->frames[i].last, fwd);
            if (d.saved_module && PROMO_SCRATCH(env, d.saved_module))
                d.saved_module = turi_val_perm_strdup(env, d.saved_module);
            if (wc->frames[i].aux) {
                size_t cap = promo_wscont_aux_cap(&wc->frames[i]);
                TuriValue *oacc = (TuriValue *)wc->frames[i].aux;
                TuriValue *nacc = (TuriValue *)turi_val_perm_alloc(env, cap * sizeof(TuriValue));
                memcpy(nacc, oacc, cap * sizeof(TuriValue));   /* keep unfilled tail */
                for (uint32_t j = 0; j < d.index && j < cap; j++)
                    nacc[j] = promo_copy(env, oacc[j], fwd);   /* relocate live prefix */
                d.aux = nacc;
            }
            nw->frames[i] = d;
        }
    }
    return nw;
}

static TuriValue promo_copy(TuriEnv *env, TuriValue v, PromoMap *fwd) {
    switch (v.tag) {
    case TURI_CSTR:
        if (PROMO_SCRATCH(env, v.as_cstr)) v.as_cstr = turi_val_perm_strdup(env, v.as_cstr);
        return v;
    case TURI_ERROR: case TURI_REJECTION:
        if (PROMO_SCRATCH(env, v.as_error)) v.as_error = turi_val_perm_strdup(env, v.as_error);
        return v;
    case TURI_STRUCT_TYPE:
        if (PROMO_SCRATCH(env, v.as_cstr)) v.as_cstr = turi_val_perm_strdup(env, v.as_cstr);
        return v;
    case TURI_CLOSURE: {
        TuriClosure *cl = v.as_closure;
        if (!cl || !PROMO_SCRATCH(env, cl)) return v;
        void *seen = promo_map_get(fwd, cl);
        if (seen) return turi_closure((TuriClosure *)seen);
        TuriClosure *nc = (TuriClosure *)turi_val_perm_alloc(env, sizeof *nc);
        promo_map_put(fwd, cl, nc);
        *nc = *cl;                             /* fn/native/native_ud/module/... */
        nc->captured = promo_copy_frame(env, cl->captured, fwd);
        if (nc->module && PROMO_SCRATCH(env, nc->module))
            nc->module = turi_val_perm_strdup(env, nc->module);
        return turi_closure(nc);
    }
    case TURI_STRUCT: {
        TuriStruct *s = v.as_struct;
        if (!s || !PROMO_SCRATCH(env, s)) return v;
        void *seen = promo_map_get(fwd, s);
        if (seen) return turi_struct_val((TuriStruct *)seen);
        TuriStruct *ns = (TuriStruct *)turi_val_perm_alloc(env, sizeof *ns);
        promo_map_put(fwd, s, ns);
        *ns = *s;                              /* name/n_fields/ctor */
        if (s->n_fields && s->fields) {
            ns->fields = (TuriValue *)turi_val_perm_alloc(env, s->n_fields * sizeof(TuriValue));
            for (uint32_t i = 0; i < s->n_fields; i++)
                ns->fields[i] = promo_copy(env, s->fields[i], fwd);
        }
        if (ns->name && PROMO_SCRATCH(env, ns->name))
            ns->name = turi_val_perm_strdup(env, ns->name);
        return turi_struct_val(ns);
    }
    case TURI_GEN: {
        /* Part 2: relocate an unstarted/completed generator.  promo_check
         * already refused any suspended (started, not-done) generator, so the
         * coroutine stack here holds no live scratch pointers.  Copy the struct
         * verbatim (env/body/stack/contexts/flags/box) and deep-copy the two
         * members that reach scratch -- the captured frame and the error slot. */
        TuriGen *g = v.as_gen;
        if (!g || !PROMO_SCRATCH(env, g)) return v;
        void *seen = promo_map_get(fwd, g);
        if (seen) return turi_gen_val((TuriGen *)seen);
        TuriGen *ng = (TuriGen *)turi_val_perm_alloc(env, sizeof *ng);
        promo_map_put(fwd, g, ng);
        *ng = *g;
        ng->frame     = promo_copy_frame(env, g->frame, fwd);
        ng->error_val = promo_copy(env, g->error_val, fwd);
        return turi_gen_val(ng);
    }
    case TURI_EFFECT_CONT: {
        /* Part 2: relocate an escaping work-stack continuation.  promo_check
         * already refused the ucontext (ws == NULL) variant, so any scratch
         * cont reaching here is a heap-owned ws continuation.  Copy the struct
         * verbatim (the ucontext/stack members are zero for a ws cont, and
         * body_result/resume_val stay nil) and deep-copy the ws graph. */
        TuriEffectCont *c = v.as_cont;
        if (!c || !PROMO_SCRATCH(env, c)) return v;
        void *seen = promo_map_get(fwd, c);
        if (seen) return turi_effect_cont((TuriEffectCont *)seen);
        TuriEffectCont *nc = (TuriEffectCont *)turi_val_perm_alloc(env, sizeof *nc);
        promo_map_put(fwd, c, nc);
        *nc = *c;
        nc->ws = promo_copy_wscont(env, c->ws, fwd);
        return turi_effect_cont(nc);
    }
    case TURI_HANDLER: {
        /* Part 2: relocate a detached handler value.  Its cases[] array is
         * embedded in the struct and points at permanent elaborator AST, so a
         * verbatim struct copy is a complete relocation. */
        TuriHandlerVal *hv = v.as_handler;
        if (!hv || !PROMO_SCRATCH(env, hv)) return v;
        void *seen = promo_map_get(fwd, hv);
        if (seen) return turi_handler_val((TuriHandlerVal *)seen);
        TuriHandlerVal *nhv = (TuriHandlerVal *)turi_val_perm_alloc(env, sizeof *nhv);
        promo_map_put(fwd, hv, nhv);
        *nhv = *hv;
        return turi_handler_val(nhv);
    }
    case TURI_SYNTAX:
        /* Arena-resident Form* (never scratch): nothing to relocate. */
        return v;
    default:
        /* Scalars and the shapes promo_check proved are non-scratch: unchanged. */
        return v;
    }
}

/* ---- TR3: eval-boundary collection sweep ----------------------------------
 *
 * Runs only immediately after a successful promotion rewind, which is the
 * moment the live value graph is provably exactly what is reachable from the
 * (already promoted) eval result plus the globals -- the same invariant the
 * rewind itself stakes scratch safety on.  Collection handles are opaque
 * TURI_INT carriers the promotion walk passes through untouched, so tracked
 * boxes (Vec / Set / Map wrappers, TVar cells) accumulate until teardown even
 * though promotion has zeroed the value pool.  This pass bounds them:
 *
 *   mark:  walk the live graph the way promo_copy does (frames, struct
 *          fields, unstarted generators, ws continuations), treating every
 *          TURI_INT as a candidate box address; a hit marks the box and
 *          queues its own contents (via the box's scan callback) for the
 *          same treatment, transitively -- a vec-of-vecs or a struct held
 *          in a vec cell keeps what it references alive.
 *   sweep: any live box the mark never reached is unreachable garbage; run
 *          its destroy and recycle the tracking node.  BUT if any *marked*
 *          box could not enumerate its contents completely (a non-empty
 *          Set/Map's entries are untyped, so a struct-valued entry hiding a
 *          handle cannot be ruled out), the whole cycle is mark-only and
 *          nothing is freed: leak-on-doubt, never free-on-doubt.
 *
 * False positives (an ordinary int that happens to equal a box address) only
 * keep a dead box alive one more cycle -- safe by construction.  THROW /
 * FUTURE / REF values cannot be live here (the quiescent gate + promo_check
 * exclude them), the same envelope promotion relies on. */

typedef struct {
    TuriEnv      *env;
    PromoMap     *boxmap;    /* box address -> TuriCollBuf* (live boxes only) */
    TuriCollBuf **wl;        /* queue of newly-marked boxes awaiting a scan */
    size_t        wl_len, wl_cap;
    bool          complete;  /* no marked box hid entries from the mark */
} CollMark;

static void collmark_value(TuriEnv *env, TuriValue v, PromoMap *seen, CollMark *mc);

static void collmark_candidate(CollMark *mc, int64_t x) {
    if (!x) return;
    TuriCollBuf *node =
        (TuriCollBuf *)promo_map_get(mc->boxmap, (const void *)(intptr_t)x);
    if (!node || node->marked) return;
    node->marked = true;
    if (mc->wl_len == mc->wl_cap) {
        size_t ncap = mc->wl_cap ? mc->wl_cap * 2 : 32;
        TuriCollBuf **nwl =
            (TuriCollBuf **)realloc(mc->wl, ncap * sizeof(*nwl));
        if (!nwl) { mc->complete = false; return; }   /* mark-only on OOM */
        mc->wl = nwl;
        mc->wl_cap = ncap;
    }
    mc->wl[mc->wl_len++] = node;
}

static void collmark_bindings(TuriEnv *env, EvalBinding *b, PromoMap *seen,
                              CollMark *mc) {
    for (; b; b = b->next) collmark_value(env, b->value, seen, mc);
}

static void collmark_frame(TuriEnv *env, EvalFrame *f, PromoMap *seen,
                           CollMark *mc) {
    for (; f; f = f->parent) {
        if (promo_map_get(seen, f)) return;
        promo_map_put(seen, f, f);
        collmark_bindings(env, f->bindings, seen, mc);
        /* tyvars carry types only -- no values to mark. */
    }
}

static void collmark_wscont(TuriEnv *env, TuriWsCont *wc, PromoMap *seen,
                            CollMark *mc) {
    if (!wc || promo_map_get(seen, wc)) return;
    promo_map_put(seen, wc, wc);
    collmark_frame(env, wc->handler_frame, seen, mc);
    for (size_t i = 0; wc->frames && i < wc->n_frames; i++) {
        collmark_frame(env, wc->frames[i].frame, seen, mc);
        collmark_value(env, wc->frames[i].last, seen, mc);
        if (wc->frames[i].aux) {
            /* Only an argument accumulator's live prefix holds values --
             * mirrors promo_copy_wscont / promo_wscont_aux_cap exactly. */
            size_t cap = promo_wscont_aux_cap(&wc->frames[i]);
            TuriValue *acc = (TuriValue *)wc->frames[i].aux;
            for (uint32_t j = 0; j < wc->frames[i].index && j < cap; j++)
                collmark_value(env, acc[j], seen, mc);
        }
    }
}

static void collmark_value(TuriEnv *env, TuriValue v, PromoMap *seen,
                           CollMark *mc) {
    switch (v.tag) {
    case TURI_INT:
        collmark_candidate(mc, v.as_int);
        return;
    case TURI_CLOSURE: {
        TuriClosure *cl = v.as_closure;
        if (!cl || promo_map_get(seen, cl)) return;
        promo_map_put(seen, cl, cl);
        collmark_frame(env, cl->captured, seen, mc);
        return;
    }
    case TURI_STRUCT: {
        TuriStruct *s = v.as_struct;
        if (!s || promo_map_get(seen, s)) return;
        promo_map_put(seen, s, s);
        for (uint32_t i = 0; s->fields && i < s->n_fields; i++)
            collmark_value(env, s->fields[i], seen, mc);
        return;
    }
    case TURI_GEN: {
        TuriGen *g = v.as_gen;
        if (!g || promo_map_get(seen, g)) return;
        promo_map_put(seen, g, g);
        collmark_frame(env, g->frame, seen, mc);
        collmark_value(env, g->error_val, seen, mc);
        return;
    }
    case TURI_EFFECT_CONT: {
        TuriEffectCont *c = v.as_cont;
        if (!c || promo_map_get(seen, c)) return;
        promo_map_put(seen, c, c);
        collmark_wscont(env, c->ws, seen, mc);
        return;
    }
    default:
        /* NIL/BOOL/FLOAT/CSTR/ERROR/STRUCT_TYPE/HANDLER/REJECTION/SYNTAX
         * carry no collection handles. */
        return;
    }
}

/* Adapter so a box's scan callback re-enters the marker (TuriCollBufMarkFn). */
typedef struct { PromoMap *seen; CollMark *mc; } CollMarkFnCtx;
static void collmark_markfn(TuriValue v, void *ctx) {
    CollMarkFnCtx *c = (CollMarkFnCtx *)ctx;
    collmark_value(c->mc->env, v, c->seen, c->mc);
}

static void collsweep_after_rewind(TuriEnv *env, TuriValue result) {
    if (!env->coll_bufs) return;

    PromoMap boxmap;
    promo_map_init(&boxmap);
    bool any_live = false;
    for (TuriCollBuf *n = env->coll_bufs; n; n = n->next) {
        n->marked = false;
        if (n->box) { promo_map_put(&boxmap, n->box, n); any_live = true; }
    }

    CollMark mc = { env, &boxmap, NULL, 0, 0, true };
    if (any_live) {
        PromoMap seen;
        promo_map_init(&seen);
        collmark_value(env, result, &seen, &mc);
        for (EnvBinding *b = env->globals; b; b = b->next)
            collmark_value(env, b->value, &seen, &mc);
        /* Drain: scans may mark further boxes, growing the queue mid-loop. */
        CollMarkFnCtx fnctx = { &seen, &mc };
        for (size_t i = 0; i < mc.wl_len; i++) {
            TuriCollBuf *n = mc.wl[i];
            if (!n->scan || !n->scan(n->box, collmark_markfn, &fnctx))
                mc.complete = false;
        }
        promo_map_free(&seen);
    }
    promo_map_free(&boxmap);
    free(mc.wl);

    if (any_live && mc.complete) {
        env->collsweep_runs++;
        for (TuriCollBuf **pp = &env->coll_bufs; *pp; ) {
            TuriCollBuf *n = *pp;
            if (n->box && !n->marked) {
                n->destroy(n->box);
                n->box = NULL;
                env->collsweep_freed++;
            }
            if (!n->box) {   /* swept now, or tombstoned by an explicit free */
                *pp = n->next;
                n->next = env->coll_bufs_free;
                env->coll_bufs_free = n;
            } else {
                pp = &n->next;
            }
        }
    } else {
        if (any_live) env->collsweep_markonly++;
        /* Mark-only cycle: free nothing, but still recycle tombstones. */
        for (TuriCollBuf **pp = &env->coll_bufs; *pp; ) {
            TuriCollBuf *n = *pp;
            if (!n->box) {
                *pp = n->next;
                n->next = env->coll_bufs_free;
                env->coll_bufs_free = n;
            } else {
                pp = &n->next;
            }
        }
    }
}

/* Promote everything that escapes this top-level eval (its result plus every
 * global) into value_perm, then rewind value_scratch.  A no-op unless promotion
 * is enabled; conservatively skips the rewind whenever safety cannot be proven. */
/* TR2 (turi-incremental-elaboration-design): accumulated top-level Form vector.
 *
 * Holds every top-level Form parsed so far this session, in parse order, so a
 * long-lived env can re-read only the newly appended source instead of
 * re-parsing the whole accumulated blob each turn. The Forms live in the eval
 * arenas that produced them (all retained), and are immutable after parse, so
 * holding them across evals is sound. Storage is a plain malloc'd pointer
 * vector, freed in turi_env_free. */
static bool acc_forms_set(TuriEnv *env, Form **src, uint32_t n) {
    if (n > env->cap_acc_forms) {
        uint32_t ncap = env->cap_acc_forms ? env->cap_acc_forms : 16;
        while (ncap < n) ncap *= 2;
        Form **grown = (Form **)realloc(env->acc_forms, (size_t)ncap * sizeof(Form *));
        if (!grown) return false;
        env->acc_forms     = grown;
        env->cap_acc_forms = ncap;
    }
    if (n && src != env->acc_forms)
        memcpy(env->acc_forms, src, (size_t)n * sizeof(Form *));
    env->n_acc_forms = n;
    return true;
}

static bool acc_forms_append(TuriEnv *env, Form **src, uint32_t n) {
    uint32_t base = env->n_acc_forms;
    uint32_t total = base + n;
    if (total < base) return false;               /* overflow guard */
    if (total > env->cap_acc_forms) {
        uint32_t ncap = env->cap_acc_forms ? env->cap_acc_forms : 16;
        while (ncap < total) ncap *= 2;
        Form **grown = (Form **)realloc(env->acc_forms, (size_t)ncap * sizeof(Form *));
        if (!grown) return false;
        env->acc_forms     = grown;
        env->cap_acc_forms = ncap;
    }
    if (n) memcpy(env->acc_forms + base, src, (size_t)n * sizeof(Form *));
    env->n_acc_forms = total;
    return true;
}

static void turi_promote_escaping(TuriEnv *env, TuriValue *result) {
    if (!env || !env->scratch_promotion) return;
    env->promo_attempts++;   /* TR0: promotion attempted this eval boundary */
    if (!promo_env_quiescent(env)) { env->promo_decline_busy++; return; }

    /* Pass 1: is the whole root set relocatable? */
    PromoMap seen;
    promo_map_init(&seen);
    bool ok = promo_check(env, *result, &seen);
    if (ok) {
        for (EnvBinding *b = env->globals; b; b = b->next) {
            if (!promo_check(env, b->value, &seen)) { ok = false; break; }
        }
    }
    promo_map_free(&seen);
    if (!ok) { env->promo_decline_unrelocatable++; return; }   /* keep scratch intact this cycle */

    /* Pass 2: copy roots into perm, rewriting pointers (shared fwd table keeps
     * cross-root sharing and cycles consistent). */
    PromoMap fwd;
    promo_map_init(&fwd);
    TuriValue promoted = promo_copy(env, *result, &fwd);
    for (EnvBinding *b = env->globals; b; b = b->next)
        b->value = promo_copy(env, b->value, &fwd);
    promo_map_free(&fwd);
    *result = promoted;

    /* Everything reachable now lives in value_perm; reclaim the scratch region. */
    arena_reset(&env->value_scratch);
    env->promo_rewinds++;   /* TR0: scratch actually reclaimed this cycle */

    /* TR3: with the live graph now provably rooted at result+globals, sweep
     * tracked collection boxes nothing references any more. */
    collsweep_after_rewind(env, *result);
}

static TuriValue turi_eval_impl(TuriEnv *env, const char *src, const char *path,
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
        const char  *rest     = src_body;
        size_t       rest_len  = body_len;
        LangLayerSet layers    = 0;
        const char  *bad       = NULL;
        size_t       bad_len   = 0;
        ReaderType   detected  = detect_lang_layered(src_body, body_len,
                                                     &rest, &rest_len,
                                                     &layers, &bad, &bad_len);
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
            /* Unknown layer token is a hard error (TUR-E0330), matching the
             * compiled path. */
            if (bad) {
                return turi_errorf("error [TUR-E0330]: unknown #lang layer '%.*s'",
                                   (int)bad_len, bad);
            }
            /* Strip the directive from the source body. */
            if (detected != env->reader_type) {
                /* Reader type is changing: discard accumulated source (and the
                 * forms and elaboration session built from it) so that prior
                 * input isn't re-parsed under an incompatible reader.  The
                 * pinned stdlib preload survives -- it is reader-agnostic, and
                 * dropping it is what made the first `#map{}` after a `#lang`
                 * switch fail as "unknown ... 'hamt-of'". */
                turi_env_reset_to_prelude(env);
                env->reader_type = detected;
            }
            /* Layers are additive and file-scoped; union them into the
             * session set so reader layers stay active across the eval blob. */
            env->lang_layers |= layers;
            src_body = rest;
            body_len = rest_len;
        }
    }

    /* 1. Build combined source: all prior definitions + new source (sans #lang).
     *
     * TR2.3: this goes into an env-owned buffer that is REUSED every eval,
     * rather than a fresh Buf copied into the per-eval arena. The old
     * arena_strdup retained a full copy of the accumulated source in every eval
     * arena -- O(N) per eval, O(N^2) over a session, which was the dominant
     * residue once elaboration became incremental. Nothing holds a pointer into
     * this buffer past its own eval (Forms copy their bytes; the SourceFile is
     * re-registered each turn), so reusing it is safe. */
    Buf *combined = &env->src_combined;
    combined->len = 0;
    if (env->src_acc.len > 0) {
        buf_write(combined, env->src_acc.data, env->src_acc.len);
        buf_putc(combined, '\n');
    }
    buf_write(combined, src_body, body_len);
    /* NUL-terminate for any consumer that expects a C string, without counting
     * the terminator in the reported length. */
    buf_putc(combined, '\0');
    combined->len--;

    /* 2. Create a new per-call arena and link it into env. */
    ArenaNode *node = (ArenaNode *)malloc(sizeof(ArenaNode));
    arena_init(&node->arena, 0);
    node->next      = env->eval_arenas;
    env->eval_arenas = node;
    Arena *eval_arena = &node->arena;

    /* 3. Point at the env-owned combined source (TR2.3: no per-eval copy). */
    size_t      src_len  = combined->len;
    const char *src_copy = combined->data;

    /* 4. Reset diagnostics; register the eval source file.
     *
     * Snapshot the file registry first.  diag_reset() clears it, and on the
     * incremental path the `(load ...)`ed files registered by an EARLIER turn
     * are never re-registered -- that turn's Forms are reused rather than
     * re-parsed, so the load splicing does not run again -- while those Forms
     * still carry their file ids.  Without the restore below, every later
     * diag_file_path() on such an id misses and the DAP debugger reports a
     * frame as `?:19` with no `source` object.  Sound here because the
     * interpreter retains its eval arenas for the life of the env, so the saved
     * SourceFile pointers stay valid.
     * See docs/archive/incremental-elab-loses-span-file-provenance.md. */
    const SourceFile *saved_files[64];
    size_t n_saved = diag_files_save(saved_files,
                                     sizeof(saved_files) / sizeof(saved_files[0]));
    diag_reset();

    SourceFile *sfile = (SourceFile *)arena_alloc(eval_arena, sizeof(SourceFile));
    /* arena_alloc does not zero; clear so xform_map/orig_src start NULL.
     * Otherwise diagnostic snippet rendering dereferences uninitialized
     * memory (the sweet-exp xform map) and crashes. */
    memset(sfile, 0, sizeof(*sfile));
    sfile->path        = path;
    /* Resolve in-source relative paths (#use-reader-macros) against the script's
     * directory: the eval blob's path is the synthetic "<eval>" (no dirname), so
     * without this a directive like `#use-reader-macros "macros.tur"` would look
     * in cwd instead of beside the script. */
    sfile->base_dir    = env->module_base_dir;
    sfile->src         = src_copy;
    sfile->len         = src_len;
    sfile->file_id     = 0;
    sfile->reader_type = env->reader_type;
    sfile->lang_layers = env->lang_layers;   /* lang-layers-plan L1 */
    diag_register_file(sfile);
    /* Re-register the previous turn's loaded files (id 0, this turn's blob,
     * is skipped) so spans in reused Forms still resolve to their real path. */
    diag_files_restore(saved_files, n_saved);

    /* 5. Parse. RM Q#5: pass env->reader_macros so reader-macros defined
     * in earlier eval calls remain visible.
     *
     * TR2: with incremental parsing enabled, re-read only the newly appended
     * tail (offset `prefix_len` into the combined blob) and reuse the Forms
     * earlier evals already parsed, instead of re-parsing everything each turn.
     * `sfile` still carries the FULL blob, so spans stay absolute and
     * diagnostics render exactly as on the default path. Any turn we cannot
     * handle incrementally falls back to the whole-blob parse below, so
     * correctness never depends on the fast path applying. */
    const uint32_t acc_committed = env->n_acc_forms;  /* rollback point */
    const uint32_t prefix_len =
        (env->src_acc.len > 0) ? (uint32_t)env->src_acc.len + 1u : 0u;
    const bool use_incremental =
        env->incremental_elab &&
        /* sweet-exp rewrites the whole buffer, so an offset in original
         * coordinates is meaningless after transformation */
        env->reader_type != READER_SWEET &&
        prefix_len > 0 &&                       /* nothing accumulated yet */
        prefix_len <= src_len &&
        env->n_acc_forms == env->prior_toplevel; /* vector in sync with session */

    uint32_t  nforms = 0;
    Form    **forms  = NULL;
    if (use_incremental) {
        uint32_t n_new = 0;
        Form **new_forms =
            read_all_with_registry_from(eval_arena, &env->st, sfile,
                                        env->reader_macros, prefix_len,
                                        env->acc_next_line ? env->acc_next_line : 1,
                                        &n_new);
        if (!new_forms || diag_had_error()) {
            return turi_error("parse error");
        }
        if (!acc_forms_append(env, new_forms, n_new)) {
            env->n_acc_forms = acc_committed;
            return turi_error("out of memory");
        }
        forms  = env->acc_forms;
        nforms = env->n_acc_forms;
    } else {
        forms = read_all_with_registry(eval_arena, &env->st, sfile,
                                       env->reader_macros, &nforms);
        if (!forms || diag_had_error()) {
            return turi_error("parse error");
        }
        /* Whole-blob parse: the result already contains the prior forms
         * (re-parsed), so it replaces the accumulated vector wholesale. */
        if (!acc_forms_set(env, forms, nforms)) {
            env->n_acc_forms = acc_committed;
            return turi_error("out of memory");
        }
    }

    /* 5b. def/define consolidation D2: the REPL implicit-do wrap is retired.
     * It existed only to keep a top-level `(define ...)` from erroring, by
     * wrapping the turn in a `(do ...)` so the body-splice helper caught it --
     * which also scoped the binding to that one turn, so a name defined at the
     * prompt did not survive to the next one.  `define` is now a spelling of
     * `def`, so a top-level `define` is a genuine top-level binding and needs
     * no workaround.  See docs/archive/def-define-consolidation-plan.md. */
    uint32_t prior = env->prior_toplevel;

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

    /* TR2.2b: incremental elaboration. When the session already holds exactly
     * the previously-accumulated forms, hand the elaborator ONLY this turn's
     * new forms -- prior definitions resolve out of the session's accumulated
     * scope instead of being re-elaborated (and re-allocated) every turn.
     *
     * Otherwise (no session yet, or one just discarded after a failure) fall
     * back to elaborating the whole accumulated program, which both reproduces
     * today's behavior exactly and rebuilds the session from scratch. */
    if (env->incremental_elab && !env->elab_session) {
        env->elab_session       = elab_session_new();
        env->elab_session_forms = 0;
    }
    const bool use_incr_elab = env->elab_session && prior > 0 &&
                               env->elab_session_forms == prior;
    const uint32_t elab_from = use_incr_elab ? prior : 0;

    Expr *prog = elaborate_program_session(eval_arena, &env->st,
                                   forms + elab_from, nforms - elab_from,
                                   /*stdlib_prefix=*/prior - elab_from,
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
                                   env->reader_macros,
                                   env->elab_session);
    if (!prog || diag_had_error()) {
        env->n_acc_forms = acc_committed;   /* TR2: uncommit this turn's forms */
        /* A failed program may have left partial definitions in the session;
         * discard it so the next turn rebuilds from the accumulated forms. */
        if (env->elab_session) {
            elab_session_free(env->elab_session);
            env->elab_session       = NULL;
            env->elab_session_forms = 0;
        }
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
        env->n_acc_forms = acc_committed;   /* TR2: uncommit this turn's forms */
        if (env->elab_session) {            /* see the discard note above */
            elab_session_free(env->elab_session);
            env->elab_session       = NULL;
            env->elab_session_forms = 0;
        }
        return turi_error("elaboration error");
    }

    /* Publish this elaboration's TypeClassEnv BEFORE evaluating the new forms,
     * not only at the successful end of the call.  Runtime typeclass dispatch
     * (frame_bind_constraint_dicts / turi_try_show)
     * reads env->last_tc_env during evaluation.  Previously last_tc_env was set
     * only in the success epilogue below, so the FIRST program to introduce a
     * class's instances (e.g. `Show [String]`, loaded via string.tur) evaluated
     * against the PREVIOUS call's tc_env -- which lacked those instances -- and
     * a generic method like `(show-line s)` fell back to the baked int-carrier
     * representative (printing the raw String pointer instead of its content).
     * Setting it here makes the current elaboration's instances live for this
     * eval; the epilogue assignment keeps it pinned across subsequent calls. */
    env->last_tc_env = tc_env_slot;

    /* 7. Evaluate the new top-level expressions.
     *
     * elaborate_program prepends actual file-scope defs (EX_DEFMODULE nodes
     * from imported modules) before the parsed forms.  It also expands any
     * (load ...) directives inline, which increases prog->as.program.n
     * beyond nforms without contributing to n_fsd.  We use the count returned
     * via out_n_file_scope_defs to correctly separate the two:
     *
     *   [0 .. n_fsd-1]                    <- file_scope_defs (imported modules)
     *   [n_fsd .. n_fsd+prior_prog-1]     <- previously-accumulated items (already run)
     *   [n_fsd+prior_prog .. total-1]     <- new user forms (must run; incl. load-expanded)
     *
     * We must also run the file_scope_defs on every call because they are
     * freshly elaborated and not yet registered in the runtime env.  Since
     * EX_FN_DEF is idempotent (just overwrites the env binding), re-running
     * them on subsequent calls is harmless.
     *
     * The skip boundary is prior_prog_items -- a count of already-run *program
     * items* -- NOT prior_toplevel (a *parsed-form* count).  A (load ...) form
     * expands inline to many program items, so the two diverge; keying the
     * boundary off the parsed count let a re-expanded (load ...) shift it and
     * re-run earlier top-level forms (interp-generator-resume-across-evals: a
     * prior (gen-next g) double-advanced the suspended generator). */
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
            last = turi_error("uncaught exception");                          \
            goto eval_done;                                                   \
        }                                                                     \
        if (turi_is_error(last)) goto eval_done;                             \
    }                                                                         \
} while (0)

    TuriValue last = turi_nil();
    /* The already-run tail of the program is n_fsd file-scope defs followed by
     * prior_prog non-fsd program items from earlier evals.  Skip exactly that
     * many; everything after is genuinely new (see prior_prog_items above --
     * parsed-form count is the wrong unit once (load ...) expands inline). */
    /* TR2.2b: under incremental elaboration the program holds ONLY this turn's
     * items, so none of them have run before -- the already-run prefix is zero.
     * On the whole-program path the cumulative count applies, as before. */
    uint32_t prior_prog = use_incr_elab ? 0u : env->prior_prog_items;
    EVAL_TOPLEVEL_RANGE(0, n_fsd);                   /* imported module bodies */
    EVAL_TOPLEVEL_RANGE(n_fsd + prior_prog, total);  /* new user forms         */
eval_done:;
#undef EVAL_TOPLEVEL_RANGE

    /* 8. Update accumulated state only on success.
     * Store nforms (parsed count) rather than total so the n_fsd formula
     * remains correct on subsequent calls that re-elaborate the same imports. */
    if (!turi_is_error(last)) {
        /* Append new source (without any leading #lang line) to accumulator */
        if (env->src_acc.len > 0) buf_putc(&env->src_acc, '\n');
        buf_write(&env->src_acc, src_body, body_len);
        /* TR2: advance the incremental line cursor across the chunk just
         * appended, counting newlines in the NEW text only (never rescanning
         * the prefix, which would reintroduce an O(N) term per eval). The next
         * chunk is separated by one '\n', hence the trailing +1. */
        {
            uint32_t start_line = env->acc_next_line ? env->acc_next_line : 1;
            uint32_t nl = 0;
            for (size_t i = 0; i < body_len; i++)
                if (src_body[i] == '\n') nl++;
            env->acc_next_line = start_line + nl + 1;
        }
        env->prior_toplevel = nforms;  /* track parsed count, not total */
        /* Every non-fsd program item run this call is "accumulated" next time.
         * total = n_fsd + (all non-fsd items), so total - n_fsd is that count.
         * Keying the skip off this (not the parsed nforms) is what keeps a
         * re-expanded (load ...) from re-running earlier top-level forms. */
        /* TR2.2b: keep this CUMULATIVE across turns. The whole-program path
         * elaborates everything, so (total - n_fsd) is already the running
         * total; the incremental path only sees this turn's items, so add. */
        if (use_incr_elab) env->prior_prog_items += (total - n_fsd);
        else               env->prior_prog_items  = total - n_fsd;
        /* The session has now absorbed every accumulated form. */
        if (env->elab_session) env->elab_session_forms = nforms;
        /* SI4: persist TypeClassEnv for turi_try_show dispatch. */
        env->last_tc_env = tc_env_slot;
        /* SI4: extract type tag from the last new top-level expression. */
        env->last_result_type = NULL;
        if (total > n_fsd + prior_prog) {
            Expr *last_expr = prog->as.program.items[total - 1];
            if (last_expr) {
                if (out_type_tag && tag_cap > 0)
                    extract_type_tag(last_expr->type, out_type_tag, tag_cap);
                /* Retain the FULL type alongside the head-only tag.  Lives in
                 * the same arena as last_tc_env (set just above), so it stays
                 * valid for the display pass that follows this eval. */
                env->last_result_type = (void *)&last_expr->type;
            }
        }
    } else {
        /* TR2: this turn's source is not appended to src_acc on failure, so its
         * forms must not stay in the accumulated vector either -- otherwise the
         * next turn's offset/count bookkeeping would disagree with src_acc. */
        env->n_acc_forms = acc_committed;
        /* TR2.2b: the session HAS absorbed this turn's definitions even though
         * the turn failed at runtime, so it no longer matches the accumulated
         * forms. Discard it; the next turn rebuilds from acc_forms. */
        if (env->elab_session) {
            elab_session_free(env->elab_session);
            env->elab_session       = NULL;
            env->elab_session_forms = 0;
        }
    }

    /* turi-value-pool-scratch-promotion-plan: at this top-level boundary,
     * promote escapees and rewind the scratch value pool (no-op unless the env
     * opted in via turi_env_set_scratch_promotion). */
    turi_promote_escaping(env, &last);

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
        turi_env_reset_to_prelude(env);
        env->reader_type = ext_type;
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
        const CtorDef *cd = s->ctor;
        if (!cd || !cd->fields || s->n_fields == 0) {
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
            const char *fname = (i < cd->n_fields) ? cd->fields[i].name : "?";
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
    case TURI_REJECTION:
        snprintf(buf, cap, "#<rejection: %s>", v.as_error ? v.as_error : "");
        break;
    case TURI_SYNTAX: {
        /* Show the wrapped form's text: `#<syntax (+ 1 2)>`. */
        if (!v.as_syntax) { snprintf(buf, cap, "#<syntax>"); break; }
        Buf fb;
        buf_init(&fb);
        form_print(&fb, v.as_syntax);
        snprintf(buf, cap, "#<syntax %.*s>", (int)fb.len, fb.data);
        buf_free(&fb);
        break;
    }
    }
}

void turi_value_repr(char *buf, size_t cap, TuriValue v) {
    turi_value_repr_d(buf, cap, v, 4);
}

/* =========================================================================
 * Debugger Phase 2 -- interactive interpreter debugger
 *
 * See docs/archive/history/debugger-plan.md (Phase 2).  A TuriDebugger is attached to
 * a TuriEnv by turi_debug_enable(); the eval loop then calls turi_dbg_before_node
 * before each AST node and turi_dbg_push/pop around each turi-body activation.
 * On a breakpoint or a satisfied step predicate the loop yields to a small
 * command REPL (dbg_repl) reading from dbg->in and writing to dbg->out.
 *
 * Stepping is line-granular: STEP_IN stops at the next node on a different
 * source line; STEP_OVER additionally requires the call depth to be <= the
 * depth we stepped from (so a call on the current line is run to completion);
 * STEP_OUT stops once the depth drops below the stepped-from depth.
 * ========================================================================= */

#define DBG_MAX_BPS     64
#define DBG_MAX_FRAMES  512   /* backtrace storage cap; depth() still counts past it */

typedef enum {
    DBG_STEP_NONE = 0,  /* run freely; only breakpoints / (break) stop us */
    DBG_STEP_IN,        /* stop at the next line, any depth */
    DBG_STEP_OVER,      /* stop at the next line at depth <= step_depth */
    DBG_STEP_OUT,       /* stop once depth < step_depth */
} DbgStep;

typedef struct {
    char     file[128];  /* basename matched against a node's source file */
    uint32_t line;
    char     cond[160];  /* DAP conditional breakpoint expr; "" = unconditional */
} DbgBreakpoint;

typedef struct {
    const char *fn_name;  /* function owning this activation (points into sym storage) */
    EvalFrame  *cf;       /* its lexical frame (updated while this frame is active) */
    Span        cur;      /* span of the node currently executing in this frame */
} DbgStackFrame;

typedef struct TuriDebugger {
    FILE          *in;
    FILE          *out;
    bool           armed;        /* false until turi_debug_arm(): no node stops */
    bool           in_repl;      /* reentrancy guard while the command loop runs */
    bool           break_now;    /* (break) builtin: stop at the next located node */
    DbgStep        step;
    int            depth;        /* live call depth (turi-body activations) */
    int            step_depth;   /* depth recorded at the last stop */
    uint16_t       stop_file;    /* file_id of the last stop (for line_changed) */
    uint32_t       stop_line;    /* source line of the last stop */
    uint16_t       prev_file;    /* file_id of the previously executed node */
    uint32_t       prev_line;    /* line of the previously executed node (for bp entry) */
    const Expr    *skip_node;    /* dedup: node hooked by eval_expr, skip its driver re-hook */
    bool           entry;        /* next stop is the program-entry stop (Phase 3 reason) */
    DbgBreakpoint  bps[DBG_MAX_BPS];
    int            n_bps;
    DbgStackFrame  frames[DBG_MAX_FRAMES];

    /* Phase 3 (DAP) control surface.  When pause_fn is set, a stop dispatches to
     * it instead of dbg_repl; the handler reads cur_frame / cur_expr for the
     * innermost frame and resumes via turi_debug_resume_*. */
    TuriDbgPauseFn pause_fn;
    void          *pause_ud;
    TuriDbgCondFn  cond_fn;      /* conditional-breakpoint predicate (DAP) */
    void          *cond_ud;
    TuriDbgBpMatchFn bp_match_fn; /* custom breakpoint matcher (Godot) */
    void          *bp_match_ud;
    EvalFrame     *cur_frame;    /* lexical frame of the currently paused node */
    const Expr    *cur_expr;     /* the currently paused node */
    TuriDbgStop    stop_reason;  /* why we paused (for the pause handler) */
} TuriDebugger;

/* Last path component of a source path (no allocation). */
static const char *dbg_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* -------- call-stack maintenance (invoked from the driver) ---------------- */

static void turi_dbg_push(TuriEnv *env, const FnDef *fn, EvalFrame *cf) {
    TuriDebugger *dbg = (TuriDebugger *)env->debugger;
    if (!dbg) return;
    if (dbg->depth >= 0 && dbg->depth < DBG_MAX_FRAMES) {
        DbgStackFrame *f = &dbg->frames[dbg->depth];
        f->fn_name = (fn && fn->binding && fn->binding->name)
                       ? fn->binding->name->name : "<fn>";
        f->cf  = cf;
        f->cur = SPAN_UNKNOWN;
    }
    dbg->depth++;
}

static void turi_dbg_pop(TuriEnv *env) {
    TuriDebugger *dbg = (TuriDebugger *)env->debugger;
    if (!dbg) return;
    if (dbg->depth > 0) dbg->depth--;
}

static void turi_dbg_set_top(TuriEnv *env, const FnDef *fn, EvalFrame *cf) {
    TuriDebugger *dbg = (TuriDebugger *)env->debugger;
    if (!dbg) return;
    int top = dbg->depth - 1;
    if (top >= 0 && top < DBG_MAX_FRAMES) {
        DbgStackFrame *f = &dbg->frames[top];
        f->fn_name = (fn && fn->binding && fn->binding->name)
                       ? fn->binding->name->name : "<fn>";
        f->cf  = cf;
        f->cur = SPAN_UNKNOWN;
    }
}

/* -------- source listing -------------------------------------------------- */

/* Print the source line `line` of file_id `fid` (1-based), prefixed with
 * `marker`.  Best-effort: silently does nothing when the file is unreadable. */
static void dbg_print_source_line(TuriDebugger *dbg, uint16_t fid,
                                  uint32_t line, const char *marker) {
    const char *path = diag_file_path(fid);
    if (!path || line == 0) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[1024];
    uint32_t cur = 0;
    while (fgets(buf, sizeof buf, f)) {
        cur++;
        if (cur == line) {
            size_t n = strlen(buf);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
            fprintf(dbg->out, "%s%u\t%s\n", marker, line, buf);
            break;
        }
    }
    fclose(f);
}

/* List a window of `radius` lines on either side of `line`. */
static void dbg_list_source(TuriDebugger *dbg, uint16_t fid,
                            uint32_t line, uint32_t radius) {
    if (line == 0) { fprintf(dbg->out, "no source location\n"); return; }
    uint32_t lo = (line > radius) ? line - radius : 1;
    uint32_t hi = line + radius;
    for (uint32_t l = lo; l <= hi; l++)
        dbg_print_source_line(dbg, fid, l, l == line ? "=> " : "   ");
}

/* -------- inspection ------------------------------------------------------ */

/* Print every lexically-visible binding reachable from `frame`, innermost
 * first, suppressing names already shown (shadowed outer bindings). */
static void dbg_print_locals(TuriEnv *env, TuriDebugger *dbg, EvalFrame *frame) {
    (void)env;
    const char *seen[256];
    int n_seen = 0;
    int shown  = 0;
    for (EvalFrame *f = frame; f; f = f->parent) {
        for (EvalBinding *b = f->bindings; b; b = b->next) {
            bool dup = false;
            for (int i = 0; i < n_seen; i++)
                if (strcmp(seen[i], b->name) == 0) { dup = true; break; }
            if (dup) continue;
            if (n_seen < 256) seen[n_seen++] = b->name;
            char repr[256];
            turi_value_repr(repr, sizeof repr, b->value);
            fprintf(dbg->out, "  %s = %s\n", b->name, repr);
            shown++;
        }
    }
    if (shown == 0) fprintf(dbg->out, "  (no locals in scope)\n");
}

/* Print a single binding by name, resolving through the frame chain then the
 * globals (mirrors eval_lookup's order). */
static void dbg_print_var(TuriEnv *env, TuriDebugger *dbg, EvalFrame *frame,
                          const char *name) {
    for (EvalFrame *f = frame; f; f = f->parent)
        for (EvalBinding *b = f->bindings; b; b = b->next)
            if (strcmp(b->name, name) == 0) {
                char repr[256];
                turi_value_repr(repr, sizeof repr, b->value);
                fprintf(dbg->out, "%s = %s\n", name, repr);
                return;
            }
    TuriValue g = turi_env_get(env, name);
    if (!turi_is_error(g)) {
        char repr[256];
        turi_value_repr(repr, sizeof repr, g);
        fprintf(dbg->out, "%s = %s\n", name, repr);
        return;
    }
    fprintf(dbg->out, "no binding named '%s' in scope\n", name);
}

static void dbg_print_backtrace(TuriDebugger *dbg) {
    int top = dbg->depth;
    if (top > DBG_MAX_FRAMES) top = DBG_MAX_FRAMES;
    if (top <= 0) { fprintf(dbg->out, "  (no frames)\n"); return; }
    for (int i = top - 1; i >= 0; i--) {
        DbgStackFrame *f = &dbg->frames[i];
        const char *path = dbg_basename(diag_file_path(f->cur.file_id));
        fprintf(dbg->out, "  #%-2d %s  at %s:%u\n",
                top - 1 - i, f->fn_name ? f->fn_name : "<fn>",
                path[0] ? path : "?", f->cur.line);
    }
}

/* -------- breakpoints ----------------------------------------------------- */

/* Return the index of the first breakpoint matching span `s`, or -1. */
static int dbg_bp_match(TuriDebugger *dbg, Span s) {
    if (dbg->n_bps == 0 || s.line == 0) return -1;
    const char *base = dbg_basename(diag_file_path(s.file_id));
    for (int i = 0; i < dbg->n_bps; i++) {
        if (dbg->bps[i].line != s.line) continue;
        if (dbg->bps[i].file[0] == '\0' ||
            strcmp(dbg->bps[i].file, base) == 0)
            return i;
    }
    return -1;
}

/* Parse "<line>" or "<file>:<line>" into a breakpoint and record it. Supports "break <line> if <expr>". */
static void dbg_add_breakpoint(TuriDebugger *dbg, const char *arg) {
    if (dbg->n_bps >= DBG_MAX_BPS) {
        fprintf(dbg->out, "breakpoint table full (max %d)\n", DBG_MAX_BPS);
        return;
    }
    DbgBreakpoint bp = {{0}, 0, {0}};
    const char *if_part = strstr(arg, " if ");
    char cond[256] = {0};
    char target[128] = {0};
    if (if_part) {
        size_t tlen = (size_t)(if_part - arg);
        if (tlen >= sizeof target) tlen = sizeof target - 1;
        memcpy(target, arg, tlen);
        target[tlen] = '\0';
        snprintf(cond, sizeof cond, "%s", if_part + 4);
        /* Trim spaces from cond */
        size_t cn = strlen(cond);
        while (cn > 0 && (cond[cn - 1] == ' ' || cond[cn - 1] == '\t')) cond[--cn] = '\0';
        arg = target;
    }

    const char *colon = strrchr(arg, ':');
    if (colon) {
        size_t flen = (size_t)(colon - arg);
        if (flen >= sizeof bp.file) flen = sizeof bp.file - 1;
        memcpy(bp.file, arg, flen);
        bp.file[flen] = '\0';
        bp.line = (uint32_t)strtoul(colon + 1, NULL, 10);
    } else {
        bp.line = (uint32_t)strtoul(arg, NULL, 10);
    }
    if (bp.line == 0) { fprintf(dbg->out, "bad breakpoint: '%s'\n", arg); return; }
    if (cond[0]) {
        snprintf(bp.cond, sizeof bp.cond, "%s", cond);
    }
    dbg->bps[dbg->n_bps++] = bp;
    if (cond[0]) {
        fprintf(dbg->out, "breakpoint %d set at %s%s%u if %s\n",
                dbg->n_bps, bp.file, bp.file[0] ? ":" : "", bp.line, bp.cond);
    } else {
        fprintf(dbg->out, "breakpoint %d set at %s%s%u\n",
                dbg->n_bps, bp.file, bp.file[0] ? ":" : "", bp.line);
    }
}

static void dbg_delete_breakpoint(TuriDebugger *dbg, const char *arg) {
    if (!arg || !*arg) { dbg->n_bps = 0; fprintf(dbg->out, "all breakpoints cleared\n"); return; }
    int idx = atoi(arg);
    if (idx < 1 || idx > dbg->n_bps) { fprintf(dbg->out, "no breakpoint %s\n", arg); return; }
    for (int i = idx - 1; i < dbg->n_bps - 1; i++) dbg->bps[i] = dbg->bps[i + 1];
    dbg->n_bps--;
    fprintf(dbg->out, "breakpoint %d deleted\n", idx);
}

static void dbg_print_help(TuriDebugger *dbg) {
    fprintf(dbg->out,
        "commands:\n"
        "  break <line> [if <expr>]              set a breakpoint   (b)\n"
        "  delete [n]                            clear breakpoint n (all if omitted)\n"
        "  continue                              run to next breakpoint    (c)\n"
        "  step                                  step into / next line     (s)\n"
        "  next                                  step over calls           (n)\n"
        "  finish                                run until current fn returns (fin)\n"
        "  backtrace                             print the call stack    (bt, where)\n"
        "  locals                                print all locals in scope (l)\n"
        "  print <name>                          print one binding         (p)\n"
        "  eval <expr>                           evaluate arbitrary expr   (e)\n"
        "  list                                  show source around here   (ls)\n"
        "  quit                                  abort the program         (q)\n"
        "  help                                  this message              (h)\n");
}

/* -------- the command REPL ------------------------------------------------ */

/* Read commands until one resumes execution (continue / step / next / finish)
 * or aborts (quit).  `frame` is the lexical frame of the stopped node `e`. */
static void dbg_repl(TuriEnv *env, TuriDebugger *dbg, EvalFrame *frame,
                     const Expr *e) {
    Span s = e->span;
    const char *path = dbg_basename(diag_file_path(s.file_id));
    fprintf(dbg->out, "\nstopped at %s:%u\n", path[0] ? path : "?", s.line);
    dbg_print_source_line(dbg, s.file_id, s.line, "=> ");

    char line[512];
    for (;;) {
        fprintf(dbg->out, "(tur-dbg) ");
        fflush(dbg->out);
        if (!fgets(line, sizeof line, dbg->in)) {
            /* EOF on the command stream: detach and let the program finish. */
            dbg->armed = false;
            dbg->step  = DBG_STEP_NONE;
            return;
        }
        /* split into command + rest */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *cmd = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p) { *p = '\0'; p++; }
        while (*p == ' ' || *p == '\t') p++;
        char *arg = p;
        size_t an = strlen(arg);
        while (an > 0 && (arg[an - 1] == '\n' || arg[an - 1] == '\r' ||
                          arg[an - 1] == ' '  || arg[an - 1] == '\t')) arg[--an] = '\0';

        if (*cmd == '\0') continue;  /* blank line: re-prompt */

        if (!strcmp(cmd, "continue") || !strcmp(cmd, "c")) {
            dbg->step = DBG_STEP_NONE; return;
        }
        if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
            dbg->step = DBG_STEP_IN; dbg->step_depth = dbg->depth;
            dbg->stop_file = s.file_id; dbg->stop_line = s.line; return;
        }
        if (!strcmp(cmd, "next") || !strcmp(cmd, "n")) {
            dbg->step = DBG_STEP_OVER; dbg->step_depth = dbg->depth;
            dbg->stop_file = s.file_id; dbg->stop_line = s.line; return;
        }
        if (!strcmp(cmd, "finish") || !strcmp(cmd, "fin") || !strcmp(cmd, "out")) {
            dbg->step = DBG_STEP_OUT; dbg->step_depth = dbg->depth;
            dbg->stop_file = s.file_id; dbg->stop_line = s.line; return;
        }
        if (!strcmp(cmd, "break") || !strcmp(cmd, "b")) {
            if (*arg) dbg_add_breakpoint(dbg, arg);
            else fprintf(dbg->out, "usage: break <line> | break <file>:<line>\n");
            continue;
        }
        if (!strcmp(cmd, "delete") || !strcmp(cmd, "d")) {
            dbg_delete_breakpoint(dbg, arg); continue;
        }
        if (!strcmp(cmd, "backtrace") || !strcmp(cmd, "bt") || !strcmp(cmd, "where")) {
            dbg_print_backtrace(dbg); continue;
        }
        if (!strcmp(cmd, "locals") || !strcmp(cmd, "l")) {
            dbg_print_locals(env, dbg, frame); continue;
        }
        if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
            if (*arg) dbg_print_var(env, dbg, frame, arg);
            else fprintf(dbg->out, "usage: print <name>\n");
            continue;
        }
        if (!strcmp(cmd, "eval") || !strcmp(cmd, "e")) {
            if (*arg) {
                char val[512];
                if (turi_debug_eval_expr(env, 0, arg, val, sizeof val)) {
                    fprintf(dbg->out, "%s\n", val);
                } else {
                    fprintf(dbg->out, "%s\n", val);
                }
            } else {
                fprintf(dbg->out, "usage: eval <expr>\n");
            }
            continue;
        }
        if (!strcmp(cmd, "list") || !strcmp(cmd, "ls")) {
            dbg_list_source(dbg, s.file_id, s.line, 3); continue;
        }
        if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "?")) {
            dbg_print_help(dbg); continue;
        }
        if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) {
            fprintf(dbg->out, "aborting\n");
            fflush(dbg->out);
            turi_run_pending_defers(env);
            _exit(0);
        }
        fprintf(dbg->out, "unknown command '%s' (try 'help')\n", cmd);
    }
}

/* -------- the per-node hook ----------------------------------------------- */

static bool dbg_line_changed(TuriDebugger *dbg, Span s) {
    return s.line != dbg->stop_line || s.file_id != dbg->stop_file;
}

static void turi_dbg_before_node(TuriEnv *env, EvalFrame *frame,
                                 const Expr *e, bool from_driver) {
    TuriDebugger *dbg = (TuriDebugger *)env->debugger;
    if (!dbg || dbg->in_repl || !e) return;

    /* Dedup: eval_expr hooks a node, then immediately dispatches the
     * driver-folded kinds (let/if/do/program/call/match) to eval_drive, which
     * would re-hook the very same node.  Mark it on the eval_expr pass and
     * swallow the driver's duplicate. */
    if (from_driver) {
        if (e == dbg->skip_node) { dbg->skip_node = NULL; return; }
    } else {
        switch (e->kind) {
        case EX_LET: case EX_LETREC: case EX_IF: case EX_DO:
        case EX_PROGRAM: case EX_CALL: case EX_MATCH:
            dbg->skip_node = e; break;
        default: break;
        }
    }

    Span s = e->span;
    if (s.line == 0) return;  /* synthetic / span-less node: never a stop point */

    /* Keep the active frame's current location fresh for the backtrace. */
    if (dbg->depth > 0 && dbg->depth <= DBG_MAX_FRAMES)
        dbg->frames[dbg->depth - 1].cur = s;

    if (!dbg->armed) { dbg->prev_file = s.file_id; dbg->prev_line = s.line; return; }

    /* A line breakpoint fires only on *entry* to the line (a transition from a
     * different source line), not once per node that shares the line. */
    bool line_entry = (s.line != dbg->prev_line || s.file_id != dbg->prev_file);

    bool hit = false;
    TuriDbgStop reason = TURI_DBG_STOP_STEP;
    if (dbg->break_now) { dbg->break_now = false; hit = true; reason = TURI_DBG_STOP_PAUSE; }
    else if (line_entry) {
        if (dbg->bp_match_fn) {
            const char *path = diag_file_path(s.file_id);
            if (dbg->bp_match_fn(env, path ? path : "", s.line, dbg->bp_match_ud)) {
                hit = true; reason = TURI_DBG_STOP_BREAKPOINT;
            }
        } else {
            int bpi = dbg_bp_match(dbg, s);
            if (bpi >= 0) {
                /* Conditional breakpoint: stop only when the predicate holds.  Done
                 * here (not after recording prev_line) so the line-entry test stays
                 * consistent whether or not we ultimately stop. */
                const char *cond = dbg->bps[bpi].cond;
                if (cond[0] != '\0' && dbg->cond_fn) {
                    dbg->cur_frame = frame; dbg->cur_expr = e;
                    if (dbg->cond_fn(env, cond, dbg->cond_ud)) {
                        hit = true; reason = TURI_DBG_STOP_BREAKPOINT;
                    }
                } else {
                    hit = true; reason = TURI_DBG_STOP_BREAKPOINT;
                }
            }
        }
    }
    if (!hit) {
        switch (dbg->step) {
        case DBG_STEP_IN:   hit = dbg_line_changed(dbg, s); break;
        case DBG_STEP_OVER: hit = dbg_line_changed(dbg, s) &&
                                  dbg->depth <= dbg->step_depth; break;
        case DBG_STEP_OUT:  hit = dbg->depth < dbg->step_depth; break;
        case DBG_STEP_NONE: default: break;
        }
        if (hit) reason = TURI_DBG_STOP_STEP;
    }

    /* Record this node's line as "previous" for the next entry test.  Done
     * before any REPL so a same-line node after resuming does not re-trigger. */
    dbg->prev_file = s.file_id;
    dbg->prev_line = s.line;

    if (!hit) return;

    /* The first stop after arming is the program-entry stop, regardless of how
     * the predicate above classified it. */
    if (dbg->entry) { reason = TURI_DBG_STOP_ENTRY; dbg->entry = false; }

    dbg->cur_frame   = frame;
    dbg->cur_expr    = e;
    dbg->stop_reason = reason;
    dbg->in_repl = true;
    if (dbg->pause_fn) dbg->pause_fn(env, reason, dbg->pause_ud);
    else              dbg_repl(env, dbg, frame, e);
    dbg->in_repl   = false;
    dbg->cur_frame = NULL;
    dbg->cur_expr  = NULL;
    dbg->stop_file = s.file_id;
    dbg->stop_line = s.line;
}

/* -------- (break) builtin ------------------------------------------------- */

/* (break) -- force the debugger to pause at the next located node.  A no-op
 * when no debugger is attached, so a program peppered with (break) still runs
 * normally under plain `tur --interpret`. */
static TuriValue native_dbg_break(TuriEnv *env, TuriValue *args, uint32_t n,
                                  void *ud) {
    (void)args; (void)n; (void)ud;
    TuriDebugger *dbg = (TuriDebugger *)env->debugger;
    if (dbg && dbg->armed) dbg->break_now = true;
    return turi_nil();
}

/* -------- public API ------------------------------------------------------ */

void turi_debug_register_break_builtin(TuriEnv *env) {
    if (!env) return;
    turi_env_register_native(env, "break", native_dbg_break, NULL);
}

static bool dbg_default_cond_fn(TuriEnv *env, const char *condition, void *ud) {
    (void)ud;
    char val[256];
    if (!turi_debug_eval_expr(env, 0, condition, val, sizeof val)) {
        return true; /* evaluation failed -- fall back to stop */
    }
    /* Stop if the evaluated value is truthy (not false and not nil) */
    return (strcmp(val, "false") != 0 && strcmp(val, "nil") != 0);
}

void turi_debug_enable(TuriEnv *env, FILE *in, FILE *out) {
    if (!env || env->debugger) return;
    TuriDebugger *dbg = (TuriDebugger *)calloc(1, sizeof(TuriDebugger));
    if (!dbg) return;
    dbg->in   = in  ? in  : stdin;
    dbg->out  = out ? out : stdout;
    dbg->step = DBG_STEP_NONE;
    dbg->cond_fn = dbg_default_cond_fn;
    env->debugger = dbg;
    /* Register the (break) builtin so source-driven breakpoints resolve. */
    turi_debug_register_break_builtin(env);
}

void turi_debug_arm(TuriEnv *env) {
    if (!env || !env->debugger) return;
    TuriDebugger *dbg = (TuriDebugger *)env->debugger;
    dbg->armed     = true;
    dbg->step      = DBG_STEP_IN;   /* stop at the first located node (entry) */
    dbg->entry     = true;          /* the first stop is the program-entry stop */
    dbg->stop_file = 0;
    dbg->stop_line = 0;
}

void turi_debug_arm_breakpoints(TuriEnv *env) {
    if (!env || !env->debugger) return;
    TuriDebugger *dbg = (TuriDebugger *)env->debugger;
    dbg->armed     = true;
    dbg->step      = DBG_STEP_NONE;
    dbg->entry     = false;
    dbg->stop_file = 0;
    dbg->stop_line = 0;
}

/* -------- Phase 3: DAP control surface ------------------------------------ */

static TuriDebugger *dbg_of(TuriEnv *env) {
    return (env && env->debugger) ? (TuriDebugger *)env->debugger : NULL;
}

void turi_debug_set_pause_handler(TuriEnv *env, TuriDbgPauseFn cb, void *ud) {
    TuriDebugger *d = dbg_of(env);
    if (!d) return;
    d->pause_fn = cb;
    d->pause_ud = ud;
}

void turi_debug_set_cond_handler(TuriEnv *env, TuriDbgCondFn cb, void *ud) {
    TuriDebugger *d = dbg_of(env);
    if (!d) return;
    d->cond_fn = cb;
    d->cond_ud = ud;
}

void turi_debug_set_bp_match_handler(TuriEnv *env, TuriDbgBpMatchFn cb, void *ud) {
    TuriDebugger *d = dbg_of(env);
    if (!d) return;
    d->bp_match_fn = cb;
    d->bp_match_ud = ud;
}

void turi_debug_clear_breakpoints(TuriEnv *env) {
    TuriDebugger *d = dbg_of(env);
    if (d) d->n_bps = 0;
}

void turi_debug_clear_breakpoints_for_file(TuriEnv *env, const char *basename) {
    TuriDebugger *d = dbg_of(env);
    if (!d || !basename) return;
    const char *base = dbg_basename(basename);
    int w = 0;
    for (int i = 0; i < d->n_bps; i++) {
        if (strcmp(d->bps[i].file, base) != 0) {
            if (w != i) d->bps[w] = d->bps[i];
            w++;
        }
    }
    d->n_bps = w;
}

int turi_debug_add_breakpoint(TuriEnv *env, const char *basename, uint32_t line,
                              const char *cond) {
    TuriDebugger *d = dbg_of(env);
    if (!d || line == 0 || d->n_bps >= DBG_MAX_BPS) return -1;
    DbgBreakpoint *bp = &d->bps[d->n_bps];
    memset(bp, 0, sizeof *bp);
    const char *base = basename ? dbg_basename(basename) : "";
    snprintf(bp->file, sizeof bp->file, "%s", base);
    bp->line = line;
    if (cond && *cond) snprintf(bp->cond, sizeof bp->cond, "%s", cond);
    d->n_bps++;
    return d->n_bps;  /* 1-based id */
}

void turi_debug_resume_continue(TuriEnv *env) {
    TuriDebugger *d = dbg_of(env);
    if (d) d->step = DBG_STEP_NONE;
}
void turi_debug_resume_step_in(TuriEnv *env) {
    TuriDebugger *d = dbg_of(env);
    if (d) { d->step = DBG_STEP_IN;   d->step_depth = d->depth; }
}
void turi_debug_resume_step_over(TuriEnv *env) {
    TuriDebugger *d = dbg_of(env);
    if (d) { d->step = DBG_STEP_OVER; d->step_depth = d->depth; }
}
void turi_debug_resume_step_out(TuriEnv *env) {
    TuriDebugger *d = dbg_of(env);
    if (d) { d->step = DBG_STEP_OUT;  d->step_depth = d->depth; }
}

int turi_debug_frame_count(TuriEnv *env) {
    TuriDebugger *d = dbg_of(env);
    if (!d) return 0;
    int n = d->depth;
    if (n > DBG_MAX_FRAMES) n = DBG_MAX_FRAMES;
    if (n <= 0 && d->cur_expr) n = 1;  /* paused at a top-level node */
    return n;
}

/* Map DAP frame index (0 = innermost) to a stored-frame span + name. */
static bool dbg_frame_span(TuriDebugger *d, int idx, Span *out_s,
                           const char **out_name) {
    int n = d->depth;
    if (n > DBG_MAX_FRAMES) n = DBG_MAX_FRAMES;
    if (n <= 0 && d->cur_expr) n = 1;
    if (idx < 0 || idx >= n) return false;
    if (d->depth > 0) {
        int si = d->depth - 1 - idx;
        if (si < 0) si = 0;
        if (si >= DBG_MAX_FRAMES) si = DBG_MAX_FRAMES - 1;
        *out_s    = d->frames[si].cur;
        *out_name = d->frames[si].fn_name;
        /* The innermost frame's executing node is the paused node itself, which
         * is more precise than the last span we stamped on the activation. */
        if (idx == 0 && d->cur_expr) *out_s = d->cur_expr->span;
    } else {
        *out_s    = d->cur_expr ? d->cur_expr->span : SPAN_UNKNOWN;
        *out_name = "<top>";
    }
    return true;
}

bool turi_debug_frame_at(TuriEnv *env, int idx, TuriDbgFrame *out) {
    TuriDebugger *d = dbg_of(env);
    if (!d || !out) return false;
    Span s; const char *name;
    if (!dbg_frame_span(d, idx, &s, &name)) return false;
    const char *path = diag_file_path(s.file_id);
    out->fn_name   = name ? name : "<fn>";
    out->file_path = path ? path : "";
    out->line      = s.line;
    out->col       = s.col_start ? s.col_start : 1;
    out->end_line  = s.line;
    out->end_col   = s.col_end ? s.col_end : out->col;
    return true;
}

/* The lexical frame to inspect for locals at DAP frame index idx. */
static EvalFrame *dbg_lexframe(TuriDebugger *d, int idx) {
    if (d->depth > 0) {
        if (idx == 0 && d->cur_frame) return d->cur_frame;
        int si = d->depth - 1 - idx;
        if (si >= 0 && si < DBG_MAX_FRAMES) return d->frames[si].cf;
        return NULL;
    }
    return d->cur_frame;
}

void turi_debug_frame_locals(TuriEnv *env, int idx,
                             void (*cb)(const char *, const char *, void *),
                             void *ud) {
    TuriDebugger *d = dbg_of(env);
    if (!d || !cb) return;
    EvalFrame *fr = dbg_lexframe(d, idx);
    const char *seen[256];
    int n_seen = 0;
    for (EvalFrame *f = fr; f; f = f->parent) {
        for (EvalBinding *b = f->bindings; b; b = b->next) {
            bool dup = false;
            for (int i = 0; i < n_seen; i++)
                if (strcmp(seen[i], b->name) == 0) { dup = true; break; }
            if (dup) continue;
            if (n_seen < 256) seen[n_seen++] = b->name;
            char repr[256];
            turi_value_repr(repr, sizeof repr, b->value);
            cb(b->name, repr, ud);
        }
    }
}

bool turi_debug_eval_name(TuriEnv *env, int idx, const char *name,
                          char *out_repr, size_t cap) {
    TuriDebugger *d = dbg_of(env);
    if (!d || !name || !out_repr || cap == 0) return false;
    EvalFrame *fr = dbg_lexframe(d, idx);
    for (EvalFrame *f = fr; f; f = f->parent)
        for (EvalBinding *b = f->bindings; b; b = b->next)
            if (strcmp(b->name, name) == 0) {
                turi_value_repr(out_repr, cap, b->value);
                return true;
            }
    TuriValue g = turi_env_get(env, name);
    if (!turi_is_error(g)) {
        turi_value_repr(out_repr, cap, g);
        return true;
    }
    return false;
}

static void format_turi_value_as_expr(char *buf, size_t cap, TuriValue v) {
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
        snprintf(buf, cap, "\"%s\"", v.as_cstr);
        break;
    case TURI_STRUCT:
        if (v.as_struct && v.as_struct->name) {
            snprintf(buf, cap, "(unsafe-cast %lld : %s)",
                     (long long)(intptr_t)v.as_struct, v.as_struct->name);
        } else {
            snprintf(buf, cap, "%lld", (long long)(intptr_t)v.as_struct);
        }
        break;
    default:
        snprintf(buf, cap, "%lld", (long long)v.as_int);
        break;
    }
}

bool turi_debug_eval_expr(TuriEnv *env, int idx, const char *src,
                          char *out_repr, size_t cap) {
    TuriDebugger *d = dbg_of(env);
    if (!d || !src || !out_repr || cap == 0) return false;
    EvalFrame *fr = dbg_lexframe(d, idx);
    if (!fr) {
        snprintf(out_repr, cap, "error: no frame at index %d", idx);
        return false;
    }

    /* 1. Walk visible local variables in the paused frame (innermost binding first). */
    const char *seen[256];
    TuriValue seen_vals[256];
    int n_seen = 0;

    for (EvalFrame *f = fr; f; f = f->parent) {
        for (EvalBinding *b = f->bindings; b; b = b->next) {
            bool dup = false;
            for (int i = 0; i < n_seen; i++) {
                if (strcmp(seen[i], b->name) == 0) { dup = true; break; }
            }
            if (dup) continue;
            if (n_seen < 256) {
                seen[n_seen] = b->name;
                seen_vals[n_seen] = b->value;
                n_seen++;
            }
        }
    }

    /* 2. Construct wrapped source string wrapping the expression in a local let-form:
     * (let [a <val> b <val> ...] <src>) */
    Buf wrapped_src;
    buf_init(&wrapped_src);
    buf_puts(&wrapped_src, "(let [");
    for (int i = 0; i < n_seen; i++) {
        char val_buf[256];
        format_turi_value_as_expr(val_buf, sizeof val_buf, seen_vals[i]);
        buf_printf(&wrapped_src, "%s %s ", seen[i], val_buf);
    }
    buf_puts(&wrapped_src, "] ");
    buf_puts(&wrapped_src, src);
    buf_puts(&wrapped_src, ")");
    buf_putc(&wrapped_src, '\0');

    /* Set in_repl to prevent nested debugger stops. */
    bool was_in_repl = d->in_repl;
    d->in_repl = true;

    TuriValue result = turi_eval(env, wrapped_src.data);

    d->in_repl = was_in_repl;
    buf_free(&wrapped_src);

    /* 3. Format the result and return */
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        snprintf(out_repr, cap, "%s", msg ? msg : "evaluation failed");
        return false;
    }

    turi_value_repr(out_repr, cap, result);
    return true;
}

void turi_debug_disable(TuriEnv *env) {
    if (!env || !env->debugger) return;
    free(env->debugger);
    env->debugger = NULL;
}

/* -------------------------------------------------------------------------
 * SI4: turi_try_show — call Show typeclass instance for TURI_STRUCT values
 * ---------------------------------------------------------------------- */

/* Look up Show [<type_name>] and invoke its `show` method on `val`.  Shared by
 * turi_try_show (TURI_STRUCT receiver, name from the struct tag) and
 * turi_try_show_by_tag (TURI_INT heap pointer, name from the elaborated type
 * tag).  Returns a strdup'd string, or NULL when no matching instance exists
 * or the method does not return a cstr. */
static const char *turi_call_show_named(TuriEnv *env, const char *type_name,
                                        TuriValue val, const Type *recv_ty) {
    if (!env || !env->last_tc_env || !type_name) return NULL;
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

    /* Find the Show [<type_name>] instance.  gde_type_head_name descends a
     * parameterised head (TY_APP) and reads a TY_ADT/TY_REC name, so Vec, Set,
     * Map and user structs/GADTs all match by their surface constructor name. */
    FnDef *show_impl = NULL;
    for (TypeClassInstance *inst = tc_env->instances; inst; inst = inst->next) {
        if (inst->typeclass != show_tc) continue;
        if (inst->n_type_args < 1) continue;
        const char *inst_name = gde_type_head_name(&inst->type_args[0]);
        if (!inst_name || strcmp(inst_name, type_name) != 0) continue;
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

    /* Root cause B (docs/archive/map-show-keyword-key-raw-int.md): a generic
     * instance body -- `Show [Map]`'s `map-show-loop [^Show K ^Show V]` -- shows
     * each element through `(show (:: (hamt/iter-cur-key iter) K))`.  That
     * ascription re-resolves the baked int-carrier representative instance only
     * if `K` is bound in the frame chain, and the ordinary call path binds it
     * from the call site's abi_bindings -- which a call synthesised here has
     * none of.  So every element showed as `Show[int]`, printing the raw
     * carrier: Sym keys AND cstr keys alike (int was right only by the
     * coincidence of carrier == value).
     *
     * Seed the tyvars the same way the compiled path does, from the receiver's
     * own type.  frame_lookup_tyvar walks the parent chain and eval_apply_driven
     * parents the callee frame to cl->captured, so a synthetic frame hung here
     * is visible throughout the instance body; the nested `map-show-loop` call
     * then resolves K/V out of it via frame_record_abi.  recv_ty is NULL for a
     * caller that has only the head tag, which degrades to the old behavior
     * rather than failing. */
    if (recv_ty) {
        EvalFrame *tyframe = eval_frame_new(env, NULL);
        frame_bind_instance_constraint_tyvars(env, tyframe, show_impl, recv_ty);
        if (tyframe->tyvars) cl->captured = tyframe;
    }

    TuriValue fn_val = turi_closure(cl);

    TuriValue result = turi_call(env, fn_val, &val, 1);
    free(cl);

    /* A pre-Stage-4 instance may still hand back a bare cstr. */
    if (result.tag == TURI_CSTR && result.as_cstr) return strdup(result.as_cstr);
    /* Stage 4 (cb414fbd8): `Show`'s method now returns an owned `String`
     * (`defopaque String :ptr<void>`, carried as a TURI_INT handle in the
     * interpreter).  Copy its bytes for display, then release the owned String
     * so the render does not leak one String per REPL result. */
    if (result.tag == TURI_INT && result.as_int) {
        void       *s   = (void *)(intptr_t)result.as_int;
        const char *cs  = tur_string_cstr(s);
        char       *out = cs ? strdup(cs) : NULL;
        tur_string_release(s);
        return out;
    }
    return NULL;
}

const char *turi_try_show(TuriEnv *env, TuriValue val) {
    if (!env || !env->last_tc_env) return NULL;
    if (val.tag != TURI_STRUCT || !val.as_struct || !val.as_struct->name)
        return NULL;
    /* A TURI_STRUCT receiver names its own type and carries its fields as real
     * values, so it needs no element-type seeding. */
    return turi_call_show_named(env, val.as_struct->name, val, NULL);
}

/* Tags that must NOT route through a Show-instance lookup: primitives (their
 * default repr is already correct and cheaper) and ptr<void> (its Show reads
 * the pointer as a result<T,E>, which is wrong for an arbitrary heap value).
 * Pair/Cons are handled ahead of this by turi_show_result. */
static bool show_tag_is_skipped(const char *tag) {
    static const char *const skip[] = {
        "int", "int8", "int16", "int32", "int64",
        "uint8", "uint16", "uint32", "uint64",
        "float", "float32", "float64", "bool", "cstr", "nil", "fn", "sym",
        "ptr<void>", "unknown", "Pair", "PairPtr", "Cons", "ConsPtr", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(tag, skip[i]) == 0) return true;
    return false;
}

const char *turi_try_show_by_tag(TuriEnv *env, TuriValue val,
                                 const char *type_tag) {
    if (!env || val.tag != TURI_INT || !type_tag || !type_tag[0]) return NULL;
    if (show_tag_is_skipped(type_tag)) return NULL;
    /* Pair the head tag with the full type the same eval produced, so a
     * collection's element types survive into instance selection.  The two are
     * set together in turi_eval_impl and describe the same expression; a stale
     * or absent type simply yields NULL seeding. */
    const Type *recv_ty = (const Type *)env->last_result_type;
    if (recv_ty) {
        /* Guard against the tag and the type having drifted apart: only seed
         * when the retained type really is the one this tag names. */
        const char *hn = gde_type_head_name(recv_ty);
        if (!hn || strcmp(hn, type_tag) != 0) recv_ty = NULL;
    }
    return turi_call_show_named(env, type_tag, val, recv_ty);
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
    /* A String RESULT value (Stage 4: `defopaque String :ptr<void>`) displays
     * like a string literal -- quoted, matching the TURI_CSTR repr -- rather
     * than routing through Show[String] (which would print it bare).  This is
     * the owned result the REPL binds to `_`, so read its bytes without
     * releasing it. */
    if (strcmp(type_tag, "String") == 0) {
        const char *cs = val.as_int
                         ? tur_string_cstr((void *)(intptr_t)val.as_int) : "";
        if (!cs) cs = "";
        size_t need = strlen(cs) + 3;   /* two quotes + NUL */
        char *buf = (char *)malloc(need);
        if (!buf) return NULL;
        snprintf(buf, need, "\"%s\"", cs);
        return buf;
    }
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

/* C4: the eval_depth recursion guard was retired (interpreter recursion is now
 * heap-bounded); this setter is kept as a no-op for API/ABI compatibility.
 * Bound total work with turi_env_set_fuel instead. */
void turi_env_set_max_depth(TuriEnv *env, uint32_t depth) {
    (void)env; (void)depth;
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
