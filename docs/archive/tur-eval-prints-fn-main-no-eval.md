# `tur eval '<expr>'` prints `#<fn main>` and never evaluates

**Status: RESOLVED (2026-07-21).** Root cause was the top-level statement ->
synthesized-`main` fold in `src/compiler/elab_toplevel.c` (`elaborate_program`),
which graduated to default-on with the `cps-tramp-resume` experiment on
2026-07-19 -- exactly the v0.30.0 regression this report suspected. The fold is a
**compile-only** CPS/DK-backend transform: it rewrites a bare top-level statement
into `(defn main [] : int (do <stmt> 0))` so a top-level effect handler reaches
the CPS classifier. Under the tree-walking interpreter there is no CPS/DK backend,
and the fold made `turi_eval` return the synthesized `main` closure (`#<fn main>`)
instead of the expression's value; at the entry points that don't separately call
`main` (`tur eval '<expr>'`, the non-interactive REPL) the statements never ran
at all. The interactive product REPL was unaffected because it predates the
graduation.

**Fix:** gate the fold on `!g_interpret_mode` (one condition added at the fold's
`if`), so the interpreter leaves top-level forms in place and evaluates each
directly in `turi_eval_impl`'s loop -- side effects run and the last expression's
value is returned. Verified: `tur eval '(+ 2 3)'` -> `5`,
`tur eval '(println (+ 2 3))'` -> `5`, piped `tur repl` -> `=> 5` (+ side
effects), bare atom / `(do ...)` / string / `(def ..)` all correct, and
`tur interpret <file>` parity preserved (bare-statement file still runs; explicit
`main` still invoked). `tests/run-flags.sh` eval tests (`eval-basic`,
`eval-nil-silent`, `eval-error`, `eval-file`) -- red before, now green (78/0);
full suite 2246 passed, 0 failed. The compiled-path fold is unchanged
(`g_interpret_mode` is false there), so CPS/DK top-level-handle lowering keeps its
byte-identical codegen.

---

**Severity:** medium -- the documented one-shot eval path is non-functional;
non-interactive `tur repl` shows the same symptom. Interactive/product REPL
(Trowel, which bundles v0.29.1) appears to work, so this may be a v0.30.0
regression or specific to the non-interactive path.

## Summary

`tur eval '<expr>'` (help: "evaluate an inline expression") compiles a wrapper
but prints the wrapper function object `#<fn main>` instead of evaluating the
expression. It does not even run side effects, so nothing is actually
evaluated.

## Repro (v0.30.0, `./build/tur`)

```sh
$ ./build/tur eval '(+ 2 3)'
#<fn main>                       # expected: 5

$ ./build/tur eval '(println (+ 2 3))'
#<fn main>                       # expected: 5  (side effect never runs)
```

Control -- the interpreter on the same expression works:

```sh
$ printf '(println (+ 2 3))\n' > /tmp/ctl.tur
$ ./build/tur interpret /tmp/ctl.tur
5
```

Non-interactive `tur repl` (piped stdin) shows the same shape: every
non-`defn` line prints `=> #<fn main>` rather than its value; a `defn` line
prints `=> #<fn NAME>` (which is at least plausible). `map` in a piped REPL
line also warns `TUR-W0040: unknown name 'map'` (no default stdlib import),
which is expected for the piped path but worth noting for anyone testing REPL
snippets.

## Expected

`tur eval '(+ 2 3)'` prints `5`; `tur eval '(println (+ 2 3))'` prints `5` as a
side effect. The eval/REPL value-display path should print the evaluated result
of the top-level expression, not the synthesized `main` closure.

## Root cause (direction, not confirmed)

The eval/repl entry point looks like it wraps the input expression in a
synthesized `main` and then prints/returns that function value rather than
invoking it and printing the call result. Look at the `tur eval` subcommand
handler and the REPL's non-interactive read/eval/print loop (the wrap-in-`main`
codegen path) in `src/cli/` -- the fix is to actually call the wrapper and
show its return value (using the compiler's value-printer), matching what
`tur interpret` does.

## Impact / how it was found

Found while verifying Turmeric snippets for the `turmeric-lang.com/trowel`
landing page. It means REPL transcripts cannot be checked via `tur eval` /
piped `tur repl` on this build; the page's REPL depiction was reconciled
against `tur interpret` output plus the product screenshot instead.
