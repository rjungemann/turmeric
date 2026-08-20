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
/* Compiler-side flag (set from --panic-trace) deciding whether emitted main()
 * turns on the *generated program's* runtime g_panic_trace. Named distinctly
 * from that emitted runtime global so a compiled program which links this
 * header (e.g. via `import turi/eval`) does not collide with its own
 * `static int g_panic_trace`. */
extern bool g_emit_panic_trace;

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

/* jit-ffi-c2mir-plan: the program elaborated a dlopen/dlsym/dlclose builtin
 * (or a call-ptr), so the emitted C needs <dlfcn.h> and the link needs -ldl
 * (a no-op on glibc >= 2.34 / macOS, required on older glibc).  Same
 * lifecycle as g_needs_hamt: set during elaboration, read by the preamble. */
extern bool g_needs_dlfcn;

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

/* Phase SZ8: --dump-sizes flag — print inferred size index per sized-GADT
 * constructor application during elaboration (requires -Xsized-types) */
extern bool g_dump_sizes;

/* ER1: --strict-effects flag — warn/check unannotated effectful functions */
extern bool g_strict_effects;

/* ER6: --dump-effects flag — print inferred effect row for each top-level defn */
extern bool g_dump_effects;

/* G1 (docs/upcoming/mutable-globals-plan.md): --dump-write-frames flag — print
 * the WF2 verdict for every DECLARED `#writes` frame, plus the global-write
 * answer that can downgrade it.  A diagnostic knob, not an experiment: it
 * reports what the checker decided and changes nothing.  Without it the only
 * observable difference between VERIFIED and UNVERIFIED is the absence of a
 * diagnostic, which is not something a fixture can assert on. */
extern bool g_dump_write_frames;

/* R4 slice 2 (trusted-refinement-claims-plan): --dump-read-frames flag --
 * print the read-frame verification verdict for every `#reads`-annotated
 * function (VERIFIED / EXCEEDED / UNVERIFIED).  Same character as
 * --dump-write-frames: a diagnostic knob, not an experiment -- it reports
 * what rf_resolve_read_frames decided and changes nothing.  Setting it also
 * makes that pass run even without --enable=checked-reads, so the verdicts
 * are inspectable before opting into the refusal tier. */
extern bool g_dump_read_frames;

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

/* `tur expand`: print each macro expansion (outside stdlib load) to stdout
 * as it happens during elaboration.  Set by cmd_expand in main.c; read at
 * the expansion site in elab_call.c. */
extern bool g_dump_expansion;

/* Stage 3 (macro-system-direction-plan): --macro-caps=io grants the
 * macro-time env TURI_CAP_IO for the rare legitimately-effectful macro
 * (embed-file style).  Default deny; io is the only grantable capability
 * -- FFI/Unsafe/inline-C/async at macro time are never offered.  Set by
 * the global flag parser in main.c; read at macro-env creation in
 * src/turi/macro_env.c. */
extern bool g_macro_caps_io;

/* interp-stdlib-class-method-shadows-user-defn: true while an interpreter
 * entry point (--interpret / REPL / WASM) is preloading stdlib modules via
 * `(load ...)` turns.  Those turns run with stdlib_prefix == 0, so
 * `e->in_stdlib_load` never brackets them and every typeclass they register
 * would read as USER-defined -- flipping the documented "user defn overrides
 * a stdlib method" resolution (prefer_method_dispatch in elab_call.c) and
 * the TUR-W0039 clash warning.  Typeclass registration ORs this in for
 * `tc->from_stdlib`.  Deliberately NOT applied to binding-level
 * `is_from_stdlib`: interpreter fixtures legitimately redefine stdlib defns
 * (the benchmark head/tail stub pattern), and marking those would turn the
 * MF3 collision error on under --interpret. */
extern bool g_turi_stdlib_preload;

/* F4 (cross-plan-followups): --Werror=deprecated flag — promotes
 * ^deprecated use-site warnings to errors. */
extern bool g_werror_deprecated;

/* Phase C: --Werror=inline-c-narrow-params — promote narrow-type-in-inline-C
 * warnings to errors so a strict build can gate against unannotated narrow
 * parameters reaching inline-C bodies. */
extern bool g_werror_inline_c_narrow_params;

/* forall-kinds GRADUATED 2026-07-06 -- explicit kind annotations on
 * forall/exists bound variables ((f :: * -> *)) are always accepted; the enable
 * bit and its elab_types.c gate are gone.  forall-constraints and hkt-hrt
 * graduated the same day (see docs/archive/history/constrained-hkt-forall-plan.md). */

