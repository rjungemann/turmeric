# Stdlib Advanced Typing Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** stdlib API hardening -- linearity, effects, sessions, refinement, typeclass consolidation
> **Sibling plan:** [stdlib-opaque-handle-types-plan.md](stdlib-opaque-handle-types-plan.md)

---

## Overview

This plan covers the stdlib hardening opportunities that go beyond plain
`defopaque` newtypes. It is the companion to
[stdlib-opaque-handle-types-plan.md](stdlib-opaque-handle-types-plan.md),
which already addresses identity-based handle typing for the concurrency
core and mid-level sync primitives.

Five language features have either shipped or have partial stdlib support
but are under-applied:

1. **Linearity / affinity** -- `stdlib/io.tur:288` declares `FileHandle :linear`
   as a template, but the high-traffic resources (mutex, chan, promise,
   tmpfile, process, taskgroup, reactor, allocations) are not linear.
2. **Session types** -- `-Xsessions` infrastructure exists and `stdlib/session.tur`
   has demo protocols, but the production `Chan`/`AsyncChan` API stays untyped.
3. **Effects / capabilities** -- `#{Unsafe}` syntax is in use (see `reactor.tur`)
   and `stdlib/{capability,effects}.tur` are opt-in, but the I/O-touching
   stdlib (`fs`, `process`, `env`, `net`, `random`) has no effect annotations.
4. **GADTs / refinement** -- `sized.tur`/`sized-buf.tur` model sized vectors,
   and `gadt-vec.tur` proves the GADT machinery works, but partial functions
   in `list.tur`/`vec.tur`/`slice.tur` remain partial and `range.tur` still
   encodes bound kinds as ints.
5. **HKT typeclass consolidation** -- HKT phases S1-S8 are complete, but
   `parsec.tur`/`logic.tur`/`backtrack.tur` each re-implement the
   `Monad`/`Alternative` interface, and `result.tur`/`option.tur` are missing
   `Bifunctor`/`MonadError` instances that downstream stdlib consumers
   (`httpd.tur`, `csv.tur`, `json.tur`) end up open-coding.

Each section below stands alone; the phases can be cut into independent PRs.

---

## Relationship to the opaque-handle plan

The opaque-handle plan wraps handles in `defopaque` newtypes. This plan
**layers on top of that work** in two places, so ordering matters:

- **Linearity (Phase L)** modifies the same handle declarations the opaque
  plan touches. It must land *after* the opaque plan's Phase 1-2, or be
  folded into them as a follow-up commit per module.
- **Session types (Phase S)** wraps `Chan`/`AsyncChan` -- whose `defopaque`
  newtypes come from the opaque plan's Tier 1. Sequence it after Phase 1 of
  the opaque plan.

The remaining sections (effects, refinement, typeclass consolidation) are
independent of the opaque-handle work and can land in any order.

---

## Phase L -- Linearity / affinity for resource handles

### Motivation

`stdlib/io.tur:288` already uses `defopaque FileHandle :linear`, which the
type checker tracks for use-exactly-once / use-at-most-once discipline. The
same discipline catches the following classes of bugs at compile time:

- Double-free of a `Chan`, `Mutex`, `Promise`, or `TaskGroup`.
- Use-after-free across reactor callbacks.
- Forgetting `task-group-wait` before dropping a group.
- Calling `promise-fulfill` twice on the same `Promise`.

### Inventory

| Module           | Handle              | Discipline | Notes |
|------------------|---------------------|------------|-------|
| `tur/mutex`      | `Mutex`             | linear     | new/free pair |
| `tur/chan`       | `Chan`, `AsyncChan` | linear     | |
| `tur/future`     | `Promise`           | linear     | consumed by `promise-fulfill` |
| `tur/future`     | `Future`            | affine     | optional cancel / drop |
| `tur/taskgroup`  | `TaskGroup`         | linear     | `wait` consumes |
| `tur/reactor`    | `Reactor`           | linear     | |
| `tur/fs`         | `TmpFile`           | linear     | implicit unlink-on-close |
| `tur/process`    | `ChildHandle`       | linear     | `wait` consumes |
| `tur/serial`     | `Bytes`             | linear     | `bytes-alloc` / `bytes-free` |

### Design

Promote the opaque declarations from the opaque-handle plan to `:linear`
(or `:affine` for cancellable resources):

```turmeric
(defopaque Promise   :ptr<void> :linear)   ;; consumed by promise-fulfill
(defopaque Future    :ptr<void> :affine)   ;; may be dropped uncollected
(defopaque TaskGroup :ptr<void> :linear)   ;; wait consumes
```

