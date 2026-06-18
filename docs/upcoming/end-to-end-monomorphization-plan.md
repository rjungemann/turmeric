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
**step 1 is LANDED** (by-value `unwrap-or` + `unwrap-or-carrier` shim,
regression-free); its remaining producer migration (step 2) is gated behind
1.4 (the sole remaining stdlib shim caller is the kleisli `comp`). 1.5 (bucket
C exit criterion) cannot close until 1.4 + 1.3-step-2 do.
Net dependency order is now: 1.1 ✓, 1.2 ✓, 1.3-step-1 ✓, then Phases 2-3 (HKT),
then 1.4 -> 1.3-step-2 -> 1.5. See per-item notes below.

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
- [x] **Step 1 LANDED (2026-06-18).** Retyped `unwrap-or` to by-value
      `[A] [o : (Option A) dflt : A] : A` (pure-Turmeric `(if (.is-some o)
      (.value o) dflt)`) and added the `unwrap-or-carrier` inline-C shim for
      carrier-context callers. Most call sites are by-value producers
      (`some`/`none`/`option-map`) and now dispatch by-value (reducing carrier
      crossings -- the goal); the genuinely carrier-context sites (zipper's
      `zipper-move-right` result, kleisli's `k-apply` result, and the deliberate
      `option-map-byvalue-result-into-carrier-consumer-let-inside-arg` spill-bridge
      fixture) flip to `unwrap-or-carrier`, preserving their coverage. Suite
      green (1682; only the pre-existing stale ECS spices fixture fails); 85
      stdlib snapshots regenerated. Per-module producer migration (step 2) and
      shim retirement (step 3) remain, gated on by-value Option producers (the
      sole remaining stdlib `unwrap-or-carrier` caller is kleisli, M7-gated).
      Finding recorded in
      [`unwrap-or-byvalue-cascade.md`](../reported/unwrap-or-byvalue-cascade.md).
- [x] Retype `unwrap-or` in `stdlib/option.tur` to
      `[A] [o : (Option A) dflt : A] : A` with a pure-Turmeric body:
      `(if (.is-some o) (.value o) dflt)`. (Step 1, landed.)
- [x] **Step 2 -- zipper producer migration (2026-06-18).** Migrated
      `stdlib/zipper.tur` to a by-value Option producer: split each handle op
      into a `-raw` inline-C helper (taking/returning the bare `:int` carrier)
      plus an element-typed by-value wrapper -- `zipper-new [A] [... focus : A
      ...] : (Zipper A)`, `zipper-focus [A] [z : (Zipper A)] : A`,
      `zipper-move-left`/`-right [A] [z : (Zipper A)] : (Option (Zipper A))`,
      `zipper-free [A] [z : (Zipper A)] : void`. The move ops now return
      `(some (:: r (Zipper A)))` / `(none)` instead of hand-malloc'ing a carrier
      Option struct in C. **Root blocker fixed in-PR:** `Zipper` was declared
      `(defstruct Zipper [A] (left ...) (left-len) (focus) (right) (right-len))`
      but every helper treats it as an opaque heap pointer (malloc'd, passed as
      int64) -- nothing ever constructs it by-value or reads a field through
      Turmeric. The 5-field by-value layout was a phantom that made
      `(Option (Zipper A))` un-threadable (`(:: int (Zipper A))` tried to
      reinterpret an int64 into a 5-field struct -> cc type error). Retyped it
      to `(defopaque Zipper [A] :int)`, matching the real pointer-handle
      representation. Fixtures `zipper-basic` / `typed/zipper-basic` flipped to
      by-value `unwrap-or` (dflt `(:: 0 (Zipper int))`) and `(not (.is-some
      opt))` none-detection (was the carrier-peeking `(= opt 0)`).
- [ ] Remaining step-2/step-3: the sole stdlib `unwrap-or-carrier` caller left
      is the kleisli `comp` body (M7-gated, see 1.4); shim retirement waits on it.
- [x] Regenerate any affected `tests/fixtures/*/expected.c` snapshots
      in this same PR (85 snapshots regenerated for the zipper preamble change).
- [x] **Validation:** `bash tests/run.sh` -- 1682 passed; only the pre-existing
      stale-ECS-spices `errors/ecs-defsystem-writes-unauthorized` fixture fails
      (tracked in `docs/reported/ecs-defsystem-writes-unauthorized-stale-diag.md`,
      unrelated to zipper).
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
35 `definstance` lines across Parser, Option, Result/Either, Goal,
Backtrack, Schema, rc, and 43 fmap/bind/pure/lift2 call sites across
stdlib (the old "~199" figure is stale; spices adds 0 -- see 2.1).
Per-instantiation expansion blast radius is now known to be
stdlib-bounded; the design pass picks the model before the implementation
phase commits.

### 2.1 -- Measure HKT call-graph cost per option

**Measurement (2026-06-18; spices half now COMPLETE -- `../turmeric-spices/`
IS checked out, 522 `.tur` files):**

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
- **Spices half (now measured): ZERO.** Across all 522 `.tur` files in
  `../turmeric-spices/spices/` there are **0** HKT `definstance` lines and
  **0** genuine HKT method call sites. Every grep hit for the method names is
  a false positive at a different binding: `bind-vao`/`bind-vbo`/`bind-texture`
  (OpenGL), `bind(fd, ...)` (POSIX socket in `httpd/server.tur`), `first`/
  `second` (pair/list accessors), `empty` (collection emptiness predicates),
  "pure Turmeric" (prose comments). The "~199" figure in the gating note above
  was therefore **not** spices-driven either; it is simply stale/overcounted.
  **The HKT monomorphization surface is entirely stdlib-bounded:** 9 classes,
  35 instances, 43 call sites; spices adds nothing to the `A` dimension.

