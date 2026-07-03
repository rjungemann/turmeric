/* globals.c — Definitions for global configuration variables shared between
 * the tur compiler driver (main.c) and libturi (tur_eval_basic, REPL, etc.).
 * main.c modifies these after parsing CLI flags; all defaults are "off". */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "globals.h"  /* TurNativeRetType + native-sig registry prototypes */

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

/* Phase C2: --no-contracts strips contract checks (assert!/require!/ensure!/
 * invariant!) at elaboration time -- the predicate is never evaluated -- and
 * makes contract-enabled? fold to false. */
bool g_no_contracts = false;

/* Debugger Phase 4: --debug emits `#line` directives into the generated C and
 * builds single-file targets with `-g -O0`. */
bool g_emit_debug_lines = false;

/* Phase B5: --backtrack-depth N global flag (0 = unlimited) */
int64_t g_backtrack_depth = 0;

/* Phase B5: --dump-clone-plan flag */
bool g_dump_clone_plan = false;

/* CPS1: --dump-cps-coloring flag */
bool g_dump_cps_coloring = false;

/* CPS3: --cps-path flag */
bool g_cps_path = false;

/* Phase U5: Global statistics for unsafe linting */
uint32_t g_unsafe_block_count = 0;
uint32_t g_unsafe_total_lines = 0;

/* Phase P3: HAMT lowering - track if HAMT is needed for this compilation */
bool g_needs_hamt = false;

/* stdlib/re.tur: track when any inline-C in this compilation references
 * <regex.h>. When true, the emitter hoists `#include <regex.h>` into the
 * preamble so multiple re_* functions can share the typedef. */
bool g_needs_regex_h = false;

/* inline-c-function-scope-include-guards fix: deduped set of
 * `#include <...>` / `#include "..."` directives lifted from the top of
 * inline-C bodies during elaboration. Emitted at file scope by emit_program
 * (monolithic) and emit_header (separate-compilation) so include-guarded
 * headers (e.g. sqlite3.h) are visible to every inline-C function in the TU.
 * Per-function `#include` is otherwise skipped by the header's own include
 * guard after the first occurrence -- hiding the header's typedefs from
 * subsequent functions. */
char    **g_hoisted_includes = NULL;
uint32_t  g_n_hoisted_includes = 0;
uint32_t  g_cap_hoisted_includes = 0;

/* AR8: Variadic rest parameters -- track if any variadic defn is compiled */
bool g_has_variadics = false;

/* prelude-macros (Defect B / F3): set when the user-callable `cons` runtime
 * list constructor is referenced.  Gates emission of the `cons` cons-cell
 * helper in both single-file (emit_program) and project-mode
 * (emit_implementation) output so non-cons programs don't churn snapshots. */
bool g_uses_cons = false;

/* Phase G1: -Xgadt flag — enable defgadt syntax.
 * range-gadt-typeclass-migration-plan A1: graduated to default-on. defgadt and
 * GADT pattern matching now work without a flag; -Xgadt is a deprecated no-op
 * (still accepted, mirroring the -Xcallcc graduation). */
bool g_gadt_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xdata-literals is now an accept-and-warn
 * no-op; data-literal syntax (#map{...}, #set{...}, [...] in expression
 * position) is unconditionally on. */
bool g_data_literals_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xjson-reader is now an accept-and-warn
 * no-op; the #json(...) reader macro is unconditionally on. */
bool g_json_reader_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xschema-reader is now an accept-and-warn
 * no-op; #json-str<T>(...) typed-decode readers are unconditionally on. */
bool g_schema_reader_enabled = true;

/* Phase SZ4: -Xsized-types — was opt-in; now ON by default. Sized types are
 * mature: stdlib uses (Static n) (stdlib/sized*.tur), the tur-ecs spice
 * depends on sized storages, and the in-tree fixture coverage is broad. The
 * `-Xsized-types` CLI flag is now a deprecated no-op, mirroring -Xgadt. */
bool g_sized_types_enabled = true;

/* M7: by-value HKT dispatch.  Default ON (end-to-end-monomorphization-plan,
 * stdlib-migration step 5 / "flip default first"): the flag-on suite is green,
 * so the by-value HKT path is the default.  `TUR_M7_HKT=0` opts back out to the
 * legacy carrier path while the stdlib migration is in progress. */
bool g_m7_hkt_enabled = true;

/* Phase SZ8: --dump-sizes flag */
bool g_dump_sizes = false;

/* ER1: --strict-effects flag */
bool g_strict_effects = false;

/* ER6: --dump-effects flag */
bool g_dump_effects = false;

/* CPS2: --dump-cps flag */
bool g_dump_cps = false;

/* Phase I: --emit-abi-trace flag */
bool g_emit_abi_trace = false;

/* ER6: --lint-effects flag */
bool g_lint_effects = false;

bool g_linear_enabled = true;

bool g_unique_enabled = true;

bool g_substructural_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xunion-types is now an accept-and-warn
 * no-op; union type syntax and checking is unconditionally on. */
bool g_union_types_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xintersection-types is now an
 * accept-and-warn no-op; intersection type syntax is unconditionally on. */
