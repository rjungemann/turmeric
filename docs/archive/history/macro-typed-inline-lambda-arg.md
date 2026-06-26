# Typed inline lambda passed as a macro argument: "compile-time fn parameter must be a symbol"

**Component:** macro expander / compile-time evaluation
**Severity:** ergonomic -- affects any `with-*`-style macro whose binding
initializer is an inline typed closure
**Status:** RESOLVED

## Summary

When a macro argument contained a typed inline lambda `(fn [x : int] : void ...)`,
macro expansion failed with `compile-time fn parameter must be a symbol`. The
expander treated the user lambda as a *compile-time* function (whose parameters
must be bare symbols) rather than as inert runtime AST to be spliced. Passing a
**named** top-level `defn` in the same position worked.

## Minimal reproduction

```turmeric
(defmacro with-thing [binding & body]
  `(let [~(first binding) ~(first (rest binding))]
     ~@body))

(defn apply-cb [f : (fn [int] void) x : int] : int (f x) x)

(defn main [] : int
  (with-thing [r (apply-cb (fn [n : int] : void 0) 7)]
    r))
```

```
...: error: compile-time fn parameter must be a symbol
```

Naming the callback (`(defn cb [n : int] : void 0)`, then passing `cb`) made the
identical expansion compile and run.

## Root cause

`src/compiler/elab_macros.c`, the `fn` branch of `ct_eval_call`
(`head == env->elab->sym_fn`). The compile-time evaluator dispatches `fn` to a
compile-time-lambda constructor that requires every parameter to be a bare
`F_SYM`. A typed parameter `[x : int]` reads as `F_SYM(x)` followed by an
`F_TYPE_ANN` node, so the per-parameter `p->tag != F_SYM` check fired and
rejected it.

This path is reached for a typed inline lambda only when it is threaded through
as **data** -- an argument value spliced via `~`/`~@`. A literal typed `fn`
sitting in a quasiquote *template* is handled by `ct_eval_quasiquote` (which
preserves it verbatim) and was never affected; only a fully-substituted `fn`
form reaching `ct_eval_form` hit the compile-time-fn validator.

## Fix

In the `fn` branch, before constructing a compile-time fn, detect the
runtime-lambda shape -- any parameter that is not a bare `F_SYM` (a `: T`
annotation), or a return-type annotation (`F_TYPE_ANN`/`F_KEYWORD`) right after
the param vector -- and return the form untouched (`ct_value_form(f)`) so it is
spliced verbatim. Genuine compile-time fns (bare params, no return type, applied
at expansion time, e.g. inside a compile-time `map`) are unaffected.

Regression fixture: `tests/fixtures/macro-typed-inline-lambda-arg/`.
