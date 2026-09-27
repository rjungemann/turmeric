# `#lang r7rs`: a primitive applied to the wrong type panics; `guard` cannot catch it

**RESOLVED 2026-09-27.** A value of the wrong type reaching a standard
procedure raises an R7RS error object on both back ends: `(car 5)` is
"car: not a pair" with 5 as its irritant, and `guard`, `with-exception-handler`
and SRFI 64's `test-error` catch it. Pinned by
`tests/fixtures/r7rs-type-errors-raise`. The rest of this file is the
original report.

## Fix

Three paths reached a panic, and each now reaches the prelude's
`r7rs-type-error__`, which builds the error object and raises it:

- **The seam's checked cast.** A cast the elaborator writes in Scheme source
  (`elab_any_unbox_to`, any span `lang_span_is_scheme` accepts, the prelude
  included) is marked `scheme_raise`, with the target in Scheme's words
  ("a pair", "a vector", "an exact integer") and, at a call argument, the
  procedure's Scheme name (`elab_any_cast_note_callee`, through the rename
  table: `r7rs-car` is `car`). The emitter calls
  `__tur_any_cast_check_r7(v, tag, who, what)` for it, written once per unit
  (`ensure_r7rs_cast_helper`), which on a mismatch calls the hook
  `tur_r7rs_type_error_hook`; the prelude points the hook at
  `r7rs-type-error__` at startup (`r7rs-type-error-hook-install__`, run by a
  `def`). The interpreter's `EX_ANY_CAST` calls the procedure by name. The
  interpreter also used to pass a `(Vec any)` cast unchecked, so
  `(vector-ref '() 0)` read a field of the empty list; a Scheme cast to a
  type application now compares the box's name, as `is?` does.
- **The dynamic operator.** The prelude's own numeric code runs Turmeric's
  dynamic `+` and `<` on the program's values (`(negative? "four")`), and
  those panicked "no operator for a cstr argument". The compiled
  `__tur_dyn_bad_operand` and the interpreter's dyn-op arm call the same
  hook, and still panic when there is none (a Saffron program).
- **The prelude's "it is an error" helpers.** `r7rs-fail-any__` and its
  twins, `string-set!`'s index, a mutated literal and `string-ref`'s index
  (in inline C) raise with their message. So do `+`, `-`, `*` and the five
  comparisons given a non-number: each checks its operands only after the
  int64 fast path (`r7rs-intflos?__`).

These raise through `r7rs-raise-type__`, an inline-C call into the same
hook, rather than `raise`. A direct `raise` makes the procedure that reaches
it effectful, and the effect analysis then compiles its callers to CPS. That
is why these were panics (see the note on `r7rs-exint-of__`). A call into C
is opaque to the analysis. Measured on a one-line program, the only function
that became CPS is `r7rs-type-error__` itself. `r7rs-tail-calls` passes, and
the chibi count holds at 1223 on both back ends.

A bignum passed where a Turmeric `int` is wanted says so: "vec-get: not an
exact integer in the int64 range" (`r7rs-bignum-int-seam`, which now raises
where it panicked).

The SRFI suites moved: SRFI 1 from 157 to 160, and SRFI 41 from 174 to 184
(`(car '())`, `(every odd? '(1 3 . x))`, `(stream->list "four" s)`). Their
floors are raised.

---

**Severity:** medium. `(car 5)` reached through a variable aborts the program
with a Turmeric panic. It does not raise an R7RS error that `guard` or
`with-exception-handler` can catch. chibi and Racket raise a catchable error
here, and portable code (test suites especially, SRFI 64's `test-error`
among them) relies on it.

Filed 2026-09-27 while landing r7rs-srfi-plan S2.

**Narrowed 2026-09-27 (S6).** `vector-ref` and `vector-set!` check their
index and raise an error object ("vector-ref: index out of range", the vector
and index as irritants), so `guard` and SRFI 64's `test-error` catch
`(vector-ref v 99)` -- the example SRFI 64's own meta-suite tests.  A wrong
TYPE (`(car 5)`, `(vector-ref 'x 0)`) still panics, as do the other index
checks (`string-ref`, `list-ref`, `substring`'s range -- since raised, below).

**Narrowed again 2026-09-27 (S7).** `bytevector-u8-ref` and
`bytevector-u8-set!` check their index the same way, and `bytevector`,
`make-bytevector` and `bytevector-u8-set!` raise "byte out of range 0..255"
for a byte that is not an exact integer in 0..255 (`bytevector-u8-set!` used
to store it silently).  And the start/end range check every sequence
procedure shares (`substring`, `vector->list`, `bytevector-copy!`,
`string-copy!`, ...) raises "<who>: range out of bounds" with the start and
end as irritants, where it panicked.  SRFI 4 and 66 check their elements
through the same path (`tests/fixtures/r7rs-bytevector-range-errors`).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (h x) (car x))
(write (guard (e (#t (list 'caught (error-object? e)))) (h 5)))
(newline)
```

Expected `(caught #t)`. Both back ends instead end the program:

```
panic: cast: any holds int, not R7rsPair
```

(compiled: `panic at /tmp/tur-build/..._tur.c:NNNN: cast: any holds int, not
R7rsPair`, then `Aborted`, exit 134).

## Root cause

The prelude's procedures take typed parameters (`(defn r7rs-car [p :
R7rsPair] ...)`, stdlib/r7rs/prelude.tur:116). A Scheme value arrives as
`any`, and the dynamic-file call inserts a checked cast. On a tag mismatch
that cast panics (the Saffron boundary check, r7rs-lang-plan's "checked
cast at the boundary"), and a panic is not an R7RS condition: the prelude's
`guard` and `with-exception-handler` see only `raise`d objects.

## Fix directions

- In a `#lang r7rs` file, lower the boundary cast's failure to a `raise` of
  an error object (`error-object-message` along the lines of "car: not a
  pair", with the value as an irritant) instead of a panic. The Saffron
  boundary may stay a panic; the choice is per dialect.
- Or give the prelude's hot procedures an `any` parameter and a
  `(if (pair? p) ... (error "car: not a pair" p))` guard. That is clearer per
  procedure but has to be repeated across the prelude, and it costs a branch
  where the cast was a tag compare.
- Whichever it is, a fixture: `guard` catches `(car 5)`, `(vector-ref '() 0)`
  and `(+ 'a 1)` on both back ends, and `error-object?` holds for each.
- [r7rs-dead-mistyped-call-refused-at-compile-time](r7rs-dead-mistyped-call-refused-at-compile-time.md)
  is the compile-time half: once a statically mistyped call is let through to
  run time, this is the failure it produces.
