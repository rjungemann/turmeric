/* experiments.c -- the experimental-feature-flag registry.
 *
 * Successor to the retired `-X<name>` surface.  See experiments.h and
 * docs/archive/history/experimental-flag-mechanism-plan.md.
 *
 * The EXPERIMENTS[] table below is the single source of truth: `tur
 * experiments`, `tur --help`, the docs site, and the release-cut script all
 * read it, and nothing restates the list.  To add a feature, append a row with
 * all seven fields populated and point `opt_global` at a `g_opt_<name>` bool the
 * feature's elaboration reads (keep the trailing `{ 0 }` sentinel last).
 *
 * Shape of a future entry (do not uncomment -- illustrative only):
 *
 *   static const ExperimentDescriptor EXPERIMENTS[] = {
 *     { "fancy-rows",
 *       "extensible row-typed records",
 *       "docs/upcoming/v1/fancy-rows-plan.md",
 *       "0.25.0",                  // introduced
 *       "0.28.0",                  // expires_at (soft deadline; on that version's
 *                                  //   cut, review the row and graduate, shelve, or bump)
 *       XF_LIFECYCLE_PROTOTYPE,
 *       &g_opt_fancy_rows },
 *   };
 */
#include "experiments.h"
#include "globals.h"   /* g_opt_<name> enable bits */

#include <stdio.h>
#include <stdlib.h>   /* getenv, exit, malloc, free */
#include <string.h>

