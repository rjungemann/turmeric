---
status: open
severity: high (silent wrong answer, no diagnostic)
discovered: 2026-08-05
area: compiler (CPS/DK backend, handler-case resume lowering)
---

# A non-tail multishot resume whose continuation re-enters an inner handle drops the rest of the clause

## Summary

On the compiled path, when a `^multishot` handler clause resumes in NON-tail
position and the resumed continuation runs back through an **inner `handle`**,
the first resume's value is delivered as the outer handle's value directly --
the remainder of the clause (its arithmetic and any further `resume`) never
runs. The program compiles cleanly and prints a wrong number.

Found 2026-08-05 while executing
[turi-ws-capturable-stale-black-box-arms](../archive/turi-ws-capturable-stale-black-box-arms.md):
the interpreter, once its work-stack driver could run these shapes at all,
produced a *different answer* from the compiled path -- and hand-evaluation
says the interpreter is right.

## Repro

```turmeric
(defeffect Ask [] : int)
(defeffect Log [n : int] : int)
(defn main [] : int
  (println (handle
             (handle (let [v (perform (Ask))] (perform (Log v)))
                     (Log [n] k2) (resume k2 (* n 2)))
             (Ask [] ^multishot k) (+ (resume k 1) (resume k 10))))
  0)
```

Expected **22**: `resume k 1` runs the continuation -- `Log 1` through the
inner handler, doubled, `2` -- and returns 2 to the `+`; `resume k 10` returns
20; the clause's value is 22. `tur --interpret` prints 22.

`tur run` prints **2**.

The weighted variant pins down what happens:

```turmeric
(Ask [] ^multishot k) (+ (* 1000 (resume k 1)) (resume k 10))
```

turi: **2020** (= 1000*2 + 20, correct). Compiled: **2** -- not 2000, not
2002. So the first resume's value (2) did not return into the clause's `(*
1000 _)` at all; it became the outer handle's value, and everything after the
first resume in the clause was discarded.

## Boundary conditions (all verified)

| Variant | Compiled | turi | Verdict |
| --- | --- | --- | --- |
| the repro (double resume, inner handle) | **2** | 22 | compiled wrong |
| single `resume k 7`, same inner handle | 14 | 14 | both right |
| double resume, NO inner handle (`(* 2 (perform (Ask)))` body) | 22 | 22 | both right |
| double resume, inner handle, perform in arg position (`(perform (Log (perform (Ask))))`) | 2 | 22 | compiled wrong (same mechanism) |

So each ingredient is individually fine; the failure needs all three: a
multishot clause, a resume in **non-tail** position, and an **inner handle**
re-entered by the resumed continuation.

## Root cause (hypothesis, not pinned)

The symptom -- "the resumed continuation's final value becomes the outer
handle's value instead of returning to the `dk_invoke` call site" -- is
exactly the semantics of a TAIL resume (`dk_tail_resume` yields to the entry
driver and the value is delivered by it, nothing follows in the clause). The
non-tail path (`emit_cps_ir.c`'s `emit_resume`, the `dk_invoke` branch) is
supposed to return the value inline. A plausible mechanism: re-entering the
inner handle re-installs a prompt/handler node whose delivery routes the
continuation's completion to the outer prompt chain rather than back out of
`dk_invoke` -- i.e. `dk_invoke`'s "run until this sub-chain completes" boundary
is not sealed against a fresh prompt installed *during* the resumed run. Not
verified; start by tracing which DK node receives the inner handle's
completion value in the repro vs. the no-inner-handle variant.

## Where the truth is pinned

`tests/fixtures/turi-ws-driven-operands` asserts the correct values on the
interpreter (22 and 2020 among them). There is deliberately no compiled
fixture for this shape yet -- it would have to assert the wrong number or
fail. When this is fixed, move those two cases into a both-paths fixture.

## Impact

High: silent wrong answers, in the composition the effects guide recommends
(nested handlers instead of transformer stacks). A user who follows
`effects-vs-monads.md`'s "nest handlers -- no lifting" advice and folds a
multishot continuation over work that touches an inner handler gets a number,
not an error.