This de-risks option 1 materially: the reachable `(combinator, instance)`
call graph does not grow with the spice ecosystem, so the per-`(f, A)`
clone blast radius is bounded by the 35-instance stdlib surface plus whatever
distinct `A`s those 43 stdlib sites instantiate -- a small, closed set, not an
open-ended ecosystem-wide expansion.

- [x] Spices-half inventory complete (2026-06-18): 0 HKT instances, 0 HKT
      call sites across 522 spice `.tur` files; surface is stdlib-bounded
      (9 classes / 35 instances / 43 call sites).
- [ ] Remaining for a precise option-1 clone count: a clone-counting probe in
      the elaborator (no such tooling exists yet) to turn the 43 stdlib sites
      into a distinct-`(f, A)`-clone total. The *instance* surface (35) and the
      *reachability bound* (stdlib-only) are now fixed; only the per-`A`
      multiplier is unmeasured, and it cannot grow beyond the stdlib call sites.
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
- [x] **Static enumeration done (2026-06-18); exact figure still probe-gated.**
      A precise static pass over the now-closed surface tightened both inputs to
      the clone count:
      - **Genuine dispatch sites ~21, confirmed.** The raw stdlib grep (110+
        hits) is dominated by false positives: all `stdlib/httpd.tur` hits are
        inline-C (`while (ap)`, POSIX `bind(fd, ...)`), `async_socket.tur:58`
        is socket `bind`, `effects.tur`/`map.tur`/`set.tur`/`mutmap.tur` `pure`
        are comments, `docstrings.tur` is auto-generated string content, and the
        `typeclass*.tur` / `comonad.tur` lines are class-default / instance
        *method definitions*, not call sites.
      - **Ground-reachable instance set = 6:** Option, Vec, Result, Pair, List,
        Parser (measured from the HKT leaf instantiations across all
        `tests/fixtures/**`).
      - **Ground element-type set is closed and small: `{int, cstr, ~2 schema
        structs (e.g. User)}`** -- i.e. the tradeoff doc's "+few" is now
        enumerated. Across the entire fixture suite no HKT site instantiates an
        open-ended `A`; the A-dimension does not grow with the (already
        HKT-free) spice ecosystem either (2.1).
      - So the option-1 clone count = (ground-reachable combinator x instance
        pairs over ~30 instance-method impls) x |A-set ~= 3-4| = **low tens**,
        with the A-multiplier now bounded by an enumerated set rather than an
        estimate.
- [ ] **Exact figure remains probe-gated (circular w/ M7):** turning the bound
      into a precise per-`(f, A)` total needs the transitive leaf-to-instance
      call graph, which an *elaborator* clone-count probe would walk. Building
      one that faithfully models the per-`(f, A)` monomorphization is itself
      M7-adjacent (it must model the expansion that does not exist yet), so the
      exact count cannot be produced ahead of Phase 3 without that tooling. The
      *bound* (low tens) and both its inputs (6 instances, A-set of 3-4) are
      now fixed and are sufficient to *size* Phase 3 as "low tens of clones."

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

### 3.0 -- THE core prerequisite: thread the element type into HKT instance methods

> **UPDATE (2026-06-19, third session -- LAYER 4 EMIT LANDED, probe prints 42).**
> The "sole remaining wall" (layer 4 = the Phase 3.2 emit-side per-`(f, A)`
> by-value instance-method monomorphization) is now implemented, flag-gated
> behind `TUR_M7_HKT` and **regression-free with the flag OFF** (suite
> 1683/0; shipped codegen byte-identical). `TUR_M7_HKT=1 ./build/tur run
> docs/upcoming/v2/m7-hkt-probe.tur` now exits **42** with no ascription
> anywhere. What landed:
>
> - **Elab (`elab_typeclasses.c`, `elab_method_call`):** for an HKT class the
>   dispatch call now attaches the class var (`g -> Option`) plus the
>   layers-1+3 element tyvars (`a/b -> int`) as the call's `abi_bindings`, so
>   `emit_abi_register_call` can mint a per-`(f, A)` spec. **Gated tightly:**
>   only when the instance body is by-value-*constructible* --
>   `m7_body_constructs_byvalue` requires the body's tail (through if/do/let) to
>   be a `#{Construct}` call. Carrier inline-C bodies and bodies that delegate
>   to a carrier helper (`Bifunctor [Result]` -> `result-bimap [container :
>   int]`) are excluded and stay on the carrier ABI even under the flag.
> - **Emit return type (`emit_fns.c` body+signature, `emit_module.c` forward
>   decl):** a by-value HKT instance-method spec whose `result_type` is a
>   concrete non-heap `TY_APP` (`Option__int`) emits the struct BY VALUE instead
>   of the int64 carrier spill -- three sites kept in sync.
> - **Emit construct recovery (`emit_module.c:1139` fall-through):** a 0-arg
>   `#{Construct}` (`(none)`) inside an active by-value HKT instance-method spec
>   now falls past the no-`abi_bindings` early-return so `construct_recovered_byvalue`
>   interns `none__spec` (the 1-arg `(some ...)` already did via its element arg).
> - **Dict base (`emit_module.c`):** the carrier base instance method is
>   carrier-noted when its by-value spec is interned, so the (still carrier-ABI)
>   dispatch dict keeps a valid reference for indirect dispatch.
>
> **Verified:** probe -> 42; flag-off suite 1683/0; all existing HKT fixtures
> (`hkt-stdlib-*`, `schema-hkt-functor`, `hkt-typeclass-instance`) stay green
> BOTH flag-off and flag-on; no flag-on regressions vs. parent across a
> 154-fixture typeclass sweep (the 3 flag-on failures -- `hrt-rankn-hkt`,
> `hrt-rankn-typeclass`, `instance-method-return-carrier-bridge` -- already
> failed flag-on at the parent commit; the rank-N pair is a separate pre-existing
> null-`Kind` deref tracked in
> [`docs/reported/rankn-hkt-null-kind-deref-under-m7-flag.md`](../reported/rankn-hkt-null-kind-deref-under-m7-flag.md)).
>
> **What remains for flag-on-by-default = Phase 4.2:** the stdlib HKT instance
> bodies that are still carrier inline-C or delegate to carrier helpers
> (`fmap`/`bind`/`pure`/`ap`/`bimap` for Option/Result/Parser/Goal/Backtrack/
> Schema). Each must be rewritten to an in-body by-value construct (the probe's
> `gmap` is the template) before the gate admits it. The layer-4 machinery is
> now in place and waiting for those rewrites.

