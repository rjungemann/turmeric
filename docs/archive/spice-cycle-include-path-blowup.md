# Cyclic `:spices` deps blow up the `-I` search path

**Status:** resolved 2026-07-29.

**Severity:** medium -- a manifest cycle produces an unreadable `cc` error
instead of a diagnostic, and the include path grows without bound.

## Resolution (2026-07-29)

`collect_dep_dirs_recursive` (`src/main.c`) now threads a `VisitedRoots` set
keyed on each package root's `realpath()`, checked before recursing into a
dep's manifest. Keying mirrors `pkg_collect_transitive_cmake_deps`, which walks
the same `:spices` graph and already had this.

**Silent dedup, not a hard error.** The report left the policy open; the
existing harness case settles it. `transitive-cmake-deps-cycle-builds` asserts
that `tur build spice-a` *succeeds* and runs, and the fixture header says the
resolver "must terminate via its visited set". A cycle between path-local
siblings is not by itself malformed -- two spices in a workspace can
legitimately reference each other -- so terminating is the behavior the design
already committed to.

The report's second, independent suggestion is also in: `push_include_dir()`
canonicalizes each dir before appending. That is what makes the *output* dedup
effective at all -- a transitively-resolved dep arrives as `a/../b/src`, which
`include_dir_seen` would otherwise treat as distinct from the `b/src` another
parent contributed.

Two dedups are now clearly separated, and the stale comment at the top of the
block claiming the output dedup also prevented cycles is corrected:
`include_dir_seen`/`push_include_dir` dedupe the **output list**; `VisitedRoots`
dedupes the **walk**.

**On this Linux ASan build the symptom was worse than reported**: not `File
name too long` from `cc` but a hard `AddressSanitizer: stack-overflow` at
`collect_dep_dirs_recursive`, ~220 frames deep, before `cc` was ever invoked.
Same root cause; the sanitizer just caught the unbounded recursion first.

### Regression coverage

The pre-existing `transitive-cmake-deps-cycle` fixture proved termination but
never imported across the cycle, so it could not catch a visited set that
prunes too eagerly. `spice-a/src/main.tur` now imports `forty-two` from
`spice-b` and prints `cycle-ok` only when the cross-cycle call returns 42, so
the same assertion covers both directions.

Two new fixtures cover the ways a too-clever fix goes wrong:

- `tests/fixtures/spice-cycle-three-hop/` -- `a -> b -> c -> a`, which a
  "is this dep my own parent?" check would miss. `a` imports from both `b` and
  `c`, so every src/ on the ring must still land on the include path.
- `tests/fixtures/spice-diamond-shared-dep/` -- one dep reached by two parents.
  The visited set must dedupe the walk *without* pruning the include dir. This
  works because the dir is pushed before the visited check gates the recursion;
  the fixture is what notices if those two are ever reordered.

Both wired into `tests/run-transitive-cmake-deps.sh` (5 passed, 0 failed).
`bash tests/run.sh`: 2409 passed, 0 failed.

### Not in scope

`collect_spice_aux_c` (the `:c-sources` / `:c-includes` collector) walks only
one level and cannot recurse, so it carries no cycle risk. The other entry
point the report cites, `src/compiler/pkg.h:300`, documents
`pkg_collect_transitive_cmake_deps` -- the walk that was already correct.

## Original report

## Summary

Two spices that declare each other as `:path` deps make the include-path
resolver walk the cycle unboundedly, emitting one `-I <a>/../<b>` segment per
lap. The result is a multi-kilobyte `-I` argument that `cc` rejects with
`File name too long` on an unrelated system header.

## Repro

`tests/fixtures/transitive-cmake-deps-cycle/` already encodes it -- `spice-a`
depends on `spice-b` and `spice-b` on `spice-a`. The harness case
`transitive-cmake-deps-cycle-builds` in `tests/run-transitive-cmake-deps.sh`
is red:

```
$ bash tests/run-transitive-cmake-deps.sh
FAIL transitive-cmake-deps-cycle-builds -- tur build exit=2:
  ...spice-a/../spice-b/../spice-a/../spice-b/[... ~60 laps ...]/sys/select.h:
  File name too long
   10 | #include <sys/select.h>
```

Note the failure surfaces on `#include <sys/select.h>` -- a red herring; the
header is fine, the search path is not.

## Root cause

The transitive `:spices` walk that unions dep `src/` dirs into the include
path has no visited-set, so a cycle recurses until some other limit trips.
Entry points are the transitive resolution in `src/main.c:4891` and the
manifest walk described at `src/compiler/pkg.h:300`. The sibling case is
already handled -- `transitive-cmake-deps-conflict-detected` passes -- so the
conflict detector has the bookkeeping this path is missing.

## Fix directions

Thread a visited set (by canonicalized manifest path) through the transitive
`:spices` walk and stop on re-entry. Decide whether a cycle should be a hard
error (`tur.lock` cannot express one) or silently deduped; a diagnostic naming
both spices is friendlier than either current behavior. Canonicalizing each
`-I` before appending would also cap the damage independently.

## Status

Pre-existing, not introduced by the TUR-E0620 manifest-slot audit -- verified
by rebuilding at `67ce68f6c` and reproducing identically.
