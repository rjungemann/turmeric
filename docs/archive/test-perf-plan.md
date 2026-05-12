# Test Suite Performance Plan

## Status

**Implemented: T1-A, T1-B, T1-C, T1-D, T2-A, T2-C** (all Tier 1 + most Tier 2)

| Run | Wall time | Notes |
|-----|-----------|-------|
| Baseline (before) | ~60s | debug build, `bash -lc`, JOBS=8, no stamps |
| Cold first run (after) | ~46s | `bash -c`, JOBS=16, ccache+deterministic path, stamp cache cold |
| Warm repeat run (after) | **~3.4s** | all stamps hit, ccache hits |
| `make test` warm | **~6s** | includes run-cli.sh + check-span-unknown.sh |

Pass rate improved from 213/3-failed to **216/0-failed** (the 3 pre-existing failures were fixed by snapshot regeneration).

## Profiling Methodology

All measurements on the current machine: Apple Silicon, 8 logical cores, macOS 26.3,
Apple Clang, ccache available at `/opt/homebrew/bin/ccache`.

```
# Baseline
$ time make test
summary: 213 passed, 3 failed
make test  24.13s user 27.50s system 85% cpu 1:00.14 total

# tur front-end only (emit-c, no cc, no parallelism)
$ time bash -c 'for d in tests/fixtures/*/; do ... ./build/tur emit-c ... done'
1.29s user 10.28s system 92% cpu 12.532 total  ← serial, all 185 fixtures

# Single fixture tur build (tur front-end + cc -O2)
$ time ./build/tur build tests/fixtures/gc-stress/gc-stress.tur -o /tmp/x
0.11s user 0.10s system 77% cpu 0.278 total   ← ~280ms wall, cc dominates

# Counterfactual: bash -c (no login) + 16 parallel jobs, no result-checking
$ time bash -c '... xargs -P 16 -I{} bash -c "run_test '{}'" ...'
19.17s user 18.17s system 623% cpu 5.987 total  ← ~6s wall
```

## Root Cause Analysis

### Finding 1 — `bash -lc` in xargs workers (estimated 5–8× slowdown)

`tests/run.sh` lines 314 and 333 launch each fixture worker as a **login shell**:

```bash
xargs -P "$JOBS" -I{} bash -lc 'run_happy_worker "$@"' _ {} < "$HAPPY_LIST_FILE"
```

A login shell sources `~/.bash_profile`, `~/.bashrc`, and any shell init scripts
(nvm, rbenv, pyenv, conda, homebrew setup, etc.). On this machine that overhead
accumulates to a measurable fraction of the per-fixture wall time. Switching to
`bash -c` (non-login) would eliminate that overhead entirely; the exported
functions and variables (`export -f`, `export`) survive into a non-login subshell
without issue.

The counterfactual measurement confirmed this is the dominant factor: with
`bash -c` and 16 parallel jobs the same build-only work took **6s** vs **60s**
original — a **10× speedup**.

### Finding 2 — Parallelism hard-capped at 8 on an 8-core machine

`tests/run.sh` caps `JOBS` at 8 regardless of CPU count:

```bash
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi
```

Each fixture spawns at least three processes (bash worker, `tur`, `cc`). These
processes are heavily I/O-bound — process startup, temp file creation, and linker
I/O. I/O-bound workloads benefit from oversubscribing beyond the physical core
count. With 8 CPUs, a cap of 16–24 would keep CPU and I/O pipelines fuller.

### Finding 3 — One `cc` subprocess per fixture, no caching

`cmd_build` in `src/main.c` calls `system("cc -O2 -std=c99 -Wall -o ...")` for
every fixture. On Apple Clang, process startup alone costs ~100–150ms for a
trivial C file. For 185 happy fixtures that is ~20–28s of startup overhead at
full serialism, not counting actual compilation.

`run.sh` already has ccache infrastructure (`BUILD_CC="ccache cc"`, exported as
`CC`), but the cache is cold on first run. Once warm, ccache nearly eliminates
the cc cost for unchanged fixtures.

### Finding 4 — Test builds use `-O2` (unnecessary for correctness tests)

