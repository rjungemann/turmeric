# Compiled fixture programs are not ASan/leak-checked

**Severity:** medium, and it is a coverage gap rather than a defect -- which
makes it the kind that stays hidden. Any memory bug in EMITTED code (leaks,
double frees, frees of non-malloc'd pointers) passes the fixture suite silently.

**Status:** OPEN. Not fixed.

## What is actually true

`CLAUDE.md` describes the Debug build as ASan/UBSan-instrumented and says
`tests/run.sh` runs "with leak detection ON". That is accurate **for the
compiler process** -- `tur build` / `emit-c` is the instrumented binary, and a
leak there does fail the suite. It is not true of the programs the compiler
produces:

- `tests/run.sh:169` builds fixtures with
  `-O2 -std=c99 -Wall -fno-strict-aliasing` -- no `-fsanitize=address`. Only
  `TUR_TSAN=1` adds a sanitizer, and that is ThreadSanitizer.
- Fixture links prefer the lean, non-ASan `libturt_runtime.a` by default
  (`TUR_RT_AUTO`), so no instrumented runtime is pulled in either.
- Runs set `ASAN_OPTIONS=...detect_leaks=0` regardless.

Confirmed by experiment, not by reading: a fixture built the normal way has zero
`__asan_init` symbols, and a program with a deliberate `malloc(1234)` that is
never freed exits clean. The same program under `valgrind --leak-check=full`
reports `1,234 bytes in 1 blocks are definitely lost`.

## Consequences

1. `requires.no-leak-check` is a no-op in the default configuration. It only
   matters in a build where fixtures do link an instrumented runtime.
2. Emitted-code memory bugs are invisible to the suite. The two open reports on
   ADT allocation were both found by hand, not by a failing test.
3. **Any change to how emitted code allocates cannot be validated by "run the
   suite".** This was found while trying to verify exactly that for the ADT slab
   allocator -- the plan was "ASan will abort loudly on a bad free", and ASan is
   not there to abort.

## What would close it

Cheapest useful step: a small opt-in harness that rebuilds a chosen subset of
fixtures with `-fsanitize=address` and runs them with leak detection on, or runs
them under `valgrind --leak-check=full --error-exitcode=N`. Valgrind needs no
rebuild and no instrumented runtime, which makes it the easy first version.

It does not need to be the whole suite to be worth having -- the allocation-heavy
fixtures would have caught both open ADT reports.
