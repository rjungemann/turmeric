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

### Implementation sketch

1. **Add the `WriteCap`/`ReadCap` opaque types** in
   `ecs/system.tur` or a new `ecs/cap.tur`. Both are `:linear`
   opaque ints; `use-cap!` consumes one.

2. **Mint per-component capability bindings** in
   `defcomponent-class-instance` (or a parallel macro): each
   `(defcomponent-class-instance GameWorld Pos)` also emits an
   instance binding for `WriteCap<Pos>` constructed at runtime
   when the system is invoked.

3. **Rewrite `defsystem`** to take `:reads` and `:writes` as
   component-name vectors, mint per-name capability bindings in the
   body's lexical scope, and (internally) compute the bitmask used
   by the scheduler.

4. **Rewrite `defcomponent-accessors`** so each `set-Comp!` /
   `get-Comp` takes the corresponding capability as a `^linear` /
   `^borrow` parameter.

5. **Spice integration tests** that demonstrate:
   - a system declaring `:writes [Pos]` and writing Pos compiles;
   - a system declaring `:writes [Pos]` and writing Vel fails to
     elaborate with a clear "missing capability for Vel" diagnostic.

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