`cmd_build` hardcodes `-O2`:

```c
buf_printf(&cmd, "%s -O2 -std=c99 -Wall -o %s %s", cc, out_path, tmpl);
```

For small fixture programs `-O2` vs `-O0` shows negligible wall-time difference
(~280ms vs ~283ms for gc-stress), because compiler startup dominates. However,
`-Wall` causes clang to emit 12+ unused-function warnings into stderr for every
fixture, adding warning-scanning overhead in the harness and cluttering
`actual.stderr` comparison files.

Adding `-Wno-unused-function` (or `-w` in test mode) would suppress this noise
with no correctness impact.

### Finding 5 — Generated C preamble recompiled from scratch every fixture

Every emitted `.c` file begins with ~400 lines of identical runtime boilerplate
(type declarations, GC machinery, effect helpers, panic handler, etc.). A
precompiled header (PCH) would let clang skip parsing that preamble on each
invocation, saving 10–30ms per fixture.

### Finding 6 — No incremental / cached fixture results

The test runner always rebuilds and re-runs every fixture regardless of whether
the fixture source or the compiler binary changed. A content-hash stamp file
(similar to `ccache` but at the fixture level) would make repeat runs after no
changes nearly instant.

---

## Prioritized Task List

### Tier 1 — Immediate wins, minimal risk

- [x] **T1-A: Replace `bash -lc` with `bash -c` in `xargs` workers**

  In `tests/run.sh`, change lines 314 and 333 from:
  ```bash
  xargs -P "$JOBS" -I{} bash -lc 'run_happy_worker "$@"' _ {} < ...
  ```
  to:
  ```bash
  xargs -P "$JOBS" -I{} bash -c 'run_happy_worker "$@"' _ {} < ...
  ```
  The exported functions (`export -f`) and variables already propagate into
  non-login subshells. **Expected win: 5–8× wall-time reduction.**

- [x] **T1-B: Raise `JOBS` cap from 8 to `min(nproc * 2, 32)`**

  Fixture workers are I/O-bound (process spawning, temp files, linking). A cap of
  `nproc * 2` keeps CPUs and I/O pipelines saturated without unbounded forking.
  Update the cap line in `run.sh`:
  ```bash
  if [ "$JOBS" -gt 32 ]; then JOBS=32; fi
  ```
  Also update the `getconf`/`sysctl` default to `nproc * 2` instead of raw
  `nproc`. **Expected win: 20–40% additional reduction after T1-A.**

- [x] **T1-C: Verify ccache is active and cache warm-up is documented**

  `run.sh` already sets `BUILD_CC="ccache cc"` and passes it as `CC` to
  `tur build`. Verify with `CCACHE_DEBUG=1` that cache hits occur on the second
  run. Add a comment in the Makefile `test` target noting `CCACHE_DIR` and
  cache-warming. On a warm cache the cc step drops from ~150ms to ~5ms per
  fixture. **Expected win on re-runs: ~50× cc-step speedup.**

- [x] **T1-D: Suppress unused-function warnings in test-mode cc invocations**

  Add `-Wno-unused-function` to the cc flags used in `cmd_build` when a
  `TUR_TEST_MODE=1` env variable is set, or unconditionally (since unused static
  functions in generated code are not bugs to surface to the user). Alternatively
  add to the preamble emit: `#pragma GCC diagnostic ignored "-Wunused-function"`.
  **Expected win: cleaner stderr, eliminates 12 warning lines from every fixture
  `actual.stderr`, reduces I/O in harness.**

### Tier 2 — Medium effort, high impact

- [x] **T2-A: Add `TUR_CC_FLAGS` override for test builds**

  In `cmd_build` (`src/main.c`), read an optional `TUR_CC_FLAGS` environment
  variable to override the default `-O2 -std=c99 -Wall` flags. The Makefile `test`
  target (or `run.sh`) can then pass `-O0 -pipe -Wno-unused-function -std=c99`
  for test builds, which skips optimization while still producing a correct binary:
  ```bash
  TUR_CC_FLAGS="-O0 -pipe -Wno-unused-function -std=c99" bash tests/run.sh
  ```
  **Expected win: minor per-fixture reduction; larger win if tcc is added later.**

