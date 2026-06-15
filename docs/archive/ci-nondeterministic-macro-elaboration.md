# CI-only: nondeterministic macro elaboration -> snapshot drift + ASan leak (compiler)

> **Not caused by, and not reproducible in, the interpreter-parity PR (#336).**
> Surfaced while triaging that PR's red CI. Filed per the STRICT bug-reporting
> rule. Root cause is **hypothesized** (see below); the CI evidence is concrete,
> the local non-reproduction is confirmed.

**Summary:** Two CI checks fail on PR #336 (head `ec2843e`) -- and on recent
`main` runs, which are also red/cancelled -- with failures that **do not
reproduce in a local Debug+ASan build**:

1. **`Check codegen snapshots`**: `DRIFT` on exactly 4 macro fixtures --
   `macro-defmacro`, `macro-multi-arg`, `macro-nested`,
   `macro-quasiquote-unquote`. Locally `tur run regen-snapshots --check` reports
   `73 snapshots are up to date` and a write-mode regen produces **zero** git
   changes; the 4 files are byte-identical to `origin/main`.
2. **`Test (ubuntu-latest)` -> `tur_stdlib_checks`**: `FAIL stdlib/sized.tur`
   with `LeakSanitizer: 64 byte(s) leaked` allocated at
   `elab_expand_macro` (`src/compiler/elab_macros.c:1095`). Locally
   `ASAN_OPTIONS=detect_leaks=1 bash tests/run-stdlib-checks.sh` reports
   `32 passed, 0 failed`.
3. **`Test (ubuntu-latest)` -> `tur_tests`**: a large wave of fixtures report
   `(build failed)` (seq-*, schema-*, vec-*, sized-*, tce*, stdlib-macros, ...).
   Under ASan a 64-byte leak at compile time makes `emit-c`/`build` exit
   nonzero, so every fixture whose elaboration hits the leak "fails to build".
   Locally `bash tests/run.sh` is `1573 passed, 0 failed` (leak detection on).

**Severity:** High for CI hygiene (main is red; PRs cannot go green), but the
shipped compiler is correct on the environments tested locally. It is a
*toolchain/environment-sensitivity* bug, not a wrong-output bug on the happy
path.

## Why these are one issue

Both failures point at **macro elaboration behaving differently on the CI
toolchain than locally**:

- The snapshot drift is in the only 4 fixtures that *define and expand* a
  `defmacro`. Their emitted code carries `__fn_N` anonymous-closure ids from a
  global counter; if elaboration visits/allocates closures in a different order
  or count, those ids shift -> drift.
- The leak is at `elab_macros.c:1095`, the `defmacro` param-parse **error
  path** (`"defmacro: expected parameter list or vector"`). For that to fire +
  leak on `stdlib/sized.tur` in CI but not locally, sized.tur must *elaborate
  differently* in CI -- the same nondeterminism.

So a single root -- nondeterministic macro elaboration -- plausibly explains
both. The likeliest mechanism is **uninitialized memory / UB in the compiler**
that the CI gcc realizes differently. Concrete leads found while triaging:

- `src/compiler/emit_core.c:1520`: `'*arg_strs' may be used uninitialized`
  (`-Werror=maybe-uninitialized`) -- blocks a local Release build outright.
- Several `-Werror=stringop-truncation` in `emit_stmt.c:441,448`,
  `emit_core.c:1752,1760`, `elab_typeclasses.c:2829` -- 63-byte buffers that
  can silently truncate mangled names, which could also perturb codegen.

These are pre-existing (`emit_core.c` last touched `3b362cd`, 2026-06-09;
the 4 snapshots last regenerated in `7b8fd8e`, 2026-06-11) and unrelated to the
interpreter-only PR that surfaced them.

## Repro (CI) / non-repro (local)

CI: PR #336 jobs `Check codegen snapshots` and `Test (ubuntu-latest)` on
`ec2843e` (and recent `main` runs).

Local (does NOT reproduce, Debug+ASan):
```sh
cmake --build build -j --config Debug
./build/tur run regen-snapshots -- --check                 # => 73 up to date
ASAN_OPTIONS=detect_leaks=1 bash tests/run-stdlib-checks.sh # => 32 passed, 0 failed
ASAN_OPTIONS=detect_leaks=1 ./build/tur emit-c stdlib/sized.tur >/dev/null # => exit 0
```

## Proposed fix directions

1. **Fix the uninitialized read** at `emit_core.c:1520` (`arg_strs`) -- safe by
   construction (initialize to NULL/empty); the most likely source of the
   nondeterminism. Then re-run CI to see if drift + leak clear.
2. **Audit macro elaboration for hash/pointer-order dependence** (gensym /
   `__fn_N` counter, instance/symbol iteration) so codegen is deterministic
   across toolchains.
3. **Widen the snapshot buffers** flagged by `-Werror=stringop-truncation`
   (63 -> larger) to remove the truncation UB.
4. **Plug the `elab_expand_macro` leak** -- whatever buffer is realloc'd at the
   `defmacro` param-error path must be freed on that path.

Each should be validated against a build matching the CI toolchain (newer gcc,
Release + ASan), since none reproduce on the local Debug build.

## Validation

After a fix: PR #336's `Check codegen snapshots` and `Test (ubuntu-latest)` go
green, and recent `main` CI recovers. A local Release build (currently blocked
by `-Werror`) should compile clean.

## Status

Filed 2026-06-12 while babysitting PR #336. The PR's own changes
(interpreter-only: `src/main.c` natives, `src/turi/eval.c`) are validated
locally (compiled 1573/0, interpreter 939/0) and do not touch the compiler
macro/codegen path. This report tracks the pre-existing compiler issue so the
red CI is not mistaken for a regression in that PR.
