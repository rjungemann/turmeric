# `tur build --shared` double-elaborated the stdlib prelude when run from a foreign cwd (broke tur_repl_spice_*) -- RESOLVED

**Severity:** high (was 5 ctest targets red: `tur_repl_spice_{load,call,reload,watch,errors}`).
**Status:** RESOLVED. Fix in `src/compiler/elab_toplevel.c` (`load_path_key`).

## Summary

`tur build --shared <root>` elaborated the auto-loaded stdlib prelude twice
into the same scope when the process's current working directory was not the
turmeric repo root, producing ~44 `extern-c: 'tur_hamt_*' is already defined`
errors from `stdlib/hamt.tur` and failing the build. From the repo root the
same command (identical absolute-path arguments) succeeded.

The REPL's spice auto-loader (`src/turi/spice_loader.c:run_build`) always spawns
`tur build --shared` with the *project* as cwd, so every `tur repl` inside a
project hit this and printed `tur repl: spice rebuild failed` -- which is what
the 5 `tur_repl_spice_*` ctest targets exercise.

## Reproduction (pre-fix)

```sh
W=$(mktemp -d); mkdir -p "$W/src" "$W/.tur-repl-cache"
printf '(defpackage p)\n' > "$W/build.tur"
printf '(defmodule m (export f) (defn f [x :int] :int (+ x 1)))\n' > "$W/src/m.tur"
CMD="$PWD/build/tur build --shared $W -o $W/.tur-repl-cache/x.so --manifest $W/.tur-repl-cache/x.mf"

$CMD 2>&1 | grep -c 'already defined'                  # repo root -> 0
( cd "$W" && $CMD 2>&1 | grep -c 'already defined' )   # foreign cwd -> 44 (now 0)
```

## Root cause

`stdlib/map.tur` and `stdlib/set.tur` each contain `(load "stdlib/hamt.tur")` --
a **cwd-relative** path. `hamt.tur` is also entry 4 of the auto-load prelude
(`g_stdlib_autoload_files`, `src/main.c`), which the project/shared build
splices ahead of every module.

`elaborate_program` (`src/compiler/elab_toplevel.c`) seeds the `(load ...)`
visited set with each auto-loaded file's canonical key,
`load_path_key(sf->path)` over the file's **absolute** path. The `(load ...)`
expander then dedups each explicit load against that set so an explicit re-load
of an already-auto-loaded stdlib module is skipped.

`load_path_key` canonicalized via `realpath()`. For `map.tur`'s
`(load "stdlib/hamt.tur")`:

- From the repo root, `realpath("stdlib/hamt.tur")` resolves to
  `/abs/.../stdlib/hamt.tur` == the seeded key -> dedup hit -> skipped.
- From any other cwd, `realpath("stdlib/hamt.tur")` fails (no such path relative
  to cwd), so the key fell back to the literal `"stdlib/hamt.tur"`, which did NOT
  match the seeded absolute key -> dedup miss -> the read-side stdlib fallback
  (already present) resolved and re-spliced `hamt.tur` -> its `extern-c`
  declarations collided with the auto-loaded copy.

Purely cwd-dependent because only the dedup key -- not the read -- depended on
`realpath` succeeding cwd-relative.

## Fix

`load_path_key` now takes the resolved stdlib dir and, when `realpath(path)`
fails on a `stdlib/<rest>` path, retries `realpath("<stdlib_dir>/<rest>")` --
mirroring the read-side fallback in `load_expand_forms`. The dedup key is then
the same canonical absolute path regardless of cwd, so `map.tur`/`set.tur`'s
`(load "stdlib/hamt.tur")` is recognized as already-auto-loaded and skipped from
any directory. Repo-root builds are unchanged (the first `realpath` still
succeeds), so codegen and the fixture snapshots are unaffected.

Also self-formatted `stdlib/map.tur` and `stdlib/set.tur` (they were left
un-formatted by an earlier change, tripping `tur_fmt_tests` -- a separate
pre-existing regression surfaced while validating this fix).

## Verification

- 0 `already defined` collisions from the repo root, a temp project dir, and
  `/tmp`.
- `tur_repl_spice_{load,call,reload,watch,errors}`: all pass.
- `tur_fmt_tests`: passes.
- `tests/run.sh` (compiled snapshots): 1985 passed, 0 failed; snapshots
  unchanged (139 up to date).
- `tests/run-turi.sh` (interpreter): 1460 passed, 0 failed.
- Full ctest set (minus the two big fixture suites validated directly): 71/71.
