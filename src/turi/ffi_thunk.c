/* ffi_thunk.c -- see ffi_thunk.h.
 *
 * The per-call hot path is:
 *
 *     TuriValue args[]  --(marshal_i/f)-->  int64_t i_vals[] / double f_vals[]
 *                                    \
 *                                     ----> tur_ffi_thunk_call (giant switch)
 *                                                   |
 *                                                   v
 *                                          typed tur_ffi_call_<ret>_<args>
 *                                                   |
 *                                                   v
 *                                          dlsym'd spice export
 *                                                   |
 *                                                   v
 *                                          int64_t / double / void
 *                                                   |
 *                                                   v
 *                                          TuriValue (turi_int / turi_float / turi_nil)
 *
 * Errors at any step short-circuit to a TuriValue error (turi_errorf),
 * which the eval loop surfaces to the user with the prompt-local span.
 */

#include "ffi_thunk.h"
#include "eval.h"                  /* turi_env_register_native */
#include "ffi_dispatch_thunk.h"    /* tur_ffi_thunk_call */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* TuriValue -> scalar marshaling                                      */
/* ------------------------------------------------------------------ */

/* int-class: int / bool / cstr / nil / struct-as-int all flatten to
 * an int64_t register. Float values are NOT auto-converted because
 * the call site expects a register class match -- silently widening
 * would corrupt the dispatcher's ABI. */
static int marshal_arg_i(const TuriValue *v, int64_t *out) {
    switch (v->tag) {
        case TURI_INT:   *out = v->as_int; return 0;
        case TURI_BOOL:  *out = v->as_bool ? 1 : 0; return 0;
        case TURI_CSTR:  *out = (int64_t)(intptr_t)v->as_cstr; return 0;
        case TURI_NIL:   *out = 0; return 0;
        default:         return -1;
    }
}

/* float-class: accept floats verbatim and ints widened to double. The
 * reverse (float -> int) would lose precision and isn't allowed; users
 * cast explicitly. */
static int marshal_arg_f(const TuriValue *v, double *out) {
    switch (v->tag) {
        case TURI_FLOAT: *out = v->as_float; return 0;
        case TURI_INT:   *out = (double)v->as_int; return 0;
        default:         return -1;
    }
}

static const char *tag_name(TuriTag t) {
    switch (t) {
        case TURI_NIL:    return "nil";
        case TURI_BOOL:   return "bool";
        case TURI_INT:    return "int";
        case TURI_FLOAT:  return "float";
        case TURI_CSTR:   return "cstr";
        default:          return "value";
    }
}

static const char *class_name(char c) {
    switch (c) {
        case 'i': return ":int-class";
        case 'f': return ":float-class";
        case 'v': return ":void";
        default:  return "<unknown>";
    }
}

/* ------------------------------------------------------------------ */
/* TuriNativeFn shim                                                   */
/* ------------------------------------------------------------------ */

/* The user-data pointer carried by each installed binding. The shim
 * looks up the typed dispatcher via tur_ffi_thunk_call using these
 * fields, then marshals args/results. We allocate one of these per
 * (binding-name, export) pair and leak them at REPL teardown -- the
 * image owns the underlying TurSpiceExport storage, and the shims
 * are themselves tiny (~64 bytes) so per-session leakage is bounded
 * by the export count. */
typedef struct FfiBindingUd {
    const TurSpiceExport *export_;
} FfiBindingUd;

