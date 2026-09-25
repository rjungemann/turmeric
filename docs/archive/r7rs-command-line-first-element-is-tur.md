# `#lang r7rs`: `(command-line)` starts with `"tur"`, not the program's path

**RESOLVED 2026-09-25, archived.** A new pre-declared global `*argv0*`
(`:cstr`, backed by `g_tur_argv0`) carries the program's own name: every
emitted `main` sets it from `argv[0]`, the interpreter from the script path,
and `r7rs-command-line` conses it in place of `"tur"`. Pinned by
`tests/fixtures/argv0-global` (Turmeric, both back ends) and the
`command-line` line of `tests/fixtures/r7rs-system-libraries`. Original
report follows.


**Severity:** low. Both back ends. R7RS 6.14 says the first element of
`(command-line)` is the command name, implementation-dependent, and every
other implementation puts `argv[0]` (chibi, Guile, Chicken, Racket) or the
script path there. Here it is the constant `"tur"`, whether the program was
compiled to `prog` or run under `--interpret`, so a program cannot find its
own binary or script (a usage message, a relaunch, a sibling data file).
Documented in docs/guides/r7rs-guide.md ("Where it differs") and
r7rs-lang-plan 9.3. chibi's one test only asks for a list.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context))
(write (command-line))
```

```
$ tur build cmdline.tur -o cmdline && ./cmdline a b
("tur" "a" "b")                    ; want ("./cmdline" "a" "b")
$ tur --interpret cmdline.tur a b
("tur" "a" "b")                    ; want ("cmdline.tur" "a" "b"), or tur's argv[0]
```

(`tur run cmdline.tur -- a b` forwards the arguments the same way; without
the `--` they are taken as `tur run`'s own.)

Measured 2026-09-25 against `./build/tur` v0.51.0 (Debug).

## Root cause

`r7rs-command-line` (stdlib/r7rs/process-context.tur:30-33) conses the
literal `"tur"` onto `*args*`, because `*args*` does not carry the command
name: the emitted `main` builds it from `argv[1..argc-1]`
(src/compiler/emit_module.c:17840-17849) and drops `argv[0]`, and the
interpreter builds it from the arguments after the script (src/main.c:8103-
8116). Nothing in the runtime records `argv[0]`.

## Fix directions

- A runtime global beside `g_tur_args` -- `g_tur_argv0`, a `cstr` -- set
  in the emitted `main` from `argv[0]` and by the interpreter from the
  script path (or `tur`'s own `argv[0]`, which is what a Scheme program run
  as `tur --interpret x.scm` most nearly is; either is defensible, pick one
  and say so in the guide). Expose it to the stdlib as `*argv0*` or a
  `(program-name)` native so nothing has to read `g_tur_args` raw
  (CLAUDE.md's CLI-argument rule).
- `r7rs-command-line` conses that instead of `"tur"`.
- Turmeric's `*args*` keeps its meaning (arguments only); this adds a
  second value, it does not change the list.
