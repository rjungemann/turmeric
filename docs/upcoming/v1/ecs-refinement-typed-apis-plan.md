---
title: ECS Refinement-Typed APIs Plan
category: Planning
description: Rewritten after refinement types landed. What the shipped `#refine{...}` surface can and cannot do for `tur-ecs`, measured against the real compiler, and the three compiler gaps that stand between here and a strict-aliveness ECS API.
---

# ECS Refinement-Typed APIs -- Plan

> **Status 2026-07-26 -- rewritten.** The previous revision of this file was
> written while refinement types were still unbuilt, and it is wrong in ways
> that matter: it assumed a `(refine T P)` type former that never shipped, it
> described the ECS accessors as `option`-returning when they are not, and it
> proposed a `/has` world bound that is not a refinement type at all.
>
> RT0--RT7 and S0--S4 landed 2026-07-24/25 behind `--enable=refined` /
> `#lang turmeric refined` (see
> [`refinement-types-plan.md`](refinement-types-plan.md) and
> [the guide](../../guides/refinement-types-guide.md)). So this plan can stop
> speculating and start measuring. **Everything asserted below about compiler
> behaviour was checked against `build-release/tur` at VERSION 0.30.8**; each
> claim carries the probe that establishes it.

## What changed, in one paragraph

The gating question is no longer "do refinement types exist". It is "can a
refinement predicate say anything about a **mutable world**". The answer today
is no, and that is not an oversight -- it is the mechanism that keeps the
feature sound. A measure is congruent (two occurrences denote one value) only
when the compiler can prove the callee pure, and `sized-alive?` reads a
generation counter out of a malloc'd control block through inline C. Making it
congruent anyway is precisely the miscompile shape the refinement work found
and fixed three separate times. So the ECS strict-aliveness API is blocked on a
*sound* way to talk about state, not on a flag.

---

## Ground truth: what the shipped compiler does

Six probes, all run against a Release build of the current tree.

### 1. A boolean-valued measure cannot be a predicate atom

```turmeric
(defn alive? [w : int e : int] #fx{} : bool (= w e))
(defn use-it [w : int e : #refine{ x : int | (alive? w x) }] : int e)
```

`refine: 1 obligation(s): 0 proven, 0 refuted, 1 unknown`, and
`TUR_REFINE_DUMP=1` prints **no VC at all** -- the obligation never reaches the
solver.

The cause is in the encoder: `enc_measure` (`refine_collect.c`) declares every
measure with `vc_declare_ufunc(..., VS_INT, ...)`, and `refine_vc_build`
rejects a goal whose sort is not `VS_BOOL` with *"predicate does not denote a
proposition"*. There is no way to write `(alive? w e)` as a predicate. This is
the single most load-bearing gap for ECS, because *every* domain predicate an
ECS wants -- alive, has-component, in-bounds-for-this-world -- is naturally a
`bool`-returning function.

Tracked in [`refine-predicate-measures-plan.md`](refine-predicate-measures-plan.md).

### 2. The int-valued workaround **works today**

Spell the same predicate as an `int` comparison and the obligation discharges:

```turmeric
(defn alive-i [w : int e : int] #fx{} : int (if (= w e) 1 0))
(defn use-it [w : int e : #refine{ x : int | (= (alive-i w x) 1) }] : int e)

(defn caller [w : int e : int] : int
  (if (= (alive-i w e) 1)      ;; guard
    (use-it w e)               ;; crossing
    0))
```

`refine: 1 obligation(s): 1 proven, 0 refuted, 0 unknown`.

This is the important positive result, and it reshapes the whole plan. The
crossing's path-condition recovery (landed 2026-07-25) already does the work
the old plan wanted a bespoke `entity-alive!` promotion form for: a call
guarded by `(if (alive? ...) ...)` discharges the callee's aliveness
refinement, with **no new type former, no flow-typing, and no `-alive`
accessor family**. The old RE0 design is obsolete; what is left is choosing the
encoding.

### 3. ...but only because `alive-i` is pure

Rewrite `alive-i` to read a generation out of a handle through inline C -- i.e.
write the function the ECS actually has -- and the same program reports
`0 proven, 1 unknown`. The purity walk is default-deny; inline C is impure;
each occurrence of the call gets a distinct symbol; the guard says nothing
about the argument.

Note it is **not** an error: `TUR-E0375` fires only on *proven* impurity, and
an `(unsafe ...)`-wrapped inline-C call classifies `RT_P_UNKNOWN`, which the
diagnostic reads as pure and congruence reads as impure. So the failure mode
here is silent loss of the proof, not a rejection.

This is the wall. Tracked in
[`refine-stateful-measures-plan.md`](refine-stateful-measures-plan.md).

