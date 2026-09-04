# Emitted-program leak checking was not generalized

**Severity:** low-medium coverage gap. **Status:** RESOLVED 2026-08-26.

An earlier revision kept this in `reported/` "because one open leak is still
marked known". That was the wrong test: the known leak belongs to
[inline-c-option-carrier-box-leaks](inline-c-option-carrier-box-leaks.md), which
has its own report, and removing the marker is tracked as row 2 of
[workarounds-to-remove](workarounds-to-remove.md). *This* report's finding --
that emitted-program leak checking was never generalized -- is fully addressed,
so it archives.

## The finding, stated correctly

An earlier revision of this report said compiled fixtures "are not
ASan/leak-checked", implying no mechanism existed. **That overstated it.** Three
harnesses already run emitted programs under AddressSanitizer, and one of them
documents the gap in almost these words:

- `tests/run-closure-env-leak.sh` and `tests/run-fat-shim-leak.sh` emit C, then
  compile it by hand with `-fsanitize=address,undefined` and run it with
  `detect_leaks=1`.
- `tests/run-gc-leak-gate.sh` builds fixtures through `tur build` with
  `TUR_CC_FLAGS="... -fsanitize=address -g"`.
- `tests/run-leak-gate.sh` covers the COMPILER's own error path, not emitted
  code, and its header already says: "tests/run.sh compiles the *generated
  program* without ASan and only the `tur` binary itself is sanitized."

What was actually true, and is the real gap:

1. The default suite does not leak-check emitted programs. `tests/run.sh:169`
   builds fixtures with `-O2 -std=c99 -Wall -fno-strict-aliasing` -- no
   sanitizer -- links the lean non-ASan runtime by default, and runs with
   `detect_leaks=0`. Confirmed by experiment: a fixture built the normal way has
   zero `__asan_init` symbols, and a deliberate `malloc(1234)` that is never
   freed exits clean.
2. Each existing harness is bespoke to one regression. There was no way to say
   "this fixture should be leak-clean" without writing a fourth script.
3. `requires.no-leak-check` is therefore a no-op in the default configuration.

## What was done

`tests/run-leak-check.sh` generalizes the existing mechanism rather than adding
another one-off: any fixture opts in with a `requires.leak-check` marker, and is
built with ASan and run with `detect_leaks=1`. It must still print its
`expected.stdout` and must report no leak.

A fixture may also carry `known-leak`, naming an open report. The leak then
shows as `KNOWN` instead of failing -- a permanently red gate is one nobody
reads -- and the gate fails if such a fixture ever runs CLEAN, so a stale marker
cannot outlive the bug it describes.

Proven to fail before being trusted: against the open `rc/of` leak it reports
the 16 bytes with a stack trace to `ctor_PA`.

The coverage map is written up for maintainers in
[../guides/test-suite-portability-guide.md](../guides/test-suite-portability-guide.md),
section "Leak checking -- what is covered and what is not", and `CLAUDE.md`'s
leak-detection policy now states the complement (emitted programs are NOT
checked by the default suite) rather than leaving it to be inferred.

## What it cannot see

LSan reports memory unreachable from roots. Anything held in a global registry
is reachable by construction and will be called live however dead it is --
`run-gc-leak-gate.sh` documents this at length for rc blocks in `gc_all_blocks`,
and is why that gate counts collector work instead. A clean run here means
"nothing was orphaned", not "nothing was retained".

## Both follow-ups are done

An earlier revision listed two. Both landed in `3c457e92` and this section was
never updated -- recorded here rather than quietly edited, because a stale
"still open" list is how a resolved report keeps looking unresolved.

- **"Only two fixtures opt in so far."** Now **54**, and they are the
  allocation-heavy ones the note asked for -- the `rc-*`, `weak-*`, `defer-*`
  and `affine-*` families. Widening from 2 to 54 is what surfaced the two
  ownership-handoff leaks that this branch went on to fix.
- **"Not wired into ctest."** Now a `tur_leak_check` target
  (`CMakeLists.txt:146`), marked `RUN_SERIAL` so it gets the machine rather
  than competing with the other suites for it.

Current state: **53 passed, 0 failed, 1 known-open**, the known one being the
inline-C carrier box.

## The staleness guard, verified

The `known-leak` mechanism is only safe if a marker cannot outlive the bug it
names, so the gate fails a fixture that is marked known and runs CLEAN
(`run-leak-check.sh:126`). Checked by experiment rather than by reading it:
marking `leak-check-clean-baseline` known-leak turns the run red with
`marked known-leak but ran clean -- fix or delete ...`, and removing the marker
restores `53 passed, 0 failed, 1 known-open`. A gate that has never been seen to
fire is not a gate.
