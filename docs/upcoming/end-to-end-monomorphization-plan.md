---
title: End-to-End Monomorphization -- Remaining Work
category: Planning -- ABI / Codegen rework
description: Successor to the archived 2026-06-13 plan (`docs/archive/end-to-end-monomorphization-plan.md`). Lists ONLY the work that has not landed, in actionable phases. Each phase has a checklist; each item names the file(s) to touch and the validation that closes it.
---

# End-to-End Monomorphization -- Remaining Work

**Snapshot:** 2026-06-19.
**Predecessor:** [`docs/archive/end-to-end-monomorphization-plan.md`](../archive/end-to-end-monomorphization-plan.md)
(rationale, non-goals, why-monomorphization framing -- keep there).
**Predecessor scope audit:** [`docs/archive/m5-scope-audit-2026-06-18.md`](../archive/m5-scope-audit-2026-06-18.md).

## What's done (one-liners; full detail in the archive)

- **M1 audit** -- shipped (`docs/monomorphization-audit.md`).
- **M2 polymorphic constructors** -- `#{Construct}` lowering for
  `ok`/`err`/`some`/`none` shipped; `option-map`/`option-eq?`/`some?`
  retyped to by-value (PRs #421, #426, #430, #431).
- **M3 polymorphic accessors** -- down-scoped: M3 bucket A'/B kept as
  by-design carrier-bridge regression coverage; bucket C is what Phase 1
  below finishes.
- **M4 non-HKT typeclass per-method ABI** -- M4a + M4c Path A shipped
  (PRs #399-#419).
- **M5 constrained-poly HOFs** -- shipped (PRs #427, #428). Verified
  2026-06-19: `tests/fixtures/poly-hof-constrained-arg-spec/` runs
  green; constrained-poly value through a polymorphic HOF dispatches
  to the per-`A` instance.

## What remains (this plan)

Five phases. **Phases 2-5 are sequential** (HKT design -> HKT impl ->
carrier-helper rewrites -> bridge delete -> re-audit).

**Status (2026-06-18):** Phase 1 was assumed fully independent of the rest;
that holds for 1.1 + 1.2 (both **landed** -- by-value `ne-from?` via a typed
`(List A)` witness, and by-value `result-map` with the N-arg `#{Construct}`
spec-selection fix). It does **not** hold for **1.3 + 1.4**: 1.4 (kleisli) is
root-caused as gated on the **HKT class dispatch** work (Phases 2-3) -- the
`Category` class is kind-`*`, so the arrow's element types are phantom and `B`
cannot be witnessed in `comp` without an HKT arrow category. 1.3 (`unwrap-or`)
has exactly one stdlib caller (the blocked kleisli `comp`), so it is gated
behind 1.4. 1.5 (bucket C exit criterion) cannot close until 1.3/1.4 do.
Net dependency order is now: 1.1 ✓, 1.2 ✓, then Phases 2-3 (HKT), then
1.4 -> 1.3 -> 1.5. See per-item notes below.

---

## Phase 1 -- Track A bucket C: finish the stdlib `Option`/`Result` consumer retypes

Independent of HKT work. Closes the per-fixture audit-floor residual
tracked in `option-consumer-retype-byvalue` and unblocks downstream
spice helpers from leaning on carrier `:int` returns.

**Umbrella status report:**
[`docs/reported/option-consumer-retype-byvalue.md`](../reported/option-consumer-retype-byvalue.md)
tracks running state across all four sub-items.

**Per-item authoritative specs** (each is the spec, this plan is the
schedule):
- 1.1 -> [`ne-from-byvalue-option-nonempty-element-type-uninferable`](../reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md)
- 1.2 (`result-map`) -- no separate spec; one-PR retype, scoped here
- 1.3 -> [`unwrap-or-byvalue-cascade`](../reported/unwrap-or-byvalue-cascade.md)
- 1.4 -> [`kleisli-byvalue-option-cascade`](../reported/kleisli-byvalue-option-cascade.md)

When updating execution detail for 1.1/1.3/1.4, edit the linked spec;
this plan owns the *ordering* and the *exit criterion*, not the per-PR
mechanics.

### 1.1 -- Introduce typed `List` for the NonEmpty step-4 cascade

Per
[`docs/reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md`](../reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md)
(authoritative spec).
`ne-from?`'s `xs : int` argument is what makes `A` uninferable; the fix
is to give it a typed list to recover `A` from.

- [x] Add `(defopaque List [A] :int)` and a `list-of [A] [& xs : A] : (List A)`
      smart constructor -- landed in a NEW `stdlib/list-typed.tur` (NOT
      auto-loaded; `List` collides with user `(defdata List ...)` -- see
      `tests/fixtures/adt-recursive/`). `refined.tur` loads it explicitly.
      Required a compiler fix: variadic `[A] [& xs : A] : (List A)` now
      substitutes the inferred `A` into the result type (`elab_call.c`,
      `elab_fns.c`).
- [x] Retype the NonEmpty surface in `stdlib/refined.tur`:
      `ne-from? [A] [xs : (List A)] : (Option (NonEmpty A))`,
      `ne-unwrap [A] [o : (Option (NonEmpty A))] : (NonEmpty A)`, pure-Turmeric.
      (`ne-head`/`ne-tail`/`ne->list` already typed on `(NonEmpty A)`.)
      Also fixed a `clone_struct_app_type` compiler leak at `emit_expr.c:4604`
      this path exposed.
- [x] Update `tests/fixtures/refined-nonempty/` to drop
      `(:: o (Option int))` ascriptions; `(ne-head ...)` resolves the element
      type without annotation.
- [x] **Validation:** `bash tests/run.sh` -- 1683 passed, 0 failed.
- [x] **Validation:** `TUR_M3_AUDIT=1` on the fixture shows zero crossings.
- [x] Archive `ne-from-byvalue-option-nonempty-element-type-uninferable.md`
      with resolution note (now in `docs/archive/`).
- [ ] **Deferred (separate report):** the `refined-nonempty-typed-list` float
      fixture proving `A = float` through `ne-head` without truncation is
      blocked on a pre-existing polymorphic float<->carrier ascription
      miscompile (also breaks `(ne-head (ne-singleton 1.5))`), tracked in
      `docs/reported/polymorphic-float-carrier-ascription-value-cast.md`. The
      variadic-collector half of that round-trip IS fixed
      (`tests/fixtures/variadic-float-cons-collect/`).

### 1.2 -- Retype `result-map` and decouple from its regression fixture

The current `:int` signature is backed by a deliberate carrier-ABI
regression test. The retype has to land alongside a *new* fixture that
keeps the carrier path covered explicitly (so removing the implicit
coverage from `result-map` is safe).

- [x] Identify the fixture(s) whose pass depends on `result-map`'s
      current `:int` signature -- `tests/fixtures/typed-slots/coerce-carrier-to-struct/`
      is the carrier-ABI regression (bare `:int` carrier -> `result-map` ->
      `(:: ... (Result int int))`); `tests/fixtures/typed/result-basic/` and
      `tests/fixtures/result-combinators/` (local `u-result-map`) also touch it.
- [x] Decoupled `coerce-carrier-to-struct` from `result-map`: it now uses a
      dedicated inline-C carrier producer (NOTE in-file), so the carrier->struct
      `::` bridge (KB-004) stays covered independently of `result-map`.
- [x] Retyped `result-map` to
      `[A B E] [r : (Result A E) ^fat f : (fn [A] B)] : (Result B E)`,
      pure-Turmeric. Fixed the construct-spec leak it surfaced: extended the
      0-arg `#{Construct}` spec-selection guard to N-arg construct callees in
      `emit_call_name` / `find_matched_abi_spec` / `abi_trace_clone_name`
      (a constructor's by-value spec vs. its carrier base differ only in return
      ABI, so the structural by-args match cannot pick between them -- they now
      resolve only from the per-`Expr*` recording).
- [x] **Validation:** `bash tests/run.sh` -- 1683 passed, 0 failed (85 stdlib
      snapshots regenerated in-PR). `result-map` struck from the "Remaining"
      list; report archived to
      [`docs/archive/result-map-byvalue-construct-spec-leak.md`](../archive/result-map-byvalue-construct-spec-leak.md).

### 1.3 -- Retype `unwrap-or` (the ~10-module cascade)

Per
[`docs/reported/unwrap-or-byvalue-cascade.md`](../reported/unwrap-or-byvalue-cascade.md)
(authoritative spec; one-PR-per-row migration via temporary
`unwrap-or-carrier` shim). Cascade spans ~10 stdlib modules
(`zipper`, `seq/*`, `json`, `safe`, `env`, `serial`, ...).

- [x] **Pre-task (2026-06-18):** empirical grep finds **one** stdlib call site
      (`stdlib/kleisli.tur:86`), not ~10 modules. The `zipper`/`seq`/`json`/...
      table is not borne out by the current tree. That sole stdlib caller is the
      kleisli `comp` body, which step 5 (1.4) *removes*. The remaining consumers
      are test fixtures that intentionally pass a carrier-int Option
      (`option-consumers-byvalue-arg`, `zipper-basic`, `kleisli-arrow-instance`).
- [ ] **GATED behind 1.4 + fixture-producer retypes.** Retyping `unwrap-or` to
      by-value makes `(unwrap-or <carrier-int> 0)` a type error at every fixture
      site whose producer is still a carrier-int Option; those cannot all flip
      to a shim without changing what they test. The stdlib cascade is empty
      once 1.4 lands (which is itself blocked -- see above). Finding recorded in
      [`unwrap-or-byvalue-cascade.md`](../reported/unwrap-or-byvalue-cascade.md).
- [ ] Retype `unwrap-or` in `stdlib/option.tur` to
      `[A] [o : (Option A) dflt : A] : A` with a pure-Turmeric body:
      `(if (.is-some o) (.value o) dflt)`.
- [ ] Walk every call site from the pre-task; for each, either:
      - resolve `A` via the surrounding context (preferred), or
      - retype the carrier-int Option producer that feeds this call to
        return `(Option A)` (cascades into the producer module).
- [ ] Regenerate any affected `tests/fixtures/*/expected.c` snapshots
      in this same PR (per CLAUDE.md fixture rule -- no follow-up).
- [ ] **Validation:** `bash tests/run.sh` clean.
- [ ] Strike `unwrap-or` from the "Remaining" list in
      `option-consumer-retype-byvalue.md`.

### 1.4 -- Retype `kleisli.tur` `comp` / `k-apply-raw` (step 5)

Per
[`docs/reported/kleisli-byvalue-option-cascade.md`](../reported/kleisli-byvalue-option-cascade.md)
(authoritative spec). Self-contained one-PR retype; **NOT blocked on 1.3**
per the spec doc -- the only external dependency is that
`TUR_APPLY1` already passes by-value Options correctly, which it does.
The `(:: r (Option int))` ascription that landed in PR #426 was
explicitly an interim patch this phase retires.

- [ ] **GATED ON PHASES 2-3 (HKT).** Definitive root cause:
      `(defclass Category [arr] ...)` (`stdlib/arrow.tur:303`) is a kind-`*`
      class, so `Category [Kleisli]` binds `arr = Kleisli` (the bare int64
      carrier) and the arrow's element types `A`/`B`/`C` are **phantom at the
      class level** -- `comp [f g]`'s params and result are all just `arr`, so
      `B` is not a free tyvar of the method and cannot be witnessed. Recovering
      it requires an HKT arrow category (`arr : * -> * -> *`), i.e. the M6/M7
      work of Phases 2-3 below -- this item is NOT independent of them, contrary
      to the original framing. The spec's prerequisite-1 gate anticipated this.
      Verified the secondary inline-C-carrier-vs-by-value-return mismatch too.
      Tracked in
      [`kleisli-k-apply-raw-B-uninferable.md`](../reported/kleisli-k-apply-raw-B-uninferable.md);
      the `(:: r (Option int))` interim bridge (PR #426) stays until HKT lands.

### 1.5 -- Bucket C exit criterion

**GATED:** cannot close until 1.3 + 1.4 land, which are themselves gated on
Phases 2-3 (HKT). The kleisli `(:: r (Option int))` bridge and the carrier
`unwrap-or` are the remaining bucket-C contributions; both clear only with the
HKT arrow category. Re-run this sweep after Phase 3.

- [ ] Re-run `TUR_M3_AUDIT=1` per-fixture sweep (the methodology is
      pinned in `docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
      "New baseline" section). The crossing count must be <= the current
      floor; bucket C contributions must be zero.
- [ ] Update `docs/upcoming/m4-typeclass-per-method-abi-plan.md`'s audit
      floor figure with the new measurement, or archive that plan if
      bucket C is its only remaining open item.

---

## Phase 2 -- M6: HKT class dispatch design pass

**Why this is gated:** the current HKT surface is broad -- 9 HKT
classes in `stdlib/typeclass*.tur` (Functor, Applicative, Monad,
Bifunctor, Foldable, Traversable, Alternative, MonadError, Comonad),
~15 `definstance` lines across Parser, Option, Result/Either, Goal,
Backtrack, Schema, rc, and ~199 fmap/bind/pure/lift2 grep hits across
stdlib. Per-instantiation expansion blast radius is not obvious; the
design pass picks the model before the implementation phase commits.

### 2.1 -- Measure HKT call-graph cost per option

**Partial measurement (2026-06-18, stdlib-only -- `../turmeric-spices/` is NOT
checked out in this environment, so the spices half is still TODO):**

- **9 HKT classes** confirmed: Functor, Applicative, Monad, Foldable,
  Traversable, Alternative, Bifunctor, Comonad, MonadError (`stdlib/typeclass*.tur`,
  `stdlib/comonad.tur`). All declare their methods with the int64-carrier
  result (`... : int`) and an HKT type param (`[^f]` / `[^m]` / `[^^f]`).
- **35 HKT `definstance` lines** across: Functor [Option/Result/Either/Parser/
  Goal/Backtrack/Schema/Cons/rc], Applicative + Monad + Alternative
  [Option/Parser/Goal/Backtrack(+Schema for Applicative/Alternative)],
  Bifunctor [Result], Foldable [rc], MonadError [Result], Comonad
  [identity/pair].
- **43** `fmap`/`bind`/`pure`/`lift2`/`ap`/`>>=`/`traverse`/... call sites in
  stdlib (the plan's "~199" figure must have counted `../turmeric-spices/`).

Still TODO for a complete option-1 estimate: the per-`(combinator, f, A)`
tuple enumeration + distinct-clone count needs (a) the spices checkout and
(b) a clone-counting probe in the elaborator (no such tooling exists yet).
This partial inventory bounds the *instance* surface (35) but not the
*monomorphization* blast radius (the `A` dimension), which is the figure that
actually decides option 1 vs 2.

- [ ] Build the per-`(f, A)` monomorphization estimate for option 1
      (full per-(f, A) clone): enumerate every `(combinator, f, A)`
      tuple reachable from stdlib + `../turmeric-spices/spices/` and
      count distinct clones the elaborator would emit. (Blocked on the
      spices checkout + a clone-count probe; stdlib instance surface = 35,
      measured above.)
- [x] Compared option 2 (dict-passing w/ payload erasure) and option 3
      (source-rewriting inline-expansion) against option 1 on generated-C delta
      and expressiveness regression -- see the tradeoff doc. Key finding:
      option 2 **reintroduces the int64 carrier** at the HKT boundary (the very
      thing Phases 4-5 delete), and option 3 regresses polymorphic/recursive
      instances (Parser/Goal/Backtrack).
- [x] Deliverable landed: [`docs/upcoming/v2/hkt-dispatch-options-tradeoff.md`](v2/hkt-dispatch-options-tradeoff.md)
      with measured numbers (9 classes, 30 instances, ~21 genuine dispatch call
      sites, element-type set ~={int,+few}; option-1 clone-count bound = low
      tens).

### 2.2 -- Pick a model and pin acceptance criteria

- [x] **Chose option 1** (full per-`(f, A)` clone). Reasoning in the tradeoff
      doc: it is the only option that achieves the plan's carrier-retirement
      goal (option 2 reintroduces the carrier; option 3 regresses recursion),
      and the measured blast radius (low tens of clones) is well under the
      "default to option 1 unless > 2x larger" gate.
- [x] Acceptance criteria pinned in the tradeoff doc (zero source-side changes
      at the ~21 call sites; HKT method results stop returning the int64
      carrier; nested `(fmap (fmap f) x)` reaches all `(f, A)` monos without a
      worklist cycle; suite + spice suites + `TUR_M3_AUDIT` clean).
- [ ] **Open before Phase 3:** build the elaborator clone-count probe to turn
      the "low tens" bound into an exact per-`(f, A)` figure and re-measure
      against the then-current spices tree (needed to *size* Phase 3, not to
      *choose* the model -- the choice is settled).

### 2.3 -- Phase 2 done when

- [x] Tradeoff doc landed; option 1 chosen and reasoned; acceptance criteria
      pinned. The only open item (clone-count probe) is a *sizing* input for
      Phase 3, not a model-changing question -- Phase 2's design decision is
      final. **Phase 2 design pass: DONE.**

---

## Phase 3 -- M7: HKT class dispatch implementation

**Largest single phase.** Implement whatever Phase 2 picked (option 1, per
`v2/hkt-dispatch-options-tradeoff.md`). Tasks below are skeleton; flesh out
per Phase 2's design doc.

### 3.0 -- THE core prerequisite (grounded 2026-06-18): thread the element type into HKT instance methods

Empirically pinned by three probes, all bottoming out on the **same gap**:

1. **Kleisli `comp`** -- `(defclass Category [arr])` is kind-`*`, so `arr =
   Kleisli` and the arrow's `A`/`B`/`C` are phantom; `comp [f g]` has no `B`.
2. **`fmap` class method** -- `(defclass Functor [^f] (fmap [container [fn :fn]]
   : int))` returns the int64 carrier and the method does not bind the element
   tyvar.
3. **Rewriting `Functor [Option]` `fmap` to pure-Turmeric** (delegating to the
   already-by-value `option-map`) **fails to typecheck**: inside the instance
   method `container` arrives as the bare `Option`, not `(Option A)`, so
   `(option-map container fn)` errors `expected (type-app Option tyvar 'A'),
   got Option`. The element type `A` is simply not in scope in the method body.

This is why **every** HKT instance body in stdlib is hand-rolled inline-C over
the raw carrier struct (`struct { bool is_some; int64_t value; } *o = ...;
return tur_some(...)`): the inline-C sidesteps the missing element type. It
also means **Phase 3 and Phase 4 are NOT sequentially independent** -- you
cannot rewrite an HKT instance body to a by-value pure-Turmeric form (Phase
4.2) until the method *receives* the element type, and you cannot make dispatch
by-value (Phase 3) until the bodies are by-value. Both depend on this one
type-system change.

**Elaborator path PROTOTYPED end-to-end + emit gap pinpointed (2026-06-18).**
A standalone probe (custom `(defclass MyFunctor [^g] (gmap [container : (g a)
f : (fn [a] b)] : (g b)))` + `definstance MyFunctor [Option]` with a
pure-Turmeric by-value body `(if (some? container) (some (f (.value container)))
(none))`) drove the full implementation. Findings (the prototype is **reverted**
-- see "Why reverted" below -- but every step was validated):

  - The class-decl parser **already accepts** HKT-applied method signatures
    `(g a)` / `(g b)` -- no parser change needed.
  - The instance body elaborates with `container : (g a)` and works by-value
    (some?/.value/`(some ...)`/`(none)` all type-check).
  - **The elaborator can be made to resolve the result type end-to-end with NO
    ascription.** Three coordinated changes did it (all prototyped, working):
    1. `tc_subst_class_params` (recurse into `TY_APP`) at the instance-return
       substitution site -- `(g b)` -> `(Option b)`.
    2. Carry a `TY_APP` return through `result_full_type` (the existing code
       set it only for `TY_FN` / ground struct/ADT; added a `TY_APP` branch at
       `elab_typeclasses.c:2974`).
    3. At the call site (`elab_method_call`, `elab_typeclasses.c:4517`) handle a
       `TY_APP` `rft` by collecting element-tyvar bindings (unify the method's
       declared param types `(g a)` / `(fn [a] b)` against the actual arg types
       `(Option int)` / `(fn [int] int)`) and substituting into the result so
       `(g b)` -> `(Option int)`. With this, `(unwrap (gmap (some 21) dbl))`
       typechecks with no ascription.

**Why reverted (the two reasons it can't land in isolation):**
  1. **Emit-side by-value HKT dispatch is missing (Phase 3.2/3.3).** With the
     type resolved to by-value `(Option int)`, the probe *compiles* but prints
     **0, not 42**: the instance method `__inst_MyFunctor_gmap_Option` still
     emits the int64 carrier (`malloc(int64)`), while the now-by-value consumer
     (`unwrap`) dereferences it as `Option*` -- a carrier-vs-by-value mismatch
     (same family as the result-map spec-leak fixed in 1.2, but at the HKT
     method boundary). The instance method must emit a by-value `(Option b)`
     return per `(f, A)` -- that is Phase 3.2 and it does not yet exist.
  2. **It regresses 4 existing parameterized-result typeclass fixtures.** The
     elaborator changes are NOT inert: `decode-bool-carrier-instance-ascription`,
     `instance-method-return-carrier-bridge`,
     `typeclass-method-parameterized-result-decode`, and
     `typeclass-return-dispatch-result-wrapped` all `build failed` with the
     prototype, because existing decode / return-dispatch methods share the
     changed `result_full_type` / call-site paths. Landing 3.0 requires
     reconciling those simultaneously.

So Phase 3 is confirmed as the "largest single phase": the type-threading is
*implementable* (prototyped + validated above), but landing it is a coordinated
change across (a) the elaborator (the 3 steps above), (b) the emit-side
by-value HKT dispatch (3.2/3.3), and (c) reconciling the 4 existing
parameterized-result fixtures -- a multi-session effort, not a single safe
increment. The prototype lives in the session transcript; re-create from the 3
steps above.

After the elaborator + emit land, the original plan item below is the follow-on:
- [ ] An HKT class method must bind the element parameter(s) of its `[^f]` /
      `[^^f]` type constructor and give each method-body parameter the *applied*
      type (`container : (f A)`), with `A` a fresh method-level tyvar that
      monomorphizes per call -- now mostly proven above; the remaining work is
      the call-site inference + migrating the 9 stdlib HKT classes' decls and
      30 instance bodies to the by-value form (each a small rewrite like the
      probe's `gmap`). Precise mechanism traced 2026-06-18:
        - `elab_typeclasses.c` (~line 674) types instance-method params with a
          hard `param_types[actual_p] = TYPE_INT;` under the comment "Default to
          int for now -- type inference for method params deferred". That carrier
          default is exactly the element erasure.
        - The class decls do not express the applied type either:
          `(defclass Functor [^f] (fmap [container [fn :fn]] : int))` has
          `container` untyped (-> int) and `: int` return. There is no `(f a)`
          anywhere to substitute. So the feature needs BOTH (a) class-decl
          syntax for HKT-applied method param/return types (`container : (f a)`,
          `: (f b)`) and (b) elaborator support to substitute the instance's `f`
          and bind `a`/`b` as fresh per-call tyvars that monomorphize.
      This is a cross-cutting type-system change touching the class decls of all
      9 HKT classes, the 30 instances, and the per-method dispatch -- not a
      codegen tweak. Until it lands, the inline-C carrier bodies are
      load-bearing and cannot be rewritten (Phase 4.2 HKT subset is gated here).
- [ ] Once methods carry `(f A)`, rewrite the inline-C HKT instance bodies to
      pure-Turmeric by-value (this is Phase 4.2 for the HKT helpers, unblocked
      by 3.0) and let option-1 per-`(f, A)` monomorphization (3.2) clone them.

**Not attempted in-session beyond the probes above:** 3.0 is a type-system
change of unknown blast radius across all 9 classes / 30 instances and the
M4 per-method dispatch; it is the genuine multi-session core of M7. The
probes that established this are reverted (suite stays 1683/0).

### 3.1 -- Dict generation for HKT instances

- [ ] Extend `emit_typeclasses.c` to emit HKT instance dicts per the
      chosen ABI. The non-HKT dispatch (M4) stays as-is.
- [ ] Update `__inst_<Class>_<method>` symbol naming if needed to
      disambiguate per-`(f, A)` clones.
- [ ] Per-instance test fixture under `tests/fixtures/hkt-<class>-<f>/`
      pinning the emitted dispatch C.

### 3.2 -- Polymorphic combinator monomorphization

- [ ] Extend `find_matched_abi_spec` / `emit_abi_intern_spec` to fire
      on HKT-constrained polymorphic defns (`(defn lift2 [^f] [^&: Functor f] ...)`).
- [ ] Per-call-site clone emission for each `(f, A, B, ...)` tuple
      reachable.
- [ ] Worklist correctness: nested HKT combinators (`(fmap (fmap f) x)`)
      must reach all needed monomorphizations without cycles.

### 3.3 -- Existing HKT consumers stay green

- [ ] `bash tests/run.sh` clean.
- [ ] Spice-side: `../turmeric-spices/spices/ecs` and `spices/json`
      test suites pass end-to-end.
- [ ] `TUR_M3_AUDIT=1` per-fixture sweep: HKT-using fixtures
      (`functor-*`, `monad-*`, `kleisli-*`, etc.) show no new bridge
      crossings.

### 3.4 -- Phase 3 done when

- [ ] All HKT call sites in stdlib + spices dispatch via the new ABI.
- [ ] No carrier round-trips at HKT method boundaries (verified via
      audit probe).

---

## Phase 4 -- Carrier-helper rewrites (M9 prerequisite)

**Why this exists as a phase:** the archived
`docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
identified the M9 blocker explicitly -- every dispatch path that
bottoms out in an inline-C carrier helper requires the bridge to spill
by-value back to int64. Deletion requires rewriting each such helper
ABI-agnostic first.

### 4.1 -- Enumerate the carrier-helper surface

- [x] **DONE (2026-06-18).** Deliverable:
      [`v2/phase4-carrier-helper-inventory.md`](v2/phase4-carrier-helper-inventory.md).
      Headline: the **non-HKT `Eq` instances no longer bottom out in an inline-C
      carrier helper** -- they were already migrated (M4c) to read by-value
      fields directly (`Eq [Vec]` uses `vec-eq-loop`, `Eq [Map]` uses
      `map-count`+fields, `Eq [Cons]`->pure `list-eq?`, etc.). The remaining
      inline-C `*-eq?` helpers (`vec-eq?`/`slice-eq?`/`result-eq?`) are
      **standalone public API**, not dispatch targets. So the only
      dispatch-backing carrier surface left is (a) the **HKT instance bodies**
      (gated on Phase 3.0) and (b) the **genuinely runtime-erased HAMT helpers**
      (`set-eq-full`/`map-eq-raw?`/`tur_hamt_*` -- carrier-essential). Full
      per-helper verdict table + revised Phase 4/5 sequencing in the deliverable.

### 4.2 -- Rewrite each helper in pure Turmeric (or accept it as
genuine carrier)

> **Gating note (2026-06-18):** the subset of these helpers that are **HKT
> instance method bodies** (`fmap`/`ap`/`bind`/`pure` for Option, Parser, Goal,
> Backtrack, Schema, ...) CANNOT be rewritten to pure-Turmeric until Phase 3.0
> lands -- the method body has no element-type tyvar in scope, so a by-value
> rewrite (`(option-map container fn)`) fails to typecheck (`container : Option`,
> not `(Option A)`). Proven 2026-06-18. Non-HKT carrier helpers
> (`vec-eq?`/`map-eq?`/`set-eq?`/...) are independent of 3.0 and can proceed.

For each enumerated helper:

- [ ] Decide: can the helper body be expressed in pure Turmeric over
      the typed payload, or is it genuinely operating on
      runtime-erased data (heterogeneous hamt, existential pack)?
- [ ] If pure-Turmeric-expressible: rewrite (template: see
      `option-eq?`/`option-map` from Phase M2). Land per-helper as a
      small PR; regen affected fixture snapshots in the same PR.
- [ ] If genuinely carrier: leave inline-C with a `;;` NOTE explaining
      why, and add the helper to the "carrier-essential" list in
      `docs/monomorphization-audit.md`.

### 4.3 -- Phase 4 done when

- [ ] Every helper from 4.1 is either rewritten or annotated as
      carrier-essential.
- [ ] `TUR_M3_AUDIT=1` sweep: the only remaining crossings are at
      annotated carrier-essential sites.
- [ ] `docs/monomorphization-audit.md` updated with the new audit
      floor; per-carrier-essential-site justification recorded.

---

## Phase 5 -- M9 + M10: delete the bridge, re-audit, archive

Once Phase 4 has shrunk the bridge consumer set to the documented
carrier-essential cases, the bridge predicate can be tightened to
fire ONLY on those sites, and the rest of the machinery deletes.

### 5.1 -- Tighten the bridge predicates

- [ ] In `src/compiler/emit_expr.c`, narrow `expr_emits_byvalue_carrier_abi`
      and `type_uses_carrier_in_dispatch` to fire only on the
      annotated carrier-essential helper-consumer pairs from Phase 4.
- [ ] Add an assertion path: if the predicate fires anywhere ELSE,
      abort with a diagnostic pointing at this plan. Catches
      regressions at compile time, not at audit time.
- [ ] **Validation:** `bash tests/run.sh` clean.

### 5.2 -- Rename `tur_ok` / `tur_err` (M8 absorbed)

- [ ] In `src/compiler/emit_module.c:2969` + callers, rename the
      prelude helpers to `tur_box_ok` / `tur_box_err` /
      `tur_box_some` / `tur_box_none`. Names now reflect the fact
      that they exist for genuinely-erased carrier values only.
- [ ] Update inline-C bodies in `stdlib/` that still hand-roll these
      names to use the new spelling, or to switch to pure-Turmeric
      construction.
- [ ] **Validation:** `bash tests/run.sh` clean.

### 5.3 -- Delete the non-essential bridge code paths

- [ ] In `src/compiler/emit_expr.c`, delete `emit_carrier_bridge`
      call sites that are now unreachable (lines 2020, 2676, 2909,
      2960, 3049, 3084, 4878, 4904 per 2026-06-19 grep -- re-verify
      before deleting).
- [ ] If `emit_carrier_bridge` itself becomes single-call-site,
      inline it. If unreachable, delete.
- [ ] Delete the `emit_byvalue_carrier_abi` flag on `Bindings`
      (3 files reference it; once dispatch goes through the M4
      per-method ABI everywhere, the flag is dead).
- [ ] **Validation:** `bash tests/run.sh` clean.

### 5.4 -- M10 re-audit

- [ ] Run `TUR_M3_AUDIT=1` per-fixture sweep one final time.
      Expected: zero crossings except at the annotated
      carrier-essential helper-consumer sites from Phase 4.
- [ ] Update `docs/monomorphization-audit.md` with the final
      audit state and the post-M10 carrier-essential inventory.
- [ ] Archive this plan to `docs/archive/`. File any residual
      surprises under `docs/reported/`.

### 5.5 -- Phase 5 done when

- [ ] The bridge machinery (`emit_carrier_bridge`,
      `expr_emits_byvalue_carrier_abi`, `type_uses_carrier_in_dispatch`)
      is gone or scoped to the carrier-essential inventory.
- [ ] Audit floor is zero except at documented carrier-essential
      sites.
- [ ] This plan is archived; the README of `docs/upcoming/` no
      longer references it.

---

## Risks and unknowns (NOT punted to a follow-up)

- **Audit baseline drift.** The "41 crossings / 11 fixtures" floor in
  `m4-typeclass-per-method-abi-plan.md` is from PR #419; PRs #421, #424,
  #425, #426, #427, #428, #430, #431 have all landed since. Phase 1.5
  re-measures explicitly. Do not quote the 41/11 figure elsewhere
  without re-running the sweep.
- **TUR_M3_AUDIT mode is currently noisy under suite runs.** A
  2026-06-19 attempt to run `TUR_M3_AUDIT=1 bash tests/run.sh` produced
  hundreds of unexplained "build failed" lines on fixtures that pass
  without the env-var. Diagnose this before relying on the suite-wide
  audit count -- the standalone per-fixture probe (per
  `docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
  "New baseline") is the methodology of record.
- **HKT cost model is unmeasured.** Phase 2.1 is the entire point;
  do not commit to option 1 vs 2 vs 3 before running it.

## Cross-references

- Archived predecessor plan + rationale:
  [`docs/archive/end-to-end-monomorphization-plan.md`](../archive/end-to-end-monomorphization-plan.md).
- Archived M5 scope audit:
  [`docs/archive/m5-scope-audit-2026-06-18.md`](../archive/m5-scope-audit-2026-06-18.md).
- M4a status:
  [`docs/upcoming/m4-typeclass-per-method-abi-plan.md`](m4-typeclass-per-method-abi-plan.md).
- Bucket C residuals:
  [`docs/reported/option-consumer-retype-byvalue.md`](../reported/option-consumer-retype-byvalue.md).
- M9 prerequisite explanation:
  [`docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`](../archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md).