### 4. A `#refine{...}` on a `defopaque` parameter compiles

```turmeric
(defopaque Entity :int)
(defn use-it [e : #refine{ x : Entity | (>= (idx-of x) 0) }] : int 0)
```

Compiles clean. `rt_sort_of_kind` (`elab_fns.c:46`) maps every non-float kind to
`VS_INT`, so an opaque newtype over `:int` is a first-class refinement subject.
Nothing about the ECS needing real handle types is blocked by the solver.

### 5. A `^borrow` struct parameter's field read stays congruent

```turmeric
(defn cap-borrowed [^borrow w : W] #fx{} : int (.n w))
(defn use-b [^borrow w : W i : #refine{ x : int | (< x (cap-borrowed w)) }] : int i)
```

Proven, identically to the by-value spelling. The guide's "behind a reference it
is declined" rule keys off the receiver's *type kind* (`TY_REF`, `TY_RC`,
`TY_PTR_VOID`), and `^borrow` is a parameter annotation rather than a reference
type. This matters because every ECS accessor takes `^borrow w : World`, and it
means a world whose state lives in **struct fields** is reasonable-about while a
world whose state lives behind a `:int` handle is not.

### 6. A refined index inside a `while` loop is Unknown

```turmeric
(while (< acc n)
  (do (get-at s acc) (set! acc (+ acc 1))))
```

`0 proven, 1 unknown`, exactly as the guide's `[deferred]` limit says. This is
the one that costs real performance rather than real safety: `for-each` lowers
to a `while` over slot indices, and the bounds facts that would let dense
storage drop its per-access check live in the loop condition. Waiting on
[`loop-invariants-plan.md`](../hold/loop-invariants-plan.md), which is on hold
for want of a demand signal -- **this plan is that signal.**

### Not found: a soundness hole

Probes 3--6 were also run adversarially, to see whether a hypothesis could be
staled by an intervening mutation and then used to elide a check. It cannot,
and the reasons are worth recording so nobody re-runs this:

- a `set!` in the caller's body abandons the crossing (verified: Unknown);
- a `^mut` parameter is **by value** in Turmeric -- a callee's `set!` is not
  visible to the caller (verified by print), so a mutating call cannot stale a
  caller's hypothesis about a struct it passed;
- a read of a `^mut` binding classifies `RT_P_UNKNOWN`, and inline C classifies
  impure, so the two remaining mutation channels both decline congruence.

The mutation channel is closed. It is closed *by the same rule* that blocks
probe 3, which is the tension the supplemental plans have to resolve without
opening it.

---

## Corrections owed to the shipped docs

Both statements below are in guides today and both are false against the spice
as it stands. Landed alongside this rewrite.

| Where | Says | Actually |
|---|---|---|
| `ecs-vs-haskell-ecs.md:37` | "`option`-returning reads (`(none)` on dead-handle)" | `defcomponent-accessors` emits `get-<Comp>` returning `~comp-name` **directly**. There is no aliveness check on the read path at all. |
| `ecs-guide.md:299` | "Aliveness: runtime, via generation comparison on every ..." | True for `sized-*` worlds (`sized-alive?`). The unsized `ecs/world` exposes **no aliveness predicate whatsoever** -- only `world-despawn!`, which bumps `gens[idx]`. |

This is a bigger finding than a doc typo. The old plan's pitch was "trade an
`option` unwrap for a type-level proof". The real trade is "**introduce** an
aliveness check where today there is none, and then prove it away". That is a
better story -- it is a correctness win first and a performance win second --
but it means RE1 is not a pure surface addition over the substrate, as the old
plan claimed. It adds an API that does not exist.

---

## Goal

Give `tur-ecs` an opt-in surface where **use-after-despawn is a compile-time
error**, and where the sized worlds' slot-index bounds are discharged statically
rather than re-checked per access. Both are opt-in; the existing accessor family
keeps its shape and its call sites stay correct.

Explicitly **not** a goal: making refinement types the default way to use
`tur-ecs`. The refined accessors stay a surface you opt into by importing
`ecs/refined-world` or calling `sized-defworld-refined`; the forgiving
`get-<Comp>` family keeps its shape either way.

> **Correction 2026-08-01 -- the flag-gating rationale is gone.** This
> paragraph used to read "The experiment expires at `0.34.0` and its graduation
> is a separate decision; an ECS that requires it is an ECS that cannot ship on
> the near side of that decision." That constraint shaped this plan's whole
> sequencing, and it no longer exists: `refined` **graduated 2026-08-01**
> (`bb7cbef61`, shipped v0.33.0). Static `#refine{...}` discharge is
> unconditional, the `EXPERIMENTS[]` and `LANG_LAYERS[]` rows are deleted, and
> a lingering `--enable=refined` / `#lang turmeric refined` is a no-op
> (TUR-W0063 / TUR-W0064) whose shim ages out one minor line later. So the
> refined surface no longer costs its consumers a flag, and "opt-in" here means
> an API choice rather than a compiler gate. See
> [`refined-graduation-plan.md`](refined-graduation-plan.md).

