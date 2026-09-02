# `perform` reached from inside a `while` has no lowering

**Status: RESOLVED 2026-09-02** -- see the Progress section and the Resolution
at the end; `examples/snake` passes `tur check`.

**Severity: medium** (expressiveness hole with a clear diagnostic, no silent
wrong answer). Found 2026-08-21 while working
`docs/reported/examples-tree-does-not-run.md` -- it is what
`examples/snake/src/main.tur` actually fails on, once its `import`-outside-
`defmodule` spelling is fixed.

## Repro

```turmeric
(defeffect Done [score : int] : nil)

(defn run [] : nil
  (let [^mut i 0]
    (while (< i 10)
      (when (= i 3) (perform (Done i)))
      (set! i (+ i 1)))))

(defn main [] : int
  (do
    (handle
      (run)
      (Done [score] k) (println score))
    0))
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset, and the direct emitter cannot lower
`perform`. The usual cause is a loop inside a `handle` clause -- hoist that
work into a helper function and call it from the clause. This is a compiler
limitation, not a mistake in this expression.
```

## What narrows it

The loop is the whole trigger, and **hoisting does not escape it** -- which
matters, because hoisting is exactly what the diagnostic tells you to do.

| Variant | Result |
| --- | --- |
| `perform` directly inside the `while` body | **error** |
| `perform` in a helper CALLED from inside the `while` body | **error** |
| `while` runs to completion, then the helper performs after it | compiles |
| no loop at all | compiles |

The third row is the important one: the same helper, the same `perform`, the
same handler -- only the call site moves out of the loop body, and it
compiles. So the rejected thing is not the shape of the performing function;
it is that the `perform` is *reachable from a loop body* at all.

The diagnostic is therefore misleading twice over. It says "a loop inside a
`handle` **clause**" when the repro's loop is in the handle **body**, and it
prescribes "hoist that work into a helper function" when hoisting into a
helper is one of the failing rows. Someone following it does the work and
lands on the same error -- which is what happened here, on
`examples/snake/src/main.tur`, before the rule above was worked out.

## Why this matters beyond the example

A `perform` inside a loop is not an exotic shape -- it is the shape of every
event loop, every "poll until something happens", every per-item effect in a
traversal. `examples/snake` is a game loop performing `GameOver` on collision;
that is the canonical use of a one-shot abort effect, and it cannot be
written today.

## Fix direction

Two levels, and the cheap one is worth doing on its own:

1. **Fix the diagnostic first.** It should say what the rule actually is (a
   `perform` reachable from a loop body), and it must stop prescribing a
   workaround that does not work. Point it at this report. This is a
   half-hour change and it stops the next person spending an hour rediscovering
   the table above.
2. **The real fix** is CPS support for a `perform` under a loop: the loop has
   to become a recursive/trampolined form the backend can suspend and resume
   through, rather than a C `while` the direct emitter owns. That is the same
   machinery `docs/archive/repr-decision-function-plan.md`-era work already
   builds for other suspension points, so start by finding out why the loop
   lowering opts out of it.

Until then, a program that needs this can invert the control flow: have the
loop body return a status and perform *after* the loop (row 3 above), which
compiles today and is what `run`/`step` splits usually want anyway.

## Progress (2026-09-02): the report's shapes lower; one residue keeps it open

Three defects sat behind the table above, and the first two are what made
the diagnostic's advice wrong:

1. **A tail-position `while` never reached the loop lowering.** `cps_tail`
   (the tail translation) had no `EX_WHILE` arm, so a loop that is the LAST
   form of a nil-returning function fell to the default and evicted the whole
   function ("unsupported form: EX_WHILE"), while the very same loop followed
   by a live value went through `cps_bind` and `build_loop`. That is the
   report's own repro, and every event loop. `cps_tail` now routes a tail loop
   through `build_loop`, which admits a unit exit (nothing live after the
   loop) alongside the single-live-after-var shape.
