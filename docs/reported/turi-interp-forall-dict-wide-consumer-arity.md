# Interpreter drops the implicit Functor dict for wide van-Laarhoven consumers

**Summary:** Under `--interpret`, a lens consumer that takes a `forall`-typed
lens param and dispatches a `^Functor` constraint fails with an arity mismatch,
because the tree-walking interpreter does not thread the implicit dictionary
argument the compiled by-value HKT path passes. Affects the three
`van-laarhoven-lens-wide-consumer-{clone,forward,resolve}` fixtures
(`requires.interp`). **Severity: low** (interpreter-only; the compiled path --
the primary target and the `expected.stdout` these fixtures also carry -- is
correct).

## Repro

```sh
./build/tur --interpret --dump-mono-specs \
  tests/fixtures/van-laarhoven-lens-wide-consumer-resolve/input.tur
# tur: eval: arity mismatch: point-x expects 2 args, got 3
```

`point-x` is `(defn point-x [^f] [^Functor f g : (-> int (f int)) s : Point] ...)`.
The compiled path passes the `^Functor` dictionary as a hidden argument (3
actuals: dict, g, s); the interpreter counts only the two source-visible value
params (g, s) and rejects the dict-carrying call.

## Root cause

The interpreter (`src/turi/eval.c`) applies these consumers without the
implicit-dictionary calling convention the compiled forall-dict-pass lowering
uses. It sees the callee's declared arity (source value params) and the caller
supplying an extra dict slot, and raises `arity mismatch` instead of binding the
dict param. This is the interpreter-side counterpart of the compiled
`forall-dict-pass` work (#607/#611/#613) and the open compiled-path report
`docs/reported/forall-dict-direct-applied-nested-lambda-dispatch.md`.

## Fix directions

Teach the interpreter's apply path the same implicit-`^Functor`/`^Show`
dictionary convention: when a callee declares constraint params (`^Class v`),
accept and bind the leading dictionary actual(s) the caller passes, rather than
counting only the trailing value params. Until then these three interpreter
fixtures stay red while their compiled `expected.stdout` passes.
