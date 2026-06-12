---
title: defsystem `:writes` list is collected but not enforced -- writes outside the set still compile
category: Reported
severity: Missing the load-bearing v1 win the ECS plan promises (the spec'd "single biggest delta vs. Haskell ECSes")
discovered: 2026-06-11, while auditing what's actually shipped against `docs/upcoming/ecs-spice-plan.md` after closing gaps A-H
location: `../turmeric-spices/spices/ecs/src/ecs/system.tur` -- the `defsystem` macro
---

# `defsystem` `:writes` list is collected but not enforced -- writes outside the set still compile

## Summary

The ECS plan ([`../upcoming/ecs-spice-plan.md`](../upcoming/ecs-spice-plan.md))
markets static `:reads`/`:writes` capabilities as the load-bearing v1
feature -- "the single biggest delta vs. Haskell ECSes" -- and claims
that the elaborator gates `set-X!` access on `:writes` membership at
compile time:

> Mis-declaring (writing to a component you didn't list) is a
> compile-time error because the elaborator only exposes `set-X!`
> capabilities for `X` in `:writes`.
>
> -- *ecs-spice-plan.md*, § "Systems and scheduling"

> **Static read/write effects.** `:reads`/`:writes` become first-class
> substructural capabilities (we have `-Xsubstructural` shipping
> today). apecs's `cmap` does not have this; it trusts the programmer.
> ... **This is the single biggest delta vs. Haskell ECSes and it
> needs zero new type-system work.**
>
> -- *ecs-spice-plan.md*, § "Where Turmeric's types pull ahead" item 3

> **E2 -- systems and scheduler (3-4 days):** ... Substructural
> capabilities back the write access -- this is the load-bearing v1
> win and it works today.
>
> -- *ecs-spice-plan.md*, § "Phasing v1 track" E2

The shipped `defsystem` macro ([`ecs/system.tur`](../../../turmeric-spices/spices/ecs/src/ecs/system.tur))
collects the `:reads` and `:writes` lists as runtime bitmasks used by
the scheduler for wave assignment, but does **not** gate `set-X!` /
`dense-set!` access by those masks. A system declaring `:writes
[Pos]` and writing to `Vel` compiles and runs successfully. The
runtime conflict-prevention scheduler still works (waves group
systems by mask disjointness), but the compile-time
mis-declaration-is-an-error claim is not honored.

## Severity

This is not a correctness bug in the shipped surface -- everything
that does compile runs correctly. It's a *missing feature* that the
plan promises as the headline E2 deliverable. Earlier summary tables
in this work stream labeled it as "elaborator (plan-deferred)" which
was incorrect: the plan does NOT defer it. The plan explicitly says
it needs **zero new type-system work** and is **load-bearing**.

The practical fallout:

- The ECS plan's empirical comparison story (`docs/guides/ecs-vs-haskell-ecs.md`,
  scheduled for E4) currently has a hole. The plan column claims
  compile-time write-cap enforcement; the spice doesn't deliver it.
- Library systems that take a world by typeclass bound (HasComponent
  classes, shipped this session) can write to *any* component the
  world carries. There is no static way to declare "this system only
  touches Pos."
- Users porting from apecs / aztecs / Bevy expect substructural
  capability checking to land at compile time (the typeclass-bounded
  systems demo in the spice plan reads as if it does); they find the
  same trust-the-programmer model as apecs.

## Concrete failure (negative repro)

```turmeric
(import ecs/system :refer [cid-bit defsystem])
(import ecs/storage :refer [dense-set!])

(def pos-cid 0)
(def vel-cid 1)

;; Declares :reads [Pos] and :writes [Pos] -- a "Pos-only" mutator.
(defsystem rogue
  (cid-bit pos-cid)   ;; reads-mask:  Pos
  (cid-bit pos-cid)   ;; writes-mask: Pos only -- the user has DECLARED
                      ;;                          that this system does not
                      ;;                          touch Vel
  (do
    (dense-set! (.Pos w) 0 100)   ;; OK -- Pos is in :writes
    (dense-set! (.Vel w) 0 999)   ;; THIS SHOULD BE A COMPILE-TIME ERROR
                                  ;; (Vel is not in :writes)
                                  ;;
                                  ;; Currently this compiles and runs,
                                  ;; silently violating the system's own
                                  ;; declared write set. The scheduler
                                  ;; assumes the declaration is correct
                                  ;; and may run this system in parallel
                                  ;; with another system that also touches
                                  ;; Vel -- producing data races at runtime
                                  ;; that the static analysis was supposed
                                  ;; to prevent.
    ))
```

This compiles cleanly and runs without warning. There is no diagnostic.

## Observed vs. expected

Observed: `(dense-set! (.Vel w) 0 999)` inside a system declared
`:writes (cid-bit pos-cid)` compiles. The runtime scheduler trusts
the declaration; if the user lies, races happen at runtime.

Expected per the plan: a compile-time error -- something like
`error: system 'rogue' writes to component 'Vel' which is not in its
:writes list`.

## Why this hasn't been delivered yet

The `defsystem` macro currently shipped lowers to two top-level forms
(gap E + gap A machinery):

```turmeric
(defsystem physics READS WRITES body)
  =>
(do
  (defn physics-impl [w : int] : nil body)
  (def  physics      (make-system READS WRITES physics-impl)))
```

The body becomes the body of an ordinary defn. There is no
capability-passing, no scope-shadowing, no elaborator post-pass.
The elaborator sees an ordinary defn that calls `dense-set!` -- a
generic `:reads/:writes`-agnostic primitive -- with arbitrary storage
arguments. There is no information the elaborator can use to gate
the call.

So the plan's "needs zero new type-system work" claim is true *only*
if the spice rewrites the user-facing API to thread the substructural
capabilities through the body. The compiler features (`-Xsubstructural`,
`^linear`, `^affine`, the lifetime / borrow machinery) ship today;
the SPICE-SIDE work to use them is the missing piece.

## Three implementation paths, ranked by realism

### Path A -- per-component write capabilities, scoped by `defsystem` (recommended)

Mint a `WriteCap[Pos]` linear capability type per component. The
`defsystem` macro inspects `:writes` and binds an instance of each
listed capability into the body's scope at lowering time:

```turmeric
(defsystem physics
  [Pos Vel]                ;; :reads as a list of comp names
  [Pos]                    ;; :writes as a list of comp names
  body)
  =>
(do
  (defn physics-impl [^linear pos-write-cap : WriteCap<Pos>
                     ^borrow  w             : int]
    : nil
    body)
  (def  physics (make-system ... physics-impl)))
```

Each component's `set-X!` requires the corresponding `WriteCap<X>` in
scope. The body that uses `(.Pos w)` writes via `set-Pos!` which
consumes the `pos-write-cap` (using `^linear` so it's used exactly
once per call, but per-iteration re-borrowed via a small wrapper).
A body that tries `(set-Vel! w e v)` fails because no `vel-write-cap`
is in scope.

Costs the user a syntax change: `:writes` becomes a list of component
names, not a bitmask integer. The current bitmask API stays for the
scheduler but is computed internally. `dense-set!` direct calls
(raw, bypassing per-component accessors) need to be either banned in
system bodies, or also routed through a capability.

This is the path most consistent with the plan's "first-class
substructural capabilities" framing.

### Path B -- elaborator post-pass scanning system bodies

Add a new elaborator pass: walk every `defsystem` body, collect the
set of `.Comp` field accesses on the world handle that are followed
by a `dense-set!` (or `sparse-set!`, `tag-set!`, ...) call, and
diff against the declared `:writes` list. Emit a diagnostic on
mismatch.

Smaller user-facing change (the spice API stays exactly as it is
today), but the pass is fragile -- any indirection (calling a helper
that wraps `dense-set!`, threading the storage handle through a
variable, etc.) defeats the analysis. The plan's "needs zero new
type-system work" claim is closer to true here, but the analysis
itself is custom-built per the ECS spice rather than reusing the
substructural machinery.

### Path C -- runtime check with a build-time flag

Wrap the storage handles in the world struct with a "write-check"
shim that aborts on a write to a non-listed component when a
`-DECS_DEBUG_CAPS` flag is set. Strip the shim in release builds.

This is what the plan disclaims (apecs's "trust the programmer"
model). Doesn't deliver the plan's compile-time promise but ships
quickly and catches the bug in test runs. Reserve for the case where
the spice goes to production without A or B landing.

## What this blocks downstream

- The ECS plan's empirical-comparison writeup (`docs/guides/ecs-vs-haskell-ecs.md`,
  E4). The plan's column for tur-ecs currently overclaims. E4 either
  has to mark write-cap enforcement as future work, or wait for A/B
  to land.