/* The registry.  Empty by design (see file header). */
static const ExperimentDescriptor EXPERIMENTS[] = {
    /* defstruct-as-defadt GRADUATED 2026-06-28 -- a `defstruct` now lowers to a
     * single-variant record `defadt` unconditionally (always-on; the gate lives
     * in defstruct_lowers_to_adt, elab_structs.c).  See
     * docs/archive/defstruct-as-defadt-plan.md. */
    /* B4 byvalue-recursive-carrier GRADUATED 2026-06-25 -- the recursive carrier
     * wrappers (Re/Expr, and wider products carrying an (F Self) field) now flow
     * by value through the fat-closure ABI unconditionally; the gate lives in
     * adt_is_byvalue_product_d (types.c).  See
     * docs/archive/history/b4-fat-closure-byvalue-adt-abi-plan.md. */
    /* forall-kinds GRADUATED 2026-07-06 -- explicit kind annotations on
     * forall/exists bound variables (e.g. (f :: * -> *)) are always accepted;
     * the pre-elaboration gate at elab_types.c is removed.  See
     * docs/archive/history/constrained-hkt-forall-plan.md. */
    /* forall-constraints GRADUATED 2026-07-06 -- typeclass constraint vectors
     * on forall types (e.g. (forall [a] [(Show a)] (-> a cstr))) are always
     * parsed and always enforced at each rank-2 instantiation site; the gate in
     * elab_types.c is removed.  See docs/archive/history/constrained-hkt-forall-plan.md. */
    /* hkt-hrt GRADUATED 2026-07-06 -- a rank-2 forall over a higher-kinded
     * variable (e.g. (forall [(f :: * -> *)] (-> (f int) int))) is always
     * validated at instantiation sites; the gate in elab_call.c is removed.
     * See docs/archive/history/constrained-hkt-forall-plan.md. */
    /* forall-dict-pass GRADUATED 2026-07-06 -- runtime dictionary passing for a
     * polymorphic constrained function used as a rank-2 argument (mode B) is
     * always-on.  Deficit 1 (dict-clone method return-type threading) and
     * Deficit 2 (multi-constraint + HKT-receiver dicts) both landed; a
     * constrained rank-2 forall carries one dictionary per constraint through
     * the poly carrier, and each class-method call in the dict-clone body
     * dispatches through the dict for its own class.  The one residual shape --
     * a constraint method dispatched from inside a nested lambda -- is rejected
     * with TUR-E0311 (never miscompiled) and tracked in
     * docs/archive/history/forall-dict-pass-nested-lambda-method.md.  See
     * docs/archive/history/forall-dict-pass-multi-constraint-hkt-plan.md. */
    /* hrt-curried-result GRADUATED 2026-07-06 -- a curried rank-2 poly fn whose
     * forall body result is itself a function (e.g. (forall [a] (-> a (-> a a))))
     * always instantiates the result to a concrete callable closure, so (l x)
     * yields a callable; the two gates in elab_call.c are removed.  See
     * docs/archive/history/constrained-hkt-forall-mode-b-plan.md. */
    /* vl-wide-functor GRADUATED 2026-07-04 (VBM4 of van-laarhoven-monomorphization-
     * plan) -- a van Laarhoven lens now focuses through a WIDE by-value functor
     * (a :copy struct / flat-product ADT wider than the one-int64 carrier)
     * unconditionally; TUR-E0309 is retired and the Path A carrier box/unbox
     * bridge is always-on (gated only by the wide-ness test, not a flag).  The
     * zero-overhead Path B redirect layers on top via --enable=vl-wide-mono. */
    /* vl-wide-mono GRADUATED 2026-07-05 (CM4 of van-laarhoven-consumer-mono-plan)
     * -- by-value HKT monomorphization across the van Laarhoven lens boundary
     * (Path B) is now unconditional: SIMPLE lenses (direct `fmap` tail) redirect
     * to a by-value mono body with no carrier box.  COMPOSED lenses fell back to
     * the always-on Path A carrier bridge at graduation; that residual was closed
     * the same day by CB1-CB5
     * (docs/archive/history/van-laarhoven-composed-byvalue-plan.md), so composed
     * lenses now thread `(f a)` by value too and Path A is only the CB5 backstop
     * for shapes that cannot be lowered.  The `g_opt_vl_wide_mono` bit is
     * retired. */
    /* panic-return-signal + stackless-catch-unwind GRADUATED 2026-07-07 -- the
     * compiled backend now always lowers panic propagation via the thread-local
     * `tur_panicking` return-path signal (no setjmp/longjmp on the catch-unwind
     * boundary), and a catch-crossing recursive function is always emitted as a
     * flat-stack heap-continuation trampoline.  The gates in emit_expr.c /
     * emit_fns.c / emit_module.c are removed; the feature is unconditional.  See
     * docs/archive/history/catch-unwind-graduation-plan.md. */
    /* cps-effects GRADUATED 2026-07-12 (F2 of
     * docs/archive/compiled-shallow-handlers-plan.md) -- the source-level
     * shallow effect handler (`handle-shallow`) is now unconditionally accepted;
     * the elab_handle gate and g_opt_cps_effects are removed.  handle-shallow
     * lowers to dk_handler_shallow on the CPS path and to the shallow_consumed
     * bubble-up on the fiber path, with compiled == interp on all shapes.  A
     * lingering --enable=cps-effects is an accept-and-warn no-op via GRADUATED[]. */
    /* cps-async GRADUATED 2026-07-19 -- the heap-continuation representation for
     * `async`/`await` is now the unconditional CPS-path lowering: a function
     * containing `await` is always CPS-colored, and each `await` lowers to a
     * `dk_shift` against the entry prompt (capturing the rest of the async body as
     * a heap continuation) instead of the fiber `tur_await_future`/swapcontext.
     * F3 closed every admissibility gap (await-as-shift, deferred pending drive,
     * bounded multi-await; docs/archive/compiled-async-heap-continuations-plan.md);
     * a recursive await evicting to the direct emitter is by-design (F4 declined;
     * docs/archive/history/compiled-stackless-recursive-await-plan.md).  The 2026-07-19
     * graduation attempt surfaced two fiber-interop gaps, both now closed so the
     * heap path is a strict superset of the fiber path:
     *   1. A pending future backed by a runnable scheduler fiber (manual spawn /
     *      TaskGroup) is now driven by __tur_await_body (it drains the scheduler
     *      run-queue before parking), so `taskgroup-async` resolves.
     *   2. An `await` nested in a handler-case body now delegates to the fiber
     *      path (build_letraw, guarded by b->in_handler_case in cps_ir.c) instead
     *      of building a CT_AWAIT the handler-case admission cannot host, so
     *      `async-effect-spawn` colors natively instead of evicting.
     * The gate did its job; the row is retired and the name moved to GRADUATED[]
     * below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/history/cps-async-graduation-plan.md. */
    /* cps-tramp-resume GRADUATED 2026-07-19 -- the CPS/DK trampolined tail-resume
     * path is now the DEFAULT and SOLE lowering for effectful colored code
     * (g_opt_cps_tramp_resume defaults true).  The full corpus DK-lowers every
     * effect (zero tur_effect_perform call sites), so the row is retired and the
     * name moved to GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).
     * See docs/archive/cps-dk-sole-effect-lowering-plan.md. */
    /* owning-cloneable-capture GRADUATED 2026-07-20 -- admitting an owning value
     * (rc handle, :heap carrier handle, by-value aggregate) captured into a
     * multi-shot cloneable continuation, with the per-frame env clone/drop
     * teardown, is now unconditional (g_opt_owning_cloneable_capture defaults
     * true; the admission predicates in elab_effects.c / cps_ir.c / emit_cps_ir.c
     * read it always-on).  Every capture channel and owning shape that is
     * leak-clean in the base language is covered; the remaining rejections are
     * bounded by the base drop's shallowness, not this gate.  The name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/history/cps-backend-owning-env-teardown-e3-plan.md. */
    /* closure-drop-glue GRADUATED 2026-07-22 -- Model R (runtime drop-glue header
     * on every heap fat-closure env, walked on release via TUR_CLOSURE_DROP) is
     * now unconditional: the header ABI is emitted in every build and every
     * fat-handle free (scope-exit, catch-unwind, effect/shift reap, struct
     * fn-field drop, httpd/reactor teardown) routes through it.  The whole corpus
     * is crash- and leak-clean under the always-on ABI (forced-on suite verified
     * before graduation).  The name moves to GRADUATED[] below (a lingering
     * --enable is a TUR-W0063 no-op).  See docs/archive/closure-drop-glue-plan.md. */
    /* CG5/CG8 cycle-gc GRADUATED 2026-08-17 -- `(gc-auto!)` is now an ordinary
     * call form, available without `--enable`.  Read the scope narrowly, the
     * way CG8 decided it: what graduated is the CALL FORM, not a default.
     * `GC_AUTO` remains strictly opt-in -- a program that never calls
     * `(gc-auto!)` still gets the pure-RC path with no collector overhead, and
     * automatic collection is never going to be the default in this language
     * (CG8 (2), rejected permanently, before or after v1).  Ungating costs a
     * non-calling program exactly nothing, because the gate was on the call
     * form while the `rc_cb_alloc_kinded` payload zeroing is conditional on
     * GC_AUTO mode at RUN time.  Baked from 0.30.8 through the whole 0.31-0.33
     * lines; pause time measured and fixed (PT1/PT2), the alloc-path cost
     * measured (~10%, fixed overhead, AUTO-only), and the real-shape workload
     * re-measured after the `set!` refcount leak it surfaced was fixed
     * (steady-state residue ~60 blocks).  The name moves to GRADUATED[] below
     * (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/gc-cycle-collection-followup-plan.md. */
    /* RT0 refined GRADUATED 2026-08-01 -- static discharge of `#refine{...}`
     * predicates is now unconditional.  The runtime contract half was always
     * on; what became unconditional is the STATIC half, so the one
     * user-visible consequence is TUR-E0371 on a refinement violated on every
     * execution reaching it.  Preconditions were measured, not assumed: the
     * in-tree blast radius was one fixture (repurposed, not deleted), and cost
     * on a real ~5400-line program was 1.004x with zero TUR-E0371.  The name
     * moves to GRADUATED[] below and to GRADUATED_LAYERS[] in lang_layers.c --
     * a lingering --enable is a TUR-W0063 no-op, and a lingering
     * `#lang turmeric refined` a TUR-W0064 one.  See
     * docs/archive/refined-graduation-plan.md. */
    /* J1-J3 jit GRADUATED 2026-08-17 -- `tur jit <file>` no longer needs
     * `--enable=jit`.  The gate existed to hold a third execution engine until
     * its parity sweep landed; J3 landed 2026-07-30 and closed every defect it
     * surfaced (three latent product bugs, all fixed), the harness denylist is
     * empty, and tests/run-jit.sh runs the whole corpus through the engine on
     * both hosts.  The remaining gate is the BUILD-TIME one and it stays:
     * `-DTUR_JIT=ON` vendors MIR, a default build carries no fetch and no
     * dependency, and `tur jit` in such a build still says so.  Engine
     * SELECTION is likewise unchanged and is not a default flip -- `cc` is
     * still what you get unless `--engine jit` / `TUR_ENGINE=jit` /
     * `:engine "jit"` says otherwise, and the REPL's in-process JIT loader now
     * hangs off that same selector rather than off this row.  The name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/jit-engine-plan.md. */
    /* sealed-opaque GRADUATED 2026-08-17 -- `(defopaque H :int :sealed)` now
     * enforces unconditionally: outside H's declaring module, `::` refuses both
     * the unwrap and the fabricate direction (TUR-E0302).  Both of the plan's
     * graduation criteria were met -- the ECS spice shipped on it with no
     * in-module pattern needing an escape, and the moduleless-top-level
     * limitation was accepted (not closed) in opaques-guide.md 2026-08-13 --
     * and the two-direction rule the gate existed to question survived that
     * adoption, so it graduates as designed rather than narrowed to
     * fabrication-only.  Unusually low-risk for a graduation: with the gate off
     * `:sealed` already parsed and imposed nothing, so becoming unconditional
     * reaches only code that deliberately wrote `:sealed`.  The name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/sealed-opaque-plan.md. */
    /* write-frames: step 2 of the trajectory stateful-refinements-guide.md
     * sketches ("trusted now -> checkable later -> effect-row eventually").
     * `#reads` is step 1 and graduated with `refined`; this is the checked
     * tier, which is what an optimization may act on.  Gated because the
     * CHECKING can reject a body that compiles today (TUR-E0382) and because
     * WF4 ELIDES a runtime check on the strength of the frame -- neither should
     * arrive unasked-for.  The annotation parses either way.
     *
     * G1 (docs/upcoming/mutable-globals-plan.md) narrows what VERIFIED claims:
     * a frame speaks about PARAMETERS, so a body that writes a mutable global
     * is downgraded to UNVERIFIED rather than stamped with a fact an
     * optimization may act on.  Silent -- a global is outside the frame's
     * vocabulary, not outside the declared frame. */
    { "write-frames",
      "`#writes w` / `#writes [a b]` -- a checked per-argument write frame; "
      "backs frame-aware hypothesis invalidation and entry-check elision",
      "docs/archive/checked-write-frames-plan.md",
      "0.34.0",                  /* introduced */
      "0.38.0",                  /* expires_at -- review at that cut: graduate,
                                  *   shelve, or bump */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_write_frames },
    /* global-state GRADUATED 2026-08-18 (G5b of mutable-globals-plan) -- a
     * `#writes` frame may name a mutable global, an exported global is
     * read-only outside its defining module, and `^atomic` / `^thread-local`
     * are ordinary global annotations.  All unconditional; the name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op). */
    /* checked-reads: R2 of docs/upcoming/trusted-refinement-claims-plan.md.
     * `#reads` is the one TRUSTED claim the refinement solver believes, and
     * its consumer GRANTS congruence -- so a frame that omits mutable state
     * the body reads buys a proof it has not earned, with no runtime fallback
     * at the crossing it enables.  TUR-W0383 already reports the broken
     * promise gatelessly; this gate ESCALATES the same positive evidence (a
     * direct read of a mutable global in the elaborated body) from warn to
     * refuse-the-override, so the crossing becomes the ordinary TUR-W0372 an
     * unframed impure measure gets.
     *
     * Gated because it can only ever turn a currently-proving program into a
     * diagnostic.  Refusal keys on "saw a read", never "could not see": an
     * inline-C measure -- essentially every measure that predates mutable
     * globals -- yields no evidence and keeps trusted behaviour even with the
     * gate on, which is what keeps the nine strict-refine #reads fixtures
     * green under the flag. */
    { "checked-reads",
      "refuse the `#reads` congruence override when the body demonstrably "
      "reads a mutable global the frame omits (escalates TUR-W0383 from "
      "warn to refuse)",
      "docs/upcoming/trusted-refinement-claims-plan.md",
      "0.34.0",                  /* introduced */
      "0.38.0",                  /* expires_at -- review at that cut: graduate,
                                  *   shelve, or bump */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_checked_reads },
    /* jit-ffi: the `(unsafe (call-ptr p [T1 T2 -> R] args...))` form of
     * docs/upcoming/jit-ffi-c2mir-plan.md -- invoke an arbitrary function
     * pointer (typically a dlsym result) with a signature stated at the
     * call site.  AOT codegen is a pure cast-and-call; under --interpret it
     * routes through the c2mir thunk provider, so it works only in
     * -DTUR_JIT=ON builds there (a clean diagnostic otherwise).  Gated
     * because the signature vocabulary is young (scalars only; the
     * struct-by-value "{...}" extension is the plan's F4) and should be
     * able to move without breaking early adopters.  The plan's F1/F2
     * plumbing -- runtime spice-export thunks and thunk-backed extern-c
     * under --interpret -- is a behavior-preserving upgrade and is NOT
     * behind this flag. */
    { "jit-ffi",
      "`(unsafe (call-ptr p [T1 T2 -> R] args...))` -- call an arbitrary "
      "function pointer with a signature stated at the call site",
      "docs/upcoming/jit-ffi-c2mir-plan.md",
      "0.34.0",                  /* introduced */
      "0.38.0",                  /* expires_at -- review at that cut: graduate,
                                  *   shelve, or bump */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_jit_ffi },
    { 0 }, /* sentinel so the array is never zero-length (C forbids that);
            * experiment_count() subtracts it off. */
};

