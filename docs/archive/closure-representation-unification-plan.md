---
title: Closure Representation Unification Plan
category: Planning
description: Eliminate the captureless-vs-capturing closure representation split that crashes stdlib closure consumers (arrow, option-map, comonad, pair, mutmap) on capturing closures. The fix normalizes closure values reaching a fat-dispatched sink to fat boxes by routing them through the now-complete ^fat mechanism, while keeping genuine raw C-function-pointer callbacks (contract.tur) on the bare-pointer representation. Companion to the Typed Closure Invocation ABI work.
---

# Closure Representation Unification -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-03
> **Type:** compiler + stdlib -- closure ABI
> **Reports this resolves:**
> - [arrow-thin-call-segfaults-capturing-closures.md](../reported/arrow-thin-call-segfaults-capturing-closures.md)
> - [ptr-void-direct-call-representation-split.md](../reported/ptr-void-direct-call-representation-split.md)
> - [fat-fn-param-capturing-closure-gap.md](../reported/fat-fn-param-capturing-closure-gap.md) (core already fixed; see below)
> **Builds on:** [closure-typed-invocation-abi-plan.md](closure-typed-invocation-abi-plan.md)

---

## Problem

A closure value has two incompatible runtime representations:

- **captureless** `(fn ...)` -> a bare C function pointer `R (*)(A...)`
  (no environment), `elab_fns.c:2798` returns it as a bare `EX_VAR` of
  type `TY_FN`;
- **capturing** `(fn ...)` -> a heap fat box `{ int64_t thunk; captures... }`
  where the thunk is `R (*)(void *env, A...)`, `EX_CLOSURE` of type
  `TY_PTR_VOID`.

Code that invokes a closure value must pick one calling convention. When
the value's representation does not match the convention the code assumes,
the program **segfaults** (jumps to a box address as code, or reads slot 0
of a bare pointer). Confirmed crash sites:

| Site | Convention assumed | Crashes on |
|---|---|---|
| `stdlib/arrow.tur` `__arrow_call1/2`, `__arrow_pair_*` | thin (bare ptr) | capturing closures |
| `stdlib/option.tur` `option-map` | thin (bare ptr) | capturing closures |
| `stdlib/comonad.tur`, `stdlib/pair.tur`, `stdlib/mutmap.tur` | thin (bare ptr) | capturing closures |
| `emit_expr.c` `TY_PTR_VOID` direct call, **n==0** | thin (bare ptr) | capturing closures |
| `emit_expr.c` `TY_PTR_VOID` direct call, **n>0** | fat dispatch | captureless fns |

The root issue: `:ptr<void>` / the int64 carrier is **overloaded** as both
"fat closure handle" and "raw C function pointer," with no local
disambiguator at the call site.

## What is already fixed

The `^fat` parameter mechanism is the disambiguator, and it is now
complete (separate commits, suite green):

- A fn-typed `^fat` parameter is **directly callable** via `(g x)` and
  dispatches through the fat protocol (slot 0 = thunk, env = box), for all
  arities and register classes.
- A fn-typed `^fat` parameter **accepts both** a captureless lambda
  (auto-shimmed to a fat box via `EX_FN_TO_FAT`) and a capturing closure
  value (`:ptr<void>`, already fat).
- Per-signature typed fat-shims (`__tur_fatshim_<R>_<A...>`) make the box
  ABI-correct for non-int64 closures (`:float` etc.).

So `^fat` is the uniform, correct closure-parameter surface. The crashes
above are the stdlib/compiler sites that have **not** been moved onto it.

## What does NOT work (proven dead ends)

- **Require `^fat` to directly-call a `:ptr<void>`.** Breaks 2117 tests:
  stdlib pervasively relies on plain `:ptr<void>` direct call meaning
  "fat dispatch" (`__cons-fmap` calls `(f x)` on a plain `:ptr<void>`
  param). Plain `:ptr<void>` direct call is an intended, widely-used fat
  convention, not a misuse.
- **Blanket-box every captureless fn at the `TY_FN -> :ptr<void>`
  coercion.** Breaks genuine raw C-function-pointer callbacks --
  `contract.tur:65` passes a handler to `tur_set_contract_handler` as a C
  `void (*)(const char *)`; a fat box there is not a function pointer.

The fix therefore cannot be a single global switch; it must distinguish
"closure sink that fat-dispatches" from "raw C-callback sink that needs a
bare pointer," and normalize only the former.

## Goal