> **UPDATE (2026-06-18, second session -- elaborator threading LANDED INERT,
> "step (a)" DISPROVEN).** The full elaborator type-threading is now implemented
> and committed behind the existing default-OFF `TUR_M7_HKT` flag (suite green
> at 1683/0 with the flag off; existing HKT fixtures behave identically with the
> flag on). Under `TUR_M7_HKT=1` the probe `(gmap (some 21) dbl)` now resolves
> end-to-end to `(Option int)` with **no ascription** and typechecks; it now
> fails only in CODEGEN (layer 4, below). This required correcting the prior
> root-cause analysis:
>
> - **"Step (a)" (resolve the application HEAD `g` to `TY_TYVAR`) is a red
>   herring.** Instrumentation proved `(g b)` already parses with a NAMED head
>   `TY_TYVAR("g")` (`type_expr_from_form` resolves `g` via the bare-class-param
>   branch at `elab_types.c:421-446`). The reliability caveat above was right.
> - **The real parse gap was the ARG:** the element tyvars `a`/`b` are NOT class
>   params, so they took the "unknown -> anonymous opaque struct" fallback
>   (`elab_types.c:549-562`), giving `TY_APP(TY_TYVAR g, TY_STRUCT{NULL})` which
>   prints `(type-app tyvar ?)` and leaves no named element to refine. **Fix:**
>   collect method-level implicit tyvars from the param/return type forms and
>   thread them as additional type params (`m7_collect_form_tyvars` /
>   `m7_is_method_tyvar_name` in `elab_typeclasses.c`), so `(g b)` parses to
>   `TY_APP(TY_TYVAR g, TY_TYVAR b)`. Gated on the flag (inert when off).
> - **Layers 0+2** (HKT head substitution `g -> Option`; carry the `TY_APP`
>   return through `result_full_type`) were already present inert from the prior
>   session and now fire correctly given the named element.
> - **Layers 1+3 (call-site refinement) IMPLEMENTED:** `m7_collect_tyvar_bindings`
>   (in `elab_typeclasses.c`, next to `elab_subst_class_tyvars`) unifies the
>   CLASS method's declared param types -- `(g a)` and the real `TY_FN`
>   `(fn [a] b)` (the `(fn ...)` form is parsed as a genuine fn-type, NOT the
>   `:fn` carrier, so its result tyvar `b` survives) -- against the actual call
>   arg types to bind `a/b`, then substitutes into the result `(Option b)` ->
>   `(Option int)` at `elab_method_call`. The declared types are read from the
>   class method (`tc->methods[i].param_types`), not the instance binding
>   (whose `arg_full_types` is NULL).
>
> **The SOLE remaining wall is layer 4 (= Phase 3.2 emit).** With the type fully
> threaded, the probe compiles but the instance method
> `__inst_MyFunctor_gmap_Option` still emits the int64 carrier return
> (`malloc(sizeof(int64_t)); return (int64_t)__tur_ret_p;`) while the by-value
> consumer reads `Option__int` -- the documented carrier-vs-by-value mismatch.
> The emit-side per-`(f, A)` by-value instance-method monomorphization (the
> "irreducible core" detailed in 3.1) is now the only thing between HEAD and a
> probe that prints 42. Because the probe body is already by-value
> pure-Turmeric, completing layer 4 lands the probe WITHOUT touching stdlib;
> enabling the flag by default additionally needs the Phase-4.2 body rewrites.

### 3.0 (historical) -- the iterative root-cause dig (superseded by the UPDATE above)

> **Reliability caveat (added 2026-06-18, end of session).** The
> layer-by-layer "precise root" notes below (the "fifth layer" / "step (a)"
> findings, and the per-commit refinements in the history) were produced by an
> iterative in-session deep-dive that **partially contradicted itself** by the
> end. In particular: the claim that an HKT instance constructor isn't a
> substitutable `Type` was later **corrected** (it is -- `elab_definstance`
> resolves it at `elab_typeclasses.c:1634-1657`); and the final "step (a)"
> claim that `type_expr_from_form` fails to resolve an application **head**
> class-param to `TY_TYVAR` is **probably wrong** -- the application path
> (`elab_types.c:1730`) recurses into the head, and the bare-type-param branch
> (`elab_types.c:421-446`) already returns a named `TY_TYVAR`, so `(g b)` likely
> parses to `TY_APP(TY_TYVAR g, TY_TYVAR b)` already. The `(type-app ? ?)` the
> probe showed was therefore more likely a bug in the (reverted) flag-gated
> elaborator edits than a real parser gap. **Do not treat the layer
> breakdown below as authoritative;** re-derive it fresh with a debugger.
> What IS reliable and held across every attempt: (1) the **emit
> monomorphization** (per-`(f, A)` by-value instance-method clone) is the
> definitive wall -- without it the HKT value path silently miscompiles (`0`
> for `42`); (2) it cannot land without the by-value instance-body rewrite
> (Phase 4.2) and the class-signature change (`: int` -> `: (f b)`); (3) the
> change is therefore atomic across elaborator + emit + the ~30 instance
> bodies, and a partial landing violates the no-silent-miscompile rule.

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

