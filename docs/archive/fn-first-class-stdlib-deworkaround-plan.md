---
title: De-workaround the stdlib HKT combinators onto first-class `:fn`
category: Planning
description: Now that a `:fn` value is a first-class callable (the fn-first-class-application work), the parser / backtrack / logic combinator stacks no longer need their `^fat`-sink shims and hand-written `apply-fat` inline-C. This plan retypes their continuation parameters to `:fn`, applies them directly with `(f x)`, and drops the `mbind-fat` / `bind-goal-raw` / `bind-parser-fat` bridges -- validating the first-class `:fn` feature end-to-end against the existing hkt-stdlib-{parser,backtrack,logic}-instances fixtures.
---

# De-workaround the stdlib HKT combinators onto first-class `:fn` -- Plan

> **Status:** COMPLETE
> **Type:** stdlib cleanup -- closure ABI consumer
> **Builds on (COMPLETE):**
> - [fn-type-first-class-application-plan.md](fn-type-first-class-application-plan.md)
>   and its landed implementation (commit "Make first-class `:fn` closure
>   values callable, constructible, and coercible"). That work delivered F1-F4
>   (a hand-written `:fn` value is callable with `(g x)`, constructible from a
>   lambda/closure, and coercible into a `^fat` sink) plus a guard for the
>   float-class carrier gap. **This plan is its deferred phase F6.**
> **Resolves:** the deferred F6 of the parent plan -- "Drop the now-unnecessary
> `^fat`-sink wrappers (`bind-parser-fat`, `mbind-fat`, `bind-goal-raw`'s `^fat`
> param, etc.) where a direct `:fn` application is now legal."

---

## Problem

Three stdlib HKT combinator stacks -- `stdlib/parsec.tur` (`Parser`),
`stdlib/backtrack.tur` (`Backtrack`), and `stdlib/logic.tur` (`Goal`) -- predate
first-class `:fn`. Their continuation-taking combinators carry the callback as
the **`:int` carrier** and invoke it through a hand-written fat-closure
dispatcher (`apply-fat` / `bt-apply-fat`, an inline-C block that reads slot 0 of
the box and calls `thunk(env, x)`). Because a typeclass-method continuation
arrives as a poly closure (`tur_poly_fn_t`, `is_poly_fn`) -- a 16-byte
`{env, fn}` struct, not an int -- each Monad/Functor/Applicative instance cannot
hand that continuation straight to an `:int`-typed worker. The bridge is a thin
**`^fat`-sink shim** whose parameter is declared `^fat`, so the compiler boxes
the poly closure into a one-word fat handle (`EX_POLY_TO_FAT`) that the worker's
`apply-fat` can then call.

This bridge was a *good* idiom while `:fn` was second-class (it kept the
instance bodies free of hand-written `__tur_poly_to_fat` boxes). But it is now
redundant: a `:fn` value is directly callable, so a worker can simply declare
its callback `:fn` and apply it with `(f x)`, and an instance can pass its poly
continuation straight through (the `:fn` -> `:fn` pass-through, probe P3 of the
parent plan).

### Workaround inventory

`^fat`-sink shims (each boxes a poly `:fn` into a fat handle for an `:int` worker):

| File | Shim | Worker it bridges to |
|---|---|---|
| `stdlib/backtrack.tur:409` | `mbind-fat [ma ^fat f]` | `mbind` |
| `stdlib/backtrack.tur` (`fmap-backtrack-raw [xs ^fat f]`) | inline worker | `mbind` + `bt-apply-fat` |
| `stdlib/logic.tur:581` | `bind-goal-raw [g ^fat k]` | `apply-goal` + `apply-fat` |
| `stdlib/logic.tur` (`fmap-goal-raw [g ^fat f]`) | inline worker | `apply-goal` + `apply-fat` |
| `stdlib/parsec.tur:878` | `bind-parser-fat [p ^fat f]` | `bind-parser` |

Hand-written fat-dispatch inline-C the workers call (candidates to retire once
the callback is `:fn`-typed and applied directly):

| File | Helper |
|---|---|
| `stdlib/parsec.tur:325` | `apply-fat [f arg]` |
| `stdlib/backtrack.tur` | `bt-apply-fat [f x]` |

The consumers are exercised by the in-repo fixtures
`tests/fixtures/hkt-stdlib-{parser,backtrack,logic}-instances` (and
`hkt-stdlib-option-result-instances`), which must keep passing with identical
behaviour after the cleanup.

## Goal

> A continuation-taking combinator declares its callback `:fn` and applies it
> with a plain `(f x)`; the typeclass instance passes its poly continuation
> directly. The `^fat`-sink shims (`mbind-fat`, `bind-goal-raw`,
> `bind-parser-fat`, and the `*-raw` map/ap workers' `^fat` params) and the
> hand-written `apply-fat` / `bt-apply-fat` inline-C are removed wherever a
> direct `:fn` application is now legal -- with byte-for-byte identical runtime
> behaviour.

## Key risk to resolve first -- deferred `:fn` capture

The parent plan validated **immediate** application and coercion of a `:fn`
value. These combinators do something more: they **capture** the continuation
into a *returned closure* and invoke it later, e.g.

```turmeric
(defn bind-goal-raw [g ^fat k] : ptr<void>
  (let [lg g lk (:: k :int)]
    (fn [state]                          ; <- the captured-and-deferred call
      (mbind (apply-goal lg state)
        (fn [s] (apply-goal (apply-fat lk s) s))))))
```

A direct rewrite would capture the `:fn` value `k` into the inner `(fn [state]
...)` closure and apply it there with `(k s)`. **Before any rewrite**, confirm
that a `:fn` (`tur_poly_fn_t`) value survives capture into a boxed closure and
is callable from the closure body -- i.e. the closure-capture machinery stores
the 16-byte carrier intact (not truncated to its first word) and the deferred
`(k s)` still emits `k.fn(k.env, s)`. If it does not, that capability is a
prerequisite (a small extension to the closure-capture path), and it -- not the
stdlib edits -- is the real content of this plan.

This is exactly why F6 was split out: the de-workaround is only safe once
captured-and-deferred `:fn` application is proven, and that is a distinct
capability from the immediate-application path the parent plan shipped.

### Resolution -- the capability was missing; it is now fixed

F6-0 found that deferred `:fn` capture **did not work**: a captured `:fn`
carrier produced a miscompile (`error: 'lk' undeclared`), and the capability
turned out to be the real content of this plan, exactly as anticipated. Three
coupled gaps in the closure-capture path were fixed:

1. **Capture-set collection (`elab_core.c`, `collect_free_vars`).** A `:fn`
   carrier referenced only through a compiler-inserted conversion shim
   (`EX_POLY_WRAP` for a `:fn`->`:fn` pass-through argument, `EX_POLY_TO_FAT` /
   `EX_FN_TO_FAT` / `EX_REINTERPRET` / `EX_CAST`) was invisible to the free-var
   walk, so the enclosing closure was emitted captureless and the inner body
   referenced an undeclared local. The walk now descends through these shim
   nodes (in both the local-defs pre-pass and the main traversal). A `:fn`
   carrier *applied directly* `(k x)` inside a closure is likewise now counted
   as a capture (the `is_poly_call` callee), mirroring the existing
   `closure_fn_binding` / `:ptr<void>` / `^fat` cases.
2. **Env-qualified callee naming (`emit_core.c`, `emit_call_name`).** The
   poly-fn dispatch path emits `<name>.fn(<name>.env, ...)`, but
   `emit_call_name` returned the bare local name, so a *captured* carrier
   emitted `lk.fn(lk.env, ...)` instead of `__env->lk.fn(...)`. The captured
   binding -> env access rewrite (previously only in `name_for_binding`) is now
   factored into a shared `capture_env_access` helper and applied here too.

With these in place the 16-byte `tur_poly_fn_t` carrier survives capture intact
and the deferred `(k s)` dispatches through the env. Guarded permanently by
`tests/fixtures/fn-first-class-application-deferred/`.

## Design

For each combinator stack, in isolation:

1. **Retype the worker's callback to `:fn`.** Change `[... ^fat f]` /
   `[... f]`-as-`:int` to `[... f : fn]`; replace `(apply-fat f x)` /
   `(bt-apply-fat f x)` with the direct application `(f x)`.
2. **Pass the continuation through unchanged.** The instance body now hands its
   poly continuation straight to the worker (`:fn` -> `:fn` pass-through); drop
   the `(:: f :int)` re-ascription and the `^fat`-sink shim entirely.
3. **Retire the dispatcher** (`apply-fat` / `bt-apply-fat`) once it has no
   remaining callers. If a non-instance caller still needs the int-carrier form
   (e.g. `apply-parser`, which dispatches a *parser* fat box, not a
   continuation), leave that dispatcher in place -- only the continuation path
   is in scope.

Keep the carrier representation identical (`tur_poly_fn_t`); this is a
source-level simplification, not an ABI change. The result value, effect rows,
and substructural discipline of each combinator must be preserved.

## Phasing (each phase ends suite-green)

0. **F6-0 -- prove deferred `:fn` capture. (DONE)** Found broken; the
   closure-capture path was fixed (see *Resolution* above). Guarded by
   `tests/fixtures/fn-first-class-application-deferred/`. *Exit met:* deferred
   `(k s)` round-trips.
1. **F6-1 -- Backtrack. (DONE)** Retyped `fmap-backtrack-raw`'s continuation to
   `:fn` (direct `(f x)`); the Monad `bind` now wraps the poly continuation in a
   plain result-mapping lambda `(mbind ma (fn [x] (k x)))`, dropping `mbind-fat`.
   `bt-apply-fat` stays -- `ap-backtrack-raw` still uses it to dispatch *function
   elements* pulled from a result list (genuine int-carried fat boxes, not poly
   `:fn` continuations). *Exit met:* `hkt-stdlib-backtrack-instances` passes with
   identical output (`4 2 3 3 4 2 2 4 11 12 1 2 3`).
2. **F6-2 -- Parser. (DONE)** Retyped the whole continuation chain
   (`bind-parser` / `bind-parser-impl` / `bind-parser-inner` / `fmap-parser-raw`)
   to `:fn`, applied directly; dropped `bind-parser-fat`. `apply-parser` (parser
   dispatch) and `apply-fat` stay -- `ap-parser-raw` uses `apply-fat` on a parsed
   *function element*. *Exit met:* `hkt-stdlib-parser-instances` passes
   (`131 66 66 66 0`).
3. **F6-3 -- Goal. (DONE)** Retyped `bind-goal-raw` / `fmap-goal-raw`'s
   continuation to `:fn`, applied inside the deferred goal closure; dropped the
   `^fat` param and the `(:: k :int)` ascription. `apply-fat` stays -- `fresh-impl`
   applies a user-supplied lambda continuation (`fresh`'s `f`), which is outside
   the HKT-instance de-workaround scope. *Exit met:* `hkt-stdlib-logic-instances`
   passes (`2 1 1 1 2`).
4. **F6-4 -- sweep. (DONE)** No `^fat`-sink continuation shims remain; the
   surviving `apply-fat` / `bt-apply-fat` callers are all *function-element*
   dispatch (`ap`) or the out-of-scope `fresh` user-lambda continuation. The
   remaining `^fat` spellings in `parsec.tur` are *return-type* boxes on
   `pfail-raw` / `item-raw` (bare-fn -> fat, not continuation sinks).
   `stdlib/docstrings.tur` regenerated. *Exit met:* `bash tests/run.sh` green
   (1465 passed, 0 failed); `run-turi.sh` green (124 passed).

## Risks

- **Deferred-capture miscompile (primary).** Covered by F6-0; do not skip it.
- **Carrier width on capture.** A `tur_poly_fn_t` is 16 bytes; a capture path
  that stores callbacks as a single `int64_t` would truncate it. Verify the
  closure env field for a captured `:fn` is the full carrier (mirror the
  `is_poly_fn` field handling already in `emit_effects.c` / `emit_fns.c`).
- **Behaviour drift.** These stacks have subtle result-ordering semantics
  (cartesian product in `ap`, disjunction union in `alt-or`). The fixtures pin
  observable output; treat any diff as a regression, not a snapshot update.
- **Float carrier is *not* a concern here.** All three carriers are int-pointer
  (Cell-list / goal-closure / Parser fat box), so the float-class gap
  ([fn-first-class-float-carrier-gap.md](../reported/fn-first-class-float-carrier-gap.md))
  does not apply.
- **Partial retirement.** `apply-fat` / `apply-parser` also dispatch
  *non-continuation* fat boxes (e.g. a parser value). Only the continuation
  callers are in scope; do not delete a dispatcher that still has legitimate
  callers.

## Validation

- `tests/fixtures/hkt-stdlib-{parser,backtrack,logic}-instances` and
  `hkt-stdlib-option-result-instances` keep passing with identical stdout.
- A deferred-`:fn`-capture fixture (F6-0) guards the prerequisite capability.
- `bash tests/run.sh` green at every phase boundary; zero `FAIL` lines.
- A grep audit shows no remaining continuation-only `^fat`-sink shim.

## Out of scope

- Retiring `tur_poly_fn_t` in favour of the boxed `TY_FN` carrier (the parent
  plan's "alternative considered").
- The float-class typed-`:fn` round-trip (parent plan phase F5; tracked in
  [fn-first-class-float-carrier-gap.md](../reported/fn-first-class-float-carrier-gap.md)).
- Any change to the observable semantics of the parser / backtrack / logic
  combinators -- this is a representation-preserving simplification only.

## Cross-references

- [fn-type-first-class-application-plan.md](fn-type-first-class-application-plan.md)
  -- the parent plan; this is its phase F6.
- [fn-first-class-float-carrier-gap.md](../reported/fn-first-class-float-carrier-gap.md)
  -- the deferred F5 carrier gap (orthogonal to this cleanup).
- `stdlib/parsec.tur`, `stdlib/backtrack.tur`, `stdlib/logic.tur` -- the
  consumers; `tests/fixtures/hkt-stdlib-*-instances` -- the validating fixtures.