bool g_intersection_types_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xeffect-types is now an accept-and-warn
 * no-op; full effect typing (ET0-ET4, LC0-LC3, MS0-MS4) is unconditionally
 * on.  Note: --strict-effects stays opt-in -- always-on effect types do
 * NOT imply always-on strict warnings. */
bool g_effect_types_enabled = true;

/* CT3: Contract checking configuration */
bool g_contracts_enabled = true;          /* contracts always active by default */
bool g_keep_contracts_in_release = false; /* --keep-contracts: retain in release builds */

bool g_sessions_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xdynamic-vars is now an accept-and-warn
 * no-op; dynamic var syntax and checking is unconditionally on. */
bool g_dynvar_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xcallcc is now an accept-and-warn no-op;
 * call/cc / escape are real and unconditionally on (CPS substrate). */
bool g_callcc_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xsymbols is now an accept-and-warn no-op;
 * first-class runtime symbol (:Sym) values are unconditionally on. */
bool g_symbols_enabled = true;

/* INT-2: --interpret mode — true when running tur --interpret. */
bool g_interpret_mode = false;

/* F4 (cross-plan-followups): --Werror=deprecated promotes deprecation
 * warnings emitted by elab_lookup_sym to errors so a clean build can
 * gate against new uses of deprecated APIs. */
bool g_werror_deprecated = false;

/* Phase C: --Werror=inline-c-narrow-params promotes narrow-param-in-inline-C
 * warnings to errors. */
bool g_werror_inline_c_narrow_params = false;

/* XF1 (experimental-flag-mechanism-plan): --allow-experimental suppresses the
 * TUR-W006x experimental-feature warnings.  Default off; intended only for the
 * Turmeric project's own CI matrix runs. */
bool g_allow_experimental = false;

/* Slice 1 of constrained-hkt-forall-plan: `forall-kinds` experiment enable bit.
 * Default off; flipped by --enable=forall-kinds (or :experiments) via the
 * EXPERIMENTS[] descriptor in experiments.c. */
bool g_opt_forall_kinds = false;

/* Slice 2 of constrained-hkt-forall-plan: `forall-constraints` experiment
 * enable bit.  Default off; flipped by --enable=forall-constraints (or
 * :experiments) via the EXPERIMENTS[] descriptor in experiments.c. */
bool g_opt_forall_constraints = false;

/* Slice 3 of constrained-hkt-forall-plan: `hkt-hrt` experiment enable bit.
 * Default off; flipped by --enable=hkt-hrt (or :experiments) via the
 * EXPERIMENTS[] descriptor in experiments.c. */
bool g_opt_hkt_hrt = false;

/* MB1 of constrained-hkt-forall-mode-b-plan: `forall-dict-pass` experiment
 * enable bit.  Default off. */
bool g_opt_forall_dict_pass = false;

/* MB3 of constrained-hkt-forall-mode-b-plan: `hrt-curried-result` experiment
 * enable bit.  Default off.  Gates the curried rank-2 result-is-a-function
 * elaboration ((l x) yields a callable closure). */
bool g_opt_hrt_curried_result = false;

/* WF1 of van-laarhoven-wide-functor-carrier-plan: `vl-wide-functor` experiment
 * enable bit.  Default off.  Gates the wide-by-value functor carrier bridge at
 * the van Laarhoven lens boundary (and lifts TUR-E0309 when set). */
bool g_opt_vl_wide_functor = false;

/* ---------------------------------------------------------------------------
 * Interpreter-native return-type signature registry (see globals.h).
 * --------------------------------------------------------------------------- */
typedef struct TurNativeSig {
    char            *name;   /* owned (strdup) */
    TurNativeRetType ret;
} TurNativeSig;

static TurNativeSig *g_native_sigs;
static size_t        g_native_sigs_count;
static size_t        g_native_sigs_cap;

void tur_native_sig_register(const char *name, TurNativeRetType ret) {
    if (!name) return;
    for (size_t i = 0; i < g_native_sigs_count; i++) {
        if (strcmp(g_native_sigs[i].name, name) == 0) {
            g_native_sigs[i].ret = ret;
            return;
        }
    }
    if (g_native_sigs_count == g_native_sigs_cap) {
        size_t nc = g_native_sigs_cap ? g_native_sigs_cap * 2u : 8u;
        TurNativeSig *grown =
            (TurNativeSig *)realloc(g_native_sigs, nc * sizeof(TurNativeSig));
        if (!grown) return;
        g_native_sigs     = grown;
        g_native_sigs_cap = nc;
    }
    TurNativeSig *e = &g_native_sigs[g_native_sigs_count++];
    e->name = strdup(name);
    e->ret  = ret;
}

bool tur_native_sig_lookup(const char *name, TurNativeRetType *ret) {
    if (!name) return false;
    for (size_t i = 0; i < g_native_sigs_count; i++) {
        if (strcmp(g_native_sigs[i].name, name) == 0) {
            if (ret) *ret = g_native_sigs[i].ret;
            return true;
        }
    }
    return false;
}

void tur_native_sig_clear(void) {
    for (size_t i = 0; i < g_native_sigs_count; i++) free(g_native_sigs[i].name);
    free(g_native_sigs);
    g_native_sigs       = NULL;
    g_native_sigs_count = 0;
    g_native_sigs_cap   = 0;
}
