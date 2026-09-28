# `#lang r7rs`: a mistyped primitive call is refused at compile time, even where it never runs

**RESOLVED 2026-09-28.** A statically mistyped call in `#lang r7rs` compiles,
and raises an error object if it runs, on both back ends. So does a call
with too many arguments to a known procedure. Pinned by
`tests/fixtures/r7rs-dead-mistyped-call`. The rest of this file is the
original report.

## Fix

- **A fixed-arity procedure** (`elab_call_fn_inner`, src/compiler/elab_call.c,
  just before the Saffron seam). In a Scheme file (`lang_span_is_scheme`), an
  argument whose concrete type the ordinary check refuses is widened to `any`
  (`elab_coerce_to_any`). The seam below then inserts its checked unbox, which
  in Scheme source raises "car: not a pair" with the value as the irritant
  ([r7rs-type-errors-are-uncatchable-panics](r7rs-type-errors-are-uncatchable-panics.md)).
  Saffron keeps its static refusal.
- **A variadic procedure's fixed parameters** were not checked at all, so
  `(vector-fill! 5 0)`, `(vector->list 'a)`, `(vector-copy "s")` compiled
  with a C pointer warning and crashed or answered garbage, and
  `(make-vector 2.5)` sized the vector from the float's bits (killed for
  memory under `--interpret`). In Scheme source a mismatched argument takes
  the same widen and checked cast. An exact integer into a `float` parameter
  keeps its C conversion.
- **Too many arguments to a known procedure** was Turmeric's
  over-application, "function 'f' returns any, which is not callable". In
  user Scheme source it now raises "f: too many arguments (expects 1, got 2)"
  after running the arguments, through the helper the too-few case uses
  (`scheme_arity_error`).
- **The interpreter's checked cast to `Sym`** checked nothing (the
  `default` arm of `EX_ANY_CAST` in src/turi/eval.c), so `(symbol->string
  "s")` read a string as a symbol record and crashed printing it. It now
  compares the box's name, as `is? Sym` does. Found while testing the fix;
  a value whose type is only known at run time hit it too.

What this does not change: `(string-length 7.1)` says "not a string" without
naming the procedure, because a string parameter takes its own unbox
(`r7rs_string_unbox`); that predates this fix.

---

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
- Whichever lands, the run-time failure it produces is the cast that
  [r7rs-type-errors-are-uncatchable-panics](../archive/r7rs-type-errors-are-uncatchable-panics.md)
  made raise an R7RS error object (resolved 2026-09-27), so this fix's error
  is already an R7RS one: letting the call through is all that is left.
