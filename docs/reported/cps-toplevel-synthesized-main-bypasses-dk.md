# Top-level `(handle ...)` in a synthesized main bypasses the CPS/DK backend (THE endgame taint root)

**STATUS: PARTIALLY LANDED (Approach A, gated on `--enable=cps-tramp-resume`).**
`elaborate_program` (src/compiler/elab_toplevel.c) now folds trailing top-level
STATEMENT forms into a synthesized `(defn main [] : int (do <stmts> 0))` so a
top-level handler flows through `fn_is_d2b_main` + `emit_cps_ir` like a user main.
Conservative + macro-safe (fires only when there is no user `main` and every
user-region form is a def*/directive or a plain non-macro call; any macro / `do`
/ ambiguous head aborts the fold). **Flag-off is byte-identical** (default suite
2202/0) -- the fold is gated because flag-off the base CPS subset is narrower and
the N6.5 fallback is retired, so a synthesized d2b main leaving the subset would
hard-error rather than fiber. **Flag-on payoff: real fiber-live fixtures 39 -> 27**
(12 effect-* fixtures now DK-lower: effect-handler/-nested*/-multiple/-oneshot/
-perform-handle/-resume-value/-declaration/-console/-defer/-rc/-capture-k*/
linear-effect-handler; `effect-handler` emits `compute__cps`, zero `eff=1`, output
`104`).

**Fold is now CONSERVATIVE -- zero flag-on regressions.** Two top-level-handle
shapes the DK backend cannot lower yet were routed onto the DK by the initial fold
and MISCOMPILED flag-on; both are pre-existing DK gaps the fold merely EXPOSED, not
caused. `fold_stmt_is_risky` (elab_toplevel.c) now leaves them on the historical
fiber path (a statement whose handle subtree has >=2 handle heads, or a `set!`
alongside a handle, is not folded):

- `effect-capture-k` -- ESCAPING MUTABLE: a `set!` writes the captured continuation
  `k` into an outer `^mut`, resumed AFTER the handle exits. The DK backend has no
  by-reference mutable capture (`collect_caps` walks only a `set!`'s VALUE, not its
  TARGET, and captures are copied in by value -- src/compiler/emit_cps_ir.c:2872,
  4636), so the `k` store emits as a bare local write in the lifted handler case
  (`k_hystore_* undeclared`). Fixing it is a real DK feature: heap-cell (by-ref)
  capture of a mutable shared across lifted continuation/handler frames.
- `effect-nested` -- NESTED HANDLE IN VALUE POSITION: an inner handle whose result
  feeds arithmetic (`(+ (get-val) (handle ...))`), so its continuation needs
  `__kont` threaded from the sub-continuation (`__kont undeclared`); the
  handler-case-in-handler-continuation `__kont` threading gap
  (cps-handler-case-effect-reopening-needs-emission.md family).

  **Precise diagnosis (2026):** the OUTER handle's heap-join frame (`main_j1`,
  emitted by `emit_heap_join`, emit_cps_ir.c) reifies `(+ __t2 <inner-handle>)`.
  Its body INSTALLS the inner handle, whose own continuation frame captures the
  enclosing continuation (`main_hk2_env->__k = __kont`) -- but `main_j1` is emitted
  as an `LH_PERFORM_CONT` DKFrame `(env, value)` with no `__kont`, so `__kont` is
  undeclared.  `needs_kont` (= `jbody_has_cps_tailcall || jbody_has_perform`) does
  not detect a nested handle/reset in the jbody.  ATTEMPTED FIX + INSUFFICIENT
  (reverted): adding `jbody_has_delim` (a nested `CT_HANDLE`/`CT_RESET` in the
  jbody) to `needs_kont` promotes `main_j1` to an `LH_RESUME_CONT` resume-frame so
  it COMPILES -- but the program then HANGS (infinite loop) at run time.  So the
  value-position nested handle needs more than threading `__kont` into the join:
  the resume-frame's downstream-chain semantics for a nested handle whose result
  is consumed by an outer expression are wrong (the inner handle's re-install /
  `dk_copy_enclosing_handlers` loops).  A real fix must get the nested handle's
  continuation to deliver its value to the join's `__kont` exactly once, not
  re-enter the enclosing handler chain.

**Refinement (later):** the blanket `n_handle >= 2` rejection is now narrowed to
only a nested handle in a VALUE position (`fold_handle_in_value_position`).  A
cleanly STACKED nested handle -- each inner handle the direct body/case of its
enclosing handle, `(handle (handle ...) ...)` -- DK-lowers fine, so it now folds:
`effect-console` (`(handle (handle (echo-doubled) (Write ..)) (Read ..))`) DK-
lowers (zero `eff=1`, output `result:` / `42`).  `effect-nested` stays on the
fiber (its handle is a `+` operand) with correct output `52`.  Flag-off byte-
identical; flag-on sweep clean.

