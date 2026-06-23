---
title: ECS Cross-World Followups Plan
category: Planning
description: Lift the 64-system-per-wave hard cap, generalize from two-world to N-world `defxsystem`, auto-generate world-box scaffolding instead of asking users to hand-write `box-<W>` / `load-<W>` / `free-<W>-box`, expand `defmirror` to multi-component, and accept clause order flexibly.
---

# `tur-ecs` Cross-World Followups -- Plan

## Context

Cross-world `defxsystem`, `defmirror`, and the `XStage` scheduler
shipped in `../turmeric-spices/spices/ecs/`. The user-facing surface
is documented in
[`docs/guides/ecs-guide.md#cross-world-systems`](../../guides/ecs-guide.md#cross-world-systems).
The original plan that motivated the work is now at
[`docs/archive/history/ecs-cross-world-systems-plan.md`](../../archive/history/ecs-cross-world-systems-plan.md).

Writing the guide surfaced five rough edges in the shipped surface,
all of which are tractable without changing the scheduler's static
non-conflict story:

1. **Hard 64-system cap per wave.** `xstage.tur` line 223 sizes the
   per-wave index buffer as `int idxs[64]`. A scheduler whose graph
   exceeds 64 ready systems in any wave silently truncates the wave.
   Real games with sized worlds + many extract systems are within
   sight of this cap.
2. **Two-world only.** `defxsystem` accepts exactly two world
   parameters; the macro's clause-parser counts positions hardcoded
   for 12 tokens of header (6 per world). A three-world snapshot/diff
   /apply pipeline (`AuthoritativeWorld` -> `PredictedWorld` ->
   `RenderWorld`) needs three `(world, reads, writes)` triples.
3. **Hand-written world-box plumbing.** Each world needs the user to
   define `(def <C>-cid N)` per component and `box-<W>` /
   `load-<W>` / `free-<W>-box` helpers per world. This is mechanical
   scaffolding that the macro could generate; today's user-side
   boilerplate runs ~20-40 lines per world and is easy to get wrong
   (off-by-one cids, miscounted boxes).
4. **`defmirror` is one-component only.** A render extract that copies
   `Pos + Sprite + Color` together needs three separate `defmirror`s
   today. They all run, but the scheduler can't see "these three move
   together," so it has no opportunity to fuse them, and the user
   surface is noisier than it should be.
5. **Clause-order rigidity.** `defxsystem` requires the four clauses
   in fixed order (`reads w0`, `writes w0`, `reads w1`, `writes w1`).
   The plan doc framed this as a non-issue; in practice, a system that
   reads from one world and only writes to another wants to omit the
   empty clauses, not write `:writes-to src []` / `:reads-from dst []`
   as boilerplate.

## Goals

1. **CAP-V0 -- lift the 64-cap.** Wave buffer grows with the system
   count; no silent truncation; existing behavior for `<= 64` is
   unchanged.
2. **N-W-V0 -- N-world `defxsystem` and `XStage`.** Three or more
   world parameters; the per-(world, component) lock-target model is
   identical, only the bookkeeping arity changes.
3. **GEN-V0 -- macro-generated world-box scaffolding.** A
   `defworld` form that takes the component set and emits the box +
   load + free helpers plus the cid constants. Existing hand-written
   helpers keep working.
4. **MULTI-MIR-V0 -- multi-component `defmirror`.** One mirror
   declares N components; the generated `XSystem` reads the source
   tuple in one query pass and writes all N targets in one block.
5. **CLAUSE-V0 -- clause-order flexibility.** Accept clauses in any
   order; omitted `reads`/`writes` for a world default to `[]`.

## Non-goals

- Auto-detecting which worlds a system touches from its body. The
  explicit clauses ARE the static-non-conflict declaration; that's a
  feature, not a verbosity tax.
- A cross-world query operator (`for-each` that crosses worlds and
  joins by entity ID). Joins across worlds are an application concern
  -- the existing pattern of "extract to a small world, then query
  that world" is fine.
- Migration paths between two-world and N-world systems for users.
  N-world is a superset; two-world calls don't change.
- A scheduler that re-plans waves dynamically based on runtime queue
  depth. Static planning is the whole point.

## Design

### CAP-V0 -- dynamic wave buffer

```c
/* current: */
int idxs[64]; int n_in_wave = 0;

/* after: */
int *idxs = NULL; int n_in_wave = 0, cap_in_wave = 0;
/* grow with realloc(2x) on append; freed at wave-end. */
```

The buffer is wave-scoped, so `free` on wave completion is enough --
no long-lived allocation, no GC interaction. The constant 64 was
already a "pretty big, probably enough" guess; the runtime overhead
of `realloc(NULL, 16)` -> `realloc(_, 32)` -> ... is dominated by the
work the systems themselves do.

Same change in any sibling buffers in `xstage.tur` that share the
hardcoded `[64]` shape -- grep before editing.

### N-W-V0 -- N-world `defxsystem`

The shipped macro parses 12 fixed positions plus body:

```
0:reads-from 1:w0 2:r0vec 3:writes-to 4:w0 5:w0vec
6:reads-from 7:w1 8:r1vec 9:writes-to 10:w1 11:w1vec 12:body
```

Generalize to "parse `[(:reads-from W [C ...]) | (:writes-to W [C ...])]*
body`": consume clauses until the next token is not a recognized
clause keyword, then the rest is the body. Validate that every world
name in `worlds` appears in at least one clause (warn if a world has
no clauses; that's a real "did you mean to use it?" smell).

`XStage` itself becomes a vector of `(world, cid-bit)` lock targets
rather than a fixed two-world record. The scheduler's wave logic is
identical: a system is ready when every (world, component) read/write
it claims is unblocked.

**The 2-world path stays valid syntactically** -- the generalized
parser accepts the old shape unchanged. CLAUSE-V0 adds the
empty-clause-omission convenience separately.

### GEN-V0 -- `defworld`

```turmeric
;; today (user-written):
(def Pos-cid 0)
(def Vel-cid 1)
(def Sprite-cid 2)
(defn box-Sim   [w : Sim] : ^linear WorldBox<Sim> ...)
(defn load-Sim  [^borrow b : WorldBox<Sim>] : Sim ...)
(defn free-Sim-box [^linear b : WorldBox<Sim>] : void ...)

;; after:
(defworld Sim
  :components [Pos Vel Sprite])
;; emits the cid constants, the boxes, and registers the world with the
;; cap discipline.
```

The macro emits:

- `(def <C>-cid <i>)` for each `C` in `:components`, numbered in
  declaration order. (Numbering must be stable across rebuilds; the
  macro literally counts the list.)
- `box-<W>` / `load-<W>` / `free-<W>-box`, all parametric on `<W>`
  via the existing `WorldBox<T>` opaque.
- A registration call into the per-process world registry so
  `tur-ecs/inspect` (a future debugging surface) can enumerate worlds.

**Compatibility.** Users who already hand-wrote the scaffolding keep
it; `defworld` does not overlap with existing names unless `:components`
listing matches the user's existing `(def ...cid)` numbering. Document
the migration as "delete your hand-written helpers; add `defworld`."

### MULTI-MIR-V0 -- multi-component `defmirror`

```turmeric
(defmirror render-extract
  :count 10000
  :from  SimWorld
  :to    RenderWorld
  :components [Pos Sprite Color])  ;; was: :component Pos
```

The generated `XSystem` reads the three-component tuple from the
source world (one `for-each3` pass), then writes the target world's
three components in one block under the existing per-component
write cap. The lock-targets are
`(src, Pos|Sprite|Color)` read and `(dst, Pos|Sprite|Color)` write
-- the OR is just multiple targets in the same system, which the
scheduler already handles.

`:component` (singular) stays as sugar for the one-element list.

The plan's
[archived motivation](../../archive/history/ecs-cross-world-systems-plan.md)
called out render extract as the primary use case; render extract
"always" wants more than one component, so this is closing an
underdone surface, not adding a new feature.

### CLAUSE-V0 -- clause-order flexibility

Two relaxations:

1. **Any order.** The macro consumes clauses until a non-clause
   token appears; world doesn't have to come in declaration order.
2. **Omit empty clauses.** A world that only reads and doesn't write
   (or vice versa) omits the empty side; the macro defaults the
   missing side to `[]`.

```turmeric
;; today (mandatory empty clauses):
(defxsystem extract-positions [sim ren]
  :reads-from  sim [Pos]
  :writes-to   sim []           ;; required even though empty
  :reads-from  ren []           ;; required
  :writes-to   ren [Pos]
  ...)

;; after CLAUSE-V0:
(defxsystem extract-positions [sim ren]
  :reads-from sim [Pos]
  :writes-to  ren [Pos]
  ...)
```

The validation that "every declared world appears at least once"
keeps unintentional drops honest; if you list `ren` in the worlds
vector but never read or write it, that's a warning.

## Work items

| # | Item | File(s) |
|---|------|---------|
| CAP-V0.1 | Replace fixed `idxs[64]` with grow-on-append buffer; free on wave end. Audit `xstage.tur` for any other hardcoded `[64]`. | `../turmeric-spices/spices/ecs/src/ecs/xstage.tur` |
| CAP-V0.2 | Test: a scheduler graph with 200 ready systems in one wave runs all 200 (not 64); compare against a tracker counter inside each system body. | `../turmeric-spices/spices/ecs/tests/xstage-wide-wave.tur` (new) |
| N-W-V0.1 | Generalize the `defxsystem` clause parser to N worlds; validate every declared world appears in >= 1 clause. | `../turmeric-spices/spices/ecs/src/ecs/xsystem.tur` |
| N-W-V0.2 | Generalize `XStage` lock-target storage from 2-tuple to vector. | `../turmeric-spices/spices/ecs/src/ecs/xstage.tur` |
| N-W-V0.3 | Tests: 3-world `defxsystem` schedules without conflict; existing 2-world fixtures still pass byte-equal codegen (snapshot diff). | new fixture + `tests/xworld-*.tur` |
| N-W-V0.4 | Guide update: extend the "Cross-world systems" section with a 3-world example (snapshot -> predicted -> render). | `docs/guides/ecs-guide.md` |
| GEN-V0.1 | `defworld` macro in a new module `ecs/world.tur`; emits cid constants + box/load/free + registration call. | `../turmeric-spices/spices/ecs/src/ecs/world.tur` (new) |
| GEN-V0.2 | Tests: defworld produces the same compiled output as the hand-written scaffolding (golden test); two `defworld`s in one TU don't collide on cid numbering. | new fixture |
| GEN-V0.3 | Guide update: "Defining a world" subsection becomes the canonical form; hand-written form moves to an "Advanced / migration" callout. | `docs/guides/ecs-guide.md` |
| MULTI-MIR-V0.1 | `defmirror :components [...]` (and keep `:component` as sugar); generate the multi-component XSystem body. | `../turmeric-spices/spices/ecs/src/ecs/xmirror.tur` |
| MULTI-MIR-V0.2 | Tests: 3-component mirror copies all 3; the scheduler sees the right read+write cap sets; per-component throughput vs three separate mirrors is no worse than ~3x (sanity, not perf gate). | `../turmeric-spices/spices/ecs/tests/xmirror-multi.tur` (new) |
| MULTI-MIR-V0.3 | Guide update: the "Mirror shorthand" example uses `:components`. | `docs/guides/ecs-guide.md` |
| CLAUSE-V0.1 | Macro: accept clauses in any order; default missing side to `[]`; warn on declared-but-unused world. | `../turmeric-spices/spices/ecs/src/ecs/xsystem.tur` |
| CLAUSE-V0.2 | Tests: every order permutation parses to the same body; omitting empties produces equivalent codegen; declared-but-unused world warns. | new fixture |
| CLAUSE-V0.3 | Guide update: the existing "two-world `defsystem` syntax" subsection drops the empty-clauses. | `docs/guides/ecs-guide.md` |

**Ordering.**

- CAP-V0 is independent and tiny; land first.
- N-W-V0 depends on CAP-V0 only because N-W-V0's tests want wide
  waves; either order works.
- CLAUSE-V0 should land *with* or *after* N-W-V0 since both touch
  the clause parser; one PR is cleaner.
- GEN-V0 is independent.
- MULTI-MIR-V0 is independent (the mirror macro is its own
  expansion).

## Testing

Existing `../turmeric-spices/spices/ecs/tests/xworld-*.tur` fixtures
cover the two-world happy path. The new tests above slot in as
siblings; the `tests/run.sh` discovery is automatic.

`tests/xstage-wide-wave.tur` (CAP-V0.2) is the most load-bearing new
test: it's the regression check that the 64-cap doesn't come back via
some other hardcoded buffer.

Add a `requires.dedicated-runner` marker only if any new test needs
isolation; the existing xworld tests run under the standard worker.

## Risk

- **GEN-V0 cid-numbering compatibility.** A user who today writes
  `(def Pos-cid 0)` `(def Sprite-cid 1)` and then switches to
  `(defworld Sim :components [Pos Sprite])` gets the same numbering
  by coincidence -- but reversing the `:components` list flips the
  cids and breaks any persisted save file keyed by cid. The guide
  must call out: **cids are stable per `:components` order; reordering
  is a breaking change to any external consumer of cid numbers**.
- **N-W-V0 might tempt over-decomposition.** Three worlds is
  legitimate; thirteen worlds is probably a missed-design smell. The
  guide should set the expectation that N-world is for the specific
  motivating shapes (snapshot/predict/render, sim/auth/render), not
  for "split this enum into one world per arm."
- **CAP-V0's silent truncation today is a latent bug.** Existing
  consumers in a > 64 wave configuration are silently dropping work.
  Worth a `git grep` of in-tree consumers (ECS demo, raylib demo)
  before claiming CAP-V0 has no functional change for existing code
  -- if there's a demo currently quietly missing systems, fixing it
  is a behavior change.
- **MULTI-MIR-V0 per-component vs grouped lock-target.** A mirror
  that writes three components must claim three write caps in the
  same system. If two of the three write caps are taken by a
  different system, the multi-mirror is gated on all three; users
  who want independence should keep separate `defmirror`s. Document
  the tradeoff.

## Out of scope

- Cross-world `for-each` joins.
- Dynamic re-planning of waves at runtime.
- Auto-detecting touched worlds from system bodies.
- Per-entity migration helpers between worlds (this is what mirror
  IS, just framed differently).
- A world-versioning / snapshot-rewind facility.
- ECS persistence (save/load) -- separate concern; sometimes a
  consumer of cids and worlds but not in this plan.