**CONCLUSIVE (2026-06-18, second iteration): elaborator + emit are ATOMIC.**
A follow-up narrowed the elaborator change to fire ONLY on HKT-param-headed
returns (`(g b)` where `g` is the class `[^g]` param), excluding the
decode-style `(Result a cstr)` pattern via `tc_return_is_hkt_param_headed`.
This made the change **regression-free on the suite** (1682 pass; only the
pre-existing stale ECS spices fixture fails) -- the 4 decode/return-dispatch
fixtures pass again. BUT the narrowed change still **regresses the HKT-method
value path even WITH the `(:: r (Option int))` ascription**: a probe that
prints `42` on unmodified HEAD prints `0` after the change, because setting a
`TY_APP` `result_full_type` on the instance method flips its emit to the
double-boxing generic-carrier return (the declared `(Option b)` has `b`
unresolved in the generic instance method, so `__TUR_RET__` wraps a `malloc`'d
int64 that the by-value consumer then misreads). The suite stays green only
because no fixture exercises an HKT-param-headed method.

So there is **no safe incremental landing of the elaborator alone** -- it
silently miscompiles the HKT-method value path the moment it is active, which
violates the no-silent-miscompile rule. The elaborator type-threading and the
Phase 3.2 emit-side (per-`(f, A)` by-value monomorphization of the instance
method, so `b` resolves and it returns `Option__int` by value) **must land
together**. This is the empirical confirmation that Phase 3 is the "largest
single phase": an atomic elaborator+emit change across all 9 HKT classes / 30
instances. Both elaborator iterations (broad, then narrowed) are reverted;
suite restored to green (HKT-via-ascription prints 42 again).

**Third iteration (2026-06-18): flag-gated probe pins a FOURTH layer below the
documented three.** Re-attempted the elaborator threading behind a default-OFF
`TUR_M7_HKT` env gate (so the shipped path stays byte-identical), implementing
the impl-side `TY_APP` `result_full_type` branch (now `elab_typeclasses.c`
~2884) and a call-site `m7_unify_named` refinement that walks the method's
declared param types through `TY_APP`/`TY_FN` (`arg_full_types`/
`result_full_type` do preserve the element tyvar names, so `b` *is*
recoverable). Result under the flag: the probe STILL resolves to
`(type-app ? ?)`, because the instance method's `return_type` is the raw class
return `(g b)` with the **HKT class param `g` itself unsubstituted** -- so the
carried `rft` is `(type-app ? ?)`, not `(Option b)`, and there is nothing
concrete to refine. So the prerequisite ordering is now four-deep:
  0. **(new) Substitute the instance's HKT constructor for the class param**
     (`g` -> `Option`) into `return_type` at instance-definition time
     (`elab_subst_class_tyvars` exists at `elab_typeclasses.c:1513` but is not
     applied to the HKT-headed return here) so `rft` is `(Option b)`.
  1-3. The documented `tc_subst_class_params` / `result_full_type` TY_APP carry
     / call-site element-tyvar refinement (the latter two were reconstructed
     and build clean; step 0 gates them).
  4. The Phase 3.2 emit monomorphization (the wall: even with 0-3, the generic
     instance method emits the double-boxing carrier return and prints `0`).
All flag-gated experimental code was **reverted**; the tree matches HEAD and
the suite is green. Net: the layering is now fully mapped (0->1->2->3->4) and
each layer's exact code site is pinned, but the bottom layer (4, emit) remains
the atomic blocker that cannot land without the by-value instance-body rewrite
(Phase 4.2), confirming the "must land together" conclusion from a third angle.

**Fourth iteration (2026-06-18, second hands-on attempt): a FIFTH layer below
layer 0.** Re-applied all of layers 0-3 behind `TUR_M7_HKT` and added the
layer-0 `elab_subst_class_tyvars` call on a `TY_APP` instance return
(`elab_typeclasses.c` ~2464). Under the flag the probe STILL resolved to
`(type-app ? ?)`: the substitution left the HKT constructor head `g`
unsubstituted because an HKT instance's constructor argument (`Option` in
`MyFunctor [Option]`) is **not carried in `type_args` as a substitutable
`Type`** -- the `[^g]`-kinded slot is tracked via `type_arg_syms` / a separate
representation, so `elab_subst_class_tyvars(g -> type_args[0])` finds no Type to
plug in for `g`. So the true prerequisite is a -1th layer: **give HKT instance
constructor args a first-class substitutable `Type` representation** (or teach
the layer-0 substitution to read the constructor from `type_arg_syms` and build
the applied head). Each hands-on fix exposes the next erasure in the HKT type
representation; the feature is a sustained representational thread-through
(constructor type -> element tyvar -> result -> emit mono -> by-value bodies),
not an incremental patch. Code reverted; tree at HEAD, suite green.