---

## Prerequisites

Three compiler gaps and one spice-side prerequisite. The compiler gaps have
their own plans; this section states what ECS needs from each, not how to build
it.

| # | Gap | Needed for | Plan |
|---|---|---|---|
| C1 | Boolean-sorted measures -- `(alive? w e)` usable as a predicate atom | ergonomics of every ECS predicate | [`refine-predicate-measures-plan.md`](refine-predicate-measures-plan.md) -- **RM-B1 LANDED 2026-07-26** |
| C2 | A sound route for a measure over mutable world state | RE1 at all | [`refine-stateful-measures-plan.md`](refine-stateful-measures-plan.md) -- **LANDED 2026-07-26** (`#reads` + the `frozen` region) |
| C3 | User-written `while` invariants | RE2's bounds elimination | [`loop-invariants-plan.md`](../hold/loop-invariants-plan.md) |

**C1 is not strictly blocking** -- probe 2 shows the `(= (alive-i w x) 1)`
encoding proves today. It is blocking on *whether anyone would write it*. An
ECS whose accessor signatures read
`#refine{ x : Entity | (= (alive-i w x) 1) }` is an ECS nobody adopts.

> **C1 landed (RM-B1), and a purity caveat surfaced for RE1.** A `bool`-returning
> function is now a first-class predicate atom, so `#refine{ x : Entity |
> (alive? w x) }` type-checks and, when `alive?` is pure, discharges through
> an `if`-guard. **But RE0's handle-unwrap helpers are inline-C** (`slot->int`,
> `entity-index`, `entity-generation`, ...), and the purity walk is default-deny
> on inline C -- so any predicate that unpacks a `Slot`/`Entity` (e.g.
> `(in-bounds? n (slot->int x))`) is classified *impure*, gets a fresh symbol
> per occurrence, and does **not** discharge as a congruent measure (verified:
> `0 proven, 1 unknown`). This is the same wall as C2, reached one step earlier:
> for RE1's accessor predicates to be congruent, the predicate must be a pure
> function of values, which today means either (a) predicates that compare
> handles/newtypes without unwrapping through inline C, or (b) making the RE0
> unwrappers pure primitives the purity walk accepts. Fold this into the C2
> design rather than treating C1 as sufficient on its own.

~~**C2 is hard-blocking for RE1.** There is no encoding of a mutable-state
predicate that discharges today, and there should not be one until the design
question is answered.~~

> **Resolved 2026-07-26 -- C2 landed; RE1 is not blocked.** The struck text
> above was true when written and is kept for the record. The design question
> was answered by `#reads` plus the borrow-based `frozen` region: an impure
> measure declares the state it reads, and inside a region holding `(& w)` it
> is congruent, because a mutator declared `^unique ^mut w` cannot be called
> there (`TUR-E0200`). That is in the shipped compiler --
> `enc_reads_arg_frozen` (`src/compiler/refine_collect.c:234`, granted at
> `:470`) and `rt_pred_reads_measure` (`src/compiler/elab_fns.c:794`) -- and
> every acceptance fixture in the C2 plan is marked DONE. RE1 is complete and
> promoted to the real sized-world stack as `ecs/sized-refined`; see the RE1
> phase banners below.

**C3 is blocking for RE2 only.** RE1 does not need it.

### Spice-side prerequisite: real types at the API surface

`ecs/entity` declares `(defopaque Entity :int)` and then types every function
`[e : int] : int`. `defcomponent-accessors` emits `[e : int]`. `sized-alive?`
is `[s : int e : int] : bool`. The world state cell is `:int`.

This is the `:int` stand-in pattern `CLAUDE.md` forbids, and it is not merely
stylistic here -- it is what makes the old plan's negative fixture
unwritable. "Calling `get-Pos-alive` on an un-refined `Entity` fails to
elaborate" has no meaning when `Entity`, slot index, generation counter, state
cell and component id are all the same type. A refinement on `:int` is a
refinement on everything.

Probe 4 establishes there is no solver-side reason to keep the erasure: a
`defopaque` over `:int` refines exactly as well as a bare `:int` does. This is
phase RE0.

---

## Phasing

### RE0 -- Real handle types at the ECS API surface (spice-side, LANDED)

