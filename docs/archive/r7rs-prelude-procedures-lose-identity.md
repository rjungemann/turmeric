# `#lang r7rs`: `(eqv? car car)` is `#f` -- a typed prelude procedure has a new identity at every reference

**RESOLVED 2026-09-27**, by the first fix direction: one adaptor per
function. `(eqv? car car)`, `(assv car table)` and the rest of the repro
answer `(#t #t #t car vref caddr f #t)` on both back ends, and in a library
and its importer alike. Pinned by `tests/fixtures/r7rs-procedure-identity`
and `run-r7rs-import.sh`'s `library-shares-procedure-identity`. SRFI 17 is
unblocked. The rest of this file is the original report.

## Fix

The hook the report looked for already existed: the adaptor is a
non-capturing lambda, which `elab_fn` lifts to a file-scope function
(`elab_register_file_def`) and returns as an `EX_VAR` of it. So
`saffron_dyn_fn_adaptor` now caches that lifted binding on the wrapped
function's `Binding` (`any_adaptor`) the first time it boxes the function.
Every later boxing returns a fresh `EX_VAR` of the same binding, and the
emitter's static fat box for a global function is then the procedure's one
identity.

- **Across modules.** The prelude's bindings are shared by every module of
  a compile. Unless this is separate compilation, all modules' file defs land
  in one translation unit, so a library and the program share the adaptor.
  Under separate compilation each module makes its own, since the adaptor is
  a `static` function of the TU that made it (`any_adaptor_module`).
- **A local alias** of a global function (`(let ((g car)) ...)`,
  `widen_fn_alias`) is boxed through the global's adaptor.
- **The interpreter** re-homed every reference to a lifted lambda onto the
  referencing frame, which made each one a new closure. A lambda that names
  a frame-local function needs that. The shared adaptor calls only a global,
  so it is marked `is_shared_any_adaptor` and not re-homed.

SRFI 69 keeps its case-folding default hash: it is right for all five
standard equivalences, so nothing there changes.

---

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
