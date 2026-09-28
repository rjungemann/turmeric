# `tur repl --lang r7rs` echoes multiple values as `#<R7rsValues>`

**RESOLVED 2026-09-28.** The R7RS prompt echoes each value on its own `=>`
line and nothing for `(values)` (src/turi/repl.c, the `LANG_R7RS` echo). It
tests the result with the prelude's `r7rs-values?__` and walks
`r7rs-values-items__`. `_` is the first value, though Scheme source cannot
name `_` at the prompt. A definition now echoes nothing either:
`(define x 3)` printed `=> 3`, and `(define (g) ...)` or a
`define-record-type` printed `=> #<procedure>`. The evaluator records whether
a turn's last item was a `def`/`defn` (`last_result_is_def`, src/turi/env.h),
and the echo skips it. Pinned by the hook fixture
`tests/fixtures/r7rs-repl-echoes-multiple-values`;
`r7rs-repl-echo-widened` and `r7rs-repl-shadowed-name-persists` lost their
definition echoes. The rest of this file is the original report.

---

**Severity:** low. Display only; the values are right.

Filed 2026-09-26 while writing `tests/fixtures/r7rs-repl-shadowed-name-persists`
(r7rs-srfi-plan S0).

## Repro

```sh
printf '(values 1 2)\n(values)\n(exact-integer-sqrt 17)\n(values 7)\n' \
  | ./build/tur repl --lang r7rs 2>/dev/null | grep '^=>'
```

Prints:

```
=> #<R7rsValues>
=> #<R7rsValues>
=> #<R7rsValues>
=> 7
```

Expected, as chibi and Racket print them: each value on its own line (`=> 1`,
`=> 2`), and nothing for zero values.

## Root cause

The R7RS prompt echoes a turn's value through the prelude's `write`
(src/turi/repl.c, the `LANG_R7RS` branch after "r7rs-lang-plan R9: an R7RS
session echoes in Scheme's own spelling"). Any count of values but one is the
prelude's `R7rsValues` carrier (stdlib/r7rs/prelude.tur, `(defstruct
R7rsValues [items : any])`), and `write` has no case for it, so it falls
through to the struct printer.

## Fix directions

In the echo branch, test the result with the prelude's `r7rs-values?__`; when
it holds, walk `r7rs-values-items__` and write each item on its own `=> ` line
(none for an empty list). Keep `_` bound to the first value, or to the
carrier, whichever the REPL guide chooses to document. Pin with a hook fixture
next to `tests/fixtures/r7rs-repl-echo-widened`.
