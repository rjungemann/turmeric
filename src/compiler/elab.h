#ifndef TUR_ELAB_H
#define TUR_ELAB_H

#include "arena.h"
#include "expr.h"
#include "forms.h"
#include "symbols.h"
#include "typeclass.h"

/* Elaborate a sequence of top-level Forms into a typed Expr program. The
 * result is an Expr of kind EX_PROGRAM, or NULL if any error was diagnosed.
 *
 * `arena` and `st` must outlive the returned Expr (it points into them).
 * `module_base_dir` is the directory to search for imported module files
 * (Phase M2); pass NULL or "." to use the current working directory.
 * `separate_compilation` (Phase M3): when true, imported modules are not
 * inlined; callers emit separate .h/.c per module and #include them.
 * `out_tc_env` (CPS-CL10): if non-NULL, the typeclass environment collected
 * during elaboration is written here on success; the caller may pass this
 * to cps_transform for Clone-instance checking of captured bindings.
 * `out_n_file_scope_defs` (Tier 3): if non-NULL, the number of file-scope
 * definition items prepended to the program (from imported modules) is
 * written here on success; used by the interpreter to distinguish these
 * from (load ...) -expanded forms when computing evaluation ranges. */
/* Transitive-RM: `user_macros` is the shared reader-macro registry used
 * by module loaders (elab_module.c, elab_toplevel.c) so imported files
 * see the entry file's user macros. Pass NULL when no registry is
 * available (REPL, eval, legacy paths). Forward-declared to keep this
 * header lean. */
struct ReaderMacroRegistry;
Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms,
                        uint32_t stdlib_prefix,
                        const char *module_base_dir,
                        bool separate_compilation,
                        bool sandboxed,
                        TypeClassEnv *out_tc_env,
                        const char **include_dirs,
                        int n_include_dirs,
                        uint32_t *out_n_file_scope_defs,
                        struct ReaderMacroRegistry *user_macros);

/* LS2 (local-spice-dev-workflow-plan): per-compile workspace-sibling
 * resolution context. Set by the entry-point (main.c's compile_to_c)
 * around a call into the elaborator and cleared after; the elaborator
 * picks it up when initializing Elab. Outside of compile_to_c the
 * pointer is NULL and the warning code is inert.
 *
 * All arrays are parallel to the include_dirs array passed alongside:
 *   - producer_per_inc[i]: workspace-member directory (e.g. "spices/alpha")
 *     for include dirs added by auto-workspace discovery; NULL for -I
 *     flags, the consumer's own src/, and URL/path :spices deps.
 *   - warned_per_inc[i]: dedup flag, set to true after the first warning
 *     fires for include dir i (per (consumer, producer) pair, per tur
 *     invocation).
 *   - declared_spices[]: keys of the consumer's :spices map, used to
 *     suppress the warning when the producer is already declared.
 *   - debug_resolver: TUR_DEBUG_RESOLVER=1 (full search-path breadcrumbs).
 */
typedef struct Ls2ResolverCtx {
    const char **producer_per_inc;
    bool        *warned_per_inc;
    int          n_inc;
    const char **declared_spices;
    int          n_declared_spices;
    bool         debug_resolver;
} Ls2ResolverCtx;

void                  ls2_resolver_ctx_set(const Ls2ResolverCtx *ctx);
const Ls2ResolverCtx *ls2_resolver_ctx_active(void);

/* used-attr-whole-program: per-compile list of module names to force-load
 * during whole-program elaboration so a `#[used]` defn reached only via a raw
 * mangled C symbol (no `(import)`) is still emitted.  Set by main.c's
 * compile_to_c around the elaborate_program call and cleared after; NULL/0
 * elsewhere (REPL, eval, separate compilation), where the force-load is inert.
 * Module names are slash-separated (e.g. "app/a"), matching `(import ...)`. */
typedef struct UsedModulesCtx {
    const char **modules;
    int          n;
} UsedModulesCtx;

void                  used_modules_ctx_set(const UsedModulesCtx *ctx);
const UsedModulesCtx *used_modules_ctx_active(void);

#endif