/* Graduated experiments: names that WERE gated behind `--enable=` and are now
 * unconditionally on.  A CLI / manifest / user-config reference to one is
 * accepted as a no-op (TUR-W0063) rather than the hard TUR-E0310 an unknown
 * name gets, so a downstream build.tur or experiments.tur that opted in keeps
 * compiling across the graduation boundary.  This mirrors the retired -X<name>
 * accept-and-warn no-ops (drop-x-flags-plan).  Entries can age out one minor
 * line after graduation, once downstream configs have dropped the flag. */
static const char *const GRADUATED[] = {
    /* A graduated name is added here so a lingering
     * --enable/build.tur/experiments.tur reference to it is accepted as a no-op
     * (TUR-W0063) for one minor line, rather than the hard TUR-E0310 an unknown
     * name gets.  cps-backend graduated 2026-07-11 and its shim was retired once
     * no config referenced it. */
    "cps-effects",   /* graduated 2026-07-12; handle-shallow is now always-on */
    "cps-tramp-resume", /* graduated 2026-07-19; DK trampolined tail-resume is the default+sole effect lowering */
    "cps-async",     /* graduated 2026-07-19; heap-continuation async/await is the unconditional CPS-path lowering */
    "owning-cloneable-capture", /* graduated 2026-07-20; owning capture into a multi-shot cloneable continuation is always-on */
    "closure-drop-glue", /* graduated 2026-07-22; Model R drop-glue header ABI is unconditional */
    "refined",       /* graduated 2026-08-01; static discharge of #refine{...} is unconditional */
    "cycle-gc",      /* graduated 2026-08-17; (gc-auto!) is an ordinary call form -- GC_AUTO is still opt-in, never a default */
    "jit",           /* graduated 2026-08-17; `tur jit` needs only the -DTUR_JIT=ON build gate */
    "sealed-opaque", /* graduated 2026-08-17; `:sealed` enforcement is unconditional */
    "global-state",  /* graduated 2026-08-18; globals in write frames, read-only exported globals, ^atomic / ^thread-local */
    NULL,
};

