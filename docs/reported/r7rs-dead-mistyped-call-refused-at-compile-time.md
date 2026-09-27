# `#lang r7rs`: a mistyped primitive call is refused at compile time, even where it never runs

**Severity:** medium. An R7RS program that is valid, because the mistyped call
is never executed, does not build. R7RS makes `(car 5)` an error only when it
is evaluated (R7RS 1.3.2: "it is an error" describes a situation at run time).
Any code whose argument's type is statically known can hit this: a literal,
or a `let`-bound literal that a guard makes unreachable. SRFI 2's `and-let*` is
the natural way to write such a guard, which is how it was found.

Filed 2026-09-27 while landing r7rs-srfi-plan S2.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(write (let ((x #f)) (if x (car x) 0)))   ; valid: (car x) never runs
(newline)
```

Both back ends (`tur run`, `tur --interpret`):

```
error [TUR-E0001]: function 'r7rs-car' arg 1: expected R7rsPair, got bool
```

The same holds for a procedure nothing calls: `(define (g) (car 5))` is
"expected R7rsPair, got int". And through SRFI 2:
`(and-let* ((x #f) (y (car x))) y)`, which SRFI 2 says is `#f`.

## Root cause

The prelude's procedures are typed (`(defn r7rs-car [p : R7rsPair] : any
...)`, stdlib/r7rs/prelude.tur:116). A Scheme value whose static type is
`any` reaches them through a checked cast at run time. But a value whose
static type is a concrete, different type (`bool` from `#f`, `int` from `5`) is
refused by the ordinary argument check in src/compiler/elab_call.c (the
TUR-E0001 "function '%s' arg %u: expected %s, got %s" sites, :5234 and :8005).
Nothing in a dynamically typed file demotes that refusal to a run-time
failure.

## Fix directions

- In a dynamically typed file (r7rs-lang-plan's "is this file dynamically
  typed?" predicate), when a call argument's concrete static type does not
  match the parameter, widen it to `any` and let the existing checked cast
  fail at run time, rather than refusing the call. It could emit a warning
  (a Scheme compiler's "this call will always fail" note), never an error.
- Or have the Scheme lowering give every `let`-bound variable the static
  type `any`. That covers the `let` case but not a literal argument, and it
  gives up static types the rest of the file benefits from.
- Whichever lands, the run-time failure it produces is the uncatchable
  panic [r7rs-type-errors-are-uncatchable-panics](r7rs-type-errors-are-uncatchable-panics.md)
  describes. Fixing that one first makes this fix's error an R7RS one.
