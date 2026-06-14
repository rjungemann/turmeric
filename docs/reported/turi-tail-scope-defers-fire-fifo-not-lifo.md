# turi: single-scope defers at function exit fire FIFO, not LIFO (interpreter/compiled divergence)

**One-line:** Under `tur --interpret`, defers registered in a single scope that
fires at **function exit** (tail position) run in registration order (FIFO),
whereas the compiled path -- and the interpreter's own *non-tail* scope path --
run them in reverse registration order (LIFO). Silent wrong-output divergence
(rc=0), surfaced while landing T2 of the eval-trampoline plan.

**Severity:** medium. Not a crash; wrong observable ordering of defer side
effects. Scoped to a real but narrow shape (defers whose enclosing scope is in
tail position of its function), which is why no fixture currently catches it.

## Minimal repro

```turmeric
;; m2.tur -- the let is the TAIL body of f, and `x` is the let's last form
(defn f [] : int
  (let [x 1]
    (defer (println 10))
    (defer (println 20))
    (println 30)
    x))
(defn main [] : int (println (f)) 0)
```

| Path | Output |
| --- | --- |
| `tur build` (compiled) | `30` / `20` / `10` / `1` (LIFO) |
| `tur --interpret`      | `30` / `10` / `20` / `1` (FIFO) -- **wrong** |

Move the same defers into a **non-tail** scope and the interpreter agrees with
compiled (LIFO):

```turmeric
;; m1.tur -- the let is NOT the last form of main (the `0` follows it)
(defn main [] : int
  (let [x 1]
    (defer (println 10))
    (defer (println 20))
    (println 30))   ;; non-tail let
  0)
;; both compiled and --interpret: 30 / 20 / 10  (LIFO, correct)
```

The existing `tests/fixtures/defer-order` fixture only exercises the non-tail
shape (its `let` is followed by `0` in `main`), so it passes under both paths and
does not catch this.

## Root cause

The interpreter has two defer-firing helpers in `src/turi/eval.c`:

- `fire_defers_to_mark` (LIFO) -- walks `env->defer_stack` head-first (most
  recently pushed first). Used by the per-scope exit paths, e.g. the non-tail
  `EX_LET` (now the T2 driver's `DK_LET_BODY`). Correct, matches compiled.
- `fire_defers_to_mark_reversed` (FIFO / oldest-first) -- counts the chain and
  fires in reverse. Its banner says it exists to "match compiled
  `tur_frame_fire_chain` semantics: on early-return, outer defers fire before
  inner defers."

The function-exit path (`eval_apply` / `eval_body_tco`, tail position) fires the
**entire** pending defer chain with `fire_defers_to_mark_reversed`. That is the
right idea for *cross-scope* ordering on early-return (outer scope before inner
scope), but it is wrong *within a single scope*: compiled `tur_frame_fire_chain`
fires same-scope defers LIFO and only orders *scopes* outer-first. Reversing the
flat chain collapses both into one FIFO walk, so multiple defers in one
tail-position scope come out FIFO instead of LIFO.

Pointers: `fire_defers_to_mark` (`src/turi/eval.c:542`),
`fire_defers_to_mark_reversed` (`src/turi/eval.c:576`), and the tail dispatch in
`eval_body_tco` / `eval_apply` that calls the reversed form at function exit.

This is **pre-existing** and independent of the T2 trampoline work: the T2 slice
only converted the *non-tail* `EX_LET`/`EX_LETREC` (which already uses the LIFO
`fire_defers_to_mark` and is correct); the divergence lives entirely in the
tail/function-exit path, which T2 did not touch. Confirmed by the full
`run-turi.sh` staying green (1186 passed) across the T2 change.

## Proposed fix direction

`tur_frame_fire_chain` (compiled) is the oracle: same-scope LIFO, scopes
outer-first. The interpreter should mirror that two-level ordering rather than
flat-reversing. Options:

1. Track a per-scope mark on the defer chain (a scope boundary marker pushed at
   each frame/let entry) and, at function exit, walk scope-by-scope outer-first
   while firing each scope's defers LIFO -- i.e. reverse the *list of scopes*,
   not the list of *items*.
2. Or fire each scope's defers eagerly at that scope's exit with
   `fire_defers_to_mark` (LIFO), so only genuinely cross-scope early-return
   ordering needs the reversed walk -- and even then, reverse by scope, not item.

## How to validate a fix

- `m2.tur` above prints `30 / 20 / 10 / 1` under `--interpret` (matching
  compiled).
- Add a fixture with **multiple** defers in a tail-position scope plus a nested
  inner scope with its own defers and an early `return`, asserting the compiled
  ordering, and run it under both `run.sh` and `run-turi.sh` (no marker).
- `tools/check_turi_parity.py` 0-gaps; full `run-turi.sh` / `run.sh` green.