Net after the guard: **10 effect fixtures fold to the DK, zero flag-on
regressions** (flag-off byte-identical 2202/0; full flag-on build sweep clean).
Remaining SIG-TAINT is the effect-poly/-row/-subtype family (E2b /
effect-subtype-capability adjacent), the permanent session/export carve-outs, and
the 4 non-permanent CPS roots. Follow-ons to graduate the fold: (1) by-reference
mutable capture in the DK backend (unblocks the `set!`-in-handle shape); (2)
nested-handle `__kont` threading; then broaden the base subset / retire N6.5 so the
fold can go always-on, drop the `--enable` gate, and regen the top-level-expr
snapshots.

---

**Severity:** HIGH for the endgame (the single largest lever). Correctness is
fine -- these programs run correctly on the fiber. But this one root is why
**~33 ordinary effect fixtures still ride the fiber effect runtime flag-on**
(`effect-handler`, `effect-nested`, `effect-multiple`, `effect-poly-*`,
`effect-subtype-*`, `effect-perform-handle`, `effect-resume-value`, ...). Clearing
it collapses almost the entire remaining `SIG-TAINT` bucket in one move.

## The finding (pinned, reproduced)

A program whose effect handler sits in a **top-level expression** (the idiomatic
`(println (handle (compute) ...))` with no `(defn main ...)`) keeps its performer
on the fiber; the byte-identical program written as an explicit `(defn main [] ...)`
DK-lowers fully.

```turmeric
;; p4 -- TOP-LEVEL expr form (== tests/fixtures/effect-handler): compute SIG-TAINT, 3 fiber-performs
(defeffect Add [x :int] :int)
(defeffect Mul [x :int] :int)
(defn compute [] : int (* (perform (Add 3)) (perform (Mul 4))))
(println (handle (compute)
  (Add [x] k) (resume k (+ x 10))
  (Mul [x] k) (resume k (* x 2))))

;; p3 -- SAME code inside (defn main ...): compute AND main both emit __cps, zero eff=1, zero fiber-performs
(defn main [] : int
  (do (println (handle (compute) (Add [x] k) (resume k (+ x 10))
                                 (Mul [x] k) (resume k (* x 2)))) 0))
```

Measured (`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume`):
- **p3 / defn-main:** `--dump-cps` shows `cps-fn compute` and `cps-fn main` (both DK);
  zero `eff=1`; zero `tur_effect_perform(` in emitted C.
- **p4 / top-level:** `SIG-TAINT eff=1 compute`; three `tur_effect_perform(` calls;
  `main` installs the handler via the **fiber** `EffectHandlerFrame` path, not `dk_handler`.

## Root cause (architectural)

Top-level non-`defn` forms are stored in `EX_PROGRAM.items` **as bare exprs**
(`elab_toplevel.c:1711`) -- there is no synthesized `main` `FnDef`. Two consequences:

1. **They never reach the CPS classifier.** `emit_cps_ir`'s classification loop
   (`emit_cps_ir.c:3618`) iterates `program.items` and only classifies
   `EX_FN_DEF` nodes. Bare top-level exprs are skipped, so the `handle` in
   `(println (handle ...))` is never a d2b candidate and never DK-lowers.
2. **They are direct/fiber-emitted into a synthesized `int main()`.** When
   `!user_has_main`, `emit_module.c:10606` writes the top-level statements'
   already-direct-emitted `body` inline into `int main(argc, argv)`. That `body`
   contains the fiber `handle` lowering (`EffectHandlerFrame` +
   `tur_effect_perform`).

