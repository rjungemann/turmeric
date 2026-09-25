# Compiled back end: three dynamic-closure shapes `#lang r7rs` R2 needs

**RESOLVED 2026-09-24 (r7rs-lang-plan R6), archived.** Sections 1, 2 and 2b
are closed on both back ends and pinned by `tests/fixtures/saffron-letrec-any-
closure`, `saffron-variadic-dynamic-call`, `r7rs-control`, and the four
r7rs fixtures that carried `requires.interp-only` (`r7rs-core-forms-interp`,
`r7rs-named-let-sum`, `r7rs-syntax-rules-do`, `r7rs-numbers-values`), which
run compiled now:

- (1) the letrec placeholder in a dynamic file is `any`, not `int`, and an
  unannotated lambda init is pinned `: any` to match (elab_forms.c
  `elab_letrec`); a capturing closure's VALUE type carries its rest marker
  (elab_fns.c, `clo_ty`).
- (2) every Scheme lambda is lowered `: any` (scheme_lower.c), so a
  nil-tailed body is boxed as `(fn [any] : any)` and the all-`any` call site
  admits it.
- (2b) a fn type's `any` box id spells its rest slot (`tur_fn_type_key`,
  `(fn [& any] : any)`); the emitter registers every boxed variadic with its
  fixed count (`emit_any_type_id` -> `__tur_dyn_reg_variadic`), the dynamic
  call and the T6 trampoline pack the surplus arguments into the `(Cons
  any)` chain and call through the variadic signature (`__tur_dyn_call_var`),
  the fat shim types the rest slot as the chain pointer, and the H8 adaptor
  declines a variadic (it would have called it with a bare word).  The
  interpreter packs at its `EX_DYN_CALL` in the `make-struct Cons`
  representation a static site builds.
- Section 3 was misdiagnosed: the crash is `list-length` (stdlib/list.tur,
  inline C over 8-byte-head cells) walking a `(Cons any)` chain whose heads
  are 16-byte boxes -- not the boxing of the rest list, which is fine.  That
  is its own open report, docs/reported/list-length-on-cons-any-segfaults.md.

The original report follows.

---

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

**2b (found landing R5, and this face is on BOTH back ends): a VARIADIC
callee reached through `apply`.** R5 gives `+`, `max`, `<`, ... value-position
procedures with a `& xs : any` rest parameter, so `(apply + '(1 2 3))` is a
dynamic call of a variadic. Compiled it is the panic above; interpreted it is
`eval: arity mismatch: __fn_2523 expects 2 args, got 3` -- the H8 outbound
adaptor that boxes a function into `any` is synthesized with the callee's
declared parameter count (fixed + one rest slot) and knows nothing about
packing surplus arguments into the rest chain. A rest-formal lambda has the
same shape: `(define f (lambda xs (length xs)))` then `(apply f '(1 2 3 4))`
fails identically on both back ends while `(f 1 2 3)` works. The fix is in
the adaptor (elab_fns.c H8) or the dynamic-call helpers, not in the
interpreter's closure-call arity check: packing there was tried and never
reached, because the adaptor intercepts first.

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
  `r7rs-named-let-sum` carry `requires.interp-only` (owned by
  `tests/run-turi.sh`); drop the markers when the repros above compile, and
  delete this report.