> **Status 2026-07-25 -- landed** on the `turmeric-spices` branch
> `claude/ecs-refinement-re0`. `ecs/entity` now carries `Slot` and
> `Generation` newtypes beside `Entity` (with `slot->int` /
> `generation->int` escape hatches and `slot-new` / `generation-new`
> constructors); `ecs/sized-world` carries a `WorldState` newtype for the
> control block and threads `Entity`/`Slot`/`Generation`/`WorldState`
> through spawn/despawn/alive and the `sized-defworld` `state` field;
> `ecs/world`'s `world-alloc-entity!` / `world-despawn!` are `Entity`-typed.
> The negative fixture is `spices/ecs/tests/errors/slot-not-entity.tur`
> (a `Slot` where an `Entity` is required -> `TUR-E0001`). No regressions
> against the pre-existing v0.31.0 suite baseline (which carries ~23
> unrelated failures from a `(Storage T)` associated-type regression --
> the accessor/for-each validation surface -- so those were validated
> against the passing `sized-world-*` tests instead).
>
> **Two sub-items intentionally deferred** (noted, not done): the
> `defcomponent-accessors` / `sized-defcomponent-accessors` slot parameter
> stays a bare `:int` storage index, and `for-each`'s slot binding stays
> `:int` -- both because lifting them to `Slot` requires lifting the
> int-keyed storage layer (`ecs/storage`, `ecs/sized-storage`) to `Slot`
> too, which is out of RE0's three-module scope, and because every one of
> their consumers is currently in the `(Storage T)`-broken set (so the
> change is neither validatable nor "passes unchanged" today). Fold this
> into the storage-side follow-up.
>
> **Update 2026-07-26 -- the `(Storage T)` skew is FIXED and the deferral's
> stated blocker is gone.** `struct_field_type_from_form` was missing the
> assoc-type-projection dispatch (`(Storage Pos)` -> spurious TUR-E0012) and
> the SZ8 Size-literal placeholder (`(SizedDense (Static 8) Pos)`); both now
> mirror `type_expr_from_form` (fixture: `defstruct-assoc-sized-fields`).
> With the spice tests' legacy by-value box triples also retired
> (`defworld-box-helpers`), the ecs suite is **66/66 green** -- so the
> accessor/for-each consumers are validatable again, and the two deferred
> `Slot`-typing sub-items are unblocked whenever the storage-side follow-up
> is picked up.
>
> **Update 2026-07-26 (later) -- both deferred sub-items DONE; RE0 fully
> complete.** Every public storage index (`dense-*`, `sparse-*`, `tag-*`,
> and the sized trios), the `StorageOps` class methods, the accessor
> emitters' slot parameter, and `for-each`/`sized-for-each`'s binder are
> `Slot`-typed; `defmirror` and `sized-defworld-copy-into`'s generated loops
> follow. Inline-C bodies were unchanged (same int64 carrier) -- the lift is
> signatures + `(slot-new ...)` at the honest int-to-slot boundaries. A raw
> int where a `Slot` is expected is `TUR-E0001`
> (`tests/errors/int-not-slot.tur`); the guide's canonical for-each body
> pattern (binder straight into storage/accessors) is now correct BY TYPE.
> Suite 66/66 before and after (turmeric-spices `830e911`). RE1's refined
> signatures were already `Entity`-typed, so nothing there needed rewriting
> -- the sequencing (types before more refinements) held.

Retire the `:int` stand-ins on the public surface of `ecs/entity`,
`ecs/world`, and `ecs/sized-world`:

- `Entity` (already a `defopaque`) used as the parameter and return type of
  `entity-new` / `entity-index` / `entity-generation` / `entity=?` /
  `world-despawn!` / `sized-spawn!` / `sized-despawn` / `sized-alive?`.
- A `defopaque Slot :int` for the raw slot index, distinct from `Entity` --
  `for-each` currently binds a slot index to a name the user reads as an
  entity, which is a live confusion independent of refinements.
- A `defopaque Generation :int`.
- A `defopaque WorldState :int` for the sized world's control block, or a
  `:ptr<...>` if the struct can be named.

Inline-C bodies keep taking the carrier; only the signatures change. The
existing `unsafe` raw helpers (`__sized-state-*-raw`) stay `:int` internally.

**Why first:** it is the only phase with no compiler prerequisite, it is
independently correct under `CLAUDE.md`, and every later phase writes
refinements *on these types*. Doing it after RE1 means rewriting RE1's
signatures.

**Acceptance:** the existing spice test suite passes unchanged; a new negative
fixture under `spices/ecs/tests/errors/` passes a `Slot` where an `Entity` is
expected and fails to elaborate.

### RE1 -- Strict aliveness (needs C2, wants C1)

