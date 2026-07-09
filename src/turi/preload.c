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