2. **A conditional `perform` in statement position emitted invalid C.**
   `(when c (perform ...))` followed by more statements lowers to a join
   (`letcont j`), and a perform continuation is lifted into its own C
   function -- which then tried to `goto` the join's label in the parent
   function (`0 = __t5; goto L4;`). Loop or no loop: the same broke a plain
   function. Such a join is now reified as a DK resume-frame
   (`emit_escaping_join`): the parent frame runs it directly, a lifted frame
   reaches it through its downstream chain (the frame was `cur_k` while the
   body emitted), and the join body delivers through the frame's next exactly
   once. Admission mirrors the heap-join rules (slot captures only, closed
   joins); otherwise the function evicts as before.
3. **The diagnostic** now names `TUR_TRACE_EVICT=1` and the shapes that
   actually evict, and no longer prescribes hoisting -- which, re-checked,
   does not work for the handler-clause loop either (the hoisted helper is
   evicted with its caller by the effect taint), so that fixture
   (`errors/effect-handler-clause-loop-perform-unsupported`) now pins the
   accurate wording.

Pinned by `tests/fixtures/effect-perform-in-tail-while` (the repro, abortive
and resuming) and `tests/fixtures/effect-perform-conditional-join` (a plain
function and a loop, resuming). Guides updated: effects-system-guide gains
"Performs inside loops and conditionals"; effects-vs-monads no longer
prescribes the hoist.

**The snake residue, closed the same day.** Its loop assigns `tick` twice
per iteration, once inside a `when` arm, and `loop_guard` rejects a
conditional or repeated assignment of a loop-carried variable (the helper's
parameters resolve reads to the loop-entry version and give each variable one
`$next` slot). Four more pieces, each found by the next eviction:

- **Cell-carried variables.** A variable the strict guard rejects is carried
  in the B7 shared heap cell instead of evicting the loop (`build_loop`
  classifies per variable; `byref_scan`'s CT_LOOP case promotes exactly the
  delegated `set!` targets that are not loop params). Its `set!`s lower as
  ordinary delegated writes through the cell and its reads deref it, so the
  helper, every lifted frame inside it, and the code after the loop see one
  location. Variables the guard accepts stay parameters, so the strict
  shape's codegen is unchanged.
- **A loop followed by more statements.** `build_loop` admitted only a
  continuation that was a bare delivery. It now reifies any other
  continuation as an escaping join (`letcont j(x) = rest in LOOP`, the loop
  delivering unit to `j`), with every carried variable a cell so the code
  after the loop reads its final value; `emit_loop` threads the join frame
  as the helper's kont.
- **Nested escaping joins.** A join reached from inside another escaping
  join's frame is itself escaping (`join_escapes_lifted_rec` treats a
  reified nested join's body as lifted), and its frame delivers to the outer
  one through its own downstream chain; `needs_heap_join` seeds the
  closedness check with the enclosing escaping joins.
- **Back-edges from lifted frames.** A frame that takes the loop's back-edge
  re-enters the helper with its loop-invariant extra arguments (the cell
  pointer, a param read but never assigned), so `collect_caps` merges the
  enclosing loop's invariants into such a frame's captures.
- **A call in loop-tail position** (`(render-frame)` at the end of the body)
  reached the emitter as a join jump; `cps_tail` now binds a non-structural
  tail form under the loop kont and takes the back-edge.

And one coloring fix that had nothing to do with loops: the coloring pass
treated a call to an `extern-c` binding as an unresolved indirect call, so
every helper touching the FFI (`render-frame`, `show-game-over`) was colored,
and a colored helper called from the handler clause is a heap join the clause
grammar does not admit. An `extern-c` callee is the same opaque-C leaf as an
inline-C body and is now exempt.

Pinned by `tests/fixtures/effect-perform-in-loop-conditional-set` (the snake
loop: abortive and resuming, plus the loop-followed-by-statements form).
`examples/snake/src/main.tur` passes `tur check`. `TUR_TRACE_CORE=1` names
the form the structural check rejects; the eviction trace's
`BODY-STRUCT-OR-TAINT` splits into `BODY-STRUCT-CORE`, `BODY-STRUCT-JOIN`
and `BODY-TAINT`.

## Guides updated

- docs/guides/effects-system-guide.md -- "Performs inside loops and conditionals".
- docs/guides/effects-vs-monads.md -- handler-clause restrictions no longer prescribe the hoist.
