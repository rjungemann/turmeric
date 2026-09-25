# Top-level `def` initializers run before every top-level expression (compiled)

> **RESOLVED 2026-09-25.** Every top-level form runs in source order on
> the compiled back end, as it always did under the interpreter. Two
> pieces: (1) with no user `main`, a top-level `def`'s initializer is a
> statement of the synthesized `int main()` at its source position,
> interleaved with the top-level expressions (`emit_module.c`, the EX_DEF
> arm of the file-scope pass; `__tur_module_def_init` keeps only the
> user-has-main case), and the synthesized-main fold (`elab_toplevel.c`)
> steps aside for a `def` after a statement whose initializer is a call, so
> that path is the one such a program takes; (2) a `#lang r7rs` program with
> imports lowers to a module whose `def` initializers all run before its
> body, so the Scheme lowering declares a `define` after the first
> expression unset (`(def ^mut x : any (r7rs-void))`) and assigns it in the
> body (`(set! x init)`, in order with its neighbours). Pinned by
> `tests/fixtures/toplevel-def-init-order` (both back ends, via
> `run-turi.sh` too) and `tests/fixtures/r7rs-toplevel-order`. The repro
> below prints `one two three 2` compiled.

**Severity:** medium. A compiled/interpreted divergence in evaluation ORDER,
silent on both sides. It matters as soon as an initializer has an effect --
under `#lang r7rs` (r7rs-lang-plan R8) that is any `(define p (open-...-file
...))`, `(define x (read))` after a prompt, or `(define r (begin (display ...)
...))`. R7RS 5.1 evaluates a program's top-level forms in order.

## Repro

```turmeric
(println "one")
(def x (do (println "two") 2))
(println "three")
(println x)
```

```
$ tur run order.tur          # compiled
two
one
three
2
$ tur --interpret order.tur  # interpreted
one
two
three
2
```

The same holds under `#lang saffron` and `#lang r7rs`. Found at R8: a Scheme
program that wrote a file with a top-level expression and then did
`(define bp (open-binary-output-file path))` opened (and truncated) the file
before the earlier write ran.

## Root cause

A top-level `def` with a non-constant initializer is filled in
`__tur_static_init()`, which `main` calls first (emit_module.c, the
`__tur_static_init` band); the top-level expressions are emitted into `main`
after it. So every initializer runs before the first top-level expression,
whatever their source order. The interpreter evaluates forms in order.

## Fix directions

- Emit a non-constant initializer at its source position inside `main`,
  interleaved with the top-level expressions, keeping `__tur_static_init` for
  constant and stdlib/prelude initializers. The risk to measure first: a
  program that calls, from a top-level expression, a function reading a
  global defined LATER in the file works today and would read an
  uninitialized global.
- A narrower cut: interleave only in a file with top-level expressions and
  only for `def`s that FOLLOW the first expression.

Until then, the R8 fixtures keep effectful initializers out of top-level
`define`s.