static TuriValue ffi_native_shim(TuriEnv *env, TuriValue *args, uint32_t n,
                                  void *ud) {
    (void)env;
    const FfiBindingUd *bud = (const FfiBindingUd *)ud;
    const TurSpiceExport *e = bud->export_;

    /* RP4 v1 limits: reject variadic exports and unsupported classes
     * with messages that point at the most useful next step. */
    if (e->is_variadic) {
        return turi_errorf(
            "ffi: variadic spice export '%s/%s' is not callable from the "
            "REPL yet (cons-list marshaling lands in a later RP)",
            e->module, e->name);
    }
    if (e->ret_class == '?') {
        return turi_errorf(
            "ffi: spice export '%s/%s' returns a type the FFI layer "
            "can't represent yet -- only :int / :float / :void / "
            "primitives are supported in v1",
            e->module, e->name);
    }
    if (n != e->n_args) {
        return turi_errorf(
            "ffi: '%s/%s' expects %u arg%s, got %u",
            e->module, e->name,
            (unsigned)e->n_args, e->n_args == 1 ? "" : "s",
            (unsigned)n);
    }

    /* Marshal args -- abort on the first mismatch with a per-arg diagnostic so
     * the user knows exactly which value was wrong.  Small arities use inline
     * scratch; wider ones spill to the heap (no fixed arity cap here).  A single
     * cleanup path frees any spill on every exit. */
    int64_t  i_inl[TUR_SPICE_ARITY_FASTPATH];
    double   f_inl[TUR_SPICE_ARITY_FASTPATH];
    int64_t *i_vals = i_inl, *i_heap = NULL;
    double  *f_vals = f_inl, *f_heap = NULL;
    if (n > TUR_SPICE_ARITY_FASTPATH) {
        i_heap = (int64_t *)malloc((size_t)n * sizeof(int64_t));
        f_heap = (double  *)malloc((size_t)n * sizeof(double));
        if (!i_heap || !f_heap) {
            free(i_heap); free(f_heap);
            return turi_error("ffi: out of memory marshalling call");
        }
        i_vals = i_heap; f_vals = f_heap;
    }

    TuriValue result;
    for (uint32_t k = 0; k < n; k++) {
        char cls = e->arg_classes[k];
        if (cls == 'i') {
            if (marshal_arg_i(&args[k], &i_vals[k]) != 0) {
                result = turi_errorf(
                    "ffi: '%s/%s' arg %u: expected %s, got %s",
                    e->module, e->name, k,
                    class_name(cls), tag_name(args[k].tag));
                goto cleanup;
            }
        } else if (cls == 'f') {
            if (marshal_arg_f(&args[k], &f_vals[k]) != 0) {
                result = turi_errorf(
                    "ffi: '%s/%s' arg %u: expected %s, got %s",
                    e->module, e->name, k,
                    class_name(cls), tag_name(args[k].tag));
                goto cleanup;
            }
        } else {
            result = turi_errorf(
                "ffi: '%s/%s' arg %u has unsupported class '%c'",
                e->module, e->name, k, cls);
            goto cleanup;
        }
    }

    {
        int64_t out_i = 0;
        double  out_f = 0.0;
        if (e->ffi_shim) {
            /* interpreter-arbitrary-arity-ffi (Phase 3): the spice emitted a
             * per-export shim with its concrete signature baked in.  Call it
             * directly with the marshalled buffers -- no shape table, so no
             * arity ceiling and int/float mix is irrelevant. */
            e->ffi_shim(i_vals, f_vals, &out_i, &out_f);
        } else {
            /* Fallback: a spice built before shim emission.  The generated
             * dispatcher covers arg-shapes up to its --max-arity (6 by
             * default); a call whose shape exceeds that fails here with a
             * rebuild/regenerate diagnostic.  This is an interpreter-FFI
             * dispatch limit, independent of the (now unbounded) descriptor:
             * the export loaded fine and its siblings remain callable. */
            int rc = tur_ffi_thunk_call(e->ret_class,
                                        e->arg_classes, e->n_args,
                                        e->fn_ptr, i_vals, f_vals,
                                        &out_i, &out_f);
            if (rc != 0) {
                result = turi_errorf(
                    "ffi: '%s/%s' has no registered dispatcher for shape "
                    "(arity %u). Rebuild the spice (its sources changed -> "
                    "the REPL regenerates an unbounded per-export shim), or "
                    "regenerate src/runtime/ffi_dispatch_thunk.c with "
                    "`python3 tools/gen_ffi_dispatch.py --max-arity N`.",
                    e->module, e->name, (unsigned)e->n_args);
                goto cleanup;
            }
        }
        switch (e->ret_class) {
            case 'i': result = turi_int(out_i);   break;
            case 'f': result = turi_float(out_f); break;
            case 'v': result = turi_nil();        break;
            default:  result = turi_error("ffi: internal: bad ret class"); break;
        }
    }

cleanup:
    free(i_heap);
    free(f_heap);
    return result;
}

/* ------------------------------------------------------------------ */
/* (reload) native                                                     */
/* ------------------------------------------------------------------ */

