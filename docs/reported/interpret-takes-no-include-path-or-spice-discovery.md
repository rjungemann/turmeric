# `tur --interpret` takes no include path and runs no spice discovery, so no multi-module program can be interpreted

**Severity: low**, but it is load-bearing for a claim the tree makes elsewhere.
Nothing computes a wrong answer. What it costs is that **`--interpret` is
reachable only for single-file programs**: any program whose imports are not in
the importing file's own directory or the stdlib cannot be interpreted at all.
That silently excludes every spice, every workspace, and every test that lives
in a `tests/` sibling of `src/` -- which is to say all of them.

**Status:** open. Found 2026-09-17 archiving
[signal-compose-hand-rolled-vec-readers](../archive/signal-compose-hand-rolled-vec-readers.md),
whose whole stated payoff was interpreter coverage for the `signal/compose`
fixtures. That report's fix landed and is correct; the coverage it was buying
turns out to be blocked one layer up, here.

## Repro

From `turmeric-spices/spices/signal`, which has a `build.tur`:

```
$ tur --interpret tests/signal/test_compose.tur
tests/signal/test_compose.tur:11:3: error: module 'signal/core' not found
  searched:
    tests/signal/signal/core.tur    (importing file's directory)
    <stdlib>/signal/core.tur    (stdlib)
  hint: this looks like an intra-spice import.
        try `tur check -I src <file>` from the spice root,
        or build the whole spice with `tur build src/`
```

Taking the hint does not work, and fails in a way that names nothing:

```
$ tur --interpret -I src tests/signal/test_compose.tur
<eval>:68:7: error: load: cannot open '-I'
68 | (load "-I")
```

`-I` was taken as the **filename**. The `--interpret` arm has no flag parsing at
all: `src/main.c:12123` passes `argv[2]` as the path and `argv + 3` as the
program's `*args*`, unconditionally. `tur debug` (`src/main.c:12135`) and
`tur eval --file` share the shape.

**Two controls, both green on the same file, which is what isolates this to the
interpreter path:**

```
$ tur run tests/signal/test_compose.tur     # compiling engine
PASS test_compose
$ tur check tests/signal/test_compose.tur   # auto-spice discovery
$ echo $?
0
```

## Root cause

The elaborator already supports include dirs and already prefers them for
exactly this case -- `elab_load_module` walks the importing file's directory,
then the stdlib, then each `-I` dir (`src/compiler/elab_module.c:337`). Nothing
is missing there. The gap is entirely in what the interpreter entry point hands
it:

- `src/main.c:12123` -- the `interpret` / `--interpret` arm never calls
  `parse_include_flags` (`src/main.c:9541`) or skips positions with
  `is_include_flag` (`src/main.c:9520`), which every compiling subcommand does.
- `src/turi/eval.c:13474` and `src/turi/eval.c:13809` -- both
  `elaborate_program` calls from the interpreter pass
  `/*include_dirs=*/NULL, /*n_include_dirs=*/0` hard-coded. Even if the CLI
  parsed `-I`, there is no channel to these two call sites.
- Spice auto-discovery is not wired in either. `src/turi/spice_loader.c` has
  the `build.tur` walk-up (`find_build_tur_root`), but its only caller is
  `src/turi/repl.c` -- that is the `tur repl` feature, which AOT-compiles the
  spice to a `.so` and dlopens it rather than interpreting it. `cmd_eval_h`
  sets `module_base_dir` to the script's directory and stops.

## The stale comment that hides it

`src/main.c:4797-4800`, in `run_delegate_engine`, asserts the opposite:

```c
if (strcmp(engine, "interp") == 0) {
    /* The tree-walker discovers the enclosing spice itself (per-file
     * auto-spice), so user -I dirs are not threaded; program args after
     * `--` become *args*. */
```

It does not. That comment is the recorded *reason* `-I` is dropped on this path,
so it is worth correcting whether or not the capability is added:

```
$ tur run --engine=interp tests/signal/test_compose.tur
tests/signal/test_compose.tur:11:3: error: module 'signal/core' not found
```

Same failure, from the same directory where the compiling engine passes.

## Why it has gone unnoticed

`tests/run-turi.sh` drives single-file fixtures, so the suite that exists to
police interpreter parity cannot reach this. And the diagnostic's own hint sends
the user to `tur check -I src` -- a *different subcommand* -- which reads as
advice rather than as "the thing you asked for is impossible."

## Fix directions

1. **Parse `-I` in the interpreter arm and thread it down.** Collect with the
   existing `parse_include_flags` / `is_include_flag` pair (so `-Ifoo` and
   `-I foo` both work and the consumed positions do not leak into `*args*`),
   carry it to `cmd_eval_h`, and pass it at the two `elaborate_program` call
   sites in `src/turi/eval.c`. The receiving end already works, so this is
   plumbing, not semantics.
2. **Run the same auto-spice walk-up the compiling per-file commands run.**
   `find_spice_root` + `auto_append_spice_includes` (`src/main.c:3222`) is what
   `tur check` uses; honoring `--no-auto-spice` comes free. This is the half
   that makes `tur --interpret` inside a spice work with no flags, and it is
   what `src/main.c:4797` already claims happens.
3. Once either lands, correct the comment at `src/main.c:4797-4800` and thread
   `user_inc` / `n_user_inc` through the `interp` branch instead of ignoring
   them.

Worth pairing with a `run-turi.sh` case that actually imports a sibling module,
since a single-file suite is how this stayed invisible.

## Not this

**Not the REPL.** `tur repl` genuinely does discover the enclosing spice
(`docs/guides/repl.md`), by a different mechanism -- AOT-compile to
`.tur-repl-cache/` and dlopen. That route is unaffected and is not the one to
generalize here; an interpreted spice wants source-level module resolution, not
a compiled shared library.
