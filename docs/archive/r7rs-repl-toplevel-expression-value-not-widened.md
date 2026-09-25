# `tur repl --lang r7rs` prints a top-level `let`'s vector as a number

**RESOLVED 2026-09-25, archived.** At the prompt (a synthetic `<eval>`
source, the user's lines after the pinned preload) the Scheme lowering
passes a top-level expression through `r7rs-repl-value__`, an identity
with an `any` parameter, so its value is widened the way a procedure's
result is and `write` reads it as a Scheme value. A program's top-level
expressions, and a Turmeric form typed at the prompt, are left alone.
Pinned by `tests/fixtures/r7rs-repl-echo-widened` (a hook fixture driving
`tur repl --lang r7rs`). Original report follows.

**Severity:** low-medium. The R7RS prompt echoes the wrong value for any
top-level expression whose elaborated type is a collection or other
representation the prelude's `write` cannot read as a Scheme value. Programs
(both back ends) are unaffected; the prompt, and any embedder that reads a
`turi_eval` result of an R7RS session, is.

## Repro

```sh
$ printf '(vector 1 2)\n(let ((v (vector 1 2))) v)\n(define (g) (let ((v (vector 1 2))) v))\n(g)\n' \
    | tur repl --lang r7rs
=> #(1 2)
=> 88167089277264          # want #(1 2)
=> #<procedure>
=> #(1 2)
```

`tur --interpret` and `tur run` print `#(1 2)` for the same `let` inside a
`(write ...)`.

## Root cause

A top-level expression's value is its ELABORATED type's representation. A
`let` whose body is a vector has the bare `(Vec any)` type, and its value is a
`TURI_INT` holding the pointer. A procedure's result is widened to `any` at
its return, so `(g)` is a boxed value `write` recognizes; the top-level `let`
is never widened. The REPL's R7RS echo (`src/turi/repl.c`, the
`env->lang == LANG_R7RS` branch) hands the raw value to `r7rs-write`, which
prints the integer.

## Fix directions

- Widen the value of a top-level expression to `any` in an R7RS session
  before echoing it -- the same move r7rs-lang-plan T4 makes for `eval`,
  whose embedded evaluator wraps every expression in
  `(r7rs-bridge-value__ ...)` (stdlib/r7rs/read.tur), an identity through an
  `any` parameter (src/turi/r7rs_embed.c, `turi_r7rs_embed_eval`).
- Or have the elaborator give an R7RS top-level expression statement type
  `any`, so every consumer of `turi_eval`'s result sees the Scheme value.
