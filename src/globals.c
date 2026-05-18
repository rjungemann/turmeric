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