Constructor / consumer signatures:

```turmeric
(defn promise-new []           : Promise)
(defn promise-fulfill [p : Promise v : int] : nil)   ;; consumes p
(defn task-group-wait [g : TaskGroup] : int)        ;; consumes g
```

The opaque-handle plan's "C-side declarations are unaffected" guarantee
still holds: the linearity attribute is enforced in the type checker, not
the C ABI.

### Phasing

1. **L1** -- Promote mutex/chan/promise handles to linear in lockstep with
   their opaque-handle PRs (or as the immediately following PR per module).
2. **L2** -- Taskgroup/reactor/process/tmpfile (touches more call sites).
3. **L3** -- Re-audit `tur/io` consumers; convert `bytes-alloc`/`bytes-free`
   in `tur/serial`.

### Risks

- Linearity errors will surface in user code that today legally aliases
  handles. Provide an `unsafe-dup` escape hatch on each linear newtype for
  the rare cases where aliasing is intended.
- Closure captures of linear values need the existing "closure consumes"
  rule; surface this in the `tur/effects` guide.

---

## Phase S -- Session-typed channel wrappers

### Motivation

`stdlib/session.tur` ships with `-Xsessions` and demonstrates protocol
typing, but the channels used in practice (`tur/chan`, `tur/taskgroup`,
`tur/reactor` event sources) are untyped `int` / `:ptr<void>` payloads. A
session-typed wrapper makes the request/response shape of a worker channel
or RPC pipe a compile-time check.

### Design

A thin generic wrapper over the (already-opaque) `Chan` newtype:

```turmeric
(defopaque SChan p :ptr<void> :linear)   ;; p :: protocol phantom

(defn schan-new   [:p]                       : SChan<p>)
(defn schan-send  [c : SChan<Send T rest> v : T] : SChan<rest>)
(defn schan-recv  [c : SChan<Recv T rest>]      : Pair<T SChan<rest>>)
(defn schan-close [c : SChan<Close>]            : nil)
```

The implementation delegates to `chan-send`/`chan-recv`; the protocol is a
phantom parameter advanced by each operation. Combined with linearity
(Phase L), this enforces that every protocol step happens exactly once and
in order.

### Scope

- Wrapper module `stdlib/schan.tur`. Original `tur/chan` keeps its untyped
  surface for low-level use.
- Convert `taskgroup` worker-pool example fixtures to demonstrate the
  wrapper end-to-end.
- Defer wrapping `reactor` event sources to a follow-up -- the variant
  shape there is more complex (multiple source kinds in one queue).

---

## Phase E -- Effect rows on I/O-touching stdlib

### Motivation

`#{Unsafe}` is already in use (e.g. `reactor.tur`) and the language has
effect-set syntax. The I/O-touching stdlib (`fs`, `process`, `env`, `net`,
`random`, `log`, `time`) silently performs side effects with no signal in
the signature. Annotating them with coarse effect tags is a one-pass change
that gives the type system capability discipline without changing semantics.

### Design

Standardise on five new effect tags:

| Tag        | Used by                                 |
|------------|-----------------------------------------|
| `#{IO}`    | umbrella, implied by the others         |
| `#{FS}`    | `fs.tur`, `path.tur`, `tmpfile`         |
| `#{Net}`   | `net.tur`, `async_socket.tur`, `httpd.tur` |
| `#{Proc}`  | `process.tur`, `env.tur`                |
| `#{Rand}`  | `random.tur`                            |

Annotate existing signatures; no call-site changes required for code that
doesn't opt into effect checking. Code that does opt in (via a
`requires-effect-checking` directive at module top) gets the discipline.

### Phasing

1. **E1** -- Land tag definitions in `stdlib/effects.tur`; document in
   `docs/guides/effects-guide.md`.
2. **E2** -- Annotate `fs`, `path`, `process`, `env`, `random`, `net`.
3. **E3** -- Annotate downstream: `httpd`, `log`, `csv` (via fs), `json`
   (via fs/net helpers).

### Out of scope

- No effect *inference*; this pass is purely annotation.
- No effect *masking* / handlers; that is `tur/capability`'s job and is
  unchanged.

---

## Phase R -- Refinement / GADT bridges for collections

### Motivation

`sized.tur`/`sized-buf.tur` already model `SizedVec n` / `SizedBuf n`, and
`gadt-vec.tur` demonstrates GADT typing for length-indexed vectors. Two
partial-function clusters remain unprotected:

1. `list-head`, `vec-first`, `slice-first` are partial on empty input.
2. `vec-get`, `slice-get` accept unchecked indices.

