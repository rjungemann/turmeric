# eval-s4 fixture uses non-existent try/catch/throw exception syntax

**Status: RESOLVED** (rewrote the fixture onto panic/catch-unwind/defer; see
Resolution below).

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

## Resolution

Took fix direction 1, without needing to touch the REPL prelude. Rather than
inspect the `catch-unwind` `Result` (whose predicates `ok?`/`err?` are not bound
under `:reload`), the rewritten fixture observes exception behavior through
*control flow* alone, using only names the REPL already binds:

- **Test 2 (=> 42):** `(catch-unwind (fn [] : int (panic "oops")))` then return
  `42`. Reaching the `42` is itself the assertion -- an uncontained panic aborts
  the process before the function returns.
- **Test 3 (=> `defer-ran`, 99):** a thunk with a `(defer (println "defer-ran"))`
  panics; `catch-unwind` contains it while the `defer` fires during unwind. The
  `.sh` now also asserts the `defer-ran` marker, so the defer path is genuinely
  covered (not just the return value).

This keeps the fixture's stated scope -- structs, exceptions, defer -- while
exercising the real `panic`/`catch-unwind`/`defer` machinery.

- `tests/turi/eval-s4.tur`: rewritten (ASCII-clean; the old file also used
  non-ASCII `->` arrows in comments).
- `tests/turi/eval-s4.sh`: checks `7`, `42`, `defer-ran`, `99`.

Verified: `tur_eval_s4` ctest target passes.

The broader REPL prelude gap (Result/Option predicates and `Ref` are unbound
under `tur repl`, unlike `--interpret`) is real but out of scope for this
fixture and left for a separate report if it bites.