static bool experiment_is_graduated(const char *name) {
    if (!name) return false;
    for (size_t i = 0; GRADUATED[i]; i++)
        if (strcmp(GRADUATED[i], name) == 0) return true;
    return false;
}

/* Number of real entries (excluding the trailing { 0 } sentinel). */
size_t experiment_count(void) {
    return (sizeof(EXPERIMENTS) / sizeof(EXPERIMENTS[0])) - 1;
}

const ExperimentDescriptor *experiment_at(size_t i) {
    if (i >= experiment_count()) return NULL;
    return &EXPERIMENTS[i];
}

const ExperimentDescriptor *experiment_lookup(const char *name) {
    if (!name) return NULL;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return &EXPERIMENTS[i];
    }
    return NULL;
}

/* Per-index enable source + once-per-compile warning dedup.  Sized to a fixed
 * cap that comfortably exceeds any sane table size (the mechanism is designed
 * so no more than ~2 flags live here at once). */
#define XF_MAX 64
static ExperimentSource g_src[XF_MAX];   /* XF_SRC_NONE = disabled */
static bool             g_warned[XF_MAX]; /* TUR-W006x emitted this compile */

static long experiment_index(const char *name) {
    if (!name) return -1;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return (long)i;
    }
    return -1;
}

/* Once-per-process dedup for the graduated-experiment notice.  A CLI enable is
 * applied in both the parent and each worker, so warn only the first time. */
