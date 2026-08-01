---
title: "Delimited Control Operators: shift/reset vs shift0/escape"
category: Advanced Control Flow
description: How shift/reset, shift0/reset0, call/cc, and escape differ -- prompt re-installation, where the captured value returns, and which to reach for
---

# Delimited Control Operators: shift/reset vs shift0/escape

Turmeric ships a family of delimited control operators: `shift`/`reset`,
`shift0`/`reset0`, `call/cc`, and `escape` (plus the multi-shot `call/cc*` and
the cloneable/serializable variants documented elsewhere). They look similar
and share one runtime (`tur_cont`, one-shot delimited continuations backed by
fiber-context save/restore), but they differ in two precise ways that decide
which one you want. This guide explains those differences.

## Overview

Every delimited control operator has **two halves**:

- A **delimiter** (`reset` / `reset0`) marks a boundary on the stack: "the
  captured continuation reaches this far and no further."
- A **capture** operator (`shift` / `shift0`) grabs the slice of stack from the
  capture site *up to* the nearest enclosing delimiter, reifies it as a callable
  function `k`, and hands it to your body.

The whole family is one runtime mechanism. What distinguishes the operators is
the answer to two questions:

1. **On capture, is the enclosing delimiter consumed (popped) or left in place?**
2. **On resuming `k`, is the delimiter re-installed around the resumed work?**

`call/cc` and `escape` are sugar layered on top of `shift` and `shift0`
respectively, specialized for the common "escape / early return" use case.

## The two axes

|            | Pops delimiter on capture? | Re-installs delimiter on resume? | Value of `(k v)` surfaces at ... |
|------------|----------------------------|----------------------------------|-----------------------------------|
| `shift` / `reset`   | No  | Yes | the capture (`shift`) site |
| `shift0` / `reset0` | Yes | No  | the boundary (`reset0`) |
| `call/cc` (= `shift` sugar)  | No  | Yes | the `call/cc` site |
| `escape` (= `shift0` sugar)  | Yes | No  | the boundary |

The remaining two corners of the lattice -- Felleisen's `control`/`prompt` and
`control0`/`prompt0` -- differ in how *composed* continuations stack their
delimiters. Turmeric does not expose those; the four above cover every
motivating use case in this codebase.

## `shift` / `reset` -- static, re-delimiting

This is the well-behaved default (Danvy--Filinski semantics):

- When `shift` captures, the surrounding `reset` **stays in place**.
- When you invoke the captured `k`, its body is **re-wrapped in a fresh
  `reset`** -- the delimiter is reinstated around the resumed computation.

Because every invocation of `k` is freshly delimited, captured continuations
compose like ordinary functions, and a `shift` always sees the same
statically-enclosing prompt. This is exactly why `shift`/`reset` is the right
lowering target for resumable effect handlers (each `resume` runs inside a
fresh handler scope).

```turmeric
(reset
  (+ 1 (shift k (k (k 10)))))
;; k = (fn [x] (+ 1 x)), re-delimited on each call
;; => (+ 1 (+ 1 10)) = 12, all surfacing inside the reset
```
```sweet-exp
reset
  (+ 1
     (shift k
       k(k(10))))
;; k = (fn [x] {1 + x}), re-delimited on each call
;; => {1 + {1 + 10}} = 12, all surfacing inside the reset
```

## `shift0` / `reset0` -- pops the prompt

`shift0`/`reset0` are strictly more expressive, at the cost of being harder to
reason about locally:

