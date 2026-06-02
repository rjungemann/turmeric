---
title: Control Flow -- Completeness Audit
category: Language Features
description: Audit of Turmeric's control-flow machinery (continuations, effects, generators, async, fibers, backtracking, exceptions, recursion), separating pre-v1.0.0 gaps from work deferred to a later release
---

# Control Flow -- Completeness Audit

> Snapshot: `0.14.6`. Companion to
> [typing-gap-audit.md](typing-gap-audit.md). This document covers the
> non-local control-flow stack: delimited continuations, `call/cc`,
> algebraic-effect handlers, generators, async/await, fibers/scheduler,
> backtracking, exceptions/panic, and recursion/tail calls.

Turmeric's control-flow features are built on one shared substrate: the
delimited-continuation machinery (Phase 18) plus the algebraic-effect handler
chain (Phase 19). Generators, async, fibers, and backtracking are all
expressed on top of it. The central architectural fact below shapes every
gap in this document.

## Architectural note: there is no full CPS pass

`src/passes/cps.c:14`:

```
/* For v1, we use a simplified approach:
 * - Functions containing shift/reset are marked as "may_capture"
 * - No full CPS transformation is performed
 * - The emitter handles shift/reset using runtime continuation support
 *
 * Full CPS transformation (where every function call becomes a tail call
 * into a continuation) will be implemented in a future phase. */
```

So delimited continuations are realized by (a) a `may_capture` analysis that
marks functions which contain `shift`/`reset`, and (b) runtime continuation
support emitted by the backend (fiber-context save/restore on x64/arm64; see
`src/async/fiber_ctx_*.S`). This works and is tested, but several gaps below
are downstream consequences of *not* having a true whole-program CPS
transform.

## What is solid (shipped and exercised end-to-end)