`range.tur` separately encodes inclusive/exclusive/unbounded as sentinel
ints (`range-bound-new [inclusive :int ...]`, `range-abort-not-connected`),
a classic stringly-typed sum that the GADT machinery can replace.

### Design

```turmeric
(defopaque NonEmpty A :int)                    ;; phantom-tagged Cons
(defn ne-head  [xs : NonEmpty<A>] : A)           ;; total
(defn ne-of    [x  : A xs : List<A>] : NonEmpty<A>)
(defn ne-from? [xs : List<A>]  : Option<NonEmpty<A>>)

(defopaque BoundedIdx n :int)                  ;; refinement on int
(defn vec-get-checked [v : SizedVec<n A> i : BoundedIdx<n>] : A)
```

And a GADT for range bounds:

```turmeric
(defadt Bound A
  Inclusive [A]
  Exclusive [A]
  Unbounded [])
```

### Scope

- Add `NonEmpty`, `BoundedIdx` to `stdlib/sized.tur` (or a new
  `stdlib/refined.tur`).
- Rewrite `tur/range`'s internal bound representation to use the GADT;
  keep a wrapped `int`-returning compatibility shim for the existing API.

### Phasing

1. **R1** -- `NonEmpty` + total accessors. Migrate `option.tur`, `list.tur`
   internal helpers.
2. **R2** -- `BoundedIdx` + sized-vec checked indexing. Opt-in.
3. **R3** -- `range.tur` bound-kind GADT rewrite.

### Out of scope

- Decidable refinement solving; the user supplies the proof obligation via
  `ne-from?`-style smart constructors.

---

## Phase T -- HKT typeclass consolidation

### Motivation

HKT phases S1-S8 are complete (see `project_hkt_phase.md`). Three stdlib
modules still ship hand-rolled monad interfaces:

- `stdlib/parsec.tur` -- `pmzero`, `preturn`, `pmplus`, `pmbind`
- `stdlib/logic.tur` -- `mzero`, `mreturn`, `mplus`, `mbind`
- `stdlib/backtrack.tur` -- same shape, third copy

Separately, `result.tur` and `option.tur` are missing `Bifunctor` /
`MonadError` instances; downstream (`httpd.tur`, `csv.tur`, `json.tur`)
re-implements `result-map`/`result-and-then` chains by hand.

### Design

```turmeric
(definstance Monad       Parser   ...)
(definstance Alternative Parser   ...)
(definstance Monad       Logic    ...)
(definstance Alternative Logic    ...)
(definstance Monad       Backtrack ...)
(definstance Alternative Backtrack ...)

(definstance Bifunctor   Result   ...)
(definstance MonadError  Result   ...)
```

After the instances land, `parsec`/`logic`/`backtrack` users get
`for`/`do-m` from `stdlib/macros.tur` for free and the bespoke combinators
can be deleted. Downstream `result-map` open-coding in `httpd`/`csv`/`json`
becomes `fmap` / `bimap` / `>>=` against the new instances.

### Phasing

1. **T1** -- `Bifunctor`/`MonadError` for `Result`; convert one downstream
   consumer (`csv.tur`) as the proof.
2. **T2** -- `Monad`/`Alternative` for `Parser`; delete bespoke combinators.
3. **T3** -- `Logic`, `Backtrack`. (Largest test surface; ship last.)

### Out of scope

- Refactoring the underlying parser / logic engine internals.
- Adding new typeclass hierarchies (Comonad-Free, Profunctor, etc.); this
  is purely consolidation of what stdlib already inlines.

---

## Cross-cutting risks

- **Snapshot churn.** Each phase touches `tests/fixtures/*/expected.c`.
  Follow the standard snapshot regeneration recipe in `CLAUDE.md` and
  commit snapshots with the change.
- **Conflict with opaque-handle plan.** Phases L and S must sequence after
  the corresponding opaque-handle phases. Phases E, R, T are independent.
- **User-visible breakage.** Linearity (Phase L) and total accessors
  (Phase R) will reject programs that today silently misuse handles or
  partial functions. Document the migration in the changelog for each
  phase and provide `unsafe-dup` / `ne-from?` escape hatches.

---

## Acceptance criteria (per phase)

- Affected modules expose the new types / instances.
- `bash tests/run.sh` passes with zero `FAIL` lines (ASan/LSan on).
- A representative fixture under `tests/fixtures/` exercises the new
  discipline -- e.g. a "double-fulfill" test that now fails to compile
  for Phase L1, a session protocol mismatch for Phase S, a `result`
  consumer using `do-m` for Phase T1.
- `tur run docs` regenerated; new types/instances appear in the API
  reference.
