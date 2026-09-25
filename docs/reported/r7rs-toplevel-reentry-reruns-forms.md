# `#lang r7rs`: re-entering a continuation at top level re-runs the forms after it

**Severity:** low-medium. A behavior change from r7rs-lang-plan T5, not a
crash, and R7RS does not pin it down. But it differs from what chibi and
Racket do, and the obvious test of re-entry after return no longer ends.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define saved #f)
(display (call/cc (lambda (k) (set! saved k) 1))) (newline)
(saved 2)
(display "not reached") (newline)
```

```
$ tur run p.tur        # and tur --interpret
1
2
2
2
...                    # forever
```

chibi and Racket print `1`, `2`, `not reached`. chibi's loader reads a
program's forms one at a time. Racket puts a prompt around each module-level
form. Either way, re-entering form 2's continuation from form 3 finishes form
2 and then carries on after form 3. Until T5 this program was the named error
"continuation invoked after its call/cc prompt returned". That is what
`tests/fixtures/r7rs-continuation-after-return` used to pin; it now counts
its re-entries so that it ends.

## Root cause

A T5 continuation is a copy of the C stack from the `call/cc` up to the
thread's stack base. That copy includes compiled `main`'s frame, whose
top-level forms are consecutive statements. Under `--interpret` it includes
the interpreter's loop over the program's forms, and eval.c
`turi_cont_state_capture` puts the loop's work stack back as well. So the
continuation is the rest of the whole program, and `(saved 2)` in form 3
resumes form 2 and then runs form 3 again.

Code: `r7rs-cont-capture__` (stdlib/r7rs/prelude.tur, `r7k_stack_base`)
and `native_r7rs_cont_capture` (src/turi/interpreter_natives.c).

## Fix directions

Delimit each top-level form the Racket way, with a prompt per form:

- The Scheme lowering wraps each top-level expression in a prelude
  `r7rs-toplevel__` call that records the stack base for continuations
  captured inside it.
- A capture then copies only up to that form's base.
- A re-entry from another form would splice the copied frames under the
  CURRENT form's prompt. Compiled, that frame sits at the same address, since
  `main` calls the prompt at one stack depth for every form. Under the
  interpreter, the loop over the forms would have to leave the prompt at a
  fixed depth too.
- A top-level `define` initializer also runs before the program's
  expressions on the compiled back end
  ([toplevel-def-initializers-run-before-toplevel-expressions](toplevel-def-initializers-run-before-toplevel-expressions.md)).
  Any per-form prompt has to follow the order the forms actually run in.
