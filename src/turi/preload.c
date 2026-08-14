/* preload.c -- shared stdlib preload sequence for interpreter entry points.
 *
 * See preload.h for the rationale.  Relocated from the inline blocks in
 * src/main.c's cmd_eval so the native `--interpret` path and the WASM REPL
 * (src/web/wasm_glue.c) share one load list.
 */

#include "turi/preload.h"

#include "turi/eval.h"
#include "buf.h"

#include <stdio.h>
#include <string.h>

/* Resolve NULL/"" to the legacy cwd-relative default. */
static const char *preload_root(const char *stdlib_root) {
    return (stdlib_root && stdlib_root[0]) ? stdlib_root : "stdlib";
}

/* Emit and evaluate `(load "<root>/<base>")`. */
static void preload_one(TuriEnv *env, const char *root, const char *base) {
    char form[4300];
    snprintf(form, sizeof form, "(load \"%s/%s\")", root, base);
    TuriValue sv = turi_eval(env, form);
    (void)sv; /* a failed stdlib load surfaces via the env's diag sink */
}

void turi_env_preload_macros(TuriEnv *env, const char *stdlib_root) {
    if (!env) return;
    const char *root = preload_root(stdlib_root);
    /* Each in its own eval so the file_id / Phase M7 promotion ordering that
     * cmd_eval documents holds: macros first (and/or/when/cond/for/...), then
     * contract (assert!/require!/ensure!/invariant!). */
    preload_one(env, root, "macros.tur");
    preload_one(env, root, "contract.tur");
}

void turi_env_preload_native_stubs(TuriEnv *env) {
    if (!env) return;
    /* Inject typed stubs so the elaborator knows the signatures of the native
     * functions used by benchmark scripts AND the untyped carrier-list ops
     * (nil-value/cons/head/tail).  Without these, a bare `cons`/`head`/`tail`
     * resolves to the elaborator's BS_FUNC_CALL builtin, which the tree-walker's
     * eval_builtin cannot execute -- its default arm silently returns nil (see
     * src/turi/eval.c).  That is exactly the REPL-only
     * `(list-head (cons 65 (cons 66 0))) => nil` divergence
     * (docs/archive/repl-list-head-over-cons-returns-nil.md): under
     * `--interpret` these stubs make `cons` a user-defn call the runtime native
     * (registered by wk_register_stdlib_natives, which overrides the stub body)
     * actually services, so the same expression returns 65.  Loaded AFTER
     * turi_env_preload_macros and BEFORE turi_env_preload_collections so the real
     * module defns own any overlapping name, and BEFORE
     * turi_env_register_interpreter_natives so the native shims, registered last,
     * override any inline-C module body the interpreter cannot execute.  The
     * stubs whose names ARE defined by a preloaded collection module (vec-get,
     * ok?/some?/err?, ...) are deliberately omitted to avoid the "already defined
     * by an auto-loaded stdlib module" collision. */
    TuriValue sv = turi_eval(env,
        /* list operations */
        "(defn nil-value [] :int 0)\n"
        /* Head is a polymorphic tyvar, not :int, so the stub matches the
         * compiled-path `cons` builtin's wildcard head (elab_call.c's
         * cons_wildcard bypass): a cons cell is a pointer-as-int64 carrier and
         * accepts any 64-bit-sized head -- int, cstr, opaque handle.  Typing it
         * :int made the elaborator reject a cstr head (`(cons "a" 0)`) under
         * --interpret while the compiled path accepted it, the re-string parity
         * gap.  The tail stays :int (the carrier) and the return stays :int; the
         * runtime native (native_cons) boxes the head through intptr_t exactly
         * as codegen does. */
        "(defn cons [A] [v :A n :int] :int 0)\n"
        "(defn head [lst :int] :int 0)\n"
        "(defn tail [lst :int] :int 0)\n"
        /* vec operations.  vec-get/vec-set!/vec-free are dropped here -- the real
         * vec.tur (preloaded next) defines them, and a stub would collide with
         * "already defined by an auto-loaded stdlib module".  vec-new-filled is
         * benchmark-only (no module defn). */
        "(defn vec-new-filled [n :int v :int] :int 0)\n"
        /* numeric helpers.  cstr->parse-int / int->float / bit-shr / bit-xor
         * stubs are dropped: all are native-backed and/or kind-preserving
         * builtins the bare call resolves at elaboration -- a :int stub would
         * shadow the narrow-int builtin behavior. */
        "(defn println-float [x :float d :int] :nil nil)\n"
        "(defn int->unit-float [x :int] :float 0.0)\n"
        "(defn tur-sqrt [x :float] :float 0.0)\n"
        /* HAMT operations for hash_map benchmark */
        "(defn hamt-new [] :int 0)\n"
        "(defn hamt-free [m :int] :nil nil)\n"
        "(defn hamt-set [m :int hash :int key :int val :int] :int 0)\n"
        "(defn hamt-get [m :int hash :int key :int] :int 0)\n"
        "(defn hamt-hash-ptr [p :int] :int 0)\n"
        /* I/O benchmark helpers (file_read.tur, file_write.tur) */
        "(defn write-temp-file [path :cstr n :int] :nil nil)\n"
        "(defn io-fopen-read [path :cstr] :int 0)\n"
        "(defn io-fread-chunk [fp :int buf :int] :int 0)\n"
        "(defn io-fclose [fp :int] :nil nil)\n"
        "(defn io-remove [path :cstr] :nil nil)\n"
        "(defn io-buf-new [] :int 0)\n"
        "(defn io-buf-free [buf :int] :nil nil)\n"
        "(defn io-alloc [n :int v :int] :int 0)\n"
        "(defn io-free [buf :int] :nil nil)\n"
        "(defn io-fopen-write [path :cstr] :int 0)\n"
        "(defn io-fwrite-chunk [fp :int buf :int offset :int chunk :int] :int 0)\n"
        /* Whole-benchmark natives (random_access, thread_ring, nbody, ray_tracing) */
        "(defn random-access-bench [size :int reads :int] :int 0)\n"
        "(defn run-ring [n :int m :int] :nil nil)\n"
        "(defn run-nbody [n :int steps :int] :nil nil)\n"
        "(defn run-raytracer [w :int h :int] :int 0)\n"
        /* none? predicate signature the elaborator needs so the native types as
         * :bool (not :int, which trips the strict "if condition must be bool"
         * check).  ok?/err?/some? are dropped because result.tur / option.tur
         * (both preloaded next) define them; none? has no module defn. */
        "(defn none? [r :int] :bool false)\n"
    );
    (void)sv;
}

