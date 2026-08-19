/* macro_env.c -- the per-compile macro-time interpreter env.
 *
 * Stage 1 of docs/upcoming/macro-system-direction-plan.md: the elaborator
 * gets a lazily-created, capability-denied, fuel-limited turi env hung off
 * its Elab/ElabSession, torn down with the session.  Stage 2's procedural
 * macros (`defmacro*`) evaluate their bodies in this env; nothing in the
 * compiler calls into it yet beyond creation/teardown.
 *
 * This file lives on the turi side of the tree because the include
 * direction in this codebase is turi -> compiler (src/turi/eval.c already
 * includes elab.h); the compiler sees only the two lean declarations in
 * elab.h with `struct TuriEnv` opaque.
 *
 * The two reentrancy hazards this file owns (the Stage 1 audit):
 *
 *  - `turi_env_new` unconditionally sets the process-global
 *    `g_interpret_mode = true` (env.c, libturi-embed-interpret-mode-flag)
 *    on the theory that anybody creating an env is an interpreter
 *    embedder.  Here the caller is the COMPILER, mid-elaboration, and
 *    leaving the flag flipped would change the enclosing compile's
 *    semantics: `#?(:tur ...)` reader-conds would pick the `:turi` arm and
 *    unknown call heads would demote from hard errors to TUR-W0040
 *    runtime dispatch.  So creation brackets the flag (the same
 *    save/set/call/restore the JIT REPL uses around compile_to_c).
 *
 *  - Each `turi_eval` against the env is already safe: turi_eval_with_sink
 *    (eval.c, "Gap 7") snapshots g_interpret_mode to the env's own
 *    interpret_mode bit for the duration of the call and restores it, and
 *    saves/restores the diagnostic sink and file registry around the
 *    nested elaboration.  Macro-time evaluation deliberately KEEPS
 *    env->interpret_mode = true: macro bodies run interpreted, so they see
 *    `#?(:turi ...)` arms and interpreter call semantics -- that is what
 *    "the macro-time language is the interpreted language" means.
 */
#include "turi/eval.h"
#include "turi/env.h"

#include "elab.h"
#include "elab_internal.h"
#include "globals.h"

struct TuriEnv *elab_macro_env_get(ElabSession *session) {
    Elab *e = (Elab *)session;
    if (!e) return NULL;
    if (e->macro_env) return e->macro_env;

    /* Bracket the constructor's g_interpret_mode side effect (see above). */
    bool saved_interp = g_interpret_mode;
    TuriEnv *env = turi_env_new();
    g_interpret_mode = saved_interp;
    if (!env) return NULL;

    /* Macro expansion must be deterministic and effect-free: deny every
     * capability (no I/O, FFI, inline-C, async, unsafe, import) and bound
     * total work with step fuel so a runaway macro-time loop is a
     * diagnostic, not a hung compile.  Stage 3 grants TURI_CAP_IMPORT
     * transiently for `:for-macros` module loads; nothing else is ever
     * granted here. */
    turi_env_deny(env, TURI_CAP_ALL);
    turi_env_set_fuel(env, TURI_DEFAULT_SANDBOX_FUEL);

    e->macro_env = env;
    return env;
}

void elab_macro_env_dispose(struct TuriEnv *env) {
    if (env) turi_env_free(env);
}
