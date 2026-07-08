# `tur build --shared` double-elaborates the stdlib prelude when run from a foreign cwd (breaks tur_repl_spice_* ctest)

**Severity:** high (5 ctest targets red: `tur_repl_spice_{load,call,reload,watch,errors}`).

## Summary

`tur build --shared <root>` elaborates the auto-loaded stdlib prelude twice
into the same scope when the process's **current working directory is not the
turmeric repo root**, producing ~44 `extern-c: 'tur_hamt_*' is already defined`
errors from `stdlib/hamt.tur` and failing the build. From the repo root the same
command (identical absolute-path arguments) succeeds.

The REPL's spice auto-loader (`src/turi/spice_loader.c:run_build`) always spawns
`tur build --shared` with the *project* as cwd, so every `tur repl` inside a
project hits this and prints `tur repl: spice rebuild failed`. That is what the
5 `tur_repl_spice_*` ctest targets exercise, so they are all red.

## Reproduction

```sh
W=$(mktemp -d); mkdir -p "$W/src" "$W/.tur-repl-cache"
printf '(defpackage p)\n' > "$W/build.tur"
printf '(defmodule m (export f) (defn f [x :int] :int (+ x 1)))\n' > "$W/src/m.tur"
CMD="$PWD/build/tur build --shared $W -o $W/.tur-repl-cache/x.so --manifest $W/.tur-repl-cache/x.mf"

# From the repo root: succeeds (0 collisions)
$CMD 2>&1 | grep -c 'already defined'      # -> 0

# From any other cwd: fails (44 collisions)
( cd "$W" && $CMD 2>&1 | grep -c 'already defined' )   # -> 44
( cd /tmp  && $CMD 2>&1 | grep -c 'already defined' )   # -> 44
```

Deterministic and purely cwd-dependent. `TUR_STDLIB_DIR=<abs>`,
`--no-auto-spice`, and `TUR_NO_AUTO_SPICE=1` do not change it (still 44).

## Root cause (partial)

The project/shared build injects the stdlib auto-load list
(`g_stdlib_autoload_files`, `src/main.c`) as a prelude before each module via
`load_project_prelude` (`src/main.c:870`). `hamt.tur` is entry 4 of that list.

Instrumenting the prelude loader and the `extern-c` duplicate check
(`src/compiler/elab_fns.c:4521`) shows that, from a foreign cwd, `hamt.tur`'s
externs are declared once by the prelude (low file_id) and then **re-declared
at a second, higher file_id (observed 12) into the same scope** -- a second
stdlib-elaboration path. From the repo root the prelude is loaded into separate
scopes (more prelude reads, no collision). The exact reason cwd selects between
"one shared scope" vs "separate scopes" was not pinned down; likely the presence
of a `stdlib/` directory in cwd (the repo root has one) changes module/include
resolution or the per-TU scope handling in the `--shared` descent.

## Attribution

- Reproduces on `main` HEAD (`bef1b21`, #641) with no local changes (verified by
  stashing this session's fixes and rebuilding).
- The `tur_repl_spice_*` targets **passed** at `#640` (`8a0c533`) in this
  session's ctest run, and failed after fast-forwarding to `#641/#642`. So the
  regression was most likely introduced by
  `#641` (Relocate collection natives into libturi) or `#642`. A `git bisect`
  across `8a0c533..bef1b21` would confirm which.

## Fix directions

Ensure the `--shared`/project descent elaborates the stdlib prelude exactly once
per scope: dedup stdlib files already injected by `load_project_prelude` against
any second elaboration path the descent uses, or make prelude scope handling
cwd-invariant. Because the prelude/scope machinery feeds codegen and the fixture
snapshots, any fix must be validated against `tests/run.sh` (the compiled
snapshot suite) as well as `tur_repl_spice_*`.

## Scope note

Filed separately from the turi interpreter fixture fixes landed in this branch
(list-concat crash, Option heap-payload unwrap, applied-unary Option instance).
This is a distinct build-system regression, not one of those interpreter bugs.