void turi_env_preload_collections(TuriEnv *env, const char *stdlib_root) {
    if (!env) return;
    const char *root = preload_root(stdlib_root);

    /* The typeclass-stub + typed-collection set the compiled path auto-loads.
     * Kept in sync with cmd_eval's `prelude[]` in src/main.c.  contract.tur is
     * NOT here (it is loaded up front by turi_env_preload_macros, next to
     * macros.tur, because the M7 macro promotion is order-sensitive).  sym.tur
     * is appended last so first-class :Sym ops (and the Hash/Eq[Sym] instances
     * a keyword-keyed map needs) are available. */
    static const char *prelude[] = {
        "safe.tur",
        "typeclass-eq.tur", "typeclass-functor.tur", "typeclass-clone.tur",
        "typeclass-drop.tur",
        "typeclass-hash.tur", "typeclass-applicative.tur",
        "typeclass-alternative.tur", "typeclass-monad.tur",
        "typeclass-monaderror.tur", "typeclass-bifunctor.tur",
        "hamt.tur", "set.tur", "map.tur",
        "vec.tur", "slice.tur", "option.tur", "result.tur",
        "pair.tur", "tuple.tur", "list.tur", "grid.tur", "zipper.tur",
        "mutmap.tur",
        "unique.tur",
        "sym.tur",
        NULL
    };

    /* Build one `(load A)(load B)...` blob so the whole prelude elaborates in a
     * single turi_eval -- distinct file_ids, deps resolved in declaration
     * order. */
    Buf src;
    buf_init(&src);
    for (int i = 0; prelude[i] != NULL; i++) {
        buf_puts(&src, "(load \"");
        buf_puts(&src, root);
        buf_putc(&src, '/');
        buf_puts(&src, prelude[i]);
        buf_puts(&src, "\")\n");
    }
    buf_putc(&src, '\0'); /* turi_eval calls strlen; NUL-terminate */
    TuriValue sv = turi_eval(env, src.data);
    (void)sv;
    buf_free(&src);
}

void turi_env_preload_typeclasses(TuriEnv *env, const char *stdlib_root) {
    if (!env) return;
    /* Load ONLY typeclass-show.tur (Show class + primitive instances +
     * Show[Vec]/Show[Set]/Show[Map]), not the full typeclass.tur.  The rest of
     * typeclass.tur (Error/Display/Debug[ptr<void>]) has inline-C instance
     * methods -- notably Error's error-message -- that would shadow interpreter
     * builtins the async runtime relies on.  Loaded after
     * turi_env_preload_collections so the collection Show instances see their
     * backing Vec/Set/Map types. */
    preload_one(env, preload_root(stdlib_root), "typeclass-show.tur");
}
