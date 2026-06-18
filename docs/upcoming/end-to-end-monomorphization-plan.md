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

Five phases. **Phase 1 is independent** of the rest and can ship in
parallel; **Phases 2-5 are sequential** (HKT design -> HKT impl ->
carrier-helper rewrites -> bridge delete -> re-audit).

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

- [ ] Add `(defopaque List [A] :int)` and a `list-of [A] [& xs : A] : (List A)`
      smart constructor in `stdlib/list.tur` (or a new `stdlib/list-typed.tur`
      if the existing module's untyped surface should stay intact).
- [ ] Retype the NonEmpty surface in `stdlib/refined.tur`:
      `ne-from? [A] [xs : (List A)] : (Option (NonEmpty A))`,
      `ne-unwrap [A] [o : (Option (NonEmpty A))] : (NonEmpty A)`,
      `ne-head`/`ne-tail`/`ne->list` all on `(NonEmpty A)` / `(List A)`.
- [ ] Update `tests/fixtures/refined-nonempty/` to drop
      `(:: o (Option int))` ascriptions; the element type must resolve
      at `(ne-head ...)` without annotation.
- [ ] **Validation:** `bash tests/run.sh 2>&1 | grep '^FAIL'` empty.
- [ ] **Validation:** `TUR_M3_AUDIT=1 ./build/tur build
      tests/fixtures/refined-nonempty/input.tur 2>&1 | grep m3-audit`
      shows zero crossings.
- [ ] Archive `docs/reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md`
      with resolution note.

### 1.2 -- Retype `result-map` and decouple from its regression fixture

The current `:int` signature is backed by a deliberate carrier-ABI
regression test. The retype has to land alongside a *new* fixture that
keeps the carrier path covered explicitly (so removing the implicit
coverage from `result-map` is safe).

- [ ] Identify the fixture(s) whose pass depends on `result-map`'s
      current `:int` signature -- `grep -rn 'result-map' tests/fixtures/`
      and read each to confirm which one is the carrier-ABI regression.
- [ ] Synthesize a replacement fixture that exercises the same
      carrier-bridge path *without* going through `result-map`
      (e.g. via a small stdlib helper that intentionally stays inline-C
      with `;;` NOTE). File under `tests/fixtures/carrier-bridge-regression-<topic>/`.
- [ ] Retype `result-map` in `stdlib/result.tur` to
      `[A B E] [r : (Result A E) f : (fn [A] B)] : (Result B E)` with a
      pure-Turmeric body (`if (.is-ok r) (ok (f (.ok-val r))) (err (.err-val r))`).
- [ ] **Validation:** `bash tests/run.sh` clean.
- [ ] Strike `result-map` from the "Remaining" list in
      `docs/reported/option-consumer-retype-byvalue.md`.

### 1.3 -- Retype `unwrap-or` (the ~10-module cascade)

Per
[`docs/reported/unwrap-or-byvalue-cascade.md`](../reported/unwrap-or-byvalue-cascade.md)
(authoritative spec; one-PR-per-row migration via temporary
`unwrap-or-carrier` shim). Cascade spans ~10 stdlib modules
(`zipper`, `seq/*`, `json`, `safe`, `env`, `serial`, ...).

- [ ] **Pre-task:** generate the actual list of call sites:
      `grep -rn '(unwrap-or' stdlib/ ../turmeric-spices/spices/ > /tmp/unwrap-or-sites.txt`.
      The "~10 modules" claim is unverified; replace with the empirical
      list and update this checkbox with the count before estimating.
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

- [ ] After 1.1-1.3 land, re-read `stdlib/kleisli.tur` and identify the
      remaining producers that still hand `comp`/`k-apply-raw` a carrier
      Option. If the list is empty, retype directly.
- [ ] Retype `comp` / `k-apply-raw` to by-value `(Option A)` parameters.
- [ ] **Validation:** `bash tests/run.sh` clean; `tests/fixtures/kleisli-*`
      all green.
- [ ] Strike step 5 from `option-consumer-retype-byvalue.md`.
- [ ] If everything in `option-consumer-retype-byvalue.md`'s Remaining
      list is struck, archive it.

### 1.5 -- Bucket C exit criterion

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

- [ ] Build the per-`(f, A)` monomorphization estimate for option 1
      (full per-(f, A) clone): enumerate every `(combinator, f, A)`
      tuple reachable from stdlib + `../turmeric-spices/spices/` and
      count distinct clones the elaborator would emit.
- [ ] Compare against option 2 (dict-passing with payload erasure at
      the HKT boundary) and option 3 (source-rewriting inline-expansion):
      for each, estimate generated-C delta and which HKT features
      regress (recursion, polymorphic recursion, etc.).
- [ ] Deliverable: `docs/upcoming/v2/hkt-dispatch-options-tradeoff.md`
      with the three options scored on binary-size, compile-time,
      expressiveness regression. Numbers, not adjectives.

### 2.2 -- Pick a model and pin acceptance criteria

- [ ] Choose option 1 / 2 / 3 (default: option 1 unless 2.1's binary
      estimate is > 2x larger). Record the decision and reasoning in
      the tradeoff doc.
- [ ] Specify the new dispatch ABI shape concretely: what does
      `Functor [Option]`'s dict look like in C? What does `lift2` look
      like for option 2? For option 3, what does an inlined `fmap (fmap f) x`
      look like at the call site?
- [ ] Identify which existing HKT call sites need to change vs. stay
      source-stable (target: zero source-side changes).

### 2.3 -- Phase 2 done when

- [ ] Tradeoff doc landed; design choice recorded; no implementation
      questions remain that would change the deliverable. Phase 3 below
      can be sized in sessions.

---

## Phase 3 -- M7: HKT class dispatch implementation

**Largest single phase.** Implement whatever Phase 2 picked. Tasks
below are skeleton; flesh out per Phase 2's design doc.

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

- [ ] Grep `stdlib/` for inline-C blocks that take `int64_t` and back
      a typeclass method: `grep -rn 'tur_is_some\|tur_opt_value\|vec_eq\|map_eq\|set_eq\|list_eq' stdlib/`.
- [ ] Walk the result; for each helper, identify the `definstance`
      that consumes it and which typeclass method it backs.
- [ ] Deliverable: a checklist of helpers (`vec-eq?`, `map-eq?`,
      `mutmap-eq?`, `option-eq?` [done], `set-eq?`, `list-eq?`,
      `vec-fmap`, `map-fmap`, ...) with their consuming instances.

### 4.2 -- Rewrite each helper in pure Turmeric (or accept it as
genuine carrier)

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
