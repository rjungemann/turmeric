/* globals.c — Definitions for global configuration variables shared between
 * the tur compiler driver (main.c) and libturi (tur_eval_basic, REPL, etc.).
 * main.c modifies these after parsing CLI flags; all defaults are "off". */
#include <stdbool.h>
#include <stdint.h>

/* Phase U5: Global configuration for unsafe linting */
uint32_t g_unsafe_max_lines = 20;
bool g_unsafe_warn_nested = false;
bool g_unsafe_require_safety = false;
bool g_unsafe_stats_enabled = false;
bool g_lint_unsafe_enabled = false;
/* U6: warn on inline-C outside #{Unsafe}-annotated functions */
bool g_lint_inline_c_unsafe = false;

/* Phase R5: Panic strategy configuration */
bool g_panic_abort = false;
bool g_panic_trace = false;

/* Phase R6: Result/panic linting configuration */
bool g_warn_unused_result = false;
bool g_lint_panic = false;

/* Phase B5: --backtrack-depth N global flag (0 = unlimited) */
int64_t g_backtrack_depth = 0;

/* Phase B5: --dump-clone-plan flag */
bool g_dump_clone_plan = false;

/* Phase U5: Global statistics for unsafe linting */
uint32_t g_unsafe_block_count = 0;
uint32_t g_unsafe_total_lines = 0;

/* Phase P3: HAMT lowering - track if HAMT is needed for this compilation */
bool g_needs_hamt = false;

/* stdlib/re.tur: track when any inline-C in this compilation references
 * <regex.h>. When true, the emitter hoists `#include <regex.h>` into the
 * preamble so multiple re_* functions can share the typedef. */
bool g_needs_regex_h = false;

/* AR8: Variadic rest parameters -- track if any variadic defn is compiled */
bool g_has_variadics = false;

/* Phase G1: -Xgadt flag — enable defgadt syntax */
bool g_gadt_enabled = false;

/* ER1: --strict-effects flag */
bool g_strict_effects = false;

/* ER6: --dump-effects flag */
bool g_dump_effects = false;

/* ER6: --lint-effects flag */
bool g_lint_effects = false;

/* LT0: -Xlinear flag — enable linear type checking */
bool g_linear_enabled = false;

/* UT0: -Xunique-types flag -- enable uniqueness type checking */
bool g_unique_enabled = false;

/* ST0: -Xsubstructural flag — enable substructural type checking (implies -Xlinear) */
bool g_substructural_enabled = false;

/* IT0: -Xunion-types flag — enable union type syntax and checking */
bool g_union_types_enabled = false;

/* IT2: -Xintersection-types flag — enable intersection type syntax */
bool g_intersection_types_enabled = false;

/* ET4: -Xeffect-types flag — enable full effect typing */
bool g_effect_types_enabled = false;  /* ET4: -Xeffect-types */

/* CT3: Contract checking configuration */
bool g_contracts_enabled = true;          /* contracts always active by default */
bool g_keep_contracts_in_release = false; /* --keep-contracts: retain in release builds */

/* SS0a: -Xsessions flag — enable session type syntax and checking */
bool g_sessions_enabled = false;

/* DV0: -Xdynamic-vars flag — enable dynamic var syntax and checking */
bool g_dynvar_enabled = false;

/* INT-2: --interpret mode — true when running tur --interpret. */
bool g_interpret_mode = false;

/* F4 (cross-plan-followups): --Werror=deprecated promotes deprecation
 * warnings emitted by elab_lookup_sym to errors so a clean build can
 * gate against new uses of deprecated APIs. */
bool g_werror_deprecated = false;
