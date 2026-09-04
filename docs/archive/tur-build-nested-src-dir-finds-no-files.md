# `tur build <dir>` / `tur test <dir>` do not recurse, so a nested `src/<pkg>/` layout reports "no .tur files found"

**Severity:** low-medium. No miscompilation -- the command fails loudly with
exit 1. But the message is actively wrong (the files are right there), and it
fires on the invocation the compiler's *own* `module not found` hint tells you
to run, so the recommended recovery from one confusing error is a second
confusing error.

**Status:** open. Found 2026-08-13 while verifying the fix for
[manifest-read-failure-degrades-to-module-not-found](../archive/manifest-read-failure-degrades-to-module-not-found.md).
Unrelated to that report -- it reproduces with a perfectly valid manifest.

## Repro

A spice whose modules live one directory down, which is the layout
`:exports "demo/lib"` implies and the one the guides use:

```
nested/build.tur          (defpackage demo :name "demo" :version "0.1.0"
                             :exports #map{ "demo/lib" ["answer"] })
nested/src/demo/lib.tur   (defmodule demo/lib (export answer)
                             (defn answer [] : int 42))
```

```
$ cd nested
$ tur build src/
tur: no .tur files found in 'src/'          # exit 1

$ tur build .                                # exit 0 -- builds fine
```

`src/demo/lib.tur` plainly exists. Flatten the layout and the same command
works:

```
$ touch src/top.tur                          # any .tur directly in src/
$ tur build src/
...                                          # descends, compiles, gets to link
```

So the discriminator is **nesting depth**, not content.

## Root cause

Two collectors, and the non-project path uses the flat one.

- `collect_tur_files` (`src/main.c:2880`) is a single `readdir` loop with no
  recursion -- it only ever sees `.tur` files sitting *directly* in `dir`.
- `collect_tur_recursive` (`src/main.c:2923`) is the recursive one, and it
  exists and works.
- Project mode uses the recursive one: `collect_project_src_files`
  (`:2955`) calls `collect_tur_recursive(src_dir, ...)` when `<root>/src`
  is a directory. That is why `tur build .` succeeds.
- The bare-directory commands use the flat one:
  `cmd_test` (`:4329`), and the two other `no .tur files found` sites at
  `:4511` and `:5336`.

So `tur build src/` is not project mode -- `src/` holds no `build.tur` -- and
falls into the flat collector.

## Why it matters more than it looks

`elab_module.c` (the `module not found` diagnostic) recommends this exact
invocation:

```
  hint: this looks like an intra-spice import.
        try `tur check -I src <file>` from the spice root,
        or build the whole spice with `tur build src/`
```

For any spice using the nested layout -- which is the normal one -- following
that hint produces `no .tur files found in 'src/'`. A user reasonably concludes
their tree is malformed.

It also contradicts the documented descent behaviour. `CLAUDE.md` says project
mode descends into `src/` "recursively, including nested `src/<pkg>/`", which is
true of `tur build <root>` and false of `tur build <root>/src`. Two spellings of
"build this spice", one recursive and one not, with nothing in the output to
suggest which you got.

`tur test <dir>` shares the collector (`cmd_test`, `:4329`), so a `tests/` tree
organised into subdirectories tests only the top level. That failure is quieter:
if *some* tests sit directly in `tests/`, the subdirectory ones are skipped with
no message at all and the summary still reads green.

## Fix directions

1. Point the bare-directory commands at `collect_tur_recursive`, matching
   project mode. Mechanically a one-line swap at each of the three sites.
2. Weigh the blast radius on `tur test <dir>` first -- today it is
   top-level-only, so recursing would start running test files that currently
   never run. That is almost certainly the intent, but it is a behaviour change
   and may light up test trees that have been quietly half-covered. Worth
   checking `../turmeric-spices/` before flipping it.
3. Whichever way 2 goes, the "no .tur files found" message should say whether
   the scan was recursive, and mention `tur build <spice-root>` when the dir it
   was handed sits under one. A message that names its own search strategy would
   have made this a five-second diagnosis.
4. If 1 lands, revisit the `tur build src/` hint text in `elab_module.c` -- it
   becomes correct, but only then.

## References

- `src/main.c:2880` -- `collect_tur_files` (flat)
- `src/main.c:2923` -- `collect_tur_recursive` (recursive, unused by these paths)
- `src/main.c:2955` -- `collect_project_src_files`, which picks the right one
- `src/main.c:4329`, `:4511`, `:5336` -- the three `no .tur files found` sites
- `src/compiler/elab_module.c` -- the hint recommending `tur build src/`

## Resolution (2026-08-13)

Fix directions 1 and 2 landed, plus a second half the report did not identify.

### The blast radius (direction 2) is nil in this tree

The report says to weigh `tur test <dir>` first, since recursing would start
running test files that currently never run. Checked: every case under
`tests/cli/` is flat, and the sibling `../turmeric-spices/` checkout is absent
here so its trees could not be inspected. Nothing in this repo changes behaviour
except by finding more files, which is the point. Flipped all three commands --
`cmd_test`, `cmd_check_dir`, `cmd_build_multi` -- to a `collect_tur_files_deep`
wrapper over the existing `collect_tur_recursive`, which already skips dot
subtrees and manifest filenames.

### Finding the files was only half of it

Not in the report, and it would have shipped as a half fix: with the walk
recursive, `tur build src/` collected `src/demo/lib.tur` and `src/app/main.tur`
and then failed with **`module 'demo/lib' not found`**. The bare-directory build
passed no include path at all, so the sibling that does `(import demo/lib)` had
nothing to resolve against. `dir` now goes on the include path as its own module
root, which is the bare-directory equivalent of what project mode already does.

That matters for the report's own argument: the value of fixing this is that the
`module not found` hint recommends `tur build src/`, and a `tur build src/` that
finds the files and then cannot link them still does not honour the
recommendation.

Verified end to end -- the nested two-module spice builds and prints `42`.

### Directions 3 and 4, deliberately not done

- **3 (make the message name its search strategy).** Moot: the message no longer
  fires for this shape, and inventing wording for a case that now works would be
  speculative. Worth revisiting if some other empty-directory case shows up.
- **4 (revisit the `tur build src/` hint text).** The report says it "becomes
  correct, but only then" -- it has, and it needs no edit.

### Coverage

Four cases, all verified to fail against a deliberately-reverted build:

- `tests/cli/tur-test-nested-dirs` -- a `cases/` tree with files at the top, one
  level down, and two levels down; `3 tests, 3 passed` where the flat collector
  saw one. Two levels rather than one, so a one-level-deep walk would not pass
  it either.
- `tests/spice-resolver-tests.sh` gains three: `tur check src/` exits 0 on the
  nested layout; the `no .tur files found` string is **absent** (asserted
  separately from the exit code, so a future regression names itself); and
  `tur build src/ -o out` produces a binary printing `42`, which is the half
  that fails if only the collector is fixed. The fixture's `main.tur` imports
  across the nesting for exactly that reason -- a case with no cross-file import
  would pass on a half fix.

### Verification

`tests/run.sh`: 2596 passed, 0 failed. `tests/run-cli.sh`: 4 passed, 0 failed.
`tests/spice-resolver-tests.sh`: 76 passed, 0 failed. The 16 CLI/build/spice/
repl ctest targets pass.