Every closure value that reaches a **fat-dispatched** sink is a fat box,
so a single fat-dispatch lowering is always correct. Raw C-function-pointer
callbacks keep the bare representation. No closure invocation guesses by
arity.

## Design

The disambiguator is the parameter contract, and `^fat` already encodes
"this sink fat-dispatches; normalize my closure argument to a fat box."
The plan is to **move every fat-dispatching closure consumer onto `^fat`**
and make the consumer dispatch through the fat protocol, leaving raw
C-callback params as plain `:ptr<void>`.

### Phase 0 -- compiler prerequisites (DONE 2026-06-03)

A spike found the gaps below; all are now resolved and the suite is green,
so the Phase 1 migration is unblocked.

- **[FIXED] Bare `^fat g` (no fn-type annotation) was not directly
  callable.** It defaulted to `TY_INT`; `(g x)` reported "not a function".
  A bare `^fat` param now defaults to `:ptr<void>` (a fat box), routing
  `(g x)` through fat dispatch. An explicit annotation still overrides.
- **[FIXED] `is_fat` was not preserved through `let`.** `(let [fv f] (fv x))`
  on a fn-typed `^fat` param dropped the marker and crashed. `is_fat` now
  propagates through let aliases, and a fn-typed `^fat` alias is declared
  as the int64 carrier (not a thin fn pointer).
- **[FIXED, pre-existing] A called `:ptr<void>`/`^fat` binding was not
  captured by an enclosing closure.** `collect_free_vars` ignored call
  heads without `closure_fn_binding`, so `(fn [x] (gv (fv x)))` -- the
  migration's exact shape -- emitted an undeclared reference. Now such
  call heads are captured. (This was the actual blocker for the arrow
  pattern; surfaced once bare `^fat` became `:ptr<void>`.)
- **[PARTIAL] `:ptr<void>` direct-call result type is hard-coded
  `TYPE_INT`.** The **annotated** form (`^fat g :(fn [...] :T)`) threads
  the correct result type and works for non-int closures (verified for
  `:float`). The **bare** form has no signature to thread, so it is
  int-result only; a non-int closure through a bare `^fat` param silently
  miscompiles -- tracked in
  [bare-fat-param-non-int-result-miscompiles.md](../reported/bare-fat-param-non-int-result-miscompiles.md).
  Not a Phase 1 blocker: the stdlib migration is int-carrier (bare `^fat`
  int is correct) and non-int closures use the annotated form.

Net: typed `^fat` parameters now support both captureless and capturing
closures, are directly callable, survive `let` aliasing, and can be called
from within an enclosing closure -- the full set of capabilities the
Phase 1 combinator rewrites need.

### Phase 1 -- stdlib thin-call consumers -> fat dispatch

Migrate the confirmed thin-call sites. Each gets a `^fat`-typed closure
parameter and dispatches via the fat protocol -- preferably by calling the
closure directly in Turmeric (a plain/`^fat` `:ptr<void>` direct call
already fat-dispatches), eliminating the bespoke int64 thin-cast inline-C:

- `stdlib/arrow.tur`: `arr`, `>>>`, `arrow-first`, `arrow-second`,
  `par-comp`, `arrow-split` take `^fat` closures; retire `__arrow_call1/2`
  and `__arrow_pair_*` thin casts in favor of direct calls (and `TUR_APPLY*_T`
  where inline-C over `Tuple2` internals is still wanted).
- `stdlib/option.tur` `option-map`, `stdlib/comonad.tur`,
  `stdlib/pair.tur`, `stdlib/mutmap.tur`: same treatment.

Validation: a new fixture per site composing a **capturing** closure;
existing captureless coverage still passes.

### Phase 2 -- compiler: fat-dispatch the nullary `:ptr<void>` path

`emit_expr.c:1753` (the n==0 `TY_PTR_VOID` direct call) emits a thin call,
inconsistent with the n>0 fat-dispatch on the same line of reasoning. Once
Phase 1 + the boxing in Phase 3 guarantee `:ptr<void>` closure values are
fat, make the nullary path fat-dispatch too, removing the
arity-dependent representation guess.

### Phase 3 -- box captureless fns at fat-dispatched `:ptr<void>` sinks