static bool g_grad_warned;

bool experiment_enable(const char *name, ExperimentSource src) {
    long idx = experiment_index(name);
    if (idx < 0) {
        /* Not a live experiment.  If it graduated, accept it as an always-on
         * no-op with a one-time deprecation notice; otherwise the caller
         * reports TUR-E0310 for an unknown name. */
        if (experiment_is_graduated(name)) {
            if (!g_grad_warned) {
                g_grad_warned = true;
                fprintf(stderr,
                        "warning [TUR-W0063]: experiment '%s' graduated and is "
                        "now on by default; --enable is no longer needed\n",
                        name);
            }
            return true;
        }
        return false;
    }
    if (idx >= XF_MAX) return false;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->opt_global) *d->opt_global = true;
    /* Higher-numbered source wins.  CLI beats manifest beats user-config
     * beats not-yet-set (XF_SRC_NONE); a lower-precedence enable never
     * downgrades a higher one that already ran. */
    if (src > g_src[idx]) g_src[idx] = src;
    return true;
}

bool experiment_is_enabled(const char *name) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return false;
    return g_src[idx] != XF_SRC_NONE;
}

ExperimentSource experiment_source_at(size_t i) {
    if (i >= experiment_count() || i >= XF_MAX) return XF_SRC_NONE;
    return g_src[i];
}

