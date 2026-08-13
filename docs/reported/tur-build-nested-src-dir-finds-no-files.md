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