**Correction to the "fifth layer" (2026-06-18, code re-read):** the HKT
constructor IS recoverable -- `elab_definstance`
(`elab_typeclasses.c:1634-1657`) resolves an instance head like `[Option]` via
`scope_lookup`, so for a `defstruct`/`defdata` constructor `type_args[0]` is a
real `TY_STRUCT`/`TY_ADT` carrying its def (and `type_arg_syms[0]` the symbol);
`elab_subst_class_tyvars(g -> type_args[0])` therefore HAS something concrete to
plug in. The flag-gated probe still saw `(type-app ? ?)` not because the
constructor is missing but because the probe's class-method return `(g b)` did
not arrive as a `TY_APP` at the layer-0 site -- a `parse_typeclass_method`
representation gap for an HKT-param-applied return. **The larger root:** the
real stdlib HKT classes do not declare the `(g b)` shape at all -- every method
returns a bare `: int` carrier (`fmap [container [fn :fn]] : int`, etc.). So M7
requires **changing all 9 HKT class method *signatures*** from `: int` to the
applied-head form (`: (f b)`); that is the true source of the
"change-everything-together" atomicity. Revised first step for the next
session: (a) teach `parse_typeclass_method` to represent an HKT-param-applied
return `(f b)` as `TY_APP(TY_TYVAR f, TY_TYVAR b)`; (b) convert one class
(Functor) + its Option instance to that shape; (c) wire layers 0-3 (now
unblocked -- the constructor type is available); (d) the emit monomorphization
(layer 4). The suite-green gate sits at the end of (d): (a)-(c) without (d)
still miscompile the value path, so they cannot land separately.

**Precise root of step (a) (2026-06-18, traced to one site):** for an applied
return `(g b)`, `parse_typeclass_method` (`elab_typeclasses.c:975-987`) calls
`type_expr_from_form(ret_form, ..., class_type_params, ...)`. That helper
resolves a class type-param in **argument** position (`b`) to a `TY_TYVAR`, but
resolves the **head/constructor** position (`g`) as a concrete constructor
lookup -- so `(g b)` becomes `TY_APP(<unknown/opaque head>, TY_TYVAR b)`, which
prints `(type-app ? ?)` and gives the layer-0 `elab_subst_class_tyvars(g -> ...)`
no `TY_TYVAR("g")` head to match. **Step (a) is therefore: teach
`type_expr_from_form` (or the class-method return path) to resolve a class
type-param that appears as an application HEAD to `TY_TYVAR(name)`,** so `(g b)`
parses to `TY_APP(TY_TYVAR g, TY_TYVAR b)`. With that, layer 0's substitution
fires (g -> the instance's `TY_STRUCT`/`TY_ADT` constructor, which 1634-1657
already provides), layers 1-3 refine `b`, and the result resolves to
`(Option int)` at the call site. The emit monomorphization (d) remains the
suite-green gate. This is the single most actionable starting point and the
deepest the root has been localized.

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

**Sixth iteration (2026-06-18): flag + layers 0 & 2 landed INERT (not reverted).**
Unlike iterations 1-5 (all reverted), this attempt's scaffolding is kept,
because it is fully gated behind a new default-OFF `TUR_M7_HKT` env flag
(`g_m7_hkt_enabled`, `src/runtime/globals.{h,c}`, set in `main.c`) -- with the
flag off, codegen is byte-identical and the suite is green (1682; only the
pre-existing ECS fixture). Landed under the flag:
  - **Layer 0** (`elab_typeclasses.c` ~2465): a TY_APP instance-method return
    `(g b)` substitutes the HKT class param in HEAD position with the
    instance's constructor (`type_args[ti]`, e.g. `Option`), leaving the
    element tyvar `b` abstract -> `(Option b)`.
  - **Layer 2** (`elab_typeclasses.c` ~2886): carry that TY_APP return through
    `result_full_type` so the call site receives the named applied head + `b`
    (instead of an anonymous `(type-app ? ?)`).
Confirmed STILL INSUFFICIENT on the probe (prints the same call-site error)
because three layers remain: **step (a)** -- the parse representation: `(g b)`
arrives with an anonymous (un-named) head/arg, so layer 0's
`return_type.as.app.fn->kind == TY_TYVAR` guard does not even fire (the
`type_expr_from_form` head-position fix must precede it); **layers 1 & 3** --
call-site element-tyvar refinement in `elab_method_call`; and **layer 4** --
the emit-side per-`(f,A)` by-value monomorphization. The emit gate is now
pinned exactly: `emit_module.c:736` sets `spec->typeclass_inst` (routing to
the M4 per-instantiation by-value emit) ONLY when `!is_hkt`; flipping it for
HKT is necessary BUT the 35 real stdlib HKT instance bodies are carrier
inline-C and would miscompile under by-value per-instantiation emit -- which
is precisely why the Phase 4.2 body rewrites must land WITH layer 4. The
probe's own `gmap` body is by-value, so a complete a+0+1+2+3+4 lands the probe
(prints 42) without touching stdlib; enabling the flag by default additionally
needs Phase 4.2. Net: the scaffolding (flag, layers 0+2) is in place and
inert; next session does step (a) -> layers 1,3 -> layer 4 against the probe.

