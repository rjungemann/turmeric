# A CPS function inlines a closure as a partial application whatever its body passes

**RESOLVED 2026-09-26**, the day it was found. `pap_extract`
(src/passes/cps_ir.c) now requires the wrapper body's arguments to be exactly
the closure's captures followed by its own parameters, in order, before it
treats the closure as a partial application. Regression fixtures:
`tests/fixtures/cps-pap-inline-ignores-wrapper-arguments` (plain Turmeric; the
two wrong answers, plus two genuine partial applications as controls, which
the rewrite still takes) and `tests/fixtures/r7rs-closure-one-capture-call`
(the `#lang r7rs` compile error). Full `tests/run.sh`: 3220 passed, 0 failed.

**Severity: high.** A silent wrong answer, in every dialect, from a common
shape: a closure called in place or through a `let`, inside any function the
CPS pass compiles (one that calls `bt-scope`, performs an effect, and every
`#lang r7rs` procedure that calls a procedure).

Found executing the open R7RS reports: a test of
[r7rs-define-record-type-not-an-internal-definition](r7rs-define-record-type-not-an-internal-definition.md)
wrote `((lambda () (list ...)))` over a `let`-bound variable, and `cc` refused
the program. The record types had nothing to do with it.

## Repro

```turmeric
(defn sub [a : int b : int] : int (- a b))
(defn neg [a : int] : int (- 0 a))

(defn order [p : int] : int
  (bt-scope (fn [] 0))                  ;; only here to make the defn CPS
  ((fn [x : int] : int (sub x p)) 10))

(defn value [p : int] : int
  (bt-scope (fn [] 0))
  ((fn [] : int (neg (+ p 1)))))

(defn main [] : int
  (println (order 3))   ;; 7
  (println (value 3))   ;; -4
  0)
```

Before the fix this printed `-7` and `-3`: `(sub p 10)` and `(neg p)`. The
interpreter printed `7` and `-4`.

Under `#lang r7rs`, `(let ((p (+ v 1))) ((lambda () (list p))))` emitted
`r7rs_hylist(p)` -- `list`'s one parameter is its packed rest chain, and it was
handed the bare value -- which `cc` rejects ("incompatible type for argument 1
of 'r7rs_hylist'"). `(car (list p 1))` in the same place became `(car p)`.

## Root cause

The CPS IR builder inlines a let-bound partial application: the elaborator
lowers an under-saturated `(TARGET c0 ...)` to a closure over a synthesized
`__pap` thunk whose body is `(TARGET __papc0 ... __papr0 ...)`, and
`pap_register_let` / `pap_maybe_rewrite` rewrite each saturated call of the
closure to `(TARGET cap0 ... args...)`, dropping the closure.

`pap_extract` recognized that shape by ARITY alone: a closure with captures
whose body is one saturated call to a named function, with at least as many
parameters as captures. Any written lambda with a one-call body has that
shape. The rewrite then substituted the captures and the call site's
arguments for whatever the body passed:

- `(fn [x] (sub x p))`, one capture: arity 2 >= 1 capture, so `(f 10)` became
  `(sub p 10)`.
- `(fn [] (neg (+ p 1)))`, one capture: arity 1 >= 1, so `(f)` became
  `(neg p)`.
- `(fn [] (list p q))`, two captures: arity 1 < 2, refused -- which is why a
  lambda over two variables worked and one over a single variable did not.

The comment in `pap_extract` already said the arity test could not tell a
partial application from a thunk (from fixing
[cps-direct-bt-scope-closure-temp-undeclared](cps-direct-bt-scope-closure-temp-undeclared.md)),
but pointed at `pap_calls_saturated` as the real guard. That one checks how
the closure is USED; nothing checked what its body DOES.

## Fix

`pap_extract` checks the body's arguments by identity: argument `i` is an
`EX_VAR` (optionally under the `EX_POLY_WRAP` a rank-2 `__pap` carries) naming
`captures[i]` for the first `n_captures`, then `params[1 + j]` (params[0] is
the env) for the rest, and the thunk takes exactly the remaining arity. An
elaborator `__pap` meets that by construction, and so does a written
`(fn [x] (g p x))`, so the optimization still fires where it is sound (the
fixture's controls, and `--dump-cps` shows their closures dropped).
