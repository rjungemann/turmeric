# eval-s4 fixture uses non-existent try/catch/throw exception syntax

**Summary:** `tests/turi/eval-s4.tur` (driven by `tests/turi/eval-s4.sh`, ctest
target `tur_eval_s4`) tests exceptions with `(try ... (throw "x") (catch [e] ...))`.
That form is not in the language -- both the interpreter *and* the compiler
reject `throw` as an unknown operator. The current mechanism is
`catch-unwind` (returns `Result`) + `panic`. **Severity: low** (stale test
fixture; not a compiler defect).

## Repro

```sh
./build/tur eval '(try (throw "x") (catch [e] 42))'
#   warning: unknown name 'throw'; error: unbound symbol 'e'
echo '(defn main [] : int (try (throw "o") (catch [e] 42)))' > /tmp/t.tur
./build/tur build /tmp/t.tur -o /tmp/t
#   error: unknown function or operator 'throw'
```

The fixture's `test-struct` (returns 7) is fine; only the `throw`/`catch`
tests (expecting 42 and 99) fail, because the syntax does not exist.

## Root cause

The fixture predates the panic/`catch-unwind` exception model (see
`tests/fixtures/panic-catch-unwind-basic/input.tur`). There is no `throw`,
`try`, or `(catch [e] ...)` binding form.

Complication for a rewrite: the REPL `:reload` path (which `eval-s4.sh` drives)
does not have the `Result` predicates (`ok?` / `err?`) bound -- they warn
"unknown name" and the subsequent `if` fails typecheck. So a straight rewrite to
`(if (ok? (catch-unwind ...)) ...)` does not run under `:reload` either; the
Result-inspection prelude gap must be closed first, or the fixture must inspect
the `catch-unwind` result some other way.

## Fix directions

1. Rewrite `eval-s4.tur` to the current idiom (`catch-unwind` + `panic`) *and*
   ensure the REPL `:reload` prelude binds the Result predicates it needs
   (`ok?`/`err?`), or inspect the result via a form that is already bound.
2. Update `eval-s4.sh`'s expected `42`/`99` to whatever the rewritten fixture
   prints.