/* UC-4 (user-config-experiments-plan): the TUR-W006x lifecycle warnings now
 * fire unconditionally.  Enabling an experiment (via --enable=<name>,
 * build.tur, or ~/.config/turmeric/experiments.tur) is itself the
 * acknowledgment; there is no longer a --allow-experimental gate to suppress
 * them. */
void experiment_warn_if_used(const char *name) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return;
    if (g_src[idx] == XF_SRC_NONE) return;   /* not enabled -> nothing to warn */
    if (g_warned[idx]) return;               /* once per compile */
    g_warned[idx] = true;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->lifecycle == XF_LIFECYCLE_BETA) {
        fprintf(stderr,
                "warning [TUR-W0061]: experimental feature '%s' (beta) -- "
                "graduates in %s; see %s\n",
                d->name, d->expires_at, d->plan_path);
    } else {
        fprintf(stderr,
                "warning [TUR-W0060]: experimental feature '%s' (prototype) -- "
                "breaking changes likely; see %s\n",
                d->name, d->plan_path);
    }
}

void experiment_reset_warnings(void) {
    memset(g_warned, 0, sizeof(g_warned));
}

/* ------------------------------------------------------------------------- *
 * UC-2: user-level experiments file reader.
 *
 * The file's grammar is a tiny subset of turmeric syntax -- a handful of
 * `:key [atom ...]` pairs, with `;` line comments and `#| |#` block comments.
 * A dedicated hand reader (no manifest reader, no `.tur` evaluation) keeps the
 * surface understandable and free of project-manifest assumptions; see the
 * plan's "The reader" section (option B).
 * ------------------------------------------------------------------------- */

/* Resolve the platform path to the user-level experiments file into `buf`.
 * Returns true (and fills `buf`) when a config directory is known -- the file
 * itself may or may not exist -- and false when no home/config dir can be
 * determined. */
static bool uc_config_path(char *buf, size_t buflen) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        int n = snprintf(buf, buflen, "%s\\turmeric\\experiments.tur", appdata);
        return n > 0 && (size_t)n < buflen;
    }
    return false;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        int n = snprintf(buf, buflen, "%s/turmeric/experiments.tur", xdg);
        return n > 0 && (size_t)n < buflen;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        int n = snprintf(buf, buflen, "%s/.config/turmeric/experiments.tur", home);
        return n > 0 && (size_t)n < buflen;
    }
    return false;
#endif
}

/* Slurp a whole file into a NUL-terminated malloc'd buffer.  Returns NULL if
 * the file cannot be opened (absent / unreadable) or on allocation failure.
 * The caller frees. */
static char *uc_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

typedef enum { UC_EOF, UC_LBRACK, UC_RBRACK, UC_ATOM } UcTok;

