# Plan: Pre-existing ASan/LSan Leaks in the Debug `tur`

> **Status:** Implemented (Phase 1 + Phase 2)
> **Last Updated:** 2026-05-29
> **Type:** Build/test hygiene + (optional) compiler memory-management
> **Related:**
> - `src/compiler/emit_core.c` -- `emit_resolve_type` (leak sites)
> - `src/compiler/emit_module.c` -- `emit_abi_instantiate_type` (leak sites)
> - `CMakeLists.txt` -- `set_tests_properties(... ASAN_OPTIONS=detect_leaks=0)`
> - `CLAUDE.md` -- "Before opening any PR, run `bash tests/run.sh`"
> - `docs/manifest-driven-build-descent-plan.md` (surfaced these while testing)

---

## Overview

The Debug build compiles `tur` with `-fsanitize=address,undefined`
(`src/CMakeLists.txt`). On Linux, ASan ships LeakSanitizer enabled, so any
allocation still reachable at process exit is reported as a leak and the
process exits non-zero.

`tur` is a short-lived compiler process: several codegen paths allocate
scratch that lives for the lifetime of the process and is intentionally never
freed. LSan flags this design choice as leaks. The project already documents
and tolerates one instance of this (the tree-walking interpreter's closures)
by disabling leak detection for the test targets:

```cmake
# CMakeLists.txt
# LeakSanitizer ships enabled with ASan on Linux ... the tree-walking
# turi/eval interpreter intentionally does not free closures (they live for
# the process lifetime). Disable leak detection so the test run still catches
# real memory errors ... without flagging that design choice.
set_tests_properties(tur_tests tur_cli_tests ... 
  PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
```

There is a **second** class of process-lifetime allocation that the comment
does not mention: the codegen type-instantiation paths used for ABI
specialization (generic monomorphization). These leak too, and surface as
spurious `tur build` failures whenever leak detection is *on*.

## Symptoms

Running the fixture suite **directly** (as `CLAUDE.md` instructs) rather than
through `ctest`:

```sh
$ bash tests/run.sh
...
FAIL list-basic -- build failed
FAIL option-basic -- build failed
FAIL result-typed-basic -- build failed
FAIL tuple-345-basic -- build failed
FAIL tuple-arity-6 -- exit 1, expected 0
FAIL tuple2-eq-macro -- build failed
FAIL emit-abi-trace -- build failed
```

Each "build failed" is a LeakSanitizer abort inside the `tur` compiler:

```
==NNNN==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 896 byte(s) in 4 object(s) allocated from:
    #1 emit_resolve_type        src/compiler/emit_core.c:73
Direct leak of 896 byte(s) in 4 object(s) allocated from:
    #1 emit_resolve_type        src/compiler/emit_core.c:72
Direct leak of 448 byte(s) in 2 object(s) allocated from:
    #1 emit_abi_instantiate_type src/compiler/emit_module.c:176
Direct leak of 448 byte(s) in 2 object(s) allocated from:
    #1 emit_abi_instantiate_type src/compiler/emit_module.c:175
SUMMARY: AddressSanitizer: 2688 byte(s) leaked in 12 allocation(s).
```

Running the *same* fixtures through `ctest` passes, because the ctest target
sets `ASAN_OPTIONS=detect_leaks=0`. CI (`.github/workflows/ci.yml`) runs
`ctest`, so the leaks are invisible there. **The gap is the raw
`bash tests/run.sh` path that `CLAUDE.md` tells contributors to use before a
PR -- it surprises anyone whose shell does not already export
`ASAN_OPTIONS`.**

## Root cause

Both leak sites deep-copy/instantiate `Type` trees while resolving generic
type variables to concrete types for an ABI specialization:

- `emit_resolve_type` (`emit_core.c`): for `TY_APP` / `TY_UNION` /
  `TY_INTERSECTION`, mallocs new `Type` nodes for the resolved children and
  returns a `Type` by value whose pointers reference that heap.
- `emit_abi_instantiate_type` (`emit_module.c`): the same shape, instantiating
  type-variable bindings.

The returned `Type` values flow into codegen (struct/ADT app emission, ABI
trace, specialized call emission) and the heap nodes are never reclaimed.
Because the process exits right after emitting C, nothing frees them -- a
classic "arena-by-process-exit" pattern. Single-segment / non-generic programs
never hit these branches, which is why only the generic-collection fixtures
(`list`, `option`, `result`, `tuple*`, and the `emit-abi-trace` fixture) fail.

These are **not** use-after-free or out-of-bounds bugs (ASan's
address-checking still passes); they are reachable-at-exit allocations.

## Goals / non-goals

Goals:
- Make `bash tests/run.sh` behave consistently with `ctest` so contributors
  don't see spurious failures (the immediate pain).
- Decide a durable policy for process-lifetime codegen allocations, and make
  the ABI-specialization leaks either fixed or explicitly, narrowly tolerated
  -- not blanket-suppressed in a way that hides real leaks.

Non-goals:
- Auditing every allocation in `tur` for leaks. This plan is scoped to the
  documented process-lifetime pattern plus the ABI-spec type scratch.
- Changing the interpreter's closure-lifetime design.

## Options

### A. Make the suppression policy consistent + discoverable (low effort)

`tests/run.sh` (and the other `run-*.sh` harnesses) default `ASAN_OPTIONS` to
`detect_leaks=0` unless the caller has already set it:

```sh
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
```

This mirrors the existing ctest policy in one place the developer actually
invokes, removes the surprise, and still lets `ASAN_OPTIONS=detect_leaks=1
bash tests/run.sh` opt back in. Pair with a one-line note in `CLAUDE.md`.

- Pro: trivial, matches existing intent, fixes the reported pain immediately.
- Con: keeps leak detection off for the whole compiler path -- real future
  leaks in `tur build` go unnoticed.

### B. Narrow LSan suppression file (low/medium effort)

Add `tests/lsan.supp` listing only the two known intentional sites:

```
leak:emit_resolve_type
leak:emit_abi_instantiate_type
```

Point the test env at it: `ASAN_OPTIONS=suppressions=tests/lsan.supp` (instead
of `detect_leaks=0`). Then leak detection stays **on** everywhere else, so a
genuinely new leak in codegen still fails the suite.

- Pro: keeps the safety net for everything except the two documented sites.
- Con: suppression matching is by symbol/substring and can over-match; needs a
  comment trail so the entries don't outlive their justification.

### C. Arena-allocate the ABI-spec type scratch (the principled fix)

Give `EmitCtx` an arena (or a tracked free-list) for the transient `Type`
nodes that `emit_resolve_type` / `emit_abi_instantiate_type` allocate, and
release it in bulk when the compile finishes (or retain it but have it be a
single tracked root so LSan sees it as reachable, not leaked). Turmeric already
has an `Arena` abstraction used elsewhere -- reuse it.

- Pro: removes the leaks for real; lets leak detection be re-enabled for the
  `tur build` path, catching future regressions; no suppression debt.
- Con: must thread arena ownership through the codegen paths that currently
  return these `Type` values by value; needs care that no returned `Type`
  outlives the arena (it shouldn't -- everything is consumed within a single
  emit pass, but that invariant must be verified).

## Recommendation

Two-step, decoupled so the pain is fixed immediately without blocking the real
fix:

1. **Now:** Option A (default `detect_leaks=0` in the `run-*.sh` harnesses) so
   `bash tests/run.sh` matches CI, plus a `CLAUDE.md` note. Cheap, unblocks
   contributors. Optionally layer Option B so the net stays up elsewhere.
2. **Follow-up:** Option C (arena the ABI-spec `Type` scratch) and then
   re-enable leak detection for the compiler path, updating the `CMakeLists.txt`
   comment to reflect that only the interpreter-closure case remains
   intentionally suppressed.

## Plan

### Phase 1 -- Consistent, discoverable suppression (Option A [+ B])

1. In `tests/run.sh`, `tests/run-cli.sh`, `tests/run-flags.sh`, and the other
   compile-and-run harnesses, add near the top:
   ```sh
   export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
   ```
   (The new `tests/run-build-project.sh` already does this; bring the rest in
   line.)
2. (Optional, Option B) Add `tests/lsan.supp` with the two documented entries
   and switch the harness/env to `suppressions=...` instead of
   `detect_leaks=0` so other leaks still fail.
3. Add a one-line note to `CLAUDE.md` next to the `bash tests/run.sh`
   instruction explaining the leak-detection policy and the opt-in override.
4. Verify: `bash tests/run.sh` reports zero `FAIL` on a clean checkout (no
   `../turmeric-spices`, so `scscm-compile` auto-skips).

### Phase 2 -- Eliminate the ABI-spec leaks (Option C)

1. Add an arena handle to `EmitCtx` (init at emit start, dispose at emit end).
2. Route the `malloc`s in `emit_resolve_type` (`emit_core.c:72-73` and the
   `TY_UNION`/`TY_INTERSECTION` member arrays) and `emit_abi_instantiate_type`
   (`emit_module.c:175-176` and its member arrays) through the arena.
3. Audit every consumer of the returned `Type` to confirm none escapes the
   emit pass (so bulk-free at emit end is sound).
4. Re-enable leak detection for the `tur build` path (drop `detect_leaks=0`
   for `tur_tests` et al., or flip the `lsan.supp` entries to no-ops) and
   confirm the suite stays green with detection ON.
5. Update the `CMakeLists.txt` comment so it documents only the remaining
   intentional case (interpreter closures).

## Risks

1. **Option A hides real leaks.** Blanket `detect_leaks=0` means a future
   `tur build` leak won't fail CI. Mitigated by pairing with Option B now, or
   by committing to Phase 2.
2. **Arena lifetime bugs (Option C).** If any returned `Type` is cached beyond
   the emit pass (e.g. memoized across modules), bulk-free turns the leak into
   a use-after-free. The Phase 2 audit (step 3) is the gate; keep ASan's
   address checking on throughout to catch a mistake immediately.
3. **Suppression over-match (Option B).** `leak:emit_*` substring rules can
   silence unrelated frames in the same function. Keep entries specific and
   commented.

## Validation checklist

- [x] `bash tests/run.sh` reports zero `FAIL` on a clean checkout (sibling
      spices repo absent so `scscm-compile` auto-skips). Now runs with leak
      detection ON (1045 passed, 0 failed).
- [x] `ctest --test-dir build` stays green. (Pre-existing, unrelated failure
      in `tur_stdlib_checks` -- a `TUR-E0042` mixed-width arithmetic type
      error in `stdlib/typeclass.tur:182`, reproducible on the base with
      `detect_leaks=0`, i.e. not a leak and out of scope for this plan. All 32
      other targets pass, including the now-unsuppressed `tur_tests`,
      `tur_cli_tests`, and `tur_span_tests`.)
- [x] `ASAN_OPTIONS=detect_leaks=1 bash tests/run.sh` reproduced the leaks
      before Phase 2 and reports zero leaks after Phase 2.
- [x] (Phase 2) the generic fixtures (`list-basic`, `option-basic`,
      `result-typed-basic`, `tuple-345-basic`, `tuple-arity-6`,
      `tuple2-eq-macro`, `emit-abi-trace`) build clean with leak detection ON.
- [x] `CLAUDE.md` documents the policy and the opt-in override.

## Implementation notes (what was actually done)

Rather than the interim two-step (Option A now, Option C later), both phases
landed together, which let the end state be stronger than a blanket
suppression:

- **Phase 2 / Option C (the principled fix):** `EmitCtx` gained a
  `type_arena` (`Arena *`). `emit_resolve_type` (`emit_core.c`) and
  `emit_abi_instantiate_type` (`emit_module.c`) now allocate their transient
  `Type` scratch from that arena instead of bare `malloc`. The arena is
  initialized at the start of each emit pass (`emit_program`,
  `emit_implementation`, and the separate-compilation `hdr_ctx`) and
  bulk-freed at the end. Audit confirmed no instantiated `Type` escapes the
  emit pass: the cross-module borrow-spec cache persists only `TypeKind`
  values (`.kind`) and `strdup`'d names, never `Type` pointers. A NULL arena
  falls back to `malloc` so any future context without an arena is still
  correct.
- **Re-enabled leak detection for the compiler path:** because the compiler is
  now leak-clean, `tur_tests`, `tur_cli_tests`, and `tur_span_tests` were
  removed from the `detect_leaks=0` set in `CMakeLists.txt`. They now run with
  LSan ON as a regression guard. The remaining suppressed targets all exercise
  the tree-walking interpreter (`turi`/`eval`/`repl`/`flags`/`install`), which
  intentionally never frees closures/registered natives.
- **Phase 1 / discoverability (adapted):** `bash tests/run.sh` is left with
  leak detection ON (it is the mandated pre-PR harness and is now a real net).
  Only the interpreter-exercising harnesses that genuinely leak when invoked
  directly -- `tests/run-flags.sh` and `tests/run-turi.sh` -- default
  `ASAN_OPTIONS=detect_leaks=0` to mirror their ctest targets, with an opt-in
  override documented. `CLAUDE.md` gained a "Leak detection (ASan/LSan)
  policy" subsection.

Option B (a `lsan.supp` file) was not needed: Option C removed the leaks
outright, so there is nothing left to suppress on the compiler path.

## Appendix: affected fixtures (observed)

`list-basic`, `option-basic`, `result-typed-basic`, `tuple-345-basic`,
`tuple-arity-6`, `tuple2-eq-macro`, `emit-abi-trace` -- all generic /
ABI-specialized programs. Non-generic fixtures (the large majority) are
unaffected because they never enter the leaking `TY_APP` / `TY_UNION`
instantiation branches.