Because the top-level handler is fiber-emitted, its effect is `base_taint`
(`emit_cps_ir.c:3610` -- "effects performed/handled by any top-level code that is
NEVER CPS-emitted"). That taint seeds the fixpoint: every performer of that effect
(`compute`, and transitively the whole family) is dropped from `S` and reported
`SIG-TAINT`. An explicit `(defn main ...)` avoids all of this: it is a real
`FnDef`, `fn_is_main && fn_is_d2b_main` (E3', `emit_cps_ir.c:2520` -- a main that
installs a `handle` is a d2b candidate), so it enters classification and DK-lowers,
and its handled effect never taints.

**In one line:** the CPS/DK effect lowering only applies to `handle`s that live in
a `FnDef` body. The idiomatic top-level handler is not in a `FnDef`, so it -- and
everything whose effect it handles -- stays on the fiber.

## Why this is THE lever

The `SIG-TAINT` bucket (measured at HEAD after PR #684 + the owning-field fix: 39
real fiber-live fixtures, of which ~33 are `SIG-TAINT`) is dominated by exactly
this shape: a plain effect fixture with its handler in a top-level
`(println (handle ...))` or bare top-level `(handle ...)`. None of them have a
separate root -- each is tainted by its OWN top-level handler. So this is not 33
independent slices; it is ONE root. Clearing it should DK-lower the whole family
and shrink the fiber-live set to the genuine remainder (the 4 non-permanent CPS
roots + the permanent session/export carve-outs).

## Fix direction

**Approach A (correct, recommended): synthesize a `main` `FnDef` from trailing
top-level statements.** When `!user_has_main` and there is at least one non-`defn`
top-level statement, wrap them into `(defn main [] : int (do <stmts...> 0))` as a
real elaborated+colored `FnDef` and add it to `program.items` (or a dedicated slot
the emitter consumes). Then:
- `emit_cps_ir` classifies it exactly like a user main -- `fn_is_d2b_main` already
  admits a handle-installing zero-arg main (E3'), so it DK-lowers with the existing
  `int main -> main__cps` trampoline (`emit_cps_ir.c:5240`).
- `emit_module.c`'s `!user_has_main` synthesized-main branch is suppressed (the
  FnDef-main path emits `int main`), so no double-emit.
- Preserve the current prologue ordering the synthesized branch owns: WIN binary
  stdio, `*args*` cons-list build, `def_init_body` (Gap F def initializers run
  before the statements), panic-trace init. These already have a shared helper
  (`emit_fns.c:3362` notes the user-main / synthesized-main / D2b-wrapper paths
  call the same prologue helper), so the FnDef-main path should reuse them.

**Scope / risk (why this needs a deliberate slice, not a drive-by):**
- **Wide snapshot churn.** Every fixture with top-level statements and no `defn main`
  gets a new `int main` shape (the `main__cps` trampoline + the DK-lowered body).
  That is a large `expected.c` regen -- coordinate it (CLAUDE.md fixture-churn rule).
- **Coloring the synthesized main.** The wrapped statements must be run through the
  same coloring/elaboration a `defn` body gets, so `cps_colored` and effect rows are
  set; a naive AST wrap without coloring will mis-classify.
- **Ordering + `def_init` semantics.** Gap F (top-level `(def x init)` initializers
  run before statements) and `*args*` must land in the same order inside `main__cps`.
  When a user main IS present, top-level statements are already dropped into a
  `__constructor__` (`emit_module.c:10630`) -- that path is unchanged (this fix only
  touches the `!user_has_main` case).
- **Fiber-reaching guard already exists.** If a synthesized main's handle subtree
  reaches genuinely fiber-only effect code (e.g. an inline-C performer), the E3'
  fixpoint drops it from `S` and it falls back to the direct/fiber main -- exactly
  today's behaviour, no regression. So the change is safe-by-construction: it can
  only MOVE a top-level handler onto the DK when the whole subtree is admissible.

**Approach B (rejected):** special-case the emitter to DK-lower a top-level handle
in place without a FnDef. This duplicates the d2b-main machinery for the top-level
context and does not give the classifier a binding to reason about -- strictly worse
than A.

## Verification

- Repro: the p3/p4 pair above (or `tests/fixtures/effect-handler` vs. the same code
  wrapped in `defn main`). Target: p4 emits `compute__cps` + `main__cps`, zero
  `eff=1`, zero `tur_effect_perform(`.
- After the fix, re-run the flag-on `eff=1` sweep: the `SIG-TAINT` effect-family
  should collapse to zero (or name the residual, which is then a genuine second
  root). Default suite (`bash tests/run.sh`, 12-min timeout) green; regen the
  top-level-expr `expected.c` snapshots in the same change.
- Guardrail: byte-identical program OUTPUT for every moved fixture (direct-vs-DK
  equivalence); e.g. `effect-handler` still prints `104`.

## Context

The highest-leverage item in docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md
Sec 4/7. It is the concrete mechanism behind the plan's "SIG-TAINT dissolves"
(E6) -- but E6 assumed the only sig_perm seeds were export/main-reject/inline-C.
This is a fourth seed the plan under-counted: the **synthesized main** is an
un-classified fiber handler. E3' fixed the *explicit* `defn main`; the synthesized
main is the same fix applied one level up, at program elaboration.
