# `perform` reached from inside a `while` has no lowering

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

**Still open -- `examples/snake` itself.** Its loop assigns `tick` twice per
iteration, once inside a `when` arm (`(set! tick (- tick MOVE_INTERVAL))`),
and `loop_guard` rejects a conditional or repeated assignment of a
loop-carried variable: the lowering resolves reads to the loop-entry version
and writes one `$next` slot per variable, so a second or conditional write has
no sound home. The fix direction is to carry such a variable in a shared cell
(the B7 by-reference mechanism handler clauses already use) rather than in
the helper's parameters, so reads and writes anywhere in the body -- lifted
frames included -- see one location. Until then snake needs the
restructuring the guide describes: compute the next `tick` into a `let` and
assign once at the end of the body.

## Guides to update when fixed

- docs/guides/effects-guide.md -- it does not currently state this limit.