**Re-verified on HEAD (2026-06-18) + probe preserved in-repo.** Re-ran the
minimal probe against the current (unmodified) compiler and reproduced the
exact documented failure: `(gmap (some 21) dbl)` resolves to `(type-app ? ?)`
and the error surfaces at the CONSUMER call site --
`function 'unwrap' arg 1: expected (type-app Option tyvar 'A'), got
(type-app ? ?)`. So `gmap`'s declared `(g b)` return reaches the call site
with anonymous (un-named, un-refined) tyvars: neither `g -> Option` nor
`b -> int` is recovered. The probe is now committed at
[`docs/upcoming/v2/m7-hkt-probe.tur`](v2/m7-hkt-probe.tur) (deliberately NOT a
suite fixture -- it errors by design) so the next dedicated M7 session drives
the implementation against a fixed reproduction instead of reconstructing it
from the transcript. **Single most actionable next step is still step (a)
above** (`type_expr_from_form` head-position class-param -> `TY_TYVAR`), then
layers 1-3 (call-site refinement), then the layer-4 emit monomorphization
+ Phase-4.2 by-value instance bodies, which must land together. No safe
partial landing exists: threading the type without the emit half compiles
but prints `0` (silent miscompile), so this stays scheduled as a dedicated
multi-session effort rather than an in-session increment.

### 3.1 -- Dict generation for HKT instances

**Exact emit gate located (2026-06-18):** the per-instantiation
instance-method spec machinery the non-HKT M4 path uses **already exists** and
is **deliberately gated off for HKT classes** at
`src/compiler/emit_module.c:726-736`:

```c
if (fd && fd->owner_instance && fd->owner_instance->typeclass) {
    TypeClass *tc = fd->owner_instance->typeclass;
    bool is_hkt = /* any tc->type_param_kinds[i] != KIND_STAR */;
    if (!is_hkt) spec->typeclass_inst = fd->owner_instance;  // <-- HKT excluded
}
```

The comment there says verbatim: "HKT-class instance methods keep the uniform
carrier ABI per Plan M6/M7 -- leave typeclass_inst NULL so the legacy dispatch
path stays unchanged for them." So **enabling HKT by-value dispatch is exactly
the M7 implementation** -- removing/relaxing this gate AND making the
per-instantiation path handle the HKT element type (the elaborator work in 3.0
+ a per-`(f, A)` by-value spec for the instance method body).

**Experiment (2026-06-18):** relaxing the gate alone (always set
`typeclass_inst`, no elaborator change) was tested. Result: it routes existing
HKT instances through the per-instantiation path with **mixed** outcomes --
`hkt-stdlib-option-result-instances`, `hkt-stdlib-backtrack-instances`,
`schema-hkt-functor` still PASS, but `hkt-stdlib-parser-instances` and
`hkt-typeclass-instance` **break** (diff / exit 1). So the gate is load-bearing
-- the per-instantiation path does not yet handle all HKT instances -- and the
flip is NOT safe. Reverted. Combined with the 3.0 result (elaborator alone
silently miscompiles the value path), this proves **both ends of the change are
individually unsafe**: M7 must land the elaborator element-type threading AND
the emit per-`(f, A)` by-value path AND the parser/typeclass-instance dispatch
fixes together. The two ends (`emit_module.c:736` + the 3.0 elaborator sites)
are now both located and characterized.

- [x] **Coordinated narrowing prototyped (2026-06-18, reverted).** Implemented
      the full coordinated change: the 3.0 elaborator (hkt-param-headed only) +
      a NARROWED gate at `emit_module.c:736` that sets `typeclass_inst` for an
      HKT method ONLY when its `result_full_type` is a `TY_APP` (the new
      by-value style), leaving `:int`-returning HKT methods on the carrier. This
      is **regression-free** (1682 pass; existing HKT fixtures incl. parser /
      typeclass-instance stay green -- the earlier unconditional-relax breakage
      is avoided). The remaining gap is below.
- [x] **Per-`(f, A)` by-value SPEC INTERNING -- LANDED (2026-06-19, flag-gated).**
      Implemented end-to-end; the probe now prints 42 under `TUR_M7_HKT=1`. See
      the 2026-06-19 UPDATE at the top of Phase 3.0 for the exact sites. The
      mechanism that finally interned the by-value spec was attaching the HKT
      element-tyvar bindings to the dispatch call at elab (gated on a genuinely
      by-value-constructible instance body), which lets the existing
      `construct_recovered_byvalue` + by-value return-type emit fire. The
      historical dead-end notes below are retained for context.