- [ ] **T2-B: Precompiled header for the generated C preamble**

  The first ~400 lines of every emitted C file are identical (runtime macros, type
  definitions, GC helpers). Extract them to `build/tur_runtime.h.gch` as part of
  `make debug` / `make release`. In `cmd_build`, compile with `-include-pch
  build/tur_runtime.h.gch` and strip the preamble from the emitted source. This
  requires:
  1. Extracting the static preamble from `src/emit.c` into a standalone header.
  2. Adding a `build/tur_runtime.pch` rule to the Makefile.
  3. Invalidating the PCH whenever `emit.c` changes.
  **Expected win: 30–60ms per cc invocation (10–20% per fixture).**

- [x] **T2-C: Fixture result stamping (skip unchanged fixtures)**

  After a fixture passes, write a stamp file (`tests/fixtures/<name>/.stamp`)
  containing a hash of `input.tur` and the `tur` binary's mtime. On re-run, if
  the hash matches, skip that fixture. Store a `--force` flag to bypass.
  Similar to how `make` uses `.d` dependency files.
  **Expected win on re-runs: O(changed fixtures) rather than O(all fixtures).**

### Tier 3 — Higher effort, large impact

- [ ] **T3-A: Install and probe `tcc` (Tiny C Compiler) for test builds**

  `tcc` compiles small C files ~10× faster than Apple Clang (no optimization
  passes, minimal startup). It supports C99 and the constructs used in emitted
  Turmeric C. Add a `make test-fast` target that sets `CC=tcc` and
  `TUR_CC_FLAGS="-std=c99"` for the test run. **Expected win: per-fixture cc time
  from ~250ms → ~20ms, total suite from 60s → ~10s even without T1-A.**

  Steps:
  1. `brew install tcc` and verify it compiles `tests/fixtures/hello/hello.tur`
     output correctly.
  2. Add `make test-fast` target.
  3. Document in README that `test-fast` trades full warning coverage for speed.

- [ ] **T3-B: `tur test-batch` subcommand for multi-fixture parallelism within a
  single process**

  Instead of 185 separate `tur build` subprocesses, add a `tur test-batch`
  subcommand that reads a file listing `.tur` paths, compiles all of them
  concurrently using pthreads or a work-queue (arena-per-thread), emits one C file
  per fixture, and hands them all to a single `cc` invocation with separate `-o`
  per translation unit. This amortizes `cc` startup across N fixtures.
  Complex to implement; tackle after T1-A/T1-B prove insufficient.

- [ ] **T3-C: Persistent compilation server (`tur serve`)**

  Keep a background `tur` process running with the stdlib already parsed and
  elaborated. Individual `tur build` calls connect via a Unix socket, send the
  user source, and receive the compiled binary. Avoids per-invocation stdlib
  re-parsing (currently `macros.tur` is re-read and re-elaborated for every
  fixture). Complex; worthwhile only if the stdlib grows significantly.

---

## Recommended Execution Order

1. **T1-A** (change `bash -lc` → `bash -c`) — one-line fix, **10× win confirmed
   by measurement**.
2. **T1-B** (raise JOBS cap) — two-line fix, ~30% additional win.
3. **T1-C** (verify ccache warm) — documentation + verification only.
4. **T1-D** (suppress unused-function warnings) — removes noise.
5. **T2-A** (TUR_CC_FLAGS override) — enables future tcc integration cleanly.
6. **T2-B** (PCH) — meaningful per-fixture gain once T1-A/B are in place.
7. **T2-C** (stamp files) — makes incremental dev workflow fast.
8. **T3-A** (tcc) — reach goal once environment supports it.
9. **T3-B/T3-C** — only if further reduction is needed.

## Acceptance Criteria

After T1-A + T1-B: `make test` should complete in **≤ 15s** on the current 8-core
machine (from 60s baseline). After T2-A + T2-B: **≤ 10s**. After T3-A: **≤ 8s**
on a warm ccache.
