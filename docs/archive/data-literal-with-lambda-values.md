# A `#map{...}` whose values are lambdas does not compile; a closure read out of a `(Vec any)` is an int under `--interpret`

**RESOLVED 2026-09-26, the day both were found** (probing closure ownership in
Saffron for
[dynamic-returned-closure-env-is-never-freed](../reported/dynamic-returned-closure-env-is-never-freed.md)).
Filed here for the paper trail; neither had an open report.

## 1. `#map{"add" (fn [a b] (+ a b))}` -- both engines

```turmeric
#lang saffron
(defn main []
  (let [ops #map{"add" (fn [a b] (+ a b)) "mul" (fn [a b] (* a b))}]
    (println ((map-get ops "add") 3 4)))
  0)
```

```
error: compile-time macro evaluation expected a form value
```

Any UNANNOTATED lambda as a map value (or inside a `set-of` element) did it,
in a typed file as well as a Saffron one; `(fn [y : int] ...)` did not.

**Cause.** A macro parameter is SUBSTITUTED into the body before the
compile-time evaluator runs it (`substitute_params`, then
`elab_eval_macro_form`, `src/compiler/elab_macros.c`); only a `^syntax`
parameter is bound as the raw argument form instead. `hamt-assoc-each__`
recurses with the accumulated `(map-assoc-consuming__ ... (fn [a b] ...))` as
its `m`, and its base case `(if (empty? kvs) m ...)` then EVALUATED that form
at compile time -- where `(fn [a b] ...)` is the evaluator's own lambda
special form, producing a compile-time function rather than a form. `hamt-of`'s
`~@kvs` splice did the same to the argument list. An annotated lambda is not
the special form's shape, which is why it slipped through.

**Fix.** Every parameter of the two literal families is `^syntax`:
`hamt-of`, `hamt-homog-chain__`, `hamt-assoc-each__` (stdlib/map.tur),
`set-of`, `set-add-each__` (stdlib/set.tur). The expansion is otherwise
identical -- no `expected.c` snapshot moved. `vec-of` never had the shape (its
accumulator is the symbol `__v`).

## 2. A closure read out of a `(Vec any)` -- `tur --interpret`

```turmeric
#lang saffron
(defn main []
  (let [fs (vec-new)]
    (vec-push! fs (fn [y] (+ y 1)))
    (println ((vec-get fs 0) 2)))
  0)
```

Compiled: `3`. Interpreted: `panic: cannot call a int value -- it is not a
function`.

**Cause.** The interpreter's vec stores raw int64 cells and re-tags them on
read from a per-element tag table (`vec_retag_cell`,
`src/turi/collections_native.c`); it had arms for float, cstr, bool and struct
but not for a closure, so the cell came back `TURI_INT`. Typed code never
noticed, because its `(fn ...)` ascription at the read re-typed the word; a
`(Vec any)` element is called dynamically, with nothing in between.

**Fix.** A `TURI_CLOSURE` arm, re-wrapping the cell's `TuriClosure *`.

Both pinned by `tests/fixtures/saffron-data-literal-lambda-values` -- a
dispatch table with a capturing entry, a computed key through `hamt-of`, a
`set-of` over lambda-computed elements, and closures read back out of a vec --
identical compiled and under `--interpret`.