The remaining gap (report #5, the n>0-captureless cell): a captureless fn
passed to a plain `:ptr<void>` closure param arrives bare and crashes the
fat dispatch. Box it. The sink must be distinguished from a raw C-callback:

- **Option A (preferred):** treat a plain `:ptr<void>` *closure-callback*
  parameter as implicitly `^fat` when its body directly calls it, and
  require raw C-callback params to opt out (e.g. a `^cfn` / `^rawptr`
  marker, or keep them as `extern-c` boundary casts). Then call-site
  boxing (the existing `^fat` machinery) covers captureless inputs.
- **Option B:** introduce a first-class closure type distinct from
  `:ptr<void>` so closures are uniformly fat and C-callbacks are a
  separate type; `:ptr<void>` reverts to "raw pointer only." Larger, but
  removes the overload at its root and subsumes Phases 1-3.

**Phase 3 / Option B is COMPLETE (2026-06-03).** Sub-phases B-0..B-4 all
shipped, suite green; see
[closure-first-class-type-plan.md](closure-first-class-type-plan.md). Capturing
closures are a first-class boxed `TY_FN` value (B-1); `^fat` sinks fat-dispatch
for all arities incl. nullary (B-2); the `*-eq?` carrier helpers + the Eq
synthesis dispatcher fat-dispatch their comparators (B-3); and a raw
`:ptr<void>` is no longer directly callable -- `:ptr<void>` is raw-pointer-only
(B-4). The reports
[ptr-void-direct-call-representation-split.md](../reported/ptr-void-direct-call-representation-split.md)
and
[eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md](../reported/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md)
are resolved.

**Decision (2026-06-03): pursue Option B for Phase 3.** Rather than papering
over the `:ptr<void>` overload with an implicit-`^fat` heuristic (Option A),
introduce the first-class closure type so closures are uniformly fat and
C-callbacks are a distinct type. This removes the bare/fat representation
split at its root and subsumes the remaining Phase 1 holdouts -- the
`option-eq?` / `pair-eq-carrier?` / `mutmap-eq?` comparators that the
constrained-`Eq` per-call-site synthesis dispatcher feeds bare captureless
pointers (see
[eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md](../reported/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md))
-- without needing a per-call-site boxing heuristic that has to enumerate
every fat sink. Option A remains documented above as the smaller incremental
fallback if Option B proves too large to land in one piece. The Option B
implementation is scoped into sub-phases in
[closure-first-class-type-plan.md](closure-first-class-type-plan.md).

## Phasing summary

1. Migrate stdlib thin-call consumers to `^fat` + fat dispatch (no
   compiler change; uses the now-complete `^fat` surface).
2. Fat-dispatch the nullary `:ptr<void>` call path.
3. Unify the closure type (Option B -- chosen): a first-class closure type
   distinct from `:ptr<void>` makes closures uniformly fat and C-callbacks a
   separate type, boxing captureless fns at fat-dispatched sinks at the root.
   (Option A -- per-call-site implicit-`^fat` boxing -- is the documented
   incremental fallback.)

Each phase is independently mergeable and suite-gated.

## Validation

- Per phase: `bash tests/run.sh` zero `FAIL`, leak detection on.
- New fixtures composing **capturing** closures through every migrated
  site (arrow `>>>`/`arrow-first`/..., `option-map`, comonad, pair),
  covering `:int` and `:float` (register-class-distinct).
- A `:ptr<void>` callback round-trip fixture covering all four cells of
  the report-#5 matrix once Phases 2-3 land.
- The `contract.tur` C-callback path keeps working (raw pointer preserved).

## Risks

- **Public stdlib signature churn.** Adding `^fat` + fn-type annotations
  to stdlib closure params changes their declared types; audit callers.
- **Generic arrows.** Arrow is conceptually polymorphic but int64-carrier
  in practice; typed `^fat` params spell `:(fn [:int] #{} :int)`, matching
  the existing runtime. Confirm no caller depends on a non-int64 arrow.
- **C-callback misclassification.** Phase 3 must not box a fn destined for
  a raw C function-pointer parameter; the `extern-c`/raw-callback set
  (`contract.tur`, any future qsort-style binding) must be explicitly
  excluded.
- **Fixture churn.** Regenerate snapshots for every migrated site in the
  same PR.

## Acceptance checklist

- [ ] arrow / option-map / comonad / pair / mutmap dispatch capturing
      closures without crashing; thin int64 casts retired.
- [ ] nullary `:ptr<void>` direct call fat-dispatches.
- [ ] captureless fn passed to a fat-dispatched `:ptr<void>` sink is
      boxed; raw C-callbacks remain bare.
- [ ] capturing-closure fixtures pass for `:int` and `:float` at every
      migrated site.
- [ ] `bash tests/run.sh` zero `FAIL`.