- The "library system over arbitrary worlds" use case the
  HasComponent typeclass-bounded systems work this session enabled.
  Library systems written by third parties can write to any
  component on the world they're applied to, with no way to declare
  "this is a read-only effect."
- Any apecs-grade demo that wants to make race-freedom claims based
  on the type system. The runtime scheduler's wave grouping is
  correct given correct declarations -- but it can't catch a user
  declaring `:writes [Pos]` and then writing Vel.

## Proposed fix direction

Path A. The substructural machinery ships today (`-Xsubstructural`,
`^linear`, `^affine`); the elaborator already enforces single-use of
linear values. A `WriteCap` is a `defopaque` linear type. The
`defsystem` macro mints one capability binding per `:writes` entry,
binds them in the body scope at lowering, and changes the user-
visible `:writes` slot from a bitmask to a component-name list (the
bitmask becomes an internal computation derived from the names).

Per-component `set-Comp!` (which `defcomponent-accessors` already
emits, gap G's payoff) is the natural place to require the
capability:

```turmeric
;; Old (gap-G version, no enforcement):
(defn set-Pos! [^borrow w : GameWorld e : int v : Pos] : nil
  (dense-set! (.Pos w) e v))

;; New (capability-gated):
(defn set-Pos! [^linear cap : WriteCap<Pos>
                ^borrow w   : GameWorld
                e           : int
                v           : Pos]
  : nil
  (do
    (use-cap! cap)
    (dense-set! (.Pos w) e v)))
```

Or sugar: a `(set! w Pos e v)` macro that pulls `Pos-write-cap` from
the surrounding scope by convention (the cap binding is named per
the macro's invariant). Misuse outside a `defsystem` body has no cap
in scope and fails to elaborate.

### Phased implementation plan (Path A)

The work is sized for sequential landing -- each phase is independently
shippable (tests green, no half-state in the spice surface). Phases I1-I2
are pure-additive: they add machinery without touching the user-visible
`defsystem` API. Phases I3-I4 flip the API and are where the breaking
change lands. Phases I5-I6 are validation and downstream cleanup.

#### Phase I1 -- Capability types + `use-cap!` primitive (foundational, ~0.5 day)

Goal: land the linear capability machinery in isolation, with no
`defsystem` changes yet. Nothing observable to spice users.

Tasks:

- [ ] Decide host file: extend `ecs/system.tur` vs. new `ecs/cap.tur`.
      Lean `ecs/cap.tur` -- `system.tur` is already the macro-heavy
      module and caps are conceptually orthogonal to scheduling.
- [ ] Define `WriteCap<T>` and `ReadCap<T>` as `:linear` opaque ints
      using `defopaque`. Both are zero-cost wrappers (the underlying
      int is the component CID, used only for diagnostics).
- [ ] Define `use-cap!` :: `^linear WriteCap<T> -> nil` -- consumes one
      capability. Implementation is a no-op at runtime; the elaborator's
      single-use check is what does the work.
- [ ] Define `borrow-cap` :: `^linear WriteCap<T> -> (^borrow WriteCap<T>, ^linear WriteCap<T>)`
      or equivalent re-borrow helper so a system body can iterate and
      re-use the cap each tick. Cross-check with
      `docs/guides/substructural-types-guide.md` for the idiomatic
      re-borrow pattern.
- [ ] Unit fixtures in the spice: `tests/cap-linear-single-use.tur`
      (passes); `tests/errors/cap-double-use.tur` (fails with
      TUR-E0005-style diagnostic).

Validation: `tur run test` in `../turmeric-spices/spices/ecs/` is green;
no main-repo fixture churn.

#### Phase I2 -- Per-component cap minting in `defcomponent-class-instance` (~0.5 day)

Goal: when a world declares it has a component, also mint the cap
binding. Still no `defsystem` change.

Tasks:

- [ ] Extend `defcomponent-class-instance GameWorld Pos` to also emit a
      `(def Pos-write-cap (make-write-cap pos-cid))` binding at the
      instance site. Naming convention: `<Comp>-write-cap` /
      `<Comp>-read-cap`.
- [ ] Add `make-write-cap` / `make-read-cap` constructors (private,
      not re-exported) that take a CID and return the opaque linear
      value. These are the only legal cap producers.
- [ ] Decide cap lifetime: per-world (minted once at world creation,
      re-borrowed each tick) vs. per-system-invocation (minted on
      entry to the system body). Lean per-world -- matches how the
      bitmask is computed today, and `^borrow` semantics give us the
      single-use-per-write guarantee inside the body.
- [ ] Audit all current `defcomponent-class-instance` usages in the
      ECS spice + downstream demos; confirm cap bindings don't collide
      with user names.

Validation: existing spice tests still green; no surface change yet.

#### Phase I3 -- New `defsystem` API: component-name vectors (~1 day, BREAKING)

Goal: flip `:reads`/`:writes` from bitmask integers to component-name
vectors and thread caps into the body scope. This is the user-visible
break.

Tasks:

- [ ] Rewrite the `defsystem` macro signature:
      `(defsystem name [reads-comps...] [writes-comps...] body)`.
      Old bitmask form removed -- no compat shim (see "Migration"
      below for why).
- [ ] In the macro lowering:
      - Compute `reads-mask` / `writes-mask` from the name vectors
        using the per-component CIDs.
      - Look up the cap binding for each name in `:writes`
        (`Pos-write-cap`, `Vel-write-cap`, ...) and bind a fresh
        `^linear` shadow at the top of the body's scope.
      - Same for `:reads` with `^borrow ReadCap<T>`.
      - Lower the rest of the body unchanged.
- [ ] Migrate all in-repo `defsystem` call sites in `../turmeric-spices/`
      to the new form. Grep: `rg 'defsystem' ../turmeric-spices/`.
- [ ] Update `ecs/system.tur` exports + the spice README example to the
      new syntax.

Validation: spice tests green under the new API; the scheduler still
groups waves by mask disjointness (unchanged behavior).

Migration note: no compat shim. The old bitmask form silently failed to
deliver enforcement, so leaving it callable would re-introduce the bug.
Hard-break with a clear deprecation diagnostic that names the new form.

#### Phase I4 -- Cap-gated `set-Comp!` / `get-Comp` accessors (~1 day)

Goal: make per-component accessors require the cap. This is what
actually enforces the write set.

Tasks:

- [ ] Rewrite `defcomponent-accessors` so each emitted `set-Comp!`
      takes `^linear cap : WriteCap<Comp>` as its first parameter.
      Inside the body: `(use-cap! cap)` then `(dense-set! ...)`.
- [ ] Same for `get-Comp` with `^borrow ReadCap<Comp>` (read-only,
      no consume).
- [ ] Decide policy on raw `dense-set!` / `sparse-set!` / `tag-set!`
      inside system bodies. Options:
      (a) ban via elaborator pass (small dedicated check),
      (b) leave callable but document as "escape hatch -- bypasses
          cap checking,"
      (c) make them take a cap parameter too.
      **Recommend (a)** -- the whole point of the phase is closing the
      hole; an escape hatch reopens it. Implementation: a one-pass
      walk of `defsystem` bodies rejecting bare storage-primitive calls.
- [ ] Add `(set! w Pos e v)` sugar macro that pulls `Pos-write-cap`
      from the surrounding scope by naming convention. Optional --
      can ship in I5 if it slips.
- [ ] Update all per-component accessor call sites in
      `../turmeric-spices/` and any in-tree demos.

Validation: the report's negative repro (declare `:writes [Pos]`,
write Vel) now fails to elaborate with a diagnostic naming Vel.

#### Phase I5 -- Tests, fixtures, diagnostics polish (~0.5 day)

Tasks:

- [ ] Spice integration test (positive): `defsystem` declaring
      `:writes [Pos]` and writing Pos elaborates + runs.
- [ ] Spice integration test (negative): `defsystem` declaring
      `:writes [Pos]` and writing Vel fails with
      `error: system 'rogue' writes to component 'Vel' which is not
      in its :writes list` (or equivalent capability-missing
      diagnostic). The diagnostic must name the offending component
      by source name, not by CID.
- [ ] Main-repo error fixture:
      `tests/fixtures/errors/ecs-defsystem-writes-unauthorized/`
      with `input.tur` + `expected.err` (the diagnostic) +
      `requires.spices` marker.
- [ ] Tighten the diagnostic if the default TUR-E0005 "linear value
      not in scope" message is too cryptic -- consider a dedicated
      `TUR-Ecaps` code with a "did you mean to add Vel to :writes?"
      hint.
- [ ] Full `bash tests/run.sh` clean (zero FAIL).

Validation: error fixture passes; full suite green; spice suite green.

#### Phase I6 -- Downstream unblocks + doc updates (~0.5 day)

Tasks:

- [ ] Update `docs/upcoming/ecs-spice-plan.md` § "Phasing v1 track E2"
      to point at the now-shipped enforcement instead of a future
      promise.
- [ ] Update `docs/guides/ecs-vs-haskell-ecs.md` (E4) -- the tur-ecs
      column can now honestly claim compile-time write-cap
      enforcement. If E4 hasn't landed yet, add a note in its plan
      that this prereq is satisfied.
- [ ] Add this report as **gap I** in
      `docs/upcoming/ecs-prereq-plan.md`, alongside A-H, and mark it
      shipped.
- [ ] Move this `docs/reported/` file to `docs/archive/history/` once
      I1-I5 land (per the `churn-docs` workflow). Leave the
      `## Severity` and `## Concrete failure` sections intact as a
      historical record.
- [ ] Migration note in the spice CHANGELOG: "BREAKING -- `defsystem`
      `:reads`/`:writes` are now component-name vectors, not bitmask
      ints. See <migration link>."

Validation: archive sweep clean; plan docs accurate; no stale
"future work" references to write-cap enforcement.

#### Out-of-scope (deliberately deferred)

- Cross-system cap reasoning (e.g. "this system promises not to
  write Vel transitively through a helper function"). The current
  plan only checks direct `set-Comp!` calls in the system body.
  Helpers that take a cap parameter still work; helpers that try to
  conjure caps don't.
- ReadCap enforcement on `get-Comp` -- nice-to-have, mostly
  prevents stale-data bugs. Defer to a follow-up if I3-I4 slip.
- Path B (elaborator scanner) and Path C (debug runtime check)
  remain documented fallbacks; do not implement unless Path A
  hits a blocker.

#### Risk register

| Risk | Likelihood | Mitigation |
|------|------------|------------|
| `^linear` re-borrow ergonomics force ugly user code | Medium | Prototype the iteration pattern in I1; if rough, ship the `(set! w Pos e v)` sugar in I4 instead of I5 |
| Banning raw `dense-set!` breaks a demo we forgot about | Low | Grep `../turmeric-spices/` for direct storage-primitive calls before I4 lands |
| Diagnostic from raw TUR-E0005 is too cryptic | Medium | Budget for the dedicated `TUR-Ecaps` code in I5 |
| Per-world cap lifetime conflicts with parallel scheduler waves | Low-Medium | Validate in I2 that re-borrow into wave-thread closures typechecks; fall back to per-system-invocation minting if not |

## Validation plan

A fix is validated when:

- The negative repro above (`defsystem rogue` with `:writes [Pos]`
  but body writing Vel) fails to compile with a diagnostic naming
  Vel.
- All existing ECS spice tests continue to pass (they're all
  internally consistent, so they should still typecheck once the
  user-visible API change is migrated).
- A new `tests/fixtures/errors/ecs-defsystem-writes-unauthorized/`
  fixture in the main repo regression-tests the diagnostic.
- The plan's `ecs-vs-haskell-ecs.md` writeup (E4) can honestly claim
  compile-time write-cap enforcement.

## Workaround in the meantime

None at the compile-time level. The user can:
- Audit every `defsystem` body manually to confirm writes match
  declarations. (Brittle; defeats the plan's value prop.)
- Add a Path-C runtime check (a debug-build assertion that records
  writes per system-run and diffs against the system's writes-mask).
  Catches the bug in tests but doesn't ship the plan's compile-time
  promise.

## References

- `../upcoming/ecs-spice-plan.md` § "Systems and scheduling" --
  the spec'd `set-X!`-only-for-`:writes` rule.
- `../upcoming/ecs-spice-plan.md` § "Where Turmeric's types pull
  ahead" item 3 -- the "single biggest delta vs. Haskell ECSes"
  claim.
- `../upcoming/ecs-spice-plan.md` § "Phasing E2" -- the
  "load-bearing v1 win and it works today" claim.
- `../guides/substructural-types-guide.md` -- the
  `-Xsubstructural`/`^linear` machinery the fix would build on.
- `../../../turmeric-spices/spices/ecs/src/ecs/system.tur` -- the
  current `defsystem` macro, where the fix lands.
- `../upcoming/ecs-prereq-plan.md` -- add this as gap I once
  filed, alongside A-H.
