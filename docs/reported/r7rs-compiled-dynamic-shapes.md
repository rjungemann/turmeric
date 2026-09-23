# Compiled back end: three dynamic-closure shapes `#lang r7rs` R2 needs

**Severity: medium.** Every `#lang r7rs` program that uses a named `let`, a
`do` loop, `letrec`, `call-with-values`, `apply` or `for-each` is
interpreter-only today. The plan (r7rs-lang-plan.md, D6/R6) stages R7RS
interpreter-first and lifts the compiled path in R6, so this is expected --
but each shape is a plain Saffron program too, and none of the three failures
is Scheme-specific. Found 2026-09-23 while landing R2; `tur --interpret`
answers every repro correctly.

## 1. A letrec-bound closure over `any` that calls itself

```turmeric
#lang saffron
(defn main [] : int
  (letrec [loop (fn [i acc] (if (< i 5) (loop (+ i 1) (+ acc i)) acc))]
    (println (loop 0 0)))
  0)
```

```
$ tur run b5.tur
b5_tur.c: In function '__fn_1640':
b5_tur.c:5944:13: error: aggregate value used where an integer was expected
 5944 |             __t62 = TUR_TAG(3, (int64_t)(intptr_t)(__ps_63));
```

The same `letrec` with annotated `int` parameters compiles
(`tests/fixtures/fn-recursion-letrec-and-named-let`); the unannotated (`any`)
version hits the closure-capture path with a `tur_tagged_t` where the
emitter expects a carrier word. This is the shape every Scheme named `let`,
`do` loop and `letrec` lowers to (`src/compiler/scheme_lower.c`).

## 2. A dynamic call the fat-closure apply helpers refuse

```turmeric
#lang r7rs
(for-each (lambda (x) (display x) (display ",")) (list 1 2))
```

```
$ tur run c7.tur
panic at c7_tur.c:1099: cannot call this function here -- it takes a different
number of arguments, or parameters this call site cannot supply
```

`r7rs-for-each` calls `(f (.head l))` with `f : any`; the lambda's body ends
in a `nil`-returning call. `map` with `(lambda (x) (* x x))` -- an
`any`-returning body -- works through the same call site, so the refusal is
about the callee's return representation, not its arity. The same panic
stops `call-with-values` / `apply` (`r7rs-apply` calls `(f a b)` with
`f : any` and a two-parameter lambda) and a `case-lambda` clause. Static
side: "calling a dynamic value with 5 arguments is not supported by the
compiled back end (the fat-closure apply helpers stop at 4)", which is why
`r7rs-apply` caps at four arguments.

## 3. `type-of` on an `any`-typed rest parameter

```turmeric
#lang saffron
(defn t [x] x)
(defn g [& xs : any] (println (type-of (t xs))) (list-length (:: xs (Cons any))))
(defn main [] : int (println (g 7.1 2)) 0)
```

Compiled: `Segmentation fault`; interpreted: `Cons` then `2`. The
`(:: xs (Cons any))` half alone is fine (it is `r7rs-list`'s whole body and
`tests/fixtures/r7rs-core-forms` runs it compiled), so the crash is in
boxing the rest list through `t`.

## Fix directions

- (1) is in the closure-env fill for a `letrec`-bound lambda whose
  parameters are `any` (emit_fns.c / the closure capture path): the captured
  self-reference is written as a carrier word where the cell is a fat
  `tur_tagged_t`. T6 of proper-tail-calls-plan.md landed the fat-closure
  protocol for Saffron's dynamic calls; this is the letrec-cell half of it.
- (2) is the dynamic-call helper's admissibility test: a `nil`-returning
  callee (and a variadic one) needs a row. Lifting the four-argument cap is
  the same table.
- Until both land, `tests/fixtures/r7rs-core-forms-interp` and
  `r7rs-named-let-sum` carry `requires.interp`; drop the markers when the
  repros above compile, and delete this report.
