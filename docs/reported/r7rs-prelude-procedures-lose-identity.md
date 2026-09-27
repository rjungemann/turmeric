# `#lang r7rs`: `(eqv? car car)` is `#f` -- a typed prelude procedure has a new identity at every reference

**Severity:** medium. R7RS 6.1 makes a procedure `eqv?` to itself. `(eqv? car
car)` is `#t` in chibi and Racket. Here it is `#f` on both back ends, and so
is every comparison of a standard procedure with itself: `(eq? vector-ref
vector-ref)`, and `(assv car table)` over a table keyed by `car`. A procedure
the program defines keeps its identity. So do some library procedures
(`caddr`).

This blocks SRFI 17 (r7rs-srfi-plan S2). Its `setter` is a table keyed by
procedure identity, and its standard entries are exactly these procedures:
`(setter car)` must be `set-car!`.

SRFI 69 (S4) works around it. Its reference picks a table's default hash
function by comparing the equivalence with `eq?`, `string=?`, `string-ci=?`
and the rest by `eq?`. Here the default is one hash that is right for all of
them, so `(make-hash-table string-ci=?)` works without the comparison.

Filed 2026-09-27 while landing r7rs-srfi-plan S2.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme cxr))
(define (f x) x)
(define tbl (list (cons car 'car) (cons vector-ref 'vref) (cons caddr 'caddr) (cons f 'f)))
(write (list (eqv? car car) (eq? vector-ref vector-ref) (eqv? f f)
             (let ((p (assv car tbl))) (and p (cdr p)))
             (let ((p (assv vector-ref tbl))) (and p (cdr p)))
             (let ((p (assv caddr tbl))) (and p (cdr p)))
             (let ((p (assv f tbl))) (and p (cdr p)))
             (let ((g car)) (eqv? g car))))
```

Both back ends: `(#f #f #t #f #f caddr f #f)`. Expected: `(#t #t #t car vref
caddr f #t)`.

## Root cause

`car` lowers to the prelude's typed `r7rs-car` (`(defn r7rs-car [p :
R7rsPair] : any ...)`, stdlib/r7rs/prelude.tur:116). When a typed function is
boxed as an `any`, saffron-dynamic-surface-pass H8's outbound adaptor wraps
it: `saffron_dyn_fn_adaptor` (src/compiler/elab_call.c:825) elaborates a
fresh `(fn [__da0 ...] : any (NAME __da0 ...))` at each place the value is
boxed. Two references make two lambdas, two lifted functions and two fat
boxes, so two different procedures. A function whose signature is already
all-`any` (the program's own, or `caddr`) needs no adaptor. It is boxed as
itself, through one static fat box per function, and keeps its identity.

## Fix directions

- Make the adaptor per FUNCTION rather than per site. The first time a
  binding is boxed, synthesize one global adaptor for it (a top-level `defn`
  with the all-`any` signature), and box a reference to that global at every
  site. The emitter already gives a global function one static fat box,
  which is the identity. Elaboration has no hook yet for adding a top-level
  definition from inside an expression; that is the part to build. It also
  has to be one adaptor per compile, not per module, or a library and the
  program would each make their own.
- Or, in the Scheme lowering only: a standard procedure in value position
  names a per-procedure global value (`(def r7rs-car--value (:: r7rs-car
  any))`), made once per compile the way an SRFI's definitions are spliced
  once (r7rs-srfi-plan D3). This is narrower (`#lang r7rs` only) and has the
  same once-per-compile requirement.
- A fixture: the repro above on both back ends, and SRFI 17's `(setter car)`
  once 17 lands.
