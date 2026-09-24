---
title: "R7RS Scheme -- #lang r7rs"
category: Getting Started
description: "#lang r7rs runs R7RS-small Scheme on the Turmeric compiler and runtime, compiled or interpreted, and lets Scheme and Turmeric modules import each other. This guide covers getting started, what is there, how it differs from the standard, and the tooling."
---

# R7RS Scheme

`#lang r7rs` runs R7RS-small Scheme on Turmeric's compiler and runtime. A
Scheme file compiles to C like any Turmeric file, runs under the interpreter
too, and can import Turmeric modules or be imported by them.

```scheme
#lang r7rs
(import (scheme base) (scheme write))

(define (fact n)
  (if (= n 0)
    1
    (* n (fact (- n 1)))))
(display (fact 20))
(newline)
```

```sh
tur run fact.tur          # compile and run
tur --interpret fact.tur  # the tree-walking interpreter
```

The dialect is a **prototype**. The `#lang r7rs` line is its own enable, so no
`--enable=` flag is needed, but every compile prints the lifecycle warning
TUR-W0060. The stages, design decisions and known gaps live in
[docs/upcoming/r7rs-lang-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/r7rs-lang-plan.md).

The single-file examples in this guide are compiled and run, on both back
ends, by `tests/fixtures/docs-r7rs-guide-examples`; the library examples by
`tests/run-r7rs-import.sh`.

## Getting started

- **A program** is a file of top-level forms. `tur run prog.tur` builds and
  runs it; there is no `main` to write.
- **A project**: `tur init --r7rs demo` scaffolds one that builds with
  `tur build .` and tests with `tur test tests`. Add `--lib` for a
  `define-library` instead of a program.
- **The REPL**: `tur repl --lang r7rs`, or type `#lang r7rs` at any prompt.
  Results echo in Scheme's own spelling (`=> (a "b" #\c)`), and nothing is
  echoed for the unspecified value.
- **Formatting**: `tur fmt` re-indents a Scheme file and never rewrites a
  token. Each line's leading whitespace is recomputed; `#t`, `#\x`,
  `|two words|` and `#e1.5` stay exactly as written.
- **Editors**: the vim pack and the VS Code grammar highlight the Scheme
  lexemes in a `#lang r7rs` file, and the language server analyses and
  formats it.

## What is there

All of R7RS-small except the evaluator libraries:

| Library | Status |
|---|---|
| `(scheme base)` | complete, including string, bytevector and file ports |
| `(scheme case-lambda)`, `(scheme lazy)`, `(scheme inexact)` | complete |
| `(scheme char)` | complete, Unicode case mapping and classification included |
| `(scheme cxr)`, `(scheme complex)` | complete; complex numbers are reals only |
| `(scheme write)` | `write` and `display` label cycles; `write-shared`, `write-simple` |
| `(scheme read)` | `read`, datum labels and cycles included |
| `(scheme file)`, `(scheme time)`, `(scheme process-context)` | complete; loaded only when imported |
| `(scheme eval)`, `(scheme repl)`, `(scheme load)` | refused at the import, with the reason |

The core forms are all there: `define`, `lambda`, the `let` family and named
`let`, `do`, `case`, `cond` with `=>`, `when`/`unless`, `case-lambda`, the
`-values` forms, `define-record-type`, `define-library` and `import` with
`only`/`prefix`/`rename`, `cond-expand`, `syntax-rules`, `guard`,
`parameterize`, `delay`, `delay-force`, quasiquote.

## Lists, vectors, strings

```scheme
(define xs (list 3 1 2))
(write (map (lambda (x) (* x x)) xs))        ; (9 1 4)
(write (vector-map + #(1 2) #(10 20)))       ; #(11 22)
(write (string-upcase "shout"))              ; "SHOUT"
(let loop ((i 0) (acc '()))
  (if (< i 3)
    (loop (+ i 1) (cons i acc))
    (begin (write acc) (newline))))          ; (2 1 0)
```

Every Scheme procedure call is a proper tail call on both back ends, so a
named-`let` loop runs in constant space however long it runs.

A string is a sequence of characters. `string-length`, `string-ref`,
`substring` and the rest count characters, not bytes. A string a procedure
makes -- `make-string`, `string`, `string-copy`, `substring`,
`string-append`, `list->string` -- is mutable, so `string-set!`,
`string-fill!` and `string-copy!` work on it:

```scheme
(define s (string-copy "caf\xE9;"))
(string-set! s 0 #\C)
(write (list s (string-length s)))           ; ("Café" 4)
```

A string literal is immutable, which R7RS allows. Mutating one is an error
that names the fix: `string-copy` it first. So is mutating a string from
`symbol->string`, `number->string`, `read` or Turmeric.

## Numbers

An exact integer has no size limit, an exact non-integer is a ratio, and an
inexact real is a double:

```scheme
(write (list (/ 7 2) (/ 6 2) (exact->inexact 3) (+ 7.1 0.25) (expt 2 100)))
; (7/2 3 3.0 7.35 1267650600228229401496703205376)
```

An exact integer is a 64-bit int while it fits, which keeps the common case
fast. Arithmetic that leaves 64 bits continues as a **bignum**, and a result
that fits again is an int again. Nothing wraps, and nothing stops the
program. Bignums work everywhere integers do: literals, `read`,
`string->number` and `number->string` in any radix, `quotient` and the other
divisions, `gcd`, `expt`, `exact-integer-sqrt`, and `exact` of a large
double (`(exact 1e30)`). A comparison between a bignum and a double is exact,
so `(= (- (expt 2 1000) 1) (inexact (expt 2 1000)))` is `#f`.

A bignum is a Scheme value only. Passed to a Turmeric procedure that takes an
`int`, it is the import's checked cast error (`cast: any holds R7rsBig, not
int`); a procedure that takes `any` receives it as it is.

Division of exact numbers is exact: `(/ 7 2)` is the ratio 7/2, kept in
lowest terms, and `(/ 6 2)` is the integer 3. Ratios work through the whole
tower. Arithmetic and comparison are exact, and `floor`, `ceiling`,
`truncate` and `round` give exact integers (`round` takes a tie to even).
`numerator` and `denominator` give the parts. `exact` of a double gives its
exact value, so `(exact .5)` is 1/2. `(expt 2 -10)` is 1/1024, `(sqrt 4/9)`
is 2/3, and `rationalize` finds the simplest rational in an interval.
`inexact` gives the nearest double:

```scheme
(write (list (+ 1/2 1/3) (exact .5) (round 7/2) (inexact 1/3)))
; (5/6 1/2 4 0.3333333333333333)
```

`#e` reads a decimal exactly, so `#e1.2` is 6/5, not the value of the double
1.2. A ratio passed to a Turmeric `int` or `float` parameter is a checked
cast error, as a bignum is; convert it on the Scheme side with `inexact` or
`round`.

**Visible change:** before r7rs-lang-plan T2, `(/ 7 2)` was the inexact 3.5.

Numbers are read by one parser: in a source file, by `read`, and by
`string->number`. It knows the whole R7RS number syntax, so a ratio or a
complex number is one token. A complex number whose imaginary part is an
exact zero is the real it is, so `3+0i` is 3. Any other complex number is
refused, with the reason and the task in the plan that brings it. That
happens at compile time for a literal. `read` and `string->number` raise an
error a program can `guard`. The number is never split into a number and a
stray symbol, and never read as a different number:

```scheme
(write (list 10/2 #i3/2 3+0i (string->number "1e2")))   ; (5 1.5 3 100.0)
(string->number "1+2i")
; error: string->number: `1+2i`: a non-real complex number needs complex numbers ...
```

## Macros

`syntax-rules` with the full pattern language, and hygiene for the
template's own binders:

```scheme
(define-syntax swap!
  (syntax-rules ()
    ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))
(let ((p 1) (q 2))
  (swap! p q)
  (write (list p q)))                        ; (2 1)
```

The `tmp` in the template cannot capture a `tmp` at the use site. The other
direction holds too: a free identifier in a template means what it meant
where the macro was defined, however the use site binds that name.

```scheme
(define-syntax my-list (syntax-rules () ((_ x) (list x))))
(write (let ((list vector)) (my-list 1)))    ; (1)
```

A local variable shadows a keyword or a macro of the same name, as R7RS says:
`(let ((if even?)) (if 7))` calls `even?`.

## Control

```scheme
(write (call/cc (lambda (k) (+ 1 (k 42)))))  ; 42
(write (guard (e ((error-object? e) (error-object-message e)))
         (error "something broke" 'detail))) ; "something broke"
(define depth (make-parameter 0))
(write (parameterize ((depth 1)) (depth)))   ; 1
(dynamic-wind
  (lambda () (display "[in]"))
  (lambda () (display "body"))
  (lambda () (display "[out]")))             ; [in]body[out]
```

