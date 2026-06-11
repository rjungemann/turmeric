---
title: ECS prerequisite work -- unblocking E2 and beyond
category: Planning
description: Sequenced compiler/macro-evaluator prerequisites that the next ECS spice phases need. Lists each blocker by leverage, ranks them, and proposes a minimum-viable landing order so we stop accumulating workarounds.
status: Plan -- Tier 1 (A) shipped 2026-06-11; Tiers 3 (B) and 4 (C) found resolved on probe; only Tier 2 (D) remains live.
created: 2026-06-11
last-checked: 2026-06-11
---

# ECS prerequisite work -- unblocking E2 and beyond

> **Update 2026-06-11:** Tier 1 (gap A, `str->sym`) is shipped --
> `src/compiler/elab_macros.c:321-326`. While verifying, gaps B
> (return-type inference) and C (backquote/dot-sym) also probe as
> resolved against the minimal repros in their reports. The original
> B and C report files were drafted earlier in the session but did
> not persist to disk, so they're not in `docs/reported/` -- they
> went straight from "filed in narrative" to "fixed upstream." Only
> gap D (macro emitting inline-C) remains live. See § "Status update
> 2026-06-11" near the bottom.

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
| **D** | [macro-cannot-emit-inline-c-block.md](../reported/macro-cannot-emit-inline-c-block.md) | **Open -- but no longer blocks the plan** (research 2026-06-11 narrowed it: workaround is one extra `do`-wrap; ECS doesn't need inline-C macros at all -- top-level defn names auto-coerce to `ptr<void>`) | One specific failing macro shape; cosmetic diagnostic |
| **E** | [macro-cannot-emit-multiple-top-level-forms.md](../reported/macro-cannot-emit-multiple-top-level-forms.md) | **Open** -- discovered 2026-06-11 while executing step 3 of this plan. Blocks `(defworld Name [Comps])` accessor generation and `(defsystem ...)` two-emit shape. Workaround: user writes `(def physics (make-system ... (fn [w] body)))` by hand instead of `(defsystem physics ...)`. | `defworld` per-component accessors; `defsystem` two-form emit; any "one declaration -> family of defns" macro |
| **F** | [top-level-def-init-dropped.md](../reported/top-level-def-init-dropped.md) | **Fixed 2026-06-11** -- `__attribute__((constructor))` wired into single-file emit via a dedicated `def_init_body` buffer; tests/fixtures/top-level-def-init-runs-before-main/ regression-tests it. Separate-compilation path at emit_module.c:6520 still has the same bug; flagged as follow-up. | Every top-level `(def name value)` declaration; potentially `stdlib/math.tur::PI`, `stdlib/reactor.tur::READ/WRITE/...`, `stdlib/schema.tur::SCHEMA_*` |

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
[macro-cannot-emit-inline-c-block.md](../reported/macro-cannot-emit-inline-c-block.md);
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

## Recommended sequencing

```
   A (str->sym)   B (return-type)   C (backquote dot-sym)   D (macro inline-C)   E (multi-top-emit)
   ───────────    ───────────────   ─────────────────────   ──────────────────   ──────────────────
   ✅ shipped     ✅ shipped         ✅ shipped               open, low priority   open, blocks step 3
```

**E is the one remaining compiler prereq that blocks the spec'd
shape of `defworld` and `defsystem` macros.** `str->sym` (A) gives
us the ability to *compute* per-component identifier names; E is the
ability to *emit* the resulting defns from a single macro
invocation. Without E:

- `(defworld Name [Pos Vel])` emits the world struct only;
  per-component accessors (`get-Pos`, `set-Pos!`, ...) cannot be
  generated by the same call.
- `(defsystem physics ...)` can't emit both `(defn physics-impl ...)`
  and `(def physics (make-system ...))` in one invocation. Users
  write `(def physics (make-system ... (fn [w] body)))` by hand
  instead.

**The ECS plan still progresses without E.** Sequential `stage`,
parallel `stage-parallel`, `HasComponent` classes (via two-step
user invocation), and the full E2 scheduler all ship against the
substrate as it stands. Per-component named accessors and the
single-call `defsystem` shape land once E does.

**Stop-or-go gates (final):**

- **A, B, C are in.** Gap A unblocks identifier *synthesis*; gap E
  is the remaining piece for identifier *emission*. Gaps B and C
  are pure ergonomic wins downstream of `dense-get` and macro
  authoring.
- **D is optional polish.** Parallel-on-threadpool works via the
  implicit defn-name -> ptr<void> coercion that's already shipping.
- **E blocks the ideal `defworld` and `defsystem` surfaces** but
  does *not* block E2 functionality. The workaround is verbose
  user-side construction.

## What ECS itself does in the meantime

If the prerequisite work is going to take more than a session, the
spice's `main` branch should still progress. Options, ordered by
how much they prejudice the eventual real implementation:

1. **Pause ECS at E1'.** Tag the spice `v0.2.0-E1prime`, freeze the
   surface, work on the compiler instead. Resume E2 when A and D
   land. *Preferred.*

2. **Land E2 sequential-only.** Ship `defsystem` as a
   user-constructor pattern (`(def physics (make-system reads
   writes (fn [w] body)))`), implement `stage-seq` over a vec of
   System values, file the parallel path as gap D-blocked. Surface
   API drifts further from the plan, but E3 (raylib demo) doesn't
   strictly need parallel scheduling to ship a meaningful 60-FPS
   demo.

3. **Land E2 with hand-rolled per-system trampolines.** User writes
   the trampoline inline-C by hand once per system. Ugliest path;
   not recommended.

Recommendation: **(1)**. Each prerequisite landed makes E2/E3 cheaper
and the eventual `defsystem` shape match what the plan describes.

## Order-of-operations checklist

Revised after the 2026-06-11 probe and research:

1. ~~File gap D as a proper report.~~ **Done** -- filed as
   [`../reported/macro-cannot-emit-inline-c-block.md`](../reported/macro-cannot-emit-inline-c-block.md);
   subsequently narrowed by research to a surface-syntax pothole.
2. ~~Implement A (`str->sym`).~~ **Done** --
   `src/compiler/elab_macros.c:321-326`.
3. **Resume the ECS spice's E2 work** -- the partially unblocked
   path. The user-side surface for `defsystem` is verbose until E
   lands: `(def physics (make-system reads writes (fn [w] body)))`
   in place of `(defsystem physics ...)`. The system's `:fn` field
   holds a closure; the implicit `defn-name -> ptr<void>` coercion
   works for top-level defns but not for inline fn-values, so the
   parallel-on-threadpool path uses a separate per-system top-level
   helper or routes through fibers instead. `defworld` per-component
   accessors are gated on E. `HasComponent` classes can be generated
   via two-step user invocation (a per-component `defclass` macro)
   without E.
4. **Spice-side cleanup pass** (independent of compiler work):
   delete `dense-get-w` / `sparse-get-w` and their callers (gap B
   is in); rewrite `ecs/query.tur`'s `(list ...)` macros back to
   natural backquote (gap C is in); remove the four broken links
   to non-persisted B/C report files from the spice README and
   module docstrings.
5. **E2 parallel scheduler.** Uses `thread-pool-submit` with each
   system's run-fn coerced from its defn name. No new compiler
   feature required.
6. **(Optional polish) Implement D.** Either auto-wrap CBLOCK-headed
   lists with `do` in `ct_eval_quasiquote`, or rewrite the
   diagnostic to name the actual problem. Low priority; not on
   any plan's critical path.

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
  ([`../reported/macro-cannot-emit-inline-c-block.md`](../reported/macro-cannot-emit-inline-c-block.md)).
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
  [`../reported/macro-cannot-emit-inline-c-block.md`](../reported/macro-cannot-emit-inline-c-block.md)
  § "Research findings 2026-06-11".
- **Filed gap E**
  ([`../reported/macro-cannot-emit-multiple-top-level-forms.md`](../reported/macro-cannot-emit-multiple-top-level-forms.md))
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

## Validation that the prerequisite work paid off

A fix-batch is validated when, returning to the ECS spice:

- The README's "Known limitations" section shrinks by at least three
  entries (the entries pointing at reports A, B, C, D close).
- The E2 plan's "`defworld` ... generates `world-new`, `spawn`,
  `despawn`, and per-component `get-Pos`, `set-Pos`, `remove-Pos`,
  ..." sentence becomes literally true; today it is documented as
  blocked.
- The E2 plan's "Auto-generated `HasComponent` classes" sentence
  becomes implementable; today it is documented as blocked.
- The `dense-get-w` / `sparse-get-w` variants disappear from
  `ecs/storage.tur` and `ecs/sparse.tur`.
- `ecs/query.tur`'s module docstring loses the "written in
  (list ...) style because of macro-backquote-dot-sym-drops-siblings"
  paragraph.

If those four don't all become true after the prereq batch, the
batch missed something and we file the residual gap before
continuing the plan.

## References

- [`ecs-spice-plan.md`](ecs-spice-plan.md) -- the plan being
  unblocked.
- [`../archive/history/ecs-macro-symbol-synthesis-missing.md`](../archive/history/ecs-macro-symbol-synthesis-missing.md)
  -- gap A.
- [`../reported/generic-return-type-not-inferred-from-context.md`](../reported/generic-return-type-not-inferred-from-context.md)
  -- gap B.
- [`../reported/macro-backquote-dot-sym-drops-siblings.md`](../reported/macro-backquote-dot-sym-drops-siblings.md)
  -- gap C.
- [`../reported/macro-cannot-emit-inline-c-block.md`](../reported/macro-cannot-emit-inline-c-block.md)
  -- gap D.
- `src/compiler/elab_macros.c` -- where A and D land.
- `src/compiler/emit_module.c:emit_abi_register_call` -- where B
  lands (the per-instantiation specialization hook).
