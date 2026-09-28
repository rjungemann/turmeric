# `#lang r7rs`: a call with too few arguments returns a procedure instead of raising

**RESOLVED 2026-09-27.** A call with the wrong number of arguments raises an
error object on both back ends. Pinned by
`tests/fixtures/r7rs-too-few-arguments`. SRFI 41's suite now passes in full
(187 on both back ends; floor raised). The rest of this file is the original
report.

## Fix

- **A known procedure** (`elab_call_fn_inner`, the partial-application
  branch). In user Scheme source (`scheme_span_is_user_source`: `#lang r7rs`
  outside the Turmeric-shaped prelude files, which keep currying) an
  under-saturated call elaborates as `(do arg... (r7rs-signal__ "msg"))`.
  The supplied arguments still run, in order, and then the error is raised:
  "g: too few arguments (expects 2, got 1)", or "expects at least" for a
  variadic procedure. The name is the one the program wrote
  (`scheme_source_name` undoes the prelude's `r7rs-` spelling and the
  lowering's `__v<N>` / `--user` respellings).
- **A procedure reached through a variable**, called with too few or too many
  arguments. The compiled dynamic call's check (`__tur_dyn_call_arity`)
  panicked "cannot call this function here", and the interpreter returned
  "eval: arity mismatch", which ended the program. In a Scheme program both
  now raise "wrong number of arguments (N given)" through the hook from
  r7rs-type-errors-are-uncatchable-panics. The compiled check cannot see the
  callee's own arity, so neither back end names it. Calling a value that is
  not a procedure raises "not a procedure" with the value as its irritant.

Too many arguments to a KNOWN procedure was still a compile-time refusal
here; it raises too since
[r7rs-dead-mistyped-call-refused-at-compile-time](r7rs-dead-mistyped-call-refused-at-compile-time.md)
(resolved 2026-09-28).

---

**Severity:** medium. R7RS makes calling a procedure with fewer arguments
than it requires an error; here the call is a Turmeric partial application
and returns a procedure, so the program goes on with a wrong value:

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (f a . rest) a)
(write (guard (e (#t 'raised)) (f)))   ; writes #<procedure>, not raised
```

Filed 2026-09-27 while landing r7rs-srfi-plan S7: SRFI 41's suite tests
`(test-error (stream-zip))` and `(stream-for-each proc)` with no stream, and
both "pass" through without an error on both back ends. A call to a known
procedure with too many or wrong-typed arguments is refused at compile time
instead ([r7rs-dead-mistyped-call-refused-at-compile-time](r7rs-dead-mistyped-call-refused-at-compile-time.md)),
which is how `(test-error (stream-map odd?))` fails to build compiled.

## Root cause

A Scheme `define` lowers to a Turmeric `defn`, and Turmeric curries a call
that supplies fewer than the required positional parameters (CLAUDE.md,
"Not auto-curried": a variadic defn can still be under-saturated up to its
required parameters, returning a closure). The Scheme lowering does not opt
its procedures out.

## Fix directions

- In a `#lang r7rs` file, lower an under-saturated call to a known procedure
  to a raise of an error object ("f: expects at least 1 argument, got 0"),
  and have the dynamic call path check the count against the procedure's
  arity before it builds a partial application.
- A fixture: `guard` catches the too-few call on both back ends, and
  SRFI 41's two `test-error` cases start passing (raise its floor).
