# `#[used]` is not honored on the single-file / whole-program build path (`tur test`, `tur run <file>`, `tur build <file>`)

**Severity:** low-medium. A `#[used]` defn reached **only** through its raw
mangled C symbol from a sibling module that is *not* `(import)`ed was dropped at
link time when the program was built single-file / whole-program. `tur build
<project>` (separate compilation) always handled this case; only the
single-file path regressed.

**Status:** RESOLVED 2026-06-24 (v0.25.0). `cmd_build` now force-loads
`#[used]`-bearing modules found on the `-I` search path into the whole-program
TU, so the extern resolves. Regression test:
`tests/run-build-project.sh` -> `build-file-used-attr-whole-program`.

## Summary

`#[used]` (`Binding.retain_c_linkage`, `src/compiler/expr.h:106-113`) tells the
compiler to keep an unexported, Turmeric-unreachable defn alive with external C
linkage, for defns reached only via their mangled C symbol (cross-module
inline-C bridges, by-address C-ABI callbacks).

In **project builds** (`cmd_build_project`, `src/main.c`), the whole-program
single-main shortcut is correctly disqualified when any non-entry module carries
`#[used]`: `file_has_used_attr` (`src/main.c`) is consulted at the shortcut
decision, and the build falls through to separate compilation, which compiles
and links **every** project module.

In **single-file / whole-program builds** -- `tur build <file>`, `tur run
<file>`, and `tur test <dir>` (which builds each test file via the single-file
`cmd_build` path) -- there was no such fallback. The whole-program emitter
inlines only the entry module's transitive **Turmeric import closure**
(`emit_program`, `ctx.separate_compilation = false`, `emit_module.c`), so a
sibling module reached **only** through a raw `extern <mangled>` reference --
and therefore not in the import closure -- was never inlined, never emitted, and
dangled at the C link step.

Note the boundary: when the sibling module **is** in the import closure (the
normal case -- the importing module `(import)`s it and uses its public API),
the `#[used]` defn was retained even before this fix and links fine as a
same-TU `static`. The gap was specifically the *no-import* raw-extern case.

## Repro (now passing)

This is the exact shape of the canonical `#[used]` test in
`tests/run-build-project.sh` (`app/a`'s `__helper`, reached only via its mangled
symbol, with **no** `(import app/a)`):

```
/tmp/used/build.tur
  (defpackage tur-used-attr :name "tur-used-attr" :version "0.1.0"
    :exports #{ "app/main" ["main"]  "app/a" [] })

/tmp/used/src/app/a.tur
  (defmodule app/a
    (defn #[used] __helper [x : int] : int
      ```c
      return x + 1;
      ```))

/tmp/used/src/app/main.tur
  (defmodule app/main
    (defn use [x : int] : int
      ```c
      extern int64_t app__a____helper(int64_t);
      return app__a____helper(x);
      ```)
    (defn main [] : int (use 41)))
```

```sh
# project build -- separate compilation, #[used] honored: OK (always was)
tur build /tmp/used -o /tmp/used/proj && /tmp/used/proj; echo $?   # => 42

# whole-program single-file (the `tur test` / `tur run <file>` path):
# was: undefined reference to `app__a____helper'; now links and runs.
tur build /tmp/used/src/app/main.tur -I /tmp/used/src -o /tmp/used/wp \
  && /tmp/used/wp; echo $?                                          # => 42
tur run /tmp/used/src/app/main.tur -I /tmp/used/src; echo $?        # => 42
```

## Root cause

`file_has_used_attr` was only consulted by `cmd_build_project`. The single-file
entry points (`cmd_build` for `tur build <file>`; `cmd_test` -> `cmd_build`;
`tur run <file>`) never checked it, and the whole-program emitter had no notion
of "retain this module even though nothing imports it." So a `#[used]` defn in
an unimported sibling was dropped before it was ever emitted.

## Fix

Force-load `#[used]`-bearing modules into the whole-program TU on the
single-file build path. Concretely:

1. `src/main.c` -- `collect_used_attr_modules()` scans each `-I` module-search
   dir (recursively, via `collect_tur_recursive`) for `.tur` files carrying a
   `#[used]` attribute (reusing `file_has_used_attr`) and returns their
   slash-separated module names (path relative to the include dir, sans
   `.tur`), excluding the entry file's own module. `cmd_build` publishes the
   list via `used_modules_ctx_set()` around its `compile_to_c` call. Scoped to
   the build path only (not `check`/`emit-c`), so inspection output and codegen
   snapshots are unaffected. `#[used]` is rare, so the common case is a cheap
   read-and-no-match per `.tur` file; `realpath` is paid only for the few files
   that actually match.

2. `src/compiler/elab.h` / `elab_toplevel.c` -- a per-compile `UsedModulesCtx`
   (set/active accessors), mirroring the existing `Ls2ResolverCtx` idiom, so no
   `elaborate_program` signature churn. After the main elaboration pass and
   before the file-scope prepend, `elaborate_program` reads the active list and
   force-loads each module.

3. `src/compiler/elab_module.c` -- `elab_force_load_module()` wraps the existing
   (static) `elab_load_module`. Loading is idempotent (dedup by name), applies
   no `:refer` (so no module names leak into the entry's scope), and registers
   the loaded module's `EX_DEFMODULE` for file-scope emission exactly as an
   ordinary `(import ...)` would -- so the module-private `#[used]` defn is
   emitted into the single TU and the raw extern resolves. Inert under separate
   compilation (every project module is already compiled and linked there).

## Workaround (no longer required, but still idiomatic)

Follow the documented pattern: `(import)` the sibling module and reference its
defn through the module system or the `__TUR_CNAME_<name>__` splice rather than
a hand-spelled raw `extern`. With the import present the module joins the
closure on every build path. See
`docs/guides/c-integration-guide.md` ("Calling a sibling `defn` from inline C").