- [ ] **(historical) Per-`(f, A)` by-value SPEC INTERNING -- the precise final piece.**
      Even with the elaborator + narrowed gate, the probe still prints `0`:
      the generated C shows `__inst_MyFunctor_gmap_Option` emitted **once** as
      `int64_t(int64_t,int64_t)` (carrier) with **no** `__spec` clone. Routing
      through `typeclass_inst` (the M4 per-instance dict) is NOT enough -- M4
      monomorphizes on the dispatch type (`g` = Option, already concrete in the
      instance), but the HKT *element* `b` stays a tyvar, so the method body's
      `(some ...)` double-boxes the carrier. The fix: make the HKT
      instance-method DISPATCH CALL intern a by-value `abi_spec` that
      monomorphizes `b` from the call args (mirroring how a direct call to the
      generic `option-map` interns `option_map__int_int`) and emit the
      `gmap_Option__int` clone with a by-value `Option__int` return. This is the
      genuine Phase 3.2 emit core -- a standalone feature comparable to the
      option-map by-value spec work, applied to dispatch-reached instance
      methods. Because the current prototype silently miscompiles
      hkt-param-headed methods (no `__spec` clone) it is reverted, not landed.

      **Deepest root cause traced (2026-06-18):** `emit_abi_register_call`
      (`emit_module.c`) does not intern a by-value spec for the HKT dispatch
      call because the call's result `(Option int)` is a **carrier-ABI** type
      (`type_uses_carrier_abi` is true for Option), so `abi_changes` stays false
      and the call falls through to the bare carrier method
      (`emit_module.c:1532`). The construct-body `needs_byvalue_spec` logic
      (`emit_module.c:1559+`) that forces `option-map`'s by-value spec is NOT
      reached for dispatch-reached HKT instance methods, and the instance-method
      spec gate (`emit_module.c:1514-1531`) further skips specs whose arg/result
      spine `type_c_name`'s to `int64_t` (the unresolved-element case). So the
      Phase 3.2 emit work is: make the HKT instance-method dispatch call's
      construct body (`(if ... (some ...) (none))`) trigger the same
      `needs_byvalue_spec` per-`(f, A)` by-value interning that a direct
      `option-map` call gets, with the element `b` resolved from the call args.
      This is subtle (it interacts with Option's default carrier ABI) and is the
      irreducible core of M7's emit half.

      **Irreducible core, fully traced (2026-06-18):** the by-value spec
      machinery has a circular dependency for HKT instance methods:
        - The `needs_byvalue_spec` block (`emit_module.c:1559+`) is gated on
          `body_qualifies_for_carrier_skip`, which requires the body to be
          `EX_MAKE_STRUCT` (a `#{Construct}` template) or marker-less inline-C.
          The HKT instance method body is `(if (some? c) (some ...) (none))` --
          an `EX_IF` -- so this block is SKIPPED; it never even considers a
          by-value spec for the method.
        - `option-map` (same `EX_IF` body shape) gets its by-value emit instead
          via `construct_recovered_byvalue` -- the `(some ...)` construct is
          recovered by-value from an ENCLOSING spec whose result is already
          by-value. For a dispatch-reached HKT instance method there is no such
          enclosing by-value spec, because interning one requires the call's
          `(Option int)` result to register as a by-value `abi_change` -- and
          `(Option int)` is carrier-ABI by default, so it does not.
      Breaking the circle is the M7 emit task: make a dispatch-reached HKT
      instance method whose body constructs `(some ...)`/`(none)` over a
      by-value-resolvable element intern a per-`(f, A)` by-value spec (resolving
      the element from the call args) so `construct_recovered_byvalue` then
      fires inside it. This is new emit machinery deeply entangled with the
      carrier-ABI default -- the genuine multi-session core of M7.

      **Final disproof of the shortcut (2026-06-18):** tested whether a
      *genuinely by-value* Option argument (not the `(:: ... (Option int))`
      ascription, but a `(mk) : (Option int)` producer) would trigger the
      instance-method dispatch to intern a by-value spec the way a direct
      `option-map` call does. It does NOT: with the full elaborator + relaxed
      gate + by-value arg, the probe still prints `0` and `grep` finds **zero**
      `gmap__spec` clones. So the instance-method DISPATCH path does not intern
      by-value specs by any existing trigger (arg-driven `abi_changes`,
      construct-body, or otherwise) -- unlike a direct generic-defn call.
      Also confirmed: the stdlib HKT classes cannot be migrated by delegation
      either, because their class methods declare `: int` returns (not `(f b)`),
      so `container` stays the bare `Option` and `(option-map container fn)`
      fails to typecheck until the **class decls themselves** are rewritten to
      `(f a)`/`(f b)` -- cascading to all 9 classes' 30 instances. There is no
      single-instance or delegation shortcut; M7 is the full coordinated build.

      **All existing emit hooks exhausted (2026-06-18, ~12 experiments).**
      Beyond the elaborator + gate work, also tested forcing `needs_byvalue_spec`
      directly in `emit_abi_register_call` for HKT instance methods (extending
      `body_qualifies_for_carrier_skip` to admit their `EX_IF` construct bodies
      and forcing the by-value-spec flag). Result: STILL no `__spec` clone is
      interned and the value is still `0`. Combined with the disproven
      `typeclass_inst` (M4) routing, by-value-arg, and delegation paths, the
      per-`(f, A)` by-value HKT instance-method spec machinery is absent at
      EVERY existing layer (scan, `register_call`, the M4 dict path, the
      construct-recovery path). It must be built one level deeper: making a
      dispatch-reached instance method participate in generic-style per-element
      spec interning the way a direct `option-map` call does. No existing hook
      short-circuits it -- this is the irreducible M7 emit build.

      **Guard chain mapped (2026-06-18, 13th experiment).** Attaching the
      element-tyvar bindings as the dispatch call's `abi_bindings` (so it
      survives `emit_abi_register_call`'s no-bindings early-return at
      `emit_module.c:1139`) DOES make the result type resolve end-to-end with no
      ascription -- but the spec STILL is not interned, because the NEXT guard
      fires: the instance-method spec gate at `emit_module.c:1514-1531` skips a
      spec whose arg type `type_c_name`'s to `int64_t`, and `(Option int)` is
      carrier-ABI (`int64_t`) by default. So the M7 emit build is a *chain* of
      coordinated guard changes -- `:1139` (abi_bindings, done in the probe),
      `:1514-1531` (carrier-ABI arg skip), `:736` (typeclass_inst gate),
      `:1559+` (carrier-skip body gate) -- plus emitting the instance-method
      spec body by-value, ALL entangled with Option's default carrier ABI
      (an `(Option int)` arg/result lowers to `int64_t` unless it is the Track A
      by-value Option). Each guard defeated reveals the next; this is the
      layered, deliberate M6/M7 boundary, and completing it is the multi-session
      build. Exact guard chain recorded here for the next session.
- [ ] Update `__inst_<Class>_<method>` symbol naming to disambiguate
      per-`(f, A)` clones.
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

