# `tur repl --lang r7rs` echoes multiple values as `#<R7rsValues>`

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
