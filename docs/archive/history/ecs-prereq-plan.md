---
title: ECS prerequisite work -- unblocking E2 and beyond
category: Planning
description: Sequenced compiler/macro-evaluator prerequisites that the ECS spice phases needed. Historical -- all nine gaps (A-I) shipped 2026-06-11. Retained as a status record and to document the leverage ordering for future plan-vs-prereq sequencing decisions.
status: Closed -- all gaps A-I shipped 2026-06-11.
created: 2026-06-11
last-checked: 2026-06-11
---

# ECS prerequisite work -- unblocking E2 and beyond

> **Status 2026-06-11 (final):** All nine gaps surfaced by this plan
> shipped within a single day -- A through I in the status table
> below. The plan is retained for the leverage ordering and the
> what-blocked-what narrative; the action sections below describe
> what shipped, not what remains. There is no remaining work tracked
> here; future ECS gaps will be filed as fresh reports.

## Why this exists

E0 and E1' of [`ecs-spice-plan.md`](ecs-spice-plan.md) shipped, but
each phase generated multiple language-level gap reports and the
spice's user-facing surface drifted further from the plan as we
worked around them. E2 (systems and scheduler) is now blocked on the
same gaps -- specifically, attempting to ship `defsystem` in this
session ran into a fourth gap (macros cannot construct inline-C
blocks) within minutes.

Stopping to close the gaps will produce a better E2 / E3 / E4 than
continuing to layer workarounds. This document sequences that work
by leverage: smallest changes that unblock the most plan, first.

## Status snapshot

**Shipped:**

- E0 (skeleton): `entity`, `storage` (dense), `world`, smoke test.
- E1' (queries): `sparse` (Robin Hood after the fix), `tag`, `query`
  (for-each1..3, world-tagged?/untagged?), six test fixtures.
- Two compiler fixes already landed alongside the spice work:
  per-instantiation monomorphization for struct-by-value generics,
  and the `ecs/sparse` Robin Hood rewrite.

**Status as of 2026-06-11 (post-research):**