TuriValue tur_ffi_reload_spice(TuriEnv *env) {
    if (!env) return turi_error("(reload) no env");

    /* RP7: self-heal path -- when the initial REPL load failed
     * (compile error in user code, missing build.tur, etc.), the
     * user can fix the source and retry without restarting. We try
     * a fresh discover from cwd. If there's no project at all, the
     * loader returns 1 and we surface a clean "nothing to reload". */
    if (!env->spice_image) {
        TurSpiceImage *fresh0 = NULL;
        int rc0 = tur_spice_image_load(".", getenv("TUR_BIN"), &fresh0);
        if (rc0 == 1) {
            printf("(reload) no spice project here; nothing to reload\n");
            fflush(stdout);
            return turi_error("(reload) no spice loaded");
        }
        if (rc0 != 0 || !fresh0) {
            /* Loader already printed a diagnostic. */
            return turi_error("(reload) failed");
        }
        env->spice_image = fresh0;
        uint32_t n0 = tur_spice_image_count(fresh0);
        (void)tur_ffi_install_spice_bindings(env, fresh0);
        printf("(reload) loaded %u export%s from %s\n",
               n0, n0 == 1 ? "" : "s", tur_spice_image_root(fresh0));
        fflush(stdout);
        return turi_nil();
    }
    if (tur_spice_image_is_fresh(env->spice_image)) {
        printf("(reload) no changes\n");
        fflush(stdout);
        return turi_nil();
    }

    /* Take a snapshot of where we are so we can reload the *same*
     * project even if the cwd has since drifted (the user may have
     * changed directories via shell escapes, or a spice may have
     * chdir'd). */
    const char *root = tur_spice_image_root(env->spice_image);
    TurSpiceImage *fresh = NULL;
    int rc = tur_spice_image_load(root, getenv("TUR_BIN"), &fresh);
    if (rc != 0 || !fresh) {
        printf("(reload) failed; previous spice image left in place\n");
        fflush(stdout);
        return turi_error("(reload) failed");
    }

    /* Push the old image onto the retired list so existing FFI
     * bindings keep their borrowed name strings valid. */
    struct TurSpiceImageNode *node =
        (struct TurSpiceImageNode *)calloc(1, sizeof(*node));
    if (!node) {
        /* Out of memory mid-swap: leak the new image and abort the
         * reload to preserve the old one (any other choice trades a
         * small leak for use-after-free risk in installed bindings). */
        tur_spice_image_free(fresh);
        return turi_error("(reload) oom");
    }
    node->image = env->spice_image;
    node->next  = env->retired_spice_images;
    env->retired_spice_images = node;

    env->spice_image = fresh;
    uint32_t n_exports = tur_spice_image_count(fresh);
    (void)tur_ffi_install_spice_bindings(env, fresh);
    printf("(reload) rebuilt %u export%s\n",
           n_exports, n_exports == 1 ? "" : "s");
    fflush(stdout);
    return turi_nil();
}

static TuriValue native_reload(TuriEnv *env, TuriValue *args, uint32_t n,
                                void *ud) {
    (void)args; (void)ud;
    if (n != 0) {
        return turi_errorf("(reload) takes no arguments, got %u", n);
    }
    return tur_ffi_reload_spice(env);
}

void tur_ffi_register_reload_native(TuriEnv *env) {
    if (!env) return;
    turi_env_register_native(env, "reload", native_reload, NULL);
}

/* ------------------------------------------------------------------ */
/* Binding installer                                                   */
/* ------------------------------------------------------------------ */

uint32_t tur_ffi_install_spice_bindings(TuriEnv *env, TurSpiceImage *img) {
    if (!env || !img) return 0;
    uint32_t count = tur_spice_image_count(img);
    for (uint32_t i = 0; i < count; i++) {
        const TurSpiceExport *e = tur_spice_image_at(img, i);
        if (!e) continue;

        /* One FfiBindingUd per binding; both bindings (bare + qualified)
         * share it. Leaked at process exit -- see comment on the struct. */
        FfiBindingUd *ud = (FfiBindingUd *)calloc(1, sizeof(*ud));
        if (!ud) {
            fprintf(stderr,
                    "tur repl: ffi binding install for '%s/%s' failed (oom)\n",
                    e->module, e->name);
            continue;
        }
        ud->export_ = e;

        /* Bare name: collisions go last-write-wins, matching how the
         * env already handles `(def foo ...)` redefinition. */
        turi_env_register_native(env, e->name, ffi_native_shim, ud);

        /* Qualified `<module>/<defn>` form. We have to build the
         * string ourselves because turi_env_register_native expects a
         * NUL-terminated key it can borrow long-term; passing a local
         * stack buffer would dangle. */
        size_t qlen = strlen(e->module) + 1 + strlen(e->name) + 1;
        char  *qkey = (char *)malloc(qlen);
        if (qkey) {
            snprintf(qkey, qlen, "%s/%s", e->module, e->name);
            turi_env_register_native(env, qkey, ffi_native_shim, ud);
            /* qkey is referenced by the env binding's name field; do
             * not free here. */
        }
    }
    return count;
}