> **APPROACH UPDATE (2026-06-19): "extend layer-4 via probes first."** The
> stdlib HKT instance/class rewrites CANNOT be the small flag-off-byte-identical
> steps the earlier phases were: the element-type threading is flag-gated
> (`TUR_M7_HKT`) but stdlib **class signatures are shared source** (changing
> `Functor`'s sig from `: int` to `(f b)` changes flag-off parsing AND forces
> rewriting all 10 Functor instances at once, since the inline-C bodies depend on
> the `:fn` carrier shape). So before touching stdlib, the layer-4 emit machinery
> is being hardened against the full set of HKT method shapes via flag-gated
> reference probes (kept flag-off byte-identical). Progress:
>
> - **Functor `fmap` shape: DONE across all element representations.** The
>   layer-4 by-value path now works for element types `{int, cstr, float,
>   struct}` (probes verified). **Bug fixed in-session:** the fn-value call
>   inside a by-value HKT spec was casting the mapper `f`'s RESULT to the int64
>   carrier (`((int64_t (*)(int64_t))f)`) instead of resolving the result tyvar
>   `b` through the active spec -- so `(gmap (some 7) to-cstr)` miscompiled
>   (worked "by luck" only for `b = int`). Now resolves via `emit_type_c_name`
>   at the thin-fn-call site (`emit_expr.c`), symmetric to the existing arg-side
>   resolution. Flag-off suite stays 1683/0.
> - **Monad `bind` shape: DONE end-to-end (2026-06-19).** The probe
>   (`docs/upcoming/v2/m7-hkt-probe-bind.tur`) exits 21 under the flag; verified
>   for B = cstr and `(none)` short-circuit. Two coordinated fixes landed (both
>   resolved/archived):
>   ([kind-check](../archive/m7-hkt-fn-returning-applied-type-kind-mismatch.md))
>   thread the class param kinds through `parse_typeclass_method` so an HKT param
>   `^m` resolves to a KIND_ARROW TY_TYVAR even nested inside a fn type; and
>   ([layer-4 emit](../archive/m7-hkt-bind-body-byvalue-emit.md)) `m7_body_constructs_byvalue`
>   now admits a tail call to a LOCAL fn returning the `(f b)` family (bind's
>   `(k (.value ma))`), and the HKT class var is bound to the receiver's
>   constructor HEAD (`Option`) so `(m b)` resolves to the by-value
>   `Option__int`. Remaining for the monadic family: a continuation passed as a
>   bare `:fn` poly/fat-closure carrier (the probe's is a typed lambda with an
>   explicit `: (Option b)` return) -- a further follow-on.
> - **Applicative `ap` shape: DONE end-to-end (2026-06-18).** Probe at
>   `docs/upcoming/v2/m7-hkt-probe-ap.tur` exits 42 under the flag.
>   `ap [ff : (f (fn [a] b)) fa : (f a)] : (f b)` is the first shape whose result
>   element `b` is reachable only THROUGH a wrapped function value, so it needed
>   the "fat-closure carrier" follow-on resolved. Two coordinated fixes landed
>   (report archived to
>   [`docs/archive/m7-hkt-ap-fn-element-carrier-erasure.md`](../archive/m7-hkt-ap-fn-element-carrier-erasure.md)):
>   - **Fix direction 3 (defensive guard):** a residual free result element
>     tyvar aborts the by-value HKT monomorphization and falls back to carrier
>     dispatch (`m7_type_has_free_tyvar` + `m7_byvalue_grounded` in
>     `elab_typeclasses.c`), instead of emitting a half-by-value spec with a
>     dangling carrier-base dict reference. This both converted the original cc
>     error into a clean diagnostic AND backstops the genuinely-uninferable
>     `(ap (none) (some _))` case (no function anywhere -> `b` undetermined ->
>     clean `(type-app ? ?)` error, not a miscompile).
>   - **Fix direction 1 (preserve the fn type), three pieces:** (1) the producer
>     `EX_FN_TO_FAT` shim keeps the precise `(fn [int] int)` signature (marked
>     `boxed`) on its static type instead of erasing to `ptr<void>`, so
>     `(some add1) : (Option (fn [int] int))` (`elab_call.c`); (2) the call site
>     then recovers `b` via `m7_collect_tyvar_bindings` and grounds `(f b)` ->
>     `(Option int)`; (3) the instance body param's element fn is marked `boxed`
>     (`m7_box_hkt_element_fns` on `elab_param_type`) so `((.value ff) x)`
>     fat-dispatches through the box thunk rather than bare-calling the box
>     address. All three flag-gated.
>   Verified `b = int` (-> 42) and `b = cstr` (the wrapped fn's RETURN type
>   drives `b`). Inert flag-off (suite 1683/0, byte-identical codegen) and inert
>   flag-on for the grounded `fmap`/`bind` shapes; the 92-fixture HKT/typeclass
>   flag-on codegen sweep is unchanged vs. the parent commit (the fix only
>   enables the previously-broken `ap` shape).

> **Gating note (2026-06-18, superseded in part by the 2026-06-19 update above):**
> the subset of these helpers that are **HKT
> instance method bodies** (`fmap`/`ap`/`bind`/`pure` for Option, Parser, Goal,
> Backtrack, Schema, ...) CANNOT be rewritten to pure-Turmeric until Phase 3.0
> lands -- the method body has no element-type tyvar in scope, so a by-value
> rewrite (`(option-map container fn)`) fails to typecheck (`container : Option`,
> not `(Option A)`). Proven 2026-06-18. (3.0 + layer-4 have since landed
> flag-gated; the remaining blocker for the monadic shapes is the kind-system
> prerequisite above, not the element-type scoping.) Non-HKT carrier helpers
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
