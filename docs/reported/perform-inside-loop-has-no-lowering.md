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

## Guides to update when fixed

- docs/guides/effects-guide.md -- it does not currently state this limit.