| ID | Report | Status | Affects |
|---|---|---|---|
| **A** | [ecs-macro-symbol-synthesis-missing.md](../archive/history/ecs-macro-symbol-synthesis-missing.md) | **Fixed** | Per-component accessors (name synthesis is in; emission is gap E), `defsystem` name-keyed metadata (same), `HasComponent` classes (same) |
| **B** | *(report never persisted; minimal repro now passes)* | **Fixed** | `dense-get` / `sparse-get` for struct components without witness arg |
| **C** | *(report never persisted; minimal repro now passes)* | **Fixed** | All macro authors using backquote + `dot-sym` |
| **D** | [macro-cannot-emit-inline-c-block.md](../archive/history/macro-cannot-emit-inline-c-block.md) | **Fixed 2026-06-11** -- `ct_eval_quasiquote` auto-wraps a CBLOCK-headed list with `do`. The misleading diagnostic is gone; the three known macro shapes (bare CBLOCK, `(do CBLOCK)`, bare `(CBLOCK)`) all compile and produce identical fn pointers. Regression: `tests/fixtures/macro-emits-list-with-cblock-head/`. | One specific failing macro shape; cosmetic diagnostic |
| **E** | [macro-cannot-emit-multiple-top-level-forms.md](../archive/history/macro-cannot-emit-multiple-top-level-forms.md) | **Fixed 2026-06-11** -- top-level `(do ...)` now splices into program items in `flatten_program_items`. Matches CL top-level-progn. `(defworld Name [Comps])` accessor generation and `(defsystem ...)` two-form emit are unblocked. Regression: `tests/fixtures/macro-emits-multiple-top-level-forms/`. | `defworld` per-component accessors; `defsystem` two-form emit; any "one declaration -> family of defns" macro |
| **F** | [top-level-def-init-dropped.md](../archive/history/top-level-def-init-dropped.md) | **Fixed 2026-06-11** -- `__attribute__((constructor))` wired into single-file emit via a dedicated `def_init_body` buffer; tests/fixtures/top-level-def-init-runs-before-main/ regression-tests it. Separate-compilation path at emit_module.c:6520 still has the same bug; flagged as follow-up. | Every top-level `(def name value)` declaration; potentially `stdlib/math.tur::PI`, `stdlib/reactor.tur::READ/WRITE/...`, `stdlib/schema.tur::SCHEMA_*` |
| **G** | [macro-unquote-in-type-position-rejected.md](../archive/history/macro-unquote-in-type-position-rejected.md) | **Fixed 2026-06-11** -- `substitute_params` now recurses into F_TYPE_ANN's payload so unquotes inside type slots get substituted. The ECS spice's `defcomponent-accessors` macro mints typed `get-/set-!/has-?` for one (world, component) pair; covered by `tests/defcomponent-accessors.tur` (spice) and `tests/fixtures/macro-unquote-in-type-position/` (compiler). | `defworld` per-component accessors; any macro-emitted typed defn parameterized over a type arg |
| **H** | [typeclass-constrained-defn-rejected.md](../archive/history/typeclass-constrained-defn-rejected.md) | **Fully fixed 2026-06-11.** All three items shipped: (1) defn parser accepts the `(defn name [TypeVars] [(Class V) ...] [params] :ret)` shape; (2) defensive null-def check in `elab_method_call` removes the typed-method-param SEGV; (3) `emit_abi_fn_skip_generic` skips the dead-code carrier body of a constrained polymorphic wrapper when a specialization has a TY_STRUCT arg, eliminating the carrier-int dispatch mismatch. The ECS plan's spec'd "typeclass-bounded systems" surface now works end-to-end over by-value struct worlds; the same polymorphic wrapper dispatches to multiple instances by receiver type. Regressions: `tests/fixtures/defn-class-constraint-list-syntax/`, `tests/fixtures/errors/typeclass-typed-method-param-null-def-safe/`, `tests/fixtures/typeclass-poly-wrapper-struct-receiver/`, `tests/has-component-polymorphic.tur` (spice). | ECS HasComponent classes (monomorphic SHIPPED; polymorphic SHIPPED); any world-polymorphic library |
| **I** | [ecs-defsystem-write-caps-not-enforced.md](../archive/history/ecs-defsystem-write-caps-not-enforced.md) | **Fixed 2026-06-11** -- Path A landed across Phases I1-I6. `ecs/cap` ships `WriteCap<T>` / `ReadCap<T>` as parametric `:linear` opaques (unblocked by the parametric-linear propagation fix in `src/compiler/types.c::type_app` / `propagate_app_discipline`). `defsystem` flipped from bitmask ints to component-name vectors and binds `<Comp>-write-cap` / `<Comp>-read-cap` in body scope for each entry in `:reads`/`:writes`; `defcomponent-accessors` cap-gates `set-<Comp>!` / `get-<Comp>` so a body that did not declare `:writes [Vel]` fails to elaborate when it tries to call `set-Vel!`. Regressions: `tests/fixtures/errors/ecs-defsystem-writes-unauthorized/` (main repo), plus eight positive and six negative tests under `../turmeric-spices/spices/ecs/tests/`. | Plan's compile-time race-freedom claim; honest E4 writeup; library systems that want to declare read-only effects |

## New gap discovered this session

While starting E2, a fourth gap surfaced that this plan needs to
capture before sequencing the fixes:

**Gap D: Macros cannot construct inline-C blocks.** A `defmacro`
body that backquotes an inline-C fence (`` `(```c ... ```) ``) fails
at expansion with "expression in call head has type `nil`, which is
not callable" -- the reader treats the C-fence as a value whose
position is then read as a call head rather than a quoted form.

```turmeric
(defmacro fnptr-of [name]
  `(```c
    return (void *)&my_hyfunc;
   ```))                                    ;; <- fails to expand
```

This blocks the natural shape of `defsystem`:

```turmeric
(defsystem physics [w] :reads [Pos Vel] :writes [Pos] body)
  =>
(defn physics-impl [w] body)
(defn physics-fnptr [] : ptr<void>
  ```c return (void *)&physics_hyimpl; ```)  ;; <- this can't be macro-emitted
(def  physics (make-system ... (physics-fnptr)))
```

without which the user writes the trampoline by hand once per
system. Filed as
[macro-cannot-emit-inline-c-block.md](../archive/history/macro-cannot-emit-inline-c-block.md);
it interacts with gap A (without A we can't synthesize
`physics-impl` / `physics-fnptr` names either) so they probably
want to be triaged together.

## Leverage ranking

I'm ranking by **(downstream plan unblocked) / (compiler work)**.

### Tier 1 -- ship-or-shelve unblocker

**A. `str->sym` builtin in the CT macro evaluator.** ✅ Shipped 2026-06-11.

- *Unblocks:* per-component `set-Pos!`/`get-Pos`/`remove-Pos`
  accessors (E0 plan target), `defworld`-generated `HasPos`/`HasVel`
  classes (E2 plan target), `defsystem` name-keyed metadata
  binding (E2 plan target), defcomponent registry with per-component
  ID constants, every future macro that wants a family of bindings
  keyed by a passed-in name.
- *Implementation:* `src/compiler/elab_macros.c:321-326`. Mirrors
  `dot-sym` (lines 303-314); takes an `F_STR` arg, interns via
  `symtab_intern`, returns a fresh `F_SYM`. Also listed in the
  `form_contains_ct_builtins` switch around line 210.
- *Validation:* the report's minimal repro now expands. Verified
  end-to-end with a `(defmacro mint-get [T] ...)` that mints
  `get-Pos` / `get-Vel` as top-level defns; output `7\n9` from
  the natural call sites.

### Tier 2 -- substantial leverage, modest scope

**D. Macros emit inline-C blocks.**

- *Unblocks:* `defsystem`'s auto-generated function-pointer
  trampolines (gets E2's parallel-on-threadpool path off the ground),
  and any future macro that needs to bridge to C (FFI generation,
  per-component memcpy fast paths, vectorized iteration emits).
- *Scope:* identify why `` `(```c ... ```) `` doesn't survive the
  quasiquote walker; likely a missing case in
  `src/compiler/elab_macros.c` around the F_CBLOCK handling
  (referenced at lines 107, 248, 692, 863, 888). Suspect the
  walker drops or evaluates `F_CBLOCK` inside backquote rather
  than treating it as a literal. Estimated medium-small once
  reproduced in a debugger.
- *Validation:* one fixture that uses a macro to emit a tiny inline-C
  defn (e.g. a `(make-getter Pos)` that emits `(defn pos-get [...]
  ```c ... ```)`).
- *Note:* if the walker also needs `__TUR_TY_` markers re-expanded
  in macro-emitted inline-C, that's a second item -- but the basic
  "macro can emit a literal C block" comes first.

### Tier 3 -- meaningful but workaround exists

**B. Return-type inference for generic `[A]`.** ✅ Found resolved on probe 2026-06-11.

- *Unblocks:* clean `(dense-get s i)` / `(sparse-get s i)` for
  struct-by-value components -- removes the witness-argument hack
  the E1' tests rely on (`dense-get-w` / `sparse-get-w`).
- *Verified:* all three minimal-repro variants
  (`(:: (make-default) :Pos)`, `(let [p : Pos (make-default)] p)`,
  `(defn via-return-type [] : Pos (make-default))`) now produce a
  Pos-specialized clone of the generic; `.x` reads as 0 in all
  three. The report file was drafted earlier in this session but
  did not persist to disk; the underlying fix must have landed
  alongside or before the per-instantiation monomorphization work.
- *Downstream cleanup:* `ecs/storage.tur:dense-get-w` and
  `ecs/sparse.tur:sparse-get-w` (plus three documentation references
  to the non-existent report file) can be removed in a follow-up
  spice-side sweep.

### Tier 4 -- correctness ick, workaround exists

**C. Backquote + `dot-sym` silently drops sibling forms.** ✅ Found resolved on probe 2026-06-11.

- *Unblocks:* normal backquote-style macro writing in the spice
  (right now `ecs/query.tur` is written in `(list ...)` style with
  a module-docstring caveat).
- *Verified:* the `pp-bad` minimal repro (macro with
  `\`(do (println 100) (println (~(dot-sym A) ~w)) (println 300))`)
  now prints `100\n7\n300` as expected, not just `300`. The report
  file was drafted earlier in this session but did not persist to
  disk; the underlying fix must have landed in the same pass as
  the gap-A `str->sym` work, since they touch the same CT-eval
  paths in `src/compiler/elab_macros.c`.
- *Downstream cleanup:* `ecs/query.tur`'s "written in `(list ...)`
  style because of backquote+dot-sym drop" docstring paragraph and
  the macros themselves can be rewritten back to natural backquote
  in a follow-up spice-side sweep.

## Recommended sequencing (historical -- what shipped, in order)

```
   A   B   C   D   E   F   G   H   I
   ─   ─   ─   ─   ─   ─   ─   ─   ─
   ✅  ✅  ✅  ✅  ✅  ✅  ✅  ✅  ✅   (all shipped 2026-06-11)
```

Gaps A-C were closed first (low-hanging-fruit and probe-shipped
work); D, E, F, G, H each landed as their own compiler fix in the
same window; I (the load-bearing ECS write-cap surface) shipped
last across the I1-I6 phase plan in the now-archived report.

**Why the prereq batch landed cleanly.** A unblocked identifier
synthesis (`str->sym`); E unblocked identifier emission (top-level
`(do ...)` splicing); G unblocked unquotes inside type slots; H
unblocked typeclass-constrained polymorphic defns; the parametric
`:linear` propagation fix unblocked Phase I's `WriteCap<T>` /
`ReadCap<T>` capability types. None required a redesign of the
existing macro language or type system -- they were missing cases
in already-shipping infrastructure.

## What ECS shipped against this plan

The prereq batch landed without forcing any of the originally
contemplated fallbacks. For the historical record:

- **The "pause ECS at E1'" option was not taken.** All nine
  compiler-side gaps closed in the same session window, so the
  spice's `main` branch resumed E2 with the full prereq surface
  available.
- **`defsystem` did not need the hand-rolled-trampoline workaround.**
  The implicit `defn-name -> ptr<void>` coercion plus gap E's
  top-level `(do ...)` splice together let the macro emit both the
  impl `defn` and the `(def name (make-system ...))` from one
  invocation, no inline-C trampoline. Gap D (macros emitting inline
  C) shipped anyway but turned out not to be on the critical path.
- **`defworld` per-component accessors shipped via
  `defcomponent-accessors`.** Gap G (unquote in type position) was
  the load-bearing piece; gaps A + E + G together let one macro
  call mint the typed `get-<Comp>` / `set-<Comp>!` / `has-<Comp>?`
  family. The I4 ship later cap-gated these accessors.
- **The HasComponent typeclass surface shipped end-to-end.** Gap H
  closed the typeclass-constrained-defn shape; the polymorphic
  wrapper now dispatches by receiver type across multiple
  instances, no two-step user invocation needed.
- **Compile-time write-cap enforcement shipped.** Gap I (the
  load-bearing ECS plan promise) landed across the I1-I6 phase plan
  in the now-archived report. The spice's `:writes [Pos]` body
  writing `Vel` example now fails to elaborate, as the plan
  originally promised.

## Order-of-operations -- what was actually done

All steps below shipped 2026-06-11:

1. **Filed gap D** as
   [`../archive/history/macro-cannot-emit-inline-c-block.md`](../archive/history/macro-cannot-emit-inline-c-block.md);
   narrowed by research to a surface-syntax pothole and fixed via
   `ct_eval_quasiquote` auto-wrap.
2. **Shipped A (`str->sym`)** -- `src/compiler/elab_macros.c:321-326`.
3. **Shipped E2** -- the spice's `defsystem` / `defworld` / accessor
   surface lit up against the unblocked compiler. Per-component
   `get-<Comp>` / `set-<Comp>!` and the `HasComponent` polymorphic
   wrapper both work.
4. **Spice-side cleanup pass** -- removed the `dense-get-w` /
   `sparse-get-w` witness variants and their callers (gap B made
   them obsolete); the natural-backquote rewrite of
   `ecs/query.tur`'s remaining `(list ...)` macros; corrected the
   archived-report link paths across spice docstrings and the
   README.
5. **Parallel scheduler (E2)** -- ships via `thread-pool-submit` and
   the implicit `defn-name -> ptr<void>` coercion.
6. **Phase I write-cap enforcement (I1-I6)** -- closed the gap I
   load-bearing surface, including a parametric-`:linear`
   propagation fix in `src/compiler/types.c` that unblocked the
   `WriteCap<T>` / `ReadCap<T>` capability shape.

## Status update 2026-06-11

What was done this session:

- Verified gap A (`str->sym`) is shipped at
  `src/compiler/elab_macros.c:321-326`. End-to-end smoke:
  `(defmacro mint-get [T] ...)` mints `get-Pos` / `get-Vel` as
  real top-level defns from a single macro invocation.
- Verified gap B is fixed against the report's three minimal-repro
  variants; all return a Pos-specialized clone now.
- Verified gap C is fixed against the report's `pp-bad` minimal
  repro; the do-block executes all three siblings.
- Filed gap D as a proper report
  ([`../archive/history/macro-cannot-emit-inline-c-block.md`](../archive/history/macro-cannot-emit-inline-c-block.md)).
- **Researched gap D in depth** (two parallel subagents on
  compiler internals + Lisp-family precedent, plus first-hand
  probes). Findings reshaped the gap:
  - The bug is narrow -- only `` `(```c ... ```) `` fails;
    `` `(do ```c ... ```) `` and bare `` ```c ... ``` `` both work.
  - The root cause is *not* in macro evaluation; it's the
    elaborator treating an F_CBLOCK-headed list as a call. Three
    fix locations identified, smallest is a `ct_eval_quasiquote`
    auto-wrap.
  - Lisp precedent (Clojure, SBCL, Chicken, Carp) consistently
    says "leaves in quasiquote self-evaluate." Turmeric's
    quasiquote walker already does this for F_CBLOCK -- the bug
    is downstream.
  - **The implicit `defn-name -> ptr<void>` coercion** (verified
    in `tests/fixtures/channel-basic/input.tur:174`) means
    `defsystem` does not need to emit inline-C at all. Gap D's
    "blocks parallel E2" framing was overstated.
  Full research notes in
  [`../archive/history/macro-cannot-emit-inline-c-block.md`](../archive/history/macro-cannot-emit-inline-c-block.md)
  § "Research findings 2026-06-11".
- **Filed gap E**
  ([`../archive/history/macro-cannot-emit-multiple-top-level-forms.md`](../archive/history/macro-cannot-emit-multiple-top-level-forms.md))
  discovered while starting step 3 of this plan. `(do (defn ...)
  (defn ...))` at top level aborts the emitter with `EX_FN_DEF in
  stmt position`. `defmodule` works but doesn't surface names to
  the call site. Blocks the spec'd shapes of `defworld` accessor
  generation and `defsystem`; does not block E2 functionality (the
  user-side workaround is to write the boilerplate by hand).

What was *not* done this session (left for follow-ups):

- The spice-side cleanup sweep (step 4 above): `ecs/storage.tur`,
  `ecs/sparse.tur`, `ecs/query.tur`, and the spice README still
  reference the non-persisted B/C report files (six dangling links
  total) and still use the `-w` witness variants where the real
  API would now work.
- Verified that `tests/fixtures/macro-str-to-sym/` exists and
  regression-tests the gap-A contract -- no new fixture needed.

## Why this matters for the plan, not just the spice

The ECS spice is partly an empirical comparison ("how far can a
language with coherent typeclasses get with the ECS pattern before
the compiler has to give up and start trusting the programmer?").
Each gap currently surfaced is exactly that: a place where the spice
has to give up and trust the programmer (or ask them to type more
ceremony). Closing the gaps changes the answer to the empirical
question -- and the eventual `docs/guides/ecs-vs-haskell-ecs.md`
writeup is meaningfully different depending on what shipped.

The macro-symbol-synthesis gap especially is upstream of *many*
spices, not just ECS. Frame-DSL (currently planning a `defframe`
macro that names per-column accessors), the eventual session-types
front-end (planning per-protocol classes), and any future
typeclass-derive macro all want the same `str->sym` primitive.
Landing it earns interest beyond this spice.

## Validation -- what shipped against the original checklist

The prereq batch's success criteria (kept verbatim from the
2026-06-11 plan), all met:

- ✅ The spice README's "Known limitations" section shrinks by at
  least three entries -- the original items 1-3 (per-component
  accessors, generic return-type inference, backquote/dot-sym
  sibling drop) are gone; only the for-eachN dense-only iteration
  note remains.
- ✅ The E2 plan's "`defworld` ... generates per-component
  `get-Pos`, `set-Pos`, ..." sentence is now literally true; ships
  via `defcomponent-accessors`.
- ✅ The E2 plan's "Auto-generated `HasComponent` classes" sentence
  is implementable and ships in `ecs/world.tur::defcomponent-class`
  / `defcomponent-class-instance`.
- ✅ The `dense-get-w` / `sparse-get-w` witness variants are gone
  from `ecs/storage.tur` and `ecs/sparse.tur`.
- ✅ `ecs/query.tur`'s "written in (list ...) style" caveat
  paragraph is gone; the three remaining `(list ...)` macros got
  rewritten to natural backquote.

In addition (not on the original checklist, but spec'd by the broader
ECS plan):

- ✅ Compile-time write-cap enforcement -- a body declaring
  `:writes [Pos]` and trying to write `Vel` now fails to elaborate.
  Originally listed in this doc as "Open"; shipped 2026-06-11 via
  the I1-I6 phase plan (see gap I in the status table).

## References

- [`ecs-spice-plan.md`](ecs-spice-plan.md) -- the plan this prereq
  work unblocked.
- [`../archive/history/ecs-macro-symbol-synthesis-missing.md`](../archive/history/ecs-macro-symbol-synthesis-missing.md)
  -- gap A.
- [`../archive/history/macro-cannot-emit-inline-c-block.md`](../archive/history/macro-cannot-emit-inline-c-block.md)
  -- gap D.
- [`../archive/history/macro-cannot-emit-multiple-top-level-forms.md`](../archive/history/macro-cannot-emit-multiple-top-level-forms.md)
  -- gap E.
- [`../archive/history/top-level-def-init-dropped.md`](../archive/history/top-level-def-init-dropped.md)
  -- gap F.
- [`../archive/history/macro-unquote-in-type-position-rejected.md`](../archive/history/macro-unquote-in-type-position-rejected.md)
  -- gap G.
- [`../archive/history/typeclass-constrained-defn-rejected.md`](../archive/history/typeclass-constrained-defn-rejected.md)
  -- gap H.
- [`../archive/history/ecs-defsystem-write-caps-not-enforced.md`](../archive/history/ecs-defsystem-write-caps-not-enforced.md)
  -- gap I; the I1-I6 phase plan that shipped it.
- [`../archive/history/parametric-linear-opaque-not-enforced.md`](../archive/history/parametric-linear-opaque-not-enforced.md)
  -- the compiler-side enabler for gap I.
- `src/compiler/elab_macros.c` -- where A, D, G land.
- `src/compiler/types.c::propagate_app_discipline` -- where the
  parametric `:linear` propagation fix lands.
