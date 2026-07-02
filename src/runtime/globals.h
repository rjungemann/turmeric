#pragma once
/* globals.h — extern declarations for global compiler configuration variables.
 * Definitions are in globals.c. Include this wherever these globals are used. */
#include <stdbool.h>
#include <stdint.h>

/* Phase U5: unsafe linting configuration */
extern uint32_t g_unsafe_max_lines;
extern bool g_unsafe_warn_nested;
extern bool g_unsafe_require_safety;
extern bool g_unsafe_stats_enabled;
extern bool g_lint_unsafe_enabled;
/* U6: warn on inline-C outside #{Unsafe}-annotated functions */
extern bool g_lint_inline_c_unsafe;

/* Phase R5: panic strategy */
extern bool g_panic_abort;
extern bool g_panic_trace;

/* Phase R6: result/panic linting */
extern bool g_warn_unused_result;
extern bool g_lint_panic;

/* Phase C2: --no-contracts (strip contract checks at elaboration) */
extern bool g_no_contracts;

/* Debugger Phase 4: --debug emits `#line N "file.tur"` directives into the
 * generated C (so gdb/lldb step through .tur source) and switches the single-
 * file `tur build` C-compile to `-g -O0`.  Off by default so ordinary builds
 * and `emit-c` snapshots are unchanged. */
extern bool g_emit_debug_lines;

/* Phase B5: backtrack depth + clone plan dump */
extern int64_t g_backtrack_depth;
extern bool g_dump_clone_plan;

/* CPS1: --dump-cps-coloring flag — print colored/uncolored partition after CPS1 */
extern bool g_dump_cps_coloring;

/* CPS3: --cps-path flag — emit CPS wrappers for colored functions */
extern bool g_cps_path;

/* Phase U5: unsafe linting statistics */
extern uint32_t g_unsafe_block_count;
extern uint32_t g_unsafe_total_lines;

/* Phase P3: HAMT lowering */
extern bool g_needs_hamt;

/* AR8: Variadic rest parameters -- set when any variadic defn is compiled */
extern bool g_has_variadics;

/* prelude-macros (Defect B / F3): set when the user-callable `cons` runtime
 * list constructor is referenced. */
extern bool g_uses_cons;

/* Phase G1: -Xgadt flag — enable defgadt syntax */
extern bool g_gadt_enabled;

/* DL0 (data-literals-plan): -Xdata-literals flag — enable map/vec/set data
 * literal syntax (#map{...}, #set{...}, and [...] in expression position). */
extern bool g_data_literals_enabled;

/* JR0 (json-reader-macro-plan): -Xjson-reader flag — enable the #json(...)
 * compile-time reader macro. */
extern bool g_json_reader_enabled;

/* RD (return-type-dispatch-and-schema plan): -Xschema-reader flag — enable the
 * #json-str<T>(...) family of typed-decode reader macros.  Implies
 * -Xjson-reader and additionally auto-loads schema.tur. */
extern bool g_schema_reader_enabled;

/* Phase SZ4: -Xsized-types flag — enable sized types (implies -Xgadt) */
extern bool g_sized_types_enabled;

/* M7 (end-to-end-monomorphization-plan Phase 3): TUR_M7_HKT env gate — enable
 * the experimental by-value HKT class-method dispatch (element-type threading
 * through `(g b)` returns + per-(f,A) by-value instance-method emit). Default
 * OFF: the shipped codegen path is byte-identical to HEAD. The full feature
 * additionally needs the stdlib HKT instance bodies rewritten to by-value
 * (Phase 4.2); under the flag only by-value-bodied instances (e.g. the
 * docs/upcoming/v2/m7-hkt-probe.tur reference) work end-to-end. */
extern bool g_m7_hkt_enabled;

/* Phase SZ8: --dump-sizes flag — print inferred size index per sized-GADT
 * constructor application during elaboration (requires -Xsized-types) */
extern bool g_dump_sizes;

/* ER1: --strict-effects flag — warn/check unannotated effectful functions */
extern bool g_strict_effects;

/* ER6: --dump-effects flag — print inferred effect row for each top-level defn */
extern bool g_dump_effects;

/* CPS2 (cps-transform-plan): --dump-cps flag — print the ANF/CPS IR for each
 * colored user-level top-level defn */
extern bool g_dump_cps;

/* Phase I: --emit-abi-trace flag — print the resolved ABI path (concrete-clone,
 * carrier, dictionary, polymorphic-wrapper) for each call site during emit-c */
extern bool g_emit_abi_trace;

/* ER6: --lint-effects flag — advisory warnings for unannotated effectful functions */
extern bool g_lint_effects;

/* LT0: -Xlinear flag — enable linear type checking */
extern bool g_linear_enabled;

/* UT0: -Xunique-types flag -- enable uniqueness type checking */
extern bool g_unique_enabled;

/* ST0: -Xsubstructural flag — enable substructural type checking (implies -Xlinear) */
extern bool g_substructural_enabled;

/* IT0: -Xunion-types flag — enable union type syntax and checking */
extern bool g_union_types_enabled;

/* IT2: -Xintersection-types flag — enable intersection type syntax */
extern bool g_intersection_types_enabled;

/* ET4: -Xeffect-types enables full effect typing (TY_HANDLER, handler typing, ET4 checks) */
extern bool g_effect_types_enabled;