> **Status 2026-07-26 -- pattern PROVEN end-to-end; two compiler deps found +
> fixed; harness/module integration remains.** C2 landed (`#reads` + the sound
> `frozen` region), so RE1 is unblocked. The aliveness-refined accessor works
> against the real `ecs/freeze` region: `alive?` reads liveness through an opaque
> handle (a malloc'd `gens` array -- `sized-alive?`'s shape, so genuinely impure
> and `#reads`-carrying), `despawn!` is `^unique ^mut` (locked out in the region,
> `TUR-E0200`), and a guarded `(frozen w (if (alive? w e) (get-x! w e) ...))`
> reports **refine: 1 proven** and runs; the same read with NO region is
> **1 unknown -> TUR-W0372** (so `#reads`+`frozen` is load-bearing). Fixtures:
> `turmeric-spices/spices/ecs/tests/refined/alive-frozen.tur` (positive) and
> `.../tests/errors/refined-alive-no-region.tur` (negative); dogfood write-up
> `turmeric-spices/docs/ecs-re1-refined-aliveness.md`.
>
> Getting here required two compiler fixes (both landed, validated, suite 2367/0):
> (1) a `#reads`-refined param could not codegen -- the impure entry contract was
> `TUR-E0375`; now suppressed (the accessor keeps its own internal check as the
> backstop). (2) the `frozen` *macro* did not compose with guard-discharge --
> macro expansion copies the body, so the crossing path walk missed it under
> pointer identity; fixed with a source-span crossing match (`rt_form_ident`).
>
> **Update 2026-07-26 -- (b) shipped, with a characterized encapsulation limit.**
> The self-contained facade is now a real module, `ecs/refined-world`
> (turmeric-spices, in `build.tur :exports`), and the refined accessor discharges
> **cross-module**: an importer guards in its own `frozen` region and calls the
> module's `rgworld-get-x!` -- **1 proven, runs -> 42**; the same read with no
> region is `TUR-W0372`. `RGWorld` is an opaque affine handle, so a `frozen`
> borrow locks out its `^unique ^mut` mutators (`TUR-E0200`). The soundness
> caveat is real and documented: neither a `defstruct` field nor a `defopaque`
> encapsulates against the `::` coercing cast -- `(:: w :int)` unwraps the handle
> and `(:: int RGWorld)` reconstructs an alias, so a deliberate `::`/inline-C
> bypass can despawn inside the region. That is the same trust boundary `#reads`
> already carries (sound for ordinary code, not adversarial code); a hard
> guarantee needs a language feature (module-private construction / a `::`-sealed
> newtype). Filed as `docs/reported/frozen-region-aliasing-via-coercing-cast.md`.
>
> **Update 2026-07-26 -- (a) tooling built, but auto-running BLOCKED by a
> compiler bug; (c) pattern proven.**
>
> **(a)** `tur test` gained two leading-comment directives --
> `;; tur-test-flags: --strict-refine` (per-test strict compile, so an unproven
> crossing is a hard error, which ENFORCES the proof) and
> `;; tur-test-expect-error: TUR-W0372` (must fail to compile and name the
> diagnostic; run phase skipped). The directive feature works and is general (a
> reusable `tur test` improvement). BUT auto-running the *refined* tests through
> it surfaced a serious pre-existing compiler bug: **compiling multiple refined
> files in one process (`tur test <dir>`, LSP/worker) corrupts memory and
> segfaults nondeterministically** (6-8/8 crashes; ASan-silent -> arena/stack, not
> heap). A partial fix landed (`cmd_build` now calls `refine_discharge_reset()` --
> the memo held stale per-compile-arena VC pointers), but a second channel
> remains. Filed as `docs/reported/refined-multi-compile-memory-corruption.md`.
> So the RE1 refined tests are kept in `spices/ecs/tests/refined/` (a subdir
> `tur test tests` does not descend into) and verified **individually** (each
> passes on its own `tur run`/`tur check`), NOT auto-run. Auto-running is blocked
> until the corruption is fixed (then: run each via its own invocation, or move
> them flat).
>
> **Update 2026-07-26 (later) -- corruption FIXED; (a) fully unblocked.** The
> second channel was root-caused via the executed
> `docs/archive/history/arena-debug-poisoning-plan.md` (the AP4 guard mode's clean run
> disproved the UAF theory): `parse_typeclass_method` left the RT1 memo field
> `TypeClassMethod.refine_class_binding` UNINITIALIZED in non-zeroed arena
> memory, so the second in-process compile read recycled-slab junk as a
> `Binding*`. Fixed by zeroing the struct. The `tur test tests/refined` repro
> went 8/8 SIGSEGV -> 0/20 failures, so the refined tests now auto-run via
> `tur test` (moved flat into `spices/ecs/tests/`). Resolved report:
> `docs/archive/history/refined-multi-compile-memory-corruption.md`.
>
> **(c)** The `for-each` aliveness refinement is PROVEN. A refined LOOP whose
> body's `rgworld-get-x!` discharges per-entity works today
> (`tests/refined/refined-loop-alive.tur`: 1 proven, runs -> 40, correctly skips
> a despawned entity). The mechanism: the `while`+`set!` form is blocked -- the
> loop counter's `set!` trips the whole-body `mentions_set` decline in the
> crossing path-cond collector (the C3-adjacent gap) -- so it uses TAIL RECURSION
> (TCO'd; verified 5M calls) + a re-borrow of `w` inside the recursive helper (so
> `alive?` is congruent there) + the `alive?` guard.
>
> **Correction (2026-07-27):** an earlier note here claimed "turmeric has no
> `loop`/`recur` or self-recursive `let`-`fn`, and a macro cannot emit a
> top-level recursive helper." That was WRONG (bad probing -- I used a plain
> `let`, not `letrec`). Verified: **named-let (`(let go [...] ...)`) and `letrec`
> both work** (local recursion exists; a hand-written named-let refined loop
> discharges, `1 proven`), and **a macro CAN emit a top-level `defn`**. Only
> `(loop [...] (recur ...))` is genuinely absent, and named-let covers it. So the
> recursive form does NOT need the order-aware-`set!` fix at all. What actually
> blocks an ergonomic `for-each-alive` MACRO is a different bug: a macro that
> *generates* a refined guard/crossing (via the quasiquote template, not `~@body`
> splicing the user's forms) does not discharge -- spurious `TUR-W0372`. Filed as
> `docs/reported/macro-generated-refined-crossings-do-not-discharge.md`.
>
> **Remaining for RE1:** fix the refined-multi-compile corruption (unblocks
> auto-running all refined tests); fix macro-generated refined-crossing discharge
> (unblocks an ergonomic `for-each-alive` macro). The while-based `for-each`
> order-aware-`set!` fix is optional -- the recursive form already works.
>
> **Update 2026-07-26 (later) -- BOTH remaining blockers fixed; RE1 complete.**
> The corruption was an uninitialized `refine_class_binding` memo field (see the
> earlier update); the macro-generated-crossing bug is fixed by recording each
> macro call's expansion (`refine_note_macro_expansion`) and letting the
> crossing path walk traverse INTO expansions -- `rt_form_occurrences` /
> `rt_collect_path_conds` / `rt_form_mentions_set` walk a macro call AS its
> expansion (resolved report:
> `docs/archive/history/macro-generated-refined-crossings-do-not-discharge.md`). The
> set!-scan depth also rose 12 -> 24 (an expansion is legitimately deeper than
> the source spelling it; the old limit's conservative "too deep, assume
> assignment" answer spuriously declined clean for-each expansions). The
> ergonomic **`for-each-alive` macro now proves**: one macro generates the
> recursive loop + frozen re-borrow + aliveness guard, the user's refined read
> is spliced as the body, and the crossing discharges per-entity
> (`tests/fixtures/refine-macrogen-foreach`; the three report shapes + nesting
> in `tests/fixtures/refine-macrogen-crossings`; adversarial negatives in
> `tests/fixtures/errors/refine-macrogen-*`). Shipped to the ecs spice as
> `ecs/refined-world`'s `for-each-alive!`.

> **Update 2026-07-26 -- the promotion is SHIPPED: `ecs/sized-refined`.** The
> accessor family below now exists against the REAL sized-world stack, emitted
> per world/component: `(sized-defworld-refined W)` -> `<W>-alive?` (`#reads`)
> + `<W>-despawn!` (`^unique ^mut`); `(sized-defcomponent-accessor-refined W
> C)` -> the cap-gated `get-<C>!` with the refined entity parameter -- the
> exact signature in the code block below; `(for-each-alive W w n e body)` for
> the per-entity-proven iteration. Getting there took one more compiler fix:
> macro TEMPLATES could not emit `#reads` (fx_prov dropped by both template
> copiers) or substitute into a `#refine{...}` predicate (F_CONTRACT_TYPE
> returned as-is) -- fixed in `elab_macros.c`, pinned by
> `tests/fixtures/refine-template-emitters`. Acceptance: spawn/despawn/read
> proves + runs, no-region is `TUR-W0372`, in-region despawn is `TUR-E0200`,
> for-each proves per-entity (spices `tests/refined-stack-*`, suite 70/70).

Add an opt-in accessor family that will not compile against a handle whose
aliveness has not been established:

```turmeric
;; today's surface -- unchanged, no aliveness check anywhere
(get-Pos read-cap w e)

;; the refined surface
(defn get-Pos! [^borrow cap : (ReadCap Pos)
                ^borrow w   : GameWorld
                e           : #refine{ x : Entity | (alive? w x) }]
             : Pos
  ...)

(if (alive? w e)
  (get-Pos! read-cap w e)     ;; discharged from the guard
  (handle-dead-entity))
```

Three things to decide at elaboration time, listed because they are the
decisions and not the typing:

1. **What `alive?` is a function of.** The honest answer is "the world's
   despawn history", which is not a value. C2's job is to supply a spelling
   that is a function of values -- see that plan for the two candidates (a
   version/epoch argument, and a scope-bounded congruence window backed by the
   linear caps `ecs/cap` already ships).
2. **Whether `for-each` bodies get it.** `for-each` splices its body inline and
   binds a raw slot, never checking generations. Making the loop's binding
   carry an aliveness refinement is the highest-value version of this feature
   and the one most exposed to C3.
3. **Whether the entry check is acceptable.** It always is emitted (guide,
   `[by design]`). For `get-Pos!` that is one integer compare against
   `gens[idx]` -- which is *cheaper* than what the sized worlds do today and
   strictly more than the unsized worlds do today (nothing). The refinement's
   value here is the compile error, not the elision. Measured evidence backs
   this reading: the parent plan's "whole-program entry-check elision:
   measured, declined" section found **zero** runtime and code-size benefit
   from eliding a parameter check.

**Acceptance:**
- `spices/ecs/tests/` fixture: spawn N, despawn half, read `Pos` through
  `get-Pos!` inside an `(if (alive? ...))` guard, `refine: N proven`.
- `spices/ecs/tests/errors/`: the same call *without* the guard fails under
  `--strict-refine`, and warns (`TUR-W0372`) without it.
- A fixture pinning the **negative** direction: a `despawn!` between the guard
  and the call must NOT discharge. This is the fixture that would catch C2
  being implemented as an escape hatch, and it should be written before the
  feature, not after.

### RE2 -- Bounded slot indices on sized worlds (needs C3)

A sized world knows its capacity at the type level. `sized-dense-get` re-checks
`0 <= i < cap` on every access; the check is provable from the loop condition
and the world's `n`.

```turmeric
(defn sized-get-at [^borrow w : (GameWorld n)
                    i : #refine{ x : Slot | (and (>= x 0) (< x n)) }] : Pos
  ...)
```

Probe 6 says this is Unknown inside a `while` today, which is where every real
call site lives. With a written `:invariant` on the `for-each` expansion's loop
it becomes an ordinary path-splitting obligation.

> **Probe update 2026-07-26 -- the RECURSION shape discharges bounds TODAY,
> no C3.** A bounds-refined accessor (`#refine{ x | (and (>= x 0) (< x 8)) }`)
> called from a tail-recursive loop proves under `--strict-refine`: the upper
> bound comes from the loop guard `(< i 8)` as an ordinary path condition, and
> the lower bound rides a refined parameter (`i : #refine{ x | (>= x 0) }`)
> inductively -- the recursive crossing proves `i+1 >= 0` from `i >= 0`, the
> canonical decreasing-argument shape path conditions already handle. Negative
> controls both reject (an off-by-one guard `(< i 9)`; a dropped lower-bound
> refinement). A sized capacity is a type-level constant, so the whole proof
> lives in the PURE fragment -- no `#reads`, no trust. So C3 gates only the
> `while` lowering: RE1 (c)'s `for-each-alive!` pattern (a macro generating
> the named-let loop) carries over directly, and RE2's remaining gate is the
> PROFILE alone. The `#reads`/`#writes` trajectory still matters here for two
> follow-ons: checked write-frames would replace the coarse whole-body `set!`
> decline (unblocking the `while` form), and `frozen` + `#reads` extends
> bounds elimination to RESIZABLE storage, where `(in-bounds? buf i)` reads
> mutable capacity (see stateful-refinements-guide.md "Where this
> generalizes").

Deliberately sequenced last: it is the only phase whose payoff is measured in
nanoseconds, and the parent plan's benchmarking section is a standing warning
that this class of win tends to evaporate under `cc -O2`, which already proves
locally-derived bounds for free. **RE2 does not start without a profile.**

### RE3 -- Documentation (DONE 2026-07-26)

- Fix the two false statements in the table above. *(Landed with the plan
  rewrite.)*
- `ecs-guide.md`: replace the "gated on the refinement-types work" pointer with
  what actually ships. *(Done: the Entities section and the runtime-checks
  bullet now describe `ecs/refined-world` + `for-each-alive!` under
  `--enable=refined`, with the facade-vs-full-stack scope note.)*
- `ecs-vs-haskell-ecs.md`: the aliveness row gains a compile-time entry
  alongside the runtime default; the polymorphism row is **not** touched (see
  below). *(Done: row, "still runtime-checked" section -- the "open design
  question" text replaced with the shipped `#reads` + `frozen` answer and its
  trust-boundary caveat -- and the honest-scorecard bullet.)*
- Also landed in the same pass: `stateful-refinements-guide.md` gained the
  "Macros compose" section (splicing vs generating macros both discharge;
  `for-each-alive!` as the shipped consumer), and
  `refinement-types-guide.md`'s pointer to it no longer says "in-flight".

---

## Dropped from the previous revision

### `/has` world bounds are not refinement types

The old RE1 proposed `(refine W (/has Pos /has Vel))` as a third polymorphism
encoding. This should not be built as written, for a reason more basic than
"the elaborator does not support it":

A refinement type constrains a **value**. `/has Pos` constrains a **type** --
it asks whether the type `W` has a field named `Pos`. Those are different
judgements over different objects, and the shipped refinement machinery has no
representation for the second: the guide lists "no refinements on type
parameters" as a `[prototype]` limit, and the VC term language has integers,
reals, booleans and uninterpreted functions -- no types.

Encoding a structural type predicate through the SMT layer would mean teaching
the solver about the type system, which is a great deal of machinery to
reimplement a question `defworld` can answer by field lookup at elaboration
time. The right home for it is row/presence constraints, re-filed as
[`ecs-component-set-bounds-plan.md`](ecs-component-set-bounds-plan.md).

Meanwhile the `(HasPos W)` typeclass encoding **ships and works**, so nothing
is lost by the deferral except one dictionary indirection per polymorphic call.

### The `-alive` accessor family and `entity-alive!`

Superseded by probe 2. Crossing path-condition recovery gives the promotion for
free from an ordinary `if`; a bespoke promotion form would be a second, weaker
mechanism for something the general one already does. RE1 keeps only the
refined *accessors*, not the promotion form.

### Sized `-alive` analogues

Same reasoning; the sized accessors take the same refinement on their `Entity`
parameter.

---

## Relationship to the graduation decision

[`refined-dogfooding-plan.md`](../../archive/refined-dogfooding-plan.md) is on hold
"waiting on a program to exist, not on effort", and feeds graduation
precondition 2 (cost on something that is not a fixture).

**`tur-ecs` is that program**, and the fit is close enough to be worth stating
against the dogfooding plan's own tier list:

| Dogfooding item | What ECS supplies |
|---|---|
| Calls guarded by `if` / `let` / `match` | RE1 is nothing but this, at every accessor call site |
| A typeclass with a refined method parameter | `StorageOps` / `Component`, whose methods take the slot index |
| `match` on an ADT with refined arms | `sized-spawn`'s `(Result Entity WorldFull)` |
| Recursion, incl. a mutually-recursive pair | the `for-each` / `defworld` macro expansions are recursive at expansion time, not runtime -- **ECS does not supply this** |
| Floats with non-zero fractional parts | `ecs-raylib`'s `Pos`/`Vel` are float components |
| Compile-time cost on a real program | `spices/ecs` is ~5400 lines across 22 modules |

So the ordering that falls out is: **RE0 now** (no prerequisite), then C1
(small, verified, unblocks readable signatures), then C2 (the real design
work), then RE1 as the dogfooding vehicle, then C3/RE2 only against a profile.

---

## Out of scope

- Anything not gated on refinement types -- shipped in the archived parent plan.
- Cross-world systems / `World-Mirror` -- `xworld.tur` / `xstage.tur`, tracked
  separately.
- Routing `defcomponent-accessors` through `StorageOps` -- shipped (E2d-P6).
- Refinements in function types (`TUR-E0378`). `for-each` splices inline and
  `defsystem` resolves statically, so the ECS does not need them; a stage
  scheduler holding system *values* would, and that is a reason not to build
  the scheduler that way.
- Parameterized refinement aliases (`(deftype (Alive w) ...)`). Would let RE1
  write `[e : (Alive w)]` instead of inlining the predicate at every accessor.
  Real ergonomics, no semantics -- worth doing if C1 lands and the signatures
  are still unpleasant, not worth its own plan before then.

## References

- Parent plan (archived): [`ecs-spice-plan`](../../archive/ecs-spice-plan.md)
- [`refinement-types-plan.md`](refinement-types-plan.md) -- what landed
- [`refinement-types-guide.md`](../../guides/refinement-types-guide.md) -- the surface
- [`refined-graduation-plan.md`](refined-graduation-plan.md)
- [`refined-dogfooding-plan.md`](../../archive/refined-dogfooding-plan.md)
- [`loop-invariants-plan.md`](../hold/loop-invariants-plan.md)
- `docs/guides/ecs-guide.md`, `docs/guides/ecs-vs-haskell-ecs.md`,
  `docs/guides/ecs-storage-guide.md`
- `docs/guides/substructural-types-guide.md` -- the linear caps C2 leans on
