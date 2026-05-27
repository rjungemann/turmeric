# Plan: Expand the `stdlib-checks` Allowlist

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** Stdlib / Tests
> **Tracks:** follow-up from `docs/stdlib-arrow-typeclass-plan.md` Phase 3

---

## Overview

`tests/run-stdlib-checks.sh` (added with the arrow typeclass plan) runs
`tur check` over an allowlist of stdlib files so regressions in their
loadability are caught before they spread. The initial allowlist is one
file (`stdlib/arrow.tur`). This plan tracks the work needed to bring the
rest of `stdlib/` into coverage.

The original arrow plan's Phase 3 proposed checking *every* stdlib file.
A blanket sweep turned up 30 / 86 files that fail standalone, split into
two distinct buckets that need separate solutions.

---

## Current state (snapshot 2026-05-27)

| Bucket | Count | Behaviour | Owner of the fix |
| --- | --- | --- | --- |
| Passes `tur check` standalone (eligible for allowlist) | 56 | Works today; just needs adding | tests |
| Auto-loaded; fails standalone with duplicate-definition errors | 17 | Compiler hard-codes them into every program, so a second `tur check` of the same file produces "already defined" diagnostics. Needs a way to skip auto-load. | compiler |
| Non-auto-loaded; fails standalone for genuine load-time reasons | 13 | Real bugs (missing flags, missing deps, codegen issues, etc.) | per-file |

### Bucket A -- auto-loaded files that need a no-stdlib flag

These are referenced in `src/main.c`'s `stdlib_files[]` list and load
unconditionally on every compile:

```
macros.tur safe.tur contract.tur hamt.tur typeclass-eq.tur map.tur
vec.tur slice.tur option.tur result.tur pair.tur tuple.tur list.tur
grid.tur zipper.tur set.tur mutmap.tur
```

To check these in isolation, the compiler needs a `--no-auto-stdlib`
(or `--check-stdlib-file`) flag that suppresses the auto-load when
type-checking a file that *is* one of the auto-loaded ones.

### Bucket B -- non-auto-loaded files that fail standalone

Identified by `for f in stdlib/*.tur; do tur check "$f"; done` and
cross-referencing the auto-load list:

```
dynvar.tur effects.tur equal.tur future.tur gadt-vec.tur nat.tur
rc.tur ref.tur session.tur sized.tur str.tur threadpool.tur
typeclass.tur
```

Each needs investigation; typical causes are:

- Missing `-X<feature>` flag at check time (e.g. `-Xsubstructural`,
  `-Xgadt`, `-Xdynamic-vars`).
- Implicit dependency on a not-yet-loaded sibling stdlib module.
- Genuine codegen / typechecker bugs that surfaced only when the file
  is loaded out of order.

---

## Plan

### Phase A -- Add the 56 passing files to the allowlist (low risk)

For each of the 56 files that already passes `tur check`:

1. Append the path to `STDLIB_FILES` in `tests/run-stdlib-checks.sh`.
2. Run `tests/run-stdlib-checks.sh` locally to confirm it stays green.
3. Land in small batches (~10 at a time) so a regression bisects to a
   specific batch rather than the whole sweep.

No code changes outside the allowlist; this is pure coverage expansion.

### Phase B -- Fix the 13 non-auto-loaded failures (medium risk)

Triage each file individually. Expected outcomes per file:

- **Needs a flag.** Add the required `-X` flag to the allowlist entry
  (extend `STDLIB_FILES` to support per-file flags, e.g. a parallel
  `STDLIB_FLAGS` array indexed by filename, or migrate to a TSV).
- **Needs a dependency.** Make the file `(load ...)` its deps explicitly
  at the top, matching what callers already do.
- **Genuine bug.** File a small targeted plan, fix, then add to the
  allowlist.

Order by triviality: do the flag/dep ones first (mostly mechanical),
then the bug-fix ones.

### Phase C -- Add `--no-auto-stdlib` for Bucket A (compiler change)

Compiler-level work:

1. Thread a `no_auto_stdlib` boolean from `src/main.c` argv parsing into
   the autoload site (`stdlib_files[]` loop ~ line 652).
2. Surface it under the `tur check` subcommand only -- there's no
   reason to suppress auto-loads at `tur build` / `tur run` time.
3. Decide what to do about transitive auto-load deps (e.g. `macros.tur`
   is needed by almost everything). Two viable shapes:
   - Hard skip: file resolves itself plus any explicit `(load ...)`
     dependencies; user is responsible for ordering.
   - Smart skip: skip auto-load only when the file being checked *is*
     one of the auto-loaded ones, so the file doesn't conflict with
     itself.

The smart-skip shape is less invasive and matches the actual failure
mode (`'Cons' is already defined`).

### Phase D -- Add Bucket A to the allowlist (depends on C)

Once `--no-auto-stdlib` exists, extend the runner to invoke it for the
auto-loaded files and append all 17 to the allowlist.

---

## Out of scope

- Replacing the shell runner with a ctest-per-file `add_test(...)` per
  stdlib file. The shell loop is simpler and reports failures in one
  place; the per-file ctest layout would clutter the test list and slow
  CI by spawning more processes.
- A full migration of stdlib coverage into `tests/fixtures/`. Fixtures
  exercise behaviour (run + assert output); the `tur check` sweep only
  asserts loadability. Both have a place.
- Building (not just checking) every stdlib file. The arrow plan
  already showed that builds expose codegen issues that aren't in
  scope here. A separate `stdlib-builds` target could be added once
  this one is stable.

---

## Risks

- **Allowlist drift.** A file that passes today can start failing
  silently if the runner accidentally swallows errors. The runner
  fails-loud (non-zero exit) so this should not happen, but verify
  the CI signal on first regression.
- **Flag explosion.** If many files need bespoke `-X` flags, the
  parallel-array shape gets unwieldy. Migrate to a TSV
  (`tests/stdlib-checks.tsv` with columns `file<TAB>flags`) before
  it crosses ~5 files needing flags.
- **Compiler `--no-auto-stdlib` semantics.** Smart-skip is
  fail-safe; hard-skip will surface a confusing parade of "macro
  `cond` not defined" errors for any caller that forgets to load
  macros.tur. Default to smart-skip.

---

## Verification

- `bash tests/run-stdlib-checks.sh` exits 0 after each batch.
- `ctest -R tur_stdlib_checks` passes in CI.
- For Phase C, a focused fixture under `tests/fixtures/errors/` that
  invokes `tur check --no-auto-stdlib stdlib/option.tur` and asserts
  it succeeds (today it fails with the duplicate-definition error).
