---
title: turi fired a scope's defers BEFORE evaluating a tail call
category: Archive
description: The interpreter's frame-reusing tail call (F3) fired the current activation's defers as "frame completion" before entering the callee, so `(defer (println "d")) (shout)` printed "d" first and a defer-based bt-scope undid the trail before the body wrote to it. The compiled path never tail-calls out of a scope with a defer. Found 2026-09-11 rewriting bt-scope onto a defer; fixed the same day.
---

# turi fired a scope's defers BEFORE evaluating a tail call

**RESOLVED 2026-09-11, the day it was found.** Filed straight into the archive
as the paper trail for the fix.

**Severity: medium-high** -- a silent interpreter/compiled divergence on the
ORDER of side effects, which for a resource bracket is the whole point: the
cleanup ran before the work.

## Repro

```turmeric
(defn shout [] : int (println "body ran") 1)
(defn g [] : int
  (let [m 1]
    (defer (println "defer fired"))
    (shout)))
(defn main [] : int (g) 0)
```

| Path | Output |
| --- | --- |
| `tur build` | `body ran` / `defer fired` |
| `tur --interpret` | `defer fired` / `body ran` -- **wrong** |

Same for a `do` body and a bare defn body. `(let [r (shout)] r)` in place of
the tail call was fine, which is what isolated it.

## Root cause

`src/turi/eval.c`, the F3 tail call in `DK_CALL_ARG`: when the call is in tail
position it REUSES the enclosing activation's `DK_CALL_RET`, and "finishing"
that activation included `fire_defers_to_mark(env, ret->aux)` -- before the
callee's body had run. The compiled emitter never treats a scope holding a
`defer` as a tail position ("defers break tail", `emit_fns.c`).

## Fix

The reuse is now gated on the activation having registered no defers
(`env->defer_stack == st[len-2].aux`). With a defer pending the call takes the
ordinary non-tail fold: the callee gets its own `DK_CALL_RET`, and the
activation's own `DK_CALL_RET` fires the defers once the value has come back.
Only the O(1)-stack property of the tail chain is given up, and only in a
scope that already holds a defer frame -- the same trade the compiler makes.
The fire that sat in the reuse path is gone (it was a no-op under the guard).

Pinned by `tests/fixtures/bt-scope-panic-undo` and
`defer-generic-hof-caught-panic` under `run-turi.sh`, and by the existing
`defer-tail-scope-order` / `defer-early-return` (unchanged).
