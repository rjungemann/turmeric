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
| `(scheme char)` | complete; case mapping is ASCII |
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

## Numbers

An exact integer is a 64-bit integer, and an inexact real is a double:

```scheme
(write (list (/ 7 2) (/ 6 2) (exact->inexact 3) (+ 7.1 0.25) (expt 2 62)))
; (3.5 3 3.0 7.35 4611686018427387904)
```

There are no exact rationals and no bignums yet. So `(/ 7 2)` is the inexact
3.5 rather than 7/2, a literal like `1/2` is refused, and an exact result
outside 64 bits **stops the program** with a message naming the rule. It is
never a silently wrapped number, but it is a panic, not a condition `guard`
can catch.

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

The `tmp` in the template cannot capture a `tmp` at the use site. The one
known gap is the other direction: a free identifier in a template that the use
site shadows resolves to the use site's binding. The plan tracks it as a
named failing test.

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
signature. `tests/run-r7rs-import.sh` pins both directions on both back ends.

## Where it differs from R7RS

- **Strings are immutable.** `string-set!`, `string-fill!` and `string-copy!`
  are refused with the reason. Every other string procedure is there.
- **No exact rationals or bignums.** See Numbers above.
- **`call/cc` is an escape only.** A continuation cannot be re-entered after
  its `call/cc` returns.
- **`apply` and dynamic calls take at most four arguments.**
- **Case mapping is ASCII.** A non-ASCII character maps to itself.
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

## See also

- [saffron-guide.md](saffron-guide.md) -- the dynamically typed Turmeric
  dialect whose substrate `#lang r7rs` shares.
- [syntax-guide.md](syntax-guide.md) -- the `#lang` line and every base dialect.
- [docs/upcoming/r7rs-lang-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/r7rs-lang-plan.md)
  -- the plan, stage by stage, with what shipped.