| Mechanism | State | Evidence |
|---|---|---|
| Delimited continuations | Working | `reset`/`shift`/`shift0`; `continuation-basic`, `continuation-advanced` |
| Cloneable continuations | Working | `cloneable-reset`/`cloneable-shift`, Clone check (TUR-E0014); backtracking fixtures |
| Serializable continuations | Working | `serial-reset`/`serial-shift`, Serializable check (TUR-E0018); `serializable-continuations-guide.md` |
| Effect handlers | Working | `handle`/`perform`/`resume`/`discontinue`; ~60 `effect-*` fixtures |
| Linear / multi-shot continuations | Working | `^linear` (exact-once, TUR-E0100/0101), `^multishot` (copy); `effect-cont-*` fixtures |
| Generators | Working (with caveats) | `yield`, `yield*`; `gen-*` and `gen-yield-star` fixtures |
| Async / await | Working | `async-await-basic`, `async-channel`, `async-select`, `async-cancel`, epoll/kqueue I/O |
| Fibers + scheduler | Working | `fiber-*`, `scheduler-*` (incl. `scheduler-multithread`, `scheduler-io-park`) |
| Backtracking | Working | `backtrack-*` (n-queens, sudoku, interleave) on cloneable continuations |
| Exceptions / panic | Working | `panic-*`, `try-catch-compiled`, `try-with-*`, typed catch + downcast + unwind |
| letrec / named-let / mutual recursion | Working | `letrec-*`, `named-let-*`, `mutual-recursion` (PR #118) |

## Pre-v1.0.0 gaps -- incompleteness in shipped control-flow surface

1. **`call/cc` and `escape` are degenerate sugar.**
   `elab_effects.c:1183-1230`: `(call/cc f)` desugars to
   `(let [__cc_f f] (__cc_f 0))` -- the "continuation" handed to `f` is the
   integer `0`, not a callable. The `continuation-callcc` / `continuation-escape`
   fixtures run, but only because they never actually invoke the captured
   continuation. This is the single largest correctness gap: a user who calls
   the captured `k` gets nonsense. Either implement real capture (needs the
   CPS pass) or gate `call/cc`/`escape` off until it exists.

2. **`compose-handlers` is a stub.** `elab_effects.c:982` -- elaborates to a
   nil-typed placeholder with "runtime semantics TBD." Handler composition is
   a natural ask once you have effects; today it silently produces nil.

3. **`shift`/`shift0` result type is a placeholder.** `elab_effects.c:61,100,202`
   use `body->type` as the result type pending full inference. Mis-typing of
   delimited-continuation expressions is possible.

4. **Generator limitations are real, not cosmetic** (`generators-guide.md:115-116`):
   - `yield` inside `match` arms -- **not supported** (v1 limitation).
   - Recursive generators -- **not supported** (v1 limitation).

   Both fall out of the `may_capture` / no-full-CPS design: a `yield` buried
   in an arbitrary sub-expression position cannot be lowered. These are likely
   to surprise users and should be either fixed or prominently documented at
   1.0.

5. **No general tail-call optimization.** Named-let desugars to `letrec`
   (`elab_forms.c:158,1269`), i.e. ordinary recursive function calls -- there
   is no self-tail-call -> loop lowering and no trampoline. Idiomatic
   `(let loop [...] ... (loop ...))` countdown loops consume stack proportional
   to iteration count and can overflow. For a Lisp this is a notable
   completeness gap; at minimum self-tail-recursion should lower to a loop
   before 1.0.

6. **Send-bound checking across await points is incomplete.**
   `borrow_check.c:389-397` (AW-012 / AW-011B-1): the boundary Send check
   exists, but full enforcement of "values live across an await must be Send"
   requires tracking which values are live at each await point and is not done.
   This is a soundness hole in the async story.

7. **Cloneable-continuation deep clone is a bitwise copy.** `cps.c:846` uses a
   v1 bitwise copy instead of field-by-field deep clone, and `cps.c:753`
   leaves the Drop typeclass path unimplemented ("Drop not yet implemented;
   leave NULL"). Cloning a continuation that captures owning/heap state may
   alias or mis-handle ownership.

8. **Cloneable-shift capture check is conservative** (`elab_effects.c:114`):
   it covers every binding in scope rather than being liveness-precise, so it
   can reject valid programs. Precise liveness needs the CPS pass.

## Post-v1.0.0 gaps -- deferred future work

- **Full CPS transformation** (`cps.c:14`) -- the root enabler. Would unblock
  real `call/cc`, `yield` in arbitrary positions, recursive generators, and
  precise liveness for cloneable continuations. Large, separable piece of
  work; correctly deferred.

  > **CF4 update (2026-06-02): the "whole-program CPS pass" substrate is
  > addressed by [`cps-transform-plan.md`](../../upcoming/cps-transform-plan.md),
  > phases CPS0--CPS6.** That plan delivers the selective may-capture coloring
  > (CPS1), the ANF/CPS IR + selective lowering with direct<->CPS boundary
  > bridging (CPS2--CPS3), a heap-reified unbounded-capture continuation runtime
  > + trampoline (CPS4, validated at 500k frames), a multi-prompt
  > delimited-control machine with an implicit root prompt (CPS5), and retires
  > the 16-frame ceiling on the CPS path (CPS6). **CPS8 (2026-06-02) wires the
  > base `reset`/`shift`/`shift0` codegen lowerings onto that substrate**
  > (`emit_cps.c`): a delimited reset now compiles to a run on the multi-prompt
  > `DK` machine emitted into the program, validated end-to-end by the executing
  > `continuation-substrate` fixture. `call/cc*` (cloneable/multi-shot),
  > `serial-*`, and undelimited `call/cc` (root-prompt capture) remain on their
  > existing lowerings -- the next increment tracked in the plan; the substrate
  > pieces they need (`dk_invoke`, `dk_run_root`) already exist.

- **Multi-threaded scheduler integration.** `scheduler.c:542,550` (SCH-003):
  `tur_scheduler_mt_from_threadpool` / `..._set_for_threadpool` print
  "not yet integrated" and are non-functional. Single-threaded and the
  existing multithread fixtures work; the ThreadPool<->scheduler bridge does
  not.

- **Effect-handler composition semantics** (gated on the `compose-handlers`
  stub above) -- a designed runtime composition story, not just a non-nil
  return.

- **Trampolining / guaranteed TCO for mutual and general tail calls** -- even
  after self-tail-call loop lowering, mutually recursive tail calls would
  still need a trampoline or `musttail`-style mechanism.

## Bottom line

The control-flow substrate (delimited continuations + effect handlers + fiber
runtime) is real and powers generators, async, fibers, and backtracking with
broad fixture coverage. The pre-1.0 risks are concentrated and specific:
**`call/cc`/`escape` are non-functional sugar**, **`compose-handlers` returns
nil**, **generators reject `yield` in `match`/recursion**, **there is no TCO**,
and **async Send-checking has a soundness hole**. Most trace back to the
absence of a full CPS pass, which is the correct large post-1.0 investment.
For 1.0, the actionable choice is per-feature: implement, lower (TCO), or
gate-off-and-document.

## See also

- [control-flow-completeness-plan.md](control-flow-completeness-plan.md) -- phased pre-1.0 plan that closes the gaps above
- [typing-gap-audit.md](typing-gap-audit.md)
- [effects-system-guide.md](../guides/effects-system-guide.md)
- [generators-guide.md](../guides/generators-guide.md)
- [async-await-guide.md](../guides/async-await-guide.md)
- [backtracking-guide.md](../guides/backtracking-guide.md)
- [serializable-continuations-guide.md](../guides/serializable-continuations-guide.md)
