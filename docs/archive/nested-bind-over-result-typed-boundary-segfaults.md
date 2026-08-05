---
status: RESOLVED 2026-08-01 -- fat-closure continuation spill shim (archived)
severity: medium
area: compiler (HKT method result typing / continuation-wrapper ABI pairing)
---

# Chaining two `bind`s over `Result` segfaults; one `bind` is fine

## Resolution (2026-08-01)

Not the flag-clobbering the "likely direction" below guessed at --
`ctx->poly_wrap_callee_carrier` was already saved/restored around the
call-arg emission (`emit_expr.c`), and the OUTER continuation was in fact
paired correctly. The gap was one shape lower: **the increment-2 spill shim
only covers NAMED wrappers.**

Nesting is the trigger because it is what makes the inner continuation
*capture*. `(fn [b] (ok (+ a b)))` closes over the outer bound `a`, so it
lowers to a fat closure rather than a named `__poly_N` wrapper -- and its
`__fn` (`__fn_1332`) returns the by-value aggregate
`tur_adt_Result__int__int`. The EX_POLY_WRAP fat-closure branch had no
spill arm at all: it emitted the raw env slot as
`(int64_t(*)(void*,int64_t))(intptr_t)((int64_t*)env)[0]`, so
`__inst_Monad_bind_Result_tyvar` invoked a struct-returning function through
an int64-returning pointer. The struct return (RAX:RDX, or an sret hidden
pointer that consumes the env argument) was then read back as a carrier
handle and dereferenced -- the segfault.

`Option` escapes at both depths for the same reason it escaped in the parent
report: its continuation resolves to a by-value spec whose pairing is
unshimmed by design.

The fix mirrors the named-wrapper gate on the fat side:

- `ensure_fat_aggregate_spill_shim` (`src/compiler/emit_module.c`) -- keyed
  on the SIGNATURE rather than a callee name, because a capturing
  continuation's real entry point is only known at run time. It reads that
  entry point out of the closure env's `__fn` slot (offset 0 -- the same
  offset the uncast emit already assumed), calls it at its true aggregate
  return type, boxes the result, and hands back the int64 carrier. Returns
  NULL when the return already rides the carrier or a param is not
  int-register-class, so nothing else changes shape.
- The `spill_result_is_byvalue_aggregate` predicate is now factored out and
  shared with `ensure_aggregate_spill_shim`, so the named and fat shims
  cannot drift on which returns need boxing.
- The EX_POLY_WRAP fat-closure branch (`src/compiler/emit_expr.c`) consults
  it under the same gate as the named path
  (`boxes_aggregate || ctx->poly_wrap_callee_carrier`).

Pinned by `tests/fixtures/result-monad-nested-bind-typed-boundary/`: the
`do-m` form, the explicit nested-`bind` spelling, the `(:: ...)` ascription
boundary, three binds deep, a `(Result int cstr)` error type, the err
short-circuit, and the `Option` control. Full suite: 2500 passed, 0 failed,
no snapshot churn (no existing fixture reached the fat-aggregate shape).

`do-m` over `Result` now has no depth limit, so the guide's `Option` and
`Result` examples sit side by side.

## Summary

`result-monad-bind-typed-boundary-miscompiles` (archived, RESOLVED) fixed a
**single** `bind` over `(Result A B)` crossing a typed boundary. Chaining a
second `bind` inside the first one's continuation still segfaults at runtime.
It checks clean (`tur check` exits 0) and compiles clean; only the run dies.

This matters for the docs because `do-m` desugars to nested `.bind`, so any
`do-m` over `Result` with **two or more** binds crashes. `Option` is
unaffected at both depths, so the "`Option` and `Result` behave identically at
the same call shape" bar from the parent report is met at depth 1 and not at
depth 2. It is why `docs/guides/effects-vs-monads.md` still ships a `do-m`
example over `Option` with no `Result` counterpart.

## Repro

    ;; SEGFAULT -- two binds
    (defn half-r [x : int] : (Result int int)
      (if (= x (* 2 (/ x 2))) (ok (/ x 2)) (err 1)))
    (defn quarter-sum-r [x : int] : (Result int int)
      (do-m a (half-r x)
            b (half-r a)
            (ok (+ a b))))
    (defn main [] : int
      (let [r (quarter-sum-r 20)] (println (if (ok? r) (ok-val r) -1)))
      0)

    $ ./build/tur check q.tur   # exit 0
    $ ./build/tur run q.tur     # exit 139, no output

The explicit nested-`bind` spelling fails identically:

    (bind (half-r x) (fn [a] (bind (half-r a) (fn [b] (ok (+ a b))))))

## What narrows it

| Shape | Result |
| --- | --- |
| one `bind`, typed `(Result int int)` defn boundary | works (`20`) |
| one `bind`, typed `(Result int cstr)` defn boundary | works (`20`) |
| two `bind`s, one per `defn` (sequential, not nested) | works (`21`) |
| two `bind`s nested, defn boundary | **SIGSEGV** |
| two `bind`s nested, `(:: ... (Result int int))` ascription, no defn | **SIGSEGV** |
| two `bind`s nested over `(Option int)` (the guide's `do-m` example) | works (`15`, `-1`) |

So it is the **nesting**, not the boundary form and not the error type:
a `bind` appearing inside another `bind`'s continuation is the trigger, and
each `bind` in isolation crosses its boundary correctly.

## Likely direction

The parent report's fix pairs the continuation wrapper's return ABI with the
entry point the dispatch selects (`ctx->poly_wrap_callee_carrier`, set at
call-arg emission and consulted at the EX_POLY_WRAP spill gate). That flag
reads like it is scoped to one call-arg emission; an inner `bind` emitted
while the outer one's arg is in flight is the obvious way for the pairing to
be decided once and applied to the wrong wrapper, or clobbered. Start by
checking whether the flag needs to be saved/restored around nested
`EX_POLY_WRAP` emission rather than set globally on the emit context.

## Guide upkeep -- DELETE, do not amend

This defect is documented in a published guide. When it is fixed, the guide
text must be **removed**, not annotated. Do not leave a "this used to crash"
note behind; guides state current behavior only.

1. In [docs/guides/effects-vs-monads.md](../guides/effects-vs-monads.md),
   **delete the whole `### do-m over Result is limited to one bind` subsection**
   from "Sharp edges". It is written to be self-contained so it lifts out in one
   piece, leaving the surrounding sharp-edge list reading correctly with no
   edit to its neighbors.
2. In the same guide, add the `Result` counterpart beside the `Option`
   `quarter-sum` `do-m` example, which currently stands alone *because* of this
   bug. After the fix the two should sit side by side with no commentary about
   either having once been unavailable.
3. In
   [docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
   move this row from the open-cells table into the closed-cells table with a
   one-line resolution note.

Cross-check afterward: `grep -rn "one bind\|nested-bind-over-result" docs/guides/`
should return nothing.