`call/cc` captures an **escape**: invoking the continuation while its
`call/cc` is still running returns from it, through any `dynamic-wind` exits.
Invoking it after the `call/cc` has returned, which would re-enter it, is a
named error. An uncaught `raise` reports on the current error port and exits
with status 70.

## Ports

String, bytevector and file ports, with the current ports as parameters:

```scheme
(let ((out (open-output-string)))
  (write '(a "b" #\c) out)
  (write (get-output-string out)))           ; "(a \"b\" #\\c)"
(let ((l (list 1 2 3)))
  (set-cdr! (cddr l) l)
  (write l))                                 ; #0=(1 2 3 . #0#)
(write (read (open-input-string "(1 (2 . 3) #(4))")))  ; (1 (2 . 3) #(4))
```

`write` and `display` label only the structure on a cycle, so they always
terminate. `write-shared` labels everything that appears twice, and
`write-simple` labels nothing.

## Libraries and Turmeric

A `define-library` compiles to a Turmeric module:

```scheme
#lang r7rs
(define-library (mylib)
  (export add)
  (import (scheme base))
  (begin
    (define (add a b) (+ a b))))
```

A Scheme program imports it as `(import (mylib))`, and a Turmeric module as
`(import mylib :refer [add])`. The library's exports are `any` on the Turmeric
side, narrowed with `cast`. In the other direction, a Scheme program reaches
Turmeric through the `(turmeric ...)` head:

```scheme
#lang r7rs
(import (scheme base) (scheme write) (turmeric stdlib/vec))
(define v (vec-new))
(vec-push! v 1)
(display (vec-len v))                        ; 1
```

Each argument crossing into a typed Turmeric function is checked against its
signature. A string crosses into a Turmeric `cstr` as a fresh UTF-8 copy.
Turmeric's strings stay immutable, so mutating the Scheme string afterwards
changes nothing on the Turmeric side. A Turmeric module `cast`ing a Scheme
library's string result to `cstr` gets the same copy.
`tests/run-r7rs-import.sh` pins both directions on both back ends.

## Where it differs from R7RS

- **String literals are immutable.** R7RS allows this. See Lists,
  vectors, strings above.
- **No complex numbers.** See Numbers above.
- **`call/cc` is an escape only.** A continuation cannot be re-entered after
  its `call/cc` returns.
- **`apply` and dynamic calls take at most four arguments.**
- **`char-ready?` and `u8-ready?` always answer `#t`.**
- **`(except ...)` in an import is refused.** A Turmeric import cannot say
  "all but these names"; the error says to list them with `(only ...)`.
- **`(scheme eval)`, `(scheme repl)`, `(scheme load)` and `include`** are
  refused at the import, with the reason: they need an evaluator at run time.
- **Compiled top-level order.** On the compiled back end a top-level
  `define` whose initializer has an effect runs before the program's
  top-level expressions. Opening a file or reading input in a top-level
  `define` is the case to watch. Put such code inside a procedure; the
  interpreter evaluates in order either way.
- **`command-line`** starts with `"tur"`, not the program's own path.

## Conformance

chibi-scheme's R7RS test suite (`tests/r7rs/chibi-r7rs-tests.scm`) runs as
the ctest target `tur_r7rs_conformance`, which reports a count rather than a
verdict:

```sh
bash tests/run-r7rs-conformance.sh      # both back ends, about two minutes
python3 tests/r7rs/run-conformance.py --backend interp --list-failures
```

**1147 of the 1216 tests** written in the suite pass, the same on the
interpreter and the compiled back end. Nearly all the rest are the
differences listed above: complex numbers (`3+4i`), `eval` and
`environment`, and re-entering a continuation.
What remains after those is two float spellings that differ from chibi's
own (`1.7976931348623157e308` rather than `e+308`; both are R7RS).

The target fails only when the count drops below its floor, so raise the
floor in `tests/run-r7rs-conformance.sh` when the count goes up.

## See also

- [saffron-guide.md](saffron-guide.md) -- the dynamically typed Turmeric
  dialect whose substrate `#lang r7rs` shares.
- [syntax-guide.md](syntax-guide.md) -- the `#lang` line and every base dialect.
- [docs/upcoming/r7rs-lang-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/r7rs-lang-plan.md)
  -- the plan, stage by stage, with what shipped.