- When `shift0` captures, it **also removes the enclosing `reset0`** (it "pops
  the prompt").
- When you invoke `k`, the delimiter is **not** reinstated.

The consequence: a `shift0` can "see past" its immediate prompt to the *next*
dynamically-enclosing one. This is what lets you reach outward through multiple
prompt layers -- something `shift` cannot do, because it always re-establishes
its own boundary first.

The *local* result-typing rule is identical to `shift`: the `(shift0 f x)`
expression has the type of `f`'s codomain. Only the runtime delimiter behavior
differs.

```turmeric
;; The expression has f's codomain type (here :bool), exactly like shift.
(defn is-zero [n : int] : bool
  (reset
    (shift0 (fn [v : int] (= v 0)) n)))
```
```sweet-exp
;; The expression has f's codomain type (here :bool), exactly like shift.
defn is-zero [n :int] :bool
  reset
    (shift0
      (fn [v :int]
        =(v 0))
      n)
```

## `call/cc` -- delimited capture, value at the capture site

`call/cc` is `shift`-flavored sugar:

```
(call/cc f)  ==>  (reset (shift k (f k)))
```

It allocates a one-shot continuation for "the rest of the computation from the
`call/cc` site up to the boundary" and passes it to `f`:

1. If `f` returns normally, that value is the value of the `call/cc` expression
   (the captured `k` is dropped).
2. If `f` (or anything it calls) invokes `(k v)`, control returns **to the
   `call/cc` site** with `v` as its value; whatever `f` was doing is abandoned.

Because it is built on `shift`, the resumed continuation is re-delimited, and
the value surfaces at the capture site -- not the boundary.

> **Delimited, not undelimited.** Unlike Scheme's top-level
> `call-with-current-continuation`, Turmeric's `call/cc` captures only up to the
> nearest enclosing `reset`. Use `call/cc*` for the multi-shot cloneable
> variant.

## `escape` -- abort to the boundary

`escape` is the abort-flavored sibling, lowered via `shift0`:

```
(escape f)  ==>  (reset (shift0 k (f k)))
```

The difference from `call/cc` is *where the value surfaces*: invoking `(k v)`
**unwinds to the boundary** and produces `v` from the *boundary* (the `reset`),
not the `escape` site. Nothing is re-delimited on the way out. This is the
classic non-local exit, matching C `longjmp`, Common Lisp `return-from`, Java
`throw`, and OCaml `discontinue`.

Reach for `escape` when you want to bail out of deep recursion with a value and
*not* resume -- early return without manual `Option`/`Result` plumbing:

```turmeric
;; Conceptual: exit produces the value at the boundary, abandoning the rest.
(defn first-negative [xs : list<int>] : int
  (escape (fn [exit]
    (for [x xs]
      (when (< x 0)
        (exit x)))   ; unwinds straight to the boundary with x
    0)))             ; no negative found
```
```sweet-exp
;; Conceptual: exit produces the value at the boundary, abandoning the rest.
defn first-negative [xs :list<int>] :int
  escape
    fn [exit]
      for [x xs]
        when <(x 0)
          exit(x)   ; unwinds straight to the boundary with x
      0            ; no negative found
```

Where `shift`/`call/cc` is for splicing a captured slice back into a
computation (possibly composed, possibly more than once with `call/cc*`),
`escape` is purely abortive: you jump *out* and you do not re-enter.

> **Implementation status.** `shift`/`reset` and `shift0`/`reset0` are
> runtime-backed today. `call/cc`/`escape` are delimited sugar over them; the
> full early-exit lowering (`escape` via `shift0`, an enforced enclosing
> boundary) is tracked in
> [`call-cc-completion-plan.md`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/call-cc-completion-plan.md). Until
> that lands, treat the semantics above as the contract and check the plan for
> the current stub caveats.

## The boundary requirement

All four operators are **delimited**: a capture extends only to the nearest
enclosing `reset`/`reset0`, never to the top of the program. A `call/cc` or
`escape` with no enclosing boundary is a compile-time error (`TUR-E0705` once
the completion plan lands) rather than an implicit whole-program capture. A
captured `k` cannot escape past its boundary; calling `k` after the boundary
has already returned is a defined runtime error, not undefined behavior.

## Decision guide

| You want to ... | Reach for |
|---|---|
| Compose a captured continuation, resume inside a fresh scope, lower an effect handler | `shift` / `reset` |
| Reach *past* the immediate prompt into an outer one (layered handlers, multi-prompt control) | `shift0` / `reset0` |
| Capture once, resume with the value surfacing at the capture site | `call/cc` |
| Non-local early exit -- bail out with a value, surface it at the boundary, do not resume | `escape` |
| Resume a captured continuation *more than once* (backtracking, generators) | `call/cc*` (cloneable) |
| Persist or migrate a suspended computation across processes | `serial-shift` / `serial-reset` |

## How it lowers (compiler pipeline)

These operators are more than surface theory here -- the compiler leans on
their semantics:

1. Algebraic effects (`perform`/`handle`) **lower to `shift`/`reset`** in
   `PASS_EFFECT_LOWER`. The re-delimiting property of `shift` is exactly what
   makes each `resume` run inside a fresh handler scope.
2. `shift`/`reset` are then **CPS-transformed to trampolined IR** in
   `PASS_CPS`, which compiles down to the `tur_cont` runtime.

The symbol bindings live in
[`src/compiler/elab_internal.h`](https://github.com/rjungemann/turmeric/blob/main/src/compiler/elab_internal.h) (`sym_shift`,
`sym_reset`, `sym_shift0`, `sym_call_cc`, `sym_escape`, ...), elaboration in the
`elab_*` entry points there, and codegen in `src/compiler/emit_internal.h`
(`emit_effects_shift`, `emit_effects_reset`, `emit_effects_shift0`, ...). The
runtime structures are in
[`src/runtime/runtime.h`](https://github.com/rjungemann/turmeric/blob/main/src/runtime/runtime.h) (`tur_cont`,
`tur_cloneable_cont`, `tur_frame`).

### Abortive shift and the direct/CPS oracle

A base `shift` is **abortive**: it discards its delimited context. The value
delivered to the enclosing `reset` is `f(body-value)`, where `f` is the shift's
receiver -- not the body value itself. Two invariants fall out of this, and both
have been the root cause of silent miscompiles:

1. **The receiver must be applied.** Lowering `(shift f body)` as merely the CPS
   of the body value -- dropping `f` -- makes `(shift (fn [v] v) x)` and
   `(shift (fn [v] (* 2 v)) x)` produce identical IR. The receiver is part of
   the semantics, so the IR must lower it as "apply `f` to the body value."
2. **An out-of-subset `reset` must not degrade to plain body-eval.** If a
   backend cannot lower a given reset shape and falls back to
   `emit_value(body)`, the enclosed `shift` collapses to "return its operand"
   and the reset yields the wrong value (the abortive continuation is never
   discarded). The direct backend instead lowers a branch-bearing base reset
   through a `setjmp`/`longjmp` escape (`emit_cps_reset_escape`) to the
   innermost reset landing.

Because the direct and CPS backends lower these shapes independently, they are
cross-checked by `cps-oracle-*` fixtures (e.g. `cps-oracle-reset-join-escape`,
`cps-oracle-reset-both-branch-shift`) that assert `direct == cps` on the same
program. When you extend the admitted reset/shift subset, add or re-admit the
matching oracle -- a lowering retired or narrowed on a usage metric alone can
silently drop the sole correct emitter for a shape, so guard the shapes you stop
handling with a hard codegen error (as TUR-E0710 does for cloneable contexts)
rather than a fallback.

## See Also

- [Effects System Guide](effects-system-guide.md) -- Algebraic effects, which lower to `shift`/`reset`
- [Custom Effects Tutorial](custom-effects-tutorial.md) -- Building handlers on the continuation machinery
- [Async/Await Guide](async-await-guide.md) -- One-shot continuations for async I/O
- [Logic Programming Guide](logic-programming-guide.md) -- Cloneable continuations (`call/cc*`) for backtracking
- [Checkpointing Guide](checkpointing-guide.md) -- Cloneable continuations for persistent workflows
- [Serializable Continuations Guide](serializable-continuations-guide.md) -- Capturing and resuming across process boundaries
- [Compiler Internals](compiler-internals.md) -- Effect lowering and the CPS pass
- [`call-cc-completion-plan.md`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/call-cc-completion-plan.md) -- The plan finishing `call/cc`/`escape`
- [Bibliography](bibliography.md#delimited-continuations) -- Papers behind these operators

## References

The semantics above come from the delimited-continuation literature; see the
[Bibliography](bibliography.md#delimited-continuations) for full citations:

- **Danvy & Filinski**, *Abstracting Control* (LFP 1990) -- the static
  `shift`/`reset` semantics.
- **Felleisen**, *The Theory and Practice of First-Class Prompts* (POPL 1988)
  -- prompts and the `control`/`prompt` family.
- **Materzok & Biernacki**, *Subtyping Delimited Continuations* (ICFP 2011) --
  the `shift0`/`reset0` hierarchy and its relation to `shift`/`reset`.
- **Dybvig, Peyton Jones & Sabry**, *A Monadic Framework for Delimited
  Continuations* (JFP 2007) -- a unifying account used by several
  implementations.
