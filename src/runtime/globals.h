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

/* Phase B5: backtrack depth + clone plan dump */
extern int64_t g_backtrack_depth;
extern bool g_dump_clone_plan;

/* Phase U5: unsafe linting statistics */
extern uint32_t g_unsafe_block_count;
extern uint32_t g_unsafe_total_lines;

/* Phase P3: HAMT lowering */
extern bool g_needs_hamt;

/* AR8: Variadic rest parameters -- set when any variadic defn is compiled */
extern bool g_has_variadics;

/* Phase G1: -Xgadt flag — enable defgadt syntax */
extern bool g_gadt_enabled;

/* Phase SZ4: -Xsized-types flag — enable sized types (implies -Xgadt) */
extern bool g_sized_types_enabled;

/* Phase SZ8: --dump-sizes flag — print inferred size index per sized-GADT
 * constructor application during elaboration (requires -Xsized-types) */
extern bool g_dump_sizes;

/* ER1: --strict-effects flag — warn/check unannotated effectful functions */
extern bool g_strict_effects;

/* ER6: --dump-effects flag — print inferred effect row for each top-level defn */
extern bool g_dump_effects;

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

/* CF4 (control-flow-completeness-plan): -Xcallcc flag — unlock the experimental
 * (unsound, no real capture) call/cc / escape desugar.  Default off; ungated
 * call/cc / escape raise TUR-E0700 / TUR-E0701.  Real capture needs the
 * post-1.0 CPS pass. */
extern bool g_callcc_enabled;

/* INT-2: --interpret mode flag — set by cmd_eval before elaboration. */
extern bool g_interpret_mode;

/* F4 (cross-plan-followups): --Werror=deprecated flag — promotes
 * ^deprecated use-site warnings to errors. */
extern bool g_werror_deprecated;

/* Phase C: --Werror=inline-c-narrow-params — promote narrow-type-in-inline-C
 * warnings to errors so a strict build can gate against unannotated narrow
 * parameters reaching inline-C bodies. */
extern bool g_werror_inline_c_narrow_params;