/* CT3: Contract checking configuration */
extern bool g_contracts_enabled;          /* -Xcontracts: enable contract syntax (always on in debug) */
extern bool g_keep_contracts_in_release;  /* --keep-contracts: retain checks in release builds */

/* SS0a: -Xsessions flag — enable session type syntax and checking (implies -Xsubstructural) */
extern bool g_sessions_enabled;

/* DV0: -Xdynamic-vars flag — enable dynamic var syntax and checking */
extern bool g_dynvar_enabled;

/* CF4 / call-cc-completion CC5: vestigial.  -Xcallcc once gated the unsound
 * call/cc / escape stub; call/cc / escape are now real, sound, and enabled by
 * default on the CPS substrate, and the flag is a deprecated no-op.  No longer
 * read by the elaborator; kept for one release to avoid churn. */
extern bool g_callcc_enabled;

/* SYM0 (runtime-symbols-plan): -Xsymbols flag — enable first-class runtime
 * symbol values.  When off, a keyword in expression position is a hard error
 * (its only legal uses are syntactic: annotations, :refer, field selectors).
 * When on, `:foo` elaborates to a :Sym literal. */
extern bool g_symbols_enabled;

/* INT-2: --interpret mode flag — set by cmd_eval before elaboration. */
extern bool g_interpret_mode;

/* F4 (cross-plan-followups): --Werror=deprecated flag — promotes
 * ^deprecated use-site warnings to errors. */
extern bool g_werror_deprecated;

/* Phase C: --Werror=inline-c-narrow-params — promote narrow-type-in-inline-C
 * warnings to errors so a strict build can gate against unannotated narrow
 * parameters reaching inline-C bodies. */
extern bool g_werror_inline_c_narrow_params;

/* XF1 (experimental-flag-mechanism-plan): --allow-experimental suppresses the
 * per-use TUR-W006x experimental-feature warnings.  Deliberately ugly --
 * intended for the Turmeric project's own CI matrix; spice users should not
 * set it.  Read by experiment_warn_if_used in src/runtime/experiments.c. */
extern bool g_allow_experimental;

/* Slice 1 of constrained-hkt-forall-plan: enable bit for the `forall-kinds`
 * experiment.  When set, a `forall`/`exists` bound variable may carry an
 * explicit kind annotation `(f :: * -> *)` instead of relying on the
 * lowercase-letter heuristic.  Points at the EXPERIMENTS[] `opt_global` for
 * "forall-kinds"; read by the quantifier parser in elab_types.c. */
extern bool g_opt_forall_kinds;

/* Slice 2 of constrained-hkt-forall-plan: enable bit for the
 * `forall-constraints` experiment.  When set, a `forall` type may carry a
 * constraint vector `[(Show a) ...]`, and each rank-2 instantiation site inside
 * a callee re-discharges those constraints against the concrete type filling
 * the bound variable (TUR-E0294 if no instance is in scope).  Points at the
 * EXPERIMENTS[] `opt_global` for "forall-constraints". */
extern bool g_opt_forall_constraints;

/* Slice 3 of constrained-hkt-forall-plan: enable bit for the `hkt-hrt`
 * experiment.  When set, a rank-2 `forall` parameter may quantify a
 * higher-kinded variable (`(f :: * -> *)`) used as `(f a)` in the body, and
 * the call/instantiation sites validate that the type filling `f` is a type
 * application whose constructor kind matches (TUR-E0295/TUR-E0296).  Points at
 * the EXPERIMENTS[] `opt_global` for "hkt-hrt". */
extern bool g_opt_hkt_hrt;


/* ---------------------------------------------------------------------------
 * Interpreter-native return-type signatures
 * (untyped-native-registration-blocks-curated-facades fix)
 *
 * `turi_register_default_native` / `turi_env_register_native` register an
 * embedder native by (name, fn, ud) with no type information, so the
 * elaborator's eval-mode fallback used to default every native call to :int
 * (with a hand-rolled allow-list for `error?`/`error-message`).  A defn that
 * wraps a native and declares its honest non-:int return type then failed
 * elaboration (TUR-E0707/E0708) because the body was typed :int.
 *
 * This process-global registry lets the typed registration API record the
 * Turmeric type a native's TuriValue result carries at runtime.  The
 * elaborator consults it (replacing the allow-list) so both a direct call and
 * a curated typed wrapper see the right return type.  The enum lives here --
 * in the neutral runtime layer shared by the compiler and the embedder API --
 * so neither side has to include the other's headers.  See
 * docs/archive/untyped-native-registration-blocks-curated-facades.md.
 * --------------------------------------------------------------------------- */
typedef enum TurNativeRetType {
    TUR_NRT_INT = 0,   /* default; matches the historical untyped behavior */
    TUR_NRT_FLOAT,
    TUR_NRT_BOOL,
    TUR_NRT_CSTR,
    TUR_NRT_VOID,
    TUR_NRT_PTR,       /* opaque handle -> ptr<void> */
} TurNativeRetType;

/* Register (or replace) the return-type signature for native `name`.  `name`
 * is copied; last write wins.  Registering TUR_NRT_INT is a no-op-equivalent
 * (the elaborator default), but is still recorded so a later override is
 * visible. */
void tur_native_sig_register(const char *name, TurNativeRetType ret);
/* Look up `name`; on hit writes *ret and returns true, else returns false. */
bool tur_native_sig_lookup(const char *name, TurNativeRetType *ret);
/* Drop every registered signature (mirrors turi_clear_default_natives). */
void tur_native_sig_clear(void);
