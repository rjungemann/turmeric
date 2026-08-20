# Quickstart tutorial stack teaches an Option/Result/for/struct API that does not exist

**Severity: high** -- the interactive `:tutorial quickstart` fails at the real
REPL; quickstart.md and repl-tutorial.md walk new users into unknown-function
errors on their first session. Found in the 2026-08-20 docs audit.

## Repro

```
tur repl
(option-some 42)        ; unknown function
(for i 0 5 (println i)) ; `for` is the monadic comprehension (for [x seq] body),
                        ; not a counted loop (stdlib/macros.tur:64, repl.c:752)
(Point-x p)             ; no StructName-field accessors are generated; field
                        ; access is (.x p)
```

## Root cause

Tutorial content was authored against a planned API that never shipped. The
real names are `some`/`none`/`some?`/`option-unwrap`/`option-unwrap-or`
(src/turi/interpreter_natives.c:2972-2988, stdlib/option.tur) and
`ok`/`err`/`ok?`/`err?`/`result-unwrap-or` (:2994-3001). There are no
`option-some*` names and **no `none?` predicate at all**.

## Fix direction

Either add the aliases + a `none?` predicate + a counted-for form, or rewrite
the content onto the real API. The content lives in four coupled files that
must change together: `docs/guides/repl-tutorial.md` (steps 8-13, 16),
`docs/guides/quickstart.md` (Option/Result, `for`, Structs sections),
`docs/guides/quickstart-tutorial-plan.md`, and `tutorials/quickstart.yaml`.

## Guides to update when fixed

- docs/guides/repl-tutorial.md
- docs/guides/quickstart.md
- docs/guides/quickstart-tutorial-plan.md
