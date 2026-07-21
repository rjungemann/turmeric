# `tur eval '<expr>'` prints `#<fn main>` and never evaluates

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