/* forall-dict-pass GRADUATED 2026-07-06 -- always-on.  A genuinely polymorphic
 * constrained function passed as a rank-2 argument is compiled to dispatch its
 * class methods through a runtime dictionary threaded via the poly carrier (a
 * dict-clone of the function + one leading dict argument per constraint,
 * resolved at each invocation).  The enable bit is gone.  See
 * docs/archive/history/forall-dict-pass-multi-constraint-hkt-plan.md. */

/* hrt-curried-result GRADUATED 2026-07-06 -- a rank-2 poly fn whose forall body
 * RESULT is itself a function type (e.g. `forall a. a -> (a -> a)`) always
 * instantiates that result to a concrete callable closure, so `(l x)` yields a
 * closure `((l x) y)` can apply (van Laarhoven optic composition).  The enable
 * bit and its two elab_call.c gates are gone.  See
 * docs/archive/history/constrained-hkt-forall-mode-b-plan.md. */

/* vl-wide-functor GRADUATED 2026-07-04 (VBM4).  A van Laarhoven lens may focus
 * through a WIDE by-value aggregate functor (a `:copy` struct / single-variant
 * flat-product ADT wider than the one-int64 carrier word) unconditionally -- no
 * flag, TUR-E0309 retired.  Path A boxes the aggregate into the carrier at each
 * lens crossing (the dict-dispatched `fmap`, the fat-boxed functor-wrapping `g`,
 * and the lens result) and unboxes it back on the way out, mirroring the
 * direct-shape MB2.5 bridge; the wide-ness test (non-opaque, non-:heap
 * flat-product) still gates it, so carrier-compatible functors are untouched.
 * The former `g_opt_vl_wide_functor` bool is gone; its guards are now
 * unconditional. */

/* VBM1-CM4 (docs/archive/history/van-laarhoven-monomorphization-plan.md +
 * van-laarhoven-consumer-mono-plan.md): the by-value HKT monomorphization path
 * (Path B) GRADUATED 2026-07-05 -- the former `vl-wide-mono` experiment is
 * retired and its registration/redirect/clone emit are unconditional.
 * elab_poly_call registers a spec key for each van Laarhoven lens site whose
 * pinned functor `f` is a WIDE by-value aggregate (see mono_specs.c); the
 * poly-call emit redirects every SIMPLE lens call site that resolves uniquely
 * to a by-value mono body -- no `(f S)`/`(f A)` heap box on that path.  COMPOSED
 * lenses (lens_is_simple_for_pathb == false) fall back to the (unconditional)
 * Path A carrier bridge, which also backs runtime-selected sites.
 * `g_dump_mono_specs` (from `--dump-mono-specs`) prints the registry after
 * elaboration. */
extern bool g_dump_mono_specs;

/* `g_dump_cps_mono` (from `--dump-cps-mono`) prints, for each colored-generic
 * MONOMORPH the direct emitter specializes, whether that monomorph's body +
 * concrete signature would land in the CPS-backend subset (its generic template
 * sig-rejects on the tyvar TY_APP, so the template is never a candidate).  See
 * docs/archive/cps-backend-generic-monomorph-classification-plan.md (G1).
 * Analysis only -- it changes no emitted code. */
extern bool g_dump_cps_mono;

/* g_opt_cps_effects RETIRED 2026-07-12 -- the `cps-effects` experiment graduated
 * and `handle-shallow` is now unconditionally accepted (see experiments.c). */

/* E7: trampolined tail-resume (cps-tramp-resume experiment). See globals.c. */
extern bool g_opt_cps_tramp_resume;
/* E3a: admit an OWNING value captured into a genuinely multi-shot (cloneable /
 * serializable) continuation, giving each captured frame's env clone/drop glue
 * so resumes are memory-safe (cps-backend-owning-env-teardown-e3-plan.md). Read
 * by the cloneable/serial capture checks (elab_effects.c) and the cloneable
 * codegen (emit_cps_ir.c). Gated by the `owning-cloneable-capture` experiment. */
extern bool g_opt_owning_cloneable_capture;