static bool uc_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* Advance `*pp` past whitespace, `;' line comments, and `#| |#' block
 * comments (non-nesting, matching the manifest reader's convention). */
static void uc_skip_ws(const char **pp) {
    const char *p = *pp;
    for (;;) {
        while (uc_is_space(*p)) p++;
        if (*p == ';') {                         /* line comment to EOL */
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '#' && p[1] == '|') {        /* block comment */
            p += 2;
            while (*p && !(p[0] == '|' && p[1] == '#')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    *pp = p;
}

/* Lex one token.  For UC_ATOM, [*ts, *te) delimits the atom text (a leading
 * ':' is kept, marking a keyword).  Brackets carry no text. */
static UcTok uc_next(const char **pp, const char **ts, const char **te) {
    uc_skip_ws(pp);
    const char *p = *pp;
    if (*p == '\0') return UC_EOF;
    if (*p == '[') { *pp = p + 1; return UC_LBRACK; }
    if (*p == ']') { *pp = p + 1; return UC_RBRACK; }
    const char *start = p;
    while (*p && !uc_is_space(*p) && *p != '[' && *p != ']' && *p != ';') p++;
    *ts = start;
    *te = p;
    *pp = p;
    return UC_ATOM;
}

/* Copy the atom [ts,te) into `name` (truncating to fit) and enable it at
 * XF_SRC_USER_CONFIG.  Returns experiment_enable's result; on false, `name`
 * holds the offending token for the caller's error message. */
static bool uc_try_enable(const char *ts, const char *te,
                          char *name, size_t namesz) {
    size_t len = (size_t)(te - ts);
    if (len >= namesz) len = namesz - 1;
    memcpy(name, ts, len);
    name[len] = '\0';
    return experiment_enable(name, XF_SRC_USER_CONFIG);
}

bool experiments_read_user_config(void) {
    char path[4096];
    if (!uc_config_path(path, sizeof(path))) return false;
    char *src = uc_slurp(path);
    if (!src) return false;   /* absent / unreadable -> no-op */

    const char *p = src;
    const char *ts, *te;
    UcTok t;
    while ((t = uc_next(&p, &ts, &te)) != UC_EOF) {
        if (t != UC_ATOM || ts[0] != ':') continue;  /* expect a :key; skip stray tokens */

        /* Copy the key name (without the leading ':'). */
        char key[128];
        size_t klen = (size_t)(te - ts) - 1;
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, ts + 1, klen);
        key[klen] = '\0';

        if (strcmp(key, "enable") == 0) {
            const char *save = p;
            t = uc_next(&p, &ts, &te);
            if (t == UC_LBRACK) {
                char name[128];
                while ((t = uc_next(&p, &ts, &te)) == UC_ATOM) {
                    if (!uc_try_enable(ts, te, name, sizeof(name))) {
                        fprintf(stderr,
                                "error [TUR-E0310]: unknown experiment '%s' in "
                                "%s :enable; run 'tur experiments' for the list\n",
                                name, path);
                        free(src);
                        exit(2);
                    }
                }
                /* t is UC_RBRACK or UC_EOF -- either way the list is done. */
            } else if (t == UC_ATOM) {
                /* Bare `:enable name` (no vector) -- accept the single name. */
                char name[128];
                if (!uc_try_enable(ts, te, name, sizeof(name))) {
                    fprintf(stderr,
                            "error [TUR-E0310]: unknown experiment '%s' in "
                            "%s :enable; run 'tur experiments' for the list\n",
                            name, path);
                    free(src);
                    exit(2);
                }
            } else {
                p = save;  /* nothing followed -- let the outer loop resync */
            }
        } else {
            /* Unknown key: warn (TUR-W0062) and skip its value so we resync. */
            fprintf(stderr,
                    "warning [TUR-W0062]: unknown key ':%s' in %s\n", key, path);
            const char *save = p;
            t = uc_next(&p, &ts, &te);
            if (t == UC_LBRACK) {
                int depth = 1;
                while (depth > 0) {
                    t = uc_next(&p, &ts, &te);
                    if (t == UC_EOF) break;
                    if (t == UC_LBRACK) depth++;
                    else if (t == UC_RBRACK) depth--;
                }
            } else if (t == UC_RBRACK || t == UC_EOF) {
                p = save;  /* not our value -- resync at the outer loop */
            }
            /* else: a single-atom value, already consumed. */
        }
    }
    free(src);
    return true;
}
