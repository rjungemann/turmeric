# A self-recursive call returning a fn value crashes when the result feeds a `^fat` parameter

**Severity:** medium-high. Silent miscompile -> runtime SIGSEGV, and the shape
is the natural way to build a combinator chain recursively -- the first thing
anyone composing searches, parsers, or streams will write.

**Status:** OPEN. Minimal repro and a one-line workaround below; root cause
narrowed but not fixed. Found building SX2's depth-first driver
(`stdlib/backtrack-dfs.tur`).

## Repro

```turmeric
(load "stdlib/backtrack-dfs.tur")
(defn mk-succeed [] : (fn [(fn [] bool)] bool) (dfs-succeed))

(defn gb [c : BtCell d : int] : (fn [(fn [] bool)] bool)
  (if (<= d 0) (mk-succeed)
    (dfs-and (dfs-choose-int c 1 2) (gb c (- d 1)))))   ;; SELF-call -> ^fat sink

(defn main [] : int
  (let [c (bt-cell-new 0)]
    (println (dfs-solve (gb c 0) (fn [] true)))   ;; 1 -- fine (no self-call runs)
    (println (dfs-solve (gb c 1) (fn [] true)))   ;; SIGSEGV
    (bt-cell-free c))
  0)
```

`dfs-and` takes its arguments `^fat`. The crash needs BOTH: the callee is the
enclosing function itself, and the result lands in a `^fat` parameter.

## The boundary, established by probes

| shape | result |
|---|---|
| non-recursive call result -> `^fat` sink | works |
| if-joined fn value -> `^fat` sink | works |
| let-bound SELF-call result -> `^fat` sink | **SIGSEGV** (binding it first does not help) |
| direct SELF-call result -> `^fat` sink | **SIGSEGV** |
| self-call routed through a one-line FORWARDER | **works** |

The forwarder is the workaround, and its effectiveness is the diagnostic clue:

```turmeric
(defn gb-fwd [c : BtCell d : int] : (fn [(fn [] bool)] bool) (gb c d))
;; inside gb, call (gb-fwd c (- d 1)) instead of (gb c (- d 1))  -- works
```

The only difference between the two calls is WHICH binding the call goes
through: the completed forwarder's, or the still-being-elaborated function's
own. That points at the fn-value fat-normalization state on the self-binding:
inside the body, the function's own binding has not yet taken whatever marking
the finished defn gets, so the call result is treated as needing a shim it
already has (or vice versa) -- the same double-shim family as the fixed
closure-in-defdata-field bugs, where slot 0 of a re-wrapped box is code that
then gets dispatched as an env.

## In-tree workaround sites (revert when fixed)

- `benchmarks/bench-dfs-queens.tur` -- `queens-goal-fwd`, a forwarder whose
  docstring names this report. Inline it when this lands.

`stdlib/backtrack-dfs.tur` itself is NOT affected: its recursion
(`dfs-choose-go`) passes `k` through as a parameter rather than feeding a
self-call's result to a `^fat` sink -- a driver-internal style that sidesteps
the bug and is worth knowing as the second workaround.

## Fix direction

Wherever the stage-2 fn-value normalization marks a completed defn's binding
(so callers know its result is already fat), the function's OWN binding during
body elaboration needs the same answer a fresh caller would get -- the
forwarder works precisely because it consults the finished marking. Compare
how the recursion in `emit_abi_register_call` recovers a self-call's result
type through the active spec's bindings; this is the fat-normalization twin of
that situation.
