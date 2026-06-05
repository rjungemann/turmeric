---
title: `Category` / `ArrowZero` Implementation in `stdlib/arrow.tur`
category: Planning
description: Add a `Category` typeclass with `ident` and an `ArrowZero` instance surface to `stdlib/arrow.tur`, reversing the T5/T9 deferrals from the arrow-typeclass reintroduction. Dispatch is unblocked by PR #261 (return-type dispatch for nullary arrow methods); this plan lands the typeclass declarations, the `(->)`-or-equivalent instances, and the fixtures that exercise them.
---

# `Category` / `ArrowZero` Implementation -- Plan

## Why

Two methods on the planned arrow hierarchy are still missing from
`stdlib/arrow.tur`:

- **`Category.ident`** -- the identity arrow. `Category` was deferred entirely
  in T9 of [[stdlib-arrow-typeclass-reintroduction-plan]] ("`Category` does not
  exist in stdlib, so composition is inlined into `Arrow`").
- **`ArrowZero.zeroArrow`** -- the zero arrow. The class is declared but T5
  chose Option A: no `(->)` instance, because the function arrow has no
  generic zero.

Both decisions were made when the **dispatch mechanism for nullary arrow
methods did not exist**. PR #261
([[return-type-dispatch-nullary-arrow-methods-plan]], merged 2026-06-05) now
resolves them via Mechanism B (unique-instance fallback in
`elab_try_return_dispatch`) -- proven by `tests/fixtures/arrow-instance-nullary`.

With dispatch unblocked, the typeclass surface is incomplete without these two
methods. Downstream consumers (notably `stdlib/arrow.tur`'s own composition
laws, and any future Kleisli-arrow or SF-arrow instance) need a real
`Category` head, and `ArrowZero` is part of the documented hierarchy in
`docs/guides/arrows-guide.md`.

### Relation to tur-signal -- the v1 gate

tur-signal is **the** gate by which the language either ships v1 or gets
shelved. Every stdlib commitment in this plan is justified by, and bounded by,
what tur-signal's combinator surface actually needs. This plan is not a
"complete the hierarchy for its own sake" exercise -- it implements `Category`
and `ArrowZero` **because and only insofar as tur-signal exercises them**.

The earlier TEMP.md note that suggested deferring on the basis of tur-signal
not touching these methods had the polarity right but the conclusion wrong:
the right move is not to defer, it is to **audit tur-signal first**, then
implement the exact instance shape tur-signal calls. T0 below is that audit
and it gates everything after it. If tur-signal needs only `ident`,
`ArrowZero` ships as a sequel; if tur-signal needs neither, this plan
collapses to a no-op and the question goes back to TEMP.md as resolved-by-audit.

## Current state (2026-06-05)

- `stdlib/arrow.tur` has six `defclass` forms (`Arrow`, `ArrowZero`,
  `ArrowPlus`, `ArrowChoice`, `ArrowLoop`, `ArrowApply`) and four `(->)`
  instances (`Arrow`, `ArrowChoice`, `ArrowLoop`, `ArrowApply`).
- No `Category` class exists.
- `ArrowZero` is declared (with `zeroArrow :: arr a b`) but has zero instances.
- `ident` is implemented as a bare combinator -- no dispatch surface.
- `tests/fixtures/arrow-instance-nullary` proves nullary return-type dispatch
  works end-to-end (`(let [i (ident)] (i 41))` -> `41`,
  `(let [h (comp (ident) add1)] (h 41))` -> `42`).

## Design

### D1 -- `Category` typeclass

Add to `stdlib/arrow.tur`:

```turmeric
(defclass Category [arr]
  (ident : (arr a a))
  (>>> : (arr a b) -> (arr b c) -> (arr a c)))
```

Two methods is the canonical surface; `>>>` is already on `Arrow`. Choose one:

- **D1a (preferred):** `Category` owns `>>>`; `Arrow` extends `Category` and
  inherits it. This matches Haskell's hierarchy and avoids method-name
  collision via the namespace-sharing fix already landed
  ([[typeclass-methods-share-value-namespace-with-defns]]).
- **D1b (fallback):** `Category` only declares `ident`; `>>>` stays on `Arrow`.
  Less faithful to the hierarchy but avoids touching `Arrow`'s declaration.

T2 below evaluates which is reachable without breaking the 1482-fixture suite.

### D2 -- `Category [(->)]` instance

```turmeric
(definstance Category [(->)]
  (defn ident [] : (fn [:a] :a)
    (fn [x] x))
  (defn >>> [f g] : (fn [:a] :c)
    (fn [x] (g (f x)))))
```

Nullary `ident` relies on the Mechanism-B fallback from PR #261. The
eta-expanded `(fn [x] x)` form is required by the same constraint flagged in
[[instance-method-returning-untyped-param-loses-result-type]] -- a bare
`identity` reference would lose its result type for the dispatched call.

### D3 -- `ArrowZero` instance strategy

T5's "(->) has no generic zero" objection is **mathematically correct** for a
truly polymorphic `zeroArrow :: a -> b` (no inhabitant exists for arbitrary
`b`). Three viable framings:

- **D3a (constrained instance):** `ArrowZero [(->)]` *with* a `Default b`
  constraint, returning `(fn [_] (default-of))`. This piggybacks on the
  existing `default-of` typeclass and is the only honest `(->)` zero.
- **D3b (Maybe-arrow instance):** introduce a `MaybeArrow` newtype
  (`a -> Option b`) and put `ArrowZero` there, with `zeroArrow = (fn [_] none)`.
  Honest, but requires a new arrow type and exits scope.
- **D3c (panic stub):** `(fn [_] (panic! "zeroArrow on (->) has no inhabitant"))`.
  Documents the hole at the type level. **Rejected** -- silently runtime-failing
  stdlib is a footgun.

Plan picks **D3a**. The `Default` constraint is already in stdlib; the
`ArrowZero [(->)]` instance becomes:

```turmeric
(definstance (ArrowZero [(->)]) (where [Default b])
  (defn zeroArrow [] : (fn [:a] :b)
    (fn [_] (default-of))))
```

If the typeclass front-end does not yet support method-level constraints on a
class-head instance, T3 below escalates to a small front-end task or falls back
to D3b.

### D4 -- Composition consistency

If D1a is chosen, the existing `Arrow [(->)]` instance's `>>>` method moves to
the new `Category [(->)]` instance. The bare `>>>` defn in `stdlib/arrow.tur`
stays untouched (namespace-sharing fix); existing call sites continue to
resolve via dispatch.

## Tasks

### T0 -- tur-signal audit (gates every later task)
- Grep `../turmeric-spices/tur-signal/` for `ident`, `zeroArrow`, `Category`,
  `ArrowZero`, and any composition pattern that *should* call `ident` (e.g.
  identity-element folds, neutral-element initialisers, `compose-float` seeds).
  Capture the call sites and their concrete arrow types.
- Cross-check against the v1 scope in
  [[tur-signal-rebuild-plan]] -- specifically the Tier 1 combinator list and
  the `compose.tur` design.
- Record findings in an `Audit` section appended to this plan before
  proceeding. The audit decides:
  - **(a) tur-signal calls `ident`** -> T2/T3 proceed as written.
  - **(b) tur-signal calls `zeroArrow`** -> T4 proceeds; otherwise T4 is
    deferred to a sequel plan and `ArrowZero [(->)]` does **not** ship in this
    PR.
  - **(c) tur-signal calls neither** -> close this plan as resolved-by-audit;
    file a one-line note in `docs/reported/` recording the decision and revert
    the TEMP.md entry to closed. Do **not** ship typeclass surface area
    tur-signal does not exercise.
- The audit is load-bearing -- no instance shape gets locked in before it.
  The concrete arrow type tur-signal uses (raw `(->)`, an `SF` newtype, etc.)
  dictates the instance head in T3/T4 and may force D3b over D3a.

### T1 -- Prerequisite checks
- Confirm `tests/fixtures/arrow-instance-nullary` passes on a clean
  Debug build.
- Confirm the namespace-sharing fix is in the tree (a free `ident` defn
  and a `Category.ident` method can coexist without an E-code).
- Confirm `default-of` typeclass + `Default` constraint syntax for instances
  (read `stdlib/default.tur`; if instance-level constraints are unsupported,
  raise a finding before T3).

### T2 -- `Category` class declaration
- Pick D1a vs. D1b based on whether moving `>>>` off `Arrow` keeps all
  arrow fixtures green. Spike: rename + run
  `bash tests/run.sh 2>&1 | grep "^FAIL"`. If clean, commit D1a; otherwise
  D1b.
- Add `;;;` docstrings on the class and both methods.

### T3 -- `Category [(->)]` instance
- Add the instance per D2.
- Regenerate `stdlib/docstrings.tur` and `docs/api/` via `tur run docs`.

### T4 -- `ArrowZero [(->)]` instance (D3a)
- Add the constrained instance per D3.
- If the elaborator rejects method-level `Default` constraints, file a
  `docs/reported/` note and switch to D3b as a follow-up plan -- do **not**
  fall back to D3c.

### T5 -- Fixtures
- `tests/fixtures/category-instance-basic` (stdout-only):
  - `(let [i (ident)] (i 41))` -> `41` (regression for the nullary case).
  - `(let [h (>>> (ident) add1)] (h 41))` -> `42` (Category composition).
  - `(let [h (>>> add1 (ident))] (h 41))` -> `42` (identity-right law).
- `tests/fixtures/arrowzero-instance-default-int` (stdout-only):
  - `(let [z (zeroArrow)] (z 99))` -> `0` (`Default Int` is `0`).
  - Ascribed variant `(:: (zeroArrow) (fn [:int] :int))` for the
    expected-type path.
- A negative-error fixture for `(zeroArrow)` at a type with **no** `Default`
  instance, if the suite supports expected-error fixtures.

### T6 -- Full suite + doc updates
- `bash tests/run.sh 2>&1 | grep "^FAIL"` must be empty (target: ~1484 passed).
- Update `docs/guides/arrows-guide.md`: add `Category` section, replace the
  "ArrowZero is declared but not instantiated at (->)" note with the new
  constrained-instance documentation.
- Update [[stdlib-arrow-typeclass-reintroduction-plan]]: mark T5 and T9 as
  superseded by this plan; add a "Follow-up landed" entry pointing here.
- Archive this plan to `docs/archive/` once landed and one release has passed.

## Edge cases

- **`(ident)` with no expected type and multiple `Category` instances.** Once a
  second instance lands (e.g. for a Kleisli or SF arrow), the Mechanism-B
  uniqueness gate fails and the existing ambiguity error fires -- correct, and
  resolvable by ascription.
- **`(zeroArrow)` with `Default` constraint at a polymorphic call site.** The
  constraint must be solvable at the call site; otherwise the elaborator must
  emit the existing "cannot infer" diagnostic, not silently pick a default.
- **`>>> (ident) (ident)`**. Should resolve to `(ident)`. The fixture above
  covers it indirectly; consider adding `(>>> (ident) (ident)) 41 -> 41` if
  T2 picks D1a.

## Risks

- **Hierarchy churn (D1a).** Moving `>>>` off `Arrow` touches every existing
  arrow fixture's instance declaration. The 1482-fixture suite is the guard
  rail; if regressions appear that aren't trivial renames, fall back to D1b
  rather than chasing.
- **Default-constrained instance unsupported.** If T4 hits an elaborator gap,
  filing a finding and shipping `Category` alone (T2/T3/T5 partial) is
  acceptable -- `ArrowZero` becomes a sequel plan. Do not regress to D3c.
- **Snapshot churn.** Adding two `definstance` forms regenerates the
  typeclass-dispatch snapshots. Regenerate fixture snapshots per CLAUDE.md's
  "Fixture Snapshots -- STRICT RULE" and commit alongside the code change.

## Out of scope

- A Kleisli or SF `Category` instance (sequel; needed before ambiguity
  pressure becomes real).
- `ArrowZero` instances at non-`(->)` arrows (D3b is a separate plan if needed).
- Revisiting the bare-combinator `ident` in `stdlib/arrow.tur` -- it stays
  alongside the dispatched method per the namespace-sharing model.

## Validation summary

- `tests/fixtures/category-instance-basic` and
  `tests/fixtures/arrowzero-instance-default-int` green.
- `tests/fixtures/arrow-instance-nullary` and
  `tests/fixtures/arrow-instance-basic` snapshot-stable.
- `bash tests/run.sh 2>&1 | grep "^FAIL"` empty.
- `docs/guides/arrows-guide.md` and `stdlib/docstrings.tur` regenerated.

## Audit (T0) -- 2026-06-05: resolved-by-audit, outcome (c)

T0 is the load-bearing gate: no instance shape is locked in until the audit
decides whether tur-signal actually exercises `ident` and/or `zeroArrow`. The
audit was run on 2026-06-05. **Outcome: (c) -- tur-signal calls neither.** This
plan therefore collapses to a no-op for stdlib; `Category`/`ArrowZero [(->)]`
do **not** ship. No `definstance`/`defclass` was added to `stdlib/arrow.tur`.

### Sources consulted

- **`docs/upcoming/tur-signal-rebuild-plan.md`** -- the authoritative v1 scope
  for tur-signal. Its Tier 1 surface table (lines 327-356, "the rebuild ships
  when this works") is the contract for what the spice's combinator surface
  must support.
- **`stdlib/arrow.tur`** -- current instance/method surface.
- **`stdlib/`** (grep) -- for the `Default` typeclass D3a depends on.

> Caveat: the sibling `../turmeric-spices/tur-signal/` checkout is **absent**
> in this container (it is `requires.spices`-gated), so the source-level grep
> called for in T0 could not be run directly. The cross-check was made against
> the rebuild plan's Tier 1 table, which is the design of record for what
> tur-signal v1 ships and is sufficient to settle the gate. If the source ever
> diverges from that table to introduce a polymorphic `ident`/`zeroArrow` call,
> reopen this plan.

### Findings

1. **Composition is `compose-float`, not the typeclass.** tur-signal's only
   composition combinator, `effects-chain` (Tier 1, `compose` module), is
   specified as "composition of `Vec<SF Sample Sample>`" implemented via
   `stdlib/arrow.tur`'s **`compose-float`** -- the bare `:float -> :float`
   combinator (rebuild plan lines 141, 172-173, 356). It does not call the
   `Category`/`Arrow` typeclass method `>>>`/`comp`, so no `ident` neutral
   element is threaded through dispatch.

2. **The "identity" arrows it needs are concrete, not polymorphic.** Tier 1
   identity-shaped symbols (`time-signal`, `invert`, etc.) are concrete
   `:float -> :float` functions -- a literal `(fn [x : float] : float x)` --
   not the polymorphic `Category.ident :: arr a a`. An empty-`Vec`
   `effects-chain` seed, if needed, is likewise a concrete float identity, not
   `(ident)`.

3. **No `zeroArrow` consumer.** Nothing in the Tier 1 table (oscillators,
   filters, shapers, envelope, compose) produces or requires a zero/empty
   arrow. `ArrowZero` has no call site.

4. **D3a was unbuildable anyway.** D3a's `(definstance (ArrowZero [(->)])
   (where [Default b]) ...)` depends on a `Default` typeclass / `default-of`
   method. Grepping `stdlib/` finds **no `Default` class and no `default-of`**.
   So even had the audit gone the other way, T4 would have hit the
   "Default-constrained instance unsupported" risk immediately and fallen back
   to a sequel plan.

### Decision (audit recommendation -- subsequently overridden)

The audit's own recommendation, per T0(c), was to close as
**resolved-by-audit**: don't ship typeclass surface tur-signal doesn't
exercise. **The maintainer explicitly overrode this on 2026-06-05** and directed
shipping the surface anyway, with a worked Kleisli instance as the example
consumer. The implementation below records what actually landed; the audit
stands as the rationale for *why the (->) `ArrowZero` is not honest* and why the
example uses Kleisli instead.

## Implementation (2026-06-05) -- shipped under maintainer override

The plan was executed (not closed). What landed:

### Category (T2/T3) -- `stdlib/arrow.tur`

- `(defclass Category [arr] (ident [] : arr) (comp [f g] : arr))`. Chose **D1b**
  over D1a: `Category` declares its own `comp` rather than moving `>>>` off
  `Arrow`. This is the lowest-risk choice -- it avoids the operator-mangling
  collision and the hierarchy churn D1a's `>>>`-move would impose on every
  existing arrow fixture -- and it mirrors the exact shape proven by
  `tests/fixtures/arrow-instance-nullary` (`ident` + `comp`).
- `(definstance Category [(->)] (ident [] (fn [x] x)) (comp [f g] (fn [x] (g (f x)))))`.
  The eta-expanded `(fn [x] x)` keeps the dispatched nullary call's result type
  concrete (per `instance-method-returning-untyped-param-loses-result-type`).

### ArrowZero, honestly (T4 via D3b, not D3a) -- `stdlib/kleisli.tur` (new)

D3a (`ArrowZero [(->)]` constrained on `Default b`) was **abandoned**: grepping
`stdlib/` confirms there is **no `Default` typeclass / `default-of`** to
constrain on, so D3a was unbuildable. Rather than fall back to the rejected D3c
panic stub, the implementation takes the **D3b** route the plan had parked as a
separate effort, because it yields a genuinely honest zero:

- `(defopaque Kleisli [A B] :int)` -- `Kleisli A B = A -> Option B`, carrier an
  int-as-pointer fat closure (the `stdlib/backtrack.tur` handle convention).
- `Category [Kleisli]`: `ident == \a -> some a`;
  `comp f g == \a -> f a >>= g` (Option-monad bind; `none` short-circuits).
- `ArrowZero [Kleisli]`: `zero-arrow == \_ -> none` -- the inhabitant `(->)`
  cannot provide. This is why `ArrowZero` is instantiated at Kleisli and still
  **not** at `(->)`, vindicating the original T5 decision.

The existing `ArrowZero` class in `stdlib/arrow.tur` (method `zero-arrow`, not
the plan's `zeroArrow`) is reused as-is.

### Fixtures (T5)

- `tests/fixtures/category-instance-basic` -- `(->)` identity + left/right
  identity laws of `comp`. Single `Category` instance in scope, so bare
  `(ident)` resolves via the unique-instance fallback (no ascription).
- `tests/fixtures/kleisli-arrow-instance` -- two `Category` instances in scope:
  exercises Kleisli identity, composition with `none` short-circuit, and the
  honest `zero-arrow`. The nullary `(ident)`/`(zero-arrow)` are ascribed to
  `:Kleisli` to resolve the (expected) two-instance ambiguity.

### Defect found and reported

Implementing the ascription-disambiguated fixtures surfaced a real linearity
defect: the opaque ascription cast `(:: expr :Kleisli)` marked its result
move-once even though `Kleisli` is an unrestricted opaque. **Root-caused and
fixed** in `src/compiler/elab_types.c` (the `F_KEYWORD` type path hardcoded
`copy_kind = CK_MOVE` for known opaques instead of carrying the def's
discipline like the `F_SYM` path). Report:
`docs/reported/opaque-ascription-cast-marks-value-move-once.md`. The
`kleisli-arrow-instance` fixture now reuses the ascribed arrows directly, and
`tests/fixtures/opaque-ascription-copy-reuse` is a focused regression.

### Not done (deliberately)

- **D1a** (`Category` owns `>>>`, `Arrow` extends it) -- skipped for D1b per
  above.
- **`ArrowZero [(->)]`** -- remains unshipped (no honest inhabitant; no `Default`
  typeclass). The Kleisli instance is the honest home for `zero-arrow`.
- Updating the reintroduction plan's T5/T9 status -- T5's "no `(->)` `ArrowZero`"
  decision still holds; T9's "no `Category`" is now superseded (a `Category`
  class exists). See that plan's follow-up note.

The earlier `docs/reported/category-arrowzero-resolved-by-audit.md` tombstone is
updated to point here.