/* CG5/CG8 cycle-gc GRADUATED 2026-08-17 -- `(gc-auto!)` is an ordinary call
 * form; the g_opt_cycle_gc enable bit and the elab_gc_auto gate are gone.
 * GC_AUTO itself is still opt-in (you must CALL `(gc-auto!)`) and is never a
 * default.  See docs/archive/gc-cycle-collection-followup-plan.md. */

/* J1-J3 jit GRADUATED 2026-08-17 -- `tur jit` needs only the -DTUR_JIT=ON
 * build-time gate; the g_opt_jit enable bit is gone and the REPL's in-process
 * JIT loader hangs off engine selection instead.  See
 * docs/archive/jit-engine-plan.md. */

/* closure-drop-glue GRADUATED 2026-07-22 -- the Model R drop-glue header ABI
 * (env[-1] -> drop_glue_env_N, released via TUR_CLOSURE_DROP) is now
 * unconditional; the g_opt_closure_drop_glue enable bit and its codegen gates are
 * gone.  See docs/archive/closure-drop-glue-plan.md. */

/* RT0 refined GRADUATED 2026-08-01 -- static discharge of `#refine{...}`
 * predicates is unconditional; the g_opt_refined enable bit and its
 * elaboration gates are gone.  See
 * docs/archive/refined-graduation-plan.md. */

/* sealed-opaque GRADUATED 2026-08-17 -- the `:sealed` defopaque attribute's
 * ENFORCEMENT is unconditional; the g_opt_sealed_opaque enable bit and the
 * ascribe_check_sealed gate are gone.  See
 * docs/archive/sealed-opaque-plan.md. */

/* write-frames (WF1/WF2, docs/archive/checked-write-frames-plan.md): gates the
 * `#writes` write-frame annotation -- its CHECKING (WF2's TUR-E0382) and every
 * consumer that acts on a checked frame (WF3's callee-frame widening, WF4's
 * entry-check elision).  The annotation itself always PARSES so that adding one
 * is not a breaking change for a consumer who has not enabled the experiment;
 * what the gate withholds is the checking and the acting.
 *
 * `#reads` lived under the `refined` experiment, but that graduated 2026-08-01,
 * so this feature needs its own lifecycle home rather than a retired one. */
extern bool g_opt_write_frames;

/* `checked-reads` experiment (docs/upcoming/trusted-refinement-claims-plan.md,
 * R2): on positive evidence that a `#reads` measure's body reads a mutable
 * global (the same evidence TUR-W0383 reports gatelessly), REFUSE the
 * congruence override instead of merely warning -- the crossing then gets the
 * ordinary TUR-W0372 an unframed impure measure gets.  Refusal keys on "saw a
 * read", never on "could not see": an inline-C body yields no evidence and
 * keeps today's trusted behaviour even with the gate on. */
extern bool g_opt_checked_reads;

/* `jit-ffi` experiment (docs/upcoming/jit-ffi-c2mir-plan.md, F3): the
 * `(unsafe (call-ptr p [T1 T2 -> R] args...))` form -- call an arbitrary
 * function pointer with a signature stated at the site.  Gated because the
 * surface is young and the turi routing depends on a JIT build; the F1/F2
 * plumbing (spice-export thunks, thunk-backed extern-c) is behavior-neutral
 * upgrade and is NOT behind this flag. */
extern bool g_opt_jit_ffi;

/* lang-layers L4: true once a project manifest declared an `:experiments`
 * key (even the empty list), i.e. the project owner scoped the experiment set.
 * A `#lang <base> <semantic-layer>` file whose backing experiment is absent
 * from that scoped set is then a hard error instead of a silent ignore. */
extern bool g_manifest_experiments_scoped;

/* --strict-refine: a diagnostic-strictness knob (NOT an experiment).  Upgrades
 * TUR-E0371 / TUR-W0372 from "keep the runtime check" to a hard compile error,
 * for users who want a fully-discharged build with no silent runtime
 * fallbacks. */
extern bool g_strict_refine;


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
 * docs/archive/history/untyped-native-registration-blocks-curated-facades.md.
 * --------------------------------------------------------------------------- */
typedef enum TurNativeRetType {
    TUR_NRT_INT = 0,   /* default; matches the historical untyped behavior */
    TUR_NRT_FLOAT,
    TUR_NRT_BOOL,
    TUR_NRT_CSTR,
    TUR_NRT_VOID,
    TUR_NRT_PTR,       /* opaque handle -> ptr<void> */
    TUR_NRT_SYNTAX,    /* syntax object (TURI_SYNTAX) -> the Syntax type */
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
